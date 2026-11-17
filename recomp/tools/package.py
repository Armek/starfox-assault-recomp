"""Make a release folder (and archive) of the recompiled game, without any game data.

The folder holds the executable (plus SDL2.dll on Windows), a launcher on Linux, and PLAY.txt with the
setup steps. Players add their own disc image (.iso/.gcm) next to the executable; saves go to
user/ there. With --with-fonts, the replacement system fonts from user/fonts/ (and their license) are
included too.

usage: python recomp/tools/package.py [--build-dir DIR] [--out dist] [--with-fonts] [--no-archive]
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
     GAME DISC    finds an .iso next to the program by itself, or browse to yours
     SYSTEM FONT  text in dialogs and comm subtitles needs the GameCube system font: browse to
                  Dolphin's Sys/GC folder and it copies font_western.bin and font_japanese.bin
                  into user/fonts/ (or put them there yourself, or a dump of your own console's
                  boot ROM as user/GF7E01/ipl.bin){fonts_note}
     DISPLAY      window or fullscreen
     PLAY         start the game (START on a controller)
   Your choices are remembered in user/launcher.cfg. To skip the start screen, run with --play
   or name the disc image on the command line.

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
    ap.add_argument("--with-fonts", action="store_true", help="include user/fonts (Apache 2.0) and the license")
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
    os.makedirs(os.path.join(dest, "user", "fonts"))

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

    fonts_note = ""
    if args.with_fonts:
        src = os.path.join(ROOT, "user", "fonts")
        copied = [f for f in os.listdir(src) if f.endswith(".bin") or "license" in f.lower()] if os.path.isdir(src) else []
        for f in copied:
            shutil.copy2(os.path.join(src, f), os.path.join(dest, "user", "fonts"))
        if copied:
            fonts_note = (".\n                  This package already includes Dolphin's replacement fonts\n"
                          "                  (Apache 2.0; see user/fonts/font-licenses.txt)")

    help_text = subprocess.run([os.path.join(dest, exe_name), "--help"], capture_output=True, text=True,
                               cwd=dest).stdout.strip()
    run = "sfa.exe" if is_win else "./sfa.sh (fullscreen; add it to Steam as a non-Steam game) or ./sfa"
    with open(os.path.join(dest, "PLAY.txt"), "w", newline="\n" if not is_win else "\r\n") as f:
        f.write(PLAY_TXT.format(platform=plat, run=run, help=help_text, fonts_note=fonts_note))

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
                if not files and base != dest:  # keep the empty user/fonts folder
                    z.write(base, os.path.relpath(base, args.out) + "/")
    else:
        archive = dest + ".tar.gz"
        with tarfile.open(archive, "w:gz") as t:
            t.add(dest, arcname=name)
    print(f"archive: {archive} ({os.path.getsize(archive) // (1 << 20)} MB)")


if __name__ == "__main__":
    main()
