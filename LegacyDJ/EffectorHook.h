#pragma once
// Effector faders: the physical faders set the TDJ subscreen effector sliders.
//
// Hooks the two game functions found by GameLayout.h:
//   slider update  the game reads one slider value; a fader that moved writes
//                  its value into the game's slider array just before that
//   panel sync     the subscreen effector panel copies its touch widgets into
//                  the slider array every frame while it is shown, so a fader
//                  that moved also moves the touch widget (value and knob)
// A fader only writes when its step changes, so touch changes stay until the
// fader is moved again. With FADERS=LOG nothing is written, only logged.
//
// Changes are logged to the console and legacydj.log in the game folder.

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include "minhook.h"
#include "FaderMapping.h"
#include "GameLayout.h"
#include "Log.h"

namespace effector {

// Fills the 5 raw fader bytes (0x01..0xFF), false while there are none
typedef std::function<bool(uint8_t out[PHYSICAL_COUNT])> fader_source;

// A fader move is pushed into the touch widget only this soon after it
// happened, so a stale one never lands on a panel opened much later
const uint64_t WIDGET_PENDING_MS = 500;

struct widget_state {
    int index = -1;
    float value = 0, pos = 0, pos2 = 0, min = 0, max = 0;
};

// What the hooks saw last, filled on the game's thread
struct game_snapshot {
    int cabinet_type = -1;
    bool sliders_valid = false;
    int sliders[SLIDER_COUNT] = {0};
    unsigned polled_mask = 0;
    uint64_t update_calls = 0;
    uint64_t panel_calls = 0;
    uint64_t panel_shown_calls = 0;
    bool panel_shown = false;
    widget_state widgets[SLIDER_COUNT];
    // fader moves that were sent to the game
    uint32_t moves[PHYSICAL_COUNT] = {0};
    int moved_to[PHYSICAL_COUNT] = {0};
    uint32_t widget_writes = 0;
};

class monitor {
public:
    static monitor& instance() {
        static monitor m;
        return m;
    }

    // Finds the game functions and hooks them, then starts the logging thread
    bool install(uint8_t* module, fader_source faders, const fader_config& fader_settings) {
        source = faders;
        config = fader_settings;
        hooklog::open();

        std::ostringstream report;
        resolve(module, game, report);
        log("---- legacydj effector started, FADERS=%s",
            config.mode == fader_mode::on ? "ON" : "LOG");
        std::string line;
        std::istringstream lines(report.str());
        while (std::getline(lines, line)) log("%s", line.c_str());
        if (config.mode == fader_mode::on) {
            for (int f = 0; f < PHYSICAL_COUNT; f++) log("fader %d -> %s", f + 1, target_names(f).c_str());
            log("faders %s, deadband %d", config.invert ? "inverted" : "not inverted", config.deadband);
        }

        if (game.sliders_ok) {
            hook(game.slider_update, (void*)slider_update_hook, (void**)&slider_update_orig, "slider update");
        }
        if (game.panel_ok) {
            hook(game.panel_sync, (void*)panel_sync_hook, (void**)&panel_sync_orig, "panel sync");
        } else if (game.sliders_ok && config.mode == fader_mode::on) {
            log("Effector: without the panel, fader moves are lost while the effector panel is shown");
        }
        CreateThread(nullptr, 0, logger_thread, this, 0, nullptr);
        return game.sliders_ok;
    }

    void log(const char* format, ...) {
        char text[1024];
        va_list args;
        va_start(args, format);
        vsnprintf(text, sizeof(text), format, args);
        va_end(args);
        hooklog::write("%s", text);
    }

private:
    typedef void (*game_fn)(uint8_t* self);

    void hook(uint8_t* target, void* detour, void** original, const char* name) {
        MH_STATUS created = MH_CreateHook(target, detour, original);
        MH_STATUS enabled = created == MH_OK ? MH_EnableHook(target) : created;
        if (enabled == MH_OK) {
            log("Effector: hooked %s", name);
        } else {
            log("Effector: could not hook %s (%s)", name, MH_StatusToString(enabled));
        }
    }

    std::string target_names(int fader) const {
        std::string names;
        for (int k = 0; k < SLIDER_COUNT; k++) {
            if (config.targets[fader] & (1u << k)) names += (names.empty() ? "" : ", ") + std::string(SLIDER_NAMES[k]);
        }
        return names.empty() ? "nothing" : names;
    }

