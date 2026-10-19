# Licensing

Bytemania is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). You may use, study, change and share it. If you give someone a firmware built from it, give them its
complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The app `firmware/src/bytemania.c` as it came from [Dinghy](https://github.com/zvenson/dinghy), the wiring `platform.c`, the host mode `tests/`, the docs | zvenson |
| The changes to the app and the build since Dinghy | llozd |

## The built-in formulas

`firmware/src/formulas.h` holds classic bytebeat formulas as published in viznut's
[Some deep analysis of one-line music programs](http://countercomplex.blogspot.com/2011/10/some-deep-analysis-of-one-line-music.html)
(2011) and in the [bytebeat composer](https://dollchan.net/bytebeat)'s library. Each is credited on the screen and in
the file as its source credits it: viznut, Niklas Roy, miiro, stephth, Rrrola, droid, Mu6k, krcko, Blueberry, ryg,
Lord Graga, marmakoide, skurk, raer, erlehmann, and "several people" for the Forty-Two Melody.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Terminus font 8x16 (ter-u16n), rendered into `firmware/gen/felucca_font.h` | SIL OFL 1.1 | `assets/fonts/Terminus-LICENSE.txt` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## The logo

Dinghy's logo (the designburg signet) is not under the GPL and is not part of Bytemania.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments; "M-VAVE" and "FM-1" belong to their
owners. Bytemania is not made or endorsed by them, or by the authors of Dinghy, Felucca or SLOOP.
