#pragma once
// main/display/screen/ai_voice_chat_screen/ai_voice_chat_screen.h —— AI 语音对话 APP。
// 按住"按住说话"录音,松手 → 语音转文字(MiniMax)→ 文字发给 OpenAI 兼容大模型 → 显示回复。
// 纯 UI,无后台常驻;返回即退出销毁。
#include <lvgl.h>

class AiVoiceChatScreen {
public:
    static lv_obj_t* Create();
};
