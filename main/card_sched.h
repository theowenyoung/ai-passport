// main/card_sched.h — which card comes next, and what the device remembers.
//
// A simplified Anki where "cards viewed" replaces calendar days, because the
// device has no reliable date. Pure C with an injected random source, so every
// rule is covered by host tests. See docs/blog-cards/design.md.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHED_VIEW_FIRST   200u    // a first view returns after this many cards
#define SCHED_VIEW_MAX     20000u
#define SCHED_KEEP_FIRST   30u     // a kept card returns much sooner
#define SCHED_KEEP_MAX     1000u
#define SCHED_RECENT       8u      // never repeat one of the last N cards when avoidable
#define SCHED_NEW_ONE_IN   3u      // with both due and new cards, one pick in N is new

enum {
    SCHED_SEEN   = 1u << 0,
    SCHED_KEPT   = 1u << 1,
    SCHED_HIDDEN = 1u << 2,
};

typedef struct {
    uint32_t id;
    uint32_t published_at;
    uint16_t year;             // local calendar year, for balancing new cards
    uint8_t featured;
} sched_meta_t;

typedef struct {
    uint32_t due;              // global tick at which the card may return
    uint16_t interval;         // in cards viewed
    uint8_t flags;
    uint8_t views;             // saturating
} sched_state_t;

typedef struct {
    uint32_t count;
    const sched_meta_t *meta;  // one per pack card, owned by the caller
    sched_state_t *state;      // one per pack card, owned by the caller
    uint16_t *by_id;           // indices sorted by id, owned by the caller
    uint32_t tick;
    uint32_t fresh_after;      // newest publish time at the last save
    uint32_t rng;
    int32_t recent[SCHED_RECENT];
    uint32_t recent_next;
} card_sched_t;

typedef struct {
    uint32_t seen;
    uint32_t kept;
    uint32_t hidden;
} sched_stats_t;

// Local calendar year of a Unix time shifted by `tz_offset_min`.
uint16_t sched_year(uint32_t unix_time, int16_t tz_offset_min);

// Local date as year/month/day.
void sched_date(uint32_t unix_time, int16_t tz_offset_min, int *year, int *month, int *day);

// Reset to "nothing seen". `by_id` must hold `count` entries; count <= 65535.
void sched_init(card_sched_t *s, uint32_t count, const sched_meta_t *meta, sched_state_t *state,
                uint16_t *by_id, uint32_t seed);

// Next pack index to show, or -1 when every card is hidden.
int32_t sched_next(card_sched_t *s);

// Record that `index` was shown: advance the tick and reschedule it.
void sched_shown(card_sched_t *s, uint32_t index);

void sched_set_kept(card_sched_t *s, uint32_t index, bool kept);
void sched_hide(card_sched_t *s, uint32_t index);
uint32_t sched_unhide_all(card_sched_t *s);
sched_stats_t sched_stats(const card_sched_t *s);
int32_t sched_find(const card_sched_t *s, uint32_t id);

// Saved state: a 32-byte header plus 12 bytes per remembered card.
#define SCHED_BLOB_HEADER 32u
#define SCHED_BLOB_ENTRY  12u

// Bytes needed to save the current state.
size_t sched_blob_size(const card_sched_t *s);

// Write the state with sequence number `seq`; returns bytes written or 0 when
// `cap` is too small. `fresh_after` records the newest publish time in the pack.
size_t sched_save(const card_sched_t *s, uint32_t seq, uint32_t fresh_after, uint8_t *buf, size_t cap);

// Validate a saved blob and report its sequence number without applying it.
bool sched_blob_check(const uint8_t *buf, size_t len, uint32_t *seq);

// Apply a validated blob. Entries for cards no longer in the pack are dropped.
bool sched_load(card_sched_t *s, const uint8_t *buf, size_t len);
