/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <cassert>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_video_init.h"
#include "freertos/queue.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

#include "esp_lcd_touch_gt911.h"

#include "app_video.h"
#include "Camera.hpp"
#include "parent_guard.hpp"
#include "ui/ui.h"

#define ALIGN_UP_BY(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

#define CAMERA_INIT_TASK_WAIT_MS            (8000)
/** LVGL / SoTi must not block forever waiting for CSI stream teardown. */
#define CAMERA_STREAM_STOP_WAIT_MS          (5000)
#define FPS_PRINT                           (0)

typedef enum {
    CAMERA_EVENT_TASK_RUN = BIT(0),
    CAMERA_EVENT_DELETE = BIT(1),
} camera_event_id_t;

typedef enum {
    CAM_WORK_SNAP = 1,
    CAM_WORK_SOTI = 2,
} cam_work_type_t;

typedef struct {
    cam_work_type_t type;
    uint8_t *buf;
    size_t len;
} cam_work_item_t;

static void camera_worker_task(void *arg);
static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                       size_t camera_buf_len);

LV_IMG_DECLARE(img_app_camera);

static const char *TAG = "Camera";

static size_t data_cache_line_size = 0;
static EventGroupHandle_t camera_event_group;
static QueueHandle_t s_cam_work_queue = nullptr;

static Camera *s_cam_instance = nullptr;
/** Hardware singleton (Camera::init); used when another app drives preview. */
static Camera *s_camera_hw_singleton = nullptr;
static lv_obj_t *s_preview_canvas_override = nullptr;
static bool s_camera_stream_running = false;

static SemaphoreHandle_t s_snapshot_sem = nullptr;
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_snapshot_requested = false;

static SemaphoreHandle_t s_soti_snap_sem = nullptr;
static portMUX_TYPE s_soti_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_soti_snapshot_requested = false;

/** SPIRAM JPEG copy for SoTi → R2 upload (filled in camera frame callback). */
static uint8_t *s_soti_upload_jpeg = nullptr;
static size_t s_soti_upload_jpeg_len = 0;
static portMUX_TYPE s_soti_upload_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_jpeg_enc_mutex = nullptr;
static SemaphoreHandle_t s_camera_lifecycle_mutex = nullptr;

static bool camera_lifecycle_lock(uint32_t timeout_ms)
{
    return s_camera_lifecycle_mutex != nullptr &&
           xSemaphoreTakeRecursive(s_camera_lifecycle_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void camera_lifecycle_unlock(void)
{
    xSemaphoreGiveRecursive(s_camera_lifecycle_mutex);
}

static bool jpeg_encoder_lock(void)
{
    return s_jpeg_enc_mutex != nullptr &&
           xSemaphoreTake(s_jpeg_enc_mutex, pdMS_TO_TICKS(12000)) == pdTRUE;
}

static void jpeg_encoder_unlock(void)
{
    xSemaphoreGive(s_jpeg_enc_mutex);
}

static void stashSoTiUploadCopy(const uint8_t *src, size_t len)
{
    if (src == nullptr || len == 0) {
        return;
    }
    uint8_t *copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == nullptr) {
        ESP_LOGE(TAG, "SoTi upload JPEG malloc failed (%u)", (unsigned)len);
        return;
    }
    memcpy(copy, src, len);
    portENTER_CRITICAL(&s_soti_upload_mux);
    uint8_t *old = s_soti_upload_jpeg;
    s_soti_upload_jpeg = copy;
    s_soti_upload_jpeg_len = len;
    portEXIT_CRITICAL(&s_soti_upload_mux);
    /* Heap operations may take locks; never call them while holding a spinlock. */
    heap_caps_free(old);
}

uint8_t *Camera::takeSoTiUploadJpeg(size_t *jpeg_len_out)
{
    if (jpeg_len_out != nullptr) {
        *jpeg_len_out = 0;
    }
    portENTER_CRITICAL(&s_soti_upload_mux);
    uint8_t *p = s_soti_upload_jpeg;
    size_t n = s_soti_upload_jpeg_len;
    s_soti_upload_jpeg = nullptr;
    s_soti_upload_jpeg_len = 0;
    portEXIT_CRITICAL(&s_soti_upload_mux);
    if (jpeg_len_out != nullptr) {
        *jpeg_len_out = n;
    }
    return p;
}

static bool build_snapshot_path(char *out, size_t out_sz)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    char base[72];
    snprintf(base, sizeof(base), "%d-%d-%d-%d-%02d.jpg",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min);

    int n = snprintf(out, out_sz, "%s/%s", BSP_SD_MOUNT_POINT, base);
    if (n <= 0 || (size_t)n >= out_sz) {
        return false;
    }

    for (int suffix = 1; access(out, F_OK) == 0 && suffix < 1000; suffix++) {
        snprintf(base, sizeof(base), "%d-%d-%d-%d-%02d_%d.jpg",
                 tm_info.tm_year + 1900,
                 tm_info.tm_mon + 1,
                 tm_info.tm_mday,
                 tm_info.tm_hour,
                 tm_info.tm_min,
                 suffix);
        n = snprintf(out, out_sz, "%s/%s", BSP_SD_MOUNT_POINT, base);
        if (n <= 0 || (size_t)n >= out_sz) {
            return false;
        }
    }
    return true;
}

