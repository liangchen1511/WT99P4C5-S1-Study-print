#include "AiChat.hpp"
#include "parent_chat/parent_chat_api.hpp"
#include "parent_album_sync.h"
#include "parent_print_sync.h"
#include "xiaozhi_config.h"
#include "lv_font_ui_zh.h"
#include "lvgl.h"
#include "esp_err.h"
#include "esp_log.h"
#include <cstring>
LV_IMG_DECLARE(img_app_aichat);
static const char *TAG = "AiChat";
struct AiChatUiOp { AiChat *app; xz_svc_evt_t evt; };
AiChat::AiChat() : ESP_Brookesia_PhoneApp("AI助手", &img_app_aichat, true) {}
AiChat::~AiChat() = default;
void AiChat::setStatus(const char *txt)
{
    if (_status && txt) lv_label_set_text(_status, txt);
}
void AiChat::appendBubble(bool user, const char *txt)
{
    if (!_col || !txt || !txt[0]) return;
    lv_obj_t *row = lv_obj_create(_col);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *b = lv_obj_create(row);
    lv_obj_set_width(b, lv_pct(78));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_all(b, 10, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(b, lv_color_hex(user ? 0x2563EB : 0x334155), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *lb = lv_label_create(b);
    lv_obj_set_width(lb, lv_pct(100));
    lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lb, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lb, txt);
    if (_scroll) lv_obj_scroll_to_y(_scroll, LV_COORD_MAX, LV_ANIM_ON);
}
static void ui_apply_evt(void *p)
{
    AiChatUiOp *op = static_cast<AiChatUiOp *>(p);
    if (op && op->app) op->app->onSvcEvt(&op->evt);
    delete op;
}
void AiChat::onSvcEvt(const xz_svc_evt_t *e)
{
    if (!e) return;
    ESP_LOGI(TAG, "svc event state=%d line_len=%u text_len=%u user=%d",
             (int)e->state, (unsigned)strlen(e->line), (unsigned)strlen(e->text), (int)e->user);
    if (e->line[0]) setStatus(e->line);
    if (e->text[0]) appendBubble(e->user, e->text);
    if (e->state == XZ_SVC_NEED_ACTIVATE) lv_obj_add_state(_ptt, LV_STATE_DISABLED);
    else if (e->state == XZ_SVC_READY) lv_obj_clear_state(_ptt, LV_STATE_DISABLED);
    else if (e->state == XZ_SVC_ERROR) lv_obj_add_state(_ptt, LV_STATE_DISABLED);
}
void AiChat::svc_tramp(const xz_svc_evt_t *e, void *ctx)
{
    AiChat *app = static_cast<AiChat *>(ctx);
    if (!app || !e) return;
    AiChatUiOp *op = new AiChatUiOp{app, *e};
    lv_async_call(ui_apply_evt, op);
}
void AiChat::on_ptt(lv_event_t *e)
{
    AiChat *app = static_cast<AiChat *>(lv_event_get_user_data(e));
    if (!app) return;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (app->_ptt_dn) return;
        ESP_LOGI(TAG, "ptt pressed");
        esp_err_t err = xz_svc_ptt_down();
        ESP_LOGI(TAG, "ptt down result=%s", esp_err_to_name(err));
        if (err == ESP_OK) app->_ptt_dn = true;
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (!app->_ptt_dn) return;
        ESP_LOGI(TAG, "ptt released event=%d", (int)c);
        esp_err_t err = xz_svc_ptt_up();
        ESP_LOGI(TAG, "ptt up result=%s", esp_err_to_name(err));
        app->_ptt_dn = false;
    }
}
bool AiChat::run(void)
{
    parent_chat_bg_poll_stop();
    parent_album_sync_pause(true);
    parent_print_sync_pause(true);
    lv_area_t area = getVisualArea();
    const lv_coord_t vw = area.x2 - area.x1, vh = area.y2 - area.y1;
    _root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(_root, vw, vh);
    lv_obj_set_style_pad_all(_root, 8, 0);
    lv_obj_set_style_bg_color(_root, lv_color_hex(0x0F172A), 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    _status = lv_label_create(_root);
    lv_obj_set_width(_status, lv_pct(100));
    lv_label_set_long_mode(_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_status, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_status, lv_color_hex(0x94A3B8), 0);
    lv_label_set_text(_status, "初始化...");
    _scroll = lv_obj_create(_root);
    lv_obj_set_width(_scroll, lv_pct(100));
    lv_obj_set_flex_grow(_scroll, 1);
    lv_obj_set_style_bg_opa(_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_scroll, 0, 0);
    lv_obj_set_style_pad_all(_scroll, 4, 0);
    _col = lv_obj_create(_scroll);
    lv_obj_set_width(_col, lv_pct(100));
    lv_obj_set_height(_col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_col, 0, 0);
    lv_obj_set_style_pad_row(_col, 6, 0);
    lv_obj_set_flex_flow(_col, LV_FLEX_FLOW_COLUMN);
    _ptt = lv_btn_create(_root);
    lv_obj_set_width(_ptt, lv_pct(100));
    lv_obj_set_height(_ptt, 56);
    lv_obj_set_style_radius(_ptt, 16, 0);
    lv_obj_add_state(_ptt, LV_STATE_DISABLED);
    lv_obj_add_event_cb(_ptt, on_ptt, LV_EVENT_ALL, this);
    lv_obj_t *pl = lv_label_create(_ptt);
    lv_label_set_text(pl, "按住说话");
    lv_obj_set_style_text_font(pl, &lv_font_ui_zh_22, 0);
    lv_obj_center(pl);
    if (!_svc_on) {
        esp_err_t err = xz_svc_start(svc_tramp, this);
        ESP_LOGI(TAG, "service start result=%s", esp_err_to_name(err));
        if (err == ESP_OK) {
            _svc_on = true;
        } else {
            setStatus("AI service start failed");
        }
    }
    return true;
}
bool AiChat::pause(void)
{
    if (_ptt_dn) { xz_svc_ptt_up(); _ptt_dn = false; }
    parent_chat_bg_poll_wait_stop(XZ_BG_WAIT_MS);
    parent_album_sync_pause(false);
    parent_print_sync_pause(false);
    return true;
}
bool AiChat::resume(void)
{
    parent_chat_bg_poll_stop();
    parent_album_sync_pause(true);
    parent_print_sync_pause(true);
    return true;
}
bool AiChat::back(void)
{
    notifyCoreClosed();
    return true;
}
bool AiChat::close(void)
{
    if (_ptt_dn) { xz_svc_ptt_up(); _ptt_dn = false; }
    xz_svc_stop();
    _svc_on = false;
    parent_chat_bg_poll_wait_stop(XZ_BG_WAIT_MS);
    parent_album_sync_pause(false);
    parent_print_sync_pause(false);
    _root = _scroll = _col = _status = _ptt = nullptr;
    return true;
}
