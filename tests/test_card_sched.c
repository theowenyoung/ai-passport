#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "card_sched.h"

#define N 40

static sched_meta_t meta[N];
static sched_state_t state[N];
static uint16_t by_id[N];

static void setup(card_sched_t *s, uint32_t count, uint32_t seed)
{
    for (uint32_t i = 0; i < count; i++) {
        // Ids deliberately out of order, to exercise sched_find().
        meta[i] = (sched_meta_t){ .id = 1000u + (i * 7919u) % 997u, .published_at = 1600000000u + i,
                                  .year = 2024, .featured = 0 };
    }
    sched_init(s, count, meta, state, by_id, seed);
}

static void test_dates(void)
{
    int y, m, d;
    sched_date(0, 0, &y, &m, &d);
    assert(y == 1970 && m == 1 && d == 1);
    sched_date(951782400u, 0, &y, &m, &d);  // 2000-02-29 00:00 UTC
    assert(y == 2000 && m == 2 && d == 29);
    sched_date(1700000000u, 0, &y, &m, &d);  // 2023-11-14 22:13 UTC
    assert(y == 2023 && m == 11 && d == 14);
    sched_date(1700000000u, 480, &y, &m, &d);  // next morning in UTC+8
    assert(y == 2023 && m == 11 && d == 15);
    assert(sched_year(1704063600u, 0) == 2023 && sched_year(1704063600u, 60) == 2024);  // 23:00 UTC on Dec 31
}

static void test_find(void)
{
    card_sched_t s;
    setup(&s, N, 1);
    for (uint32_t i = 0; i < N; i++) assert(sched_find(&s, meta[i].id) == (int32_t)i);
    assert(sched_find(&s, 5) == -1);
}

static void test_every_card_before_any_repeat(void)
{
    card_sched_t s;
    setup(&s, N, 7);
    bool seen[N] = { false };
    for (int k = 0; k < N; k++) {
        int32_t i = sched_next(&s);
        assert(i >= 0 && !seen[i]);
        seen[i] = true;
        sched_shown(&s, (uint32_t)i);
    }
    assert(sched_stats(&s).seen == N);
    // Nothing is due or new now; the next pick is the soonest-due card, never a recent one.
    int32_t again = sched_next(&s);
    assert(again >= 0);
    for (uint32_t r = 0; r < SCHED_RECENT; r++) assert(s.recent[r] != again);
}

static void test_intervals(void)
{
    card_sched_t s;
    setup(&s, N, 3);
    sched_shown(&s, 0);
    assert(state[0].interval == SCHED_VIEW_FIRST && state[0].due == s.tick + SCHED_VIEW_FIRST);
    sched_shown(&s, 0);
    assert(state[0].interval == 500);
    sched_shown(&s, 0);
    assert(state[0].interval == 1250);
    for (int k = 0; k < 10; k++) sched_shown(&s, 0);
    assert(state[0].interval == SCHED_VIEW_MAX && state[0].views == 13);

    sched_set_kept(&s, 1, true);
    assert((state[1].flags & (SCHED_KEPT | SCHED_SEEN)) == (SCHED_KEPT | SCHED_SEEN));
    assert(state[1].interval == SCHED_KEEP_FIRST && state[1].due == s.tick + SCHED_KEEP_FIRST);
    sched_shown(&s, 1);
    assert(state[1].interval == 60);
    for (int k = 0; k < 10; k++) sched_shown(&s, 1);
    assert(state[1].interval == SCHED_KEEP_MAX);
    sched_set_kept(&s, 1, false);
    assert(!(state[1].flags & SCHED_KEPT) && state[1].interval == SCHED_VIEW_FIRST);
}

static void test_kept_card_returns_soon(void)
{
    card_sched_t s;
    setup(&s, N, 11);
    for (int k = 0; k < N; k++) sched_shown(&s, (uint32_t)sched_next(&s));
    sched_set_kept(&s, 5, true);
    int found_after = -1;
    for (int k = 0; k < 60 && found_after < 0; k++) {
        int32_t i = sched_next(&s);
        if (i == 5) found_after = k;
        sched_shown(&s, (uint32_t)i);
    }
    assert(found_after >= 0 && found_after <= (int)(SCHED_KEEP_FIRST + SCHED_RECENT));
}

