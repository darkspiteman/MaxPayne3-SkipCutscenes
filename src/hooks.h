#pragma once
#include "mem.h"

// Builders for the three kinds of x86 hooks the mod needs. Each one only prepares a mem::Patch (and, if needed, a stub
// in executable memory); nothing in the game is modified until mem::ApplyPatches() runs.
namespace hooks
{
    // Re-targets an existing "call rel32" (E8). original receives the previous destination.
    bool MakeCallRetarget(const char* name, uint8_t* callInsn, const void* newTarget, mem::Patch& out,
                          void** original);

    // Detours a function whose first 8 bytes are exactly
    //     56              push esi
    //     8B F1           mov  esi, ecx
    //     E8 rel32        call <callee>
    // trampoline receives a stub that runs those instructions (call re-based) and continues at fn + 8.
    bool MakeEntryDetour(const char* name, uint8_t* fn, const void* hook, mem::Patch& out, void** trampoline);

    // Replaces one position-independent instruction of insnLen (5..12) bytes with a jump to a stub that executes
    //     mov byte ptr [flagA], 1
    //     mov byte ptr [flagB], 1      (when flagB != nullptr)
    //     <the original instruction>
    //     jmp site + insnLen
    // No registers or flags are touched by the added instructions.
    bool MakeFlagMidHook(const char* name, uint8_t* site, int insnLen, volatile uint8_t* flagA,
                         volatile uint8_t* flagB, mem::Patch& out);
}
