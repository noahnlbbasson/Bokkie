//
// Created by noahn on 2026/11/13.
//

#ifndef BOKKIE_CONVERSATION_H
#define BOKKIE_CONVERSATION_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../ToolRegistry.h"

class Conversation {
public:
    void set_max_context_tokens(int max_context_tokens);
    void record_token_usage(int prompt_tokens, int completion_tokens);

    int max_context_tokens() const;
    std::int64_t context_tokens_used() const;
    std::int64_t context_tokens_left() const;
    bool has_token_usage() const;

    void add_user(const std::string& message);
    void add_assistant(const std::string& message, const std::vector<ToolCall>& tool_calls);
    void add_tool_result(const std::string& id, const std::string& name,
                         const std::string& result);
    void load_messages(const std::vector<nlohmann::json>& messages);

    const std::vector<nlohmann::json>& get_messages() const;
    bool applyCompaction();

private:
    int max_context_tokens_ = 0;
    std::int64_t context_tokens_used_ = 0;
    std::int64_t context_tokens_left_ = 0;
    bool has_token_usage_ = false;
    std::vector<nlohmann::json> messages_;
    std::vector<size_t> turn_starts_;
};


#endif //BOKKIE_CONVERSATION_H
