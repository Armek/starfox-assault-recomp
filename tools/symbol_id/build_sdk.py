"""Compile every dolsdk2004 source file in release mode with mwcceppc GC/1.2.5n."""
import subprocess, sys, os, glob, concurrent.futures as cf

sdk, mwcc, out = sys.argv[1], sys.argv[2], sys.argv[3]
signed = ("src/card/CARDRename.c", "src/card/CARDOpen.c", "src/mtx/mtx.c", "src/mtx/mtx44.c")
def flags(rel):
    char = "-char signed" if rel in signed or rel.startswith("src/dvd/") or rel.startswith("src/demo/") else "-char unsigned"
    opt = "-O3,p" if rel == "src/exi/EXIBios.c" else "-O4,p"
    sym = [] if rel in ("src/gd/GDIndirect.c", "src/G2D/G2D.c") else ["-sym", "on"]
    return ["-c", opt, "-inline", "auto", *sym, *char.split(), "-nodefaults", "-proc", "gekko", "-fp", "hard",
            "-Cpp_exceptions", "off", "-enum", "int", "-warn", "off", "-pragma", "cats off",
            "-D__GEKKO__", f"-DSDK_REVISION={os.environ.get('SDK_REVISION','2')}", "-I-", "-Iinclude", "-Iinclude/libc", "-ir", "src", "-DRELEASE"]

srcs = sorted(p.replace("\\", "/") for p in glob.glob("src/**/*.c", root_dir=sdk, recursive=True))
def build(rel):
    o = os.path.join(out, rel[:-2].replace("/", "__") + ".o")
    r = subprocess.run([mwcc, *flags(rel), rel, "-o", o], cwd=sdk, capture_output=True, text=True)
    return rel, r.returncode, (r.stdout + r.stderr)[-300:]
os.makedirs(out, exist_ok=True)
ok = 0
with cf.ThreadPoolExecutor(8) as ex:
    for rel, rc, msg in ex.map(build, srcs):
        if rc == 0: ok += 1
        else: print("FAIL", rel, msg.strip().splitlines()[-1] if msg.strip() else "")
print(f"compiled {ok}/{len(srcs)}")
