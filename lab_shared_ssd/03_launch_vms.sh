#!/usr/bin/env bash
# 03_launch_vms.sh — 启动两个 QEMU 虚拟机。
#
# 拓扑：
#   VM1 (umm-vm1)                     VM2 (umm-vm2)
#   ├─ NIC1 user NAT (ssh :2221)      ├─ NIC1 user NAT (ssh :2222)
#   ├─ NIC2 10.0.0.11 ──socket netdev──┤ NIC2 10.0.0.12   ← UMM 管控面
#   └─ nvme0n1/nvme1n1 ══同一后端文件══┤ nvme0n1/nvme1n1  ← 共享 SSD
#
# 共享盘实现：QEMU NVMe 模拟设备 + 同一 raw 后端文件 + file.locking=off
# （绕过 QEMU 镜像锁；两 VM 的 I/O 最终落在宿主同一文件上）。
# 注意：NVMe 协议本身没有多主机一致性语义，本实验两个 VM 的分配区间
# 互不相交（由 umms 保证），各自只读写自己的区间，无并发写同一 LBA。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh
cd "$WORK_DIR"

if [ -w /dev/kvm ]; then ACCEL=kvm; CPU=host; else ACCEL=tcg; CPU=max; fi
echo "[launch] accel=$ACCEL cpu=$CPU mem=${VM_MEM_MB}M smp=$VM_SMP"

# 共享 SSD 的 -drive/-device 参数（两 VM 完全一致：同一文件、同一顺序、
# 同一 serial → 两 VM 内 /dev/nvme0n1、/dev/nvme1n1 顺序一致）
SSD_ARGS=(
    -drive "driver=raw,file.driver=file,file.filename=$PWD/$SSD0,file.locking=off,if=none,id=ssd0"
    -device "nvme,drive=ssd0,serial=UMMSSD0"
    -drive "driver=raw,file.driver=file,file.filename=$PWD/$SSD1,file.locking=off,if=none,id=ssd1"
    -device "nvme,drive=ssd1,serial=UMMSSD1"
)

# 共享内存窗口（Phase 2.5）：MEM_TIER_BACKING=/dev/pmem0 时，给两个 VM 挂
# 同一块 virtio-pmem——宿主 $SHM_FILE 为后备、share=on（与共享 SSD 的
# file.locking=off 同一原理：两个 QEMU 进程映射同一份宿主物理页）。
# 语义差别：DAX mmap 直达共享页，无 guest 页缓存隔着，x86 硬件 cacheline
# 一致性 → 跨 VM 即时可见，不需要 S5 那套 fence 落盘 + invalidate 纪律。
SHM_ARGS=()
if [ "$MEM_TIER_BACKING" = "/dev/pmem0" ]; then
    if [ ! -f "$SHM_FILE" ]; then
        qemu-img create -f raw "$SHM_FILE" "$SHM_SIZE" >/dev/null
        echo "[launch] $SHM_FILE 创建（共享内存窗口后备, raw $SHM_SIZE）"
    fi
    SHM_ARGS=(
        -object "memory-backend-file,id=shmem0,share=on,mem-path=$PWD/$SHM_FILE,size=$SHM_SIZE"
        -device "virtio-pmem-pci,memdev=shmem0,id=pmem0"
    )
    echo "[launch] 共享内存窗口: $SHM_FILE($SHM_SIZE) → 两 VM 的 /dev/pmem0"
elif [ -n "$MEM_TIER_BACKING" ]; then
    echo "[launch] 失败：MEM_TIER_BACKING=$MEM_TIER_BACKING 暂只支持 /dev/pmem0" >&2
    exit 1
fi

# virtio-pmem 属于内存设备：QEMU 要求 -m 用 size=...,maxmem=... 形式显式
# 声明内存扩展能力（maxmem 须 > size），否则报错 "the configuration is
# not prepared for memory devices"。不挂 pmem 时保持旧的纯容量写法。
MEM_ARG="$VM_MEM_MB"
if [ ${#SHM_ARGS[@]} -gt 0 ]; then
    case "$SHM_SIZE" in
        *G|*g) shm_mb=$(( ${SHM_SIZE%[Gg]} * 1024 )) ;;
        *M|*m) shm_mb=$(( ${SHM_SIZE%[Mm]} )) ;;
        *) echo "[launch] 失败：SHM_SIZE=$SHM_SIZE 只支持 M/G 后缀" >&2; exit 1 ;;
    esac
    MEM_ARG="size=${VM_MEM_MB}M,slots=2,maxmem=$((VM_MEM_MB + shm_mb))M"
fi

