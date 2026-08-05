#!/usr/bin/env bash
# 04_provision.sh — 把 UMM 源码同步到两个 VM 并编译；放行共享盘访问权限。
set -euo pipefail
cd "$(dirname "$0")"
source ./env.sh

TARBALL=/tmp/umm_lab_src.tar.gz

# apt 代理选项：经 ssh 传到 VM 内执行（双引号字符串，宿主侧展开）
APT_OPTS=""
if [ -n "${APT_PROXY:-}" ]; then
    APT_OPTS="-o Acquire::http::Proxy=$APT_PROXY -o Acquire::https::Proxy=$APT_PROXY"
fi
echo "[prov] 打包源码 $REPO_ROOT（排除 .git/构建产物/work；保留 bin/*.c 源文件）"
# 注意1：umm/bin/ 下编译产物与 .c 源文件混放（ummd.c/umms.c 等），
# 只能精确排除二进制与 .so，不能整目录排除——否则 VM 侧 make 缺源文件。
# 注意2：umm/build 和 umm/build_pic 都是构建产物目录，必须一并排除！
# libumm.so 链接用 build_pic/*.o；若旧 build_pic 混入包内，tar 保留的 mtime
# 会让 VM 侧 make 误判"目标文件比源码新"而跳过编译，链接出旧代码的 .so
# （本实验踩过：客户端 .so 是旧代码、服务端却是新代码的"新壳旧芯"事故）。
tar -czf "$TARBALL" -C "$REPO_ROOT" \
    --exclude=.git --exclude='umm/build' --exclude='umm/build_pic' \
    --exclude='umm/bin/ummd' --exclude='umm/bin/umms' \
    --exclude='umm/bin/test_*' --exclude='umm/bin/ssd_sim_test' \
    --exclude='umm/bin/umm_nds_rpc_server' --exclude='umm/bin/*.so' \
    --exclude=lab_shared_ssd/work --exclude='__pycache__' .
# 双保险：打包后立即校验 tar 内不含任何残留目标文件/构建目录。
if tar -tzf "$TARBALL" | grep -E '\.o$|/build(_pic)?/' >/dev/null; then
    echo "[prov] 失败：源码包内仍含构建产物，请检查 exclude 规则" >&2
    tar -tzf "$TARBALL" | grep -E '\.o$|/build(_pic)?/' | head -10 >&2
    exit 1
fi

