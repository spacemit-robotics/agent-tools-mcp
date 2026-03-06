# MCP Client SDK

C++ MCP (Model Context Protocol) 客户端库，支持 stdio / Unix Socket / Streamable HTTP 三种传输方式，可同时管理多个 MCP 服务器并自动路由工具调用。

- SDK 版本: 1.0.0
- 协议版本: 2024-11-05
- 依赖: C++17, nlohmann/json, libcurl (HTTP 传输)

## 快速开始

### 构建

**S-Robot SDK 集成构建**:

```bash
source build/envsetup.sh
lunch <target>
cd components/smart_voice/mcp && mm
```

**独立构建**:

```bash
cd components/smart_voice/mcp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 可选: 安装到系统
cmake --install build --prefix /usr/local
```

产物: `build/lib/libmcp.{a,so}`, `build/bin/{llm_chat,example_simple,example_multi}`

CMake 选项:

| 选项 | 默认 | 说明 |
|------|------|------|
| `MCP_BUILD_SHARED` | ON | 构建动态库 |
| `MCP_BUILD_STATIC` | ON | 构建静态库 |
| `MCP_BUILD_EXAMPLES` | ON | 构建示例程序 |

### 运行示例

```bash
cd components/smart_voice/mcp

# 推荐: 使用配置文件（Stdio 模式，自动启动服务，无需额外操作）
./build/bin/llm_chat -c examples/configs/config_stdio.json

# 命令行方式
./build/bin/llm_chat -b llama -m qwen3:0.6b \
    -S calc:python3:examples/services/calculator/stdio_server.py
```

> **注意**: 以下所有命令均假设工作目录为 `components/smart_voice/mcp/`。

## 三种连接方式

### 1. Stdio — 子进程管道通信

客户端 fork 子进程运行 MCP 服务脚本，通过 stdin/stdout 管道通信。服务进程由客户端自动管理，连接断开时子进程自动退出。

**适用场景**: 开发调试、单机部署、不需要长驻服务的场景。

**命令行**:

```bash
# 格式: -S <name>:<command>:<arg1>:<arg2>:...
#   name    = 自定义标识名（用于日志和工具路由）
#   command = 启动命令
#   arg1... = 命令参数（用冒号分隔）

./build/bin/llm_chat -S calc:python3:examples/services/calculator/stdio_server.py
```

**配置文件** (`config_stdio.json`):

```json
{
  "servers": [
    {
      "name": "Calculator",
      "type": "stdio",
      "command": "python3",
      "args": ["examples/services/calculator/stdio_server.py"]
    }
  ]
}
```

**C++ API**:

```cpp
mcp::MCPManager manager;
manager.addStdioServer("calc", {
    .command = "python3",
    .args = {"examples/services/calculator/stdio_server.py"}
});
manager.startAll();
```

### 2. Unix Socket — 本地套接字连接

连接到已运行的 Unix Domain Socket 服务。需要先在另一个终端启动服务。

**适用场景**: 本地长驻服务、低延迟通信、进程间解耦。

**先启动服务**:

```bash
# 每个服务需要单独启动（另开终端）
python3 examples/services/calculator/socket_server.py   # -> /tmp/mcp_calculator.sock
python3 examples/services/time/socket_server.py         # -> /tmp/mcp_time.sock
```

**命令行**:

```bash
# 格式: -U <name>:<socket_path>
./build/bin/llm_chat \
    -U calc:/tmp/mcp_calculator.sock \
    -U time:/tmp/mcp_time.sock
```

**配置文件** (`config_socket.json`):

```json
{
  "servers": [
    {"name": "Calculator", "type": "socket", "path": "/tmp/mcp_calculator.sock"},
    {"name": "TimeService", "type": "socket", "path": "/tmp/mcp_time.sock"}
  ]
}
```

**C++ API**:

```cpp
manager.addUnixSocketServer("calc", {.socketPath = "/tmp/mcp_calculator.sock"});
```

### 3. HTTP (Streamable HTTP) — 网络端点

通过 HTTP POST 请求与 MCP 服务通信，支持 SSE (Server-Sent Events) 响应流。使用 `Mcp-Session-Id` 维持会话。

**适用场景**: 远程部署、跨网络访问、微服务架构。

**启动服务**:

```bash
# 方法 1: 一键启动所有 HTTP 服务 + 注册中心
cd examples && ./start_all_services.sh start

# 方法 2: 单独启动
python3 examples/services/calculator/http_server.py --port 8001
python3 examples/services/time/http_server.py --port 8002
python3 examples/services/system_monitor/http_server.py --port 8003
```

