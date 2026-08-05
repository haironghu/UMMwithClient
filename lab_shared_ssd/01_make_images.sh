#!/usr/bin/env bash
# 01_make_images.sh — 下载云镜像、创建两个 VM 系统盘、创建两块共享 SSD 裸盘。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# 1) 云镜像（共享 backing file，只读使用）
if [ ! -f "$BASE_IMG" ]; then
    echo "[img] 下载 $BASE_URL"
    curl -fSL --progress-bar -o "$BASE_IMG" "$BASE_URL"
else
    echo "[img] $BASE_IMG 已存在，跳过下载"
fi

# 2) 两个 VM 的系统盘（qcow2 backing，互不干扰）
for vm in vm1 vm2; do
    if [ ! -f "$vm.qcow2" ]; then
        qemu-img create -f qcow2 -b "$BASE_IMG" -F qcow2 "$vm.qcow2" "${ROOT_DISK_GB}G"
        echo "[img] $vm.qcow2 创建（backing=$BASE_IMG, ${ROOT_DISK_GB}G）"
    else
        echo "[img] $vm.qcow2 已存在，跳过"
    fi
done

# 3) 两块共享 SSD：raw 稀疏文件。同一文件将同时挂给两个 VM
#    （03 脚本用 file.locking=off 绕过 QEMU 镜像锁）。
#    注意：不要在 VM 内对这两块盘分区/格式化/挂载 —— UMM 直接裸设备读写。
for d in "$SSD0" "$SSD1"; do
    if [ ! -f "$d" ]; then
        qemu-img create -f raw "$d" "$SSD_SIZE"
        echo "[img] $d 创建（raw 稀疏, $SSD_SIZE）"
    else
        echo "[img] $d 已存在，跳过（如需重置请先 07_teardown.sh --purge）"
    fi
done

echo "== 镜像就绪：ls -lh $WORK_DIR =="
ls -lh "$WORK_DIR"
