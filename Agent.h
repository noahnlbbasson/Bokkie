//
// Created by noahn on 2026/11/14.
//

#ifndef BOKKIE_AGENT_H
#define BOKKIE_AGENT_H

#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "ToolRegistry.h"
#include "brain/Conversation.h"

using json = nlohmann::json;

class Workspace;

struct ChatTarget {
    bool quick = true;
    std::string workspace;
    std::string chat;
};

class Agent {
public:
    // Binds tools, builds the system prompt and loads the saved chat if applicable.
    Agent(Workspace& workspaces, const ChatTarget& target);

    // Returns true for :b and false for :q or when input ends.
    bool run();

private:
    Workspace& workspaces_;
    ChatTarget target_;
    std::string workspace_;
    std::string system_prompt_;
    ToolRegistry registry_;
    Conversation conversation_;
    bool save_warned_ = false;

    void save_chat();
};

#endif //BOKKIE_AGENT_H
