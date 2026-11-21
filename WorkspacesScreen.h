#ifndef BOKKIE_WORKSPACESSCREEN_H
#define BOKKIE_WORKSPACESSCREEN_H

#include <string>
#include <vector>

#include "Terminal.h"
#include "workspaces/Workspace.h"

struct WorkspacesResult {
    enum class Action { Back, OpenChat };
    Action action = Action::Back;
    std::string workspace;
    std::string chat;
    std::string notice;
};

class WorkspacesScreen {
public:
    explicit WorkspacesScreen(Workspace& workspaces);

    WorkspacesResult run();

private:
    enum class Mode { Workspaces, Chats, NewWorkspace, ConfirmWorkspace, ConfirmChat };

    struct Entry {
        std::string name;
        std::string project;
        bool projectMissing = false;
    };

    Workspace& workspaces_;
    Mode mode_ = Mode::Workspaces;
    std::vector<Entry> entries_;
    std::vector<Workspace::ChatInfo> chats_;
    std::string current_;
    std::string currentProject_;
    bool currentMissing_ = false;
    int selected_ = 0;
    int chatSelected_ = 0;
    std::string input_;
    std::string notice_;
    const char* noticeColor_ = term::color::WARNING;
    std::string pendingName_;

    void refreshWorkspaces();
    void refreshChats();
    void openWorkspace(const std::string& name);
    void submitNewWorkspace();
    std::string newChatName();
    void setNotice(const std::string& text, const char* noticeColor);
    void clearNotice();

    bool handleKey(const term::KeyEvent& key, WorkspacesResult& result);
    void render(bool full);
};

#endif //BOKKIE_WORKSPACESSCREEN_H
