#!/usr/bin/env bash
# 00_check_prereqs.sh — 检查宿主机依赖，不满足则给出安装提示后退出非 0。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh

fail=0
need() {  # need <cmd> <apt包名>
    if command -v "$1" >/dev/null 2>&1; then
        echo "[ok]   $1 ($($1 --version 2>/dev/null | head -1))"
    else
        echo "[MISS] $1  ←  apt install $2"
        fail=1
    fi
}

need qemu-system-x86_64 qemu-system-x86
need qemu-img            qemu-utils
need ssh                 openssh-client
need scp                 openssh-client
need ssh-keygen          openssh-client
need curl                curl

# seed ISO 三选一
if command -v cloud-localds >/dev/null 2>&1; then
    echo "[ok]   cloud-localds (cloud-image-utils)"
elif command -v genisoimage >/dev/null 2>&1; then
    echo "[ok]   genisoimage"
elif command -v xorriso >/dev/null 2>&1; then
    echo "[ok]   xorriso"
else
    echo "[MISS] cloud-localds / genisoimage / xorriso 三选一  ←  apt install cloud-image-utils"
    fail=1
fi

# KVM 可用性（无 KVM 也能跑，TCG 纯软件模拟会慢 5-10 倍）
if [ -w /dev/kvm ]; then
    echo "[ok]   /dev/kvm 可写 → 使用 KVM 硬件加速"
else
    echo "[warn] /dev/kvm 不可用 → 回退 TCG 软件模拟（慢，但功能一致）"
fi

# 磁盘空间：系统盘 2x20G(qcow2 稀疏) + SSD 2x8G(raw 稀疏) + 云镜像 ~700M
avail=$(df -BG "$LAB_DIR" | awk 'NR==2 {gsub("G","",$4); print $4}')
if [ "${avail:-0}" -lt 16 ]; then
    echo "[warn] $LAB_DIR 可用空间 ${avail}G，稀疏盘按需增长，建议 >= 16G"
else
    echo "[ok]   磁盘可用 ${avail}G"
fi

[ "$fail" -eq 0 ] && echo "== 依赖检查通过 ==" || { echo "== 依赖缺失，请先安装 =="; exit 1; }
