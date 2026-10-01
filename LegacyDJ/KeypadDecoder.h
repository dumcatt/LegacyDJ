#pragma once
// Card reader keypad presses, read out of the status libaio-iob.dll's
// AIO_IOB_ICCA::GetDeviceStatus hands the game, and what each key does in
// TDJ mode. No Windows dependencies, so this can be tested on its own.
//
// Settings (legacydj.conf):
//   KEYPAD=ON|LOG|OFF   ON passes the keypads to the game, LOG only logs,
//                       OFF hooks nothing

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace keypad {

// AIO_IOB_ICCA::DEVSTATUS, as libaio-iob's reader status parser fills it.
// Each key event is a code (which key), a 3-bit sequence number that counts
// up with every press, and a valid flag. Event 0 is the newest, event 1 the
// one before it. The 12 key states are flags, one per key code.
const int DEVSTATUS_SIZE = 0x34;
const int EVENT0 = 0x0E;
const int EVENT1 = 0x11;
const int KEY_STATES = 0x14;
const int KEY_CODES = 12;
// keypad started, both key events and the key states
const int KEY_BYTES = 0x0D;
const int KEY_BYTES_SIZE = KEY_STATES + KEY_CODES - KEY_BYTES;

// Key code -> the game's key number (0..9 digits, 10 "00", 11 decimal);
// the same table the game uses for the reader keypad on LDJ cabinets
const int GAME_KEY[KEY_CODES] = {0, 1, 4, 7, 10, 2, 5, 8, 11, 3, 6, 9};
const int CODE_00 = 4;
const int CODE_DECIMAL = 8;
// On TDJ both "00" and decimal are the touch keypad's erase button
const int GAME_KEY_ERASE_A = 10;
const int GAME_KEY_ERASE_B = 11;

const char* const KEY_NAMES[KEY_CODES] = {"0", "1", "4", "7", "00", "2", "5", "8", ".", "3", "6", "9"};

enum class keypad_mode { off, log, on };

struct keypad_config {
    keypad_mode mode = keypad_mode::on;
};

// Applies the KEYPAD* settings (key already upper case); returns the problems found
inline std::vector<std::string> parse_keypad_settings(
    const std::vector<std::pair<std::string, std::string>>& settings, keypad_config& config) {
    std::vector<std::string> errors;
    for (const auto& kv : settings) {
        std::string value;
        for (char c : kv.second) {
            if (c != ' ' && c != '\t') value += (char)toupper((unsigned char)c);
        }
        if (kv.first == "KEYPAD") {
            if (value == "ON") config.mode = keypad_mode::on;
            else if (value == "LOG") config.mode = keypad_mode::log;
            else if (value == "OFF") config.mode = keypad_mode::off;
            else errors.push_back("Invalid KEYPAD: " + kv.second);
        } else {
            errors.push_back("Unknown setting: " + kv.first);
        }
    }
    return errors;
}

// What a key code does in TDJ mode
inline bool is_toggle(int code) { return code == CODE_DECIMAL; }

// Whether a press of key `code` should count as the game key `game_key`
inline bool code_matches(int code, int game_key) {
    if (code < 0 || code >= KEY_CODES || is_toggle(code)) return false;
    if (code == CODE_00) return game_key == GAME_KEY_ERASE_A || game_key == GAME_KEY_ERASE_B;
    return GAME_KEY[code] == game_key;
}

// New key presses of one reader, from successive statuses
class reader_decoder {
public:
    // Returns the key codes pressed since the last status, oldest first. The
    // first status only notes where the sequence numbers stand.
    std::vector<int> update(const uint8_t* status) {
        std::vector<int> pressed;
        event e0 = read_event(status + EVENT0);
        event e1 = read_event(status + EVENT1);
        if (!started) {
            started = true;
            last_seq = e0.valid ? e0.seq : -1;
            return pressed;
        }
        if (!e0.valid || e0.seq == last_seq) return pressed;
        // two presses between polls: event 1 is the one before event 0
        if (e1.valid && e1.seq != last_seq && e1.seq == ((e0.seq + 7) & 7)) pressed.push_back(e1.code);
        pressed.push_back(e0.code);
        last_seq = e0.seq;
        return pressed;
    }

    static bool held(const uint8_t* status, int code) {
        return code >= 0 && code < KEY_CODES && status[KEY_STATES + code] != 0;
    }

private:
    struct event {
        bool valid;
        int seq;
        int code;
    };

    static event read_event(const uint8_t* p) {
        return event{p[2] != 0, p[1] & 7, p[0] & 0x0F};
    }

    bool started = false;
    int last_seq = -1;
};

// One reader's keys as the game should see them: one new press per status
// poll (the way the LDJ reader code hands them out), the rest queued
class reader_keys {
public:
    // Call once per status poll. Returns the code of the press for this poll,
    // -1 for none. A decimal press is returned too, but is not a game key.
    int poll(const uint8_t* status) {
        for (int code : decoder.update(status)) {
            if (code >= 0 && code < KEY_CODES) queue.push_back(code);
        }
        for (int c = 0; c < KEY_CODES; c++) held_now[c] = reader_decoder::held(status, c);
        if (queue.empty()) {
            current = -1;
        } else {
            current = queue.front();
            queue.pop_front();
        }
        return current;
    }

    // Game key pressed in this poll
    bool pressed(int game_key) const { return code_matches(current, game_key); }

    // Game key held down past the poll it was pressed in
    bool held(int game_key) const {
        for (int c = 0; c < KEY_CODES; c++) {
            if (held_now[c] && c != current && code_matches(c, game_key)) return true;
        }
        return false;
    }

    int current_code() const { return current; }

private:
    reader_decoder decoder;
    std::deque<int> queue;
    int current = -1;
    bool held_now[KEY_CODES] = {false};
};

}  // namespace keypad
