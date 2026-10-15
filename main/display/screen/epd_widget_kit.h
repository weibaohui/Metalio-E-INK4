// main/display/screen/epd_widget_kit.h
// 墨水屏标准 UI 控件库 (EpdWidgetKit):
// 专为 480x800 墨水屏 (E-Ink) 设计的高对比度、零动效消耗、抗残影组件库。
// 帮助后续新 App 快速以声明式搭积木的方式完成界面开发, 统一整机视觉与交互规范。
#pragma once

#include <lvgl.h>
#include <functional>
#include <string>
#include "screen_common.h"

namespace EpdWidgetKit {

/**
 * @brief 墨水屏标准顶栏组件 (包含返回键、主标题与右侧可选操作区)
 */
class EpdHeader {
public:
    static lv_obj_t* Create(lv_obj_t* parent, const char* title, lv_event_cb_t on_back = nullptr);
};

/**
 * @brief 墨水屏标准列表条目组件 (Cell)
 */
class EpdCell {
public:
    /** @brief 文本展示/导航条目 (左侧标题, 右侧数值或箭头) */
    static lv_obj_t* CreateText(lv_obj_t* parent, const char* title, const char* value_text, lv_event_cb_t on_click = nullptr);

    /** @brief 开关配置条目 (左侧标题, 右侧 Switch 开关) */
    static lv_obj_t* CreateSwitch(lv_obj_t* parent, const char* title, bool initial_state, lv_event_cb_t on_toggle = nullptr);
};

/**
 * @brief 墨水屏轻量提示小浮窗 (Toast)
 */
class EpdToast {
public:
    static void Show(const char* message, uint32_t duration_ms = 2000);
};

/**
 * @brief 墨水屏标准二选一确认对话框 (Dialog)
 */
class EpdDialog {
public:
    using ResultCallback = std::function<void(bool confirmed)>;
    static void Show(const char* title, const char* message, const char* confirm_text, ResultCallback on_result);
};

/**
 * @brief 墨水屏滚动与上下翻页底层函数族 (EpdScroll)
 * 
 * 专为墨水屏设计的整页视口翻动与手势算法:
 * 1. 禁用高频动画渲染 (LV_ANIM_OFF), 杜绝滑动过程中的严重拖影与 SPI 阻塞;
 * 2. 自动预留重叠留白 (overlap_px), 保证阅读长文本/列表时长行不被视口截断;
 * 3. 集成 Claim-and-Cancel 机制, 手势翻页时自动抑制子控件误触 (防止松手误点卡片或按钮)。
 */
namespace EpdScroll {
    /**
     * @brief 视口整页翻动 (Page Scroll)
     * @param scrollable 可滚动的 LVGL 容器对象
     * @param dir 翻页方向: +1 向下/后翻 (下一页), -1 向上/前翻 (上一页)
     * @param overlap_px 两页重叠留白像素 (默认 40px)
     * @return true 实际发生了位移; false 已处于边界
     */
    bool PageTurn(lv_obj_t* scrollable, int dir, int overlap_px = 40);

    /** @brief 快速滚动到开头 */
    void ScrollToTop(lv_obj_t* scrollable);

    /** @brief 快速滚动到结尾 */
    void ScrollToBottom(lv_obj_t* scrollable);

    /**
     * @brief 计算当前对象的分页信息
     * @param scrollable 可滚动的 LVGL 容器对象
     * @param cur_page 当前页码 (从 1 开始)
     * @param total_pages 总页数 (至少为 1)
     * @param overlap_px 两页重叠留白像素
     */
    void GetPageInfo(lv_obj_t* scrollable, int* cur_page, int* total_pages, int overlap_px = 40);

