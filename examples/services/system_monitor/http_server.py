#!/usr/bin/env python3
"""
MCP SystemMonitor 服务 (Streamable HTTP) - FastMCP 版本

使用 FastMCP + Starlette + uvicorn 实现标准 MCP Streamable HTTP 协议。
提供完整的系统监控功能：CPU、内存、磁盘、网络、进程等。
支持可选的注册中心自动注册/注销。

使用方式:
    # 启动服务（不使用注册中心）
    python http.py --port 8003

    # 启动服务（自动注册到注册中心）
    python http.py --port 8003 --register http://127.0.0.1:9000

依赖:
    pip install mcp starlette uvicorn psutil
"""

import argparse
import contextlib
import json
import threading
import time
import os
import platform
import atexit
from datetime import datetime
from urllib.request import urlopen, Request
from urllib.error import URLError

import uvicorn
from starlette.applications import Starlette
from starlette.routing import Mount
from starlette.middleware.cors import CORSMiddleware
from mcp.server.fastmcp import FastMCP

# 尝试导入 psutil
try:
    import psutil
    HAS_PSUTIL = True
except ImportError:
    HAS_PSUTIL = False
    print("[Warning] psutil not installed. Install with: pip install psutil")

# 创建 FastMCP 服务器
mcp = FastMCP(
    "system-monitor",
    stateless_http=True,
    json_response=True
)


# ============================================================================
# 辅助函数
# ============================================================================

def format_bytes(bytes_val: int) -> str:
    """格式化字节数"""
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if bytes_val < 1024:
            return f"{bytes_val:.2f} {unit}"
        bytes_val /= 1024
    return f"{bytes_val:.2f} PB"


def require_psutil(func):
    """检查 psutil 是否可用的装饰器"""
    def wrapper(*args, **kwargs):
        if not HAS_PSUTIL:
            return "错误: 需要安装 psutil 才能使用此功能\n请运行: pip install psutil"
        return func(*args, **kwargs)
    wrapper.__name__ = func.__name__
    wrapper.__doc__ = func.__doc__
    return wrapper


# ============================================================================
# MCP 工具定义
# ============================================================================

@mcp.tool()
@require_psutil
def get_cpu_info(**kwargs) -> str:
    """获取 CPU 详细信息，包括使用率、核心数、频率"""
    cpu_percent = psutil.cpu_percent(interval=0.5)
    cpu_count = psutil.cpu_count(logical=False)
    cpu_count_logical = psutil.cpu_count(logical=True)
    cpu_freq = psutil.cpu_freq()

    result = f"""CPU 信息:
- 使用率: {cpu_percent}%
- 物理核心: {cpu_count}
- 逻辑核心: {cpu_count_logical}"""

    if cpu_freq:
        result += f"\n- 当前频率: {cpu_freq.current:.0f} MHz"

    return result


@mcp.tool()
@require_psutil
def get_memory_info(**kwargs) -> str:
    """获取内存使用信息，包括物理内存和 Swap"""
    mem = psutil.virtual_memory()
    swap = psutil.swap_memory()

    return f"""内存信息:
- 总内存: {format_bytes(mem.total)}
- 已使用: {format_bytes(mem.used)} ({mem.percent}%)
- 可用: {format_bytes(mem.available)}
- Swap 总量: {format_bytes(swap.total)}
- Swap 已用: {format_bytes(swap.used)} ({swap.percent}%)"""


@mcp.tool()
@require_psutil
def get_disk_info(**kwargs) -> str:
    """获取所有磁盘分区的使用信息"""
    partitions = psutil.disk_partitions()
    result = "磁盘信息:\n"

    for p in partitions:
        try:
            usage = psutil.disk_usage(p.mountpoint)
            result += f"""
[{p.device}] 挂载于 {p.mountpoint}
  - 文件系统: {p.fstype}
  - 总容量: {format_bytes(usage.total)}
  - 已使用: {format_bytes(usage.used)} ({usage.percent}%)
  - 可用: {format_bytes(usage.free)}"""
        except PermissionError:
            continue

    return result


