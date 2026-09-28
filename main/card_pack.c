#include "card_pack.h"

#include <string.h>

static const uint8_t MAGIC[4] = { 'B', 'L', 'G', 'C' };
#define PACK_VERSION 1u

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Byte-wise table for the reflected polynomial 0xEDB88320 (zlib's CRC-32).
// The pack is checked on every boot, including each wake from deep sleep, and
// the bit-by-bit loop took about a quarter of a second for 250 KB.
static const uint32_t CRC_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

uint32_t card_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ data[i]) & 0xFFu];
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
