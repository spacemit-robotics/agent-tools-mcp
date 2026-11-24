#!/usr/bin/env python3
"""SystemMonitor tool contracts. Run with: python -m unittest discover -s
examples/services/system_monitor -p 'test_*.py' -v
"""

import importlib.util
import inspect
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "system_monitor_http_server", Path(__file__).with_name("http_server.py")
)
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)


def result_text(result):
    """Collect text from FastMCP's content and optional structured result."""
    if isinstance(result, tuple):
        result = result[0]
    return "\n".join(item.text for item in result if hasattr(item, "text"))


class SystemMonitorToolsTest(unittest.IsolatedAsyncioTestCase):
    """Check the public tool schema and dispatch, not only Python calls."""

    async def test_all_tools_expose_only_real_parameters(self):
        tools = {tool.name: tool for tool in await server.mcp.list_tools()}
        no_argument_tools = {
            "get_cpu_info", "get_memory_info", "get_disk_info", "get_network_info",
            "get_system_overview", "get_load_average",
        }
        self.assertEqual(set(tools), no_argument_tools | {"get_top_processes"})
        for name in no_argument_tools:
            with self.subTest(tool=name):
                self.assertEqual(tools[name].inputSchema.get("properties", {}), {})
                self.assertFalse(tools[name].inputSchema.get("required"))

        schema = tools["get_top_processes"].inputSchema
        self.assertEqual(set(schema["properties"]), {"limit", "sort_by"})
        self.assertEqual(schema["properties"]["limit"]["type"], "integer")
        self.assertEqual(schema["properties"]["limit"]["default"], 5)
        self.assertEqual(schema["properties"]["sort_by"]["type"], "string")
        self.assertEqual(schema["properties"]["sort_by"]["default"], "memory")
        self.assertFalse(schema.get("required"))

    def test_decorator_preserves_signature_and_annotations(self):
        signature = inspect.signature(server.get_top_processes)
        self.assertEqual(list(signature.parameters), ["limit", "sort_by"])
        self.assertIs(signature.parameters["limit"].annotation, int)
        self.assertIs(signature.parameters["sort_by"].annotation, str)
        self.assertIs(signature.return_annotation, str)
        self.assertEqual(server.get_top_processes.__name__, "get_top_processes")
        self.assertIn("获取资源占用最高的进程列表", server.get_top_processes.__doc__)

    async def test_memory_tool_accepts_empty_arguments(self):
        memory = SimpleNamespace(total=4096, used=1024, available=3072, percent=25.0)
        swap = SimpleNamespace(total=2048, used=512, percent=25.0)
        with patch.object(server, "HAS_PSUTIL", True), \
                patch.object(server.psutil, "virtual_memory", return_value=memory), \
                patch.object(server.psutil, "swap_memory", return_value=swap):
            result = result_text(await server.mcp.call_tool("get_memory_info", {}))
        self.assertIn("总内存: 4.00 KB", result)
        self.assertIn("Swap 已用: 512.00 B (25.0%)", result)

    async def test_process_limit_sort_and_defaults(self):
        processes = [
            SimpleNamespace(info={"pid": i, "name": f"process-{i}",
                                  "cpu_percent": float(i), "memory_percent": 7.0 - i})
            for i in range(1, 7)
        ]
        with patch.object(server, "HAS_PSUTIL", True), \
                patch.object(server.psutil, "process_iter", return_value=processes):
            result = result_text(await server.mcp.call_tool(
                "get_top_processes", {"limit": 2, "sort_by": "cpu"}
            ))
            default_result = result_text(await server.mcp.call_tool("get_top_processes", {}))
        self.assertEqual(len(result.splitlines()), 3)
        self.assertIn("1. [6] process-6", result)
        self.assertIn("2. [5] process-5", result)
        self.assertEqual(len(default_result.splitlines()), 6)
        self.assertIn("Top 5 进程 (按 memory 排序)", default_result)
        self.assertIn("1. [1] process-1", default_result)

    async def test_missing_psutil_keeps_installation_error(self):
        with patch.object(server, "HAS_PSUTIL", False):
            for name in ("get_cpu_info", "get_memory_info", "get_disk_info",
                         "get_network_info", "get_system_overview", "get_top_processes"):
                with self.subTest(tool=name):
                    result = result_text(await server.mcp.call_tool(name, {}))
                    self.assertEqual(
                        result, "错误: 需要安装 psutil 才能使用此功能\n请运行: pip install psutil"
                    )

    async def test_disk_permission_error_skips_only_unreadable_partition(self):
        partitions = [
            SimpleNamespace(device="blocked", mountpoint="/blocked", fstype="ext4"),
            SimpleNamespace(device="readable", mountpoint="/readable", fstype="ext4"),
        ]
        usage = SimpleNamespace(total=4096, used=1024, free=3072, percent=25.0)
        with patch.object(server, "HAS_PSUTIL", True), \
                patch.object(server.psutil, "disk_partitions", return_value=partitions), \
                patch.object(server.psutil, "disk_usage", side_effect=[PermissionError(), usage]):
            result = result_text(await server.mcp.call_tool("get_disk_info", {}))
        self.assertNotIn("blocked", result)
        self.assertIn("[readable] 挂载于 /readable", result)

    async def test_load_average_fallback_and_unsupported_error(self):
        with patch.object(server.os, "getloadavg", side_effect=OSError()), \
                patch.object(server, "HAS_PSUTIL", True), \
                patch.object(server.psutil, "getloadavg", return_value=(1.0, 2.0, 3.0)):
            result = result_text(await server.mcp.call_tool("get_load_average", {}))
        self.assertIn("1分钟: 1.00", result)
        with patch.object(server.os, "getloadavg", side_effect=OSError()), \
                patch.object(server, "HAS_PSUTIL", False):
            result = result_text(await server.mcp.call_tool("get_load_average", {}))
        self.assertEqual(result, "系统负载: 当前平台不支持")


if __name__ == "__main__":
    unittest.main()
