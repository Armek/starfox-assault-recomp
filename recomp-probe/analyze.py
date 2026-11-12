import struct, sys, os, re, glob, collections

root = sys.argv[1]
dol = open(os.path.join(root, "sys", "main.dol"), "rb").read()
u32 = lambda b, o: struct.unpack_from(">I", b, o)[0]

print("== main.dol")
secs = []
for i in range(18):
    off, addr, size = u32(dol, i*4), u32(dol, 0x48+i*4), u32(dol, 0x90+i*4)
    if size:
        kind = "text" if i < 7 else "data"
        secs.append((kind, i, off, addr, size))
        print(f"  {kind}{i if i<7 else i-7}: addr {addr:08X} size {size:#x}")
print(f"  bss {u32(dol,0xD8):08X} size {u32(dol,0xDC):#x}  entry {u32(dol,0xE0):08X}")

text = b"".join(dol[o:o+s] for k,i,o,a,s in secs if k=="text")
words = [u32(text, j) for j in range(0, len(text)-3, 4)]
blr = sum(1 for w in words if w == 0x4E800020)
prolog = sum(1 for w in words if (w & 0xFFFF0000) == 0x94210000 and (w & 0x8000))  # stwu r1,-X(r1)
fp = sum(1 for w in words if (w >> 26) in (48,49,50,51,52,53,54,55,59,63))
psq = sum(1 for w in words if (w >> 26) in (56,57,60,61) or ((w>>26)==4))
print(f"  text bytes {len(text):,}  instrs {len(words):,}  blr {blr:,}  stack-frame prologues {prolog:,}")
print(f"  FPU instrs {fp:,}  paired-single (Gekko) instrs {psq:,}")

strs = set(m.group().decode("latin1") for m in re.finditer(rb"[\x20-\x7e]{6,}", dol))
def grep(pat, n=15):
    hits = sorted(s for s in strs if re.search(pat, s, re.I))
    return hits[:n], len(hits)
for label, pat in [("SDK/lib version strings", r"<< Dolphin SDK|Metrowerks|MSL|CodeWarrior|MetroTRK|Build:|ver\."),
                   ("source file names", r"\.(c|cpp|h)\b"),
                   ("GX / hardware", r"\bGX|__GX|\bDSP|\bAX[A-Z]|\bARQ|\bAR[A-Z]|\bCARD|\bDVD|\bPAD|\bVI[A-Z]|\bOS[A-Z]"),
                   ("REL / module loader", r"\.rel\b|OSLink|module"),
                   ("Middleware", r"CRI|ADX|SFD|Sofdec|JSystem|J3D|NNS|Criware|MusyX|Factor5")]:
    hits, n = grep(pat)
    print(f"\n-- {label}: {n} strings")
    for h in hits: print("   ", h[:110])

print("\n== REL modules")
tot_rel = 0
for p in sorted(glob.glob(os.path.join(root, "files", "*.rel"))):
    r = open(p, "rb").read()
    mid, nsec, secoff = u32(r,0), u32(r,0xC), u32(r,0x10)
    ver = u32(r, 0x1C)
    impoff, impsz = u32(r,0x28), u32(r,0x2C)
    tsz = 0
    for i in range(nsec):
        o, s = u32(r, secoff+i*8), u32(r, secoff+i*8+4)
        if o & 1: tsz += s
    imps = [u32(r, impoff+i*8) for i in range(impsz//8)]
    tot_rel += tsz
    print(f"  {os.path.basename(p):12} id {mid:3} v{ver} sections {nsec:2} code {tsz:#8x}  links-to modules {imps}")
print(f"  total REL code bytes {tot_rel:,}")
