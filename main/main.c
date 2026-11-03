// main/main.c — speaker timer firmware: bring up the board, then time one round per
// speaker (3 minutes of sharing, then 2 of discussion; see round_timer.h).
//
// Keys, on the press: OK starts a round, Down jumps to the next segment, Up
// resets to Ready. The beep volume is fixed.
//
// Power: while a round runs the screen stays on and the chip never sleeps. In
// Ready or Over, ten minutes without a key turn the screen off and
// put the chip in deep sleep; any key wakes it into Ready. With a USB host attached
// the chip stays awake (sleep would drop the serial port) and only the screen goes
// off; the first key then only turns it back on.
//
// The baseline hardware-test demo sources (demo_*.c, ui_pixel*.c) stay in this
// directory for the upstream host tests but are not built into this firmware.
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "round_timer.h"
#include "timer_sound.h"
#include "timer_ui.h"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "timer";

#define INPUT_QUEUE_DEPTH     8
#define LOOP_MS               50
#define LOCK_MS               100
#define IDLE_SLEEP_MS         (10 * 60 * 1000)
#define SLEEP_RETRY_MS        (10 * 1000)
#define BATTERY_POLL_MS       (30 * 1000)
#define SOUND_STOP_MS         1000
#define KEY_GUARD_MAX_US      (10LL * 1000 * 1000)
#define SCREEN_GUARD_MAX_US   (3LL * 1000 * 1000)

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later and would report it as
// a fresh press. Drop key actions until that press comes up (see key_guard.h).
static key_guard_t s_key_guard;
// The same for the key that turns a dark screen back on (app task only).
static key_guard_t s_screen_guard;

static round_timer_t s_timer;
static round_phase_t s_phase;
static bool s_sound_ok;
static bool s_screen_off;
static int s_battery = -1;
static int64_t s_last_activity_ms;
static int64_t s_last_battery_ms;
static int64_t s_next_sleep_try_ms;

static void set_screen(bool on)
{
    s_screen_off = !on;
    bsp_display_backlight(on ? 100 : 0);
}

static void play(round_sound_t sound)
{
    if (sound != ROUND_SOUND_NONE && s_sound_ok) timer_sound_play(sound);
}

// Every key acts the moment it goes down; clicks, double clicks, long presses and
// releases are ignored, so holding a key does nothing more.
static bool round_key_for(const input_event_t *input, round_key_t *key)
{
    if (input->event != BSP_BTN_PRESS) return false;
    switch (input->btn) {
    case BSP_BTN_OK:   *key = ROUND_KEY_OK; return true;
    case BSP_BTN_UP:   *key = ROUND_KEY_UP; return true;
    case BSP_BTN_DOWN: *key = ROUND_KEY_DOWN; return true;
    }
    return false;
}

static void handle_input(const input_event_t *input, int64_t now_us)
{
    const int64_t now_ms = now_us / 1000;
    s_last_activity_ms = now_ms;
    if (s_screen_off) {
        set_screen(true);
        key_guard_arm(&s_screen_guard, now_us, SCREEN_GUARD_MAX_US);
    }
    if (key_guard_consume(&s_screen_guard, input->event == BSP_BTN_RELEASE, now_us)) return;

    round_key_t key;
    if (round_key_for(input, &key)) round_timer_key(&s_timer, key, now_ms);
}

static void log_sleep_step(const char *step, esp_err_t err)
{
    if (err != ESP_OK) ESP_LOGW(TAG, "deep sleep continues: %s failed: %s", step, esp_err_to_name(err));
}

