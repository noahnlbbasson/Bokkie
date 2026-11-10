#include "Client.h"
#include "ToolRegistry.h"
#include "Tools.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>



namespace {
using json = nlohmann::json;


struct ResponseContext {
    bool stream;
    std::string body;
    std::string pendingLine;
    std::string completion;
    std::string error;
};

bool ProcessSseLine(const std::string& line, ResponseContext& context) {
    if (line.rfind("data:", 0) != 0) {
        return true;
    }

    std::string data = line.substr(5);
    const size_t firstContent = data.find_first_not_of(" \t");
    if (firstContent == std::string::npos) {
        return true;
    }
    data.erase(0, firstContent);

    if (data == "[DONE]") {
        return true;
    }

    const json event = json::parse(data, nullptr, false);
    if (event.is_discarded()) {
        context.error = "Received invalid JSON in streaming response.";
        return false;
    }

    const auto error = event.find("error");
    if (error != event.end()) {
        context.error = "Server error: " + error->dump();
        return false;
    }

    const auto choices = event.find("choices");
    if (choices == event.end() || !choices->is_array() || choices->empty()) {
        return true;
    }

    const auto delta = choices->at(0).find("delta");
    if (delta == choices->at(0).end() || !delta->is_object()) {
        return true;
    }

    const auto content = delta->find("content");
    if (content != delta->end() && content->is_string()) {
        const std::string token = content->get<std::string>();
        context.completion += token;
        std::cout << token << std::flush;
    }

    return true;
}

size_t WriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
    auto* context = static_cast<ResponseContext*>(userp);
    const size_t totalSize = size * nmemb;
    context->body.append(contents, totalSize);

    if (!context->stream) {
        return totalSize;
    }

    context->pendingLine.append(contents, totalSize);
    size_t newline = context->pendingLine.find('\n');
    while (newline != std::string::npos) {
        std::string line = context->pendingLine.substr(0, newline);
        context->pendingLine.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!ProcessSseLine(line, *context)) {
            return 0;
        }
        newline = context->pendingLine.find('\n');
    }

    return totalSize;
}
}

std::ostream& Client::send_payload(const std::string& prompt, ToolRegistry& registry, bool debug, bool stream) {

    std::string system_prompt = R"(
    "You are Bokkie, an autonomous AI agent operating through a local C++ execution environment.

    You have access to a collection of tool/s provided by the application.

    TOOL USAGE:
    - You must inspect the available tool definitions before deciding how to interact with the environment.
    - Each tool/s has a specific name, description and set of required arguments.
    - When a task requires an available tool, call the appropriate tool rather than pretending to have performed the operation.
    - Always provide arguments in the exact format specified by the tool definition.
    - You may call multiple tools when a task requires multiple operations.
    - Wait for the execution result before deciding what to do next.
    - Use tool results to inform subsequent decisions.
    - Never fabricate tool execution results.

    FILE OPERATIONS:
    - Always inspect relevant files before modifying existing code.
    - Use paths within the permitted workspace.
    - Report errors accurately.

    AGENT INSTRUCTIONS:
    - You may create and update your own persistent agent instructions using the provided instruction-management tools.
    - Changes to persistent instructions should reflect improvements to your behaviour, working methods and preferences.
    - Never overwrite the application's protected core instructions.
    - Only modify persistent instructions when explicitly requested or when the application authorizes the change.
    - After updating persistent instructions, use the updated instructions in subsequent conversations.

    EXECUTION:
    You are responsible for planning tasks, selecting appropriate tools, interpreting their results and deciding what to do next.
    A tool call is a request for the application to perform an operation. It is not evidence that the operation has succeeded.
    Always verify the result before reporting completion.

    TOOL CALL FORMAT:
    When you need to execute a tool, respond with ONLY a valid JSON object
    using the following structure:
    {
        "tool_call": {
            "name": "tool_name",
            "arguments": {
                "argument_name": "argument_value"
            }
        }
    }
    Rules:
    - The tool name must match an available tool exactly.
    - Arguments must match the tool's specified parameters.
    - Do not wrap tool calls in Markdown code blocks.
    - Do not include explanations alongside a tool call.
    - Only request one tool at a time.
    - When you receive a tool execution result, use it to continue the task.
    - When no tool is required, respond normally in natural language.

    AVAILABLE TOOL/s:

    )";

    std::string available_tools = registry.list_registered_tools().dump(2);
    system_prompt += available_tools;

    const json payload = {
        {"messages", json::array({
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"}, {"content", prompt}}
        })},
        {"temperature", 0.7},
        {"max_tokens", 512},
        {"stream", stream}
    };
    const std::string requestBody = payload.dump();

    const CURLcode globalResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (globalResult != CURLE_OK) {
        std::cerr << "Failed to initialize curl: " << curl_easy_strerror(globalResult) << '\n';
        return {};
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to create curl request.\n";
        curl_global_cleanup();
        return {};
    }

    ResponseContext response{stream};
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(
        headers,
        stream ? "Accept: text/event-stream" : "Accept: application/json"
    );

    if (!headers) {
        std::cerr << "Failed to allocate HTTP headers.\n";
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return {};
    }

    curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8080/v1/chat/completions");
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(requestBody.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    const CURLcode result = curl_easy_perform(curl);
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

    if (result != CURLE_OK) {
        if (!response.error.empty()) {
            std::cerr << response.error << '\n';
        } else {
            std::cerr << "Request failed: " << curl_easy_strerror(result) << '\n';
        }
    } else if (statusCode < 200 || statusCode >= 300) {
        std::cerr << "HTTP request failed (" << statusCode << "): "
                  << response.body << '\n';
    } else if (stream) {
        if (!response.pendingLine.empty()) {
            if (!ProcessSseLine(response.pendingLine, response) && !response.error.empty()) {
                std::cerr << response.error << '\n';
            }
        }
    } else {
        const json resultJson = json::parse(response.body, nullptr, false);
        if (resultJson.is_discarded()) {
            std::cerr << "Server returned invalid JSON: " << response.body << '\n';
        } else if (resultJson.contains("error")) {
            std::cerr << "Server error: " << resultJson["error"].dump() << '\n';
        } else if (resultJson.contains("choices")
                   && resultJson["choices"].is_array()
                   && !resultJson["choices"].empty()
                   && resultJson["choices"][0].contains("message")
                   && resultJson["choices"][0]["message"].contains("content")
                   && resultJson["choices"][0]["message"]["content"].is_string()) {
            response.completion = resultJson["choices"][0]["message"]["content"].get<std::string>();
            // this streams the output to terminal but does not return a streamed answer which is what I want instead of this
            // because it will allow me to implement parsing logic in main where I call the function, instead
            // of parsing within the function here mixing up the concerns, a function should only do one thing and
            // send payload should only and simply return the streamed output as if it were happening below so I
            // can connect this function call to the active parser in main as it returns the streamed output as it
            // actively returning the streamed output the parser is actively working with the streamed output in main.
            // Do you understand?
            std::cout << response.completion;
        } else {
            std::cerr << "Response did not contain choices[0].message.content: "
                      << response.body << '\n';
        }
    }

    if (debug && !response.body.empty()) {
        std::cerr << "\nRaw response:\n" << response.body << '\n';
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return response.completion;
}
