#include "hooks.h"

#include <string.h>

namespace hooks
{
    namespace
    {
        void PutRel32(uint8_t* insn, uint8_t opcode, const void* target)
        {
            insn[0] = opcode;
            int32_t rel = mem::RelTo(insn, target);
            memcpy(insn + 1, &rel, 4);
        }

        void PutAbs32(uint8_t* at, const void* address)
        {
            uint32_t value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(address));
            memcpy(at, &value, 4);
        }
    }

    bool MakeCallRetarget(const char* name, uint8_t* callInsn, const void* newTarget, mem::Patch& out,
                          void** original)
    {
        if (!callInsn || callInsn[0] != 0xE8 || !newTarget)
            return false;
        if (original)
            *original = mem::RelTarget(callInsn);

        out = mem::Patch();
        out.name = name;
        out.addr = callInsn + 1;
        out.len = 4;
        memcpy(out.expect, callInsn + 1, 4);
        int32_t rel = mem::RelTo(callInsn, newTarget);
        memcpy(out.data, &rel, 4);
        // A rel32 operand is part of a single instruction: a suspended thread is either before or after it.
        out.multiInstruction = false;
        return true;
    }

    bool MakeEntryDetour(const char* name, uint8_t* fn, const void* hook, mem::Patch& out, void** trampoline)
    {
        if (!fn || !hook || !trampoline)
            return false;
        if (fn[0] != 0x56 || fn[1] != 0x8B || fn[2] != 0xF1 || fn[3] != 0xE8)
            return false;

        uint8_t* stub = mem::AllocStub(16);
        if (!stub)
            return false;
        stub[0] = 0x56;
        stub[1] = 0x8B;
        stub[2] = 0xF1;
        PutRel32(stub + 3, 0xE8, mem::RelTarget(fn + 3));
        PutRel32(stub + 8, 0xE9, fn + 8);
        *trampoline = stub;

        out = mem::Patch();
        out.name = name;
        out.addr = fn;
        out.len = 8;
        memcpy(out.expect, fn, 8);
        out.data[0] = 0xE9;
        int32_t rel = mem::RelTo(fn, hook); // relative to where the jump will live, not to out.data

        memcpy(out.data + 1, &rel, 4);
        out.data[5] = 0x90;
        out.data[6] = 0x90;
        out.data[7] = 0x90;
        out.multiInstruction = true;
        return true;
    }

    bool MakeFlagMidHook(const char* name, uint8_t* site, int insnLen, volatile uint8_t* flagA,
                         volatile uint8_t* flagB, mem::Patch& out)
    {
        if (!site || !flagA || insnLen < 5 || insnLen > 12)
            return false;

        uint8_t* stub = mem::AllocStub(7 + 7 + 12 + 5);
        if (!stub)
            return false;
        uint8_t* p = stub;
        // C6 05 <abs32> 01 : mov byte ptr [abs32], 1
        p[0] = 0xC6;
        p[1] = 0x05;
        PutAbs32(p + 2, const_cast<uint8_t*>(flagA));
        p[6] = 0x01;
        p += 7;
        if (flagB)
        {
            p[0] = 0xC6;
            p[1] = 0x05;
            PutAbs32(p + 2, const_cast<uint8_t*>(flagB));
            p[6] = 0x01;
            p += 7;
        }
        memcpy(p, site, static_cast<size_t>(insnLen));
        p += insnLen;
        PutRel32(p, 0xE9, site + insnLen);

        out = mem::Patch();
        out.name = name;
        out.addr = site;
        out.len = static_cast<uint8_t>(insnLen);
        memcpy(out.expect, site, static_cast<size_t>(insnLen));
        out.data[0] = 0xE9;
        int32_t rel = mem::RelTo(site, stub);
        memcpy(out.data + 1, &rel, 4);
        for (int i = 5; i < insnLen; ++i)
            out.data[i] = 0x90;
        // One instruction is replaced: a suspended thread can only be at its start or after it.
        out.multiInstruction = false;
        return true;
    }
}
