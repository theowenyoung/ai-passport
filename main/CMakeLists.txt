idf_component_register(
    SRCS "main.c"
         "round_timer.c"
         "timer_sound.c"
         "timer_ui.c"
         "../assets/fonts/timer/timer_digits_100.c"
         "../assets/fonts/timer/timer_digits_80.c"
         "../assets/fonts/timer/timer_text_26.c"
         "../assets/fonts/timer/timer_text_20.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_driver_usb_serial_jtag esp_hw_support esp_timer
)
