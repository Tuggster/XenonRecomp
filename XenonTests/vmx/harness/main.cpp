// Driver for the VMX instruction tests.
//
//   XenonVmxTests                 run every registered case
//   XenonVmxTests --group <stem>  run the cases from cases/<stem>.toml (repeatable)
//   XenonVmxTests --list          list cases without running them
//   XenonVmxTests -v              also print source/disassembly for passing cases
//
// Exit status is nonzero if any case fails, is unexpectedly passing (XPASS), or
// is not implemented in the recompiler and not marked xfail.

#include "harness.h"

#include <cstdio>
#include <cstring>
#include <fmt/core.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

// ppc_context.h declares this; generated code only needs it for indirect calls.
PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{
    enum class Outcome
    {
        Pass,
        Fail,
        XFail,
        XPass,
        NotImplemented,
        BadCase,
    };

    uint8_t* allocateGuestMemory()
    {
#ifdef _WIN32
        return reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        void* p = mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : reinterpret_cast<uint8_t*>(p);
#endif
    }

    void printDetails(const XenonTest::Case& c, const std::vector<std::string>& failures)
    {
        for (auto& f : failures)
            fmt::println("      {}", f);

        std::string disasm = c.disasm;
        for (size_t pos = 0; pos < disasm.size();)
        {
            size_t end = disasm.find('\n', pos);
            if (end == std::string::npos)
                end = disasm.size();
            fmt::println("      insn:   {}", disasm.substr(pos, end - pos));
            pos = end + 1;
        }
        fmt::println("      source: {}", c.source);
    }
}

int main(int argc, char* argv[])
{
    std::vector<std::string> groups;
    bool list = false;
    bool verbose = false;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--group" || arg == "-g")
        {
            if (i + 1 >= argc)
            {
                fmt::println(stderr, "--group needs an argument");
                return 2;
            }
            groups.push_back(argv[++i]);
        }
        else if (arg == "--list")
            list = true;
        else if (arg == "-v" || arg == "--verbose")
            verbose = true;
        else if (arg == "-h" || arg == "--help")
        {
            fmt::println("usage: XenonVmxTests [--group <stem>]... [--list] [-v]");
            return 0;
        }
        else
            groups.push_back(arg);
    }

    auto& all = XenonTest::registry();
    std::vector<const XenonTest::Case*> selected;
    for (auto* c : all)
    {
        if (groups.empty())
            selected.push_back(c);
        else
        {
            for (auto& g : groups)
            {
                if (g == c->group)
                {
                    selected.push_back(c);
                    break;
                }
            }
        }
    }

    if (selected.empty())
    {
        // Every generated group registers at least one case, so this is either a
        // typo or a stale ctest entry for a case file that was removed.
        fmt::println("WARNING: no cases selected ({} registered); nothing to run", all.size());
        return 0;
    }

    if (list)
    {
        for (auto* c : selected)
        {
            fmt::println("{}/{}{}{}", c->group, c->name,
                c->xfail ? " [xfail]" : "",
                c->notImplemented ? " [not implemented]" : c->badCase ? " [bad case]" : "");
            if (verbose)
                printDetails(*c, {});
        }
        return 0;
    }

    uint8_t* base = allocateGuestMemory();
    if (base == nullptr)
    {
        fmt::println(stderr, "failed to reserve {} bytes of guest memory", PPC_MEMORY_SIZE);
        return 2;
    }

    PPCFPSCRRegister hostFpscr{};
    hostFpscr.loadFromHost();

    size_t counts[6] = {};
    bool anyFailure = false;

    for (auto* c : selected)
    {
        Outcome outcome;
        std::vector<std::string> failures;

        if (c->badCase != nullptr)
        {
            outcome = Outcome::BadCase;
            failures.push_back(c->badCase);
        }
        else if (c->notImplemented != nullptr)
        {
            outcome = c->xfail ? Outcome::XFail : Outcome::NotImplemented;
            failures.push_back(c->notImplemented);
        }
        else
        {
            PPCContext ctx{};
            ctx.fpscr.loadFromHost();

            XenonTest::Run run{ ctx, base };
            c->run(run);

            hostFpscr.setcsr(hostFpscr.csr);

            failures = std::move(run.failures);
            bool passed = failures.empty();
            if (c->xfail)
                outcome = passed ? Outcome::XPass : Outcome::XFail;
            else
                outcome = passed ? Outcome::Pass : Outcome::Fail;
        }

        counts[size_t(outcome)]++;

        const char* label = nullptr;
        bool failed = false;
        switch (outcome)
        {
        case Outcome::Pass:           label = "PASS           "; break;
        case Outcome::Fail:           label = "FAIL           "; failed = true; break;
        case Outcome::XFail:          label = "XFAIL          "; break;
        case Outcome::XPass:          label = "XPASS          "; failed = true; break;
        case Outcome::NotImplemented: label = "NOT IMPLEMENTED"; failed = true; break;
        case Outcome::BadCase:        label = "BAD CASE       "; failed = true; break;
        }
        anyFailure |= failed;

        fmt::println("{} {}/{}", label, c->group, c->name);
        if (outcome == Outcome::XPass)
            fmt::println("      case is marked xfail but passed: remove `xfail = true`");
        if (failed || outcome == Outcome::XFail || verbose)
            printDetails(*c, failures);
    }

    fmt::println("");
    fmt::println("{} cases: {} passed, {} failed, {} xfail, {} xpass, {} not implemented, {} bad",
        selected.size(),
        counts[size_t(Outcome::Pass)], counts[size_t(Outcome::Fail)],
        counts[size_t(Outcome::XFail)], counts[size_t(Outcome::XPass)],
        counts[size_t(Outcome::NotImplemented)], counts[size_t(Outcome::BadCase)]);

    return anyFailure ? 1 : 0;
}
