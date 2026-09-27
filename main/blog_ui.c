// main/blog_ui.c — Blog Cards: one card per screen, drawn from the card pack.
//
// Threading: button callbacks only enqueue; the app task owns all state and
// takes the LVGL lock for every LVGL call. The toast timer runs in the LVGL
// task and only hides the toast.
#include "blog_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "card_layout.h"
#include "card_pack.h"
#include "card_sched.h"
#include "card_store.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

LV_FONT_DECLARE(blog_font_16);
LV_FONT_DECLARE(blog_font_20);
LV_FONT_DECLARE(blog_font_24);

static const char *TAG = "blog_ui";

// Layout of the 240 x 320 portrait screen, inside the 30 px rounded mask.
#define SCREEN_W      240
#define SCREEN_H      320
#define PAD_X         20
#define HEADER_Y      16
#define BODY_Y        50
#define BODY_W        (SCREEN_W - 2 * PAD_X)
#define BODY_H        226
#define FOOTER_Y      286
// Header, left to right: date (and Thread position), then a star for kept cards
// and a battery icon at the right edge. The date label is capped so the three
// never overlap: "2023.11.14 · 12/39" is 146 px at 16 px.
#define BATTERY_X     (SCREEN_W - PAD_X - 22)
#define STAR_X        (BATTERY_X - 22)
#define DATE_MAX_W    (STAR_X - PAD_X - 4)

#define MAX_LINES     320
#define MAX_PAGES     48
#define HISTORY_LEN   16
#define LINE_BUF      512

#define DIM_AFTER_MS  30000
#define OFF_AFTER_MS  120000
#define SAVE_AFTER_MS 8000
#define BATTERY_MS    30000
#define TOAST_MS      1400
#define LOCK_MS       500

typedef struct {
    const lv_font_t *font;
    int8_t line_space;
} body_font_t;

// Largest first: a card uses the largest size that fits on one screen. Extra
// line space brings the pitch to about 1.4-1.5 em (34/29/24 px), which reads
// comfortably for Chinese; measured on the real cards, 74% fit one screen.
static const body_font_t BODY_FONTS[] = {
    { &blog_font_24, 3 },
    { &blog_font_20, 4 },
    { &blog_font_16, 3 },
};
#define FONT_COUNT (sizeof(BODY_FONTS) / sizeof(BODY_FONTS[0]))

// Quotes and notes are told apart by colour and the quote bar, not by a label.
typedef struct {
    uint32_t bg, ink, accent, muted;
} theme_t;

static const theme_t THEME_QUOTE = { 0xF4EEE2, 0x2B2622, 0xA8452F, 0x9A8F80 };
static const theme_t THEME_NOTE  = { 0xE9EFF2, 0x1F2A30, 0x2F6481, 0x7F8C93 };
#define BATTERY_LOW_COLOR 0xC0392B

static const uint8_t BRIGHTNESS[] = { 100, 60, 30 };
static const char *const BRIGHTNESS_NAME[] = { "高", "中", "低" };

enum { MENU_KEEP, MENU_HIDE, MENU_UNHIDE, MENU_BRIGHTNESS, MENU_BACK, MENU_COUNT };

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_event_t;

typedef enum { POWER_ON, POWER_DIM, POWER_OFF } power_t;

// Card data and schedule (app task only).
static card_pack_t s_pack;
static card_pack_status_t s_pack_status;
static card_sched_t s_sched;
static sched_meta_t *s_meta;
static sched_state_t *s_state;
static uint16_t *s_by_id;
static uint32_t s_newest;
static int32_t s_current = -1;
static int32_t s_history[HISTORY_LEN];
static int s_history_len;

// Layout of the current card.
static layout_line_t s_lines[MAX_LINES];
static uint16_t s_pages[MAX_PAGES];
static int s_line_count, s_page_count, s_page, s_font, s_pitch, s_per_page;
static char s_buf[LINE_BUF];

// Application state.
static QueueHandle_t s_keys;
static bool s_in_menu;
static int s_menu_sel;
static bool s_dirty;
static int64_t s_last_input_ms, s_last_change_ms, s_last_battery_ms;
static power_t s_power = POWER_ON;
static uint8_t s_brightness;
static int s_battery = -1;

