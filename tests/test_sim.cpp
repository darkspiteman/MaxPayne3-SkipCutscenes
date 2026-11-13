// "Simulated game": hand-assembled x86 code that contains the same byte signatures and call layout the mod looks for
// in MaxPayne3.exe, backed by C++ stand-ins for the game's functions. The production code (skip.cpp, hooks.cpp,
// mem.cpp) is installed into it unchanged, then cutscene / movie scenarios are played.
//
// This verifies the mod's own logic and hook mechanics. It can NOT verify that the real game matches the assumed
// layout - that is only known from a run in the real game (see docs/TESTING.md).
//
// Exit code 0 = all passed.

#include "config.h"
#include "log.h"
#include "mem.h"
#include "skip.h"

#include <stdio.h>
#include <string.h>

namespace
{
    int g_failed = 0;
    int g_passed = 0;

#define CHECK(cond)                                                                                                   \
    do                                                                                                                \
    {                                                                                                                 \
        if (cond)                                                                                                     \
            ++g_passed;                                                                                               \
        else                                                                                                          \
        {                                                                                                             \
            ++g_failed;                                                                                               \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                  \
        }                                                                                                             \
    } while (0)

    // ---- the game's data ------------------------------------------------------------------------------------------
    constexpr ptrdiff_t kState = 0x104, kActive = 0x130, kTime = 0x13C, kStart = 0x2D44, kEnd = 0x2D48,
                        kSeekPending = 0x3508, kSeekFrame = 0x3520, kSkippable = 0x39F8, kMovieSkippable = 0x62;

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

    struct Sim
    {
        uint8_t* image;
        uint8_t* data;
        uint8_t* manager;
        uint8_t* moviePlayer;
        uint8_t* mpFlagA;
        uint8_t* mpFlagB;
        uint8_t* frontend;
        uint8_t audioOwner[0x80];
        uint8_t audioEntity[16];
        uint8_t controls[16];

        int frames, otherCalls, nativeSkips, speechSkips, streamSkips, loadAllCalls, enableSkipCalls;
        float speechTime, streamTime;
        void* speechThis;
        bool gameSkipPressed;
        bool keyDown;
        uint32_t paused, requests, moviePlaying, bulletCam;
        int movieFrame, movieStops, framesRemainingCalls;
        int bulletTerminations;

        void(__cdecl* tick)();
        void(__fastcall* skipCheck)(uint8_t*, void*);
        void(__fastcall* resetA)(uint8_t*, void*);
        void(__fastcall* resetB)(uint8_t*, void*);
        bool(__cdecl* otherCheck)();
        bool(__cdecl* movieUpdate)(); // true = the movie was stopped by the game's own skip
    } S;

    template <typename T> T& At(ptrdiff_t offset)
    {
        return *reinterpret_cast<T*>(S.manager + offset);
    }

    // ---- the game's functions -------------------------------------------------------------------------------------
    void __cdecl SimGameProcess()
    {
        ++S.frames;
    }
    void __cdecl SimOther()
    {
        ++S.otherCalls;
    }
    bool __fastcall SimIsActive(uint8_t* manager, void*)
    {
        return manager[kActive] != 0;
    }
    int __fastcall SimMovieGetFrame(uint8_t* slot, void*)
    {
        if (slot != S.moviePlayer)
            printf("  unexpected movie slot pointer\n");
        return S.movieFrame;
    }
    void __fastcall SimMovieStop(uint8_t* slot, void*)
    {
        if (slot != S.moviePlayer)
            printf("  unexpected movie slot pointer\n");
        ++S.movieStops;
        S.moviePlaying = 0;
    }
    void* __cdecl SimGetControls(int)
    {
        return S.controls;
    }
    bool __fastcall SimIsSkipJustPressed(void* controls, void*)
    {
        if (controls != S.controls)
            printf("  unexpected controls pointer\n");
        const bool pressed = S.gameSkipPressed;
        S.gameSkipPressed = false;
        return pressed;
    }
    void* __cdecl SimGetCutsceneAudio()
    {
        return S.audioOwner;
    }
    void __fastcall SimAudioSkipSpeech(void* self, void*, float seconds)
    {
        ++S.speechSkips;
        S.speechTime = seconds;
        S.speechThis = self;
    }
    void __fastcall SimAudioSkipStreams(void*, void*, float seconds)
    {
        ++S.streamSkips;
        S.streamTime = seconds;
    }
    // What the game does when a skippable cutscene is skipped: jump to the end
    void __fastcall SimNativeSkip(uint8_t* manager, void*)
    {
        ++S.nativeSkips;
        manager[kActive] = 0;
    }

