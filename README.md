# Bokkie 🦌

A command-line AI agent written in C++ that talks to a local LLM server. It streams responses in real time, keeps track of conversation history, and can call tools to interact with the environment.

## What it does

- Connects to a local LLM server using an OpenAI-compatible API (default: `localhost:8080`).
- Streams responses token by token using Server-Sent Events.
- Calls tools using OpenAI-compatible structured function calls for workspace-scoped operations.
- Shows brief action narration before tool execution and caps each user request at ten tool calls.
- Remembers the conversation context across turns in a session.
- Compacts older context under pressure while keeping the current request and latest complete tool exchange intact.
- Tracks persistent multi-step work with task instructions, progress notes, outcomes, and statuses.

## Dependencies

- C++17
- libcurl
- nlohmann/json
- CMake 3.16+

## Building

```bash
cmake -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
```

Or just open the project in CLion and use the pre-configured build profile.

## Usage

1. Make sure you have a local LLM server running with an OpenAI-compatible endpoint at `http://localhost:8080/v1/chat/completions`.
2. Use a model whose llama.cpp chat template supports tools, then build and run the project.
3. Type your prompts at the `>` prompt.
4. Type `:q` to quit.

Tool definitions are sent through the standard `tools` field, and tool results are returned in
subsequent requests with their matching call IDs. llama.cpp applies the loaded model's chat
template to represent these calls in that model's format. There is no universal model-specific
tool schema to retrieve: the API schema is standardized, but support depends on the active
chat template. A model/template that does not support tools may reject or fail to emit calls;
Bokkie does not fall back to parsing tool-call text from ordinary responses.

The context counter shows token usage for the latest model request, not the sum for the session.
Conversation history is compacted when that request reaches 70% of the configured context size.

## Project Structure

- `main.cpp` — Entry point; the CLI loop and structured tool dispatch.
- `Client.h` / `Client.cpp` — HTTP client that sends chat messages and structured tool definitions and parses streamed responses.
- `ToolRegistry.h` / `ToolRegistry.cpp` — System for registering and executing tools.
- `Tools.h` / `Tools.cpp` — Workspace-scoped file and directory operations and project search.
- `brain/TaskState.*` — Persistent agent task state stored in `brain/task_state.json`.
- `CMakeLists.txt` — Build configuration.

The agent can create, update, list, add progress to, and complete or fail persistent tasks.
Task tool results are printed as readable task summaries in the terminal, not raw JSON, so
users can see task statuses, progress, and outcomes.
Active tasks and a short history of recently completed or failed tasks are included in the
agent context at startup. Task instructions are for continuing work; they do not override
the current user request or the agent's safety constraints.

## Adding Tools

Tools are registered in `main.cpp`. To add a new one:

1. Write the handler in `Tools.cpp` (static method that takes and returns `json`).
2. Declare it in `Tools.h`.
3. Register it in `main.cpp` with `registry.register_tool()`.

The registered tools include `read_file`, `write_file`, `edit_file`, `append_file`,
`delete_file`, `list_files`, `list_directories`, `list_directory_contents`,
`move_path`, `copy_path`, `create_directory`, `delete_directory`,
and `get_path_info`. Paths are resolved against `harness.workspace` configured
in `config.json`; absolute paths are accepted only when they remain inside that
workspace. Existing symlinks and junctions are resolved before the containment
check, including for destination paths whose final component does not exist.
Successful path operations return the resolved path. The Python and terminal
execution tools are currently disabled. `delete_directory` recursively removes
the requested directory but refuses to delete the workspace root.

These checks restrict paths used by Bokkie's file tools; they are not an
OS-enforced sandbox and do not protect against filesystem changes racing a tool
operation. Do not re-enable arbitrary command or code execution without adding an
OS-level sandbox.

## Configuration

The server URL is hardcoded in `Client.cpp`. Change the `CURLOPT_URL` line if you're running your LLM on a different port or host.

## License

MIT — see [LICENSE](LICENSE) for details.
