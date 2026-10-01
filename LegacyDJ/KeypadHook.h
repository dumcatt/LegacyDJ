#pragma once
// Card reader keypads in TDJ mode.
//
// In TDJ mode the game still reads the card readers (through libaio-iob.dll)
// and that library still decodes their keypads, but the game only takes keys
// from the subscreen touch keypad. This passes the reader keypads on:
//
//   AIO_IOB_ICCA::GetDeviceStatus   (libaio-iob.dll export) picks up each
//                                   reader's new key presses
//   key pressed / key held query    (bm2dx.dll) also answer yes for a key
//                                   pressed or held on that player's reader
//   home update                     (bm2dx.dll) the decimal key presses the
//                                   player's show/hide keypad button (the
//                                   one in the screen corner) for a frame
//
// Digits are digits, "00" is erase (the touch keypad's erase button answers
// to both 10 and 11), decimal shows/hides the touch keypad. With KEYPAD=LOG
// nothing is passed on, the presses are only logged.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include "minhook.h"
#include "KeypadDecoder.h"
#include "KeypadLayout.h"
#include "Log.h"

namespace keypad {

const char* const LIBAIO_IOB = "libaio-iob.dll";
const char* const GET_DEVICE_STATUS = "?GetDeviceStatus@AIO_IOB_ICCA@@QEBAXAEAUDEVSTATUS@1@@Z";

// A decimal press opens/closes the keypad only this soon after it happened
const uint64_t TOGGLE_PENDING_MS = 500;

class monitor {
public:
    static monitor& instance() {
        static monitor m;
        return m;
    }

    bool install(uint8_t* module, const keypad_config& settings) {
        config = settings;
        hooklog::open();
        hooklog::write("---- legacydj keypad started, KEYPAD=%s", config.mode == keypad_mode::on ? "ON" : "LOG");

        std::ostringstream report;
        resolve(module, game, report);
        std::string line;
        std::istringstream lines(report.str());
        while (std::getline(lines, line)) hooklog::write("%s", line.c_str());

        HMODULE aio = GetModuleHandleA(LIBAIO_IOB);
        void* get_status = aio ? (void*)GetProcAddress(aio, GET_DEVICE_STATUS) : nullptr;
        if (get_status == nullptr) {
            hooklog::write("Keypad: %s or its reader status function not found, keypads not hooked", LIBAIO_IOB);
            return false;
        }
        hook(get_status, (void*)get_status_hook, (void**)&get_status_orig, "reader status");
        if (game.keys_ok) {
            hook(game.key_pressed, (void*)key_pressed_hook, (void**)&key_pressed_orig, "key pressed query");
            if (game.key_held) hook(game.key_held, (void*)key_held_hook, (void**)&key_held_orig, "key held query");
            if (game.key_pressed_open) {
                hook(game.key_pressed_open, (void*)key_pressed_open_hook, (void**)&key_pressed_open_orig,
                     "open keypad query");
            }
        }
        if (game.toggle_ok) {
            hook(game.home_update, (void*)home_update_hook, (void**)&home_update_orig, "home update");
        }
        CreateThread(nullptr, 0, status_thread, this, 0, nullptr);
        return true;
    }

private:
    typedef void (*get_status_fn)(const void* self, uint8_t* out);
    typedef bool (*key_query_fn)(void* self, int player, int key);
    typedef void (*home_update_fn)(uint8_t* self);

    void hook(void* target, void* detour, void** original, const char* name) {
        MH_STATUS created = MH_CreateHook(target, detour, original);
        MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
        if (enabled == MH_OK) {
            hooklog::write("Keypad: hooked %s", name);
        } else {
            hooklog::write("Keypad: could not hook %s (%s)", name, MH_StatusToString(enabled));
        }
    }

    bool passing_on() const { return config.mode == keypad_mode::on; }

