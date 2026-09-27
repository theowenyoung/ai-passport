#include "card_sched.h"

#include "card_pack.h"

#include <string.h>

static const uint8_t BLOB_MAGIC[4] = { 'B', 'C', 'S', 'T' };
#define BLOB_VERSION 1u
#define MAX_YEARS 32u

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void sched_date(uint32_t unix_time, int16_t tz_offset_min, int *year, int *month, int *day)
{
    // Days-to-civil conversion (proleptic Gregorian), valid for any uint32 time.
    int64_t z = ((int64_t)unix_time + (int64_t)tz_offset_min * 60) / 86400 + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp = (5 * doy + 2) / 153;
    int m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = m;
    *year = (int)((int64_t)yoe + era * 400 + (m <= 2));
}

uint16_t sched_year(uint32_t unix_time, int16_t tz_offset_min)
{
    int year, month, day;
    sched_date(unix_time, tz_offset_min, &year, &month, &day);
    return (uint16_t)year;
}

static uint32_t rand_below(card_sched_t *s, uint32_t n)
{
    // xorshift32; modulo bias is irrelevant at these sizes.
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return n ? x % n : 0;
}

static bool is_recent(const card_sched_t *s, uint32_t index)
{
    for (uint32_t i = 0; i < SCHED_RECENT; i++) {
        if (s->recent[i] == (int32_t)index) return true;
    }
    return false;
}

void sched_init(card_sched_t *s, uint32_t count, const sched_meta_t *meta, sched_state_t *state,
                uint16_t *by_id, uint32_t seed)
{
    memset(s, 0, sizeof(*s));
    s->count = count;
    s->meta = meta;
    s->state = state;
    s->by_id = by_id;
    s->rng = seed ? seed : 0x9E3779B9u;
    for (uint32_t i = 0; i < SCHED_RECENT; i++) s->recent[i] = -1;
    memset(state, 0, sizeof(*state) * count);

    // Shell sort of indices by card id, for sched_find().
    for (uint32_t i = 0; i < count; i++) by_id[i] = (uint16_t)i;
    for (uint32_t gap = count / 2; gap > 0; gap /= 2) {
        for (uint32_t i = gap; i < count; i++) {
            uint16_t moving = by_id[i];
            uint32_t j = i;
            while (j >= gap && meta[by_id[j - gap]].id > meta[moving].id) {
                by_id[j] = by_id[j - gap];
                j -= gap;
            }
            by_id[j] = moving;
        }
    }
}

int32_t sched_find(const card_sched_t *s, uint32_t id)
{
    uint32_t lo = 0, hi = s->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t found = s->meta[s->by_id[mid]].id;
        if (found == id) return s->by_id[mid];
        if (found < id) lo = mid + 1; else hi = mid;
    }
    return -1;
}

static bool is_new(const card_sched_t *s, uint32_t i)
{
    return !(s->state[i].flags & (SCHED_SEEN | SCHED_HIDDEN));
}

static int32_t pick_new_balanced(card_sched_t *s)
{
    // Pick a year first, so a busy year does not crowd out the quieter ones,
    // then a card inside it, counting featured cards twice.
    uint16_t years[MAX_YEARS];
    uint32_t year_count = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (!is_new(s, i) || is_recent(s, i)) continue;
        uint32_t k = 0;
        while (k < year_count && years[k] != s->meta[i].year) k++;
        if (k == year_count && year_count < MAX_YEARS) years[year_count++] = s->meta[i].year;
    }
    if (!year_count) return -1;
    uint16_t year = years[rand_below(s, year_count)];

    uint32_t total = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (is_new(s, i) && !is_recent(s, i) && s->meta[i].year == year) total += s->meta[i].featured ? 2 : 1;
    }
    uint32_t target = rand_below(s, total);
    for (uint32_t i = 0; i < s->count; i++) {
        if (!is_new(s, i) || is_recent(s, i) || s->meta[i].year != year) continue;
        uint32_t weight = s->meta[i].featured ? 2 : 1;
        if (target < weight) return (int32_t)i;
        target -= weight;
    }
    return -1;
}

