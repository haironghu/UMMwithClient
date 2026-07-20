#!/usr/bin/env python3
"""bmpclient 演示脚本的公共辅助模块：检查 UMM 服务状态。"""

import os
import sys
import socket

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # bmpclient/
WORKSPACE_ROOT = os.path.dirname(PROJECT_ROOT)  # vs-workspace/
UMM_ROOT = os.path.join(WORKSPACE_ROOT, "UMM")

META_PORT = 20001
MEM_PORT = 20002
META_ADDR = f"127.0.0.1:{META_PORT}"
MEM_ADDR = f"127.0.0.1:{MEM_PORT}"


def _port_open(host: str, port: int, timeout: float = 2.0) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except Exception:
        return False


def check_services() -> None:
    """检查 UMM 服务是否已启动，未启动则报错退出。"""
    if not _port_open("127.0.0.1", META_PORT):
        print(f"错误: 无法连接到 metadata service (127.0.0.1:{META_PORT})")
        print("请先启动 UMM 服务:")
        print(f"  cd {UMM_ROOT} && ./scripts/start_services.sh")
        sys.exit(1)

    if not _port_open("127.0.0.1", MEM_PORT):
        print(f"错误: 无法连接到 memory service (127.0.0.1:{MEM_PORT})")
        print("请先启动 UMM 服务:")
        print(f"  cd {UMM_ROOT} && ./scripts/start_services.sh")
        sys.exit(1)

    print(f"✓ 服务检查通过: metadata={META_PORT}, memory={MEM_PORT}")
