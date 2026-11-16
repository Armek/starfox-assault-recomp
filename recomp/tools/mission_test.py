"""Run each mission's first stage with --stage=N --auto-press and summarize what happened.

For each mission this starts the game, lets auto-press get through the menus (the game starts Mission 1,
which the runtime swaps for the requested stage), and stops after a fixed time. It keeps the log and a
contact sheet of the frames dumped during the run under build/GF7E01/missions/, and prints a summary:
the stages reached, the last frame number, and any fatal or illegal-instruction lines.

usage: python recomp/tools/mission_test.py [--seconds 150] [--missions 2,3,4] [--exe path]
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIRST_STAGE = {1: 0, 2: 5, 3: 9, 4: 11, 5: 15, 6: 19, 7: 20, 8: 24, 9: 27, 10: 29}
TROUBLE = re.compile(r"fatal|illegal|unsupported|unknown opcode|panic|assert|exception", re.I)


def contact_sheet(frames, path, cols=6, tw=320, th=224):
    try:
        from PIL import Image
    except ImportError:
        return False
    if not frames:
        return False
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tw, rows * th))
    for i, f in enumerate(frames):
        sheet.paste(Image.open(f).convert("RGB").resize((tw, th)), ((i % cols) * tw, (i // cols) * th))
    sheet.save(path)
    return True


def run_mission(exe, mission, seconds, outdir):
    stage = FIRST_STAGE[mission]
    frames_dir = os.path.join(ROOT, "build", "GF7E01", "frames")
    os.makedirs(frames_dir, exist_ok=True)
    start = time.time()
    log_path = os.path.join(outdir, f"mission{mission:02d}.log")
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        p = subprocess.Popen([exe, "--auto-press", "--dump-frames", f"--stage={stage}"], cwd=ROOT,
                             stdout=log, stderr=subprocess.STDOUT)
        try:
            code = p.wait(timeout=seconds)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            code = None  # still running when stopped: no crash
    text = open(log_path, encoding="utf-8", errors="replace").read()
    stages = re.findall(r"\[stage\] start stage (\d+) \(mission (\d+), part ([0-9A-F]+)\)", text)
    trouble = [line for line in text.splitlines() if TROUBLE.search(line)][:5]
    frames = sorted(f for f in glob.glob(os.path.join(frames_dir, "*.bmp")) if os.path.getmtime(f) >= start)
    sheet = os.path.join(outdir, f"mission{mission:02d}.png")
    contact_sheet(frames[::max(1, len(frames) // 30)], sheet)
    last = os.path.basename(frames[-1]) if frames else "none"
    return {"mission": mission, "stage": stage, "exit": code, "stages": stages, "trouble": trouble,
            "frames": len(frames), "last_frame": last}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seconds", type=int, default=150)
    ap.add_argument("--missions", default="2,3,4,5,6,7,8,9,10")
    default_exe = os.path.join(ROOT, "build", "GF7E01", "cmake", "sfa.exe" if os.name == "nt" else "sfa")
    ap.add_argument("--exe", default=default_exe)
    args = ap.parse_args()
    outdir = os.path.join(ROOT, "build", "GF7E01", "missions")
    os.makedirs(outdir, exist_ok=True)
    for m in (int(x) for x in args.missions.split(",")):
        r = run_mission(args.exe, m, args.seconds, outdir)
        status = "ran until stopped" if r["exit"] is None else f"EXITED with code {r['exit']}"
        reached = ", ".join(f"{s}(m{mm} p{pp})" for s, mm, pp in r["stages"]) or "none"
        print(f"mission {m:2d} (stage {r['stage']:2d}): {status}; stages started: {reached}; "
              f"{r['frames']} frames dumped, last {r['last_frame']}")
        for line in r["trouble"]:
            print(f"    ! {line[:160]}")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
