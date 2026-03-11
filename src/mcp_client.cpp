/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCPClient 实现
 */

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "../include/mcp_service.hpp"
#include "../include/internal/json_rpc.h"

namespace mcp {

// ============================================================================
// MCPClient::Impl
// ============================================================================

class MCPClient::Impl {
public:
    Impl(std::unique_ptr<Transport> transport, const ClientConfig& config)
        : transport_(std::move(transport))
        , config_(config)
        , rpc_(*transport_) {
        rpc_.setNotificationHandler([this](const json& message) {
            handleNotification(message);
        });
    }

    bool connect() {
        if (connected_) {
            return true;
        }
        connected_ = transport_->connect();
        if (connected_) {
            rpc_.start();
        }
        return connected_;
    }

    bool initialize(std::chrono::milliseconds timeout) {
        if (initialized_) {
            return true;
        }

        if (!connected_) {
            return false;
        }

        json params = {
            {"protocolVersion", PROTOCOL_VERSION},
            {"capabilities", {{"roots", {{"listChanged", true}}}}},
            {"clientInfo", {
                {"name", config_.name},
                {"version", config_.version}
            }}
        };

        auto response = rpc_.sendRequest("initialize", params, resolveTimeout(timeout));

        if (!response.contains("result")) {
            return false;
        }

        // 提取服务器信息
        if (response["result"].contains("serverInfo")) {
            auto& info = response["result"]["serverInfo"];
            serverInfo_.name = info.value("name", "");
            serverInfo_.version = info.value("version", "");
        }
        if (response["result"].contains("capabilities")) {
            serverInfo_.capabilities = response["result"]["capabilities"];
        }

        // 发送 initialized 通知
        rpc_.sendNotification("notifications/initialized", json::object());

        initialized_ = true;
        return true;
    }

    std::vector<Tool> listTools(std::chrono::milliseconds timeout) {
        std::vector<Tool> tools;

        auto response = rpc_.sendRequest("tools/list", json::object(), resolveTimeout(timeout));

        if (response.contains("result") && response["result"].contains("tools")) {
            for (const auto& t : response["result"]["tools"]) {
                tools.push_back(Tool::fromJson(t));
            }
        }

        return tools;
    }

    ToolResult callTool(const std::string& name, const json& args, std::chrono::milliseconds timeout) {
        ToolResult result;

        json params = {
            {"name", name},
            {"arguments", args}
        };

        auto response = rpc_.sendRequest("tools/call", params, resolveTimeout(timeout));

        if (response.contains("error")) {
            result.success = false;
            result.error = response["error"].dump();
            return result;
        }

        if (response.contains("result")) {
            result.rawResult = response["result"];

            if (response["result"].contains("content")) {
                for (const auto& content : response["result"]["content"]) {
                    if (content.contains("text")) {
                        result.contents.push_back(content["text"].get<std::string>());
                    }
                }
            }

            if (response["result"].contains("isError") && response["result"]["isError"].get<bool>()) {
                result.success = false;
                if (!result.contents.empty()) {
                    result.error = result.contents[0];
                }
            }
        }

        return result;
    }

    void onToolsChanged(ToolsChangedHandler handler) {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        toolsChangedHandler_ = std::move(handler);
    }

    void shutdown() {
        rpc_.stop();
        if (transport_) {
            transport_->disconnect();
        }
        connected_ = false;
        initialized_ = false;
    }

    bool isConnected() const { return connected_; }
    bool isInitialized() const { return initialized_; }
    const ServerInfo& serverInfo() const { return serverInfo_; }
    const std::string& name() const { return config_.name; }

private:
    std::chrono::milliseconds resolveTimeout(std::chrono::milliseconds timeout) const {
        if (timeout.count() > 0) {
            return timeout;
        }
        return config_.requestTimeout;
    }

    void handleNotification(const json& message) {
        if (!message.contains("method")) {
            return;
        }

        const std::string method = message.value("method", "");
        if (method != "notifications/tools/list_changed") {
            return;
        }

        ToolsChangedHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlerMutex_);
            handler = toolsChangedHandler_;
        }

        if (handler) {
            handler();
        }
    }

    std::unique_ptr<Transport> transport_;
    ClientConfig config_;
    internal::JsonRpcHandler rpc_;
    bool connected_ = false;
    bool initialized_ = false;
    ServerInfo serverInfo_;
    std::mutex handlerMutex_;
    ToolsChangedHandler toolsChangedHandler_;
};

// ============================================================================
// MCPClient
// ============================================================================

MCPClient::MCPClient(std::unique_ptr<Transport> transport, const ClientConfig& config)
    : impl_(std::make_unique<Impl>(std::move(transport), config)) {}

MCPClient::~MCPClient() = default;

MCPClient::MCPClient(MCPClient&&) noexcept = default;
MCPClient& MCPClient::operator=(MCPClient&&) noexcept = default;

bool MCPClient::connect() {
    return impl_->connect();
}

bool MCPClient::initialize(std::chrono::milliseconds timeout) {
    return impl_->initialize(timeout);
}

std::vector<Tool> MCPClient::listTools(std::chrono::milliseconds timeout) {
    return impl_->listTools(timeout);
}

ToolResult MCPClient::callTool(
    const std::string& name,
    const json& args,
    std::chrono::milliseconds timeout) {
    return impl_->callTool(name, args, timeout);
}

void MCPClient::onToolsChanged(ToolsChangedHandler handler) {
    impl_->onToolsChanged(std::move(handler));
}

void MCPClient::shutdown() {
    impl_->shutdown();
}

bool MCPClient::isConnected() const {
    return impl_->isConnected();
}

bool MCPClient::isInitialized() const {
    return impl_->isInitialized();
}

const ServerInfo& MCPClient::serverInfo() const {
    return impl_->serverInfo();
}

const std::string& MCPClient::name() const {
    return impl_->name();
}

}  // namespace mcp
