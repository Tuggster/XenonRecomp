# VMX instruction tests

Instruction-level tests for XenonRecomp that need no PPC toolchain. Each case
gives raw 32-bit instruction words, an initial register state and the expected
state afterwards. At build time `XenonTestGen` feeds the words (plus an implicit
`blr`) through the real `Recompiler::Recompile` path, so the C++ under test is
exactly what the game gets, and `XenonVmxTests` runs it on a `PPCContext`.

Expectations are derived from Xenia (the semantic reference): cite the file and
function in the case's `source` field. Xenia paths below are repo-relative.

## Run

From the parent repo root (the normal `build-tools` configure):

```sh
cmake -S tools -B build-tools -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ctest --test-dir build-tools/XenonRecomp --output-on-failure
```

`ctest` builds the suite itself (the `vmx.build` fixture), so `cmake --build
build-tools` never compiles the tests and a broken case can't break the normal
XenonRecomp/XenonAnalyse build. `--test-dir` points at the XenonRecomp subdir
because `enable_testing()` lives in XenonRecomp's CMakeLists, not the parent's.

Useful variants:

```sh
ctest --test-dir build-tools/XenonRecomp -R vmx.all           # every case, one process
ctest --test-dir build-tools/XenonRecomp -R vmx.vupkd3d128    # one group (cases/vupkd3d128.toml)
build-tools/XenonRecomp/XenonTests/vmx/XenonVmxTests --group vupkd3d128 -v   # run the binary directly
build-tools/XenonRecomp/XenonTests/vmx/XenonVmxTests --list
```

ctest reads its test list at configure time. A case file you just added is
compiled in by `vmx.build` (the glob is `CONFIGURE_DEPENDS`) and runs under
`vmx.all` immediately; its own `vmx.<group>` entry appears on the next ctest run.

## Add cases

Create `cases/<group>.toml`. Nothing else needs editing; the file is globbed.
Use a group name nobody else will use (the opcode name is the convention:
`vupkd3d128.toml`). One `[[case]]` per test:

```toml
[[case]]
name = "xenia_test_vupkd3d128_short2_0"      # unique within the file; becomes part of a C symbol
mnemonic = "vupkd3d128"                       # optional: insns[0] must disassemble to this
source = "xenia src/xenia/cpu/ppc/testing/instr_vupkd3d128.s test_vupkd3d128_short2_0; src/xenia/cpu/backend/x64/x64_seq_vector.cc UNPACK EmitSHORT_2"
insns = [0x18641FF0]                          # vupkd3d128 v3,v3,4 (one or more words, blr is implicit)
[case.in]
v3 = { u32 = [0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD, 0x7FFF8001] }
[case.out]
v3 = { u32 = [0x40407FFF, 0x403F8001, 0x00000000, 0x3F800000] }
```

Fields:

| Key | Meaning |
|-|-|
| `name` | Required. Unique in the file. |
| `source` | Required, free text. Which Xenia file/function the expectation comes from. |
| `insns` | Required. Big-endian instruction words as written (ints or `"0x..."` strings). |
| `callee` | Optional. Words of a second function (`blr` implicit), laid out right after the case's code and mapped for indirect calls (`bctrl`, `blrl`) at the address `"callee"`. See `cases/blrl.toml`. |
| `mnemonic` | Optional guard: the first word must decode to this name (as in `thirdparty/disasm/ppc-dis.c`, e.g. `"vcmpbfp128."` for the record form). |
| `xfail` | Optional. `true` marks a known failure; see below. |
| `[case.in]` | Initial state. Everything not listed is zero. |
| `[case.out]` | Expected state after execution; only listed registers are checked. Required, non-empty. |

Registers in `in`/`out`:

