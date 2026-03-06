#!/usr/bin/env python3
"""
MCP Calculator 服务 (Streamable HTTP) - FastMCP 版本

使用 FastMCP + Starlette + uvicorn 实现标准 MCP Streamable HTTP 协议。
支持可选的注册中心自动注册/注销。

使用方式:
    # 启动服务（不使用注册中心）
    python http.py --port 8001

    # 启动服务（自动注册到注册中心）
    python http.py --port 8001 --register http://127.0.0.1:9000

依赖:
    pip install mcp starlette uvicorn
"""

import argparse
import contextlib
import json
import threading
import time
import atexit
from urllib.request import urlopen, Request
from urllib.error import URLError

import uvicorn
from starlette.applications import Starlette
from starlette.routing import Mount
from starlette.middleware.cors import CORSMiddleware
from mcp.server.fastmcp import FastMCP

# 创建 FastMCP 服务器
mcp = FastMCP(
    "calculator",
    stateless_http=True,
    json_response=True
)


# ============================================================================
# 辅助函数
# ============================================================================

def parse_number(value, param_name: str) -> float:
    """解析数字参数"""
    if isinstance(value, (int, float)):
        return float(value)

    if isinstance(value, str):
        value = value.strip()
        operators = ['+', '-', '*', '/', '(', ')']
        check_value = value[1:] if value.startswith('-') else value

        for op in operators:
            if op in check_value:
                raise ValueError(
                    f"参数 '{param_name}' 不能是表达式 '{value}'，必须是纯数字"
                )

        try:
            return float(value)
        except ValueError:
            raise ValueError(f"参数 '{param_name}' = '{value}' 不是有效数字")

    raise ValueError(f"参数 '{param_name}' 类型错误")


# ============================================================================
# MCP 工具定义
# ============================================================================

@mcp.tool()
def add(a: float, b: float, **kwargs) -> str:
    """将两个数字相加 (a + b)"""
    result = a + b
    return f"{a} + {b} = {result}"


@mcp.tool()
def subtract(a: float, b: float, **kwargs) -> str:
    """从第一个数字减去第二个数字 (a - b)"""
    result = a - b
    return f"{a} - {b} = {result}"


@mcp.tool()
def multiply(a: float, b: float, **kwargs) -> str:
    """将两个数字相乘 (a × b)"""
    result = a * b
    return f"{a} × {b} = {result}"


@mcp.tool()
def divide(a: float, b: float, **kwargs) -> str:
    """将第一个数字除以第二个数字 (a ÷ b)"""
    if b == 0:
        return "错误: 除数不能为零"
    result = a / b
    return f"{a} ÷ {b} = {result}"


# ============================================================================
# Starlette 应用
# ============================================================================

@contextlib.asynccontextmanager
async def lifespan(app: Starlette):
    async with mcp.session_manager.run():
        yield


# 创建 Starlette 应用
app = Starlette(
    routes=[Mount("/", app=mcp.streamable_http_app())],
    lifespan=lifespan,
)

# 添加 CORS 支持
app = CORSMiddleware(
    app,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
    expose_headers=["Mcp-Session-Id"],
)


# ============================================================================
# 注册中心集成
# ============================================================================

def register_to_registry(registry_url: str, name: str, service_url: str) -> bool:
    """注册到注册中心"""
    try:
        data = json.dumps({"name": name, "type": "http", "url": service_url}).encode('utf-8')
        req = Request(f"{registry_url}/mcp/register", data=data, method='POST')
        req.add_header('Content-Type', 'application/json')
        with urlopen(req, timeout=5) as resp:
            if resp.status == 200:
                print(f"[Registry] Registered as '{name}' -> {service_url}")
                return True
    except URLError as e:
        print(f"[Registry] Failed to register: {e}")
    return False


def unregister_from_registry(registry_url: str, name: str) -> bool:
    """从注册中心注销"""
    try:
        req = Request(f"{registry_url}/mcp/unregister/{name}", method='DELETE')
        with urlopen(req, timeout=5) as resp:
            if resp.status == 200:
                print(f"[Registry] Unregistered '{name}'")
                return True
    except URLError as e:
        print(f"[Registry] Failed to unregister: {e}")
    return False


def heartbeat_thread(registry_url: str, name: str, interval: int = 10):
    """心跳线程"""
    while True:
        time.sleep(interval)
        try:
            req = Request(f"{registry_url}/mcp/heartbeat/{name}", method='POST')
            with urlopen(req, timeout=5):
                pass
        except URLError:
            pass


# ============================================================================
# 主函数
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='MCP Calculator Service (FastMCP)')
    parser.add_argument('--port', type=int, default=8001, help='Service port (default: 8001)')
    parser.add_argument('--register', type=str, default='', help='Registry URL (e.g., http://127.0.0.1:9000)')
    parser.add_argument('--name', type=str, default='Calculator', help='Service name (default: Calculator)')
    args = parser.parse_args()

    service_url = f"http://127.0.0.1:{args.port}/mcp"

    print("Calculator MCP Server (FastMCP + Streamable HTTP)")
    print(f"Endpoint: http://0.0.0.0:{args.port}/mcp")

    # 注册到注册中心
    if args.register:
        if register_to_registry(args.register, args.name, service_url):
            # 启动心跳线程
            t = threading.Thread(target=heartbeat_thread, args=(args.register, args.name), daemon=True)
            t.start()
            # 注册退出钩子
            atexit.register(lambda: unregister_from_registry(args.register, args.name))

    print("Press Ctrl+C to stop")

    # 启动 uvicorn
    uvicorn.run(app, host="0.0.0.0", port=args.port, log_level="warning")


if __name__ == '__main__':
    main()
