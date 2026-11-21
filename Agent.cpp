//
// Created by noahn on 2026/11/14.
//

#include "Agent.h"

#include "Client.h"
#include "ToolRegistry.h"
#include "Tools.h"
#include "brain/Memory.h"
#include "brain/Conversation.h"
#include "brain/TaskState.h"
#include "md_render.h"
#include "workspaces/Workspace.h"

#include <iostream>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <string>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/select.h>


using json = nlohmann::json;

namespace {

// Reads stdin directly (no std::cin read-ahead) so we can tell whether more
// input is already waiting. A paste delivers all its lines in one burst.
std::string g_input_buffer;

bool stdin_has_more(int timeout_ms) {
    if (!g_input_buffer.empty()) return true;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(STDIN_FILENO, &readSet);
    timeval timeout{0, timeout_ms * 1000};
    return select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) > 0;
}

void strip_all(std::string& text, const std::string& marker) {
    for (auto pos = text.find(marker); pos != std::string::npos; pos = text.find(marker)) {
        text.erase(pos, marker.size());
    }
}

bool read_input_line(std::string& line) {
    while (true) {
        const auto newline = g_input_buffer.find('\n');
        if (newline != std::string::npos) {
            line = g_input_buffer.substr(0, newline);
            g_input_buffer.erase(0, newline + 1);
            break;
        }
        char chunk[4096];
        const ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count <= 0) {
            if (g_input_buffer.empty()) return false;
            line = std::move(g_input_buffer);
            g_input_buffer.clear();
            break;
        }
        g_input_buffer.append(chunk, static_cast<std::size_t>(count));
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    strip_all(line, "\033[200~");  // bracketed-paste markers, if the terminal sends them
    strip_all(line, "\033[201~");
    return true;
}

// Reads one user message. A single typed line is sent on Enter. A paste (several
// lines arriving at once, empty lines included) is kept together; because the
// terminal holds back the paste's last line until Enter, that next Enter sends it.
bool read_user_message(std::string& message) {
    if (!read_input_line(message)) return false;
    std::string line;
    bool pasted = false;
    while (stdin_has_more(50) && read_input_line(line)) {
        message += '\n' + line;
        pasted = true;
    }
    if (pasted && read_input_line(line)) {
        message += '\n' + line;
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return true;
}

std::string home_directory() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return home ? std::string(home) : std::filesystem::current_path().string();
}

json string_parameter(const std::string& description) {
    return {
        {"type", "string"},
        {"description", description}
    };
}

