/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <errno.h>
#include "esp_err.h"
#include "esp_log.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "app_video.h"

static const char *TAG = "app_video";

#define MAX_BUFFER_COUNT                (6)
#define MIN_BUFFER_COUNT                (2)
#define VIDEO_TASK_STACK_SIZE           (8 * 1024)
#define VIDEO_TASK_PRIORITY             (6)

typedef enum {
    VIDEO_TASK_DELETE = BIT(0),
    VIDEO_TASK_DELETE_DONE = BIT(1),
} video_event_id_t;

typedef struct {
    uint8_t *camera_buffer[MAX_BUFFER_COUNT];
    size_t camera_buf_size;
    uint32_t camera_buf_hes;
    uint32_t camera_buf_ves;
    struct v4l2_buffer v4l2_buf;
    uint8_t camera_mem_mode;
    app_video_frame_operation_cb_t user_camera_video_frame_operation_cb;
    TaskHandle_t video_stream_task_handle;
    EventGroupHandle_t video_event_group;
    /** Valid for stream task lifetime; do not pass stack &video_fd into the task. */
    int stream_task_video_fd;
} app_video_t;

static app_video_t app_camera_video;

esp_err_t app_video_main(i2c_master_bus_handle_t i2c_bus_handle)
{
#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = true,
                .i2c_config = {
                    .port      = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_PORT,
                    .scl_pin   = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
                    .sda_pin   = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
                },
                .freq      = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_FREQ,
            },
            .reset_pin = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN,
            .pwdn_pin  = CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_PWDN_PIN,
        },
    };

    if (i2c_bus_handle != NULL) {
        csi_config[0].sccb_config.init_sccb = false;
        csi_config[0].sccb_config.i2c_handle = i2c_bus_handle;
    }
#endif

    esp_video_init_config_t cam_config = {
#if CONFIG_EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR > 0
        .csi      = csi_config,
#endif
    };

    return esp_video_init(&cam_config);
}

int app_video_open(char *dev, video_fmt_t init_fmt)
{
    struct v4l2_format default_format;
    struct v4l2_capability capability;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

#if CONFIG_EXAMPLE_ENABLE_CAM_SENSOR_PIC_VFLIP || CONFIG_EXAMPLE_ENABLE_CAM_SENSOR_PIC_HFLIP
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[1];
#endif

    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "Open video failed");
        return -1;
    }

    if (ioctl(fd, VIDIOC_QUERYCAP, &capability)) {
        ESP_LOGE(TAG, "failed to get capability");
        goto exit_0;
    }

    ESP_LOGI(TAG, "version: %d.%d.%d", (uint16_t)(capability.version >> 16),
             (uint8_t)(capability.version >> 8),
             (uint8_t)capability.version);
    ESP_LOGI(TAG, "driver:  %s", capability.driver);
    ESP_LOGI(TAG, "card:    %s", capability.card);
    ESP_LOGI(TAG, "bus:     %s", capability.bus_info);

    memset(&default_format, 0, sizeof(struct v4l2_format));
    default_format.type = type;
    if (ioctl(fd, VIDIOC_G_FMT, &default_format) != 0) {
        ESP_LOGE(TAG, "failed to get format");
        goto exit_0;
    }

    ESP_LOGI(TAG, "width=%" PRIu32 " height=%" PRIu32, default_format.fmt.pix.width, default_format.fmt.pix.height);

    app_camera_video.camera_buf_hes = default_format.fmt.pix.width;
    app_camera_video.camera_buf_ves = default_format.fmt.pix.height;

    if (default_format.fmt.pix.pixelformat != init_fmt) {
        struct v4l2_format format = {
            .type = type,
            .fmt.pix.width = default_format.fmt.pix.width,
            .fmt.pix.height = default_format.fmt.pix.height,
            .fmt.pix.pixelformat = init_fmt,
        };

        if (ioctl(fd, VIDIOC_S_FMT, &format) != 0) {
            ESP_LOGE(TAG, "failed to set format");
            goto exit_0;
        }
    }

