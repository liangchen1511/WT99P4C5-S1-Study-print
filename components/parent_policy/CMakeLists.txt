idf_component_register(
    SRCS "parent_policy.cpp" "parent_guard.cpp"
    INCLUDE_DIRS "." "../apps/SoTi"
    REQUIRES esp_http_client esp-tls nvs_flash json esp_wifi esp_netif lvgl__lvgl lv_font_ui_zh
)