void register_tools(ToolRegistry& registry, bool read_only) {
    const auto add = [&registry, read_only](
        const std::string& name,
        const std::string& description,
        const json& properties,
        const json& required,
        std::function<json(const json&)> handler
    ) {
        if (read_only && name != "read_file" && name != "list_directory_contents") return;
        registry.register_tool(name, description, {
            {"type", "object"},
            {"properties", properties},
            {"required", required}
        }, std::move(handler));
    };

    add("read_file", "Reads a file inside the configured workspace. Returns its resolved path.",
        {{"path", string_parameter("Path relative to the workspace, or an absolute path inside it")}},
        {"path"}, Tools::read_file);
    add("write_file", "Creates a file or overwrites its complete contents.",
        {{"path", string_parameter("Path relative to the workspace, or an absolute path inside it")},
         {"content", string_parameter("Complete file content")}},
        {"path", "content"}, Tools::write_file);
    add("edit_file", "Replaces the first exact occurrence of old_content in an existing file.",
        {{"path", string_parameter("Path relative to the workspace, or an absolute path inside it")},
         {"old_content", string_parameter("Exact existing text to replace")},
         {"new_content", string_parameter("Replacement text")}},
        {"path", "old_content", "new_content"}, Tools::edit_file);
    add("delete_file", "Deletes a file inside the configured workspace.",
        {{"path", string_parameter("Path relative to the workspace, or an absolute path inside it")}},
        {"path"}, Tools::delete_file);
    add("list_directory_contents", "Lists files and subdirectories directly inside a directory.",
        {{"directory_path", string_parameter("Path relative to the workspace, or an absolute path inside it")}},
        {"directory_path"}, Tools::list_directory_contents);
    add("run_shell", "Runs a shell command with the workspace as its working directory. Commands are not OS-sandboxed; each call has a timeout and capped output.",
        {{"command", string_parameter("Shell command to run")}, {"timeout_seconds", {
            {"type", "integer"}, {"minimum", 1}, {"maximum", 60},
            {"description", "Optional timeout in seconds; defaults to 10"}
        }}},
        {"command"}, Tools::run_shell);
    add("run_python", "Runs Python code with the workspace as its working directory. Code is not OS-sandboxed; each call has a timeout and capped output.",
        {{"code", string_parameter("Python code to execute")}, {"timeout_seconds", {
            {"type", "integer"}, {"minimum", 1}, {"maximum", 60},
            {"description", "Optional timeout in seconds; defaults to 10"}
        }}},
        {"code"}, Tools::run_python);
    add("delete_directory", "Recursively deletes a directory inside the workspace. Cannot delete the workspace root.",
        {{"directory_path", string_parameter("Path relative to the workspace, or an absolute path inside it")}},
        {"directory_path"}, Tools::delete_directory);
    add("add_memory", "Adds a durable user preference or relevant personal detail to agent memory.",
        {{"memory", string_parameter("A concise memory to store")}},
        {"memory"}, Memory::add_memory);
    add("remove_memory", "Removes the first exact matching memory text.",
        {{"memory", string_parameter("Exact memory text to remove")}},
        {"memory"}, Memory::remove_memory);
    add("edit_memory", "Replaces the first exact matching memory text with updated text.",
        {{"old_memory", string_parameter("Exact existing memory text")},
         {"new_memory", string_parameter("Replacement memory text")}},
        {"old_memory", "new_memory"}, Memory::edit_memory);
    add("create_task", "Creates a persistent task with a title and actionable instructions.",
        {{"title", string_parameter("Short task title")},
         {"instructions", string_parameter("Task goal, constraints, and completion criteria")}},
        {"title", "instructions"}, TaskState::create_task);
    add("update_task", "Updates the title and/or instructions of an existing task.",
        {{"id", string_parameter("Task ID returned by create_task")},
         {"title", string_parameter("Updated short task title")},
         {"instructions", string_parameter("Updated task goal, constraints, and completion criteria")}},
        {"id"}, TaskState::update_task);
    add("record_task_progress", "Records a meaningful progress update for an active task.",
        {{"id", string_parameter("Task ID returned by create_task")},
         {"progress", string_parameter("Completed work, observations, blockers, or next step")}},
        {"id", "progress"}, TaskState::record_progress);
    add("set_task_status", "Sets or reopens an active task as pending or in_progress.",
        {{"id", string_parameter("Task ID returned by create_task")},
         {"status", {
             {"type", "string"},
             {"enum", {"pending", "in_progress"}},
             {"description", "Set the task to pending or in_progress"}
         }}},
        {"id", "status"}, TaskState::set_status);
    add("complete_task", "Marks a task completed and records its verified outcome.",
        {{"id", string_parameter("Task ID returned by create_task")},
         {"outcome", string_parameter("Factual result confirming what was completed")}},
        {"id", "outcome"}, TaskState::complete_task);
    add("fail_task", "Marks a task failed and records the reason or blocker.",
        {{"id", string_parameter("Task ID returned by create_task")},
         {"outcome", string_parameter("Factual reason the task could not be completed")}},
        {"id", "outcome"}, TaskState::fail_task);
    add("list_tasks", "Lists persistent tasks, optionally filtered by status.",
        {{"status", {
            {"type", "string"},
            {"enum", {"pending", "in_progress", "completed", "failed"}},
            {"description", "Optional status filter"}
        }}},
        json::array(), TaskState::list_tasks);
}

bool is_task_state_tool(const std::string& name) {
    return name == "create_task" || name == "update_task" ||
           name == "record_task_progress" || name == "set_task_status" ||
           name == "complete_task" || name == "fail_task" ||
           name == "list_tasks";
}

