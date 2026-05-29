set(APPS_DIR ./)
file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)

# Release: exclude unused duplicate assets (see docs/OPTIMIZATION_CHANGELOG.md)
list(FILTER APPS_C_SRCS EXCLUDE REGEX ".*/gui_music/assets/.*_large\\.c$")
list(FILTER APPS_C_SRCS EXCLUDE REGEX ".*/video_player/assets/breaking_news\\.c$")

list(FILTER APPS_CPP_SRCS EXCLUDE REGEX ".*/camera/app_humanface_detect\\.cpp$")
list(FILTER APPS_CPP_SRCS EXCLUDE REGEX ".*/camera/app_pedestrian_detect\\.cpp$")
list(FILTER APPS_CPP_SRCS EXCLUDE REGEX ".*/camera/app_camera_pipeline\\.cpp$")

set(_SOTI_EXTRA_CPP
    "${CMAKE_CURRENT_LIST_DIR}/SoTi/soti_answer_format.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/SoTi/soti_print_sections.cpp"
)
foreach(_soti_cpp IN LISTS _SOTI_EXTRA_CPP)
    if(EXISTS "${_soti_cpp}")
        list(FIND APPS_CPP_SRCS "${_soti_cpp}" _soti_idx)
        if(_soti_idx LESS 0)
            list(APPEND APPS_CPP_SRCS "${_soti_cpp}")
        endif()
    endif()
endforeach()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES lvgl__lvgl esp_event esp_wifi esp_netif nvs_flash esp_driver_jpeg esp_mm esp-brookesia bsp_extra wt99p4c5_s1_board esp_video espressif__esp_lcd_touch_gt911 espressif__esp_jpeg lv_font_ui_zh esp_http_client json usb_escpos_printer parent_policy power_manager parent_album_sync parent_print_sync esp_timer)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
