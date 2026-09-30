# TickerHookSerial
Hook dll for capturing ticker text from IIDX. Drives the cabinet's ticker, spotlights and neon through a COM port.

This is a fork of [TickerHook](https://github.com/Radioo/TickerHook).

# Usage
Copy appropriate .dll file into your base directory, then load the dll using your tools, examples below:

inject.exe:  
`inject iidxhook1.dll TickerHook.dll bm2dx.exe --config iidxhook-09.conf %*`  
launcher.exe:  
`launcher -H 134217728 -B iidxhook9.dll -K TickerHook.dll bm2dx.dll --config iidxhook-28.conf %*`  
spice.exe/spice64.exe:  
`spice64 -k TickerHook.dll`  

After this, configure the COM port by creating a `tickerhook.conf` file (see [tickerhook.conf.example](tickerhook.conf.example) for every option).

```
PORT=COM1
MODE=RELAY
```

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

# Arduino mode
`MODE=ARDUINO` sends each ticker text as a line to the [iidx-BiTwinkIO](https://github.com/dumcatt/iidx-BiTwinkIO) Arduino sketch, at `BAUD`.

# Building
**Visual Studio:** open `TickerHook.sln`. The project expects [MinHook](https://github.com/TsudaKageyu/minhook) under `C:\Soft\minhook` (headers in `include`, libraries in `build\VC17\lib\<Configuration>`).

**MinGW:** with [MSYS2](https://www.msys2.org/) and its `mingw-w64-ucrt-x86_64-gcc` (64-bit) or `mingw-w64-i686-gcc` (32-bit) package installed, run

```
powershell -ExecutionPolicy Bypass -File build-mingw.ps1
powershell -ExecutionPolicy Bypass -File build-mingw.ps1 -Arch x86
```

This builds MinHook from source along with the dll, and also builds `relaytest.exe` for testing the relay board without the game. Pass `-MinHook <folder>` to point it at a MinHook source checkout; by default it looks for the copy bundled with spice2x next to this repository. Output goes to `dist\x64` or `dist\x86`; everything is linked statically.

## TODO
- make the faders work somehow?
- game controlled neon and spotlights
- a better way to detect static texts instead of hardcoding them