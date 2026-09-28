idf_component_register(
    SRCS "main.c"
         "blog_ui.c"
         "card_pack.c"
         "card_sched.c"
         "card_layout.c"
         "card_store.c"
         "../assets/fonts/blog-cards/blog_font_16.c"
         "../assets/fonts/blog-cards/blog_font_20.c"
         "../assets/fonts/blog-cards/blog_font_24.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_driver_usb_serial_jtag esp_hw_support esp_partition esp_timer nvs_flash
)
