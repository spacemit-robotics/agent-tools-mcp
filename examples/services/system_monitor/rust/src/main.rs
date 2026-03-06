//! System Monitor MCP Server
//! 
//! 通过 Unix Socket 提供系统监控服务
//! - CPU 使用率
//! - 内存使用情况
//! - 磁盘使用情况
//! - 网络流量
//! - 进程列表（Top 10）

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::os::unix::net::{UnixListener, UnixStream};
use std::io::{BufRead, BufReader, Write};
use std::path::Path;
use std::sync::Arc;
use std::thread;
use sysinfo::{System, Disks, Networks, Pid};

const SOCKET_PATH: &str = "/tmp/mcp_system_monitor.sock";

// ============================================================================
// JSON-RPC 结构
// ============================================================================

#[derive(Debug, Deserialize)]
struct JsonRpcRequest {
    jsonrpc: String,
    id: Option<Value>,
    method: String,
    #[serde(default)]
    params: Value,
}

#[derive(Debug, Serialize)]
struct JsonRpcResponse {
    jsonrpc: String,
    id: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    result: Option<Value>,
    #[serde(skip_serializing_if = "Option::is_none")]
    error: Option<JsonRpcError>,
}

#[derive(Debug, Serialize)]
struct JsonRpcError {
    code: i32,
    message: String,
}

// ============================================================================
// MCP Tool 定义
// ============================================================================

fn get_tools() -> Value {
    json!([
        {
            "name": "get_cpu_info",
            "description": "获取 CPU 使用率和信息",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "get_memory_info",
            "description": "获取内存使用情况（总量、已用、可用）",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "get_disk_info",
            "description": "获取磁盘使用情况",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "get_network_info",
            "description": "获取网络接口流量信息",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        },
        {
            "name": "get_top_processes",
            "description": "获取资源占用前 N 的进程",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "sort_by": {
                        "type": "string",
                        "description": "排序方式: cpu 或 memory",
                        "enum": ["cpu", "memory"],
                        "default": "cpu"
                    },
                    "limit": {
                        "type": "integer",
                        "description": "返回数量，默认 10",
                        "default": 10
                    }
                },
                "required": []
            }
        },
        {
            "name": "get_system_overview",
            "description": "获取系统概览（主机名、系统、内核版本、运行时间）",
            "inputSchema": {
                "type": "object",
                "properties": {},
                "required": []
            }
        }
    ])
}

// ============================================================================
// 系统监控实现
// ============================================================================

fn format_bytes(bytes: u64) -> String {
    const KB: u64 = 1024;
    const MB: u64 = KB * 1024;
    const GB: u64 = MB * 1024;
    
    if bytes >= GB {
        format!("{:.2} GB", bytes as f64 / GB as f64)
    } else if bytes >= MB {
        format!("{:.2} MB", bytes as f64 / MB as f64)
    } else if bytes >= KB {
        format!("{:.2} KB", bytes as f64 / KB as f64)
    } else {
        format!("{} B", bytes)
    }
}

fn get_cpu_info() -> String {
    let mut sys = System::new_all();
    // 需要刷新两次才能获取准确的 CPU 使用率
    sys.refresh_cpu_all();
    std::thread::sleep(std::time::Duration::from_millis(200));
    sys.refresh_cpu_all();
    
    let cpus = sys.cpus();
    let cpu_count = cpus.len();
    let total_usage: f32 = cpus.iter().map(|c| c.cpu_usage()).sum::<f32>() / cpu_count as f32;
    
    let mut result = format!("CPU 信息:\n");
    result.push_str(&format!("  核心数: {}\n", cpu_count));
    result.push_str(&format!("  总体使用率: {:.1}%\n", total_usage));
    result.push_str("  各核心使用率:\n");
    
    for (i, cpu) in cpus.iter().enumerate() {
        result.push_str(&format!("    CPU {}: {:.1}%\n", i, cpu.cpu_usage()));
    }
    
    result
}

fn get_memory_info() -> String {
    let mut sys = System::new_all();
    sys.refresh_memory();
    
    let total = sys.total_memory();
    let used = sys.used_memory();
    let available = sys.available_memory();
    let swap_total = sys.total_swap();
    let swap_used = sys.used_swap();
    
    let usage_percent = (used as f64 / total as f64) * 100.0;
    
    let mut result = format!("内存信息:\n");
    result.push_str(&format!("  总内存: {}\n", format_bytes(total)));
    result.push_str(&format!("  已使用: {} ({:.1}%)\n", format_bytes(used), usage_percent));
    result.push_str(&format!("  可用: {}\n", format_bytes(available)));
    
    if swap_total > 0 {
        let swap_percent = (swap_used as f64 / swap_total as f64) * 100.0;
        result.push_str(&format!("  交换区总量: {}\n", format_bytes(swap_total)));
        result.push_str(&format!("  交换区已用: {} ({:.1}%)\n", format_bytes(swap_used), swap_percent));
    }
    
    result
}

