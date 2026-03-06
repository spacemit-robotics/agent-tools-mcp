#!/usr/bin/env python3
"""
MCP 服务注册中心

简单的 Flask 服务，用于 MCP 服务的动态发现。

使用方式:
    # 启动注册中心
    python registry_server.py

    # 注册服务
    curl -X POST http://127.0.0.1:9000/mcp/register \
         -H "Content-Type: application/json" \
         -d '{"name": "Calculator", "type": "http", "url": "http://127.0.0.1:8001/mcp"}'

    # 获取服务列表
    curl http://127.0.0.1:9000/mcp/services

    # 注销服务
    curl -X DELETE http://127.0.0.1:9000/mcp/unregister/Calculator
"""

from flask import Flask, jsonify, request
from datetime import datetime
import threading
import time

app = Flask(__name__)

# 服务注册表
services = {}
services_lock = threading.Lock()

# 服务超时时间（秒），超过此时间未心跳则移除
SERVICE_TIMEOUT = 30


@app.route('/mcp/services', methods=['GET'])
def get_services():
    """获取所有在线服务列表"""
    with services_lock:
        # 过滤超时的服务
        now = time.time()
        active_services = [
            {"name": s["name"], "type": s["type"], "url": s["url"]}
            for s in services.values()
            if now - s["last_seen"] < SERVICE_TIMEOUT
        ]
        return jsonify({"services": active_services})


@app.route('/mcp/register', methods=['POST'])
def register_service():
    """注册服务"""
    data = request.json
    if not data or 'name' not in data:
        return jsonify({"error": "Missing 'name' field"}), 400

    name = data['name']
    with services_lock:
        services[name] = {
            "name": name,
            "type": data.get("type", "http"),
            "url": data.get("url", ""),
            "last_seen": time.time()
        }

    print(f"[{datetime.now().strftime('%H:%M:%S')}] Registered: {name} -> {data.get('url', '')}")
    return jsonify({"status": "ok", "message": f"Service '{name}' registered"})


@app.route('/mcp/unregister/<name>', methods=['DELETE'])
def unregister_service(name):
    """注销服务"""
    with services_lock:
        if name in services:
            del services[name]
            print(f"[{datetime.now().strftime('%H:%M:%S')}] Unregistered: {name}")
            return jsonify({"status": "ok", "message": f"Service '{name}' unregistered"})
        else:
            return jsonify({"error": f"Service '{name}' not found"}), 404


@app.route('/mcp/heartbeat/<name>', methods=['POST'])
def heartbeat(name):
    """服务心跳"""
    with services_lock:
        if name in services:
            services[name]["last_seen"] = time.time()
            return jsonify({"status": "ok"})
        else:
            return jsonify({"error": f"Service '{name}' not found"}), 404


def cleanup_thread():
    """后台线程：清理超时的服务"""
    while True:
        time.sleep(10)
        now = time.time()
        with services_lock:
            expired = [
                name for name, s in services.items()
                if now - s["last_seen"] >= SERVICE_TIMEOUT
            ]
            for name in expired:
                print(f"[{datetime.now().strftime('%H:%M:%S')}] Expired: {name}")
                del services[name]


if __name__ == '__main__':
    # 启动清理线程
    t = threading.Thread(target=cleanup_thread, daemon=True)
    t.start()

    print("MCP Registry Server")
    print("=" * 40)
    print("Listening on http://0.0.0.0:9000")
    print(f"Service timeout: {SERVICE_TIMEOUT}s")
    print()
    print("Endpoints:")
    print("  GET  /mcp/services          - List services")
    print("  POST /mcp/register          - Register service")
    print("  DELETE /mcp/unregister/<n>  - Unregister service")
    print("  POST /mcp/heartbeat/<n>     - Service heartbeat")
    print("=" * 40)
    print()

    app.run(host='0.0.0.0', port=9000, threaded=True)