void Camera::setPreviewCanvasOverride(lv_obj_t *canvas)
{
    s_preview_canvas_override = canvas;
}

bool Camera::allocateCaptureBuffersIfNeeded(void)
{
    if (s_camera_hw_singleton == nullptr) {
        return false;
    }
    Camera *cam = s_camera_hw_singleton;
    esp_err_t ac = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ac != ESP_OK) {
        ESP_LOGE(TAG, "capture buffers: cache alignment failed");
        return false;
    }
    const size_t sz = (size_t)cam->_hor_res * (size_t)cam->_ver_res * BSP_LCD_BITS_PER_PIXEL / 8;

    bool all_ready = true;
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        if (cam->_cam_buffer[i] == nullptr || cam->_cam_buffer_size[i] != sz) {
            all_ready = false;
            break;
        }
    }
    if (all_ready) {
        return true;
    }

    /* Repair a partial allocation as one unit. This is only called while the
     * lifecycle lock is held and no CSI stream is using the pointers. */
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        if (cam->_cam_buffer[i] != nullptr) {
            heap_caps_free(cam->_cam_buffer[i]);
            cam->_cam_buffer[i] = nullptr;
            cam->_cam_buffer_size[i] = 0;
        }
    }

    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        cam->_cam_buffer[i] =
            (uint8_t *)heap_caps_aligned_alloc(data_cache_line_size, sz, MALLOC_CAP_SPIRAM);
        if (cam->_cam_buffer[i] == nullptr) {
            ESP_LOGE(TAG, "capture buffer %d alloc failed (%u bytes)", i, (unsigned)sz);
            for (int j = 0; j < i; j++) {
                heap_caps_free(cam->_cam_buffer[j]);
                cam->_cam_buffer[j] = nullptr;
                cam->_cam_buffer_size[j] = 0;
            }
            return false;
        }
        cam->_cam_buffer_size[i] = sz;
    }

    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = cam->_hor_res,
            .h = cam->_ver_res,
        },
        .data_size = cam->_cam_buffer_size[0],
        .data = (const uint8_t *)cam->_cam_buffer[0],
    };
    memcpy(&cam->_img_refresh_dsc, &img_dsc, sizeof(lv_img_dsc_t));
    return true;
}

bool Camera::preallocateCaptureBuffers(void)
{
    if (!camera_lifecycle_lock(10000)) {
        ESP_LOGE(TAG, "preallocateCaptureBuffers: lifecycle mutex timeout");
        return false;
    }
    bool ok = allocateCaptureBuffersIfNeeded();
    camera_lifecycle_unlock();
    return ok;
}

bool Camera::ensurePreviewStreaming(void)
{
    if (s_camera_hw_singleton == nullptr || !camera_lifecycle_lock(10000)) {
        return false;
    }

    Camera *cam = s_camera_hw_singleton;
    if (s_camera_stream_running && app_video_stream_task_is_active()) {
        camera_lifecycle_unlock();
        return true;
    }
    if (app_video_stream_task_is_active()) {
        ESP_LOGW(TAG, "ensurePreviewStreaming: stopping stale stream task before rebind");
        if (app_video_stream_task_stop(cam->_camera_ctlr_handle) != ESP_OK ||
            app_video_stream_wait_stop(CAMERA_STREAM_STOP_WAIT_MS) != ESP_OK) {
            ESP_LOGE(TAG, "ensurePreviewStreaming: stale stream did not stop");
            camera_lifecycle_unlock();
            return false;
        }
    }
    s_camera_stream_running = false;

    if (!allocateCaptureBuffersIfNeeded()) {
        ESP_LOGE(TAG, "ensurePreviewStreaming: capture buffers unavailable");
        camera_lifecycle_unlock();
        return false;
    }

    if (cam->_camera_ctlr_handle < 0) {
        cam->_camera_ctlr_handle = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
        if (cam->_camera_ctlr_handle < 0) {
            ESP_LOGE(TAG, "ensurePreviewStreaming: reopen camera failed");
            camera_lifecycle_unlock();
            return false;
        }
    }

    esp_err_t e = app_video_set_bufs(cam->_camera_ctlr_handle, EXAMPLE_CAM_BUF_NUM,
                                     (const void **)cam->_cam_buffer);
    if (e != ESP_OK) {
        /* A failed VIDIOC_REQBUFS must not poison every future shutter. Reopen
         * the device once and bind the already-reserved buffers again. */
        ESP_LOGW(TAG, "ensurePreviewStreaming: set_bufs failed, reopen and retry");
        (void)app_video_release_bufs(cam->_camera_ctlr_handle);
        (void)app_video_close(cam->_camera_ctlr_handle);
        cam->_camera_ctlr_handle = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
        if (cam->_camera_ctlr_handle < 0) {
            ESP_LOGE(TAG, "ensurePreviewStreaming: camera reopen after set_bufs failed");
            camera_lifecycle_unlock();
            return false;
        }
        e = app_video_set_bufs(cam->_camera_ctlr_handle, EXAMPLE_CAM_BUF_NUM,
                               (const void **)cam->_cam_buffer);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ensurePreviewStreaming: set_bufs failed");
        camera_lifecycle_unlock();
        return false;
    }
    e = app_video_stream_task_start(cam->_camera_ctlr_handle, 0);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ensurePreviewStreaming: stream start failed");
        (void)app_video_release_bufs(cam->_camera_ctlr_handle);
        camera_lifecycle_unlock();
        return false;
    }
    s_camera_stream_running = true;
    camera_lifecycle_unlock();
    return true;
}

