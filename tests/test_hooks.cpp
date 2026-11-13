// Tests for the pattern scanner and the three hook primitives on hand-assembled x86 code.
// Exit code 0 = all passed.

#include "hooks.h"
#include "mem.h"

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

    uint8_t* g_code = nullptr;
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

    void Align()
    {
        while ((g_cursor - g_code) & 15)
            *g_cursor++ = 0xCC;
    }

    // ---- targets -------------------------------------------------------------------------------------------------
    volatile LONG g_hookHits = 0;
    volatile LONG g_calleeHits = 0;
    uint32_t(__fastcall* g_entryOriginal)(void*, void*) = nullptr;

    int __cdecl ReturnSeven()
    {
        return 7;
    }

    bool __cdecl Callee()
    {
        InterlockedIncrement(&g_calleeHits);
        return true;
    }

    uint32_t __fastcall EntryHook(void* self, void*)
    {
        InterlockedIncrement(&g_hookHits);
        return g_entryOriginal(self, nullptr) + 100;
    }

    volatile LONG g_stop = 0;
    volatile LONG g_workerErrors = 0;
    uint32_t(__fastcall* g_workerFn)(void*, void*) = nullptr;

    DWORD WINAPI Worker(LPVOID)
    {
        uint32_t marker = 0x1000;
        while (!g_stop)
        {
            uint32_t r = g_workerFn(reinterpret_cast<void*>(static_cast<uintptr_t>(marker)), nullptr);
            if (r != marker && r != marker + 100)
                InterlockedIncrement(&g_workerErrors);
        }
        return 0;
    }

    void TestPatterns()
    {
        static const uint8_t data[] = {0x00, 0xE8, 0x11, 0x22, 0x33, 0x44, 0x8B, 0x0D, 0xE8, 0x55,
                                       0x66, 0x77, 0x88, 0x8B, 0x0D, 0x90, 0xE8, 0x01, 0x02};
        mem::Region r;
        r.begin = const_cast<uint8_t*>(data);
        r.end = r.begin + sizeof(data);

        mem::Pattern p;
        CHECK(mem::ParsePattern("E8 ? ? ? ? 8B 0D", p) && p.len == 7);
        CHECK(!mem::ParsePattern("E8 ZZ", p));
        CHECK(!mem::ParsePattern("", p));
        CHECK(!mem::ParsePattern("E", p));

        uint8_t* hits[4] = {};
        CHECK(mem::FindAll(r, "E8 ? ? ? ? 8B 0D", hits, 4) == 2);
        CHECK(hits[0] == r.begin + 1 && hits[1] == r.begin + 8);
        int count = -1;
        CHECK(mem::FindUnique(r, "E8 ? ? ? ? 8B 0D", &count) == nullptr && count == 2);
        CHECK(mem::FindUnique(r, "8B 0D 90", &count) == r.begin + 13 && count == 1);
        CHECK(mem::FindUnique(r, "? ? 90 E8", &count) == r.begin + 13 && count == 1);
        // a match must not run past the end of the region
        CHECK(mem::FindAll(r, "E8 01 02 ?", hits, 4) == 0);
        CHECK(mem::FindAll(r, "E8 01 02", hits, 4) == 1 && hits[0] == r.begin + 16);
        CHECK(mem::FindAll(r, "AA BB", hits, 4) == 0);
    }

    void TestCallRetarget()
    {
        Align();
        uint8_t* one = Emit("B8 01 00 00 00 C3"); // mov eax,1 / ret
        Align();
        uint8_t* caller = g_cursor;
        uint8_t* call = EmitRel(0xE8, one);
        Emit("C3");

        auto fn = reinterpret_cast<int(__cdecl*)()>(caller);
        CHECK(fn() == 1);

        mem::Patch patch;
        void* original = nullptr;
        CHECK(hooks::MakeCallRetarget("call", call, reinterpret_cast<void*>(&ReturnSeven), patch, &original));
        CHECK(original == one);
        CHECK(fn() == 1); // nothing changed yet
        CHECK(mem::ApplyPatches(&patch, 1) == mem::ApplyResult::Ok);
        CHECK(fn() == 7);

        // Not a call instruction
        CHECK(!hooks::MakeCallRetarget("bad", one, reinterpret_cast<void*>(&ReturnSeven), patch, &original));

        // The code changed after the patch was prepared: nothing may be written
        mem::Patch stale;
        CHECK(hooks::MakeCallRetarget("stale", call, one, stale, &original));
        stale.expect[0] ^= 0xFF;
        CHECK(mem::ApplyPatches(&stale, 1) == mem::ApplyResult::Mismatch);
        CHECK(fn() == 7);
    }

    void TestEntryDetour()
    {
        Align();
        uint8_t* calleeThunk = EmitRel(0xE9, reinterpret_cast<void*>(&Callee));
        Align();
        // uint32_t __thiscall Fn(this) { Callee(); return (uint32_t)this; }
        uint8_t* fn = Emit("56 8B F1");
        EmitRel(0xE8, calleeThunk);
        Emit("8B C6 5E C3");

        auto call = reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(fn);
        void* self = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
        g_calleeHits = 0;
        CHECK(call(self, nullptr) == 0x12345678 && g_calleeHits == 1);

        // Hook it while another thread keeps calling it
        g_workerFn = call;
        HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        Sleep(50);

        mem::Patch patch;
        CHECK(hooks::MakeEntryDetour("entry", fn, reinterpret_cast<void*>(&EntryHook), patch,
                                     reinterpret_cast<void**>(&g_entryOriginal)));
        CHECK(g_entryOriginal != nullptr);
        int attempts = 0;
        CHECK(mem::ApplyPatches(&patch, 1, &attempts) == mem::ApplyResult::Ok);
        Sleep(100);
        g_stop = 1;
        WaitForSingleObject(worker, 5000);
        CloseHandle(worker);
        CHECK(g_workerErrors == 0);
        CHECK(g_hookHits > 0);
        printf("  entry detour applied on attempt %d, hook hits from the worker thread: %ld\n", attempts,
               static_cast<long>(g_hookHits));

        g_hookHits = 0;
        g_calleeHits = 0;
        CHECK(call(self, nullptr) == 0x12345678 + 100);
        CHECK(g_hookHits == 1 && g_calleeHits == 1);
        // The trampoline alone behaves like the original
        CHECK(g_entryOriginal(self, nullptr) == 0x12345678 && g_calleeHits == 2);

        // A different prologue is refused
        uint8_t* other = Emit("55 8B EC 5D C3");
        void* trampoline = nullptr;
        CHECK(!hooks::MakeEntryDetour("bad", other, reinterpret_cast<void*>(&EntryHook), patch, &trampoline));
    }

    void TestMidHook()
    {
        Align();
        // void __thiscall Reset(this) { this->skippable = 1; }
        uint8_t* fn = Emit("56 8B F1");
        uint8_t* site = Emit("C6 86 F8 39 00 00 01");
        Emit("8B C6 5E C3");

        static uint8_t object[0x3A00];
        static volatile uint8_t flagA = 0, flagB = 0;
        auto call = reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(fn);

        CHECK(call(object, nullptr) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object)));
        CHECK(object[0x39F8] == 1 && flagA == 0 && flagB == 0);
        object[0x39F8] = 0;

        mem::Patch patch;
        CHECK(hooks::MakeFlagMidHook("mid", site, 7, &flagA, &flagB, patch));
        CHECK(mem::ApplyPatches(&patch, 1) == mem::ApplyResult::Ok);
        CHECK(site[0] == 0xE9 && site[5] == 0x90 && site[6] == 0x90);

        CHECK(call(object, nullptr) == static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object)));
        CHECK(object[0x39F8] == 1 && flagA == 1 && flagB == 1);

        // Only one flag
        Align();
        uint8_t* fn2 = Emit("56 8B F1");
        uint8_t* site2 = Emit("C6 86 F8 39 00 00 01");
        Emit("8B C6 5E C3");
        static volatile uint8_t flagC = 0;
        object[0x39F8] = 0;
        CHECK(hooks::MakeFlagMidHook("mid2", site2, 7, &flagC, nullptr, patch));
        CHECK(mem::ApplyPatches(&patch, 1) == mem::ApplyResult::Ok);
        auto call2 = reinterpret_cast<uint32_t(__fastcall*)(void*, void*)>(fn2);
        call2(object, nullptr);
        CHECK(object[0x39F8] == 1 && flagC == 1);

        CHECK(!hooks::MakeFlagMidHook("short", site2, 4, &flagC, nullptr, patch));
    }
}

int main()
{
    g_code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_code)
        return 2;
    memset(g_code, 0xCC, 0x2000);
    g_cursor = g_code;

    TestPatterns();
    TestCallRetarget();
    TestEntryDetour();
    TestMidHook();

    printf("test_hooks: %d checks passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
