//
// Created by Noah Luke Basson on 2026/11/18.
//

#include "LandingScreen.h"
#include "Client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ls_ansi {
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

namespace {
    const std::vector<std::string> kMenuItems = {
        "Quick chat",
        "Workspaces",
        "Settings",
    };

#ifndef _WIN32
    // Puts the terminal in raw mode for the lifetime of the object and always puts it back.
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
}


LandingScreen::LandingScreen(Workspace& workspaces) : workspaces_(workspaces) {
}

void LandingScreen::setNotice(const std::string& notice) {
    notice_ = notice;
}

size_t LandingScreen::displayWidth(const std::string& text) {
    size_t width = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x1B && i + 1 < text.size() && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() &&
                   !(static_cast<unsigned char>(text[i]) >= 0x40 &&
                     static_cast<unsigned char>(text[i]) <= 0x7E)) ++i;
            continue;
        }
        if ((c & 0xC0) != 0x80) ++width;
    }
    return width;
}

std::string LandingScreen::truncate(const std::string& text, size_t maxLen) {
    if (visibleLength(text) <= maxLen) return text;
    if (maxLen < 2) return "…";
    size_t count = 0, i = 0;
    while (i < text.size() && count < maxLen - 1) {
        ++i;
        while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) ++i;
        ++count;
    }
    return text.substr(0, i) + "…";
}

std::string LandingScreen::formatAge(std::int64_t epochSeconds) {
    if (epochSeconds <= 0) return "";
    const std::int64_t now = static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    const std::int64_t diff = std::max<std::int64_t>(0, now - epochSeconds);
    if (diff < 60) return "now";
    if (diff < 3600) return std::to_string(diff / 60) + "m";
    if (diff < 86400) return std::to_string(diff / 3600) + "h";
    if (diff < 86400 * 14) return std::to_string(diff / 86400) + "d";
    return std::to_string(diff / (86400 * 7)) + "w";
}

std::string LandingScreen::recentLine(const Workspace::ChatInfo& chat, bool selected, size_t width) {
    std::string suffix;
    if (width >= 34) {
        const std::string age = formatAge(chat.updated_at);
        suffix = truncate(chat.workspace, width / 3);
        if (!age.empty()) suffix += "  " + age;
    }
    const size_t reserved = 2 + (suffix.empty() ? 0 : visibleLength(suffix) + 2);
    const std::string title = truncate(chat.name, width > reserved ? width - reserved : 1);
    const size_t used = 2 + visibleLength(title) + (suffix.empty() ? 0 : visibleLength(suffix));
    const size_t pad = suffix.empty() ? 0 : (width > used ? width - used : 2);

    std::string line;
    if (selected) {
        line = std::string(ls_ansi::COPPER) + "> " + ls_ansi::BOLD + ls_ansi::TEXT + title + ls_ansi::RESET;
    } else {
        line = std::string("  ") + ls_ansi::SECONDARY + title + ls_ansi::RESET;
    }
    if (!suffix.empty()) line += std::string(pad, ' ') + ls_ansi::MUTED + suffix + ls_ansi::RESET;
    return line;
}

int LandingScreen::terminalWidth() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
#else
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
#endif
    return 80;
}

int LandingScreen::contentWidth() {
    return std::max(40, terminalWidth() - 2);
}

size_t LandingScreen::visibleLength(const std::string &text) {
    size_t length = 0;
    for (unsigned char c: text)
        if ((c & 0xC0) != 0x80) ++length;
    return length;
}

std::string LandingScreen::fitTail(const std::string &text, size_t maxLen) {
    if (visibleLength(text) <= maxLen) return text;
    if (maxLen < 2) return "…";
    size_t start = text.size(), count = 0;
    while (start > 0 && count < maxLen - 1) {
        --start;
        if ((static_cast<unsigned char>(text[start]) & 0xC0) != 0x80) ++count;
    }
    return "…" + text.substr(start);
}

std::string LandingScreen::centered(const std::string &text, int width) {
    const int length = static_cast<int>(visibleLength(text));
    const int pad = std::max(0, (width - length) / 2);
    return std::string(static_cast<size_t>(pad), ' ') + text;
}

void LandingScreen::printRule(int width) {
    std::cout << ls_ansi::MUTED;
    for (int i = 0; i < width; ++i) std::cout << "─";
    std::cout << ls_ansi::RESET << '\n';
}

void LandingScreen::checkStatus() {
    const ServerStatus server = Client::probe_server();
    status_.serverUp = server.reachable;
    status_.model = server.model_name;
    status_.workspaceCount = workspaces_.getWorkspaces().size();
}

void LandingScreen::printRow(const std::string &label, const std::string &value,
                             const char *valueColor, int width) {
    const size_t labelLen = visibleLength(label);
    const size_t w = static_cast<size_t>(width);
    const size_t maxValue = w > labelLen + 2 ? w - labelLen - 2 : 1;
    const std::string shown = fitTail(value, maxValue);
    const int used = static_cast<int>(labelLen + visibleLength(shown));
    const int gap = std::max(1, width - used);
    std::cout << ls_ansi::SECONDARY << label << ls_ansi::RESET
            << std::string(static_cast<size_t>(gap), ' ')
            << valueColor << shown << ls_ansi::RESET << '\n';
}

