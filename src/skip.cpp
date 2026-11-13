// Cutscene / transition movie skipping for Max Payne 3 (Steam, 1.0.0.276).
//
// The technique (signatures, CutSceneManager offsets and the "move a script-locked cutscene close to its end" idea)
// comes from MaxPayne3.FusionFix by ThirteenAG and contributors, source/cutsceneskip.ixx, licensed GPL-3.0:
//     https://github.com/ThirteenAG/MaxPayne3.FusionFix
// This file is an independent, dependency-free re-implementation of that module plus:
//     - an extra skip key (Space by default),
//     - strict run-time validation of everything found by signature,
//     - a diagnostic log.
//
// How it works
//   1. Cutscenes the scripts allow to skip: untouched, the game's own skip runs (the extra key is OR-ed into the
//      game's "skip just pressed" check).
//   2. Cutscenes the scripts lock with CUTSCENE_ENABLE_SKIP(FALSE): the cutscene is moved to a few seconds before its
//      end. The scripts' timed steps (requests for the next section, music, flags) still run, everything they asked
//      for is loaded synchronously (LOAD_ALL_OBJECTS_NOW) and the cutscene ends the normal way.
//   3. Transition movies (the Bink "loading" movies, s_*.bik): become skippable once the game has had no streaming
//      requests for a number of consecutive frames. The game's own skip (movie update, RVA 0xA9C90 in 1.0.0.276) needs
//      "skip pressed", the skippable flag and "current frame > 240"; most transition movies are shorter than 240
//      frames, so the "current frame" call of that check is hooked and reports a later frame once loading is done.

#include "skip.h"

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "mem.h"

