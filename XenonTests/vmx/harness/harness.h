#pragma once

// Runtime side of the VMX instruction tests. Generated case files include this
// header, then the recompiled functions, then the per-case setup/check code.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Full PPCContext: no register is a local, nothing is skipped.
#define PPC_CONFIG_H_INCLUDED
#define PPC_IMAGE_BASE 0x82000000ull
#define PPC_IMAGE_SIZE 0x10000ull
#define PPC_CODE_BASE 0x82000000ull
#define PPC_CODE_SIZE 0x10000ull
// Same table lookup as the ppc_context.h default, but a target nothing is
// mapped at (see Run::mapFunc) fails the case instead of crashing the suite.
#define PPC_CALL_INDIRECT_FUNC(x) do { if (!XenonTest::callIndirect(ctx, base, uint32_t(x))) return; } while (0)
#include <ppc_context.h>

namespace XenonTest
{
    enum class VecKind : uint8_t
    {
        Bits, // compare bytes exactly (subject to care mask)
        F32,  // 32-bit lanes may use anyNaN / tol
        F64,  // 64-bit lanes may use anyNaN / tol
    };

    // Expected 128-bit value in PPCVRegister byte order (u8[15] is PPC byte 0).
    struct VecExpect
    {
        uint8_t bytes[16];
        uint8_t care[16];   // 0xFF: compare this byte, 0: don't care
        uint8_t anyNaN[4];  // per 32-bit lane (PPCVRegister::u32 index): accept any NaN
        VecKind kind;
        double tol;         // > 0: |actual - expected| <= tol instead of exact bits
    };

    struct Run
    {
        PPCContext& ctx;
        uint8_t* base;
        std::vector<std::string> failures;
        bool trapped = false;

        void call(PPCFunc* fn);

        // Makes fn the target of PPC_CALL_INDIRECT_FUNC(guest) (the real PPC_LOOKUP_FUNC table).
        void mapFunc(uint32_t guest, PPCFunc* fn);

        void writeMem(uint32_t addr, const uint8_t* bytes, size_t count);

        void expectVec(const char* name, const PPCVRegister& actual, const VecExpect& expected);
        void expectU64(const char* name, uint64_t actual, uint64_t expected);
        void expectF64(const char* name, const PPCRegister& actual, uint64_t expectedBits, bool anyNaN, double tol);
        void expectCR(const char* name, const PPCCRRegister& actual, uint8_t lt, uint8_t gt, uint8_t eq, uint8_t so);
        void expectXER(const PPCXERRegister& actual, uint8_t so, uint8_t ov, uint8_t ca);
        void expectMem(uint32_t addr, const uint8_t* bytes, size_t count);
    };

    struct Case
    {
        const char* group;
        const char* name;
        const char* source;
        const char* disasm;          // one instruction per line
        bool xfail;
        const char* notImplemented;  // non-null: the recompiler emitted no real code (reason)
        const char* badCase;         // non-null: the generator rejected the case (reason)
        void (*run)(Run&);
    };

    std::vector<const Case*>& registry();

    struct Registrar
    {
        Registrar(const Case* cases, size_t count);
    };

    // Called by the __builtin_debugtrap() replacement in generated code.
    void trapHit();

    // Called by the PPC_CALL_INDIRECT_FUNC replacement: calls the function mapped
    // at guest and returns true, or records the unmapped target and returns false.
    bool callIndirect(PPCContext& ctx, uint8_t* base, uint32_t guest);

    std::string formatVec(const PPCVRegister& v);
}
