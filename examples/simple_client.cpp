/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCP SDK 简单客户端示例
 *
 * 演示如何使用 MCPClient 连接到单个 MCP 服务器
 */

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <mcp_service.hpp>

using mcp::ClientConfig;
using mcp::createStdioTransport;
using mcp::MCPClient;
using mcp::SDKInit;
using mcp::StdioConfig;

int main(int argc, char* argv[]) {
    auto print_usage = [argv]() {
        std::cout
            << "Usage: " << argv[0] << " [-h] [script_path]\n"
            << "  -h            Show this help message\n"
            << "  script_path   Path to MCP python server script "
            << "(default: examples/services/calculator/stdio_server.py)\n";
    };

    // RAII 初始化
    SDKInit sdk;

    // 默认使用 stdio 连接 calculator stdio 服务
    std::string command = "python3";
    std::string script = "examples/services/calculator/stdio_server.py";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h") {
            print_usage();
            return 0;
        }
        if (!arg.empty() && arg[0] != '-') {
            script = arg;
            break;
        }
    }

    std::cout << "=== MCP SDK Simple Client Example ===" << std::endl;
    std::cout << "Connecting to: " << command << " " << script << std::endl;

    // 创建 Stdio Transport
    StdioConfig config;
    config.command = command;
    config.args = {script};

    auto transport = createStdioTransport(config);

    // 创建客户端
    ClientConfig clientConfig;
    clientConfig.name = "example-client";

    MCPClient client(std::move(transport), clientConfig);

    // 连接
    if (!client.connect()) {
        std::cerr << "Failed to connect" << std::endl;
        return 1;
    }
    std::cout << "Connected!" << std::endl;

    // 等待服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 初始化
    if (!client.initialize()) {
        std::cerr << "Failed to initialize" << std::endl;
        return 1;
    }

    std::cout << "Server: " << client.serverInfo().name
        << " v" << client.serverInfo().version << std::endl;

    // 列出工具
    auto tools = client.listTools();
    std::cout << "\nAvailable tools (" << tools.size() << "):" << std::endl;
    for (const auto& tool : tools) {
        std::cout << "  - " << tool.name << ": " << tool.description << std::endl;
    }

    // 调用工具
    std::cout << "\n--- Calling add(10, 20) ---" << std::endl;
    auto result = client.callTool("add", {{"a", 10}, {"b", 20}});
    if (result.success) {
        std::cout << "Result: " << result.contents[0] << std::endl;
    } else {
        std::cerr << "Error: " << result.error << std::endl;
    }

    std::cout << "\n--- Calling multiply(7, 8) ---" << std::endl;
    result = client.callTool("multiply", {{"a", 7}, {"b", 8}});
    if (result.success) {
        std::cout << "Result: " << result.contents[0] << std::endl;
    } else {
        std::cerr << "Error: " << result.error << std::endl;
    }

    // 关闭
    client.shutdown();
    std::cout << "\nDone!" << std::endl;

    return 0;
}
