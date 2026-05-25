idf_component_register(
    SRCS
        "parent_album_fs.c"
        "parent_album_sync.cpp"
    INCLUDE_DIRS "include" "../apps/SoTi"
    REQUIRES
        esp_http_client
        esp-tls
        esp_wifi
        esp_netif
        json
        fatfs
        vfs
        nvs_flash
        power_manager
        wt99p4c5_s1_board
)
