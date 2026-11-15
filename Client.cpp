#include "Client.h"
#include "ToolRegistry.h"
#include "Tools.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include <map>
#include <vector>
#include <cstdlib>

namespace {
using json = nlohmann::json;


struct ResponseContext {
    std::function<void(const std::string&)> onToken;
    std::string body;
    std::string pendingLine;
    std::string completion;
    std::string error;
    TokenUsage usage;
    std::map<size_t, ToolCall> toolCalls;
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

    const auto usage = event.find("usage");
    if (usage != event.end() && usage->is_object()) {
        const auto promptTokens = usage->find("prompt_tokens");
        const auto completionTokens = usage->find("completion_tokens");
        if (promptTokens != usage->end() && promptTokens->is_number_integer() &&
            completionTokens != usage->end() && completionTokens->is_number_integer()) {
            context.usage.prompt_tokens = promptTokens->get<int>();
            context.usage.completion_tokens = completionTokens->get<int>();
            context.usage.available =
                context.usage.prompt_tokens >= 0 && context.usage.completion_tokens >= 0;
        }
    }

    const auto choices = event.find("choices");
    if (choices == event.end() || !choices->is_array() || choices->empty()) {
        return true;
    }

    const auto delta = choices->at(0).find("delta");
    if (delta == choices->at(0).end() || !delta->is_object()) {
        return true;
    }

    const auto toolCalls = delta->find("tool_calls");
    if (toolCalls != delta->end() && toolCalls->is_array()) {
        for (const auto& partial : *toolCalls) {
            if (!partial.is_object() || !partial.contains("index") ||
                (!partial["index"].is_number_unsigned() &&
                 !partial["index"].is_number_integer())) {
                context.error = "Received a malformed tool call in streaming response.";
                return false;
            }

            const auto index_value = partial["index"].get<std::int64_t>();
            if (index_value < 0) {
                context.error = "Received a negative tool-call index in streaming response.";
                return false;
            }
            ToolCall& call = context.toolCalls[static_cast<size_t>(index_value)];
            if (partial.contains("id") && partial["id"].is_string()) {
                call.id += partial["id"].get<std::string>();
                call.id_valid = true;
            }

            const auto function = partial.find("function");
            if (function == partial.end() || !function->is_object()) {
                continue;
            }
            const auto name = function->find("name");
            if (name != function->end() && name->is_string()) {
                call.name += name->get<std::string>();
            }
            const auto arguments = function->find("arguments");
            if (arguments != function->end() && arguments->is_string()) {
                call.raw_arguments += arguments->get<std::string>();
            }
        }
    }

    const auto content = delta->find("content");
    if (content != delta->end() && content->is_string()) {
        const std::string token = content->get<std::string>();
        context.completion += token;
        if (context.onToken) {
            context.onToken(token);
        }
    }

    return true;
}

size_t WriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
    auto* context = static_cast<ResponseContext*>(userp);
    const size_t totalSize = size * nmemb;
    context->body.append(contents, totalSize);

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

size_t AppendBodyCallback(char* contents, size_t size, size_t nmemb, void* userp) {
    auto* body = static_cast<std::string*>(userp);
    const size_t totalSize = size * nmemb;
    body->append(contents, totalSize);
    return totalSize;
}
}

