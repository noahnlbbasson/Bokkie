#ifndef BOKKIE_TASKSTATE_H
#define BOKKIE_TASKSTATE_H

#include <string>

#include <nlohmann/json.hpp>

class TaskState {
public:
    static nlohmann::json create_task(const nlohmann::json& arguments);
    static nlohmann::json update_task(const nlohmann::json& arguments);
    static nlohmann::json record_progress(const nlohmann::json& arguments);
    static nlohmann::json set_status(const nlohmann::json& arguments);
    static nlohmann::json complete_task(const nlohmann::json& arguments);
    static nlohmann::json fail_task(const nlohmann::json& arguments);
    static nlohmann::json list_tasks(const nlohmann::json& arguments);
    static std::string render_task_state();
    static std::string format_tool_result(const std::string& tool_name,
                                          const nlohmann::json& result);
};

#endif
