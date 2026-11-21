#ifndef BOKKIE_TERMINAL_H
#define BOKKIE_TERMINAL_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace term {

namespace color {
constexpr const char *RESET = "\033[0m";
constexpr const char *BOLD = "\033[1m";
constexpr const char *DIM = "\033[2m";
constexpr const char *COPPER = "\033[38;5;173m";
constexpr const char *MUTED_COPPER = "\033[38;5;137m";
constexpr const char *TEXT = "\033[38;5;252m";
constexpr const char *SECONDARY = "\033[38;5;245m";
constexpr const char *MUTED = "\033[38;5;240m";
constexpr const char *SUCCESS = "\033[38;5;108m";
constexpr const char *WARNING = "\033[38;5;179m";
constexpr const char *FAILURE = "\033[38;5;167m";
constexpr const char *INFO = "\033[38;5;110m";
}

enum class Key { Up, Down, Left, Right, Enter, Escape, Backspace, Quit, Char };

struct KeyEvent {
    Key key = Key::Char;
    std::string text;
};

inline int width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
#else
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    return 80;
}

inline int height() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Bottom - info.srWindow.Top + 1;
#else
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
#endif
    return 24;
}

inline bool input_ready(int timeout_ms) {
#ifdef _WIN32
    (void) timeout_ms;
    return true;
#else
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) > 0;
#endif
}

inline std::vector<KeyEvent> read_keys() {
    std::vector<KeyEvent> keys;
#ifdef _WIN32
    return keys;
#else
    char buf[4096];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
        keys.push_back({Key::Quit, ""});
        return keys;
    }
    for (ssize_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0x1B) {
            if (i + 1 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
                ssize_t j = i + 2;
                while (j < n && !(buf[j] >= 0x40 && buf[j] <= 0x7E)) ++j;
                if (j < n) {
                    switch (buf[j]) {
                        case 'A': keys.push_back({Key::Up, ""}); break;
                        case 'B': keys.push_back({Key::Down, ""}); break;
                        case 'C': keys.push_back({Key::Right, ""}); break;
                        case 'D': keys.push_back({Key::Left, ""}); break;
                        default: break;
                    }
                }
                i = j;
            } else {
                keys.push_back({Key::Escape, ""});
            }
        } else if (c == '\r' || c == '\n') {
            keys.push_back({Key::Enter, ""});
        } else if (c == 127 || c == 8) {
            keys.push_back({Key::Backspace, ""});
        } else if (c == 3 || c == 4) {
            keys.push_back({Key::Quit, ""});
        } else if (c >= 32) {
            std::string character(1, buf[i]);
            while (i + 1 < n && (static_cast<unsigned char>(buf[i + 1]) & 0xC0) == 0x80) {
                character += buf[++i];
            }
            keys.push_back({Key::Char, character});
        }
    }
    return keys;
#endif
}

#ifndef _WIN32
class RawMode {
public:
    RawMode() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;
        if (tcgetattr(STDIN_FILENO, &saved_) != 0) return;
        termios raw = saved_;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
        active_ = true;
        std::cout << "\033[?25l" << std::flush;
    }

    ~RawMode() {
        if (!active_) return;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
        std::cout << "\033[?25h" << std::flush;
    }

    RawMode(const RawMode &) = delete;
    RawMode &operator=(const RawMode &) = delete;

    bool active() const { return active_; }

private:
    termios saved_{};
    bool active_ = false;
};
#else
class RawMode {
public:
    bool active() const { return false; }
};
#endif

inline std::size_t display_width(const std::string &text) {
    std::size_t w = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1B && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !(static_cast<unsigned char>(text[i]) >= 0x40 &&
                     static_cast<unsigned char>(text[i]) <= 0x7E)) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++w;
    }
    return w;
}

inline std::string truncate(const std::string &text, std::size_t max_len) {
    if (display_width(text) <= max_len) return text;
    if (max_len < 2) return "…";
    std::size_t count = 0, i = 0;
    while (i < text.size() && count < max_len - 1) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        ++count;
    }
    return text.substr(0, i) + "…";
}

inline std::string fit_tail(const std::string &text, std::size_t max_len) {
    if (display_width(text) <= max_len) return text;
    if (max_len < 2) return "…";
    std::size_t start = text.size(), count = 0;
    while (start > 0 && count < max_len - 1) {
        --start;
        if ((static_cast<unsigned char>(text[start]) & 0xC0) != 0x80) ++count;
    }
    return "…" + text.substr(start);
}

inline std::string format_age(std::int64_t epoch_seconds) {
    if (epoch_seconds <= 0) return "";
    const std::int64_t now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    const std::int64_t diff = now > epoch_seconds ? now - epoch_seconds : 0;
    if (diff < 60) return "now";
    if (diff < 3600) return std::to_string(diff / 60) + "m";
    if (diff < 86400) return std::to_string(diff / 3600) + "h";
    if (diff < 86400 * 14) return std::to_string(diff / 86400) + "d";
    return std::to_string(diff / (86400 * 7)) + "w";
}

}  // namespace term

#endif //BOKKIE_TERMINAL_H
