// main/card_pack.h — read-only view of the card pack written by tools/blog_cards.py.
// Pure C: no ESP-IDF or LVGL, so host tests can exercise every validation path.
// The format is documented in docs/blog-cards/design.md; keep both in step.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CARD_PACK_HEADER_SIZE 64u
#define CARD_PACK_RECORD_SIZE 32u
#define CARD_SEGMENT_MARK     '\x1f'   // starts a date line inside a merged Thread

enum { CARD_KIND_QUOTE = 1, CARD_KIND_NOTE = 2 };

enum {
    CARD_FLAG_FEATURED           = 1u << 0,
    CARD_FLAG_HIDDEN_FROM_LATEST = 1u << 1,
    CARD_FLAG_SOURCE_INHERITED   = 1u << 2,
    CARD_FLAG_MERGED_THREAD      = 1u << 3,
};

typedef enum {
    CARD_PACK_OK = 0,
    CARD_PACK_EMPTY,          // erased Flash: nothing has been written yet
    CARD_PACK_FOREIGN,        // other data (e.g. left by earlier firmware), not a card pack
    CARD_PACK_BAD_HEADER,     // card pack header with a bad size or checksum
    CARD_PACK_BAD_VERSION,
    CARD_PACK_TRUNCATED,      // header claims more bytes than are available
    CARD_PACK_BAD_CHECKSUM,   // payload checksum mismatch
    CARD_PACK_BAD_LAYOUT,     // offsets, strings, or records out of range
} card_pack_status_t;

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t count;
    uint32_t built_at;
    int16_t tz_offset_min;
    uint32_t index_offset;
    uint32_t strings_offset;
    uint32_t strings_size;
} card_pack_t;

typedef struct {
    uint32_t id;
    uint32_t published_at;
    const char *text;         // NUL-terminated, points into the pack
    uint32_t text_len;
    const char *source;       // NUL-terminated, may be empty
    uint16_t source_len;
    uint8_t kind;
    uint8_t flags;
    uint16_t thread_pos;
    uint16_t thread_len;
} card_t;

// zlib-compatible CRC-32 (the pack builder uses Python's zlib.crc32).
uint32_t card_crc32(const uint8_t *data, size_t len);

// Validate only the 64-byte header and report the pack's total size, so the
// caller can map exactly that many bytes before calling card_pack_open().
card_pack_status_t card_pack_peek(const uint8_t *header, size_t len, uint32_t *total_size);

// Validate the whole pack: header, payload checksum, every record and string.
card_pack_status_t card_pack_open(card_pack_t *pack, const uint8_t *data, size_t available);

// Fill `out` for record `index`; returns false when out of range.
bool card_pack_get(const card_pack_t *pack, uint32_t index, card_t *out);

const char *card_pack_status_text(card_pack_status_t status);
