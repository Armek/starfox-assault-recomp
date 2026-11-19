"""Make a release folder (and archive) of the recompiled game, without any game data.

The folder holds the executable (plus SDL2.dll on Windows; the replacement system fonts are compiled
in), a launcher on Linux, PLAY.txt with the setup steps and font-licenses.txt. Players add their own
disc image (.iso/.gcm) next to the executable; saves go to user/ there.

usage: python recomp/tools/package.py [--build-dir DIR] [--out dist] [--no-archive]
"""
import argparse
import os
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PLAY_TXT = """Star Fox: Assault - static recompilation ({platform})
=====================================================

This folder holds only the recompiled program. You need your own copy of the game.

Setup
-----
1. Put a disc image of your Star Fox: Assault (USA, GF7E01) in this folder, as an .iso or .gcm file
   (any file name). If Dolphin has it as .rvz or another compressed format, right-click the game in
   Dolphin, choose Convert File..., and pick ISO. (An extracted disc also works: Dolphin's
   Properties > Filesystem > Extract Entire Disc, placed here as orig/GF7E01/sys and orig/GF7E01/files.)
2. Run {run}. A start screen comes up (mouse, keyboard or controller):
     Game disc    finds an .iso next to the program by itself; Browse... opens your system's
                  file dialog (with a controller, or in the Deck's Game Mode, a built-in browser)
     Display      window or fullscreen
     Frame rate   Original (on-foot missions and cutscenes run at 30 fps, as on the GameCube) or
                  60 fps everywhere (experimental; where your PC can't hold 60, the
                  game goes back to its own rate for a while, so it never runs in slow motion)
     Play        start the game (START on a controller)
   Your choices are remembered in user/launcher.cfg. To skip the start screen, run with --play
   or name the disc image on the command line.

The GameCube's system font (dialog text, comm subtitles) is built in: replacement fonts made from
Droid Sans by the Dolphin project, under the Apache License 2.0 (see font-licenses.txt). A dump of
your own console's boot ROM at user/GF7E01/ipl.bin, or font files in user/fonts/, are used instead
when present.

Controls
--------
Any SDL game controller (the Steam Deck's own controls included), or the keyboard:
    stick: WASD or arrow keys     C-stick: numpad 8/4/6/2
    A: Space or J    B: K or Backspace    X: L    Y: I    Z: Z
    L: Q             R: E                 START: Enter
F11 or Alt+Enter toggles fullscreen.

Options
-------
{help}
Saves (memory card files) are kept in user/GF7E01/card_a/.

If the game stops with an error or crashes, a report is added to user/crash.log. When it isn't
started from a terminal (from Steam, a file manager, or the Deck's game mode), everything it prints
also goes to user/sfa.log for the last run. Send both files with a bug report.
"""

LAUNCHER = """#!/bin/sh
# Starts the game fullscreen from this folder; extra options are passed on.
cd "$(dirname "$0")" && exec ./sfa --fullscreen "$@"
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    is_win = os.name == "nt"
    default_build = os.path.join(ROOT, "build", "GF7E01", "cmake" if is_win else "../linux")
    ap.add_argument("--build-dir", default=os.path.normpath(default_build))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--no-archive", action="store_true")
    args = ap.parse_args()

    exe_name = "sfa.exe" if is_win else "sfa"
    exe = os.path.join(args.build_dir, exe_name)
    if not os.path.exists(exe):
        sys.exit(f"no {exe}: build first (see recomp/README.md)")
    plat = "windows-x64" if is_win else f"linux-{platform.machine() or 'x86_64'}"
    name = f"sfa-recomp-{plat}"
    dest = os.path.join(args.out, name)
    if os.path.exists(dest):
        shutil.rmtree(dest)
    os.makedirs(dest)
    shutil.copy2(os.path.join(ROOT, "recomp", "fonts", "font-licenses.txt"), dest)  # the built-in fonts' license

    shutil.copy2(exe, dest)
    if is_win:
        dll = os.path.join(args.build_dir, "SDL2.dll")
        if not os.path.exists(dll):
            sys.exit(f"no {dll} next to the executable")
        shutil.copy2(dll, dest)
    else:
        out_exe = os.path.join(dest, exe_name)
        if shutil.which("strip"):  # drop debug info: ~300 MB -> a few tens of MB
            subprocess.run(["strip", out_exe], check=True)
        launcher = os.path.join(dest, "sfa.sh")
        with open(launcher, "w", newline="\n") as f:
            f.write(LAUNCHER)
        os.chmod(launcher, os.stat(launcher).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    help_text = subprocess.run([os.path.join(dest, exe_name), "--help"], capture_output=True, text=True,
                               cwd=dest).stdout.strip()
    run = "sfa.exe" if is_win else "./sfa.sh (fullscreen; add it to Steam as a non-Steam game) or ./sfa"
    with open(os.path.join(dest, "PLAY.txt"), "w", newline="\n" if not is_win else "\r\n") as f:
        f.write(PLAY_TXT.format(platform=plat, run=run, help=help_text))

    print(f"folder: {dest}")
    if args.no_archive:
        return
    if is_win:
        archive = dest + ".zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            for base, _, files in os.walk(dest):
                for f in files:
                    full = os.path.join(base, f)
                    z.write(full, os.path.relpath(full, args.out))
    else:
        archive = dest + ".tar.gz"
        with tarfile.open(archive, "w:gz") as t:
            t.add(dest, arcname=name)
    print(f"archive: {archive} ({os.path.getsize(archive) // (1 << 20)} MB)")


if __name__ == "__main__":
    main()
