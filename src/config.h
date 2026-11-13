#pragma once
#include <windows.h>

struct Config
{
    int enabled = 1;
    // Extra skip key as a Windows virtual-key code (32 = VK_SPACE). 0 = only the game's own skip button.
    int skipKey = VK_SPACE;
    // Allow skipping cutscenes the scripts lock with CUTSCENE_ENABLE_SKIP(FALSE).
    int unlockCutscenes = 1;
    // What to do with a cutscene the scripts keep locked after everything was loaded:
    // 1 = move it to LockedTailFrames before its end and let it finish (the scripts' timed steps still run),
    // 2 = end it with the game's own skip anyway (what the old ScriptHook skipper did; faster, riskier).
    int lockedMode = 1;
    // How much of a script-locked cutscene still plays after the move, in cutscene frames (30 per second).
    int lockedTailFrames = 30;
    // Frames to wait after loading for the scripts to unlock the skip themselves.
    int allowWaitFrames = 4;
    // A cutscene or transition movie that starts within this many milliseconds after a skipped one ended is
    // skipped too, without another key press. 0 = off.
    int chainWindowMs = 2000;
    // The extra key also ends kill cameras (bullet cameras).
    int skipKillCams = 1;
    // Kill cameras the scripts stage themselves (a scripted shot that has to land) are not ended.
    int protectScriptedKillCams = 1;
    // A staged kill camera may be ended once it has run this long (the shot has landed by then). 0 = never.
    int scriptedKillCamDelayMs = 1500;
    // Make transition (loading) movies skippable once the game has no streaming requests left.
    int unlockMovies = 1;
    // Number of consecutive game frames without streaming requests before a movie becomes skippable.
    int movieStableFrames = 5;
    // Experimental, off: the game's own movie skip needs "frame > 240"; 1 = drop that limit once the game has nothing
    // left to load. In 0.2.5 this only paused the video (it stuttered) while the scripts kept the transition going
    // for its usual time, so nothing was gained.
    int movieIgnoreMinFrames = 0;
    // Diagnostics: log what the scripts do with the movie natives around a transition movie.
    int logMovieNatives = 1;
    // Also accept the extra skip key in the game's other "skip pressed" checks (transition movies).
    int skipKeyInOtherChecks = 1;
    // Diagnostics: write the frame rate to the log every 5 seconds.
    int logFps = 0;
    // Diagnostics: hex-dump the movie player's code and the skip checks to the log at startup.
    int dumpMovieCode = 0;
    // 0 = off, 1 = normal, 2 = verbose
    int logLevel = 1;
};

extern Config g_config;

// Reads "<module path without extension>.ini" (section [Main]); a missing file or key keeps the default.
// Returns true when the file exists.
bool LoadConfig(HMODULE self, char* pathOut, int pathSize);
