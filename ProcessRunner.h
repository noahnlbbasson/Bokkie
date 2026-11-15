#ifndef BOKKIE_PROCESSRUNNER_H
#define BOKKIE_PROCESSRUNNER_H

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct ProcessResult {
    bool started = false;
    int exit_code = -1;
    bool timed_out = false;
    bool output_truncated = false;
    std::string output;
    std::string error;
};

namespace ProcessRunner {
ProcessResult run(const std::string& executable,
                  const std::vector<std::string>& arguments,
                  const std::filesystem::path& working_directory,
                  int timeout_seconds,
                  size_t output_limit_bytes,
                  const std::function<void(const char*, size_t)>& on_output = {});
}

#endif
