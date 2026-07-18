/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_brookesia.hpp"
#include "app_video.h"
#include "esp_video_init.h"
#include "driver/jpeg_encode.h"

class Camera: public ESP_Brookesia_PhoneApp {
public:
    Camera(uint16_t hor_res, uint16_t ver_res);
    ~Camera();

    bool run(void);
    bool pause(void);
    bool resume(void);
    bool back(void);
    bool close(void);

    bool init(void) override;

    /** Frame callback invokes these; must be callable from static context. */
    bool snapshot_save_and_copy_thumb(const uint8_t *rgb565, size_t len);
    void apply_album_thumbnail_ui(void);

    /** Save one RGB565 camera frame as JPEG to SD (shared HW encoder). */
    bool saveJpegToSdFromFrame(const uint8_t *rgb565, size_t len);

    /** Route live preview to this LVGL canvas, or nullptr for built-in Camera UI. */
    static void setPreviewCanvasOverride(lv_obj_t *canvas);

    /** Start MIPI stream if not running (e.g. SoTi opened before Camera). */
    static bool ensurePreviewStreaming(void);

    /** Stop stream task so DQBUF/QBUF cannot wedge after preview pause (e.g. before MJPEG play). */
    static void stopPreviewStreamingIfRunning(void);

    /** Stop stream and free CSI frame buffers in PSRAM (e.g. before large MJPEG decode framebuffer). */
    static void releasePreviewPsramBuffers(void);

    static bool ensureJpegEncoderForHw(void);

    /** Release HW JPEG encoder (shares peripheral with MJPEG HW decode in video player). */
    static void releaseJpegEncoderHw(void);

    static void setFrameProcessingEnabled(bool enable);

    /** SoTi 进入/快门前：清 DELETE、开帧处理、重启 CSI 流、就绪 JPEG 编码器。 */
    static bool prepareSoTiCapture(void);

    /** Arm JPEG capture; blocks until the next frame is encoded or timeout. */
    static bool requestSoTiSnapshotAndWait(uint32_t timeout_ms);

    /**
     * After requestSoTiSnapshotAndWait() succeeds: take SPIRAM copy of JPEG for upload.
     * Caller must heap_caps_free() the returned pointer (can be nullptr if encode failed).
     */
    static uint8_t *takeSoTiUploadJpeg(size_t *jpeg_len_out);

    /**
     * HW JPEG encode arbitrary RGB565 region (w×h). w,h should be multiples of 8 for best compatibility.
     * Returns malloc'd JPEG in SPIRAM/internal; caller heap_caps_free(*jpeg_out).
     */
    static bool encodeRgb565ToJpegMalloc(const uint8_t *rgb565, size_t rgb_len, uint16_t w, uint16_t h,
                                         uint8_t **jpeg_out, size_t *jpeg_len_out);

    /** Frame callback: encode one RGB565 frame for SoTi upload (SPIRAM JPEG copy only; no SD write). */
    bool encodeSoTiSnapshotFromFrame(const uint8_t *rgb565, size_t len);

private:
    static bool allocateCaptureBuffersIfNeeded(void);

    static void taskCameraInit(Camera *app);
    static void onScreenCameraShotBtnClick(lv_event_t *e);
    static void onScreenCameraShotAlbumClick(lv_event_t *e);

    bool init_jpeg_encoder(void);
    void deinit_jpeg_encoder(void);
    bool jpegEncodeFrame(const uint8_t *rgb565, size_t len, uint32_t *jpeg_size_out);
    bool jpegEncodeFrameSized(const uint8_t *rgb565, size_t len, uint16_t w, uint16_t h, uint32_t *jpeg_size_out);
    bool writeLastJpegToSd(uint32_t jpeg_size);
    bool save_jpeg_to_sd(const uint8_t *rgb565, size_t len);

    enum {
        SCREEN_CAMERA_SHOT,
        SCREEN_CAMERA_PHOTO,
        SCREEN_CAMERA_AI,
    } _screen_index;
    uint16_t _hor_res;
    uint16_t _ver_res;
    uint16_t _img_album_dsc_size;
    uint32_t _img_album_buf_bytes;
    uint8_t *_img_album_buffer;
    SemaphoreHandle_t _camera_init_sem;
    int _camera_ctlr_handle;
    lv_img_dsc_t _img_refresh_dsc;
    lv_img_dsc_t _img_album_dsc;
    lv_img_dsc_t _img_photo_dsc;
    lv_obj_t *_img_album;
    uint8_t *_cam_buffer[EXAMPLE_CAM_BUF_NUM];
    size_t _cam_buffer_size[EXAMPLE_CAM_BUF_NUM];

    jpeg_encoder_handle_t _jpeg_enc;
    uint8_t *_jpeg_out_buf;
    size_t _jpeg_out_cap;
    jpeg_encode_cfg_t _jpeg_enc_cfg;
};

