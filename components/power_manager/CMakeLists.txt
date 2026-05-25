idf_component_register(
    SRCS "power_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES wt99p4c5_s1_board espressif__esp_lvgl_port lvgl__lvgl esp_wifi freertos esp_driver_gpio espressif__esp_lcd_touch
)
