# Card reader keypads in TDJ mode

In TDJ mode the PIN and every other number is entered on the subscreen's touch keypad; the keypads on a legacy cabinet's card readers do nothing. With `KEYPAD=ON` legacydj passes them on:

| Reader key | In the game |
| :---: | :--- |
| 0 - 9 | The same digit on that player's touch keypad |
| 00 | Erase |
| . | Shows or hides that player's touch keypad on the home screen |

Each reader counts for its own player (P1 / P2). The touch keypad keeps working as before.

This needs the game to talk to the card readers itself, through its own libraries (no spice I/O hooks).

**Status:** confirmed on a cabinet with IIDX 34 in TDJ mode: PIN entry, game menus and options all take the reader keypads, and `.` shows and hides the touch keypad. The signatures also resolve in IIDX 32 (checked offline with `sigtest`).

## What the game does (IIDX 32)

* **Reading the readers:** on an LDJ cabinet the game reads the readers through `libacio` and takes the keypad from them. On a TDJ cabinet it reads them through `libaio-iob.dll` (`AIO_IOB_ICCA`) and uses them for cards only. That library still decodes the keypad into the reader status it hands the game: the two latest key presses (key, a sequence number that counts up with each press, valid) and which keys are held.
* **Asking for keys:** the game asks "was key K pressed for player P" (and a twin for "held"). Number entry (the PIN, and other numbers typed in) asks a third function instead: "was key K pressed on the touch keypad that is open". On TDJ these answer from the touch keypad's buttons; on LDJ they ask the reader, the third one by passing the question on to the first. Keys are 0-9, 10 ("00") and 11 (decimal); on TDJ 10 and 11 are both the touch keypad's erase button.
* **Showing the touch keypad:** the subscreen home screen has a show/hide keypad button per player (in the screen corner). In the frame that button reads as pressed, the game opens the keypad if it is closed (and allowed on this screen) or closes it if it is open. The open keypad has a second button that can only close it.

So legacydj hooks the reader status export in `libaio-iob.dll` to pick up new presses, makes the three key functions also answer yes for a key pressed (or held) on that player's reader, and for `.` sets the player's show/hide keypad button to pressed for one frame.

The two key functions are found through the key -> touch button table the game builds a map from at startup (digit 0 is button 9, and so on): the code that loads the table stores the map, and the key functions read that map right at their start. Whether a function is "pressed" or "held" follows from how it tests the button state; the open keypad one is the function that reads the map and, on LDJ, jumps straight to "pressed". IIDX 34 checks more touch buttons per key than IIDX 32, so a plain signature of the function itself does not fit both. The home screen is found from its button checks. All of it is in [KeypadLayout.h](../LegacyDJ/KeypadLayout.h); `sigtest.exe` checks it along with the effector signatures.

| | IIDX 32 (2025-08-18) | IIDX 34 |
| :--- | :---: | :---: |
| Key pressed | `0xA84750` | `0xA67BE0` |
| Key held | `0xA844B0` | `0xA67700` |
| Key pressed on the open keypad | `0xA83E90` | `0xA670E0` |
| Home screen update | `0x9DC8B0` | `0x9AC1F0` |

Show/hide keypad buttons `+0x18` / `+0x20`, button state `+0x60` in both.

## The log

* `reader Pn key ...` a key press on a reader and what it was passed on as
* `reader Pn status ...` the whole reader status whenever its key part changes (to check the layout on other game versions)
* `touch Pn game key ...` a key pressed on the touch keypad, showing which game key each touch button is
* `keypad Pn: pressed the show/hide keypad button` a `.` press reaching the home screen
* `keypad status` every 10 seconds: how often the readers are polled and the key functions asked
