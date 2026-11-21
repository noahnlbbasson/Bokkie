//
// Created by Noah Luke Basson on 2026/11/20.
//

#ifndef BOKKIE_WORKSPACE_H
#define BOKKIE_WORKSPACE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Workspace {
public:
    struct ChatInfo {
        std::string workspace;
        std::string name;
        std::int64_t updated_at = 0;
        std::size_t message_count = 0;
    };

    struct CreateResult {
        bool ok = false;
        bool existed = false;
        std::string name;
        std::string error;
    };

    explicit Workspace(std::filesystem::path workspaces_root = {});
    std::filesystem::path root() const;

    // Workspaces: Bokkie's own folder per project, never the project itself.
    CreateResult createWorkspaceForProject(const std::filesystem::path& project_path);
    bool deleteWorkspace(const std::string& name);
    std::vector<std::string> getWorkspaces() const;
    std::filesystem::path projectPath(const std::string& name) const;

    // Chats
    bool createChat(const std::string& workspace_name, const std::string& chat_name);
    bool deleteChat(const std::string& workspace_name, const std::string& chat_name);
    bool updateChat(const std::string& workspace_name, const std::string& chat_name,
                    const nlohmann::json& messages);
    bool updateChat(const std::string& workspace_name, const std::string& chat_name,
                    const std::vector<nlohmann::json>& messages);
    nlohmann::json loadChat(const std::string& workspace_name,
                            const std::string& chat_name) const;
    std::vector<ChatInfo> listChats(const std::string& workspace_name) const;
    std::vector<ChatInfo> recentChats(std::size_t limit) const;

    static std::filesystem::path parseUserPath(const std::string& text);

private:
    struct ChatFile {
        std::string workspace;
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };

    std::filesystem::path workspaces_root_;

    std::filesystem::path workspacePath(const std::string& name) const;
    std::filesystem::path chatPath(const std::string& workspace_name,
                                   const std::string& chat_name) const;
    bool workspaceExists(const std::string& name) const;
    std::vector<ChatFile> chatFiles(const std::string& workspace_name) const;
    static ChatInfo readChatInfo(const ChatFile& file);
    static std::string validateName(const std::string& name);
    static std::string chatFileName(const std::string& chat_name);
    bool moveToTrash(const std::filesystem::path& source, const std::string& kind) const;
};

#endif //BOKKIE_WORKSPACE_H
