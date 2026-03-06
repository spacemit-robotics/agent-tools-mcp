#!/usr/bin/env python3
"""
MCP Calculator Server - Unix Socket 版本
独立运行，等待客户端连接
"""

import asyncio
import json
import os
import signal
import sys
from mcp.server import Server
from mcp.types import Tool, TextContent

# Socket 路径
SOCKET_PATH = "/tmp/mcp_calculator.sock"

server = Server("calculator")


def parse_number(value, param_name: str) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        value = value.strip()
        check_value = value[1:] if value.startswith('-') else value
        for op in ['+', '-', '*', '/', '(', ')']:
            if op in check_value:
                raise ValueError(f"参数 '{param_name}' 不能是表达式")
        return float(value)
    raise ValueError("参数类型错误")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(name="add", description="加法 a + b",
             inputSchema={"type": "object", "properties": {
                 "a": {"type": "number"}, "b": {"type": "number"}
             }, "required": ["a", "b"]}),
        Tool(name="subtract", description="减法 a - b",
             inputSchema={"type": "object", "properties": {
                 "a": {"type": "number"}, "b": {"type": "number"}
             }, "required": ["a", "b"]}),
        Tool(name="multiply", description="乘法 a * b",
             inputSchema={"type": "object", "properties": {
                 "a": {"type": "number"}, "b": {"type": "number"}
             }, "required": ["a", "b"]}),
        Tool(name="divide", description="除法 a / b",
             inputSchema={"type": "object", "properties": {
                 "a": {"type": "number"}, "b": {"type": "number"}
             }, "required": ["a", "b"]}),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    try:
        a = parse_number(arguments.get("a"), "a")
        b = parse_number(arguments.get("b"), "b")
    except Exception as e:
        return [TextContent(type="text", text=f"错误：{e}")]

    if name == "add":
        return [TextContent(type="text", text=f"{a} + {b} = {a + b}")]
    elif name == "subtract":
        return [TextContent(type="text", text=f"{a} - {b} = {a - b}")]
    elif name == "multiply":
        return [TextContent(type="text", text=f"{a} × {b} = {a * b}")]
    elif name == "divide":
        if b == 0:
            return [TextContent(type="text", text="错误：除数不能为0")]
        return [TextContent(type="text", text=f"{a} ÷ {b} = {a / b}")]
    return [TextContent(type="text", text=f"未知工具: {name}")]


class JsonRpcHandler:
    """处理单个客户端连接"""

    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.reader = reader
        self.writer = writer

    async def handle(self):
        print("[Calculator] 客户端已连接")

        try:
            while True:
                line = await self.reader.readline()
                if not line:
                    break

                try:
                    request = json.loads(line.decode())
                    response = await self.process_request(request)
                    if response:
                        self.writer.write((json.dumps(response) + "\n").encode())
                        await self.writer.drain()
                except Exception as e:
                    print(f"[Calculator] 错误: {e}")
        finally:
            print("[Calculator] 客户端断开")
            self.writer.close()
            await self.writer.wait_closed()

    async def process_request(self, request: dict) -> dict | None:
        method = request.get("method", "")
        req_id = request.get("id")
        params = request.get("params", {})

        if req_id is None:
            return None

        result = None
        error = None

        if method == "initialize":
            result = {
                "protocolVersion": "2024-11-05",
                "serverInfo": {"name": "calculator", "version": "1.0.0"},
                "capabilities": {"tools": {}}
            }
        elif method == "tools/list":
            tools = await list_tools()
            result = {"tools": [
                {"name": t.name, "description": t.description, "inputSchema": t.inputSchema}
                for t in tools
            ]}
        elif method == "tools/call":
            tool_name = params.get("name")
            arguments = params.get("arguments", {})
            contents = await call_tool(tool_name, arguments)
            result = {"content": [{"type": c.type, "text": c.text} for c in contents]}
        else:
            error = {"code": -32601, "message": f"Unknown method: {method}"}

        response = {"jsonrpc": "2.0", "id": req_id}
        if error:
            response["error"] = error
        else:
            response["result"] = result
        return response


async def main():
    if os.path.exists(SOCKET_PATH):
        os.unlink(SOCKET_PATH)

    async def client_handler(reader, writer):
        handler = JsonRpcHandler(reader, writer)
        await handler.handle()

    srv = await asyncio.start_unix_server(client_handler, path=SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o777)

    print("╔════════════════════════════════════════╗")
    print("║      Calculator MCP Server             ║")
    print("╚════════════════════════════════════════╝")
    print(f"[Calculator] Socket: {SOCKET_PATH}")
    print("[Calculator] 等待客户端连接... (Ctrl+C 退出)")

    def cleanup(sig, frame):
        print("\n[Calculator] 关闭服务器...")
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
