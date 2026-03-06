/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * HTTP Transport 实现 (Streamable HTTP)
 */

#include "../include/internal/transport_http.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace mcp {
namespace internal {

HttpTransport::HttpTransport(const HttpConfig& config)
    : config_(config) {}

HttpTransport::~HttpTransport() {
    disconnect();
}

bool HttpTransport::connect() {
    if (connected_) return true;

    curl_ = curl_easy_init();
    if (!curl_) {
        return false;
    }

    connected_ = true;
    return true;
}

void HttpTransport::disconnect() {
    if (!connected_) return;

    // 尝试终止会话
    if (!sessionId_.empty()) {
        terminateSession();
    }

    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }

    connected_ = false;
    sessionId_.clear();
    lastEventId_.clear();

    // 清空消息队列
    std::lock_guard<std::mutex> lock(queueMutex_);
    while (!messageQueue_.empty()) {
        messageQueue_.pop();
    }
}

bool HttpTransport::isConnected() const {
    return connected_;
}

size_t HttpTransport::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    auto* transport = static_cast<HttpTransport*>(userp);
    transport->responseBuffer_.append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

size_t HttpTransport::headerCallback(char* buffer, size_t size, size_t nitems, void* userp) {
    size_t totalSize = size * nitems;
    auto* transport = static_cast<HttpTransport*>(userp);
    transport->responseHeaders_.append(buffer, totalSize);
    return totalSize;
}

bool HttpTransport::send(const std::string& data) {
    if (!connected_ || !curl_) return false;

    // 重置响应缓冲
    responseBuffer_.clear();
    responseHeaders_.clear();

    // 设置 HTTP 头
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");

    // 添加会话 ID
    if (!sessionId_.empty()) {
        std::string sessionHeader = "Mcp-Session-Id: " + sessionId_;
        headers = curl_slist_append(headers, sessionHeader.c_str());
    }

    // 添加 Last-Event-ID (用于重连)
    if (!lastEventId_.empty()) {
        std::string eventHeader = "Last-Event-ID: " + lastEventId_;
        headers = curl_slist_append(headers, eventHeader.c_str());
    }

    // 添加用户自定义头
    for (const auto& [key, value] : config_.headers) {
        std::string header = key + ": " + value;
        headers = curl_slist_append(headers, header.c_str());
    }

    // 配置 libcurl
    curl_easy_setopt(curl_, CURLOPT_URL, config_.url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<int64_t>(data.size()));
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl_, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, static_cast<int64_t>(config_.timeout.count()));
    curl_easy_setopt(curl_, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);  // 强制 IPv4，避免 Linux IPv6 问题

    // 执行请求
    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        return false;
    }

    // 检查 HTTP 状态码
    int64_t httpCode = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &httpCode);
    if (httpCode < 200 || httpCode >= 300) {
        return false;
    }

    // 提取 session ID
    extractSessionId();

    // 获取响应类型
    char* contentType = nullptr;
    curl_easy_getinfo(curl_, CURLINFO_CONTENT_TYPE, &contentType);

    // 解析响应
    parseResponse(contentType ? contentType : "");

    return true;
}

void HttpTransport::extractSessionId() {
    // 从响应头中查找 Mcp-Session-Id
    std::istringstream stream(responseHeaders_);
    std::string line;

    while (std::getline(stream, line)) {
        // 移除行尾的 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 查找 Mcp-Session-Id 头
        const std::string prefix = "Mcp-Session-Id:";
        if (line.substr(0, prefix.size()) == prefix) {
            sessionId_ = line.substr(prefix.size());
            // 去除前导空格
            size_t start = sessionId_.find_first_not_of(" \t");
            if (start != std::string::npos) {
                sessionId_ = sessionId_.substr(start);
            }
            break;
        }

        // 不区分大小写的匹配
        std::string lowerLine = line;
        std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
        const std::string lowerPrefix = "mcp-session-id:";
        if (lowerLine.substr(0, lowerPrefix.size()) == lowerPrefix) {
            sessionId_ = line.substr(lowerPrefix.size());
            size_t start = sessionId_.find_first_not_of(" \t");
            if (start != std::string::npos) {
                sessionId_ = sessionId_.substr(start);
            }
            break;
        }
    }
}

void HttpTransport::parseResponse(const std::string& contentType) {
    if (contentType.find("text/event-stream") != std::string::npos) {
        // SSE 响应
        parseSSEData(responseBuffer_);
    } else {
        // JSON 响应
        if (!responseBuffer_.empty()) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            messageQueue_.push(responseBuffer_);
        }
    }
}

void HttpTransport::parseSSEData(const std::string& data) {
    std::istringstream stream(data);
    std::string line;
    std::string currentData;

    while (std::getline(stream, line)) {
        // 移除行尾的 \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) {
            // 空行表示事件结束
            if (!currentData.empty()) {
                std::lock_guard<std::mutex> lock(queueMutex_);
                messageQueue_.push(currentData);
                currentData.clear();
            }
            continue;
        }

        // 解析 SSE 字段
        if (line.substr(0, 6) == "data: ") {
            if (!currentData.empty()) {
                currentData += "\n";
            }
            currentData += line.substr(6);
        } else if (line.substr(0, 5) == "data:") {
            if (!currentData.empty()) {
                currentData += "\n";
            }
            currentData += line.substr(5);
        } else if (line.substr(0, 3) == "id:") {
            lastEventId_ = line.substr(3);
            // 去除前导空格
            size_t start = lastEventId_.find_first_not_of(" ");
            if (start != std::string::npos) {
                lastEventId_ = lastEventId_.substr(start);
            }
        } else if (line.substr(0, 4) == "id: ") {
            lastEventId_ = line.substr(4);
        }
    }

    // 处理最后一个事件（如果没有以空行结束）
    if (!currentData.empty()) {
        std::lock_guard<std::mutex> lock(queueMutex_);
        messageQueue_.push(currentData);
    }
}

std::string HttpTransport::receive(std::chrono::milliseconds timeout) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!messageQueue_.empty()) {
                std::string msg = messageQueue_.front();
                messageQueue_.pop();
                return msg;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed >= timeout) {
            return "";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool HttpTransport::terminateSession() {
    if (!connected_ || !curl_ || sessionId_.empty()) {
        return false;
    }

    // 设置 HTTP 头
    struct curl_slist* headers = nullptr;
    std::string sessionHeader = "Mcp-Session-Id: " + sessionId_;
    headers = curl_slist_append(headers, sessionHeader.c_str());

    // 配置 DELETE 请求
    curl_easy_setopt(curl_, CURLOPT_URL, config_.url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, nullptr);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, 0L);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    // 重置请求方法
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, nullptr);

    return res == CURLE_OK;
}

}  // namespace internal

// 工厂函数实现
std::unique_ptr<Transport> createHttpTransport(const HttpConfig& config) {
    return std::make_unique<internal::HttpTransport>(config);
}

}  // namespace mcp
