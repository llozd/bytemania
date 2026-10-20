#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The README's screenshots, from host mode: each screen is a short script (tests/host.c's events) ending in a "shot",
saved 2x as docs/screens/NAME.png. Run it after changing what a screen looks like.

  python3 tests/screenshots.py"""
import subprocess
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host/shots"
DOCS = ROOT / "docs/screens"
# buttons and encoders, as platform.c numbers them
OCT_DOWN, FX, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC = 0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13
SELECT, KNOB1, KNOB2, KNOB3, KNOB4, PRESETS = 0, 2, 3, 4, 5, 6
C4 = 1 << 7                                     # key 7


class Script:
    def __init__(self):
        self.t, self.lines = 100, []

    def ev(self, what, gap=100):
        self.lines.append(f"{self.t} {what}")
        self.t += gap
        return self

    def btn(self, b): return self.ev(f"btn {b}")
    def hold(self, mask): return self.ev(f"hold {mask:#x}")
    def enc(self, e, n): return self.ev(f"enc {e} {n}")
    def keys(self, mask): return self.ev(f"keys {mask:#x}")

    def type(self, text):                       # the name screen: key k types 'A' + k, the top key (26) a space
        for ch in text:
            self.ev(f"keys {1 << (26 if ch == ' ' else ord(ch) - 65):#x}", 10).ev("keys 0", 10)
        return self

    def delete(self, n):
        for _ in range(n): self.ev(f"btn {OCT_DOWN}", 10)
        return self

    def save_as(self, name, old_len):           # SAVE, SAVE (the slot it offers), the name, SAVE
        return self.btn(SAVE).btn(SAVE).delete(old_len).type(name).btn(SAVE)

    def shot(self, wait=1200):
        self.t += wait
        self.ev("shot")
        self.lines.append(f"{self.t} end")
        return "\n".join(self.lines) + "\n"


SHOTS = {
    "main": Script().keys(C4).shot(),
    "raster": Script().btn(FX).keys(C4).shot(2400),
    "waves": Script().enc(PRESETS, -7).keys(C4).shot(),                       # 11 FIRST -> 04 SINE
    "knobs-eh": Script().hold(1 << HOME).enc(KNOB1, 40).enc(KNOB2, 7).enc(KNOB3, 3).enc(KNOB4, 90).keys(C4).shot(),
    "dual": Script().hold(1 << LFO).hold(0).keys(C4).hold(1 << LFO).enc(KNOB1, 12).enc(KNOB2, 40).shot(),
    "multi": Script().enc(PRESETS, -10).hold(1 << LFO).hold(0).hold(1 << LFO).hold(0).hold(1 << LFO).hold(0)
                     .btn(PLAY).enc(PRESETS, 1).enc(KNOB3, -64).keys(C4).shot(),
    "loop": Script().keys(C4).hold(1 << SEQ).enc(KNOB4, 2).enc(KNOB3, -2).enc(KNOB1, 1).shot(2400),
    "cc": Script().btn(GLO).enc(KNOB1, 75).enc(SELECT, 1).enc(KNOB1, 72).enc(SELECT, 1).enc(KNOB1, 2)
                  .enc(KNOB4, -128).enc(SELECT, -1).ev("midi 0xB0 74 100").ev("midi 0xB0 71 30").shot(300),
    "cc-learn": Script().btn(GLO).btn(REC).shot(300),
    "edit": Script().btn(EDIT).btn(ARP).btn(PLAY).shot(),
    "edit-fx": Script().btn(EDIT).hold(1 << FX).shot(300),
    "name": Script().btn(SAVE).btn(SAVE).delete(5).type("ACID LEAD").shot(300),
    "save": Script().save_as("ACID LEAD", 5).enc(PRESETS, 4).save_as("SOFT SINE", 4).btn(SAVE).shot(300),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    DOCS.mkdir(parents=True, exist_ok=True)
    exe = OUT / "bytemania"
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DDINGHY_HOST", '-DAPP_VER="dev"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/bytemania.c"], cwd=ROOT, check=True)
    for name, text in SHOTS.items():
        script = OUT / f"{name}.txt"
        script.write_text(text)
        subprocess.run([str(exe), str(OUT / name), str(script)], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        t = int(text.splitlines()[-2].split()[0])  # the shot's time
        Image.open(OUT / f"{name}-{t}.ppm").resize((480, 480), Image.NEAREST).save(DOCS / f"{name}.png", optimize=True)
        print("screen ->", DOCS / f"{name}.png")


if __name__ == "__main__":
    main()
