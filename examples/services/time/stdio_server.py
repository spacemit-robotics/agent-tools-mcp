#!/usr/bin/env python3
"""
MCP Time Server - 时间服务
使用官方MCP SDK，stdio协议
"""

import asyncio
from datetime import datetime
from zoneinfo import ZoneInfo  # Python 3.9+ 内置，无需安装pytz
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent


server = Server("time-server")


@server.list_tools()
async def list_tools() -> list[Tool]:
    """返回可用的工具列表"""
    return [
        Tool(
            name="get_current_time",
            description="获取当前时间，可指定时区",
            inputSchema={
                "type": "object",
                "properties": {
                    "timezone": {
                        "type": "string",
                        "description": "时区名称，如 'Asia/Shanghai'、'America/New_York'、'UTC'。默认为本地时间",
                    }
                },
                "required": [],
            },
        ),
        Tool(
            name="get_timestamp",
            description="获取当前Unix时间戳（秒）",
            inputSchema={
                "type": "object",
                "properties": {},
                "required": [],
            },
        ),
        Tool(
            name="format_time",
            description="将时间戳格式化为指定格式的时间字符串",
            inputSchema={
                "type": "object",
                "properties": {
                    "timestamp": {
                        "type": "number",
                        "description": "Unix时间戳（秒）"
                    },
                    "format": {
                        "type": "string",
                        "description": "时间格式，如 '%Y-%m-%d %H:%M:%S'",
                        "default": "%Y-%m-%d %H:%M:%S"
                    },
                    "timezone": {
                        "type": "string",
                        "description": "目标时区",
                    }
                },
                "required": ["timestamp"],
            },
        ),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    """执行工具调用"""

    if name == "get_current_time":
        timezone_str = arguments.get("timezone")

        try:
            if timezone_str:
                tz = ZoneInfo(timezone_str)
                now = datetime.now(tz)
            else:
                now = datetime.now().astimezone()

            result = now.strftime("%Y-%m-%d %H:%M:%S %Z")
            return [TextContent(type="text", text=f"当前时间：{result}")]

        except Exception as e:
            return [TextContent(
                type="text",
                text=f"错误：{str(e)}。常用时区：Asia/Shanghai, America/New_York, Europe/London, UTC"
            )]

    elif name == "get_timestamp":
        import time
        ts = int(time.time())
        return [TextContent(type="text", text=f"当前时间戳：{ts}")]

    elif name == "format_time":
        timestamp = arguments.get("timestamp")
        fmt = arguments.get("format", "%Y-%m-%d %H:%M:%S")
        timezone_str = arguments.get("timezone")

        if timestamp is None:
            return [TextContent(type="text", text="错误：缺少 timestamp 参数")]

        try:
            dt = datetime.fromtimestamp(float(timestamp))
            if timezone_str:
                tz = ZoneInfo(timezone_str)
                dt = dt.astimezone(tz)

            result = dt.strftime(fmt)
            return [TextContent(type="text", text=f"格式化时间：{result}")]

        except Exception as e:
            return [TextContent(type="text", text=f"错误：{str(e)}")]

    else:
        return [TextContent(type="text", text=f"错误：未知工具 '{name}'")]


async def main():
    """主函数 - 启动stdio服务器"""
    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            server.create_initialization_options(),
        )


if __name__ == "__main__":
    asyncio.run(main())