    void __cdecl NativeEnableSkip(NativeContext* ctx)
    {
        ++S.enableSkipCalls;
        S.manager[kSkippable] = static_cast<uint8_t>(static_cast<uint32_t*>(ctx->args)[0]);
    }
    void __cdecl NativePaused(NativeContext* ctx)
    {
        *static_cast<uint32_t*>(ctx->result) = S.paused;
    }
    void __cdecl NativeRequests(NativeContext* ctx)
    {
        *static_cast<uint32_t*>(ctx->result) = S.requests;
    }
    void __cdecl NativeLoadAll(NativeContext*)
    {
        ++S.loadAllCalls;
        S.requests = 0;
    }
    void __cdecl NativeMoviePlaying(NativeContext* ctx)
    {
        *static_cast<uint32_t*>(ctx->result) = S.moviePlaying;
    }

    void __cdecl NativeMovieSkippable(NativeContext* ctx)
    {
        S.moviePlayer[kMovieSkippable] = static_cast<uint8_t>(static_cast<uint32_t*>(ctx->args)[0]);
    }
    void __cdecl NativeBulletCamActive(NativeContext* ctx)
    {
        *static_cast<uint32_t*>(ctx->result) = S.bulletCam;
    }
    void __cdecl NativeBulletCamTerminate(NativeContext*)
    {
        ++S.bulletTerminations;
        S.bulletCam = 0;
    }

    void __cdecl NativeCamVictim(NativeContext*)
    {
    }
    constexpr uint32_t kHashCamVictim = 0xFB6C221F;
    constexpr uint32_t kHashEnableSkip = 0xE9BB90DF;
    constexpr uint32_t kHashMovieSkippable = 0x4278FA3D;
    void __cdecl NativeMovieFramesRemaining(NativeContext* ctx)
    {
        ++S.framesRemainingCalls;
        *static_cast<uint32_t*>(ctx->result) = 150 - S.movieFrame;
    }
    struct
    {
        uint32_t hash;
        NativeHandler handler;
    } g_natives[] = {
        {0xC483FB4A, {10, NativeMovieFramesRemaining}},
        {kHashEnableSkip, {1, NativeEnableSkip}}, {0xD3600591, {2, NativePaused}},
        {0xC2EE9A02, {3, NativeRequests}},        {0xC9DBDA90, {4, NativeLoadAll}},
        {0x66BD2CB0, {5, NativeMoviePlaying}},    {kHashMovieSkippable, {6, NativeMovieSkippable}},
        {0xB5B03846, {7, NativeBulletCamActive}}, {0xEE7F2F5D, {8, NativeBulletCamTerminate}},
        {kHashCamVictim, {9, NativeCamVictim}},
    };

    NativeHandler* __fastcall SimGetNative(void*, void*, uint32_t hash)
    {
        for (auto& n : g_natives)
        {
            if (n.hash == hash)
                return &n.handler;
        }
        return nullptr;
    }

    // A script calling a native: looked up through the table every time, like the script VM does
    void ScriptCall(uint32_t hash, uint32_t arg)
    {
        NativeHandler* handler = SimGetNative(nullptr, nullptr, hash);
        NativeContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.args = ctx.stack;
        ctx.result = ctx.stack;
        ctx.argCount = 1;
        ctx.stack[0] = arg;
        handler->fn(&ctx);
    }

    bool KeyProbe()
    {
        return S.keyDown;
    }