#if CONFIG_EXAMPLE_ENABLE_CAM_SENSOR_PIC_VFLIP
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count      = 1;
    controls.controls   = control;
    control[0].id       = V4L2_CID_VFLIP;
    control[0].value    = 1;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to mirror the frame horizontally and skip this step");
    }
#endif

#if CONFIG_EXAMPLE_ENABLE_CAM_SENSOR_PIC_HFLIP
    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count      = 1;
    controls.controls   = control;
    control[0].id       = V4L2_CID_HFLIP;
    control[0].value    = 1;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to mirror the frame horizontally and skip this step");
    }
#endif

    return fd;
exit_0:
    close(fd);
    return -1;
}

esp_err_t app_video_close(int video_fd)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return close(video_fd) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_video_set_bufs(int video_fd, uint32_t fb_num, const void **fb)
{
    if (fb_num > MAX_BUFFER_COUNT) {
        ESP_LOGE(TAG, "buffer num is too large");
        return ESP_FAIL;
    } else if (fb_num < MIN_BUFFER_COUNT) {
        ESP_LOGE(TAG, "At least two buffers are required");
        return ESP_FAIL;
    }

    struct v4l2_requestbuffers req;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&req, 0, sizeof(req));
    req.count = fb_num;
    req.type = type;

    app_camera_video.camera_mem_mode = req.memory = fb ? V4L2_MEMORY_USERPTR : V4L2_MEMORY_MMAP;

    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "req bufs failed");
        goto errout_req_bufs;
    }
    for (int i = 0; i < fb_num; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = type;
        buf.memory = req.memory;
        buf.index = i;

        if (ioctl(video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "query buf failed");
            goto errout_req_bufs;
        }

        if (req.memory == V4L2_MEMORY_MMAP) {
            app_camera_video.camera_buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, video_fd, buf.m.offset);
            if (app_camera_video.camera_buffer[i] == NULL) {
                ESP_LOGE(TAG, "mmap failed");
                goto errout_req_bufs;
            }
        } else {
            if (!fb[i]) {
                ESP_LOGE(TAG, "frame buffer is NULL");
                goto errout_req_bufs;
            }
            buf.m.userptr = (unsigned long)fb[i];
            app_camera_video.camera_buffer[i] = (uint8_t *)fb[i];
        }

        app_camera_video.camera_buf_size = buf.length;

        if (ioctl(video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "queue frame buffer failed");
            goto errout_req_bufs;
        }
    }

    return ESP_OK;

errout_req_bufs:
    /* Do not close video_fd here. The owner retains the descriptor and may
     * release/rebind buffers or explicitly reopen after a transient failure. */
    req.count = 0;
    (void)ioctl(video_fd, VIDIOC_REQBUFS, &req);
    for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
        app_camera_video.camera_buffer[i] = NULL;
    }
    return ESP_FAIL;
}

esp_err_t app_video_release_bufs(int video_fd)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = app_camera_video.camera_mem_mode;
    if (req.memory == 0) {
        req.memory = V4L2_MEMORY_USERPTR;
    }
    if (ioctl(video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGW(TAG, "release bufs (REQBUFS 0) failed");
        return ESP_FAIL;
    }
    for (int i = 0; i < MAX_BUFFER_COUNT; i++) {
        app_camera_video.camera_buffer[i] = NULL;
    }
    return ESP_OK;
}

