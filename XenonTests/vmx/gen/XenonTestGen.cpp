// XenonTestGen <cases/group.toml> <out.cpp>
//
// Feeds each case's instruction words (plus an implicit blr) through the real
// Recompiler and writes a C++ file with the recompiled functions, one setup/check
// function per case, and a static registration into the XenonVmxTests registry.
//
// Problems with a case never fail the build: they become a "bad case" entry
// that fails at run time with the message, so one group can't block another.

#include "recompiler.h"
#include <function.h>
#include <byteswap.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr uint64_t kGuestMemorySize = 0x100000000ull; // PPC_MEMORY_SIZE

    struct CaseError : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    template<class... Args>
    [[noreturn]] void fail(fmt::format_string<Args...> f, Args&&... args)
    {
        throw CaseError(fmt::vformat(f.get(), fmt::make_format_args(args...)));
    }

    // --- C++ emission helpers ------------------------------------------------

    std::string cstr(std::string_view s)
    {
        std::string out = "\"";
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F)
                    out += fmt::format("\\x{:02X}\"\"", c);
                else
                    out += char(c);
            }
        }
        out += '"';
        return out;
    }

    std::string ident(std::string_view s)
    {
        std::string out;
        for (char c : s)
            out += (isalnum(static_cast<unsigned char>(c)) ? c : '_');
        if (out.empty() || isdigit(static_cast<unsigned char>(out[0])))
            out = "_" + out;
        return out;
    }

    // --- TOML value parsing --------------------------------------------------

    std::string trim(std::string_view s)
    {
        size_t b = s.find_first_not_of(" \t");
        size_t e = s.find_last_not_of(" \t");
        return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
    }

    // Integer from a TOML integer or a string ("0x1F", "-5", "0b101").
    uint64_t parseInt(const toml::node& n, std::string_view what)
    {
        if (auto v = n.value<int64_t>())
            return static_cast<uint64_t>(*v);

        if (auto s = n.value<std::string>())
        {
            std::string t = trim(*s);
            bool neg = false;
            if (!t.empty() && (t[0] == '-' || t[0] == '+'))
            {
                neg = t[0] == '-';
                t.erase(0, 1);
            }
            int base = 10;
            if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))
            {
                base = 16;
                t.erase(0, 2);
            }
            else if (t.size() > 2 && t[0] == '0' && (t[1] == 'b' || t[1] == 'B'))
            {
                base = 2;
                t.erase(0, 2);
            }
            if (t.empty())
                fail("{}: '{}' is not an integer", what, *s);
            char* end = nullptr;
            errno = 0;
            uint64_t v = strtoull(t.c_str(), &end, base);
            if (*end != '\0' || errno != 0)
                fail("{}: '{}' is not an integer", what, *s);
            return neg ? (~v + 1) : v;
        }

        fail("{}: expected an integer or a string like \"0x1F\"", what);
    }

    // Value for r*/ctr/lr: an integer as parseInt takes it, or a code address
    // relative to the case, "self" (insns[0]) or "callee" (callee[0]), with an
    // optional " + N" / " - N". Returns a C++ expression; run_N defines kSelf
    // and kCallee, since addresses are only known once the code is laid out.
    std::string parseIntOrAddr(const toml::node& n, std::string_view what, bool hasCallee)
    {
        if (auto s = n.value<std::string>())
        {
            std::string t = trim(*s);
            for (const char* sym : { "self", "callee" })
            {
                size_t len = strlen(sym);
                if (t.compare(0, len, sym) != 0)
                    continue;
                if (!hasCallee && sym[0] == 'c')
                    fail("{}: \"callee\" needs a callee = [...] in the case", what);
                std::string rest = trim(t.substr(len));
                char sign = '+';
                uint64_t off = 0;
                if (!rest.empty())
                {
                    sign = rest[0];
                    if (sign != '+' && sign != '-')
                        fail("{}: '{}' is not an address (\"{}\", \"{} + 4\", \"{} - 4\")", what, *s, sym, sym, sym);
                    off = parseInt(toml::value<std::string>(trim(rest.substr(1))), what);
                }
                return fmt::format("({} {} 0x{:X}ull)", sym[0] == 's' ? "kSelf" : "kCallee", sign, off);
            }
        }
        return fmt::format("0x{:X}ull", parseInt(n, what));
    }

    enum class LaneType { U8, S8, U16, S16, U32, S32, U64, S64, F32, F64 };

    struct LaneTypeInfo
    {
        const char* key;
        LaneType type;
        size_t size;
        bool isFloat;
        bool isSigned;
    };

    constexpr LaneTypeInfo kLaneTypes[] = {
        { "u8",  LaneType::U8,  1, false, false },
        { "s8",  LaneType::S8,  1, false, true  },
        { "u16", LaneType::U16, 2, false, false },
        { "s16", LaneType::S16, 2, false, true  },
        { "u32", LaneType::U32, 4, false, false },
        { "s32", LaneType::S32, 4, false, true  },
        { "u64", LaneType::U64, 8, false, false },
        { "s64", LaneType::S64, 8, false, true  },
        { "f32", LaneType::F32, 4, true,  true  },
        { "f64", LaneType::F64, 8, true,  true  },
    };

    struct Lane
    {
        uint64_t bits = 0;
        bool anyNaN = false;
        bool ignore = false;
    };

    uint64_t floatBits(double value, size_t size)
    {
        if (size == 4)
        {
            float f = static_cast<float>(value);
            uint32_t bits;
            memcpy(&bits, &f, 4);
            return bits;
        }
        uint64_t bits;
        memcpy(&bits, &value, 8);
        return bits;
    }

    Lane parseLane(const toml::node& n, const LaneTypeInfo& type, bool isExpect, std::string_view what)
    {
        Lane lane;

        if (auto s = n.value<std::string>())
        {
            std::string t = trim(*s);
            std::string lower;
            for (char c : t) lower += char(tolower(static_cast<unsigned char>(c)));

            if (lower == "*")
            {
                if (!isExpect)
                    fail("{}: \"*\" (don't care) is only valid in [case.out]", what);
                lane.ignore = true;
                return lane;
            }

            if (type.isFloat)
            {
                if (lower == "nan" || lower == "+nan" || lower == "-nan")
                {
                    if (isExpect)
                        lane.anyNaN = true; // any NaN matches
                    lane.bits = type.size == 4 ? 0x7FC00000ull : 0x7FF8000000000000ull;
                    if (lower[0] == '-')
                        lane.bits |= type.size == 4 ? 0x80000000ull : 0x8000000000000000ull;
                    return lane;
                }
                if (lower == "inf" || lower == "+inf")
                {
                    lane.bits = type.size == 4 ? 0x7F800000ull : 0x7FF0000000000000ull;
                    return lane;
                }
                if (lower == "-inf")
                {
                    lane.bits = type.size == 4 ? 0xFF800000ull : 0xFFF0000000000000ull;
                    return lane;
                }
                if (lower.rfind("0x", 0) == 0 || lower.rfind("-0x", 0) == 0)
                {
                    lane.bits = parseInt(n, what); // raw bits
                    return lane;
                }
                char* end = nullptr;
                double d = strtod(t.c_str(), &end);
                if (t.empty() || *end != '\0')
                    fail("{}: '{}' is not a float, \"nan\", \"inf\", \"-inf\", \"*\" or hex bits", what, *s);
                lane.bits = floatBits(d, type.size);
                return lane;
            }

            lane.bits = parseInt(n, what);
        }
        else if (type.isFloat)
        {
            if (auto d = n.value<double>())
                lane.bits = floatBits(*d, type.size);
            else if (auto i = n.value<int64_t>())
                lane.bits = floatBits(static_cast<double>(*i), type.size);
            else
                fail("{}: expected a float", what);
            return lane;
        }
        else if (auto i = n.value<int64_t>())
        {
            lane.bits = static_cast<uint64_t>(*i);
        }
        else
        {
            fail("{}: expected an integer", what);
        }

        // Range check: accept the signed and unsigned range of the lane width.
        if (type.size < 8)
        {
            uint64_t max = (1ull << (type.size * 8)) - 1;
            int64_t s = static_cast<int64_t>(lane.bits);
            int64_t min = -static_cast<int64_t>(1ull << (type.size * 8 - 1));
            if (!(lane.bits <= max || s >= min))
                fail("{}: value does not fit in {} bits", what, type.size * 8);
            lane.bits &= max;
        }
        return lane;
    }

    struct VecSpec
    {
        uint8_t bytes[16] = {};
        uint8_t care[16] = {};
        uint8_t anyNaN[4] = {};
        const char* kind = "Bits";
        double tol = 0.0;
    };

    // { u32 = [e0, e1, e2, e3] } etc. Lanes are in PPC element order; element 0
    // is the most significant, which is PPCVRegister::u32[3] on the host.
    VecSpec parseVec(const toml::node& n, bool isExpect, std::string_view what)
    {
        auto tbl = n.as_table();
        if (tbl == nullptr)
            fail("{}: expected a table like {{ u32 = [..] }}", what);

        const LaneTypeInfo* type = nullptr;
        const toml::array* lanes = nullptr;
        VecSpec spec;

        for (auto&& [key, value] : *tbl)
        {
            std::string k(key.str());
            if (k == "tol")
            {
                if (!isExpect)
                    fail("{}: tol is only valid in [case.out]", what);
                auto d = value.value<double>();
                if (!d || *d < 0.0)
                    fail("{}: tol must be a non-negative float", what);
                spec.tol = *d;
                continue;
            }

            const LaneTypeInfo* found = nullptr;
            for (auto& t : kLaneTypes)
                if (k == t.key)
                    found = &t;
            if (found == nullptr)
                fail("{}: unknown key '{}' (expected one of u8 s8 u16 s16 u32 s32 u64 s64 f32 f64, tol)", what, k);
            if (type != nullptr)
                fail("{}: more than one lane type given", what);

            type = found;
            lanes = value.as_array();
            if (lanes == nullptr)
                fail("{}: {} must be an array", what, k);
        }

        if (type == nullptr)
            fail("{}: no lane type given (u32 = [..], f32 = [..], ...)", what);

        size_t count = 16 / type->size;
        if (lanes->size() != count)
            fail("{}: {} needs exactly {} elements, got {}", what, type->key, count, lanes->size());

        if (spec.tol > 0.0 && !type->isFloat)
            fail("{}: tol is only valid with f32/f64 lanes", what);

        spec.kind = type->type == LaneType::F32 ? "F32" : type->type == LaneType::F64 ? "F64" : "Bits";

        for (size_t i = 0; i < count; i++)
        {
            Lane lane = parseLane(*lanes->get(i), *type, isExpect, fmt::format("{} {}[{}]", what, type->key, i));

            size_t offset = (count - 1 - i) * type->size;
            for (size_t b = 0; b < type->size; b++)
            {
                spec.bytes[offset + b] = uint8_t(lane.bits >> (b * 8));
                spec.care[offset + b] = lane.ignore ? 0 : 0xFF;
            }
            if (lane.anyNaN)
            {
                spec.anyNaN[offset / 4] = 1;
                if (type->size == 8)
                    spec.anyNaN[offset / 4 + 1] = 1;
            }
        }
        return spec;
    }

    struct CRSpec { uint8_t lt = 0, gt = 0, eq = 0, so = 0; };

    CRSpec parseCR(const toml::node& n, std::string_view what)
    {
        CRSpec cr;
        if (auto tbl = n.as_table())
        {
            for (auto&& [key, value] : *tbl)
            {
                std::string k(key.str());
                uint8_t bit;
                if (auto b = value.value<bool>())
                    bit = *b;
                else if (auto i = value.value<int64_t>())
                    bit = *i != 0;
                else
                    fail("{}: {} must be 0/1 or true/false", what, k);

                if (k == "lt") cr.lt = bit;
                else if (k == "gt") cr.gt = bit;
                else if (k == "eq") cr.eq = bit;
                else if (k == "so" || k == "un") cr.so = bit;
                else fail("{}: unknown CR field '{}' (lt, gt, eq, so/un)", what, k);
            }
            return cr;
        }
        if (auto s = n.value<std::string>())
        {
            std::string t = *s;
            for (char& c : t)
                if (c == '|' || c == ',' || c == '+') c = ' ';
            size_t pos = 0;
            while (pos < t.size())
            {
                size_t start = t.find_first_not_of(' ', pos);
                if (start == std::string::npos) break;
                size_t end = t.find(' ', start);
                if (end == std::string::npos) end = t.size();
                std::string tok = t.substr(start, end - start);
                if (tok == "lt") cr.lt = 1;
                else if (tok == "gt") cr.gt = 1;
                else if (tok == "eq") cr.eq = 1;
                else if (tok == "so" || tok == "un") cr.so = 1;
                else fail("{}: unknown CR flag '{}' (lt, gt, eq, so/un)", what, tok);
                pos = end;
            }
            return cr;
        }
        if (auto i = n.value<int64_t>())
        {
            if (*i < 0 || *i > 15)
                fail("{}: CR field value must be 0..15 (lt=8 gt=4 eq=2 so=1)", what);
            cr.lt = (*i >> 3) & 1;
            cr.gt = (*i >> 2) & 1;
            cr.eq = (*i >> 1) & 1;
            cr.so = *i & 1;
            return cr;
        }
        fail("{}: expected a table {{ lt=0, gt=0, eq=1, so=0 }}, a string like \"eq\" or an integer 0..15", what);
    }

    std::vector<uint8_t> parseBytes(const toml::node& n, std::string_view what)
    {
        std::vector<uint8_t> bytes;
        if (auto arr = n.as_array())
        {
            for (size_t i = 0; i < arr->size(); i++)
            {
                uint64_t v = parseInt(*arr->get(i), fmt::format("{}[{}]", what, i));
                if (v > 0xFF && v < 0xFFFFFFFFFFFFFF00ull)
                    fail("{}[{}]: byte out of range", what, i);
                bytes.push_back(uint8_t(v));
            }
            return bytes;
        }
        if (auto s = n.value<std::string>())
        {
            std::string hex;
            for (char c : *s)
                if (c != ' ' && c != '_')
                    hex += c;
            if (hex.size() % 2 != 0)
                fail("{}: hex string needs an even number of digits", what);
            for (size_t i = 0; i < hex.size(); i += 2)
            {
                char* end = nullptr;
                std::string pair = hex.substr(i, 2);
                unsigned long v = strtoul(pair.c_str(), &end, 16);
                if (*end != '\0')
                    fail("{}: '{}' is not hex", what, pair);
                bytes.push_back(uint8_t(v));
            }
            return bytes;
        }
        fail("{}: expected an array of bytes or a hex string", what);
    }

    std::string bytesLiteral(const uint8_t* bytes, size_t count)
    {
        std::string out = "{";
        for (size_t i = 0; i < count; i++)
            out += fmt::format("{}0x{:02X}", i ? "," : "", bytes[i]);
        return out + "}";
    }

    // Parses "v3" -> ('v', 3) with range checks; returns false if not a register.
    bool parseRegName(std::string_view key, char& kind, unsigned& index)
    {
        if (key.size() < 2 || key.size() > 4)
            return false;
        kind = key[0];
        unsigned max;
        switch (kind)
        {
        case 'v': max = 127; break;
        case 'r': case 'f': max = 31; break;
        case 'c':
            if (key.rfind("cr", 0) != 0) return false;
            key.remove_prefix(1);
            max = 7;
            break;
        default: return false;
        }
        std::string digits(key.substr(1));
        if (digits.empty() || digits.size() > 3)
            return false;
        for (char c : digits)
            if (!isdigit(static_cast<unsigned char>(c)))
                return false;
        index = unsigned(atoi(digits.c_str()));
        return index <= max && (digits.size() == 1 || digits[0] != '0');
    }

    // --- Register state tables -----------------------------------------------

    // Emits C++ for [case.in] (isExpect = false) or [case.out] (isExpect = true).
    // hasCallee: the case has a callee, so "callee" addresses are allowed.
    std::string emitState(const toml::table& tbl, bool isExpect, bool hasCallee)
    {
        std::string out;
        const char* section = isExpect ? "out" : "in";

        for (auto&& [key, value] : tbl)
        {
            std::string k(key.str());
            std::string what = fmt::format("[case.{}] {}", section, k);

            char kind;
            unsigned index;
            if (parseRegName(k, kind, index))
            {
                switch (kind)
                {
                case 'v':
                {
                    VecSpec spec = parseVec(value, isExpect, what);
                    if (isExpect)
                    {
                        out += fmt::format("\trun.expectVec(\"v{0}\", ctx.v{0}, XenonTest::VecExpect{{ {1}, {2}, {3}, XenonTest::VecKind::{4}, {5} }});\n",
                            index, bytesLiteral(spec.bytes, 16), bytesLiteral(spec.care, 16), bytesLiteral(spec.anyNaN, 4), spec.kind, spec.tol);
                    }
                    else
                    {
                        for (size_t w = 0; w < 4; w++)
                        {
                            uint32_t v = 0;
                            memcpy(&v, spec.bytes + w * 4, 4);
                            out += fmt::format("\tctx.v{}.u32[{}] = 0x{:08X}u;\n", index, w, v);
                        }
                    }
                    break;
                }
                case 'r':
                {
                    std::string v = parseIntOrAddr(value, what, hasCallee);
                    if (isExpect)
                        out += fmt::format("\trun.expectU64(\"r{0}\", ctx.r{0}.u64, {1});\n", index, v);
                    else
                        out += fmt::format("\tctx.r{}.u64 = {};\n", index, v);
                    break;
                }
                case 'f':
                {
                    // f3 = 1.5 | f3 = "0x3FF8000000000000" | f3 = { f64 = 1.5, tol = 1e-9 } | { u64 = "0x.." }
                    uint64_t bits = 0;
                    bool anyNaN = false;
                    double tol = 0.0;
                    const LaneTypeInfo& f64 = kLaneTypes[9];
                    if (auto t = value.as_table())
                    {
                        bool have = false;
                        for (auto&& [fk, fv] : *t)
                        {
                            std::string fkey(fk.str());
                            if (fkey == "tol")
                            {
                                if (!isExpect) fail("{}: tol is only valid in [case.out]", what);
                                tol = fv.value_or(0.0);
                            }
                            else if (fkey == "f64")
                            {
                                Lane lane = parseLane(fv, f64, isExpect, what);
                                bits = lane.bits; anyNaN = lane.anyNaN; have = true;
                            }
                            else if (fkey == "u64")
                            {
                                bits = parseInt(fv, what); have = true;
                            }
                            else
                                fail("{}: unknown key '{}' (f64, u64, tol)", what, fkey);
                        }
                        if (!have) fail("{}: needs f64 or u64", what);
                    }
                    else
                    {
                        Lane lane = parseLane(value, f64, isExpect, what);
                        bits = lane.bits; anyNaN = lane.anyNaN;
                    }
                    if (isExpect)
                        out += fmt::format("\trun.expectF64(\"f{0}\", ctx.f{0}, 0x{1:016X}ull, {2}, {3});\n", index, bits, anyNaN, tol);
                    else
                        out += fmt::format("\tctx.f{}.u64 = 0x{:016X}ull;\n", index, bits);
                    break;
                }
                case 'c':
                {
                    CRSpec cr = parseCR(value, what);
                    if (isExpect)
                        out += fmt::format("\trun.expectCR(\"cr{0}\", ctx.cr{0}, {1}, {2}, {3}, {4});\n", index, cr.lt, cr.gt, cr.eq, cr.so);
                    else
                        out += fmt::format("\tctx.cr{0}.lt = {1}; ctx.cr{0}.gt = {2}; ctx.cr{0}.eq = {3}; ctx.cr{0}.so = {4};\n", index, cr.lt, cr.gt, cr.eq, cr.so);
                    break;
                }
                }
                continue;
            }

            if (k == "xer")
            {
                auto t = value.as_table();
                if (t == nullptr) fail("{}: expected {{ so = 0, ov = 0, ca = 0 }}", what);
                uint8_t so = 0, ov = 0, ca = 0;
                for (auto&& [xk, xv] : *t)
                {
                    std::string xkey(xk.str());
                    uint8_t bit = uint8_t(parseInt(xv, what) != 0);
                    if (xkey == "so") so = bit;
                    else if (xkey == "ov") ov = bit;
                    else if (xkey == "ca") ca = bit;
                    else fail("{}: unknown XER field '{}' (so, ov, ca)", what, xkey);
                }
                if (isExpect)
                    out += fmt::format("\trun.expectXER(ctx.xer, {}, {}, {});\n", so, ov, ca);
                else
                    out += fmt::format("\tctx.xer.so = {}; ctx.xer.ov = {}; ctx.xer.ca = {};\n", so, ov, ca);
                continue;
            }

            if (k == "ctr" || k == "lr")
            {
                std::string v = parseIntOrAddr(value, what, hasCallee);
                std::string member = k == "ctr" ? "ctx.ctr.u64" : "ctx.lr";
                if (isExpect)
                    out += fmt::format("\trun.expectU64(\"{}\", {}, {});\n", k, member, v);
                else
                    out += fmt::format("\t{} = {};\n", member, v);
                continue;
            }

            if (k == "mem")
            {
                auto arr = value.as_array();
                if (arr == nullptr) fail("{}: expected an array of {{ addr = .., bytes = .. }}", what);
                for (size_t i = 0; i < arr->size(); i++)
                {
                    auto entry = arr->get(i)->as_table();
                    std::string ewhat = fmt::format("{}[{}]", what, i);
                    if (entry == nullptr) fail("{}: expected {{ addr = .., bytes = .. }}", ewhat);
                    auto addrNode = entry->get("addr");
                    auto bytesNode = entry->get("bytes");
                    if (addrNode == nullptr || bytesNode == nullptr) fail("{}: needs addr and bytes", ewhat);
                    for (auto&& [ek, ev] : *entry)
                        if (ek.str() != "addr" && ek.str() != "bytes") fail("{}: unknown key '{}'", ewhat, ek.str());
                    uint64_t addr = parseInt(*addrNode, ewhat + " addr");
                    std::vector<uint8_t> bytes = parseBytes(*bytesNode, ewhat + " bytes");
                    if (bytes.empty()) fail("{}: bytes is empty", ewhat);
                    if (addr + bytes.size() > kGuestMemorySize) fail("{}: address out of guest memory", ewhat);
                    out += fmt::format("\t{{ static const uint8_t b[] = {}; run.{}(0x{:X}u, b, sizeof(b)); }}\n",
                        bytesLiteral(bytes.data(), bytes.size()), isExpect ? "expectMem" : "writeMem", addr);
                }
                continue;
            }

            fail("{}: unknown register (v0-v127, r0-r31, f0-f31, cr0-cr7, xer, ctr, lr, mem)", what);
        }
        return out;
    }

    // --- Cases ---------------------------------------------------------------

    struct GenCase
    {
        std::string name;
        std::string source;
        std::string mnemonic;
        std::vector<uint32_t> insns;
        std::vector<uint32_t> callee;   // optional second function, laid out after insns (blr implicit)
        bool xfail = false;
        bool analyze = false;           // size the function with Function::Analyze instead of the word count
        std::string setup;
        std::string checks;

        std::string symbol;
        std::string calleeSymbol;
        size_t base = 0;                // guest address of insns[0]
        size_t calleeBase = 0;          // guest address of callee[0], if any
        std::string disasm;
        std::string notImplemented;
        std::string badCase;
    };

    constexpr uint32_t kBlr = 0x4E800020;
    constexpr size_t kImageBase = 0x82000000;

    std::vector<uint32_t> parseWords(const toml::node* n, const char* key)
    {
        auto arr = n ? n->as_array() : nullptr;
        if (arr == nullptr || arr->empty())
            fail("{} must be a non-empty array of 32-bit instruction words", key);
        std::vector<uint32_t> words;
        for (size_t i = 0; i < arr->size(); i++)
        {
            uint64_t w = parseInt(*arr->get(i), fmt::format("{}[{}]", key, i));
            if (w > 0xFFFFFFFFull)
                fail("{}[{}]: 0x{:X} is not a 32-bit word", key, i, w);
            words.push_back(uint32_t(w));
        }
        return words;
    }

    void parseCase(const toml::table& tbl, GenCase& c)
    {
        static const char* kKeys[] = { "name", "source", "insns", "callee", "xfail", "analyze", "mnemonic", "in", "out" };
        for (auto&& [key, value] : tbl)
        {
            bool known = false;
            for (auto k : kKeys) known |= key.str() == k;
            if (!known)
                fail("unknown key '{}' (name, source, insns, callee, xfail, analyze, mnemonic, in, out)", key.str());
        }

        auto name = tbl["name"].value<std::string>();
        if (!name || name->empty())
            fail("missing name");
        c.name = *name;

        auto source = tbl["source"].value<std::string>();
        if (!source || trim(*source).empty())
            fail("missing source (cite the Xenia file/function the expectation comes from)");
        c.source = *source;

        c.mnemonic = tbl["mnemonic"].value_or<std::string>("");

        if (auto xfail = tbl.get("xfail"))
        {
            auto b = xfail->value<bool>();
            if (!b) fail("xfail must be true or false");
            c.xfail = *b;
        }

        if (auto analyze = tbl.get("analyze"))
        {
            auto b = analyze->value<bool>();
            if (!b) fail("analyze must be true or false");
            c.analyze = *b;
        }

        c.insns = parseWords(tbl.get("insns"), "insns");
        if (auto callee = tbl.get("callee"))
            c.callee = parseWords(callee, "callee");

        if (auto in = tbl.get("in"))
        {
            auto t = in->as_table();
            if (t == nullptr) fail("[case.in] must be a table");
            c.setup = emitState(*t, false, !c.callee.empty());
        }

        auto outNode = tbl.get("out");
        auto outTbl = outNode ? outNode->as_table() : nullptr;
        if (outTbl == nullptr) fail("[case.out] is required");
        c.checks = emitState(*outTbl, true, !c.callee.empty());
        if (c.checks.empty()) fail("[case.out] has no expectations");
    }

    void writeOutput(const std::string& path, const std::string& group, const std::string& recompiled, const std::vector<GenCase>& cases)
    {
        std::string out;
        out += fmt::format("// Generated by XenonTestGen from cases/{}.toml. Do not edit.\n", group);
        out += "#include \"harness.h\"\n\n";
        out += "// An unimplemented instruction form emits this; make it a test failure, not a crash.\n";
        out += "#define __builtin_debugtrap() do { XenonTest::trapHit(); return; } while (0)\n\n";
        out += recompiled;
        out += "\nnamespace\n{\n";

        for (size_t i = 0; i < cases.size(); i++)
        {
            const GenCase& c = cases[i];
            out += fmt::format("// {}\n", c.name);
            out += fmt::format("void run_{}(XenonTest::Run& run)\n{{\n", i);
            if (c.symbol.empty())
            {
                out += "\t(void)run;\n";
            }
            else
            {
                out += "\tPPCContext& ctx = run.ctx;\n\t(void)ctx;\n";
                out += fmt::format("\tconstexpr uint64_t kSelf = 0x{:X}ull; (void)kSelf;\n", c.base);
                if (!c.calleeSymbol.empty())
                {
                    out += fmt::format("\tconstexpr uint64_t kCallee = 0x{:X}ull;\n", c.calleeBase);
                    out += fmt::format("\trun.mapFunc(uint32_t(kCallee), __imp__{});\n", c.calleeSymbol);
                }
                out += c.setup;
                out += fmt::format("\trun.call(__imp__{});\n", c.symbol);
                out += c.checks;
            }
            out += "}\n\n";
        }

        out += "const XenonTest::Case kCases[] =\n{\n";
        for (size_t i = 0; i < cases.size(); i++)
        {
            const GenCase& c = cases[i];
            out += fmt::format("\t{{ {}, {}, {}, {}, {}, {}, {}, &run_{} }},\n",
                cstr(group), cstr(c.name), cstr(c.source), cstr(c.disasm),
                c.xfail ? "true" : "false",
                c.notImplemented.empty() ? "nullptr" : cstr(c.notImplemented),
                c.badCase.empty() ? "nullptr" : cstr(c.badCase),
                i);
        }
        out += "};\n\n";
        out += "XenonTest::Registrar kRegistrar(kCases, sizeof(kCases) / sizeof(kCases[0]));\n}\n";

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            fmt::println(stderr, "XenonTestGen: cannot write {}", path);
            exit(1);
        }
        file << out;
    }
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        fmt::println(stderr, "usage: XenonTestGen <cases/group.toml> <out.cpp>");
        return 2;
    }

    const std::string inPath = argv[1];
    const std::string outPath = argv[2];
    const std::string group = std::filesystem::path(inPath).stem().string();
    std::vector<GenCase> cases;

    toml::table root;
    std::string parseError;
