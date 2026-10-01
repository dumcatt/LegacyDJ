#pragma once
// Finds where the game keeps the effector slider values (TDJ mode), using
// byte signatures instead of fixed addresses. Everything here only reads the
// game's code, nothing is called or changed, so it can be tested offline
// (see SigTest).
//
// Found in IIDX 32 (bm2dx.dll 2025-08-18, ReleaseTDJ):
//
//   slider update   called each frame for each of the 7 sliders. On a TDJ
//                   cabinet it reads the value from an int[7] in the game
//                   state object, otherwise from the BI2A I/O (LDJ).
//   panel sync      the subscreen effector panel. While it is shown it writes
//                   every slider value from its touch widget, every frame.
//
// Slider order in the int[7]: vefx, low, hi, filter, play volume, low mid,
// hi mid; values 0..15.

#include <cstdint>
#include <sstream>
#include <string>
#include "FaderMapping.h"
#include "Signature.h"

namespace effector {

// Function start + 2. The wildcards are the struct offsets and constants,
// which are read out of the match below.
//   mov eax,[rcx+0C] / mov rbx,rcx / mov [rcx+10],eax / mov byte [rcx+14],0
//   call cabinet_info / cmp dword [rax+08],03 / jne ...
//   call game_state / lea rcx,[rax+3B8]
const char* const SLIDER_UPDATE_SIG =
    "48 83 EC 20 8B 41 0C 48 8B D9 89 41 10 C6 41 14 00 E8 ?? ?? ?? ?? 83 78 ?? ?? 75 ?? "
    "E8 ?? ?? ?? ?? 48 8D 88 ?? ?? ?? ??";

// Inside the panel sync, right after the prologue:
//   cmp byte [rcx+F8],0 / mov rdi,rcx / je ... / cmp byte [rcx+F9],0 / je ...
//   cmp qword [rcx+20],0 / je ...
const char* const PANEL_SYNC_SIG =
    "80 B9 ?? ?? 00 00 00 48 8B F9 0F 84 ?? ?? ?? ?? 80 B9 ?? ?? 00 00 00 0F 84 ?? ?? ?? ?? 48 83 79 ?? 00 0F 84";
// lea rsi,[rcx+A0]  (the panel's slider entries)
const char* const PANEL_ENTRIES_SIG = "48 8D B1 ?? ?? ?? ??";
// mov r14d,7  (number of entries)
const char* const PANEL_COUNT_SIG = "41 BE ?? 00 00 00";
// movss xmm6,[rax+6C]  (value of the touch widget, in the write-back loop)
const char* const WIDGET_VALUE_SIG = "F3 0F 10 70 ??";

struct layout {
    uint8_t* module = nullptr;

    // sliders
    bool sliders_ok = false;
    uint8_t* slider_update = nullptr;
    uint8_t** cabinet_global = nullptr;
    int32_t cabinet_add = 0;
    uint8_t cabinet_type_offset = 0;
    uint8_t tdj_type = 0;
    uint8_t** state_global = nullptr;
    uint32_t slider_offset = 0;

    // subscreen panel
    bool panel_ok = false;
    uint8_t* panel_sync = nullptr;
    uint32_t panel_enabled_offset = 0;
    uint32_t panel_visible_offset = 0;
    uint8_t panel_layer_offset = 0;
    uint32_t panel_entries_offset = 0;
    uint8_t panel_entry_count = 0;
    uint8_t widget_value_offset = 0;

    // -1 while the game has not set it up yet
    int cabinet_type() const {
        uint8_t* info = *cabinet_global;
        if (info == nullptr) return -1;
        return *(int*)(info + cabinet_add + cabinet_type_offset);
    }

    // nullptr while the game has not set it up yet
    int* sliders() const {
        uint8_t* state = *state_global;
        return state == nullptr ? nullptr : (int*)(state + slider_offset);
    }