| Key | Value |
|-|-|
| `v0`..`v127` | `{ u32 = [e0, e1, e2, e3] }`; lane types `u8 s8 u16 s16 u32 s32 u64 s64 f32 f64`, exactly 16 bytes' worth. Ints or hex strings; floats, `"inf"`, `"-inf"`, `"nan"`, or hex-string raw bits for `f32`/`f64`. In `out`: `"*"` ignores a lane, `"nan"` accepts any NaN, `tol = 1e-6` switches f32/f64 lanes to absolute tolerance. |
| `r0`..`r31`, `ctr`, `lr` | 64-bit int or hex string, or a code address: `"self"` (the case's first word), `"callee"` (the callee's first word), optionally `"self + 4"` / `"callee - 8"`. An indirect call to an address nothing is mapped at fails the case. |
| `f0`..`f31` | Float (f64), hex string for raw bits, or `{ f64 = .., tol = .. }` / `{ u64 = ".." }`. |
| `cr0`..`cr7` | `{ lt = 0, gt = 0, eq = 1, so = 0 }`, a flag string `"eq"` / `"lt|so"` / `""`, or an int 0..15 (lt=8 gt=4 eq=2 so=1). All four bits are checked. |
| `xer` | `{ so = 0, ov = 0, ca = 0 }` |
| `mem` | `[ { addr = 0x10000, bytes = [0x12, 0x34] }, { addr = .., bytes = "12 34" } ]`. 4 GiB of zeroed guest memory is mapped. |

**Lane order.** Vector lanes are written in PPC element order, element 0 first,
the same order as Xenia's `#_ REGISTER_IN v3 [a, b, c, d]` test annotations and
its `vec128i(x, y, z, w)` constants. The harness maps element `i` to
`PPCVRegister::u32[3 - i]` (XenonRecomp stores vectors byte-reversed; likewise
`u16[7 - i]`, `u8[15 - i]`). Failure messages print vectors in element order.

Floating point runs with denormals flushed to zero, as the recompiled game code
does (`fpscr.enableFlushMode()` is emitted before VMX float ops). Avoid
denormal inputs unless that is what you're testing. NaN payload propagation
differs between x86 and arm64; use `"nan"` for NaN results.

### Failure modes

`XenonVmxTests` exits nonzero if any case is `FAIL`, `XPASS`, `NOT IMPLEMENTED`
(without `xfail`) or `BAD CASE`. The last two never break the build: the
generator turns its own errors into failing cases so one group can't block
another.

```
NOT IMPLEMENTED vcmpbfp128/all_in_bounds_sets_cr6_eq
      vcmpbfp128. emits __builtin_debugtrap() (instruction form not implemented in XenonRecomp)
      insn:   0x180111C0  vcmpbfp128. v0,v1,v2
      source: xenia src/xenia/cpu/ppc/ppc_emit_altivec.cc InstrEmit_vcmpbfp_ ...
FAIL            vupkd3d128/short2
      v3: expected [40407FFF 403F8001 00000000 3F800000] actual [40407FFF 403F8000 00000000 3F800000]
      as f32: expected [3.0000076 2.9999924 0 1] actual [...]
BAD CASE        vupkd3d128/typo
      [case.out] q3: unknown register (v0-v127, r0-r31, f0-f31, cr0-cr7, xer, ctr, lr, mem)
```

`NOT IMPLEMENTED` is detected statically: the generator looks for
`__builtin_debugtrap` in what the recompiler emitted for the case (or the
recompiler rejecting the instruction). As a backstop, generated code also
redefines `__builtin_debugtrap()` to record the trap and return, so a trap on a
path the generator didn't see is reported as a failure, not a crash.

`xfail = true` inverts the result: a failing or not-implemented case reports
`XFAIL` and passes; a passing one reports `XPASS` and fails, so the flag has to
be removed when the implementation lands. Put xfail cases for an opcode in that
opcode's own file.

## Instruction words

There is no assembler here, and VMX128 is not in binutils anyway. Compute words
with `encode.py`, which reads opcode/form/operand order from XenonRecomp's
disassembler table so the word is guaranteed to decode back to the mnemonic:

```sh
python3 XenonTests/vmx/encode.py 'vupkd3d128 v3,v3,4'     # 0x18641FF0
python3 XenonTests/vmx/encode.py 'vpkd3d128 v4,v3,3,1,3'  # vd, vb, type, pack, shift
python3 XenonTests/vmx/encode.py 'vcmpbfp128. v0,v1,v2'   # record form is a separate mnemonic
python3 XenonTests/vmx/encode.py --list
```