// LVGL objects.
static lv_obj_t *s_scr, *s_meta_label, *s_star, *s_batt, *s_batt_fill, *s_batt_tip, *s_bar, *s_body;
static lv_obj_t *s_source, *s_page_label;
static lv_obj_t *s_toast, *s_menu, *s_menu_rows[MENU_COUNT], *s_menu_info;
static lv_timer_t *s_toast_timer;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const theme_t *theme_for(const card_t *card)
{
    return card->kind == CARD_KIND_QUOTE ? &THEME_QUOTE : &THEME_NOTE;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static int glyph_width(uint32_t cp, uint32_t next, void *ctx)
{
    return lv_font_get_glyph_width((const lv_font_t *)ctx, cp, next);
}

static void format_date(uint32_t unix_time, char *out, size_t size)
{
    int y, m, d;
    sched_date(unix_time, s_pack.tz_offset_min, &y, &m, &d);
    snprintf(out, size, "%04d.%02d.%02d", y, m, d);
}

static bool current_card(card_t *card)
{
    return s_current >= 0 && card_pack_get(&s_pack, (uint32_t)s_current, card);
}

static void mark_dirty(void)
{
    s_dirty = true;
    s_last_change_ms = now_ms();
}

static void save_if_needed(bool force)
{
    if (!s_dirty || !s_meta) return;
    if (!force && now_ms() - s_last_change_ms < SAVE_AFTER_MS) return;
    if (card_store_save(&s_sched, s_newest) == ESP_OK) s_dirty = false;
}

static void apply_backlight(void)
{
    switch (s_power) {
    case POWER_ON: bsp_display_backlight(BRIGHTNESS[s_brightness]); break;
    case POWER_DIM: bsp_display_backlight(BRIGHTNESS[s_brightness] / 4 + 2); break;
    case POWER_OFF: bsp_display_backlight(0); break;
    }
}

static void load_brightness(void)
{
    nvs_handle_t nvs;
    uint8_t level = 0;
    if (nvs_open("blogcards", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, "bright", &level);
        nvs_close(nvs);
    }
    s_brightness = level < sizeof(BRIGHTNESS) ? level : 0;
}

static void store_brightness(void)
{
    nvs_handle_t nvs;
    if (nvs_open("blogcards", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_u8(nvs, "bright", s_brightness);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// ---------------------------------------------------------------------------
// Layout and rendering (LVGL lock held)
// ---------------------------------------------------------------------------

static void layout_card(const card_t *card)
{
    for (size_t f = 0; f < FONT_COUNT; f++) {
        const lv_font_t *font = BODY_FONTS[f].font;
        int pitch = lv_font_get_line_height(font) + BODY_FONTS[f].line_space;
        int per_page = BODY_H / pitch;
        int count = layout_lines(card->text, card->text_len, BODY_W, glyph_width, (void *)font,
                                 s_lines, MAX_LINES);
        bool last = f == FONT_COUNT - 1;
        if (count < 0 && !last) continue;
        if (count < 0) count = MAX_LINES;  // absurdly long: show what fits in the buffer
        if (count <= per_page || last) {
            s_font = (int)f;
            s_pitch = pitch;
            s_per_page = per_page;
            s_line_count = count;
            s_page_count = layout_pages(s_lines, count, per_page, s_pages, MAX_PAGES);
            if (s_page_count < 0) s_page_count = MAX_PAGES;
            if (s_page_count == 0) {
                s_page_count = 1;
                s_pages[0] = 0;
            }
            s_page = 0;
            return;
        }
    }
}

static void render_status(const theme_t *theme)
{
    bool kept = s_current >= 0 && (s_state[s_current].flags & SCHED_KEPT);
    lv_obj_set_style_text_color(s_star, lv_color_hex(theme->accent), 0);
    if (kept) lv_obj_remove_flag(s_star, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_star, LV_OBJ_FLAG_HIDDEN);

    // Battery icon instead of a percentage: the level shows as the fill width.
    if (s_battery < 0) {
        lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_tip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    uint32_t color = s_battery <= 15 ? BATTERY_LOW_COLOR : theme->muted;
    lv_obj_set_style_border_color(s_batt, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(s_batt_tip, lv_color_hex(color), 0);
    int fill = s_battery * 14 / 100;
    lv_obj_set_width(s_batt_fill, fill < 1 ? 1 : fill);
    lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_batt_tip, LV_OBJ_FLAG_HIDDEN);
}

static void render_page(void)
{
    card_t card;
    if (!current_card(&card)) return;
    const theme_t *theme = theme_for(&card);
    const lv_font_t *font = BODY_FONTS[s_font].font;

    lv_obj_set_style_bg_color(s_scr, lv_color_hex(theme->bg), 0);

    char date[16];
    format_date(card.published_at, date, sizeof(date));
    if (card.thread_len > 1) {
        snprintf(s_buf, sizeof(s_buf), "%s · %u/%u", date, card.thread_pos + 1u, (unsigned)card.thread_len);
    } else {
        snprintf(s_buf, sizeof(s_buf), "%s", date);
    }
    lv_label_set_text(s_meta_label, s_buf);
    lv_obj_set_style_text_color(s_meta_label, lv_color_hex(theme->muted), 0);
    render_status(theme);

    lv_obj_clean(s_body);
    int first = s_pages[s_page];
    int last = s_page + 1 < s_page_count ? s_pages[s_page + 1] : s_line_count;
    if (last - first > s_per_page) last = first + s_per_page;
    int y = 0;
    for (int i = first; i < last; i++) {
        const layout_line_t *line = &s_lines[i];
        if (line->kind != LAYOUT_BLANK) {
            bool date_line = line->kind == LAYOUT_DATE;
            lv_obj_t *label = make_label(s_body, date_line ? &blog_font_16 : font,
                                         date_line ? theme->muted : theme->ink);
            uint32_t n = line->end - line->start;
            if (n >= LINE_BUF) n = LINE_BUF - 1;
            memcpy(s_buf, card.text + line->start, n);
            s_buf[n] = '\0';
            lv_label_set_text(label, s_buf);
            lv_obj_set_pos(label, 0, y + (date_line ? (s_pitch - lv_font_get_line_height(&blog_font_16)) : 0));
        }
        y += s_pitch;
    }

    bool quote = card.kind == CARD_KIND_QUOTE;
    if (quote && y > 0) {
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(theme->accent), 0);
        lv_obj_set_height(s_bar, y - (s_pitch - lv_font_get_line_height(font)) - 6);
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }

    bool last_page = s_page == s_page_count - 1;
    if (last_page && card.source_len) {
        snprintf(s_buf, sizeof(s_buf), "— %s", card.source);
        lv_label_set_text(s_source, s_buf);
        lv_obj_set_style_text_color(s_source, lv_color_hex(theme->muted), 0);
        lv_obj_remove_flag(s_source, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_source, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_page_count > 1) {
        lv_label_set_text_fmt(s_page_label, "%d/%d", s_page + 1, s_page_count);
        lv_obj_set_style_text_color(s_page_label, lv_color_hex(theme->muted), 0);
        lv_obj_remove_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_source, BODY_W - 44);
    } else {
        lv_obj_add_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(s_source, BODY_W);
    }
}

static void toast_hide(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(s_toast_timer);
}

static void show_toast(const char *text)
{
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}

static void render_menu(void)
{
    bool kept = s_current >= 0 && (s_state[s_current].flags & SCHED_KEPT);
    sched_stats_t stats = sched_stats(&s_sched);
    char text[48];
    lv_label_set_text(s_menu_rows[MENU_KEEP], kept ? "取消留住" : "★ 留住这张");
    lv_label_set_text(s_menu_rows[MENU_HIDE], "隐藏这张");
    if (stats.hidden) snprintf(text, sizeof(text), "恢复隐藏的卡片（%u）", (unsigned)stats.hidden);
    else snprintf(text, sizeof(text), "恢复隐藏的卡片");
    lv_label_set_text(s_menu_rows[MENU_UNHIDE], text);
    snprintf(text, sizeof(text), "亮度：%s", BRIGHTNESS_NAME[s_brightness]);
    lv_label_set_text(s_menu_rows[MENU_BRIGHTNESS], text);
    lv_label_set_text(s_menu_rows[MENU_BACK], "返回");

    for (int i = 0; i < MENU_COUNT; i++) {
        bool selected = i == s_menu_sel;
        bool disabled = i == MENU_UNHIDE && !stats.hidden;
        lv_obj_t *row = lv_obj_get_parent(s_menu_rows[i]);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_menu_rows[i],
            lv_color_hex(disabled ? 0x6B6F75 : (selected ? 0xFFFFFF : 0xE6E1D8)), 0);
    }
    char date[16];
    format_date(s_pack.built_at, date, sizeof(date));
    snprintf(s_buf, sizeof(s_buf), "已读 %u / %u · 留住 %u\n卡片更新于 %s",
             (unsigned)stats.seen, (unsigned)s_pack.count, (unsigned)stats.kept, date);
    lv_label_set_text(s_menu_info, s_buf);
}

static void build_screen(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(THEME_QUOTE.bg), 0);

    s_meta_label = make_label(s_scr, &blog_font_16, THEME_QUOTE.muted);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_meta_label, DATE_MAX_W);
    lv_obj_set_pos(s_meta_label, PAD_X, HEADER_Y);
    s_star = make_label(s_scr, &blog_font_16, THEME_QUOTE.accent);
    lv_label_set_text(s_star, "★");
    lv_obj_set_pos(s_star, STAR_X, HEADER_Y);
    lv_obj_add_flag(s_star, LV_OBJ_FLAG_HIDDEN);

    s_batt = make_box(s_scr);  // 20 x 10 outline, fill inset by 2 px, 2 x 4 tip
    lv_obj_set_pos(s_batt, BATTERY_X, HEADER_Y + 6);
    lv_obj_set_size(s_batt, 20, 10);
    lv_obj_set_style_border_width(s_batt, 1, 0);
    lv_obj_set_style_radius(s_batt, 2, 0);
    s_batt_fill = make_box(s_batt);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_pos(s_batt_fill, 2, 2);
    lv_obj_set_size(s_batt_fill, 14, 6);
    s_batt_tip = make_box(s_scr);
    lv_obj_set_style_bg_opa(s_batt_tip, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_batt_tip, BATTERY_X + 20, HEADER_Y + 9);
    lv_obj_set_size(s_batt_tip, 2, 4);

    s_bar = make_box(s_scr);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 2, 0);
    lv_obj_set_pos(s_bar, PAD_X - 10, BODY_Y + 6);
    lv_obj_set_size(s_bar, 3, 10);

    // Wider than the text column so hanging punctuation is not clipped.
    s_body = make_box(s_scr);
    lv_obj_set_pos(s_body, PAD_X, BODY_Y);
    lv_obj_set_size(s_body, BODY_W + 18, BODY_H);

    s_source = make_label(s_scr, &blog_font_16, THEME_QUOTE.muted);
    lv_label_set_long_mode(s_source, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_source, BODY_W);
    lv_obj_set_pos(s_source, PAD_X, FOOTER_Y);
    s_page_label = make_label(s_scr, &blog_font_16, THEME_QUOTE.muted);
    lv_obj_align(s_page_label, LV_ALIGN_TOP_RIGHT, -PAD_X, FOOTER_Y);

    s_toast = make_label(s_scr, &blog_font_16, 0xFFFFFF);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2B2622), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 6, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -52);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
    lv_timer_pause(s_toast_timer);

    s_menu = make_box(s_scr);
    lv_obj_set_size(s_menu, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_menu, lv_color_hex(0x1E2024), 0);
    lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
    lv_obj_t *title = make_label(s_menu, &blog_font_16, 0x9A9EA5);
    lv_label_set_text(title, "菜单");
    lv_obj_set_pos(title, PAD_X + 4, HEADER_Y + 4);
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = make_box(s_menu);
        lv_obj_set_size(row, SCREEN_W - 2 * 14, 38);
        lv_obj_set_pos(row, 14, 52 + i * 42);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3F47), 0);
        s_menu_rows[i] = make_label(row, &blog_font_20, 0xE6E1D8);
        lv_obj_align(s_menu_rows[i], LV_ALIGN_LEFT_MID, 12, 0);
    }
    s_menu_info = make_label(s_menu, &blog_font_16, 0x9A9EA5);
    lv_obj_set_pos(s_menu_info, PAD_X + 4, 266);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_scr);
}