int32_t sched_next(card_sched_t *s)
{
    uint32_t due_count = 0, new_count = 0;
    int32_t fresh = -1, soonest = -1, soonest_recent = -1, new_recent = -1;
    for (uint32_t i = 0; i < s->count; i++) {
        const sched_state_t *st = &s->state[i];
        if (st->flags & SCHED_HIDDEN) continue;
        bool recent = is_recent(s, i);
        if (!(st->flags & SCHED_SEEN)) {
            if (recent) { new_recent = (int32_t)i; continue; }
            new_count++;
            // Posts added since the last save come first, oldest first.
            if (s->fresh_after && s->meta[i].published_at > s->fresh_after &&
                (fresh < 0 || s->meta[i].published_at < s->meta[fresh].published_at)) {
                fresh = (int32_t)i;
            }
            continue;
        }
        if (recent) {
            if (soonest_recent < 0 || st->due < s->state[soonest_recent].due) soonest_recent = (int32_t)i;
            continue;
        }
        if (st->due <= s->tick) due_count++;
        if (soonest < 0 || st->due < s->state[soonest].due) soonest = (int32_t)i;
    }
    if (fresh >= 0) return fresh;

    if (new_count && (!due_count || rand_below(s, SCHED_NEW_ONE_IN) == 0)) return pick_new_balanced(s);
    if (due_count) {
        uint32_t target = rand_below(s, due_count);
        for (uint32_t i = 0; i < s->count; i++) {
            const sched_state_t *st = &s->state[i];
            if ((st->flags & (SCHED_SEEN | SCHED_HIDDEN)) != SCHED_SEEN || is_recent(s, i) || st->due > s->tick) continue;
            if (target-- == 0) return (int32_t)i;
        }
    }
    if (soonest >= 0) return soonest;
    // Fewer cards than the recent window: repeating is unavoidable.
    if (new_recent >= 0) return new_recent;
    return soonest_recent;
}

void sched_shown(card_sched_t *s, uint32_t index)
{
    if (index >= s->count) return;
    sched_state_t *st = &s->state[index];
    s->tick++;
    if (st->views < UINT8_MAX) st->views++;
    uint32_t interval;
    if (st->flags & SCHED_KEPT) {
        interval = (st->flags & SCHED_SEEN) && st->interval ? (uint32_t)st->interval * 2 : SCHED_KEEP_FIRST;
        if (interval > SCHED_KEEP_MAX) interval = SCHED_KEEP_MAX;
    } else {
        interval = (st->flags & SCHED_SEEN) && st->interval ? (uint32_t)st->interval * 5 / 2 : SCHED_VIEW_FIRST;
        if (interval > SCHED_VIEW_MAX) interval = SCHED_VIEW_MAX;
    }
    st->interval = (uint16_t)interval;
    st->flags |= SCHED_SEEN;
    st->due = s->tick + interval;
    s->recent[s->recent_next] = (int32_t)index;
    s->recent_next = (s->recent_next + 1) % SCHED_RECENT;
}

void sched_set_kept(card_sched_t *s, uint32_t index, bool kept)
{
    if (index >= s->count) return;
    sched_state_t *st = &s->state[index];
    if (kept) {
        st->flags |= SCHED_KEPT | SCHED_SEEN;
        st->interval = SCHED_KEEP_FIRST;
    } else {
        st->flags &= (uint8_t)~SCHED_KEPT;
        st->interval = SCHED_VIEW_FIRST;
    }
    st->due = s->tick + st->interval;
}

void sched_hide(card_sched_t *s, uint32_t index)
{
    if (index < s->count) s->state[index].flags |= SCHED_HIDDEN;
}