    /**
     * @brief 为任意控件绑定上下滑动手势, 自动转化为整页翻动
     * @param touch_target 接收触摸手势的对象 (如当前 Screen 或滚动容器本身)
     * @param scrollable 实际被滚动的容器 (若为 nullptr 则与 touch_target 相同)
     * @param overlap_px 重叠留白像素
     * @param on_turn 翻页完成后的可选回调
     */
    void AttachSwipeGesture(lv_obj_t* touch_target, lv_obj_t* scrollable = nullptr, int overlap_px = 40, std::function<void(int dir)> on_turn = nullptr);
}

/**
 * @brief 墨水屏专用翻页滚动容器控件 (EpdPageScrollView)
 * 
 * 开箱即用的高级复合控件, 解决墨水屏无痛浏览长列表、长文本与卡片流的核心痛点:
 * 1. 内部集成专为墨水屏优化的视口容器, 零拖动残影;
 * 2. 原生支持触摸上下滑动 (Swipe Up/Down) 顺畅翻页;
 * 3. 内置高对比度大热区翻页控制栏 ([ ▲ 上一页 ] [ 第 1 / 4 页 ] [ ▼ 下一页 ]), 附带触觉振动反馈;
 * 4. 自动管理页码计算与首尾边界禁用状态;
 * 5. 生命周期绑定: 外部删除父容器时, C++ 控件对象自动安全析构。
 */
struct EpdPageScrollConfig {
    bool show_page_bar = true;       ///< 是否显示翻页控制栏
    bool auto_hide_single_page = true;///< 仅 1 页时自动隐藏翻页控制栏，释放屏幕空间 (默认开启)
    bool enable_swipe = true;         ///< 是否启用触摸上下滑动手势
    int overlap_px = 40;              ///< 翻页视口重叠留白 (像素)
    int bar_height = 48;              ///< 控制栏高度
    bool bar_at_bottom = true;        ///< 控制栏置于底部 (true) 或顶部 (false)
};

/**
 * @brief 墨水屏专用翻页滚动容器控件 (EpdPageScrollView)
 * 
 * 开箱即用的高级复合控件, 解决墨水屏无痛浏览长列表、长文本与卡片流的核心痛点:
 * 1. 内部集成专为墨水屏优化的视口容器, 零拖动残影;
 * 2. 原生支持触摸上下滑动 (Swipe Up/Down) 顺畅翻页;
 * 3. 内置高对比度大热区翻页控制栏 ([ ▲ 上一页 ] [ 第 1 / 4 页 ] [ ▼ 下一页 ]), 附带触觉振动反馈;
 * 4. 自动管理页码计算与首尾边界禁用状态;
 * 5. 支持业务层接管页码与拦截前后翻页事件;
 * 6. 生命周期绑定: 外部删除父容器时, C++ 控件对象自动安全析构。
 */
class EpdPageScrollView {
public:
    using Config = EpdPageScrollConfig;

    /**
     * @brief 创建墨水屏翻页滚动视图
     * @param parent 父容器
     * @param cfg 配置选项
     * @return 控件实例指针 (生命周期与 LVGL 关联, 销毁时自动析构)
     */
    static EpdPageScrollView* Create(lv_obj_t* parent, const Config& cfg = Config{});

    /**
     * @brief 获取可向内放置子控件/内容的内容容器
     */
    lv_obj_t* GetContainer() const { return container_; }

    /**
     * @brief 获取外层根对象 (用于排版布局)
     */
    lv_obj_t* GetRoot() const { return root_; }

    /** @brief 向上翻一页 (上一页) */
    bool PageUp();

    /** @brief 向下翻一页 (下一页) */
    bool PageDown();

    /** @brief 回到顶部 */
    void ScrollToTop();

    /** @brief 跳到底部 */
    void ScrollToBottom();

    /** @brief 内容动态增减后, 重新测量并刷新页码指示与按钮可用状态 */
    void UpdatePageInfo();

    /** @brief 允许应用自行接管页码显示 (如由业务数据决定的第 x / y 页) */
    void SetPageInfo(int cur_page, int total_pages);

    /** @brief 自定义中间页码文本 (如 "第 1 / 2 组织" 等) */
    void SetPageText(const char* text);

    /** @brief 重置为自动基于视口滚动高度计算页码 */
    void ResetAutoPageInfo();

