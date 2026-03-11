/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCPManager 实现
 */

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "../include/mcp_service.hpp"
#include "../include/internal/transport_stdio.h"
#include "../include/internal/transport_unix_socket.h"
#include "../include/internal/transport_http.h"

namespace mcp {

// ============================================================================
// 服务器配置内部结构
// ============================================================================

enum class TransportType {
    Stdio,
    UnixSocket,
    Http
};

struct ServerConfigInternal {
    std::string name;
    TransportType type;
    StdioConfig stdioConfig;
    UnixSocketConfig unixSocketConfig;
    HttpConfig httpConfig;
};

// ============================================================================
// MCPManager::Impl
// ============================================================================

class MCPManager::Impl {
public:
    Impl() = default;

    ~Impl() {
        stopReconnectThread();
        stopAll();
    }

    void addStdioServer(const std::string& name, const StdioConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        ServerConfigInternal cfg;
        cfg.name = name;
        cfg.type = TransportType::Stdio;
        cfg.stdioConfig = config;
        configs_[name] = cfg;
    }

    void addUnixSocketServer(const std::string& name, const UnixSocketConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        ServerConfigInternal cfg;
        cfg.name = name;
        cfg.type = TransportType::UnixSocket;
        cfg.unixSocketConfig = config;
        configs_[name] = cfg;
    }

    void addHttpServer(const std::string& name, const HttpConfig& config) {
        std::lock_guard<std::mutex> lock(mutex_);
        ServerConfigInternal cfg;
        cfg.name = name;
        cfg.type = TransportType::Http;
        cfg.httpConfig = config;
        configs_[name] = cfg;
    }

    bool removeServer(const std::string& name) {
        auto client = detachServer(name, true);
        if (client) {
            client->shutdown();
        }
        return true;
    }

    bool startServer(const std::string& name) {
        return startServerInternal(name);
    }

    void stopServer(const std::string& name) {
        auto client = detachServer(name, false);
        if (client) {
            client->shutdown();
        }
    }

