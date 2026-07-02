#pragma once
#include "esp_brookesia.hpp"
#include "xiaozhi_service.h"
class AiChat : public ESP_Brookesia_PhoneApp {
public:
    AiChat();
    ~AiChat() override;
    bool init(void) override { return true; }
    bool run(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool back(void) override;
    bool close(void) override;
    void onSvcEvt(const xz_svc_evt_t *e);
    void appendBubble(bool user, const char *txt);
    void setStatus(const char *txt);
private:
    static void on_ptt(lv_event_t *e);
    static void svc_tramp(const xz_svc_evt_t *e, void *ctx);
    lv_obj_t *_root = nullptr, *_scroll = nullptr, *_col = nullptr, *_status = nullptr, *_ptt = nullptr;
    bool _svc_on = false, _ptt_dn = false;
};