**配置文件** (`config_http.json`):

```json
{
  "servers": [
    {"name": "Calculator", "type": "http", "url": "http://localhost:8001/mcp"},
    {"name": "TimeService", "type": "http", "url": "http://localhost:8002/mcp"},
    {"name": "SystemMonitor", "type": "http", "url": "http://localhost:8003/mcp"}
  ]
}
```

**C++ API**:

```cpp
manager.addHttpServer("calc", {.url = "http://localhost:8001/mcp"});
```

### 对比

| 特性 | Stdio | Unix Socket | HTTP |
|------|-------|-------------|------|
| 需要预启动服务 | 否（自动管理） | 是 | 是 |
| 网络访问 | 不支持 | 不支持 | 支持 |
| 延迟 | 低 | 低 | 中 |
| 部署复杂度 | 最简单 | 中等 | 较高 |
| 多客户端共享 | 不支持 | 支持 | 支持 |
| 命令行参数 | `-S name:cmd:args` | `-U name:path` | 仅配置文件 |
| 适用场景 | 开发调试、单机 | 本地长驻服务 | 远程/微服务 |

### 混合模式

可以在同一个配置中混合使用不同传输方式:

```json
{
  "servers": [
    {"name": "Calculator", "type": "stdio", "command": "python3",
     "args": ["examples/services/calculator/stdio_server.py"]},
    {"name": "SystemMonitor", "type": "socket", "path": "/tmp/mcp_system_monitor.sock"}
  ]
}
```

## 配置文件说明

所有配置文件位于 `examples/configs/`:

| 文件 | 传输方式 | 说明 |
|------|---------|------|
| `config_stdio.json` | stdio | Calculator + TimeService 通过子进程启动 |
| `config_socket.json` | socket | 三个服务通过 Unix Socket 连接 |
| `config_http.json` | http | 三个服务通过 HTTP 端点连接 |
| `config_mixed.json` | stdio + socket | Calculator (stdio) + SystemMonitor (socket) |
| `config_registry.json` | http + 注册中心 | 从注册中心动态发现服务 |

完整配置字段:

```json
{
  "backend": "llama",
  "url": "http://localhost:8080",
  "model": "qwen2.5:7b",
  "timeout": 120,
  "system_prompt": "你是一个智能助手...",
  "registry_url": "http://127.0.0.1:9000/mcp/services",
  "registry_poll_interval": 5,
  "servers": [...]
}
```

- `backend` / `url` / `model` / `timeout` — LLM 后端设置（仅 llm_chat 使用）
- `system_prompt` — LLM 系统提示词
- `registry_url` — 注册中心地址（可选，设置后自动发现 HTTP 服务）
- `registry_poll_interval` — 注册中心轮询间隔（秒）
- `servers` — 静态服务列表

## 接入 omni_agent

`application/native/omni_agent/` 通过 MCP 库实现 LLM 工具调用能力。

### 步骤

1. **编写 MCP 服务** — 用 Python (MCP SDK) 或 C (mlink) 实现工具
2. **编写配置文件** — 指定 LLM 后端和 MCP 服务列表
3. **启动服务** — Socket/HTTP 服务需要预先启动，Stdio 服务自动管理
4. **启动 omni_agent** — 传入配置文件路径:
   ```bash
   voice_chat -c /path/to/mcp_config.json
   ```

### 调用链路

```
voice_chat
  ├─ initMCP(config_path)                   # engine_init.cpp
  │   ├─ loadMCPConfig()                    # mcp_helper.cpp — 解析 JSON 配置
  │   ├─ MCPManager::addXxxServer()         # 注册服务器
  │   ├─ MCPManager::startAll()             # 连接所有服务
  │   ├─ MCPManager::getAllTools()           # 获取工具列表
  │   └─ convertMCPToolsToString()          # 转为 LLM function calling 格式
  │
  └─ voice_pipeline 工具调用循环             # voice_pipeline.cpp
      ├─ LLM chat_stream(messages, tools)   # LLM 推理（带工具列表）
      ├─ 检测 tool_calls?
      │   ├─ YES: MCPManager::callTool()    # 执行工具，结果加入对话历史
      │   │       └─ 继续循环（最多 10 轮）
      │   └─ NO: TTS 合成 → 播放
      └─ 结束
```

