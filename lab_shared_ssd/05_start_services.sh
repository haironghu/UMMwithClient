#!/usr/bin/env bash
# 05_start_services.sh — 在 VM1 上启动 ummD（元数据）与 umms（分配权威）。
#
# umms 打开两块共享盘建立 ssd_pool（分配权威）；两个 VM 的客户端通过
# RPC 向它申请/释放 offset 区间，数据面则在本机盘上直接 I/O（不过网络）。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh

# 0) 前置一致性检查：设备顺序/容量必须与 POOL_DEVICES 配置一致
#    （nvme0n1=UMMSSD0、nvme1n1=UMMSSD1，容量=SSD_SIZE）
echo "[svc] 校验 VM1 上设备命名与容量"
expect_bytes=$(numfmt --from=iec "$SSD_SIZE")
ssh_vm "$SSH_PORT_VM1" "
    set -e
    s0=\$(lsblk -b -d -n -o SIZE /dev/nvme0n1)
    s1=\$(lsblk -b -d -n -o SIZE /dev/nvme1n1)
    [ \"\$s0\" = $expect_bytes ] && [ \"\$s1\" = $expect_bytes ] \
        || { echo \"设备容量与 SSD_SIZE=$SSD_SIZE($expect_bytes) 不符: \$s0 \$s1\"; exit 1; }
    lsblk -d -o NAME,SERIAL,SIZE | grep -E 'nvme|UMMSSD'
" || { echo "[svc] 设备检查失败：确认 03 启动参数与 POOL_DEVICES 顺序"; exit 1; }

# 1) umms 配置（memory_size=内存层容量：无 CXL 硬件时为 malloc 后备，
#    物理上即 VM1 的 DRAM；路线A 占 CXL 槽位/tier=1，路线B 占 DRAM 槽/tier=0。
#    MEM_TIER_BACKING=/dev/pmem0 时内存层落共享内存窗口，容量与 SHM_SIZE
#    对齐——bitmap 容量超过窗口会让越界访问 SIGBUS，必须一致）
if [ -n "$MEM_TIER_BACKING" ]; then
    mem_bytes=$(numfmt --from=iec "$SHM_SIZE")
    mem_desc="$SHM_SIZE/$MEM_TIER_KIND/backing=$MEM_TIER_BACKING（共享窗口）"
else
    mem_bytes=$(numfmt --from=iec "$MEM_TIER_SIZE")
    mem_desc="$MEM_TIER_SIZE/$MEM_TIER_KIND（私有 malloc/mock）"
fi
echo "[svc] 生成 VM1:~/umms.yaml（pool=$POOL_DEVICES，内存层=$mem_desc）"
ssh_vm "$SSH_PORT_VM1" "cat > ~/umms.yaml" <<EOF
node_id: 0
listen_addr: "0.0.0.0"
listen_port: $MEM_PORT
memory_size: $mem_bytes
memory_tier: "$MEM_TIER_KIND"
base_gpa: 0
ssd_devices: "$POOL_DEVICES"
rpc_token: "$RPC_TOKEN"
allow_cidrs: "$VM_NET"
EOF
if [ -n "$MEM_TIER_BACKING" ]; then
    ssh_vm "$SSH_PORT_VM1" "echo 'memory_device: \"$MEM_TIER_BACKING\"' >> ~/umms.yaml"
fi

# 2) 清理旧进程并启动（pkill 后必须等旧实例真正退出再启动：
#    优雅退出含 munmap 大设备映射，可能 >1s；旧实例还占着端口时新实例
#    bind 会 EADDRINUSE → FATAL 退出 → 端口无人监听，客户端全 -7）
echo "[svc] VM1 启动 ummD(:$META_PORT) 与 umms(:$MEM_PORT)"
ssh_vm "$SSH_PORT_VM1" "
    pkill -x ummD 2>/dev/null; pkill -x ummd 2>/dev/null; pkill -x umms 2>/dev/null
    for i in \$(seq 1 40); do
        pgrep -x ummD >/dev/null || pgrep -x ummd >/dev/null || pgrep -x umms >/dev/null || break
        sleep 0.5
    done
    pkill -9 -x ummD 2>/dev/null; pkill -9 -x ummd 2>/dev/null; pkill -9 -x umms 2>/dev/null
    sleep 1
    cd ~/UMM/umm
    nohup ./bin/ummd -p $META_PORT -b 0.0.0.0 > ~/ummd.log 2>&1 &
    UMM_ALLOW_BLOCK_DEVICE=1 nohup ./bin/umms -c ~/umms.yaml > ~/umms.log 2>&1 &
    sleep 2
    ss -ltn | grep -q ':$META_PORT\b' && ss -ltn | grep -q ':$MEM_PORT\b' \
        || { echo '[svc] 端口未监听'; tail -5 ~/ummd.log ~/umms.log; exit 1; }
    grep -E 'registered SSD|listening' ~/umms.log | tail -4
"

# 3) 从 VM2 验证管控面可达
ssh_vm "$SSH_PORT_VM2" "
    (exec 3<>/dev/tcp/$VM1_IP/$META_PORT && exec 3>&- 3<&-) \
        && echo '[svc] vm2 → ummD($VM1_IP:$META_PORT) 可达'
    (exec 3<>/dev/tcp/$VM1_IP/$MEM_PORT && exec 3>&- 3<&-) \
        && echo '[svc] vm2 → umms($VM1_IP:$MEM_PORT) 可达'
" || { echo "[svc] 管控面不可达，检查 VM1 防火墙/绑定地址"; exit 1; }

echo "== UMM 服务就绪：VM1 上 ummD:$META_PORT + umms:$MEM_PORT，pool=2x$SSD_SIZE =="