static void show_message(const char *title, const char *detail)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(THEME_QUOTE.bg), 0);
    lv_obj_t *head = make_label(scr, &blog_font_24, THEME_QUOTE.ink);
    lv_label_set_text(head, title);
    lv_obj_set_pos(head, PAD_X + 4, 90);
    lv_obj_t *body = make_label(scr, &blog_font_16, THEME_QUOTE.muted);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(body, BODY_W - 8);
    lv_label_set_text(body, detail);
    lv_obj_set_pos(body, PAD_X + 4, 140);
    lv_screen_load(scr);
}

// ---------------------------------------------------------------------------
// Actions (app task)
// ---------------------------------------------------------------------------

static void show_card(int32_t index, bool counted, bool remember)
{
    if (index < 0) return;
    if (remember && s_current >= 0) {
        if (s_history_len == HISTORY_LEN) {
            memmove(s_history, s_history + 1, sizeof(s_history[0]) * (HISTORY_LEN - 1));
            s_history_len--;
        }
        s_history[s_history_len++] = s_current;
    }
    if (counted) {
        sched_shown(&s_sched, (uint32_t)index);
        mark_dirty();
    }
    s_current = index;
    card_t card;
    if (!current_card(&card) || !bsp_lvgl_lock(LOCK_MS)) return;
    layout_card(&card);
    render_page();
    bsp_lvgl_unlock();
}

