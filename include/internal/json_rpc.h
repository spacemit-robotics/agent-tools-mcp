/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * JSON-RPC 协议层内部实现
 */

#ifndef JSON_RPC_H
#define JSON_RPC_H

#include <atomic>
#include <string>
#include "../mcp_service.hpp"

namespace mcp {
namespace internal {

/**
 * JSON-RPC 处理器
 * 封装请求/响应序列化和 ID 管理
 */
class JsonRpcHandler {
public:
    explicit JsonRpcHandler(Transport& transport);

    /**
     * 发送请求并等待响应
     * @param method 方法名
     * @param params 参数
     * @param timeout 超时时间
     * @return 响应 JSON（包含 result 或 error）
     */
    json sendRequest(
        const std::string& method,
        const json& params,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /**
     * 发送通知（无需响应）
     * @param method 方法名
     * @param params 参数
     */
    void sendNotification(const std::string& method, const json& params);

    /**
     * 获取下一个请求 ID
     */
    int nextId();

private:
    Transport& transport_;
    std::atomic<int> requestId_{0};

    /**
     * 读取响应
     * @param expectedId 期望的响应 ID
     * @param timeout 超时时间
     * @return 响应 JSON
     */
    json readResponse(int expectedId, std::chrono::milliseconds timeout);
};

}  // namespace internal
}  // namespace mcp

#endif  // JSON_RPC_H
