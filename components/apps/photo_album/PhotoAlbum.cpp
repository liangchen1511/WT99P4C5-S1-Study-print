/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include "freertos/task.h"

#include "jpeg_decoder.h"

#include "PhotoAlbum.hpp"
#include "camera/Camera.hpp"
#include "parent_album_sync.h"
#include "parent_chat/parent_chat_api.hpp"
#include "parent_policy.hpp"
#include "SoTi/SoTi.hpp"
#include "lv_font_ui_zh.h"

LV_IMG_DECLARE(img_app_photo_album);

static const char *TAG = "PhotoAlbum";

static uint16_t s_canvas_placeholder;

namespace {

struct album_status_async_ctx {
    PhotoAlbum *app;
    char msg[96];
};

void album_status_async_cb(void *ud)
{
    album_status_async_ctx *c = (album_status_async_ctx *)ud;
    if (c != nullptr && c->app != nullptr) {
        c->app->notifyAlbumStatus(c->msg);
    }
    free(c);
}

struct sync_status_async_ctx {
    PhotoAlbum *app;
    char line[96];
};

static void sync_status_async_cb(void *ud)
{
    sync_status_async_ctx *c = (sync_status_async_ctx *)ud;
    if (c != nullptr && c->app != nullptr) {
        c->app->set_sync_status(c->line);
    }
    free(c);
}

static void album_focus_enter_cb(void)
{
    Camera::stopPreviewStreamingIfRunning();
    Camera::setFrameProcessingEnabled(false);
    parent_chat_bg_poll_stop();
    parent_chat_bg_poll_wait_stop(3000);
    parent_policy_poll_pause(true);
}

static void album_focus_exit_cb(void)
{
    parent_policy_poll_pause(false);
    parent_chat_bg_poll_start();
    Camera::setFrameProcessingEnabled(true);
}

static void album_sync_status_cb(const char *line, void *user)
{
    PhotoAlbum *app = (PhotoAlbum *)user;
    if (app == nullptr || line == nullptr) {
        return;
    }
    sync_status_async_ctx *c = (sync_status_async_ctx *)calloc(1, sizeof(*c));
    if (c == nullptr) {
        return;
    }
    c->app = app;
    snprintf(c->line, sizeof(c->line), "%s", line);
    if (lv_async_call(sync_status_async_cb, c) != LV_RES_OK) {
        free(c);
    }
}

struct album_sync_job {
    PhotoAlbum *app;
    bool ok;
};

static void album_sync_done_async_cb(void *ud)
{
    album_sync_job *job = (album_sync_job *)ud;
    if (job != nullptr && job->app != nullptr) {
        job->app->refreshAfterSync(job->ok);
    }
    free(job);
}

static void album_sync_task(void *arg)
{
    album_sync_job *job = (album_sync_job *)arg;
    if (job == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    job->ok = (parent_album_sync_run_once() == ESP_OK);
    if (lv_async_call(album_sync_done_async_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(nullptr);
}

} // namespace

static void photo_album_apply_cjk_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &lv_font_ui_zh_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        photo_album_apply_cjk_font(lv_obj_get_child(obj, i));
    }
}

static bool is_jpeg_filename(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}

PhotoAlbum::PhotoAlbum(int so_ti_app_id)
    : ESP_Brookesia_PhoneApp("相册", &img_app_photo_album, true),
      _so_ti_app_id(so_ti_app_id),
      _dd(nullptr),
      _canvas(nullptr),
      _btn_del(nullptr),
      _btn_soti(nullptr),
      _btn_sync(nullptr),
      _lbl_status(nullptr),
      _lbl_sync(nullptr),
      _root_col(nullptr),
      _scroll(nullptr),
      _preview_buf(nullptr),
      _img_w(0),
      _img_h(0),
      _pending_delete_idx(0)
{
}

PhotoAlbum::~PhotoAlbum()
{
}

bool PhotoAlbum::init(void)
{
    static bool hooks_set;
    if (!hooks_set) {
        parent_album_sync_hooks_t hooks = {
            .on_enter = album_focus_enter_cb,
            .on_exit = album_focus_exit_cb,
        };
        parent_album_sync_set_hooks(&hooks);
        parent_album_sync_set_status_callback(album_sync_status_cb, this);
        hooks_set = true;
    }
    return true;
}

void PhotoAlbum::notifyAlbumStatus(const char *msg)
{
    set_status(msg);
}

void PhotoAlbum::postAlbumStatusAsync(const char *msg)
{
    album_status_async_ctx *c = (album_status_async_ctx *)calloc(1, sizeof(*c));
    if (c == nullptr) {
        return;
    }
    c->app = this;
    if (msg != nullptr) {
        snprintf(c->msg, sizeof(c->msg), "%s", msg);
    } else {
        c->msg[0] = '\0';
    }
    if (lv_async_call(album_status_async_cb, c) != LV_RES_OK) {
        free(c);
    }
}

void PhotoAlbum::clear_preview(void)
{
    if (_preview_buf != nullptr) {
        heap_caps_free(_preview_buf);
        _preview_buf = nullptr;
        _img_w = 0;
        _img_h = 0;
    }
    if (_canvas != nullptr) {
        lv_canvas_set_buffer(_canvas, &s_canvas_placeholder, 1, 1, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(_canvas, 1, 1);
    }
}

void PhotoAlbum::set_status(const char *msg)
{
    if (_lbl_status == nullptr) {
        return;
    }
    if (msg == nullptr || msg[0] == '\0') {
        lv_label_set_text(_lbl_status, "");
        lv_obj_add_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(_lbl_status, msg);
        lv_obj_clear_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);
    }
}

void PhotoAlbum::set_sync_status(const char *msg)
{
    if (_lbl_sync == nullptr) {
        return;
    }
    lv_label_set_text(_lbl_sync, msg != nullptr ? msg : "");
}

struct hint_refresh_job {
    PhotoAlbum *app;
};

static void hint_refresh_async_cb(void *ud)
{
    hint_refresh_job *job = (hint_refresh_job *)ud;
    if (job != nullptr && job->app != nullptr) {
        job->app->refresh_sync_hint();
    }
    free(job);
}

static void hint_refresh_task(void *arg)
{
    hint_refresh_job *job = (hint_refresh_job *)arg;
    if (job == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    (void)parent_album_sync_refresh_pending();
    if (lv_async_call(hint_refresh_async_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(nullptr);
}

void PhotoAlbum::request_sync_hint_refresh(void)
{
    hint_refresh_job *job = (hint_refresh_job *)calloc(1, sizeof(*job));
    if (job == nullptr) {
        return;
    }
    job->app = this;
    if (xTaskCreate(hint_refresh_task, "alb_hint", 8192, job, 4, nullptr) != pdPASS) {
        free(job);
        refresh_sync_hint();
    }
}

void PhotoAlbum::refresh_sync_hint(void)
{
    if (_lbl_sync == nullptr) {
        return;
    }
    uint32_t n = parent_album_sync_pending_count();
    char buf[64];
    if (n > 0) {
        snprintf(buf, sizeof(buf), "待同步 %u 张（家长站）", (unsigned)n);
    } else {
        snprintf(buf, sizeof(buf), "家长站传图：设备联网自动同步");
    }
    lv_label_set_text(_lbl_sync, buf);
}

void PhotoAlbum::refreshAfterSync(bool ok)
{
    if (ok) {
        set_status("同步完成");
        scan_jpeg_files();
        clear_preview();
        if (_dd != nullptr) {
            rebuild_dropdown();
            if (_entries.empty()) {
                lv_obj_add_state(_btn_del, LV_STATE_DISABLED);
                lv_obj_add_state(_btn_soti, LV_STATE_DISABLED);
            } else {
                lv_obj_clear_state(_btn_del, LV_STATE_DISABLED);
                lv_obj_clear_state(_btn_soti, LV_STATE_DISABLED);
                lv_dropdown_set_selected(_dd, 0);
                load_preview_for_index(0);
            }
        }
    } else {
        set_status("同步失败");
    }
    refresh_sync_hint();
}

void PhotoAlbum::scan_jpeg_files(void)
{
    _entries.clear();
    DIR *d = opendir(BSP_SD_MOUNT_POINT);
    if (d == nullptr) {
        ESP_LOGW(TAG, "Cannot open SD mount point %s", BSP_SD_MOUNT_POINT);
        return;
    }
    struct dirent *dir;
    while ((dir = readdir(d)) != nullptr) {
        if (dir->d_type == DT_DIR) {
            continue;
        }
        if (!is_jpeg_filename(dir->d_name)) {
            continue;
        }
        if (_entries.size() >= kMaxFiles) {
            ESP_LOGW(TAG, "JPEG file list truncated at %u", (unsigned)kMaxFiles);
            break;
        }
        _entries.emplace_back(dir->d_name);
    }
    closedir(d);
    std::sort(_entries.begin(), _entries.end());
}

void PhotoAlbum::rebuild_dropdown(void)
{
    lv_dropdown_clear_options(_dd);
    for (size_t i = 0; i < _entries.size(); i++) {
        lv_dropdown_add_option(_dd, _entries[i].c_str(), i);
    }
}

bool PhotoAlbum::load_preview_for_index(uint16_t idx)
{
    clear_preview();
    if (idx >= _entries.size() || _canvas == nullptr) {
        set_status("No image");
        return false;
    }

    char path[kPathMax];
    int plen = snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, _entries[idx].c_str());
    if (plen <= 0 || plen >= (int)sizeof(path)) {
        set_status("Path too long");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "Open failed: %s", path);
        set_status("Open failed");
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_status("Read error");
        return false;
    }
    long fsz = ftell(f);
    if (fsz <= 0 || (uint32_t)fsz > kMaxJpegBytes) {
        fclose(f);
        set_status("File too large");
        return false;
    }
    rewind(f);

    uint8_t *jpeg_buf = (uint8_t *)heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == nullptr) {
        fclose(f);
        set_status("No memory");
        return false;
    }
    if (fread(jpeg_buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        heap_caps_free(jpeg_buf);
        fclose(f);
        set_status("Read failed");
        return false;
    }
    fclose(f);

    /* Software JPEG (esp_jpeg / TJpgDec): does not share HW with camera JPEG encoder. */
    esp_jpeg_image_cfg_t jpeg_cfg = {};
    jpeg_cfg.indata = jpeg_buf;
    jpeg_cfg.indata_size = (uint32_t)fsz;
    jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_0;
    /* Match LVGL lv_color_t / framebuffer: CONFIG_LV_COLOR_16_SWAP is off on this board.
     * swap_color_bytes=1 was inverting RGB565 halves → false-color ("thermal") preview. */
    jpeg_cfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t jinfo = {};
    esp_err_t err = esp_jpeg_get_image_info(&jpeg_cfg, &jinfo);
    if (err != ESP_OK) {
        heap_caps_free(jpeg_buf);
        set_status("Not a valid baseline JPEG");
        return false;
    }

    uint64_t px = (uint64_t)jinfo.width * (uint64_t)jinfo.height;
    if (px == 0 || px > kMaxDecodePixels) {
        heap_caps_free(jpeg_buf);
        set_status("Image too large");
        return false;
    }

    uint8_t *out_buf = (uint8_t *)heap_caps_malloc(jinfo.output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out_buf == nullptr) {
        heap_caps_free(jpeg_buf);
        set_status("No memory for decode");
        return false;
    }

    jpeg_cfg.outbuf = out_buf;
    jpeg_cfg.outbuf_size = jinfo.output_len;

    /* PC/phone JPEGs often embed larger Huffman/DQT segments than the camera encoder.
     * Default esp_jpeg scratch (~3.1 KiB) can hit TJpgDec JDR_MEM1 during jd_prepare.
     * Use a ~64 KiB pool in PSRAM (same order as JD_FASTDECODE==2 in esp_jpeg). */
    static constexpr size_t kJpegDecodeWorkBytes = 65536;
    uint8_t *jpeg_work = (uint8_t *)heap_caps_malloc(kJpegDecodeWorkBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_work == nullptr) {
        heap_caps_free(jpeg_buf);
        heap_caps_free(out_buf);
        set_status("No memory");
        return false;
    }
    jpeg_cfg.advanced.working_buffer = jpeg_work;
    jpeg_cfg.advanced.working_buffer_size = kJpegDecodeWorkBytes;

    esp_jpeg_image_output_t outimg = {};
    err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    heap_caps_free(jpeg_work);
    heap_caps_free(jpeg_buf);

    if (err != ESP_OK) {
        heap_caps_free(out_buf);
        ESP_LOGE(TAG, "esp_jpeg_decode failed: %s", esp_err_to_name(err));
        set_status("Decode failed");
        return false;
    }

    _preview_buf = out_buf;
    _img_w = outimg.width;
    _img_h = outimg.height;

    lv_canvas_set_buffer(_canvas, _preview_buf, _img_w, _img_h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_size(_canvas, _img_w, _img_h);

    lv_obj_t *scroll = lv_obj_get_parent(_canvas);
    if (scroll != nullptr) {
        lv_obj_update_layout(scroll);
        lv_obj_scroll_to(scroll, 0, 0, LV_ANIM_OFF);
    }

    set_status("");
    return true;
}

bool PhotoAlbum::run(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t vw = area.x2 - area.x1;
    const lv_coord_t vh = area.y2 - area.y1;

    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    _root_col = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root_col, vw, vh);
    lv_obj_set_flex_flow(_root_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(_root_col, 6, 0);
    lv_obj_set_style_pad_row(_root_col, 2, 0);
    lv_obj_set_style_border_width(_root_col, 0, 0);
    lv_obj_set_style_bg_color(_root_col, lv_color_black(), 0);

    lv_obj_t *top = lv_obj_create(_root_col);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);

    _dd = lv_dropdown_create(top);
    lv_obj_set_width(_dd, (lv_coord_t)(vw * 38 / 100));
    lv_dropdown_clear_options(_dd);
    lv_obj_add_event_cb(_dd, on_dropdown_changed, LV_EVENT_VALUE_CHANGED, this);

    _btn_del = lv_btn_create(top);
    lv_obj_set_height(_btn_del, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(_btn_del, 8, 0);
    lv_obj_set_style_pad_ver(_btn_del, 3, 0);
    lv_obj_set_style_min_width(_btn_del, 0, 0);
    lv_obj_t *lbl_del = lv_label_create(_btn_del);
    lv_label_set_text(lbl_del, "删除");
    lv_obj_center(lbl_del);
    lv_obj_set_style_text_font(lbl_del, &lv_font_ui_zh_22, 0);
    lv_obj_add_event_cb(_btn_del, on_delete_clicked, LV_EVENT_CLICKED, this);

    _btn_soti = lv_btn_create(top);
    lv_obj_set_height(_btn_soti, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(_btn_soti, 8, 0);
    lv_obj_set_style_pad_ver(_btn_soti, 3, 0);
    lv_obj_set_style_min_width(_btn_soti, 0, 0);
    lv_obj_set_style_bg_color(_btn_soti, lv_color_hex(0x2563EB), 0);
    lv_obj_t *lbl_soti = lv_label_create(_btn_soti);
    lv_label_set_text(lbl_soti, "搜题");
    lv_obj_center(lbl_soti);
    lv_obj_set_style_text_font(lbl_soti, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(lbl_soti, lv_color_white(), 0);
    lv_obj_add_event_cb(_btn_soti, on_soti_clicked, LV_EVENT_CLICKED, this);

    _btn_sync = lv_btn_create(top);
    lv_obj_set_height(_btn_sync, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(_btn_sync, 8, 0);
    lv_obj_set_style_pad_ver(_btn_sync, 3, 0);
    lv_obj_set_style_min_width(_btn_sync, 0, 0);
    lv_obj_set_style_bg_color(_btn_sync, lv_color_hex(0x7c3aed), 0);
    lv_obj_t *lbl_sync_btn = lv_label_create(_btn_sync);
    lv_label_set_text(lbl_sync_btn, "同步");
    lv_obj_center(lbl_sync_btn);
    lv_obj_set_style_text_font(lbl_sync_btn, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(lbl_sync_btn, lv_color_white(), 0);
    lv_obj_add_event_cb(_btn_sync, on_sync_clicked, LV_EVENT_CLICKED, this);

    _lbl_status = lv_label_create(_root_col);
    lv_obj_set_width(_lbl_status, LV_PCT(100));
    lv_label_set_long_mode(_lbl_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(_lbl_status, lv_color_white(), 0);
    photo_album_apply_cjk_font(_lbl_status);
    lv_obj_add_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);

    _lbl_sync = lv_label_create(_root_col);
    lv_obj_set_width(_lbl_sync, LV_PCT(100));
    lv_label_set_long_mode(_lbl_sync, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(_lbl_sync, lv_color_hex(0xcbd5e1), 0);
    photo_album_apply_cjk_font(_lbl_sync);
    request_sync_hint_refresh();

    _scroll = lv_obj_create(_root_col);
    lv_obj_set_width(_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(_scroll, 1);
    lv_obj_set_style_border_width(_scroll, 1, 0);
    lv_obj_set_style_bg_color(_scroll, lv_color_hex(0x202020), 0);
    lv_obj_set_style_pad_top(_scroll, 0, 0);
    lv_obj_add_flag(_scroll, LV_OBJ_FLAG_SCROLLABLE);

    _canvas = lv_canvas_create(_scroll);
    /* TOP_LEFT so horizontal scroll range matches image pixels; TOP_MID breaks scrollbar vs pan alignment on wide JPEGs. */
    lv_obj_align(_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    scan_jpeg_files();
    if (_entries.empty()) {
        set_status("SD 卡根目录无 JPEG（检查是否挂载 /sdcard）");
        lv_obj_add_state(_btn_del, LV_STATE_DISABLED);
        lv_obj_add_state(_btn_soti, LV_STATE_DISABLED);
    } else {
        rebuild_dropdown();
        lv_dropdown_set_selected(_dd, 0);
        load_preview_for_index(0);
    }

    return true;
}

bool PhotoAlbum::back(void)
{
    notifyCoreClosed();
    return true;
}

bool PhotoAlbum::pause(void)
{
    return true;
}

bool PhotoAlbum::resume(void)
{
    request_sync_hint_refresh();
    if (_dd != nullptr && _canvas != nullptr && !_entries.empty()) {
        uint16_t sel = lv_dropdown_get_selected(_dd);
        load_preview_for_index(sel);
    }
    return true;
}

bool PhotoAlbum::close(void)
{
    clear_preview();
    return true;
}

void PhotoAlbum::on_sync_clicked(lv_event_t *e)
{
    PhotoAlbum *app = (PhotoAlbum *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    album_sync_job *job = (album_sync_job *)calloc(1, sizeof(*job));
    if (job == nullptr) {
        app->set_status("内存不足");
        return;
    }
    job->app = app;
    app->set_status("同步中");
    if (xTaskCreate(album_sync_task, "album_sync", 16384, job, 5, nullptr) != pdPASS) {
        free(job);
        app->set_status("无法启动同步");
    }
}

void PhotoAlbum::on_dropdown_changed(lv_event_t *e)
{
    PhotoAlbum *app = (PhotoAlbum *)lv_event_get_user_data(e);
    if (app == nullptr || app->_dd == nullptr) {
        return;
    }
    uint16_t sel = lv_dropdown_get_selected(app->_dd);
    app->load_preview_for_index(sel);
}

void PhotoAlbum::on_soti_clicked(lv_event_t *e)
{
    PhotoAlbum *app = (PhotoAlbum *)lv_event_get_user_data(e);
    if (app == nullptr || app->_dd == nullptr || app->_entries.empty() || app->_so_ti_app_id < 0) {
        return;
    }

    const uint16_t sel = lv_dropdown_get_selected(app->_dd);
    if (sel >= app->_entries.size()) {
        return;
    }

    char path[kPathMax];
    const int plen = snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, app->_entries[sel].c_str());
    if (plen <= 0 || plen >= (int)sizeof(path)) {
        app->set_status("路径过长");
        return;
    }

    SoTi::armSdCardJpegUpload(path);

    ESP_Brookesia_Phone *phone = app->getPhone();
    if (phone == nullptr) {
        SoTi::armSdCardJpegUpload(nullptr);
        return;
    }

    /*
     * CoreManager::processAppRun() does not pause the current foreground app (unlike resume).
     * Starting SoTi while相册 is still the active app breaks SoTi run() and falls back to desktop.
     * Going HOME first matches the launcher path: pause foreground, clear active_app, then START.
     */
    if (!phone->sendNavigateEvent(ESP_BROOKESIA_CORE_NAVIGATE_TYPE_HOME)) {
        ESP_LOGW(TAG, "sendNavigateEvent(HOME) failed before SoTi");
        SoTi::armSdCardJpegUpload(nullptr);
        return;
    }

    ESP_Brookesia_CoreAppEventData_t ev = {};
    ev.id = app->_so_ti_app_id;
    ev.type = ESP_BROOKESIA_CORE_APP_EVENT_TYPE_START;
    ev.data = nullptr;
    if (!phone->sendAppEvent(&ev)) {
        ESP_LOGW(TAG, "Could not open SoTi app");
        SoTi::armSdCardJpegUpload(nullptr);
    }
}

void PhotoAlbum::on_delete_clicked(lv_event_t *e)
{
    PhotoAlbum *app = (PhotoAlbum *)lv_event_get_user_data(e);
    if (app == nullptr || app->_dd == nullptr || app->_entries.empty()) {
        return;
    }

    app->_pending_delete_idx = lv_dropdown_get_selected(app->_dd);
    if (app->_pending_delete_idx >= app->_entries.size()) {
        return;
    }

    char txt[384];
    snprintf(txt, sizeof(txt), "是否删除「%s」？", app->_entries[app->_pending_delete_idx].c_str());

    static const char *btns[] = {"取消", "删除", ""};
    lv_obj_t *mbox = lv_msgbox_create(lv_layer_sys(), "确认", txt, btns, false);
    photo_album_apply_cjk_font(mbox);
    lv_obj_add_event_cb(mbox, on_msgbox_event, LV_EVENT_VALUE_CHANGED, app);
    lv_obj_center(mbox);
}

void PhotoAlbum::on_msgbox_event(lv_event_t *e)
{
    PhotoAlbum *app = (PhotoAlbum *)lv_event_get_user_data(e);
    lv_obj_t *mbox = lv_event_get_current_target(e);
    if (app == nullptr || mbox == nullptr) {
        return;
    }

    if (lv_msgbox_get_active_btn(mbox) != 1) {
        lv_msgbox_close(mbox);
        return;
    }

    uint16_t idx = app->_pending_delete_idx;
    if (idx < app->_entries.size()) {
        char path[kPathMax];
        snprintf(path, sizeof(path), "%s/%s", BSP_SD_MOUNT_POINT, app->_entries[idx].c_str());
        if (unlink(path) != 0) {
            ESP_LOGE(TAG, "unlink failed: %s", path);
            app->set_status("删除失败");
        } else {
            app->scan_jpeg_files();
            app->clear_preview();
            app->rebuild_dropdown();
            if (app->_entries.empty()) {
                lv_obj_add_state(app->_btn_del, LV_STATE_DISABLED);
                lv_obj_add_state(app->_btn_soti, LV_STATE_DISABLED);
                app->set_status("已无 JPEG");
            } else {
                lv_obj_clear_state(app->_btn_del, LV_STATE_DISABLED);
                lv_obj_clear_state(app->_btn_soti, LV_STATE_DISABLED);
                uint16_t next = idx >= app->_entries.size() ? app->_entries.size() - 1 : idx;
                lv_dropdown_set_selected(app->_dd, next);
                app->load_preview_for_index(next);
                app->set_status("已删除");
            }
        }
    }

    lv_msgbox_close(mbox);
}