    // ---- assembling the fake code ---------------------------------------------------------------------------------
    uint8_t* g_cursor = nullptr;

    uint8_t* Emit(const char* hex)
    {
        mem::Pattern p;
        if (!mem::ParsePattern(hex, p))
        {
            printf("bad hex in test: %s\n", hex);
            ExitProcess(2);
        }
        uint8_t* at = g_cursor;
        memcpy(g_cursor, p.bytes, static_cast<size_t>(p.len));
        g_cursor += p.len;
        return at;
    }
    uint8_t* EmitRel(uint8_t opcode, const void* target)
    {
        uint8_t* at = g_cursor;
        at[0] = opcode;
        int32_t rel = mem::RelTo(at, target);
        memcpy(at + 1, &rel, 4);
        g_cursor += 5;
        return at;
    }
    void EmitAbs(const void* address)
    {
        uint32_t value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address));
        memcpy(g_cursor, &value, 4);
        g_cursor += 4;
    }
    void Align()
    {
        while ((g_cursor - S.image) & 15)
            *g_cursor++ = 0xCC;
    }
    void PadTo(uint8_t* fn, ptrdiff_t offset)
    {
        if (g_cursor > fn + offset)
        {
            printf("layout error: already past +0x%X\n", static_cast<unsigned>(offset));
            ExitProcess(2);
        }
        while (g_cursor < fn + offset)
            *g_cursor++ = 0x90;
    }
    template <typename F> uint8_t* Thunk(F function)
    {
        Align();
        return EmitRel(0xE9, reinterpret_cast<void*>(function));
    }

    void BuildGame()
    {
        constexpr size_t kImageSize = 0x10000, kTextSize = 0x4000;
        S.image = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kImageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        memset(S.image, 0xCC, kTextSize);
        S.data = S.image + kTextSize;
        memset(S.data, 0, kImageSize - kTextSize);
        S.manager = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x3A00, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        uint8_t** ppManager = reinterpret_cast<uint8_t**>(S.data + 0x00);
        *ppManager = S.manager;
        S.mpFlagA = S.data + 0x10;
        S.mpFlagB = S.data + 0x11;
        S.frontend = S.data + 0x12;
        uint8_t* scratch = S.data + 0x20;
        S.moviePlayer = S.data + 0x100;
        uint8_t* nativeTable = S.data + 0x200;
        void* audioEntity = S.audioEntity;
        memcpy(S.audioOwner + 0x58, &audioEntity, sizeof(audioEntity));

        g_cursor = S.image + 0x40;
        uint8_t* tGameProcess = Thunk(&SimGameProcess);
        uint8_t* tOther = Thunk(&SimOther);
        uint8_t* tIsActive = Thunk(&SimIsActive);
        uint8_t* tGetControls = Thunk(&SimGetControls);
        uint8_t* tIsSkipPressed = Thunk(&SimIsSkipJustPressed);
        uint8_t* tGetAudio = Thunk(&SimGetCutsceneAudio);
        uint8_t* tSkipSpeech = Thunk(&SimAudioSkipSpeech);
        uint8_t* tSkipStreams = Thunk(&SimAudioSkipStreams);
        uint8_t* tNativeSkip = Thunk(&SimNativeSkip);
        uint8_t* tMovieGetFrame = Thunk(&SimMovieGetFrame);
        uint8_t* tMovieStop = Thunk(&SimMovieStop);

        // Main loop tick: E8 x4 E8 x4 8B 0D x4 8B 15 x4 89 0D x4
        Align();
        S.tick = reinterpret_cast<void(__cdecl*)()>(g_cursor);
        EmitRel(0xE8, tGameProcess);
        EmitRel(0xE8, tOther);
        Emit("8B 0D");
        EmitAbs(scratch);
        Emit("8B 15");
        EmitAbs(scratch + 4);
        Emit("89 0D");
        EmitAbs(scratch + 8);
        Emit("C3");

        // Native lookup: 53 8B 59 04 85 DB ...
        Align();
        uint8_t* getNative = Emit("53 8B 59 04 85 DB 5B");
        EmitRel(0xE9, reinterpret_cast<void*>(&SimGetNative));
        *reinterpret_cast<uint32_t*>(nativeTable + 4) = 5;

        // Reference to the native table (never executed): B9 imm32 E8 rel32 85 C0 75 07
        Align();
        Emit("B9");
        EmitAbs(nativeTable);
        EmitRel(0xE8, getNative);
        Emit("85 C0 75 07 C3");

        // Reference to the cutscene manager pointer (never executed)
        Align();
        Emit("83 3D");
        EmitAbs(ppManager);
        Emit("00 75 28 68 00 3A 00 00 C3");

        // Reference to the movie player (never executed)
        Align();
        Emit("8B 44 24 04 50 6A 00 B9");
        EmitAbs(S.moviePlayer);
        EmitRel(0xE8, tOther);
        Emit("C3");

        // The two places that reset the skip flag
        Align();
        S.resetA = reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(Emit("56 8B F1 C6 86 F8 39 00 00 01 5E C3"));
        Align();
        S.resetB = reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(Emit("56 8B F1 C6 86 F8 39 00 00 01 5E C3"));

        // Another "skip just pressed" check on the player's controls: bool OtherCheck()
        Align();
        S.otherCheck = reinterpret_cast<bool(__cdecl*)()>(Emit("6A 01"));
        EmitRel(0xE8, tGetControls);
        Emit("83 C4 04 8B C8");
        EmitRel(0xE8, tIsSkipPressed);
        Emit("C3");

        // The skip part of the movie update, laid out like the game's (RVA 0xA9CA5..0xA9CDF): bool MovieUpdate()
        Align();
        S.movieUpdate = reinterpret_cast<bool(__cdecl*)()>(Emit("56 BE"));
        EmitAbs(S.moviePlayer);          // mov esi, slot 0
        Emit("6A 01");
        EmitRel(0xE8, tGetControls);
        Emit("83 C4 04 8B C8");
        EmitRel(0xE8, tIsSkipPressed);
        Emit("84 C0 74 29");             // test al,al / je no
        Emit("80 7E 62 00 74 23");       // skippable?
        Emit("80 7E 63 00 75 1D");       // no-skip flag?
        Emit("8B CE");
        EmitRel(0xE8, tMovieGetFrame);
        Emit("3D F0 00 00 00 7E 0F");    // frame > 240?
        Emit("8B CE C6 46 65 01");
        EmitRel(0xE8, tMovieStop);
        Emit("B0 01 5E C3");
        Emit("32 C0 5E C3");             // no:

        // The cutscene skip check, laid out like the game's
        Align();
        uint8_t* check = Emit("56 8B F1");
        S.skipCheck = reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(check);
        EmitRel(0xE8, tIsActive);        // +03
        Emit("84 C0 0F 84 9F 00 00 00"); // +08 test al,al / jz end (+AF)
        Emit("6A 01");                   // +10
        EmitRel(0xE8, tGetControls);     // +12
        Emit("83 C4 04 8B C8");          // +17
        EmitRel(0xE8, tIsSkipPressed);   // +1C
        Emit("8A 0D");                   // +21 mov cl,[mpA]
        EmitAbs(S.mpFlagA);
        Emit("0A 0D");                   // +27 or cl,[mpB]
        EmitAbs(S.mpFlagB);
        Emit("74 0B 80 3D");             // +2D jz +3A / +2F cmp byte [frontend],0
        EmitAbs(S.frontend);
        Emit("00 74 02 32 C0");          // +35 .. +38 xor al,al
        Emit("80 BE F8 39 00 00 00");    // +3A cmp byte [esi+39F8],0
        Emit("74 6C 84 C0 74 68");       // +41 jz end / test al,al / jz end
        PadTo(check, 0x6A);
        EmitRel(0xE8, tGetAudio);        // +6A
        PadTo(check, 0x85);
        Emit("68 00 00 00 00");          // push 0.0f
        EmitRel(0xE8, tSkipSpeech);      // +8A
        PadTo(check, 0x9E);
        Emit("68 00 00 00 00");
        EmitRel(0xE8, tSkipStreams);     // +A3
        Emit("8B CE");                   // +A8 mov ecx,esi
        EmitRel(0xE8, tNativeSkip);      // +AA
        if (g_cursor != check + 0xAF)
        {
            printf("layout error: end of the skip check is at +0x%X\n", static_cast<unsigned>(g_cursor - check));
            ExitProcess(2);
        }
        Emit("5E C3");                   // +AF
    }

    // ---- scenario helpers -----------------------------------------------------------------------------------------
    void StartCutscene(int start, int end)
    {
        S.resetA(S.manager, nullptr);
        At<uint8_t>(kActive) = 1;
        At<int32_t>(kState) = 8;
        At<int32_t>(kStart) = start;
        At<int32_t>(kEnd) = end;
        At<float>(kTime) = 5.0f;
        At<int32_t>(kSeekPending) = 1;
        At<int32_t>(kSeekFrame) = 0;
    }

    // One game frame: the main loop tick, then the cutscene manager's skip check while a cutscene plays
    void Frame()
    {
        S.tick();
        if (At<uint8_t>(kActive) && At<int32_t>(kState) == 8)
            S.skipCheck(S.manager, nullptr);
    }

    void PressKeyFor(int frames)
    {
        S.keyDown = true;
        for (int i = 0; i < frames; ++i)
            Frame();
        S.keyDown = false;
        Frame();
    }

    void ResetCounters()
    {
        S.nativeSkips = S.speechSkips = S.streamSkips = S.loadAllCalls = 0;
    }

    void EndCutscene()
    {
        At<uint8_t>(kActive) = 0;
        Frame();
    }
}

