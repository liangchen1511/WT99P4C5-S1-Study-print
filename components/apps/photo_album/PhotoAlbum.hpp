/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <string>
#include <vector>
#include "lvgl.h"
#include "esp_brookesia.hpp"

class PhotoAlbum : public ESP_Brookesia_PhoneApp {
public:
    explicit PhotoAlbum(int so_ti_app_id);
    ~PhotoAlbum();

    bool run(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    void postAlbumStatusAsync(const char *msg);
    void notifyAlbumStatus(const char *msg);
    void set_sync_status(const char *msg);
    void refreshAfterSync(bool ok);
    void refresh_sync_hint(void);
    void request_sync_hint_refresh(void);

private:
    static constexpr size_t kMaxFiles = 128;
    static constexpr size_t kPathMax = 512;
    static constexpr uint32_t kMaxJpegBytes = 6 * 1024 * 1024;
    static constexpr uint32_t kMaxDecodePixels = 1280 * 960;

    void scan_jpeg_files(void);
    void rebuild_dropdown(void);
    bool load_preview_for_index(uint16_t idx);
    void clear_preview(void);
    void set_status(const char *msg);

    static void on_dropdown_changed(lv_event_t *e);
    static void on_delete_clicked(lv_event_t *e);
    static void on_print_clicked(lv_event_t *e);
    static void on_soti_clicked(lv_event_t *e);
    static void on_sync_clicked(lv_event_t *e);
    static void on_msgbox_event(lv_event_t *e);

    std::vector<std::string> _entries;

    int _so_ti_app_id;

    lv_obj_t *_dd;
    lv_obj_t *_canvas;
    lv_obj_t *_btn_del;
    lv_obj_t *_btn_print;
    lv_obj_t *_btn_soti;
    lv_obj_t *_btn_sync;
    lv_obj_t *_lbl_status;
    lv_obj_t *_lbl_sync;
    lv_obj_t *_root_col;
    lv_obj_t *_scroll;

    uint8_t *_preview_buf;
    uint16_t _img_w;
    uint16_t _img_h;

    uint16_t _pending_delete_idx;
};
