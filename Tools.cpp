#include "Tools.h"
#include "ProcessRunner.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <cwchar>
#endif

namespace {
namespace fs = std::filesystem;
fs::path workspace_root;

std::string required_string(const json& arguments, const char* name) {
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_string()) {
        throw std::invalid_argument(std::string("Missing or invalid string argument: ") + name);
    }
    return value->get<std::string>();
}

json failure(const std::string& message) {
    return {{"success", false}, {"error", message}};
}

std::string utf8_output(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t index = 0; index < input.size();) {
        const unsigned char lead = static_cast<unsigned char>(input[index]);
        size_t length = 0;
        if (lead <= 0x7f) length = 1;
        else if (lead >= 0xc2 && lead <= 0xdf) length = 2;
        else if (lead >= 0xe0 && lead <= 0xef) length = 3;
        else if (lead >= 0xf0 && lead <= 0xf4) length = 4;
        bool valid = length != 0 && index + length <= input.size();
        for (size_t offset = 1; valid && offset < length; ++offset) {
            const unsigned char next = static_cast<unsigned char>(input[index + offset]);
            valid = next >= 0x80 && next <= 0xbf;
        }
        if (valid && length == 3) {
            const unsigned char second = static_cast<unsigned char>(input[index + 1]);
            valid = !(lead == 0xe0 && second < 0xa0) &&
                    !(lead == 0xed && second >= 0xa0);
        }
        if (valid && length == 4) {
            const unsigned char second = static_cast<unsigned char>(input[index + 1]);
            valid = !(lead == 0xf0 && second < 0x90) &&
                    !(lead == 0xf4 && second >= 0x90);
        }
        if (valid) {
            output.append(input, index, length);
            index += length;
        } else {
            output += "\xef\xbf\xbd";
            ++index;
        }
    }
    return output;
}

int execution_timeout(const json& arguments) {
    const auto value = arguments.find("timeout_seconds");
    if (value == arguments.end()) return 10;
    if (!value->is_number_integer()) {
        throw std::invalid_argument("timeout_seconds must be an integer");
    }
    const int timeout = value->get<int>();
    if (timeout < 1 || timeout > 60) {
        throw std::invalid_argument("timeout_seconds must be between 1 and 60");
    }
    return timeout;
}

json execution_result(const ProcessResult& result) {
    json output = {
        {"success", result.started && !result.timed_out && result.exit_code == 0},
        {"exit_code", result.started ? json(result.exit_code) : json(nullptr)},
        {"timed_out", result.timed_out},
        {"output", utf8_output(result.output)},
        {"output_truncated", result.output_truncated}
    };
    if (!result.error.empty()) output["error"] = result.error;
    return output;
}

bool same_path_component(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
#else
    return left == right;
#endif
}

bool is_within_workspace(const fs::path& path) {
    auto root_part = workspace_root.begin();
    auto path_part = path.begin();
    for (; root_part != workspace_root.end(); ++root_part, ++path_part) {
        if (path_part == path.end() || !same_path_component(*root_part, *path_part)) {
            return false;
        }
    }
    return true;
}

fs::path resolve_workspace_path(const std::string& requested_path) {
    if (workspace_root.empty()) {
        throw std::runtime_error("Workspace has not been configured");
    }
    if (requested_path.empty()) {
        throw std::invalid_argument("Path must not be empty");
    }

    const fs::path requested(requested_path);
#ifdef _WIN32
    if (requested.has_root_name() && !requested.is_absolute()) {
        throw std::invalid_argument("Drive-relative paths are not allowed");
    }
#endif
    const fs::path combined = requested.is_absolute() ? requested : workspace_root / requested;
    std::error_code error;
    const fs::path resolved = fs::weakly_canonical(combined, error);
    if (error) {
        throw std::runtime_error("Could not resolve path: " + error.message());
    }
    if (!is_within_workspace(resolved)) {
        throw std::invalid_argument("Path escapes the configured workspace");
    }
    return resolved;
}

