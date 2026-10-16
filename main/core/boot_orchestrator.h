// main/core/boot_orchestrator.h —— 系统开机引导编排器
#pragma once

class Application;

class BootOrchestrator {
public:
    static void Run(Application& app);
};