The generator prints the disassembly of every word into the generated C++ and
into failure output, and `mnemonic = ".."` makes a wrong word a `BAD CASE`.
Xenia's hand-coded test words in `src/xenia/cpu/ppc/testing/instr_vupkd3d128.s`
and `instr_vpkd3d128.s` are another source of known-good words.

### VMX128 field layouts

Bit numbers below are little-endian (bit 0 = LSB of the word). The primary
opcode is bits 26..31; the extended opcode (`xop`) occupies the low 11 bits,
masked per form. The same layouts are in:

- XenonRecomp: `thirdparty/disasm/ppc-dis.c`, `VX128*` macros near "Macros used
  to form opcodes" and `insert_vds128`/`insert_va128`/`insert_vb128`/
  `insert_vperm`; the `VD3D0`/`VD3D1`/`VD3D2`/`VC128`/`UIMM` entries of the
  operand table; the opcode table entries flagged `PPCVEC128`.
- Xenia: `src/xenia/cpu/ppc/ppc_instr.h` (`VX128`, `VX128_1` .. `VX128_5`,
  `VX128_P`, `VX128_R` bitfield structs, written LSB first), the `VX128_*`
  accessor macros at the top of `src/xenia/cpu/ppc/ppc_emit_altivec.cc`, and
  the decoder switch in `src/xenia/cpu/ppc/ppc_opcode_lookup_gen.cc`.

Split register fields (7-bit register numbers):

| Field | Encoding |
|-|-|
| `VD128` / `VS128` | `(vd & 0x1F) << 21 \| (vd & 0x60) >> 3` (low 5 bits at 21..25, high 2 bits at 2..3) |
| `VA128` | `(va & 0x1F) << 16 \| (va & 0x20) \| (va & 0x40) << 4` (bits 16..20, bit 5, bit 10) |
| `VB128` | `(vb & 0x1F) << 11 \| (vb & 0x60) >> 5` (bits 11..15, bits 0..1) |

Forms (xop mask is what `VX128_x(op, xop)` keeps; the other low bits are operands):

| Form | xop mask | Operands besides VD128 | Examples |
|-|-|-|-|
| `VX128` | `0x3D0` | VA128, VB128 | `vaddfp128` (op 5, xop 16), `vcmpbfp128` (op 6, xop 384, Rc bit 6: xop 448 = `vcmpbfp128.`) |
| `VX128_1` | `0x7F3` | RA (16..20), RB (11..15) | `lvx128`, `stvx128` |
| `VX128_2` | `0x210` | VA128, VB128, VC (3 bits at 6..8) | `vsel128`, `vmsum3fp128` |
| `VX128_3` | `0x7F0` | VB128, UIMM (5 bits at 16..20) | `vupkd3d128` (op 6, xop 2032; type = UIMM >> 2), `vspltw128`, `vcfpsxws128` |
| `VX128_4` | `0x730` | VB128, VD3D0 (3 bits at 18..20), VD3D1 (2 bits at 16..17), VD3D2 (2 bits at 6..7) | `vpkd3d128` (op 6, xop 1552): type, pack, shift |
| `VX128_5` | `0x010` | VA128, VB128, SH (4 bits at 6..9) | `vsldoi128` |
| `VX128_P` | `0x630` | VB128, VPERM (`(perm & 0xE0) << 1 \| (perm & 0x1F) << 16`) | `vpermwi128`, `vrlimi128` |
| `VX128_R` | `0x3D0` | VA128, VB128, Rc (bit 6) | Xenia's name for the compare forms; the disassembler lists them as `VX128` with the Rc bit folded into xop |

## Layout

```
XenonTests/vmx/
  cases/*.toml          one group per file, globbed (CONFIGURE_DEPENDS)
  gen/XenonTestGen.cpp  generator; links LibXenonRecomp, writes gen/<group>.cpp into the build dir
  harness/              harness.h (PPCContext config + expectations), harness.cpp, main.cpp (driver)
  encode.py             instruction word calculator
```

`LibXenonRecomp` is the recompiler compiled as a static library
(`XenonRecomp/CMakeLists.txt`); the `XenonRecomp` executable is `main.cpp`
linked against it. The tests are registered only when `XENON_RECOMP_VMX_TESTS`
is on (default), from XenonRecomp's top-level CMakeLists, so they work both
standalone and as a subdirectory of the parent repo's `tools/` project.
