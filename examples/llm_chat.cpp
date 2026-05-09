/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCP SDK - LLM Chat 示例
 *
 * 完整的 LLM + MCP 工具调用演示
 * 支持 llama.cpp (OpenAI 兼容) 后端，stdio / Unix Socket / HTTP 三种服务器传输
 *
 * 用法:
 *   ./llm_chat -b llama -u http://localhost:8080
 *   ./llm_chat -b openai -u http://localhost:8080
 *   ./llm_chat -c config.json
 */

#include <curl/curl.h>
#include <getopt.h>
#include <termios.h>
#include <unistd.h>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <mcp_service.hpp>

using json = nlohmann::json;

// ============================================================================
// 配置结构
// ============================================================================

struct Config {
    std::string backend = "llama";
    std::string url = "http://localhost:8080";
    std::string model = "qwen2.5:7b";
    int timeout = 120;
    std::string system_prompt = "你是一个智能助手，可以使用工具帮助用户。请用中文回复。";

    struct ServerEntry {
        std::string name;
        std::string type;       // "stdio", "socket", or "http"
        // stdio
        std::string command;
        std::vector<std::string> args;
        int startup_timeout = 30000;
        int request_timeout = 30000;
        // socket
        std::string socketPath;
        // http
        std::string url;
    };
    std::vector<ServerEntry> servers;
};

// ============================================================================
// UTF-8 感知的行输入读取器
// ============================================================================

class Utf8LineReader {
public:
    Utf8LineReader() {
        tcgetattr(STDIN_FILENO, &originalTermios_);
    }

    ~Utf8LineReader() {
        tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios_);
    }

    std::string readline(bool& eof) {
        enableRawMode();
        std::string input;
        eof = false;

        while (true) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);

            if (n <= 0) {
                eof = true;
                break;
            }

            if (c == '\n' || c == '\r') {
                std::cout << '\n';
                break;
            }

            if (c == 127 || c == '\b') {  // Backspace
                if (!input.empty()) {
                    size_t charLen = utf8CharLengthBackward(input);
                    std::string deletedChar = input.substr(input.size() - charLen);
                    input.erase(input.size() - charLen);

                    int displayWidth = isWideChar(deletedChar) ? 2 : 1;
                    for (int i = 0; i < displayWidth; ++i) {
                        std::cout << "\b \b" << std::flush;
                    }
                }
                continue;
            }

            if (c == 4) {  // Ctrl+D
                if (input.empty()) {
                    eof = true;
                    break;
                }
                continue;
            }

            if (c == 3) {  // Ctrl+C
                input.clear();
                std::cout << "^C\n";
                break;
            }

            // UTF-8 多字节字符处理
            int bytesNeeded = utf8BytesNeeded(static_cast<unsigned char>(c));
            input += c;
            std::cout << c << std::flush;

            for (int i = 1; i < bytesNeeded; ++i) {
                if (read(STDIN_FILENO, &c, 1) > 0) {
                    input += c;
                    std::cout << c << std::flush;
                }
            }
        }

        disableRawMode();
        return input;
    }

private:
    struct termios originalTermios_;

    void enableRawMode() {
        struct termios raw = originalTermios_;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    void disableRawMode() {
        tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios_);
    }

    int utf8BytesNeeded(unsigned char firstByte) {
        if ((firstByte & 0x80) == 0) return 1;
        if ((firstByte & 0xE0) == 0xC0) return 2;
        if ((firstByte & 0xF0) == 0xE0) return 3;
        if ((firstByte & 0xF8) == 0xF0) return 4;
        return 1;
    }

    size_t utf8CharLengthBackward(const std::string& str) {
        if (str.empty()) return 0;
        size_t pos = str.size() - 1;
        while (pos > 0 && (str[pos] & 0xC0) == 0x80) {
            --pos;
        }
        return str.size() - pos;
    }

    bool isWideChar(const std::string& utf8Char) {
        if (utf8Char.size() < 3) return false;
        unsigned char b0 = utf8Char[0];
        // CJK 字符范围 (简化检测)
        if (b0 >= 0xE4 && b0 <= 0xE9) return true;
        return false;
    }
};

// ============================================================================
// LLM Backend 抽象接口
// ============================================================================