esp_err_t app_video_get_bufs(int fb_num, void **fb)
{
    if (fb_num > MAX_BUFFER_COUNT) {
        ESP_LOGE(TAG, "buffer num is too large");
        return ESP_FAIL;
    } else if (fb_num < MIN_BUFFER_COUNT) {
        ESP_LOGE(TAG, "At least two buffers are required");
        return ESP_FAIL;
    }

    for (int i = 0; i < fb_num; i++) {
        if (app_camera_video.camera_buffer[i] != NULL) {
            fb[i] = app_camera_video.camera_buffer[i];
        } else {
            ESP_LOGE(TAG, "frame buffer is NULL");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

uint32_t app_video_get_buf_size(void)
{
    uint32_t buf_size = app_camera_video.camera_buf_hes * app_camera_video.camera_buf_ves * (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3);

    return buf_size;
}

static inline esp_err_t video_receive_video_frame(int video_fd)
{
    memset(&app_camera_video.v4l2_buf, 0, sizeof(app_camera_video.v4l2_buf));
    app_camera_video.v4l2_buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    app_camera_video.v4l2_buf.memory = app_camera_video.camera_mem_mode;

    int res = ioctl(video_fd, VIDIOC_DQBUF, &(app_camera_video.v4l2_buf));
    if (res != 0) {
        goto errout;
    }

    return ESP_OK;

errout:
    return ESP_FAIL;
}

static inline void video_operation_video_frame(int video_fd)
{
    if (app_camera_video.v4l2_buf.index >= MAX_BUFFER_COUNT ||
        app_camera_video.camera_buffer[app_camera_video.v4l2_buf.index] == NULL ||
        app_camera_video.user_camera_video_frame_operation_cb == NULL) {
        ESP_LOGE(TAG, "drop invalid camera frame index=%u", (unsigned)app_camera_video.v4l2_buf.index);
        return;
    }
    app_camera_video.v4l2_buf.m.userptr = (unsigned long)app_camera_video.camera_buffer[app_camera_video.v4l2_buf.index];
    app_camera_video.v4l2_buf.length = app_camera_video.camera_buf_size;

    uint8_t buf_index = app_camera_video.v4l2_buf.index;

    app_camera_video.user_camera_video_frame_operation_cb(
                        app_camera_video.camera_buffer[buf_index],
                        buf_index,
                        app_camera_video.camera_buf_hes,
                        app_camera_video.camera_buf_ves,
                        app_camera_video.camera_buf_size
                    );
}

static inline esp_err_t video_free_video_frame(int video_fd)
{
    if (ioctl(video_fd, VIDIOC_QBUF, &(app_camera_video.v4l2_buf)) != 0) {
        goto errout;
    }

    return ESP_OK;

errout:
    return ESP_FAIL;
}

static inline esp_err_t video_stream_start(int video_fd)
{
    ESP_LOGI(TAG, "Video Stream Start");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMON, &type)) {
        ESP_LOGE(TAG, "failed to start stream");
        goto errout;
    }

    struct v4l2_format format = {0};
    format.type = type;
    if (ioctl(video_fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "get fmt failed");
        goto errout;
    }

    return ESP_OK;

errout:
    return ESP_FAIL;
}

static inline esp_err_t video_stream_stop(int video_fd)
{
    ESP_LOGI(TAG, "Video Stream Stop");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(video_fd, VIDIOC_STREAMOFF, &type)) {
        ESP_LOGE(TAG, "failed to stop stream");
        /* Still signal DONE so waiters (e.g. LVGL) do not block forever. */
        xEventGroupSetBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE_DONE);
        return ESP_FAIL;
    }

    xEventGroupSetBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE_DONE);

    return ESP_OK;
}

static void video_stream_task(void *arg)
{
    (void)arg;
    const int video_fd = app_camera_video.stream_task_video_fd;
    unsigned recv_fail_streak = 0;
    TickType_t last_dqbuf_warn_tick = 0;
    TickType_t last_qbuf_warn_tick = 0;

    while (1) {
        if (xEventGroupGetBits(app_camera_video.video_event_group) & VIDEO_TASK_DELETE) {
            xEventGroupClearBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE);
            if (video_stream_stop(video_fd) != ESP_OK) {
                ESP_LOGE(TAG, "video_stream_stop failed");
            }
            app_camera_video.video_stream_task_handle = NULL;
            vTaskDelete(NULL);
        }

        esp_err_t recv_err = video_receive_video_frame(video_fd);
        if (recv_err != ESP_OK) {
            int err = errno;
            recv_fail_streak++;
            TickType_t now = xTaskGetTickCount();
            if (recv_fail_streak == 1U || (now - last_dqbuf_warn_tick) > pdMS_TO_TICKS(3000)) {
                last_dqbuf_warn_tick = now;
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed errno=%d streak=%u (retry)", err, recv_fail_streak);
            }
            /* First frames after STREAMON may return EAGAIN; other errors are retried instead of aborting. */
            if (err == EAGAIN || err == EWOULDBLOCK) {
                vTaskDelay(1);
            } else {
                vTaskDelay(pdMS_TO_TICKS(recv_fail_streak > 500U ? 50 : 5));
            }
            continue;
        }
        recv_fail_streak = 0;

        video_operation_video_frame(video_fd);

        unsigned q_fail = 0;
        while (video_free_video_frame(video_fd) != ESP_OK) {
            int err = errno;
            q_fail++;
            TickType_t now = xTaskGetTickCount();
            if (q_fail == 1U || (now - last_qbuf_warn_tick) > pdMS_TO_TICKS(3000)) {
                last_qbuf_warn_tick = now;
                ESP_LOGW(TAG, "VIDIOC_QBUF failed errno=%d q_fail=%u (retry)", err, q_fail);
            }
            if (xEventGroupGetBits(app_camera_video.video_event_group) & VIDEO_TASK_DELETE) {
                xEventGroupClearBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE);
                (void)video_stream_stop(video_fd);
                app_camera_video.video_stream_task_handle = NULL;
                vTaskDelete(NULL);
            }
            vTaskDelay(1);
        }
    }
}

