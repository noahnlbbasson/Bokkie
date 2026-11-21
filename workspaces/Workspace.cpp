//
// Created by Noah Luke Basson on 2026/11/20.
//

#include "Workspace.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kWorkspaceFile = ".workspace.json";

std::int64_t current_time_seconds() {
    return static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::string compact_trash_name(const fs::path& source) {
    std::ostringstream name;
    name << current_time_seconds() << "_" << source.filename().string();
    return name.str();
}

fs::path default_workspaces_root() {
    if (const char* custom = std::getenv("BOKKIE_HOME")) {
        return fs::path(custom) / "workspaces";
    }
#ifdef _WIN32
    if (const char* base = std::getenv("APPDATA"))
        return fs::path(base) / "Bokkie" / "workspaces";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "Library" / "Application Support" / "Bokkie" / "workspaces";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "bokkie" / "workspaces";
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / ".local" / "share" / "bokkie" / "workspaces";
#endif
    return fs::current_path() / "workspaces";
}

std::string sanitize_name(const std::string& raw, std::size_t max_len) {
    std::string out;
    for (unsigned char c : raw) {
        const bool bad = c < 32 ||
            std::string("<>:\"|?*/\\").find(static_cast<char>(c)) != std::string::npos;
        out.push_back(bad ? '-' : static_cast<char>(c));
    }
    const std::size_t start = out.find_first_not_of(". ");
    if (start == std::string::npos) return {};
    out.erase(0, start);
    if (out.size() > max_len) {
        std::size_t cut = max_len;
        while (cut > 0 && cut < out.size() &&
               (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out;
}

bool write_json_atomic(const fs::path& path, const json& data) {
    const fs::path temporary(path.string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        const std::string serialized = data.dump(2) + "\n";
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) return false;
    }
    std::error_code error;
    fs::rename(temporary, path, error);
#ifdef _WIN32
    if (error) {
        fs::remove(path, error);
        if (error) return false;
        fs::rename(temporary, path, error);
    }
#endif
    return !error;
}
}

Workspace::Workspace(fs::path workspaces_root)
    : workspaces_root_(workspaces_root.empty()
                           ? default_workspaces_root()
                           : std::move(workspaces_root)) {
    workspaces_root_ = fs::absolute(workspaces_root_);
}

fs::path Workspace::root() const {
    return workspaces_root_;
}

std::string Workspace::validateName(const std::string& name) {
    if (name.empty()) return "Name cannot be empty";
    if (name.front() == '.') return "Name cannot start with a dot";
    if (name.back() == ' ' || name.back() == '.') return "Name cannot end with a space or a dot";
    if (name.find_first_of("<>:\"|?*") != std::string::npos)
        return "Name cannot contain < > : \" | ? *";
    if (name.size() > 100) return "Name cannot be longer than 100 characters";
    if (name == "." || name == "..") return "Name cannot be '.' or '..'";
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        return "Name cannot contain a path separator";
    }
    for (unsigned char character : name) {
        if (character < 32) return "Name cannot contain control characters";
    }
    return {};
}

std::string Workspace::chatFileName(const std::string& chat_name) {
    std::string file_name;
    file_name.reserve(chat_name.size());
    for (unsigned char character : chat_name) {
        if (std::isalnum(character) || character == '-' || character == '_' ||
            character == '.') {
            file_name.push_back(static_cast<char>(std::tolower(character)));
        } else if (character == ' ') {
            if (!file_name.empty() && file_name.back() != '-') file_name.push_back('-');
        }
    }
    if (file_name.empty() || file_name == "." || file_name == "..") file_name = "chat";
    while (file_name.size() > 1 && file_name.back() == '-') file_name.pop_back();
    if (file_name.size() > 80) file_name.resize(80);
    return file_name + ".json";
}

fs::path Workspace::workspacePath(const std::string& name) const {
    const std::string error = validateName(name);
    if (!error.empty()) throw std::invalid_argument(error);
    return workspaces_root_ / name;
}

fs::path Workspace::chatPath(const std::string& workspace_name,
                             const std::string& chat_name) const {
    const std::string error = validateName(chat_name);
    if (!error.empty()) throw std::invalid_argument(error);
    return workspacePath(workspace_name) / chatFileName(chat_name);
}

bool Workspace::workspaceExists(const std::string& name) const {
    std::error_code error;
    return fs::exists(workspaces_root_ / name, error);
}

bool Workspace::moveToTrash(const fs::path& source, const std::string& kind) const {
    std::error_code error;
    const fs::path trash = workspaces_root_ / ".trash" / kind;
    fs::create_directories(trash, error);
    if (error) return false;

    fs::path destination = trash / compact_trash_name(source);
    int suffix = 1;
    while (fs::exists(destination, error)) {
        destination = trash /
            (compact_trash_name(source) + "_" + std::to_string(suffix++));
    }

    fs::rename(source, destination, error);
    if (error) {
        fs::copy(source, destination,
                 fs::copy_options::recursive | fs::copy_options::copy_symlinks,
                 error);
        if (!error) fs::remove_all(source, error);
    }
    return !error && fs::exists(destination);
}

// Moves only Bokkie's own workspace folder (record + chats) to the trash.
// Never touches the project path.
bool Workspace::deleteWorkspace(const std::string& name) {
    try {
        const fs::path path = workspacePath(name);
        std::error_code error;
        if (!fs::is_regular_file(path / kWorkspaceFile, error) || error) return false;
        return moveToTrash(path, "workspaces");
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<std::string> Workspace::getWorkspaces() const {
    std::vector<std::pair<std::string, fs::file_time_type>> workspaces;
    std::error_code error;
    if (!fs::is_directory(workspaces_root_, error) || error) return {};

    for (const fs::directory_entry& entry :
         fs::directory_iterator(workspaces_root_, error)) {
        if (error) break;
        if (!entry.is_directory(error) || error) continue;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.') continue;
        std::error_code record_error;
        if (!fs::is_regular_file(entry.path() / kWorkspaceFile, record_error)) continue;

        fs::file_time_type modified = fs::last_write_time(entry.path() / kWorkspaceFile, error);
        if (error) modified = fs::file_time_type::min();
        workspaces.emplace_back(name, modified);
    }

    std::sort(workspaces.begin(), workspaces.end(),
              [](const auto& left, const auto& right) {
                  return left.second != right.second
                             ? left.second > right.second
                             : left.first < right.first;
              });

    std::vector<std::string> names;
    names.reserve(workspaces.size());
    for (const auto& [name, modified] : workspaces) names.push_back(name);
    return names;
}

bool Workspace::createChat(const std::string& workspace_name,
                           const std::string& chat_name) {
    try {
        const fs::path workspace = workspacePath(workspace_name);
        const fs::path chat = chatPath(workspace_name, chat_name);
        std::error_code error;
        if (!fs::is_regular_file(workspace / kWorkspaceFile, error) || error) return false;
        if (fs::exists(chat, error)) return false;

        json data = {
            {"version", 1},
            {"workspace", workspace_name},
            {"chat", chat_name},
            {"created_at", current_time_seconds()},
            {"updated_at", current_time_seconds()},
            {"messages", json::array()}
        };
        return write_json_atomic(chat, data);
    } catch (const std::exception&) {
        return false;
    }
}

bool Workspace::deleteChat(const std::string& workspace_name,
                           const std::string& chat_name) {
    try {
        const fs::path chat = chatPath(workspace_name, chat_name);
        std::error_code error;
        if (!fs::is_regular_file(chat, error) || error) return false;
        return moveToTrash(chat, "chats");
    } catch (const std::exception&) {
        return false;
    }
}

bool Workspace::updateChat(const std::string& workspace_name,
                           const std::string& chat_name,
                           const json& messages) {
    try {
        const fs::path chat = chatPath(workspace_name, chat_name);
        std::error_code error;
        if (!fs::is_regular_file(chat, error) || error) return false;

        json data;
        {
            std::ifstream input(chat, std::ios::binary);
            if (!input) return false;
            try {
                input >> data;
            } catch (const json::exception&) {
                return false;
            }
        }
        if (!data.is_object()) return false;

        json message_array;
        if (messages.is_array()) {
            message_array = messages;
        } else if (messages.is_object() && messages.contains("messages") &&
                   messages["messages"].is_array()) {
            message_array = messages["messages"];
        } else {
            return false;
        }

        data["version"] = 1;
        data["workspace"] = workspace_name;
        data["chat"] = chat_name;
        data["updated_at"] = current_time_seconds();
        data["messages"] = std::move(message_array);
        return write_json_atomic(chat, data);
    } catch (const std::exception&) {
        return false;
    }
}

bool Workspace::updateChat(const std::string& workspace_name,
                           const std::string& chat_name,
                           const std::vector<json>& messages) {
    return updateChat(workspace_name, chat_name, json(messages));
}

json Workspace::loadChat(const std::string& workspace_name,
                         const std::string& chat_name) const {
    try {
        const fs::path chat = chatPath(workspace_name, chat_name);
        std::error_code error;
        if (!fs::is_regular_file(chat, error) || error) return {};
        std::ifstream input(chat, std::ios::binary);
        if (!input) return {};
        json data;
        try {
            input >> data;
        } catch (const json::exception&) {
            return {};
        }
        if (!data.is_object() || !data.contains("messages") ||
            !data["messages"].is_array()) return {};
        return data;
    } catch (const std::exception&) {
        return {};
    }
}

fs::path Workspace::parseUserPath(const std::string& text) {
    std::string s = text;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    s.erase(0, start);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
#ifndef _WIN32
    std::string unescaped;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) ++i;
        unescaped.push_back(s[i]);
    }
    s = unescaped;
#endif
    if (!s.empty() && s[0] == '~' && (s.size() == 1 || s[1] == '/' || s[1] == '\\')) {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home) s = std::string(home) + s.substr(1);
    }
    return fs::path(s);
}