// Power everything down in the order the BSP requires and sleep until a key.
// Returns only when sleep has to wait (a USB host, or a key held down).
static void try_deep_sleep(void)
{
    if (usb_serial_jtag_is_connected()) return;
    // Every key pulls the shared pin low, and the wake fires on low: a held key
    // would wake the chip the moment it slept. Check while the keys still work.
    int mv = bsp_button_read_mv();
    if (mv >= 0 && mv < BSP_BTN_MV_RELEASED_MIN) return;

    if (!timer_sound_stop(SOUND_STOP_MS)) {
        ESP_LOGE(TAG, "sound worker did not stop; restarting instead of sleeping");
        esp_restart();
    }

    // Terminal from here: each step releases something that only a restart brings
    // back. A failed key hand-off restarts rather than leave a screen without keys.
    s_input_ready = false;
    int level = 0;
    esp_err_t err = bsp_button_prepare_deep_sleep(&level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key pad hand-off failed (%s); restarting", esp_err_to_name(err));
        esp_restart();
    }
    if (level != 1) ESP_LOGW(TAG, "a key went down just now; this sleep may end at once");
    ESP_LOGI(TAG, "entering deep sleep; any key wakes");
    // The first argument is a pin bit mask, not a pin number.
    log_sleep_step("GPIO wake arm",
                   esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW));
    // CW2017 and ES8311 share I2C, so both go before the bus.
    log_sleep_step("CW2017 suspend", bsp_battery_sleep());
    log_sleep_step("ES8311 suspend", bsp_audio_sleep());
    log_sleep_step("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_sleep_step("I2C pin release", bsp_i2c_prepare_deep_sleep());
    // Wait for the current flush and keep LVGL from drawing to a sleeping panel.
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "cannot stop LVGL before deep sleep; restarting");
        esp_restart();
    }
    log_sleep_step("ST7789 suspend", bsp_display_prepare_deep_sleep());
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start returned; restarting");
    esp_restart();
}

static void housekeeping(int64_t now_ms)
{
    if (now_ms - s_last_battery_ms >= BATTERY_POLL_MS) {
        s_last_battery_ms = now_ms;
        s_battery = bsp_battery_soc();
    }

    bool idle = !round_timer_running(&s_timer) && now_ms - s_last_activity_ms >= IDLE_SLEEP_MS;
    if (!idle) return;
    if (!s_screen_off) set_screen(false);
    if (now_ms >= s_next_sleep_try_ms) {
        s_next_sleep_try_ms = now_ms + SLEEP_RETRY_MS;
        try_deep_sleep();
    }
}

static void render(int64_t now_ms)
{
    if (s_screen_off) return;
    round_view_t view;
    round_timer_view(&s_timer, now_ms, &view);
    if (!bsp_lvgl_lock(LOCK_MS)) return;
    timer_ui_render(&view, s_battery, s_sound_ok);
    bsp_lvgl_unlock();
}

static void app_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        bool got = xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(LOOP_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        const int64_t now_ms = now_us / 1000;
        if (got) handle_input(&input, now_us);
        play(round_timer_tick(&s_timer, now_ms));
        if (s_timer.phase != s_phase) {
            // A segment ending is activity too: Over gets its own ten minutes.
            s_phase = s_timer.phase;
            s_last_activity_ms = now_ms;
        }
        render(now_ms);
        housekeeping(now_ms);
    }
}

// Button callbacks run on the shared esp_timer task; enqueue only and return.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) ESP_LOGI(TAG, "wake key released; keys active");
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void)
{
    const bool key_wake = esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO);
    ESP_LOGI(TAG, "speaker timer starting (%s)", key_wake ? "key wake" : "cold start");
    if (key_wake) key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed; check SPI wiring (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    round_timer_init(&s_timer);
    s_phase = s_timer.phase;

    esp_err_t err = bsp_audio_init();
    if (err == ESP_OK) err = timer_sound_start();
    s_sound_ok = err == ESP_OK;
    if (!s_sound_ok) ESP_LOGE(TAG, "no sound: %s", esp_err_to_name(err));
    if (bsp_battery_init() == ESP_OK) s_battery = bsp_battery_soc();

    if (bsp_lvgl_lock(1000)) {
        timer_ui_create();
        bsp_lvgl_unlock();
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    render(now_ms);
    set_screen(true);
    s_last_activity_ms = s_last_battery_ms = now_ms;

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue || xTaskCreate(app_task, "timer_app", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the app task");
        return;
    }
    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "buttons unavailable: %s", esp_err_to_name(err));
    } else if (s_key_guard.armed) {
        // If the wake key came up before the button driver started, no release
        // will ever arrive: sync the guard to the pin once now.
        key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    }
    s_input_ready = true;
    ESP_LOGI(TAG, "ready: sound=%d battery=%d%%", s_sound_ok, s_battery);
}
