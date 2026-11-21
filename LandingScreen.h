#ifndef BOKKIE_LANDINGSCREEN_H
#define BOKKIE_LANDINGSCREEN_H

#include <cstdint>
#include <string>
#include <vector>

#include "workspaces/Workspace.h"

struct LandingChoice {
    enum class Action { Quit, QuickChat, OpenChat, Workspaces, Settings };
    Action action = Action::Quit;
    std::string workspace;
    std::string chat;
};

class LandingScreen {
public:
    explicit LandingScreen(Workspace& workspaces);

    LandingChoice displayLandingScreen();
    void setNotice(const std::string& notice);

private:
    enum class Key { Up, Down, Left, Right, Enter, Quit };
    enum class Focus { Menu, Recent };

    struct Status {
        bool serverUp = false;
        std::string model;
        std::size_t workspaceCount = 0;
    };

    Workspace& workspaces_;
    Status status_;
    std::vector<Workspace::ChatInfo> recents_;
    Focus focus_ = Focus::Menu;
    int selected_ = 0;
    int recentSelected_ = 0;
    std::string notice_;
    bool interactive_ = false;

    void checkStatus();
    void displayBrand();
    void displayStatus();
    void displayMenu();
    void displayFooter();
    void render(bool full);

    static int terminalWidth();
    static int contentWidth();
    static size_t visibleLength(const std::string& text);
    static size_t displayWidth(const std::string& text);
    static std::string fitTail(const std::string& text, size_t maxLen);
    static std::string truncate(const std::string& text, size_t maxLen);
    static std::string centered(const std::string& text, int width);
    static std::string formatAge(std::int64_t epochSeconds);
    static std::string recentLine(const Workspace::ChatInfo& chat, bool selected, size_t width);
    static void printRule(int width);
    static void printRow(const std::string& label, const std::string& value,
                         const char* valueColor, int width);
    static bool inputReady(int timeoutMs);
    static void discardLine();
    static std::vector<Key> readKeys();
};

#endif //BOKKIE_LANDINGSCREEN_H
