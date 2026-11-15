//
// Created by noahn on 2026/11/09.
//

#ifndef BOKKIE_TOOLS_H
#define BOKKIE_TOOLS_H

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

class Tools {
public:
    static void set_workspace(const std::string& workspace);
    static json read_file(const json& arguments);
    static json write_file(const json& arguments);
    static json edit_file(const json& arguments);
    static json append_file(const json& arguments);
    static json delete_file(const json& arguments);
    static json list_files(const json& arguments);
    static json list_directories(const json& arguments);
    static json list_directory_contents(const json& arguments);
    static json move_path(const json& arguments);
    static json copy_path(const json& arguments);
    static json create_directory(const json& arguments);
    static json delete_directory(const json& arguments);
    static json get_path_info(const json& arguments);
    static json run_shell(const json& arguments);
    static json run_python(const json& arguments);

};


#endif //BOKKIE_TOOLS_H
