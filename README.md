# Bytemania

A custom firmware for the M-VAVE FM-1, built on [Dinghy](https://github.com/zvenson/dinghy).

A bytebeat synth: a one-line formula of `t` makes an 8-bit sample each step of `t`, and the note sets how fast
`t` runs (middle C is 8 kHz, the classic bytebeat rate).

## Controls

| | |
| --- | --- |
| Keys | play (mono, last note wins). With SEL lit, each key picks a formula instead |
| SEL | switch the keys between playing and picking formulas |
| FX | switch the view: scope (the waveform) or raster (a row per 256 steps of `t`, coloured by value) |
| GLO | open or close the CC screen |
| PRESETS | the patch: the built-in formulas, then your saved slots |
| SAVE | save what plays to one of 64 user slots |
| EDIT | open or close the formula editor |
| KNOB1-4 | the variables `a`-`d` (0-255, faster when turned fast). Hold HOME for `e`-`h` |
| SEQ | tap to switch the loop on or off. Hold it and the knobs set the loop (below) |
| SELECT | glide (0-10) |
| OCT- / OCT+ | octave |
| MASTER | volume |
| USB-MIDI | notes in, any channel, with velocity. Pitch bend is +-2 semitones |

## Loop

Hold SEQ and the knobs set the loop:

| | |
| --- | --- |
| KNOB1 START | where it starts, in raster rows (256 steps of `t`) |
| KNOB2 FINE | plus 0-255 steps |
| KNOB3 LEN | its length: 16 to 65536 steps, in powers of two |
| KNOB4 MODE | off, loop, or ping (forwards then backwards) |

A note plays from `t = 0` until it reaches the start, then loops. A quick tap on SEQ switches the loop on or off.
The loop settings are also CC targets.

## User slots

SAVE opens the save screen. Pick a slot with SELECT or PRESETS, press SAVE again to save, or HOME to cancel. A slot
keeps the whole patch: the formula, `a`-`h`, the loop and glide. Saving waits until nothing is playing, since writing
to flash briefly stops the sound.

## Editor

EDIT opens the formula editor. The keys type: the screen shows a keyboard with each key's token, and holding FX
switches to a second layer (`/ % ~ ! < > ? :`, the two-character operators, hex and space). Every change compiles
straight away. If it compiles you hear it, otherwise the screen shows the error and where it is, and the last version
that compiled keeps playing.

| | |
| --- | --- |
| Keys | type a token at the cursor |
| FX (held) | the second layer of tokens |
| SELECT | move the cursor |
| OCT- / OCT+ | delete before / after the cursor |
| PLAY | a held C4 to listen with, on or off |
| ARP | replace the formula with a random one |
| KNOB1-4 | the variables, as on the main screen |
| SAVE | save to a user slot |
| EDIT | back to the main screen |

An edit that isn't saved shows a `*` after the name, and keeps playing until you pick another patch. Built-in
formulas can't be overwritten, so saving an edited one saves a copy to a user slot.

ARP makes a random formula from the pieces classic bytebeats are built from, with its numbers as the variables `a`-`d`
so the knobs reach them. Each one is checked before you hear it: anything silent, stuck or noise is thrown away and
another made.

## CC mapping

GLO opens the CC screen: 12 slots, each mapping a MIDI CC (any channel) to a parameter (`a`-`h`, glide, volume or the loop settings)
over a min-max range. Setting min above max inverts it. Mappings are saved.

| | |
| --- | --- |
| SELECT | the slot |
| KNOB1 | its CC, or off |
| KNOB2 | its parameter (resets the range to the parameter's full range) |
| KNOB3 / KNOB4 | min / max |
| REC | learn: the next CC that comes in goes to the selected slot |

## Formulas

C expressions of `t` and the variables `a`-`h`, on 32-bit signed integers. The low 8 bits are the sample.

```
t  a..h  123  0x7F  ( )
?:  ||  &&  |  ^  &  == !=  < <= > >=  << >>  + -  * / %  unary - ~ !
```

Where C would be undefined, overflow wraps, shifts use their count `& 31`, and `x / 0` and `x % 0` are 0.

The 22 built-in formulas are classic bytebeats from viznut's
[Some deep analysis of one-line music programs](http://countercomplex.blogspot.com/2011/10/some-deep-analysis-of-one-line-music.html)
(2011) and the [bytebeat composer](https://dollchan.net/bytebeat)'s library, with some of their numbers turned into
variables. The defaults give the original formula.

<p align="center"><img src="docs/screen.png" width="240" alt="Bytemania's screen"></p>

## Layout

```
firmware/src/bytemania.c  <- the app: sound, screen, keys
firmware/src/bytebeat.c   <- the formula compiler and interpreter
firmware/src/formulas.h   <- the built-in formulas
firmware/src/randbeat.c   <- the random formulas
firmware/src/platform.c   <- Dinghy's plumbing (boot, USB, updates, audio ISR, settings)
firmware/src/*.c, hal/    <- Felucca's platform, as SLOOP carries it
```

[docs/API.md](docs/API.md) covers what `platform.c` gives the app. [docs/RULES.md](docs/RULES.md) covers the chip's
rules and what the build checks.

## Setup

Linux on x86-64 (or macOS / Windows with Docker or WSL), Python 3 with Pillow, the JieLi toolchain and the JieLi AC79
SDK:

```sh
pip3 install Pillow mido python-rtmidi
tools/get_toolchain.sh
git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
    https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
```

Set `JIELI_TOOLCHAIN` and `AC79_SDK` if they live elsewhere.

## Build and run

```sh
tests/run_tests.sh                                  # unit tests for the formula compiler
tests/run_host.sh                                   # runs bytemania.c on your computer -> build/host/ (WAV + screen PNGs)
./build.sh                                          # -> build/bytemania.fwsc
python3 tools/fm1_install.py build/bytemania.fwsc   # install over USB
```

If a build won't start, hold **OCT-** while switching on to get the USB rescue, then install a working build or go
back to the official firmware from the [FM-1 Firmware Hub](https://thefirmwarehub.com). Installing firmware is at your
own risk.

## Releases

Push a version tag (`git tag v0.1 && git push origin v0.1`). The release workflow builds `bytemania-<version>.fwsc`
with the hub's shared [fm1-build](https://github.com/zvenson/fm1-build) workflow, attests it and attaches it to the
release. Don't re-upload or rebuild the file by hand.

## Licence

GPL-3.0-only. See [LICENSING.md](LICENSING.md) for who made what.