#include <string.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace
{
    // ---------------------------------------------------------------------------------------------------------------
    // Game layout
    // ---------------------------------------------------------------------------------------------------------------

    // CutSceneManager (singleton)
    constexpr ptrdiff_t kCutsceneState = 0x104;       // int, 8 = playing
    constexpr ptrdiff_t kCutsceneActive = 0x130;      // byte, HAS_CUTSCENE_FINISHED = !active
    constexpr ptrdiff_t kCutsceneTime = 0x13C;        // float, seconds from the start frame
    constexpr ptrdiff_t kCutsceneStartFrame = 0x2D44; // int
    constexpr ptrdiff_t kCutsceneEndFrame = 0x2D48;   // int
    constexpr ptrdiff_t kCutsceneSeekPending = 0x3508; // int, 0 = seek to kCutsceneSeekFrame
    constexpr ptrdiff_t kCutsceneSeekFrame = 0x3520;  // int
    constexpr ptrdiff_t kCutsceneSkippable = 0x39F8;  // byte, CUTSCENE_ENABLE_SKIP
    // Movie player, slot 0 is the transition movie
    constexpr ptrdiff_t kMovieSkippable = 0x62;       // byte, SET_TRANSITION_MOVIE_SKIPPABLE
    constexpr ptrdiff_t kMovieNoSkip = 0x63;          // byte, the game's skip never works while it is set
    constexpr ptrdiff_t kMovieSkipped = 0x65;         // byte, set by the game's own skip
    constexpr int kMovieGameMinFrame = 240;           // the game's skip needs "current frame > 240"
    // Cutscene audio entity inside the object returned by GetCutsceneAudio()
    constexpr ptrdiff_t kCutsceneAudioEntity = 0x58;

    constexpr int kStatePlaying = 8;
    constexpr int kCutsceneFps = 30;

    // Script natives (hashes of the native names)
    constexpr uint32_t kNativeCutsceneEnableSkip = 0xE9BB90DF;         // CUTSCENE_ENABLE_SKIP
    constexpr uint32_t kNativeIsPauseMenuActive = 0xD3600591;          // IS_PAUSE_MENU_ACTIVE
    constexpr uint32_t kNativeGetNumberOfStreamingRequests = 0xC2EE9A02; // GET_NUMBER_OF_STREAMING_REQUESTS
    constexpr uint32_t kNativeLoadAllObjectsNow = 0xC9DBDA90;          // LOAD_ALL_OBJECTS_NOW
    constexpr uint32_t kNativeIsTransitionMoviePlaying = 0x66BD2CB0;   // IS_TRANSITION_MOVIE_PLAYING

    // Signatures. "?" is a wildcard byte.
    constexpr const char* kSigGameProcessCall = "E8 ? ? ? ? E8 ? ? ? ? 8B 0D ? ? ? ? 8B 15 ? ? ? ? 89 0D";
    constexpr const char* kSigGetNativeAddress = "53 8B 59 04 85 DB";
    constexpr const char* kSigNativeTable = "B9 ? ? ? ? E8 ? ? ? ? 85 C0 75 07";
    constexpr const char* kSigCutsceneManager = "83 3D ? ? ? ? 00 75 28 68 00 3A 00 00";
    constexpr const char* kSigMoviePlayer = "8B 44 24 04 50 6A 00 B9 ? ? ? ? E8 ? ? ? ? C3";
    constexpr const char* kSigSkipFlagReset = "C6 86 F8 39 00 00 01";
    constexpr const char* kSigSkipCheck =
        "56 8B F1 E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 6A 01 E8 ? ? ? ? 83 C4 04 8B C8 E8 ? ? ? ? "
        "8A 0D ? ? ? ? 0A 0D ? ? ? ? 74 0B 80 3D ? ? ? ? 00 74 02 32 C0 80 BE F8 39 00 00 00";
    // Inside the movie update, right after "skip just pressed":
    //     cmp byte [esi+62h],0 / je / cmp byte [esi+63h],0 / jne / mov ecx,esi / call GetFrame / cmp eax,240 / jle
    //     mov ecx,esi / mov byte [esi+65h],1 / call Stop
    constexpr const char* kSigMovieFrameGate =
        "80 7E 62 00 74 ? 80 7E 63 00 75 ? 8B CE E8 ? ? ? ? 3D F0 00 00 00 7E ? 8B CE C6 46 65 01 E8 ? ? ? ?";
    constexpr ptrdiff_t kGateGetFrameCall = 0x0E;
    constexpr ptrdiff_t kGateStopCall = 0x20;
    // push 1 / call GetControls / add esp,4 / mov ecx,eax / call IsSkipJustPressed
    constexpr const char* kSigSkipPressedSite = "6A 01 E8 ? ? ? ? 83 C4 04 8B C8 E8 ? ? ? ?";

    // Offsets inside the skip check function
    constexpr ptrdiff_t kCheckGetControlsCall = 0x12;
    constexpr ptrdiff_t kCheckIsSkipPressedCall = 0x1C;
    constexpr ptrdiff_t kCheckMultiplayerFlagA = 0x23;
    constexpr ptrdiff_t kCheckMultiplayerFlagB = 0x29;
    constexpr ptrdiff_t kCheckFrontendActive = 0x31;
    constexpr ptrdiff_t kCheckGetCutsceneAudioCall = 0x6A;
    constexpr ptrdiff_t kCheckAudioSkipSpeechCall = 0x8A;
    constexpr ptrdiff_t kCheckAudioSkipStreamsCall = 0xA3;

    struct NativeContext
    {
        void* thread;
        void* result;
        uint32_t argCount;
        void* args;
        uint32_t dataCount;
        void* originalData[4];
        float temporaryData[4][4];
        uint32_t stack[16];
    };

    struct NativeHandler
    {
        int32_t index;
        void(__cdecl* fn)(NativeContext*);
    };

    using GetNativeFn = NativeHandler*(__fastcall*)(void* table, void* edx, uint32_t hash);
    using IsSkipPressedFn = bool(__fastcall*)(void* controls, void* edx);

    struct Game
    {
        mem::Region image;
        mem::Region text;

        uint8_t* processCall;
        void(__cdecl* processOriginal)();

        GetNativeFn getNative;
        void* nativeTable;

        uint8_t** ppManager;
        uint8_t* skipCheck;
        void*(__cdecl* getControls)(int);
        IsSkipPressedFn isSkipPressed;
        void*(__cdecl* getCutsceneAudio)();
        void(__fastcall* audioSkipSpeech)(void* audio, void* edx, float seconds);
        void(__fastcall* audioSkipStreams)(void* audio, void* edx, float seconds);
        uint8_t* multiplayerFlagA;
        uint8_t* multiplayerFlagB;
        uint8_t* frontendActive;
        void(__fastcall* skipCheckOriginal)(uint8_t* manager, void* edx);
        uint8_t* resetSites[2];

        uint8_t* moviePlayer;
        uint8_t* movieGate;

        // What could be resolved and validated
        bool frameHook;
        bool natives;
        bool skipKeyInCutscenes;
        bool lockedCutscenes;
        bool movies;
        bool movieFrameGate;
    };

    Game G;

    // ---------------------------------------------------------------------------------------------------------------
    // Run-time state (game thread only, except the two flags written by the reset stubs)
    // ---------------------------------------------------------------------------------------------------------------

    constexpr uint32_t kNativeSetTransitionMovieSkippable = 0x4278FA3D; // SET_TRANSITION_MOVIE_SKIPPABLE
    constexpr uint32_t kNativeIsBulletCameraActive = 0xB5B03846;        // IS_BULLET_CAMERA_ACTIVE
    constexpr uint32_t kNativeTerminateGameBulletCamera = 0xEE7F2F5D;   // TERMINATE_GAME_BULLET_CAMERA

    // What the scripts asked for the current cutscene. The game resets its flag to "skippable" for every cutscene;
    // the reset stubs mirror that here.
    volatile uint8_t g_scriptAllowsSkip = 1;
    volatile uint8_t g_resetSeen = 0;

    unsigned g_frame = 0;

    bool g_keyDown = false;
    bool g_keyPulse = false;

    // --- the current cutscene ---
    enum class Phase
    {
        Idle,        // nothing requested, or waiting for the skip check to go live
        WaitAllow,   // everything was loaded; give the scripts a few frames to allow the skip themselves
        NeedSeek,    // the scripts keep it locked: move it close to its end from the skip check
        Seeking,     // moved; load what the scripts request at the new time, then let it end
        ForceNative, // LockedMode=2: end it with the game's own skip although the scripts lock it
        Done,
    };
    bool g_csActive = false;
    bool g_csIntent = false;    // the player wants this cutscene gone
    bool g_csCheckLive = false; // the game runs its skip check for this cutscene (it starts after the first second)
    bool g_csFire = false;      // make the game's own check see "skip pressed" right now
    bool g_csFired = false;
    Phase g_csPhase = Phase::Idle;
    int g_csWait = 0;
    int g_csLoads = 0;
    DWORD g_csIntentTick = 0;
    DWORD g_csFireTick = 0;

    // A script-locked cutscene moved towards its end
    int g_lockedSkipFrame = -1;
    int g_lockedSkipWait = 0;
    DWORD g_lockedSkipTick = 0;

    // Something was skipped and ended at this time: what starts right after it is skipped too
    bool g_chainValid = false;
    DWORD g_chainTick = 0;

    // --- the current transition movie ---
    bool g_moviePlaying = false;
    bool g_movieIntent = false;
    bool g_movieForce = false; // keep the movie skippable whatever the scripts say
    bool g_movieFire = false;  // one frame of "skip pressed" for the game's other skip checks
    bool g_movieUnlockLogged = false;
    DWORD g_movieStartTick = 0;
    int g_movieZeroFrames = 0;
    int g_movieLoads = 0;
    int g_movieFireCooldown = 0;
    int g_movieScriptCalls = 0;
    int g_movieScriptLast = -1;
    // Read by the movie update, which may run on another thread
    volatile bool g_movieLoaded = false; // nothing left to load: the game's minimum frame no longer applies
    volatile LONG g_movieGateFrame = -1; // the frame at which the minimum was bypassed
    int(__fastcall* g_movieFrameOriginal)(uint8_t* slot, void* edx) = nullptr;
    constexpr int kMaxOtherSites = 8;
    uintptr_t g_otherCallers[kMaxOtherSites] = {};
    unsigned g_otherCalls[kMaxOtherSites] = {};

    uintptr_t Rva(const void* p)
    {
        return p ? static_cast<uintptr_t>(static_cast<const uint8_t*>(p) - G.image.begin) : 0;
    }

    uint8_t* Manager()
    {
        return G.ppManager ? *G.ppManager : nullptr;
    }

    template <typename T> T& At(uint8_t* base, ptrdiff_t offset)
    {
        return *reinterpret_cast<T*>(base + offset);
    }

    bool ChainActive()
    {
        return g_config.chainWindowMs > 0 && g_chainValid &&
               GetTickCount() - g_chainTick <= static_cast<DWORD>(g_config.chainWindowMs);
    }

    void MarkChain()
    {
        g_chainValid = true;
        g_chainTick = GetTickCount();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Natives
    // ---------------------------------------------------------------------------------------------------------------

    NativeHandler* FindNative(uint32_t hash)
    {
        if (!G.natives)
            return nullptr;
        NativeHandler* handler = G.getNative(G.nativeTable, nullptr, hash);
        if (!handler || !handler->fn)
            return nullptr;
        return handler;
    }

    struct CachedNative
    {
        uint32_t hash;
        NativeHandler* handler;
    };

    CachedNative g_nativePauseMenu = {kNativeIsPauseMenuActive, nullptr};
    CachedNative g_nativeStreamingRequests = {kNativeGetNumberOfStreamingRequests, nullptr};
    CachedNative g_nativeLoadAllObjects = {kNativeLoadAllObjectsNow, nullptr};
    CachedNative g_nativeMoviePlaying = {kNativeIsTransitionMoviePlaying, nullptr};
    CachedNative g_nativeMovieFramesRemaining = {0xC483FB4A, nullptr}; // GET_TRANSITION_MOVIE_FRAMES_REMAINING
    DWORD g_movieSampleTick = 0;
    CachedNative g_nativeBulletCamActive = {kNativeIsBulletCameraActive, nullptr};
    CachedNative g_nativeBulletCamTerminate = {kNativeTerminateGameBulletCamera, nullptr};

    // Calls a native without arguments. Returns false when the native isn't registered (yet).
    bool CallNative(CachedNative& native, uint32_t* result)
    {
        if (!native.handler)
        {
            native.handler = FindNative(native.hash);
            if (!native.handler)
                return false;
        }
        NativeContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.thread = ctx.stack;
        ctx.args = ctx.stack;
        ctx.result = ctx.stack;
        native.handler->fn(&ctx);
        if (result)
            *result = ctx.stack[0];
        return true;
    }

    int StreamingRequests()
    {
        uint32_t value = 0;
        if (!CallNative(g_nativeStreamingRequests, &value))
            return -1;
        return static_cast<int>(value);
    }

    // Loads everything the game has requested, synchronously. Returns how long it took.
    DWORD LoadAllObjectsNow()
    {
        const DWORD before = GetTickCount();
        CallNative(g_nativeLoadAllObjects, nullptr);
        return GetTickCount() - before;
    }

    void(__cdecl* g_enableSkipOriginal)(NativeContext*) = nullptr;
    void(__cdecl* g_movieSkippableOriginal)(NativeContext*) = nullptr;

    // The scripts' CUTSCENE_ENABLE_SKIP(toggle). The game's flag is kept "skippable" so the skip prompt stays up and
    // the skip check keeps running; what the scripts wanted is remembered in g_scriptAllowsSkip.
    void __cdecl EnableSkipHook(NativeContext* ctx)
    {
        auto args = ctx ? static_cast<uint32_t*>(ctx->args) : nullptr;
        if (!args)
        {
            g_enableSkipOriginal(ctx);
            return;
        }

        const uint32_t requested = args[0];
        const uint8_t allows = requested != 0 ? 1 : 0;
        if (allows != g_scriptAllowsSkip)
        {
            g_scriptAllowsSkip = allows;
            logx::Write("cutscene: scripts %s the skip (CUTSCENE_ENABLE_SKIP(%u))", allows ? "allow" : "LOCK",
                        requested);
        }

        if (!requested && g_config.enabled && g_config.unlockCutscenes && G.lockedCutscenes)
        {
            args[0] = 1;
            g_enableSkipOriginal(ctx);
            args[0] = requested;
            return;
        }
        g_enableSkipOriginal(ctx);
    }

    // The scripts' SET_TRANSITION_MOVIE_SKIPPABLE(toggle)
    void __cdecl MovieSkippableHook(NativeContext* ctx)
    {
        auto args = ctx ? static_cast<uint32_t*>(ctx->args) : nullptr;
        if (!args)
        {
            g_movieSkippableOriginal(ctx);
            return;
        }

        const uint32_t requested = args[0];
        ++g_movieScriptCalls;
        if (static_cast<int>(requested != 0) != g_movieScriptLast)
        {
            g_movieScriptLast = requested != 0;
            logx::Write("movie: scripts set skippable = %u (SET_TRANSITION_MOVIE_SKIPPABLE)", requested);
        }

        if (!requested && g_config.enabled && g_movieForce && g_moviePlaying)
        {
            args[0] = 1;
            g_movieSkippableOriginal(ctx);
            args[0] = requested;
            return;
        }
        g_movieSkippableOriginal(ctx);
    }

    bool WritePointer(void** where, void* value)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(where, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
            return false;
        const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mbi.Protect & writable)
        {
            *where = value;
            return true;
        }
        DWORD oldProtect = 0;
        if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &oldProtect))
            return false;
        *where = value;
        VirtualProtect(where, sizeof(void*), oldProtect, &oldProtect);
        return true;
    }

    // Natives the scripts use to stage a kill camera of their own (a scripted shot that has to land, like the
    // hostage rescue in chapter 1). A kill camera that follows one of these calls is left alone.
    struct CamMarker
    {
        const char* name;
        uint32_t hash;
        bool arms; // a call marks the next kill camera as staged by the scripts
    };
    constexpr CamMarker kCamMarkers[] = {
        {"SETUP_BULLET_CAMERA_SCENE", 0x536FD4DA, true},
        {"SETUP_BULLET_CAMERA_ANIM_SCENE", 0xC527E29F, true},
        {"SET_BULLET_CAMERA_VICTIM", 0xFB6C221F, true},
        {"SET_BULLET_CAM_ON_NEXT_PROJECTILE", 0xE9B08533, true},
        {"SET_PED_BULLET_CAMERA_ON_NEXT_HIT", 0xF43737FB, true},
        {"SET_BULLET_CAMERA_TRACKED_PED", 0x450D1234, true},
        {"SET_BULLET_CAMERA_TRACKER", 0x0B09B233, true},
        {"SET_VEHICLE_BULLET_CAMERA_ON_NEXT_HIT", 0x7394BC1C, true},
        {"SET_PED_VALIDATE_BULLET_CAM_HIT", 0xFBB7E5C8, true},
        {"ALLOW_BULLET_CAMERA_CHEAT", 0x3A19D0D1, false},
        {"ALLOW_BULLET_CAMERA_SLOWDOWN", 0xC6AB9205, false},
        {"ALLOW_BULLET_CAMERA_TO_GAMEPLAY", 0x2BB2C58F, false},
        {"BULLET_CAMERA_ALLOW_TRACK_BULLET", 0x5754CF87, false},
        {"BULLET_CAMERA_TRACK_UNTIL_GROUNDED", 0xF32B2C96, false},
        {"ENABLE_BULLET_CAMERA_WIPE", 0xDCCD38E5, false},
        {"FORCE_CUSTOM_WIPE_BULLET_CAMERA", 0xDC58C1A2, false},
        {"SET_BULLET_CAM_PREF", 0xC82AD002, true},
        {"SET_BULLET_CAMERA_OBJECT_TRACKER", 0x4E75A262, false},
        {"SET_PED_CAN_INITIATE_BULLET_CAM", 0x63621CC9, false},
        {"SET_PED_CAN_TRIGGER_BULLET_CAM", 0x7DF0538C, false},
        {"RESET_BULLET_CAMERA_TIMEWARP_SCALE", 0x69EE7ED4, false},
        {"ADD_BULLET_CAM_TEXT", 0x3A5A1CDB, false},
        {"SET_OBJECT_BULLET_CAM_PREF", 0x35B1DB56, false},
        {"SET_TIME_SCALE", 0xA7F84694, false},
    };
    constexpr int kCamMarkerCount = sizeof(kCamMarkers) / sizeof(kCamMarkers[0]);
    void(__cdecl* g_camMarkerOriginal[kCamMarkerCount])(NativeContext*) = {};
    unsigned g_camMarkerCalls[kCamMarkerCount] = {};
    DWORD g_camMarkerTick[kCamMarkerCount] = {};
    uint32_t g_camMarkerArgs[kCamMarkerCount][2] = {};
    bool g_camScripted = false;
    DWORD g_camScriptedTick = 0;
    bool g_camActive = false;
    bool g_camRefusedLogged = false;
    DWORD g_camStartTick = 0;

    void OnCamMarker(int index, NativeContext* ctx)
    {
        auto args = ctx ? static_cast<uint32_t*>(ctx->args) : nullptr;
        ++g_camMarkerCalls[index];
        g_camMarkerTick[index] = GetTickCount();
        g_camMarkerArgs[index][0] = args ? args[0] : 0;
        g_camMarkerArgs[index][1] = args ? args[1] : 0;
        if (!kCamMarkers[index].arms)
            return;
        // SET_PED_VALIDATE_BULLET_CAM_HIT(ped, FALSE) switches the requirement off
        if (kCamMarkers[index].hash == 0xFBB7E5C8 && args && args[1] == 0)
            return;
        g_camScripted = true;
        g_camScriptedTick = GetTickCount();
    }

    template <int N> void __cdecl CamMarkerHook(NativeContext* ctx)
    {
        OnCamMarker(N, ctx);
        g_camMarkerOriginal[N](ctx);
    }

    // Diagnostics (LogMovieNatives=1): what the scripts do with the movie natives around a transition movie
    struct MovieProbe
    {
        const char* name;
        uint32_t hash;
        bool quiet; // called every frame: only counted and summarised
    };
    constexpr MovieProbe kMovieProbes[] = {
        {"GET_TRANSITION_MOVIE_FRAMES_REMAINING", 0xC483FB4A, false},
        {"CUTSCENE_SET_WAIT_FOR_MOVIE", 0x8D86C813, false},
        {"CUTSCENE_IS_WAITING_FOR_MOVIE", 0x04C1C831, false},
        {"PRELOAD_MOVIE", 0xC3182968, false},
        {"PLAY_MOVIE", 0x4F39ED7A, false},
        {"STOP_MOVIE", 0x7827E516, false},
        {"RELEASE_MOVIE", 0x9EAF5CA0, false},
        {"SET_MOVIE_FRAME", 0xFD8B4DAA, false},
        {"SET_MOVIE_TIME", 0xA1930880, false},
        {"SET_MOVIE_LOOP", 0x14A87C9F, false},
        {"IS_SCRIPT_MOVIE_PLAYING", 0x772006AE, false},
        {"GET_SCRIPT_MOVIE_FRAMES_REMAINING", 0xA173AAD6, false},
        {"DRAW_MOVIE", 0xF563194B, true},
    };
    constexpr int kMovieProbeCount = sizeof(kMovieProbes) / sizeof(kMovieProbes[0]);
    struct MovieProbeState
    {
        void(__cdecl* original)(NativeContext*);
        unsigned calls;     // since the current movie started
        DWORD tick;         // the last call
        uint32_t argCount, args[2], result;
        bool logged;
        DWORD loggedTick;
        uint32_t loggedArgs[2], loggedResult;
    };
    MovieProbeState g_movieProbes[kMovieProbeCount] = {};
    bool g_movieProbeSilent = false; // the mod's own call
    DWORD g_movieEndTick = 0;

    void OnMovieProbe(int index, NativeContext* ctx)
    {
        MovieProbeState& p = g_movieProbes[index];
        p.original(ctx);
        if (g_movieProbeSilent || !ctx)
            return;
        auto args = static_cast<uint32_t*>(ctx->args);
        const DWORD now = GetTickCount();
        ++p.calls;
        p.tick = now;
        p.argCount = ctx->argCount;
        p.args[0] = args && ctx->argCount > 0 ? args[0] : 0;
        p.args[1] = args && ctx->argCount > 1 ? args[1] : 0;
        p.result = ctx->result ? *static_cast<uint32_t*>(ctx->result) : 0;
        if (kMovieProbes[index].quiet || !(g_moviePlaying || now - g_movieEndTick < 2000))
            return;
        const bool changed = !p.logged || p.args[0] != p.loggedArgs[0] || p.args[1] != p.loggedArgs[1] ||
                             p.result != p.loggedResult;
        if (!changed || (p.logged && now - p.loggedTick < 300))
            return;
        p.logged = true;
        p.loggedTick = now;
        p.loggedArgs[0] = p.args[0];
        p.loggedArgs[1] = p.args[1];
        p.loggedResult = p.result;
        logx::Write("movie:   scripts: %s(%u args: 0x%X, 0x%X) -> %d, call #%u, %u ms after the movie's start",
                    kMovieProbes[index].name, p.argCount, p.args[0], p.args[1], static_cast<int>(p.result), p.calls,
                    now - g_movieStartTick);
    }

    template <int N> void __cdecl MovieProbeHook(NativeContext* ctx)
    {
        OnMovieProbe(N, ctx);
    }

    struct NativeHook
    {
        const char* name;
        uint32_t hash;
        void(__cdecl* hook)(NativeContext*);
        void(__cdecl** original)(NativeContext*);
        bool installed;
        bool failed;
    };

    NativeHook g_nativeHooks[] = {
        {"CUTSCENE_ENABLE_SKIP", kNativeCutsceneEnableSkip, EnableSkipHook, &g_enableSkipOriginal, false, false},
        {"SET_TRANSITION_MOVIE_SKIPPABLE", kNativeSetTransitionMovieSkippable, MovieSkippableHook,
         &g_movieSkippableOriginal, false, false},
#define CAM_MARKER(N) {kCamMarkers[N].name, kCamMarkers[N].hash, CamMarkerHook<N>, &g_camMarkerOriginal[N], false, false}
        CAM_MARKER(0), CAM_MARKER(1), CAM_MARKER(2), CAM_MARKER(3), CAM_MARKER(4), CAM_MARKER(5),
        CAM_MARKER(6), CAM_MARKER(7), CAM_MARKER(8), CAM_MARKER(9), CAM_MARKER(10), CAM_MARKER(11),
        CAM_MARKER(12), CAM_MARKER(13), CAM_MARKER(14), CAM_MARKER(15), CAM_MARKER(16), CAM_MARKER(17),
        CAM_MARKER(18), CAM_MARKER(19), CAM_MARKER(20), CAM_MARKER(21), CAM_MARKER(22), CAM_MARKER(23),
#undef CAM_MARKER
#define MOVIE_PROBE(N)                                                                                                \
    {kMovieProbes[N].name, kMovieProbes[N].hash, MovieProbeHook<N>, &g_movieProbes[N].original, false, false}
        MOVIE_PROBE(0), MOVIE_PROBE(1), MOVIE_PROBE(2), MOVIE_PROBE(3), MOVIE_PROBE(4), MOVIE_PROBE(5),
        MOVIE_PROBE(6), MOVIE_PROBE(7), MOVIE_PROBE(8), MOVIE_PROBE(9), MOVIE_PROBE(10), MOVIE_PROBE(11),
        MOVIE_PROBE(12),
#undef MOVIE_PROBE
    };
    static_assert(kMovieProbeCount == 13, "update the MOVIE_PROBE list");
    bool IsMovieProbeHook(const NativeHook& h);
    bool& g_nativeHookInstalled = g_nativeHooks[0].installed;

    bool IsMovieProbeHook(const NativeHook& h)
    {
        for (const MovieProbeState& p : g_movieProbes)
            if (h.original == &p.original)
                return true;
        return false;
    }

    void TryInstallNativeHooks()
    {
        for (NativeHook& h : g_nativeHooks)
        {
            if (h.installed || h.failed)
                continue;
            if (h.hash == kNativeCutsceneEnableSkip && !G.lockedCutscenes)
                continue;
            if (h.hash == kNativeSetTransitionMovieSkippable && !G.movies)
                continue;
            if (h.original >= &g_camMarkerOriginal[0] && h.original < &g_camMarkerOriginal[kCamMarkerCount] &&
                !(g_config.skipKillCams && g_config.protectScriptedKillCams))
                continue;
            if (IsMovieProbeHook(h) && !(G.movies && g_config.logMovieNatives))
                continue;

            NativeHandler* handler = FindNative(h.hash);
            if (!handler)
            {
                if (g_frame == 3000)
                    logx::Write("WARNING: %s is still not registered after %u frames", h.name, g_frame);
                continue;
            }
            if (handler->fn == h.hook)
            {
                h.installed = true;
                continue;
            }

            *h.original = handler->fn;
            if (!WritePointer(reinterpret_cast<void**>(&handler->fn), reinterpret_cast<void*>(h.hook)))
            {
                logx::Write("ERROR: can't write the %s handler at 0x%08X", h.name,
                            static_cast<unsigned>(reinterpret_cast<uintptr_t>(handler)));
                h.failed = true;
                if (h.hash == kNativeCutsceneEnableSkip)
                    G.lockedCutscenes = false;
                continue;
            }
            h.installed = true;
            const void* original = reinterpret_cast<void*>(*h.original);
            logx::Write("native hook: %s handler 0x%08X, original fn 0x%08X (%s, RVA 0x%X), frame %u", h.name,
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(handler)),
                        static_cast<unsigned>(reinterpret_cast<uintptr_t>(original)),
                        G.text.Contains(original) ? "game code" : "NOT in the game's code - another mod?",
                        static_cast<unsigned>(G.image.Contains(original) ? Rva(original) : 0), g_frame);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Extra skip key
    // ---------------------------------------------------------------------------------------------------------------

    bool (*g_skipKeyProbe)() = nullptr; // tests only

    bool SkipKeyDownNow()
    {
        if (!g_config.skipKey)
            return false;
        if (g_skipKeyProbe)
            return g_skipKeyProbe();
        if (!(GetAsyncKeyState(g_config.skipKey) & 0x8000))
            return false;
        // Only while the game window has the focus
        HWND foreground = GetForegroundWindow();
        if (!foreground)
            return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        return pid == GetCurrentProcessId();
    }

    void UpdateSkipKey()
    {
        const bool down = SkipKeyDownNow();
        g_keyPulse = down && !g_keyDown;
        g_keyDown = down;
    }

    // True on the frame the extra skip key went down
    bool SkipKeyJustPressed()
    {
        // Without the per-frame hook the state is sampled right here (once per call of a skip check)
        if (!G.frameHook)
            UpdateSkipKey();
        return g_keyPulse;
    }

    void RequestCutsceneSkip(const char* source)
    {
        if (g_csIntent)
            return;
        g_csIntent = true;
        g_csIntentTick = GetTickCount();
        logx::Write("cutscene: skip requested (%s), scripts %s it", source,
                    g_scriptAllowsSkip ? "allow" : "lock");
    }

    // Replaces the game's "is skip just pressed" call inside the cutscene skip check
    bool __fastcall CutsceneSkipPressedHook(void* controls, void*)
    {
        if (G.isSkipPressed(controls, nullptr))
        {
            RequestCutsceneSkip("game's skip button");
            return true;
        }
        if (!g_config.enabled)
            return false;
        if (SkipKeyJustPressed())
        {
            RequestCutsceneSkip("extra key");
            return true;
        }
        if (g_csFire)
        {
            g_csFire = false;
            g_csFired = true;
            g_csFireTick = GetTickCount();
            logx::Write("cutscene: ending it with the game's own skip, %u ms after the request",
                        g_csFireTick - g_csIntentTick);
            return true;
        }
        return false;
    }

    // Replaces the same call in the game's other skip checks; the extra key only counts during a transition movie
    bool __fastcall OtherSkipPressedHook(void* controls, void*)
    {
#if defined(_MSC_VER) && !defined(__clang__)
        const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
        if (g_moviePlaying)
        {
            for (int i = 0; i < kMaxOtherSites; ++i)
            {
                if (g_otherCallers[i] == caller || g_otherCallers[i] == 0)
                {
                    g_otherCallers[i] = caller;
                    ++g_otherCalls[i];
                    break;
                }
            }
        }

        if (G.isSkipPressed(controls, nullptr))
            return true;
        return g_config.enabled && g_moviePlaying && (g_movieFire || SkipKeyJustPressed());
    }

    // Replaces the "current frame" call of the movie update's skip condition. The game gets here only when skip was
    // just pressed and the movie is skippable; the result is compared with 240.
    int __fastcall MovieFrameHook(uint8_t* slot, void*)
    {
        const int frame = g_movieFrameOriginal(slot, nullptr);
        if (frame > kMovieGameMinFrame || !g_config.enabled || !g_config.movieIgnoreMinFrames ||
            slot != G.moviePlayer || !g_moviePlaying || !g_movieLoaded)
            return frame;
        if (InterlockedCompareExchange(&g_movieGateFrame, frame, -1) == -1)
            logx::Write("movie: the game's skip accepted at frame %d (its own minimum is %d), %u ms after the start",
                        frame, kMovieGameMinFrame, GetTickCount() - g_movieStartTick);
        return kMovieGameMinFrame + 1;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Cutscenes
    // ---------------------------------------------------------------------------------------------------------------

    // Moves a script-locked cutscene to a moment shortly before its end
    void SeekToTail(uint8_t* manager)
    {
        const int start = At<int32_t>(manager, kCutsceneStartFrame);
        const int end = At<int32_t>(manager, kCutsceneEndFrame);
        const float time = At<float>(manager, kCutsceneTime);
        if (start < 0 || end <= start || end > 30 * 60 * 60 * kCutsceneFps || !(time >= 0.0f) || time > 36000.0f)
        {
            logx::Write("locked cutscene: implausible timeline (start %d, end %d, time %d ms) - not touching it",
                        start, end, static_cast<int>(time * 1000.0f));
            g_csPhase = Phase::Done;
            return;
        }

        const int current = start + static_cast<int>(time * kCutsceneFps);
        const int target = end - g_config.lockedTailFrames;
        if (target <= current + kCutsceneFps / 2)
        {
            logx::Write("locked cutscene: already at its end (frame %d of %d..%d) - letting it finish", current,
                        start, end);
            g_csPhase = Phase::Done;
            return;
        }

        At<int32_t>(manager, kCutsceneSeekFrame) = target;
        At<int32_t>(manager, kCutsceneSeekPending) = 0;

        void* audioOwner = G.getCutsceneAudio();
        void* audio = audioOwner ? At<void*>(static_cast<uint8_t*>(audioOwner), kCutsceneAudioEntity) : nullptr;
        if (audio)
        {
            const float seconds = static_cast<float>(target) / kCutsceneFps;
            G.audioSkipSpeech(audio, nullptr, seconds);
            G.audioSkipStreams(audio, nullptr, seconds);
        }

        g_lockedSkipFrame = target;
        g_lockedSkipWait = 0;
        g_lockedSkipTick = GetTickCount();
        g_csPhase = Phase::Seeking;
        logx::Write("locked cutscene: frame %d -> %d (range %d..%d, tail %d frames, audio %s)", current, target,
                    start, end, g_config.lockedTailFrames, audio ? "synced" : "none");
    }

    // CutSceneManager's skip check, called by the game every frame of a playing cutscene after its first second
    void __fastcall SkipCheckHook(uint8_t* manager, void*)
    {
        if (!g_config.enabled || !G.lockedCutscenes || !g_nativeHookInstalled || !manager)
        {
            G.skipCheckOriginal(manager, nullptr);
            return;
        }
        g_csCheckLive = true;

        if (g_scriptAllowsSkip || g_csPhase == Phase::ForceNative)
        {
            // The game's own skip. Requested earlier (a chain, or the scripts have just unlocked it): press it now.
            if (g_csFired && GetTickCount() - g_csFireTick > 3000)
                g_csFired = false; // still here 3 s later: try again
            if (g_csIntent && !g_csFired && manager[kCutsceneActive])
                g_csFire = true;
            G.skipCheckOriginal(manager, nullptr);
            g_csFire = false;
            return;
        }

        if (!manager[kCutsceneActive] || !manager[kCutsceneSkippable])
            return;

        if (!g_csIntent)
        {
            CutsceneSkipPressedHook(G.getControls(1), nullptr);
            if ((*G.multiplayerFlagA || *G.multiplayerFlagB) && *G.frontendActive)
                g_csIntent = false;
            return; // the work starts on the next frame
        }
        if (g_csPhase == Phase::NeedSeek)
            SeekToTail(manager);
    }

    void UpdateSeeking(uint8_t* manager)
    {
        const int start = At<int32_t>(manager, kCutsceneStartFrame);
        const int end = At<int32_t>(manager, kCutsceneEndFrame);
        const int frame = start + static_cast<int>(At<float>(manager, kCutsceneTime) * kCutsceneFps);

        // The seek is done once the cutscene plays again at the new time; the scripts need a couple of frames to see
        // it and make their requests. What's still streaming in when the cutscene is about to end is loaded then.
        if (At<int32_t>(manager, kCutsceneState) == kStatePlaying && frame >= g_lockedSkipFrame - 1)
        {
            ++g_lockedSkipWait;
            const bool last = frame >= end - kCutsceneFps / 2;
            const int requests = last ? StreamingRequests() : 0;
            if (g_lockedSkipWait == 3 || (last && requests > 0))
            {
                const DWORD took = LoadAllObjectsNow();
                logx::Write("locked cutscene: LOAD_ALL_OBJECTS_NOW at frame %d (%s, %d requests), took %u ms", frame,
                            g_lockedSkipWait == 3 ? "after the seek" : "before the end", requests, took);
            }
            if (last && g_lockedSkipWait > 3)
            {
                logx::Write("locked cutscene: reached its last frames %u ms after the seek",
                            GetTickCount() - g_lockedSkipTick);
                g_csPhase = Phase::Done;
            }
        }
        else if (GetTickCount() - g_lockedSkipTick > 60000)
        {
            logx::Write("WARNING: locked cutscene: the seek to frame %d didn't complete in 60 s (state %d, frame %d)",
                        g_lockedSkipFrame, At<int32_t>(manager, kCutsceneState), frame);
            g_csPhase = Phase::Done;
        }
    }

    // Start / end of a cutscene. Runs every frame, also while paused.
    void TrackCutscene()
    {
        uint8_t* manager = Manager();
        const bool active = manager && manager[kCutsceneActive];
        if (active == g_csActive)
            return;
        g_csActive = active;

        if (active)
        {
            g_csIntent = false;
            g_csCheckLive = false;
            g_csFire = false;
            g_csFired = false;
            g_csPhase = Phase::Idle;
            g_csLoads = 0;
            g_lockedSkipFrame = -1;
            const bool chained = ChainActive();
            logx::Write("cutscene: started (frames %d..%d)%s", At<int32_t>(manager, kCutsceneStartFrame),
                        At<int32_t>(manager, kCutsceneEndFrame), chained ? " - right after a skip, skipping it too" : "");
            if (chained)
                RequestCutsceneSkip("chain");
        }
        else
        {
            if (g_csIntent)
            {
                logx::Write("cutscene: ended %u ms after the skip request", GetTickCount() - g_csIntentTick);
                MarkChain();
            }
            else
            {
                logx::Write("cutscene: ended");
            }
            g_csIntent = false;
            g_csPhase = Phase::Idle;
            g_lockedSkipFrame = -1;
        }
    }

    // Drives a requested skip of a script-locked cutscene
    void UpdateCutscene()
    {
        if (!G.lockedCutscenes || !g_nativeHookInstalled)
            return;
        if (!g_csActive || !g_csIntent || !g_csCheckLive)
            return;
        uint8_t* manager = Manager();
        if (!manager)
            return;

        if (g_csPhase == Phase::Seeking)
        {
            UpdateSeeking(manager);
            return;
        }
        if (g_scriptAllowsSkip)
            return; // the skip check ends it with the game's own skip

        switch (g_csPhase)
        {
        case Phase::Idle:
        {
            // Whatever the scripts are waiting for, get it loaded now
            const int requests = StreamingRequests();
            const DWORD took = LoadAllObjectsNow();
            g_csLoads = 1;
            g_csWait = 0;
            g_csPhase = Phase::WaitAllow;
            logx::Write("locked cutscene: LOAD_ALL_OBJECTS_NOW (%d requests), took %u ms", requests, took);
            break;
        }
        case Phase::WaitAllow:
        {
            const int requests = StreamingRequests();
            if (requests > 0 && g_csLoads < 4)
            {
                const DWORD took = LoadAllObjectsNow();
                ++g_csLoads;
                g_csWait = 0;
                logx::Write("locked cutscene: %d new requests, LOAD_ALL_OBJECTS_NOW again, took %u ms", requests,
                            took);
                break;
            }
            if (++g_csWait < g_config.allowWaitFrames)
                break;
            if (g_config.lockedMode == 2)
            {
                g_csPhase = Phase::ForceNative;
                logx::Write("locked cutscene: still locked %d frames after loading - LockedMode=2, "
                            "ending it with the game's own skip anyway",
                            g_csWait);
            }
            else
            {
                g_csPhase = Phase::NeedSeek;
                logx::Write("locked cutscene: still locked %d frames after loading - moving it to its end",
                            g_csWait);
            }
            break;
        }
        default:
            break;
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Transition movies
    // ---------------------------------------------------------------------------------------------------------------

    void RequestMovieSkip(const char* source)
    {
        if (g_movieIntent)
            return;
        g_movieIntent = true;
        logx::Write("movie: skip requested (%s) %u ms after its start", source, GetTickCount() - g_movieStartTick);
    }

    void TrackMovie()
    {
        if (!G.movies)
            return;
        uint32_t value = 0;
        const bool playing = CallNative(g_nativeMoviePlaying, &value) && value != 0;
        if (playing == g_moviePlaying)
            return;

        g_moviePlaying = playing;
        if (playing)
        {
            g_movieStartTick = GetTickCount();
            g_movieZeroFrames = 0;
            g_movieLoads = 0;
            g_movieFireCooldown = 0;
            g_movieUnlockLogged = false;
            g_movieIntent = false;
            g_movieForce = false;
            g_movieScriptCalls = 0;
            g_movieScriptLast = -1;
            g_movieLoaded = false;
            g_movieGateFrame = -1;
            memset(g_otherCallers, 0, sizeof(g_otherCallers));
            memset(g_otherCalls, 0, sizeof(g_otherCalls));
            const bool chained = ChainActive();
            logx::Write("movie: transition movie started (skippable %d, no-skip flag %d, streaming requests %d)%s",
                        G.moviePlayer[kMovieSkippable], G.moviePlayer[kMovieNoSkip], StreamingRequests(),
                        chained ? " - right after a skip, skipping it too" : "");
            for (int i = 0; i < kMovieProbeCount; ++i)
            {
                MovieProbeState& p = g_movieProbes[i];
                if (p.tick && g_movieStartTick - p.tick <= 5000)
                    logx::Write("movie:   %u ms before: %s(%u args: 0x%X, 0x%X) -> %d", g_movieStartTick - p.tick,
                                kMovieProbes[i].name, p.argCount, p.args[0], p.args[1], static_cast<int>(p.result));
                p.calls = 0;
                p.logged = false;
            }
            g_movieSampleTick = 0;
            if (chained)
                RequestMovieSkip("chain");
        }
        else
        {
            const LONG gateFrame = g_movieGateFrame;
            logx::Write("movie: transition movie ended after %u ms (skip %s, %s, scripts called "
                        "SET_TRANSITION_MOVIE_SKIPPABLE %d times)",
                        GetTickCount() - g_movieStartTick, g_movieIntent ? "requested" : "not requested",
                        gateFrame >= 0                      ? "ended by the game's skip with the frame limit lifted"
                        : G.moviePlayer[kMovieSkipped] != 0 ? "ended by the game's skip"
                                                            : "ran to its end or was stopped by the scripts",
                        g_movieScriptCalls);
            g_movieLoaded = false;
            g_movieEndTick = GetTickCount();
            for (int i = 0; i < kMovieProbeCount; ++i)
                if (g_movieProbes[i].calls)
                    logx::Write("movie:   scripts called %s %u times, last (0x%X, 0x%X) -> %d", kMovieProbes[i].name,
                                g_movieProbes[i].calls, g_movieProbes[i].args[0], g_movieProbes[i].args[1],
                                static_cast<int>(g_movieProbes[i].result));
            for (int i = 0; i < kMaxOtherSites && g_otherCallers[i]; ++i)
                logx::Write("movie:   skip check returning to RVA 0x%X ran %u times",
                            static_cast<unsigned>(Rva(reinterpret_cast<void*>(g_otherCallers[i]))), g_otherCalls[i]);
            if (g_movieIntent)
                MarkChain();
            g_movieIntent = false;
            g_movieForce = false;
            g_movieFire = false;
        }
    }

    // Diagnostics: the movie's state twice a second
    void SampleMovie()
    {
        if (!G.movies || !g_config.logMovieNatives || !g_moviePlaying)
            return;
        const DWORD now = GetTickCount();
        if (g_movieSampleTick && now - g_movieSampleTick < 500)
            return;
        g_movieSampleTick = now;
        uint32_t remaining = 0;
        g_movieProbeSilent = true;
        const bool ok = CallNative(g_nativeMovieFramesRemaining, &remaining);
        g_movieProbeSilent = false;
        const uint8_t* slot = G.moviePlayer;
        const uint8_t* manager = Manager();
        logx::Write("movie:   at %u ms: frames remaining %d%s, slot flags 61=%u 62=%u 63=%u 64=%u 65=%u 66=%u 6C=%u, "
                    "streaming requests %d, cutscene %s",
                    now - g_movieStartTick, static_cast<int>(remaining), ok ? "" : " (native not found)", slot[0x61],
                    slot[0x62], slot[0x63], slot[0x64], slot[0x65], slot[0x66], slot[0x6C], StreamingRequests(),
                    manager && manager[kCutsceneActive] ? "active" : "not active");
    }

    void UpdateMovie()
    {
        g_movieFire = false;
        SampleMovie();
        if (!G.movies || !g_config.unlockMovies || !g_moviePlaying)
            return;

        if (g_keyPulse)
            RequestMovieSkip("extra key");

        const int requests = StreamingRequests();
        if (requests != 0)
        {
            g_movieZeroFrames = 0;
            if (g_movieIntent && requests > 0 && g_movieLoads < 4)
            {
                const DWORD took = LoadAllObjectsNow();
                ++g_movieLoads;
                logx::Write("movie: LOAD_ALL_OBJECTS_NOW (%d requests), took %u ms", requests, took);
            }
            return;
        }
        if (++g_movieZeroFrames < g_config.movieStableFrames)
            return;

        // Nothing left to load: the movie may go
        g_movieLoaded = true;
        if (!G.moviePlayer[kMovieSkippable])
            G.moviePlayer[kMovieSkippable] = 1;
        if (!g_movieUnlockLogged)
        {
            g_movieUnlockLogged = true;
            logx::Write("movie: skippable %u ms after its start (no streaming requests for %d frames)",
                        GetTickCount() - g_movieStartTick, g_movieZeroFrames);
        }
        if (g_movieIntent)
        {
            g_movieForce = true;
            if (g_movieFireCooldown-- <= 0)
            {
                g_movieFire = true; // one frame of "skip pressed", repeated twice a second until the movie is gone
                g_movieFireCooldown = 15;
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Kill cameras
    // ---------------------------------------------------------------------------------------------------------------

    void UpdateKillCam()
    {
        if (!g_config.skipKillCams || !G.natives)
            return;
        uint32_t value = 0;
        const bool active = CallNative(g_nativeBulletCamActive, &value) && value != 0;
        if (active != g_camActive)
        {
            g_camActive = active;
            g_camRefusedLogged = false;
            g_camStartTick = GetTickCount();
            if (active)
            {
                logx::Write("kill cam: started (%s)", g_camScripted ? "staged by the scripts" : "regular");
                const DWORD now = GetTickCount();
                for (int i = 0; i < kCamMarkerCount; ++i)
                {
                    if (g_camMarkerCalls[i] && now - g_camMarkerTick[i] < 15000)
                        logx::Write("kill cam:   %u ms before: %s(0x%X, 0x%X), %u calls so far",
                                    now - g_camMarkerTick[i], kCamMarkers[i].name, g_camMarkerArgs[i][0],
                                    g_camMarkerArgs[i][1], g_camMarkerCalls[i]);
                }
            }
            else
                g_camScripted = false; // the staged shot is over
        }
        if (!active)
        {
            // a staging call that no kill camera followed
            if (g_camScripted && GetTickCount() - g_camScriptedTick > 30000)
                g_camScripted = false;
            return;
        }
        if (!g_keyPulse || g_csActive || g_moviePlaying)
            return;

        if (g_camScripted && g_config.protectScriptedKillCams)
        {
            const DWORD age = GetTickCount() - g_camStartTick;
            const DWORD delay = static_cast<DWORD>(g_config.scriptedKillCamDelayMs);
            if (delay == 0 || age < delay)
            {
                if (!g_camRefusedLogged)
                    logx::Write("kill cam: staged by the scripts - not ended %u ms after its start, "
                                "the shot has to land",
                                age);
                g_camRefusedLogged = true;
                return;
            }
            logx::Write("kill cam: staged by the scripts, but %u ms old - ending it", age);
        }
        const bool ok = CallNative(g_nativeBulletCamTerminate, nullptr);
        logx::Write("kill cam: %s", ok ? "ended (TERMINATE_GAME_BULLET_CAMERA)" : "can't end it, native not found");
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Per-frame hook
    // ---------------------------------------------------------------------------------------------------------------

    // Diagnostics (LogFps=1): frame rate, worst frame and window size every 5 seconds
    void LogFrameRate()
    {
        static DWORD windowStart = 0, last = 0, worst = 0;
        static unsigned frames = 0;
        const DWORD now = GetTickCount();
        if (!windowStart)
        {
            windowStart = last = now;
            return;
        }
        if (now - last > worst)
            worst = now - last;
        last = now;
        ++frames;
        if (now - windowStart < 5000)
            return;

        RECT rc = {};
        HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        if (foreground)
            GetWindowThreadProcessId(foreground, &pid);
        if (pid == GetCurrentProcessId())
            GetClientRect(foreground, &rc);
        logx::Write("fps: %u (worst frame %u ms) window %dx%d%s%s", frames * 1000 / (now - windowStart), worst,
                    static_cast<int>(rc.right), static_cast<int>(rc.bottom), g_csActive ? " [cutscene]" : "",
                    g_moviePlaying ? " [movie]" : "");
        windowStart = now;
        frames = 0;
        worst = 0;
    }

    void OnFrame()
    {
        ++g_frame;
        if (g_config.logFps)
            LogFrameRate();
        if (g_frame == 1)
            logx::Write("first game frame (thread %u)", GetCurrentThreadId());

        TryInstallNativeHooks();
        UpdateSkipKey();

        if (g_resetSeen)
        {
            g_resetSeen = 0;
            logx::Debug("cutscene: the game reset the skip flag");
        }

        if (!g_config.enabled)
            return;

        TrackCutscene();
        TrackMovie();

        uint32_t paused = 0;
        if (CallNative(g_nativePauseMenu, &paused) && paused)
        {
            g_movieFire = false;
            return;
        }

        UpdateCutscene();
        UpdateMovie();
        UpdateKillCam();
    }

    void __cdecl GameProcessHook()
    {
        OnFrame();
        G.processOriginal();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // Resolving
    // ---------------------------------------------------------------------------------------------------------------

    bool IsCallInto(const uint8_t* insn, const mem::Region& text)
    {
        return insn[0] == 0xE8 && text.Contains(mem::RelTarget(insn));
    }

    // Fills G from the game's code. verbose = write what was found to the log.
    void Resolve(Game& g, const mem::Region& image, const mem::Region& textRegion, bool verbose)
    {
        memset(&g, 0, sizeof(g));
        g.image = image;
        g.text = textRegion;
        const mem::Region& text = g.text;
        int count = 0;

#define REPORT(name, ptr)                                                                                             \
    if (verbose)                                                                                                      \
        logx::Write("  %-22s %s (matches: %d, RVA 0x%X)", name, (ptr) ? "ok" : "NOT FOUND", count,                    \
                    static_cast<unsigned>((ptr) ? static_cast<uintptr_t>((const uint8_t*)(ptr) - g.image.begin) : 0))

        // Per-frame call
        g.processCall = mem::FindUnique(text, kSigGameProcessCall, &count);
        if (g.processCall && !IsCallInto(g.processCall, text))
            g.processCall = nullptr;
        REPORT("game process call", g.processCall);

        // Native lookup
        uint8_t* getNative = mem::FindUnique(text, kSigGetNativeAddress, &count);
        REPORT("native lookup", getNative);
        uint8_t* tableRef = nullptr;
        count = mem::FindAll(text, kSigNativeTable, &tableRef, 1);
        if (count < 1 || !getNative || mem::RelTarget(tableRef + 5) != getNative)
            tableRef = nullptr;
        REPORT("native table", tableRef);
        if (getNative && tableRef)
        {
            void* table = *reinterpret_cast<void**>(tableRef + 1);
            if (g.image.Contains(table))
            {
                g.getNative = reinterpret_cast<GetNativeFn>(getNative);
                g.nativeTable = table;
            }
        }

        // CutSceneManager
        uint8_t* managerRef = mem::FindUnique(text, kSigCutsceneManager, &count);
        REPORT("cutscene manager", managerRef);
        if (managerRef)
        {
            auto pp = *reinterpret_cast<uint8_t***>(managerRef + 2);
            if (g.image.Contains(pp))
                g.ppManager = pp;
        }

        // Skip check
        uint8_t* check = mem::FindUnique(text, kSigSkipCheck, &count);
        REPORT("cutscene skip check", check);
        bool checkComplete = false;
        if (check)
        {
            g.skipCheck = check;
            g.getControls = reinterpret_cast<void*(__cdecl*)(int)>(mem::RelTarget(check + kCheckGetControlsCall));
            g.isSkipPressed = reinterpret_cast<IsSkipPressedFn>(mem::RelTarget(check + kCheckIsSkipPressedCall));
            g.multiplayerFlagA = *reinterpret_cast<uint8_t**>(check + kCheckMultiplayerFlagA);
            g.multiplayerFlagB = *reinterpret_cast<uint8_t**>(check + kCheckMultiplayerFlagB);
            g.frontendActive = *reinterpret_cast<uint8_t**>(check + kCheckFrontendActive);

            const bool callsOk = text.Contains(reinterpret_cast<void*>(g.getControls)) &&
                                 text.Contains(reinterpret_cast<void*>(g.isSkipPressed));
            const bool dataOk = g.image.Contains(g.multiplayerFlagA) && g.image.Contains(g.multiplayerFlagB) &&
                                g.image.Contains(g.frontendActive);
            const bool tailOk = check + kCheckAudioSkipStreamsCall + 5 <= text.end &&
                                IsCallInto(check + kCheckGetCutsceneAudioCall, text) &&
                                IsCallInto(check + kCheckAudioSkipSpeechCall, text) &&
                                IsCallInto(check + kCheckAudioSkipStreamsCall, text);
            if (!callsOk)
            {
                g.skipCheck = nullptr;
            }
            else if (dataOk && tailOk)
            {
                g.getCutsceneAudio =
                    reinterpret_cast<void*(__cdecl*)()>(mem::RelTarget(check + kCheckGetCutsceneAudioCall));
                g.audioSkipSpeech = reinterpret_cast<void(__fastcall*)(void*, void*, float)>(
                    mem::RelTarget(check + kCheckAudioSkipSpeechCall));
                g.audioSkipStreams = reinterpret_cast<void(__fastcall*)(void*, void*, float)>(
                    mem::RelTarget(check + kCheckAudioSkipStreamsCall));
                checkComplete = true;
            }
            if (verbose)
                logx::Write("  %-22s calls %s, data %s, audio calls %s", "  skip check layout",
                            callsOk ? "ok" : "BAD", dataOk ? "ok" : "BAD", tailOk ? "ok" : "BAD");
        }

        // The two places where the game resets the skip flag for a new cutscene
        uint8_t* resets[2] = {};
        count = mem::FindAll(text, kSigSkipFlagReset, resets, 2);
        const bool resetsOk = count == 2;
        if (verbose)
            logx::Write("  %-22s %s (matches: %d, RVA 0x%X, 0x%X)", "skip flag resets", resetsOk ? "ok" : "NOT FOUND",
                        count, static_cast<unsigned>(resets[0] ? resets[0] - g.image.begin : 0),
                        static_cast<unsigned>(resets[1] ? resets[1] - g.image.begin : 0));
        if (resetsOk)
        {
            g.resetSites[0] = resets[0];
            g.resetSites[1] = resets[1];
        }

        // Movie player
        uint8_t* movieRef = mem::FindUnique(text, kSigMoviePlayer, &count);
        REPORT("movie player", movieRef);
        if (movieRef)
        {
            auto player = *reinterpret_cast<uint8_t**>(movieRef + 8);
            if (g.image.Contains(player))
                g.moviePlayer = player;
        }

        // The minimum frame in the movie's own skip condition
        uint8_t* gate = mem::FindUnique(text, kSigMovieFrameGate, &count);
        REPORT("movie frame limit", gate);
        if (gate && IsCallInto(gate + kGateGetFrameCall, text) && IsCallInto(gate + kGateStopCall, text))
            g.movieGate = gate;
#undef REPORT

        g.frameHook = g.processCall != nullptr;
        g.natives = g.getNative != nullptr && g.nativeTable != nullptr;
        g.skipKeyInCutscenes = g.skipCheck != nullptr && g_config.skipKey != 0;
        g.lockedCutscenes = g_config.unlockCutscenes && g.frameHook && g.natives && g.skipCheck && checkComplete &&
                            g.ppManager && resetsOk;
        g.movies = g_config.unlockMovies && g.frameHook && g.natives && g.moviePlayer;
        g.movieFrameGate = g.movies && g.movieGate && g_config.movieIgnoreMinFrames;
    }

    void HexDump(const char* title, const uint8_t* from, const uint8_t* to)
    {
        if (from < G.text.begin)
            from = G.text.begin;
        if (to > G.text.end)
            to = G.text.end;
        logx::Write("dump %s: RVA 0x%X..0x%X", title, static_cast<unsigned>(Rva(from)),
                    static_cast<unsigned>(Rva(to)));
        static const char digits[] = "0123456789ABCDEF";
        for (const uint8_t* p = from; p < to; p += 32)
        {
            char line[32 * 3 + 1];
            int n = 0;
            for (const uint8_t* q = p; q < p + 32 && q < to; ++q)
            {
                line[n++] = digits[*q >> 4];
                line[n++] = digits[*q & 15];
                line[n++] = ' ';
            }
            line[n] = 0;
            logx::Write("  %06X: %s", static_cast<unsigned>(Rva(p)), line);
        }
    }

    // Diagnostics (DumpMovieCode=1): the movie player's methods and the game's other skip checks, as hex, so the
    // transition movie's own skip logic can be studied. Nothing is changed.
    void DumpMovieCode()
    {
        logx::Write("dump: movie player object at RVA 0x%X", static_cast<unsigned>(Rva(G.moviePlayer)));
        // mov ecx, offset moviePlayer / call method
        static uint8_t* targets[40];
        int count = 0;
        if (G.moviePlayer)
        {
            const uint32_t address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(G.moviePlayer));
            for (uint8_t* p = G.text.begin; p + 10 <= G.text.end; ++p)
            {
                if (p[0] != 0xB9 || memcmp(p + 1, &address, 4) != 0 || p[5] != 0xE8)
                    continue;
                uint8_t* target = mem::RelTarget(p + 5);
                if (!G.text.Contains(target))
                    continue;
                bool known = false;
                for (int i = 0; i < count; ++i)
                    known = known || targets[i] == target;
                logx::Write("dump: call site RVA 0x%X -> method RVA 0x%X", static_cast<unsigned>(Rva(p)),
                            static_cast<unsigned>(Rva(target)));
                if (!known && count < 40)
                    targets[count++] = target;
            }
        }
        for (int i = 0; i < count; ++i)
            HexDump("movie player method", targets[i], targets[i] + 0x180);

        static uint8_t* sites[16];
        const int found = mem::FindAll(G.text, kSigSkipPressedSite, sites, 16);
        for (int i = 0; i < found && i < 16; ++i)
            HexDump("skip check site", sites[i] - 0x200, sites[i] + 0x140);

        if (G.movieGate)
        {
            uint8_t* getFrame = g_movieFrameOriginal ? reinterpret_cast<uint8_t*>(g_movieFrameOriginal)
                                                     : mem::RelTarget(G.movieGate + kGateGetFrameCall);
            HexDump("movie get frame", getFrame - 0x20, getFrame + 0xA0);
            HexDump("movie stop", mem::RelTarget(G.movieGate + kGateStopCall) - 0x20,
                    mem::RelTarget(G.movieGate + kGateStopCall) + 0x180);
        }
    }

    constexpr int kMaxPatches = 24;
    mem::Patch g_patches[kMaxPatches];
    uint8_t* g_otherSites[64];

    const char* ResultName(mem::ApplyResult r)
    {
        switch (r)
        {
        case mem::ApplyResult::Ok:
            return "ok";
        case mem::ApplyResult::Mismatch:
            return "code changed while patching (another mod?)";
        case mem::ApplyResult::Busy:
            return "a thread kept running inside the code to patch";
        default:
            return "VirtualProtect failed";
        }
    }
}

namespace skip
{
    void SetSkipKeyProbeForTests(bool (*probe)())
    {
        g_skipKeyProbe = probe;
    }

    InstallResult Install(HMODULE game)
    {
        mem::Region image, text;
        if (!mem::GetImageRange(game, image) || !mem::GetSection(game, ".text", text))
        {
            logx::Write("ERROR: no .text section in the host executable");
            return InstallResult::Unsupported;
        }
        return InstallIn(image, text);
    }

    InstallResult InstallIn(const mem::Region& image, const mem::Region& text)
    {
        // Resolve twice with a pause in between: everything must be identical, otherwise the image is still being
        // unpacked/relocated by the game's startup stub.
        Game first;
        Resolve(first, image, text, false);
        if (!first.processCall && !first.skipCheck && !first.ppManager)
            return InstallResult::NotReady;
        // The native table gets its buckets when the game registers its script natives, i.e. when the game's own
        // initialisation is running - long after the startup stub has unpacked and relocated the code.
        if (first.natives && *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(first.nativeTable) + 4) == 0)
            return InstallResult::NotReady;
        Sleep(150);
        Resolve(G, image, text, false);
        if (memcmp(&first, &G, sizeof(Game)) != 0)
            return InstallResult::NotReady;

        logx::Write("image 0x%08X..0x%08X, .text 0x%08X..0x%08X",
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(G.image.begin)),
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(G.image.end)),
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(G.text.begin)),
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(G.text.end)));
        logx::Write("signatures:");
        Resolve(G, image, text, true);

        logx::Write("features: per-frame hook %s, natives %s, extra key in cutscenes %s, "
                    "script-locked cutscenes %s, transition movies %s, movie frame limit lifted %s",
                    G.frameHook ? "yes" : "NO", G.natives ? "yes" : "NO", G.skipKeyInCutscenes ? "yes" : "NO",
                    G.lockedCutscenes ? "yes" : "NO", G.movies ? "yes" : "NO", G.movieFrameGate ? "yes" : "NO");

        if (!G.skipKeyInCutscenes && !G.lockedCutscenes && !G.movies)
        {
            logx::Write("nothing to do for this executable - the game is left untouched");
            return InstallResult::Unsupported;
        }

        mem::Patch* patches = g_patches;
        int n = 0;
        bool ok = true;

        if (G.frameHook && (G.lockedCutscenes || G.movies || G.skipKeyInCutscenes))
        {
            ok = ok && hooks::MakeCallRetarget("game process call", G.processCall,
                                               reinterpret_cast<void*>(&GameProcessHook), patches[n++],
                                               reinterpret_cast<void**>(&G.processOriginal));
        }
        else
        {
            G.frameHook = false;
        }

        if (G.skipCheck && (G.skipKeyInCutscenes || G.lockedCutscenes))
        {
            void* previous = nullptr;
            ok = ok && hooks::MakeCallRetarget("skip pressed (cutscene)", G.skipCheck + kCheckIsSkipPressedCall,
                                               reinterpret_cast<void*>(&CutsceneSkipPressedHook), patches[n++],
                                               &previous);
        }

        if (G.lockedCutscenes)
        {
            ok = ok && hooks::MakeEntryDetour("cutscene skip check", G.skipCheck,
                                              reinterpret_cast<void*>(&SkipCheckHook), patches[n++],
                                              reinterpret_cast<void**>(&G.skipCheckOriginal));
            ok = ok && hooks::MakeFlagMidHook("skip flag reset #1", G.resetSites[0], 7, &g_scriptAllowsSkip,
                                              &g_resetSeen, patches[n++]);
            ok = ok && hooks::MakeFlagMidHook("skip flag reset #2", G.resetSites[1], 7, &g_scriptAllowsSkip,
                                              &g_resetSeen, patches[n++]);
        }

        // The game's other "skip just pressed" checks on the player's controls (the transition movie's among them)
        if (G.skipCheck && G.movies && g_config.skipKey && g_config.skipKeyInOtherChecks)
        {
            uint8_t** sites = g_otherSites;
            int found = mem::FindAll(G.text, kSigSkipPressedSite, sites, 64);
            int used = 0;
            for (int i = 0; i < found && i < 64 && ok; ++i)
            {
                uint8_t* site = sites[i];
                if (site + 2 == G.skipCheck + kCheckGetControlsCall)
                    continue; // the cutscene's own check, handled above
                if (mem::RelTarget(site + 2) != reinterpret_cast<uint8_t*>(G.getControls) ||
                    mem::RelTarget(site + 12) != reinterpret_cast<uint8_t*>(G.isSkipPressed))
                    continue;
                if (n >= kMaxPatches)
                {
                    logx::Write("  more skip checks than expected; the rest keep the game's key only");
                    break;
                }
                void* previous = nullptr;
                ok = ok && hooks::MakeCallRetarget("skip pressed (other)", site + 12,
                                                   reinterpret_cast<void*>(&OtherSkipPressedHook), patches[n++],
                                                   &previous);
                logx::Write("  other skip check at RVA 0x%X: extra key enabled while a transition movie plays",
                            static_cast<unsigned>(Rva(site)));
                ++used;
            }
            if (!used)
                logx::Write("  no other skip checks found: transition movies are skipped with the game's own "
                            "skip button only");
        }

        if (G.movieFrameGate)
        {
            ok = ok && hooks::MakeCallRetarget("movie current frame", G.movieGate + kGateGetFrameCall,
                                               reinterpret_cast<void*>(&MovieFrameHook), patches[n++],
                                               reinterpret_cast<void**>(&g_movieFrameOriginal));
        }

        if (!ok)
        {
            logx::Write("ERROR: failed to prepare the hooks - the game is left untouched");
            memset(&G, 0, sizeof(G));
            return InstallResult::Failed;
        }

        int attempts = 0;
        const mem::ApplyResult result = mem::ApplyPatches(patches, n, &attempts);
        if (result != mem::ApplyResult::Ok)
        {
            // Hooks may not run: make sure nothing of ours is considered active.
            G.frameHook = G.skipKeyInCutscenes = G.lockedCutscenes = G.movies = G.movieFrameGate = false;
            logx::Write("ERROR: patching failed: %s (attempt %d) - %s", ResultName(result), attempts,
                        result == mem::ApplyResult::Error ? "see the next run's log" : "the game is left untouched");
            return InstallResult::Failed;
        }

        for (int i = 0; i < n; ++i)
            logx::Write("  patched %-24s at RVA 0x%X (%d bytes)", patches[i].name,
                        static_cast<unsigned>(Rva(patches[i].addr)), patches[i].len);
        logx::Write("hooks installed (%d patches, attempt %d)", n, attempts);
        if (g_config.dumpMovieCode)
            DumpMovieCode();
        return InstallResult::Ok;
    }
}
