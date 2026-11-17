#!/usr/bin/env python3
"""Static recompiler: Gekko (PowerPC 750CL) machine code in main.dol -> C.

Reads the original main.dol and the function/jump-table symbols that decomp-toolkit produced
(config/GF7E01/symbols.txt), and writes one C function per guest function:

    void f_XXXXXXXX(Ctx *c);   // XXXXXXXX = original address

Everything is translated except:
  - functions named in recomp/runtime/hle_overrides.txt, which the runtime reimplements (HLE), and
  - the MetroTRK debugger, which becomes logging stubs (hle_stubs.c) unless overridden.
Dolphin SDK functions get an RT_ENTER_SDK hook so the runtime can trace calls into the SDK.

Usage: python recomp/tools/ppcrecomp.py [--out build/GF7E01/recomp]
"""
import argparse
import os
import re
import struct
import zlib
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
VERSION = "GF7E01"

SDK_RANGE = (0x8012D438, 0x8015CA04)  # Dolphin SDK: recompiled, but traced
STUB_RANGES = [(0x801E8D48, 0x801EEFA0, "MetroTRK")]
HLE_LIST = os.path.join(ROOT, "recomp", "runtime", "hle_overrides.txt")
C_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
FUNCS_PER_FILE = 64

# --------------------------------------------------------------------------------------------- input

def load_dol(path):
    b = open(path, "rb").read()
    u = lambda o: struct.unpack_from(">I", b, o)[0]
    segs = [(u(0x48 + i * 4), b[u(i * 4): u(i * 4) + u(0x90 + i * 4)], i < 7)
            for i in range(18) if u(0x90 + i * 4)]
    entry = u(0xE0)
    bss = (u(0xD8), u(0xDC))
    return segs, entry, bss


class Image:
    def __init__(self, segs):
        self.segs = segs

    def word(self, addr):
        for a, data, _ in self.segs:
            if a <= addr < a + len(data) - 3:
                return struct.unpack_from(">I", data, addr - a)[0]
        return None


SYM = re.compile(r"^(\S+) = \.(\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")


def load_symbols(path):
    funcs, tables = [], []
    for line in open(path, encoding="utf-8"):
        m = SYM.match(line)
        if not m:
            continue
        name, sec, addr, kind, size = m.group(1), m.group(2), int(m.group(3), 16), m.group(4), int(m.group(5) or "0", 16)
        if kind == "function" and size:
            funcs.append((addr, size, name, sec))
        elif kind == "object" and name.startswith("jumptable_"):
            tables.append((addr, size))
    funcs.sort()
    return funcs, tables

# -------------------------------------------------------------------------------------------- helpers

def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


def hx(v):
    if isinstance(v, str):  # already a C expression (a relocated value in a REL module)
        return v
    return f"0x{v & 0xFFFFFFFF:X}u"


def R(n):
    return f"c->r[{n}]"


def RA0(n):
    return "0u" if n == 0 else R(n)


def F(n):
    return f"c->f[{n}]"


def mask(mb, me):
    m, i = 0, mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


def ea_d(ra, d):
    return f"({RA0(ra)} + {hx(d)})"


def ea_x(ra, rb):
    return f"({RA0(ra)} + {R(rb)})"

# -------------------------------------------------------------------------------------------- emitter

