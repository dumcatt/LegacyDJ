#pragma once
// Byte signatures ("48 8B 05 ?? ?? ?? ??", ?? = any byte) searched in the
// executable sections of a loaded module, plus helpers to follow the
// relative addresses inside x64 instructions.

#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

struct section_range {
    uint8_t* start;
    uint8_t* end;
};

// The module's code sections, or (code = false) its read-only data sections
inline std::vector<section_range> module_sections(uint8_t* base, bool code) {
    std::vector<section_range> out;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
        DWORD c = section->Characteristics;
        bool is_code = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool is_rodata = !is_code && (c & IMAGE_SCN_CNT_INITIALIZED_DATA) && !(c & IMAGE_SCN_MEM_WRITE);
        if (code ? is_code : is_rodata) {
            // only the part backed by the file: the rest is zeros
            DWORD size = section->SizeOfRawData < section->Misc.VirtualSize ? section->SizeOfRawData
                                                                            : section->Misc.VirtualSize;
            out.push_back(section_range{base + section->VirtualAddress, base + section->VirtualAddress + size});
        }
    }
    return out;
}

class signature {
public:
    explicit signature(const char* text) {
        std::istringstream in(text);
        std::string token;
        while (in >> token) {
            bytes.push_back(token == "??" ? -1 : (int)strtoul(token.c_str(), nullptr, 16));
        }
    }

    size_t size() const { return bytes.size(); }

    bool matches(const uint8_t* p) const {
        for (size_t i = 0; i < bytes.size(); i++) {
            if (bytes[i] >= 0 && p[i] != (uint8_t)bytes[i]) return false;
        }
        return true;
    }

    // Every match in [begin, end), stopping after `limit` of them
    std::vector<uint8_t*> find(uint8_t* begin, uint8_t* end, size_t limit = 2) const {
        std::vector<uint8_t*> found;
        if (bytes.empty() || bytes[0] < 0 || end - begin < (ptrdiff_t)bytes.size()) return found;
        uint8_t first = (uint8_t)bytes[0];
        uint8_t* last = end - bytes.size();
        for (uint8_t* p = begin; p <= last; p++) {
            p = (uint8_t*)memchr(p, first, (size_t)(last - p) + 1);
            if (p == nullptr) break;
            if (matches(p)) {
                found.push_back(p);
                if (found.size() >= limit) break;
            }
        }
        return found;
    }

    // Matches in every executable section of the module at `base`
    std::vector<uint8_t*> find_in_module(uint8_t* base, size_t limit = 2) const {
        return find_in(module_sections(base, true), limit);
    }

    // Matches in the read-only data of the module at `base`
    std::vector<uint8_t*> find_in_data(uint8_t* base, size_t limit = 2) const {
        return find_in(module_sections(base, false), limit);
    }

    std::vector<uint8_t*> find_in(const std::vector<section_range>& sections, size_t limit) const {
        std::vector<uint8_t*> found;
        for (const section_range& s : sections) {
            for (uint8_t* p : find(s.start, s.end, limit - found.size())) found.push_back(p);
            if (found.size() >= limit) break;
        }
        return found;
    }

private:
    std::vector<int> bytes;
};

inline int32_t read_i32(const uint8_t* p) {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline uint32_t read_u32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// Target of a relative call or jump (E8/E9 rel32) at p
inline uint8_t* call_target(uint8_t* p) {
    return p + 5 + read_i32(p + 1);
}

// Address a RIP-relative operand points to: `disp` is where the 32-bit
// displacement sits, `insn_end` the address of the next instruction
inline uint8_t* rip_target(uint8_t* disp, uint8_t* insn_end) {
    return insn_end + read_i32(disp);
}

// Every RIP-relative memory operand in the module's code that points into
// [lo, hi), as the address of its 32-bit displacement. Found by trying every
// byte after a ModRM byte of the [rip+disp32] form, so it can include the odd
// false hit; callers check the bytes around each one.
inline std::vector<uint8_t*> rip_refs(uint8_t* base, const uint8_t* lo, const uint8_t* hi) {
    std::vector<uint8_t*> out;
    for (const section_range& s : module_sections(base, true)) {
        for (uint8_t* p = s.start + 1; p + 4 <= s.end; p++) {
            if ((p[-1] & 0xC7) != 0x05) continue;
            const uint8_t* target = p + 4 + read_i32(p);
            if (target >= lo && target < hi) out.push_back(p);
        }
    }
    return out;
}

// How many direct calls (E8 rel32) in the module's code go to `function`
inline size_t count_calls(uint8_t* base, const uint8_t* function) {
    size_t n = 0;
    for (const section_range& s : module_sections(base, true)) {
        for (uint8_t* p = s.start; p + 5 <= s.end; p++) {
            if (*p == 0xE8 && p + 5 + read_i32(p + 1) == function) n++;
        }
    }
    return n;
}

// First match of `sig` in [from, from + window)
inline uint8_t* find_near(const signature& sig, uint8_t* from, size_t window) {
    std::vector<uint8_t*> found = sig.find(from, from + window, 1);
    return found.empty() ? nullptr : found[0];
}

inline std::string hex(uintptr_t v) {
    std::ostringstream s;
    s << "0x" << std::hex << std::uppercase << v;
    return s.str();
}

inline std::string bytes_at(const uint8_t* p, int n) {
    std::ostringstream s;
    s << std::hex << std::uppercase;
    for (int i = 0; i < n; i++) s << (i ? " " : "") << (p[i] < 16 ? "0" : "") << (int)p[i];
    return s.str();
}

#ifdef _WIN64
// Start of the function containing p, from the module's x64 unwind data.
// Functions split into chunks list their parent chunk through
// UNW_FLAG_CHAININFO, which is followed back to the real entry point.
inline uint8_t* function_start(uint8_t* p) {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry((DWORD64)p, &image_base, nullptr);
    for (int depth = 0; entry != nullptr && depth < 32; depth++) {
        const uint8_t* unwind = (const uint8_t*)(image_base + entry->UnwindData);
        const uint8_t UNW_FLAG_CHAININFO_BIT = 0x4;
        if (!((unwind[0] >> 3) & UNW_FLAG_CHAININFO_BIT)) {
            return (uint8_t*)(image_base + entry->BeginAddress);
        }
        uint8_t code_count = unwind[2];
        entry = (PRUNTIME_FUNCTION)(unwind + 4 + ((code_count + 1) & ~1) * 2);
    }
    return nullptr;
}
#endif
