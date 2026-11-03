// main/timer_ui.c — the timer's single screen, black, for a 240x320 portrait panel.
//
//   battery                       top right
//   segment chip                  分享 (green) / 交流 (blue) / 时间到 (red)
//   big digits                    time left in this segment, or the overtime
//   two-part progress bar         3 : 2, sharing then discussion
//   hint                          what the keys do right now
//   sound warning                 Ready only, when the codec failed
//
// Every Chinese string below is in the generated fonts' charset: after changing
// one, run `python3 tools/timer_fonts.py` and rebuild (see assets/README.md).
#include "timer_ui.h"

#include "lvgl.h"

LV_FONT_DECLARE(timer_digits_100);
LV_FONT_DECLARE(timer_digits_80);
LV_FONT_DECLARE(timer_text_26);
LV_FONT_DECLARE(timer_text_20);

#define COLOR_BG          0x000000
#define COLOR_DIGITS      0xFFFFFF
#define COLOR_WARN        0xF2A33A
#define COLOR_OVER        0xF26D6D
#define COLOR_HINT        0x9A9A9A
#define COLOR_TRACK       0x303030
#define COLOR_SHARE_FILL  0x8CC152
#define COLOR_TALK_FILL   0x6FA8E8
#define COLOR_OVER_FILL   0xE24B4A
#define COLOR_BATT_FRAME  0x8A8A8A
#define COLOR_BATT_OK     0x8CC152
#define COLOR_BATT_LOW    0xE24B4A

#define BAR_X     20
#define BAR_Y     226
#define BAR_W     200
#define BAR_H     10
#define BAR_GAP   6
#define SHARE_W   ((BAR_W - BAR_GAP) * 3 / 5)
#define TALK_W    (BAR_W - BAR_GAP - SHARE_W)
#define BATT_W    24
#define BATT_FILL_MAX (BATT_W - 6)

typedef struct {
    uint32_t bg, fg;
    const char *text;
} chip_style_t;

static const chip_style_t CHIPS[] = {
    [ROUND_READY] = { 0x2A2A2A, 0xCFCFCF, "分享 3 + 交流 2" },
    [ROUND_SHARE] = { 0x27500A, 0xC0DD97, "分享" },
    [ROUND_TALK]  = { 0x0C447C, 0xB5D4F4, "交流" },
    [ROUND_OVER]  = { 0x791F1F, 0xF7C1C1, "时间到" },
};

static const char *const HINTS[] = {
    [ROUND_READY] = "按确定 开始",
    [ROUND_SHARE] = "下键 跳到交流\n上键 复位",
    [ROUND_TALK]  = "下键 结束本轮\n上键 复位",
    [ROUND_OVER]  = "按确定 回到待机",
};
static const char *const NO_SOUND_TEXT = "声音不可用";

static lv_obj_t *s_chip, *s_chip_label;
static lv_obj_t *s_digits, *s_over_digits;
static lv_obj_t *s_share_track, *s_share_fill, *s_talk_track, *s_talk_fill;
static lv_obj_t *s_hint;
static lv_obj_t *s_no_sound;
static lv_obj_t *s_batt, *s_batt_fill;

// What the screen shows now, so a render touches only what changed.
static struct {
    bool valid;
    round_view_t view;
    int battery;
    bool sound_ok;
} s_shown;

