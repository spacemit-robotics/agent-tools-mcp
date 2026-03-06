#!/usr/bin/env python3
"""
MCP TimeService 服务 (Streamable HTTP) - FastMCP 版本

使用 FastMCP + Starlette + uvicorn 实现标准 MCP Streamable HTTP 协议。
提供时间相关的工具：获取当前时间、日期、时间戳等。
支持可选的注册中心自动注册/注销。

使用方式:
    # 启动服务（不使用注册中心）
    python http.py --port 8002

    # 启动服务（自动注册到注册中心）
    python http.py --port 8002 --register http://127.0.0.1:9000

依赖:
    pip install mcp starlette uvicorn
"""

import argparse
import contextlib
import json
import threading
import time
import atexit
from datetime import datetime, timezone
from urllib.request import urlopen, Request
from urllib.error import URLError

try:
    from zoneinfo import ZoneInfo
except ImportError:
    # Python 3.8 兼容
    from backports.zoneinfo import ZoneInfo

import uvicorn
from starlette.applications import Starlette
from starlette.routing import Mount
from starlette.middleware.cors import CORSMiddleware
from mcp.server.fastmcp import FastMCP

# 创建 FastMCP 服务器
mcp = FastMCP(
    "time-service",
    stateless_http=True,
    json_response=True
)


# ============================================================================
# MCP 工具定义
# ============================================================================

@mcp.tool()
def get_current_time(format: str = "%Y-%m-%d %H:%M:%S", timezone_name: str = "Asia/Shanghai", **kwargs) -> str:
    """
    获取当前时间，返回格式化的时间字符串

    Args:
        format: 时间格式，如 '%H:%M:%S' 或 '%Y-%m-%d %H:%M:%S'
        timezone_name: 时区名称，如 "Asia/Shanghai", "UTC"
    """
    try:
        tz = ZoneInfo(timezone_name)
        now = datetime.now(tz)
        return f"当前时间 ({timezone_name}): {now.strftime(format)}"
    except Exception as e:
        return f"错误：{e}"


@mcp.tool()
def get_current_date(**kwargs) -> str:
    """获取当前日期"""
    now = datetime.now()
    return f"当前日期: {now.strftime('%Y年%m月%d日')}"


@mcp.tool()
def get_timestamp(**kwargs) -> str:
    """获取当前 Unix 时间戳（秒）"""
    ts = int(datetime.now(timezone.utc).timestamp())
    return f"当前 Unix 时间戳: {ts}"


@mcp.tool()
def get_weekday(**kwargs) -> str:
    """获取今天是星期几"""
    weekdays = ['星期一', '星期二', '星期三', '星期四', '星期五', '星期六', '星期日']
    now = datetime.now()
    return f"今天是: {weekdays[now.weekday()]}"


@mcp.tool()
def format_timestamp(
    timestamp: int, format_str: str = "%Y-%m-%d %H:%M:%S",
    timezone_name: str = "Asia/Shanghai", **kwargs
) -> str:
    """
    将 Unix 时间戳格式化为可读时间

    Args:
        timestamp: Unix 时间戳（秒）
        format_str: 格式字符串，如 "%Y-%m-%d %H:%M:%S"
        timezone_name: 目标时区
    """
    try:
        tz = ZoneInfo(timezone_name)
        dt = datetime.fromtimestamp(timestamp, tz=tz)
        formatted = dt.strftime(format_str)
        return f"时间戳 {timestamp} = {formatted} ({timezone_name})"
    except Exception as e:
        return f"错误：{e}"


@mcp.tool()
def time_diff(timestamp1: int, timestamp2: int, **kwargs) -> str:
    """
    计算两个时间戳之间的差值

    Args:
        timestamp1: 第一个时间戳
        timestamp2: 第二个时间戳
    """
    diff = abs(timestamp2 - timestamp1)
    days = diff // 86400
    hours = (diff % 86400) // 3600
    minutes = (diff % 3600) // 60
    seconds = diff % 60

    parts = []
    if days > 0:
        parts.append(f"{days}天")
    if hours > 0:
        parts.append(f"{hours}小时")
    if minutes > 0:
        parts.append(f"{minutes}分钟")
    if seconds > 0 or not parts:
        parts.append(f"{seconds}秒")

    return f"时间差: {''.join(parts)}"


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
    parser = argparse.ArgumentParser(description='MCP TimeService (FastMCP)')
    parser.add_argument('--port', type=int, default=8002, help='Service port (default: 8002)')
    parser.add_argument('--register', type=str, default='', help='Registry URL')
    parser.add_argument('--name', type=str, default='TimeService', help='Service name')
    args = parser.parse_args()

    service_url = f"http://127.0.0.1:{args.port}/mcp"

    print("TimeService MCP Server (FastMCP + Streamable HTTP)")
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
