#pragma once
#include "esp_err.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    XZ_SVC_NEED_WIFI,
    XZ_SVC_NEED_ACTIVATE,
    XZ_SVC_CONNECTING,
    XZ_SVC_READY,
    XZ_SVC_LISTENING,
    XZ_SVC_THINKING,
    XZ_SVC_SPEAKING,
    XZ_SVC_ERROR,
    XZ_SVC_IDLE,
} xz_svc_state_t;
typedef struct {
    xz_svc_state_t state;
    char line[160];
    char text[512];
    bool user;
} xz_svc_evt_t;
typedef void (*xz_svc_cb_t)(const xz_svc_evt_t *e, void *ctx);
esp_err_t xz_svc_start(xz_svc_cb_t cb, void *ctx);
esp_err_t xz_svc_stop(void);
esp_err_t xz_svc_ptt_down(void);
esp_err_t xz_svc_ptt_up(void);
bool xz_svc_running(void);
#ifdef __cplusplus
}
#endif
