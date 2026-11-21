# Bokkie TODO


- [ ] when going into a recent chat, reprint the chat
- [ ] allow for the terminal to clear fully when entering different screens


- [ ] add interruptions, queuing and steering
  - **NOTE:**
    - Typing while the model is still answering: two or more lines typed during that time count as a paste, so you'll need one more Enter to send them.
    - Very long single lines: macOS terminals cap one line at about 1024 characters, so a longer single line in a paste may get cut off. Fixing that would mean rewriting how keyboard input is handled; say if you want it.
- [ ] there is something wrong with memory, agent assumes memory is in the workspace, and there might be issue with the tool itself, system prompt investigate aswell
- [ ] build retry logic for empty model response tokens (see [Empty Responses](#1-empty-responses) below)
- [ ] investigate and implement robust input handling
- [ ] Markdown renderer (md_render.h) - resume here
  - Pull terminal_width() / stdout_is_tty() into one shared header (renderer and landing screen both need them)
  - Test on Windows (PowerShell, cmd, Windows Terminal): colours, UTF-8 box drawing, links
  - Gate OSC 8 links by terminal (TERM_PROGRAM, WT_SESSION, KITTY_WINDOW_ID, VTE_VERSION) with a BOKKIE_LINKS=0/1 override
  - Check colours on a light terminal theme
  - Table cells: <br> and <ul> HTML prints literally; CJK/emoji widths; wrapped lists and quotes have no hanging indent
  - Not handled: strikethrough, task lists, images, underscore emphasis
  - Maybe later: larger heading text (OSC 66), expand the full reasoning with a /thinking command

- [ ] add multi agentic orchestration
- [ ] add tab auto complete in IDEs which connect to Bokkie terminal via plugin
- [ ] show changes agent made in terminal, such as code changes, commands and output, ecs
---

## Resolving Common LLM Failure Modes and implementing Production Fallbacks

- [ ] **Resolving Common LLM Failure Modes and implementing Production Fallbacks**

This review combines the original report with code-level corrections and additional findings. The code references below are approximate and refer to the current files.

Each section keeps the original **What happens / Production handling / Your code** text. Below that, **Fix (production pattern)** gives a code sketch written against the current Bokkie sources. **Verified / additional findings** notes anything confirmed or newly found while checking the code.

---

### 1. Empty Responses

- [ ] Empty Responses

**What happens:** The model returns no content and no tool calls, or the stream ends before a usable response is received.

**Production handling:**
- Detect an empty successful response separately from a transport or server error.
- Retry a small, bounded number of times when retrying is safe; use backoff for transient failures.
- Avoid a minimum-length rule that rejects valid short answers or tool-only responses.
- Change temperature or seed only if there is evidence it helps and the server supports the parameter.

**Your code:** `Client::send_payload` can return empty content with no tool calls. `Agent::run` treats that as a completed response and does not retry or display a specific empty-response warning (`Client.cpp:264-268`; `Agent.cpp:342-359`). A tool-only response with empty text is valid, so detection must also check that there are no tool calls.

**Verified / additional findings:**
- Confirmed. In addition, `ProcessSseLine` only reads `delta.content` (`Client.cpp:119-126`). llama.cpp streams the thinking output of reasoning models such as the configured Qwen3.5 profile in `delta.reasoning_content`. If the model only "thinks" and then stops (for example on `finish_reason: "length"`), Bokkie sees a completely empty response. Record `reasoning_content` length and `finish_reason` so an empty answer can be diagnosed.
- Only retry if **nothing was streamed to the terminal yet**; otherwise the user sees duplicated text.

**Fix (production pattern):**

```cpp
// Client.h — let the agent tell "empty" apart from "error" and know why the stream stopped
struct ChatResponse {
    std::string content;
    std::vector<ToolCall> tool_calls;
    TokenUsage usage;
    std::string error;
    std::string finish_reason;      // "stop", "length", "tool_calls", ... (empty = never received)
    bool received_done = false;     // saw "data: [DONE]"
    size_t reasoning_chars = 0;     // delta.reasoning_content seen but not shown
    bool streamed_any_token = false;
    std::string partial_content;    // text streamed before a failure; shown, never treated as an answer
};

inline bool is_blank(const std::string& text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline bool is_empty_success(const ChatResponse& r) {
    return r.error.empty() && r.tool_calls.empty() && is_blank(r.content);
}
```

```cpp
// Agent.cpp — Agent::run: bounded retry with backoff around the model call
// (needs <thread> and <chrono>)
constexpr int max_empty_retries = 2;
ChatResponse response;
for (int attempt = 0;; ++attempt) {
    MarkdownRenderer renderer;
    response = Client::send_payload(
        conversation_.get_messages(), system_prompt_, registry_,
        [&renderer](const std::string& token) { renderer.feed(token); });
    renderer.finish();

    if (!is_empty_success(response) || response.streamed_any_token ||
        attempt >= max_empty_retries) {
        break;
    }
    std::cerr << "[empty model response (finish_reason=" << response.finish_reason
              << ", reasoning chars=" << response.reasoning_chars << "); retrying "
              << attempt + 1 << '/' << max_empty_retries << "]\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(250 << attempt));  // 250ms, 500ms
}

if (is_empty_success(response)) {
    std::cout << "[model returned an empty response"
              << (response.finish_reason == "length" ? " (hit the token limit)" : "")
              << "; send a follow-up to retry]\n";
    break;  // do NOT add an empty assistant message to history
}
```

---

### 2. Tool-Calling Failures

- [ ] Tool-Calling Failures

**What happens:** A model returns malformed arguments, an unknown tool name, or arguments that do not match what the tool expects.

**Production handling:**
registered parameter schemas. Some handlers validate their own inputs, and unknown tool names return an error result (`Tools.cpp`; `ToolRegistry.cpp`). There is no automatic argument correction or text-only fallback. The README explicitly says Bokkie does not parse ordinary text as a tool call (`README.md:38-43`).

> The text above was already cut off in the original (it starts mid-sentence, and the staged copy has the same truncation). It is kept word for word. The bullets below rebuild the missing production-handling list and the "Your code" summary from the current sources:
> - Validate tool arguments against the registered JSON parameter schemas (required fields, types, enums, numeric bounds) before running a handler.
> - Return a structured, specific error to the model (which field is wrong, what was expected, the raw arguments it sent) so it can correct itself on the next step.
> - Return unknown-tool errors that list the available tool names.
> - Do not guess or "repair" arguments silently, and do not parse ordinary text as a tool call.
>
> **Your code (rebuilt):** `Client.cpp:268-281` marks unparsable or non-object arguments as invalid and replaces them with `{}`. `Agent.cpp:381-386` rejects them with a generic error that does not include the original raw arguments. `ToolRegistry::execute` does not check arguments against the registered parameter schemas.

**Verified / additional findings:**
- `ToolRegistry::register_tool` stores the schema only in `tool_definitions` (for the API), not for validation (`ToolRegistry.cpp:13-32`).
- `call.id += ...` (`Client.cpp:100`) concatenates the ID if a server repeats `id` in every chunk (some OpenAI-compatible servers do). Assign instead of appending when a non-empty ID arrives.

**Fix (production pattern):**

```cpp
// ToolRegistry.h — keep the schema next to the handler
std::unordered_map<std::string, json> schemas;

// ToolRegistry.cpp — register_tool(): also store the schema
schemas[name] = parameters;

// ToolRegistry.cpp — minimal validator for the subset of JSON Schema Bokkie uses
// (for full JSON Schema use pboettch/json-schema-validator, built on nlohmann::json)
namespace {
std::string validate_arguments(const json& schema, const json& args) {
    if (!args.is_object()) return "arguments must be a JSON object";
    for (const auto& field : schema.value("required", json::array())) {
        if (!args.contains(field.get<std::string>()))
            return "missing required argument '" + field.get<std::string>() + "'";
    }
    const json properties = schema.value("properties", json::object());
    for (const auto& [key, value] : args.items()) {
        const auto prop = properties.find(key);
        if (prop == properties.end()) return "unknown argument '" + key + "'";
        const std::string type = prop->value("type", "");
        if (type == "string" && !value.is_string()) return "'" + key + "' must be a string";
        if (type == "integer") {
            if (!value.is_number_integer()) return "'" + key + "' must be an integer";
            if (prop->contains("minimum") && value.get<long long>() < (*prop)["minimum"].get<long long>())
                return "'" + key + "' is below the minimum";
            if (prop->contains("maximum") && value.get<long long>() > (*prop)["maximum"].get<long long>())
                return "'" + key + "' is above the maximum";
        }
        if (prop->contains("enum")) {
            const auto& allowed = (*prop)["enum"];
            if (std::find(allowed.begin(), allowed.end(), value) == allowed.end())
                return "'" + key + "' must be one of " + allowed.dump();
        }
    }
    return {};
}
}

json ToolRegistry::execute(const std::string& name, const json& arguments) {
    auto it = tools.find(name);
    if (it == tools.end()) {
        json available = json::array();
        for (const auto& [tool_name, _] : tools) available.push_back(tool_name);
        return {{"success", false},
                {"error", "Tool not found: " + name},
                {"available_tools", available}};
    }
    if (const std::string problem = validate_arguments(schemas[name], arguments); !problem.empty()) {
        return {{"success", false}, {"error", "Invalid arguments for " + name + ": " + problem}};
    }
    return it->second(arguments);
}
```

```cpp
// Agent.cpp — Agent::run: give the model the raw text it sent so it can fix it
if (!call.id_valid || !call.arguments_valid || call.name.empty()) {
    toolResult = {
        {"success", false},
        {"error", "Invalid structured tool call: arguments must be a JSON object matching the tool schema"},
        {"received_arguments", call.original_raw_arguments.substr(0, 500)}
    };
}

// Client.cpp — ProcessSseLine: don't concatenate repeated IDs
if (partial.contains("id") && partial["id"].is_string() &&
    !partial["id"].get<std::string>().empty()) {
    call.id = partial["id"].get<std::string>();
    call.id_valid = true;
}
```

---

### 3. Repeated Tool-Call Loops

- [ ] Repeated Tool-Call Loops

**What happens:** The model repeatedly calls a tool with the same arguments, often without responding to a previous failure.

**Production handling:**
- Track repeated calls, especially identical calls that keep failing.
- Stop or escalate after a small number of repeated failures.
- Retain a hard per-turn limit as a backstop.
- Avoid rejecting every repeated call: repeating a successful read or check may be intentional.

**Your code:** There is no repeated-call or repeated-failure detection. Bokkie does have a cap of 10 tool calls and 10 model steps per user request (`Agent.cpp:330-334, 401-410`).

**Fix (production pattern):**

```cpp
// Agent.cpp — Agent::run, declared next to tool_calls_this_turn (reset every user turn)
// needs <map>
std::map<std::string, int> failed_call_counts;   // key = name + canonical arguments
constexpr int max_identical_failures = 2;

// inside the tool loop, before executing:
const std::string call_key = call.name + '\n' + call.arguments.dump();  // nlohmann sorts keys
if (failed_call_counts[call_key] >= max_identical_failures) {
    toolResult = {
        {"success", false},
        {"error", "This exact call already failed " + std::to_string(max_identical_failures) +
                  " times. Do not repeat it; change the arguments or explain the problem to the user."}
    };
    repeated_failure_stop = true;   // break out of the step loop after this batch
} else {
    // ... existing execute path ...
}

// after the result is known:
if (!toolResult.value("success", false)) {
    ++failed_call_counts[call_key];
}   // successful repeats are allowed; the 10-call / 10-step caps stay as the backstop

// after the tool loop:
if (repeated_failure_stop) {
    std::cout << "[stopped: the model kept repeating a failing tool call]\n";
    break;
}
```

---

### 4. Context Overflow and Compaction

- [ ] Context Overflow and Compaction

**What happens:** The request exceeds the model's context capacity, or the model's quality degrades as the conversation grows.

**Production handling:**
- Compact or prune history before the context limit is reached.
- Keep the current user request and any tool-call/result exchanges needed to continue.
- Measure behavior with the actual model before changing the compaction threshold.
- Define what to do when token-usage data is unavailable.

**Your code:** Conversation compaction triggers at 70% based on the latest reported prompt-plus-completion usage, and runs after a model response (`brain/Conversation.cpp:18-19, 193-210`; `Agent.cpp:359-360, 405-406`). If usage is unavailable, compaction is skipped. The suggestion to reduce the threshold to 30–50% is unverified and should be tested against the model actually in use. The checked-in `config.json` names a Qwen3.5 profile, not LFM3-1.7B; the repository alone cannot establish which model is running.

**Verified / additional findings:**
- Confirmed: `config.json` only has the `Qwen3.5-9B-UD-Q5_K_XL` profile.
- `applyCompaction` sets `has_token_usage_ = false` after compacting (`Conversation.cpp:297`). So does a response without usage, and the next check is skipped until a new usage report arrives. Large tool results added after the last response (for example a big `read_file`) are not counted before the next request is sent.
- When the prompt is too large, llama.cpp returns HTTP 400 with an `exceed_context_size_error` type. Today that is only reported, never acted on.

**Fix (production pattern):**

```cpp
// Conversation.cpp — make the threshold tunable for measurement instead of hardcoding it
double compaction_trigger_fraction() {
    if (const char* value = std::getenv("BOKKIE_COMPACTION_FRACTION")) {
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (end != value && *end == '\0' && parsed > 0.1 && parsed < 0.95) return parsed;
    }
    return 0.7;
}

// Conversation.cpp — fallback estimate when usage is unavailable (~4 chars/token is a
// conservative rule of thumb; llama.cpp's POST /tokenize gives an exact count if needed)
std::int64_t Conversation::estimate_tokens(const std::string& system_prompt) const {
    size_t chars = system_prompt.size();
    for (const auto& message : messages_) chars += message.dump().size();
    return static_cast<std::int64_t>(chars / 4);
}

// Conversation.h — compaction that works from an estimate when usage is unavailable
bool applyCompaction(std::int64_t estimated_tokens_if_no_usage = -1);
```

```cpp
// Agent.cpp — pre-flight check BEFORE each request (catches large tool results),
// plus a single compact-and-retry when the server reports a context overflow
if (!conversation_.has_token_usage()) {
    safe_compact(conversation_.estimate_tokens(system_prompt_));   // see section 14
}
ChatResponse response = /* send */;
if (response.error.find("exceed_context_size") != std::string::npos ||
    response.error.find("context size") != std::string::npos) {
    if (safe_compact(conversation_.max_context_tokens())) {   // force: treat as 100% full
        response = /* send again, once */;
    }
}
```

---

### 5. Hallucinations and Incorrect Claims

- [ ] Hallucinations and Incorrect Claims

**What happens:** The model fabricates facts, misreports tool results, or says an operation succeeded when it failed.

**Production handling:**
- Base success messages on actual tool results.
- Validate important outputs with deterministic checks where possible.
- Require confirmation before destructive or otherwise high-impact operations when appropriate.
- Treat separate-model fact checking or confidence scoring as optional features, not guarantees.

**Your code:** The system prompt tells the model not to claim success unless a tool result confirms it (`Agent.cpp`, system-prompt construction around lines 293-310), and tool results include success or error information. There is no general programmatic verification of final claims. Destructive file tools can be called without an explicit confirmation step.

**Verified / additional findings:**
- The system prompt is built at `Agent.cpp:283-311`. The terminal only prints `[tool: name]` (`Agent.cpp:371`), never whether the tool **actually** succeeded. So the only success/failure report the user sees is the model's own claim. Printing the real result next to it is the cheapest deterministic check.

**Fix (production pattern):**

```cpp
// Agent.cpp — show the ground-truth outcome of every tool call next to the model's narration
const bool ok = toolResult.value("success", false);
std::cout << "[tool: " << toolName << (ok ? " ✓" : " ✗ ")
          << (ok ? "" : toolResult.value("error", std::string("failed"))) << "]\n";
```

```cpp
// Agent.cpp — optional human-in-the-loop confirmation for high-impact tools
bool requires_confirmation(const std::string& name) {
    return name == "delete_file" || name == "delete_directory" ||
           name == "write_file" || name == "run_shell" || name == "run_python";
}

bool confirm_action(const ToolCall& call) {
    std::cout << "[confirm] " << describe_tool_action(call) << " — allow? [y/N] " << std::flush;
    std::string answer;
    if (!read_input_line(answer)) return false;
    return answer == "y" || answer == "Y" || answer == "yes";
}

// in the tool loop, before registry_.execute(...):
if (requires_confirmation(call.name) && !confirm_action(call)) {
    toolResult = {{"success", false}, {"error", "The user declined this operation"}};
}
```

---

### 6. Refusals and Safety Responses

- [ ] Refusals and Safety Responses

**What happens:** The model declines a request or returns a safety-related response.

**Production handling:**
- Present refusals clearly as model responses.
- Do not automatically retry with a modified prompt or switch to a less restrictive model; a refusal is not necessarily a transient failure.
- If the refusal needs interpretation, make that behavior explicit and test it.

**Your code:** There is no special refusal detection; refusal text is shown as an ordinary response. This is not itself a failure requiring automatic retry.

**Fix (production pattern):** No behavioral change needed. The only rule is that the retry logic from sections 1 and 8 must never treat a non-empty answer (which a refusal is) as retryable. The one structured signal worth surfacing is a provider `content_filter` finish reason:

```cpp
// Agent.cpp — after the response is received; retry logic keys ONLY on emptiness/errors,
// so a refusal (non-empty content) is displayed once and never retried.
if (response.finish_reason == "content_filter") {
    std::cout << "[the server's content filter stopped this response]\n";
}
```

---

### 7. Streaming Interruptions and Incomplete Responses

- [ ] Streaming Interruptions and Incomplete Responses

**What happens:** A stream is interrupted, stalls, or ends without a complete response.

**Production handling:**
- Set connection and overall request timeouts; consider a stalled-stream timeout.
- Check that the stream ended in a valid, complete state before accepting the response.
- Keep partial output distinct from a complete answer; do not execute partial tool calls.
- Retry only when appropriate, and avoid presenting a retry as if it were a continuation.

**Your code:** Tokens are rendered as they arrive, so partial text may already have appeared in the terminal. But if curl fails, that partial text is not returned as `ChatResponse.content`; the agent then adds an empty assistant response and treats the turn as complete (`Client.cpp:235-268`; `Agent.cpp:342-359`). The parser does not require a terminal `[DONE]` event or check `finish_reason`, so a stream that ends cleanly but prematurely may be accepted. There are no explicit curl timeouts.

**Verified / additional findings:**
- Confirmed: `if (data == "[DONE]") return true;` (`Client.cpp:44-46`) ignores the marker, and `finish_reason` is never read.
- Use a **stall** timeout (`CURLOPT_LOW_SPEED_*`) rather than a short total `CURLOPT_TIMEOUT`. Long local generations can legitimately take minutes, but a stream that sends nothing for N seconds is dead.

**Fix (production pattern):**

```cpp
// Client.cpp — ResponseContext: track how the stream ended
struct ResponseContext {
    // ... existing fields ...
    bool received_done = false;
    std::string finish_reason;
    size_t reasoning_chars = 0;
};

// Client.cpp — ProcessSseLine
if (data == "[DONE]") {
    context.received_done = true;
    return true;
}
// ... after locating choices->at(0):
const auto& choice = choices->at(0);
if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
    context.finish_reason = choice["finish_reason"].get<std::string>();
}
// ... inside the delta handling:
if (const auto reasoning = delta->find("reasoning_content");
    reasoning != delta->end() && reasoning->is_string()) {
    context.reasoning_chars += reasoning->get_ref<const std::string&>().size();
}
```

```cpp
// Client.cpp — send_payload: timeouts (connect + stalled stream)
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);   // can't reach the server
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);   // < 1 byte/s ...
curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);  // ... for 120 s = stalled (covers slow prompt processing)

// Client.cpp — send_payload: accept only a complete stream; keep partial text separate
const bool complete = response.received_done || !response.finish_reason.empty();
if (result == CURLE_OK && statusCode >= 200 && statusCode < 300 &&
    response.error.empty() && !complete) {
    chatResponse.error = "Stream ended before completion (no finish_reason / [DONE]).";
}
chatResponse.finish_reason = response.finish_reason;
chatResponse.received_done = response.received_done;
chatResponse.streamed_any_token = !response.completion.empty();
if (!chatResponse.error.empty()) {
    chatResponse.partial_content = std::move(response.completion);  // new field; never executed
    // tool_calls are intentionally NOT populated on an incomplete stream
}
if (chatResponse.error.empty() && response.finish_reason == "length" && !response.toolCalls.empty()) {
    chatResponse.error = "Response hit the token limit while emitting a tool call; not executing it.";
}
```

---

### 8. Rate Limits, Server Errors, and Overload

- [ ] Rate Limits, Server Errors, and Overload

**What happens:** The server returns an HTTP error, is temporarily overloaded, or cannot process the request.

**Production handling:**
- Retry only transient errors, with bounded backoff; use jitter when many clients may retry together.
- Report non-retryable errors clearly.
- Add queuing or a circuit breaker only if the deployment's load and availability requirements justify them.

**Your code:** Non-2xx responses are reported, but there is no retry or backoff for rate limits, overload, or other server errors (`Client.cpp:235-248`). There is no queue or circuit breaker.

**Verified / additional findings:**
- llama.cpp returns **503** while a model is still loading and when all slots are busy, so this matters even for a local server.
- Retrying is only safe when **no token has been rendered yet**. Otherwise the retried answer is printed after half of the previous one.

**Fix (production pattern):**

```cpp
// Client.cpp — classify failures; retry only transient ones, before any output was streamed
// needs <thread>, <chrono>, <random>
namespace {
bool is_transient(CURLcode result, long status) {
    if (result == CURLE_COULDNT_CONNECT || result == CURLE_OPERATION_TIMEDOUT ||
        result == CURLE_RECV_ERROR || result == CURLE_GOT_NOTHING ||
        result == CURLE_PARTIAL_FILE) return true;
    return status == 408 || status == 429 || status == 500 ||
           status == 502 || status == 503 || status == 504;
}

std::chrono::milliseconds backoff_delay(int attempt, long retry_after_seconds) {
    if (retry_after_seconds > 0)                        // honor server's Retry-After
        return std::chrono::seconds(std::min(retry_after_seconds, 30L));
    static std::mt19937 rng{std::random_device{}()};
    const int base = 500 * (1 << attempt);              // 0.5s, 1s, 2s
    std::uniform_int_distribution<int> jitter(0, base / 2);
    return std::chrono::milliseconds(base + jitter(rng));
}
}

// send_payload: wrap curl_easy_perform
constexpr int max_attempts = 3;
CURLcode result{};
long statusCode = 0;
for (int attempt = 0; attempt < max_attempts; ++attempt) {
    response = ResponseContext{};
    response.onToken = on_token;
    result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    const bool ok = result == CURLE_OK && statusCode >= 200 && statusCode < 300;
    if (ok || !response.completion.empty() || !is_transient(result, statusCode)) break;
    curl_off_t retry_after = 0;
    curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retry_after);   // libcurl >= 7.66
    std::cerr << "[server unavailable (" << statusCode << "), retrying...]\n";
    std::this_thread::sleep_for(backoff_delay(attempt, static_cast<long>(retry_after)));
}
```

---

### 9. Nondeterministic Output and Sampling Configuration

- [ ] Nondeterministic Output and Sampling Configuration

**What happens:** The same request produces different output across runs, or sampling settings differ from what the operator expects.

**Production handling:**
- Use fixed seeds and lower temperatures only when reproducibility is needed and the server supports those settings.
- Verify that configured sampling values actually reach the model.
- Use evaluations to detect meaningful regressions; caching is optional and may be inappropriate for changing or tool-using requests.

**Your code:** Chat generation reads `BOKKIE_TEMPERATURE` (default `0.7`) and optional `BOKKIE_SEED` (`Client.cpp:172-183`). The sampling values in `config.json` are not wired into this request path, and the environment-variable values are not validated here. There is no task-based adjustment, response cache, or evaluation suite. Whether a seed is honored depends on the server and model.

**Verified / additional findings:**
- Unvalidated is worse than "ignored": `std::stod` / `std::stoi` **throw** on a bad value (e.g. `BOKKIE_TEMPERATURE=abc`). `send_payload` has no handler, so the exception reaches `main` and terminates Bokkie (`main.cpp:12-14`).
- `config.json` has `temperature`, `top_p`, `top_k`, `repeat_penalty`. llama.cpp's `/v1/chat/completions` accepts all four as request fields.

**Fix (production pattern):**

```cpp
// Client.cpp — non-throwing, range-checked env parsing
namespace {
std::optional<double> env_double(const char* name, double min, double max) {
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || parsed < min || parsed > max) {
        std::cerr << "[ignoring invalid " << name << "=" << value << "]\n";
        return std::nullopt;
    }
    return parsed;
}
}

// Client.cpp — send_payload: config.json sampling first, env vars override, all validated
// (pass `sampling` in from Agent: config_["profiles"][<active profile>]["sampling"])
json payload = { /* messages, tools, tool_choice, stream, stream_options as today */ };
for (const char* key : {"temperature", "top_p", "top_k", "repeat_penalty"}) {
    if (sampling.contains(key) && sampling[key].is_number()) payload[key] = sampling[key];
}
if (auto t = env_double("BOKKIE_TEMPERATURE", 0.0, 2.0)) payload["temperature"] = *t;
if (auto s = env_double("BOKKIE_SEED", 0, 4294967295.0)) payload["seed"] = static_cast<long long>(*s);
if (debug) std::cerr << "[sampling sent: " << payload.value("temperature", -1.0) << "]\n";  // verify it reaches the server
```

---

### 10. Endpoint Outages and Fallbacks

- [ ] Endpoint Outages and Fallbacks

**What happens:** The configured LLM endpoint becomes unavailable or the selected model cannot provide the requested capability.

**Production handling:**
- Show a clear error and preserve the user's input.
- Add another provider or model only if the application has a configured, tested alternative.
- Use a circuit breaker or local fallback when availability requirements justify the added complexity.
- Handle model/template incompatibility explicitly.

**Your code:** Chat requests use one endpoint at a time; `BOKKIE_SERVER_URL` can change the endpoint, but there is no automatic provider or model fallback (`Client.cpp:221-227`). Startup also queries `/props`, so an unavailable endpoint can prevent the agent from starting. The README notes that a model's chat template may not support structured tool calls, and Bokkie does not fall back to parsing tool-call text (`README.md:38-43`).

**Verified / additional findings:**
- Confirmed: `Client::get_max_context_tokens` throws (`Client.cpp:290-344`) inside the `Agent` constructor (`Agent.cpp:315`), and `main` exits on it.
- The README `Configuration` section (`README.md:88-90`) is out of date: it says the URL is hardcoded, but `BOKKIE_SERVER_URL` exists.

**Fix (production pattern):**

```cpp
// Agent.cpp — Agent::Agent: degrade instead of refusing to start
try {
    conversation_.set_max_context_tokens(Client::get_max_context_tokens());
} catch (const std::exception& error) {
    constexpr int fallback_ctx = 8192;   // or config_ profile "llama.ctx_size" if set
    std::cerr << "[warning] " << error.what()
              << "\n[server not reachable yet; assuming a " << fallback_ctx
              << "-token context. Requests will fail until the server is up.]\n";
    conversation_.set_max_context_tokens(fallback_ctx);
}
```

```cpp
// Agent.cpp — Agent::run: on an outage the user's message stays in history (not lost),
// and the user can just press Enter / type "retry" to resend it
if (!response.error.empty()) {
    std::cout << "[request failed: " << response.error
              << "]\n[your message was kept; send a follow-up or 'retry' to try again]\n";
    break;
}
```

---

### 11. Auxiliary Requests Can Use a Different Endpoint

- [ ] Auxiliary Requests Can Use a Different Endpoint

**What happens:** Main chat works against the configured server, but a supporting request—such as memory handling or conversation compaction—goes to another endpoint and fails.

**Production handling:**
- Use one shared endpoint configuration for chat, startup metadata, memory, and compaction requests.
- Apply consistent timeout and error handling to all of those requests.

**Your code:** Chat and `/props` honor `BOKKIE_SERVER_URL`. Conversation compaction and memory requests instead hardcode `http://localhost:8080` (`brain/Conversation.cpp:63`; `brain/Memory.cpp:130`). They can therefore fail when the main endpoint is configured elsewhere.

**Verified / additional findings:**
- Confirmed at both lines. The URL-resolution block is also duplicated in `Client.cpp:221-225` and `Client.cpp:304-308`.
- Four places each contain their own copy of the curl setup and teardown, so the timeouts from section 7 would have to be added four times. A single helper fixes both problems.

**Fix (production pattern):**

```cpp
// Client.h — one source of truth for the endpoint and for non-streaming JSON POSTs
class Client {
public:
    // ... existing ...
    static std::string server_url();   // BOKKIE_SERVER_URL or http://localhost:8080, no trailing '/'
    static json post_json(const std::string& path, const json& payload, long timeout_seconds = 120);
};

// Client.cpp
std::string Client::server_url() {
    std::string url = "http://localhost:8080";
    if (const char* configured = std::getenv("BOKKIE_SERVER_URL"); configured && *configured) {
        url = configured;
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

json Client::post_json(const std::string& path, const json& payload, long timeout_seconds) {
    // same curl setup as today, plus:
    //   CURLOPT_URL            = server_url() + path
    //   CURLOPT_CONNECTTIMEOUT = 10, CURLOPT_TIMEOUT = timeout_seconds
    //   capped response body (section 13), json::parse(body, nullptr, false)
    // throws std::runtime_error with a clear message on any failure
}

// brain/Conversation.cpp — _summarize_messages (and likewise brain/Memory.cpp:130)
const json response = Client::post_json("/v1/chat/completions", payload);
```

---

### 12. LLM Request Errors Are Not Handled as Turn Failures

- [ ] LLM Request Errors Are Not Handled as Turn Failures

**What happens:** The client detects a failed request, but the agent continues as if it received a normal empty response.

**Production handling:**
- Branch explicitly on transport, HTTP, and server errors.
- Do not add a failure-shaped empty assistant message as if it were a valid model answer.
- Show a clear status and leave the user able to retry.

**Your code:** `Client` sets and logs `ChatResponse.error`, but `Agent::run` does not check it before adding the response to conversation history and marking the response final (`Client.cpp`; `Agent.cpp:336-359`).

**Verified / additional findings:**
- Confirmed: `Agent.cpp:342-345` calls `add_assistant(...)` and sets `received_final_response = true` with no check of `response.error`.

**Fix (production pattern):**

```cpp
// Agent.cpp — Agent::run, immediately after renderer.finish() and BEFORE add_assistant()
if (!response.error.empty()) {
    std::cout << '\n';
    if (!response.partial_content.empty()) {
        std::cout << "[response interrupted — the text above is incomplete]\n";
    }
    std::cout << "[model request failed: " << response.error << "]\n"
              << "[nothing was added to the conversation; send a follow-up to retry]\n";
    received_final_response = true;   // suppress the misleading "tool-step limit" message
    break;                            // leave the step loop; user stays in control
}
conversation_.add_assistant(response.content, response.tool_calls);
```

---

### 13. Unbounded Response and Error-Body Memory Use

- [ ] Unbounded Response and Error-Body Memory Use

**What happens:** A very large response or server error body consumes excessive memory.

**Production handling:**
- Set reasonable limits for streamed response size, accumulated text, and diagnostic bodies.
- On reaching a limit, stop reading and report the condition clearly.

**Your code:** The client accumulates both the full raw response body and completion text while streaming, without an explicit size limit (`Client.cpp:16-23, 121-123, 132-134`).

**Verified / additional findings:**
- Current line numbers: `ResponseContext` at `Client.cpp:22-30`, `completion +=` at `Client.cpp:122`, `body.append` at `Client.cpp:134`. `pendingLine` (`Client.cpp:136`) can also grow without limit if a server never sends a newline.
- On success `body` is only used for `--debug` output, so it doesn't need to be kept in full.

**Fix (production pattern):**

```cpp
// Client.cpp — caps (generous for a local agent; tune as needed)
constexpr size_t max_completion_bytes   = 4 * 1024 * 1024;  // 4 MiB of answer text
constexpr size_t max_diagnostic_bytes   = 64 * 1024;        // raw body kept for errors/debug
constexpr size_t max_pending_line_bytes = 1 * 1024 * 1024;  // a single SSE line

size_t WriteCallback(char* contents, size_t size, size_t nmemb, void* userp) {
    auto* context = static_cast<ResponseContext*>(userp);
    const size_t totalSize = size * nmemb;

    if (context->body.size() < max_diagnostic_bytes) {
        context->body.append(contents, std::min(totalSize, max_diagnostic_bytes - context->body.size()));
    }
    context->pendingLine.append(contents, totalSize);
    if (context->pendingLine.size() > max_pending_line_bytes) {
        context->error = "Streaming line exceeded the size limit; response aborted.";
        return 0;   // tells curl to abort -> CURLE_WRITE_ERROR
    }
    // ... existing line loop ...
    if (context->completion.size() > max_completion_bytes) {
        context->error = "Response exceeded the size limit; response aborted.";
        return 0;
    }
    return totalSize;
}

// AppendBodyCallback (used by /props) — same idea
if (body->size() + totalSize > max_diagnostic_bytes) return 0;
```

---

### 14. Compaction Failure Can Stop the Agent

- [ ] Compaction Failure Can Stop the Agent

**What happens:** The extra request used to summarize old conversation history fails.

**Production handling:**
- Report compaction failure separately from the main model response.
- Preserve the original conversation if summarization fails; avoid silently dropping history.
- Ensure a failed compaction does not unexpectedly terminate the interactive session.

**Your code:** Compaction makes a separate, non-streaming request and throws on request or response errors (`brain/Conversation.cpp:28-107, 277`). The call is not handled locally in `Agent::run`; an exception can reach `main`, whose top-level handler exits the program (`main.cpp:9-15`).

**Verified / additional findings:**
- History is already preserved on failure: `_summarize_messages` runs (`Conversation.cpp:277`) before `messages_` is replaced (`Conversation.cpp:290`). Only the uncaught exception is the problem.
- `json::parse(response_body)` (`Conversation.cpp:89`; same in `Memory.cpp:155`) is the throwing overload, so a non-JSON body (HTML proxy page, truncated reply) throws `nlohmann::json::parse_error`.
- Same pattern at startup: `Memory::render_memory()` (`Agent.cpp:280`) makes a summarization request once the memory file is over 10,000 bytes and throws if the server is down, so Bokkie cannot start.

**Fix (production pattern):**

```cpp
// Agent.cpp — one non-fatal wrapper used at both call sites (Agent.cpp:359 and :405)
auto safe_compact = [this](std::int64_t estimate = -1) {
    try {
        if (conversation_.applyCompaction(estimate)) {
            std::cout << "[conversation compacted]\n";
            return true;
        }
    } catch (const std::exception& error) {
        std::cerr << "[compaction failed, full history kept: " << error.what() << "]\n";
    }
    return false;
};

// replaces:  if (response.usage.available && conversation_.applyCompaction()) { ... }
if (response.usage.available) safe_compact();
```

```cpp
// brain/Conversation.cpp:89 and brain/Memory.cpp:155 — non-throwing parse
const json response = json::parse(response_body, nullptr, false);
if (response.is_discarded()) {
    throw std::runtime_error("Conversation summary response was not valid JSON");
}

// brain/Memory.cpp — render_memory(): fall back to the raw (truncated) memory if summarizing fails
try {
    return summarize(memory_content);
} catch (const std::exception& error) {
    std::cerr << "[memory summary failed, using raw memory: " << error.what() << "]\n";
    return memory_content.substr(0, 10000);
}
```

---

### Real-World Numbers

The outage rates, empty-response percentages, and provider-adoption statistics in the earlier report were not sourced, and the reported uptime-to-downtime conversion is inconsistent. They should be omitted unless supported by reliable sources and a clear measurement period. The claim that small local models have a particular empty-response rate is not verifiable from this repository.

### What Bokkie Could Implement (Suggested Priority Order)

1. **Handle empty responses and request errors distinctly** — detect empty successful responses, handle `response.error`, and show a useful turn-level status. *(sections 1, 12)*
2. **Add bounded timeouts** — cover chat, `/props`, memory, and compaction requests. *(sections 7, 11)*
3. **Validate stream completion** — detect incomplete streams; keep partial output distinct from complete responses. *(section 7)*
4. **Use one endpoint configuration consistently** — remove hardcoded auxiliary-request URLs. *(section 11)*
5. **Add safe, bounded retry behavior** — retry only appropriate transient failures or empty responses; avoid retrying valid tool-only responses. *(sections 1, 8)*
6. **Strengthen tool-call safeguards** — validate arguments against schemas and detect repeated failing calls, retaining the existing 10-call and 10-step limits. *(sections 2, 3)*
7. **Protect against excessive response sizes** — cap streamed response and diagnostic-body accumulation. *(section 13)*
8. **Make compaction failure non-fatal** — preserve history and let the user continue if summarization fails. *(section 14)*
9. **Evaluate compaction settings with the actual model** — do not assume 30–50% is better without testing. *(section 4)*
10. **Consider confirmation for destructive tools** — if the intended UX requires explicit approval. *(section 5)*

Provider rotation, request queuing, automatic refusal retries, and automatic temperature changes are not immediate priorities for this single-endpoint local harness without a specific requirement and tests.

**Additional quick wins found during verification (not in the original list):**
- Validate `BOKKIE_TEMPERATURE` / `BOKKIE_SEED` without throwing; a bad value currently crashes the program. *(section 9)*
- Make startup survive an unreachable `/props` and a failed memory summary. *(sections 10, 14)*
- Print each tool's real success/failure in the terminal. *(section 5)*
- Call `curl_global_init` once in `main` instead of on every request. libcurl documents it as not thread-safe, and calling it per request is wasteful.

```cpp
// main.cpp
int main() {
    enable_ansi();
    curl_global_init(CURL_GLOBAL_DEFAULT);   // once per process
    int exit_code = 0;
    try {
        Agent agent;
        agent.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        exit_code = 1;
    }
    curl_global_cleanup();
    return exit_code;
}
// ...and remove the curl_global_init/curl_global_cleanup pairs from Client.cpp,
// brain/Conversation.cpp and brain/Memory.cpp.
``