bool Camera::stopPreviewStreamingIfRunning(void)
{
    if (s_camera_hw_singleton == nullptr || !camera_lifecycle_lock(10000)) {
        return false;
    }
    const int fd = s_camera_hw_singleton->_camera_ctlr_handle;
    if (fd < 0) {
        s_camera_stream_running = false;
        camera_lifecycle_unlock();
        return true;
    }
    /*
     * s_camera_stream_running can be false while the stream task is still alive after errors,
     * or true while the task already exited — always key off the real task state.
     */
    if (app_video_stream_task_is_active()) {
        if (app_video_stream_task_stop(fd) != ESP_OK ||
            app_video_stream_wait_stop(CAMERA_STREAM_STOP_WAIT_MS) != ESP_OK) {
            ESP_LOGE(TAG, "stopPreviewStreaming: wait timed out (fd=%d); keep buffers", fd);
            camera_lifecycle_unlock();
            return false;
        }
    }
    s_camera_stream_running = false;
    camera_lifecycle_unlock();
    return true;
}

bool Camera::releasePreviewPsramBuffers(void)
{
    if (s_camera_hw_singleton == nullptr || !camera_lifecycle_lock(10000)) {
        return false;
    }
    if (!stopPreviewStreamingIfRunning()) {
        camera_lifecycle_unlock();
        return false;
    }
    Camera *cam = s_camera_hw_singleton;
    const int fd = cam->_camera_ctlr_handle;
    if (fd >= 0) {
        if (app_video_release_bufs(fd) != ESP_OK) {
            ESP_LOGE(TAG, "releasePreviewPsramBuffers: driver still owns buffers");
            camera_lifecycle_unlock();
            return false;
        }
    }
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        if (cam->_cam_buffer[i] != nullptr) {
            heap_caps_free(cam->_cam_buffer[i]);
            cam->_cam_buffer[i] = nullptr;
            cam->_cam_buffer_size[i] = 0;
        }
    }
    cam->_img_refresh_dsc.data = nullptr;
    cam->_img_refresh_dsc.data_size = 0;

    /* close() sets CAMERA_EVENT_DELETE; clear so SoTi / other apps sharing CSI preview behave normally. */
    if (camera_event_group != nullptr) {
        xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);
    }
    camera_lifecycle_unlock();
    return true;
}

bool Camera::ensureJpegEncoderForHw(void)
{
    if (s_camera_hw_singleton == nullptr) {
        return false;
    }
    return s_camera_hw_singleton->init_jpeg_encoder();
}

void Camera::releaseJpegEncoderHw(void)
{
    if (s_camera_hw_singleton == nullptr) {
        return;
    }
    s_camera_hw_singleton->deinit_jpeg_encoder();
}

void Camera::setFrameProcessingEnabled(bool enable)
{
    if (camera_event_group == nullptr) {
        return;
    }
    if (enable) {
        xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    } else {
        xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    }
}

bool Camera::prepareSoTiCapture(void)
{
    if (s_camera_hw_singleton == nullptr) {
        ESP_LOGE(TAG, "prepareSoTiCapture: camera not inited");
        return false;
    }
    if (camera_event_group != nullptr) {
        xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);
        xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    }
    if (s_soti_snap_sem != nullptr) {
        while (xSemaphoreTake(s_soti_snap_sem, 0) == pdTRUE) {
        }
    }
    if (!ensurePreviewStreaming()) {
        stopPreviewStreamingIfRunning();
        s_camera_stream_running = false;
        if (!ensurePreviewStreaming()) {
            ESP_LOGE(TAG, "prepareSoTiCapture: stream failed");
            return false;
        }
    }
    if (!ensureJpegEncoderForHw()) {
        ESP_LOGE(TAG, "prepareSoTiCapture: JPEG encoder failed");
        return false;
    }
    return true;
}

