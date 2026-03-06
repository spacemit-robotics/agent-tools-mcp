/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCPClient 实现
 */

#include <memory>
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
        , rpc_(*transport_) {}

    bool connect() {
        if (connected_) {
            return true;
        }
        connected_ = transport_->connect();
        return connected_;
    }

    bool initialize() {
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

        auto response = rpc_.sendRequest("initialize", params, config_.timeout);

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

    std::vector<Tool> listTools() {
        std::vector<Tool> tools;

        auto response = rpc_.sendRequest("tools/list", json::object(), config_.timeout);

        if (response.contains("result") && response["result"].contains("tools")) {
            for (const auto& t : response["result"]["tools"]) {
                tools.push_back(Tool::fromJson(t));
            }
        }

        return tools;
    }

    ToolResult callTool(const std::string& name, const json& args) {
        ToolResult result;

        json params = {
            {"name", name},
            {"arguments", args}
        };

        auto response = rpc_.sendRequest("tools/call", params, config_.timeout);

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

    void shutdown() {
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
    std::unique_ptr<Transport> transport_;
    ClientConfig config_;
    internal::JsonRpcHandler rpc_;
    bool connected_ = false;
    bool initialized_ = false;
    ServerInfo serverInfo_;
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

bool MCPClient::initialize() {
    return impl_->initialize();
}

std::vector<Tool> MCPClient::listTools() {
    return impl_->listTools();
}

ToolResult MCPClient::callTool(const std::string& name, const json& args) {
    return impl_->callTool(name, args);
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
