/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCP SDK 多服务器示例
 *
 * 演示如何使用 MCPManager 管理多个 MCP 服务器
 */

#include <iostream>
#include <string>
#include <mcp_service.hpp>

using mcp::MCPManager;
using mcp::SDKInit;
using mcp::ServerState;

int main(int argc, char* argv[]) {
    auto print_usage = [](const char* prog) {
        std::cout
            << "Usage: " << prog << " [-h] [--calculator path] [--time path] [calculator_path time_path]\n"
            << "  -h                  Show this help message\n"
            << "  --calculator path   Path to calculator server script\n"
            << "  --calcultor path    (Deprecated) Kept for typo-compat; same as --calculator\n"
            << "  --time path         Path to time server script\n"
            << "  calculator_path     Positional calculator script path\n"
            << "  time_path           Positional time script path\n"
            << "Defaults:\n"
            << "  calculator: examples/services/calculator/stdio_server.py\n"
            << "  time:       examples/services/time/stdio_server.py\n";
    };

    // Simple argument parsing
    std::string calc_script = "examples/services/calculator/stdio_server.py";
    std::string time_script = "examples/services/time/stdio_server.py";

    std::string positional_calc;
    std::string positional_time;

    if (argc >= 2 && std::string(argv[1]) == "-h") {
        print_usage(argv[0]);
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "--calculator" || arg == "--calcultor") && i + 1 < argc) {
            calc_script = argv[++i];
        } else if (arg == "--time" && i + 1 < argc) {
            time_script = argv[++i];
        } else if (!arg.empty() && arg[0] != '-') {
            if (positional_calc.empty()) {
                positional_calc = arg;
            } else if (positional_time.empty()) {
                positional_time = arg;
            } else {
                std::cerr << "Unexpected extra argument: " << arg << std::endl;
                print_usage(argv[0]);
                return 1;
            }
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!positional_calc.empty()) {
        calc_script = positional_calc;
    }
    if (!positional_time.empty()) {
        time_script = positional_time;
    }

    // RAII 初始化
    SDKInit sdk;

    std::cout << "=== MCP SDK Multi-Server Example ===" << std::endl;

    // 创建管理器
    MCPManager manager;

    // 添加服务器
    manager.addStdioServer("calculator", {"python3", {calc_script}});
    manager.addStdioServer("time", {"python3", {time_script}});

    // 设置事件回调
    manager.onServerEvent([](const std::string& name, ServerState state) {
        const char* stateStr = "unknown";
        switch (state) {
            case ServerState::Disconnected: stateStr = "Disconnected"; break;
            case ServerState::Connecting: stateStr = "Connecting"; break;
            case ServerState::Initializing: stateStr = "Initializing"; break;
            case ServerState::Ready: stateStr = "Ready"; break;
            case ServerState::Reconnecting: stateStr = "Reconnecting"; break;
            case ServerState::Error: stateStr = "Error"; break;
        }
        std::cout << "[Event] " << name << " -> " << stateStr << std::endl;
    });

    // 启动所有服务器
    std::cout << "\nStarting servers..." << std::endl;
    manager.startAll();

    // 等待服务器就绪
    if (!manager.waitForAnyServer()) {
        std::cerr << "No servers available" << std::endl;
        return 1;
    }

    // 打印状态
    std::cout << "\n--- Server Status ---" << std::endl;
    for (const auto& status : manager.getStatuses()) {
        std::cout << status.name << ": ";
        switch (status.state) {
            case ServerState::Ready: std::cout << "Ready"; break;
            case ServerState::Disconnected: std::cout << "Disconnected"; break;
            default: std::cout << "Other"; break;
        }
        std::cout << " (" << status.tools.size() << " tools)" << std::endl;
    }

    // 获取所有工具
    auto tools = manager.getAllTools();
    std::cout << "\n--- All Tools (" << tools.size() << ") ---" << std::endl;
    for (const auto& tool : tools) {
        std::string server = manager.findServerForTool(tool.name);
        std::cout << "  [" << server << "] " << tool.name << ": " << tool.description << std::endl;
    }

    // 调用工具（自动路由）
    std::cout << "\n--- Tool Calls ---" << std::endl;

    auto result = manager.callTool("add", {{"a", 100}, {"b", 200}});
    if (result.success) {
        std::cout << "add(100, 200) = " << result.contents[0] << std::endl;
    }

    result = manager.callTool("get_current_time", {});
    if (result.success) {
        std::cout << "get_current_time() = " << result.contents[0] << std::endl;
    }

    result = manager.callTool("divide", {{"a", 100}, {"b", 3}});
    if (result.success) {
        std::cout << "divide(100, 3) = " << result.contents[0] << std::endl;
    }

    // 停止
    manager.stopAll();
    std::cout << "\nDone!" << std::endl;

    return 0;
}
