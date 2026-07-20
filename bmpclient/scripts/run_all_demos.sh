#!/bin/bash
set -e

cd "$(dirname "$0")/../.."

echo "=========================================="
echo "  bmpclient 功能演示（UMM 后端）"
echo "=========================================="
echo ""

# 启动 UMM 服务
echo "=== 启动 UMM 服务 ==="
UMM/scripts/start_services.sh -s 64M -d /tmp/umm_demo_ssd
echo ""

cd "bmpclient"

echo "=== 1. FineGrainedAllocator 演示 ==="
python3 scripts/demo_allocator.py

echo ""
echo "=== 2. VirtualMedia 演示 ==="
python3 scripts/demo_virtual_media.py

echo ""

# 停止 UMM 服务
cd ".."
echo "=== 停止 UMM 服务 ==="
UMM/scripts/stop_services.sh

echo ""
echo "=========================================="
echo "  所有演示已完成"
echo "=========================================="
