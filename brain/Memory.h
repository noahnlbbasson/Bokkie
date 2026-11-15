//
// Created by noahn on 2026/11/13.
//

#ifndef BOKKIE_MEMORY_H
#define BOKKIE_MEMORY_H

#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Memory {
public:
    static json add_memory(const json& arguments);
    static json remove_memory(const json& arguments);
    static json edit_memory(const json& arguments);
    static std::string render_memory();
};


#endif //BOKKIE_MEMORY_H
