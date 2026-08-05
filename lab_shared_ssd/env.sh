# env.sh — lab_shared_ssd 全套件的公共配置（各脚本 source 本文件）。
# 按需修改后重跑对应步骤即可；IP/MAC/端口默认值自洽，无冲突可不改。

# ---- 虚拟机资源 ----
VM_MEM_MB=${VM_MEM_MB:-4096}          # 每 VM 内存（混合池模式下 umms 与客户端
                                      # 各 malloc 一份内存层后备，2G 不够，默认 4G）
VM_SMP=${VM_SMP:-2}                   # 每 VM vCPU 数
ROOT_DISK_GB=${ROOT_DISK_GB:-20}      # 每 VM 系统盘（qcow2，backing 自云镜像）

# ---- 两块共享 SSD（QEMU NVMe 模拟，同一后端文件同时挂给两个 VM）----
SSD_SIZE=${SSD_SIZE:-8G}              # 单盘容量；qemu-img 与 umms/客户端共用此值
SSD0=${SSD0:-ssd0.raw}
SSD1=${SSD1:-ssd1.raw}

# ---- 云镜像（Ubuntu 24.04 cloudimg）----
BASE_IMG=${BASE_IMG:-noble-server-cloudimg-amd64.img}
BASE_URL=${BASE_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img}

# ---- 可选：VM 内 apt 代理（实验室出网需代理时设置）----
# 例：APT_PROXY=http://10.90.54.64:3128 ./02_make_seed.sh
# 注入 cloud-config 的 apt 段；04 的工具链预检也会用它打印修复命令。
APT_PROXY=${APT_PROXY:-}

# ---- 网络 ----
# 每 VM 两张网卡：
#   NIC1 user-mode NAT：出网（apt）+ 宿主 ssh 转发（127.0.0.1:SSH_PORT_VMx -> :22）
#   NIC2 socket 直连：VM 间 L2 链路，跑 UMM 管控面（无需 root/网桥）
SSH_PORT_VM1=${SSH_PORT_VM1:-2221}
SSH_PORT_VM2=${SSH_PORT_VM2:-2222}
SOCK_PORT=${SOCK_PORT:-12345}         # socket netdev：VM1 listen，VM2 connect
VM1_IP=${VM1_IP:-10.0.0.11}
VM2_IP=${VM2_IP:-10.0.0.12}
VM_NET=${VM_NET:-10.0.0.0/24}
MAC_WAN_VM1=${MAC_WAN_VM1:-52:54:00:aa:00:11}
MAC_WAN_VM2=${MAC_WAN_VM2:-52:54:00:aa:00:12}
MAC_P2P_VM1=${MAC_P2P_VM1:-52:54:00:bb:00:11}
MAC_P2P_VM2=${MAC_P2P_VM2:-52:54:00:bb:00:12}

# ---- UMM 服务 ----
META_PORT=${META_PORT:-20001}         # ummD（VM1）
MEM_PORT=${MEM_PORT:-20002}           # umms（VM1）
RPC_TOKEN=${RPC_TOKEN:-lab-token}     # 仅实验环境使用
UMM_NODE_VM1=${UMM_NODE_VM1:-0}       # VM1 客户端 node-id
UMM_NODE_VM2=${UMM_NODE_VM2:-1}       # VM2 客户端 node-id
POOL_DEVICES=${POOL_DEVICES:-/dev/nvme0n1:${SSD_SIZE},/dev/nvme1n1:${SSD_SIZE}}

# ---- 实验参数 ----
EXP_CHUNKS=${EXP_CHUNKS:-2}
EXP_CHUNK_SIZE=${EXP_CHUNK_SIZE:-3G}  # 3G 时 VM2 的 6G→9G 分配跨越 8G 设备边界

# ---- 混合池（内存层+SSD，无 CXL 硬件）----
# 内存层容量（umms.yaml 的 memory_size）。注意内存层在两侧都占 DRAM：
# VM1 上 umms 一份 + 客户端本地数据面一份，VM 内存（VM_MEM_MB）须能装下。
MEM_TIER_SIZE=${MEM_TIER_SIZE:-512M}
# 服务端内存层注册形态：cxl=路线A（mock CXL 槽位，零代码改动）；
#                        dram=路线B（真 DRAM tier，umms.yaml memory_tier: dram）
MEM_TIER_KIND=${MEM_TIER_KIND:-cxl}
# 客户端内存层 chunk 参数（路线A tier=1=mock CXL；路线B tier=0=真 DRAM）
EXP_MEM_TIER=${EXP_MEM_TIER:-1}
EXP_MEM_CHUNKS=${EXP_MEM_CHUNKS:-1}
EXP_MEM_CHUNK_SIZE=${EXP_MEM_CHUNK_SIZE:-128M}

# ---- S5 落盘/刷盘语义场景（A 写→fence 落盘→B invalidate→读回）----
EXP_S5=${EXP_S5:-1}                 # 1=06 跑完 S0-S4 后追加 S5 阶段；0=跳过
EXP_S5_SIZE=${EXP_S5_SIZE:-16M}     # S5 chunk 大小（页缓存效应下 16M 足够复现）
EXP_S5_TIMEOUT=${EXP_S5_TIMEOUT:-300}  # writer/编排 等待对端标记的超时秒数

# ---- 共享内存窗口（Phase 2.5：内存层从私有 malloc 升级为跨 VM 共享）----
# MEM_TIER_BACKING=/dev/pmem0 时：03 给两个 VM 挂同一块 virtio-pmem
# （宿主 $SHM_FILE 为后备，share=on），umms 与客户端的内存层后备都指向它。
# 语义：硬件一致共享内存（DAX mmap 直达宿主共享页，无 guest 页缓存隔着，
# 不需要 fence 落盘 + invalidate——与共享 SSD 的 S5 纪律形成对照）。
# 空 = 旧行为（malloc/mock 私有后备，S6 自动跳过）。
MEM_TIER_BACKING=${MEM_TIER_BACKING:-}
SHM_SIZE=${SHM_SIZE:-1G}            # 共享窗口容量（umms memory_size 须与之对齐）
SHM_FILE=${SHM_FILE:-dram_shared.raw}  # 宿主后备文件（work 目录下）

# ---- S6 共享内存窗口对照场景（A 写→屏障→B 直接读，不 invalidate）----
EXP_S6=${EXP_S6:-1}                 # 1=MEM_TIER_BACKING 非空时追加 S6；0=跳过
EXP_S6_SIZE=${EXP_S6_SIZE:-16M}
EXP_S6_TIMEOUT=${EXP_S6_TIMEOUT:-300}

# ---- 路径 ----
LAB_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$LAB_DIR/.." && pwd)  # UMM 源码树（lab_shared_ssd 的上一级）
WORK_DIR=${WORK_DIR:-$LAB_DIR/work}   # 镜像/seed/pidfile/console 日志都放这里
SSH_KEY=${SSH_KEY:-$WORK_DIR/lab_key}

ssh_vm() {  # ssh_vm <port> [cmd...]
    local port=$1; shift
    ssh -i "$SSH_KEY" -p "$port" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 -o LogLevel=ERROR umm@127.0.0.1 "$@"
}
scp_vm() {  # scp_vm <port> <src...> <dst>
    local port=$1; shift
    scp -i "$SSH_KEY" -P "$port" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR "$@"
}