launch() {  # launch <vm> <ssh_port> <wan_mac> <p2p_mac> <socket参数>
    local vm=$1 ssh_port=$2 wan_mac=$3 p2p_mac=$4 sock=$5
    if [ -f "$vm.pid" ] && kill -0 "$(cat "$vm.pid")" 2>/dev/null; then
        echo "[launch] $vm 已在运行 (pid $(cat "$vm.pid"))，跳过"
        return
    fi
    qemu-system-x86_64 \
        -machine q35,accel=$ACCEL -cpu $CPU \
        -smp "$VM_SMP" -m "$MEM_ARG" \
        -drive "file=$vm.qcow2,if=virtio,format=qcow2" \
        -drive "file=$vm-seed.iso,if=virtio,format=raw,media=cdrom" \
        -netdev "user,id=wan,hostfwd=tcp:127.0.0.1:$ssh_port-:22" \
        -device "virtio-net-pci,netdev=wan,mac=$wan_mac" \
        -netdev "socket,id=p2p,$sock" \
        -device "virtio-net-pci,netdev=p2p,mac=$p2p_mac" \
        "${SSD_ARGS[@]}" \
        ${SHM_ARGS[@]+"${SHM_ARGS[@]}"} \
        -display none -daemonize \
        -serial "file:$vm-console.log" \
        -monitor "unix:$vm.mon,server,nowait" \
        -pidfile "$vm.pid"
    echo "[launch] $vm 启动 (pid $(cat "$vm.pid"))，console=$WORK_DIR/$vm-console.log"
}

# VM1 必须先起（socket netdev listen 方），VM2 connect 时对端须已就绪
launch vm1 "$SSH_PORT_VM1" "$MAC_WAN_VM1" "$MAC_P2P_VM1" "listen=:$SOCK_PORT"
sleep 2
launch vm2 "$SSH_PORT_VM2" "$MAC_WAN_VM2" "$MAC_P2P_VM2" "connect=127.0.0.1:$SOCK_PORT"

echo "[launch] 等待 cloud-init 完成（首次启动含 apt，约 1-3 分钟）..."
for pair in "vm1:$SSH_PORT_VM1" "vm2:$SSH_PORT_VM2"; do
    vm=${pair%%:*}; port=${pair##*:}
    for i in $(seq 1 90); do
        if ssh_vm "$port" true 2>/dev/null; then break; fi
        [ "$i" = 90 ] && { echo "[launch] $vm ssh 超时，看 $vm-console.log"; exit 1; }
        sleep 5
    done
    ci_status=$(ssh_vm "$port" cloud-init status --wait 2>/dev/null || true)
    # cloud-init 的 apt 模块失败（如出网需代理未配）时状态为 error——
    # 必须在此拦截，否则 04 编译才发现 make 不存在，排查绕远。
    if ! echo "$ci_status" | grep -q "done"; then
        echo "[launch] $vm cloud-init 未干净完成: ${ci_status:-查询失败}" >&2
        ssh_vm "$port" "tail -20 /var/log/cloud-init-output.log" >&2 || true
        echo "[launch] 排查：VM 内 apt 是否需代理（设 APT_PROXY 后重建 seed 与系统盘，" >&2
        echo "      或 VM 内手工 apt-get install build-essential python3-dev make）" >&2
        exit 1
    fi
    echo "[launch] $vm cloud-init 完成（$ci_status）"
done

# 联通性 + 共享盘可见性自检
for pair in "vm1:$SSH_PORT_VM1:$VM1_IP" "vm2:$SSH_PORT_VM2:$VM2_IP"; do
    IFS=: read -r vm port ip <<< "$pair"
    ssh_vm "$port" "ip -4 addr show | grep -q '$ip'" \
        && echo "[check] $vm p2p 地址 $ip 已配置" \
        || { echo "[check] $vm 未拿到 $ip，看 cloud-init network-config"; exit 1; }
    ssh_vm "$port" "lsblk -d -o NAME,SERIAL | grep -E 'UMMSSD[01]'" \
        || { echo "[check] $vm 未看到 UMMSSD0/1 两块 NVMe 盘"; exit 1; }
    if [ "$MEM_TIER_BACKING" = "/dev/pmem0" ]; then
        ssh_vm "$port" "ls -l /dev/pmem0" \
            || { echo "[check] $vm 未看到 /dev/pmem0（virtio-pmem 驱动未就绪？看 dmesg | grep -i pmem）"; exit 1; }
    fi
done
ssh_vm "$SSH_PORT_VM2" "ping -c1 -W2 $VM1_IP >/dev/null" \
    && echo "[check] vm2 → $VM1_IP 连通" \
    || { echo "[check] vm2 ping $VM1_IP 失败"; exit 1; }

echo "== 两个 VM 就绪：ssh_vm $SSH_PORT_VM1 / $SSH_PORT_VM2（env.sh 已定义 helper）=="