static lv_obj_t *box(lv_obj_t *parent, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    return obj;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void build_battery(lv_obj_t *scr)
{
    s_batt = box(scr, BATT_W, 12, COLOR_BG, 3);
    lv_obj_set_style_border_width(s_batt, 2, 0);
    lv_obj_set_style_border_color(s_batt, lv_color_hex(COLOR_BATT_FRAME), 0);
    lv_obj_set_pos(s_batt, 180, 16);
    lv_obj_t *tip = box(scr, 2, 6, COLOR_BATT_FRAME, 0);
    lv_obj_set_pos(tip, 180 + BATT_W, 19);
    s_batt_fill = box(s_batt, BATT_FILL_MAX, 6, COLOR_BATT_OK, 1);
    lv_obj_set_pos(s_batt_fill, 1, 1);
}

static void build_bar(lv_obj_t *scr)
{
    s_share_track = box(scr, SHARE_W, BAR_H, COLOR_TRACK, BAR_H / 2);
    lv_obj_set_pos(s_share_track, BAR_X, BAR_Y);
    s_share_fill = box(s_share_track, 0, BAR_H, COLOR_SHARE_FILL, BAR_H / 2);
    s_talk_track = box(scr, TALK_W, BAR_H, COLOR_TRACK, BAR_H / 2);
    lv_obj_set_pos(s_talk_track, BAR_X + SHARE_W + BAR_GAP, BAR_Y);
    s_talk_fill = box(s_talk_track, 0, BAR_H, COLOR_TALK_FILL, BAR_H / 2);
}

void timer_ui_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_battery(scr);

    s_chip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chip);
    lv_obj_remove_flag(s_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_chip, 18, 0);
    lv_obj_set_style_pad_ver(s_chip, 4, 0);
    lv_obj_align(s_chip, LV_ALIGN_TOP_MID, 0, 40);
    s_chip_label = text(s_chip, &timer_text_26, 0xFFFFFF);

    s_digits = text(scr, &timer_digits_100, COLOR_DIGITS);
    lv_obj_align(s_digits, LV_ALIGN_TOP_MID, 0, 116);
    s_over_digits = text(scr, &timer_digits_80, COLOR_OVER);
    lv_obj_align(s_over_digits, LV_ALIGN_TOP_MID, 0, 124);

    build_bar(scr);

    s_hint = text(scr, &timer_text_20, COLOR_HINT);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_hint, 4, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 248);

    s_no_sound = text(scr, &timer_text_20, COLOR_OVER);
    lv_label_set_text_static(s_no_sound, NO_SOUND_TEXT);
    lv_obj_align(s_no_sound, LV_ALIGN_TOP_MID, 0, 284);

    s_shown.valid = false;
    lv_screen_load(scr);
}

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static int fill_width(uint16_t fill, int track_w)
{
    if (fill == 0) return 0;
    int w = track_w * fill / 1000;
    // A sliver narrower than the bar's height would draw as a dot; show at least that.
    return w < BAR_H ? BAR_H : w;
}

static void render_battery(int soc)
{
    if (soc < 0) {
        set_hidden(s_batt_fill, true);  // unknown: an empty frame, no guessed level
        return;
    }
    set_hidden(s_batt_fill, false);
    int w = BATT_FILL_MAX * (soc > 100 ? 100 : soc) / 100;
    lv_obj_set_width(s_batt_fill, w < 2 ? 2 : w);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(soc <= 20 ? COLOR_BATT_LOW : COLOR_BATT_OK), 0);
}

void timer_ui_render(const round_view_t *v, int battery_soc, bool sound_ok)
{
    const round_view_t *old = s_shown.valid ? &s_shown.view : NULL;
    bool phase_changed = !old || old->phase != v->phase;

    if (phase_changed) {
        const chip_style_t *chip = &CHIPS[v->phase];
        lv_obj_set_style_bg_color(s_chip, lv_color_hex(chip->bg), 0);
        lv_obj_set_style_text_color(s_chip_label, lv_color_hex(chip->fg), 0);
        lv_label_set_text_static(s_chip_label, chip->text);
        bool over = v->phase == ROUND_OVER;
        set_hidden(s_digits, over);
        set_hidden(s_over_digits, !over);
        uint32_t fill = over ? COLOR_OVER_FILL : COLOR_SHARE_FILL;
        lv_obj_set_style_bg_color(s_share_fill, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_color(s_talk_fill, lv_color_hex(over ? COLOR_OVER_FILL : COLOR_TALK_FILL), 0);
    }

    if (phase_changed || old->seconds != v->seconds) {
        char buf[8];
        bool over = v->phase == ROUND_OVER;
        round_format_time(v->seconds, over, buf, sizeof buf);
        lv_label_set_text(over ? s_over_digits : s_digits, buf);
    }
    if (phase_changed || old->warn != v->warn) {
        lv_obj_set_style_text_color(s_digits, lv_color_hex(v->warn ? COLOR_WARN : COLOR_DIGITS), 0);
    }
    if (phase_changed || old->share_fill != v->share_fill) {
        lv_obj_set_width(s_share_fill, fill_width(v->share_fill, SHARE_W));
    }
    if (phase_changed || old->talk_fill != v->talk_fill) {
        lv_obj_set_width(s_talk_fill, fill_width(v->talk_fill, TALK_W));
    }
    if (phase_changed) lv_label_set_text_static(s_hint, HINTS[v->phase]);
    if (phase_changed || s_shown.sound_ok != sound_ok) {
        set_hidden(s_no_sound, v->phase != ROUND_READY || sound_ok);
    }

    if (!s_shown.valid || s_shown.battery != battery_soc) render_battery(battery_soc);

    s_shown.valid = true;
    s_shown.view = *v;
    s_shown.battery = battery_soc;
    s_shown.sound_ok = sound_ok;
}