    // Game thread (either hook). Turns fader moves into pending writes.
    // Caller holds write_lock.
    void poll_faders() {
        uint8_t raw[PHYSICAL_COUNT];
        if (config.mode != fader_mode::on || !source || !source(raw)) return;
        unsigned changed = filter.update(raw, config.deadband);
        // only on a TDJ cabinet does the game read the slider array
        if (changed == 0 || game.cabinet_type() != game.tdj_type) return;

        uint64_t now = GetTickCount64();
        std::lock_guard<std::mutex> lock(snapshot_lock);
        for (int f = 0; f < PHYSICAL_COUNT; f++) {
            if (!(changed & (1u << f))) continue;
            int value = game_value(filter.step(f), config);
            for (int k = 0; k < SLIDER_COUNT; k++) {
                if (!(config.targets[f] & (1u << k))) continue;
                pending_array[k] = value;
                pending_widget[k] = value;
                pending_widget_ms[k] = now;
            }
            snapshot.moves[f]++;
            snapshot.moved_to[f] = value;
        }
    }

    // Game thread: the game is about to read one slider
    static void slider_update_hook(uint8_t* self) {
        monitor& m = instance();
        int index = *(int*)self;
        {
            std::lock_guard<std::mutex> lock(m.write_lock);
            m.poll_faders();
            if (index >= 0 && index < SLIDER_COUNT && m.pending_array[index] >= 0) {
                int* sliders = m.game.sliders();
                if (sliders != nullptr) sliders[index] = m.pending_array[index];
                m.pending_array[index] = -1;
            }
        }

        slider_update_orig(self);

        std::lock_guard<std::mutex> lock(m.snapshot_lock);
        game_snapshot& s = m.snapshot;
        s.update_calls++;
        if (index >= 0 && index < SLIDER_COUNT) s.polled_mask |= 1u << index;
        s.cabinet_type = m.game.cabinet_type();
        int* sliders = m.game.sliders();
        s.sliders_valid = sliders != nullptr;
        if (sliders != nullptr) {
            for (int i = 0; i < SLIDER_COUNT; i++) s.sliders[i] = sliders[i];
        }
    }

    // Game thread: the panel is about to copy its touch widgets into the
    // sliders. The entries are only touched when the game itself is about to
    // read them.
    static void panel_sync_hook(uint8_t* self) {
        monitor& m = instance();
        const layout& g = m.game;
        bool shown = self[g.panel_enabled_offset] != 0 && self[g.panel_visible_offset] != 0 &&
                     *(void**)(self + g.panel_layer_offset) != nullptr;
        uint8_t** entries = (uint8_t**)(self + g.panel_entries_offset);
        {
            std::lock_guard<std::mutex> lock(m.write_lock);
            m.poll_faders();
            uint64_t now = GetTickCount64();
            for (int k = 0; shown && k < SLIDER_COUNT; k++) {
                uint8_t* entry = entries[k];
                int index = *(int*)entry;
                if (index < 0 || index >= SLIDER_COUNT || m.pending_widget[index] < 0) continue;
                if (now - m.pending_widget_ms[index] <= WIDGET_PENDING_MS) {
                    // value runs 0..15 the other way round to the slider;
                    // pos is the knob, pos2 where a drag started
                    float* f = (float*)(*(uint8_t**)(entry + 8) + g.widget_value_offset);
                    float value = (float)(STEPS - 1 - m.pending_widget[index]);
                    float pos = f[4] > f[3] ? (value - f[3]) / (f[4] - f[3]) : 0.0f;
                    f[0] = value;
                    f[1] = pos;
                    f[2] = pos;
                    std::lock_guard<std::mutex> snap(m.snapshot_lock);
                    m.snapshot.widget_writes++;
                }
                m.pending_widget[index] = -1;
            }
            if (!shown) {
                // the panel builds its widgets from the slider array when it opens
                for (int k = 0; k < SLIDER_COUNT; k++) m.pending_widget[k] = -1;
            }
        }

        {
            std::lock_guard<std::mutex> lock(m.snapshot_lock);
            game_snapshot& s = m.snapshot;
            s.panel_calls++;
            s.panel_shown = shown;
            if (shown) {
                s.panel_shown_calls++;
                for (int k = 0; k < SLIDER_COUNT; k++) {
                    uint8_t* entry = entries[k];
                    float* f = (float*)(*(uint8_t**)(entry + 8) + g.widget_value_offset);
                    widget_state& w = s.widgets[k];
                    w.index = *(int*)entry;
                    w.value = f[0];
                    w.pos = f[1];
                    w.pos2 = f[2];
                    w.min = f[3];
                    w.max = f[4];
                }
            }
        }
        panel_sync_orig(self);
    }

    static DWORD WINAPI logger_thread(LPVOID param) {
        ((monitor*)param)->logger_loop();
        return 0;
    }

