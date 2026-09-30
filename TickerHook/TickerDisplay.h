#pragma once
// What the ticker, spotlights and neon should show at a given moment, and the
// packet that tells the sub IO ("relay board") to show it.
//
// Relay board protocol, as captured between a BIO2 on BI2A firmware and the
// board: 115200 baud 8N1, ACIO framing, one request about every 8ms and no
// handshake before the first one.
//
//   request   AA 00 01 12 00 0C <12 data bytes> <checksum>
//             [0] button lamps  [1..9] ticker digits (ASCII)
//             [10] spotlights   [11] neon         all active low
//   response  AA AA 80 01 12 00 0D <13 data bytes> <checksum>
//             [6..10] faders 1..5, 0x01..0xFF
//
// Every byte after the leading AA that is AA or FF goes out as FF followed by
// its complement; the checksum is the sum of the bytes from the address on.
//
// No Windows dependencies, so this can be tested on its own.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct ticker_config {
    int scroll_interval_ms = 300;   // one scroll step
    int scroll_gap = 12;            // spaces between the end and the start of scrolling text
    long spotlight_interval_ms = 1000; // >0 blink, 0 off, -1 on
    long neon_interval_ms = 1000;      // >0 blink, 0 off, -1 on
    // Text containing one of these (and no longer than the ticker) is
    // centred on it instead of scrolling
    std::vector<std::string> static_texts = {
        "*********", "DEMO PLAY", "WELCOME", "ENTRY", "DECIDE!", "MODE?", "STAY COOL", "TUTORIAL",
    };
};

class ticker_display {
public:
    static const int SIZE = 9;
    static const int NOT_STATIC = -100;

    explicit ticker_display(const ticker_config& config) : config(config) {
        set_text("*********", 0);
    }

    // The game keeps handing over the text it is already showing,
    // that must not restart the scroll
    void set_text(const std::string& new_text, unsigned long now_ms) {
        if (new_text == text) return;
        text = new_text;
        for (char& c : text) {
            // the ticker shows 'm' as a full stop
            if (c == '.') c = 'm';
        }
        static_offset = find_static_offset();
        // Start scrolling text on the 3rd segment (index 2)
        long virtual_len = (long)text.size() + config.scroll_gap;
        scroll_index = virtual_len > 2 ? virtual_len - 2 : 0;
        last_scroll = now_ms;
    }

    // The 9 characters to show right now
    void render(unsigned long now_ms, char out[SIZE]) {
        long len = (long)text.size();
        if (static_offset != NOT_STATIC) {
            for (int i = 0; i < SIZE; i++) {
                long idx = i - static_offset;
                out[i] = (idx >= 0 && idx < len) ? text[idx] : ' ';
            }
            return;
        }

        long virtual_len = len + config.scroll_gap;
        if (virtual_len <= 0) {
            memset(out, ' ', SIZE);
            return;
        }
        if (config.scroll_interval_ms > 0) {
            while (now_ms - last_scroll >= (unsigned long)config.scroll_interval_ms) {
                last_scroll += config.scroll_interval_ms;
                if (++scroll_index >= virtual_len) scroll_index = 0;
            }
        }
        for (int i = 0; i < SIZE; i++) {
            long pos = (i + scroll_index) % virtual_len;
            out[i] = pos < len ? text[pos] : ' ';
        }
    }

    static bool light_state(long interval_ms, unsigned long now_ms) {
        if (interval_ms < 0) return true;
        if (interval_ms == 0) return false;
        return (now_ms / (unsigned long)interval_ms) % 2 == 0;
    }

    bool spotlights_on(unsigned long now_ms) const { return light_state(config.spotlight_interval_ms, now_ms); }
    bool neon_on(unsigned long now_ms) const { return light_state(config.neon_interval_ms, now_ms); }