bool Camera::requestSoTiSnapshotAndWait(uint32_t timeout_ms)
{
    if (s_soti_snap_sem == nullptr) {
        s_soti_snap_sem = xSemaphoreCreateBinary();
        if (s_soti_snap_sem == nullptr) {
            return false;
        }
    }
    while (xSemaphoreTake(s_soti_snap_sem, 0) == pdTRUE) {
    }
    portENTER_CRITICAL(&s_soti_snap_mux);
    s_soti_snapshot_requested = true;
    portEXIT_CRITICAL(&s_soti_snap_mux);
    return xSemaphoreTake(s_soti_snap_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool Camera::saveJpegToSdFromFrame(const uint8_t *rgb565, size_t len)
{
    return save_jpeg_to_sd(rgb565, len);
}

Camera::Camera(uint16_t hor_res, uint16_t ver_res):
    ESP_Brookesia_PhoneApp("相机", &img_app_camera, false),  // auto_resize_visual_area
    _screen_index(SCREEN_CAMERA_SHOT),
    _hor_res(hor_res),
    _ver_res(ver_res),
    _img_album_dsc_size(hor_res > ver_res ? ver_res : hor_res),
    _img_album_buffer(NULL),
    _camera_init_sem(NULL),
    _camera_init_task_handle(NULL),
    _camera_init_completed(false),
    _camera_ctlr_handle(-1),
    _jpeg_enc(nullptr),
    _jpeg_out_buf(nullptr),
    _jpeg_out_cap(0)
{
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        _cam_buffer[i] = nullptr;
        _cam_buffer_size[i] = 0;
    }
    _img_album_buf_bytes = _img_album_dsc_size * _img_album_dsc_size * sizeof(lv_color_t);
}

Camera::~Camera()
{
}

bool Camera::run(void)
{
    if (!parent_guard_app_run("camera")) {
        return false;
    }
    s_cam_instance = this;
    setPreviewCanvasOverride(nullptr);
    if (_jpeg_enc == nullptr && !init_jpeg_encoder()) {
        ESP_LOGW(TAG, "JPEG encoder init failed; photo save disabled");
    }

    if (!_camera_init_completed) {
        if (_camera_init_sem == nullptr) {
            ESP_LOGE(TAG, "Camera init semaphore unavailable");
            return false;
        }
        if (_camera_init_task_handle == nullptr) {
            while (xSemaphoreTake(_camera_init_sem, 0) == pdTRUE) {
            }
            if (xTaskCreatePinnedToCore((TaskFunction_t)taskCameraInit, "Camera Init", 4096, this, 2,
                                        &_camera_init_task_handle, 0) != pdPASS) {
                ESP_LOGE(TAG, "Camera init task create failed");
                _camera_init_task_handle = nullptr;
                return false;
            }
        }
        if (xSemaphoreTake(_camera_init_sem, pdMS_TO_TICKS(CAMERA_INIT_TASK_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Camera init timeout; task will finish safely in background");
            return false;
        }
        if (!_camera_init_completed) {
            ESP_LOGE(TAG, "Camera init failed; next open will retry");
            return false;
        }
    }

    /* After MJPEG player stops the stream, reopening Camera must restart CSI without re-running Init task. */
    if (!ensurePreviewStreaming()) {
        ESP_LOGE(TAG, "ensurePreviewStreaming failed");
        return false;
    }

    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

    // UI initialization
    ui_camera_init();

    // The following is the additional UI initialization
    _img_album_buffer = (uint8_t *)heap_caps_aligned_alloc(128, _img_refresh_dsc.data_size, MALLOC_CAP_SPIRAM);
    if (_img_album_buffer == NULL) {
        ESP_LOGE(TAG, "Allocate memory for album buffer failed");
        return false;
    }
    lv_img_dsc_t img_dsc = {
        .header = {
            .cf = LV_IMG_CF_TRUE_COLOR,
            .always_zero = 0,
            .reserved = 0,
            .w = _hor_res,
            .h = _ver_res,
        },
        .data_size = _img_album_buf_bytes,
        .data = (const uint8_t *)_img_album_buffer,
    };
    memcpy(&_img_album_dsc, &img_dsc, sizeof(lv_img_dsc_t));

    lv_obj_refr_size(ui_PanelCameraShotAlbum);
    lv_obj_clear_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);

    _img_album = lv_imgbtn_create(ui_PanelCameraShotAlbum);
    lv_obj_add_flag(_img_album, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(_img_album, 100, 100);
    lv_obj_center(_img_album);
    lv_obj_add_event_cb(_img_album, onScreenCameraShotAlbumClick, LV_EVENT_CLICKED, this);

    img_dsc.header.w = _hor_res;
    img_dsc.header.h = _ver_res;
    img_dsc.data_size = _img_refresh_dsc.data_size;
    memcpy(&_img_photo_dsc, &img_dsc, sizeof(lv_img_dsc_t));
    memcpy(_img_album_buffer, _img_refresh_dsc.data, _img_refresh_dsc.data_size);
    lv_obj_set_width(ui_ImageCameraPhotoImage, _hor_res);
    lv_obj_set_height(ui_ImageCameraPhotoImage, _ver_res);
    lv_img_set_src(ui_ImageCameraPhotoImage, &_img_photo_dsc);

    lv_obj_add_event_cb(ui_ButtonCameraShotBtn, onScreenCameraShotBtnClick, LV_EVENT_CLICKED, this);

    lv_obj_add_flag(ui_PanelCameraShotTitle, LV_OBJ_FLAG_HIDDEN);

    return true;
}

bool Camera::pause(void)
{
    /* Stop frame processing before touching JPEG encoder (same HW block as album decoder). */
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    deinit_jpeg_encoder();

    return true;
}

bool Camera::resume(void)
{
    if (!ensurePreviewStreaming()) {
        ESP_LOGW(TAG, "resume: preview stream unavailable");
    }
    if (_jpeg_enc == nullptr && !init_jpeg_encoder()) {
        ESP_LOGW(TAG, "JPEG encoder re-init failed; snapshots may not save");
    }
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);

    return true;
}

bool Camera::back(void)
{
    notifyCoreClosed();

    return true;
}

bool Camera::close(void)
{
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupSetBits(camera_event_group, CAMERA_EVENT_DELETE);

    setPreviewCanvasOverride(nullptr);
    (void)releasePreviewPsramBuffers();

    s_cam_instance = nullptr;
    deinit_jpeg_encoder();

    if (_img_album_buffer) {
        heap_caps_free(_img_album_buffer);
        _img_album_buffer = NULL;
    }

    /* Reserve contiguous CSI memory again while the UI album buffer has just been freed. */
    if (!preallocateCaptureBuffers()) {
        ESP_LOGW(TAG, "close: unable to reserve capture buffers for next open");
    }

    return true;
}

bool Camera::init(void)
{
    s_camera_hw_singleton = this;

    if (s_camera_lifecycle_mutex == nullptr) {
        s_camera_lifecycle_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (s_jpeg_enc_mutex == nullptr) {
        s_jpeg_enc_mutex = xSemaphoreCreateMutex();
    }
    if (_camera_init_sem == nullptr) {
        _camera_init_sem = xSemaphoreCreateBinary();
    }
    if (s_camera_lifecycle_mutex == nullptr || s_jpeg_enc_mutex == nullptr ||
        _camera_init_sem == nullptr) {
        ESP_LOGE(TAG, "camera synchronization allocation failed");
        return false;
    }

    if (s_snapshot_sem == nullptr) {
        s_snapshot_sem = xSemaphoreCreateBinary();
        if (s_snapshot_sem == nullptr) {
            ESP_LOGE(TAG, "snapshot semaphore allocation failed");
            return false;
        }
    }

    camera_event_group = xEventGroupCreate();
    if (camera_event_group == nullptr) {
        ESP_LOGE(TAG, "camera event group allocation failed");
        return false;
    }
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_TASK_RUN);
    xEventGroupClearBits(camera_event_group, CAMERA_EVENT_DELETE);

    if (s_cam_work_queue == nullptr) {
        s_cam_work_queue = xQueueCreate(2, sizeof(cam_work_item_t));
        if (s_cam_work_queue == nullptr ||
            xTaskCreate(camera_worker_task, "cam_worker", 8192, nullptr, 5, nullptr) != pdPASS) {
            ESP_LOGE(TAG, "camera worker allocation failed");
            return false;
        }
    }

    i2c_master_bus_handle_t i2c_bus_handle = bsp_i2c_get_handle();
    esp_err_t ret = app_video_main(i2c_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
    }

    // Open the video device
    _camera_ctlr_handle = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT_RGB565);
    if (_camera_ctlr_handle < 0) {
        ESP_LOGE(TAG, "video cam open failed");

        if (ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) || ESP_OK == i2c_master_probe(i2c_bus_handle, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100)) {
            ESP_LOGI(TAG, "gt911 touch found");
        } else {
            ESP_LOGE(TAG, "Touch not found");
        }
    }

    ret = esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size);
    if (ret != ESP_OK || data_cache_line_size == 0) {
        ESP_LOGE(TAG, "camera cache alignment unavailable: %s", esp_err_to_name(ret));
        return false;
    }
    if (!preallocateCaptureBuffers()) {
        ESP_LOGW(TAG, "camera buffers not reserved at boot; first open will retry");
    }

    // Register the video frame operation callback
    ret = app_video_register_frame_operation_cb(camera_video_frame_operation);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera frame callback registration failed: %s", esp_err_to_name(ret));
        return false;
    }

    return true;
}

