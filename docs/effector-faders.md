# Physical effector faders in TDJ mode

In TDJ mode the effector lives on the subscreen as seven touch sliders: VEFX, Low-EQ, Low-Mid-EQ, Hi-Mid-EQ, Hi-EQ, Filter and Play volume. A legacy cabinet has five physical faders, which the relay board reports to legacydj. With `FADERS=ON` they set the touch sliders:

| Physical fader | Subscreen sliders (default) |
| :---: | :--- |
| 1 | VEFX |
| 2 | Low-EQ and Low-Mid-EQ |
| 3 | Hi-EQ and Hi-Mid-EQ |
| 4 | Filter |
| 5 | Play volume |

`FADER1` to `FADER5` change this mapping.

A fader only writes when it moves to a different step. So you can set Low-EQ and Low-Mid-EQ together with fader 2, then fine-tune Low-Mid-EQ on the touchscreen, and the touch value stays until fader 2 is moved again. Where the faders rest when the game starts is not applied.

`FADER_DEADBAND` keeps a fader that rests right on the edge between two steps from flickering between them (and so from overwriting touch changes). Raise it if a fader you did not touch still shows up in the log as `fader N -> ...`.

**Status:** confirmed on a cabinet with IIDX 32 in TDJ mode: all five faders set their sliders, and the touch knobs follow while the effector panel is shown.

## What the game does (IIDX 32)

Everything is located with byte signatures (see [GameLayout.h](../LegacyDJ/GameLayout.h)); the struct offsets are read out of the matched instructions where they appear in them. They have matched two different IIDX 32 builds so far.

* **Slider values:** an `int[7]` in the game's state object, 0 to 15, in the order vefx, low, hi, filter, play volume, low mid, hi mid. The first five are the same order as the physical faders. The EQs start at 7, the middle; filter and play volume at 15.
* **Slider update** (one call per slider per frame, 120 frames a second): on a TDJ cabinet it reads the value from that array. On an LDJ cabinet it asks the BI2A I/O instead, and the two mid EQs do not exist.
* **Subscreen effector panel:** only runs while it is on screen, and then copies every touch widget into the array every frame (array value = 15 - widget value). So a fader move is written into the array and, while the panel is shown, into the touch widget too, which also moves the knob. When the panel opens it builds its widgets from the array.
* **Touch widget:** value 0..15 (upside down compared to the array), knob position 0..1 (value / 15), and where the current drag started.

In bm2dx.dll 2025-08-18 (ReleaseTDJ): slider update at `0x98C1A0`, game state pointer at `0xA7D67F8` with the sliders at `+0x3B8`, panel at `0x7B8380` (shown flags `+0xF8`/`+0xF9`, slider entries `+0xA0`, widget value `+0x6C`).

## Checking the signatures without the game

`sigtest.exe` (built with the 64-bit dll) maps a bm2dx.dll without running any of it and prints what it finds:

```
sigtest.exe D:\LDJ\modules\bm2dx.dll
```

It ends with `sliders OK, panel OK` when both parts are found. Results for other game versions are welcome.

## The log

The console and `legacydj.log` show:

* `physical` the faders as 0 to 15 and as raw bytes
* `fader N -> ...` a fader move that was sent to the game
* `game` the slider values the game uses
* `panel` / `touch` whether the effector panel is shown, and its touch widgets
* `status` every 10 seconds: how often the hooked functions run, and how many touch widgets were moved by the faders
