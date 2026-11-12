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

    def jump(self, t):
        if self.inside(t):
            self.labels.add(t)
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
        return f"switch (c->ctr) {{ {cases} default: rt_jump(c, c->ctr); return; }}"

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

    # ---- instruction translation
    def insn(self, w, pc):
        op = w >> 26
        rd, ra, rb, rc = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        uimm, simm, Rc = w & 0xFFFF, sx16(w & 0xFFFF), w & 1
        rc0 = lambda reg: f" RC0(c, {reg});" if Rc else ""

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
            pre, cond = self.bc_cond(bo, bi)
            action = ("{ " + self.call(t, pc) + " }") if w & 1 else self.jump(t)
            return self.branch_cond(pre, cond, action)
        if op == 17:
            return f"rt_syscall(c, {hx(pc)});"
        if op == 18:  # b
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = (li if w & 2 else pc + li) & 0xFFFFFFFF
            return self.call(t, pc) if w & 1 else self.jump(t)
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
    def generate(self):
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
        out = [f"/* {self.name} */", f"void f_{self.start:08X}(Ctx *c) {{"]
        sdk = SDK_RANGE[0] <= self.start < SDK_RANGE[1]
        out.append(f"    {'RT_ENTER_SDK' if sdk else 'RT_ENTER'}(c, {hx(self.start)});")
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
    def __init__(self, img, funcs, tables, hle):
        self.img = img
        self.func_starts = {a for a, _, _, _ in funcs}
        self.jt_targets = set()
        self.unsupported = defaultdict(int)
        self.found_tables = 0
        for addr, size in tables:
            for o in range(0, size, 4):
                t = img.word(addr + o)
                if t is not None and t in range(0x80003100, 0x80200000):
                    self.jt_targets.add(t)

# ---------------------------------------------------------------------------------------------- main

def is_stub(addr):
    return any(lo <= addr < hi for lo, hi, _ in STUB_RANGES)


def load_hle_list():
    names = set()
    if os.path.exists(HLE_LIST):
        for line in open(HLE_LIST, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                names.add(line)
    return names


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", VERSION, "recomp"))
    args = ap.parse_args()

    segs, entry, _ = load_dol(os.path.join(ROOT, "orig", VERSION, "sys", "main.dol"))
    img = Image(segs)
    funcs, tables = load_symbols(os.path.join(ROOT, "config", VERSION, "symbols.txt"))
    ctx = Context(img, funcs, tables, STUB_RANGES)

    overrides = load_hle_list()
    missing = overrides - {n for _, _, n, _ in funcs}
    if missing:
        sys.exit(f"hle_overrides.txt names unknown functions: {sorted(missing)}")
    recomp = [f for f in funcs if f[2] not in overrides and not is_stub(f[0])]
    hle = [f for f in funcs if f[2] not in overrides and is_stub(f[0])]
    os.makedirs(args.out, exist_ok=True)

    with open(os.path.join(args.out, "funcs.h"), "w", newline="\n") as fh:
        fh.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#pragma once\n#include \"ppc.h\"\n\n")
        for a, _, n, _ in funcs:
            fh.write(f"void f_{a:08X}(Ctx *c); /* {n} */\n")
        fh.write(f"\n#define GUEST_ENTRY 0x{entry:08X}u\n")
        fh.write("\n/* Named access to guest functions: G_<name> is the address, F_<name> the C function. */\n")
        for a, _, n, _ in funcs:
            if C_IDENT.match(n):
                fh.write(f"#define G_{n} 0x{a:08X}u\n#define F_{n} f_{a:08X}\n")

    total_unknown, files = 0, []
    for i in range(0, len(recomp), FUNCS_PER_FILE):
        chunk = recomp[i:i + FUNCS_PER_FILE]
        name = f"recomp_{i // FUNCS_PER_FILE:03d}.c"
        files.append(name)
        with open(os.path.join(args.out, name), "w", newline="\n") as fc:
            fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#include \"funcs.h\"\n\n")
            for a, s, n, _ in chunk:
                g = FuncGen(ctx, a, s, n)
                fc.write(g.generate() + "\n\n")
                total_unknown += g.unknown

    with open(os.path.join(args.out, "hle_stubs.c"), "w", newline="\n") as fc:
        fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit.\n"
                 " * Placeholders for functions the runtime must reimplement (Dolphin SDK, MetroTRK).\n"
                 " * Define HLE_IMPL_XXXXXXXX in the runtime to replace one. */\n#include \"funcs.h\"\n\n")
        for a, _, n, _ in hle:
            fc.write(f"#ifndef HLE_IMPL_{a:08X}\nvoid f_{a:08X}(Ctx *c) {{ rt_hle_unimplemented(c, 0x{a:08X}u, {c_string(n)}); }}\n#endif\n")

    with open(os.path.join(args.out, "func_table.c"), "w", newline="\n") as fc:
        fc.write("/* Generated by recomp/tools/ppcrecomp.py - do not edit. */\n#include \"funcs.h\"\n\n")
        fc.write("typedef struct { uint32_t addr; GuestFn fn; const char *name; } FuncEntry;\n")
        fc.write(f"const unsigned g_func_count = {len(funcs)};\nconst FuncEntry g_funcs[] = {{\n")
        for a, _, n, _ in funcs:
            fc.write(f"    {{ 0x{a:08X}u, f_{a:08X}, {c_string(n)} }},\n")
        fc.write("};\n")

    with open(os.path.join(args.out, "sources.txt"), "w", newline="\n") as fs:
        fs.write("\n".join(files + ["hle_stubs.c", "func_table.c"]) + "\n")

    print(f"functions: {len(funcs)} total, {len(recomp)} recompiled, {len(overrides)} HLE overrides, "
          f"{len(hle)} stubs")
    print(f"jump tables: {len(tables)} from symbols ({len(ctx.jt_targets)} targets), {ctx.found_tables} bctr sites resolved by pattern")
    print(f"untranslated instructions: {total_unknown}")
    for k, v in sorted(ctx.unsupported.items(), key=lambda x: -x[1]):
        print(f"  {v:6}  {k}")
    print(f"wrote {len(files)} source files to {args.out}")


if __name__ == "__main__":
    sys.exit(main())