### 注册中心（动态服务发现）

配置 `registry_url` 后，omni_agent 会:
1. 启动时从注册中心获取在线服务列表
2. 按 `registry_poll_interval` 周期轮询
3. 自动连接新上线的服务，移除已下线的服务
4. 动态更新 LLM 可用工具列表

注册中心启动: `python3 examples/registry_server.py` (端口 9000)

HTTP 服务注册: `python3 examples/services/calculator/http_server.py --port 8001 --register http://127.0.0.1:9000`

### 关键文件

| 文件 | 作用 |
|------|------|
| `application/native/omni_agent/src/mcp_helper.cpp` | 配置加载 + 工具格式转换 |
| `application/native/omni_agent/src/engine_init.cpp` | MCPManager 初始化 |
| `application/native/omni_agent/src/voice_pipeline.cpp` | 运行时工具调用循环 |
| `application/native/omni_agent/CMakeLists.txt` | `USE_MCP` 编译选项 |

构建时通过 `USE_MCP` 控制是否启用 MCP 支持:
```bash
mm                    # 默认启用
mm -DUSE_MCP=OFF      # 禁用
```

## 自定义 MCP 工具

### 方法一: Python + MCP SDK

安装依赖: `pip install mcp`

#### Stdio 服务（推荐入门）

```python
#!/usr/bin/env python3
import asyncio
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent

server = Server("my_service")

@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="greet",
            description="生成问候语",
            inputSchema={
                "type": "object",
                "properties": {
                    "name": {"type": "string", "description": "姓名"}
                },
                "required": ["name"]
            }
        )
    ]

@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name == "greet":
        return [TextContent(type="text", text=f"你好, {arguments['name']}!")]
    return [TextContent(type="text", text=f"未知工具: {name}")]

async def main():
    async with stdio_server() as (read_stream, write_stream):
        await server.run(read_stream, write_stream, server.create_initialization_options())

if __name__ == "__main__":
    asyncio.run(main())
```

使用:
```bash
./build/bin/llm_chat -S greet:python3:path/to/my_stdio_server.py
```

#### Socket 服务

在 Stdio 版基础上，替换 `main()` 为自定义 `JsonRpcHandler` + `asyncio.start_unix_server()`，手动处理 JSON-RPC 请求。

完整示例见 `examples/services/calculator/socket_server.py`。

#### HTTP 服务（FastMCP，最简洁）

```python
#!/usr/bin/env python3
from mcp.server.fastmcp import FastMCP
import uvicorn, contextlib
from starlette.applications import Starlette
from starlette.routing import Mount

mcp = FastMCP("my_service", stateless_http=True, json_response=True)

@mcp.tool()
def greet(name: str, **kwargs) -> str:
    """生成问候语"""
    return f"你好, {name}!"

@contextlib.asynccontextmanager
async def lifespan(app):
    async with mcp.session_manager.run():
        yield

app = Starlette(routes=[Mount("/", app=mcp.streamable_http_app())], lifespan=lifespan)

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8080, log_level="warning")
```

额外依赖: `pip install mcp starlette uvicorn`

### 方法二: C 语言嵌入式（mlink）

适用于嵌入式设备直接注册工具:

```c
#include "mcp/mcp_tool.h"

char *my_tool_handler(const struct mcp_property_list *args, void *ctx) {
    // 处理工具调用，返回 JSON 字符串
    return strdup("{\"result\": \"ok\"}");
}

struct mcp_property_list *props = mcp_property_list_create();
mcp_property_list_add_string(props, "name", "姓名", true);

struct mcp_tool *tool = mcp_tool_create(
    "greet", "生成问候语", props, my_tool_handler, NULL, false);
```

详见 `components/smart_voice/mlink/device/include/mcp/mcp_tool.h`。

### 工具定义规范

每个工具需要:
- **name** — 工具名称（唯一标识，LLM 通过此名称调用）
- **description** — 工具描述（LLM 据此判断何时使用该工具）
- **inputSchema** — 参数定义（JSON Schema 格式）

```json
{
  "name": "get_weather",
  "description": "查询指定城市的天气",
  "inputSchema": {
    "type": "object",
    "properties": {
      "city": {"type": "string", "description": "城市名称"},
      "unit": {"type": "string", "enum": ["celsius", "fahrenheit"], "description": "温度单位"}
    },
    "required": ["city"]
  }
}
```

## API 参考

详见 [API.md](API.md)。
