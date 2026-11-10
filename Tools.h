//
// Created by noahn on 2026/11/09.
//

#ifndef BOKKIE_TOOLS_H
#define BOKKIE_TOOLS_H

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Tools {
    public:
        static json read_file(const json& arguments);

};


#endif //BOKKIE_TOOLS_H
