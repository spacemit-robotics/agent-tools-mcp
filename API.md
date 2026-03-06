# MCP Client SDK API

MCP C++ 客户端库 API 参考。头文件: `include/mcp_service.hpp`。

## 功能特性

- **多传输**: stdio (子进程管道) / Unix Socket / Streamable HTTP
- **多服务器管理**: MCPManager 统一管理，工具调用自动路由
- **异步事件**: 服务器状态变化回调、工具列表变化回调
- **错误处理**: 分层错误码 + MCPException 异常

---

## C++ API

```cpp
#include <mcp_service.hpp>

namespace mcp {

// =============================================================================
// 版本信息
// =============================================================================

constexpr const char* SDK_VERSION = "1.0.0";
constexpr const char* PROTOCOL_VERSION = "2024-11-05";

// =============================================================================
// ErrorCode - 错误码
// =============================================================================

enum class ErrorCode {
    None = 0,

    // 传输层 (1xx)
    TransportError = 100,
    ConnectionFailed = 101,
    ConnectionClosed = 102,
    SendFailed = 103,
    ReceiveFailed = 104,
    Timeout = 105,

    // 协议层 (2xx)
    ProtocolError = 200,
    InvalidResponse = 201,
    InitializeFailed = 202,

    // 服务器 (3xx)
    ServerError = 300,
    ToolNotFound = 301,
    ServerNotFound = 302,
    ToolCallFailed = 303,
};

// =============================================================================
// MCPException - 异常
// =============================================================================

class MCPException : public std::runtime_error {
public:
    MCPException(ErrorCode code, const std::string& msg);
    ErrorCode code() const;
};

// =============================================================================
// Transport - 传输层抽象基类
// =============================================================================

class Transport {
public:
    virtual bool connect() = 0;              // 连接/启动
    virtual void disconnect() = 0;           // 断开/关闭
    virtual bool isConnected() const = 0;    // 连接状态
    virtual bool send(const std::string& data) = 0;  // 发送数据
    virtual std::string receive(             // 接收一行（阻塞，带超时）
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) = 0;
    virtual std::string typeName() const = 0;  // 传输类型名
};

// =============================================================================
// Transport 配置
// =============================================================================

struct StdioConfig {
    std::string command;                     // 要执行的命令
    std::vector<std::string> args;           // 命令参数
    std::string workingDir;                  // 工作目录（可选）
    std::map<std::string, std::string> env;  // 额外环境变量（可选）
};

struct UnixSocketConfig {
    std::string socketPath;                  // Socket 文件路径
};

struct HttpConfig {
    std::string url;                         // MCP 端点 URL
    std::map<std::string, std::string> headers;  // 额外 HTTP 头（可选）
    std::chrono::milliseconds timeout{30000};    // 请求超时
    bool enableSSE = true;                       // 启用 SSE 响应
};

// =============================================================================
// Transport 工厂函数
// =============================================================================

std::unique_ptr<Transport> createStdioTransport(const StdioConfig& config);
std::unique_ptr<Transport> createUnixSocketTransport(const UnixSocketConfig& config);
std::unique_ptr<Transport> createHttpTransport(const HttpConfig& config);

// =============================================================================
// Tool - 工具定义
// =============================================================================

struct Tool {
    std::string name;         // 工具名称
    std::string description;  // 工具描述
    json inputSchema;         // 输入参数 JSON Schema

    static Tool fromJson(const json& j);
    json toJson() const;
};

// =============================================================================
// ToolResult - 工具调用结果
// =============================================================================

struct ToolResult {
    bool success = true;                // 是否成功
    std::vector<std::string> contents;  // 文本内容列表
    std::string error;                  // 错误信息（如果失败）
    json rawResult;                     // 原始 JSON 结果
};

// =============================================================================
// ServerInfo - 服务器信息
// =============================================================================

struct ServerInfo {
    std::string name;
    std::string version;
    json capabilities;
};

// =============================================================================
// MCPClient - 单服务器客户端
// =============================================================================

struct ClientConfig {
    std::string name = "mcp-cpp-client";
    std::string version = "1.0.0";
    std::chrono::milliseconds timeout{5000};
};

class MCPClient {
public:
    explicit MCPClient(std::unique_ptr<Transport> transport,
                       const ClientConfig& config = {});
    ~MCPClient();

    MCPClient(MCPClient&&) noexcept;
    MCPClient& operator=(MCPClient&&) noexcept;

    bool connect();                    // 连接到服务器
    bool initialize();                 // 初始化 MCP 会话
    std::vector<Tool> listTools();     // 获取工具列表
    ToolResult callTool(const std::string& name, const json& args);  // 调用工具
    void shutdown();                   // 关闭连接

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& serverInfo() const;
    const std::string& name() const;
};

// =============================================================================
// MCPManager - 多服务器管理
// =============================================================================

enum class ServerState {
    Disconnected,   // 未连接
    Connecting,     // 连接中
    Initializing,   // 初始化中
    Ready,          // 就绪
    Reconnecting,   // 重连中
    Error           // 错误
};

struct ServerStatus {
    std::string name;
    ServerState state;
    std::vector<Tool> tools;
    std::string lastError;
};

class MCPManager {
public:
    MCPManager();
    ~MCPManager();

    // 服务器管理
    void addStdioServer(const std::string& name, const StdioConfig& config);
    void addUnixSocketServer(const std::string& name, const UnixSocketConfig& config);
    void addHttpServer(const std::string& name, const HttpConfig& config);
    bool removeServer(const std::string& name);
    bool startServer(const std::string& name);
    void stopServer(const std::string& name);
    void startAll();
    void stopAll();

    // 等待服务器就绪
    bool waitForAnyServer(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
    bool waitForAllServers(std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

    // 工具操作（自动路由到对应服务器）
    std::vector<Tool> getAllTools() const;
    json getAllToolsJson() const;
    ToolResult callTool(const std::string& toolName, const json& args);
    std::string findServerForTool(const std::string& toolName) const;

    // 状态查询
    std::vector<ServerStatus> getStatuses() const;
    ServerStatus getStatus(const std::string& name) const;
    size_t readyServerCount() const;
    bool hasAvailableServers() const;

    // 事件回调
    using ServerEventHandler = std::function<void(const std::string& serverName, ServerState state)>;
    using ToolChangeHandler = std::function<void(const std::vector<Tool>& tools)>;
    void onServerEvent(ServerEventHandler handler);
    void onToolChange(ToolChangeHandler handler);
};

// =============================================================================
// SDK 初始化
// =============================================================================

void init();       // 初始化（可选）
void cleanup();    // 清理资源

struct SDKInit {   // RAII 助手
    SDKInit();     // 调用 init()
    ~SDKInit();    // 调用 cleanup()
};

}  // namespace mcp
```

