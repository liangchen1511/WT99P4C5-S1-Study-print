/*
 * SPDX-License-Identifier: Apache-2.0
 * Plan A: layered LVGL boot animation (~3s) for 9th / AI design logo.
 */

#include "boot_splash.h"
#include "boot_pin_coords.h"

#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

LV_IMG_DECLARE(img_boot_chip);
LV_IMG_DECLARE(img_boot_text);

#define BOOT_COLOR_BRAND        0x2C5077
#define BOOT_ZOOM_BASE          256
#define BOOT_ZOOM_START         220
#define BOOT_GAP_CHIP_TEXT      12
#define BOOT_PIN_SIZE           6
#define BOOT_FADE_OUT_MS        280
#define BOOT_PIN_RIGHT_BASE     15 /* first right-side pin in boot_pin_pos_nominal[] */

typedef struct {
    lv_obj_t *root;
    lv_obj_t *chip_grp;
    lv_obj_t *chip;
    lv_obj_t *text;
    lv_obj_t *scanline;
    lv_obj_t *pins[BOOT_PIN_COUNT];
    lv_anim_timeline_t *timeline;
    lv_coord_t chip_x;
    lv_coord_t chip_y;
    lv_coord_t text_x;
    lv_coord_t text_y;
    lv_coord_t group_w;
    lv_coord_t group_h;
} boot_ctx_t;

static boot_ctx_t s_boot;

static lv_coord_t pin_pos_x(lv_coord_t nom_x, lv_coord_t chip_w)
{
    return (lv_coord_t)((int32_t)nom_x * chip_w / BOOT_CHIP_NOMINAL_W);
}

static lv_coord_t pin_pos_y(lv_coord_t nom_y, lv_coord_t chip_h)
{
    return (lv_coord_t)((int32_t)nom_y * chip_h / BOOT_CHIP_NOMINAL_H);
}

static void anim_set_zoom(void *var, int32_t v)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void anim_set_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_set_x(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void anim_set_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static lv_obj_t *make_pin(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *pin = lv_obj_create(parent);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, BOOT_PIN_SIZE, BOOT_PIN_SIZE);
    lv_obj_set_style_radius(pin, BOOT_PIN_SIZE / 2, 0);
    lv_obj_set_style_bg_color(pin, lv_color_hex(BOOT_COLOR_BRAND), 0);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pin, 0, 0);
    lv_obj_set_pos(pin, x - BOOT_PIN_SIZE / 2, y - BOOT_PIN_SIZE / 2);
    lv_obj_set_style_opa(pin, LV_OPA_20, 0);
    return pin;
}

static void timeline_add_opa(lv_anim_timeline_t *at, uint32_t start_ms, lv_obj_t *obj, lv_opa_t v0, lv_opa_t v1,
                             uint32_t dur_ms, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, v0, v1);
    lv_anim_set_early_apply(&a, true);
    lv_anim_set_exec_cb(&a, anim_set_opa);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_time(&a, dur_ms);
    lv_anim_timeline_add(at, start_ms, &a);
}

