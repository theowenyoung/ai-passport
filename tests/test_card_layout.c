#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "card_layout.h"

// Stub metrics: CJK and full-width marks 16 px, ASCII 8 px, space 4 px.
static int width(uint32_t cp, uint32_t next, void *ctx)
{
    (void)next;
    (void)ctx;
    if (cp == ' ') return 4;
    return cp < 0x80 ? 8 : 16;
}

static layout_line_t lines[64];

static int run(const char *text, int max_width)
{
    return layout_lines(text, (uint32_t)strlen(text), max_width, width, NULL, lines, 64);
}

static void expect(const char *text, int index, const char *want)
{
    const layout_line_t *l = &lines[index];
    size_t n = l->end - l->start;
    if (n != strlen(want) || memcmp(text + l->start, want, n) != 0) {
        fprintf(stderr, "line %d: got '%.*s', want '%s'\n", index, (int)n, text + l->start, want);
        assert(0);
    }
}

int main(void)
{
    const char *t = "一二三四五六七八九十";
    assert(run(t, 64) == 3);
    expect(t, 0, "一二三四");
    expect(t, 1, "五六七八");
    expect(t, 2, "九十");

    t = "一二三四，五";  // the comma hangs instead of starting a line
    assert(run(t, 64) == 2);
    expect(t, 0, "一二三四，");
    expect(t, 1, "五");

    t = "一二三四。”五";  // up to two closing marks hang
    assert(run(t, 64) == 2);
    expect(t, 0, "一二三四。”");

    t = "一二三（四五";  // an opening bracket never ends a line
    assert(run(t, 64) == 2);
    expect(t, 0, "一二三");
    expect(t, 1, "（四五");

    t = "abc defgh";  // words stay whole; the space at the break is dropped
    assert(run(t, 48) == 2);
    expect(t, 0, "abc");
    expect(t, 1, "defgh");

    t = "abcdefghij";  // a word wider than the line is split
    assert(run(t, 48) == 2);
    expect(t, 0, "abcdef");
    expect(t, 1, "ghij");

    t = "中文abc中文";  // breaks are allowed around an ASCII word
    assert(run(t, 64) == 2);
    expect(t, 0, "中文abc");
    expect(t, 1, "中文");

    t = "\n甲\n\n\n乙\n";  // one gap between paragraphs, none at the edges
    assert(run(t, 64) == 3);
    assert(lines[0].kind == LAYOUT_TEXT && lines[1].kind == LAYOUT_BLANK && lines[2].kind == LAYOUT_TEXT);
    expect(t, 2, "乙");

    t = "\x1f" "2023-11-14\n内容";
    assert(run(t, 64) == 2);
    assert(lines[0].kind == LAYOUT_DATE);
    expect(t, 0, "2023-11-14");
    expect(t, 1, "内容");

    t = "一二三四五六七八九十";
    assert(layout_lines(t, (uint32_t)strlen(t), 16, width, NULL, lines, 5) == -1);

    uint32_t pos = 0;
    const char bad[] = { (char)0xE4, (char)0xB8, 'x', 0 };
    assert(layout_utf8_next(bad, 3, &pos) == 0xFFFD && pos == 2);
    assert(layout_utf8_next(bad, 3, &pos) == 'x' && pos == 3);

    layout_line_t page_lines[6] = {
        { 0, 1, LAYOUT_TEXT }, { 1, 2, LAYOUT_TEXT }, { 2, 3, LAYOUT_TEXT },
        { 3, 3, LAYOUT_BLANK }, { 3, 4, LAYOUT_TEXT }, { 4, 5, LAYOUT_TEXT },
    };
    uint16_t starts[4];
    assert(layout_pages(page_lines, 6, 3, starts, 4) == 2);
    assert(starts[0] == 0 && starts[1] == 4);  // the gap does not open page 2
    assert(layout_pages(page_lines, 6, 1, starts, 4) == -1);

    puts("card layout tests: PASS");
    return 0;
}