    static const char* slider_name(int index) {
        return index >= 0 && index < SLIDER_COUNT ? SLIDER_NAMES[index] : "?";
    }

    void logger_loop() {
        game_snapshot last;
        int last_steps[PHYSICAL_COUNT] = {-1, -1, -1, -1, -1};
        bool had_faders = false, first = true;
        uint64_t last_status = GetTickCount64();
        game_snapshot at_status;

        for (;;) {
            Sleep(50);
            game_snapshot now;
            {
                std::lock_guard<std::mutex> lock(snapshot_lock);
                now = snapshot;
            }

            uint8_t raw[PHYSICAL_COUNT] = {0};
            bool have_faders = source && source(raw);
            if (have_faders) {
                int steps[PHYSICAL_COUNT];
                bool changed = !had_faders;
                for (int i = 0; i < PHYSICAL_COUNT; i++) {
                    steps[i] = raw[i] >> 4;
                    if (steps[i] != last_steps[i]) changed = true;
                }
                if (changed) {
                    log("physical  S1..S5 = %2d %2d %2d %2d %2d   (raw %02X %02X %02X %02X %02X)", steps[0], steps[1],
                        steps[2], steps[3], steps[4], raw[0], raw[1], raw[2], raw[3], raw[4]);
                    memcpy(last_steps, steps, sizeof(steps));
                }
            } else if (had_faders || first) {
                log("physical  no fader data (relay board not answering, or MODE=ARDUINO)");
            }
            had_faders = have_faders;

            for (int f = 0; f < PHYSICAL_COUNT; f++) {
                if (now.moves[f] != last.moves[f]) {
                    log("fader %d   -> %s = %d", f + 1, target_names(f).c_str(), now.moved_to[f]);
                }
            }
            if (now.cabinet_type != last.cabinet_type) {
                log("cabinet type %d (TDJ is %d)", now.cabinet_type, (int)game.tdj_type);
            }
            if (now.sliders_valid &&
                (!last.sliders_valid || memcmp(now.sliders, last.sliders, sizeof(now.sliders)) != 0)) {
                log("game      vefx %2d  low %2d  hi %2d  filter %2d  volume %2d  low_mid %2d  hi_mid %2d",
                    now.sliders[0], now.sliders[1], now.sliders[2], now.sliders[3], now.sliders[4], now.sliders[5],
                    now.sliders[6]);
            }
            if (now.panel_shown != last.panel_shown) {
                log("panel     %s", now.panel_shown ? "shown" : "hidden");
            }
            if (now.panel_shown) {
                for (int k = 0; k < SLIDER_COUNT; k++) {
                    const widget_state& a = now.widgets[k];
                    const widget_state& b = last.widgets[k];
                    if (!last.panel_shown || a.index != b.index || a.value != b.value || a.pos != b.pos ||
                        a.pos2 != b.pos2 || a.min != b.min || a.max != b.max) {
                        log("touch     entry %d %-11s value %7.3f  pos %6.3f  pos2 %6.3f  min %g  max %g", k,
                            slider_name(a.index), a.value, a.pos, a.pos2, a.min, a.max);
                    }
                }
            }

            uint64_t t = GetTickCount64();
            if (t - last_status >= 10000) {
                double secs = (t - last_status) / 1000.0;
                log("status    slider update %.0f/s, panel sync %.0f/s (shown %.0f/s), sliders polled mask %02X, "
                    "widget writes %u",
                    (now.update_calls - at_status.update_calls) / secs,
                    (now.panel_calls - at_status.panel_calls) / secs,
                    (now.panel_shown_calls - at_status.panel_shown_calls) / secs, now.polled_mask,
                    now.widget_writes - at_status.widget_writes);
                at_status = now;
                last_status = t;
            }

            last = now;
            first = false;
        }
    }

    layout game;
    fader_source source;
    fader_config config;
    std::mutex snapshot_lock;
    game_snapshot snapshot;

    // fader -> game, guarded by write_lock (both hooks run on the game's thread,
    // the lock only matters if they ever do not)
    std::mutex write_lock;
    fader_filter filter;
    int pending_array[SLIDER_COUNT] = {-1, -1, -1, -1, -1, -1, -1};
    int pending_widget[SLIDER_COUNT] = {-1, -1, -1, -1, -1, -1, -1};
    uint64_t pending_widget_ms[SLIDER_COUNT] = {0};

    static game_fn slider_update_orig;
    static game_fn panel_sync_orig;
};

monitor::game_fn monitor::slider_update_orig = nullptr;
monitor::game_fn monitor::panel_sync_orig = nullptr;

}  // namespace effector
