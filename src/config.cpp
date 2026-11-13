#include "config.h"

Config g_config;

namespace
{
    int Clamp(int v, int lo, int hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }
}

bool LoadConfig(HMODULE self, char* pathOut, int pathSize)
{
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 4)
        return false;
    for (int i = static_cast<int>(len) - 1; i >= 0 && path[i] != '\\' && path[i] != '/'; --i)
    {
        if (path[i] == '.')
        {
            path[i] = 0;
            break;
        }
    }
    lstrcatA(path, ".ini");
    if (pathOut && pathSize > 0)
        lstrcpynA(pathOut, path, pathSize);

    const char* s = "Main";
    Config d;
    g_config.enabled = GetPrivateProfileIntA(s, "Enabled", d.enabled, path) != 0;
    g_config.skipKey = Clamp(static_cast<int>(GetPrivateProfileIntA(s, "SkipKey", d.skipKey, path)), 0, 254);
    g_config.unlockCutscenes = GetPrivateProfileIntA(s, "UnlockCutscenes", d.unlockCutscenes, path) != 0;
    g_config.lockedMode = Clamp(static_cast<int>(GetPrivateProfileIntA(s, "LockedMode", d.lockedMode, path)), 1, 2);
    g_config.lockedTailFrames =
        Clamp(static_cast<int>(GetPrivateProfileIntA(s, "LockedTailFrames", d.lockedTailFrames, path)), 20, 450);
    g_config.allowWaitFrames =
        Clamp(static_cast<int>(GetPrivateProfileIntA(s, "AllowWaitFrames", d.allowWaitFrames, path)), 1, 120);
    g_config.chainWindowMs =
        Clamp(static_cast<int>(GetPrivateProfileIntA(s, "ChainWindowMs", d.chainWindowMs, path)), 0, 30000);
    g_config.skipKillCams = GetPrivateProfileIntA(s, "SkipKillCams", d.skipKillCams, path) != 0;
    g_config.protectScriptedKillCams =
        GetPrivateProfileIntA(s, "ProtectScriptedKillCams", d.protectScriptedKillCams, path) != 0;
    g_config.scriptedKillCamDelayMs = Clamp(
        static_cast<int>(GetPrivateProfileIntA(s, "ScriptedKillCamDelayMs", d.scriptedKillCamDelayMs, path)), 0, 60000);
    g_config.unlockMovies = GetPrivateProfileIntA(s, "UnlockTransitionMovies", d.unlockMovies, path) != 0;
    g_config.movieStableFrames =
        Clamp(static_cast<int>(GetPrivateProfileIntA(s, "MovieStableFrames", d.movieStableFrames, path)), 1, 600);
    g_config.movieIgnoreMinFrames =
        GetPrivateProfileIntA(s, "MovieIgnoreMinFrames", d.movieIgnoreMinFrames, path) != 0;
    g_config.logMovieNatives = GetPrivateProfileIntA(s, "LogMovieNatives", d.logMovieNatives, path) != 0;
    g_config.skipKeyInOtherChecks =
        GetPrivateProfileIntA(s, "SkipKeyInOtherChecks", d.skipKeyInOtherChecks, path) != 0;
    g_config.dumpMovieCode = GetPrivateProfileIntA(s, "DumpMovieCode", d.dumpMovieCode, path) != 0;
    g_config.logFps = GetPrivateProfileIntA(s, "LogFps", d.logFps, path) != 0;
    g_config.logLevel = Clamp(static_cast<int>(GetPrivateProfileIntA(s, "LogLevel", d.logLevel, path)), 0, 2);

    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}
