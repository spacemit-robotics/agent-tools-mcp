#!/usr/bin/env python3
"""
MCP Calculator Server - 加减乘除计算器
使用官方MCP SDK，stdio协议
增强版：更好的输入验证和错误提示
"""

import asyncio
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent


server = Server("calculator")


def parse_number(value, param_name: str) -> float:
    """
    解析数字参数，支持int/float/str格式
    如果是表达式字符串则报错
    """
    if isinstance(value, (int, float)):
        return float(value)

    if isinstance(value, str):
        # 去除空格
        value = value.strip()

        # 检查是否包含运算符（表达式）
        operators = ['+', '-', '*', '/', '(', ')']
        # 允许负数开头的减号
        check_value = value[1:] if value.startswith('-') else value

        for op in operators:
            if op in check_value:
                raise ValueError(
                    f"参数 '{param_name}' 不能是表达式 '{value}'，必须是纯数字。"
                    f"请分步计算，每次只传入数字。"
                )

        try:
            return float(value)
        except ValueError:
            raise ValueError(f"参数 '{param_name}' = '{value}' 不是有效数字")

    raise ValueError(f"参数 '{param_name}' 类型错误，期望数字，得到 {type(value).__name__}")


@server.list_tools()
async def list_tools() -> list[Tool]:
    """返回可用的工具列表"""
    return [
        Tool(
            name="add",
            description="计算两个数的和 (a + b)，参数必须是数字",
            inputSchema={
                "type": "object",
                "properties": {
                    "a": {"type": "number", "description": "第一个加数（必须是数字）"},
                    "b": {"type": "number", "description": "第二个加数（必须是数字）"},
                },
                "required": ["a", "b"],
            },
        ),
        Tool(
            name="subtract",
            description="计算两个数的差 (a - b)，参数必须是数字",
            inputSchema={
                "type": "object",
                "properties": {
                    "a": {"type": "number", "description": "被减数（必须是数字）"},
                    "b": {"type": "number", "description": "减数（必须是数字）"},
                },
                "required": ["a", "b"],
            },
        ),
        Tool(
            name="multiply",
            description="计算两个数的积 (a * b)，参数必须是数字",
            inputSchema={
                "type": "object",
                "properties": {
                    "a": {"type": "number", "description": "第一个因数（必须是数字）"},
                    "b": {"type": "number", "description": "第二个因数（必须是数字）"},
                },
                "required": ["a", "b"],
            },
        ),
        Tool(
            name="divide",
            description="计算两个数的商 (a / b)，参数必须是数字",
            inputSchema={
                "type": "object",
                "properties": {
                    "a": {"type": "number", "description": "被除数（必须是数字）"},
                    "b": {"type": "number", "description": "除数（必须是数字，不能为0）"},
                },
                "required": ["a", "b"],
            },
        ),
    ]


@server.call_tool()
async def call_tool(name: str, arguments: dict) -> list[TextContent]:
    """执行工具调用"""

    # 检查参数是否存在
    if "a" not in arguments or "b" not in arguments:
        return [TextContent(type="text", text="错误：缺少参数 a 或 b")]

    # 解析并验证参数
    try:
        a = parse_number(arguments["a"], "a")
        b = parse_number(arguments["b"], "b")
    except ValueError as e:
        return [TextContent(type="text", text=f"参数错误：{e}")]

    # 执行计算
    if name == "add":
        result = a + b
        return [TextContent(type="text", text=f"{a} + {b} = {result}")]

    elif name == "subtract":
        result = a - b
        return [TextContent(type="text", text=f"{a} - {b} = {result}")]

    elif name == "multiply":
        result = a * b
        return [TextContent(type="text", text=f"{a} × {b} = {result}")]

    elif name == "divide":
        if b == 0:
            return [TextContent(type="text", text="错误：除数不能为0")]
        result = a / b
        return [TextContent(type="text", text=f"{a} ÷ {b} = {result}")]

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
