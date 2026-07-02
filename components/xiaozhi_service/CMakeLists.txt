idf_component_register(SRCS "xiaozhi_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_xiaozhi 78__esp-opus-encoder 78__esp-opus bsp_extra parent_policy power_manager esp_timer freertos)
