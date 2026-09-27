#include "card_pack.h"

#include <string.h>

static const uint8_t MAGIC[4] = { 'B', 'L', 'G', 'C' };
#define PACK_VERSION 1u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t card_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

card_pack_status_t card_pack_peek(const uint8_t *header, size_t len, uint32_t *total_size)
{
    if (len < CARD_PACK_HEADER_SIZE) return CARD_PACK_TRUNCATED;
    if (memcmp(header, MAGIC, sizeof(MAGIC)) != 0) {
        static const uint8_t erased[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        return memcmp(header, erased, sizeof(erased)) == 0 ? CARD_PACK_EMPTY : CARD_PACK_FOREIGN;
    }
    if (card_crc32(header, CARD_PACK_HEADER_SIZE - 4) != rd32(header + 60)) {
        return CARD_PACK_BAD_HEADER;
    }
    if (rd16(header + 4) != PACK_VERSION) return CARD_PACK_BAD_VERSION;
    if (rd16(header + 6) != CARD_PACK_HEADER_SIZE || rd16(header + 12) != CARD_PACK_RECORD_SIZE) {
        return CARD_PACK_BAD_HEADER;
    }
    if (total_size) *total_size = rd32(header + 28);
    return CARD_PACK_OK;
}

static bool string_ok(const card_pack_t *pack, uint32_t offset, uint32_t len)
{
    // The terminating NUL must also lie inside the string area.
    if (offset >= pack->strings_size || len >= pack->strings_size - offset) return false;
    return pack->data[pack->strings_offset + offset + len] == 0;
}

card_pack_status_t card_pack_open(card_pack_t *pack, const uint8_t *data, size_t available)
{
    memset(pack, 0, sizeof(*pack));
    uint32_t total = 0;
    card_pack_status_t status = card_pack_peek(data, available, &total);
    if (status != CARD_PACK_OK) return status;
    if (total > available || total < CARD_PACK_HEADER_SIZE) return CARD_PACK_TRUNCATED;

    card_pack_t p = {
        .data = data,
        .size = total,
        .count = rd32(data + 8),
        .tz_offset_min = (int16_t)rd16(data + 14),
        .index_offset = rd32(data + 16),
        .strings_offset = rd32(data + 20),
        .strings_size = rd32(data + 24),
        .built_at = rd32(data + 32),
    };
    if (p.index_offset != CARD_PACK_HEADER_SIZE ||
        p.count > (total - CARD_PACK_HEADER_SIZE) / CARD_PACK_RECORD_SIZE ||
        p.strings_offset != p.index_offset + p.count * CARD_PACK_RECORD_SIZE ||
        p.strings_size > total - p.strings_offset ||
        p.strings_offset + p.strings_size != total) {
        return CARD_PACK_BAD_LAYOUT;
    }
    if (card_crc32(data + CARD_PACK_HEADER_SIZE, total - CARD_PACK_HEADER_SIZE) != rd32(data + 36)) {
        return CARD_PACK_BAD_CHECKSUM;
    }
    for (uint32_t i = 0; i < p.count; i++) {
        const uint8_t *r = data + p.index_offset + i * CARD_PACK_RECORD_SIZE;
        uint8_t kind = r[22];
        uint16_t pos = rd16(r + 24), len = rd16(r + 26);
        if (!string_ok(&p, rd32(r + 8), rd32(r + 12)) || !string_ok(&p, rd32(r + 16), rd16(r + 20)) ||
            (kind != CARD_KIND_QUOTE && kind != CARD_KIND_NOTE) || len == 0 || pos >= len) {
            return CARD_PACK_BAD_LAYOUT;
        }
    }
    *pack = p;
    return CARD_PACK_OK;
}

bool card_pack_get(const card_pack_t *pack, uint32_t index, card_t *out)
{
    if (!pack->data || index >= pack->count) return false;
    const uint8_t *r = pack->data + pack->index_offset + index * CARD_PACK_RECORD_SIZE;
    const char *strings = (const char *)pack->data + pack->strings_offset;
    out->id = rd32(r);
    out->published_at = rd32(r + 4);
    out->text = strings + rd32(r + 8);
    out->text_len = rd32(r + 12);
    out->source = strings + rd32(r + 16);
    out->source_len = rd16(r + 20);
    out->kind = r[22];
    out->flags = r[23];
    out->thread_pos = rd16(r + 24);
    out->thread_len = rd16(r + 26);
    return true;
}

const char *card_pack_status_text(card_pack_status_t status)
{
    switch (status) {
    case CARD_PACK_OK: return "ok";
    case CARD_PACK_EMPTY: return "empty";
    case CARD_PACK_FOREIGN: return "not a card pack";
    case CARD_PACK_BAD_HEADER: return "bad header";
    case CARD_PACK_BAD_VERSION: return "unsupported version";
    case CARD_PACK_TRUNCATED: return "truncated";
    case CARD_PACK_BAD_CHECKSUM: return "checksum mismatch";
    case CARD_PACK_BAD_LAYOUT: return "bad layout";
    }
    return "unknown";
}
