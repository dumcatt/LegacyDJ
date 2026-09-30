// Tries the relay board mode of tickerhook without the game.
//
// Reads tickerhook.conf from the current folder exactly like the hook does,
// shows the text given on the command line, then shows every line typed in.
// Fader positions are printed every two seconds. "quit" exits.
// With a number of seconds after the text it runs that long instead of
// reading typed lines.
//
//   relaytest.exe "HELLO WORLD"
//   relaytest.exe "HELLO WORLD" 10

#include "../TickerHook/SerialServer.cpp"

#include <thread>

int main(int argc, char** argv) {
    serial_server server;
    server.send(argc > 1 ? argv[1] : "HELLO");

    std::thread io([&server] { server.run(); });

    std::atomic<bool> quit{false};
    std::thread status([&server, &quit] {
        while (!quit) {
            for (int i = 0; i < 20 && !quit; i++) Sleep(100);
            if (quit) break;
            const uint8_t* f = server.faders();
            std::cout << "replies " << server.replies() << ", faders 1-5:";
            for (int i = 0; i < relay_board::FADER_COUNT; i++) std::cout << ' ' << (f[i] >> 4);
            std::cout << std::endl;
        }
    });

    if (argc > 2) {
        Sleep((DWORD)(atoi(argv[2]) * 1000));
    } else {
        std::cout << "Type a line to show it on the ticker, \"quit\" to exit." << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == "quit") break;
            server.send(line.c_str());
        }
    }

    quit = true;
    server.stop();
    status.join();
    io.join();
    return 0;
}
