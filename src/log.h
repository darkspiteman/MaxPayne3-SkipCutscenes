#pragma once
#include <windows.h>

namespace logx
{
    // Opens "<module path without extension>.log" (truncates the previous run's log).
    void Open(HMODULE self);
    // printf-like, limited to what wvsprintfA supports (%s %d %u %x %X %08X %c). No floating point.
    void Write(const char* fmt, ...);
    // Only written when LogLevel >= 2.
    void Debug(const char* fmt, ...);
    void SetLevel(int level);
}