bool Camera::init_jpeg_encoder(void)
{
    if (!jpeg_encoder_lock()) {
        ESP_LOGE(TAG, "JPEG encoder mutex timeout during init");
        return false;
    }
    if (_jpeg_enc != nullptr && _jpeg_out_buf != nullptr) {
        jpeg_encoder_unlock();
        return true;
    }
    if (_jpeg_out_buf != nullptr) {
        heap_caps_free(_jpeg_out_buf);
        _jpeg_out_buf = nullptr;
        _jpeg_out_cap = 0;
    }
    if (_jpeg_enc != nullptr) {
        jpeg_del_encoder_engine(_jpeg_enc);
        _jpeg_enc = nullptr;
    }

    jpeg_encode_engine_cfg_t encode_eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 5000,
    };
    esp_err_t err = jpeg_new_encoder_engine(&encode_eng_cfg, &_jpeg_enc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: %s", esp_err_to_name(err));
        _jpeg_enc = nullptr;
        jpeg_encoder_unlock();
        return false;
    }

    size_t src_len = (size_t)_hor_res * _ver_res * 2;
    jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    _jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(src_len / 2, &mem_cfg, &_jpeg_out_cap);
    if (_jpeg_out_buf == nullptr) {
        ESP_LOGE(TAG, "jpeg_alloc_encoder_mem failed");
        jpeg_del_encoder_engine(_jpeg_enc);
        _jpeg_enc = nullptr;
        jpeg_encoder_unlock();
        return false;
    }

    _jpeg_enc_cfg.width = _hor_res;
    _jpeg_enc_cfg.height = _ver_res;
    _jpeg_enc_cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    _jpeg_enc_cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    _jpeg_enc_cfg.image_quality = 85;

    jpeg_encoder_unlock();
    return true;
}