    void startAll() {
        std::vector<std::string> serverNames;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [name, cfg] : configs_) {
                serverNames.push_back(name);
            }
        }
        for (const auto& name : serverNames) {
            startServerInternal(name);
        }
        startReconnectThread();
    }

    void stopAll() {
        std::map<std::string, std::shared_ptr<MCPClient>> clientsToStop;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            clientsToStop.swap(clients_);
            toolRoutes_.clear();
            allTools_.clear();
        }

        for (auto& [name, client] : clientsToStop) {
            if (client) {
                client->shutdown();
            }
        }
    }

    bool waitForAnyServer(std::chrono::milliseconds timeout) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!clients_.empty()) {
                    return true;
                }
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed > timeout) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    bool waitForAllServers(std::chrono::milliseconds timeout) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (clients_.size() == configs_.size() && !configs_.empty()) {
                    return true;
                }
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed > timeout) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    std::vector<Tool> getAllTools() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allTools_;
    }

    json getAllToolsJson() const {
        std::lock_guard<std::mutex> lock(mutex_);
        json tools = json::array();
        for (const auto& tool : allTools_) {
            tools.push_back(tool.toJson());
        }
        return tools;
    }

    ToolResult callTool(const std::string& toolName, const json& args) {
        std::shared_ptr<MCPClient> client;
        std::string serverName;
        {
            std::lock_guard<std::mutex> lock(mutex_);

            auto routeIt = toolRoutes_.find(toolName);
            if (routeIt == toolRoutes_.end()) {
                ToolResult result;
                result.success = false;
                result.error = "Tool not found: " + toolName;
                return result;
            }

            serverName = routeIt->second;
            auto clientIt = clients_.find(serverName);
            if (clientIt == clients_.end() || !clientIt->second) {
                ToolResult result;
                result.success = false;
                result.error = "Server not connected: " + serverName;
                return result;
            }

            client = clientIt->second;
        }

        return client->callTool(toolName, args);
    }

    std::string findServerForTool(const std::string& toolName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = toolRoutes_.find(toolName);
        return (it != toolRoutes_.end()) ? it->second : "";
    }

    std::vector<ServerStatus> getStatuses() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ServerStatus> statuses;

        for (const auto& [name, cfg] : configs_) {
            ServerStatus status;
            status.name = name;

            auto clientIt = clients_.find(name);
            if (clientIt != clients_.end() && clientIt->second) {
                if (clientIt->second->isInitialized()) {
                    status.state = ServerState::Ready;
                } else if (clientIt->second->isConnected()) {
                    status.state = ServerState::Connecting;
                } else {
                    status.state = ServerState::Disconnected;
                }

                // 获取工具列表
                for (const auto& tool : allTools_) {
                    auto routeIt = toolRoutes_.find(tool.name);
                    if (routeIt != toolRoutes_.end() && routeIt->second == name) {
                        status.tools.push_back(tool);
                    }
                }
            } else {
                status.state = ServerState::Disconnected;
            }

            statuses.push_back(status);
        }

        return statuses;
    }

    ServerStatus getStatus(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ServerStatus status;
        status.name = name;
        status.state = ServerState::Disconnected;

        auto clientIt = clients_.find(name);
        if (clientIt != clients_.end() && clientIt->second) {
            if (clientIt->second->isInitialized()) {
                status.state = ServerState::Ready;
            } else if (clientIt->second->isConnected()) {
                status.state = ServerState::Connecting;
            }
        }

        return status;
    }

    size_t readyServerCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t count = 0;
        for (const auto& [name, client] : clients_) {
            if (client && client->isInitialized()) {
                ++count;
            }
        }
        return count;
    }

    bool hasAvailableServers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !clients_.empty();
    }

    void onServerEvent(ServerEventHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        serverEventHandler_ = std::move(handler);
    }

    void onToolChange(ToolChangeHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        toolChangeHandler_ = std::move(handler);
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, ServerConfigInternal> configs_;
    std::map<std::string, std::shared_ptr<MCPClient>> clients_;
    std::map<std::string, std::string> toolRoutes_;  // tool_name -> server_name
    std::vector<Tool> allTools_;
    ServerEventHandler serverEventHandler_;
    ToolChangeHandler toolChangeHandler_;

    // 后台重连线程
    std::thread reconnectThread_;
    std::atomic<bool> reconnectRunning_{false};

    ClientConfig createClientConfig(const ServerConfigInternal& cfg) const {
        ClientConfig clientConfig;
        if (cfg.type == TransportType::Stdio) {
            clientConfig.requestTimeout = cfg.stdioConfig.requestTimeout;
        }
        return clientConfig;
    }

    std::chrono::milliseconds createStartupTimeout(
            const ServerConfigInternal& cfg,
            const ClientConfig& clientConfig) const {
        if (cfg.type == TransportType::Stdio) {
            return cfg.stdioConfig.startupTimeout;
        }
        return clientConfig.requestTimeout;
    }

    void replaceServerToolsLocked(const std::string& name, const std::vector<Tool>& tools) {
        allTools_.erase(
            std::remove_if(
                allTools_.begin(),
                allTools_.end(),
                [this, &name](const Tool& tool) {
                    auto routeIt = toolRoutes_.find(tool.name);
                    return routeIt != toolRoutes_.end() && routeIt->second == name;
                }),
            allTools_.end());

        for (auto it = toolRoutes_.begin(); it != toolRoutes_.end(); ) {
            if (it->second == name) {
                it = toolRoutes_.erase(it);
            } else {
                ++it;
            }
        }

        for (const auto& tool : tools) {
            toolRoutes_[tool.name] = name;
            allTools_.push_back(tool);
        }
    }

    void refreshServerTools(const std::string& name) {
        std::shared_ptr<MCPClient> client;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto clientIt = clients_.find(name);
            if (clientIt == clients_.end() || !clientIt->second || !clientIt->second->isInitialized()) {
                return;
            }
            client = clientIt->second;
        }

        auto tools = client->listTools();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto clientIt = clients_.find(name);
            if (clientIt == clients_.end() || clientIt->second != client) {
                return;
            }
            replaceServerToolsLocked(name, tools);
        }

        notifyToolChange();
    }

    std::shared_ptr<MCPClient> detachServer(const std::string& name, bool removeConfig) {
        std::shared_ptr<MCPClient> client;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client = detachServerLocked(name, removeConfig);
        }

        if (client) {
            notifyServerEvent(name, ServerState::Disconnected);
            notifyToolChange();
        }

        return client;
    }

    std::shared_ptr<MCPClient> detachServerLocked(const std::string& name, bool removeConfig) {
        auto it = clients_.find(name);
        std::shared_ptr<MCPClient> client = (it != clients_.end()) ? it->second : nullptr;

        if (removeConfig) {
            configs_.erase(name);
        }

        if (!client) {
            return nullptr;
        }

        for (auto toolIt = toolRoutes_.begin(); toolIt != toolRoutes_.end(); ) {
            if (toolIt->second == name) {
                for (auto allIt = allTools_.begin(); allIt != allTools_.end(); ) {
                    if (allIt->name == toolIt->first) {
                        allIt = allTools_.erase(allIt);
                    } else {
                        ++allIt;
                    }
                }
                toolIt = toolRoutes_.erase(toolIt);
            } else {
                ++toolIt;
            }
        }

        if (it != clients_.end()) {
            clients_.erase(it);
        }

        return client;
    }

    void startReconnectThread() {
        if (reconnectRunning_) return;

        reconnectRunning_ = true;
        reconnectThread_ = std::thread([this]() {
            while (reconnectRunning_) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!reconnectRunning_) break;

                std::vector<std::string> reconnectNames;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& [name, cfg] : configs_) {
                        auto it = clients_.find(name);
                        if (it == clients_.end() || !it->second ||
                            !it->second->isInitialized()) {
                            reconnectNames.push_back(name);
                        }
                    }
                }

                for (const auto& name : reconnectNames) {
                    notifyServerEvent(name, ServerState::Reconnecting);
                    startServerInternal(name);
                }
            }
        });
    }

    void stopReconnectThread() {
        reconnectRunning_ = false;
        if (reconnectThread_.joinable()) {
            reconnectThread_.join();
        }
    }

    bool startServerInternal(const std::string& name) {
        ServerConfigInternal cfg;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cfgIt = configs_.find(name);
            if (cfgIt == configs_.end()) {
                return false;
            }

            auto clientIt = clients_.find(name);
            if (clientIt != clients_.end() && clientIt->second && clientIt->second->isInitialized()) {
                return true;
            }

            cfg = cfgIt->second;
        }

        // 创建传输层
        std::unique_ptr<Transport> transport;
        if (cfg.type == TransportType::Stdio) {
            transport = std::make_unique<internal::StdioTransport>(cfg.stdioConfig);
        } else if (cfg.type == TransportType::UnixSocket) {
            transport = std::make_unique<internal::UnixSocketTransport>(cfg.unixSocketConfig);
        } else if (cfg.type == TransportType::Http) {
            transport = std::make_unique<internal::HttpTransport>(cfg.httpConfig);
        }

        ClientConfig clientConfig = createClientConfig(cfg);
        auto startupTimeout = createStartupTimeout(cfg, clientConfig);
        auto client = std::make_shared<MCPClient>(std::move(transport), clientConfig);

        if (!client->connect()) {
            notifyServerEvent(name, ServerState::Error);
            return false;
        }

        notifyServerEvent(name, ServerState::Connecting);

        // 初始化
        if (!client->initialize(startupTimeout)) {
            notifyServerEvent(name, ServerState::Error);
            return false;
        }

        client->onToolsChanged([this, name]() {
            refreshServerTools(name);
        });

        notifyServerEvent(name, ServerState::Initializing);

        auto tools = client->listTools(startupTimeout);

        std::shared_ptr<MCPClient> oldClient;
        bool installed = false;
        bool configRemoved = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto cfgIt = configs_.find(name);
            if (cfgIt == configs_.end()) {
                configRemoved = true;
            } else {
                auto clientIt = clients_.find(name);
                if (clientIt != clients_.end() && clientIt->second && clientIt->second->isInitialized()) {
                    oldClient = clientIt->second;
                } else {
                    if (clientIt != clients_.end()) {
                        oldClient = clientIt->second;
                    }

                    clients_[name] = client;
                    replaceServerToolsLocked(name, tools);
                    installed = true;
                }
            }
        }

        if (configRemoved) {
            client->shutdown();
            return false;
        }

        if (!installed) {
            client->shutdown();
            return true;
        }

        if (oldClient && oldClient != client) {
            oldClient->shutdown();
        }

        notifyServerEvent(name, ServerState::Ready);
        notifyToolChange();

        return true;
    }

    void notifyServerEvent(const std::string& name, ServerState state) {
        ServerEventHandler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = serverEventHandler_;
        }
        if (handler) {
            handler(name, state);
        }
    }

    void notifyToolChange() {
        ToolChangeHandler handler;
        std::vector<Tool> toolsSnapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = toolChangeHandler_;
            toolsSnapshot = allTools_;
        }
        if (handler) {
            handler(toolsSnapshot);
        }
    }
};

