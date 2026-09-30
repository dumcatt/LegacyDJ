# Connecting the relay board to a PC

On a legacy cabinet, the 16 segment ticker, the spotlights and the neon are not driven by the BIO2 itself. The BIO2 talks RS-232 to the sub IO board (the "relay board", Konami PWB116784480000), which passes everything on over RS-422 to the ticker board and from there to the top lights:

```
PC --USB--> BIO2 --RS-232--> relay board --RS-422--> ticker board --RS-422--> spotlights / neon
```

With `MODE=RELAY`, tickerhook takes the place of the BIO2 on that RS-232 link. All you need is a RS-232 Serial interface.

## What you need

* A RS-232 interface. It has to be real RS-232, not a 3.3v/5v "TTL serial" module: I'm using my motherboard's serial header but another great option are adapters with prolific chipsets such as the Ugreen USB To RS-232
* Three wires from the adapter to the relay board. Dupont jumpers work but a female DB9 breakout is cleaner.

## Wiring

**Unplug the BIO2 from the relay board first.** (Labled COM on the PC side)

Keep the relay board powered the way it normally is in the cabinet, and leave its cables to the ticker board and the top lights connected.

| Adapter DB9 pin | Relay board Harness (COM) |
| :---: | :--- |
| 2 (RXD) | 1 |
| 3 (TXD) | 3 |
| 5 (GND) | 2 |

Getting 2 and 3 the wrong way round does no harm with RS-232; the board simply will not answer. Swap them and try again.

## Using it with the game

Copy `tickerhook.dll` and your `tickerhook.conf` into the game folder and load the dll as described in the README. The console window it opens says:

```
Serial port COM20 opened successfully at 115200 baud.
Relay board mode: scroll 300 ms, gap 12, spotlights 1000, neon -1, 8 static texts
```

and, if the board stops answering for more than two seconds, `Relay board is not answering on COM20`.

## Troubleshooting

| What you see | Likely cause |
| :--- | :--- |
| `Error opening serial port` | Wrong `PORT`, or another program (a serial monitor, a second copy of the tool) has the port open |
| `replies 0` / `Relay board is not answering` | TX and RX swapped, ground missing, relay board not powered, or a TTL adapter instead of an RS-232 one |
| Replies come back but the ticker stays dark | The RS-422 cable from the relay board to the ticker board is loose |
| Garbage on the ticker | Another device still drives the line, usually the BIO2 is still plugged in |

## Limitations

* The relay board also reads the effector panel's five faders (and probably its buttons). tickerhook reads the faders, but does not pass them on to the game.
* The effector button lamps are always sent as off.
* Which bit in the spotlight byte belongs to which spotlight is taken from MAME's twinkle driver and has not been checked on a cabinet one spotlight at a time.

## Protocol

For reference, as captured between a BIO2 on BI2A firmware and the relay board: 115200 baud 8N1, Konami ACIO framing, one request about every 8 ms, no handshake before the first one.

* Request: `AA 00 01 12 00 0C <12 bytes> <checksum>`. The 12 bytes, all active low: effector button lamps, the 9 ticker characters (ASCII), spotlights, neon.
* Reply: `AA AA 80 01 12 00 0D <13 bytes> <checksum>`. Bytes 7-11 are faders 1-5 (`01`-`FF`); the rest are not identified yet.

Every byte after the leading `AA` that is `AA` or `FF` is sent as `FF` followed by its complement. The checksum is the sum of the bytes from the address on.
