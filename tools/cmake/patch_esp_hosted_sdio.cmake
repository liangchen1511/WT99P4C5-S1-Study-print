# ESP-Hosted / Wi-Fi Remote are managed (gitignored) deps. Patch for IDF 5.5
# so clean component downloads keep building on WT99P4C5-S1.
set(_hosted_root "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_hosted")
set(_wifi_rmt_root "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_wifi_remote")
set(_sdio_drv "${_hosted_root}/host/drivers/transport/sdio/sdio_drv.c")
set(_hosted_kconfig "${_hosted_root}/Kconfig")
set(_wifi_rmt_kconfig "${_wifi_rmt_root}/Kconfig")

# IDF 5.5 kconfiglib: $(ESP_IDF_VERSION) expands blank → Kconfig / ldgen abort
# or silently skip orsource of Kconfig.idf_v5.5.in (no SLAVE/WIFI_RMT symbols).
if(EXISTS "${_hosted_kconfig}")
    file(READ "${_hosted_kconfig}" _kconfig_source)
    string(FIND "${_kconfig_source}" "depends on $(ESP_IDF_VERSION) >= \"6.1\"" _kcfg_pos)
    if(NOT _kcfg_pos EQUAL -1)
        string(REPLACE
            "depends on $(ESP_IDF_VERSION) >= \"6.1\""
            "depends on n"
            _kconfig_patched "${_kconfig_source}")
        file(WRITE "${_hosted_kconfig}" "${_kconfig_patched}")
        message(STATUS "Neutralized ESP-Hosted IDF>=6.1 Kconfig depends for IDF 5.5")
    endif()
endif()

if(EXISTS "${_wifi_rmt_kconfig}")
    file(READ "${_wifi_rmt_kconfig}" _wrmt_source)
    string(FIND "${_wrmt_source}" "Kconfig.idf_v\$ESP_IDF_VERSION.in" _wrmt_pos)
    if(NOT _wrmt_pos EQUAL -1)
        string(REPLACE
            "orsource \"./Kconfig.idf_v\$ESP_IDF_VERSION.in\""
            "orsource \"./Kconfig.idf_v5.5.in\""
            _wrmt_patched "${_wrmt_source}")
        if(_wrmt_patched STREQUAL _wrmt_source)
            message(FATAL_ERROR "esp_wifi_remote Kconfig ESP_IDF_VERSION orsource layout changed")
        endif()
        file(WRITE "${_wifi_rmt_kconfig}" "${_wrmt_patched}")
        message(STATUS "Pinned esp_wifi_remote Kconfig to idf_v5.5 (IDF 5.5 blank macro)")
    endif()
endif()

if(EXISTS "${_sdio_drv}")
    file(READ "${_sdio_drv}" _sdio_source)
    string(FIND "${_sdio_source}" "WiFi RX payload OOM" _guard_pos)
    if(_guard_pos EQUAL -1)
        set(_sdio_patched "${_sdio_source}")
        string(REPLACE
            "assert(*buf);"
            "/* WT99 SDIO OOM guard: preserve the process and retry later. */\n\t\tif (!*buf) {\n\t\t\tESP_LOGE(TAG, \"SDIO streaming RX buffer OOM (%lu bytes)\", len);\n\t\t\treturn NULL;\n\t\t}"
            _sdio_patched "${_sdio_patched}")
        string(REPLACE
            "assert(rxbuff);"
            "if (!rxbuff) {\n\t\t\tSDIO_DRV_UNLOCK();\n\t\t\tg_h.funcs->_h_msleep(10);\n\t\t\tcontinue;\n\t\t}"
            _sdio_patched "${_sdio_patched}")
        string(REPLACE
            "assert(copy_payload);"
            "if (!copy_payload) {\n\t\t\t\t\tESP_LOGW(TAG, \"WiFi RX payload OOM, dropping %u-byte packet\",\n\t\t\t\t\t\tbuf_handle->payload_len);\n\t\t\t\t\tH_FREE_PTR_WITH_FUNC(buf_handle->free_buf_handle, buf_handle->priv_buffer_handle);\n\t\t\t\t\tcontinue;\n\t\t\t\t}"
            _sdio_patched "${_sdio_patched}")

        if(_sdio_patched STREQUAL _sdio_source)
            message(FATAL_ERROR "ESP-Hosted SDIO layout changed; refusing to build without the OOM guard")
        endif()
        file(WRITE "${_sdio_drv}" "${_sdio_patched}")
        message(STATUS "Applied ESP-Hosted SDIO OOM guard")
    endif()
endif()
