/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Unix Socket Transport 实现
 */

#include "../include/internal/transport_unix_socket.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>

namespace mcp {
namespace internal {

UnixSocketTransport::UnixSocketTransport(const UnixSocketConfig& config)
    : config_(config) {}

UnixSocketTransport::~UnixSocketTransport() {
    disconnect();
}

bool UnixSocketTransport::isAvailable() const {
    return access(config_.socketPath.c_str(), F_OK) == 0;
}

bool UnixSocketTransport::connect() {
    if (connected_) {
        return true;
    }

    if (!isAvailable()) {
        return false;
    }

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config_.socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    // 设置非阻塞
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    connected_ = true;
    return true;
}

void UnixSocketTransport::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    connected_ = false;
    readBuffer_.clear();
}

bool UnixSocketTransport::isConnected() const {
    return connected_;
}

bool UnixSocketTransport::send(const std::string& data) {
    if (!connected_) {
        return false;
    }

    ssize_t total = 0;
    ssize_t len = static_cast<ssize_t>(data.size());
    const char* buf = data.c_str();

    while (total < len) {
        ssize_t n = write(fd_, buf + total, len - total);
        if (n <= 0) {
            connected_ = false;
            return false;
        }
        total += n;
    }
    return true;
}

std::string UnixSocketTransport::receive(std::chrono::milliseconds timeout) {
    if (!connected_) {
        return "";
    }

    auto start = std::chrono::steady_clock::now();
    char c;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed > timeout) {
            return "";
        }

        ssize_t n = read(fd_, &c, 1);
        if (n > 0) {
            if (c == '\n' && !readBuffer_.empty()) {
                std::string line = readBuffer_;
                readBuffer_.clear();
                return line;
            } else if (c != '\n') {
                readBuffer_ += c;
            }
        } else if (n == 0) {
            // 连接关闭
            connected_ = false;
            return "";
        } else {
            // EAGAIN/EWOULDBLOCK
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

}  // namespace internal
}  // namespace mcp
