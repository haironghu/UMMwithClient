#!/usr/bin/env bash
# 07_teardown.sh — 关闭虚拟机；--purge 时连镜像/seed/密钥一起删除。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh
cd "$WORK_DIR"

for vm in vm1 vm2; do
    if [ -f "$vm.pid" ] && kill -0 "$(cat "$vm.pid")" 2>/dev/null; then
        # 先试 monitor 优雅关机，5 秒后强杀
        echo "system_powerdown" | socat - "UNIX-CONNECT:$vm.mon" 2>/dev/null \
            || kill "$(cat "$vm.pid")" 2>/dev/null || true
        sleep 2
        kill -9 "$(cat "$vm.pid")" 2>/dev/null || true
        echo "[down] $vm 已关闭"
    fi
    rm -f "$vm.pid" "$vm.mon"
done

if [ "${1:-}" = "--purge" ]; then
    rm -f vm1.qcow2 vm2.qcow2 vm1-seed.iso vm2-seed.iso \
          "$SSD0" "$SSD1" lab_key lab_key.pub \
          result_vmA.json result_vmB.json exp-vmA.log exp-vmB.log \
          vm1-console.log vm2-console.log
    rm -rf seed-vm1 seed-vm2
    echo "[purge] work 目录已清空（云镜像 $BASE_IMG 保留，重跑可复用）"
fi
echo "== teardown 完成 =="
