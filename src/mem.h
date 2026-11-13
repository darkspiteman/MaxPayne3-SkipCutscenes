#pragma once
#include <windows.h>
#include <stdint.h>

// Memory helpers: PE section lookup, wildcard byte-pattern search and code patching that is applied while all other
// threads of the process are suspended. No third-party code.
namespace mem
{
    struct Region
    {
        uint8_t* begin = nullptr;
        uint8_t* end = nullptr;

        bool Contains(const void* p) const
        {
            return static_cast<const uint8_t*>(p) >= begin && static_cast<const uint8_t*>(p) < end;
        }
        size_t Size() const
        {
            return static_cast<size_t>(end - begin);
        }
    };

    bool GetImageRange(HMODULE mod, Region& out);
    bool GetSection(HMODULE mod, const char* name, Region& out);

    constexpr int kMaxPattern = 128;
    struct Pattern
    {
        uint8_t bytes[kMaxPattern];
        uint8_t mask[kMaxPattern]; // 1 = byte must match, 0 = wildcard
        int len = 0;
    };

    // "E8 ? ? ? ? 8B 0D" -> Pattern. Returns false on a malformed or too long string.
    bool ParsePattern(const char* text, Pattern& out);
    // Returns the total number of matches; the first maxOut addresses are stored in out.
    int FindAll(const Region& region, const Pattern& pattern, uint8_t** out, int maxOut);
    int FindAll(const Region& region, const char* pattern, uint8_t** out, int maxOut);
    // Returns the match only when there is exactly one. count receives the number of matches.
    uint8_t* FindUnique(const Region& region, const char* pattern, int* count = nullptr);

    // Destination of an E8/E9 rel32 instruction.
    inline uint8_t* RelTarget(const uint8_t* insn)
    {
        return const_cast<uint8_t*>(insn) + 5 + *reinterpret_cast<const int32_t*>(insn + 1);
    }
    inline int32_t RelTo(const uint8_t* insn, const void* target)
    {
        return static_cast<int32_t>(static_cast<const uint8_t*>(target) - (insn + 5));
    }

    // Executable memory for hook stubs (never freed; a few dozen bytes for the process lifetime).
    uint8_t* AllocStub(size_t size);

    constexpr int kMaxPatchBytes = 16;
    struct Patch
    {
        const char* name = "";
        uint8_t* addr = nullptr;
        uint8_t len = 0;
        uint8_t expect[kMaxPatchBytes] = {}; // bytes that must be at addr right before patching
        uint8_t data[kMaxPatchBytes] = {};   // bytes to write
        // The patch overwrites more than one instruction: no thread may be suspended inside (addr, addr + len).
        bool multiInstruction = false;
    };

    enum class ApplyResult
    {
        Ok,
        Mismatch, // code changed between scanning and patching (another mod?) - nothing was written
        Busy,     // a thread kept executing inside a patched range - nothing was written
        Error,    // VirtualProtect failed - nothing or only part was written (logged by the caller)
    };

    // All-or-nothing: suspends every other thread, verifies, writes, resumes.
    ApplyResult ApplyPatches(const Patch* patches, int count, int* attempts = nullptr);
}
