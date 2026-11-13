#include "log.h"

#include <stdarg.h>

namespace
{
    HANDLE g_file = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION g_lock;
    bool g_lockReady = false;
    int g_level = 1;

    void WriteLine(const char* fmt, va_list args)
    {
        if (g_file == INVALID_HANDLE_VALUE || !g_lockReady)
            return;

        char text[1024 + 32];
        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = wsprintfA(text, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        // wvsprintfA never writes more than 1024 bytes
        n += wvsprintfA(text + n, fmt, args);
        text[n++] = '\r';
        text[n++] = '\n';

        EnterCriticalSection(&g_lock);
        DWORD written = 0;
        WriteFile(g_file, text, static_cast<DWORD>(n), &written, nullptr);
        LeaveCriticalSection(&g_lock);
    }
}

namespace logx
{
    void Open(HMODULE self)
    {
        if (!g_lockReady)
        {
            InitializeCriticalSection(&g_lock);
            g_lockReady = true;
        }

        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH - 4)
            return;
        // replace the extension with .log
        for (int i = static_cast<int>(len) - 1; i >= 0 && path[i] != '\\' && path[i] != '/'; --i)
        {
            if (path[i] == '.')
            {
                path[i] = 0;
                break;
            }
        }
        lstrcatA(path, ".log");

        g_file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    void SetLevel(int level)
    {
        g_level = level;
    }

    void Write(const char* fmt, ...)
    {
        if (g_level < 1)
            return;
        va_list args;
        va_start(args, fmt);
        WriteLine(fmt, args);
        va_end(args);
    }

    void Debug(const char* fmt, ...)
    {
        if (g_level < 2)
            return;
        va_list args;
        va_start(args, fmt);
        WriteLine(fmt, args);
        va_end(args);
    }
}
