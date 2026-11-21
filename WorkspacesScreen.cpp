#include "WorkspacesScreen.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>

namespace color = term::color;

namespace {

struct Window {
    int start = 0;
    int end = 0;
};

Window windowFor(int total, int selected, int rows) {
    Window w;
    if (total <= rows) {
        w.end = total;
        return w;
    }
    w.start = std::clamp(selected - rows / 2, 0, total - rows);
    w.end = w.start + rows;
    return w;
}

std::string item(const std::string& title, const std::string& detail, const char* detailColor,
                 bool selected, size_t width, const char* titleColor) {
    const size_t avail = width > 2 ? width - 2 : 1;
    const std::string shownDetail = detail.empty() ? "" : term::fit_tail(detail, avail * 55 / 100);
    const size_t detailLen = term::display_width(shownDetail);
    const size_t titleMax = avail > detailLen + 2 ? avail - detailLen - 2 : 1;
    const std::string shownTitle = term::truncate(title, titleMax);
    const size_t titleLen = term::display_width(shownTitle);
    const size_t pad = avail > titleLen + detailLen ? avail - titleLen - detailLen : 1;

    std::string line = selected
        ? std::string(color::COPPER) + "> " + color::BOLD + color::TEXT + shownTitle + color::RESET
        : std::string("  ") + titleColor + shownTitle + color::RESET;
    if (!shownDetail.empty()) {
        line += std::string(pad, ' ') + detailColor + shownDetail + color::RESET;
    }
    return line;
}

void popUtf8(std::string& text) {
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
    if (!text.empty()) text.pop_back();
}

}  // namespace

WorkspacesScreen::WorkspacesScreen(Workspace& workspaces) : workspaces_(workspaces) {
}

void WorkspacesScreen::setNotice(const std::string& text, const char* noticeColor) {
    notice_ = text;
    noticeColor_ = noticeColor;
}

void WorkspacesScreen::clearNotice() {
    notice_.clear();
}

void WorkspacesScreen::refreshWorkspaces() {
    entries_.clear();
    for (const std::string& name : workspaces_.getWorkspaces()) {
        Entry entry;
        entry.name = name;
        const std::filesystem::path project = workspaces_.projectPath(name);
        entry.project = project.string();
        std::error_code error;
        entry.projectMissing = project.empty() || !std::filesystem::is_directory(project, error);
        entries_.push_back(entry);
    }
    selected_ = std::clamp(selected_, 0, static_cast<int>(entries_.size()));
}

void WorkspacesScreen::refreshChats() {
    chats_ = workspaces_.listChats(current_);
    chatSelected_ = std::clamp(chatSelected_, 0, static_cast<int>(chats_.size()));
}

void WorkspacesScreen::openWorkspace(const std::string& name) {
    current_ = name;
    const std::filesystem::path project = workspaces_.projectPath(name);
    currentProject_ = project.string();
    std::error_code error;
    currentMissing_ = project.empty() || !std::filesystem::is_directory(project, error);
    chatSelected_ = 0;
    refreshChats();
    clearNotice();
    if (currentMissing_) {
        setNotice("The project folder is missing, so chats can't be opened until it exists.",
                  color::WARNING);
    }
    mode_ = Mode::Chats;
}

void WorkspacesScreen::submitNewWorkspace() {
    if (input_.find_first_not_of(" \t") == std::string::npos) {
        setNotice("Paste a folder path first.", color::WARNING);
        return;
    }
    const Workspace::CreateResult made =
        workspaces_.createWorkspaceForProject(Workspace::parseUserPath(input_));
    if (!made.ok) {
        setNotice(made.error + ".", color::FAILURE);
        return;
    }
    refreshWorkspaces();
    openWorkspace(made.name);
    setNotice(made.existed ? "Opened the existing workspace \"" + made.name + "\"."
                           : "Created workspace \"" + made.name + "\".",
              color::SUCCESS);
}

std::string WorkspacesScreen::newChatName() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H.%M", &local);
    const std::string base = std::string("Chat ") + buffer;
    for (int n = 1; n <= 20; ++n) {
        const std::string name = n == 1 ? base : base + " (" + std::to_string(n) + ")";
        if (workspaces_.createChat(current_, name)) return name;
    }
    return {};
}