class LLMBackend {
public:
    virtual ~LLMBackend() = default;

    virtual json convertTools(const json& mcp_tools) = 0;

    virtual json chatStream(
        const std::vector<json>& messages,
        const json& tools,
        std::function<void(const std::string&)> on_token) = 0;

protected:
    struct StreamContext {
        std::function<void(const std::string&)> callback;
        std::string buffer;
    };

    static size_t streamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        auto* ctx = static_cast<StreamContext*>(userp);
        ctx->buffer.append(reinterpret_cast<char*>(contents), total);

        size_t pos;
        while ((pos = ctx->buffer.find('\n')) != std::string::npos) {
            std::string line = ctx->buffer.substr(0, pos);
            ctx->buffer.erase(0, pos + 1);
            if (!line.empty() && ctx->callback) ctx->callback(line);
        }
        return total;
    }

    void httpPostStream(const std::string& url, const std::string& body,
                        std::function<void(const std::string&)> callback, int timeout) {
        CURL* curl = curl_easy_init();
        if (curl) {
            StreamContext ctx{callback, ""};
            struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<int64_t>(timeout));
            curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }
    }
};

// ============================================================================
// llama.cpp Server Backend (OpenAI 兼容)
// ============================================================================

class LlamaBackend : public LLMBackend {
public:
    LlamaBackend(const std::string& url, const std::string& model, int timeout)
        : url_(url), model_(model), timeout_(timeout) {}

    json convertTools(const json& mcp_tools) override {
        json tools = json::array();
        for (const auto& tool : mcp_tools) {
            tools.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool["name"]},
                    {"description", tool["description"]},
                    {"parameters", tool["inputSchema"]}
                }}
            });
        }
        return tools;
    }

    json chatStream(
        const std::vector<json>& messages,
        const json& tools,
        std::function<void(const std::string&)> on_token) override {
        json request = {
            {"messages", messages},
            {"stream", true}
        };

        if (!model_.empty()) {
            request["model"] = model_;
        }

        if (!tools.empty()) {
            request["tools"] = tools;
            request["tool_choice"] = "auto";
        }

        std::string full_content;
        json tool_calls = json::array();
        std::map<int, json> tool_call_map;

        httpPostStream(url_ + "/v1/chat/completions", request.dump(), [&](const std::string& chunk) {
            std::string data = chunk;
            if (data.substr(0, 6) == "data: ") {
                data = data.substr(6);
            }
            if (data == "[DONE]" || data.empty()) return;

            try {
                json j = json::parse(data);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& delta = j["choices"][0]["delta"];

                    if (delta.contains("content") && !delta["content"].is_null()) {
                        std::string token = delta["content"];
                        if (!token.empty()) {
                            full_content += token;
                            if (on_token) on_token(token);
                        }
                    }

                    if (delta.contains("tool_calls")) {
                        for (const auto& tc : delta["tool_calls"]) {
                            int idx = tc.value("index", 0);

                            if (tc.contains("id")) {
                                tool_call_map[idx] = {
                                    {"id", tc["id"]},
                                    {"type", "function"},
                                    {"function", {{"name", ""}, {"arguments", ""}}}
                                };
                            }

                            if (tc.contains("function")) {
                                if (tc["function"].contains("name")) {
                                    tool_call_map[idx]["function"]["name"] =
                                        tool_call_map[idx]["function"]["name"].get<std::string>() +
                                        tc["function"]["name"].get<std::string>();
                                }
                                if (tc["function"].contains("arguments")) {
                                    tool_call_map[idx]["function"]["arguments"] =
                                        tool_call_map[idx]["function"]["arguments"].get<std::string>() +
                                        tc["function"]["arguments"].get<std::string>();
                                }
                            }
                        }
                    }
                }
            } catch (...) {}
        }, timeout_);

        for (const auto& [idx, tc] : tool_call_map) {
            tool_calls.push_back(tc);
        }

        json result = {{"content", full_content}};
        if (!tool_calls.empty()) {
            result["tool_calls"] = tool_calls;
        }
        return result;
    }

private:
    std::string url_;
    std::string model_;
    int timeout_;
};

// ============================================================================
// 命令行解析
// ============================================================================

