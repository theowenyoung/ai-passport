// main/main.c — Blog Cards firmware entry: bring up the board, then start the app.
//
// The baseline hardware-test demo sources (demo_*.c, ui_pixel*.c) stay in this
// directory for the upstream host tests but are not built into this firmware.
#include "blog_ui.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "Blog Cards starting");
    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed; check SPI wiring (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }

    // Blog Cards plays no sound: suspend the codec so it idles at its lowest draw.
    if (bsp_audio_init() == ESP_OK && bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "audio codec suspend failed");
    }

    blog_ui_start();
}