---

## C++ 示例

### 单服务器

```cpp
#include <mcp_service.hpp>

int main() {
    mcp::SDKInit sdk;

    // 创建 Stdio Transport
    auto transport = mcp::createStdioTransport({
        .command = "python3",
        .args = {"examples/services/calculator/stdio_server.py"}
    });

    // 连接 + 初始化
    mcp::MCPClient client(std::move(transport));
    client.connect();
    client.initialize();

    // 获取工具列表
    auto tools = client.listTools();
    for (const auto& t : tools) {
        printf("  %s: %s\n", t.name.c_str(), t.description.c_str());
    }

    // 调用工具
    auto result = client.callTool("add", {{"a", 10}, {"b", 20}});
    if (result.success) {
        printf("结果: %s\n", result.contents[0].c_str());
    }

    client.shutdown();
    return 0;
}
```

### 多服务器

```cpp
#include <mcp_service.hpp>

int main() {
    mcp::SDKInit sdk;
    mcp::MCPManager manager;

    // 添加不同传输类型的服务器
    manager.addStdioServer("calc", {
        .command = "python3",
        .args = {"examples/services/calculator/stdio_server.py"}
    });
    manager.addUnixSocketServer("time", {
        .socketPath = "/tmp/mcp_time.sock"
    });
    manager.addHttpServer("monitor", {
        .url = "http://localhost:8003/mcp"
    });

    // 启动并等待
    manager.startAll();
    if (!manager.waitForAnyServer(std::chrono::milliseconds(10000))) {
        fprintf(stderr, "没有服务器就绪\n");
        return 1;
    }

    // 调用工具（自动路由到 calc 服务器）
    auto result = manager.callTool("add", {{"a", 1}, {"b", 2}});
    printf("结果: %s\n", result.contents[0].c_str());

    // 查找工具所属服务器
    auto server = manager.findServerForTool("add");  // -> "calc"

    manager.stopAll();
    return 0;
}
```

### 事件回调

```cpp
mcp::MCPManager manager;

// 服务器状态变化
manager.onServerEvent([](const std::string& name, mcp::ServerState state) {
    const char* states[] = {
        "Disconnected", "Connecting", "Initializing",
        "Ready", "Reconnecting", "Error"
    };
    printf("[%s] %s\n", name.c_str(), states[static_cast<int>(state)]);
});

// 工具列表变化
manager.onToolChange([](const std::vector<mcp::Tool>& tools) {
    printf("可用工具数: %zu\n", tools.size());
});

manager.addStdioServer("calc", {.command = "python3", .args = {"calc.py"}});
manager.startAll();
```
