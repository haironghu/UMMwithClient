#!/usr/bin/env bash
# 02_make_seed.sh — 生成两个 VM 的 cloud-init seed ISO。
# 内容：umm 用户（免密 sudo + ssh key）、按 MAC 匹配的网络配置
#       （NIC1 DHCP 出网；NIC2 静态 10.0.0.x 跑 UMM 管控面）。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh
mkdir -p "$WORK_DIR"
cd "$WORK_DIR"

# 1) ssh 密钥（宿主机侧持有私钥）
if [ ! -f "$SSH_KEY" ]; then
    ssh-keygen -t ed25519 -N "" -C umm-lab -f "$SSH_KEY" >/dev/null
    echo "[seed] 生成 ssh 密钥 $SSH_KEY"
fi
PUBKEY=$(cat "$SSH_KEY.pub")

make_seed() {  # make_seed <vm名> <hostname> <wan_mac> <p2p_mac> <p2p_ip>
    local vm=$1 hostname=$2 wan_mac=$3 p2p_mac=$4 p2p_ip=$5
    local dir="seed-$vm"
    rm -rf "$dir"; mkdir -p "$dir"

    cat > "$dir/user-data" <<EOF
#cloud-config
hostname: $hostname
ssh_pwauth: false
users:
  - name: umm
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    lock_passwd: true
    ssh_authorized_keys:
      - $PUBKEY
package_update: true
packages:
  - build-essential
  - python3
  - python3-dev
  - make
  - ca-certificates
EOF

    # 可选：apt 代理（实验室出网需代理时 cloud-init 的 apt 才能成功）
    if [ -n "$APT_PROXY" ]; then
        cat >> "$dir/user-data" <<EOF
apt:
  proxy: "$APT_PROXY"
  http_proxy: "$APT_PROXY"
  https_proxy: "$APT_PROXY"
EOF
    fi

    cat > "$dir/meta-data" <<EOF
instance-id: umm-lab-$vm
local-hostname: $hostname
EOF

    # network-config v2：按 MAC 匹配（接口名在不同内核/机器上会变）
    cat > "$dir/network-config" <<EOF
version: 2
ethernets:
  wan:
    match:
      macaddress: "$wan_mac"
    dhcp4: true
  p2p:
    match:
      macaddress: "$p2p_mac"
    addresses: [$p2p_ip/24]
EOF

    if command -v cloud-localds >/dev/null 2>&1; then
        cloud-localds -N "$dir/network-config" "$vm-seed.iso" \
            "$dir/user-data" "$dir/meta-data"
    elif command -v genisoimage >/dev/null 2>&1; then
        genisoimage -output "$vm-seed.iso" -volid cidata -joliet -rock \
            "$dir/user-data" "$dir/meta-data" "$dir/network-config"
    else
        xorriso -as mkisofs -output "$vm-seed.iso" -volid cidata -joliet -rock \
            "$dir/user-data" "$dir/meta-data" "$dir/network-config"
    fi
    echo "[seed] $vm-seed.iso 生成（hostname=$hostname, p2p=$p2p_ip）"
}

make_seed vm1 umm-vm1 "$MAC_WAN_VM1" "$MAC_P2P_VM1" "$VM1_IP"
make_seed vm2 umm-vm2 "$MAC_WAN_VM2" "$MAC_P2P_VM2" "$VM2_IP"
echo "== seed 就绪 =="
