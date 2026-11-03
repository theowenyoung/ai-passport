// main/timer_sound.c — see timer_sound.h.
//
// A beep is an 880 Hz sine with raised-cosine fades, so it starts and ends
// without a click and sounds softer than the baseline demo's square wave. The
// I2S channel sends zeros between beeps (auto_clear_after_cb in the BSP).
#include "timer_sound.h"

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <math.h>
#include <string.h>

static const char *TAG = "timer_sound";

#define SAMPLE_RATE   16000
#define TONE_HZ       880
#define AMPLITUDE     10000
#define FADE_MS       15
#define ONE_MS        220     // "sharing is over"
#define TWO_MS        160     // each of the two "discussion is over" beeps
#define TWO_GAP_MS    110
#define CHUNK_SAMPLES 256
#define QUEUE_DEPTH   4
#define TWO_PI_F      6.28318531f

// Fixed at the codec's full output, as asked for a noisy room; AMPLITUDE keeps
// the sine about 10 dB below digital full scale.
#define VOLUME_PERCENT 100

typedef struct {
    round_sound_t sound;  // ROUND_SOUND_NONE asks the worker to exit
} sound_command_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_stopped;
static TaskHandle_t s_task;
static volatile bool s_stopping;
static int16_t s_chunk[CHUNK_SAMPLES];

static esp_err_t play_tone(int ms)
{
    const int total = SAMPLE_RATE * ms / 1000;
    const int fade = SAMPLE_RATE * FADE_MS / 1000;
    const float step = TWO_PI_F * TONE_HZ / SAMPLE_RATE;
    for (int done = 0; done < total;) {
        int n = total - done < CHUNK_SAMPLES ? total - done : CHUNK_SAMPLES;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            int edge = k < total - k ? k : total - k;
            float envelope = edge >= fade ? 1.0f : 0.5f - 0.5f * cosf(3.14159265f * edge / fade);
            s_chunk[i] = (int16_t)(AMPLITUDE * envelope * sinf(step * k));
        }
        esp_err_t err = bsp_audio_write(s_chunk, (size_t)n * sizeof(int16_t));
        if (err != ESP_OK) return err;
        done += n;
    }
    return ESP_OK;
}

static esp_err_t play_silence(int ms)
{
    memset(s_chunk, 0, sizeof s_chunk);
    for (int left = SAMPLE_RATE * ms / 1000; left > 0;) {
        int n = left < CHUNK_SAMPLES ? left : CHUNK_SAMPLES;
        esp_err_t err = bsp_audio_write(s_chunk, (size_t)n * sizeof(int16_t));
        if (err != ESP_OK) return err;
        left -= n;
    }
    return ESP_OK;
}

static void sound_task(void *arg)
{
    (void)arg;
    bool format_ok = false;
    sound_command_t command;
    for (;;) {
        if (xQueueReceive(s_queue, &command, portMAX_DELAY) != pdTRUE) continue;
        if (command.sound == ROUND_SOUND_NONE) break;
        if (!format_ok) {
            esp_err_t err = bsp_audio_set_format(SAMPLE_RATE, 16, 1);
            format_ok = err == ESP_OK;
            if (!format_ok) {
                ESP_LOGW(TAG, "audio format failed: %s", esp_err_to_name(err));
                continue;
            }
            bsp_audio_set_volume(VOLUME_PERCENT);
        }

        esp_err_t err;
        if (command.sound == ROUND_SOUND_TWO) {
            err = play_tone(TWO_MS);
            if (err == ESP_OK) err = play_silence(TWO_GAP_MS);
            if (err == ESP_OK) err = play_tone(TWO_MS);
        } else {
            err = play_tone(ONE_MS);
        }
        if (err != ESP_OK) {
            // The BSP rebuilds the codec on the next format call.
            ESP_LOGW(TAG, "beep failed: %s", esp_err_to_name(err));
            format_ok = false;
        }
    }
    // The owner deletes this task after the acknowledgement; touch nothing shared after it.
    xSemaphoreGive(s_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t timer_sound_start(void)
{
    if (s_task) return ESP_OK;
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(sound_command_t));
    s_stopped = xSemaphoreCreateBinary();
    // Above the app task, so the I2S ring is refilled while the screen redraws.
    if (!s_queue || !s_stopped ||
        xTaskCreate(sound_task, "timer_sound", 3072, NULL, 6, &s_task) != pdPASS) {
        if (s_queue) vQueueDelete(s_queue);
        if (s_stopped) vSemaphoreDelete(s_stopped);
        s_queue = NULL;
        s_stopped = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_stopping = false;
    return ESP_OK;
}

void timer_sound_play(round_sound_t sound)
{
    if (sound == ROUND_SOUND_NONE || !s_task || s_stopping) return;
    const sound_command_t command = { .sound = sound };
    if (xQueueSend(s_queue, &command, 0) != pdTRUE) {
        ESP_LOGW(TAG, "beep dropped: worker busy");
    }
}

bool timer_sound_stop(int timeout_ms)
{
    if (!s_task) return true;
    s_stopping = true;
    const sound_command_t stop = { .sound = ROUND_SOUND_NONE };
    if (xQueueSendToFront(s_queue, &stop, pdMS_TO_TICKS(timeout_ms)) != pdTRUE ||
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    vTaskDelete(s_task);
    s_task = NULL;
    vQueueDelete(s_queue);
    s_queue = NULL;
    vSemaphoreDelete(s_stopped);
    s_stopped = NULL;
    return true;
}
