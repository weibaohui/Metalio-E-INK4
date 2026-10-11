#pragma once

#include <esp_err.h>

enum class ServiceState {
    Uninitialized,
    Stopped,
    Running,
    Error,
};

class AppService {
public:
    virtual ~AppService() = default;

    virtual const char* GetName() const = 0;
    virtual ServiceState GetState() const = 0;
    virtual esp_err_t Init() = 0;
    virtual esp_err_t Start() = 0;
    virtual void Stop() = 0;
};