json directory_entries(const fs::path& directory, int kind) {
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        return failure(error ? error.message() : "Path is not a directory");
    }

    json entries = json::array();
    fs::directory_iterator iterator(directory, error);
    const fs::directory_iterator end;
    while (!error && iterator != end) {
        const bool is_directory = iterator->is_directory(error);
        if (error) break;
        if (kind == 2 || (kind == 0 && !is_directory) || (kind == 1 && is_directory)) {
            entries.push_back(iterator->path().string());
        }
        iterator.increment(error);
    }
    if (error) return failure(error.message());

    std::sort(entries.begin(), entries.end());
    return {
        {"success", true},
        {"resolved_path", directory.string()},
        {"entries", std::move(entries)}
    };
}
}

void Tools::set_workspace(const std::string& workspace) {
    if (workspace.empty()) {
        throw std::invalid_argument("Configured workspace must not be empty");
    }

    std::error_code error;
    const fs::path resolved = fs::canonical(fs::path(workspace), error);
    if (error) {
        throw std::runtime_error("Could not resolve configured workspace: " + error.message());
    }
    if (!fs::is_directory(resolved, error) || error) {
        throw std::runtime_error("Configured workspace is not an accessible directory");
    }
    workspace_root = resolved;
}

json Tools::read_file(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    std::ifstream file(path, std::ios::binary);
    if (!file) return failure("Could not open file: " + path.string());

    std::ostringstream content;
    content << file.rdbuf();
    if (file.bad()) return failure("Could not read file: " + path.string());
    return {
        {"success", true},
        {"resolved_path", path.string()},
        {"content", content.str()}
    };
}

json Tools::write_file(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    const std::string content = required_string(arguments, "content");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return failure("Could not open file for writing: " + path.string());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) return failure("Could not write file: " + path.string());
    return {{"success", true}, {"resolved_path", path.string()}};
}

json Tools::edit_file(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    const std::string old_content = required_string(arguments, "old_content");
    const std::string new_content = required_string(arguments, "new_content");
    if (old_content.empty()) return failure("old_content must not be empty");

    std::ifstream file(path, std::ios::binary);
    if (!file) return failure("Could not open file: " + path.string());
    std::ostringstream contents;
    contents << file.rdbuf();
    if (file.bad()) return failure("Could not read file: " + path.string());

    std::string updated = contents.str();
    const size_t position = updated.find(old_content);
    if (position == std::string::npos) return failure("old_content was not found in the file");
    updated.replace(position, old_content.size(), new_content);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return failure("Could not open file for writing: " + path.string());
    output.write(updated.data(), static_cast<std::streamsize>(updated.size()));
    if (!output) return failure("Could not write file: " + path.string());
    return {{"success", true}, {"resolved_path", path.string()}, {"replacements", 1}};
}

json Tools::append_file(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    const std::string content = required_string(arguments, "content");
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) return failure("Could not open file for appending: " + path.string());
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!file) return failure("Could not append to file: " + path.string());
    return {{"success", true}, {"resolved_path", path.string()}};
}

json Tools::delete_file(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    std::error_code error;
    if (fs::is_directory(path, error)) {
        return failure("Path is a directory: " + path.string());
    }
    if (error) return failure(error.message());
    const bool removed = fs::remove(path, error);
    if (error) return failure(error.message());
    if (!removed) return failure("File does not exist: " + path.string());
    return {{"success", true}, {"resolved_path", path.string()}};
}

json Tools::list_files(const json& arguments) {
    return directory_entries(resolve_workspace_path(required_string(arguments, "directory_path")), 0);
}

json Tools::list_directories(const json& arguments) {
    return directory_entries(resolve_workspace_path(required_string(arguments, "directory_path")), 1);
}

json Tools::list_directory_contents(const json& arguments) {
    return directory_entries(resolve_workspace_path(required_string(arguments, "directory_path")), 2);
}

