// main/round_timer.h — the speaker timer's state machine, free of ESP-IDF and LVGL
// so the host tests can drive it with a fake clock.
//
// One round is 3 minutes of sharing followed by 2 minutes of discussion. Each key
// has one meaning: OK starts a round from Ready, Down jumps to the next segment
// (from discussion it ends the round), Up resets to Ready. The segments also end
// by themselves, with one beep and then two; after that the screen counts the
// overtime until a key sends it back to Ready. Keys never beep.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUND_SHARE_MS        (3 * 60 * 1000)
#define ROUND_TALK_MS         (2 * 60 * 1000)
#define ROUND_WARN_MS         (30 * 1000)   // the digits turn yellow for the last 30 s
#define ROUND_OVER_MAX_S      (9 * 60 + 59)  // "+9:59" is the widest that fits

typedef enum {
    ROUND_READY = 0,
    ROUND_SHARE,
    ROUND_TALK,
    ROUND_OVER,
} round_phase_t;

typedef enum {
    ROUND_KEY_OK = 0,
    ROUND_KEY_UP,
    ROUND_KEY_DOWN,
} round_key_t;

typedef enum {
    ROUND_SOUND_NONE = 0,
    ROUND_SOUND_ONE,  // sharing is over
    ROUND_SOUND_TWO,  // discussion is over
} round_sound_t;

typedef struct {
    round_phase_t phase;
    int64_t phase_start_ms;  // when the current segment (or the overtime) began
} round_timer_t;

typedef struct {
    round_phase_t phase;
    int seconds;          // remaining in the segment, overtime in Over, 180 in Ready
    bool warn;            // last ROUND_WARN_MS of a segment
    uint16_t share_fill;  // progress of the sharing segment, 0..1000
    uint16_t talk_fill;   // progress of the discussion segment, 0..1000
} round_view_t;

void round_timer_init(round_timer_t *t);

// Feed one key press, before the tick for the same moment.
void round_timer_key(round_timer_t *t, round_key_t key, int64_t now_ms);

// Advance time: ends segments that ran out. Call it at least every 100 ms while a
// round runs. Returns the sound to play.
round_sound_t round_timer_tick(round_timer_t *t, int64_t now_ms);

void round_timer_view(const round_timer_t *t, int64_t now_ms, round_view_t *view);

// True while a segment counts down; the device must not sleep then.
bool round_timer_running(const round_timer_t *t);

// "m:ss", or "+m:ss" when plus is set, capped at ROUND_OVER_MAX_S. Needs 8 bytes.
void round_format_time(int seconds, bool plus, char *out, size_t size);