void LandingScreen::displayStatus() {
    const int width = contentWidth();
    const bool ready = status_.serverUp;

    std::string model = "Unknown";
    if (status_.serverUp) model = status_.model.empty() ? "Not reported" : status_.model;
    const std::string workspaces = status_.workspaceCount == 0
        ? "None yet"
        : std::to_string(status_.workspaceCount) +
          (status_.workspaceCount == 1 ? " workspace" : " workspaces");

    std::cout << '\n';
    printRow("RUNTIME STATUS", ready ? "OPERATIONAL" : "ATTENTION REQUIRED",
             ready ? ls_ansi::SUCCESS : ls_ansi::WARNING, width);
    std::cout << '\n';
    printRow("Model server", status_.serverUp ? "Connected" : "Not reachable",
             status_.serverUp ? ls_ansi::SUCCESS : ls_ansi::FAILURE, width);
    printRow("Model", model,
             status_.serverUp && !status_.model.empty() ? ls_ansi::TEXT : ls_ansi::MUTED, width);
    printRow("Workspaces", workspaces,
             status_.workspaceCount == 0 ? ls_ansi::MUTED : ls_ansi::TEXT, width);

    if (!status_.serverUp) {
        std::cout << '\n' << ls_ansi::WARNING
                << "Model server unreachable: start llama-server or set BOKKIE_SERVER_URL."
                << ls_ansi::RESET << '\n';
    }
    std::cout << '\n';
}

void LandingScreen::displayBrand() {
    const int width = contentWidth();
    const std::string name = "BOKKIE";
    const std::string description = "LOCAL AGENT RUNTIME";
    const std::vector<std::string> logo = {
        R"(   /\  /\)",
        R"(/ \______/ \)",
        R"(     /\)",
    };

    size_t logoWidth = 0;
    for (const std::string &row: logo)
        logoWidth = std::max(logoWidth, visibleLength(row));
    const size_t pad = (static_cast<size_t>(width) - logoWidth) / 2;

    std::cout << '\n';
    for (const std::string &row: logo)
        std::cout << std::string(pad, ' ') << ls_ansi::COPPER << row << ls_ansi::RESET << '\n';
    std::cout << '\n'
            << ls_ansi::BOLD << ls_ansi::COPPER << centered(name, width) << ls_ansi::RESET << '\n'
            << ls_ansi::SECONDARY << centered(description, width) << ls_ansi::RESET << '\n'
            << '\n';
}

void LandingScreen::displayMenu() {
    const size_t width = static_cast<size_t>(contentWidth());
    const bool twoPane = width >= 72;
    const size_t gap = 3;
    const size_t leftWidth = twoPane ? std::min<size_t>(width / 2, 44) : width;
    const size_t rightWidth = twoPane ? width - leftWidth - gap : width;

    std::vector<std::string> left;
    left.push_back("");
    left.push_back(std::string(ls_ansi::TEXT) + "Welcome back." + ls_ansi::RESET);
    left.push_back(std::string(ls_ansi::SECONDARY) + "Select an operation to continue." + ls_ansi::RESET);
    left.push_back("");
    for (size_t i = 0; i < kMenuItems.size(); ++i) {
        const bool marked = focus_ == Focus::Menu && static_cast<int>(i) == selected_;
        left.push_back(marked
            ? std::string(ls_ansi::COPPER) + "> " + ls_ansi::BOLD + ls_ansi::TEXT + kMenuItems[i] + ls_ansi::RESET
            : std::string("  ") + ls_ansi::SECONDARY + kMenuItems[i] + ls_ansi::RESET);
    }

    std::vector<std::string> right;
    right.push_back("");
    right.push_back(std::string(ls_ansi::SECONDARY) + "RECENT CHATS" + ls_ansi::RESET);
    right.push_back(std::string(ls_ansi::MUTED) + "Pick up where you left off." + ls_ansi::RESET);
    right.push_back("");
    if (recents_.empty()) {
        right.push_back(std::string(ls_ansi::MUTED) + "No chats yet." + ls_ansi::RESET);
    } else {
        for (size_t i = 0; i < recents_.size(); ++i) {
            const bool marked = focus_ == Focus::Recent && static_cast<int>(i) == recentSelected_;
            right.push_back(recentLine(recents_[i], marked, rightWidth));
        }
    }

    std::vector<std::string> rows;
    if (twoPane) {
        const size_t count = std::max(left.size(), right.size());
        for (size_t i = 0; i < count; ++i) {
            const std::string l = i < left.size() ? left[i] : "";
            const std::string r = i < right.size() ? right[i] : "";
            const size_t lw = std::min(leftWidth, displayWidth(l));
            rows.push_back(l + std::string(leftWidth - lw + gap, ' ') + r);
        }
    } else {
        rows = left;
        rows.insert(rows.end(), right.begin(), right.end());
    }

    for (const std::string& row : rows) std::cout << row << "\033[K\n";
    std::cout << '\n' << ls_ansi::WARNING << notice_ << ls_ansi::RESET << "\033[K\n";
}

