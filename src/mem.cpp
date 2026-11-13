#include "mem.h"

#include <string.h>
#include <tlhelp32.h>

namespace mem
{
    namespace
    {
        IMAGE_NT_HEADERS32* NtHeaders(HMODULE mod)
        {
            auto base = reinterpret_cast<uint8_t*>(mod);
            if (!base)
                return nullptr;
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return nullptr;
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
                return nullptr;
            return nt;
        }

        int HexValue(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }
    }

    bool GetImageRange(HMODULE mod, Region& out)
    {
        auto nt = NtHeaders(mod);
        if (!nt)
            return false;
        out.begin = reinterpret_cast<uint8_t*>(mod);
        out.end = out.begin + nt->OptionalHeader.SizeOfImage;
        return true;
    }

    bool GetSection(HMODULE mod, const char* name, Region& out)
    {
        auto nt = NtHeaders(mod);
        if (!nt)
            return false;
        char wanted[IMAGE_SIZEOF_SHORT_NAME] = {};
        for (int i = 0; i < IMAGE_SIZEOF_SHORT_NAME && name[i]; ++i)
            wanted[i] = name[i];

        auto sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        {
            if (memcmp(sec->Name, wanted, IMAGE_SIZEOF_SHORT_NAME) == 0)
            {
                out.begin = reinterpret_cast<uint8_t*>(mod) + sec->VirtualAddress;
                out.end = out.begin + sec->Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    bool ParsePattern(const char* text, Pattern& out)
    {
        out.len = 0;
        const char* p = text;
        while (*p)
        {
            while (*p == ' ')
                ++p;
            if (!*p)
                break;
            if (out.len >= kMaxPattern)
                return false;
            if (*p == '?')
            {
                out.bytes[out.len] = 0;
                out.mask[out.len] = 0;
                ++out.len;
                while (*p == '?')
                    ++p;
            }
            else
            {
                int hi = HexValue(p[0]);
                int lo = hi >= 0 ? HexValue(p[1]) : -1;
                if (hi < 0 || lo < 0)
                    return false;
                out.bytes[out.len] = static_cast<uint8_t>(hi * 16 + lo);
                out.mask[out.len] = 1;
                ++out.len;
                p += 2;
            }
            if (*p && *p != ' ')
                return false;
        }
        return out.len > 0;
    }

    int FindAll(const Region& region, const Pattern& pattern, uint8_t** out, int maxOut)
    {
        if (pattern.len <= 0 || !region.begin || region.Size() < static_cast<size_t>(pattern.len))
            return 0;

        int count = 0;
        const uint8_t* last = region.end - pattern.len;
        const bool anchored = pattern.mask[0] != 0;
        for (const uint8_t* cur = region.begin; cur <= last; ++cur)
        {
            if (anchored)
            {
                cur = static_cast<const uint8_t*>(memchr(cur, pattern.bytes[0], static_cast<size_t>(last - cur) + 1));
                if (!cur)
                    break;
            }
            int i = 1;
            for (; i < pattern.len; ++i)
            {
                if (pattern.mask[i] && cur[i] != pattern.bytes[i])
                    break;
            }
            if (i == pattern.len)
            {
                if (out && count < maxOut)
                    out[count] = const_cast<uint8_t*>(cur);
                ++count;
            }
        }
        return count;
    }

    int FindAll(const Region& region, const char* pattern, uint8_t** out, int maxOut)
    {
        Pattern p;
        if (!ParsePattern(pattern, p))
            return 0;
        return FindAll(region, p, out, maxOut);
    }

    uint8_t* FindUnique(const Region& region, const char* pattern, int* count)
    {
        uint8_t* hit = nullptr;
        int n = FindAll(region, pattern, &hit, 1);
        if (count)
            *count = n;
        return n == 1 ? hit : nullptr;
    }

    uint8_t* AllocStub(size_t size)
    {
        static uint8_t* page = nullptr;
        static size_t used = 0;
        constexpr size_t kPageSize = 4096;

        size = (size + 15) & ~static_cast<size_t>(15);
        if (size > kPageSize)
            return nullptr;
        if (!page || used + size > kPageSize)
        {
            page = static_cast<uint8_t*>(
                VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            used = 0;
            if (!page)
                return nullptr;
            memset(page, 0xCC, kPageSize);
        }
        uint8_t* result = page + used;
        used += size;
        return result;
    }

    namespace
    {
        // Suspends all other threads of the process. Uses no heap between Freeze() and Thaw(), so a thread suspended
        // while holding the heap lock can't deadlock us.
        class Freezer
        {
        public:
            bool Freeze()
            {
                count_ = 0;
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                if (snapshot == INVALID_HANDLE_VALUE)
                    return false;

                const DWORD pid = GetCurrentProcessId();
                const DWORD self = GetCurrentThreadId();
                THREADENTRY32 entry;
                entry.dwSize = sizeof(entry);
                if (Thread32First(snapshot, &entry))
                {
                    do
                    {
                        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self)
                            continue;
                        if (count_ >= kMaxThreads)
                            break;
                        HANDLE thread = OpenThread(
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                            entry.th32ThreadID);
                        if (!thread)
                            continue;
                        if (SuspendThread(thread) == static_cast<DWORD>(-1))
                        {
                            CloseHandle(thread);
                            continue;
                        }
                        threads_[count_++] = thread;
                    } while (Thread32Next(snapshot, &entry));
                }
                CloseHandle(snapshot);
                return true;
            }

            // True when a suspended thread's instruction pointer is inside (lo, hi) - exclusive on both ends.
            bool AnyInside(const uint8_t* lo, const uint8_t* hi) const
            {
                for (int i = 0; i < count_; ++i)
                {
                    CONTEXT ctx;
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    if (!GetThreadContext(threads_[i], &ctx))
                        continue;
                    auto ip = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(ctx.Eip));
                    if (ip > lo && ip < hi)
                        return true;
                }
                return false;
            }

            void Thaw()
            {
                for (int i = 0; i < count_; ++i)
                {
                    ResumeThread(threads_[i]);
                    CloseHandle(threads_[i]);
                }
                count_ = 0;
            }

        private:
            static constexpr int kMaxThreads = 1024;
            HANDLE threads_[kMaxThreads];
            int count_ = 0;
        };

        Freezer g_freezer;
    }

    ApplyResult ApplyPatches(const Patch* patches, int count, int* attempts)
    {
        constexpr int kMaxAttempts = 200;
        for (int attempt = 1; attempt <= kMaxAttempts; ++attempt)
        {
            if (attempts)
                *attempts = attempt;
            if (!g_freezer.Freeze())
                return ApplyResult::Error;

            bool busy = false;
            bool mismatch = false;
            for (int i = 0; i < count; ++i)
            {
                const Patch& p = patches[i];
                if (memcmp(p.addr, p.expect, p.len) != 0)
                    mismatch = true;
                if (p.multiInstruction && g_freezer.AnyInside(p.addr, p.addr + p.len))
                    busy = true;
            }
            if (mismatch)
            {
                g_freezer.Thaw();
                return ApplyResult::Mismatch;
            }
            if (busy)
            {
                g_freezer.Thaw();
                Sleep(5);
                continue;
            }

            bool failed = false;
            for (int i = 0; i < count && !failed; ++i)
            {
                const Patch& p = patches[i];
                DWORD oldProtect = 0;
                if (!VirtualProtect(p.addr, p.len, PAGE_EXECUTE_READWRITE, &oldProtect))
                {
                    // roll back what was already written
                    for (int j = 0; j < i; ++j)
                    {
                        DWORD prot = 0;
                        if (VirtualProtect(patches[j].addr, patches[j].len, PAGE_EXECUTE_READWRITE, &prot))
                        {
                            memcpy(patches[j].addr, patches[j].expect, patches[j].len);
                            VirtualProtect(patches[j].addr, patches[j].len, prot, &prot);
                        }
                    }
                    failed = true;
                    break;
                }
                memcpy(p.addr, p.data, p.len);
                VirtualProtect(p.addr, p.len, oldProtect, &oldProtect);
            }
            for (int i = 0; i < count; ++i)
                FlushInstructionCache(GetCurrentProcess(), patches[i].addr, patches[i].len);

            g_freezer.Thaw();
            return failed ? ApplyResult::Error : ApplyResult::Ok;
        }
        return ApplyResult::Busy;
    }
}