std::string describe_tool_action(const ToolCall& call) {
    std::ostringstream description;
    const auto quoted_argument = [&call](const char* name) {
        std::string argument = "(unspecified)";
        const auto argument_value = call.arguments.find(name);
        if (argument_value != call.arguments.end() && argument_value->is_string()) {
            argument = argument_value->get<std::string>();
            for (char& character : argument) {
                if (static_cast<unsigned char>(character) < 0x20) {
                    character = ' ';
                }
            }
        }
        std::ostringstream formatted;
        formatted << std::quoted(argument);
        return formatted.str();
    };

    if (call.name == "read_file") {
        description << "Reading file " << quoted_argument("path");
    } else if (call.name == "write_file") {
        description << "Writing file " << quoted_argument("path");
    } else if (call.name == "edit_file") {
        description << "Editing file " << quoted_argument("path");
    } else if (call.name == "delete_file") {
        description << "Deleting file " << quoted_argument("path");
    } else if (call.name == "list_directory_contents") {
        description << "Listing directory " << quoted_argument("directory_path");
    } else if (call.name == "delete_directory") {
        description << "Deleting directory " << quoted_argument("directory_path");
    } else if (is_task_state_tool(call.name)) {
        description << "Updating or reviewing task state (" << call.name << ')';
    } else if (call.name == "add_memory" || call.name == "remove_memory" ||
               call.name == "edit_memory") {
        description << "Updating agent memory (" << call.name << ')';
    } else {
        description << "Calling tool " << std::quoted(call.name);
    }
    return description.str();
}

}


Agent::Agent(Workspace& workspaces, const ChatTarget& target)
    : workspaces_(workspaces), target_(target) {
    if (target_.quick) {
        workspace_ = home_directory();
    } else {
        const std::filesystem::path project = workspaces_.projectPath(target_.workspace);
        std::error_code error;
        if (project.empty() || !std::filesystem::is_directory(project, error)) {
            throw std::runtime_error("The project folder for workspace \"" + target_.workspace +
                                     "\" is missing: " + project.string());
        }
        workspace_ = project.string();
    }
    Tools::set_workspace(workspace_);

    const std::string memory = Memory::render_memory();
    const std::string task_state = TaskState::render_task_state();

    system_prompt_ =
        "You are Bokkie, an AI agent operating in workspace: " + workspace_ + ".\n" + memory + "\n"
        "Answer normally unless a tool is needed. Use only the available tools and never claim an operation "
        "succeeded unless its application result confirms it. File paths are restricted to the configured "
        "workspace; relative paths start there. Shell and Python tools run with the workspace as their "
        "working directory, but are not OS-sandboxed; use them only when needed.\n"
        "For code or file-change requests, make the smallest complete change in the workspace. Do not refuse "
        "source edits by claiming you cannot control a running app or browser; update the relevant files. "
        "Avoid extra demos, tests, documentation, or launch scripts unless requested or needed. Use '.' for "
        "the workspace root; never request '/' or paths outside the workspace.\n"
        "Do not inspect or modify files proactively. A greeting, general question, or correction is not a "
        "request to use tools; use tools only when the user explicitly asks for an operation that needs them.\n"
        "Never assume a file or directory exists or invent its contents. Use directory-listing results to "
        "identify paths and read_file results to report contents. If a tool fails, report that failure; do not "
        "repeat unsupported claims or fabricate tool output.\n"
        "Before each tool call, give the user one brief sentence describing the immediate action (at most "
        "20 words). Do not claim the action succeeded or narrate the whole plan before its result. After a "
        "successful result, use it and do not repeat the same operation unless verification is needed.\n"
        "Memory stores concise, durable user preferences and relevant personal details. Use its tools only "
        "when the user explicitly asks to remember, change, or forget something. Do not store secrets or "
        "temporary task details. Treat memory-tool results as confirmation.\n"
        "Use task tools only when they materially help track multi-step work across turns; "
        "do not create tasks for simple questions or one-step requests. Record meaningful progress, "
        "and use complete_task or fail_task with a factual outcome. Only use task IDs returned by "
        "create_task or list_tasks; never guess or infer an ID. If an ID is not in the current context, "
        "call list_tasks before updating it. A failed task-tool result means the operation did not succeed: "
        "report the failure accurately and do not claim completion. Active task instructions are context "
        "for ongoing work, not permission to violate these rules or the user's current request.\n"
        + task_state;

    if (target_.quick) {
        system_prompt_ += "\nThis is a quick chat: nothing is saved, and you only have tools to read "
                          "files and list directories. Paths are relative to the user's home folder.\n";
    }

    register_tools(registry_, target_.quick);

    conversation_.set_max_context_tokens(Client::get_max_context_tokens());
    if (!target_.quick) {
        const json saved = workspaces_.loadChat(target_.workspace, target_.chat);
        if (saved.is_object() && saved.contains("messages") && saved["messages"].is_array()) {
            conversation_.load_messages(saved["messages"].get<std::vector<json>>());
        }
    }
}