fn get_disk_info() -> String {
    let disks = Disks::new_with_refreshed_list();
    
    let mut result = format!("磁盘信息:\n");
    
    for disk in disks.list() {
        let total = disk.total_space();
        let available = disk.available_space();
        let used = total - available;
        let usage_percent = if total > 0 {
            (used as f64 / total as f64) * 100.0
        } else {
            0.0
        };
        
        result.push_str(&format!("  {} ({:?}):\n", 
            disk.mount_point().display(),
            disk.file_system()));
        result.push_str(&format!("    总容量: {}\n", format_bytes(total)));
        result.push_str(&format!("    已使用: {} ({:.1}%)\n", format_bytes(used), usage_percent));
        result.push_str(&format!("    可用: {}\n", format_bytes(available)));
    }
    
    result
}

fn get_network_info() -> String {
    let networks = Networks::new_with_refreshed_list();
    
    let mut result = format!("网络接口信息:\n");
    
    for (name, data) in networks.list() {
        let rx = data.total_received();
        let tx = data.total_transmitted();
        
        // 跳过没有流量的接口
        if rx == 0 && tx == 0 {
            continue;
        }
        
        result.push_str(&format!("  {}:\n", name));
        result.push_str(&format!("    接收: {}\n", format_bytes(rx)));
        result.push_str(&format!("    发送: {}\n", format_bytes(tx)));
        result.push_str(&format!("    MAC: {:?}\n", data.mac_address()));
    }
    
    result
}

fn get_top_processes(sort_by: &str, limit: usize) -> String {
    let mut sys = System::new_all();
    sys.refresh_all();
    
    // 再次刷新以获取准确的 CPU 使用率
    std::thread::sleep(std::time::Duration::from_millis(200));
    sys.refresh_all();
    
    let mut processes: Vec<_> = sys.processes().iter().collect();
    
    // 排序
    match sort_by {
        "memory" => {
            processes.sort_by(|a, b| b.1.memory().cmp(&a.1.memory()));
        }
        _ => {
            // 默认按 CPU 排序
            processes.sort_by(|a, b| {
                b.1.cpu_usage().partial_cmp(&a.1.cpu_usage()).unwrap_or(std::cmp::Ordering::Equal)
            });
        }
    }
    
    let sort_desc = if sort_by == "memory" { "内存" } else { "CPU" };
    let mut result = format!("进程列表 (按{}排序, Top {}):\n", sort_desc, limit);
    result.push_str(&format!("  {:<8} {:<20} {:>8} {:>12}\n", "PID", "名称", "CPU%", "内存"));
    result.push_str(&format!("  {}\n", "-".repeat(52)));
    
    for (pid, process) in processes.iter().take(limit) {
        let name = process.name().to_string_lossy();
        let name_display = if name.len() > 18 {
            format!("{}...", &name[..15])
        } else {
            name.to_string()
        };
        
        result.push_str(&format!("  {:<8} {:<20} {:>7.1}% {:>12}\n",
            pid.as_u32(),
            name_display,
            process.cpu_usage(),
            format_bytes(process.memory())
        ));
    }
    
    result
}

fn get_system_overview() -> String {
    let mut sys = System::new_all();
    sys.refresh_all();
    
    let mut result = format!("系统概览:\n");
    result.push_str(&format!("  主机名: {}\n", System::host_name().unwrap_or_default()));
    result.push_str(&format!("  系统: {} {}\n", 
        System::name().unwrap_or_default(),
        System::os_version().unwrap_or_default()));
    result.push_str(&format!("  内核版本: {}\n", System::kernel_version().unwrap_or_default()));
    
    // 运行时间
    let uptime = System::uptime();
    let days = uptime / 86400;
    let hours = (uptime % 86400) / 3600;
    let minutes = (uptime % 3600) / 60;
    result.push_str(&format!("  运行时间: {}天 {}小时 {}分钟\n", days, hours, minutes));
    
    // CPU 信息
    if let Some(cpu) = sys.cpus().first() {
        result.push_str(&format!("  CPU: {}\n", cpu.brand()));
    }
    result.push_str(&format!("  CPU 核心数: {}\n", sys.cpus().len()));
    
    // 内存概览
    let total_mem = sys.total_memory();
    let used_mem = sys.used_memory();
    result.push_str(&format!("  内存: {} / {} ({:.1}%)\n", 
        format_bytes(used_mem), 
        format_bytes(total_mem),
        (used_mem as f64 / total_mem as f64) * 100.0));
    
    result
}

// ============================================================================
// 工具调用处理
// ============================================================================

