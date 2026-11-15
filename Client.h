#ifndef BOKKIE_CLIENT_H
#define BOKKIE_CLIENT_H

#include <string>
#include <vector>

#include "ToolRegistry.h"

#include <functional>

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    bool available = false;
};

struct ChatResponse {
    std::string content;
    std::vector<ToolCall> tool_calls;
    TokenUsage usage;
    std::string error;
};

class Client {
public:
    static ChatResponse send_payload(
        const std::vector<json>& messages,
        std::string system_prompt,
        ToolRegistry& registry,
        const std::function<void(const std::string&)>& on_token,
        bool debug = false);

    static int get_max_context_tokens();
};

#endif
