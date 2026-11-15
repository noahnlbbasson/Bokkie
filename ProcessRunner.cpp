#include "ProcessRunner.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
#ifdef _WIN32
std::wstring widen_utf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("Process argument is not valid UTF-8");
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring quote_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += L'"';
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted += character;
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

void append_output(ProcessResult& result, const char* data, size_t length,
                   size_t output_limit_bytes,
                   const std::function<void(const char*, size_t)>& on_output) {
    if (on_output) on_output(data, length);
    const size_t remaining = result.output.size() < output_limit_bytes
        ? output_limit_bytes - result.output.size() : 0;
    const size_t to_append = std::min(remaining, length);
    result.output.append(data, to_append);
    result.output_truncated = result.output_truncated || to_append < length;
}
#else
void append_output(ProcessResult& result, const char* data, size_t length,
                   size_t output_limit_bytes,
                   const std::function<void(const char*, size_t)>& on_output) {
    if (on_output) on_output(data, length);
    const size_t remaining = result.output.size() < output_limit_bytes
        ? output_limit_bytes - result.output.size() : 0;
    const size_t to_append = std::min(remaining, length);
    result.output.append(data, to_append);
    result.output_truncated = result.output_truncated || to_append < length;
}
#endif
}

ProcessResult ProcessRunner::run(const std::string& executable,
                                 const std::vector<std::string>& arguments,
                                 const std::filesystem::path& working_directory,
                                 int timeout_seconds,
                                 size_t output_limit_bytes,
                                 const std::function<void(const char*, size_t)>& on_output) {
    if (executable.empty()) {
        throw std::invalid_argument("Executable must not be empty");
    }
    if (timeout_seconds <= 0) {
        throw std::invalid_argument("Process timeout must be positive");
    }

    ProcessResult result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        result.error = "Could not create process output pipe (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result.error = "Could not open null input (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    startup.hStdInput = null_input;

    std::wstring command_line = quote_argument(widen_utf8(executable));
    for (const auto& argument : arguments) {
        command_line += L" ";
        command_line += quote_argument(widen_utf8(argument));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    const std::wstring directory = working_directory.wstring();

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        CloseHandle(null_input);
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        result.error = "Could not create process job (Windows error " +
                       std::to_string(GetLastError()) + ")";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        result.error = "Could not configure process job (Windows error " +
                       std::to_string(GetLastError()) + ")";
        CloseHandle(job);
        CloseHandle(null_input);
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);
        return result;
    }

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, directory.c_str(),
        &startup, &process);
    CloseHandle(write_pipe);
    CloseHandle(null_input);
    if (!created) {
        result.error = "Could not start process (Windows error " +
                       std::to_string(GetLastError()) + ")";
        CloseHandle(job);
        CloseHandle(read_pipe);
        return result;
    }
    result.started = true;
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        result.error = "Could not assign process to job (Windows error " +
                       std::to_string(GetLastError()) + ")";
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, INFINITE);
        result.exit_code = 1;
    } else {
        ResumeThread(process.hThread);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(timeout_seconds);
        bool process_finished = false;
        char buffer[4096];
        while (!process_finished) {
            DWORD available = 0;
            if (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) &&
                available > 0) {
                DWORD bytes_read = 0;
                const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
                if (ReadFile(read_pipe, buffer, requested, &bytes_read, nullptr) && bytes_read > 0) {
                    append_output(result, buffer, bytes_read, output_limit_bytes, on_output);
                }
            }

            const DWORD wait_result = WaitForSingleObject(process.hProcess, 20);
            if (wait_result == WAIT_OBJECT_0) {
                process_finished = true;
            } else if (wait_result == WAIT_FAILED) {
                result.error = "Could not wait for process (Windows error " +
                               std::to_string(GetLastError()) + ")";
                TerminateJobObject(job, 1);
                process_finished = true;
            } else if (std::chrono::steady_clock::now() >= deadline) {
                result.timed_out = true;
                TerminateJobObject(job, 124);
                WaitForSingleObject(process.hProcess, INFINITE);
                process_finished = true;
            }
        }

        DWORD available = 0;
        while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) &&
               available > 0) {
            DWORD bytes_read = 0;
            const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
            if (!ReadFile(read_pipe, buffer, requested, &bytes_read, nullptr) || bytes_read == 0) {
                break;
            }
            append_output(result, buffer, bytes_read, output_limit_bytes, on_output);
        }
        DWORD exit_code = 1;
        if (GetExitCodeProcess(process.hProcess, &exit_code)) {
            result.exit_code = static_cast<int>(exit_code);
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(job);
    CloseHandle(read_pipe);
#else
    int output_pipe[2];
    if (pipe(output_pipe) != 0) {
        result.error = std::string("Could not create process output pipe: ") +
                       std::strerror(errno);
        return result;
    }
    const pid_t child = fork();
    if (child < 0) {
        result.error = std::string("Could not fork process: ") + std::strerror(errno);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return result;
    }
    if (child == 0) {
        setpgid(0, 0);
        dup2(output_pipe[1], STDOUT_FILENO);
        dup2(output_pipe[1], STDERR_FILENO);
        close(output_pipe[0]);
        close(output_pipe[1]);
        const int null_input = open("/dev/null", O_RDONLY);
        if (null_input >= 0) {
            dup2(null_input, STDIN_FILENO);
            close(null_input);
        }
        if (chdir(working_directory.c_str()) != 0) _exit(126);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 2);
        argv.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }

    result.started = true;
    setpgid(child, child);
    close(output_pipe[1]);
    const int flags = fcntl(output_pipe[0], F_GETFL, 0);
    fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(timeout_seconds);
    bool process_finished = false;
    int status = 0;
    char buffer[4096];
    while (!process_finished) {
        const ssize_t bytes_read = read(output_pipe[0], buffer, sizeof(buffer));
        if (bytes_read > 0) append_output(result, buffer, static_cast<size_t>(bytes_read),
                                          output_limit_bytes, on_output);
        const pid_t wait_result = waitpid(child, &status, WNOHANG);
        if (wait_result == child) {
            process_finished = true;
        } else if (wait_result < 0 && errno != EINTR) {
            result.error = std::string("Could not wait for process: ") + std::strerror(errno);
            kill(-child, SIGKILL);
            process_finished = true;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill(-child, SIGKILL);
            waitpid(child, &status, 0);
            process_finished = true;
        } else if (bytes_read <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    while (true) {
        const ssize_t bytes_read = read(output_pipe[0], buffer, sizeof(buffer));
        if (bytes_read <= 0) break;
        append_output(result, buffer, static_cast<size_t>(bytes_read), output_limit_bytes, on_output);
    }
    close(output_pipe[0]);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
#endif
    return result;
}
