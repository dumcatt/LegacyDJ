#pragma once
// Finds the game code the card reader keypads need in TDJ mode, using byte
// signatures. Like GameLayout.h this only reads code, so SigTest can check it
// offline.
//
// Found in IIDX 32 (bm2dx.dll 2025-08-18, ReleaseTDJ):
//
//   key query     bool (?, int player, int key): was this keypad key pressed
//                 this frame. Every keypad user in the game goes through it
//                 (PIN entry included). On a TDJ cabinet it asks the subscreen
//                 touch keypad, on LDJ the card reader. Keys: 0..9 digits,
//                 10 "00", 11 decimal; on TDJ both 10 and 11 are the touch
//                 keypad's erase button.
//   home update   the subscreen home screen, every frame while it is shown.
//                 It opens or closes a player's touch keypad when that
//                 player's show/hide keypad button (screen corner) is in the
//                 pressed state (1). A second button on the open keypad only
//                 closes it.
//
// The card readers themselves are read through libaio-iob.dll's
// AIO_IOB_ICCA::GetDeviceStatus, an export, so no signature is needed for it.

#include <algorithm>
#include <cstdint>
#include <map>
#include <ostream>
#include <vector>
#include "Signature.h"

namespace keypad {

// The key queries are found through the table the game builds its key ->
// touch button map from at startup ({key, button} pairs, digit 0 is button 9):
// the code that loads the table stores the map in a global, and the key
// queries read that global right at their start. Which one is "pressed" and
// which "held" follows from how each first tests a button state against 1
// (sete or setg).
const char* const KEY_BUTTON_TABLE_SIG =
    "00 00 00 00 09 00 00 00 01 00 00 00 0A 00 00 00 02 00 00 00 0B 00 00 00 03 00 00 00 0C 00 00 00";
// mov [rip+map], rax  (after the table has been loaded)
const char* const MAP_STORE_SIG = "48 89 05 ?? ?? ?? ??";

// Fallback, inside both key queries where they test the touch keypad button:
//   call get_button / mov rcx,[rax] / cmp dword [rcx+60],1 / sete   pressed this frame
//                                                          / setg   held
// The function starts are found from there through the unwind data.
const char* const KEY_PRESSED_CORE_SIG = "E8 ?? ?? ?? ?? 48 8B 08 83 79 ?? 01 0F 94";
const char* const KEY_HELD_CORE_SIG = "E8 ?? ?? ?? ?? 48 8B 08 83 79 ?? 01 0F 9F";

// In the home update, once per player, the "close keypad" button on the open
// keypad, then the show/hide keypad button (the one at the screen corner):
//   mov rax,[rdi+close] / test rax,rax / je ... / cmp dword [rax+state],1 / jne ...
//   mov rax,[rdi+toggle] / cmp dword [rax+state],1 / je ...
const char* const OPEN_BUTTON_SIG =
    "48 8B 47 ?? 48 85 C0 0F 84 ?? ?? ?? ?? 83 78 ?? 01 0F 85 ?? ?? ?? ?? 48 8B 47 ?? 83 78 ?? 01 0F 84";

struct layout {
    uint8_t* module = nullptr;

    bool keys_ok = false;
    uint8_t* key_pressed = nullptr;  // bool (?, int player, int key)
    uint8_t* key_held = nullptr;     // same, for a key held down (nullptr if not found)
    // same, for "pressed on the touch keypad that is open" (number entry such
    // as the PIN uses it); on LDJ it just jumps to key_pressed (nullptr if not found)
    uint8_t* key_pressed_open = nullptr;

    bool toggle_ok = false;
    uint8_t* home_update = nullptr;
    uint8_t toggle_button_offset[2] = {0, 0};  // per player, in the home screen object
    uint8_t button_state_offset = 0;           // 1 = pressed this frame