void Camera::deinit_jpeg_encoder(void)
{
    if (!jpeg_encoder_lock()) {
        ESP_LOGE(TAG, "JPEG encoder mutex timeout during deinit; keep hardware alive");
        return;
    }
    if (_jpeg_out_buf != nullptr) {
        heap_caps_free(_jpeg_out_buf);
        _jpeg_out_buf = nullptr;
        _jpeg_out_cap = 0;
    }
    if (_jpeg_enc != nullptr) {
        jpeg_del_encoder_engine(_jpeg_enc);
        _jpeg_enc = nullptr;
    }
    jpeg_encoder_unlock();
}

bool Camera::jpegEncodeFrameSized(const uint8_t *rgb565, size_t len, uint16_t w, uint16_t h, uint32_t *jpeg_size_out)
{
    if (_jpeg_enc == nullptr || _jpeg_out_buf == nullptr) {
        ESP_LOGE(TAG, "JPEG encoder not initialized");
        return false;
    }
    const size_t need = (size_t)w * (size_t)h * 2;
    if (len < need || w == 0 || h == 0) {
        ESP_LOGE(TAG, "jpegEncodeFrameSized bad len/w/h");
        return false;
    }
    const uint16_t prev_w = _jpeg_enc_cfg.width;
    const uint16_t prev_h = _jpeg_enc_cfg.height;
    _jpeg_enc_cfg.width = w;
    _jpeg_enc_cfg.height = h;

    uint32_t jpeg_size = 0;
    esp_err_t err = jpeg_encoder_process(_jpeg_enc, &_jpeg_enc_cfg, rgb565, (uint32_t)need,
                                         _jpeg_out_buf, (uint32_t)_jpeg_out_cap, &jpeg_size);

    _jpeg_enc_cfg.width = prev_w;
    _jpeg_enc_cfg.height = prev_h;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_encoder_process failed: %s", esp_err_to_name(err));
        return false;
    }
    if (jpeg_size_out != nullptr) {
        *jpeg_size_out = jpeg_size;
    }
    return true;
}

bool Camera::jpegEncodeFrame(const uint8_t *rgb565, size_t len, uint32_t *jpeg_size_out)
{
    if (len != _img_refresh_dsc.data_size) {
        ESP_LOGE(TAG, "frame size mismatch %u vs %u", (unsigned)len, (unsigned)_img_refresh_dsc.data_size);
        return false;
    }
    return jpegEncodeFrameSized(rgb565, len, _hor_res, _ver_res, jpeg_size_out);
}

