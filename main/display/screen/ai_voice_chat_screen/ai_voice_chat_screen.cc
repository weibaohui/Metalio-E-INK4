// main/display/screen/ai_voice_chat_screen/ai_voice_chat_screen.cc —— 见 .h(需求:AI 语音对话)。
#include "ai_voice_chat_screen/ai_voice_chat_screen.h"

#include <cmath>
#include <string>
#include <vector>

#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/ai_chat/ai_chat_service.h"
#include "services/ai_chat/minimax_speech.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "AiVoiceChat";
constexpr const char* kScreenId = "ai_voice_chat";

struct AvcUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* status_lbl = nullptr;   // 状态(录音中/识别中/思考中/播报中/出错)
    lv_obj_t* auto_tts_cb = nullptr;  // 自动发声勾选框
    lv_obj_t* talk_btn = nullptr;     // 按住说话
    lv_obj_t* talk_lbl = nullptr;
    lv_obj_t* stop_btn = nullptr;     // 停止/打断按钮
    lv_obj_t* stop_lbl = nullptr;
    lv_obj_t* chat_box = nullptr;     // 对话可滚动容器 (我+AI回答区)
    lv_obj_t* user_lbl = nullptr;     // 我: <识别文本>
    lv_obj_t* reply_lbl = nullptr;    // AI: <回复>
};
AvcUi s_ui;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

// ---- 语音设置弹窗状态 ----
minimax_speech::VoiceConfig s_temp_voice_cfg;
lv_obj_t* s_settings_mask = nullptr;

struct VoiceBtnItem {
    lv_obj_t* btn;
    lv_obj_t* lbl;
    std::string voice_id;
};
std::vector<VoiceBtnItem> s_voice_btns;

struct ChipBtnItem {
    lv_obj_t* btn;
    lv_obj_t* lbl;
    std::string val;
};
std::vector<ChipBtnItem> s_speed_btns;
std::vector<ChipBtnItem> s_emotion_btns;
std::vector<ChipBtnItem> s_vol_btns;