static void next_card(void)
{
    int32_t index = sched_next(&s_sched);
    if (index < 0) {
        if (bsp_lvgl_lock(LOCK_MS)) {
            show_toast("所有卡片都隐藏了");
            bsp_lvgl_unlock();
        }
        return;
    }
    show_card(index, true, true);
}

static void set_page(int page)
{
    s_page = page;
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    render_page();
    bsp_lvgl_unlock();
}

static void toggle_keep(void)
{
    if (s_current < 0) return;
    bool kept = !(s_state[s_current].flags & SCHED_KEPT);
    sched_set_kept(&s_sched, (uint32_t)s_current, kept);
    mark_dirty();
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    card_t card;
    if (current_card(&card)) render_status(theme_for(&card));
    show_toast(kept ? "已留住 ★" : "已取消留住");
    bsp_lvgl_unlock();
}

static void open_menu(bool open)
{
    s_in_menu = open;
    if (open) s_menu_sel = 0;
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    if (open) {
        render_menu();
        lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_menu);
    } else {
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_lvgl_unlock();
}

static void menu_activate(void)
{
    switch (s_menu_sel) {
    case MENU_KEEP:
        open_menu(false);
        toggle_keep();
        break;
    case MENU_HIDE:
        if (s_current >= 0) {
            sched_hide(&s_sched, (uint32_t)s_current);
            mark_dirty();
            save_if_needed(true);
        }
        open_menu(false);
        next_card();
        if (bsp_lvgl_lock(LOCK_MS)) {
            show_toast("已隐藏，可在菜单里恢复");
            bsp_lvgl_unlock();
        }
        break;
    case MENU_UNHIDE: {
        uint32_t restored = sched_unhide_all(&s_sched);
        if (!restored) break;
        mark_dirty();
        save_if_needed(true);
        if (bsp_lvgl_lock(LOCK_MS)) {
            char text[32];
            snprintf(text, sizeof(text), "已恢复 %u 张", (unsigned)restored);
            render_menu();
            show_toast(text);
            bsp_lvgl_unlock();
        }
        break;
    }
    case MENU_BRIGHTNESS:
        s_brightness = (uint8_t)((s_brightness + 1) % sizeof(BRIGHTNESS));
        apply_backlight();
        store_brightness();
        if (bsp_lvgl_lock(LOCK_MS)) {
            render_menu();
            bsp_lvgl_unlock();
        }
        break;
    default:
        open_menu(false);
        break;
    }
}