uint32_t sched_unhide_all(card_sched_t *s)
{
    uint32_t restored = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->state[i].flags & SCHED_HIDDEN) {
            s->state[i].flags &= (uint8_t)~SCHED_HIDDEN;
            restored++;
        }
    }
    return restored;
}

sched_stats_t sched_stats(const card_sched_t *s)
{
    sched_stats_t stats = { 0 };
    for (uint32_t i = 0; i < s->count; i++) {
        uint8_t f = s->state[i].flags;
        stats.seen += (f & SCHED_SEEN) ? 1 : 0;
        stats.kept += (f & SCHED_KEPT) ? 1 : 0;
        stats.hidden += (f & SCHED_HIDDEN) ? 1 : 0;
    }
    return stats;
}

size_t sched_blob_size(const card_sched_t *s)
{
    size_t entries = 0;
    for (uint32_t i = 0; i < s->count; i++) entries += s->state[i].flags ? 1 : 0;
    return SCHED_BLOB_HEADER + entries * SCHED_BLOB_ENTRY;
}

size_t sched_save(const card_sched_t *s, uint32_t seq, uint32_t fresh_after, uint8_t *buf, size_t cap)
{
    size_t size = sched_blob_size(s);
    if (cap < size) return 0;
    uint8_t *p = buf + SCHED_BLOB_HEADER;
    uint32_t entries = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        const sched_state_t *st = &s->state[i];
        if (!st->flags) continue;
        wr32(p, s->meta[i].id);
        wr32(p + 4, st->due);
        wr16(p + 8, st->interval);
        p[10] = st->flags;
        p[11] = st->views;
        p += SCHED_BLOB_ENTRY;
        entries++;
    }
    memcpy(buf, BLOB_MAGIC, sizeof(BLOB_MAGIC));
    wr16(buf + 4, BLOB_VERSION);
    wr16(buf + 6, 0);
    wr32(buf + 8, seq);
    wr32(buf + 12, s->tick);
    wr32(buf + 16, fresh_after);
    wr32(buf + 20, entries);
    wr32(buf + 24, card_crc32(buf + SCHED_BLOB_HEADER, entries * SCHED_BLOB_ENTRY));
    wr32(buf + 28, card_crc32(buf, SCHED_BLOB_HEADER - 4));
    return size;
}

bool sched_blob_check(const uint8_t *buf, size_t len, uint32_t *seq)
{
    if (len < SCHED_BLOB_HEADER || memcmp(buf, BLOB_MAGIC, sizeof(BLOB_MAGIC)) != 0) return false;
    if (card_crc32(buf, SCHED_BLOB_HEADER - 4) != rd32(buf + 28) || rd16(buf + 4) != BLOB_VERSION) return false;
    uint32_t entries = rd32(buf + 20);
    if (entries > (len - SCHED_BLOB_HEADER) / SCHED_BLOB_ENTRY) return false;
    if (card_crc32(buf + SCHED_BLOB_HEADER, entries * SCHED_BLOB_ENTRY) != rd32(buf + 24)) return false;
    if (seq) *seq = rd32(buf + 8);
    return true;
}

bool sched_load(card_sched_t *s, const uint8_t *buf, size_t len)
{
    if (!sched_blob_check(buf, len, NULL)) return false;
    s->tick = rd32(buf + 12);
    s->fresh_after = rd32(buf + 16);
    uint32_t entries = rd32(buf + 20);
    const uint8_t *p = buf + SCHED_BLOB_HEADER;
    for (uint32_t i = 0; i < entries; i++, p += SCHED_BLOB_ENTRY) {
        int32_t index = sched_find(s, rd32(p));
        if (index < 0) continue;
        sched_state_t *st = &s->state[index];
        st->due = rd32(p + 4);
        st->interval = rd16(p + 8);
        st->flags = p[10] & (SCHED_SEEN | SCHED_KEPT | SCHED_HIDDEN);
        st->views = p[11];
    }
    return true;
}
