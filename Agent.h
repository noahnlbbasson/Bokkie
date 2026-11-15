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

class Agent {
public:
    // Loads config.json, sets the workspace, builds the system prompt,
    // registers the tools and sets the max context size.
    Agent();

    // The whole interactive loop: read input, call the model, run tools, repeat.
    // Returns when the user types :q or input ends.
    void run();

private:
    nlohmann::json config_;
    std::string workspace_;
    std::string system_prompt_;
    ToolRegistry registry_;
    Conversation conversation_;
};

#endif //BOKKIE_AGENT_H