static void handle_card_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    card_t card;
    if (!current_card(&card)) return;
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) next_card();
        else if (ev == BSP_BTN_DOUBLE) toggle_keep();
        else if (ev == BSP_BTN_LONG) open_menu(true);
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        if (s_page + 1 < s_page_count) {
            set_page(s_page + 1);
        } else if (card.thread_pos + 1u < card.thread_len) {
            // Cards of a Thread are adjacent in the pack.
            show_card(s_current + 1, true, true);
        } else if (bsp_lvgl_lock(LOCK_MS)) {
            show_toast("最后一页 · 确定键换一张");
            bsp_lvgl_unlock();
        }
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        if (s_page > 0) {
            set_page(s_page - 1);
        } else if (s_history_len > 0) {
            show_card(s_history[--s_history_len], false, false);
        }
    }
}

static void handle_menu_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        open_menu(false);
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        menu_activate();
        return;
    }
    s_menu_sel = (s_menu_sel + (btn == BSP_BTN_DOWN ? 1 : MENU_COUNT - 1)) % MENU_COUNT;
    if (bsp_lvgl_lock(LOCK_MS)) {
        render_menu();
        bsp_lvgl_unlock();
    }
}

static void handle_key(const key_event_t *key)
{
    if (key->ev == BSP_BTN_PRESS) return;  // act on click/double/long only
    s_last_input_ms = now_ms();
    if (s_power == POWER_OFF) {
        // The first press only wakes the screen, with a fresh card.
        s_power = POWER_ON;
        apply_backlight();
        if (!s_in_menu && s_meta) next_card();
        return;
    }
    if (s_power == POWER_DIM) {
        s_power = POWER_ON;
        apply_backlight();
    }
    if (!s_meta) return;
    if (s_in_menu) handle_menu_key(key->btn, key->ev);
    else handle_card_key(key->btn, key->ev);
}