    /** @brief 动态显隐翻页控制栏 */
    void SetPageBarVisible(bool visible);

    /** @brief 独立设置前后翻页按钮可用状态 */
    void SetNavButtonsEnabled(bool prev_enabled, bool next_enabled);

    /** @brief 注册自定义翻页动作拦截器 (dir: -1=上一页, +1=下一页; 若注册则点击前后按钮时优先调用此回调) */
    void SetOnPageNav(std::function<void(int dir)> on_nav) {
        on_page_nav_ = on_nav;
    }

    int GetCurrentPage() const { return cur_page_; }
    int GetTotalPages() const { return total_pages_; }

    /** @brief 注册翻页事件回调 */
    void SetOnPageChanged(std::function<void(int cur_page, int total_pages)> cb) {
        on_page_changed_ = cb;
    }

private:
    EpdPageScrollView() = default;
    ~EpdPageScrollView() = default;

    Config cfg_;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* prev_btn_ = nullptr;
    lv_obj_t* next_btn_ = nullptr;
    lv_obj_t* page_lbl_ = nullptr;

    int cur_page_ = 1;
    int total_pages_ = 1;
    bool manual_page_info_ = false;
    std::string custom_page_text_;
    std::function<void(int dir)> on_page_nav_;
    std::function<void(int cur_page, int total_pages)> on_page_changed_;

    void RefreshBarUI();
};

/**
 * @brief 墨水屏标准页面脚手架配置
 */
struct EpdPageScaffoldConfig {
    const char* title = nullptr;          ///< 页面标题 (若为 nullptr 则不创建 Header)
    bool show_status_bar = true;          ///< 是否挂载统一系统状态栏
    bool show_back_btn = true;            ///< Header 是否带返回键 (默认调 ScreenRequestBack)
    lv_event_cb_t on_back = nullptr;      ///< 自定义返回回调 (若为空则默认 ScreenRequestBack)
    bool scrollable = false;              ///< 内容区是否自动启用 EpdPageScrollView
    EpdPageScrollConfig scroll_cfg;       ///< 滚动配置 (当 scrollable=true 时生效)
    lv_coord_t pad_h = 16;                ///< 内容区左右内边距
    lv_coord_t pad_v = 12;                ///< 内容区上下内边距
    bool flex_column = true;              ///< 内容区是否默认启用纵向 Flex 流式排版
    lv_coord_t row_gap = 12;              ///< 内容区项目纵向间距
};

/**
 * @brief 墨水屏标准页面脚手架 (EpdPageScaffold)
 * 
 * 彻底消除各页面重复样板代码的标准页面脚手架:
 * 1. 自动挂载统一系统状态栏 (StatusBarManager);
 * 2. 自动生成标准墨水屏顶栏与大热区返回键;
 * 3. 自动根据顶栏高度计算视口高度, 避免由于硬编码偏移导致的穿透与遮挡;
 * 4. 可选一键装配墨水屏翻页滚动容器 (EpdPageScrollView);
 * 5. 自动管理页面及其挂载组件生命周期。
 */
class EpdPageScaffold {
public:
    using Config = EpdPageScaffoldConfig;

    /**
     * @brief 构建标准页面脚手架
     * @param cfg 配置选项
     * @return 脚手架指针 (销毁 Screen 时自动安全析构)
     */
    static EpdPageScaffold* Create(const Config& cfg = Config{});

    lv_obj_t* GetScreen() const { return scr_; }
    lv_obj_t* GetHeader() const { return header_; }
    lv_obj_t* GetBody() const { return body_; }
    lv_obj_t* GetContentContainer() const;
    EpdPageScrollView* GetScrollView() const { return scroll_view_; }
    const EpdStatusBar& GetStatusBar() const { return status_bar_; }

private:
    EpdPageScaffold() = default;
    ~EpdPageScaffold() = default;

    Config cfg_;
    lv_obj_t* scr_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* body_ = nullptr;
    EpdStatusBar status_bar_{};
    EpdPageScrollView* scroll_view_ = nullptr;
};

} // namespace EpdWidgetKit