fs::path Workspace::projectPath(const std::string& name) const {
    try {
        std::ifstream input(workspacePath(name) / kWorkspaceFile, std::ios::binary);
        if (!input) return {};
        const json data = json::parse(input, nullptr, false);
        if (data.is_discarded() || !data.is_object() ||
            !data.contains("project_path") || !data["project_path"].is_string()) return {};
        return fs::path(data["project_path"].get<std::string>());
    } catch (const std::exception&) {
        return {};
    }
}

Workspace::CreateResult Workspace::createWorkspaceForProject(const fs::path& project_path) {
    CreateResult result;
    std::error_code error;

    if (project_path.empty()) {
        result.error = "No path given";
        return result;
    }
    if (!fs::is_directory(project_path, error) || error) {
        result.error = "That folder does not exist";
        return result;
    }
    const fs::path project = fs::canonical(project_path, error);
    if (error) {
        result.error = "Could not read that folder";
        return result;
    }

    const fs::path data_root = fs::weakly_canonical(workspaces_root_, error);
    if (!error) {
        const fs::path relative = project.lexically_relative(data_root);
        if (relative.empty() ||
            (*relative.begin() != fs::path(".."))) {
            result.error = "That folder is Bokkie's own data folder";
            return result;
        }
    }
    error.clear();

    for (const std::string& existing : getWorkspaces()) {
        if (projectPath(existing) == project) {
            result.ok = true;
            result.existed = true;
            result.name = existing;
            return result;
        }
    }

    const std::string base = sanitize_name(project.filename().string(), 40);
    std::string name = base.empty() ? "workspace" : base;
    if (workspaceExists(name)) {
        const std::string parent = sanitize_name(project.parent_path().filename().string(), 40);
        if (!parent.empty()) name += " (" + parent + ")";
    }
    const std::string stem = name;
    for (int n = 2; workspaceExists(name); ++n) name = stem + " " + std::to_string(n);

    const std::string name_error = validateName(name);
    if (!name_error.empty()) {
        result.error = name_error;
        return result;
    }

    fs::create_directories(workspaces_root_, error);
    if (error) {
        result.error = "Could not create Bokkie's data folder";
        return result;
    }
    const fs::path folder = workspacePath(name);
    fs::create_directory(folder, error);
    if (error) {
        result.error = "Could not create the workspace folder";
        return result;
    }

    const json data = {
        {"version", 1},
        {"name", name},
        {"project_path", project.string()},
        {"created_at", current_time_seconds()}
    };
    if (!write_json_atomic(folder / kWorkspaceFile, data)) {
        fs::remove_all(folder, error);
        result.error = "Could not save the workspace";
        return result;
    }
    result.ok = true;
    result.name = name;
    return result;
}

