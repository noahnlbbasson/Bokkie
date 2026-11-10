//
// Created by noahn on 2026/11/09.
//

#ifndef BOKKIE_TOOLREGISTRY_H
#define BOKKIE_TOOLREGISTRY_H

#include <string>
#include <functional>
#include <unordered_map>

#include <nlohmann/json.hpp>

using json = nlohmann::json;



class ToolRegistry {

private:
    using Handler = std::function<
        json(const json&)
    >;

    std::unordered_map<std::string, Handler> tools;

    json tool_definitions = json::array();

public:

    void register_tool(
        const std::string& name,
        const std::string& description,
        const json& parameters,
        Handler function
    );

    json list_registered_tools();

    json execute(
        const std::string& name,
        const json& arguments
    );

};

#endif //BOKKIE_TOOLREGISTRY_H
