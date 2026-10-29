#!/usr/bin/env python3
"""Encode a VMX128 (or VMX) instruction into its 32-bit word.

    encode.py 'vupkd3d128 v3,v3,4'      -> 0x18641FF0
    encode.py vaddfp128 v0 v3 v4        -> 0x14032010
    encode.py vpkd3d128 v4,v3,3,1,3
    encode.py --list                    list mnemonics and operand order

Opcodes, forms and operand order are read from XenonRecomp's disassembler table
(thirdparty/disasm/ppc-dis.c), so the word is guaranteed to disassemble to the
mnemonic you asked for. Operand field layouts follow the insert_* functions in
the same file; they match Xenia's src/xenia/cpu/ppc/ppc_instr.h VX128* structs.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PPC_DIS = os.path.join(HERE, "..", "..", "thirdparty", "disasm", "ppc-dis.c")

FORM_MASK = {
    "VX": 0x7FF,
    "VXA": 0x03F,
    "VXR": 0x7FF,  # rc is part of xop in VXR entries
    "VX128": 0x3D0,
    "VX128_1": 0x7F3,
    "VX128_2": 0x210,
    "VX128_3": 0x7F0,
    "VX128_4": 0x730,
    "VX128_5": 0x010,
    "VX128_P": 0x630,
}

# Operand name -> (encoder, is_register, bits)
def _vd128(v): return ((v & 0x1F) << 21) | ((v & 0x60) >> 3)
def _va128(v): return ((v & 0x1F) << 16) | (v & 0x20) | ((v & 0x40) << 4)
def _vb128(v): return ((v & 0x1F) << 11) | ((v & 0x60) >> 5)
def _vperm(v): return ((v & 0xE0) << 1) | ((v & 0x1F) << 16)

OPERANDS = {
    "VD128": (_vd128, True, 7), "VS128": (_vd128, True, 7),
    "VA128": (_va128, True, 7),
    "VB128": (_vb128, True, 7),
    "VC128": (lambda v: (v & 0x7) << 6, True, 3),
    "VPERM128": (_vperm, False, 8),
    "VD3D0": (lambda v: (v & 0x7) << 18, False, 3),
    "VD3D1": (lambda v: (v & 0x3) << 16, False, 2),
    "VD3D2": (lambda v: (v & 0x3) << 6, False, 2),
    "UIMM": (lambda v: (v & 0x1F) << 16, False, 5),
    "SIMM": (lambda v: (v & 0x1F) << 16, False, 5),
    "SHB": (lambda v: (v & 0xF) << 6, False, 4),
    "RA": (lambda v: (v & 0x1F) << 16, True, 5),
    "RA0": (lambda v: (v & 0x1F) << 16, True, 5),
    "RB": (lambda v: (v & 0x1F) << 11, True, 5),
    "VD": (lambda v: (v & 0x1F) << 21, True, 5), "VS": (lambda v: (v & 0x1F) << 21, True, 5),
    "VA": (lambda v: (v & 0x1F) << 16, True, 5),
    "VB": (lambda v: (v & 0x1F) << 11, True, 5),
    "VC": (lambda v: (v & 0x1F) << 6, True, 5),
}

ENTRY = re.compile(
    r'\{\s*"([\w.]+)",\s*(VX128(?:_\d|_P)?|VXA|VXR|VX)\s*\(\s*(\d+),\s*(\d+)\s*\),'
    r'\s*\w+,\s*(PPCVEC128|PPCVEC),\s*\{([^}]*)\}')


def load_table():
    table = {}
    with open(PPC_DIS) as f:
        for line in f:
            m = ENTRY.search(line)
            if not m:
                continue
            name, form, op, xop, _, ops = m.groups()
            ops = [o.strip() for o in ops.split(",") if o.strip()]
            table.setdefault(name, (form, int(op), int(xop), ops))
    return table


def encode(mnemonic, args, table):
    if mnemonic not in table:
        raise SystemExit(f"unknown mnemonic {mnemonic!r} (try --list)")
    form, op, xop, ops = table[mnemonic]
    if form not in FORM_MASK:
        raise SystemExit(f"{mnemonic}: form {form} not supported by this script")
    if len(args) != len(ops):
        raise SystemExit(f"{mnemonic} takes {len(ops)} operands {ops}, got {len(args)}")
    word = (op << 26) | (xop & FORM_MASK[form])
    for opname, arg in zip(ops, args):
        if opname not in OPERANDS:
            raise SystemExit(f"{mnemonic}: operand {opname} not supported by this script")
        enc, is_reg, bits = OPERANDS[opname]
        text = arg.strip().lower()
        if is_reg and text[:1] in "vr" and text[1:].isdigit():
            text = text[1:]
        value = int(text, 0)
        if value < 0:
            value &= (1 << bits) - 1
        if value >= (1 << bits):
            raise SystemExit(f"{mnemonic}: {opname} value {arg} does not fit in {bits} bits")
        word |= enc(value)
    return word


def main(argv):
    table = load_table()
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "--list":
        for name in sorted(table):
            form, op, xop, ops = table[name]
            if form.startswith("VX128"):
                print(f"{name:16} {form:8} op={op:<2} xop={xop:<5} {' '.join(ops)}")
        return 0
    text = " ".join(argv).replace(",", " ").split()
    word = encode(text[0], text[1:], table)
    print(f"0x{word:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
