#include "harness.h"

#include <cmath>
#include <cstring>
#include <fmt/core.h>

namespace XenonTest
{
    static bool g_trapped = false;

    void trapHit()
    {
        g_trapped = true;
    }

    std::vector<const Case*>& registry()
    {
        static std::vector<const Case*> cases;
        return cases;
    }

    Registrar::Registrar(const Case* cases, size_t count)
    {
        for (size_t i = 0; i < count; i++)
            registry().push_back(&cases[i]);
    }

    // PPC element order: [e0, e1, e2, e3] == [u32[3], u32[2], u32[1], u32[0]].
    std::string formatVec(const PPCVRegister& v)
    {
        return fmt::format("[{:08X} {:08X} {:08X} {:08X}]", v.u32[3], v.u32[2], v.u32[1], v.u32[0]);
    }

    static std::string formatVecF32(const PPCVRegister& v)
    {
        return fmt::format("[{} {} {} {}]", v.f32[3], v.f32[2], v.f32[1], v.f32[0]);
    }

    static std::string formatVecF64(const PPCVRegister& v)
    {
        return fmt::format("[{} {}]", v.f64[1], v.f64[0]);
    }

    void Run::call(PPCFunc* fn)
    {
        g_trapped = false;
        fn(ctx, base);
        trapped = g_trapped;
        if (trapped)
            failures.push_back("hit __builtin_debugtrap() at runtime (instruction form not implemented)");
    }

    void Run::writeMem(uint32_t addr, const uint8_t* bytes, size_t count)
    {
        memcpy(base + addr, bytes, count);
    }

    void Run::expectVec(const char* name, const PPCVRegister& actual, const VecExpect& e)
    {
        PPCVRegister expected;
        memcpy(expected.u8, e.bytes, 16);

        bool ok = true;
        if (e.kind == VecKind::Bits || e.tol <= 0.0)
        {
            for (size_t i = 0; i < 16 && ok; i++)
            {
                if (!e.care[i])
                    continue;

                // NaN wildcard works on whole lanes; care is all-or-nothing per lane there.
                if (e.kind == VecKind::F32 && e.anyNaN[i / 4])
                {
                    if (!std::isnan(actual.f32[i / 4]))
                        ok = false;
                    continue;
                }
                if (e.kind == VecKind::F64 && e.anyNaN[(i / 8) * 2])
                {
                    if (!std::isnan(actual.f64[i / 8]))
                        ok = false;
                    continue;
                }
                if (actual.u8[i] != e.bytes[i])
                    ok = false;
            }
        }
        else if (e.kind == VecKind::F32)
        {
            for (size_t lane = 0; lane < 4 && ok; lane++)
            {
                if (!e.care[lane * 4])
                    continue;
                if (e.anyNaN[lane])
                {
                    ok = std::isnan(actual.f32[lane]);
                    continue;
                }
                float a = actual.f32[lane], x = expected.f32[lane];
                if (std::isnan(a) || std::isnan(x))
                    ok = std::isnan(a) && std::isnan(x);
                else if (std::isinf(a) || std::isinf(x))
                    ok = a == x;
                else
                    ok = std::fabs(double(a) - double(x)) <= e.tol;
            }
        }
        else
        {
            for (size_t lane = 0; lane < 2 && ok; lane++)
            {
                if (!e.care[lane * 8])
                    continue;
                if (e.anyNaN[lane * 2])
                {
                    ok = std::isnan(actual.f64[lane]);
                    continue;
                }
                double a = actual.f64[lane], x = expected.f64[lane];
                if (std::isnan(a) || std::isnan(x))
                    ok = std::isnan(a) && std::isnan(x);
                else if (std::isinf(a) || std::isinf(x))
                    ok = a == x;
                else
                    ok = std::fabs(a - x) <= e.tol;
            }
        }

        if (ok)
            return;

        std::string msg = fmt::format("{}: expected {} actual {}", name, formatVec(expected), formatVec(actual));
        if (e.kind == VecKind::F32)
            msg += fmt::format("\n      as f32: expected {} actual {}", formatVecF32(expected), formatVecF32(actual));
        else if (e.kind == VecKind::F64)
            msg += fmt::format("\n      as f64: expected {} actual {}", formatVecF64(expected), formatVecF64(actual));

        bool partial = false, nan = false;
        for (size_t i = 0; i < 16; i++) partial |= !e.care[i];
        for (size_t i = 0; i < 4; i++) nan |= e.anyNaN[i] != 0;
        if (partial || nan || e.tol > 0.0)
        {
            msg += "\n      (";
            if (partial) msg += "some lanes ignored; ";
            if (nan) msg += "NaN lanes accept any NaN; ";
            if (e.tol > 0.0) msg += fmt::format("tol {}; ", e.tol);
            msg.resize(msg.size() - 2);
            msg += ")";
        }
        failures.push_back(msg);
    }

    void Run::expectU64(const char* name, uint64_t actual, uint64_t expected)
    {
        if (actual != expected)
            failures.push_back(fmt::format("{}: expected 0x{:X} actual 0x{:X}", name, expected, actual));
    }

    void Run::expectF64(const char* name, const PPCRegister& actual, uint64_t expectedBits, bool anyNaN, double tol)
    {
        PPCRegister expected;
        expected.u64 = expectedBits;

        bool ok;
        if (anyNaN)
            ok = std::isnan(actual.f64);
        else if (tol > 0.0 && !std::isnan(actual.f64) && !std::isnan(expected.f64) && !std::isinf(actual.f64) && !std::isinf(expected.f64))
            ok = std::fabs(actual.f64 - expected.f64) <= tol;
        else
            ok = actual.u64 == expected.u64;

        if (!ok)
            failures.push_back(fmt::format("{}: expected 0x{:016X} ({}) actual 0x{:016X} ({})", name, expected.u64, expected.f64, actual.u64, actual.f64));
    }

    void Run::expectCR(const char* name, const PPCCRRegister& actual, uint8_t lt, uint8_t gt, uint8_t eq, uint8_t so)
    {
        if (actual.lt != lt || actual.gt != gt || actual.eq != eq || actual.so != so)
        {
            failures.push_back(fmt::format("{}: expected lt={} gt={} eq={} so={} actual lt={} gt={} eq={} so={}",
                name, lt, gt, eq, so, actual.lt, actual.gt, actual.eq, actual.so));
        }
    }

    void Run::expectXER(const PPCXERRegister& actual, uint8_t so, uint8_t ov, uint8_t ca)
    {
        if (actual.so != so || actual.ov != ov || actual.ca != ca)
        {
            failures.push_back(fmt::format("xer: expected so={} ov={} ca={} actual so={} ov={} ca={}",
                so, ov, ca, actual.so, actual.ov, actual.ca));
        }
    }

    void Run::expectMem(uint32_t addr, const uint8_t* bytes, size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            if (base[addr + i] != bytes[i])
            {
                failures.push_back(fmt::format("mem[0x{:X}+0x{:X}]: expected 0x{:02X} actual 0x{:02X}",
                    addr, i, bytes[i], base[addr + i]));
                return;
            }
        }
    }
}
