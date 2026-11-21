#pragma once

// md_render.h - streaming Markdown renderer for Bokkie
//
// KNOWN ISSUES / THINGS TO INVESTIGATE
//
// Terminal support
//  - Clickable links (OSC 8): CLion's Run console prints the raw escape
//    codes as junk, Apple Terminal ignores them (plain underlined text),
//    iTerm2 / VS Code / Windows Terminal / Kitty / WezTerm / Ghostty
//    should work. Idea: only emit OSC 8 when TERM_PROGRAM, WT_SESSION,
//    KITTY_WINDOW_ID or VTE_VERSION says the terminal supports it, with
//    a BOKKIE_LINKS=0/1 override. Test the JetBrains terminal tab.
//  - Larger heading text (OSC 66, kitty text sizing protocol): only
//    Kitty 0.40+ and Ghostty as far as I know. Headings are bold colour
//    tiers for now; big text would be an optional upgrade.
//  - Windows: needs a real test in PowerShell, cmd and Windows Terminal.
//    enable_ansi() switches on colour codes and UTF-8 (box drawing).
//  - Italic and strikethrough are ignored by some older terminals.
//  - Colours are tuned for dark backgrounds; check light themes.
//  - Code block shading uses "erase to end of line" (\033[K), which
//    some consoles ignore (shading then stays ragged).
//
// Rendering
//  - Tables: widths count code points, so CJK and emoji (2 columns
//    wide) will misalign. Tables wider than the terminal shrink their
//    widest columns and wrap cell text (skipped when stdout isn't a tty).
//    HTML in cells (<br>, <ul>) prints literally. A pipe inside
//    backticks still splits a cell. Links inside wrapped cells may click
//    oddly. A table does not stream; it prints when it ends.
//  - Long lines in lists and quotes wrap without a hanging indent.
//  - Not handled: strikethrough (~~), task lists, images, underscore
//    emphasis, bold or italic that spans several lines.
//  - Paths: only absolute, ~/ and ./ paths that exist are linked.
//    Windows paths (C:\...) and file.cpp:301 style suffixes are not.
//  - Thinking line: redrawn with a carriage return only (no cursor-up),
//    which should work in nearly every terminal. Width is read once when
//    thinking starts, so resizing mid-thought can leave leftovers. When
//    stdout isn't a tty it prints plain "Thinking..." instead.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace md_ansi {
    constexpr const char *reset = "\033[0m";
    constexpr const char *bold_on = "\033[1m";
    constexpr const char *bold_off = "\033[22m";
    constexpr const char *italic_on = "\033[3m";
    constexpr const char *italic_off = "\033[23m";
    constexpr const char *code_on = "\033[48;5;236m\033[38;5;215m";
    constexpr const char *code_off = "\033[49m\033[39m";
    constexpr const char *h1 = "\033[1;38;5;81m";
    constexpr const char *h2 = "\033[1;4;38;5;111m";
    constexpr const char *h3 = "\033[1;38;5;114m";
    constexpr const char *h4 = "\033[1;38;5;245m";
    constexpr const char *dim = "\033[38;5;245m";
    constexpr const char *quote_text = "\033[38;5;250m";
    constexpr const char *fg_off = "\033[39m";
    constexpr const char *fence_bg = "\033[48;5;235m\033[38;5;252m";
    constexpr const char *link_on = "\033[4;38;5;75m";
    constexpr const char *link_off = "\033[24m\033[39m";
    constexpr const char *table = "\033[38;5;67m";
}

namespace thinking {
    // Animation frame sets. To switch animation, point `frames` at another set.
    inline const std::vector<std::string> spinner = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    inline const std::vector<std::string> dots = {"·  ", "·· ", "···", " ··", "  ·", "   "};
    inline const std::vector<std::string> bounce = {"[=   ]", "[ =  ]", "[  = ]", "[   =]", "[  = ]", "[ =  ]"};
    inline const std::vector<std::string> pulse = {"○", "◔", "◑", "◕", "●", "◕", "◑", "◔"};
    inline const std::vector<std::string> &frames = spinner;

