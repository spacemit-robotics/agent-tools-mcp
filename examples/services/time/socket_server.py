#!/usr/bin/env python3
"""
MCP Time Server - Unix Socket 版本
独立运行，等待客户端连接
"""

import asyncio
import json
import os
import signal
import sys
import time
from datetime import datetime
from zoneinfo import ZoneInfo
from mcp.server import Server
from mcp.types import Tool, TextContent

# Socket 路径
SOCKET_PATH = "/tmp/mcp_time.sock"

server = Server("time-server")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return [
        Tool(
            name="get_current_time",
            description="获取当前时间，可指定时区",
            inputSchema={
                "type": "object",
                "properties": {
                    "timezone": {
                        "type": "string",
                        "description": "时区，如 Asia/Shanghai, America/New_York, UTC"
                    }
                },
                "required": []
            }
        ),
        Tool(
            name="get_timestamp",
            description="获取当前 Unix 时间戳（秒）",
            inputSchema={"type": "object", "properties": {}, "required": []}
        ),
        Tool(
            name="format_time",
            description="将时间戳格式化为时间字符串",
            inputSchema={
                "type": "object",
                "properties": {
                    "timestamp": {"type": "number", "description": "Unix 时间戳"},
                    "format": {"type": "string", "description": "格式，默认 %Y-%m-%d %H:%M:%S"},
                    "timezone": {"type": "string", "description": "时区"}
                },
                "required": ["timestamp"]
            }
        ),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    if name == "get_current_time":
        tz_str = arguments.get("timezone")
        try:
            if tz_str:
                tz = ZoneInfo(tz_str)
                now = datetime.now(tz)
            else:
                now = datetime.now().astimezone()
            return [TextContent(type="text", text=f"当前时间：{now.strftime('%Y-%m-%d %H:%M:%S %Z')}")]
        except Exception as e:
            return [TextContent(type="text", text=f"错误：{e}")]

    elif name == "get_timestamp":
        ts = int(time.time())
        return [TextContent(type="text", text=f"时间戳：{ts}")]

    elif name == "format_time":
        timestamp = arguments.get("timestamp")
        fmt = arguments.get("format", "%Y-%m-%d %H:%M:%S")
        tz_str = arguments.get("timezone")
        try:
            dt = datetime.fromtimestamp(float(timestamp))
            if tz_str:
                dt = dt.astimezone(ZoneInfo(tz_str))
            return [TextContent(type="text", text=f"格式化时间：{dt.strftime(fmt)}")]
        except Exception as e:
            return [TextContent(type="text", text=f"错误：{e}")]

    return [TextContent(type="text", text=f"未知工具: {name}")]


class JsonRpcHandler:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        self.reader = reader
        self.writer = writer

    async def handle(self):
        print("[TimeServer] 客户端已连接")

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
                    print(f"[TimeServer] 错误: {e}")
        finally:
            print("[TimeServer] 客户端断开")
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
                "serverInfo": {"name": "time-server", "version": "1.0.0"},
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
    print("║        Time MCP Server                 ║")
    print("╚════════════════════════════════════════╝")
    print(f"[TimeServer] Socket: {SOCKET_PATH}")
    print("[TimeServer] 等待客户端连接... (Ctrl+C 退出)")

    def cleanup(sig, frame):
        print("\n[TimeServer] 关闭服务器...")
        if os.path.exists(SOCKET_PATH):
            os.unlink(SOCKET_PATH)
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