void LandingScreen::displayFooter() {
    const int width = contentWidth();
    std::string hint = "Enter Continue";
    if (interactive_) {
        hint = width >= 60 ? "↑↓ Navigate · ←→ Switch pane · Enter Select" : "↑↓ ←→ Enter";
    }
    printRow(hint, "Bokkie", ls_ansi::MUTED, width);
}

void LandingScreen::render(bool full) {
    std::cout << (full ? "\033[H\033[2J\033[3J" : "\033[H");
    const int width = contentWidth();
    displayBrand();
    printRule(width);
    displayStatus();
    printRule(width);
    displayMenu();
    printRule(width);
    displayFooter();
    std::cout << "\033[J" << std::flush;
}

bool LandingScreen::inputReady(int timeoutMs) {
#ifdef _WIN32
    (void) timeoutMs;
    return true;
#else
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);
    timeval timeout{0, timeoutMs * 1000};
    return select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) > 0;
#endif
}

void LandingScreen::discardLine() {
#ifndef _WIN32
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1 && c != '\n') {
    }
#endif
}

std::vector<LandingScreen::Key> LandingScreen::readKeys() {
    std::vector<Key> keys;
#ifdef _WIN32
    keys.push_back(Key::Quit);
    return keys;
#else
    char buf[32];
    const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
        keys.push_back(Key::Quit);
        return keys;
    }
    for (ssize_t i = 0; i < n; ++i) {
        const char c = buf[i];
        if (c == '\033' && i + 2 < n && (buf[i + 1] == '[' || buf[i + 1] == 'O')) {
            if (buf[i + 2] == 'A') keys.push_back(Key::Up);
            else if (buf[i + 2] == 'B') keys.push_back(Key::Down);
            else if (buf[i + 2] == 'C') keys.push_back(Key::Right);
            else if (buf[i + 2] == 'D') keys.push_back(Key::Left);
            i += 2;
        } else if (c == '\n' || c == '\r') {
            keys.push_back(Key::Enter);
        } else if (c == 3 || c == 4) {
            keys.push_back(Key::Quit);
        }
    }
    return keys;
#endif
}

LandingChoice LandingScreen::displayLandingScreen() {
    checkStatus();
    recents_ = workspaces_.recentChats(5);
    recentSelected_ = 0;
    focus_ = Focus::Menu;

    RawMode rawMode;
    interactive_ = rawMode.active();
    int drawnWidth = terminalWidth();
    render(true);
    LandingChoice choice;

    if (!interactive_) {
        while (!inputReady(100)) {
            const int width = terminalWidth();
            if (width != drawnWidth) {
                drawnWidth = width;
                render(true);
            }
        }
        discardLine();
        std::cout << '\n';
        choice.action = LandingChoice::Action::QuickChat;
        return choice;
    }

    while (true) {
        bool changed = false;
        if (inputReady(100)) {
            for (const Key key: readKeys()) {
                switch (key) {
                    case Key::Up:
                    case Key::Down:
                    {
                        const bool down = key == Key::Down;
                        if (focus_ == Focus::Menu) {
                            const int count = static_cast<int>(kMenuItems.size());
                            selected_ = (selected_ + (down ? 1 : count - 1)) % count;
                        } else {
                            const int count = static_cast<int>(recents_.size());
                            recentSelected_ = (recentSelected_ + (down ? 1 : count - 1)) % count;
                        }
                        notice_.clear();
                        changed = true;
                        break;
                    }
                    case Key::Left:
                        if (focus_ != Focus::Menu) {
                            focus_ = Focus::Menu;
                            notice_.clear();
                            changed = true;
                        }
                        break;
                    case Key::Right:
                        if (!recents_.empty() && focus_ != Focus::Recent) {
                            focus_ = Focus::Recent;
                            notice_.clear();
                            changed = true;
                        }
                        break;
                    case Key::Enter:
                        if (focus_ == Focus::Recent) {
                            const Workspace::ChatInfo& chat = recents_[static_cast<size_t>(recentSelected_)];
                            choice.action = LandingChoice::Action::OpenChat;
                            choice.workspace = chat.workspace;
                            choice.chat = chat.name;
                        } else if (selected_ == 0) {
                            choice.action = LandingChoice::Action::QuickChat;
                        } else if (selected_ == 1) {
                            choice.action = LandingChoice::Action::Workspaces;
                        } else {
                            choice.action = LandingChoice::Action::Settings;
                        }
                        return choice;
                    case Key::Quit:
                        choice.action = LandingChoice::Action::Quit;
                        return choice;
                }
            }
        }
        const int width = terminalWidth();
        if (width != drawnWidth) {
            drawnWidth = width;
            render(true);
        } else if (changed) {
            render(false);
        }
    }
}