    constexpr int tokens_per_frame = 4; // reasoning tokens per animation step; raise it to slow the animation
    constexpr long redraw_every = 6;
    constexpr size_t tail_chars = 60; // reasoning characters shown after the label
    constexpr const char *label = "Reasoning";
    constexpr const char *done_label = "Reasoned for";
}

inline void enable_ansi() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

class MarkdownRenderer {
public:
    explicit MarkdownRenderer(bool inline_only = false)
        : inline_only_(inline_only) {
    }

    void feed(const std::string &token) {
        if (token.empty()) return;
        end_thinking();
        answer_started_ = true;
        for (char c: token) handle(c);
        std::cout << std::flush;
    }

    void feed_reasoning(const std::string &token) {
        if (answer_started_ || token.empty()) return;
        if (!reasoning_active_) begin_thinking();
        ++think_tokens_;
        if (!think_live_) return;
        think_tail_add(token);
        if (think_tokens_ == 1 || think_tokens_ % thinking::redraw_every == 0)
            draw_think_line();
    }

    void finish() {
        end_thinking();
        if (in_table_) {
            if (!tbl_at_start_ && !table_line_.empty())
                table_lines_.push_back(table_line_);
            table_line_.clear();
            tbl_lead_.clear();
            end_table();
        } else if (in_fence_) {
            for (int i = 0; i < ticks_; ++i) std::cout << '`';
        } else if (at_line_start_ && !lead_.empty()) {
            release();
        }
        flush_holds();
        if (pending_star_) std::cout << '*';
        pending_star_ = false;
        std::cout << md_ansi::reset << std::flush;
    }

private:
    bool inline_only_ = false;
    // inline state
    bool bold_ = false, italic_ = false, code_ = false, pending_star_ = false;
    // line state
    bool at_line_start_ = true;
    std::string lead_;
    int heading_ = 0;
    int heading_len_ = 0;
    // fence state
    bool in_fence_ = false, fence_label_ = false, fence_line_start_ = false;
    int ticks_ = 0;
    // link state
    bool in_link_ = false;
    std::string link_raw_;
    std::string word_;
    char prev_ = 0;
    bool replaying_ = false;
    bool code_first_ = false, code_hold_ = false;
    std::string code_buf_;
    // table state
    bool in_table_ = false;
    bool tbl_at_start_ = false;
    std::string table_line_;
    std::string tbl_lead_;
    std::vector<std::string> table_lines_;
    // thinking line state
    bool reasoning_active_ = false;
    bool answer_started_ = false;
    bool think_live_ = false; // real terminal: carriage return redraw is used
    bool think_drawn_ = false;
    size_t think_width_ = 76;
    size_t think_last_len_ = 0; // visible width of the last drawn line
    long think_tokens_ = 0;
    std::chrono::steady_clock::time_point think_start_;
    std::string think_tail_; // recent reasoning, flattened to one line

    static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    void put(char c) {
        std::cout << c;
        prev_ = c;
        if (heading_ && (c & 0xC0) != 0x80) ++heading_len_;
    }

    void handle(char c) {
        if (inline_only_) {
            handle_inline(c);
            return;
        }
        if (in_fence_) {
            handle_fence(c);
            return;
        }
        if (in_table_) {
            handle_table(c);
            return;
        }
        if (at_line_start_) {
            handle_line_start(c);
            return;
        }
        if (c == '\n') {
            end_line();
            return;
        }
        handle_inline(c);
    }

    void end_line() {
        flush_holds();
        if (pending_star_) {
            std::cout << '*';
            pending_star_ = false;
        }
        bold_ = italic_ = code_ = false;
        std::cout << md_ansi::reset << '\n';
        if (heading_ == 1) {
            std::cout << md_ansi::h1;
            for (int i = 0; i < heading_len_; ++i) std::cout << "═";
            std::cout << md_ansi::reset << '\n';
        }
        heading_ = 0;
        heading_len_ = 0;
        prev_ = 0;
        at_line_start_ = true;
    }