#if TOML_EXCEPTIONS
    try
    {
        root = toml::parse_file(inPath);
    }
    catch (const toml::parse_error& e)
    {
        parseError = fmt::format("{}: {} (line {})", inPath, e.description(), e.source().begin.line);
    }
#else
    {
        auto result = toml::parse_file(inPath);
        if (!result)
            parseError = fmt::format("{}: {} (line {})", inPath, result.error().description(), result.error().source().begin.line);
        else
            root = std::move(result).table();
    }
#endif
    if (!parseError.empty())
    {
        GenCase c;
        c.name = "(parse error)";
        c.badCase = parseError;
        cases.push_back(c);
        writeOutput(outPath, group, "", cases);
        fmt::println(stderr, "XenonTestGen: {}", c.badCase);
        return 0;
    }

    auto arr = root["case"].as_array();
    if (arr == nullptr || arr->empty())
    {
        GenCase c;
        c.name = "(no cases)";
        c.badCase = fmt::format("{}: no [[case]] entries", inPath);
        cases.push_back(c);
        writeOutput(outPath, group, "", cases);
        fmt::println(stderr, "XenonTestGen: {}", c.badCase);
        return 0;
    }

    for (size_t i = 0; i < arr->size(); i++)
    {
        GenCase c;
        c.name = fmt::format("case #{}", i + 1);
        try
        {
            auto tbl = arr->get(i)->as_table();
            if (tbl == nullptr)
                fail("[[case]] entry is not a table");
            if (auto name = (*tbl)["name"].value<std::string>())
                c.name = *name;
            parseCase(*tbl, c);
        }
        catch (const CaseError& e)
        {
            c.badCase = e.what();
        }
        cases.push_back(std::move(c));
    }

    // Duplicate names would collide in output and in symbol names.
    for (size_t i = 0; i < cases.size(); i++)
        for (size_t j = 0; j < i; j++)
            if (cases[i].badCase.empty() && cases[i].name == cases[j].name)
                cases[i].badCase = fmt::format("duplicate case name '{}'", cases[i].name);

    // Lay every case's code out back to back in one code section: words + blr,
    // followed by the callee's words + blr when the case has one.
    auto codeBytes = [](const std::vector<uint32_t>& words) { return (words.size() + 1) * 4; };
    size_t codeSize = 0;
    for (auto& c : cases)
    {
        if (!c.badCase.empty())
            continue;
        codeSize += codeBytes(c.insns);
        if (!c.callee.empty())
            codeSize += codeBytes(c.callee);
    }

    Recompiler recompiler;
    recompiler.image.base = kImageBase;
    recompiler.image.size = uint32_t(codeSize);
    recompiler.image.data = std::make_unique<uint8_t[]>(codeSize + 16);
    uint8_t* code = recompiler.image.data.get();
    recompiler.image.Map(".text", 0, uint32_t(codeSize), SectionFlags_Code, code);

    // Writes words + blr at offset.
    auto place = [&](const std::vector<uint32_t>& words, size_t offset)
    {
        for (size_t k = 0; k < words.size(); k++)
        {
            uint32_t be = ByteSwap(words[k]);
            memcpy(code + offset + k * 4, &be, 4);
        }
        uint32_t blr = ByteSwap(kBlr);
        memcpy(code + offset + words.size() * 4, &blr, 4);
    };

    size_t offset = 0;
    for (size_t i = 0; i < cases.size(); i++)
    {
        GenCase& c = cases[i];
        if (!c.badCase.empty())
            continue;

        c.base = kImageBase + offset;
        size_t size = codeBytes(c.insns);
        place(c.insns, offset);
        size_t calleeSize = 0;
        if (!c.callee.empty())
        {
            c.calleeBase = c.base + size;
            calleeSize = codeBytes(c.callee);
            place(c.callee, offset + size);
        }

        // Disassemble up front so bad words are reported with the case, and so
        // the disassembly can be shown alongside failures.
        std::vector<std::string> names;
        auto disassemble = [&](const std::vector<uint32_t>& words, size_t base, const char* key)
        {
            bool isCallee = &words == &c.callee;
            for (size_t k = 0; k < words.size(); k++)
            {
                ppc_insn insn{};
                ppc::Disassemble(code + (base - kImageBase) + k * 4, 4, base + k * 4, insn);
                if (insn.opcode == nullptr)
                {
                    c.badCase = fmt::format("{}[{}]: 0x{:08X} does not disassemble", key, k, words[k]);
                    return;
                }
                c.disasm += fmt::format("{}0x{:08X}  {} {}{}", c.disasm.empty() ? "" : "\n", words[k], insn.opcode->name, insn.op_str, isCallee ? "  (callee)" : "");
                if (!isCallee && k == 0 && !c.mnemonic.empty() && c.mnemonic != insn.opcode->name)
                {
                    c.badCase = fmt::format("mnemonic mismatch: case says '{}' but 0x{:08X} is '{}'", c.mnemonic, words[0], insn.opcode->name);
                    return;
                }
                if (isCallee)
                    continue;
                bool dup = false;
                for (auto& n : names) dup |= n == insn.opcode->name;
                if (!dup) names.push_back(insn.opcode->name);
            }
        };
        disassemble(c.insns, c.base, "insns");
        if (c.badCase.empty() && !c.callee.empty())
            disassemble(c.callee, c.calleeBase, "callee");
        offset += size + calleeSize;
        if (!c.badCase.empty())
            continue;

        c.symbol = fmt::format("vmx_{}_{}", ident(group), ident(c.name));
        recompiler.image.symbols.emplace(c.symbol, c.base, size, Symbol_Function);
        if (!c.callee.empty())
        {
            c.calleeSymbol = c.symbol + "_callee";
            recompiler.image.symbols.emplace(c.calleeSymbol, c.calleeBase, calleeSize, Symbol_Function);
        }

        Function fn{ c.base, size };
        if (c.analyze)
        {
            // What XenonRecomp does for a function without a .pdata entry: the analyzer decides
            // where it ends. Bounded by the case's own words (plus the implicit blr).
            fn = Function::Analyze(code + offset - size - calleeSize, size, c.base);
            c.disasm += fmt::format("\nFunction::Analyze: {} of {} words", fn.size / 4, size / 4);
        }
        size_t before = recompiler.out.size();
        bool recognized = recompiler.Recompile(fn);
        if (recognized && !c.callee.empty())
        {
            Function calleeFn{ c.calleeBase, calleeSize };
            recognized = recompiler.Recompile(calleeFn);
        }
        std::string_view emitted(recompiler.out.data() + before, recompiler.out.size() - before);

        std::string joined;
        for (auto& n : names) joined += (joined.empty() ? "" : ", ") + n;

        if (!recognized)
            c.notImplemented = fmt::format("recompiler does not recognize the instruction ({})", joined);
        else if (emitted.find("__builtin_debugtrap") != std::string_view::npos)
            c.notImplemented = fmt::format("{} emits __builtin_debugtrap() (instruction form not implemented in XenonRecomp)", joined);
    }

    writeOutput(outPath, group, recompiler.out, cases);

    size_t bad = 0, notImpl = 0;
    for (auto& c : cases)
    {
        if (!c.badCase.empty())
        {
            bad++;
            fmt::println(stderr, "XenonTestGen: {}/{}: {}", group, c.name, c.badCase);
        }
        else if (!c.notImplemented.empty())
            notImpl++;
    }
    fmt::println("XenonTestGen: {}: {} cases ({} not implemented, {} bad)", group, cases.size(), notImpl, bad);
    return 0;
}