void printUsage(const char* prog) {
    std::cout << R"(
用法: )" << prog << R"( [选项]

LLM 后端选项:
    -b, --backend <name>     后端类型: llama (默认) 或 openai
    -u, --url <url>          后端 URL (默认: http://localhost:8080)
    -m, --model <name>       模型名称 (可选)
    -t, --timeout <sec>      请求超时秒数 (默认: 120)

MCP 服务器选项:
    -S, --stdio <name:cmd:args>   添加 stdio 服务器
                                name=标识名, cmd=命令, args=参数
                                例: -S calc:python3:examples/services/calculator/stdio_server.py
    -U, --unix <name:path>        添加 Unix Socket 服务器
                                例: -U calc:/tmp/mcp_calculator.sock
    -H, --http <name:url>         添加 HTTP 服务器 (通过 -c 配置文件)

提示词选项:
    -p, --prompt <text>      系统提示词
    -P, --prompt-file <file> 从文件读取系统提示词

其他选项:
    -c, --config <file>      从 JSON 配置文件加载 (推荐)
    -h, --help               显示帮助

注意: stdio 脚本路径相对于执行命令时的工作目录。
    推荐使用 -c 配置文件方式，避免手动拼接长路径。

Socket 服务器需要先在另一个终端启动:
    python3 examples/services/calculator/socket_server.py  -> /tmp/mcp_calculator.sock
    python3 examples/services/time/socket_server.py        -> /tmp/mcp_time.sock
    (Rust) cd examples/services/system_monitor/rust && cargo run  -> /tmp/mcp_system_monitor.sock

配置文件示例 (见 examples/configs/):
{
    "backend": "llama",
    "url": "http://localhost:8080",
    "model": "qwen2.5:7b",
    "servers": [
        {"name": "Calculator", "type": "stdio", "command": "python3",
        "args": ["examples/services/calculator/stdio_server.py"]},
        {"name": "TimeService", "type": "socket", "path": "/tmp/mcp_time.sock"},
        {"name": "SystemMonitor", "type": "http", "url": "http://localhost:8003/mcp"}
    ]
}

示例:
    # 推荐: 使用配置文件
    )" << prog << R"( -c examples/configs/config_stdio.json

    # stdio 服务器
    )" << prog << R"( -b llama -m qwen3:0.6b \
        -S calc:python3:examples/services/calculator/stdio_server.py

    # Unix Socket 服务器 (需先启动 socket 服务，见上方说明)
    )" << prog << R"( -U calc:/tmp/mcp_calculator.sock -U time:/tmp/mcp_time.sock

    # 混合模式 (stdio + socket)
    )" << prog << R"( -b llama -m qwen2.5:7b \
        -S calc:python3:examples/services/calculator/stdio_server.py \
        -U monitor:/tmp/mcp_system_monitor.sock
)" << std::endl;
}