ChatResponse Client::send_payload(const std::vector<json>& messages, std::string system_prompt,
                                  ToolRegistry& registry,
                                  const std::function<void(const std::string&)>& on_token,
                                  bool debug) {
    json apiMessages = json::array({
        {{"role", "system"}, {"content", system_prompt}}
    });
    for (const auto& message : messages) {
        apiMessages.push_back(message);
    }

    const char* temperature_value = std::getenv("BOKKIE_TEMPERATURE");
    const double temperature = temperature_value ? std::stod(temperature_value) : 0.7;
    json payload = {
        {"messages", std::move(apiMessages)},
        {"tools", registry.list_registered_tools()},
        {"tool_choice", "auto"},
        {"temperature", temperature},
        {"stream", true},
        {"stream_options", {{"include_usage", true}}}
    };
    if (const char* seed_value = std::getenv("BOKKIE_SEED")) {
        payload["seed"] = std::stoi(seed_value);
    }
    const std::string requestBody = payload.dump();

    ChatResponse chatResponse;
    const CURLcode globalResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (globalResult != CURLE_OK) {
        chatResponse.error = "Failed to initialize curl: " +
                             std::string(curl_easy_strerror(globalResult));
        std::cerr << chatResponse.error << '\n';
        return chatResponse;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        chatResponse.error = "Failed to create curl request.";
        std::cerr << chatResponse.error << '\n';
        curl_global_cleanup();
        return chatResponse;
    }

    ResponseContext response;
    response.onToken = on_token;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(
        headers,
        "Accept: text/event-stream"
    );

    if (!headers) {
        chatResponse.error = "Failed to allocate HTTP headers.";
        std::cerr << chatResponse.error << '\n';
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return chatResponse;
    }

    std::string server_url = "http://localhost:8080";
    if (const char* configured_server = std::getenv("BOKKIE_SERVER_URL")) {
        server_url = configured_server;
    }
    if (!server_url.empty() && server_url.back() == '/') server_url.pop_back();
    const std::string completion_url = server_url + "/v1/chat/completions";
    curl_easy_setopt(curl, CURLOPT_URL, completion_url.c_str());
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
            chatResponse.error = response.error;
        } else {
            chatResponse.error = "Request failed: " + std::string(curl_easy_strerror(result));
        }
        std::cerr << chatResponse.error << '\n';
    } else if (statusCode < 200 || statusCode >= 300) {
        chatResponse.error = "HTTP request failed (" + std::to_string(statusCode) +
                             "): " + response.body;
        std::cerr << chatResponse.error << '\n';
    } else {
        if (!response.pendingLine.empty()) {
            if (!ProcessSseLine(response.pendingLine, response) && !response.error.empty()) {
                chatResponse.error = response.error;
                std::cerr << chatResponse.error << '\n';
            }
        }
        if (!response.error.empty()) chatResponse.error = response.error;
    }

    if (debug && !response.body.empty()) {
        std::cerr << "\nRaw response:\n" << response.body << '\n';
    }

    if (result == CURLE_OK && statusCode >= 200 && statusCode < 300 &&
        response.error.empty()) {
        chatResponse.content = std::move(response.completion);
        chatResponse.usage = response.usage;
        for (auto& [index, call] : response.toolCalls) {
            if (!call.id_valid) {
                call.id = "bokkie_invalid_tool_call_" + std::to_string(index);
            }
            call.original_raw_arguments = call.raw_arguments;
            const json arguments = json::parse(call.raw_arguments, nullptr, false);
            if (!arguments.is_discarded() && arguments.is_object()) {
                call.arguments = arguments;
                call.arguments_valid = true;
            } else {
                call.raw_arguments = "{}";
            }
            chatResponse.tool_calls.push_back(std::move(call));
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return chatResponse;
}

int Client::get_max_context_tokens() {
    const CURLcode globalResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (globalResult != CURLE_OK) {
        throw std::runtime_error(
            "Failed to initialize curl: " + std::string(curl_easy_strerror(globalResult)));
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        throw std::runtime_error("Failed to create llama.cpp metadata request.");
    }

    std::string body;
    std::string server_url = "http://localhost:8080";
    if (const char* configured_server = std::getenv("BOKKIE_SERVER_URL")) {
        server_url = configured_server;
    }
    if (!server_url.empty() && server_url.back() == '/') server_url.pop_back();
    const std::string props_url = server_url + "/props";
    curl_easy_setopt(curl, CURLOPT_URL, props_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, AppendBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode result = curl_easy_perform(curl);
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);

    curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (result != CURLE_OK) {
        throw std::runtime_error(
            "Failed to read llama.cpp context size: " + std::string(curl_easy_strerror(result)));
    }
    if (statusCode < 200 || statusCode >= 300) {
        throw std::runtime_error(
            "llama.cpp /props request failed (" + std::to_string(statusCode) + "): " + body);
    }

    const json response = json::parse(body, nullptr, false);
    if (response.is_discarded() ||
        !response.contains("default_generation_settings") ||
        !response["default_generation_settings"].is_object() ||
        !response["default_generation_settings"].contains("n_ctx") ||
        !response["default_generation_settings"]["n_ctx"].is_number_integer()) {
        throw std::runtime_error("llama.cpp /props response does not contain a valid n_ctx.");
    }

    const int contextTokens =
        response["default_generation_settings"]["n_ctx"].get<int>();
    if (contextTokens <= 0) {
        throw std::runtime_error("llama.cpp reported a non-positive context size.");
    }
    return contextTokens;
}
