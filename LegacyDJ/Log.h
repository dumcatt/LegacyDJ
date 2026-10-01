#pragma once
// One log for every hook: the console and legacydj.log in the game
// folder, each line stamped with the seconds since the log was opened.

#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace hooklog {

struct state {
    std::mutex lock;
    FILE* file = nullptr;
    uint64_t start_ms = 0;
};

inline state& get() {
    static state s;
    return s;
}

inline void open() {
    state& s = get();
    std::lock_guard<std::mutex> lock(s.lock);
    if (s.file == nullptr) {
        // shared, so the log can be read while the game runs
        s.file = _fsopen("legacydj.log", "a", _SH_DENYNO);
        s.start_ms = GetTickCount64();
    }
}

inline void write(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    state& s = get();
    std::lock_guard<std::mutex> lock(s.lock);
    double t = (GetTickCount64() - s.start_ms) / 1000.0;
    printf("[%7.3f] %s\n", t, text);
    fflush(stdout);
    if (s.file != nullptr) {
        fprintf(s.file, "[%7.3f] %s\n", t, text);
        fflush(s.file);
    }
}

}  // namespace hooklog