class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, ctx, start, size, name):
        self.ctx, self.start, self.end, self.name = ctx, start, start + size, name
        self.labels = set()
        found = {t for t in ctx.jt_targets if start <= t < self.end} | self.find_jump_tables()
        self.jt_targets = sorted(found)
        self.labels.update(self.jt_targets)
        self.unknown = 0

    def find_jump_tables(self):
        """Find `lis rT,hi; addi rT,rT,lo; ... lwzx rX,rT,rI; mtctr rX; bctr` and read the table."""
        img, targets = self.ctx.img, set()
        for pc in range(self.start, self.end, 4):
            if img.word(pc) != 0x4E800420:  # bctr
                continue
            base_reg, table = None, None
            for back in range(pc - 4, max(self.start, pc - 64) - 4, -4):
                w = img.word(back)
                op = w >> 26
                if base_reg is None and op == 31 and (w >> 1) & 0x3FF == 23:  # lwzx
                    base_reg = (w >> 16) & 31
                elif base_reg is not None and op == 14 and (w >> 21) & 31 == base_reg:  # addi base, x, lo
                    rel = self.ctx.relocs.get(back) if self.ctx.relocs else None
                    if rel is not None:  # REL module: the table is `section + addend`, entries are ADDR32 relocs
                        for i in range(1024):
                            e = self.ctx.data_relocs.get((rel.sec, rel.addend + i * 4))
                            t = e.branch_target() if e is not None and e.is_code() else None
                            if t is None or not (self.start <= t < self.end):
                                break
                            targets.add(t)
                        self.ctx.found_tables += 1
                        break
                    lo, src = sx16(w & 0xFFFF), (w >> 16) & 31
                    for back2 in range(back - 4, max(self.start, back - 64) - 4, -4):
                        w2 = img.word(back2)
                        if w2 >> 26 == 15 and (w2 >> 21) & 31 == src and (w2 >> 16) & 31 == 0:  # lis
                            table = (((w2 & 0xFFFF) << 16) + lo) & 0xFFFFFFFF
                            break
                    break
            if table is None:
                continue
            for i in range(1024):
                t = img.word(table + i * 4)
                if t is None or not (self.start <= t < self.end) or t & 3:
                    break
                targets.add(t)
            self.ctx.found_tables += 1
        return targets

    # ---- control flow
    def inside(self, t):
        return self.start <= t < self.end

    def jump(self, t, pc=None):
        if self.inside(t):
            self.labels.add(t)
            if pc is not None and t <= pc:  # loop back-edge: let the runtime deliver interrupts
                return f"{{ RT_SAFEPOINT(c); goto L_{t:08X}; }}"
            return f"goto L_{t:08X};"
        if t in self.ctx.func_starts:
            return f"{{ f_{t:08X}(c); return; }}"
        return f"{{ rt_jump(c, {hx(t)}); return; }}"

    def call(self, t, pc):
        if t == pc + 4:  # bl/bcl to the next instruction: just reads the PC into LR
            return f"c->lr = {hx(pc + 4)};"
        if t in self.ctx.func_starts:
            return f"c->lr = {hx(pc + 4)}; f_{t:08X}(c);"
        return f"c->lr = {hx(pc + 4)}; rt_call(c, {hx(t)});"

    def ctr_dispatch(self):
        if not self.jt_targets:
            return "{ rt_jump(c, c->ctr); return; }"
        cases = " ".join(f"case {hx(t)}: goto L_{t:08X};" for t in self.jt_targets)
        sel = "c->ctr"
        m = self.ctx.module
        if m is not None:  # CTR holds a runtime address in the loaded module: map it to our addresses
            sel = f"(c->ctr - g_rel[{m.index}].sec[{m.text_sec}] + {hx(m.vbase)})"
        return f"switch ({sel}) {{ {cases} default: rt_jump(c, c->ctr); return; }}"

    @staticmethod
    def bc_cond(bo, bi):
        pre, conds = "", []
        if not bo & 4:
            pre = "c->ctr--; "
            conds.append("c->ctr == 0" if bo & 2 else "c->ctr != 0")
        if not bo & 16:
            conds.append(f"cr_bit(c, {bi})" if bo & 8 else f"!cr_bit(c, {bi})")
        return pre, " && ".join(f"({x})" for x in conds) if conds else None

    def branch_cond(self, pre, cond, action):
        if cond is None:
            return pre + action
        return f"{pre}if ({cond}) {action}"

    # ---- relocations (REL modules only)
    def reloc_imm_insn(self, w, pc, rel):
        """An instruction whose 16-bit immediate is patched by OSLink: use the runtime value."""
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        addr = rel.expr()
        field = {4: f"({addr} & 0xFFFFu)", 5: f"({addr} >> 16)", 6: f"((({addr}) + 0x8000u) >> 16)"}[rel.type]
        S, U, UH = f"(uint32_t)(int32_t)(int16_t){field}", field, f"({field} << 16)"
        if op == 14:
            return f"{R(rd)} = {RA0(ra)} + {S};"
        if op == 15:
            return f"{R(rd)} = {RA0(ra)} + {UH};"
        if op in (24, 25, 26, 27, 28, 29):
            opr = "|" if op in (24, 25) else "^" if op in (26, 27) else "&"
            val = U if op in (24, 26, 28) else UH
            return f"{R(ra)} = {R(rd)} {opr} {val};" + (f" RC0(c, {R(ra)});" if op in (28, 29) else "")
        if 32 <= op <= 55:
            return self.loadstore_d(op, rd, ra, S)
        raise Unsupported(f"relocated immediate on opcode {op}")

    def reloc_branch_target(self, pc, w):
        rel = self.ctx.relocs.get(pc) if self.ctx.relocs else None
        if rel and rel.type in (10, 11):
            return rel.branch_target()
        return None

    # ---- instruction translation
    def insn(self, w, pc):
        op = w >> 26
        rd, ra, rb, rc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        uimm, simm, Rc = w & 0xFFFF, sx16(w & 0xFFFF), w & 1
        rc0 = lambda reg: f" RC0(c, {reg});" if Rc else ""
        rel = self.ctx.relocs.get(pc) if self.ctx.relocs else None
        if rel and rel.type in (4, 5, 6):
            return self.reloc_imm_insn(w, pc, rel)

        if op == 3:  # twi
            return self.trap(rd, R(ra), hx(simm), pc)
        if op == 4:
            return self.paired(w, pc)
        if op == 7:
            return f"{R(rd)} = {R(ra)} * {hx(simm)};"
        if op == 8:  # subfic
            return (f"{{ uint64_t t = (uint64_t)(uint32_t)~{R(ra)} + {hx(simm)} + 1; "
                    f"c->xer_ca = (uint32_t)(t >> 32); {R(rd)} = (uint32_t)t; }}")
        if op == 10:
            return f"cmp_u(c, {rd >> 2}, {R(ra)}, {hx(uimm)});"
        if op == 11:
            return f"cmp_s(c, {rd >> 2}, (int32_t){R(ra)}, (int32_t){hx(simm)});"
        if op in (12, 13):  # addic, addic.
            s = (f"{{ uint64_t t = (uint64_t){R(ra)} + {hx(simm)}; c->xer_ca = (uint32_t)(t >> 32); "
                 f"{R(rd)} = (uint32_t)t; }}")
            return s + (f" RC0(c, {R(rd)});" if op == 13 else "")
        if op == 14:
            return f"{R(rd)} = {RA0(ra)} + {hx(simm)};"
        if op == 15:
            return f"{R(rd)} = {RA0(ra)} + {hx(uimm << 16)};"
        if op == 16:  # bc
            bo, bi = rd, ra
            bd = sx16(w & 0xFFFC)
            t = (bd if w & 2 else pc + bd) & 0xFFFFFFFF
            rt = self.reloc_branch_target(pc, w)
            if rt is not None:
                t = rt
            pre, cond = self.bc_cond(bo, bi)
            action = ("{ " + self.call(t, pc) + " }") if w & 1 else self.jump(t, pc)
            return self.branch_cond(pre, cond, action)
        if op == 17:
            return f"rt_syscall(c, {hx(pc)});"
        if op == 18:  # b
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (li if w & 2 else pc + li) & 0xFFFFFFFF
            rt = self.reloc_branch_target(pc, w)
            if rt is not None:
                t = rt
            return self.call(t, pc) if w & 1 else self.jump(t, pc)
        if op == 19:
            return self.op19(w, pc)
        if op == 20:  # rlwimi
            sh, mb, me = rb, rc, (w >> 1) & 31
            m = mask(mb, me)
            return f"{R(ra)} = (rotl32({R(rd)}, {sh}) & {hx(m)}) | ({R(ra)} & {hx(~m)});" + rc0(R(ra))
        if op == 21:  # rlwinm
            sh, mb, me = rb, rc, (w >> 1) & 31
            return f"{R(ra)} = rotl32({R(rd)}, {sh}) & {hx(mask(mb, me))};" + rc0(R(ra))
        if op == 23:  # rlwnm
            mb, me = rc, (w >> 1) & 31
            return f"{R(ra)} = rotl32({R(rd)}, {R(rb)} & 31) & {hx(mask(mb, me))};" + rc0(R(ra))
        if op == 24:
            return f"{R(ra)} = {R(rd)} | {hx(uimm)};"
        if op == 25:
            return f"{R(ra)} = {R(rd)} | {hx(uimm << 16)};"
        if op == 26:
            return f"{R(ra)} = {R(rd)} ^ {hx(uimm)};"
        if op == 27:
            return f"{R(ra)} = {R(rd)} ^ {hx(uimm << 16)};"
        if op == 28:
            return f"{R(ra)} = {R(rd)} & {hx(uimm)}; RC0(c, {R(ra)});"
        if op == 29:
            return f"{R(ra)} = {R(rd)} & {hx(uimm << 16)}; RC0(c, {R(ra)});"
        if op == 31:
            return self.op31(w, pc)
        if 32 <= op <= 55:
            return self.loadstore_d(op, rd, ra, simm)
        if op in (56, 57, 60, 61):  # psq_l, psq_lu, psq_st, psq_stu
            d = w & 0xFFF
            if d & 0x800:
                d -= 0x1000
            W, I = (w >> 15) & 1, (w >> 12) & 7
            ea = ea_d(ra, d)
            fn = "psq_l" if op in (56, 57) else "psq_st"
            if op in (57, 61):
                return f"{{ uint32_t ea = {R(ra)} + {hx(d)}; {fn}(c, {rd}, ea, {W}, {I}); {R(ra)} = ea; }}"
            return f"{fn}(c, {rd}, {ea}, {W}, {I});"
        if op == 59:
            return self.fp_single(w)
        if op == 63:
            return self.fp_double(w, pc)
        raise Unsupported(f"opcode {op}")

    def trap(self, to, a, b, pc):
        if to == 31:
            return f"rt_trap(c, {hx(pc)});"
        conds = []
        if to & 16: conds.append(f"(int32_t){a} < (int32_t){b}")
        if to & 8: conds.append(f"(int32_t){a} > (int32_t){b}")
        if to & 4: conds.append(f"{a} == {b}")
        if to & 2: conds.append(f"{a} < {b}")
        if to & 1: conds.append(f"{a} > {b}")
        if not conds:
            return ";"
        return f"if ({' || '.join(conds)}) rt_trap(c, {hx(pc)});"

    def op19(self, w, pc):
        xo = (w >> 1) & 0x3FF
        bo, bi = (w >> 21) & 31, (w >> 16) & 31
        d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if xo == 16:  # bclr
            pre, cond = self.bc_cond(bo, bi)
            if w & 1:
                act = f"{{ uint32_t t = c->lr; c->lr = {hx(pc + 4)}; rt_call(c, t); }}"
            else:
                act = "return;"
            return self.branch_cond(pre, cond, act)
        if xo == 528:  # bcctr
            pre, cond = self.bc_cond(bo | 4, bi)  # bcctr never decrements CTR
            if w & 1:
                act = f"{{ uint32_t t = c->ctr; c->lr = {hx(pc + 4)}; rt_call(c, t); }}"
            else:
                act = self.ctr_dispatch()
            return self.branch_cond(pre, cond, act)
        if xo == 0:
            return f"cr_set(c, {d >> 2}, (c->cr >> {28 - (a >> 2) * 4}) & 0xF);"
        if xo == 150:
            return ";  /* isync */"
        if xo == 50:
            return f"rt_illegal(c, {hx(pc)}, {hx(w)}); /* rfi */"
        ops = {257: "{A} & {B}", 129: "{A} & !{B}", 289: "!({A} ^ {B})", 225: "!({A} & {B})",
               33: "!({A} | {B})", 449: "{A} | {B}", 417: "{A} | !{B}", 193: "{A} ^ {B}"}
        if xo in ops:
            expr = ops[xo].format(A=f"cr_bit(c, {a})", B=f"cr_bit(c, {b})")
            return f"cr_setbit(c, {d}, {expr});"
        raise Unsupported(f"op19 xo {xo}")

    def loadstore_d(self, op, rd, ra, d):
        upd = op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55)
        ea = "ea" if upd else ea_d(ra, d)
        body = {
            32: f"{R(rd)} = rd32({ea});", 33: f"{R(rd)} = rd32({ea});",
            34: f"{R(rd)} = rd8({ea});", 35: f"{R(rd)} = rd8({ea});",
            36: f"wr32({ea}, {R(rd)});", 37: f"wr32({ea}, {R(rd)});",
            38: f"wr8({ea}, (uint8_t){R(rd)});", 39: f"wr8({ea}, (uint8_t){R(rd)});",
            40: f"{R(rd)} = rd16({ea});", 41: f"{R(rd)} = rd16({ea});",
            42: f"{R(rd)} = (uint32_t)(int32_t)(int16_t)rd16({ea});", 43: f"{R(rd)} = (uint32_t)(int32_t)(int16_t)rd16({ea});",
            44: f"wr16({ea}, (uint16_t){R(rd)});", 45: f"wr16({ea}, (uint16_t){R(rd)});",
            48: f"set_single(c, {rd}, u2f(rd32({ea})));", 49: f"set_single(c, {rd}, u2f(rd32({ea})));",
            50: f"{F(rd)}.ps0 = u2d(rd64({ea}));", 51: f"{F(rd)}.ps0 = u2d(rd64({ea}));",
            52: f"wr32({ea}, f2u((float){F(rd)}.ps0));", 53: f"wr32({ea}, f2u((float){F(rd)}.ps0));",
            54: f"wr64({ea}, d2u({F(rd)}.ps0));", 55: f"wr64({ea}, d2u({F(rd)}.ps0));",
        }
        if op == 46:  # lmw
            return "{ uint32_t ea = " + ea_d(ra, d) + "; " + " ".join(
                f"{R(r)} = rd32(ea + {4 * (r - rd)}u);" for r in range(rd, 32)) + " }"
        if op == 47:  # stmw
            return "{ uint32_t ea = " + ea_d(ra, d) + "; " + " ".join(
                f"wr32(ea + {4 * (r - rd)}u, {R(r)});" for r in range(rd, 32)) + " }"
        if upd:
            return f"{{ uint32_t ea = {R(ra)} + {hx(d)}; {body[op]} {R(ra)} = ea; }}"
        return body[op]

    def op31(self, w, pc):
        d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        xo10, xo9, Rc = (w >> 1) & 0x3FF, (w >> 1) & 0x1FF, w & 1
        rc0 = (lambda reg: f" RC0(c, {reg});" if Rc else "")
        arith = {8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491}
        xo = xo9 if xo9 in arith else xo10
        carry = lambda x, y, cin: (f"{{ uint64_t t = (uint64_t)({x}) + (uint64_t)({y}) + {cin}; "
                                   f"c->xer_ca = (uint32_t)(t >> 32); {R(d)} = (uint32_t)t; }}")
        simple = {
            266: f"{R(d)} = {R(a)} + {R(b)};",
            40: f"{R(d)} = {R(b)} - {R(a)};",
            104: f"{R(d)} = 0u - {R(a)};",
            235: f"{R(d)} = {R(a)} * {R(b)};",
            75: f"{R(d)} = (uint32_t)(((int64_t)(int32_t){R(a)} * (int64_t)(int32_t){R(b)}) >> 32);",
            11: f"{R(d)} = (uint32_t)(((uint64_t){R(a)} * (uint64_t){R(b)}) >> 32);",
            491: f"{R(d)} = ppc_divw({R(a)}, {R(b)});",
            459: f"{R(d)} = ppc_divwu({R(a)}, {R(b)});",
            10: carry(R(a), R(b), 0),
            138: carry(R(a), R(b), "c->xer_ca"),
            8: carry(f"(uint32_t)~{R(a)}", R(b), 1),
            136: carry(f"(uint32_t)~{R(a)}", R(b), "c->xer_ca"),
            202: carry(R(a), 0, "c->xer_ca"),
            234: carry(R(a), "0xFFFFFFFFu", "c->xer_ca"),
            200: carry(f"(uint32_t)~{R(a)}", 0, "c->xer_ca"),
            232: carry(f"(uint32_t)~{R(a)}", "0xFFFFFFFFu", "c->xer_ca"),
        }
        if xo in simple:
            return simple[xo] + rc0(R(d))
        logic = {28: "{S} & {B}", 60: "{S} & ~{B}", 444: "{S} | {B}", 412: "{S} | ~{B}", 124: "~({S} | {B})",
                 476: "~({S} & {B})", 316: "{S} ^ {B}", 284: "~({S} ^ {B})"}
        if xo in logic:
            return f"{R(a)} = {logic[xo].format(S=R(d), B=R(b))};" + rc0(R(a))
        if xo == 24:
            return f"{R(a)} = ({R(b)} & 0x20) ? 0u : ({R(d)} << ({R(b)} & 31));" + rc0(R(a))
        if xo == 536:
            return f"{R(a)} = ({R(b)} & 0x20) ? 0u : ({R(d)} >> ({R(b)} & 31));" + rc0(R(a))
        if xo == 792:
            return f"{R(a)} = ppc_sraw(c, {R(d)}, {R(b)});" + rc0(R(a))
        if xo == 824:  # srawi
            sh = b
            return (f"c->xer_ca = ((int32_t){R(d)} < 0) && ({R(d)} & {hx((1 << sh) - 1)}) != 0; "
                    f"{R(a)} = (uint32_t)((int32_t){R(d)} >> {sh});" + rc0(R(a)))
        if xo == 26:
            return f"{R(a)} = cntlzw({R(d)});" + rc0(R(a))
        if xo == 954:
            return f"{R(a)} = (uint32_t)(int32_t)(int8_t){R(d)};" + rc0(R(a))
        if xo == 922:
            return f"{R(a)} = (uint32_t)(int32_t)(int16_t){R(d)};" + rc0(R(a))
        if xo == 0:
            return f"cmp_s(c, {d >> 2}, (int32_t){R(a)}, (int32_t){R(b)});"
        if xo == 32:
            return f"cmp_u(c, {d >> 2}, {R(a)}, {R(b)});"
        if xo == 4:
            return self.trap(d, R(a), R(b), pc)
        # indexed loads / stores
        X = ea_x(a, b)
        idx = {
            23: f"{R(d)} = rd32({{ea}});", 87: f"{R(d)} = rd8({{ea}});", 279: f"{R(d)} = rd16({{ea}});",
            343: f"{R(d)} = (uint32_t)(int32_t)(int16_t)rd16({{ea}});",
            151: f"wr32({{ea}}, {R(d)});", 215: f"wr8({{ea}}, (uint8_t){R(d)});", 407: f"wr16({{ea}}, (uint16_t){R(d)});",
            535: f"set_single(c, {d}, u2f(rd32({{ea}})));", 599: f"{F(d)}.ps0 = u2d(rd64({{ea}}));",
            663: f"wr32({{ea}}, f2u((float){F(d)}.ps0));", 727: f"wr64({{ea}}, d2u({F(d)}.ps0));",
            983: f"wr32({{ea}}, (uint32_t)d2u({F(d)}.ps0));",
            534: f"{R(d)} = bswap32(rd32({{ea}}));", 790: f"{R(d)} = bswap16(rd16({{ea}}));",
            662: f"wr32({{ea}}, bswap32({R(d)}));", 918: f"wr16({{ea}}, bswap16((uint16_t){R(d)}));",
            20: f"{R(d)} = rd32({{ea}});",  # lwarx
        }
        upd = {55: 23, 119: 87, 311: 279, 375: 343, 183: 151, 247: 215, 439: 407, 567: 535, 631: 599, 695: 663, 759: 727}
        if xo in idx:
            return idx[xo].format(ea=X)
        if xo in upd:
            return f"{{ uint32_t ea = {R(a)} + {R(b)}; {idx[upd[xo]].format(ea='ea')} {R(a)} = ea; }}"
        if xo == 150:  # stwcx.
            return f"wr32({X}, {R(d)}); cr_set(c, 0, 2 | (c->xer_so & 1));"
        if xo == 597:
            return f"rt_lswi(c, {RA0(a)}, {d}, {b or 32});"
        if xo == 725:
            return f"rt_stswi(c, {RA0(a)}, {d}, {b or 32});"
        if xo in (54, 86, 246, 278, 470, 598, 854, 306, 210, 242):  # cache/sync/TLB/segment hints
            return ";"
        if xo == 982:
            return f"rt_icache_invalidate({X});"
        if xo == 1014:
            return f"rt_dcbz({X});"
        if xo in (595, 659):  # mfsr, mfsrin
            return f"{R(d)} = 0u;"
        spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
        if xo == 339:  # mfspr
            if spr == 1:
                return f"{R(d)} = (c->xer_so << 31) | (c->xer_ca << 29);"
            if spr == 8:
                return f"{R(d)} = c->lr;"
            if spr == 9:
                return f"{R(d)} = c->ctr;"
            if 912 <= spr <= 919:
                return f"{R(d)} = c->gqr[{spr - 912}];"
            if spr in (268, 269):
                return f"{R(d)} = (uint32_t)(rt_timebase() >> {32 if spr == 269 else 0});"
            if spr == 22:  # DEC: the decrementer is a runtime timer
                return f"{R(d)} = rt_get_dec(c);"
            return f"{R(d)} = c->spr[{spr}];"
        if xo == 467:  # mtspr
            if spr == 1:
                return f"c->xer_so = {R(d)} >> 31; c->xer_ca = ({R(d)} >> 29) & 1;"
            if spr == 8:
                return f"c->lr = {R(d)};"
            if spr == 9:
                return f"c->ctr = {R(d)};"
            if 912 <= spr <= 919:
                return f"c->gqr[{spr - 912}] = {R(d)};"
            if spr == 22:
                return f"rt_set_dec(c, {R(d)});"
            return f"c->spr[{spr}] = {R(d)};"
        if xo == 371:  # mftb
            return f"{R(d)} = (uint32_t)(rt_timebase() >> {32 if spr == 269 else 0});"
        if xo == 19:
            return f"{R(d)} = c->cr;"
        if xo == 144:  # mtcrf
            crm = (w >> 12) & 0xFF
            m = 0
            for i in range(8):
                if crm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f"c->cr = (c->cr & {hx(~m)}) | ({R(d)} & {hx(m)});"
        if xo == 83:
            return f"{R(d)} = c->msr;"
        if xo == 146:
            return f"c->msr = {R(d)};"
        if xo == 512:  # mcrxr
            return f"cr_set(c, {d >> 2}, (c->xer_so << 3) | (c->xer_ca << 1)); c->xer_so = c->xer_ca = 0;"
        raise Unsupported(f"op31 xo {xo10}")

    def fp_single(self, w):
        d, a, b, cc, xo = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
        A, B, C = f"{F(a)}.ps0", f"{F(b)}.ps0", f"{F(cc)}.ps0"
        ex = {21: f"{A} + {B}", 20: f"{A} - {B}", 25: f"{A} * {C}", 18: f"{A} / {B}",
              29: f"fma({A}, {C}, {B})", 28: f"fma({A}, {C}, -{B})", 31: f"-fma({A}, {C}, {B})",
              30: f"-fma({A}, {C}, -{B})", 24: f"ppc_fres({B})", 22: f"sqrt({B})"}
        if xo not in ex:
            raise Unsupported(f"op59 xo {xo}")
        return f"set_single(c, {d}, {ex[xo]});"

    def fp_double(self, w, pc):
        d, a, b, cc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        xo5, xo10 = (w >> 1) & 31, (w >> 1) & 0x3FF
        A, B, C = f"{F(a)}.ps0", f"{F(b)}.ps0", f"{F(cc)}.ps0"
        a_form = {21: f"{A} + {B}", 20: f"{A} - {B}", 25: f"{A} * {C}", 18: f"{A} / {B}",
                  29: f"fma({A}, {C}, {B})", 28: f"fma({A}, {C}, -{B})", 31: f"-fma({A}, {C}, {B})",
                  30: f"-fma({A}, {C}, -{B})", 23: f"({A} >= 0.0) ? {C} : {B}", 26: f"ppc_frsqrte({B})",
                  22: f"sqrt({B})"}
        if xo5 in a_form and xo5 >= 18:
            return f"{F(d)}.ps0 = {a_form[xo5]};"
        if xo10 in (0, 32):
            return f"cmp_f(c, {d >> 2}, {A}, {B});"
        if xo10 == 12:
            return f"set_single(c, {d}, {B});"
        if xo10 in (14, 15):
            return f"set_fpr_bits(c, {d}, ppc_fctiw({B}, {1 if xo10 == 15 else 0}));"
        if xo10 == 72:
            return f"{F(d)}.ps0 = {B};"
        if xo10 == 40:
            return f"{F(d)}.ps0 = -{B};"
        if xo10 == 264:
            return f"{F(d)}.ps0 = fabs({B});"
        if xo10 == 136:
            return f"{F(d)}.ps0 = -fabs({B});"
        if xo10 == 583:
            return f"set_fpr_bits(c, {d}, c->fpscr);"
        if xo10 == 711:  # mtfsf
            fm = (w >> 17) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            return f"c->fpscr = (c->fpscr & {hx(~m)}) | ((uint32_t)d2u({B}) & {hx(m)});"
        if xo10 == 38:
            return f"c->fpscr |= {hx(1 << (31 - d))};"
        if xo10 == 70:
            return f"c->fpscr &= {hx(~(1 << (31 - d)))};"
        if xo10 == 134:
            imm, sh = (w >> 12) & 0xF, 28 - 4 * (d >> 2)
            return f"c->fpscr = (c->fpscr & {hx(~(0xF << sh))}) | {hx(imm << sh)};"
        if xo10 == 64:
            return f"cr_set(c, {d >> 2}, (c->fpscr >> {28 - 4 * (a >> 2)}) & 0xF);"
        raise Unsupported(f"op63 xo {xo10}")

    def paired(self, w, pc):
        d, a, b, cc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        xo5, xo6, xo10 = (w >> 1) & 31, (w >> 1) & 0x3F, (w >> 1) & 0x3FF
        if xo5 in (6, 7) and xo6 in (6, 7, 38, 39):  # psq_lx, psq_stx, psq_lux, psq_stux
            W, I = (w >> 10) & 1, (w >> 7) & 7
            fn = "psq_l" if xo6 in (6, 38) else "psq_st"
            if xo6 in (38, 39):
                return f"{{ uint32_t ea = {R(a)} + {R(b)}; {fn}(c, {d}, ea, {W}, {I}); {R(a)} = ea; }}"
            return f"{fn}(c, {d}, {ea_x(a, b)}, {W}, {I});"
        a0, a1, b0, b1, c0, c1 = (f"{F(a)}.ps0", f"{F(a)}.ps1", f"{F(b)}.ps0", f"{F(b)}.ps1",
                                  f"{F(cc)}.ps0", f"{F(cc)}.ps1")
        two = {
            21: (f"{a0} + {b0}", f"{a1} + {b1}"), 20: (f"{a0} - {b0}", f"{a1} - {b1}"),
            25: (f"{a0} * {c0}", f"{a1} * {c1}"), 18: (f"{a0} / {b0}", f"{a1} / {b1}"),
            29: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c1}, {b1})"),
            28: (f"fma({a0}, {c0}, -{b0})", f"fma({a1}, {c1}, -{b1})"),
            31: (f"-fma({a0}, {c0}, {b0})", f"-fma({a1}, {c1}, {b1})"),
            30: (f"-fma({a0}, {c0}, -{b0})", f"-fma({a1}, {c1}, -{b1})"),
            23: (f"({a0} >= 0.0) ? {c0} : {b0}", f"({a1} >= 0.0) ? {c1} : {b1}"),
            24: (f"ppc_fres({b0})", f"ppc_fres({b1})"),
            26: (f"ppc_frsqrte({b0})", f"ppc_frsqrte({b1})"),
            10: (f"{a0} + {b1}", c1), 11: (c0, f"{a0} + {b1}"),
            12: (f"{a0} * {c0}", f"{a1} * {c0}"), 13: (f"{a0} * {c1}", f"{a1} * {c1}"),
            14: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c0}, {b1})"),
            15: (f"fma({a0}, {c1}, {b0})", f"fma({a1}, {c1}, {b1})"),
        }
        if xo5 in two:
            x0, x1 = two[xo5]
            return f"{{ double t0 = rs({x0}), t1 = rs({x1}); {F(d)}.ps0 = t0; {F(d)}.ps1 = t1; }}"
        one = {40: "-{x}", 72: "{x}", 136: "-fabs({x})", 264: "fabs({x})"}
        if xo10 in one:
            return (f"{{ double t0 = {one[xo10].format(x=b0)}, t1 = {one[xo10].format(x=b1)}; "
                    f"{F(d)}.ps0 = t0; {F(d)}.ps1 = t1; }}")
        merge = {528: (a0, b0), 560: (a0, b1), 592: (a1, b0), 624: (a1, b1)}
        if xo10 in merge:
            x0, x1 = merge[xo10]
            return f"{{ double t0 = {x0}, t1 = {x1}; {F(d)}.ps0 = t0; {F(d)}.ps1 = t1; }}"
        if xo10 in (0, 32):
            return f"cmp_f(c, {d >> 2}, {a0}, {b0});"
        if xo10 in (64, 96):
            return f"cmp_f(c, {d >> 2}, {a1}, {b1});"
        if xo10 == 1014:
            return f"rt_dcbz({ea_x(a, b)});"
        raise Unsupported(f"op4 xo {xo10}")

    # ---- whole function
    def generate(self, entry=None):
        """Emit the function. With `entry`, emit an extra C function that starts at that
        instruction (for code called in the middle, like the _savefpr_NN helpers)."""
        img = self.ctx.img
        lines, last = [], None
        for pc in range(self.start, self.end, 4):
            w = img.word(pc)
            try:
                code = self.insn(w, pc)
            except Unsupported as e:
                self.unknown += 1
                self.ctx.unsupported[str(e)] += 1
                code = f"rt_illegal(c, {hx(pc)}, {hx(w)});"
            lines.append((pc, w, code))
            last = w
        fn_addr = entry if entry is not None else self.start
        label = self.name if entry is None else f"{self.name}+0x{entry - self.start:X}"
        out = [f"/* {label} */", f"void f_{fn_addr:08X}(Ctx *c) {{"]
        sdk = SDK_RANGE[0] <= self.start < SDK_RANGE[1]
        out.append(f"    {'RT_ENTER_SDK' if sdk else 'RT_ENTER'}(c, {hx(fn_addr)});")
        if entry is None and fn_addr in ENTRY_HOOKS:
            out.append(f"    rt_hook_{fn_addr:08X}(c);")
        if entry is not None:
            self.labels.add(entry)
            out.append(f"    goto L_{entry:08X};")
        for pc, w, code in lines:
            if pc in self.labels:
                out.append(f"L_{pc:08X}:;")
            out.append(f"    /* {pc:08X} {w:08X} */ {code}")
        # falls off the end into the next function?
        op, xo = last >> 26, (last >> 1) & 0x3FF
        terminal = (op == 18 and not last & 1) or (op == 19 and xo in (16, 528) and (last >> 21) & 0x14 == 0x14 and not last & 1)
        if not terminal:
            out.append(f"    {self.jump(self.end) if self.end in self.ctx.func_starts else f'rt_jump(c, {hx(self.end)});'}")
        out.append("}")
        return "\n".join(out)


