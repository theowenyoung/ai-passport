// main/timer_ui.h — the timer's single screen. Call both functions in the LVGL
// task or while holding bsp_lvgl_lock().
#pragma once

#include <stdbool.h>

#include "round_timer.h"

void timer_ui_create(void);

// Redraw what changed since the last call. battery_soc is -1 when unknown;
// sound_ok false replaces the volume row with a warning.
void timer_ui_render(const round_view_t *view, int battery_soc, bool sound_ok);