int main()
{
    logx::Open(GetModuleHandleA(nullptr));
    logx::SetLevel(2);
    BuildGame();
    skip::SetSkipKeyProbeForTests(KeyProbe);
    g_config.chainWindowMs = 0;
    g_config.movieIgnoreMinFrames = 1; // off by default, the scenario 9b covers it

    mem::Region image, text;
    image.begin = S.image;
    image.end = S.image + 0x10000;
    text.begin = S.image;
    text.end = S.image + 0x4000;

    // ---- before installation the fake game works on its own ----
    StartCutscene(0, 3000);
    S.gameSkipPressed = true;
    Frame();
    CHECK(S.frames == 1 && S.otherCalls == 1);
    CHECK(S.nativeSkips == 1 && S.speechSkips == 1 && S.streamSkips == 1);
    CHECK(At<uint8_t>(kActive) == 0);
    ResetCounters();

    // ---- installation ----
    CHECK(skip::InstallIn(image, text) == skip::InstallResult::Ok);
    NativeHandler* enableSkip = SimGetNative(nullptr, nullptr, kHashEnableSkip);
    NativeHandler* movieSkippable = SimGetNative(nullptr, nullptr, kHashMovieSkippable);
    CHECK(enableSkip->fn == NativeEnableSkip);
    Frame();
    CHECK(S.frames == 2 && S.otherCalls == 2);
    CHECK(enableSkip->fn != NativeEnableSkip && movieSkippable->fn != NativeMovieSkippable);

    // ---- 1. skippable cutscene, the game's own skip button ----
    StartCutscene(0, 3000);
    for (int i = 0; i < 5; ++i)
        Frame();
    CHECK(S.nativeSkips == 0 && At<uint8_t>(kActive) == 1);
    S.gameSkipPressed = true;
    Frame();
    CHECK(S.nativeSkips == 1 && At<uint8_t>(kActive) == 0);
    CHECK(At<int32_t>(kSeekFrame) == 0 && S.loadAllCalls == 0);
    Frame();
    ResetCounters();

    // ---- 2. skippable cutscene, the extra key ----
    StartCutscene(0, 3000);
    Frame();
    PressKeyFor(3);
    CHECK(S.nativeSkips == 1 && At<uint8_t>(kActive) == 0);
    ResetCounters();

    // ---- 3. holding the key doesn't skip the next cutscene (chain is off here) ----
    StartCutscene(0, 3000);
    S.keyDown = true;
    Frame();
    CHECK(S.nativeSkips == 1);
    Frame();
    StartCutscene(0, 3000);
    for (int i = 0; i < 10; ++i)
        Frame();
    CHECK(S.nativeSkips == 1 && At<uint8_t>(kActive) == 1);
    S.keyDown = false;
    EndCutscene();
    ResetCounters();

    // ---- 4. cutscene the scripts never unlock: load, wait, move to the tail ----
    StartCutscene(100, 3100);
    Frame();
    ScriptCall(kHashEnableSkip, 0);
    CHECK(S.enableSkipCalls == 1 && At<uint8_t>(kSkippable) == 1);
    for (int i = 0; i < 5; ++i)
        Frame();
    CHECK(S.nativeSkips == 0 && S.loadAllCalls == 0);

    // in multiplayer with the frontend up the press is ignored
    *S.mpFlagA = 1;
    *S.frontend = 1;
    S.gameSkipPressed = true;
    Frame();
    Frame();
    CHECK(S.loadAllCalls == 0);
    *S.mpFlagA = 0;
    *S.frontend = 0;

    S.requests = 7;
    S.keyDown = true;
    Frame(); // the press is taken
    S.keyDown = false;
    CHECK(S.loadAllCalls == 0);
    Frame(); // everything is loaded first
    CHECK(S.loadAllCalls == 1 && S.requests == 0 && S.speechSkips == 0);
    Frame();
    Frame();
    Frame();
    CHECK(S.speechSkips == 0 && At<int32_t>(kSeekPending) == 1); // still giving the scripts a chance
    Frame();
    const int target = 3100 - 30;
    CHECK(S.nativeSkips == 0);
    CHECK(At<int32_t>(kSeekFrame) == target && At<int32_t>(kSeekPending) == 0);
    CHECK(S.speechSkips == 1 && S.streamSkips == 1 && S.speechThis == S.audioEntity);
    CHECK(S.speechTime > target / 30.0f - 0.01f && S.speechTime < target / 30.0f + 0.01f);

    // the engine performs the seek
    At<int32_t>(kSeekPending) = 1;
    At<float>(kTime) = (target - 100) / 30.0f + 0.001f;
    Frame();
    Frame();
    CHECK(S.loadAllCalls == 1);
    Frame();
    CHECK(S.loadAllCalls == 2);
    S.paused = 1;
    At<float>(kTime) = (3100 - 100 - 10) / 30.0f;
    S.requests = 4;
    Frame();
    CHECK(S.loadAllCalls == 2);
    S.paused = 0;
    Frame();
    CHECK(S.loadAllCalls == 3 && S.requests == 0);
    Frame();
    Frame();
    CHECK(S.loadAllCalls == 3 && S.speechSkips == 1 && S.nativeSkips == 0);
    EndCutscene();
    ResetCounters();

    // ---- 5. "still loading" cutscene: the scripts unlock it once everything is loaded ----
    StartCutscene(0, 3000);
    Frame();
    ScriptCall(kHashEnableSkip, 0);
    Frame();
    S.requests = 12;
    S.keyDown = true;
    Frame();
    S.keyDown = false;
    Frame();
    CHECK(S.loadAllCalls == 1 && S.nativeSkips == 0);
    ScriptCall(kHashEnableSkip, 1); // the script sees its assets and allows the skip
    Frame();
    CHECK(S.nativeSkips == 1 && At<uint8_t>(kActive) == 0); // ended by the game's own skip, no second key press
    CHECK(At<int32_t>(kSeekPending) == 1);
    Frame();
    ResetCounters();

    // ---- 6. LockedMode=2: the game's own skip even though the scripts keep it locked ----
    g_config.lockedMode = 2;
    StartCutscene(0, 3000);
    Frame();
    ScriptCall(kHashEnableSkip, 0);
    S.keyDown = true;
    Frame();
    S.keyDown = false;
    Frame();
    CHECK(S.loadAllCalls == 1);
    Frame();
    Frame();
    Frame();
    CHECK(S.nativeSkips == 0);
    Frame();
    CHECK(S.nativeSkips == 1 && At<int32_t>(kSeekPending) == 1 && At<uint8_t>(kActive) == 0);
    Frame();
    g_config.lockedMode = 1;
    ResetCounters();

    // ---- 7. locked cutscene that is already at its end ----
    StartCutscene(0, 3000);
    Frame();
    ScriptCall(kHashEnableSkip, 0);
    At<float>(kTime) = (3000 - 20) / 30.0f;
    S.gameSkipPressed = true;
    for (int i = 0; i < 12; ++i)
        Frame();
    CHECK(S.speechSkips == 0 && At<int32_t>(kSeekPending) == 1 && S.nativeSkips == 0 && S.loadAllCalls == 1);
    EndCutscene();
    ResetCounters();

    // ---- 8. chain: what starts right after a skipped cutscene is skipped without a key press ----
    g_config.chainWindowMs = 2000;
    StartCutscene(0, 3000);
    Frame();
    PressKeyFor(1);
    CHECK(S.nativeSkips == 1);
    StartCutscene(0, 3000); // the next one, skippable
    Frame();
    CHECK(S.nativeSkips == 2 && At<uint8_t>(kActive) == 0);
    Frame();
    StartCutscene(0, 3000); // and a locked one
    ScriptCall(kHashEnableSkip, 0);
    for (int i = 0; i < 6; ++i)
        Frame();
    CHECK(S.loadAllCalls == 1 && At<int32_t>(kSeekFrame) == 3000 - 30 && S.nativeSkips == 2);
    EndCutscene();
    ResetCounters();

    // ... and a transition movie after it
    S.moviePlaying = 1;
    S.requests = 3;
    Frame();
    CHECK(S.loadAllCalls == 1 && S.requests == 0); // loaded at once
    bool fired = false;
    for (int i = 0; i < 8; ++i)
    {
        ScriptCall(kHashMovieSkippable, 0); // the script keeps saying "not yet"
        S.tick();
        fired = fired || S.otherCheck();
    }
    CHECK(fired);
    CHECK(S.moviePlayer[kMovieSkippable] == 1);
    ScriptCall(kHashMovieSkippable, 0);
    CHECK(S.moviePlayer[kMovieSkippable] == 1); // kept skippable while a skip is requested
    S.moviePlaying = 0;
    Frame();
    ScriptCall(kHashMovieSkippable, 0);
    CHECK(S.moviePlayer[kMovieSkippable] == 0); // the scripts are in charge again
    g_config.chainWindowMs = 0;
    ResetCounters();

    // ---- 9. transition movie nobody asked to skip ----
    S.moviePlaying = 1;
    S.requests = 3;
    for (int i = 0; i < 30; ++i)
        Frame();
    CHECK(S.moviePlayer[kMovieSkippable] == 0 && S.loadAllCalls == 0);
    S.requests = 0;
    for (int i = 0; i < 4; ++i)
        Frame();
    CHECK(S.moviePlayer[kMovieSkippable] == 0);
    Frame();
    CHECK(S.moviePlayer[kMovieSkippable] == 1);
    S.tick();
    CHECK(S.otherCheck() == false);
    S.gameSkipPressed = true;
    CHECK(S.otherCheck() == true);
    S.keyDown = true;
    S.tick();
    CHECK(S.otherCheck() == true); // the extra key
    S.keyDown = false;
    S.moviePlaying = 0;
    S.moviePlayer[kMovieSkippable] = 0;
    Frame();
    S.keyDown = true;
    S.tick();
    CHECK(S.otherCheck() == false); // no movie: the key means nothing to the other checks
    S.keyDown = false;
    S.tick();
    ResetCounters();

    // ---- 9b. the game's own movie skip and its 240-frame minimum ----
    // (a) still loading: the game's limit stays, even with the script's permission and the game's own button
    S.moviePlaying = 1;
    S.movieFrame = 50;
    S.movieStops = 0;
    S.requests = 3;
    S.moviePlayer[0x65] = 0;
    Frame();
    ScriptCall(kHashMovieSkippable, 1);
    S.gameSkipPressed = true;
    CHECK(S.movieUpdate() == false && S.movieStops == 0);
    // ... and works as in the unmodified game after frame 240
    S.movieFrame = 241;
    S.gameSkipPressed = true;
    CHECK(S.movieUpdate() == true && S.movieStops == 1 && S.moviePlayer[0x65] == 1);
    Frame();
    S.moviePlayer[kMovieSkippable] = 0;
    S.moviePlayer[0x65] = 0;
    S.requests = 0;
    ResetCounters();

    // (b) loaded, the extra key: the movie goes at an early frame
    S.moviePlaying = 1;
    S.movieFrame = 50;
    S.movieStops = 0;
    Frame();
    // the diagnostic hook on a movie native passes the call through (and the mod samples it itself)
    CHECK(SimGetNative(nullptr, nullptr, 0xC483FB4A)->fn != NativeMovieFramesRemaining);
    {
        const int before = S.framesRemainingCalls;
        CHECK(before >= 1);
        ScriptCall(0xC483FB4A, 0);
        CHECK(S.framesRemainingCalls == before + 1);
    }
    CHECK(S.movieUpdate() == false);
    S.keyDown = true;
    bool stopped = false;
    for (int i = 0; i < 12 && !stopped; ++i)
    {
        S.tick();
        S.keyDown = false;
        stopped = S.movieUpdate();
    }
    CHECK(stopped && S.movieStops == 1 && S.moviePlayer[0x65] == 1 && S.moviePlaying == 0);
    Frame();
    S.moviePlayer[kMovieSkippable] = 0;
    S.moviePlayer[0x65] = 0;

    // (c) loaded, but the game's no-skip flag is set: nothing happens
    S.moviePlaying = 1;
    S.movieFrame = 50;
    S.movieStops = 0;
    S.moviePlayer[0x63] = 1;
    Frame();
    S.keyDown = true;
    stopped = false;
    for (int i = 0; i < 12; ++i)
    {
        S.tick();
        S.keyDown = false;
        stopped = stopped || S.movieUpdate();
    }
    CHECK(!stopped && S.movieStops == 0);
    S.moviePlayer[0x63] = 0;

    // (d) MovieIgnoreMinFrames=0 at run time: the game's limit is back
    g_config.movieIgnoreMinFrames = 0;
    for (int i = 0; i < 20; ++i)
    {
        S.tick();
        stopped = stopped || S.movieUpdate();
    }
    CHECK(!stopped && S.movieStops == 0);
    g_config.movieIgnoreMinFrames = 1;
    S.moviePlaying = 0;
    S.moviePlayer[kMovieSkippable] = 0;
    Frame();
    g_config.chainWindowMs = 0;
    ResetCounters();

    // ---- 10. kill camera ----
    S.bulletCam = 1;
    for (int i = 0; i < 5; ++i)
        Frame();
    CHECK(S.bulletTerminations == 0);
    S.keyDown = true;
    Frame();
    S.keyDown = false;
    Frame();
    CHECK(S.bulletTerminations == 1 && S.bulletCam == 0);
    S.keyDown = true;
    Frame(); // no kill camera: nothing is called
    S.keyDown = false;
    Frame();
    CHECK(S.bulletTerminations == 1);

    // a kill camera staged by the scripts is left alone; the next regular one can be ended again
    g_config.scriptedKillCamDelayMs = 0;
    ScriptCall(kHashCamVictim, 5);
    S.bulletCam = 1;
    Frame();
    PressKeyFor(1);
    PressKeyFor(1);
    CHECK(S.bulletTerminations == 1 && S.bulletCam == 1);
    S.bulletCam = 0;
    Frame();
    S.bulletCam = 1;
    Frame();
    PressKeyFor(1);
    CHECK(S.bulletTerminations == 2 && S.bulletCam == 0);

    printf("test_sim: %d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