class Context:
    def __init__(self, img, funcs, tables, hle, relocs=None, data_relocs=None, module=None):
        self.img = img
        self.func_starts = {a for a, _, _, _ in funcs}
        self.jt_targets = set()
        self.unsupported = defaultdict(int)
        self.found_tables = 0
        self.relocs = relocs or {}            # code address -> Reloc (REL modules)
        self.data_relocs = data_relocs or {}  # (section, offset) -> Reloc (REL modules)
        self.module = module                  # RelModule being generated, or None for main.dol
        for addr, size in tables:
            for o in range(0, size, 4):
                t = img.word(addr + o)
                if t is not None and t in range(0x80003100, 0x80200000):
                    self.jt_targets.add(t)
        # Calls (bl/bcl) that land inside another function need their own entry point.
        starts = sorted(funcs)
        import bisect
        keys = [a for a, _, _, _ in starts]
        self.mid_entries = defaultdict(set)
        for a, s, _, _ in starts:
            for pc in range(a, a + s, 4):
                w = img.word(pc)
                if w is None or not w & 1 or pc in self.relocs:  # relocated calls are resolved separately
                    continue
                op = w >> 26
                if op == 18 and not w & 2:
                    li = w & 0x03FFFFFC
                    t = (pc + (li - 0x04000000 if li & 0x02000000 else li)) & 0xFFFFFFFF
                elif op == 16 and not w & 2:
                    t = (pc + sx16(w & 0xFFFC)) & 0xFFFFFFFF
                else:
                    continue
                if t in self.func_starts or t == pc + 4:
                    continue
                i = bisect.bisect_right(keys, t) - 1
                if i >= 0 and starts[i][0] < t < starts[i][0] + starts[i][1]:
                    self.mid_entries[starts[i][0]].add(t)
        for es in self.mid_entries.values():
            self.func_starts |= es

