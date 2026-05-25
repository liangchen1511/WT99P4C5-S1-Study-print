/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "soti_mode.h"
#include "soti_print_sections.hpp"

class SoTi : public ESP_Brookesia_PhoneApp {
public:
    explicit SoTi(uint16_t hor_res, uint16_t ver_res);
    ~SoTi() override;

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool init(void) override;

    /** Upload thread finishes → LVGL thread shows result */
    void showLoadingUi(bool show);
    void showAnswerUi(const char *utf8);
    void clearRequestInflight(void);
    void setPrintCache(soti_mode_t mode, const soti_print_sections_t *sections);
    void refreshPrintButtons(void);
    void postPrintStatusAsync(const char *msg);
    const char *cachedPrintText(bool with_answer) const;

    /**
     * 相册专用入口：在 SD 上的 JPEG 绝对路径。仅由相册在 sendNavigateEvent(HOME) 之后再 START 搜题时调用；
     * 与搜题应用内快门快照（对话框 → 上传）互不共用逻辑，路径在 run()/resume() 末尾最多消费一次。
     */
    static void armSdCardJpegUpload(const char *absolute_path);

    /** LVGL thread: album SD JPEG read finished in background task. */
    void deliverSdJpegForUpload(uint8_t *jpeg, size_t len);

private:
    static void on_shutter_click(lv_event_t *e);
    static void on_upload_confirm_msgbox(lv_event_t *e);
    static void on_answer_back(lv_event_t *e);
    static void on_daily_click(lv_event_t *e);
    static void on_mode_roller_changed(lv_event_t *e);
    static void on_print_question_clicked(lv_event_t *e);
    static void on_print_with_answer_clicked(lv_event_t *e);
    static void on_loading_cancel(lv_event_t *e);

    soti_mode_t selectedModeFromRoller() const;
    void setLoadingHint(const char *line1);
    void refreshSideModeHint(void);
    void clearPrintCache(void);

    void buildAuxiliaryUi(lv_coord_t vw, lv_coord_t vh);
    void restoreLivePreview();
    /** 上传中 / 答案页：停 CSI 预览与帧处理，避免串口持续刷 camera FPS。 */
    void setPreviewPaused(bool paused);
    void tryConsumeSdCardUpload();
    void startUploadTask(uint8_t *jpeg, size_t jpeg_len, soti_mode_t mode);
    void startDailyTask(void);

    uint16_t _hor_res;
    uint16_t _ver_res;
    lv_obj_t *_root;
    lv_obj_t *_cam_canvas;
    void *_cam_bg_buf;
    lv_obj_t *_preview_side;
    lv_obj_t *_mode_roller;
    lv_obj_t *_side_mode_lbl;
    lv_obj_t *_shutter_hint_lbl;
    lv_obj_t *_loading_lbl;
    bool _preview_paused;
    bool _request_inflight; /* upload/daily HTTP 进行中 */
    soti_mode_t _result_mode;

    lv_obj_t *_loading_overlay;
    lv_obj_t *_answer_layer;
    lv_obj_t *_answer_title_lbl;
    lv_obj_t *_answer_scroll;
    lv_obj_t *_answer_lbl;
    lv_obj_t *_print_bar;
    lv_obj_t *_btn_print_question;
    lv_obj_t *_btn_print_with_answer;
    lv_obj_t *_print_status_lbl;
    soti_print_sections_t _print_cache;
};
