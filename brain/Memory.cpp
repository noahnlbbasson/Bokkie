//
// Created by noahn on 2026/11/13.
// Persistent memory for the agent, stores the agent's memories and provides access to them more leaning towards,
// towards the agent's long-term memory about the user, who the user is what they like,
// how they want to be interacted with, what they want to avoid and pursue in general not code specific, this build a
// representation of the user's preferences and personality traits that the agent can use to guide its interactions with the user.
//

#include "Memory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <cwchar>
#endif

using json = nlohmann::json;

namespace {
    std::string memory_path = "brain/memory.md";

    std::string required_string(const json& arguments, const char* name) {
        const auto value = arguments.find(name);
        if (value == arguments.end() || !value->is_string()) {
            throw std::invalid_argument(std::string("Missing or invalid string argument: ") + name);
        }
        return value->get<std::string>();
    }

    json memory_error(const std::string& message) {
        return {{"success", false}, {"error", message}};
    }

    bool read_memory(std::string& content) {
        std::error_code error;
        if (!std::filesystem::exists(memory_path, error)) {
            if (error) throw std::runtime_error("Could not inspect memory file: " + error.message());
            return false;
        }

        std::ifstream file(memory_path, std::ios::binary);
        if (!file) throw std::runtime_error("Could not open memory file for reading");
        std::stringstream buffer;
        buffer << file.rdbuf();
        if (file.bad()) throw std::runtime_error("Could not read memory file");
        content = buffer.str();
        return true;
    }

    json write_memory(const std::string& content) {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(memory_path).parent_path(), error);
        if (error) return memory_error("Could not create memory directory: " + error.message());

        std::ofstream file(memory_path, std::ios::binary | std::ios::trunc);
        if (!file) return memory_error("Could not open memory file for writing");
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!file) return memory_error("Could not write memory file");
        return {{"success", true}, {"path", memory_path}};
    }

    size_t receive_response(char* contents, size_t size, size_t count, void* user_data) {
        const size_t bytes = size * count;
        auto* response = static_cast<std::string*>(user_data);
        response->append(contents, bytes);
        return bytes;
    }

    std::string _summarize_memory() {
        std::ifstream memory_file(memory_path);
        if (!memory_file) {
            throw std::runtime_error("Could not open memory file: " + memory_path);
        }

        std::stringstream memory_stream;
        memory_stream << memory_file.rdbuf();
        if (memory_file.bad()) {
            throw std::runtime_error("Could not read memory file: " + memory_path);
        }
        std::string memory_content = memory_stream.str();
        std::error_code file_size_error;
        const uintmax_t memory_size = std::filesystem::file_size(memory_path, file_size_error);
        if (file_size_error) {
            throw std::runtime_error("Could not get memory file size: " + file_size_error.message());
        }
        if (memory_size <= 10000) return memory_content;

        const json payload = {
            {"messages", json::array({
                {{"role", "system"}, {"content", "Summarize the following text while preserving important information."}},
                {{"role", "user"}, {"content", memory_content}}
            })},
            {"temperature", 0.7},
            {"stream", false}
        };
        const std::string request_body = payload.dump();

        const CURLcode global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (global_result != CURLE_OK) {
            throw std::runtime_error(std::string("Could not initialize curl: ") +
                                     curl_easy_strerror(global_result));
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            curl_global_cleanup();
            throw std::runtime_error("Could not create curl request");
        }

        std::string response_body;
        curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        if (!headers) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Could not allocate HTTP headers");
        }

        curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:8080/v1/chat/completions");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

        const CURLcode request_result = curl_easy_perform(curl);
        long status_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        curl_global_cleanup();

        if (request_result != CURLE_OK) {
            throw std::runtime_error(std::string("Memory summary request failed: ") +
                                     curl_easy_strerror(request_result));
        }
        if (status_code < 200 || status_code >= 300) {
            throw std::runtime_error("Memory summary server returned HTTP " +
                                     std::to_string(status_code) + ": " + response_body);
        }

        const json response = json::parse(response_body);
        if (response.contains("error")) {
            throw std::runtime_error("Memory summary server error: " + response["error"].dump());
        }
        if (!response.contains("choices") || !response["choices"].is_array() ||
            response["choices"].empty() ||
            !response["choices"][0].contains("message") ||
            !response["choices"][0]["message"].contains("content") ||
            !response["choices"][0]["message"]["content"].is_string()) {
            throw std::runtime_error("Memory summary response did not contain message content");
        }
        return response["choices"][0]["message"]["content"].get<std::string>();
    }

}


// these three tools are used by the agent actively to manage memory,
// description of what this memory is, is at the beginning of this file
json Memory::add_memory(const json& arguments) {
    const std::string memory = required_string(arguments, "memory");
    if (memory.empty()) return memory_error("memory must not be empty");

    std::string content;
    if (!read_memory(content)) content.clear();
    if (!content.empty() && content.back() != '\n') content += '\n';
    content += memory;
    if (content.back() != '\n') content += '\n';
    return write_memory(content);
}

json Memory::remove_memory(const json& arguments) {
    const std::string memory = required_string(arguments, "memory");
    if (memory.empty()) return memory_error("memory must not be empty");

    std::string content;
    if (!read_memory(content)) return memory_error("Memory file does not exist");
    const size_t position = content.find(memory);
    if (position == std::string::npos) return memory_error("Memory text was not found");
    content.erase(position, memory.size());
    return write_memory(content);
}

json Memory::edit_memory(const json& arguments) {
    const std::string old_memory = required_string(arguments, "old_memory");
    const std::string new_memory = required_string(arguments, "new_memory");
    if (old_memory.empty()) return memory_error("old_memory must not be empty");

    std::string content;
    if (!read_memory(content)) return memory_error("Memory file does not exist");
    const size_t position = content.find(old_memory);
    if (position == std::string::npos) return memory_error("old_memory text was not found");
    content.replace(position, old_memory.size(), new_memory);
    return write_memory(content);
}

std::string Memory::render_memory() {
    std::error_code error;
    if (!std::filesystem::exists(memory_path, error) && !error) return {};
    if (error) throw std::runtime_error("Could not inspect memory file: " + error.message());
    return _summarize_memory();
}