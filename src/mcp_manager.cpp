/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCPManager 实现
 */

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
        std::lock_guard<std::mutex> lock(mutex_);
        stopServerInternal(name);
        configs_.erase(name);
        return true;
    }

    bool startServer(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return startServerInternal(name);
    }

    void stopServer(const std::string& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        stopServerInternal(name);
    }

    void startAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, cfg] : configs_) {
            startServerInternal(name);
        }
        // 启动后台重连线程
        startReconnectThread();
    }

    void stopAll() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, client] : clients_) {
            if (client) {
                client->shutdown();
            }
        }
        clients_.clear();
        toolRoutes_.clear();
        allTools_.clear();
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
        std::lock_guard<std::mutex> lock(mutex_);

        auto routeIt = toolRoutes_.find(toolName);
        if (routeIt == toolRoutes_.end()) {
            ToolResult result;
            result.success = false;
            result.error = "Tool not found: " + toolName;
            return result;
        }

        const std::string& serverName = routeIt->second;
        auto clientIt = clients_.find(serverName);
        if (clientIt == clients_.end() || !clientIt->second) {
            ToolResult result;
            result.success = false;
            result.error = "Server not connected: " + serverName;
            return result;
        }

        return clientIt->second->callTool(toolName, args);
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
    std::map<std::string, std::unique_ptr<MCPClient>> clients_;
    std::map<std::string, std::string> toolRoutes_;  // tool_name -> server_name
    std::vector<Tool> allTools_;
    ServerEventHandler serverEventHandler_;
    ToolChangeHandler toolChangeHandler_;

    // 后台重连线程
    std::thread reconnectThread_;
    std::atomic<bool> reconnectRunning_{false};

    void startReconnectThread() {
        if (reconnectRunning_) return;

        reconnectRunning_ = true;
        reconnectThread_ = std::thread([this]() {
            while (reconnectRunning_) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (!reconnectRunning_) break;

                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [name, cfg] : configs_) {
                    auto it = clients_.find(name);
                    if (it == clients_.end() || !it->second ||
                        !it->second->isInitialized()) {
                        // 尝试重连
                        notifyServerEvent(name, ServerState::Reconnecting);
                        startServerInternal(name);
                    }
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
        auto cfgIt = configs_.find(name);
        if (cfgIt == configs_.end()) {
            return false;
        }

        // 已经连接则跳过
        auto clientIt = clients_.find(name);
        if (clientIt != clients_.end() && clientIt->second && clientIt->second->isInitialized()) {
            return true;
        }

        const auto& cfg = cfgIt->second;

        // 创建传输层
        std::unique_ptr<Transport> transport;
        if (cfg.type == TransportType::Stdio) {
            transport = std::make_unique<internal::StdioTransport>(cfg.stdioConfig);
        } else if (cfg.type == TransportType::UnixSocket) {
            transport = std::make_unique<internal::UnixSocketTransport>(cfg.unixSocketConfig);
        } else if (cfg.type == TransportType::Http) {
            transport = std::make_unique<internal::HttpTransport>(cfg.httpConfig);
        }

        // 创建客户端
        auto client = std::make_unique<MCPClient>(std::move(transport));

        // 连接
        if (!client->connect()) {
            notifyServerEvent(name, ServerState::Error);
            return false;
        }

        notifyServerEvent(name, ServerState::Connecting);

        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // 初始化
        if (!client->initialize()) {
            notifyServerEvent(name, ServerState::Error);
            return false;
        }

        notifyServerEvent(name, ServerState::Initializing);

        // 获取工具列表
        auto tools = client->listTools();
        for (const auto& tool : tools) {
            toolRoutes_[tool.name] = name;

            // 检查是否已存在
            bool exists = false;
            for (const auto& t : allTools_) {
                if (t.name == tool.name) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                allTools_.push_back(tool);
            }
        }

        clients_[name] = std::move(client);
        notifyServerEvent(name, ServerState::Ready);
        notifyToolChange();

        return true;
    }

    void stopServerInternal(const std::string& name) {
        auto it = clients_.find(name);
        if (it != clients_.end()) {
            if (it->second) {
                it->second->shutdown();
            }

            // 清理工具路由
            for (auto toolIt = toolRoutes_.begin(); toolIt != toolRoutes_.end(); ) {
                if (toolIt->second == name) {
                    // 从 allTools_ 中移除
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

            clients_.erase(it);
            notifyServerEvent(name, ServerState::Disconnected);
            notifyToolChange();
        }
    }

    void notifyServerEvent(const std::string& name, ServerState state) {
        if (serverEventHandler_) {
            serverEventHandler_(name, state);
        }
    }

    void notifyToolChange() {
        if (toolChangeHandler_) {
            toolChangeHandler_(allTools_);
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