bool app_video_stream_task_is_active(void)
{
    if (app_camera_video.video_stream_task_handle == NULL) {
        return false;
    }
    eTaskState st = eTaskGetState(app_camera_video.video_stream_task_handle);
    return (st != eDeleted);
}

esp_err_t app_video_stream_task_start(int video_fd, int core_id)
{
    if(app_camera_video.video_event_group == NULL) {
        app_camera_video.video_event_group = xEventGroupCreate();
    }
    if (app_camera_video.video_event_group == NULL) {
        ESP_LOGE(TAG, "failed to create video event group");
        return ESP_ERR_NO_MEM;
    }
    xEventGroupClearBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE_DONE);

    app_camera_video.stream_task_video_fd = video_fd;

    if (app_camera_video.video_stream_task_handle != NULL) {
        eTaskState st = eTaskGetState(app_camera_video.video_stream_task_handle);
        if (st != eDeleted) {
            ESP_LOGW(TAG, "video stream task already active (state=%d), skip duplicate start", (int)st);
            return ESP_OK;
        }
        app_camera_video.video_stream_task_handle = NULL;
    }

    esp_err_t ss = video_stream_start(video_fd);
    if (ss != ESP_OK) {
        ESP_LOGE(TAG, "video_stream_start failed, not spawning stream task");
        return ESP_FAIL;
    }

    BaseType_t result = xTaskCreatePinnedToCore(video_stream_task, "video stream task", VIDEO_TASK_STACK_SIZE, NULL, VIDEO_TASK_PRIORITY, &app_camera_video.video_stream_task_handle, core_id);

    if (result != pdPASS) {
        ESP_LOGE(TAG, "failed to create video stream task");
        goto errout;
    }

    return ESP_OK;

errout:
    (void)video_stream_stop(video_fd);
    app_camera_video.video_stream_task_handle = NULL;
    return ESP_FAIL;
}

esp_err_t app_video_stream_task_stop(int video_fd)
{
    if (video_fd < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_camera_video.video_event_group == NULL ||
        app_camera_video.video_stream_task_handle == NULL) {
        return ESP_OK;
    }
    xEventGroupSetBits(app_camera_video.video_event_group, VIDEO_TASK_DELETE);

    return ESP_OK;
}

esp_err_t app_video_register_frame_operation_cb(app_video_frame_operation_cb_t operation_cb)
{
    app_camera_video.user_camera_video_frame_operation_cb = operation_cb;

    return ESP_OK;
}

esp_err_t app_video_stream_wait_stop(uint32_t timeout_ms)
{
    if (app_camera_video.video_event_group == NULL ||
        app_camera_video.video_stream_task_handle == NULL) {
        return ESP_OK;
    }
    const TickType_t ticks =
        (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(app_camera_video.video_event_group,
                                           VIDEO_TASK_DELETE_DONE, pdTRUE, pdTRUE, ticks);
    if ((bits & VIDEO_TASK_DELETE_DONE) == 0) {
        ESP_LOGW(TAG, "Video stream stop wait timeout (%u ms)", (unsigned)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Video Stream Task Stopped Done");

    return ESP_OK;
}
