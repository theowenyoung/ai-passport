// main/card_layout.h — line breaking and pagination for card text.
//
// Glyph widths come from a callback (LVGL on the device, a stub in host tests),
// so the rules below are tested without a display:
//   - CJK text breaks between any two characters;
//   - a run of ASCII non-space characters (a word, number, or URL label) is not
//     split unless it is wider than a whole line;
//   - closing punctuation never starts a line: up to two such characters hang
//     past the right edge instead;
//   - opening brackets and quotes never end a line;
//   - spaces at a break are dropped.
#pragma once

#include <stdint.h>

enum {
    LAYOUT_TEXT = 0,
    LAYOUT_BLANK,     // paragraph gap
    LAYOUT_DATE,      // date line of a merged Thread; `start` skips the marker
};

typedef struct {
    uint32_t start;   // byte offsets into the text, [start, end)
    uint32_t end;
    uint8_t kind;
} layout_line_t;

// Advance width in pixels of `cp`; `next` is the following code point or 0.
typedef int (*layout_width_fn)(uint32_t cp, uint32_t next, void *ctx);

// Decode one UTF-8 code point at *pos and advance it; invalid bytes yield U+FFFD.
uint32_t layout_utf8_next(const char *text, uint32_t len, uint32_t *pos);

// Break `text` into lines at most `max_width` wide (plus hanging punctuation).
// Returns the line count, or -1 when more than `cap` lines are needed.
int layout_lines(const char *text, uint32_t len, int max_width, layout_width_fn width, void *ctx,
                 layout_line_t *lines, int cap);

// Group lines into pages of `per_page`; a paragraph gap never starts a page.
// Stores each page's first line index; returns the page count or -1 past `cap`.
int layout_pages(const layout_line_t *lines, int count, int per_page, uint16_t *page_starts, int cap);
