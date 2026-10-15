// main/display/status_bar_manager.h
// 统一状态栏管理器 (StatusBarManager):
// 集中仲裁并管理墨水屏顶层状态栏 (Status Bar) 的所有槽位与状态信息。
// 规则:
// 1. 左槽位 (Left Slot): 专属于系统网络 (WiFi 连接/断开状态), 硬件事件驱动, 业务只读;
// 2. 右槽位 (Right Slot): 专属于系统硬件 (静音、音量数字、电量百分比、电池图标), 业务禁止直接篡改;
// 3. 中槽位 (Center Slot): 统一由优先级仲裁器 (Priority Arbitrator) 调度:
//    - Critical: 系统级最高优先级 (强中断警告, 如极低电量、严重错误, 粘性显示直到手动清除);
//    - Transient: 临时提示消息 (Toast/瞬时通知, 2~3秒后自动归位);
//    - Ongoing: 活跃业务持续态 (如收音机台名 "收音机: 经典947"、AI 问答 "思考中...");
//    - Default: 静默空闲态 (系统时钟 HH:MM).
#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <cstdint>

enum class StatusPriority {
    Default = 0,     ///< 默认空闲态 (系统时钟 HH:MM)
    Ongoing = 1,     ///< 持续业务态 (如后台电台播放名、AI思考中、录音中)
    Transient = 2,   ///< 临时提示 (Toast/通知, 持续设定时长后自动归位)
    Critical = 3,    ///< 关键警告 (极低电量/网络断开等强提醒, 粘性直到手动清除)
};

class StatusBarManager {
public:
    static StatusBarManager& GetInstance();

    // ==================== 状态仲裁与内容发布接口 ====================

    /**
     * @brief 设置关键系统警告 (最高优先级，直到主动调用 ClearCriticalAlert)
     */
    void SetCriticalAlert(const std::string& message);
    void ClearCriticalAlert();

    /**
     * @brief 发送临时通知消息 (显示 duration_ms 毫秒后自动归位)
     */
    void ShowToast(const std::string& message, uint32_t duration_ms = 3000);

    /**
     * @brief 注册/更新业务持续运行状态 (例如 radio: "收音机: 经典947")
     * @param owner_id 业务唯一标识 (如 "radio", "ai_chat", "book")
     * @param message 状态文案
     */
    void SetOngoing(const std::string& owner_id, const std::string& message);

    /**
     * @brief 清除业务持续状态 (自动回退至上一级状态或时钟)
     */
    void ClearOngoing(const std::string& owner_id);

    /**
     * @brief 更新系统空闲时钟显示 (通常由时间服务或心跳更新)
     */
    void UpdateClock(const std::string& clock_str);

    // ==================== 状态栏视图绑定与生命周期 ====================

    /**
     * @brief 注册当前活跃页面的状态栏中心标签控件
     * @param status_label 状态栏居中文本 Label 对象
     */
    void BindStatusWidget(lv_obj_t* status_label);

    /**
     * @brief 解绑状态栏控件 (页面销毁时调用)
     */
    void UnbindStatusWidget(lv_obj_t* status_label);

    /**
     * @brief 强制重新计算仲裁优先级并刷新当前显示的文字
     */
    void Refresh();

    /**
     * @brief 获取当前正处于展示状态的有效文案
     */
    std::string GetCurrentDisplayMessage() const;

private:
    StatusBarManager();
    ~StatusBarManager();
    StatusBarManager(const StatusBarManager&) = delete;
    StatusBarManager& operator=(const StatusBarManager&) = delete;

    mutable std::mutex mutex_;
    lv_obj_t* bound_label_ = nullptr;

    std::string critical_msg_;
    std::string transient_msg_;
    uint64_t transient_expire_us_ = 0;
    void* transient_timer_ = nullptr;

    std::vector<std::pair<std::string, std::string>> ongoing_statuses_;
    std::string clock_str_;

    std::string current_rendered_text_;

    void ArbitrateLocked(std::string& out_msg, StatusPriority& out_prio) const;
    void ApplyToUiLocked(const std::string& text);
};