fn call_tool(name: &str, arguments: &Value) -> Value {
    let text = match name {
        "get_cpu_info" => get_cpu_info(),
        "get_memory_info" => get_memory_info(),
        "get_disk_info" => get_disk_info(),
        "get_network_info" => get_network_info(),
        "get_top_processes" => {
            let sort_by = arguments.get("sort_by")
                .and_then(|v| v.as_str())
                .unwrap_or("cpu");
            let limit = arguments.get("limit")
                .and_then(|v| v.as_u64())
                .unwrap_or(10) as usize;
            get_top_processes(sort_by, limit)
        }
        "get_system_overview" => get_system_overview(),
        _ => format!("未知工具: {}", name),
    };
    
    json!({
        "content": [{
            "type": "text",
            "text": text
        }]
    })
}

// ============================================================================
// 请求处理
// ============================================================================

fn handle_request(request: JsonRpcRequest) -> Option<JsonRpcResponse> {
    let id = match request.id {
        Some(id) => id,
        None => return None, // 通知类型，不需要响应
    };
    
    let (result, error) = match request.method.as_str() {
        "initialize" => {
            let result = json!({
                "protocolVersion": "2024-11-05",
                "serverInfo": {
                    "name": "system-monitor",
                    "version": "1.0.0"
                },
                "capabilities": {
                    "tools": {}
                }
            });
            (Some(result), None)
        }
        
        "tools/list" => {
            let result = json!({
                "tools": get_tools()
            });
            (Some(result), None)
        }
        
        "tools/call" => {
            let tool_name = request.params.get("name")
                .and_then(|v| v.as_str())
                .unwrap_or("");
            let arguments = request.params.get("arguments")
                .cloned()
                .unwrap_or(json!({}));
            
            let result = call_tool(tool_name, &arguments);
            (Some(result), None)
        }
        
        _ => {
            let error = JsonRpcError {
                code: -32601,
                message: format!("Unknown method: {}", request.method),
            };
            (None, Some(error))
        }
    };
    
    Some(JsonRpcResponse {
        jsonrpc: "2.0".to_string(),
        id,
        result,
        error,
    })
}

// ============================================================================
// 客户端处理
// ============================================================================

fn handle_client(stream: UnixStream) {
    let peer = stream.peer_addr().ok();
    println!("[SystemMonitor] 客户端已连接: {:?}", peer);
    
    let reader = BufReader::new(stream.try_clone().expect("Failed to clone stream"));
    let mut writer = stream;
    
    for line in reader.lines() {
        match line {
            Ok(line) if !line.is_empty() => {
                match serde_json::from_str::<JsonRpcRequest>(&line) {
                    Ok(request) => {
                        if let Some(response) = handle_request(request) {
                            let response_str = serde_json::to_string(&response).unwrap();
                            if let Err(e) = writeln!(writer, "{}", response_str) {
                                eprintln!("[SystemMonitor] 写入错误: {}", e);
                                break;
                            }
                            let _ = writer.flush();
                        }
                    }
                    Err(e) => {
                        eprintln!("[SystemMonitor] JSON 解析错误: {}", e);
                    }
                }
            }
            Ok(_) => {} // 空行，忽略
            Err(e) => {
                eprintln!("[SystemMonitor] 读取错误: {}", e);
                break;
            }
        }
    }
    
    println!("[SystemMonitor] 客户端断开: {:?}", peer);
}

// ============================================================================
// 主函数
// ============================================================================

fn main() {
    println!("╔════════════════════════════════════════╗");
    println!("║     System Monitor MCP Server (Rust)   ║");
    println!("╚════════════════════════════════════════╝");
    
    // 清理旧的 socket 文件
    if Path::new(SOCKET_PATH).exists() {
        fs::remove_file(SOCKET_PATH).expect("Failed to remove old socket");
    }
    
    // 创建 Unix Socket 监听
    let listener = UnixListener::bind(SOCKET_PATH).expect("Failed to bind socket");
    
    // 设置权限
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(SOCKET_PATH, fs::Permissions::from_mode(0o777))
            .expect("Failed to set socket permissions");
    }
    
    println!("[SystemMonitor] Socket: {}", SOCKET_PATH);
    println!("[SystemMonitor] 等待客户端连接... (Ctrl+C 退出)");
    
    // 处理 Ctrl+C
    ctrlc_handler();
    
    // 接受连接
    for stream in listener.incoming() {
        match stream {
            Ok(stream) => {
                thread::spawn(|| handle_client(stream));
            }
            Err(e) => {
                eprintln!("[SystemMonitor] 连接错误: {}", e);
            }
        }
    }
}

fn ctrlc_handler() {
    let socket_path = SOCKET_PATH.to_string();
    ctrlc::set_handler(move || {
        println!("\n[SystemMonitor] 关闭服务器...");
        if Path::new(&socket_path).exists() {
            let _ = fs::remove_file(&socket_path);
        }
        std::process::exit(0);
    }).ok();
}