    const std::string& current_text() const { return text; }

private:
    int find_static_offset() const {
        if ((long)text.size() > SIZE) return NOT_STATIC;
        for (const std::string& candidate : config.static_texts) {
            if (candidate.empty()) continue;
            size_t match = text.find(candidate);
            if (match != std::string::npos) {
                // centre the matched part, not the whole text
                int target = (SIZE - (int)candidate.size()) / 2;
                return target - (int)match;
            }
        }
        return NOT_STATIC;
    }

    ticker_config config;
    std::string text;
    int static_offset = NOT_STATIC;
    long scroll_index = 0;
    unsigned long last_scroll = 0;
};

namespace relay_board {

const uint8_t SOF = 0xAA;
const uint8_t ESCAPE = 0xFF;
const int FADER_COUNT = 5;

// Register bit of each spotlight, left to right (same order as MAME's twinkle driver)
const int SPOTLIGHT_BITS[8] = {3, 2, 1, 0, 4, 5, 6, 7};

// lamps: bit 0 p1 start, 1 p2 start, 2 vefx, 3 effect, 4 credit
// spotlights: bit n = spotlight n from the left. neon: bit 0.
inline std::vector<uint8_t> build_request(const char chars[ticker_display::SIZE], uint8_t lamps, uint8_t spotlights,
                                          uint8_t neon) {
    uint8_t body[5 + 12] = {0x00, 0x01, 0x12, 0x00, 0x0C};
    uint8_t* data = body + 5;

    // the BIO2 sends 1F with every lamp off: 5 lamp bits, the top 3 held low
    data[0] = (uint8_t)(0x1F & ~lamps);
    for (int i = 0; i < ticker_display::SIZE; i++) {
        data[1 + i] = (uint8_t)~(uint8_t)chars[i];
    }
    uint8_t raw = 0;
    for (int i = 0; i < 8; i++) {
        if (spotlights & (1 << i)) raw |= (uint8_t)(1 << SPOTLIGHT_BITS[i]);
    }
    data[10] = (uint8_t)~raw;
    data[11] = (uint8_t)~(neon & 0x07);

    std::vector<uint8_t> frame;
    frame.reserve(sizeof(body) * 2 + 3);
    frame.push_back(SOF);
    uint8_t checksum = 0;
    for (size_t i = 0; i <= sizeof(body); i++) {
        uint8_t value;
        if (i < sizeof(body)) {
            value = body[i];
            checksum = (uint8_t)(checksum + value);
        } else {
            value = checksum;
        }
        if (value == SOF || value == ESCAPE) {
            frame.push_back(ESCAPE);
            frame.push_back((uint8_t)~value);
        } else {
            frame.push_back(value);
        }
    }
    return frame;
}

// Collects bytes from the board and picks complete, checksum-valid replies out of them
class reply_parser {
public:
    // Returns true when a reply was completed; faders holds the fader bytes of the latest one
    bool feed(uint8_t value) {
        if (value == SOF) {
            // start (or restart) of a frame; AAs never appear inside one
            body.clear();
            in_frame = true;
            escaped = false;
            return false;
        }
        if (!in_frame) return false;
        if (escaped) {
            value = (uint8_t)~value;
            escaped = false;
        } else if (value == ESCAPE) {
            escaped = true;
            return false;
        }
        body.push_back(value);
        if (body.size() >= 5 && body.size() == (size_t)(6 + body[4])) {
            in_frame = false;
            uint8_t checksum = 0;
            for (size_t i = 0; i + 1 < body.size(); i++) checksum = (uint8_t)(checksum + body[i]);
            bool ok = checksum == body.back() && body[0] == 0x80 && body[1] == 0x01 && body[2] == 0x12 &&
                      body[4] >= 11;
            if (ok) {
                for (int i = 0; i < FADER_COUNT; i++) faders[i] = body[5 + 6 + i];
                replies++;
            }
            return ok;
        }
        if (body.size() > 64) in_frame = false;
        return false;
    }

    uint8_t faders[FADER_COUNT] = {0};
    unsigned long replies = 0;

private:
    std::vector<uint8_t> body;
    bool in_frame = false;
    bool escaped = false;
};

}  // namespace relay_board
