#include "TaskState.h"

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;

fs::path configured_task_state_path() {
    if (const char* override_path = std::getenv("BOKKIE_TASK_STATE_FILE")) {
        if (*override_path != '\0') return fs::path(override_path);
    }
    return fs::path(BOKKIE_TASK_STATE_FILE);
}

const fs::path task_state_path = configured_task_state_path();

std::int64_t current_time() {
    return static_cast<std::int64_t>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::string required_string(const json& arguments, const char* name) {
    const auto value = arguments.find(name);
    if (value == arguments.end() || !value->is_string()) {
        throw std::invalid_argument(std::string("Missing or invalid string argument: ") + name);
    }
    return value->get<std::string>();
}

bool valid_status(const std::string& status) {
    return status == "pending" || status == "in_progress" ||
           status == "completed" || status == "failed";
}

json load_state() {
    std::error_code error;
    if (!fs::exists(task_state_path, error)) {
        if (error) {
            throw std::runtime_error("Could not inspect task state: " + error.message());
        }
        return {{"version", 1}, {"next_id", 1}, {"tasks", json::array()}};
    }

    std::ifstream file(task_state_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open task state for reading");
    }
    if (file.peek() == std::ifstream::traits_type::eof()) {
        if (file.bad()) {
            throw std::runtime_error("Could not inspect task state file");
        }
        return {{"version", 1}, {"next_id", 1}, {"tasks", json::array()}};
    }

    json state;
    try {
        file >> state;
    } catch (const json::exception& exception) {
        throw std::runtime_error(std::string("Task state contains invalid JSON: ") +
                                 exception.what());
    }
    if (file.bad()) {
        throw std::runtime_error("Could not read task state");
    }
    if (!state.is_object() || state.value("version", 0) != 1 ||
        !state.contains("next_id") ||
        (!state["next_id"].is_number_unsigned() && !state["next_id"].is_number_integer()) ||
        !state.contains("tasks") || !state["tasks"].is_array()) {
        throw std::runtime_error("Task state has an unsupported or invalid format");
    }
    const json& next_id = state["next_id"];
    if ((next_id.is_number_unsigned() && next_id.get<std::uint64_t>() == 0) ||
        (next_id.is_number_integer() && !next_id.is_number_unsigned() &&
         next_id.get<std::int64_t>() <= 0)) {
        throw std::runtime_error("Task state next_id must be positive");
    }
    for (const auto& task : state["tasks"]) {
        if (!task.is_object() || !task.contains("id") || !task["id"].is_string() ||
            !task.contains("title") || !task["title"].is_string() ||
            !task.contains("instructions") || !task["instructions"].is_string() ||
            !task.contains("status") || !task["status"].is_string() ||
            !valid_status(task["status"].get<std::string>()) ||
            !task.contains("progress") || !task["progress"].is_array() ||
            !task.contains("outcome") || !task["outcome"].is_string()) {
            throw std::runtime_error("Task state contains an invalid task record");
        }
        if (task.contains("status_history") && !task["status_history"].is_array()) {
            throw std::runtime_error("Task state contains invalid status history");
        }
    }
    for (auto& task : state["tasks"]) {
        if (!task.contains("status_history")) {
            task["status_history"] = json::array({
                {{"status", task["status"]},
                 {"outcome", task["outcome"]},
                 {"updated_at", task.value("updated_at", task.value("created_at", 0LL))}}
            });
        }
    }
    return state;
}

void save_state(const json& state) {
    std::error_code error;
    fs::create_directories(task_state_path.parent_path(), error);
    if (error) {
        throw std::runtime_error("Could not create task state directory: " + error.message());
    }

    fs::path temporary_path = task_state_path;
    temporary_path += ".tmp";
    {
        std::ofstream file(temporary_path, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("Could not open temporary task state for writing");
        }
        const std::string serialized = state.dump(2) + "\n";
        file.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        file.flush();
        if (!file) {
            throw std::runtime_error("Could not write temporary task state");
        }
    }

#ifdef _WIN32
    if (!MoveFileExW(temporary_path.c_str(), task_state_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error_code = GetLastError();
        fs::remove(temporary_path, error);
        throw std::runtime_error("Could not replace task state file (Windows error " +
                                 std::to_string(error_code) + ")");
    }
#else
    fs::rename(temporary_path, task_state_path, error);
    if (error) {
        fs::remove(temporary_path);
        throw std::runtime_error("Could not replace task state file: " + error.message());
    }
#endif
}

json* find_task(json& state, const std::string& id) {
    for (auto& task : state["tasks"]) {
        if (task.is_object() && task.value("id", std::string()) == id) {
            return &task;
        }
    }
    return nullptr;
}

json task_error(const std::string& message) {
    return {{"success", false}, {"error", message}};
}

void update_task_status(json& task, const std::string& status,
                        const std::string& outcome, const std::int64_t timestamp) {
    if (task["status"] != status || task["outcome"] != outcome) {
        task["status_history"].push_back({
            {"status", status},
            {"outcome", outcome},
            {"updated_at", timestamp}
        });
    }
    task["status"] = status;
    task["outcome"] = outcome;
    task["updated_at"] = timestamp;
}

json finish_task(const json& arguments, const std::string& status) {
    const std::string id = required_string(arguments, "id");
    const std::string outcome = required_string(arguments, "outcome");
    if (outcome.empty()) return task_error("outcome must not be empty");

    json state = load_state();
    json* task = find_task(state, id);
    if (!task) return task_error("Task not found: " + id);
    if ((*task)["status"] == "completed" || (*task)["status"] == "failed") {
        return task_error("Task is already completed or failed; reopen it before changing its outcome");
    }

    update_task_status(*task, status, outcome, current_time());
    save_state(state);
    return {{"success", true}, {"task", *task}};
}

}

json TaskState::create_task(const json& arguments) {
    const std::string title = required_string(arguments, "title");
    const std::string instructions = required_string(arguments, "instructions");
    if (title.empty()) return task_error("title must not be empty");
    if (instructions.empty()) return task_error("instructions must not be empty");

    json state = load_state();
    const std::uint64_t id = state["next_id"].get<std::uint64_t>();
    if (id == std::numeric_limits<std::uint64_t>::max()) {
        return task_error("Task ID space is exhausted");
    }
    const std::string task_id = "task-" + std::to_string(id);
    const std::int64_t timestamp = current_time();
    json task = {
        {"id", task_id},
        {"title", title},
        {"instructions", instructions},
        {"status", "pending"},
        {"progress", json::array()},
        {"outcome", ""},
        {"status_history", json::array({
            {{"status", "pending"}, {"outcome", ""}, {"updated_at", timestamp}}
        })},
        {"created_at", timestamp},
        {"updated_at", timestamp}
    };

    state["tasks"].push_back(task);
    state["next_id"] = id + 1;
    save_state(state);
    return {{"success", true}, {"task", task}};
}

json TaskState::update_task(const json& arguments) {
    const std::string id = required_string(arguments, "id");
    const auto title = arguments.find("title");
    const auto instructions = arguments.find("instructions");
    if (title == arguments.end() && instructions == arguments.end()) {
        return task_error("Provide title and/or instructions to update");
    }
    if (title != arguments.end() && (!title->is_string() || title->get<std::string>().empty())) {
        return task_error("title must be a non-empty string");
    }
    if (instructions != arguments.end() &&
        (!instructions->is_string() || instructions->get<std::string>().empty())) {
        return task_error("instructions must be a non-empty string");
    }

    json state = load_state();
    json* task = find_task(state, id);
    if (!task) return task_error("Task not found: " + id);
    if (title != arguments.end()) (*task)["title"] = *title;
    if (instructions != arguments.end()) (*task)["instructions"] = *instructions;
    (*task)["updated_at"] = current_time();
    save_state(state);
    return {{"success", true}, {"task", *task}};
}

json TaskState::record_progress(const json& arguments) {
    const std::string id = required_string(arguments, "id");
    const std::string progress = required_string(arguments, "progress");
    if (progress.empty()) return task_error("progress must not be empty");

    json state = load_state();
    json* task = find_task(state, id);
    if (!task) return task_error("Task not found: " + id);
    if ((*task)["status"] == "completed" || (*task)["status"] == "failed") {
        return task_error("Cannot record progress on a completed or failed task");
    }

    const std::int64_t timestamp = current_time();
    (*task)["progress"].push_back({{"note", progress}, {"created_at", timestamp}});
    if ((*task)["status"] == "pending") {
        update_task_status(*task, "in_progress", "", timestamp);
    } else {
        (*task)["updated_at"] = timestamp;
    }
    save_state(state);
    return {{"success", true}, {"task", *task}};
}

json TaskState::set_status(const json& arguments) {
    const std::string id = required_string(arguments, "id");
    const std::string status = required_string(arguments, "status");
    if (status != "pending" && status != "in_progress") {
        return task_error("status must be pending or in_progress; use complete_task or fail_task for terminal status");
    }

    json state = load_state();
    json* task = find_task(state, id);
    if (!task) return task_error("Task not found: " + id);
    update_task_status(*task, status, "", current_time());
    save_state(state);
    return {{"success", true}, {"task", *task}};
}

json TaskState::complete_task(const json& arguments) {
    return finish_task(arguments, "completed");
}

json TaskState::fail_task(const json& arguments) {
    return finish_task(arguments, "failed");
}

json TaskState::list_tasks(const json& arguments) {
    const auto status = arguments.find("status");
    if (status != arguments.end() &&
        (!status->is_string() || !valid_status(status->get<std::string>()))) {
        return task_error("status must be pending, in_progress, completed, or failed");
    }

    const json state = load_state();
    json tasks = json::array();
    for (const auto& task : state["tasks"]) {
        if (status == arguments.end() || task.value("status", std::string()) == status->get<std::string>()) {
            tasks.push_back(task);
        }
    }
    return {{"success", true}, {"tasks", std::move(tasks)}};
}

std::string TaskState::render_task_state() {
    const json state = load_state();
    json active = json::array();
    std::vector<const json*> finished;
    for (const auto& task : state["tasks"]) {
        const std::string status = task.value("status", std::string());
        if (status == "pending" || status == "in_progress") {
            json summary = {
                {"id", task["id"]},
                {"title", task["title"]},
                {"instructions", task["instructions"]},
                {"status", task["status"]},
                {"progress", task["progress"]}
            };
            active.push_back(std::move(summary));
        } else {
            finished.push_back(&task);
        }
    }
    std::stable_sort(finished.begin(), finished.end(), [](const json* left, const json* right) {
        return left->value("updated_at", std::int64_t(0)) <
               right->value("updated_at", std::int64_t(0));
    });

    json recent_finished = json::array();
    const size_t first_recent = finished.size() > 5 ? finished.size() - 5 : 0;
    for (size_t i = first_recent; i < finished.size(); ++i) {
        const json& task = *finished[i];
        recent_finished.push_back({
            {"id", task["id"]},
            {"title", task["title"]},
            {"status", task["status"]},
            {"outcome", task["outcome"]}
        });
    }

    if (active.empty() && recent_finished.empty()) return {};
    return "Persistent task state (completed/failed tasks below are history, not active instructions):\n"
           + json({{"active_tasks", std::move(active)},
                   {"recent_completed_or_failed", std::move(recent_finished)}}).dump(2);
}

std::string TaskState::format_tool_result(const std::string& tool_name,
                                          const json& result) {
    if (!result.is_object()) {
        return "Task operation returned an invalid result.";
    }

    const auto success = result.find("success");
    if (success == result.end() || !success->is_boolean()) {
        return "Task operation returned an invalid result.";
    }
    if (!success->get<bool>()) {
        const auto error = result.find("error");
        return "Task action failed: " +
            (error != result.end() && error->is_string()
                ? error->get<std::string>()
                : std::string("an unspecified error occurred"));
    }

    std::ostringstream output;
    output << "Task state";
    if (tool_name == "list_tasks") {
        const auto tasks = result.find("tasks");
        if (tasks == result.end() || !tasks->is_array()) {
            return "Task list could not be displayed because the result was invalid.";
        }
        if (tasks->empty()) {
            return "No tasks found.";
        }
        output << " (" << tasks->size() << "):";
        for (const auto& task : *tasks) {
            output << "\n\n";
            if (!task.is_object()) {
                output << "Invalid task record.";
                continue;
            }
            output << "Task " << task.value("id", std::string("(unknown ID)"))
                   << ": " << task.value("title", std::string("(untitled)"))
                   << "\n  Status: " << task.value("status", std::string("unknown"))
                   << "\n  Instructions: "
                   << task.value("instructions", std::string("(none)"));
            const auto progress = task.find("progress");
            if (progress != task.end() && progress->is_array() && !progress->empty()) {
                output << "\n  Progress:";
                for (const auto& entry : *progress) {
                    if (entry.is_object() && entry.contains("note") &&
                        entry["note"].is_string()) {
                        output << "\n    - " << entry["note"].get<std::string>();
                    }
                }
            }
            const std::string outcome = task.value("outcome", std::string());
            if (!outcome.empty()) {
                output << "\n  Outcome: " << outcome;
            }
        }
        return output.str();
    }

    const auto task = result.find("task");
    if (task == result.end() || !task->is_object()) {
        return "Task operation succeeded.";
    }
    output << " updated successfully:\n"
           << "  ID: " << task->value("id", std::string("(unknown ID)"))
           << "\n  Title: " << task->value("title", std::string("(untitled)"))
           << "\n  Status: " << task->value("status", std::string("unknown"))
           << "\n  Instructions: "
           << task->value("instructions", std::string("(none)"));
    const auto progress = task->find("progress");
    if (progress != task->end() && progress->is_array() && !progress->empty()) {
        output << "\n  Progress:";
        for (const auto& entry : *progress) {
            if (entry.is_object() && entry.contains("note") && entry["note"].is_string()) {
                output << "\n    - " << entry["note"].get<std::string>();
            }
        }
    }
    const std::string outcome = task->value("outcome", std::string());
    if (!outcome.empty()) {
        output << "\n  Outcome: " << outcome;
    }
    return output.str();
}
