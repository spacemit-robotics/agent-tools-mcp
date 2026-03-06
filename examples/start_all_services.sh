#!/bin/bash
#
# 一键启动所有 MCP 服务（包含注册中心）
#
# 使用方式:
#   ./start_all_services.sh        # 启动所有服务
#   ./start_all_services.sh stop   # 停止所有服务
#

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# PID 文件目录
PID_DIR="/tmp/mcp_services"
mkdir -p "$PID_DIR"

start_services() {
    echo "=== 启动 MCP 服务 ==="
    echo ""

    # 1. 启动注册中心
    echo "[1/4] 启动注册中心 (port 9000)..."
    python3 registry_server.py > /tmp/mcp_registry.log 2>&1 &
    echo $! > "$PID_DIR/registry.pid"
    sleep 1

    # 2. 启动 Calculator 服务
    echo "[2/4] 启动 Calculator 服务 (port 8001)..."
    python3 services/calculator/http_server.py --port 8001 --register http://127.0.0.1:9000 > /tmp/mcp_calculator.log 2>&1 &
    echo $! > "$PID_DIR/calculator.pid"
    sleep 0.5

    # 3. 启动 TimeService 服务
    echo "[3/4] 启动 TimeService 服务 (port 8002)..."
    python3 services/time/http_server.py --port 8002 --register http://127.0.0.1:9000 > /tmp/mcp_time.log 2>&1 &
    echo $! > "$PID_DIR/time.pid"
    sleep 0.5

    # 4. 启动 SystemMonitor 服务
    echo "[4/4] 启动 SystemMonitor 服务 (port 8003)..."
    python3 services/system_monitor/http_server.py --port 8003 --register http://127.0.0.1:9000 > /tmp/mcp_system.log 2>&1 &
    echo $! > "$PID_DIR/system.pid"
    sleep 0.5

    echo ""
    echo "=== 所有服务已启动 ==="
    echo ""
    echo "服务列表:"
    echo "  - 注册中心:      http://127.0.0.1:9000/mcp/services"
    echo "  - Calculator:    http://127.0.0.1:8001/mcp"
    echo "  - TimeService:   http://127.0.0.1:8002/mcp"
    echo "  - SystemMonitor: http://127.0.0.1:8003/mcp"
    echo ""
    echo "日志文件:"
    echo "  - /tmp/mcp_registry.log"
    echo "  - /tmp/mcp_calculator.log"
    echo "  - /tmp/mcp_time.log"
    echo "  - /tmp/mcp_system.log"
    echo ""
    echo "运行 voice_chat_aec:"
    echo "  ./build/bin/voice_chat_aec --mcp-config mcp/examples/configs/config_registry.json"
    echo ""
    echo "停止所有服务:"
    echo "  $0 stop"
}

stop_services() {
    echo "=== 停止 MCP 服务 ==="
    echo ""

    for service in registry calculator time system; do
        pid_file="$PID_DIR/${service}.pid"
        if [ -f "$pid_file" ]; then
            pid=$(cat "$pid_file")
            if kill -0 "$pid" 2>/dev/null; then
                echo "停止 $service (PID: $pid)..."
                kill "$pid" 2>/dev/null
            fi
            rm -f "$pid_file"
        fi
    done

    # 额外清理：查找并杀死可能残留的进程
    pkill -f "registry_server.py" 2>/dev/null
    pkill -f "services/calculator/http_server.py" 2>/dev/null
    pkill -f "services/time/http_server.py" 2>/dev/null
    pkill -f "services/system_monitor/http_server.py" 2>/dev/null

    echo ""
    echo "所有服务已停止"
}

check_status() {
    echo "=== MCP 服务状态 ==="
    echo ""

    for service in registry calculator time system; do
        pid_file="$PID_DIR/${service}.pid"
        if [ -f "$pid_file" ]; then
            pid=$(cat "$pid_file")
            if kill -0 "$pid" 2>/dev/null; then
                echo "✓ $service: 运行中 (PID: $pid)"
            else
                echo "✗ $service: 已停止"
            fi
        else
            echo "✗ $service: 未启动"
        fi
    done

    echo ""
    # 检查注册中心
    if curl -s http://127.0.0.1:9000/mcp/services > /dev/null 2>&1; then
        echo "注册中心已注册的服务:"
        curl -s http://127.0.0.1:9000/mcp/services | python3 -c "import sys,json; data=json.load(sys.stdin); [print(f'  - {s[\"name\"]}: {s[\"url\"]}') for s in data.get('services', [])]" 2>/dev/null || echo "  (无法解析)"
    fi
}

case "${1:-start}" in
    start)
        start_services
        ;;
    stop)
        stop_services
        ;;
    restart)
        stop_services
        sleep 1
        start_services
        ;;
    status)
        check_status
        ;;
    *)
        echo "用法: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
