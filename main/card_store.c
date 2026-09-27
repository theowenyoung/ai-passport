#include "card_store.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "card_store";

// Custom data subtypes; keep in step with partitions.csv.
#define CARDS_SUBTYPE 0x40
#define STATE_SUBTYPE 0x41
#define SECTOR        4096u

static esp_partition_mmap_handle_t s_pack_map;
static bool s_pack_mapped;
static int s_slot = -1;     // slot holding the newest valid copy
static uint32_t s_seq;

card_pack_status_t card_store_open_pack(card_pack_t *pack)
{
    memset(pack, 0, sizeof(*pack));
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, CARDS_SUBTYPE, "cards");
    if (!part) {
        ESP_LOGE(TAG, "no 'cards' partition; check partitions.csv");
        return CARD_PACK_EMPTY;
    }
    uint8_t header[CARD_PACK_HEADER_SIZE];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK) return CARD_PACK_TRUNCATED;
    uint32_t total = 0;
    card_pack_status_t status = card_pack_peek(header, sizeof(header), &total);
    if (status != CARD_PACK_OK) return status;
    if (total > part->size) return CARD_PACK_TRUNCATED;

    if (s_pack_mapped) {
        esp_partition_munmap(s_pack_map);
        s_pack_mapped = false;
    }
    const void *data = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, total, ESP_PARTITION_MMAP_DATA, &data, &s_pack_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mapping %u bytes of 'cards' failed: %s", (unsigned)total, esp_err_to_name(err));
        return CARD_PACK_TRUNCATED;
    }
    s_pack_mapped = true;
    status = card_pack_open(pack, data, total);
    if (status != CARD_PACK_OK) {
        esp_partition_munmap(s_pack_map);
        s_pack_mapped = false;
    }
    return status;
}

static const esp_partition_t *state_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, STATE_SUBTYPE, "cardstat");
    if (!part) ESP_LOGE(TAG, "no 'cardstat' partition; progress cannot be kept");
    return part;
}

// Read one slot into a new buffer when it holds a valid copy.
static uint8_t *read_slot(const esp_partition_t *part, int slot, size_t *len, uint32_t *seq)
{
    uint32_t slot_size = part->size / 2;
    uint32_t offset = (uint32_t)slot * slot_size;
    uint8_t header[SCHED_BLOB_HEADER];
    if (esp_partition_read(part, offset, header, sizeof(header)) != ESP_OK) return NULL;
    uint32_t entries = (uint32_t)header[20] | ((uint32_t)header[21] << 8) |
                       ((uint32_t)header[22] << 16) | ((uint32_t)header[23] << 24);
    if (memcmp(header, "BCST", 4) != 0 || entries > (slot_size - SCHED_BLOB_HEADER) / SCHED_BLOB_ENTRY) {
        return NULL;
    }
    size_t size = SCHED_BLOB_HEADER + entries * SCHED_BLOB_ENTRY;
    uint8_t *buf = malloc(size);
    if (!buf) return NULL;
    if (esp_partition_read(part, offset, buf, size) != ESP_OK || !sched_blob_check(buf, size, seq)) {
        free(buf);
        return NULL;
    }
    *len = size;
    return buf;
}

bool card_store_load(card_sched_t *s)
{
    const esp_partition_t *part = state_partition();
    if (!part) return false;
    uint8_t *best = NULL;
    size_t best_len = 0;
    for (int slot = 0; slot < 2; slot++) {
        size_t len = 0;
        uint32_t seq = 0;
        uint8_t *buf = read_slot(part, slot, &len, &seq);
        if (!buf) continue;
        if (!best || (int32_t)(seq - s_seq) > 0) {
            free(best);
            best = buf;
            best_len = len;
            s_seq = seq;
            s_slot = slot;
        } else {
            free(buf);
        }
    }
    bool ok = best && sched_load(s, best, best_len);
    free(best);
    if (ok) ESP_LOGI(TAG, "progress loaded from slot %d (seq %u)", s_slot, (unsigned)s_seq);
    else ESP_LOGI(TAG, "no saved progress");
    return ok;
}

esp_err_t card_store_save(const card_sched_t *s, uint32_t fresh_after)
{
    const esp_partition_t *part = state_partition();
    if (!part) return ESP_ERR_NOT_FOUND;
    uint32_t slot_size = part->size / 2;
    size_t size = sched_blob_size(s);
    if (size > slot_size) return ESP_ERR_NO_MEM;
    uint8_t *buf = malloc(size);
    if (!buf) return ESP_ERR_NO_MEM;
    uint32_t seq = s_seq + 1;
    sched_save(s, seq, fresh_after, buf, size);

    int slot = s_slot == 0 ? 1 : 0;
    uint32_t offset = (uint32_t)slot * slot_size;
    uint32_t erase = (uint32_t)((size + SECTOR - 1) / SECTOR * SECTOR);
    esp_err_t err = esp_partition_erase_range(part, offset, erase);
    if (err == ESP_OK) err = esp_partition_write(part, offset, buf, size);
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving progress failed: %s", esp_err_to_name(err));
        return err;
    }
    s_slot = slot;
    s_seq = seq;
    return ESP_OK;
}
