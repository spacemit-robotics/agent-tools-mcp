/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * MCP SDK 工厂函数和初始化
 */

#include <memory>
#include "../include/mcp_service.hpp"
#include "../include/internal/transport_stdio.h"
#include "../include/internal/transport_unix_socket.h"

namespace mcp {

// ============================================================================
// Transport 工厂函数
// ============================================================================

std::unique_ptr<Transport> createStdioTransport(const StdioConfig& config) {
    return std::make_unique<internal::StdioTransport>(config);
}

std::unique_ptr<Transport> createUnixSocketTransport(const UnixSocketConfig& config) {
    return std::make_unique<internal::UnixSocketTransport>(config);
}

// ============================================================================
// SDK 初始化
// ============================================================================

static bool g_initialized = false;

void init() {
    if (g_initialized) {
        return;
    }
    // 目前不需要全局初始化
    // 预留用于后续扩展（如 libcurl 初始化等）
    g_initialized = true;
}

void cleanup() {
    if (!g_initialized) {
        return;
    }
    // 目前不需要全局清理
    g_initialized = false;
}

}  // namespace mcp
