// main/timer_sound.h — the timer's beeps, played by a worker task so key handling
// and the clock never wait for audio.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "round_timer.h"

// Start the worker. bsp_audio_init() must have succeeded first.
esp_err_t timer_sound_start(void);

// Queue a sound. Never blocks; drops the sound if the worker is not running or
// already has a backlog.
void timer_sound_play(round_sound_t sound);

// Stop the worker before deep sleep: it finishes the sound it is playing, then
// exits. Returns false if it did not stop in time.
bool timer_sound_stop(int timeout_ms);