// ============================================================================
// MCPManager
// ============================================================================

MCPManager::MCPManager()
    : impl_(std::make_unique<Impl>()) {}

MCPManager::~MCPManager() = default;

void MCPManager::addStdioServer(const std::string& name, const StdioConfig& config) {
    impl_->addStdioServer(name, config);
}

void MCPManager::addUnixSocketServer(const std::string& name, const UnixSocketConfig& config) {
    impl_->addUnixSocketServer(name, config);
}

void MCPManager::addHttpServer(const std::string& name, const HttpConfig& config) {
    impl_->addHttpServer(name, config);
}

bool MCPManager::removeServer(const std::string& name) {
    return impl_->removeServer(name);
}

bool MCPManager::startServer(const std::string& name) {
    return impl_->startServer(name);
}

void MCPManager::stopServer(const std::string& name) {
    impl_->stopServer(name);
}

void MCPManager::startAll() {
    impl_->startAll();
}

void MCPManager::stopAll() {
    impl_->stopAll();
}

bool MCPManager::waitForAnyServer(std::chrono::milliseconds timeout) {
    return impl_->waitForAnyServer(timeout);
}

bool MCPManager::waitForAllServers(std::chrono::milliseconds timeout) {
    return impl_->waitForAllServers(timeout);
}

std::vector<Tool> MCPManager::getAllTools() const {
    return impl_->getAllTools();
}

json MCPManager::getAllToolsJson() const {
    return impl_->getAllToolsJson();
}

ToolResult MCPManager::callTool(const std::string& toolName, const json& args) {
    return impl_->callTool(toolName, args);
}

std::string MCPManager::findServerForTool(const std::string& toolName) const {
    return impl_->findServerForTool(toolName);
}

std::vector<ServerStatus> MCPManager::getStatuses() const {
    return impl_->getStatuses();
}

ServerStatus MCPManager::getStatus(const std::string& name) const {
    return impl_->getStatus(name);
}

size_t MCPManager::readyServerCount() const {
    return impl_->readyServerCount();
}

bool MCPManager::hasAvailableServers() const {
    return impl_->hasAvailableServers();
}

void MCPManager::onServerEvent(ServerEventHandler handler) {
    impl_->onServerEvent(std::move(handler));
}

void MCPManager::onToolChange(ToolChangeHandler handler) {
    impl_->onToolChange(std::move(handler));
}

}  // namespace mcp
