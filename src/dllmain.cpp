#include "config.h"
#include "log.h"
#include "skip.h"
#include "version.h"

#include <stdint.h>

namespace
{
    HMODULE g_self = nullptr;

    const char* BaseName(const char* path)
    {
        const char* name = path;
        for (const char* p = path; *p; ++p)
        {
            if (*p == '\\' || *p == '/')
                name = p + 1;
        }
        return name;
    }

    // Reads the fixed file version from the module's version resource (no version.dll needed).
    bool GetModuleVersion(HMODULE mod, DWORD& ms, DWORD& ls)
    {
        HRSRC res = FindResourceA(mod, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(16) /* RT_VERSION */);
        if (!res)
            return false;
        HGLOBAL handle = LoadResource(mod, res);
        DWORD size = SizeofResource(mod, res);
        auto data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
        if (!data || size < sizeof(VS_FIXEDFILEINFO))
            return false;
        for (DWORD i = 0; i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4)
        {
            auto info = reinterpret_cast<const VS_FIXEDFILEINFO*>(data + i);
            if (info->dwSignature == 0xFEEF04BD)
            {
                ms = info->dwFileVersionMS;
                ls = info->dwFileVersionLS;
                return true;
            }
        }
        return false;
    }

    BOOL CALLBACK FindOwnWindow(HWND window, LPARAM param)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(window))
        {
            *reinterpret_cast<HWND*>(param) = window;
            return FALSE;
        }
        return TRUE;
    }

    HWND WaitForGameWindow()
    {
        for (;;)
        {
            HWND window = nullptr;
            EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&window));
            if (window)
                return window;
            Sleep(250);
        }
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        logx::Open(g_self);
        char iniPath[MAX_PATH] = {};
        const bool hasIni = LoadConfig(g_self, iniPath, MAX_PATH);
        logx::SetLevel(g_config.logLevel);

        logx::Write(MOD_NAME " " MOD_VERSION " (built " __DATE__ " " __TIME__ ")");
        logx::Write("config: %s%s", iniPath, hasIni ? "" : " (not found, using defaults)");
        logx::Write("config: Enabled=%d SkipKey=%d UnlockCutscenes=%d LockedMode=%d LockedTailFrames=%d "
                    "AllowWaitFrames=%d ChainWindowMs=%d",
                    g_config.enabled, g_config.skipKey, g_config.unlockCutscenes, g_config.lockedMode,
                    g_config.lockedTailFrames, g_config.allowWaitFrames, g_config.chainWindowMs);
        logx::Write("config: UnlockTransitionMovies=%d MovieStableFrames=%d MovieIgnoreMinFrames=%d LogMovieNatives=%d SkipKeyInOtherChecks=%d SkipKillCams=%d "
                    "LogLevel=%d",
                    g_config.unlockMovies, g_config.movieStableFrames, g_config.movieIgnoreMinFrames, g_config.logMovieNatives,
                    g_config.skipKeyInOtherChecks,
                    g_config.skipKillCams, g_config.logLevel);

        HMODULE game = GetModuleHandleA(nullptr);
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(game, exePath, MAX_PATH);
        DWORD ms = 0, ls = 0;
        const bool hasVersion = GetModuleVersion(game, ms, ls);
        logx::Write("host: %s, version %u.%u.%u.%u", exePath, HIWORD(ms), LOWORD(ms), HIWORD(ls), LOWORD(ls));

        if (lstrcmpiA(BaseName(exePath), "MaxPayne3.exe") != 0)
        {
            logx::Write("host is not MaxPayne3.exe - staying idle");
            return 0;
        }
        if (!g_config.enabled)
        {
            logx::Write("Enabled=0 - staying idle, the game is left untouched");
            return 0;
        }
        if (!hasVersion || ms != TESTED_EXE_VERSION_MS || ls != TESTED_EXE_VERSION_LS)
            logx::Write("NOTE: this build was made for version 1.0.0.276; the signatures decide whether it works here");
        if (GetModuleHandleA("MaxPayne3.FusionFix.asi"))
        {
            logx::Write("MaxPayne3.FusionFix is loaded: it has its own cutscene skip that hooks the same code. "
                        "Staying idle to avoid a conflict - use only one of the two.");
            return 0;
        }

        // The first megabyte of the game's code is encrypted on disk and unpacked by the game's own startup stub
        // before its entry point runs. Wait until the game is past that (its window exists), then look for the code.
        logx::Write("waiting for the game window...");
        HWND window = WaitForGameWindow();
        char cls[64] = {};
        GetClassNameA(window, cls, sizeof(cls));
        logx::Write("game window 0x%08X (class \"%s\")", static_cast<unsigned>(reinterpret_cast<uintptr_t>(window)),
                    cls);

        // Normally the first attempt succeeds. Keep trying for a while in case the window showed up before the
        // game finished starting (a sign-in or update dialog, for example).
        skip::InstallResult result = skip::InstallResult::NotReady;
        for (int attempt = 0; attempt < 600 && result == skip::InstallResult::NotReady; ++attempt)
        {
            if (attempt == 1)
                logx::Write("the game's code is not ready yet - retrying every second for up to 10 minutes");
            if (attempt)
                Sleep(1000);
            result = skip::Install(game);
        }

        switch (result)
        {
        case skip::InstallResult::Ok:
            logx::Write("ready");
            break;
        case skip::InstallResult::NotReady:
            logx::Write("the game's cutscene code was not found (unsupported executable?) - "
                        "the game is left untouched");
            break;
        case skip::InstallResult::Unsupported:
            logx::Write("unsupported executable - the game is left untouched");
            break;
        case skip::InstallResult::Failed:
            logx::Write("installation failed - see the messages above");
            break;
        }
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
        // Nothing is done under the loader lock; the work happens on a separate thread.
        HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
