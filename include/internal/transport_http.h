/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * HTTP Transport 内部实现 (Streamable HTTP)
 *
 * 实现 MCP 官方推荐的 Streamable HTTP 传输协议
 * 参考: https://modelcontextprotocol.io/legacy/concepts/transports
 */

#ifndef TRANSPORT_HTTP_H
#define TRANSPORT_HTTP_H

#include <curl/curl.h>
#include <mutex>
#include <queue>
#include <string>
#include "../mcp_service.hpp"

namespace mcp {
namespace internal {

/**
 * HTTP Transport 实现
 *
 * 特性:
 * - 单一 /mcp 端点
 * - Mcp-Session-Id 会话管理
 * - 支持 JSON 和 SSE 响应
 * - Last-Event-ID 断线重连
 */
class HttpTransport : public Transport {
public:
    explicit HttpTransport(const HttpConfig& config);
    ~HttpTransport() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const std::string& data) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    std::string typeName() const override { return "http"; }

    /**
     * 获取当前会话 ID
     */
    const std::string& sessionId() const { return sessionId_; }

    /**
     * 终止会话 (发送 DELETE 请求)
     */
    bool terminateSession();

private:
    HttpConfig config_;
    CURL* curl_ = nullptr;
    bool connected_ = false;

    // 会话管理
    std::string sessionId_;
    std::string lastEventId_;

    // 响应缓冲队列
    std::queue<std::string> messageQueue_;
    mutable std::mutex queueMutex_;

    // HTTP 响应处理
    std::string responseBuffer_;
    std::string responseHeaders_;

    // libcurl 回调
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    static size_t headerCallback(char* buffer, size_t size, size_t nitems, void* userp);

    // SSE 解析
    void parseResponse(const std::string& contentType);
    void parseSSEData(const std::string& data);

    // 从响应头提取 session ID
    void extractSessionId();
};

}  // namespace internal
}  // namespace mcp

#endif  // TRANSPORT_HTTP_H
