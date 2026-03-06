cmake_minimum_required(VERSION 3.16)
project(mcp-sdk VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# 选项
# ============================================================================
option(MCP_BUILD_SHARED "Build shared library" ON)
option(MCP_BUILD_STATIC "Build static library" ON)
option(MCP_BUILD_EXAMPLES "Build examples" ON)

# ============================================================================
# C++ 标准
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# 依赖
# ============================================================================
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# nlohmann/json - 强制使用 3.11.3 版本以与 ollama.hpp 内嵌版本保持 ABI 兼容
# 注意：不使用 find_package，因为系统可能安装了不兼容的 3.12.x 版本
include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# ============================================================================
# 源文件
# ============================================================================
set(MCP_SOURCES
    src/mcp.cpp
    src/json_rpc.cpp
    src/mcp_client.cpp
    src/mcp_manager.cpp
    src/transport_stdio.cpp
    src/transport_unix_socket.cpp
    src/transport_http.cpp
)

set(MCP_PUBLIC_HEADERS
    include/mcp_service.hpp
)

# ============================================================================
# 库目标
# ============================================================================

# 静态库
if(MCP_BUILD_STATIC)
    add_library(mcp_static STATIC ${MCP_SOURCES})
    target_include_directories(mcp_static
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
    )
    target_link_libraries(mcp_static
        PUBLIC
            nlohmann_json::nlohmann_json
        PRIVATE
            Threads::Threads
            CURL::libcurl
    )
    set_target_properties(mcp_static PROPERTIES
        OUTPUT_NAME mcp
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    )
endif()

# 动态库
if(MCP_BUILD_SHARED)
    add_library(mcp_shared SHARED ${MCP_SOURCES})
    target_include_directories(mcp_shared
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include/internal
    )
    target_link_libraries(mcp_shared
        PUBLIC
            nlohmann_json::nlohmann_json
        PRIVATE
            Threads::Threads
            CURL::libcurl
    )
    set_target_properties(mcp_shared PROPERTIES
        OUTPUT_NAME mcp
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    )
endif()

# 别名目标
if(MCP_BUILD_STATIC)
    add_library(mcp::mcp ALIAS mcp_static)
elseif(MCP_BUILD_SHARED)
    add_library(mcp::mcp ALIAS mcp_shared)
endif()

# ============================================================================
# 示例
# ============================================================================
if(MCP_BUILD_EXAMPLES)
    add_executable(example_simple examples/simple_client.cpp)
    target_link_libraries(example_simple PRIVATE mcp::mcp)
    set_target_properties(example_simple PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    add_executable(example_multi examples/multi_server.cpp)
    target_link_libraries(example_multi PRIVATE mcp::mcp)
    set_target_properties(example_multi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )

    # LLM Chat 示例 (需要 libcurl)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        add_executable(llm_chat examples/llm_chat.cpp)
        target_link_libraries(llm_chat PRIVATE mcp::mcp CURL::libcurl)
        set_target_properties(llm_chat PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        )
    else()
        message(STATUS "libcurl not found, skipping llm_chat example")
    endif()
endif()

# ============================================================================
# 安装 (仅当作为独立项目构建时)
# ============================================================================
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(GNUInstallDirs)

# 安装头文件
install(FILES ${MCP_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 安装库
# 先安装 nlohmann_json（只需一次）
install(TARGETS nlohmann_json
    EXPORT mcp-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(MCP_BUILD_STATIC)
    install(TARGETS mcp_static
        EXPORT mcp-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()

if(MCP_BUILD_SHARED)
    install(TARGETS mcp_shared
        EXPORT mcp-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# CMake 配置文件
install(EXPORT mcp-targets
    FILE mcp-targets.cmake
    NAMESPACE mcp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

# 版本文件
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# 配置文件
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
"include(CMakeFindDependencyMacro)
find_dependency(nlohmann_json)
find_dependency(Threads)
include(\"\${CMAKE_CURRENT_LIST_DIR}/mcp-targets.cmake\")
")

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)
endif() # CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