std::vector<Workspace::ChatFile> Workspace::chatFiles(const std::string& workspace_name) const {
    std::vector<ChatFile> files;
    try {
        std::error_code error;
        const fs::path dir = workspacePath(workspace_name);
        if (!fs::is_directory(dir, error) || error) return files;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, error)) {
            if (error) break;
            if (!entry.is_regular_file(error) || error) continue;
            const std::string file_name = entry.path().filename().string();
            if (file_name.empty() || file_name.front() == '.') continue;
            if (entry.path().extension() != ".json") continue;
            fs::file_time_type modified = entry.last_write_time(error);
            if (error) modified = fs::file_time_type::min();
            files.push_back({workspace_name, entry.path(), modified});
        }
    } catch (const std::exception&) {
    }
    return files;
}

Workspace::ChatInfo Workspace::readChatInfo(const ChatFile& file) {
    ChatInfo info;
    info.workspace = file.workspace;
    info.name = file.path.stem().string();
    std::ifstream input(file.path, std::ios::binary);
    const json data = json::parse(input, nullptr, false);
    if (!data.is_discarded() && data.is_object()) {
        if (data.contains("chat") && data["chat"].is_string())
            info.name = data["chat"].get<std::string>();
        if (data.contains("updated_at") && data["updated_at"].is_number_integer())
            info.updated_at = data["updated_at"].get<std::int64_t>();
        if (data.contains("messages") && data["messages"].is_array())
            info.message_count = data["messages"].size();
    }
    return info;
}

std::vector<Workspace::ChatInfo> Workspace::listChats(const std::string& workspace_name) const {
    std::vector<ChatFile> files = chatFiles(workspace_name);
    std::sort(files.begin(), files.end(),
              [](const ChatFile& a, const ChatFile& b) { return a.modified > b.modified; });
    std::vector<ChatInfo> chats;
    for (const ChatFile& file : files) chats.push_back(readChatInfo(file));
    return chats;
}

std::vector<Workspace::ChatInfo> Workspace::recentChats(std::size_t limit) const {
    std::vector<ChatFile> all;
    for (const std::string& name : getWorkspaces()) {
        std::vector<ChatFile> files = chatFiles(name);
        all.insert(all.end(), files.begin(), files.end());
    }
    std::sort(all.begin(), all.end(),
              [](const ChatFile& a, const ChatFile& b) { return a.modified > b.modified; });
    if (all.size() > limit)
        all.erase(all.begin() + static_cast<std::ptrdiff_t>(limit), all.end());
    std::vector<ChatInfo> chats;
    for (const ChatFile& file : all) chats.push_back(readChatInfo(file));
    return chats;
}