    uintptr_t rva(const void* p) const { return (uintptr_t)p - (uintptr_t)module; }
};

// `mov rax,[rip+x]` followed by ret, `add rax,imm32; ret` or `add rax,imm8; ret`
inline bool parse_global_getter(uint8_t* f, uint8_t**& global, int32_t& add) {
    if (!(f[0] == 0x48 && f[1] == 0x8B && f[2] == 0x05)) return false;
    global = (uint8_t**)rip_target(f + 3, f + 7);
    uint8_t* next = f + 7;
    if (next[0] == 0xC3) {
        add = 0;
        return true;
    }
    if (next[0] == 0x48 && next[1] == 0x05 && next[6] == 0xC3) {
        add = read_i32(next + 2);
        return true;
    }
    if (next[0] == 0x48 && next[1] == 0x83 && next[2] == 0xC0 && next[4] == 0xC3) {
        add = (int8_t)next[3];
        return true;
    }
    return false;
}

// Fills `out` and writes what was found (or why not) to `report`
inline void resolve(uint8_t* module, layout& out, std::ostream& report) {
    out = layout();
    out.module = module;

    std::vector<uint8_t*> m = signature(SLIDER_UPDATE_SIG).find_in_module(module);
    if (m.size() != 1) {
        report << "Effector: slider update signature found " << m.size() << " times (need exactly 1)\n";
    } else {
        uint8_t* p = m[0];
        uint8_t* cabinet_getter = call_target(p + 17);
        uint8_t* state_getter = call_target(p + 28);
        int32_t state_add = 0;
        bool cab_ok = parse_global_getter(cabinet_getter, out.cabinet_global, out.cabinet_add);
        bool state_ok = parse_global_getter(state_getter, out.state_global, state_add) && state_add == 0;
        out.cabinet_type_offset = p[24];
        out.tdj_type = p[25];
        out.slider_offset = read_u32(p + 36);
#ifdef _WIN64
        out.slider_update = function_start(p);
#endif
        if (!cab_ok) {
            report << "Effector: unexpected cabinet info getter at " << hex(out.rva(cabinet_getter)) << ": "
                   << bytes_at(cabinet_getter, 16) << "\n";
        } else if (!state_ok) {
            report << "Effector: unexpected game state getter at " << hex(out.rva(state_getter)) << ": "
                   << bytes_at(state_getter, 16) << "\n";
        } else if (out.slider_update == nullptr) {
            report << "Effector: no unwind data for the slider update function\n";
        } else {
            out.sliders_ok = true;
            report << "Effector: slider update at " << hex(out.rva(out.slider_update)) << " (signature at +"
                   << hex(p - out.slider_update) << ")\n"
                   << "Effector: sliders at [" << hex(out.rva(out.state_global)) << "] + " << hex(out.slider_offset)
                   << ", cabinet type at [" << hex(out.rva(out.cabinet_global)) << "] + "
                   << hex((uint32_t)(out.cabinet_add + out.cabinet_type_offset)) << ", TDJ = " << (int)out.tdj_type
                   << "\n";
        }
    }

    m = signature(PANEL_SYNC_SIG).find_in_module(module);
    if (m.size() != 1) {
        report << "Effector: panel signature found " << m.size() << " times (need exactly 1)\n";
        return;
    }
    uint8_t* p = m[0];
    uint8_t* entries = find_near(signature(PANEL_ENTRIES_SIG), p, 0x60);
    uint8_t* count = find_near(signature(PANEL_COUNT_SIG), p, 0x80);
    uint8_t* value = find_near(signature(WIDGET_VALUE_SIG), p, 0x300);
#ifdef _WIN64
    out.panel_sync = function_start(p);
#endif
    if (entries == nullptr || count == nullptr || value == nullptr || out.panel_sync == nullptr) {
        report << "Effector: panel found at " << hex(out.rva(p)) << " but not its details ("
               << (entries ? "" : "entries ") << (count ? "" : "count ") << (value ? "" : "widget value ")
               << (out.panel_sync ? "" : "function start") << ")\n";
        return;
    }
    out.panel_enabled_offset = read_u32(p + 2);
    out.panel_visible_offset = read_u32(p + 18);
    out.panel_layer_offset = p[32];
    out.panel_entries_offset = read_u32(entries + 3);
    out.panel_entry_count = count[2];
    out.widget_value_offset = value[4];
    if (out.panel_entry_count != SLIDER_COUNT) {
        report << "Effector: panel has " << (int)out.panel_entry_count << " sliders, expected " << SLIDER_COUNT
               << "\n";
        return;
    }
    out.panel_ok = true;
    report << "Effector: panel sync at " << hex(out.rva(out.panel_sync)) << " (signature at +"
           << hex(p - out.panel_sync) << "), shown flags +" << hex(out.panel_enabled_offset) << "/+"
           << hex(out.panel_visible_offset) << ", entries +" << hex(out.panel_entries_offset)
           << ", widget value +" << hex(out.widget_value_offset) << "\n";
}

}  // namespace effector
