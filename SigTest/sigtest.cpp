// Checks the effector signatures against a game dll without starting the game.
//
// The dll is mapped with DONT_RESOLVE_DLL_REFERENCES, so none of its code
// runs and its imports are not loaded; only the signature search and the
// unwind lookup are exercised.
//
//   sigtest.exe path\to\bm2dx.dll [more dlls...]

#include <windows.h>
#include <iostream>
#include "../LegacyDJ/GameLayout.h"
#include "../LegacyDJ/KeypadLayout.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "usage: sigtest <bm2dx.dll> [...]" << std::endl;
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        std::cout << "== " << argv[i] << std::endl;
        HMODULE module = LoadLibraryExA(argv[i], nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (module == nullptr) {
            std::cout << "could not map it (error " << GetLastError() << ")" << std::endl;
            failures++;
            continue;
        }
        effector::layout layout;
        effector::resolve((uint8_t*)module, layout, std::cout);
        keypad::layout keys;
        keypad::resolve((uint8_t*)module, keys, std::cout);
        std::cout << "sliders " << (layout.sliders_ok ? "OK" : "FAILED") << ", panel "
                  << (layout.panel_ok ? "OK" : "FAILED") << ", keypad keys " << (keys.keys_ok ? "OK" : "FAILED")
                  << ", keypad toggle " << (keys.toggle_ok ? "OK" : "FAILED") << std::endl;
        if (!layout.sliders_ok || !layout.panel_ok || !keys.keys_ok || !keys.toggle_ok) failures++;
        FreeLibrary(module);
    }
    return failures;
}