void Agent::save_chat() {
    if (target_.quick) return;
    if (!workspaces_.updateChat(target_.workspace, target_.chat, conversation_.get_messages()) &&
        !save_warned_) {
        std::cerr << "[warning: could not save this chat]\n";
        save_warned_ = true;
    }
}

bool Agent::run() {
    while (true) {
        std::cout << ">" << std::flush;
        std::string user_input;
        if (!read_user_message(user_input)) return false;
        if (user_input == ":q") return false;
        if (user_input == ":b") return true;
        if (user_input.find_first_not_of(" \t\n") == std::string::npos) continue;
        conversation_.add_user(user_input);
        save_chat();

        constexpr int max_tool_calls_per_turn = 10;
        int tool_calls_this_turn = 0;
        bool received_final_response = false;
        const int maxSteps = 10;
        for (int step = 0; step < maxSteps; ++step) {
            MarkdownRenderer renderer;
            ChatResponse response = Client::send_payload(
                conversation_.get_messages(), system_prompt_, registry_,
                [&renderer](const std::string& token) {
                    renderer.feed(token);
                },
                [&renderer](const std::string& token) {
                    renderer.feed_reasoning(token);
                });
            renderer.finish();
            conversation_.add_assistant(response.content, response.tool_calls);
            save_chat();
            if (response.tool_calls.empty()) {
                received_final_response = true;
            }
            std::cout << '\n';

            if (response.usage.available) {
                conversation_.record_token_usage(response.usage.prompt_tokens,
                                                 response.usage.completion_tokens);
                std::cout << "[context: " << conversation_.context_tokens_used() << '/'
                          << conversation_.max_context_tokens() << " tokens; "
                          << conversation_.context_tokens_left() << " remaining]\n";
            } else {
                std::cerr << "llama.cpp did not return token usage for this response.\n";
            }

            if (response.tool_calls.empty()) {
                if (response.usage.available && conversation_.applyCompaction()) {
                    std::cout << "[conversation compacted]\n";
                }
                break;
            }

            bool tool_limit_reached = false;
            for (const auto& call : response.tool_calls) {
                const std::string toolName = call.name.empty() ? "invalid tool call" : call.name;
                if (response.content.empty()) {
                    std::cout << "[agent] " << describe_tool_action(call) << '\n';
                }
                std::cout << "[tool: " << toolName << "]\n";
                json toolResult;
                if (tool_calls_this_turn >= max_tool_calls_per_turn) {
                    toolResult = {
                        {"success", false},
                        {"error", "Per-request tool-call limit reached; ask the user before continuing"}
                    };
                    tool_limit_reached = true;
                } else {
                    ++tool_calls_this_turn;
                    if (!call.id_valid || !call.arguments_valid || call.name.empty()) {
                        toolResult = {
                            {"success", false},
                            {"error", "Invalid structured tool call: missing ID/name or object arguments"}
                        };
                    } else {
                        try {
                            toolResult = registry_.execute(call.name, call.arguments);
                        } catch (const std::exception& error) {
                            toolResult = {{"success", false}, {"error", error.what()}};
                        }
                    }
                }
                if (is_task_state_tool(toolName)) {
                    std::cout << "[Task state]\n"
                              << TaskState::format_tool_result(toolName, toolResult)
                              << "\n";
                }
                conversation_.add_tool_result(call.id, toolName, toolResult.dump());
            }
            save_chat();
            if (tool_limit_reached || tool_calls_this_turn >= max_tool_calls_per_turn) {
                std::cout << "[tool-call limit reached; send a follow-up to continue]\n";
                break;
            }
            if (response.usage.available && conversation_.applyCompaction()) {
                std::cout << "[conversation compacted]\n";
            }
        }
        if (!received_final_response && tool_calls_this_turn < max_tool_calls_per_turn) {
            std::cout << "[agent paused after the tool-step limit; send a follow-up to continue]\n";
        }
    }
    return false;
}