json Tools::move_path(const json& arguments) {
    const fs::path source = resolve_workspace_path(required_string(arguments, "source_path"));
    const fs::path destination = resolve_workspace_path(required_string(arguments, "destination_path"));
    std::error_code error;
    fs::rename(source, destination, error);
    if (error) return failure(error.message());
    return {
        {"success", true},
        {"resolved_source_path", source.string()},
        {"resolved_destination_path", destination.string()}
    };
}

json Tools::copy_path(const json& arguments) {
    const fs::path source = resolve_workspace_path(required_string(arguments, "source_path"));
    const fs::path destination = resolve_workspace_path(required_string(arguments, "destination_path"));
    std::error_code error;
    const fs::file_status status = fs::status(source, error);
    if (error) return failure(error.message());

    if (fs::is_directory(status)) {
        fs::copy(source, destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    } else {
        fs::copy_file(source, destination, fs::copy_options::overwrite_existing, error);
    }
    if (error) return failure(error.message());
    return {
        {"success", true},
        {"resolved_source_path", source.string()},
        {"resolved_destination_path", destination.string()}
    };
}

json Tools::create_directory(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "directory_path"));
    std::error_code error;
    const bool created = fs::create_directories(path, error);
    if (error) return failure(error.message());
    return {{"success", true}, {"resolved_path", path.string()}, {"created", created}};
}

json Tools::delete_directory(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "directory_path"));
    if (path == workspace_root) {
        return failure("Refusing to delete the workspace root");
    }
    std::error_code error;
    if (!fs::is_directory(path, error)) {
        return failure(error ? error.message() : "Path is not a directory");
    }
    const uintmax_t removed = fs::remove_all(path, error);
    if (error) return failure(error.message());
    return {{"success", true}, {"resolved_path", path.string()}, {"removed_entries", removed}};
}

json Tools::get_path_info(const json& arguments) {
    const fs::path path = resolve_workspace_path(required_string(arguments, "path"));
    std::error_code error;
    const fs::file_status status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        return {{"success", true}, {"resolved_path", path.string()}, {"exists", false}};
    }
    if (error) return failure(error.message());
    if (!fs::exists(status)) {
        return {{"success", true}, {"resolved_path", path.string()}, {"exists", false}};
    }

    std::string type = "other";
    if (fs::is_regular_file(status)) type = "file";
    else if (fs::is_directory(status)) type = "directory";
    else if (fs::is_symlink(status)) type = "symlink";

    json result = {
        {"success", true},
        {"resolved_path", path.string()},
        {"exists", true},
        {"type", type}
    };
    if (fs::is_regular_file(status)) {
        const uintmax_t size = fs::file_size(path, error);
        if (error) return failure(error.message());
        result["size_bytes"] = size;
    }
    const auto modified = fs::last_write_time(path, error);
    if (error) return failure(error.message());
    const auto system_modified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    result["last_modified"] = std::chrono::system_clock::to_time_t(system_modified);
    return result;
}

json Tools::run_shell(const json& arguments) {
    const std::string command = required_string(arguments, "command");
    if (command.empty()) return failure("command must not be empty");
    const int timeout = execution_timeout(arguments);
#ifdef _WIN32
    const auto result = ProcessRunner::run(
        "cmd.exe", {"/D", "/S", "/C", command}, workspace_root, timeout, 32768);
#else
    const auto result = ProcessRunner::run(
        "/bin/sh", {"-c", command}, workspace_root, timeout, 32768);
#endif
    return execution_result(result);
}

json Tools::run_python(const json& arguments) {
    const std::string code = required_string(arguments, "code");
    if (code.empty()) return failure("code must not be empty");
    const int timeout = execution_timeout(arguments);
#ifdef _WIN32
    const auto result = ProcessRunner::run(
        "python", {"-c", code}, workspace_root, timeout, 32768);
#else
    const auto result = ProcessRunner::run(
        "python3", {"-c", code}, workspace_root, timeout, 32768);
#endif
    return execution_result(result);
}
