# LegacyDJ
Hook dll for IIDX on a legacy (LDJ) cabinet. It drives the cabinet's ticker, spotlights and neon through a COM port, and in TDJ mode makes the cabinet's effector faders and card reader keypads work.

Formerly TickerHookSerial, a fork of [TickerHook](https://github.com/Radioo/TickerHook).

# Usage
Copy the appropriate `legacydj.dll` into your base directory, then load it with your tools, for example:

inject.exe:  
`inject iidxhook1.dll legacydj.dll bm2dx.exe --config iidxhook-09.conf %*`  
launcher.exe:  
`launcher -H 134217728 -B iidxhook9.dll -K legacydj.dll bm2dx.dll --config iidxhook-28.conf %*`  
spice.exe/spice64.exe:  
`spice64 -k legacydj.dll`  

Then create `legacydj.conf` in the same folder (see [legacydj.conf.example](legacydj.conf.example) for every option):

```
PORT=COM1
MODE=RELAY
FADERS=ON
KEYPAD=ON
```

Everything the dll does is also written to `legacydj.log` in the game folder.

# Relay board mode
With `MODE=RELAY` (the default) the dll takes the place of a BIO2 and talks to the sub IO ("relay board") of the cabinet directly, through a serial connection. **[How to connect the relay board](docs/relay-board.md)** covers the adapter, finding the pins, wiring, testing and troubleshooting.

The dll scrolls long text itself and centres the texts listed with `STATIC_TEXT`. Spotlights and neon blink or stay on/off according to `SPOTLIGHT_INTERVAL` and `NEON_INTERVAL`.

| Setting | Default | |
| :--- | :---: | :--- |
| `SCROLL_INTERVAL` | 300 | Milliseconds per scroll step |
| `SCROLL_GAP` | 12 | Spaces between the end and the start of scrolling text |
| `SPOTLIGHT_INTERVAL` | 1000 | Milliseconds on, then the same off. `0` always off, `-1` always on |
| `NEON_INTERVAL` | 1000 | Same as above, for the neon |
| `STATIC_TEXT` | see example | One per line. The first one replaces the built-in list |
| `SEND_INTERVAL` | 8 | Milliseconds between packets to the relay board |

# Effector faders
With `FADERS=ON` (the default) the cabinet's five physical faders set the seven effector sliders on the TDJ subscreen. A fader only takes over when it is moved, so the sliders can still be changed on the touchscreen in between. `FADERS=LOG` only logs, `FADERS=OFF` leaves the effector alone. See **[effector-faders.md](docs/effector-faders.md)**.

| Setting | Default | |
| :--- | :---: | :--- |
| `FADERS` | `ON` | `ON`, `LOG` or `OFF` |
| `FADER1` .. `FADER5` | see below | Sliders a fader sets, comma separated: `vefx low hi filter play_volume low_mid hi_mid` |
| `FADER_INVERT` | 0 | 1 if the faders work the wrong way round |
| `FADER_DEADBAND` | 3 | How far (0-7) a fader must move past a step boundary before its value changes |

By default fader 1 sets VEFX, 2 sets Low-EQ and Low-Mid-EQ, 3 sets Hi-EQ and Hi-Mid-EQ, 4 sets Filter and 5 sets Play volume. The faders need relay board mode.

# Card reader keypads
With `KEYPAD=ON` (the default) the keypads on the card readers work in TDJ mode like the subscreen touch keypad: digits are digits (PIN entry included), `00` erases and `.` shows or hides the touch keypad. `KEYPAD=LOG` only logs the key presses, `KEYPAD=OFF` leaves the keypads alone. The game has to read the card readers itself (no spice I/O hooks). See **[card-keypads.md](docs/card-keypads.md)**.

The effector and keypad parts need a 64-bit game with a TDJ mode. Their game addresses are found with byte signatures and the game's own data, not hardcoded; they are known to work with IIDX 32 and IIDX 34.

# Arduino mode
`MODE=ARDUINO` sends each ticker text as a line to the [iidx-BiTwinkIO](https://github.com/dumcatt/iidx-BiTwinkIO) Arduino sketch, at `BAUD`. There are no faders in this mode.

# Building
**Visual Studio:** open `LegacyDJ.sln`. The project expects [MinHook](https://github.com/TsudaKageyu/minhook) under `C:\Soft\minhook` (headers in `include`, libraries in `build\VC17\lib\<Configuration>`).

**MinGW:** with [MSYS2](https://www.msys2.org/) and its `mingw-w64-ucrt-x86_64-gcc` (64-bit) or `mingw-w64-i686-gcc` (32-bit) package installed, run

```
powershell -ExecutionPolicy Bypass -File build-mingw.ps1
powershell -ExecutionPolicy Bypass -File build-mingw.ps1 -Arch x86
```

This builds MinHook from source along with the dll, plus `relaytest.exe` (relay board without the game) and, for 64-bit, `sigtest.exe` (checks the effector and keypad signatures against a bm2dx.dll). Pass `-MinHook <folder>` to point it at a MinHook source checkout; by default it looks for the copy bundled with spice2x next to this repository. Output goes to `dist\x64` or `dist\x86`; everything is linked statically.

## TODO
- game controlled neon and spotlights
- a better way to detect static texts instead of hardcoding them