    // ---------- line start: decide what kind of line this is ----------
    void handle_line_start(char c) {
        if (c == '\n') {
            bool all_dashes = lead_.size() >= 3 &&
                              lead_.find_first_not_of('-') == std::string::npos;
            if (all_dashes) {
                lead_.clear();
                std::cout << md_ansi::dim;
                for (int i = 0; i < 40; ++i) std::cout << "─";
                std::cout << md_ansi::reset << '\n';
                return;
            }
            release();
            end_line();
            return;
        }
        lead_ += c;
        classify();
    }

    void classify() {
        size_t i = 0;
        while (i < lead_.size() && lead_[i] == ' ') ++i;
        const std::string body = lead_.substr(i);
        if (body.empty()) return;
        const char f = body[0];
        const size_t npos = std::string::npos;

        if (f == '#') {
            size_t n = body.find_first_not_of('#');
            if (n == npos) {
                if (body.size() > 6) release();
                return;
            }
            if (body[n] == ' ' && n <= 6) start_heading((int) n);
            else release();
            return;
        }
        if (f == '-' || f == '+' || f == '*') {
            if (body.size() == 1) return;
            if (body[1] == ' ') {
                start_bullet(i);
                return;
            }
            if (f == '-' && body.find_first_not_of('-') == npos) return;
            release();
            return;
        }
        if (is_digit(f)) {
            size_t n = body.find_first_not_of("0123456789");
            if (n == npos) return;
            if (body[n] == '.') {
                if (n + 1 == body.size()) return;
                if (body[n + 1] == ' ') {
                    start_ordered(i, body.substr(0, n + 1));
                    return;
                }
            }
            release();
            return;
        }
        if (f == '>') {
            if (body.size() == 1) return;
            if (body[1] == ' ') start_quote();
            else release();
            return;
        }
        if (f == '`') {
            size_t n = body.find_first_not_of('`');
            if (n == npos) {
                if (body.size() >= 3) start_fence();
                return;
            }
            release();
            return;
        }
        if (f == '|') {
            start_table();
            return;
        }
        release();
    }

    void release() {
        std::string held = lead_;
        lead_.clear();
        at_line_start_ = false;
        for (char ch: held) handle_inline(ch);
    }

    void start_heading(int level) {
        lead_.clear();
        at_line_start_ = false;
        heading_ = level;
        std::cout << (level == 1
                          ? md_ansi::h1
                          : level == 2
                                ? md_ansi::h2
                                : level == 3
                                      ? md_ansi::h3
                                      : md_ansi::h4);
    }

    void start_bullet(size_t indent) {
        lead_.clear();
        at_line_start_ = false;
        std::cout << std::string(indent, ' ') << md_ansi::dim
                << (indent >= 2 ? "◦" : "•") << md_ansi::fg_off << ' ';
    }

    void start_ordered(size_t indent, const std::string &number) {
        lead_.clear();
        at_line_start_ = false;
        std::cout << std::string(indent, ' ') << md_ansi::dim << number
                << md_ansi::fg_off << ' ';
    }

    void start_quote() {
        lead_.clear();
        at_line_start_ = false;
        std::cout << md_ansi::dim << "┃" << md_ansi::quote_text << ' ';
    }

    void start_fence() {
        lead_.clear();
        at_line_start_ = false;
        in_fence_ = true;
        fence_label_ = true;
        ticks_ = 0;
        std::cout << md_ansi::dim;
    }

    // ---------- inside a code fence: print literally ----------
    void handle_fence(char c) {
        if (fence_label_) {
            if (c == '\n') {
                fence_label_ = false;
                fence_line_start_ = true;
                std::cout << md_ansi::reset << '\n' << md_ansi::fence_bg;
            } else {
                std::cout << c;
            }
            return;
        }
        if (fence_line_start_) {
            if (c == '`') {
                if (++ticks_ == 3) {
                    in_fence_ = false;
                    ticks_ = 0;
                    std::cout << md_ansi::reset;
                }
                return;
            }
            for (int i = 0; i < ticks_; ++i) std::cout << '`';
            ticks_ = 0;
            fence_line_start_ = false;
        }
        if (c == '\n') {
            std::cout << "\033[K" << md_ansi::reset << '\n' << md_ansi::fence_bg;
            fence_line_start_ = true;
            return;
        }
        std::cout << c;
    }

