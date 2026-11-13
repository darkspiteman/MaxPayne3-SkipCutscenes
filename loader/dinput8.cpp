// Minimal open-source ASI loader for 32-bit games that import dinput8.dll (Max Payne 3 does).
//
// OPTIONAL. Use it only if you don't want a third-party loader binary: the game loads this file instead of the system
// dinput8.dll, every exported function is forwarded to the real one, and on the first call all *.asi files from the
// game's folder are loaded. Nothing is done inside DllMain.

#include <windows.h>

namespace
{
    HMODULE g_self = nullptr;
    HMODULE g_real = nullptr;
    FARPROC g_directInput8Create = nullptr;
    FARPROC g_dllCanUnloadNow = nullptr;
    FARPROC g_dllGetClassObject = nullptr;
    FARPROC g_dllRegisterServer = nullptr;
    FARPROC g_dllUnregisterServer = nullptr;
    INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
    HANDLE g_log = INVALID_HANDLE_VALUE;

    void Log(const char* fmt, ...)
    {
        if (g_log == INVALID_HANDLE_VALUE)
            return;
        char text[1024 + 8];
        va_list args;
        va_start(args, fmt);
        int n = wvsprintfA(text, fmt, args);
        va_end(args);
        text[n++] = '\r';
        text[n++] = '\n';
        DWORD written = 0;
        WriteFile(g_log, text, static_cast<DWORD>(n), &written, nullptr);
    }

    BOOL CALLBACK Initialize(PINIT_ONCE, PVOID, PVOID*)
    {
        char dir[MAX_PATH];
        DWORD len = GetModuleFileNameA(g_self, dir, MAX_PATH);
        while (len > 0 && dir[len - 1] != '\\' && dir[len - 1] != '/')
            --len;
        dir[len] = 0;
        if (len == 0 || len > MAX_PATH - 40)
            return TRUE;

        char path[MAX_PATH * 2];
        wsprintfA(path, "%sMP3AsiLoader.log", dir);
        g_log = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        Log("MP3 ASI loader (dinput8.dll proxy)");

        char system[MAX_PATH];
        UINT slen = GetSystemDirectoryA(system, MAX_PATH);
        if (slen == 0 || slen > MAX_PATH - 16)
            return TRUE;
        lstrcatA(system, "\\dinput8.dll");
        g_real = LoadLibraryA(system);
        Log("real library: %s => 0x%08X", system, static_cast<unsigned>(reinterpret_cast<UINT_PTR>(g_real)));
        if (g_real)
        {
            g_directInput8Create = GetProcAddress(g_real, "DirectInput8Create");
            g_dllCanUnloadNow = GetProcAddress(g_real, "DllCanUnloadNow");
            g_dllGetClassObject = GetProcAddress(g_real, "DllGetClassObject");
            g_dllRegisterServer = GetProcAddress(g_real, "DllRegisterServer");
            g_dllUnregisterServer = GetProcAddress(g_real, "DllUnregisterServer");
        }

        wsprintfA(path, "%s*.asi", dir);
        WIN32_FIND_DATAA find;
        HANDLE handle = FindFirstFileA(path, &find);
        if (handle != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                if (lstrlenA(find.cFileName) > MAX_PATH - 1)
                    continue;
                wsprintfA(path, "%s%s", dir, find.cFileName);
                HMODULE plugin = LoadLibraryA(path);
                if (plugin)
                    Log("loaded %s => 0x%08X", find.cFileName,
                        static_cast<unsigned>(reinterpret_cast<UINT_PTR>(plugin)));
                else
                    Log("FAILED to load %s (error %u)", find.cFileName, GetLastError());
            } while (FindNextFileA(handle, &find));
            FindClose(handle);
        }
        Log("done");
        return TRUE;
    }

    void Ensure()
    {
        InitOnceExecuteOnce(&g_once, Initialize, nullptr, nullptr);
    }
}

extern "C"
{
    HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE instance, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer)
    {
        Ensure();
        if (!g_directInput8Create)
            return E_FAIL;
        using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        return reinterpret_cast<Fn>(g_directInput8Create)(instance, version, iid, out, outer);
    }

    HRESULT WINAPI ProxyDllCanUnloadNow()
    {
        Ensure();
        if (!g_dllCanUnloadNow)
            return S_FALSE;
        return reinterpret_cast<HRESULT(WINAPI*)()>(g_dllCanUnloadNow)();
    }

    HRESULT WINAPI ProxyDllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* out)
    {
        Ensure();
        if (!g_dllGetClassObject)
            return CLASS_E_CLASSNOTAVAILABLE;
        using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
        return reinterpret_cast<Fn>(g_dllGetClassObject)(clsid, iid, out);
    }

    HRESULT WINAPI ProxyDllRegisterServer()
    {
        Ensure();
        if (!g_dllRegisterServer)
            return E_FAIL;
        return reinterpret_cast<HRESULT(WINAPI*)()>(g_dllRegisterServer)();
    }

    HRESULT WINAPI ProxyDllUnregisterServer()
    {
        Ensure();
        if (!g_dllUnregisterServer)
            return E_FAIL;
        return reinterpret_cast<HRESULT(WINAPI*)()>(g_dllUnregisterServer)();
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