bool WorkspacesScreen::handleKey(const term::KeyEvent& key, WorkspacesResult& result) {
    using term::Key;
    const bool yes = key.key == Key::Char && (key.text == "y" || key.text == "Y");
    const bool deleteKey = key.key == Key::Char && (key.text == "d" || key.text == "D");

    switch (mode_) {
        case Mode::Workspaces: {
            const int count = static_cast<int>(entries_.size()) + 1;
            switch (key.key) {
                case Key::Up:
                    selected_ = (selected_ + count - 1) % count;
                    clearNotice();
                    break;
                case Key::Down:
                    selected_ = (selected_ + 1) % count;
                    clearNotice();
                    break;
                case Key::Enter:
                    if (selected_ == 0) {
                        input_.clear();
                        clearNotice();
                        mode_ = Mode::NewWorkspace;
                    } else {
                        openWorkspace(entries_[static_cast<size_t>(selected_ - 1)].name);
                    }
                    break;
                case Key::Escape:
                case Key::Left:
                case Key::Quit:
                    return true;
                default:
                    if (deleteKey && selected_ > 0) {
                        pendingName_ = entries_[static_cast<size_t>(selected_ - 1)].name;
                        setNotice("Delete workspace \"" + pendingName_ +
                                  "\"? Its chats go to the trash; your project is untouched. [y/N]",
                                  color::WARNING);
                        mode_ = Mode::ConfirmWorkspace;
                    }
                    break;
            }
            return false;
        }
        case Mode::Chats: {
            const int count = static_cast<int>(chats_.size()) + 1;
            switch (key.key) {
                case Key::Up:
                    chatSelected_ = (chatSelected_ + count - 1) % count;
                    clearNotice();
                    break;
                case Key::Down:
                    chatSelected_ = (chatSelected_ + 1) % count;
                    clearNotice();
                    break;
                case Key::Enter: {
                    std::string chat;
                    if (chatSelected_ == 0) {
                        chat = newChatName();
                        if (chat.empty()) {
                            setNotice("Could not create a chat.", color::FAILURE);
                            break;
                        }
                    } else {
                        chat = chats_[static_cast<size_t>(chatSelected_ - 1)].name;
                    }
                    result.action = WorkspacesResult::Action::OpenChat;
                    result.workspace = current_;
                    result.chat = chat;
                    return true;
                }
                case Key::Escape:
                case Key::Left:
                    refreshWorkspaces();
                    mode_ = Mode::Workspaces;
                    clearNotice();
                    break;
                case Key::Quit:
                    return true;
                default:
                    if (deleteKey && chatSelected_ > 0) {
                        pendingName_ = chats_[static_cast<size_t>(chatSelected_ - 1)].name;
                        setNotice("Delete chat \"" + pendingName_ +
                                  "\"? It goes to the trash. [y/N]", color::WARNING);
                        mode_ = Mode::ConfirmChat;
                    }
                    break;
            }
            return false;
        }
        case Mode::NewWorkspace:
            switch (key.key) {
                case Key::Char:
                    input_ += key.text;
                    clearNotice();
                    break;
                case Key::Backspace:
                    popUtf8(input_);
                    break;
                case Key::Enter:
                    submitNewWorkspace();
                    break;
                case Key::Escape:
                case Key::Quit:
                    mode_ = Mode::Workspaces;
                    clearNotice();
                    break;
                default:
                    break;
            }
            return false;
        case Mode::ConfirmWorkspace:
            if (yes) {
                const bool ok = workspaces_.deleteWorkspace(pendingName_);
                refreshWorkspaces();
                setNotice(ok ? "Moved \"" + pendingName_ + "\" to the trash."
                             : "Could not delete \"" + pendingName_ + "\".",
                          ok ? color::SUCCESS : color::FAILURE);
            } else {
                clearNotice();
            }
            mode_ = Mode::Workspaces;
            return false;
        case Mode::ConfirmChat:
            if (yes) {
                const bool ok = workspaces_.deleteChat(current_, pendingName_);
                refreshChats();
                setNotice(ok ? "Moved \"" + pendingName_ + "\" to the trash."
                             : "Could not delete \"" + pendingName_ + "\".",
                          ok ? color::SUCCESS : color::FAILURE);
            } else {
                clearNotice();
            }
            mode_ = Mode::Chats;
            return false;
    }
    return false;
}