    uintptr_t rva(const void* p) const { return (uintptr_t)p - (uintptr_t)module; }
};

// How a function first tests a button state: 1 for `cmp dword [reg+off],1`
// followed by sete (pressed), 2 for setg (held), 0 for neither
inline int button_test_kind(const uint8_t* function) {
    for (const uint8_t* p = function; p < function + 0x180; p++) {
        if (p[0] != 0x83 || p[1] < 0x78 || p[1] > 0x7F || p[3] != 0x01) continue;
        for (const uint8_t* q = p + 4; q < p + 16; q++) {
            if (q[0] == 0x0F && q[1] == 0x94) return 1;
            if (q[0] == 0x0F && q[1] == 0x9F) return 2;
        }
        return 0;
    }
    return 0;
}

// Of several candidates, the one the game calls the most; nullptr if none
inline uint8_t* most_called(uint8_t* module, const std::vector<uint8_t*>& functions) {
    uint8_t* best = nullptr;
    size_t best_calls = 0;
    for (uint8_t* f : functions) {
        size_t n = count_calls(module, f);
        if (best == nullptr || n > best_calls) {
            best = f;
            best_calls = n;
        }
    }
    return best;
}

// The "pressed on the open touch keypad" query: a function that reads the key
// map and, on an LDJ cabinet, jumps straight to the key pressed query
inline uint8_t* find_open_keypad_query(uint8_t* module, uint8_t* map, uint8_t* key_pressed) {
#ifdef _WIN64
    for (uint8_t* p : rip_refs(module, map, map + 1)) {
        uint8_t* f = function_start(p);
        if (f == nullptr || f == key_pressed || p - f > 0x80) continue;
        for (uint8_t* q = f; q < p; q++) {
            if (q[0] == 0xE9 && call_target(q) == key_pressed) return f;
        }
    }
#endif
    return nullptr;
}

// The key queries found through the key -> touch button map; false if that
// path does not lead anywhere
inline bool resolve_key_queries_by_map(uint8_t* module, layout& out, std::ostream& report) {
#ifdef _WIN64
    // The same bytes can occur more than once (other tables); the right copy
    // is the one whose loading code (16 bytes at a time, from a little before
    // it) stores a map that key queries read
    std::vector<uint8_t*> tables = signature(KEY_BUTTON_TABLE_SIG).find_in_data(module, 8);
    for (uint8_t* table : tables) {
        std::vector<uint8_t*> loads = rip_refs(module, table - 0x20, table + 0x80);
        if (loads.empty()) continue;
        uint8_t* store = find_near(signature(MAP_STORE_SIG), loads.back(), 0x100);
        if (store == nullptr) continue;
        uint8_t* map = rip_target(store + 3, store + 7);

        // mov reg,[rip+map] near the start of a function
        std::vector<uint8_t*> pressed, held;
        for (uint8_t* p : rip_refs(module, map, map + 1)) {
            if (p[-2] != 0x8B || (p[-3] != 0x48 && p[-3] != 0x4C)) continue;
            uint8_t* f = function_start(p);
            if (f == nullptr || p - f > 0x20) continue;
            int kind = button_test_kind(f);
            std::vector<uint8_t*>& list = kind == 1 ? pressed : held;
            if (kind != 0 && std::find(list.begin(), list.end(), f) == list.end()) list.push_back(f);
        }
        if (pressed.empty()) continue;
        out.key_pressed = most_called(module, pressed);
        out.key_held = most_called(module, held);
        out.key_pressed_open = find_open_keypad_query(module, map, out.key_pressed);
        report << "Keypad: key map at " << hex(out.rva(map)) << ", " << pressed.size() << " pressed and "
               << held.size() << " held query candidates\n";
        return true;
    }
    report << "Keypad: key table found " << tables.size() << " times, none leads to the key queries\n";
    return false;
#else
    return false;
#endif
}

inline void resolve(uint8_t* module, layout& out, std::ostream& report) {
    out = layout();
    out.module = module;

    if (!resolve_key_queries_by_map(module, out, report)) {
        std::vector<uint8_t*> pressed = signature(KEY_PRESSED_CORE_SIG).find_in_module(module);
        std::vector<uint8_t*> held = signature(KEY_HELD_CORE_SIG).find_in_module(module);
#ifdef _WIN64
        if (pressed.size() == 1) out.key_pressed = function_start(pressed[0]);
        if (held.size() == 1) out.key_held = function_start(held[0]);
#endif
        if (out.key_pressed == nullptr) {
            report << "Keypad: key pressed query found " << pressed.size() << " times (need exactly 1)\n";
            out.key_held = nullptr;
        }
    }
    if (out.key_pressed != nullptr) {
        out.keys_ok = true;
        report << "Keypad: key pressed query at " << hex(out.rva(out.key_pressed)) << ", key held query "
               << (out.key_held ? hex(out.rva(out.key_held)) : std::string("not found"))
               << ", open keypad query "
               << (out.key_pressed_open ? hex(out.rva(out.key_pressed_open)) : std::string("not found")) << "\n";
    }

    // The open button pattern also shows up elsewhere; the home update is the
    // one function with exactly two matches (1P, then 2P)
    std::map<uint8_t*, std::vector<uint8_t*>> by_function;
    for (uint8_t* p : signature(OPEN_BUTTON_SIG).find_in_module(module, 64)) {
#ifdef _WIN64
        uint8_t* f = function_start(p);
        if (f != nullptr) by_function[f].push_back(p);
#endif
    }
    std::vector<uint8_t*> candidates;
    for (auto& kv : by_function) {
        const std::vector<uint8_t*>& v = kv.second;
        if (v.size() == 2 && v[0][3] + 8 == v[1][3] && v[0][15] == v[1][15]) candidates.push_back(kv.first);
    }
    if (candidates.size() != 1) {
        report << "Keypad: home update found " << candidates.size() << " times (need exactly 1)\n";
        return;
    }
    const std::vector<uint8_t*>& v = by_function[candidates[0]];
    out.home_update = candidates[0];
    out.toggle_button_offset[0] = v[0][26];
    out.toggle_button_offset[1] = v[1][26];
    out.button_state_offset = v[0][15];
    out.toggle_ok = v[0][26] + 8 == v[1][26];
    report << "Keypad: home update at " << hex(out.rva(out.home_update)) << ", show/hide keypad buttons +"
           << hex(out.toggle_button_offset[0]) << "/+" << hex(out.toggle_button_offset[1]) << ", button state +"
           << hex(out.button_state_offset) << (out.toggle_ok ? "" : " (unexpected, not used)") << "\n";
}

}  // namespace keypad