void UpdateSelectableBtn(lv_obj_t* btn, lv_obj_t* lbl, bool selected) {
    if (!btn || !lv_obj_is_valid(btn)) return;
    lv_obj_set_style_bg_color(btn, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    if (lbl && lv_obj_is_valid(lbl)) {
        lv_obj_set_style_text_color(lbl, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

void RefreshVoiceSettingsUi() {
    for (auto& item : s_voice_btns) {
        UpdateSelectableBtn(item.btn, item.lbl, item.voice_id == s_temp_voice_cfg.voice_id);
    }
    char spd_str[16];
    snprintf(spd_str, sizeof(spd_str), "%.1f", s_temp_voice_cfg.speed);
    for (auto& item : s_speed_btns) {
        UpdateSelectableBtn(item.btn, item.lbl, item.val == spd_str);
    }
    for (auto& item : s_emotion_btns) {
        UpdateSelectableBtn(item.btn, item.lbl, item.val == s_temp_voice_cfg.emotion);
    }
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%.1f", s_temp_voice_cfg.vol);
    for (auto& item : s_vol_btns) {
        UpdateSelectableBtn(item.btn, item.lbl, item.val == vol_str);
    }
}

void CloseVoiceSettingsModal() {
    if (s_settings_mask != nullptr && lv_obj_is_valid(s_settings_mask)) {
        lv_obj_delete(s_settings_mask);
    }
    s_settings_mask = nullptr;
    s_voice_btns.clear();
    s_speed_btns.clear();
    s_emotion_btns.clear();
    s_vol_btns.clear();
}

void OpenVoiceSettingsModal(lv_obj_t* parent_screen) {
    if (s_settings_mask != nullptr) {
        CloseVoiceSettingsModal();
    }
    s_temp_voice_cfg = minimax_speech::GetVoiceConfig();
    s_voice_btns.clear();
    s_speed_btns.clear();
    s_emotion_btns.clear();
    s_vol_btns.clear();

    // 遮罩层 (全屏半透明/网点)
    lv_obj_t* mask = lv_obj_create(parent_screen);
    s_settings_mask = mask;
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(mask, [](lv_event_t*) { CloseVoiceSettingsModal(); }, LV_EVENT_CLICKED, nullptr);

    // 弹窗卡片 (居中, 宽 LV_HOR_RES - 32, 高 LV_VER_RES - 100)
    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 32, LV_VER_RES - 100);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, [](lv_event_t* e) { lv_event_stop_bubbling(e); }, LV_EVENT_CLICKED, nullptr);

    // 顶部标题栏: 标题 + 关闭按钮
    lv_obj_t* head_row = lv_obj_create(card);
    lv_obj_remove_style_all(head_row);
    lv_obj_set_width(head_row, lv_pct(100));
    lv_obj_set_height(head_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(head_row);
    lv_obj_clear_flag(head_row, LV_OBJ_FLAG_CLICKABLE);

    ScreenMakeLabel(head_row, "语音播报音色设置", 25);

    lv_obj_t* close_icon_btn = lv_obj_create(head_row);
    lv_obj_remove_style_all(close_icon_btn);
    lv_obj_set_size(close_icon_btn, 36, 36);
    lv_obj_set_style_bg_color(close_icon_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(close_icon_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(close_icon_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(close_icon_btn, 2, 0);
    lv_obj_set_style_radius(close_icon_btn, 6, 0);
    lv_obj_add_flag(close_icon_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(close_icon_btn);
    lv_obj_t* close_lbl = lv_label_create(close_icon_btn);
    lv_label_set_text(close_lbl, "✕");
    lv_obj_set_style_text_font(close_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_black(), 0);
    lv_obj_center(close_lbl);
    lv_obj_clear_flag(close_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(close_icon_btn, [](lv_event_t*) { CloseVoiceSettingsModal(); }, LV_EVENT_CLICKED, nullptr);

    // 中间可滚动区域
    lv_obj_t* scroll_box = lv_obj_create(card);
    lv_obj_remove_style_all(scroll_box);
    lv_obj_set_width(scroll_box, lv_pct(100));
    lv_obj_set_flex_grow(scroll_box, 1);
    lv_obj_set_flex_flow(scroll_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(scroll_box, 12, 0);
    lv_obj_add_flag(scroll_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll_box, LV_DIR_VER);

    // 1. 音色选择
    ScreenMakeLabel(scroll_box, "音色选择 (点击切换)", 20);

    lv_obj_t* voice_grid = lv_obj_create(scroll_box);
    lv_obj_remove_style_all(voice_grid);
    lv_obj_set_width(voice_grid, lv_pct(100));
    lv_obj_set_height(voice_grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(voice_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(voice_grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(voice_grid, 8, 0);
    lv_obj_set_style_pad_column(voice_grid, 8, 0);
    lv_obj_clear_flag(voice_grid, LV_OBJ_FLAG_SCROLLABLE);

    const auto& voices = minimax_speech::GetSupportedVoices();
    for (const auto& v : voices) {
        lv_obj_t* vbtn = lv_obj_create(voice_grid);
        lv_obj_remove_style_all(vbtn);
        lv_obj_set_size(vbtn, lv_pct(48), 44);
        lv_obj_set_style_pad_all(vbtn, 6, 0);
        lv_obj_add_flag(vbtn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(vbtn, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(vbtn);

        lv_obj_t* vlbl = lv_label_create(vbtn);
        lv_label_set_text(vlbl, v.name);
        lv_obj_set_style_text_font(vlbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(vlbl);
        lv_obj_clear_flag(vlbl, LV_OBJ_FLAG_CLICKABLE);

        s_voice_btns.push_back({vbtn, vlbl, v.id});

        lv_obj_add_event_cb(vbtn, [](lv_event_t* e) {
            const char* vid = static_cast<const char*>(lv_event_get_user_data(e));
            if (vid != nullptr) {
                s_temp_voice_cfg.voice_id = vid;
                RefreshVoiceSettingsUi();
            }
        }, LV_EVENT_CLICKED, const_cast<char*>(v.id));
    }

    // 2. 语速选择
    ScreenMakeLabel(scroll_box, "语速调节", 20);
    lv_obj_t* spd_row = lv_obj_create(scroll_box);
    lv_obj_remove_style_all(spd_row);
    lv_obj_set_width(spd_row, lv_pct(100));
    lv_obj_set_height(spd_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(spd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(spd_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(spd_row, LV_OBJ_FLAG_SCROLLABLE);

    struct SpdOption { const char* name; const char* val; };
    const SpdOption spd_opts[] = {
        {"0.8x 慢", "0.8"},
        {"1.0x 标准", "1.0"},
        {"1.2x 稍快", "1.2"},
        {"1.5x 快速", "1.5"},
    };
    for (const auto& opt : spd_opts) {
        lv_obj_t* btn = lv_obj_create(spd_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, lv_pct(23), 38);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(btn);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, opt.name);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        s_speed_btns.push_back({btn, lbl, opt.val});

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            const char* val = static_cast<const char*>(lv_event_get_user_data(e));
            if (val != nullptr) {
                s_temp_voice_cfg.speed = std::strtof(val, nullptr);
                RefreshVoiceSettingsUi();
            }
        }, LV_EVENT_CLICKED, const_cast<char*>(opt.val));
    }

    // 3. 情绪表达
    ScreenMakeLabel(scroll_box, "情绪表达", 20);
    lv_obj_t* emo_row = lv_obj_create(scroll_box);
    lv_obj_remove_style_all(emo_row);
    lv_obj_set_width(emo_row, lv_pct(100));
    lv_obj_set_height(emo_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(emo_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(emo_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(emo_row, LV_OBJ_FLAG_SCROLLABLE);

    struct EmoOption { const char* name; const char* val; };
    const EmoOption emo_opts[] = {
        {"自然中性", "neutral"},
        {"欢快开心", "happy"},
        {"低沉沉静", "sad"},
        {"严肃有力", "angry"},
    };
    for (const auto& opt : emo_opts) {
        lv_obj_t* btn = lv_obj_create(emo_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, lv_pct(23), 38);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(btn);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, opt.name);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        s_emotion_btns.push_back({btn, lbl, opt.val});

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            const char* val = static_cast<const char*>(lv_event_get_user_data(e));
            if (val != nullptr) {
                s_temp_voice_cfg.emotion = val;
                RefreshVoiceSettingsUi();
            }
        }, LV_EVENT_CLICKED, const_cast<char*>(opt.val));
    }

    // 4. 音量大小
    ScreenMakeLabel(scroll_box, "合成音量", 20);
    lv_obj_t* vol_row = lv_obj_create(scroll_box);
    lv_obj_remove_style_all(vol_row);
    lv_obj_set_width(vol_row, lv_pct(100));
    lv_obj_set_height(vol_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(vol_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vol_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);

    struct VolOption { const char* name; const char* val; };
    const VolOption vol_opts[] = {
        {"0.8 柔和", "0.8"},
        {"1.0 标准", "1.0"},
        {"1.2 响亮", "1.2"},
    };
    for (const auto& opt : vol_opts) {
        lv_obj_t* btn = lv_obj_create(vol_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, lv_pct(31), 38);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        HapticAttachClick(btn);

        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, opt.name);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        s_vol_btns.push_back({btn, lbl, opt.val});

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            const char* val = static_cast<const char*>(lv_event_get_user_data(e));
            if (val != nullptr) {
                s_temp_voice_cfg.vol = std::strtof(val, nullptr);
                RefreshVoiceSettingsUi();
            }
        }, LV_EVENT_CLICKED, const_cast<char*>(opt.val));
    }

    RefreshVoiceSettingsUi();

    // 底部操作栏: 保存 / 关闭
    lv_obj_t* foot_row = lv_obj_create(card);
    lv_obj_remove_style_all(foot_row);
    lv_obj_set_width(foot_row, lv_pct(100));
    lv_obj_set_height(foot_row, 48);
    lv_obj_set_flex_flow(foot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(foot_row);
    lv_obj_clear_flag(foot_row, LV_OBJ_FLAG_CLICKABLE);

    // 保存按钮 (黑底白字)
    lv_obj_t* save_btn = lv_obj_create(foot_row);
    lv_obj_remove_style_all(save_btn);
    lv_obj_set_size(save_btn, 140, 44);
    lv_obj_set_style_bg_color(save_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(save_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(save_btn, 2, 0);
    lv_obj_set_style_radius(save_btn, 8, 0);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(save_btn);
    lv_obj_t* save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "保存设置");
    lv_obj_set_style_text_font(save_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(save_lbl, lv_color_white(), 0);
    lv_obj_center(save_lbl);
    lv_obj_clear_flag(save_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(save_btn, [](lv_event_t*) {
        minimax_speech::SaveVoiceConfig(s_temp_voice_cfg);
        CloseVoiceSettingsModal();
    }, LV_EVENT_CLICKED, nullptr);

    // 关闭/取消按钮 (白底黑字)
    lv_obj_t* cancel_btn = lv_obj_create(foot_row);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 120, 44);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(cancel_btn);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "关闭");
    lv_obj_set_style_text_font(cancel_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_black(), 0);
    lv_obj_center(cancel_lbl);
    lv_obj_clear_flag(cancel_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(cancel_btn, [](lv_event_t*) {
        CloseVoiceSettingsModal();
    }, LV_EVENT_CLICKED, nullptr);
}

static std::string s_last_rendered_user;
static std::string s_last_rendered_reply;
static std::string s_last_rendered_err;

void RefreshState() {
    if (!s_alive || s_ui.status_lbl == nullptr) return;
    lv_label_set_text(s_ui.status_lbl, AiChat_GetStateText().c_str());
    const auto st = AiChat_GetState();
    const bool recording = st == AiChatState::Recording;
    if (s_ui.talk_lbl != nullptr) {
        lv_label_set_text(s_ui.talk_lbl, recording ? "松开发送" : "按住说话");
    }
    if (s_ui.talk_btn != nullptr) {
        lv_obj_set_style_bg_color(s_ui.talk_btn, recording ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_ui.talk_btn, 0),
                                    recording ? lv_color_white() : lv_color_black(), 0);
    }
    const std::string user = AiChat_GetUserText();
    if (s_ui.user_lbl != nullptr && user != s_last_rendered_user) {
        s_last_rendered_user = user;
        if (!user.empty()) lv_label_set_text(s_ui.user_lbl, ("我:" + user).c_str());
        else lv_label_set_text(s_ui.user_lbl, "");
    }
    const std::string reply = AiChat_GetReplyText();
    const std::string err = AiChat_GetError();
    if (s_ui.reply_lbl != nullptr) {
        if (!reply.empty()) {
            if (reply != s_last_rendered_reply) {
                s_last_rendered_reply = reply;
                s_last_rendered_err.clear();
                lv_label_set_text(s_ui.reply_lbl, ("AI:" + reply).c_str());
                // 仅当大模型吐出新文本时自动跟随滚动到底部；生成完毕后不干扰用户自由上下滑动浏览
                if (s_ui.chat_box != nullptr && (st == AiChatState::Thinking || st == AiChatState::Speaking)) {
                    lv_obj_scroll_to_view(s_ui.reply_lbl, LV_ANIM_OFF);
                }
            }
        } else if (!err.empty()) {
            if (err != s_last_rendered_err) {
                s_last_rendered_err = err;
                s_last_rendered_reply.clear();
                lv_label_set_text(s_ui.reply_lbl, ("错误:" + err).c_str());
            }
        } else {
            if (!s_last_rendered_reply.empty() || !s_last_rendered_err.empty()) {
                s_last_rendered_reply.clear();
                s_last_rendered_err.clear();
                lv_label_set_text(s_ui.reply_lbl, "");
                // 新提问清空时, 将滚动条复位到顶部
                if (s_ui.chat_box != nullptr) {
                    lv_obj_scroll_to_y(s_ui.chat_box, 0, LV_ANIM_OFF);
                }
            }
        }
    }
}

static int64_t s_press_down_us = 0;

void OnTalkPressed(lv_event_t*) {
    // 按住: 开始录音 (若 STT/LLM 未配置则提示)
    if (!AiChat_SttConfigured() || !AiChat_LlmConfigured()) {
        if (s_ui.status_lbl != nullptr) {
            lv_label_set_text(s_ui.status_lbl, "先用 MCP 配置 aichat 的 stt_key 和 llm_key");
        }
        return;
    }
    s_press_down_us = esp_timer_get_time();
    HapticPulseIfEnabled();
    AiChat_BeginRecord();
    RefreshState();
}

void OnTalkReleased(lv_event_t*) {
    const int64_t held_ms = (s_press_down_us > 0) ? (esp_timer_get_time() - s_press_down_us) / 1000 : 0;
    s_press_down_us = 0;

    // 误触过滤: 按住时长 < 300ms 视为误触, 快速取消并复位 Idle
    if (held_ms < 300) {
        AiChat_CancelTap();
        RefreshState();
        return;
    }

    HapticPulseIfEnabled();
    AiChat_ReleaseRecord();
    RefreshState();
}

void OnStopClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    // 强制打断一切操作: 正在录音、转写中、大模型流式输出中、语音播报中一律立即终止并归位
    AiChat_StopAll();
    RefreshState();
}

void OnTimer(lv_timer_t*) {
    AiChat_ResetStaleState();   // 任务已死但状态卡在"播报中"等 → 归位 Idle
    RefreshState();
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    CloseVoiceSettingsModal();
    // 忙就取消:录音/播报/转写/思考任一进行中都停(播报也立刻断),
    // 否则退出后任务还在后台跑,"播报中"状态残留到下次进屏
    if (AiChat_IsBusy()) AiChat_AbortRecord();
    if (s_timer != nullptr) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_last_rendered_user.clear();
    s_last_rendered_reply.clear();
    s_last_rendered_err.clear();
    s_press_down_us = 0;
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

    // 标题行:左标题, 右边容器(自动发声勾选框 + 设置齿轮按钮)
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(head);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    ScreenMakeLabel(head, "AI 语音对话", 30);

    lv_obj_t* right_box = lv_obj_create(head);
    lv_obj_remove_style_all(right_box);
    lv_obj_set_size(right_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_box, 10, 0);
    DisableScroll(right_box);
    lv_obj_clear_flag(right_box, LV_OBJ_FLAG_CLICKABLE);

    s_ui.auto_tts_cb = lv_checkbox_create(right_box);
    lv_checkbox_set_text(s_ui.auto_tts_cb, "自动发声");
    lv_obj_set_style_text_color(s_ui.auto_tts_cb, lv_color_black(), 0);
    lv_obj_add_state(s_ui.auto_tts_cb, AiChat_AutoTts() ? LV_STATE_CHECKED : LV_STATE_DEFAULT);
    lv_obj_add_event_cb(s_ui.auto_tts_cb, [](lv_event_t* e) {
        auto* cb = static_cast<lv_obj_t*>(lv_event_get_target(e));
        AiChat_SetAutoTts(lv_obj_has_state(cb, LV_STATE_CHECKED));
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // 齿轮设置按钮
    lv_obj_t* gear_btn = lv_obj_create(right_box);
    lv_obj_remove_style_all(gear_btn);
    lv_obj_set_size(gear_btn, 68, 36);
    lv_obj_set_style_bg_color(gear_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(gear_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(gear_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(gear_btn, 2, 0);
    lv_obj_set_style_radius(gear_btn, 8, 0);
    lv_obj_add_flag(gear_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(gear_btn);
    lv_obj_t* gear_lbl = lv_label_create(gear_btn);
    lv_label_set_text(gear_lbl, "设置");
    lv_obj_set_style_text_font(gear_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(gear_lbl, lv_color_black(), 0);
    lv_obj_center(gear_lbl);
    lv_obj_clear_flag(gear_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(gear_btn, [](lv_event_t*) {
        OpenVoiceSettingsModal(s_ui.screen);
    }, LV_EVENT_CLICKED, nullptr);

    s_ui.status_lbl = ScreenMakeLabel(body, "按住说话", 25);
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // 对话显示区(我 + AI): 弹性撑满中间, 支持触摸上下滑动浏览长回复
    lv_obj_t* chat = lv_obj_create(body);
    lv_obj_remove_style_all(chat);
    lv_obj_set_width(chat, lv_pct(100));
    lv_obj_set_height(chat, 0);          // 显式设为 0, 让 flex_grow 精确计算剩余高度, 避免被长文本撑大越界
    lv_obj_set_flex_grow(chat, 1);
    lv_obj_set_flex_flow(chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chat, 8, 0);
    lv_obj_set_style_bg_color(chat, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chat, LV_OPA_COVER, 0); // 不透明背景, 确保触摸判定 100% 命中

    // 启用触摸点击与垂直手势滑动
    lv_obj_add_flag(chat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(chat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chat, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(chat, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scroll_dir(chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(chat, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(chat, lv_color_black(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(chat, LV_OPA_COVER, LV_PART_SCROLLBAR);
    s_ui.chat_box = chat;

    // wrap=true: 宽 100% + 长文自动折行, 回复再长也不超屏宽;
    // 超出对话区高度由容器竖向滚动承载 (支持手势上下滑动看全文)
    s_ui.user_lbl = ScreenMakeLabel(chat, "", 25, true);
    s_ui.reply_lbl = ScreenMakeLabel(chat, "", 25, true);
    lv_obj_add_flag(s_ui.user_lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_ui.reply_lbl, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 底部控制行: 左侧按住说话 + 右侧停止/打断小按钮 ("用那种停止的感觉，点击能打断一切")
    lv_obj_t* bottom_row = lv_obj_create(body);
    lv_obj_remove_style_all(bottom_row);
    lv_obj_set_size(bottom_row, lv_pct(100), 90);
    lv_obj_set_flex_flow(bottom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bottom_row, 10, 0);
    DisableScroll(bottom_row);
    lv_obj_clear_flag(bottom_row, LV_OBJ_FLAG_CLICKABLE);

    // 左侧: 按住说话大按钮
    s_ui.talk_btn = lv_obj_create(bottom_row);
    lv_obj_remove_style_all(s_ui.talk_btn);
    lv_obj_set_height(s_ui.talk_btn, 90);
    lv_obj_set_flex_grow(s_ui.talk_btn, 1);
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

    // 右侧: 停止/打断小按钮 ("用那种停止的感觉，点击能打断一切")
    s_ui.stop_btn = lv_obj_create(bottom_row);
    lv_obj_remove_style_all(s_ui.stop_btn);
    lv_obj_set_size(s_ui.stop_btn, 92, 90);
    lv_obj_set_style_bg_color(s_ui.stop_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_ui.stop_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ui.stop_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_ui.stop_btn, 3, 0);
    lv_obj_set_style_radius(s_ui.stop_btn, 16, 0);
    lv_obj_set_flex_flow(s_ui.stop_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ui.stop_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_ui.stop_btn, 4, 0);
    lv_obj_set_style_pad_row(s_ui.stop_btn, 4, 0);
    lv_obj_add_flag(s_ui.stop_btn, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(s_ui.stop_btn);
    HapticAttachClick(s_ui.stop_btn);
    lv_obj_add_event_cb(s_ui.stop_btn, OnStopClicked, LV_EVENT_CLICKED, nullptr);

    // 经典方形停止图标 (Solid square ■)
    lv_obj_t* stop_icon = lv_obj_create(s_ui.stop_btn);
    lv_obj_remove_style_all(stop_icon);
    lv_obj_set_size(stop_icon, 20, 20);
    lv_obj_set_style_bg_color(stop_icon, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(stop_icon, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stop_icon, 3, 0);
    lv_obj_clear_flag(stop_icon, LV_OBJ_FLAG_CLICKABLE);

    s_ui.stop_lbl = lv_label_create(s_ui.stop_btn);
    lv_label_set_text(s_ui.stop_lbl, "停止");
    lv_obj_set_style_text_font(s_ui.stop_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.stop_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.stop_lbl, LV_OBJ_FLAG_CLICKABLE);

    AiChat_ResetStaleState();   // 上次异常残留"播报中"等 → 进屏即归位
    RefreshState();
    s_timer = lv_timer_create(OnTimer, 200, nullptr);   // 200ms 轮询状态,录音指示跟手
    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{AiVoiceChatScreen::Create, nullptr});
    return screen;
}