    // Readers are told apart by where the game keeps their status: P1's
    // buffer comes before P2's. Each buffer gets a slot the first time it is
    // seen; the slot's player follows from the two addresses. Caller holds lock.
    int slot_of(const uint8_t* out) {
        for (int i = 0; i < 2; i++) {
            if (status_slot[i] == out) return i;
        }
        for (int i = 0; i < 2; i++) {
            if (status_slot[i] == nullptr) {
                status_slot[i] = out;
                return i;
            }
        }
        if (!warned_extra_reader) {
            warned_extra_reader = true;
            hooklog::write("Keypad: a third reader status buffer showed up, ignoring it");
        }
        return -1;
    }

    int player_of_slot(int slot) const {
        const uint8_t* other = status_slot[1 - slot];
        return other != nullptr && other < status_slot[slot] ? 1 : 0;
    }

    // The reader keys of a player, nullptr while that reader has not been seen
    const reader_keys* keys_of(int player) const {
        for (int i = 0; i < 2; i++) {
            if (status_slot[i] != nullptr && player_of_slot(i) == player) return &keys[i];
        }
        return nullptr;
    }

    // Reader status, on whichever thread the game polls the readers
    static void get_status_hook(const void* self, uint8_t* out) {
        get_status_orig(self, out);
        monitor& m = instance();
        std::lock_guard<std::mutex> lock(m.lock);
        int slot = m.slot_of(out);
        if (slot < 0) return;
        int p = m.player_of_slot(slot);
        m.polls[p]++;

        uint8_t* last = m.last_status[slot];
        if (memcmp(last + KEY_BYTES, out + KEY_BYTES, KEY_BYTES_SIZE) != 0) {
            // key bytes changed: show the whole status, to check the layout
            char hexdump[DEVSTATUS_SIZE * 3 + 1];
            for (int i = 0; i < DEVSTATUS_SIZE; i++) snprintf(hexdump + i * 3, 4, "%02X ", out[i]);
            hooklog::write("reader P%d status %s", p + 1, hexdump);
        }
        memcpy(last, out, DEVSTATUS_SIZE);

        int code = m.keys[slot].poll(out);
        if (code < 0) return;
        if (is_toggle(code)) {
            m.toggle_ms[p] = GetTickCount64();
            hooklog::write("reader P%d key %s -> show/hide keypad%s", p + 1, KEY_NAMES[code],
                           m.passing_on() && m.game.toggle_ok ? "" : " (not passed on)");
        } else {
            hooklog::write("reader P%d key %s -> game key %s%s", p + 1, KEY_NAMES[code],
                           code == CODE_00 ? "10/11 (erase)" : KEY_NAMES[code],
                           m.passing_on() && m.game.keys_ok ? "" : " (not passed on)");
        }
    }

    static bool key_pressed_hook(void* self, int player, int key) {
        bool touch = key_pressed_orig(self, player, key);
        monitor& m = instance();
        std::lock_guard<std::mutex> lock(m.lock);
        m.queries++;
        if (player < 0 || player > 1) return touch;
        if (touch) m.log_touch(player, key);
        const reader_keys* k = m.keys_of(player);
        return touch || (m.passing_on() && k != nullptr && k->pressed(key));
    }

    // Number entry (the PIN among others) asks this one: pressed on the touch
    // keypad that is open. On LDJ the game passes it on to key_pressed_hook.
    static bool key_pressed_open_hook(void* self, int player, int key) {
        bool touch = key_pressed_open_orig(self, player, key);
        monitor& m = instance();
        std::lock_guard<std::mutex> lock(m.lock);
        m.open_queries++;
        if (player < 0 || player > 1) return touch;
        if (touch) m.log_touch(player, key);
        const reader_keys* k = m.keys_of(player);
        return touch || (m.passing_on() && k != nullptr && k->pressed(key));
    }

    static bool key_held_hook(void* self, int player, int key) {
        bool touch = key_held_orig(self, player, key);
        monitor& m = instance();
        std::lock_guard<std::mutex> lock(m.lock);
        if (player < 0 || player > 1) return touch;
        const reader_keys* k = m.keys_of(player);
        return touch || (m.passing_on() && k != nullptr && k->held(key));
    }

