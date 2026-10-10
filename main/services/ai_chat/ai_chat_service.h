#pragma once
// main/services/ai_chat/ai_chat_service.h —— AI 语音对话(新增 APP 的服务端)。
//
// 流程:按住说话录音(mic PCM)→ 松手打包 WAV → 语音转文字(STT,MiniMax/OpenAI 兼容
// 的 multipart 转写接口)→ 文字发给 OpenAI 兼容的大模型 chat completions → 得到回复。
// 内存精贵:管线任务仅在"录音→出结果"期间存在(内部栈;HTTPS/TLS 读 CA 证书要碰
// flash,不能 PSRAM 栈),做完即退;不录音不占任务内存。
//
// 配置(NVS "aichat"):STT 用 MiniMax Key;对话用通用 OpenAI 格式 Key(base+key+model)。
// 经 MCP 填写(self.aichat.*),落盘用内部栈任务(MCP 回调在协议任务,不直接写 flash)。

#include <string>

// 状态机(供 UI 轮询)
enum class AiChatState {
    Idle,         // 空闲
    Recording,    // 录音中(按住)
    Transcribing, // 语音转文字中
    Thinking,     // 大模型思考中
    Done,         // 完成(有回复)
    Error,        // 出错(见 AiChat_GetError)
};

// 开机/进屏初始化(main 任务):读 NVS 配置 + 确保 codec 输入可用。幂等。
void AiChat_Init();

// 录音控制(屏的按住/松手调用)。内部调度到管线任务,立即返回。
void AiChat_BeginRecord();     // 按住:开始录音
void AiChat_ReleaseRecord();   // 松手:停录并跑 STT+对话

// 状态/结果快照(纯读,任意任务可调)
AiChatState AiChat_GetState();
std::string AiChat_GetUserText();   // 识别出的话
std::string AiChat_GetReplyText();  // 大模型回复
std::string AiChat_GetError();      // 错误简述
std::string AiChat_GetStateText();  // 状态的中文短描述

// 配置查询(供 MCP/状态显示)
bool AiChat_SttConfigured();
bool AiChat_LlmConfigured();

// 注册 MCP 工具(self.aichat.*)。main 任务调用一次。
void AiChat_RegisterMcpTools();
// boot 期 APP 注册表钩子(kApps.register_mcp):仅当本 APP 开启时被调;
// Init(读 NVS 配置)+ 注册工具一次做完。main 任务,内部栈。
void AiChat_BootRegister();
