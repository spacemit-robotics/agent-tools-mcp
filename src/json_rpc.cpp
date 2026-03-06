/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * JSON-RPC 协议层实现
 */

#include "../include/internal/json_rpc.h"
#include <string>
#include <thread>

namespace mcp {
namespace internal {

JsonRpcHandler::JsonRpcHandler(Transport& transport)
    : transport_(transport) {}

int JsonRpcHandler::nextId() {
    return ++requestId_;
}

json JsonRpcHandler::sendRequest(
        const std::string& method,
        const json& params,
        std::chrono::milliseconds timeout) {
    int id = nextId();

    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    std::string message = request.dump() + "\n";
    if (!transport_.send(message)) {
        return {{"error", {{"code", -1}, {"message", "Send failed"}}}};
    }

    return readResponse(id, timeout);
}

void JsonRpcHandler::sendNotification(const std::string& method, const json& params) {
    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };

    std::string message = notification.dump() + "\n";
    transport_.send(message);
}

json JsonRpcHandler::readResponse(int expectedId, std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed > timeout) {
            return {{"error", {{"code", -1}, {"message", "Timeout"}}}};
        }

        std::string line = transport_.receive(std::chrono::milliseconds(100));
        if (line.empty()) {
            continue;
        }

        try {
            json response = json::parse(line);
            if (response.contains("id") && response["id"] == expectedId) {
                return response;
            }
            // 忽略不匹配的响应（可能是通知）
        } catch (const json::parse_error&) {
            // 忽略解析错误
        }
    }
}

}  // namespace internal
}  // namespace mcp
