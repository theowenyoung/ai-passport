// main/round_timer.c — see round_timer.h.
#include "round_timer.h"

#include <stdio.h>

static int64_t segment_ms(round_phase_t phase)
{
    return phase == ROUND_SHARE ? ROUND_SHARE_MS : ROUND_TALK_MS;
}

static void enter(round_timer_t *t, round_phase_t phase, int64_t at_ms)
{
    t->phase = phase;
    t->phase_start_ms = at_ms;
}

void round_timer_init(round_timer_t *t)
{
    *t = (round_timer_t){ .phase = ROUND_READY };
}

bool round_timer_running(const round_timer_t *t)
{
    return t->phase == ROUND_SHARE || t->phase == ROUND_TALK;
}

void round_timer_key(round_timer_t *t, round_key_t key, int64_t now_ms)
{
    // The press acts on the segment the screen shows, even if that segment ran
    // out in the few milliseconds since the last tick: Down pressed at the 3:00
    // beep skips the sharing, it does not end the discussion that just began.
    switch (key) {
    case ROUND_KEY_OK:
        // OK only starts; while a round runs it does nothing, so it cannot cut a
        // speaker off. In Over it clears the screen for the next speaker.
        if (t->phase == ROUND_READY) enter(t, ROUND_SHARE, now_ms);
        else if (t->phase == ROUND_OVER) enter(t, ROUND_READY, now_ms);
        break;
    case ROUND_KEY_DOWN:
        // Skipping is silent: the person pressing the key knows.
        if (t->phase == ROUND_SHARE) enter(t, ROUND_TALK, now_ms);
        else if (t->phase != ROUND_READY) enter(t, ROUND_READY, now_ms);
        break;
    case ROUND_KEY_UP:
        if (t->phase != ROUND_READY) enter(t, ROUND_READY, now_ms);
        break;
    }
}

round_sound_t round_timer_tick(round_timer_t *t, int64_t now_ms)
{
    round_sound_t sound = ROUND_SOUND_NONE;
    // Segments end on their own boundary, not on the late tick that notices it,
    // so a slow tick cannot stretch the round. A very late tick may cross both.
    while (round_timer_running(t) && now_ms - t->phase_start_ms >= segment_ms(t->phase)) {
        int64_t end = t->phase_start_ms + segment_ms(t->phase);
        if (t->phase == ROUND_SHARE) {
            enter(t, ROUND_TALK, end);
            sound = ROUND_SOUND_ONE;
        } else {
            enter(t, ROUND_OVER, end);
            sound = ROUND_SOUND_TWO;
        }
    }
    return sound;
}

static uint16_t fill(int64_t elapsed_ms, int64_t total_ms)
{
    if (elapsed_ms <= 0) return 0;
    if (elapsed_ms >= total_ms) return 1000;
    return (uint16_t)(elapsed_ms * 1000 / total_ms);
}

void round_timer_view(const round_timer_t *t, int64_t now_ms, round_view_t *view)
{
    *view = (round_view_t){ .phase = t->phase };
    int64_t elapsed = now_ms - t->phase_start_ms;
    if (elapsed < 0) elapsed = 0;
    switch (t->phase) {
    case ROUND_READY:
        view->seconds = ROUND_SHARE_MS / 1000;
        break;
    case ROUND_SHARE:
    case ROUND_TALK: {
        int64_t left = segment_ms(t->phase) - elapsed;
        if (left < 1) left = 1;
        // Round up: the display reads 3:00 for the whole first second and never
        // shows 0:00, because the segment ends the moment it would.
        view->seconds = (int)((left + 999) / 1000);
        view->warn = left <= ROUND_WARN_MS;
        if (t->phase == ROUND_SHARE) {
            view->share_fill = fill(elapsed, ROUND_SHARE_MS);
        } else {
            view->share_fill = 1000;
            view->talk_fill = fill(elapsed, ROUND_TALK_MS);
        }
        break;
    }
    case ROUND_OVER: {
        int64_t over = elapsed / 1000;
        view->seconds = over > ROUND_OVER_MAX_S ? ROUND_OVER_MAX_S : (int)over;
        view->share_fill = 1000;
        view->talk_fill = 1000;
        break;
    }
    }
}

void round_format_time(int seconds, bool plus, char *out, size_t size)
{
    if (seconds < 0) seconds = 0;
    if (seconds > ROUND_OVER_MAX_S) seconds = ROUND_OVER_MAX_S;
    snprintf(out, size, "%s%d:%02d", plus ? "+" : "", seconds / 60, seconds % 60);
}
