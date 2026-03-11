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

JsonRpcHandler::~JsonRpcHandler() {
    stop();
}

int JsonRpcHandler::nextId() {
    return ++requestId_;
}

json JsonRpcHandler::sendRequest(
        const std::string& method,
        const json& params,
        std::chrono::milliseconds timeout) {
    start();

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
    start();

    json notification = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params}
    };

    std::string message = notification.dump() + "\n";
    transport_.send(message);
}

void JsonRpcHandler::setNotificationHandler(NotificationHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    notificationHandler_ = std::move(handler);
}

void JsonRpcHandler::start() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    if (notificationThread_.joinable()) {
        notificationThread_.join();
    }

    readerThread_ = std::thread([this]() { readerLoop(); });
    notificationThread_ = std::thread([this]() { notificationLoop(); });
}

void JsonRpcHandler::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);

    running_.exchange(false);

    responseCv_.notify_all();
    notificationCv_.notify_all();

    if (readerThread_.joinable()) {
        readerThread_.join();
    }
    if (notificationThread_.joinable()) {
        notificationThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    responses_.clear();
    notifications_.clear();
}

json JsonRpcHandler::readResponse(int expectedId, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true) {
        auto it = responses_.find(expectedId);
        if (it != responses_.end()) {
            json response = std::move(it->second);
            responses_.erase(it);
            return response;
        }

        if (!running_ || !transport_.isConnected()) {
            return {{"error", {{"code", -1}, {"message", "Connection closed"}}}};
        }

        if (responseCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            return {{"error", {{"code", -1}, {"message", "Timeout"}}}};
        }
    }
}

void JsonRpcHandler::readerLoop() {
    while (running_) {
        std::string line = transport_.receive(std::chrono::milliseconds(100));
        if (line.empty()) {
            if (!transport_.isConnected()) {
                break;
            }
            continue;
        }

        try {
            json message = json::parse(line);

            if (message.contains("id")) {
                std::lock_guard<std::mutex> lock(mutex_);
                int id = message["id"].get<int>();
                responses_[id] = std::move(message);
                responseCv_.notify_all();
            } else if (message.contains("method")) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    notifications_.push_back(std::move(message));
                }
                notificationCv_.notify_all();
            }
        } catch (const json::parse_error&) {
            // 忽略解析错误
        }
    }

    running_ = false;
    responseCv_.notify_all();
    notificationCv_.notify_all();
}

void JsonRpcHandler::notificationLoop() {
    while (true) {
        NotificationHandler handler;
        json notification;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            notificationCv_.wait(lock, [this]() {
                return !running_ || !notifications_.empty();
            });

            if (!running_ && notifications_.empty()) {
                break;
            }

            notification = std::move(notifications_.front());
            notifications_.pop_front();
            handler = notificationHandler_;
        }

        if (handler) {
            try {
                handler(notification);
            } catch (...) {
                // 后台通知线程不应因上层回调异常直接终止整个进程。
            }
        }
    }
}

}  // namespace internal
}  // namespace mcp