# ---------------------------------------------------------------------------------------------- main

def is_stub(addr):
    return any(lo <= addr < hi for lo, hi, _ in STUB_RANGES)


def load_hle_list():
    """Names in hle_overrides.txt: plain lines are HLE replacements; `entry NAME` lines are entry
    hooks (the recompiled function calls rt_hook_XXXXXXXX(c) before its body). Returns both sets."""
    names, hooks = set(), set()
    if os.path.exists(HLE_LIST):
        for line in open(HLE_LIST, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line.startswith("entry "):
                hooks.add(line[6:].strip())
            elif line:
                names.add(line)
    return names, hooks


ENTRY_HOOKS = set()  # addresses of functions with an entry hook


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


# ------------------------------------------------------------------------------------------ REL modules
# Each module's code gets its own virtual address range so its functions have unique names and its
# internal (unrelocated) branches resolve at generation time. Anything OSLink patches at run time
# becomes an expression over g_rel[module].sec[section], filled in when the game links the module.
REL_VBASE, REL_VSTRIDE = 0x90000000, 0x00400000
REL_EXCLUDED = {"m009"}       # never loaded; built against an older main.dol (see config.yml)
REL_IMPORT_BY_ID = {8: "m4", 11: "m8"}  # module ids other modules import from (id 11 is shared)


class RelModule:
    def __init__(self, name, index, data):
        u = lambda o: struct.unpack_from(">I", data, o)[0]
        self.name, self.index, self.data = name, index, data
        self.id, self.nsec = u(0), u(0xC)
        so = u(0x10)
        self.sections = [(u(so + i * 8), u(so + i * 8 + 4)) for i in range(self.nsec)]
        self.text_sec = next(i for i, (o, sz) in enumerate(self.sections) if o & 1 and sz)
        self.text_off = self.sections[self.text_sec][0] & ~3
        self.text_size = self.sections[self.text_sec][1]
        self.vbase = REL_VBASE + index * REL_VSTRIDE
        self.relocs, self.data_relocs, self.funcs = {}, {}, []


class Reloc:
    def __init__(self, rtype, target, sec, addend):
        self.type, self.target, self.sec, self.addend = rtype, target, sec, addend  # target None = main.dol

    def expr(self):
        if self.target is None:
            return hx(self.addend)
        return f"(g_rel[{self.target.index}].sec[{self.sec}] + {hx(self.addend)})"

    def is_code(self):
        return self.target is None or self.sec == self.target.text_sec

    def branch_target(self):
        if self.target is None:
            return self.addend
        if self.sec == self.target.text_sec:
            return self.target.vbase + self.addend
        return None


class RelImage:
    def __init__(self, m):
        self.m = m

    def word(self, addr):
        o = addr - self.m.vbase
        if 0 <= o <= self.m.text_size - 4:
            return struct.unpack_from(">I", self.m.data, self.m.text_off + o)[0]
        return None


def load_rels():
    files = sorted(f[:-4] for f in os.listdir(os.path.join(ROOT, "orig", VERSION, "files")) if f.endswith(".rel"))
    names = [n for n in files if n not in REL_EXCLUDED]
    mods = [RelModule(n, i, open(os.path.join(ROOT, "orig", VERSION, "files", n + ".rel"), "rb").read())
            for i, n in enumerate(names)]
    by_name = {m.name: m for m in mods}
    for m in mods:
        d = m.data
        u = lambda o: struct.unpack_from(">I", d, o)[0]
        io, isz = u(0x28), u(0x2C)
        for k in range(isz // 8):
            mid, o = u(io + k * 8), u(io + k * 8 + 4)
            target = None if mid == 0 else m if mid == m.id else by_name[REL_IMPORT_BY_ID[mid]]
            sec, off = 0, 0
            while True:
                delta, rtype, tsec, addend = struct.unpack_from(">HBBI", d, o)
                o += 8
                if rtype == 203:
                    break
                if rtype == 202:
                    sec, off = tsec, 0
                    continue
                off += delta
                if rtype == 201:
                    continue
                r = Reloc(rtype, target, tsec, addend)
                if sec == m.text_sec:
                    m.relocs[m.vbase + (off & ~3)] = r  # ADDR16 relocs point at the field (insn + 2)
                else:
                    m.data_relocs[(sec, off)] = r
        # functions: dtk's symbols, plus any code dtk left as a gap
        found = []
        for line in open(os.path.join(ROOT, "config", VERSION, m.name, "symbols.txt"), encoding="utf-8"):
            x = SYM.match(line)
            if x and x.group(4) == "function" and x.group(2) == "text" and x.group(5):
                found.append((int(x.group(3), 16), int(x.group(5), 16), x.group(1)))
        found.sort()
        pos, full = 0, []
        for off, size, name in found + [(m.text_size, 0, None)]:
            if off > pos and any(struct.unpack_from(">I", d, m.text_off + q)[0] for q in range(pos, off, 4)):
                full.append((pos, off - pos, f"gap_{pos:X}"))
            if name:
                full.append((off, size, name))
            pos = max(pos, off + size)
        m.funcs = [(m.vbase + off, size, f"{m.name}:{name}", "text") for off, size, name in full]
    return mods


def write_funcs(path, ctx, funcs, chunk_name, files):
    unknown = 0
    for i in range(0, len(funcs), FUNCS_PER_FILE):
        chunk = funcs[i:i + FUNCS_PER_FILE]
        name = f"{chunk_name}_{i // FUNCS_PER_FILE:03d}.c"
        files.append(name)
        with open(os.path.join(path, name), "w", newline="\n") as fc:
            fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#include \"funcs.h\"\n\n")
            for a, s, n, _ in chunk:
                g = FuncGen(ctx, a, s, n)
                fc.write(g.generate() + "\n\n")
                unknown += g.unknown
                for e in sorted(ctx.mid_entries.get(a, ())):
                    fc.write(FuncGen(ctx, a, s, n).generate(entry=e) + "\n\n")
    return unknown


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", VERSION, "recomp"))
    args = ap.parse_args()

    segs, entry, _ = load_dol(os.path.join(ROOT, "orig", VERSION, "sys", "main.dol"))
    img = Image(segs)
    funcs, tables = load_symbols(os.path.join(ROOT, "config", VERSION, "symbols.txt"))
    ctx = Context(img, funcs, tables, STUB_RANGES)

    # REL modules: one context each, all sharing the set of known function entry points
    mods = load_rels()
    overrides, hook_names = load_hle_list()
    by_name = {n: a for a, _, n, _ in funcs}
    missing = hook_names - set(by_name)
    if missing:
        sys.exit(f"hle_overrides.txt hooks unknown functions: {sorted(missing)}")
    ENTRY_HOOKS.update(by_name[n] for n in hook_names)
    mctx = {m.name: Context(RelImage(m), m.funcs, [], None, m.relocs, m.data_relocs, m) for m in mods}
    import bisect
    keys = [a for a, _, _, _ in funcs]
    for m in mods:  # calls from modules into the middle of a main.dol function
        for r in m.relocs.values():
            t = r.branch_target() if r.type in (10, 11) and r.target is None else None
            if t is None or t in ctx.func_starts:
                continue
            k = bisect.bisect_right(keys, t) - 1
            if k >= 0 and funcs[k][0] < t < funcs[k][0] + funcs[k][1]:
                ctx.mid_entries[funcs[k][0]].add(t)
    shared = set(ctx.func_starts)
    for es in ctx.mid_entries.values():
        shared |= es
    for c in mctx.values():
        shared |= c.func_starts
    ctx.func_starts = shared
    for c in mctx.values():
        c.func_starts = shared

    missing = overrides - {n for _, _, n, _ in funcs}
    if missing:
        sys.exit(f"hle_overrides.txt names unknown functions: {sorted(missing)}")
    recomp = [f for f in funcs if f[2] not in overrides and not is_stub(f[0])]
    hle = [f for f in funcs if f[2] not in overrides and is_stub(f[0])]
    os.makedirs(args.out, exist_ok=True)
    recomp_starts = {a for a, _, _, _ in recomp}
    extra = sorted((e, f"{n}+0x{e - a:X}") for a, _, n, _ in funcs if a in recomp_starts
                   for e in ctx.mid_entries.get(a, ()))
    mod_entries = []
    for m in mods:
        c = mctx[m.name]
        for a, _, n, _ in m.funcs:
            mod_entries.append((a, n))
            mod_entries += [(e, f"{n}+0x{e - a:X}") for e in c.mid_entries.get(a, ())]
    all_entries = sorted([(a, n) for a, _, n, _ in funcs] + extra + mod_entries)

    with open(os.path.join(args.out, "funcs.h"), "w", newline="\n") as fh:
        fh.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#pragma once\n#include \"ppc.h\"\n\n")
        dol_bytes = open(os.path.join(ROOT, "orig", VERSION, "sys", "main.dol"), "rb").read()
        fh.write(f"/* the main.dol this code was translated from: the runtime refuses a disc whose DOL differs */\n"
                 f"#define RT_DOL_SIZE {len(dol_bytes)}u\n#define RT_DOL_CRC32 0x{zlib.crc32(dol_bytes):08X}u\n\n")
        for a in sorted(ENTRY_HOOKS):
            fh.write(f"void rt_hook_{a:08X}(Ctx *c); /* entry hook (hle_overrides.txt) */\n")
        for a, n in all_entries:
            fh.write(f"void f_{a:08X}(Ctx *c); /* {n} */\n")
        fh.write(f"\n#define GUEST_ENTRY 0x{entry:08X}u\n")
        fh.write("\n/* REL modules: section addresses, filled in when the game links a module (OSNotifyLink). */\n")
        fh.write(f"#define RT_NUM_RELS {len(mods)}\n")
        fh.write("typedef struct { uint32_t sec[32]; uint32_t loaded; } RelRuntime;\n")
        fh.write("extern RelRuntime g_rel[RT_NUM_RELS];\n")
        fh.write("typedef struct { const char *name; uint32_t id, nsec, text_sec, vbase, text_size; const uint32_t *sizes; } RelInfo;\n")
        fh.write("extern const RelInfo g_rel_info[RT_NUM_RELS];\n")
        fh.write("\n/* Named access to guest functions: G_<name> is the address, F_<name> the C function. */\n")
        for a, _, n, _ in funcs:
            if C_IDENT.match(n):
                fh.write(f"#define G_{n} 0x{a:08X}u\n#define F_{n} f_{a:08X}\n")

    files = []
    total_unknown = write_funcs(args.out, ctx, recomp, "recomp", files)
    for m in mods:
        total_unknown += write_funcs(args.out, mctx[m.name], m.funcs, f"rel_{m.name}", files)

    with open(os.path.join(args.out, "hle_stubs.c"), "w", newline="\n") as fc:
        fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit.\n"
                 " * Placeholders for functions the runtime must reimplement (Dolphin SDK, MetroTRK).\n"
                 " * Define HLE_IMPL_XXXXXXXX in the runtime to replace one. */\n#include \"funcs.h\"\n\n")
        for a, _, n, _ in hle:
            fc.write(f"#ifndef HLE_IMPL_{a:08X}\nvoid f_{a:08X}(Ctx *c) {{ rt_hle_unimplemented(c, 0x{a:08X}u, {c_string(n)}); }}\n#endif\n")

    with open(os.path.join(args.out, "func_table.c"), "w", newline="\n") as fc:
        fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#include \"funcs.h\"\n\n")
        fc.write("typedef struct { uint32_t addr; GuestFn fn; const char *name; } FuncEntry;\n")
        fc.write(f"const unsigned g_func_count = {len(all_entries)};\nconst FuncEntry g_funcs[] = {{\n")
        for a, n in all_entries:
            fc.write(f"    {{ 0x{a:08X}u, f_{a:08X}, {c_string(n)} }},\n")
        fc.write("};\n\n")
        # module descriptions, used to recognise a module when the game links it
        for m in mods:
            fc.write(f"static const uint32_t sizes_{m.index}[] = {{ {', '.join(str(sz) for _, sz in m.sections)} }};\n")
        fc.write("const RelInfo g_rel_info[RT_NUM_RELS] = {\n")
        for m in mods:
            fc.write(f"    {{ {c_string(m.name)}, {m.id}, {m.nsec}, {m.text_sec}, {hx(m.vbase)}, {m.text_size}, sizes_{m.index} }},\n")
        fc.write("};\nRelRuntime g_rel[RT_NUM_RELS];\n")

    with open(os.path.join(args.out, "sources.txt"), "w", newline="\n") as fs:
        fs.write("\n".join(files + ["hle_stubs.c", "func_table.c"]) + "\n")

    nrel_funcs = sum(len(m.funcs) for m in mods)
    print(f"functions: {len(funcs)} in main.dol ({len(recomp)} recompiled, {len(overrides)} HLE overrides, "
          f"{len(hle)} stubs), {nrel_funcs} in {len(mods)} REL modules")
    print(f"REL relocations: {sum(len(m.relocs) for m in mods)} in code, "
          f"{sum(len(m.data_relocs) for m in mods)} in data")
    found = ctx.found_tables + sum(c.found_tables for c in mctx.values())
    print(f"jump tables: {len(tables)} from symbols ({len(ctx.jt_targets)} targets), "
          f"{found} bctr sites resolved by pattern")
    nmid = len(extra) + sum(len(e) for c in mctx.values() for e in c.mid_entries.values())
    print(f"extra entry points (calls into the middle of a function): {nmid}")
    unsupported = defaultdict(int)
    for c in [ctx] + list(mctx.values()):
        for k, v in c.unsupported.items():
            unsupported[k] += v
    print(f"untranslated instructions: {total_unknown}")
    for k, v in sorted(unsupported.items(), key=lambda x: -x[1]):
        print(f"  {v:6}  {k}")
    print(f"wrote {len(files)} source files to {args.out}")


if __name__ == "__main__":
    sys.exit(main())
