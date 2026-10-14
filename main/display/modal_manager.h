// main/display/modal_manager.h
// 统一模态对话框与浮层栈管理器 (ModalManager)
// 专为 480x800 墨水屏优化：
// 1. 拦截底层触摸穿透 (Anti-touch pass-through)，杜绝误触底层卡片/按键；
// 2. 避免墨水屏半透明半调网点残影，采用纯白卡片+极简高对比黑边设计；
// 3. 模态浮层栈式仲裁 (LIFO Stack)：返回键优先关闭顶层弹窗，避免页面被误退；
// 4. 切页自动清理残留弹窗，杜绝内存泄漏与野指针崩溃。
#pragma once

#include <lvgl.h>
#include <functional>
#include <string>
#include <vector>

class ModalManager {
public:
    static ModalManager& GetInstance();

    /**
     * @brief 弹出标准二选一确认对话框 (确认 / 取消)
     * @param title 对话框标题 (若为空则不显示标题栏)
     * @param message 提示消息正文
     * @param confirm_text 确认按钮文案 (默认 "确定")
     * @param cancel_text 取消按钮文案 (默认 "取消")
     * @param on_result 回调函数 (true: 点击确定; false: 点击取消或背景关闭)
     * @return 弹窗根对象指针
     */
    lv_obj_t* ShowDialog(const char* title,
                         const char* message,
                         const char* confirm_text = "确定",
                         const char* cancel_text = "取消",
                         std::function<void(bool confirmed)> on_result = nullptr);

    /**
     * @brief 弹出单按钮强提示对话框
     * @param title 标题
     * @param message 正文
     * @param btn_text 按钮文案 (默认 "我知道了")
     * @param on_close 关闭回调
     * @return 弹窗根对象指针
     */
    lv_obj_t* ShowAlert(const char* title,
                        const char* message,
                        const char* btn_text = "我知道了",
                        std::function<void()> on_close = nullptr);

    /**
     * @brief 压入自定义浮层/模态视图 (如设置面板、选择列表等)
     * @param builder 回调传入模态卡片容器 (parent)，业务自行向内填充子控件
     * @param on_dismiss 弹窗关闭时的清理回调
     * @return 弹窗根对象指针
     */
    lv_obj_t* PushCustomModal(std::function<void(lv_obj_t* card)> builder,
                              std::function<void()> on_dismiss = nullptr);

    /**
     * @brief 弹出并关闭当前最顶层模态弹窗
     * @return true 成功关闭了一个模态弹窗; false 当前无活跃弹窗
     */
    bool PopModal();

    /**
     * @brief 清除当前页面所有模态弹窗 (通常在页面卸载/切页时触发)
     */
    void DismissAll();

    /**
     * @brief 检查当前是否有活跃的模态弹窗在展示
     */
    bool HasActiveModal() const;

private:
    ModalManager() = default;
    ~ModalManager() = default;

    struct ModalEntry {
        lv_obj_t* backdrop = nullptr;
        lv_obj_t* card = nullptr;
        std::function<void()> on_dismiss;
    };

    std::vector<ModalEntry> stack_;
};