bool Camera::writeLastJpegToSd(uint32_t jpeg_size)
{
    char path[160];
    if (!build_snapshot_path(path, sizeof(path))) {
        ESP_LOGE(TAG, "could not build snapshot path");
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "fopen(%s) failed (SD mounted?)", path);
        return false;
    }
    size_t wn = fwrite(_jpeg_out_buf, 1, jpeg_size, f);
    fclose(f);
    if (wn != jpeg_size) {
        ESP_LOGE(TAG, "JPEG fwrite incomplete");
        return false;
    }

    ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)jpeg_size);
    return true;
}

bool Camera::save_jpeg_to_sd(const uint8_t *rgb565, size_t len)
{
    if (!jpeg_encoder_lock()) {
        ESP_LOGE(TAG, "save JPEG encoder mutex timeout");
        return false;
    }
    uint32_t jpeg_size = 0;
    if (!jpegEncodeFrame(rgb565, len, &jpeg_size)) {
        jpeg_encoder_unlock();
        return false;
    }
    bool ok = writeLastJpegToSd(jpeg_size);
    jpeg_encoder_unlock();
    return ok;
}

bool Camera::encodeSoTiSnapshotFromFrame(const uint8_t *rgb565, size_t len)
{
    if (!jpeg_encoder_lock()) {
        ESP_LOGE(TAG, "SoTi JPEG encoder mutex timeout");
        return false;
    }
    uint32_t jpeg_size = 0;
    const uint8_t prev_q = _jpeg_enc_cfg.image_quality;
    /* 搜题经 SDIO Wi‑Fi 上传：强压画质换更小 body（服务器侧还可二值化再送豆包）。 */
    _jpeg_enc_cfg.image_quality = 28;
    const bool ok = jpegEncodeFrame(rgb565, len, &jpeg_size);
    _jpeg_enc_cfg.image_quality = prev_q;
    if (!ok) {
        jpeg_encoder_unlock();
        return false;
    }
    stashSoTiUploadCopy(_jpeg_out_buf, jpeg_size);
    jpeg_encoder_unlock();
    return true;
}

