//
// Created by noahn on 2026/11/09.
//

#include "Tools.h"

#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// reads file contents
json Tools::read_file(const json& arguments) {
    std::string path = arguments.at("path");
    std::ifstream file(path);
    if (!file.is_open()) {
        return {
            {"success", false},
            {"error", "Could not open file"}
        };
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return {
        {"success", true},
        {"content", buffer.str()}
    };
}




/*      IGNORE THIS
 *
// edits existing file contents
std::string edit_file(const std::string& file_path, const std::string& content) {
    std::string file_contents;

    return file_contents;
}

// creates file to path
std::string create_file(const std::string& file_path, const std::string& content) {
    std::string file_contents;

    return file_contents;
}

// deletes respective file to path
std::string delete_file(const std::string& file_path) {
    std::string file_contents;

    return file_contents;
}

// writes file to path
std::string write_file(const std::string& file_path, const std::string& content) {
    std::string file_contents;

    return file_contents;
}

// appends file to path
std::string append_file(const std::string& file_path, const std::string& content) {
    std::string file_contents;

    return file_contents;
}

// lists files in directory
std::string list_files(const std::string& directory_path) {
    std::string file_contents;

    return file_contents;
}

// lists directories in directory
std::string list_directories(const std::string& directory_path) {
    std::string file_contents;

    return file_contents;
}

// project scope, lists files and directories in directory
std::string list_directory_contents(const std::string& directory_path) {
    std::string file_contents;

    return file_contents;
}
*/

