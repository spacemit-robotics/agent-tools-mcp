/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Unix Socket Transport 内部实现
 */

#ifndef TRANSPORT_UNIX_SOCKET_H
#define TRANSPORT_UNIX_SOCKET_H

#include <string>
#include "../mcp_service.hpp"

namespace mcp {
namespace internal {

/**
 * Unix Socket Transport 实现
 */
class UnixSocketTransport : public Transport {
public:
    explicit UnixSocketTransport(const UnixSocketConfig& config);
    ~UnixSocketTransport() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const std::string& data) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    std::string typeName() const override { return "unix-socket"; }

    /**
     * 检查 socket 文件是否存在
     */
    bool isAvailable() const;

private:
    UnixSocketConfig config_;
    int fd_ = -1;
    bool connected_ = false;
    std::string readBuffer_;
};

}  // namespace internal
}  // namespace mcp

#endif  // TRANSPORT_UNIX_SOCKET_H
