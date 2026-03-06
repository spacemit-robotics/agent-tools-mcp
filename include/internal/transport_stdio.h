/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Stdio Transport 内部实现
 */

#ifndef TRANSPORT_STDIO_H
#define TRANSPORT_STDIO_H

#include <sys/types.h>
#include <string>
#include "../mcp_service.hpp"

namespace mcp {
namespace internal {

/**
 * Stdio Transport 实现
 * 通过 fork/exec + pipe 与子进程通信
 */
class StdioTransport : public Transport {
public:
    explicit StdioTransport(const StdioConfig& config);
    ~StdioTransport() override;

    bool connect() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const std::string& data) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    std::string typeName() const override { return "stdio"; }

private:
    StdioConfig config_;
    pid_t pid_ = -1;
    int writeFd_ = -1;
    int readFd_ = -1;
    bool connected_ = false;
    std::string readBuffer_;
};

}  // namespace internal
}  // namespace mcp

#endif  // TRANSPORT_STDIO_H
