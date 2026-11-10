//
// Created by noahn on 2026/11/09.
//

#include "ToolRegistry.h"

#include <iostream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

void ToolRegistry::register_tool(
    const std::string& name,
    const std::string& description,
    const json& parameters,
    Handler function
) {

    // Store the actual function
    tools[name] = function;

    // Store its description and parameters
    tool_definitions.push_back({
        {"name", name},
        {"description", description},
        {"parameters", parameters}
    });
}

// returns JSON of registered tools and their descriptions for system prompt for model
json ToolRegistry::list_registered_tools() {
    return tool_definitions;
}

json ToolRegistry::execute(
    const std::string& name,
    const json& arguments
) {

    auto it = tools.find(name);

    if (it == tools.end()) {
        return {
            {"success", false},
            {"error", "Tool not found"}
        };
    }

    return it->second(arguments);
}

