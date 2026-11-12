"""Match functions from compiled reference objects against main.dol by masked machine code.

For each function symbol in each .o: take its bytes, mask every 4-byte word that has a
relocation, and look for our dtk-found functions of the same size whose bytes match on all
unmasked words. Unique matches are accepted.
"""
import struct, sys, os, re, glob, json, collections

dol_path, syms_path, obj_dir, out_json = sys.argv[1:5]
u32 = lambda b, o: struct.unpack_from(">I", b, o)[0]

dol = open(dol_path, "rb").read()
segs = [(u32(dol, 0x48+i*4), u32(dol, i*4), u32(dol, 0x90+i*4)) for i in range(18) if u32(dol, 0x90+i*4)]
def read(addr, n):
    for a, o, s in segs:
        if a <= addr and addr + n <= a + s:
            return dol[o + addr - a: o + addr - a + n]
    return None

LINE = re.compile(r"^(\S+) = \.(?:text|init):0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
ours = []
for l in open(syms_path):
    m = LINE.match(l)
    if m: ours.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))
by_size = collections.defaultdict(list)
for n, a, s in ours: by_size[s].append((n, a))
existing = {n for n, a, s in ours}

def elf_funcs(path):
    b = open(path, "rb").read()
    shoff = u32(b, 0x20); shentsize, shnum, shstrndx = struct.unpack_from(">HHH", b, 0x2E)
    sh = [struct.unpack_from(">10I", b, shoff + i*shentsize) for i in range(shnum)]
    strsec = sh[shstrndx]
    nm = lambda tab, o: b[tab+o: b.index(b"\0", tab+o)].decode("latin1")
    names = [nm(strsec[4], s[0]) for s in sh]
    relocs = collections.defaultdict(set)
    for i, s in enumerate(sh):
        if s[1] == 4:  # SHT_RELA
            tgt = s[7]
            for k in range(s[5] // 12):
                relocs[tgt].add(u32(b, s[4] + k*12) & ~3)
    funcs = []
    for s in sh:
        if s[1] == 2:  # SHT_SYMTAB
            strtab = sh[s[6]][4]
            for k in range(s[5] // 16):
                st_name, val, size, info, other, shndx = struct.unpack_from(">IIIBBH", b, s[4] + k*16)
                if info & 0xF == 2 and size and 0 < shndx < len(sh) and names[shndx] == ".text":
                    sec = sh[shndx]
                    code = b[sec[4] + val: sec[4] + val + size]
                    mask = [(val + w*4) in relocs[shndx] for w in range(size // 4)]
                    funcs.append((nm(strtab, st_name), size, code, mask))
    return funcs

matches, ambiguous, missing = {}, 0, 0
lib_hits = collections.Counter(); lib_total = collections.Counter()
for path in sorted(glob.glob(os.path.join(obj_dir, "*.o"))):
    lib = os.path.basename(path).split("__")[1]
    for name, size, code, mask in elf_funcs(path):
        lib_total[lib] += 1
        cands = []
        for on, oa in by_size.get(size, []):
            ob = read(oa, size)
            if ob and all(mask[w] or ob[w*4:w*4+4] == code[w*4:w*4+4] for w in range(size // 4)):
                cands.append((on, oa))
        if len(cands) == 1:
            matches.setdefault(f"{cands[0][1]:08X}", []).append((name, lib, cands[0][0]))
            lib_hits[lib] += 1
        elif len(cands) > 1: ambiguous += 1
        else: missing += 1

# an address matched by several SDK names (identical code) is ambiguous: keep only if one name
final = {a: v[0] for a, v in matches.items() if len({x[0] for x in v}) == 1}
dupe_names = collections.Counter(v[0] for v in final.values())
final = {a: v for a, v in final.items() if dupe_names[v[0]] == 1}
json.dump(final, open(out_json, "w"), indent=0)
print(f"unique matches {len(final)}  (ambiguous code {ambiguous}, not present {missing})")
for lib in sorted(lib_total, key=lambda l: -lib_hits[l]):
    if lib_hits[lib]: print(f"  {lib:10} {lib_hits[lib]:4} / {lib_total[lib]}")
renames = [(a, v) for a, v in final.items() if not re.match(r"^(fn|gap)_", v[2]) and v[2] != v[0]]
print("conflicts with dtk's existing names:", len(renames))
for a, v in renames[:15]: print("   ", a, v)
