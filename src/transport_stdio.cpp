/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Stdio Transport 实现
 * 通过 fork/exec + pipe 与子进程通信
 */

#include "../include/internal/transport_stdio.h"
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace mcp {
namespace internal {

StdioTransport::StdioTransport(const StdioConfig& config)
    : config_(config) {}

StdioTransport::~StdioTransport() {
    disconnect();
}

bool StdioTransport::connect() {
    if (connected_) {
        return true;
    }

    int stdinPipe[2];
    int stdoutPipe[2];

    if (pipe(stdinPipe) == -1 || pipe(stdoutPipe) == -1) {
        return false;
    }

    pid_ = fork();
    if (pid_ == -1) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return false;
    }

    if (pid_ == 0) {
        // 子进程
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdinPipe[0]);
        close(stdoutPipe[1]);

        // 重定向 stderr 到 /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // 切换工作目录
        if (!config_.workingDir.empty()) {
            if (chdir(config_.workingDir.c_str()) != 0) {
                _exit(1);
            }
        }

        // 设置环境变量
        for (const auto& [key, value] : config_.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        // 构建参数列表
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(config_.command.c_str()));
        for (const auto& arg : config_.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(config_.command.c_str(), argv.data());
        _exit(1);
    }

    // 父进程
    close(stdinPipe[0]);
    close(stdoutPipe[1]);

    writeFd_ = stdinPipe[1];
    readFd_ = stdoutPipe[0];

    // 设置非阻塞
    int flags = fcntl(readFd_, F_GETFL, 0);
    fcntl(readFd_, F_SETFL, flags | O_NONBLOCK);

    connected_ = true;
    return true;
}

void StdioTransport::disconnect() {
    if (pid_ > 0) {
        close(writeFd_);
        close(readFd_);
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
        writeFd_ = -1;
        readFd_ = -1;
    }
    connected_ = false;
    readBuffer_.clear();
}

bool StdioTransport::isConnected() const {
    return connected_;
}

bool StdioTransport::send(const std::string& data) {
    if (!connected_) {
        return false;
    }

    ssize_t total = 0;
    ssize_t len = static_cast<ssize_t>(data.size());
    const char* buf = data.c_str();

    while (total < len) {
        ssize_t n = write(writeFd_, buf + total, len - total);
        if (n <= 0) {
            return false;
        }
        total += n;
    }
    return true;
}

std::string StdioTransport::receive(std::chrono::milliseconds timeout) {
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

        ssize_t n = read(readFd_, &c, 1);
        if (n > 0) {
            if (c == '\n' && !readBuffer_.empty()) {
                std::string line = readBuffer_;
                readBuffer_.clear();
                return line;
            } else if (c != '\n') {
                readBuffer_ += c;
            }
        } else if (n == 0) {
            // EOF
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
