// Host tests for the speaker timer's state machine (main/round_timer.c), with a fake clock.
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "round_timer.h"

static round_view_t view_at(const round_timer_t *t, int64_t now)
{
    round_view_t v;
    round_timer_view(t, now, &v);
    return v;
}

// A key press followed by the loop's tick at the same moment, as main.c does.
static round_sound_t press(round_timer_t *t, round_key_t key, int64_t at)
{
    round_timer_key(t, key, at);
    return round_timer_tick(t, at);
}

static void test_format(void)
{
    char buf[8];
    round_format_time(180, false, buf, sizeof buf);
    assert(strcmp(buf, "3:00") == 0);
    round_format_time(9, false, buf, sizeof buf);
    assert(strcmp(buf, "0:09") == 0);
    round_format_time(75, true, buf, sizeof buf);
    assert(strcmp(buf, "+1:15") == 0);
    round_format_time(-5, false, buf, sizeof buf);
    assert(strcmp(buf, "0:00") == 0);
    round_format_time(100000, true, buf, sizeof buf);
    assert(strcmp(buf, "+9:59") == 0);
}

static void test_ready(void)
{
    round_timer_t t;
    round_timer_init(&t);
    round_view_t v = view_at(&t, 0);
    assert(v.phase == ROUND_READY && v.seconds == 180 && !v.warn);
    assert(v.share_fill == 0 && v.talk_fill == 0);
    assert(!round_timer_running(&t));
    // Time alone never leaves Ready, and Up/Down do nothing there.
    assert(round_timer_tick(&t, 10LL * 60 * 60 * 1000) == ROUND_SOUND_NONE);
    assert(press(&t, ROUND_KEY_UP, 1000) == ROUND_SOUND_NONE);
    assert(press(&t, ROUND_KEY_DOWN, 2000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
}

static void test_full_round(void)
{
    round_timer_t t;
    round_timer_init(&t);
    // OK starts the round on the press, silently.
    const int64_t start = 1000;
    assert(press(&t, ROUND_KEY_OK, start) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_SHARE && round_timer_running(&t));

    round_view_t v = view_at(&t, start);
    assert(v.seconds == 180 && !v.warn && v.share_fill == 0);
    assert(view_at(&t, start + 999).seconds == 180);
    assert(view_at(&t, start + 1000).seconds == 179);
    // Yellow from 0:30 on, never before.
    v = view_at(&t, start + ROUND_SHARE_MS - 30001);
    assert(v.seconds == 31 && !v.warn);
    v = view_at(&t, start + ROUND_SHARE_MS - 30000);
    assert(v.seconds == 30 && v.warn);
    v = view_at(&t, start + ROUND_SHARE_MS - 1);
    assert(v.seconds == 1 && v.share_fill == 999);

    // OK during the round does nothing.
    assert(press(&t, ROUND_KEY_OK, start + 60000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_SHARE && t.phase_start_ms == start);

    // 3:00 is up: one beep, and the discussion starts at the boundary.
    assert(round_timer_tick(&t, start + ROUND_SHARE_MS - 1) == ROUND_SOUND_NONE);
    assert(round_timer_tick(&t, start + ROUND_SHARE_MS) == ROUND_SOUND_ONE);
    assert(round_timer_tick(&t, start + ROUND_SHARE_MS + 10) == ROUND_SOUND_NONE);
    v = view_at(&t, start + ROUND_SHARE_MS);
    assert(v.phase == ROUND_TALK && v.seconds == 120 && !v.warn);
    assert(v.share_fill == 1000 && v.talk_fill == 0);
    v = view_at(&t, start + ROUND_SHARE_MS + 60000);
    assert(v.seconds == 60 && v.talk_fill == 500);
    assert(view_at(&t, start + ROUND_SHARE_MS + ROUND_TALK_MS - 30000).warn);

    // 5:00 is up: two beeps, then the overtime counts up.
    const int64_t end = start + ROUND_SHARE_MS + ROUND_TALK_MS;
    assert(round_timer_tick(&t, end) == ROUND_SOUND_TWO);
    assert(!round_timer_running(&t));
    v = view_at(&t, end);
    assert(v.phase == ROUND_OVER && v.seconds == 0 && !v.warn);
    assert(v.share_fill == 1000 && v.talk_fill == 1000);
    assert(view_at(&t, end + 15500).seconds == 15);
    assert(view_at(&t, end + 1000LL * 60 * 60 * 3).seconds == ROUND_OVER_MAX_S);
    assert(round_timer_tick(&t, end + 60000) == ROUND_SOUND_NONE);

    // OK in Over goes back to Ready; the next OK starts a new round.
    assert(press(&t, ROUND_KEY_OK, end + 70000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
    assert(press(&t, ROUND_KEY_OK, end + 80000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_SHARE && t.phase_start_ms == end + 80000);
}

static void test_late_tick_keeps_boundaries(void)
{
    round_timer_t t;
    round_timer_init(&t);
    press(&t, ROUND_KEY_OK, 0);
    // A single tick long after both segments: lands in Over with two beeps, and
    // the overtime counts from 5:00 after the start, not from the late tick.
    assert(round_timer_tick(&t, ROUND_SHARE_MS + ROUND_TALK_MS + 7000) == ROUND_SOUND_TWO);
    assert(t.phase == ROUND_OVER);
    assert(view_at(&t, ROUND_SHARE_MS + ROUND_TALK_MS + 7000).seconds == 7);
}

static void test_down_skips_silently(void)
{
    round_timer_t t;
    round_timer_init(&t);
    press(&t, ROUND_KEY_OK, 0);
    // Down in sharing: the discussion starts now, with its full two minutes.
    assert(press(&t, ROUND_KEY_DOWN, 20000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_TALK);
    round_view_t v = view_at(&t, 20000);
    assert(v.seconds == 120 && v.share_fill == 1000 && v.talk_fill == 0);
    assert(view_at(&t, 20000 + ROUND_TALK_MS - 1).seconds == 1);
    // No beep for the skipped sharing; the discussion runs on.
    assert(round_timer_tick(&t, 20000 + ROUND_TALK_MS - 1) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_TALK);

    // Down in the discussion ends the round, back to Ready, silently.
    assert(press(&t, ROUND_KEY_DOWN, 20000 + ROUND_TALK_MS - 1) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
    assert(round_timer_tick(&t, 10LL * 60 * 1000) == ROUND_SOUND_NONE);

    // Down in Over goes back to Ready too.
    press(&t, ROUND_KEY_OK, 1000000);
    round_timer_tick(&t, 1000000 + ROUND_SHARE_MS + ROUND_TALK_MS);
    assert(t.phase == ROUND_OVER);
    assert(press(&t, ROUND_KEY_DOWN, 1400000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
}

static void test_down_at_the_beep_skips_the_sharing(void)
{
    // The 3:00 boundary passed a few milliseconds before the press, but no tick has
    // run since: the screen still showed sharing, so Down skips the sharing.
    round_timer_t t;
    round_timer_init(&t);
    press(&t, ROUND_KEY_OK, 0);
    assert(round_timer_tick(&t, ROUND_SHARE_MS - 20) == ROUND_SOUND_NONE);
    assert(press(&t, ROUND_KEY_DOWN, ROUND_SHARE_MS + 10) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_TALK && t.phase_start_ms == ROUND_SHARE_MS + 10);
}

static void test_up_resets_from_anywhere(void)
{
    round_timer_t t;
    round_timer_init(&t);
    press(&t, ROUND_KEY_OK, 0);
    assert(press(&t, ROUND_KEY_UP, 5000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
    assert(view_at(&t, 5000).seconds == 180);

    press(&t, ROUND_KEY_OK, 10000);
    press(&t, ROUND_KEY_DOWN, 20000);
    assert(t.phase == ROUND_TALK);
    assert(press(&t, ROUND_KEY_UP, 30000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);

    press(&t, ROUND_KEY_OK, 40000);
    round_timer_tick(&t, 40000 + ROUND_SHARE_MS + ROUND_TALK_MS);
    assert(t.phase == ROUND_OVER);
    assert(press(&t, ROUND_KEY_UP, 400000) == ROUND_SOUND_NONE);
    assert(t.phase == ROUND_READY);
    // A reset round never beeps later.
    assert(round_timer_tick(&t, 2000000) == ROUND_SOUND_NONE);
}

int main(void)
{
    test_format();
    test_ready();
    test_full_round();
    test_late_tick_keeps_boundaries();
    test_down_skips_silently();
    test_down_at_the_beep_skips_the_sharing();
    test_up_resets_from_anywhere();
    printf("round_timer: PASS\n");
    return 0;
}