bool Camera::encodeRgb565ToJpegMalloc(const uint8_t *rgb565, size_t rgb_len, uint16_t w, uint16_t h,
                                      uint8_t **jpeg_out, size_t *jpeg_len_out)
{
    if (jpeg_out == nullptr || jpeg_len_out == nullptr || rgb565 == nullptr) {
        return false;
    }
    *jpeg_out = nullptr;
    *jpeg_len_out = 0;
    Camera *cam = s_camera_hw_singleton;
    if (cam == nullptr || !jpeg_encoder_lock()) {
        ESP_LOGE(TAG, "encodeRgb565ToJpegMalloc: no encoder");
        return false;
    }
    uint32_t jpeg_size = 0;
    if (!cam->jpegEncodeFrameSized(rgb565, rgb_len, w, h, &jpeg_size) || jpeg_size == 0) {
        jpeg_encoder_unlock();
        return false;
    }
    uint8_t *copy = (uint8_t *)heap_caps_malloc(jpeg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == nullptr) {
        copy = (uint8_t *)heap_caps_malloc(jpeg_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (copy == nullptr) {
        ESP_LOGE(TAG, "JPEG output malloc failed");
        jpeg_encoder_unlock();
        return false;
    }
    memcpy(copy, cam->_jpeg_out_buf, jpeg_size);
    jpeg_encoder_unlock();
    *jpeg_out = copy;
    *jpeg_len_out = jpeg_size;
    return true;
}

bool Camera::snapshot_save_and_copy_thumb(const uint8_t *rgb565, size_t len)
{
    bool ok = save_jpeg_to_sd(rgb565, len);
    if (ok && _img_album_buffer != nullptr) {
        memcpy(_img_album_buffer, rgb565, len);
    }
    return ok;
}

void Camera::apply_album_thumbnail_ui(void)
{
    if (_img_album != nullptr) {
        lv_img_set_src(_img_album, &_img_album_dsc);
        lv_obj_invalidate(_img_album);
    }
}

void Camera::taskCameraInit(Camera *app)
{
    bool ok = app != nullptr && ensurePreviewStreaming();
    if (!ok) {
        ESP_LOGE(TAG, "camera init task could not start preview");
    }
    if (app != nullptr) {
        app->_camera_init_completed = ok;
        app->_camera_init_task_handle = nullptr;
        if (app->_camera_init_sem != nullptr) {
            xSemaphoreGive(app->_camera_init_sem);
        }
    }
    vTaskDelete(nullptr);
    return;
}

void Camera::onScreenCameraShotAlbumClick(lv_event_t *e)
{
    lv_obj_invalidate(ui_ImageCameraPhotoImage);
}

void Camera::onScreenCameraShotBtnClick(lv_event_t *e)
{
    Camera *camera = (Camera *)e->user_data;

    if (camera == NULL || s_snapshot_sem == nullptr) {
        return;
    }

    lv_obj_add_flag(camera->_img_album, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui_PanelCameraShotAlbum, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(camera->_img_album, &camera->_img_album_dsc);

    portENTER_CRITICAL(&s_snap_mux);
    if (s_snapshot_requested) {
        portEXIT_CRITICAL(&s_snap_mux);
        ESP_LOGW(TAG, "Snapshot already in progress");
        return;
    }
    s_snapshot_requested = true;
    portEXIT_CRITICAL(&s_snap_mux);

    if (xSemaphoreTake(s_snapshot_sem, pdMS_TO_TICKS(4000)) != pdTRUE) {
        ESP_LOGE(TAG, "Snapshot timed out");
        portENTER_CRITICAL(&s_snap_mux);
        s_snapshot_requested = false;
        portEXIT_CRITICAL(&s_snap_mux);
    }
}

static bool camera_queue_frame_work(cam_work_type_t type, const uint8_t *camera_buf, size_t camera_buf_len)
{
    if (s_cam_work_queue == nullptr || camera_buf == nullptr || camera_buf_len == 0) {
        return false;
    }
    uint8_t *copy = (uint8_t *)heap_caps_malloc(camera_buf_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == nullptr) {
        return false;
    }
    memcpy(copy, camera_buf, camera_buf_len);
    cam_work_item_t item = {
        .type = type,
        .buf = copy,
        .len = camera_buf_len,
    };
    if (xQueueSend(s_cam_work_queue, &item, 0) != pdTRUE) {
        heap_caps_free(copy);
        return false;
    }
    return true;
}

static void camera_worker_task(void *arg)
{
    (void)arg;
    cam_work_item_t item;
    while (xQueueReceive(s_cam_work_queue, &item, portMAX_DELAY) == pdTRUE) {
        if (item.buf == nullptr) {
            continue;
        }
        if (item.type == CAM_WORK_SNAP && s_cam_instance != nullptr) {
            bool snap_ok = s_cam_instance->snapshot_save_and_copy_thumb(item.buf, item.len);
            if (snap_ok) {
                (void)bsp_extra_play_camera_shutter_async();
                if (bsp_display_lock(100)) {
                    s_cam_instance->apply_album_thumbnail_ui();
                    bsp_display_unlock();
                }
            }
            if (s_snapshot_sem != nullptr) {
                xSemaphoreGive(s_snapshot_sem);
            }
        } else if (item.type == CAM_WORK_SOTI && s_camera_hw_singleton != nullptr) {
            bool soti_ok = s_camera_hw_singleton->encodeSoTiSnapshotFromFrame(item.buf, item.len);
            if (!soti_ok) {
                ESP_LOGW(TAG, "SoTi snapshot encode/stash failed (retry shutter)");
            }
            if (soti_ok && s_soti_snap_sem != nullptr) {
                xSemaphoreGive(s_soti_snap_sem);
            }
            if (soti_ok) {
                (void)bsp_extra_play_camera_shutter_async();
            }
        }
        heap_caps_free(item.buf);
    }
    vTaskDelete(nullptr);
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                       uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                       size_t camera_buf_len)
{
    (void)camera_buf_index;
    EventBits_t current_bits = xEventGroupGetBits(camera_event_group);
    if ((current_bits & CAMERA_EVENT_TASK_RUN) == 0) {
        return;
    }

    bool need_snap = false;
    portENTER_CRITICAL(&s_snap_mux);
    if (s_snapshot_requested) {
        s_snapshot_requested = false;
        need_snap = true;
    }
    portEXIT_CRITICAL(&s_snap_mux);

    bool need_soti_snap = false;
    portENTER_CRITICAL(&s_soti_snap_mux);
    if (s_soti_snapshot_requested) {
        s_soti_snapshot_requested = false;
        need_soti_snap = true;
    }
    portEXIT_CRITICAL(&s_soti_snap_mux);

    if (need_snap) {
        if (!camera_queue_frame_work(CAM_WORK_SNAP, camera_buf, camera_buf_len) && s_snapshot_sem != nullptr) {
            xSemaphoreGive(s_snapshot_sem);
        }
    }

    if (need_soti_snap) {
        if (!camera_queue_frame_work(CAM_WORK_SOTI, camera_buf, camera_buf_len)) {
            ESP_LOGW(TAG, "SoTi snapshot queue full");
        }
    }

    if (!(current_bits & CAMERA_EVENT_DELETE) && bsp_display_lock(0)) {
        lv_obj_t *target = s_preview_canvas_override;
        if (target == nullptr && ui_ImageCameraShotImage != nullptr && lv_obj_is_valid(ui_ImageCameraShotImage)) {
            target = ui_ImageCameraShotImage;
        }
        if (target != nullptr) {
            lv_canvas_set_buffer(target, camera_buf, camera_buf_hes, camera_buf_ves, LV_IMG_CF_TRUE_COLOR);
            lv_obj_invalidate(target);
        }
        bsp_display_unlock();
    }
}
