// main/display/screen/ai_voice_chat_screen/ai_voice_chat_screen.cc —— 见 .h(需求:AI 语音对话)。
#include "ai_voice_chat_screen/ai_voice_chat_screen.h"

#include <string>

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/ai_chat/ai_chat_service.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "AiVoiceChat";
constexpr const char* kScreenId = "ai_voice_chat";

struct AvcUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* status_lbl = nullptr;   // 状态(录音中/识别中/思考中/出错)
    lv_obj_t* talk_btn = nullptr;     // 按住说话
    lv_obj_t* talk_lbl = nullptr;
    lv_obj_t* user_lbl = nullptr;     // 我: <识别文本>
    lv_obj_t* reply_lbl = nullptr;    // AI: <回复>
};
AvcUi s_ui;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

void RefreshState() {
    if (!s_alive || s_ui.status_lbl == nullptr) return;
    lv_label_set_text(s_ui.status_lbl, AiChat_GetStateText().c_str());
    const bool recording = AiChat_GetState() == AiChatState::Recording;
    if (s_ui.talk_lbl != nullptr) {
        lv_label_set_text(s_ui.talk_lbl, recording ? "松开发送" : "按住说话");
    }
    if (s_ui.talk_btn != nullptr) {
        lv_obj_set_style_bg_color(s_ui.talk_btn, recording ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_ui.talk_btn, 0),
                                    recording ? lv_color_white() : lv_color_black(), 0);
    }
    const std::string user = AiChat_GetUserText();
    if (s_ui.user_lbl != nullptr && !user.empty()) {
        lv_label_set_text(s_ui.user_lbl, ("我:" + user).c_str());
    }
    const std::string reply = AiChat_GetReplyText();
    const std::string err = AiChat_GetError();
    if (s_ui.reply_lbl != nullptr) {
        if (!reply.empty()) lv_label_set_text(s_ui.reply_lbl, ("AI:" + reply).c_str());
        else if (!err.empty()) lv_label_set_text(s_ui.reply_lbl, ("错误:" + err).c_str());
    }
}

void OnTalkPressed(lv_event_t*) {
    // 按住:开始录音(若 STT/LLM 未配置则提示)
    if (!AiChat_SttConfigured() || !AiChat_LlmConfigured()) {
        if (s_ui.status_lbl != nullptr) {
            lv_label_set_text(s_ui.status_lbl, "先用 MCP 配置 aichat 的 stt_key 和 llm_key");
        }
        return;
    }
    HapticPulseIfEnabled();
    AiChat_BeginRecord();
    RefreshState();
}

void OnTalkReleased(lv_event_t*) {
    if (AiChat_GetState() != AiChatState::Recording) return;
    HapticPulseIfEnabled();
    AiChat_ReleaseRecord();
    RefreshState();
}

void OnTimer(lv_timer_t*) { RefreshState(); }

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    // 若还在录音就取消(不跑 STT,直接结束)
    if (AiChat_GetState() == AiChatState::Recording) AiChat_AbortRecord();
    if (s_timer != nullptr) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_ui = {};
}

}  // namespace

lv_obj_t* AiVoiceChatScreen::Create() {
    s_ui = {};
    s_alive = true;
    ScreenSetIsHome(false);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    DisableScroll(screen);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, 14, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 进屏:停掉语音管线(避免与采麦争抢),并确保 codec i2s 输入使能。
    // 两者都含 flash/NVS,须走 main 任务(内部栈);本回调在 LVGL worker(PSRAM 栈)。
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().StopAudioServiceIfRunning();
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec != nullptr) codec->Start();
    });

    ScreenMakeLabel(body, "AI 语音对话", 30);

    s_ui.status_lbl = ScreenMakeLabel(body, "按住说话", 25);
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // 按住说话大按钮
    s_ui.talk_btn = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.talk_btn);
    lv_obj_set_size(s_ui.talk_btn, lv_pct(100), 90);
    lv_obj_set_style_bg_color(s_ui.talk_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ui.talk_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.talk_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.talk_btn, 3, 0);
    lv_obj_set_style_radius(s_ui.talk_btn, 16, 0);
    lv_obj_add_flag(s_ui.talk_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui.talk_btn, OnTalkPressed, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_ui.talk_btn, OnTalkReleased, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(s_ui.talk_btn, OnTalkReleased, LV_EVENT_PRESS_LOST, nullptr);
    s_ui.talk_lbl = lv_label_create(s_ui.talk_btn);
    lv_label_set_text(s_ui.talk_lbl, "按住说话");
    lv_obj_set_style_text_font(s_ui.talk_lbl, fontpack_lv_font_get(30, 2), 0);
    lv_obj_set_style_text_color(s_ui.talk_lbl, lv_color_black(), 0);
    lv_obj_center(s_ui.talk_lbl);
    lv_obj_clear_flag(s_ui.talk_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 对话显示:我 + AI
    s_ui.user_lbl = ScreenMakeLabel(body, "");
    s_ui.reply_lbl = ScreenMakeLabel(body, "");

    RefreshState();
    s_timer = lv_timer_create(OnTimer, 200, nullptr);   // 200ms 轮询状态,录音指示跟手
    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{AiVoiceChatScreen::Create, nullptr});
    return screen;
}