Config parseArgs(int argc, char* argv[]) {
    Config config;

    static struct option long_options[] = {
        {"backend",     required_argument, 0, 'b'},
        {"url",         required_argument, 0, 'u'},
        {"model",       required_argument, 0, 'm'},
        {"timeout",     required_argument, 0, 't'},
        {"stdio",       required_argument, 0, 'S'},
        {"unix",        required_argument, 0, 'U'},
        {"prompt",      required_argument, 0, 'p'},
        {"prompt-file", required_argument, 0, 'P'},
        {"config",      required_argument, 0, 'c'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:u:m:t:S:U:p:P:c:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'b': config.backend = optarg; break;
            case 'u': config.url = optarg; break;
            case 'm': config.model = optarg; break;
            case 't': config.timeout = std::stoi(optarg); break;
            case 'S': {
                // 格式: name:command:arg1:arg2:...
                std::string entry = optarg;
                std::vector<std::string> parts;
                std::stringstream ss(entry);
                std::string part;
                while (std::getline(ss, part, ':')) {
                    parts.push_back(part);
                }
                if (parts.size() >= 3) {
                    Config::ServerEntry srv;
                    srv.name = parts[0];
                    srv.type = "stdio";
                    srv.command = parts[1];
                    for (size_t i = 2; i < parts.size(); i++) {
                        srv.args.push_back(parts[i]);
                    }
                    config.servers.push_back(srv);
                }
                break;
            }
            case 'U': {
                // 格式: name:socket_path
                std::string entry = optarg;
                size_t pos = entry.find(':');
                if (pos != std::string::npos) {
                    Config::ServerEntry srv;
                    srv.name = entry.substr(0, pos);
                    srv.type = "socket";
                    srv.socketPath = entry.substr(pos + 1);
                    config.servers.push_back(srv);
                }
                break;
            }
            case 'p': config.system_prompt = optarg; break;
            case 'P': {
                std::ifstream f(optarg);
                if (f) {
                    std::stringstream buffer;
                    buffer << f.rdbuf();
                    config.system_prompt = buffer.str();
                }
                break;
            }
            case 'c': {
                std::ifstream f(optarg);
                if (f) {
                    json j;
                    f >> j;

                    if (j.contains("backend")) config.backend = j["backend"];
                    if (j.contains("url")) config.url = j["url"];
                    if (j.contains("model")) config.model = j["model"];
                    if (j.contains("timeout")) config.timeout = j["timeout"];
                    if (j.contains("system_prompt")) config.system_prompt = j["system_prompt"];

                    if (j.contains("servers")) {
                        for (const auto& srv : j["servers"]) {
                            Config::ServerEntry entry;
                            entry.name = srv["name"];
                            entry.type = srv.value("type", "socket");

                            if (entry.type == "stdio") {
                                entry.command = srv["command"];
                                entry.startup_timeout = srv.value("startup_timeout", entry.startup_timeout);
                                entry.request_timeout = srv.value("request_timeout", entry.request_timeout);
                                if (srv.contains("args")) {
                                    for (const auto& arg : srv["args"]) {
                                        entry.args.push_back(arg);
                                    }
                                }
                            } else if (entry.type == "http") {
                                entry.url = srv["url"];
                            } else {
                                entry.socketPath = srv.value("path", srv.value("socket", ""));
                            }
                            config.servers.push_back(entry);
                        }
                    }
                }
                break;
            }
            case 'h': printUsage(argv[0]); exit(0);
            default:  printUsage(argv[0]); exit(1);
        }
    }

    return config;
}

// ============================================================================
// 工具调用辅助
// ============================================================================

json convertToolsToJson(const std::vector<mcp::Tool>& tools) {
    json arr = json::array();
    for (const auto& t : tools) {
        arr.push_back(t.toJson());
    }
    return arr;
}

void printStatus(mcp::MCPManager& manager) {
    auto statuses = manager.getStatuses();
    std::cout << "\n=== MCP Servers ===" << std::endl;
    for (const auto& s : statuses) {
        const char* icon = (s.state == mcp::ServerState::Ready) ? "[OK]" : "[--]";
        std::cout << icon << " " << s.name << " (" << s.tools.size() << " tools)" << std::endl;
        for (const auto& tool : s.tools) {
            std::cout << "     - " << tool.name << std::endl;
        }
    }
    std::cout << std::endl;
}

