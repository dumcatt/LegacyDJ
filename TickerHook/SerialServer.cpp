#include "pch.h"
#include <windows.h>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>
#include "TickerDisplay.h"

// Sends the ticker to the COM port given in tickerhook.conf, either
//   MODE=RELAY    straight to the sub IO ("relay board") in place of a BIO2
//   MODE=ARDUINO  as a line of text, for the iidx-BiTwinkIO Arduino sketch
class serial_server {
public:
    serial_server() : hComm(INVALID_HANDLE_VALUE), port("COM1"), baudrate(115200), relay_mode(true),
                      send_interval_ms(8) {
        LoadConfig();
        display.reset(new ticker_display(config));
    }

    ~serial_server() {
        running = false;
        if (hComm != INVALID_HANDLE_VALUE) {
            CloseHandle(hComm);
        }
    }

    void LoadConfig() {
        std::ifstream configFile("tickerhook.conf");
        if (!configFile.is_open()) {
            std::cout << "Could not open tickerhook.conf, using defaults PORT=" << port << " BAUD=" << baudrate << std::endl;
            return;
        }

        bool static_texts_given = false;
        std::string line;
        while (std::getline(configFile, line)) {
            trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;

            size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            std::string key = line.substr(0, equals);
            std::string value = line.substr(equals + 1);
            trim(key);
            trim(value);
            to_upper(key);

            if (key == "PORT") {
                port = value;
            } else if (key == "BAUD") {
                read_number(key, value, baudrate);
            } else if (key == "MODE") {
                std::string mode = value;
                to_upper(mode);
                if (mode == "RELAY") relay_mode = true;
                else if (mode == "ARDUINO") relay_mode = false;
                else std::cout << "Invalid MODE in config: " << value << std::endl;
            } else if (key == "SCROLL_INTERVAL") {
                read_number(key, value, config.scroll_interval_ms);
            } else if (key == "SCROLL_GAP") {
                read_number(key, value, config.scroll_gap);
            } else if (key == "SPOTLIGHT_INTERVAL") {
                read_number(key, value, config.spotlight_interval_ms);
            } else if (key == "NEON_INTERVAL") {
                read_number(key, value, config.neon_interval_ms);
            } else if (key == "SEND_INTERVAL") {
                read_number(key, value, send_interval_ms);
            } else if (key == "STATIC_TEXT") {
                // the first STATIC_TEXT line replaces the built-in list,
                // an empty one leaves the list empty
                if (!static_texts_given) {
                    config.static_texts.clear();
                    static_texts_given = true;
                }
                if (!value.empty()) {
                    config.static_texts.push_back(value.substr(0, ticker_display::SIZE));
                }
            } else {
                std::cout << "Unknown setting in config: " << key << std::endl;
            }
        }

        if (config.scroll_gap < 0) config.scroll_gap = 0;
        if (send_interval_ms < 1) send_interval_ms = 1;
        if (relay_mode) baudrate = 115200;
    }

