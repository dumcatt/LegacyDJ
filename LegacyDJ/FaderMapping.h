#pragma once
// Which game sliders each physical fader drives, and turning the relay
// board's fader bytes into slider steps. No Windows dependencies, so this can
// be tested on its own.
//
// Settings (legacydj.conf):
//   FADERS=ON|LOG|OFF       ON drives the sliders, LOG only logs, OFF does nothing
//   FADER1..FADER5=<names>  sliders for each fader, comma separated, empty for none:
//                           vefx low hi filter play_volume low_mid hi_mid
//   FADER_INVERT=0|1        1 if the faders work the wrong way round
//   FADER_DEADBAND=<n>      raw counts (of 16 per step) a fader has to move past
//                           a step boundary before the step changes

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace effector {

const int PHYSICAL_COUNT = 5;
const int STEPS = 16;  // slider values 0..15
const int SLIDER_COUNT = 7;
const char* const SLIDER_NAMES[SLIDER_COUNT] = {"vefx",        "low",     "hi",    "filter",
                                                          "play_volume", "low_mid", "hi_mid"};

enum class fader_mode { off, log, on };

struct fader_config {
    fader_mode mode = fader_mode::on;
    bool invert = false;
    int deadband = 3;
    // bit n = game slider n
    unsigned targets[PHYSICAL_COUNT] = {1u << 0, (1u << 1) | (1u << 5), (1u << 2) | (1u << 6), 1u << 3, 1u << 4};
};

inline int game_slider_index(std::string name) {
    for (char& c : name) {
        c = (char)tolower((unsigned char)c);
        if (c == '-') c = '_';
    }
    if (name == "volume") name = "play_volume";
    for (int i = 0; i < SLIDER_COUNT; i++) {
        if (name == SLIDER_NAMES[i]) return i;
    }
    return -1;
}

inline std::string trimmed(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

// "low, low_mid" -> bitmask; false (and `error`) on an unknown name
inline bool parse_targets(const std::string& value, unsigned& mask, std::string& error) {
    mask = 0;
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string name = trimmed(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!name.empty()) {
            int index = game_slider_index(name);
            if (index < 0) {
                error = "unknown slider \"" + name + "\"";
                return false;
            }
            mask |= 1u << index;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

// Applies the FADER* settings (key already upper case); returns the problems found
inline std::vector<std::string> parse_fader_settings(const std::vector<std::pair<std::string, std::string>>& settings,
                                                     fader_config& config) {
    std::vector<std::string> errors;
    for (const auto& kv : settings) {
        const std::string& key = kv.first;
        std::string value = trimmed(kv.second);
        if (key == "FADERS") {
            std::string v;
            for (char c : value) v += (char)toupper((unsigned char)c);
            if (v == "ON") config.mode = fader_mode::on;
            else if (v == "LOG") config.mode = fader_mode::log;
            else if (v == "OFF") config.mode = fader_mode::off;
            else errors.push_back("Invalid FADERS: " + value);
        } else if (key == "FADER_INVERT") {
            config.invert = value == "1" || value == "ON" || value == "on" || value == "true";
        } else if (key == "FADER_DEADBAND") {
            int d = atoi(value.c_str());
            if (d < 0 || d > 7) errors.push_back("FADER_DEADBAND must be 0..7: " + value);
            else config.deadband = d;
        } else if (key.size() == 6 && key.compare(0, 5, "FADER") == 0 && key[5] >= '1' && key[5] <= '5') {
            unsigned mask;
            std::string error;
            if (parse_targets(value, mask, error)) config.targets[key[5] - '1'] = mask;
            else errors.push_back(key + ": " + error);
        } else {
            errors.push_back("Unknown setting: " + key);
        }
    }
    return errors;
}

// Raw fader bytes (0x01..0xFF) to steps 0..15. A step only changes once the
// fader is `deadband` counts past the edge of its current step, so a fader
// resting on a boundary does not flicker between two steps.
class fader_filter {
public:
    // Returns a bitmask of the faders whose step changed. The first reading
    // only sets the starting steps and reports no change.
    unsigned update(const uint8_t raw[PHYSICAL_COUNT], int deadband) {
        unsigned changed = 0;
        for (int i = 0; i < PHYSICAL_COUNT; i++) {
            int candidate = raw[i] / STEPS;
            if (steps[i] < 0) {
                steps[i] = candidate;
                continue;
            }
            if (candidate == steps[i]) continue;
            int centre = steps[i] * STEPS + STEPS / 2;
            int distance = raw[i] > centre ? raw[i] - centre : centre - raw[i];
            if (distance >= STEPS / 2 + deadband) {
                steps[i] = candidate;
                changed |= 1u << i;
            }
        }
        return changed;
    }

    int step(int fader) const { return steps[fader]; }

private:
    int steps[PHYSICAL_COUNT] = {-1, -1, -1, -1, -1};
};

// Game slider value for a fader step
inline int game_value(int step, const fader_config& config) {
    return config.invert ? STEPS - 1 - step : step;
}

}  // namespace effector
