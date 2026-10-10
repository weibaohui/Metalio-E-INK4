#pragma once
// main/utils/psr_worker.h —— PSRAM 栈工作线程统一入口(公共功能沉淀,
// 见 docs/code-review-optimization-2026-10.md §3.2)。
//
// 全仓纪律:内部 RAM 精贵,后台任务栈放 PSRAM;PSRAM 栈任务禁直接读写
// NVS/flash(写 flash 关 cache 触发断言),要做请 Application::Schedule 到
// main 任务(内部栈)。
//
// 本助手做两件事:
// 1) 统一 WithCaps 创建(PSRAM 栈,TCB 仍在内部 RAM);
// 2) 任务函数 return 后自动 vTaskDeleteWithCaps 自删 —— 任务函数【不要再
//    自删】,return 即可。"WithCaps 创建忘了配对删除/删错句柄"这类 bug
//    从机制上消失。
//
// out_handle 可空。注意:任务函数一返回任务即删,句柄随之失效 —— 判断
// "还在不在跑"请用业务自己的句柄自清模式(参照 glm_usage_service 的
// worker_ 退出前自清),勿对句柄做 eTaskGetState(TCB 可能已回收)。

#include <new>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace psr_worker {

struct Ctx {
    void (*fn)(void*);
    void* arg;
};

inline void Trampoline(void* p) {
    Ctx* ctx = static_cast<Ctx*>(p);
    ctx->fn(ctx->arg);
    delete ctx;
    vTaskDeleteWithCaps(nullptr);   // 配对 WithCaps 创建,防 PSRAM 栈泄漏
}

}  // namespace psr_worker

// 创建 PSRAM 栈工作线程。core:0/1 钉核,-1 不钉。
// 返回 false = 创建失败(TCB/栈分配失败,内部 RAM 不足或碎片化)。
inline bool SpawnPsrWorker(const char* name, uint32_t stack_bytes, UBaseType_t prio,
                           void (*fn)(void*), void* arg = nullptr,
                           TaskHandle_t* out_handle = nullptr, int core = -1) {
    auto* ctx = new (std::nothrow) psr_worker::Ctx{fn, arg};
    if (ctx == nullptr) {
        return false;
    }
    const BaseType_t ok =
        core >= 0
            ? xTaskCreatePinnedToCoreWithCaps(psr_worker::Trampoline, name, stack_bytes, ctx,
                                              prio, out_handle, static_cast<BaseType_t>(core),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
            : xTaskCreateWithCaps(psr_worker::Trampoline, name, stack_bytes, ctx, prio,
                                  out_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        delete ctx;
        if (out_handle != nullptr) {
            *out_handle = nullptr;
        }
        return false;
    }
    return true;
}
