#include "card_layout.h"

#include <stdbool.h>
#include <stddef.h>

#define SEGMENT_MARK 0x1Fu
#define MAX_HANG     2

uint32_t layout_utf8_next(const char *text, uint32_t len, uint32_t *pos)
{
    const uint8_t *s = (const uint8_t *)text;
    uint32_t i = *pos;
    uint8_t c = s[i];
    uint32_t cp, extra;
    if (c < 0x80) { *pos = i + 1; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { *pos = i + 1; return 0xFFFD; }
    for (uint32_t k = 1; k <= extra; k++) {
        if (i + k >= len || (s[i + k] & 0xC0) != 0x80) { *pos = i + k; return 0xFFFD; }
        cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    *pos = i + extra + 1;
    return cp;
}

static bool in_list(uint32_t cp, const uint32_t *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (list[i] == cp) return true;
    }
    return false;
}

static bool no_start(uint32_t cp)
{
    static const uint32_t list[] = {
        0xFF0C, 0x3002, 0x3001, 0xFF1B, 0xFF1A, 0xFF1F, 0xFF01, 0xFF09, 0x300B, 0x300D,
        0x300F, 0x3011, 0x3009, 0x3015, 0x201D, 0x2019, 0x2026, 0x00B7, 0x2014, 0xFF5E,
        0xFF05, 0x3003, ',', '.', ';', ':', '?', '!', ')', ']', '}', '%',
    };
    return in_list(cp, list, sizeof(list) / sizeof(list[0]));
}

static bool no_end(uint32_t cp)
{
    static const uint32_t list[] = {
        0xFF08, 0x300A, 0x300C, 0x300E, 0x3010, 0x3008, 0x3014, 0x201C, 0x2018, '(', '[', '{',
    };
    return in_list(cp, list, sizeof(list) / sizeof(list[0]));
}

static bool ascii_word(uint32_t cp) { return cp > 0x20 && cp < 0x7F; }

static bool break_ok(uint32_t prev, uint32_t cur)
{
    if (prev == ' ' || cur == ' ') return true;
    if (ascii_word(prev) && ascii_word(cur)) return false;
    return !no_start(cur) && !no_end(prev);
}

static uint32_t peek(const char *text, uint32_t end, uint32_t pos)
{
    if (pos >= end) return 0;
    return layout_utf8_next(text, end, &pos);
}

// Find where the line starting at `start` ends; *next receives where the
// following line starts. Never returns an empty line for non-empty input.
static uint32_t find_break(const char *text, uint32_t start, uint32_t end, int max_width,
                           layout_width_fn width, void *ctx, uint32_t *next)
{
    uint32_t pos = start, last_ok = start, prev = 0;
    int used = 0;
    while (pos < end) {
        uint32_t after = pos;
        uint32_t cp = layout_utf8_next(text, end, &after);
        if (pos > start && break_ok(prev, cp)) last_ok = pos;
        int w = width(cp, peek(text, end, after), ctx);
        if (pos > start && used + w > max_width && cp != ' ') {
            uint32_t cut;
            if (no_start(cp)) {
                // Hang closing punctuation rather than orphaning it.
                cut = after;
                for (int hung = 1; hung < MAX_HANG && cut < end; hung++) {
                    uint32_t probe = cut;
                    if (!no_start(layout_utf8_next(text, end, &probe))) break;
                    cut = probe;
                }
            } else {
                cut = last_ok > start ? last_ok : pos;
            }
            uint32_t line_end = cut;
            while (line_end > start && text[line_end - 1] == ' ') line_end--;
            while (cut < end && text[cut] == ' ') cut++;
            *next = cut;
            return line_end;
        }
        used += w;
        prev = cp;
        pos = after;
    }
    uint32_t line_end = end;
    while (line_end > start && text[line_end - 1] == ' ') line_end--;
    *next = end;
    return line_end;
}

static bool push(layout_line_t *lines, int cap, int *count, uint32_t start, uint32_t end, uint8_t kind)
{
    if (*count >= cap) return false;
    lines[*count] = (layout_line_t){ .start = start, .end = end, .kind = kind };
    (*count)++;
    return true;
}

int layout_lines(const char *text, uint32_t len, int max_width, layout_width_fn width, void *ctx,
                 layout_line_t *lines, int cap)
{
    int count = 0;
    uint32_t para = 0;
    while (para <= len) {
        uint32_t para_end = para;
        while (para_end < len && text[para_end] != '\n') para_end++;
        if (para_end == para) {
            // An empty paragraph is a gap; ignore gaps at the edges and repeats.
            if (count > 0 && lines[count - 1].kind != LAYOUT_BLANK && para_end < len) {
                if (!push(lines, cap, &count, para, para, LAYOUT_BLANK)) return -1;
            }
        } else if ((uint8_t)text[para] == SEGMENT_MARK) {
            if (!push(lines, cap, &count, para + 1, para_end, LAYOUT_DATE)) return -1;
        } else {
            uint32_t start = para;
            while (start < para_end && text[start] == ' ') start++;
            while (start < para_end) {
                uint32_t next;
                uint32_t line_end = find_break(text, start, para_end, max_width, width, ctx, &next);
                if (!push(lines, cap, &count, start, line_end, LAYOUT_TEXT)) return -1;
                start = next;
            }
        }
        if (para_end >= len) break;
        para = para_end + 1;
    }
    while (count > 0 && lines[count - 1].kind == LAYOUT_BLANK) count--;
    return count;
}

int layout_pages(const layout_line_t *lines, int count, int per_page, uint16_t *page_starts, int cap)
{
    if (per_page < 1) per_page = 1;
    int pages = 0;
    int i = 0;
    while (i < count) {
        while (i < count && lines[i].kind == LAYOUT_BLANK) i++;
        if (i >= count) break;
        if (pages >= cap) return -1;
        page_starts[pages++] = (uint16_t)i;
        i += per_page;
    }
    return pages;
}