// ============================================================================
// 主程序
// ============================================================================

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);

    Config config = parseArgs(argc, argv);

    if (config.servers.empty()) {
        std::cerr << "[ERROR] 没有配置 MCP 服务器。使用 -S/-U 添加或 -c 加载配置文件。" << std::endl;
        std::cerr << "        运行 " << argv[0] << " -h 查看帮助。" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    std::cout << R"(
+--------------------------------------------------------------+
|              MCP SDK - LLM Chat Demo                         |
+--------------------------------------------------------------+
)" << std::endl;

    std::cout << "[Config] Backend: " << config.backend << std::endl;
    std::cout << "[Config] URL:     " << config.url << std::endl;
    std::cout << "[Config] Model:   " << config.model << std::endl;
    std::cout << "[Config] Servers:  " << config.servers.size() << std::endl;

    // 1. 创建 LLM 后端
    std::unique_ptr<LLMBackend> llm;
    llm = std::make_unique<LlamaBackend>(config.url, config.model, config.timeout);

    // 2. 创建 MCP Manager
    mcp::MCPManager manager;

    for (const auto& srv : config.servers) {
        if (srv.type == "stdio") {
            mcp::StdioConfig sc;
            sc.command = srv.command;
            sc.args = srv.args;
            sc.startupTimeout = std::chrono::milliseconds(srv.startup_timeout);
            sc.requestTimeout = std::chrono::milliseconds(srv.request_timeout);
            manager.addStdioServer(srv.name, sc);
            std::cout << "[Server] " << srv.name
                << " (stdio: " << srv.command << ")" << std::endl;
        } else if (srv.type == "http") {
            mcp::HttpConfig hc;
            hc.url = srv.url;
            manager.addHttpServer(srv.name, hc);
            std::cout << "[Server] " << srv.name
                << " (http: " << srv.url << ")" << std::endl;
        } else {
            mcp::UnixSocketConfig uc;
            uc.socketPath = srv.socketPath;
            manager.addUnixSocketServer(srv.name, uc);
            std::cout << "[Server] " << srv.name
                << " (socket: " << srv.socketPath << ")" << std::endl;
        }
    }

    // 3. 启动服务器
    std::cout << "\n[INFO] 启动 MCP 服务器..." << std::endl;
    manager.startAll();

    if (!manager.waitForAnyServer()) {
        std::cerr << "[ERROR] 超时：没有服务器上线" << std::endl;
        curl_global_cleanup();
        return 1;
    }

    printStatus(manager);

    // 4. 对话历史
    std::vector<json> messages;
    messages.push_back({{"role", "system"}, {"content", config.system_prompt}});

    std::cout << "[INFO] 开始对话 (quit 退出, status 查看状态, clear 清空历史)\n" << std::endl;

    // 5. 交互循环
    Utf8LineReader lineReader;
    while (true) {
        std::cout << "You: " << std::flush;
        bool eof = false;
        std::string input = lineReader.readline(eof);
        if (eof) break;

        if (input.empty()) continue;
        if (input == "quit" || input == "exit") break;
        if (input == "status") {
            printStatus(manager);
            continue;
        }
        if (input == "clear") {
            messages.clear();
            messages.push_back({{"role", "system"}, {"content", config.system_prompt}});
            std::cout << "[INFO] 对话历史已清空\n" << std::endl;
            continue;
        }

        auto current_tools = manager.getAllTools();
        json backend_tools;
        if (!current_tools.empty()) {
            backend_tools = llm->convertTools(convertToolsToJson(current_tools));
        }

        messages.push_back({{"role", "user"}, {"content", input}});

        const int MAX_ROUNDS = 10;
        int rounds = 0;

        while (rounds++ < MAX_ROUNDS) {
            std::cout << "\nAssistant: " << std::flush;

            json result = llm->chatStream(messages, backend_tools,
                [](const std::string& token) { std::cout << token << std::flush; });

            std::string content = result.value("content", "");

            if (result.contains("tool_calls") && !result["tool_calls"].empty()) {
                std::cout << std::endl;

                messages.push_back({
                    {"role", "assistant"},
                    {"content", content},
                    {"tool_calls", result["tool_calls"]}
                });

                for (const auto& tc : result["tool_calls"]) {
                    std::string tool_name = tc["function"]["name"];
                    json tool_args = tc["function"]["arguments"];

                    if (tool_args.is_string()) {
                        try {
                            tool_args = json::parse(tool_args.get<std::string>());
                        } catch (...) {}
                    }

                    std::string server = manager.findServerForTool(tool_name);
                    std::cout << "[" << server << "] " << tool_name
                        << "(" << tool_args.dump() << ")" << std::flush;

                    auto tool_result = manager.callTool(tool_name, tool_args);

                    std::string result_text;
                    if (tool_result.success && !tool_result.contents.empty()) {
                        result_text = tool_result.contents[0];
                    } else if (!tool_result.error.empty()) {
                        result_text = "Error: " + tool_result.error;
                    } else {
                        result_text = tool_result.rawResult.dump();
                    }

                    std::cout << " -> " << result_text << std::endl;

                    messages.push_back({
                        {"role", "tool"},
                        {"tool_call_id", tc.value("id", "")},
                        {"content", result_text}
                    });
                }
                continue;
            }

            std::cout << "\n" << std::endl;
            messages.push_back({{"role", "assistant"}, {"content", content}});
            break;
        }
    }

    std::cout << "[INFO] Goodbye!" << std::endl;
    manager.stopAll();
    curl_global_cleanup();
    return 0;
}