@mcp.tool()
@require_psutil
def get_network_info(**kwargs) -> str:
    """获取网络接口信息和流量统计"""
    net_io = psutil.net_io_counters()
    net_if = psutil.net_if_addrs()

    result = f"""网络 I/O 总计:
- 发送: {format_bytes(net_io.bytes_sent)}
- 接收: {format_bytes(net_io.bytes_recv)}
- 发送包: {net_io.packets_sent}
- 接收包: {net_io.packets_recv}

网络接口:"""

    for iface, addrs in net_if.items():
        for addr in addrs:
            if addr.family.name == 'AF_INET':
                result += f"\n- {iface}: {addr.address}"
                break

    return result


@mcp.tool()
@require_psutil
def get_top_processes(limit: int = 5, sort_by: str = "memory", **kwargs) -> str:
    """
    获取资源占用最高的进程列表

    Args:
        limit: 返回的进程数量，默认 5
        sort_by: 排序方式，"memory" 或 "cpu"
    """
    processes = []
    for proc in psutil.process_iter(['pid', 'name', 'cpu_percent', 'memory_percent']):
        try:
            pinfo = proc.info
            processes.append(pinfo)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass

    # 排序
    if sort_by == "cpu":
        processes.sort(key=lambda x: x['cpu_percent'] or 0, reverse=True)
    else:
        processes.sort(key=lambda x: x['memory_percent'] or 0, reverse=True)

    result = f"Top {limit} 进程 (按 {sort_by} 排序):\n"
    for i, p in enumerate(processes[:limit], 1):
        result += f"{i}. [{p['pid']}] {p['name']}: CPU {p['cpu_percent']:.1f}%, 内存 {p['memory_percent']:.1f}%\n"

    return result


@mcp.tool()
@require_psutil
def get_system_overview(**kwargs) -> str:
    """获取系统概览，包括运行时长、资源使用率等"""
    boot_time = psutil.boot_time()
    boot_dt = datetime.fromtimestamp(boot_time)
    uptime = datetime.now() - boot_dt

    return f"""系统概览:
- 操作系统: {platform.system()} {platform.release()}
- 主机名: {platform.node()}
- 架构: {platform.machine()}
- Python: {platform.python_version()}
- 启动时间: {boot_dt.strftime('%Y-%m-%d %H:%M:%S')}
- 运行时长: {str(uptime).split('.')[0]}
- CPU 使用率: {psutil.cpu_percent()}%
- 内存使用率: {psutil.virtual_memory().percent}%"""


@mcp.tool()
def get_load_average(**kwargs) -> str:
    """获取系统负载（1/5/15 分钟平均值）"""
    try:
        load = os.getloadavg()
        return f"""系统负载:
- 1分钟: {load[0]:.2f}
- 5分钟: {load[1]:.2f}
- 15分钟: {load[2]:.2f}"""
    except Exception:
        if HAS_PSUTIL:
            try:
                load = psutil.getloadavg()
                return f"""系统负载:
- 1分钟: {load[0]:.2f}
- 5分钟: {load[1]:.2f}
- 15分钟: {load[2]:.2f}"""
            except Exception:
                pass
        return "系统负载: 当前平台不支持"


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
    parser = argparse.ArgumentParser(description='MCP SystemMonitor Service (FastMCP)')
    parser.add_argument('--port', type=int, default=8003, help='Service port (default: 8003)')
    parser.add_argument('--register', type=str, default='', help='Registry URL')
    parser.add_argument('--name', type=str, default='SystemMonitor', help='Service name')
    args = parser.parse_args()

    service_url = f"http://127.0.0.1:{args.port}/mcp"

    print("SystemMonitor MCP Server (FastMCP + Streamable HTTP)")
    print(f"Endpoint: http://0.0.0.0:{args.port}/mcp")

    if HAS_PSUTIL:
        print("[Server] psutil available - all features enabled")
    else:
        print("[Server] psutil not found - limited features")
        print("[Server] Install with: pip install psutil")

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