    // ---------- tables: collect lines, then print the grid once ----------
    void start_table() {
        table_line_ = lead_; // leading spaces plus the first '|'
        lead_.clear();
        table_lines_.clear();
        tbl_lead_.clear();
        at_line_start_ = false;
        in_table_ = true;
        tbl_at_start_ = false;
    }

    void handle_table(char c) {
        if (!tbl_at_start_) {
            if (c == '\n') {
                table_lines_.push_back(table_line_);
                table_line_.clear();
                tbl_at_start_ = true;
            } else {
                table_line_ += c;
            }
            return;
        }
        // at the start of a new line inside the table
        if (c == ' ' || c == '\t') {
            tbl_lead_ += c;
            return;
        }
        if (c == '|') {
            table_line_ = tbl_lead_ + c;
            tbl_lead_.clear();
            tbl_at_start_ = false;
            return;
        }
        // the table ended: this character begins an ordinary line
        std::string carry = tbl_lead_ + c;
        tbl_lead_.clear();
        end_table();
        for (char ch: carry) handle(ch);
    }

    void end_table() {
        in_table_ = false;
        tbl_at_start_ = false;
        bool ok = table_lines_.size() >= 2 &&
                  is_separator_row(split_row(table_lines_[1]));
        std::vector<std::string> lines = table_lines_;
        table_lines_.clear();
        if (ok) {
            render_table(lines);
        } else {
            // not a table after all: print the lines as ordinary text
            for (const std::string &l: lines) {
                at_line_start_ = false;
                replay(l);
                end_line();
            }
        }
        at_line_start_ = true;
        lead_.clear();
        prev_ = 0;
    }

    static std::string trim(const std::string &s) {
        size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r");
        return s.substr(a, b - a + 1);
    }