static void tick_housekeeping(void)
{
    int64_t now = now_ms();
    int64_t idle = now - s_last_input_ms;
    if (s_power == POWER_ON && idle > DIM_AFTER_MS) {
        s_power = POWER_DIM;
        apply_backlight();
    }
    if (s_power != POWER_OFF && idle > OFF_AFTER_MS) {
        save_if_needed(true);
        s_power = POWER_OFF;
        apply_backlight();
    }
    save_if_needed(false);

    if (now - s_last_battery_ms > BATTERY_MS) {
        s_last_battery_ms = now;
        int soc = bsp_battery_soc();
        if (soc != s_battery) {
            s_battery = soc;
            card_t card;
            if (s_meta && current_card(&card) && bsp_lvgl_lock(LOCK_MS)) {
                render_status(theme_for(&card));
                bsp_lvgl_unlock();
            }
        }
    }
}

static void app_task(void *arg)
{
    (void)arg;
    if (s_meta) next_card();
    key_event_t key;
    for (;;) {
        if (xQueueReceive(s_keys, &key, pdMS_TO_TICKS(250)) == pdTRUE) handle_key(&key);
        tick_housekeeping();
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const key_event_t key = { .btn = btn, .ev = ev };
    if (s_keys) (void)xQueueSend(s_keys, &key, 0);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

static bool prepare_schedule(void)
{
    uint32_t count = s_pack.count;
    if (count == 0 || count > UINT16_MAX) return false;
    s_meta = calloc(count, sizeof(*s_meta));
    s_state = calloc(count, sizeof(*s_state));
    s_by_id = calloc(count, sizeof(*s_by_id));
    if (!s_meta || !s_state || !s_by_id) {
        ESP_LOGE(TAG, "no memory for %u cards", (unsigned)count);
        free(s_meta);
        free(s_state);
        free(s_by_id);
        s_meta = NULL;
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        card_t card;
        card_pack_get(&s_pack, i, &card);
        s_meta[i] = (sched_meta_t){
            .id = card.id,
            .published_at = card.published_at,
            .year = sched_year(card.published_at, s_pack.tz_offset_min),
            .featured = (card.flags & CARD_FLAG_FEATURED) ? 1 : 0,
        };
        if (card.published_at > s_newest) s_newest = card.published_at;
    }
    sched_init(&s_sched, count, s_meta, s_state, s_by_id, esp_random());
    card_store_load(&s_sched);
    return true;
}

void blog_ui_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        // Only brightness lives in NVS; never erase it to hide a partition problem.
        ESP_LOGW(TAG, "NVS unavailable (%s); using default brightness", esp_err_to_name(err));
    }
    load_brightness();
    if (bsp_battery_init() == ESP_OK) s_battery = bsp_battery_soc();

    s_pack_status = card_store_open_pack(&s_pack);
    ESP_LOGI(TAG, "card pack: %s, %u cards", card_pack_status_text(s_pack_status), (unsigned)s_pack.count);
    bool ready = s_pack_status == CARD_PACK_OK && prepare_schedule();

    if (bsp_lvgl_lock(1000)) {
        if (ready) {
            build_screen();
        } else if (s_pack_status == CARD_PACK_EMPTY || s_pack_status == CARD_PACK_FOREIGN ||
                   s_pack_status == CARD_PACK_OK) {
            show_message("还没有卡片", "在电脑上连接设备后运行：\npython3 tools/blog_cards.py update");
        } else {
            show_message("卡片数据损坏", "请重新运行：\npython3 tools/blog_cards.py update");
        }
        bsp_lvgl_unlock();
    }
    s_power = POWER_ON;
    apply_backlight();
    s_last_input_ms = s_last_battery_ms = now_ms();

    s_keys = xQueueCreate(8, sizeof(key_event_t));
    if (!s_keys || xTaskCreate(app_task, "blog_app", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the app task");
        return;
    }
    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "buttons unavailable: %s", esp_err_to_name(err));
}