void boot_splash_show(lv_obj_t *parent)
{
    memset(&s_boot, 0, sizeof(s_boot));

    const lv_coord_t chip_w = img_boot_chip.header.w;
    const lv_coord_t chip_h = img_boot_chip.header.h;
    const lv_coord_t text_w = img_boot_text.header.w;
    const lv_coord_t text_h = img_boot_text.header.h;

    lv_coord_t pin_extend_r = 0;
    for (int i = BOOT_PIN_RIGHT_BASE; i < BOOT_PIN_COUNT; i++) {
        lv_coord_t rx = pin_pos_x(boot_pin_pos_nominal[i][0], chip_w);
        pin_extend_r = LV_MAX(pin_extend_r, rx + (BOOT_PIN_SIZE / 2) + 6 - chip_w);
    }
    pin_extend_r = LV_MAX(0, pin_extend_r);

    s_boot.group_w = chip_w + pin_extend_r + BOOT_GAP_CHIP_TEXT + text_w;
    s_boot.group_h = LV_MAX(chip_h, text_h);
    s_boot.chip_x = (lv_coord_t)((LV_HOR_RES - s_boot.group_w) / 2);
    s_boot.chip_y = (lv_coord_t)((LV_VER_RES - s_boot.group_h) / 2);
    s_boot.text_x = s_boot.chip_x + chip_w + pin_extend_r + BOOT_GAP_CHIP_TEXT;
    s_boot.text_y = s_boot.chip_y + (s_boot.group_h - text_h) / 2;

    s_boot.root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_boot.root);
    lv_obj_set_size(s_boot.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_boot.root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_boot.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_boot.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_boot.root);

    s_boot.scanline = lv_obj_create(s_boot.root);
    lv_obj_remove_style_all(s_boot.scanline);
    lv_obj_set_size(s_boot.scanline, s_boot.group_w, 3);
    lv_obj_set_style_bg_color(s_boot.scanline, lv_color_hex(BOOT_COLOR_BRAND), 0);
    lv_obj_set_style_bg_opa(s_boot.scanline, LV_OPA_40, 0);
    lv_obj_set_pos(s_boot.scanline, s_boot.chip_x, s_boot.chip_y - 8);
    lv_obj_set_style_opa(s_boot.scanline, LV_OPA_TRANSP, 0);

    /* Text below chip/pins in z-order so slide-in does not cover pin dots. */
    s_boot.text = lv_img_create(s_boot.root);
    lv_img_set_src(s_boot.text, &img_boot_text);
    lv_obj_set_pos(s_boot.text, s_boot.text_x + 40, s_boot.text_y);
    lv_obj_set_style_opa(s_boot.text, LV_OPA_TRANSP, 0);

    s_boot.chip_grp = lv_obj_create(s_boot.root);
    lv_obj_remove_style_all(s_boot.chip_grp);
    lv_obj_set_size(s_boot.chip_grp, chip_w, chip_h);
    lv_obj_set_pos(s_boot.chip_grp, s_boot.chip_x, s_boot.chip_y);
    lv_obj_add_flag(s_boot.chip_grp, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_clear_flag(s_boot.chip_grp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_zoom(s_boot.chip_grp, BOOT_ZOOM_START, 0);
    lv_obj_set_style_transform_pivot_x(s_boot.chip_grp, chip_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_boot.chip_grp, chip_h / 2, 0);

    s_boot.chip = lv_img_create(s_boot.chip_grp);
    lv_img_set_src(s_boot.chip, &img_boot_chip);
    lv_obj_set_pos(s_boot.chip, 0, 0);

    for (int i = 0; i < BOOT_PIN_COUNT; i++) {
        lv_coord_t px = pin_pos_x(boot_pin_pos_nominal[i][0], chip_w);
        lv_coord_t py = pin_pos_y(boot_pin_pos_nominal[i][1], chip_h);
        s_boot.pins[i] = make_pin(s_boot.chip_grp, px, py);
    }

    lv_obj_move_foreground(s_boot.chip_grp);

    s_boot.timeline = lv_anim_timeline_create();

    /* 0.0–0.8s chip power-on */
    lv_anim_t a_zoom;
    lv_anim_init(&a_zoom);
    lv_anim_set_var(&a_zoom, s_boot.chip_grp);
    lv_anim_set_values(&a_zoom, BOOT_ZOOM_START, BOOT_ZOOM_BASE);
    lv_anim_set_early_apply(&a_zoom, true);
    lv_anim_set_exec_cb(&a_zoom, anim_set_zoom);
    lv_anim_set_path_cb(&a_zoom, lv_anim_path_overshoot);
    lv_anim_set_time(&a_zoom, 800);
    lv_anim_timeline_add(s_boot.timeline, 0, &a_zoom);

    for (int i = 0; i < BOOT_PIN_COUNT; i++) {
        timeline_add_opa(s_boot.timeline, 200 + (uint32_t)i * 32, s_boot.pins[i], LV_OPA_TRANSP, LV_OPA_COVER, 90,
                         lv_anim_path_ease_out);
    }

    /* 1.0–1.55s scan line across chip */
    timeline_add_opa(s_boot.timeline, 1000, s_boot.scanline, LV_OPA_TRANSP, LV_OPA_60, 60, lv_anim_path_linear);
    lv_anim_t a_scan_y;
    lv_anim_init(&a_scan_y);
    lv_anim_set_var(&a_scan_y, s_boot.scanline);
    lv_anim_set_values(&a_scan_y, s_boot.chip_y - 8, s_boot.chip_y + chip_h);
    lv_anim_set_early_apply(&a_scan_y, true);
    lv_anim_set_exec_cb(&a_scan_y, anim_set_y);
    lv_anim_set_path_cb(&a_scan_y, lv_anim_path_ease_in_out);
    lv_anim_set_time(&a_scan_y, 500);
    lv_anim_timeline_add(s_boot.timeline, 1000, &a_scan_y);
    timeline_add_opa(s_boot.timeline, 1500, s_boot.scanline, LV_OPA_60, LV_OPA_TRANSP, 120, lv_anim_path_linear);

    /* 1.55–2.25s slogan */
    lv_anim_t a_text_x;
    lv_anim_init(&a_text_x);
    lv_anim_set_var(&a_text_x, s_boot.text);
    lv_anim_set_values(&a_text_x, s_boot.text_x + 40, s_boot.text_x);
    lv_anim_set_early_apply(&a_text_x, true);
    lv_anim_set_exec_cb(&a_text_x, anim_set_x);
    lv_anim_set_path_cb(&a_text_x, lv_anim_path_ease_out);
    lv_anim_set_time(&a_text_x, 700);
    lv_anim_timeline_add(s_boot.timeline, 1550, &a_text_x);
    timeline_add_opa(s_boot.timeline, 1550, s_boot.text, LV_OPA_TRANSP, LV_OPA_COVER, 700, lv_anim_path_ease_out);

    /* 2.25–3.0s hold full composition, then short fade (after BOOT_SPLASH_DURATION_MS) */
    timeline_add_opa(s_boot.timeline, BOOT_SPLASH_DURATION_MS, s_boot.root, LV_OPA_COVER, LV_OPA_TRANSP, BOOT_FADE_OUT_MS,
                     lv_anim_path_ease_in);
}

void boot_splash_run(void)
{
    if (s_boot.timeline == NULL || s_boot.root == NULL) {
        return;
    }

    bsp_display_brightness_set(100);
    lv_obj_invalidate(s_boot.root);
    lv_refr_now(NULL);

    const uint32_t wait_ms = BOOT_SPLASH_DURATION_MS + BOOT_FADE_OUT_MS + 40;

    bsp_display_unlock();
    lv_anim_timeline_start(s_boot.timeline);
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    bsp_display_lock(0);
}

void boot_splash_dismiss(void)
{
    if (s_boot.timeline) {
        lv_anim_timeline_stop(s_boot.timeline);
        lv_anim_timeline_del(s_boot.timeline);
        s_boot.timeline = NULL;
    }
    if (s_boot.root) {
        lv_obj_del(s_boot.root);
        s_boot.root = NULL;
    }
    memset(&s_boot, 0, sizeof(s_boot));
}