    // Touch keypad presses, to see which game key each touch button is.
    // Caller holds lock.
    void log_touch(int player, int key) {
        uint64_t now = GetTickCount64();
        if (key >= 0 && key < 16) {
            if (now - touch_logged_ms[player][key] < 300) return;
            touch_logged_ms[player][key] = now;
        }
        hooklog::write("touch  P%d game key %d", player + 1, key);
    }

    // Home screen, every frame while it is shown
    static void home_update_hook(uint8_t* self) {
        monitor& m = instance();
        int* pressed[2] = {nullptr, nullptr};
        int saved[2] = {0, 0};
        {
            std::lock_guard<std::mutex> lock(m.lock);
            m.home_updates++;
            uint64_t now = GetTickCount64();
            for (int p = 0; p < 2; p++) {
                if (m.toggle_ms[p] == 0) continue;
                bool fresh = now - m.toggle_ms[p] <= TOGGLE_PENDING_MS;
                m.toggle_ms[p] = 0;
                if (!fresh || !m.passing_on()) continue;
                uint8_t* button = *(uint8_t**)(self + m.game.toggle_button_offset[p]);
                if (button == nullptr) {
                    hooklog::write("keypad P%d: no keypad button on this screen", p + 1);
                    continue;
                }
                // pressed for this one frame, as if it had been touched
                pressed[p] = (int*)(button + m.game.button_state_offset);
                saved[p] = *pressed[p];
                *pressed[p] = 1;
                hooklog::write("keypad P%d: pressed the show/hide keypad button", p + 1);
            }
        }
        home_update_orig(self);
        for (int p = 0; p < 2; p++) {
            if (pressed[p] != nullptr) *pressed[p] = saved[p];
        }
    }

    static DWORD WINAPI status_thread(LPVOID param) {
        monitor& m = *(monitor*)param;
        uint64_t last[5] = {0, 0, 0, 0, 0};
        for (;;) {
            Sleep(10000);
            uint64_t now[5];
            {
                std::lock_guard<std::mutex> lock(m.lock);
                now[0] = m.polls[0];
                now[1] = m.polls[1];
                now[2] = m.queries;
                now[3] = m.home_updates;
                now[4] = m.open_queries;
            }
            hooklog::write("keypad status: reader polls P1 %.0f/s P2 %.0f/s, key queries %.0f/s, open keypad queries %.0f/s, home updates %.0f/s",
                           (now[0] - last[0]) / 10.0, (now[1] - last[1]) / 10.0, (now[2] - last[2]) / 10.0,
                           (now[4] - last[4]) / 10.0, (now[3] - last[3]) / 10.0);
            memcpy(last, now, sizeof(now));
        }
        return 0;
    }

    layout game;
    keypad_config config;
    std::mutex lock;
    const uint8_t* status_slot[2] = {nullptr, nullptr};
    bool warned_extra_reader = false;
    uint8_t last_status[2][DEVSTATUS_SIZE] = {{0}};
    reader_keys keys[2];  // by slot
    uint64_t toggle_ms[2] = {0, 0};
    uint64_t touch_logged_ms[2][16] = {{0}};
    uint64_t polls[2] = {0, 0};
    uint64_t queries = 0;
    uint64_t open_queries = 0;
    uint64_t home_updates = 0;

    static get_status_fn get_status_orig;
    static key_query_fn key_pressed_orig;
    static key_query_fn key_held_orig;
    static key_query_fn key_pressed_open_orig;
    static home_update_fn home_update_orig;
};

monitor::get_status_fn monitor::get_status_orig = nullptr;
monitor::key_query_fn monitor::key_pressed_orig = nullptr;
monitor::key_query_fn monitor::key_held_orig = nullptr;
monitor::key_query_fn monitor::key_pressed_open_orig = nullptr;
monitor::home_update_fn monitor::home_update_orig = nullptr;

}  // namespace keypad