provision() {  # provision <port> <vm名>
    local port=$1 vm=$2
    echo "[prov] $vm: 检查构建工具链"
    # cloud-init 包安装失败（典型：出网需代理未配 APT_PROXY）时在此 fail-fast，
    # 并给出带代理的修复命令，而不是等 make 报 command not found。
    ssh_vm "$port" "command -v make >/dev/null && command -v gcc >/dev/null \
        && command -v python3 >/dev/null" || {
        echo "[prov] $vm 缺少 make/gcc/python3（cloud-init 包安装可能失败）。" >&2
        if [ -n "$APT_PROXY" ]; then
            echo "      修复：ssh 进 $vm 执行" >&2
            echo "      sudo apt-get -o Acquire::http::Proxy=$APT_PROXY -o Acquire::https::Proxy=$APT_PROXY update && \\" >&2
            echo "      sudo apt-get -o Acquire::http::Proxy=$APT_PROXY -o Acquire::https::Proxy=$APT_PROXY install -y build-essential python3 python3-dev make" >&2
        else
            echo "      修复：ssh 进 $vm 执行 sudo apt-get update && sudo apt-get install -y build-essential python3 python3-dev make" >&2
            echo "      （若实验室出网需代理：export APT_PROXY=http://代理:端口 后重建 seed，或手工带 -o Acquire::http::Proxy= 安装）" >&2
        fi
        exit 1
    }

    echo "[prov] $vm: 上传源码"
    ssh_vm "$port" "rm -rf ~/UMM && mkdir -p ~/UMM"
    scp_vm "$port" "$TARBALL" "umm@127.0.0.1:/tmp/umm_lab_src.tar.gz"
    ssh_vm "$port" "tar -xzf /tmp/umm_lab_src.tar.gz -C ~/UMM; \
        find ~/UMM -name '*.o' -delete 2>/dev/null; \
        rm -rf ~/UMM/umm/build ~/UMM/umm/build_pic"

    echo "[prov] $vm: 编译（make -C umm，从零编译）"
    ssh_vm "$port" "make -C ~/UMM/umm -j\$(nproc) >/tmp/umm_make.log 2>&1" \
        || { echo "[prov] $vm 编译失败，末尾日志："; \
             ssh_vm "$port" "tail -30 /tmp/umm_make.log"; exit 1; }
    # 编译后自检：libumm.so 必须导出新符号，否则说明链入了陈旧目标文件。
    ssh_vm "$port" "nm -D ~/UMM/umm/build/libumm.so | grep -q ssd_transport_create_multi" \
        || { echo "[prov] $vm 自检失败：libumm.so 缺少 ssd_transport_create_multi（疑似旧目标文件）"; exit 1; }

    echo "[prov] $vm: 放行共享盘访问（udev 规则，实验室环境专用 0666）"
    # UMM 客户端/服务端以普通用户直接读写 /dev/nvmeXn1。
    # 仅限本实验 VM！生产请用专用用户组 + 精确设备匹配。
    ssh_vm "$port" "echo 'KERNEL==\"nvme[0-9]n1\", ATTRS{serial}==\"UMMSSD*\", MODE=\"0666\"' \
        | sudo tee /etc/udev/rules.d/99-umm-lab.rules >/dev/null; \
        sudo udevadm control --reload; sudo udevadm trigger; \
        ls -l /dev/nvme0n1 /dev/nvme1n1"

    # 共享内存窗口（Phase 2.5）：放行 /dev/pmem0 并确认容量与 SHM_SIZE 一致
    if [ "$MEM_TIER_BACKING" = "/dev/pmem0" ]; then
        # Ubuntu 云镜像把 virtio_pmem/nvdimm 驱动放在 linux-modules-extra 里，
        # 默认未装：PCI 上能看到 virtio-pmem（105b）但没有 /dev/pmem0。
        # 缺则经代理安装，再 modprobe；装不上直接 fail，不放行到 06。
        echo "[prov] $vm: 确认 virtio_pmem 驱动（linux-modules-extra）"
        ssh_vm "$port" "modinfo virtio_pmem >/dev/null 2>&1 || \
            sudo apt-get $APT_OPTS install -y linux-modules-extra-\$(uname -r) >/tmp/pmem_apt.log 2>&1 \
                || { echo 'linux-modules-extra 安装失败：'; tail -15 /tmp/pmem_apt.log; exit 1; }; \
            sudo modprobe virtio_pmem; \
            for i in 1 2 3 4 5; do [ -e /dev/pmem0 ] && break; sleep 1; done; \
            [ -e /dev/pmem0 ] || { echo 'modprobe 后仍无 /dev/pmem0'; dmesg | grep -i pmem | tail -5; exit 1; }"
        echo "[prov] $vm: 放行共享内存窗口 /dev/pmem0"
        local shm_bytes
        shm_bytes=$(numfmt --from=iec "$SHM_SIZE")
        ssh_vm "$port" "echo 'KERNEL==\"pmem*\", MODE=\"0666\"' \
            | sudo tee /etc/udev/rules.d/99-umm-pmem.rules >/dev/null; \
            sudo udevadm control --reload; sudo udevadm trigger; \
            ls -l /dev/pmem0 && \
            s=\$(sudo blockdev --getsize64 /dev/pmem0); \
            [ \"\$s\" = $shm_bytes ] \
                || { echo \"pmem0 容量 \$s 与 SHM_SIZE=$SHM_SIZE($shm_bytes) 不符\"; exit 1; }"
    fi

    echo "[prov] $vm 完成（libumm.so 在 ~/UMM/umm/build/）"
}

provision "$SSH_PORT_VM1" vm1
provision "$SSH_PORT_VM2" vm2
rm -f "$TARBALL"
echo "==  provisioning 完成 =="
