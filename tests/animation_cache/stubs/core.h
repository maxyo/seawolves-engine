#pragma once

#include "platform/platform.hpp"
#include "v_file_service.h"

struct TestControls
{
    bool GetDebugAsyncKeyState(int) { return false; }
};

struct TestCore
{
    TestControls controls;
    TestControls *Controls = &controls;
    uint32_t deltaTime = 0;
    uint32_t realDeltaTime = 0;
    uint32_t GetDeltaTime() { return deltaTime; }
    uint32_t GetRDeltaTime() { return realDeltaTime; }
    void Trace(const char *, ...) {}
};

extern TestCore core;
