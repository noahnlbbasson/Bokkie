#include "Client.h"
#include "ToolRegistry.h"
#include "Tools.h"

#include <iostream>
#include <string>
#include <vector>

// Function to join a string array into one string
std::string joinStringArray(const std::string arr[], size_t size, const std::string &separator = " ") {
    std::string result;
    for (size_t i = 0; i < size; ++i) {
        result += arr[i]; // Append element
        if (i < size - 1) {
            result += separator; // Add separator except after last element
        }
    }
    return result;
}

bool executeToolCall(
    const std::string& response,
    ToolRegistry& registry,
    std::string& toolName,
    json& toolResult
) {
    // Parse the model's response
    json parsed = json::parse(response, nullptr, false);
    // Not valid JSON
    if (parsed.is_discarded() || !parsed.is_object()) {
        return false;
    }
    // Check whether the response contains a tool call
    auto toolCall = parsed.find("tool_call");
    if (toolCall == parsed.end()) {
        return false;
    }
    // Validate the tool call structure
    if (!toolCall->is_object() ||
        !toolCall->contains("name") ||
        !toolCall->contains("arguments") ||
        !(*toolCall)["name"].is_string() ||
        !(*toolCall)["arguments"].is_object()) {

        toolName = "invalid tool call";

        toolResult = {
            {"success", false},
            {"error", "Invalid tool call format"}
        };

        return true;
        }
    // Extract the tool name and arguments
    toolName = (*toolCall)["name"].get<std::string>();
    const json arguments = (*toolCall)["arguments"];
    // Execute the requested tool
    try {
        toolResult = registry.execute(toolName, arguments);
    }
    catch (const std::exception& e) {
        toolResult = {
            {"success", false},
            {"error", e.what()}
        };
    }
    return true;
}



int main() {
    using json = nlohmann::json;
    ToolRegistry registry;
    // registering available tools
    registry.register_tool(
        "read_file",
        "Reads the contents of a specified file.",
        {
            {"type", "object"},
            {"properties", {
                {"path", {
                    {"type", "string"},
                    {"description", "Path of the file to read"}
                }}
            }},
            {"required", {"path"}}
        },

        Tools::read_file
    );

    std::vector<std::string> conversation;


    while (true) {
        std::string prompt;
        if (conversation.data()) {
            prompt += "Conversation History:\n";
            prompt += joinStringArray(conversation.data(), conversation.size(), "\n");
        }
        std::cout << ">" << std::flush;
        std::string user_input;
        if (!std::getline(std::cin, user_input)) {
            break;
        }
        if (user_input == ":q") {
            break;
        }
        conversation.push_back("User: " + user_input);
        prompt += user_input + "\n";
        std::string response = Client::send_payload(prompt, registry);
        conversation.push_back("Assistant: " + response);
        std::cout << '\n';
    }
    return 0;
}