static void test_hidden(void)
{
    card_sched_t s;
    setup(&s, 12, 5);
    for (uint32_t i = 0; i < 12; i++) if (i != 4) sched_hide(&s, i);
    for (int k = 0; k < 20; k++) {
        int32_t i = sched_next(&s);
        assert(i == 4);  // the only visible card, even though it becomes recent
        sched_shown(&s, (uint32_t)i);
    }
    sched_hide(&s, 4);
    assert(sched_next(&s) == -1);
    assert(sched_stats(&s).hidden == 12 && sched_unhide_all(&s) == 12);
    assert(sched_next(&s) >= 0);
}

static void test_year_balance(void)
{
    card_sched_t s;
    setup(&s, N, 9);
    for (uint32_t i = 0; i < N; i++) meta[i].year = i < 4 ? 2020 : 2026;  // 4 old, 36 new
    int old = 0;
    for (int k = 0; k < 8; k++) {
        int32_t i = sched_next(&s);
        if (meta[i].year == 2020) old++;
        sched_shown(&s, (uint32_t)i);
    }
    assert(old >= 2);  // an even split between years, not 4/40 of the draws
}

static void test_fresh_cards_first(void)
{
    card_sched_t s;
    setup(&s, N, 13);
    s.fresh_after = meta[30].published_at;
    for (uint32_t i = 31; i < N; i++) {
        int32_t pick = sched_next(&s);
        assert(pick == (int32_t)i);  // newer than the last save, oldest first
        sched_shown(&s, (uint32_t)pick);
    }
    assert(meta[sched_next(&s)].published_at <= s.fresh_after);
}

static void test_save_and_load(void)
{
    card_sched_t s;
    setup(&s, N, 17);
    for (int k = 0; k < 10; k++) sched_shown(&s, (uint32_t)sched_next(&s));
    sched_set_kept(&s, 3, true);
    sched_hide(&s, 7);
    uint8_t blob[SCHED_BLOB_HEADER + N * SCHED_BLOB_ENTRY];
    size_t size = sched_save(&s, 42, 1234, blob, sizeof(blob));
    assert(size == sched_blob_size(&s) && size > SCHED_BLOB_HEADER);
    assert(sched_save(&s, 42, 1234, blob, size - 1) == 0);
    uint32_t seq = 0;
    assert(sched_blob_check(blob, size, &seq) && seq == 42);

    sched_state_t before[N];
    memcpy(before, state, sizeof(before));
    uint32_t tick = s.tick;
    uint32_t kept_id = meta[3].id, hidden_id = meta[7].id;

    // A new pack: the order changes, card 3 moves to the end, card 7 disappears.
    sched_meta_t old_meta[N];
    memcpy(old_meta, meta, sizeof(old_meta));
    for (uint32_t i = 0; i < N; i++) meta[i] = old_meta[N - 1 - i];
    meta[N - 1 - 7].id = 99999;  // replaced by an unseen card
    card_sched_t t;
    sched_init(&t, N, meta, state, by_id, 1);
    assert(sched_load(&t, blob, size));
    assert(t.tick == tick && t.fresh_after == 1234);
    int32_t kept = sched_find(&t, kept_id);
    assert(kept >= 0 && (state[kept].flags & SCHED_KEPT) && state[kept].interval == before[3].interval);
    assert(sched_find(&t, hidden_id) == -1);
    assert(state[sched_find(&t, 99999)].flags == 0);

    blob[9] ^= 1;
    assert(!sched_blob_check(blob, size, NULL) && !sched_load(&t, blob, size));
    blob[9] ^= 1;
    blob[size - 1] ^= 1;
    assert(!sched_blob_check(blob, size, NULL));
    blob[size - 1] ^= 1;
    assert(!sched_blob_check(blob, SCHED_BLOB_HEADER - 1, NULL));
}

int main(void)
{
    test_dates();
    test_find();
    test_every_card_before_any_repeat();
    test_intervals();
    test_kept_card_returns_soon();
    test_hidden();
    test_year_balance();
    test_fresh_cards_first();
    test_save_and_load();
    puts("card scheduler tests: PASS");
    return 0;
}