    // Runs on its own thread. In relay mode it never returns.
    void run() {
        std::string portName = "\\\\.\\" + port; // Support COM10 and above
        hComm = CreateFileA(portName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (hComm == INVALID_HANDLE_VALUE) {
            std::cout << "Error opening serial port " << port << std::endl;
            return;
        }

        DCB dcbSerialParams = { 0 };
        dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
        if (!GetCommState(hComm, &dcbSerialParams)) {
            std::cout << "Error getting state" << std::endl;
            CloseHandle(hComm);
            hComm = INVALID_HANDLE_VALUE;
            return;
        }

        dcbSerialParams.BaudRate = baudrate;
        dcbSerialParams.ByteSize = 8;
        dcbSerialParams.StopBits = ONESTOPBIT;
        dcbSerialParams.Parity = NOPARITY;
        if (relay_mode) {
            // three-wire link, nothing to hand-shake with
            dcbSerialParams.fOutxCtsFlow = FALSE;
            dcbSerialParams.fOutxDsrFlow = FALSE;
            dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
            dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;
            dcbSerialParams.fOutX = FALSE;
            dcbSerialParams.fInX = FALSE;
        }

        if (!SetCommState(hComm, &dcbSerialParams)) {
            std::cout << "Error setting state" << std::endl;
            CloseHandle(hComm);
            hComm = INVALID_HANDLE_VALUE;
            return;
        }

        COMMTIMEOUTS timeouts = { 0 };
        if (relay_mode) {
            // reads return straight away with whatever has arrived
            timeouts.ReadIntervalTimeout = MAXDWORD;
        } else {
            timeouts.ReadIntervalTimeout = 50;
            timeouts.ReadTotalTimeoutConstant = 50;
            timeouts.ReadTotalTimeoutMultiplier = 10;
        }
        timeouts.WriteTotalTimeoutConstant = 50;
        timeouts.WriteTotalTimeoutMultiplier = 10;
        if (!SetCommTimeouts(hComm, &timeouts)) {
            std::cout << "Error setting timeouts" << std::endl;
            CloseHandle(hComm);
            hComm = INVALID_HANDLE_VALUE;
            return;
        }

        std::cout << "Serial port " << port << " opened successfully at " << baudrate << " baud." << std::endl;

        if (relay_mode) {
            print_relay_config();
            relay_loop();
        }
    }

    void send(const char* text) {
        if (relay_mode) {
            std::lock_guard<std::mutex> lock(display_lock);
            display->set_text(text, GetTickCount());
            return;
        }

        if (hComm == INVALID_HANDLE_VALUE) return;

        DWORD bytesWritten;
        WriteFile(hComm, text, (DWORD)strlen(text), &bytesWritten, NULL);
        WriteFile(hComm, "\n", 1, &bytesWritten, NULL);
    }

    // Stops relay_loop (for the test tool; the game just unloads)
    void stop() { running = false; }

    unsigned long replies() const { return parser.replies; }
    const uint8_t* faders() const { return parser.faders; }

private:
    void relay_loop() {
        DWORD lastReplyCheck = GetTickCount();
        unsigned long repliesAtCheck = 0;
        bool answering = true;

        running = true;
        while (running) {
            DWORD now = GetTickCount();
            char chars[ticker_display::SIZE];
            {
                std::lock_guard<std::mutex> lock(display_lock);
                display->render(now, chars);
            }
            std::vector<uint8_t> frame = relay_board::build_request(
                chars, 0, display->spotlights_on(now) ? 0xFF : 0x00, display->neon_on(now) ? 0x01 : 0x00);

            DWORD bytesWritten = 0;
            if (!WriteFile(hComm, frame.data(), (DWORD)frame.size(), &bytesWritten, NULL)) {
                std::cout << "Error writing to " << port << ", stopping" << std::endl;
                return;
            }

            // Replies are only read to tell whether the board is there
            uint8_t buffer[256];
            DWORD bytesRead = 0;
            while (ReadFile(hComm, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
                for (DWORD i = 0; i < bytesRead; i++) parser.feed(buffer[i]);
            }

            if (now - lastReplyCheck >= 2000) {
                bool answered = parser.replies != repliesAtCheck;
                if (answered != answering) {
                    answering = answered;
                    std::cout << (answering ? "Relay board is answering on " : "Relay board is not answering on ")
                              << port << std::endl;
                }
                repliesAtCheck = parser.replies;
                lastReplyCheck = now;
            }

            Sleep(send_interval_ms);
        }
    }

    void print_relay_config() {
        std::cout << "Relay board mode: scroll " << config.scroll_interval_ms << " ms, gap " << config.scroll_gap
                  << ", spotlights " << config.spotlight_interval_ms << ", neon " << config.neon_interval_ms
                  << ", " << config.static_texts.size() << " static texts" << std::endl;
    }

    static void to_upper(std::string& s) {
        for (char& c : s) c = (char)toupper((unsigned char)c);
    }

    static void trim(std::string& s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }

    template <typename T>
    static void read_number(const std::string& key, const std::string& value, T& target) {
        try {
            target = (T)std::stol(value);
        } catch (...) {
            std::cout << "Invalid " << key << " in config: " << value << std::endl;
        }
    }

    HANDLE hComm;
    std::string port;
    int baudrate;
    bool relay_mode;
    int send_interval_ms;
    ticker_config config;
    std::unique_ptr<ticker_display> display;
    std::mutex display_lock;
    relay_board::reply_parser parser;
    std::atomic<bool> running{false};
};