    static std::vector<std::string> split_row(const std::string &line) {
        std::string s = trim(line);
        if (!s.empty() && s.front() == '|') s.erase(0, 1);
        if (!s.empty() && s.back() == '|' &&
            !(s.size() >= 2 && s[s.size() - 2] == '\\'))
            s.pop_back();
        std::vector<std::string> cells;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == '|') {
                cur += '|';
                ++i;
            } else if (s[i] == '|') {
                cells.push_back(trim(cur));
                cur.clear();
            } else {
                cur += s[i];
            }
        }
        cells.push_back(trim(cur));
        return cells;
    }

    static bool is_separator_row(const std::vector<std::string> &cells) {
        if (cells.empty()) return false;
        for (const std::string &c: cells) {
            if (c.empty()) return false;
            if (c.find_first_not_of("-:") != std::string::npos) return false;
            if (c.find('-') == std::string::npos) return false;
        }
        return true;
    }

    static char cell_align(const std::string &c) {
        bool l = c.front() == ':', r = c.back() == ':';
        return (l && r) ? 'c' : r ? 'r' : 'l';
    }

    // how many columns a string takes on screen (escape codes excluded)
    static size_t visible_width(const std::string &s) {
        size_t w = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            unsigned char c = (unsigned char) s[i];
            if (c == 0x1B && i + 1 < s.size()) {
                if (s[i + 1] == '[') {
                    i += 2;
                    while (i < s.size() &&
                           !((unsigned char) s[i] >= 0x40 && (unsigned char) s[i] <= 0x7E))
                        ++i;
                } else if (s[i + 1] == ']') {
                    i += 2;
                    while (i < s.size() && s[i] != '\a' &&
                           !(s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\'))
                        ++i;
                    if (i < s.size() && s[i] == '\033') ++i;
                }
                continue;
            }
            if ((c & 0xC0) != 0x80) ++w;
        }
        return w;
    }

    // end of the item at `i`: one escape sequence or one UTF-8 character
    static size_t item_end(const std::string &s, size_t i, bool &is_escape) {
        is_escape = false;
        if ((unsigned char) s[i] == 0x1B && i + 1 < s.size()) {
            is_escape = true;
            size_t j = i + 2;
            if (s[i + 1] == '[') {
                while (j < s.size() &&
                       !((unsigned char) s[j] >= 0x40 && (unsigned char) s[j] <= 0x7E))
                    ++j;
                return j < s.size() ? j + 1 : j;
            }
            if (s[i + 1] == ']') {
                while (j < s.size() && s[j] != '\a' &&
                       !(s[j] == '\033' && j + 1 < s.size() && s[j + 1] == '\\'))
                    ++j;
                if (j < s.size() && s[j] == '\033') j += 2;
                else if (j < s.size()) j += 1;
                return j;
            }
            return i + 2;
        }
        size_t j = i + 1;
        while (j < s.size() && ((unsigned char) s[j] & 0xC0) == 0x80) ++j;
        return j;
    }

    // split a styled string into lines of at most `width` visible columns,
    // closing the style at each break and reopening it on the next line
    static std::vector<std::string> wrap_styled(const std::string &s, size_t width) {
        std::vector<std::string> out;
        std::string line, active;
        size_t line_w = 0;

        auto break_line = [&]() {
            out.push_back(line + md_ansi::reset);
            line = active;
            line_w = 0;
        };

        size_t i = 0;
        bool esc = false;
        while (i < s.size()) {
            if (s[i] == ' ') {
                ++i;
                continue;
            }
            size_t j = i, chunk_w = 0;
            while (j < s.size() && s[j] != ' ') {
                size_t e = item_end(s, j, esc);
                if (!esc) ++chunk_w;
                j = e;
            }
            if (line_w > 0 && line_w + 1 + chunk_w > width) break_line();
            else if (line_w > 0) {
                line += ' ';
                ++line_w;
            }
            for (size_t k = i; k < j;) {
                size_t e = item_end(s, k, esc);
                const std::string item = s.substr(k, e - k);
                if (esc) {
                    line += item;
                    if (item == md_ansi::reset) active.clear();
                    else if (item.size() > 2 && item[1] == '[' && item.back() == 'm')
                        active += item;
                } else {
                    if (line_w >= width) break_line();
                    line += item;
                    ++line_w;
                }
                k = e;
            }
            i = j;
        }
        out.push_back(line + md_ansi::reset);
        return out;
    }

    // run a cell's text through a second renderer, capturing its output
    static std::string render_cell(const std::string &text) {
        std::ostringstream oss;
        std::streambuf *old = std::cout.rdbuf(oss.rdbuf());
        {
            MarkdownRenderer cell(true);
            cell.feed(text);
            cell.finish();
        }
        std::cout.rdbuf(old);
        return oss.str();
    }

    void render_table(const std::vector<std::string> &lines) {
        std::vector<std::vector<std::string> > rows;
        std::vector<char> align;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::vector<std::string> cells = split_row(lines[i]);
            if (i == 1) {
                for (const std::string &cell: cells) align.push_back(cell_align(cell));
                continue;
            }
            rows.push_back(cells);
        }
        size_t ncols = align.size();
        for (const auto &r: rows) ncols = std::max(ncols, r.size());
        align.resize(ncols, 'l');

        std::vector<std::vector<std::string> > styled(
            rows.size(), std::vector<std::string>(ncols));
        std::vector<size_t> colw(ncols, 1);
        for (size_t r = 0; r < rows.size(); ++r) {
            for (size_t c = 0; c < ncols; ++c) {
                std::string text = c < rows[r].size() ? rows[r][c] : "";
                styled[r][c] = render_cell(text);
                colw[c] = std::max(colw[c], visible_width(styled[r][c]));
            }
        }

        if (stdout_is_tty()) {
            const size_t overhead = 3 * ncols + 1;
            const size_t term = (size_t) std::max(terminal_width() - 1, 20);
            const size_t floor_w = 8;
            while (true) {
                size_t total = overhead;
                for (size_t w: colw) total += w;
                if (total <= term) break;
                size_t widest = 0;
                for (size_t c = 1; c < ncols; ++c)
                    if (colw[c] > colw[widest]) widest = c;
                if (colw[widest] <= floor_w) break;
                --colw[widest];
            }
        }

        std::vector<std::vector<std::vector<std::string> > > cell_lines(rows.size());
        std::vector<size_t> height(rows.size(), 1);
        for (size_t r = 0; r < rows.size(); ++r) {
            cell_lines[r].resize(ncols);
            for (size_t c = 0; c < ncols; ++c) {
                cell_lines[r][c] = wrap_styled(styled[r][c], colw[c]);
                height[r] = std::max(height[r], cell_lines[r][c].size());
            }
        }

        auto border = [&](const char *left, const char *mid, const char *right) {
            std::cout << md_ansi::table << left;
            for (size_t c = 0; c < ncols; ++c) {
                for (size_t k = 0; k < colw[c] + 2; ++k) std::cout << "─";
                std::cout << (c + 1 < ncols ? mid : right);
            }
            std::cout << md_ansi::reset << '\n';
        };
        auto row_out = [&](size_t r, bool header) {
            const std::string blank;
            for (size_t k = 0; k < height[r]; ++k) {
                std::cout << md_ansi::table << "│" << md_ansi::reset;
                for (size_t c = 0; c < ncols; ++c) {
                    const std::string &text =
                            k < cell_lines[r][c].size() ? cell_lines[r][c][k] : blank;
                    size_t w = visible_width(text);
                    size_t pad = colw[c] > w ? colw[c] - w : 0;
                    size_t left = align[c] == 'r' ? pad : align[c] == 'c' ? pad / 2 : 0;
                    size_t right = pad - left;
                    std::cout << ' ' << std::string(left, ' ');
                    if (header) std::cout << md_ansi::bold_on;
                    std::cout << text << md_ansi::reset;
                    std::cout << std::string(right, ' ') << ' ';
                    std::cout << md_ansi::table << "│" << md_ansi::reset;
                }
                std::cout << '\n';
            }
        };

        border("┌", "┬", "┐");
        row_out(0, true);
        if (rows.size() > 1) {
            border("├", "┼", "┤");
            for (size_t r = 1; r < rows.size(); ++r) row_out(r, false);
        }
        border("└", "┴", "┘");
    }

    // ---------- thinking line ----------
    static bool stdout_is_tty() {
#ifdef _WIN32
        return _isatty(_fileno(stdout)) != 0;
#else
        return isatty(STDOUT_FILENO) != 0;
#endif
    }

    static int terminal_width() {
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

    static size_t ulen(const std::string &s) {
        size_t n = 0;
        for (unsigned char ch: s) if ((ch & 0xC0) != 0x80) ++n;
        return n;
    }

    void begin_thinking() {
        reasoning_active_ = true;
        think_start_ = std::chrono::steady_clock::now();
        think_tokens_ = 0;
        think_live_ = stdout_is_tty();
        think_width_ = (size_t) std::max(20, terminal_width() - 4);
        if (!think_live_)
            std::cout << md_ansi::dim << thinking::label << "..." << md_ansi::reset << '\n';
    }

    void end_thinking() {
        if (!reasoning_active_) return;
        reasoning_active_ = false;
        double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - think_start_).count();
        long s = (long) (secs + 0.5);
        std::string how = s <= 0
                              ? "less than a second"
                              : std::to_string(s) + (s == 1 ? " second" : " seconds");
        std::string line = std::string(thinking::done_label) + ' ' + how;
        if (think_live_ && think_drawn_) {
            std::cout << '\r' << md_ansi::dim << line << md_ansi::reset;
            size_t n = ulen(line);
            if (n < think_last_len_)
                std::cout << std::string(think_last_len_ - n, ' ');
            std::cout << '\n';
        } else {
            std::cout << md_ansi::dim << line << md_ansi::reset << '\n';
        }
        std::cout << std::flush;
        think_tail_.clear();
        think_last_len_ = 0;
        think_drawn_ = false;
    }

    void think_tail_add(const std::string &token) {
        for (char c: token) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            think_tail_ += c;
        }
        while (ulen(think_tail_) > thinking::tail_chars) {
            size_t i = 1;
            while (i < think_tail_.size() &&
                   ((unsigned char) think_tail_[i] & 0xC0) == 0x80)
                ++i;
            think_tail_.erase(0, i);
        }
    }

    void draw_think_line() {
        const auto &f = thinking::frames;
        const std::string &frame =
                f[(size_t) (think_tokens_ / thinking::tokens_per_frame) % f.size()];
        std::string line = frame + ' ' + thinking::label;
        size_t head_len = ulen(line);
        size_t usable = think_width_;
        size_t room = usable > head_len ? usable - head_len : 0;
        size_t avail = std::min(room, thinking::tail_chars);
        if (!think_tail_.empty() && avail > 0) {
            std::string tail = think_tail_;
            while (ulen(tail) > avail) {
                size_t i = 1;
                while (i < tail.size() &&
                       ((unsigned char) tail[i] & 0xC0) == 0x80)
                    ++i;
                tail.erase(0, i);
            }
            line += "  " + tail;
        }
        std::cout << '\r' << md_ansi::dim << line << md_ansi::reset;
        if (think_drawn_ && ulen(line) < think_last_len_)
            std::cout << std::string(think_last_len_ - ulen(line), ' ');
        std::cout << std::flush;
        think_last_len_ = ulen(line);
        think_drawn_ = true;
    }

    // ---------- inline styling ----------
    void toggle_bold() {
        bold_ = !bold_;
        std::cout << (bold_ ? md_ansi::bold_on : md_ansi::bold_off);
    }

    void toggle_italic() {
        italic_ = !italic_;
        std::cout << (italic_ ? md_ansi::italic_on : md_ansi::italic_off);
    }

    void handle_inline(char c) {
        if (code_) {
            handle_code(c);
            return;
        }
        if (in_link_) {
            handle_link(c);
            return;
        }
        if (!word_.empty()) {
            handle_word(c);
            return;
        }

        if (pending_star_) {
            pending_star_ = false;
            if (c == '*') {
                toggle_bold();
                return;
            }
            bool opens_italic = !italic_ && c != ' ' && c != '\n';
            if (italic_ || opens_italic) toggle_italic();
            else put('*');
        }
        if (c == '*') pending_star_ = true;
        else if (c == '`') {
            code_ = true;
            code_first_ = true;
            std::cout << md_ansi::code_on;
        } else if (!replaying_ && c == '[') {
            in_link_ = true;
            link_raw_ = "[";
        } else if (!replaying_ && starts_word(c)) { word_ = std::string(1, c); } else put(c);
    }

    // ---------- inline code (may hold a path until the closing backtick) ----------
    void handle_code(char c) {
        if (code_first_) {
            code_first_ = false;
            code_hold_ = (c == '/' || c == '~' || c == '.' || c == 'h');
        }
        if (c == '`') {
            if (code_hold_) finish_code_hold();
            code_ = false;
            std::cout << md_ansi::code_off;
            return;
        }
        if (code_hold_) {
            code_buf_ += c;
            return;
        }
        put(c);
    }

    void finish_code_hold() {
        std::string target = link_target(code_buf_);
        if (target.empty()) { for (char ch: code_buf_) put(ch); } else emit_link(target, code_buf_);
        code_buf_.clear();
        code_hold_ = false;
    }

    // ---------- markdown links: [text](url) ----------
    void handle_link(char c) {
        link_raw_ += c;
        size_t close = link_raw_.find(']');
        if (link_raw_.size() > 400 ||
            (close != std::string::npos && link_raw_.size() > close + 1 &&
             link_raw_[close + 1] != '(')) {
            release_link();
            return;
        }
        if (c == ')' && close != std::string::npos) {
            std::string text = link_raw_.substr(1, close - 1);
            std::string url = link_raw_.substr(close + 2, link_raw_.size() - close - 3);
            std::string target = link_target(url);
            if (target.empty()) {
                release_link();
                return;
            }
            in_link_ = false;
            link_raw_.clear();
            emit_link(target, text.empty() ? url : text);
        }
    }

    void release_link() {
        std::string raw = link_raw_;
        link_raw_.clear();
        in_link_ = false;
        replay(raw);
    }

    // ---------- bare URLs and paths ----------
    bool starts_word(char c) const {
        bool boundary = prev_ == 0 || prev_ == ' ' || prev_ == '(' ||
                        prev_ == '"' || prev_ == '\'';
        return boundary && (c == 'h' || c == '/' || c == '~' || c == '.');
    }

    static bool is_prefix(const std::string &full, const std::string &w) {
        return w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
    }

    bool word_could_match() const {
        const std::string &w = word_;
        switch (w[0]) {
            case '/': return !(w.size() >= 2 && w[1] == '/');
            case '~':
            case '.': return w.size() == 1 || w[1] == '/';
            case 'h':
                if (w.size() < 8) return is_prefix("https://", w) || is_prefix("http://", w);
                return w.compare(0, 7, "http://") == 0 || w.compare(0, 8, "https://") == 0;
        }
        return false;
    }

    void handle_word(char c) {
        if (c == ' ' || c == '\t') {
            resolve_word();
            handle_inline(c);
            return;
        }
        word_ += c;
        if (!word_could_match()) release_word();
    }

    void release_word() {
        std::string w = word_;
        word_.clear();
        replay(w);
    }

    void resolve_word() {
        static const std::string punct = ".,;:!?)\"'`*";
        std::string w = word_;
        word_.clear();
        size_t n = w.size();
        while (n > 0 && punct.find(w[n - 1]) != std::string::npos) --n;
        std::string core = w.substr(0, n);
        std::string tail = w.substr(n);
        std::string target = link_target(core);
        if (target.empty()) {
            replay(w);
            return;
        }
        emit_link(target, core);
        replay(tail);
    }

    // returns a clickable URL for a web address or an existing file, else ""
    std::string link_target(const std::string &s) const {
        namespace fs = std::filesystem;
        if (s.rfind("https://", 0) == 0) return s.size() > 8 ? s : "";
        if (s.rfind("http://", 0) == 0) return s.size() > 7 ? s : "";
        if (s.size() < 2) return "";

        fs::path p;
        std::error_code ec;
        if (s[0] == '/') {
            p = s;
        } else if (s.compare(0, 2, "~/") == 0) {
#ifdef _WIN32
            const char *home = std::getenv("USERPROFILE");
#else
            const char *home = std::getenv("HOME");
#endif
            if (!home) return "";
            p = fs::path(home) / s.substr(2);
        } else if (s.compare(0, 2, "./") == 0) {
            p = fs::current_path(ec) / s.substr(2);
        } else {
            return "";
        }
        if (!fs::exists(p, ec)) return "";
        return "file://" + uri_encode(p.lexically_normal().string());
    }

    static std::string uri_encode(const std::string &s) {
        static const char *hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char ch: s) {
            if (std::isalnum(ch) || ch == '/' || ch == '.' || ch == '_' ||
                ch == '~' || ch == '-') {
                out += (char) ch;
            } else {
                out += '%';
                out += hex[ch >> 4];
                out += hex[ch & 15];
            }
        }
        return out;
    }

    void emit_link(const std::string &url, const std::string &text) {
        std::cout << "\033]8;;" << url << "\033\\" << md_ansi::link_on;
        for (char ch: text) put(ch);
        std::cout << md_ansi::link_off << "\033]8;;\033\\";
    }

    // print held text as ordinary text, without starting new links or words
    void replay(const std::string &s) {
        replaying_ = true;
        for (char ch: s) handle_inline(ch);
        replaying_ = false;
    }

    void flush_holds() {
        if (code_hold_) {
            std::cout << code_buf_;
            code_buf_.clear();
            code_hold_ = false;
        }
        code_first_ = false;
        if (in_link_) release_link();
        if (!word_.empty()) resolve_word();
    }
};