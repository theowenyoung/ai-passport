// Parses the pack written by tests/blog_cards_fixture.py, then checks that every
// kind of corruption is rejected. Usage: test_card_pack FIXTURE.bin
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card_pack.h"

static uint32_t fnv1a32(const char *s)
{
    uint32_t v = 0x811C9DC5u;
    while (*s) v = (v ^ (uint8_t)*s++) * 0x01000193u;
    return v;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Re-seal checksums after a deliberate edit, so only the edited field is wrong.
static void reseal(uint8_t *pack, size_t size)
{
    wr32(pack + 36, card_crc32(pack + CARD_PACK_HEADER_SIZE, size - CARD_PACK_HEADER_SIZE));
    wr32(pack + 60, card_crc32(pack, CARD_PACK_HEADER_SIZE - 4));
}

static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    assert(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = (size_t)n;
    return data;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(card_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);

    size_t size;
    uint8_t *data = load(argv[1], &size);
    card_pack_t pack;
    assert(card_pack_open(&pack, data, size) == CARD_PACK_OK);
    assert(pack.count == 3 && pack.size == size);
    assert(pack.built_at == 1700000123u && pack.tz_offset_min == 480);

    card_t c;
    assert(card_pack_get(&pack, 0, &c));
    assert(c.id == fnv1a32("pst_a") && c.published_at == 1700000000u);
    assert(c.kind == CARD_KIND_QUOTE);
    assert(c.flags == (CARD_FLAG_FEATURED | CARD_FLAG_SOURCE_INHERITED));
    assert(strcmp(c.text, "引用\n\n第二段") == 0 && c.text_len == strlen(c.text));
    assert(strcmp(c.source, "《书名》") == 0 && c.source_len == strlen(c.source));
    assert(c.thread_pos == 0 && c.thread_len == 2);

    assert(card_pack_get(&pack, 1, &c));
    assert(c.id == fnv1a32("pst_b") && strcmp(c.text, "Hello, world") == 0);
    assert(c.source_len == 0 && c.source[0] == '\0' && c.thread_pos == 1);

    assert(card_pack_get(&pack, 2, &c));
    assert(c.id == fnv1a32("thread:pst_c") && c.kind == CARD_KIND_NOTE);
    assert(c.flags == CARD_FLAG_MERGED_THREAD && c.text[0] == CARD_SEGMENT_MARK);
    assert(!card_pack_get(&pack, 3, &c));

    uint32_t total = 0;
    assert(card_pack_peek(data, size, &total) == CARD_PACK_OK && total == size);

    uint8_t *copy = malloc(size);
    assert(copy);

    memset(copy, 0xFF, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_EMPTY);
    assert(pack.count == 0);

    memcpy(copy, data, size);
    memcpy(copy, "APP0", 4);  // data left by other firmware
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_FOREIGN);

    memcpy(copy, data, size);
    copy[40] ^= 1;  // reserved header byte, covered by the header checksum
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_HEADER);

    memcpy(copy, data, size);
    copy[size - 2] ^= 1;
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_CHECKSUM);

    assert(card_pack_open(&pack, data, size - 1) == CARD_PACK_TRUNCATED);
    assert(card_pack_open(&pack, data, 10) == CARD_PACK_TRUNCATED);

    memcpy(copy, data, size);
    copy[4] = 2;
    reseal(copy, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_VERSION);

    memcpy(copy, data, size);
    wr32(copy + CARD_PACK_HEADER_SIZE + 12, 0x10000);  // text length past the string area
    reseal(copy, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_LAYOUT);

    memcpy(copy, data, size);
    copy[CARD_PACK_HEADER_SIZE + 22] = 9;  // unknown kind
    reseal(copy, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_LAYOUT);

    memcpy(copy, data, size);
    copy[CARD_PACK_HEADER_SIZE + 24] = 5;  // thread_pos >= thread_len
    reseal(copy, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_LAYOUT);

    memcpy(copy, data, size);
    copy[size - 1] = 'x';  // last string loses its NUL
    reseal(copy, size);
    assert(card_pack_open(&pack, copy, size) == CARD_PACK_BAD_LAYOUT);

    free(copy);
    free(data);
    puts("card pack format tests: PASS");
    return 0;
}