void WorkspacesScreen::render(bool full) {
    const size_t width = static_cast<size_t>(std::max(40, term::width() - 2));
    const int listRows = std::max(5, term::height() - 11);
    const bool inChats = mode_ == Mode::Chats || mode_ == Mode::ConfirmChat;

    auto rule = [&]() {
        std::string line = color::MUTED;
        for (size_t i = 0; i < width; ++i) line += "─";
        return line + color::RESET;
    };
    auto twoColumn = [&](const std::string& left, const std::string& leftColor,
                         const std::string& right) {
        const size_t rightLen = term::display_width(right);
        const std::string shownLeft =
            term::truncate(left, width > rightLen + 2 ? width - rightLen - 2 : 1);
        const size_t used = term::display_width(shownLeft) + rightLen;
        const size_t pad = width > used ? width - used : 1;
        return leftColor + shownLeft + color::RESET + std::string(pad, ' ') +
               color::MUTED + right + color::RESET;
    };
    auto more = [&](const char* arrow, int count) {
        return std::string(color::MUTED) + "  " + arrow + " " + std::to_string(count) +
               " more" + color::RESET;
    };

    std::vector<std::string> lines;
    auto addList = [&](int total, int selected, const std::function<std::string(int)>& make) {
        const bool scroll = total > listRows;
        const int itemRows = scroll ? listRows - 2 : listRows;
        const Window w = windowFor(total, selected, itemRows);
        if (scroll) lines.push_back(w.start > 0 ? more("↑", w.start) : "");
        for (int i = w.start; i < w.end; ++i) lines.push_back(make(i));
        if (scroll) lines.push_back(w.end < total ? more("↓", total - w.end) : "");
    };

    std::string footerHint;
    lines.push_back("");
    if (mode_ == Mode::NewWorkspace) {
        lines.push_back(twoColumn("NEW WORKSPACE", std::string(color::BOLD) + color::COPPER, ""));
        lines.push_back(std::string(color::SECONDARY) + "Paste the path to your project folder." +
                        color::RESET);
        lines.push_back(rule());
        lines.push_back("");
        lines.push_back(std::string(color::COPPER) + "> " + color::TEXT +
                        term::fit_tail(input_, width > 4 ? width - 4 : 1) + color::RESET +
                        "\033[7m \033[27m");
        footerHint = "Enter Create · Esc Cancel";
    } else if (inChats) {
        const std::string project = term::fit_tail(
            currentProject_ + (currentMissing_ ? "  (folder missing)" : ""), width / 2);
        lines.push_back(twoColumn(current_, std::string(color::BOLD) + color::COPPER, project));
        lines.push_back(std::string(color::SECONDARY) + "Chats in this workspace." + color::RESET);
        lines.push_back(rule());
        lines.push_back("");
        addList(static_cast<int>(chats_.size()) + 1, chatSelected_, [&](int i) {
            if (i == 0) {
                return item("+ New chat", "", color::MUTED, chatSelected_ == 0, width,
                            color::MUTED_COPPER);
            }
            const Workspace::ChatInfo& chat = chats_[static_cast<size_t>(i - 1)];
            std::string detail = chat.message_count == 0
                ? "empty"
                : std::to_string(chat.message_count) +
                  (chat.message_count == 1 ? " message" : " messages");
            const std::string age = term::format_age(chat.updated_at);
            if (!age.empty()) detail += "  " + age;
            return item(chat.name, detail, color::MUTED, chatSelected_ == i, width,
                        color::SECONDARY);
        });
        footerHint = "↑↓ Navigate · Enter Open · D Delete · Esc Back";
    } else {
        const size_t count = entries_.size();
        const std::string total = count == 0 ? ""
            : std::to_string(count) + (count == 1 ? " workspace" : " workspaces");
        lines.push_back(twoColumn("WORKSPACES", std::string(color::BOLD) + color::COPPER, total));
        lines.push_back(std::string(color::SECONDARY) +
                        "Each workspace is a project folder with its own chats." + color::RESET);
        lines.push_back(rule());
        lines.push_back("");
        addList(static_cast<int>(count) + 1, selected_, [&](int i) {
            if (i == 0) {
                return item("+ New workspace", "", color::MUTED, selected_ == 0, width,
                            color::MUTED_COPPER);
            }
            const Entry& entry = entries_[static_cast<size_t>(i - 1)];
            return item(entry.name, entry.projectMissing ? "folder missing" : entry.project,
                        entry.projectMissing ? color::FAILURE : color::MUTED, selected_ == i,
                        width, color::SECONDARY);
        });
        footerHint = "↑↓ Navigate · Enter Open · D Delete · Esc Back";
    }
    if (mode_ == Mode::ConfirmWorkspace || mode_ == Mode::ConfirmChat) {
        footerHint = "Y Delete · any other key Cancel";
    }

    lines.push_back("");
    lines.push_back(notice_.empty()
                        ? std::string()
                        : std::string(noticeColor_) + term::truncate(notice_, width) + color::RESET);
    lines.push_back(rule());
    lines.push_back(twoColumn(footerHint, color::SECONDARY, "Bokkie"));

    std::cout << (full ? "\033[H\033[2J\033[3J" : "\033[H");
    for (const std::string& line : lines) std::cout << line << "\033[K\n";
    std::cout << "\033[J" << std::flush;
}

WorkspacesResult WorkspacesScreen::run() {
    WorkspacesResult result;
    term::RawMode raw;
    if (!raw.active()) {
        result.notice = "The Workspaces screen needs a real terminal; this console can't read arrow keys.";
        return result;
    }

    refreshWorkspaces();
    int drawnWidth = term::width();
    int drawnHeight = term::height();
    render(true);

    while (true) {
        bool changed = false;
        if (term::input_ready(100)) {
            for (const term::KeyEvent& key : term::read_keys()) {
                if (handleKey(key, result)) return result;
                changed = true;
            }
        }
        const int w = term::width();
        const int h = term::height();
        if (w != drawnWidth || h != drawnHeight) {
            drawnWidth = w;
            drawnHeight = h;
            render(true);
        } else if (changed) {
            render(false);
        }
    }
}
