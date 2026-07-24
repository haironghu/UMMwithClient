#!/usr/bin/env python3
"""
demo_e2e_nds.py — Phase 3 场景 D-NDS：NDS（NPU2SSD 直驱）后端端到端。

场景流程（结构镜像 demo_e2e_ssd.py）：
  1. 生成 umms 配置（SSD tier 设备来自 --spec，形如 "nds:0[+base_off]"；
     真机模式自动改写为 "nds-meta:0[+base_off]"——umms 只做分配簿记，
     不 nds_init，demo 进程是唯一 NDS 客户端，见下方"双进程部署"）
  2. 启动 umm-metadata-service（ummd）与 umm-memory-server（umms -c 配置）
  3. client 以 tier_aware 模式连接，client.enable_ssd() 上报 ummD 拓扑
  4. D0：拓扑校验（get_device_list 应看到 nds: 设备，路径/容量一致）
  5. D1：NDS 数据面回环（register_dev_mem → pool_pwrite/pread → memcmp）
  6. D2：batch 散聚 I/O（KV 页 offload 场景，128 x 8KB iov，对齐
     提供方 vLLM workload 样例 per-iov=8192）+ 负路径
  7. 关闭服务，清理临时文件

依赖注入（真机）：
  demo 进程与其启动的 umms 子进程均继承当前环境。若真实 NDS 库存在
  未声明依赖（dlopen 报 undefined symbol，如 libnds_aiv.so 依赖
  libread-write_kernel.so），export UMM_NDS_PRELOAD=<提供方库路径>
  （冒号分隔多个）即可同时惠及 demo 本地数据面与 umms 子进程。
  双进程架构（真机 libnds_aiv.so 复用 nvm_host admin queue）时另需
  export UMM_NDS_RPC_SOCKET=/tmp/nvm_host_rpc.sock，并先启动
  Process A（本仓库 umm/bin/umm_nds_rpc_server，三进程形态：
  umm_nds_rpc_server → ummD/umms → 客户端），demo 与其 umms 子进程
  均环境继承，脚本逻辑无需改动。
  另：umms 支持配置托管拉起 RPC server（umms 配置段
  nds_rpc_server_*，见 umm/config/umms_ssd_nds.yaml 与 umm/README.md
  NDS 节"运维加固 Phase 2"），可省去手工编排三进程启动顺序；
  本 demo 仍走外部 server 模式（进程由脚本/操作员显式管理），
  自身逻辑不变。

双进程部署（真机必读，nds-meta 纯分配后端）：
  umms（CPU 侧分配/元数据服务）只做 SSD 池分配簿记，不持 NPU 设备；
  本 demo 进程（对应生产中的 sglang/worker）是唯一 NDS 客户端，本地
  ssd_pool 直驱 DMA。若 umms 也用 "nds:" 后端，两个进程 nds_init 同一
  设备——真机 RPC server 单客户端串行（或 qp_id 冲突）会让第二个
  客户端卡死在 CREATE_CQ。因此真机模式下脚本自动把 umms yaml 中的
  ssd_devices 改写为 "nds-meta:<id>[+<base_off>]:<cap>"（纯分配后端：
  不 dlopen、不 nds_init、不占 RPC 连接、不 mmap，umms 侧无需
  UMM_NDS_PATH/UMM_NDS_PRELOAD/UMM_NDS_RPC_SOCKET），簿记语义与
  "nds:" 完全一致（同窗口同容量）；桩库模式不改写（umms 仍开 nds 桩
  后端，保留该路径覆盖）。

客户端库纪律（单次 nds_init）：
  客户端库（libumm.so）按拓扑/enable_ssd 构建本地数据面时遇到
  "nds:" 设备会跳过本地 backend 创建（info 日志一行），全进程仅
  下方 NdsDataPlane（ctypes 直连 ssd_pool_*）持有设备——修复前
  enable_ssd 会先 nds_init 一次，真机上 NdsDataPlane 的第二次
  nds_init 卡死在 qp 分配。跳过後 write_chunk/read_chunk 到该
  tier 返回 UMM_E_UNSUPPORTED；create_chunk（RPC 分配）与
  get_device_list（拓扑可见性）不受影响。

数据面架构（为何不走 client.write_chunk）：
  NDS 后端的 I/O 缓冲语义是 NPU device 虚拟地址，不是 host buffer——
  ssd_pool_pwrite(pool, voff, len, buf) 中的 buf 会被当作 device vaddr
  直接下发 DMA。demo_e2e_ssd.py 的 D1/D2 走 client.write_chunk（host
  bytes 经 tier_router → transport_ssd → pool_pwrite），对 NDS 后端是
  语义错误（真机会 DMA 错地址）。因此本 demo 的数据面改为在 demo 进程
  本地用 ctypes 直连 libumm.so 的 ssd_pool_* API——这正好对应真实部署
  形态：sglang worker 进程持有 HBM，本地打开 NDS 后端做数据面，RPC 仅
  用于分配（create_chunk 返回的 gpa 偏移即池虚拟偏移，单设备前提）。

窗口纪律（真机必读）：
  真机 spec 必须带 "+<base_off>" 窗口基址（如 "nds:0+0x40000000"），
  所有 I/O 落盘于 [base_off, base_off+capacity)，避开 LBA0 区域；
  不带窗口即从盘首写起，有毁盘风险。stub 模式（文件模拟盘）无此风险。

内存来源（--mem-src {auto,host,npu} / UMM_NDS_MEM_SRC，缺省 auto）：
  三种运行形态对照：
    ┌───────────────────────┬──────────┬─────────────┬────────────────────┐
    │ 形态                  │ NDS 库   │ arena       │ 用途               │
    ├───────────────────────┼──────────┼─────────────┼────────────────────┤
    │ stub + host（默认）   │ 桩库     │ host buffer │ 开发自测 / CI      │
    │ 真实库 + npu          │ 真机库   │ NPU HBM     │ 真机验收           │
    │ 真实库 + host         │ 真机库   │ host buffer │ 专家调试（显式     │
    │                       │          │             │ --mem-src host，   │
    │                       │          │             │ 带 WARNING 放行）  │
    └───────────────────────┴──────────┴─────────────┴────────────────────┘
  auto 判定：
    * UMM_NDS_PATH 由 demo 自动指向桩库（用户未显式 export）→ host；
    * 用户显式 export 了 UMM_NDS_PATH（视为真实库）→ 尝试加载
      libascendcl.so（CANN ACL 运行时）成功 → npu；失败 → 明确报错退出
      （真实库形态需要 CANN 环境：source set_env.sh 或 export
      ASCEND_HOME/LD_LIBRARY_PATH；确需调试可用 --mem-src host 强制
      专家模式）。
  安全拦截：
    * 真实库 + host arena：除显式 --mem-src host（专家模式，打印醒目
      WARNING 后放行）外一律拒绝运行——NDS 会把 host 指针当 device 地址
      下发 DMA，有越界风险；
    * 桩库 + npu arena：拒绝运行——桩库内部是 host pread/pwrite，device
      指针会段错误。
  npu 模式实现：ctypes 直连 libascendcl.so 的 aclrt 接口申请 HBM
  （aclInit → aclrtSetDevice → aclrtMalloc，device id 取自 --spec 的
  nds:<id>）；fill=aclrtMemcpy(HOST_TO_DEVICE)、readback=
  aclrtMemcpy(DEVICE_TO_HOST)、clear=aclrtMemset（符号缺失时回退
  H2D 写零），每次操作后 aclrtSynchronizeDevice 保证时序；devPtr
  存活至 pool_destroy 之后，close 顺序逐段 aclrtFree →
  aclrtResetDevice → aclFinalize。
  aclrtMalloc policy 缺省 1（ACL_MEM_MALLOC_HUGE_ONLY）——对齐 NDS
  库提供方真实测试代码（其样例即用 ACL_MEM_MALLOC_HUGE_ONLY 分配
  注册段）；可用 env UMM_NDS_ALLOC_POLICY 覆盖。

NDS 库提供方真实测试代码要点（本 demo 对齐的基准）：
  * init：g_nds.nds_init(device, queueDepth, numQueues, 4096,
    1024*1024*7)——第 5 参 max_page_num=7M 是 NDS 内部 IO 跟踪
    资源池规模（支撑 10000 iov 在途），不是单次 IO 上限（UMM 侧
    单次上限独立由 UMM_NDS_MAX_IO 控制，缺省 1MB）；
  * 多段注册：连续 nds_register(dev_mem, segment_size) 注册多个
    独立 aclrtMalloc(ACL_MEM_MALLOC_HUGE_ONLY) 段（样例 4 段），
    注册前 aclrtMemset 清零——本 demo 用 --segments 复现该模式；
  * IO 模式：IOVec{ vaddr=段基址+段内页偏移, length=8192,
    offset=ssd页号*8192 }，段选择 i % num_segments，一次
    nds_batch_read 下发整批。

用法：
  # stub + host 开发自测（CI / 无 NPU 环境，默认；
  # 自动使用 $UMM_ROOT/bin/libnds_aiv.so 桩库）
  UMM_ROOT=/path/to/umm python3 scripts/demo_e2e_nds.py

  # 真机验收（窗口 [1GB, 1GB+16GB)）：
  #   前提：CANN 环境可用（libascendcl.so 可加载），无需 torch
  source /usr/local/Ascend/ascend-toolkit/set_env.sh   # 或 export ASCEND_HOME/LD_LIBRARY_PATH
  export UMM_NDS_PATH=/path/to/libnds_aiv.so
  python3 bmpclient/scripts/demo_e2e_nds.py --spec "nds:0+0x40000000" --capacity 16G
  #   → auto 解析为 npu，arena 为 device 0 上的 32MB HBM（aclrtMalloc）

  # 数据面探针（真机 D1 失败时隔离诊断；不起 ummD/umms、不走 RPC；
  #   位置编码 pattern 可精确测 DMA 位移并识别旧运行残留）：
  #   # 第一次：写位置编码 pattern 并读回分析
  #   #   → PROBE PASS(delta=0,ratio=1) / FAIL(位移 k / 旧数据 / 全零)
  #   python3 bmpclient/scripts/demo_e2e_nds.py --probe \
  #       --spec "nds:0+0x40000000" --probe-off 0 --probe-len 8192
  #   # 第二次：跳过写直接读盘（tag 自动取盘上 dominant tag）→
  #   #   ratio≈1 且 delta 可测：写已落盘且位移在 DMA/offset 算术；
  #   #   全零/ratio=0：写未落盘
  #   python3 bmpclient/scripts/demo_e2e_nds.py --probe --probe-no-write \
  #       --spec "nds:0+0x40000000" --probe-off 0 --probe-len 8192
  #   # D1 失败根因判别（异步 DMA 源覆写竞态 vs file offset 截断）：
  #   #   E1 独立双源基线 / E2 同源覆写竞态(0/50/200ms) / E3 偏移寻址，
  #   #   结尾综合判定表给出最可能根因
  #   python3 bmpclient/scripts/demo_e2e_nds.py --probe-race \
  #       --spec "nds:0+0x40000000"
  #   # D2 batch 全失败逐级定位（1/2/N iov × 顺序/随机 × 单段/多段）：
  #   #   先 N=1（batch 路径基线）再逐级加 N / --probe-batch-shuffled /
  #   #   --probe-batch-segments 4，结尾判定表区分三种候选根因
  #   python3 bmpclient/scripts/demo_e2e_nds.py --probe-batch 1 \
  #       --spec "nds:0+0x40000000"
  #   python3 bmpclient/scripts/demo_e2e_nds.py --probe-batch 128 \
  #       --probe-batch-shuffled --probe-batch-segments 4 \
  #       --spec "nds:0+0x40000000"   # 完整复现 D2 形态

  # 专家调试（危险，仅供库提供方）：真实库 + host arena
  export UMM_NDS_PATH=/path/to/real/libnds_aiv.so
  python3 scripts/demo_e2e_nds.py --mem-src host \
      --spec "nds:0+0x40000000" --capacity 16G

环境变量（均可被命令行覆盖）：
  UMM_NDS_SPEC      设备 spec（缺省 nds:0）
  UMM_NDS_CAPACITY  容量（缺省 256M；后缀 G/M/K 或纯字节数）
  UMM_NDS_PATH      libnds_aiv.so 路径（缺省自动指向 $UMM_ROOT/bin/ 桩库；
                    显式 export 即视为真实库，参与 mem-src auto 判定）
  UMM_NDS_MEM_SRC   内存来源 auto/host/npu（缺省 auto，同 --mem-src）
  UMM_NDS_SEGMENTS  注册段数（缺省 1，同 --segments；N>1 时 arena
                    均分为 N 个独立分配段，逐段 nds_register，对齐
                    提供方测试代码的多段注册模式）
  UMM_NDS_ALLOC_POLICY  aclrtMalloc policy（缺省 1 =
                    ACL_MEM_MALLOC_HUGE_ONLY，对齐提供方测试代码）
  UMM_ASCENDCL_PATH libascendcl.so 完整路径覆盖（缺省按 $ASCEND_HOME/
                    默认安装路径/find_library("ascendcl") 顺序查找）
  UMM_ROOT          umm 目录定位
"""

import argparse
import ctypes
import ctypes.util
import os
import random
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections import Counter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PKG_DIR = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.dirname(PKG_DIR))

META_PORT = 20011          # 与 demo_e2e_ssd.py 错开，允许并行运行
MEM_PORT = 20012
MEM_SIZE = 128 * 1024 * 1024

PAGE = 4096                # NDS 缺省 page_size（对齐单位）
# 缺省 max_io = UMM_NDS_MAX_IO（1MB，与 nds_init max_page_num 资源池
# 规模解耦）；后端单次 IO 超限自动分段，D1/D2 单条 IO 均远小于该值
D1_IO = 64 * 1024          # D1 单条回环 IO 大小
ARENA_SIZE = 32 * 1024 * 1024   # 模拟 NPU HBM 的 arena（--segments 均分）

D1_CHUNK = 8 * 1024 * 1024
D2_CHUNK = 16 * 1024 * 1024
D2_NUM_IOV = 128
D2_IOV_LEN = 8192          # 一 iov = 一 NVMe 页（对齐提供方 vLLM
                           # workload 样例：req_size=8192）

# 与 include/umm.h 一致：gpa 高 8 位为 node/tier 标记
GPA_OFFSET_MASK = 0x00FFFFFFFFFFFFFF

STUB_DISK = "/tmp/nds_stub_disk.raw"   # 桩库模拟盘文件（stub_nds_aiv.cpp）


def parse_size(s: str) -> int:
    s = s.strip()
    mult = {"G": 1 << 30, "M": 1 << 20, "K": 1 << 10}
    if s and s[-1].upper() in mult:
        return int(s[:-1]) * mult[s[-1].upper()]
    return int(s, 0)


def find_umm_root() -> str:
    candidates = []
    env = os.environ.get("UMM_ROOT")
    if env:
        candidates.append(env)
    candidates += [
        os.path.join(PKG_DIR, "..", "umm"),
        os.path.join(PKG_DIR, "..", "UMM"),
        os.path.join(PKG_DIR, "..", "..", "umm"),
    ]
    for path in candidates:
        if os.path.isdir(os.path.join(path, "bin")):
            return os.path.abspath(path)
    raise FileNotFoundError(f"找不到 UMM 根目录，尝试过: {candidates}")


def find_server_binary(umm_root: str, names) -> str:
    for name in names:
        path = os.path.join(umm_root, "bin", name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise FileNotFoundError(f"在 {umm_root}/bin 下找不到任一: {names}")


def wait_port(host: str, port: int, timeout: float = 8.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def write_umms_config(spec: str, capacity: int) -> str:
    text = (
        f"node_id: 0\n"
        f'listen_addr: "127.0.0.1"\n'
        f"listen_port: {MEM_PORT}\n"
        f"memory_size: {MEM_SIZE}\n"
        f"base_gpa: 0\n"
        f'ssd_devices: "{spec}:{capacity}"\n'
    )
    fd, path = tempfile.mkstemp(prefix="umm_e2e_nds_umms_", suffix=".yaml")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    return path


def _dump_server_log(path: str, tail: int = 40) -> str:
    """读取服务进程日志末尾（失败诊断用）。"""
    try:
        with open(path, "r", errors="replace") as f:
            lines = f.readlines()
        return "".join(lines[-tail:]).rstrip()
    except OSError:
        return "(日志文件不可读)"


def start_servers(umm_root: str, cfg_path: str):
    """启动 ummD/umms。子进程继承本进程环境（含 UMM_NDS_*）。

    输出落临时日志文件而非 DEVNULL：真机上 umms 常因 NDS 库加载/
    nds_init 失败而夭折（如未 export UMM_NDS_PRELOAD / UMM_NDS_RPC_SOCKET），
    端口超时把日志末尾打印出来，避免盲猜。
    """
    ummd_bin = find_server_binary(umm_root, ["umm-metadata-service", "ummd"])
    umms_bin = find_server_binary(umm_root, ["umm-memory-server", "umms"])
    print(f"[servers] umm-metadata-service: {ummd_bin}")
    print(f"[servers] umm-memory-server   : {umms_bin} -c {cfg_path}")

    umm_dlog = tempfile.NamedTemporaryFile(
        mode="w", prefix="umm_e2e_ummd_", suffix=".log", delete=False)
    umms_log = tempfile.NamedTemporaryFile(
        mode="w", prefix="umm_e2e_umms_", suffix=".log", delete=False)
    print(f"[servers] 日志: {umm_dlog.name} / {umms_log.name}")

    proc_ummd = subprocess.Popen(
        [ummd_bin, "-p", str(META_PORT), "-b", "127.0.0.1"],
        stdout=umm_dlog, stderr=subprocess.STDOUT,
    )
    proc_umms = subprocess.Popen(
        [umms_bin, "-c", cfg_path],
        stdout=umms_log, stderr=subprocess.STDOUT,
    )
    if not wait_port("127.0.0.1", META_PORT):
        raise RuntimeError(
            "umm-metadata-service 端口未就绪，日志末尾：\n"
            + _dump_server_log(umm_dlog.name))
    if not wait_port("127.0.0.1", MEM_PORT):
        hint = ""
        if not os.environ.get("UMM_NDS_RPC_SOCKET"):
            hint = ("\n[hint] 未 export UMM_NDS_RPC_SOCKET：真机模式下 umms "
                    "走 nds-meta 纯分配后端不再 nds_init，但 demo 本地数据面"
                    "（nds: 后端）需要 RPC 引导。请先启动 umm_nds_rpc_server "
                    "并 export UMM_NDS_RPC_SOCKET。")
        raise RuntimeError(
            "umm-memory-server 端口未就绪，日志末尾：\n"
            + _dump_server_log(umms_log.name) + hint)
    print(f"[servers] ummd:{META_PORT} umms:{MEM_PORT} 已就绪")
    return proc_ummd, proc_umms


def stop_servers(*procs):
    for p in procs:
        if p and p.poll() is None:
            p.terminate()
    for p in procs:
        if p:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
    print("[servers] 已关闭")


# ---------------------------------------------------------------------------
# NDS 数据面：demo 进程本地 ctypes 直连 libumm.so 的 ssd_pool_* API。
# 对应真实部署形态：持有 HBM 的 worker 进程本地打开 NDS 后端做数据 I/O。
# ---------------------------------------------------------------------------

class UmmNdsIOVec(ctypes.Structure):
    """与 umm/src/transport/ssd_backend_nds.h 的 UmmNdsIOVec 布局一致。"""
    _fields_ = [
        ("vaddr", ctypes.c_void_p),    # NPU device 虚拟地址（stub 下为 host 指针）
        ("length", ctypes.c_uint64),
        ("offset", ctypes.c_uint64),   # 池虚拟偏移（batch API 语义）
    ]


class NdsDataPlane:
    """本地 ssd_pool 数据面（NDS 后端）。

    前提：池内单设备，RPC 分配的 chunk gpa 偏移（mask 掉 node/tier 位后）
    即本池的虚拟偏移——两端同设备同容量、虚拟布局一致。
    """

    def __init__(self, libumm_path: str, spec: str, capacity: int):
        self.lib = ctypes.CDLL(libumm_path)
        c = ctypes
        L = self.lib
        L.ssd_pool_create.restype = c.c_void_p
        L.ssd_pool_create.argtypes = []
        L.ssd_pool_destroy.restype = None
        L.ssd_pool_destroy.argtypes = [c.c_void_p]
        L.ssd_pool_add_device.restype = c.c_int
        L.ssd_pool_add_device.argtypes = [c.c_void_p, c.c_char_p, c.c_uint64]
        L.ssd_pool_register_dev_mem.restype = c.c_int
        L.ssd_pool_register_dev_mem.argtypes = [c.c_void_p, c.c_void_p,
                                                c.c_uint64]
        L.ssd_pool_pread.restype = c.c_int
        L.ssd_pool_pread.argtypes = [c.c_void_p, c.c_uint64, c.c_uint64,
                                     c.c_void_p]
        L.ssd_pool_pwrite.restype = c.c_int
        L.ssd_pool_pwrite.argtypes = [c.c_void_p, c.c_uint64, c.c_uint64,
                                      c.c_void_p]
        L.ssd_pool_batch_read.restype = c.c_int
        L.ssd_pool_batch_read.argtypes = [
            c.c_void_p, ctypes.POINTER(UmmNdsIOVec), c.c_size_t]
        L.ssd_pool_batch_write.restype = c.c_int
        L.ssd_pool_batch_write.argtypes = [
            c.c_void_p, ctypes.POINTER(UmmNdsIOVec), c.c_size_t]

        self.pool = L.ssd_pool_create()
        assert self.pool, "ssd_pool_create 失败"
        rc = L.ssd_pool_add_device(self.pool, spec.encode(), capacity)
        if rc != 0:
            raise RuntimeError(
                f"ssd_pool_add_device({spec}) 失败 rc={rc}"
                "（NDS 库缺失或设备打开失败）")

    def register_dev_mem(self, addr: int, size: int) -> None:
        rc = self.lib.ssd_pool_register_dev_mem(self.pool, addr, size)
        assert rc == 0, f"ssd_pool_register_dev_mem 失败 rc={rc}"

    def pwrite(self, voff: int, length: int, vaddr: int) -> int:
        return self.lib.ssd_pool_pwrite(self.pool, voff, length, vaddr)

    def pread(self, voff: int, length: int, vaddr: int) -> int:
        return self.lib.ssd_pool_pread(self.pool, voff, length, vaddr)

    def batch_write(self, iovs) -> int:
        return self.lib.ssd_pool_batch_write(self.pool, iovs, len(iovs))

    def batch_read(self, iovs) -> int:
        return self.lib.ssd_pool_batch_read(self.pool, iovs, len(iovs))

    def close(self) -> None:
        if self.pool:
            self.lib.ssd_pool_destroy(self.pool)
            self.pool = None


# ---------------------------------------------------------------------------
# I/O arena：D1/D2 的 pattern 填充与读回校验统一走 fill/readback 接口，
# vaddr 指针运算（arena.addr + offset）两种实现下保持一致。
# ---------------------------------------------------------------------------

class HostArena:
    """ctypes host buffer 模拟 NPU HBM（桩库形态，host pread/pwrite）。

    segments>1 时均分为 N 个独立 buffer（模拟多段注册场景，每段
    独立 nds_register）；fill/readback/clear 均带 seg 形参（缺省段0）。
    """

    SRC_NAME = "HOST arena"

    def __init__(self, size: int, segments: int = 1):
        assert segments >= 1 and size % segments == 0
        self.size = size
        self.n_segments = segments
        self.seg_size = size // segments
        self._bufs = [ctypes.create_string_buffer(self.seg_size)
                      for _ in range(segments)]
        self._addrs = [ctypes.addressof(b) for b in self._bufs]
        self.addr = self._addrs[0]      # 段0 基址（单段形态兼容）

    def seg_addr(self, seg: int) -> int:
        return self._addrs[seg]

    def fill(self, offset: int, data: bytes, seg: int = 0) -> None:
        ctypes.memmove(self._addrs[seg] + offset, data, len(data))

    def readback(self, offset: int, length: int, seg: int = 0) -> bytes:
        return ctypes.string_at(self._addrs[seg] + offset, length)

    def clear(self, offset: int, length: int, seg: int = 0) -> None:
        ctypes.memset(self._addrs[seg] + offset, 0, length)

    def close(self) -> None:
        self._bufs = []


# ---------------------------------------------------------------------------
# libascendcl.so（CANN ACL 运行时）定位：ctypes 直连 aclrt 接口，去 torch
# 依赖。查找顺序（可被 UMM_ASCENDCL_PATH 完整路径覆盖）：
#   1. $UMM_ASCENDCL_PATH
#   2. $ASCEND_HOME/lib64/libascendcl.so 与 $ASCEND_HOME/runtime/lib64/
#      libascendcl.so（ASCEND_HOME 已设时）
#   3. /usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so
#   4. ctypes.util.find_library("ascendcl")（走 ldconfig/LD_LIBRARY_PATH）
# ---------------------------------------------------------------------------

# aclrtMemcpy kind（acl/acl_rt.h 的 aclrtMemcpyKind 枚举值）
ACL_MEMCPY_HOST_TO_DEVICE = 1
ACL_MEMCPY_DEVICE_TO_HOST = 2
# aclInit 重复初始化错误码（重复初始化视为成功，幂等容忍）
ACL_ERROR_REPEAT_INIT = 100002

ASCENDCL_DEFAULT_PATH = \
    "/usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so"

ASCENDCL_HINT = (
    "请 source /usr/local/Ascend/ascend-toolkit/set_env.sh，"
    "或 export ASCEND_HOME=<toolkit 根> / 把 libascendcl.so 所在目录加入 "
    "LD_LIBRARY_PATH，或 export UMM_ASCENDCL_PATH=/完整路径/libascendcl.so")


def find_ascendcl() -> str:
    """定位 libascendcl.so；找不到抛 RuntimeError（含排查提示）。"""
    candidates = []
    env = os.environ.get("UMM_ASCENDCL_PATH")
    if env:
        candidates.append(env)
    ascend_home = os.environ.get("ASCEND_HOME")
    if ascend_home:
        candidates.append(os.path.join(ascend_home, "lib64",
                                       "libascendcl.so"))
        candidates.append(os.path.join(ascend_home, "runtime", "lib64",
                                       "libascendcl.so"))
    candidates.append(ASCENDCL_DEFAULT_PATH)
    for path in candidates:
        if os.path.isfile(path):
            return path
    found = ctypes.util.find_library("ascendcl")
    if found:
        return found
    raise RuntimeError(
        "找不到 libascendcl.so（已尝试: " + "、".join(candidates)
        + " 与 find_library('ascendcl')）。\n" + ASCENDCL_HINT)


def ascendcl_available() -> bool:
    """libascendcl.so 可定位、可 dlopen 且导出 aclrt 符号（仅加载库）。"""
    try:
        lib = ctypes.CDLL(find_ascendcl())
        return all(hasattr(lib, sym) for sym in
                   ("aclInit", "aclrtSetDevice", "aclrtMalloc",
                    "aclrtMemcpy", "aclrtFree"))
    except Exception:
        return False


class AclArena:
    """真实 NPU HBM arena（ctypes 直连 libascendcl.so 的 aclrt 接口）。

    初始化序列：aclInit(NULL)（重复初始化 rc=100002 视为成功）→
    aclrtSetDevice(dev_id) → 逐段 aclrtMalloc（policy 缺省 1
    ACL_MEM_MALLOC_HUGE_ONLY——对齐 NDS 库提供方真实测试代码，其
    注册段即用 HUGE 页分配；可用 env UMM_NDS_ALLOC_POLICY 覆盖）。
    segments>1 时均分为 N 个独立 aclrtMalloc 段（对齐提供方测试代码
    的多段 nds_register 模式，每段独立注册）。
    fill=aclrtMemcpy(H2D)、readback=aclrtMemcpy(D2H)、
    clear=aclrtMemset（符号探测失败回退 H2D 写零），每次操作后
    aclrtSynchronizeDevice 保证时序；close 顺序为逐段 aclrtFree →
    aclrtResetDevice → aclFinalize（finalize 失败仅 warn）。

    devPtr 必须存活至 ssd_pool_destroy 之后（注册期内 DMA 随时可能
    访问该地址），由调用方保证 close() 在 pool 销毁后才调用。
    """

    SRC_NAME = "NPU HBM"

    def __init__(self, size: int, dev_id: int, segments: int = 1):
        assert segments >= 1 and size % segments == 0
        c = ctypes
        lib = c.CDLL(find_ascendcl())
        lib.aclInit.restype = c.c_int
        lib.aclInit.argtypes = [c.c_char_p]
        lib.aclFinalize.restype = c.c_int
        lib.aclFinalize.argtypes = []
        lib.aclrtSetDevice.restype = c.c_int
        lib.aclrtSetDevice.argtypes = [c.c_int32]
        lib.aclrtResetDevice.restype = c.c_int
        lib.aclrtResetDevice.argtypes = [c.c_int32]
        lib.aclrtMalloc.restype = c.c_int
        lib.aclrtMalloc.argtypes = [c.POINTER(c.c_void_p), c.c_size_t,
                                    c.c_int]
        lib.aclrtFree.restype = c.c_int
        lib.aclrtFree.argtypes = [c.c_void_p]
        lib.aclrtMemcpy.restype = c.c_int
        lib.aclrtMemcpy.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p,
                                    c.c_size_t, c.c_int]
        lib.aclrtSynchronizeDevice.restype = c.c_int
        lib.aclrtSynchronizeDevice.argtypes = []
        # aclrtMemset(void *devPtr, size_t maxCount, int32_t value,
        #             size_t count)：符号探测失败（老版本 CANN）则
        # clear() 回退 H2D 写零
        self._has_memset = hasattr(lib, "aclrtMemset")
        if self._has_memset:
            lib.aclrtMemset.restype = c.c_int
            lib.aclrtMemset.argtypes = [c.c_void_p, c.c_size_t,
                                        c.c_int32, c.c_size_t]
        self._lib = lib
        self.dev_id = dev_id
        self.size = size
        self.n_segments = segments
        self.seg_size = size // segments
        self._dev_ptrs = []

        rc = lib.aclInit(None)
        if rc not in (0, ACL_ERROR_REPEAT_INIT):
            raise RuntimeError(f"aclInit 失败 rc={rc}")
        self._check(lib.aclrtSetDevice(c.c_int32(dev_id)),
                    f"aclrtSetDevice({dev_id})")

        # alloc policy：缺省 1（ACL_MEM_MALLOC_HUGE_ONLY，对齐提供方
        # 测试代码）；env UMM_NDS_ALLOC_POLICY 可覆盖
        policy = int(os.environ.get("UMM_NDS_ALLOC_POLICY", "1"), 0)
        self.alloc_policy = policy
        try:
            for s in range(segments):
                dev_ptr = c.c_void_p()
                self._check(
                    lib.aclrtMalloc(c.byref(dev_ptr), self.seg_size,
                                    policy),
                    f"aclrtMalloc(seg={s}, size={self.seg_size}, "
                    f"policy={policy})")
                if not dev_ptr.value:
                    raise RuntimeError(
                        f"aclrtMalloc(seg={s}, size={self.seg_size}) "
                        "返回空指针")
                self._dev_ptrs.append(dev_ptr.value)
        except Exception:
            for p in self._dev_ptrs:
                lib.aclrtFree(c.c_void_p(p))
            self._dev_ptrs = []
            raise
        self.addr = self._dev_ptrs[0]   # 段0 基址（单段形态兼容）

    @staticmethod
    def _check(rc: int, api: str) -> None:
        if rc != 0:
            raise RuntimeError(f"{api} 失败 rc={rc}")

    def seg_addr(self, seg: int) -> int:
        return self._dev_ptrs[seg]

    def fill(self, offset: int, data: bytes, seg: int = 0) -> None:
        # create_string_buffer 保证 host 侧可寻址；buffer 存活至
        # synchronize 完成（本帧局部变量），随后即可释放
        n = len(data)
        src = ctypes.create_string_buffer(data, n)
        self._check(self._lib.aclrtMemcpy(
            ctypes.c_void_p(self._dev_ptrs[seg] + offset), n,
            ctypes.cast(src, ctypes.c_void_p), n,
            ACL_MEMCPY_HOST_TO_DEVICE),
            f"aclrtMemcpy(H2D, {n}B @ seg{seg}+0x{offset:x})")
        self._check(self._lib.aclrtSynchronizeDevice(),
                    "aclrtSynchronizeDevice")

    def readback(self, offset: int, length: int, seg: int = 0) -> bytes:
        buf = ctypes.create_string_buffer(length)
        self._check(self._lib.aclrtMemcpy(
            ctypes.cast(buf, ctypes.c_void_p), length,
            ctypes.c_void_p(self._dev_ptrs[seg] + offset), length,
            ACL_MEMCPY_DEVICE_TO_HOST),
            f"aclrtMemcpy(D2H, {length}B @ seg{seg}+0x{offset:x})")
        self._check(self._lib.aclrtSynchronizeDevice(),
                    "aclrtSynchronizeDevice")
        return buf.raw

    def clear(self, offset: int, length: int, seg: int = 0) -> None:
        """清零 device 内存（对齐提供方测试代码的 aclrtMemset 用法）。

        优先 aclrtMemset(devPtr+off, length, 0, length)；符号缺失
        （老版本 CANN）回退 H2D 写零。
        """
        if self._has_memset:
            self._check(self._lib.aclrtMemset(
                ctypes.c_void_p(self._dev_ptrs[seg] + offset), length,
                0, length),
                f"aclrtMemset({length}B @ seg{seg}+0x{offset:x})")
            self._check(self._lib.aclrtSynchronizeDevice(),
                        "aclrtSynchronizeDevice")
        else:
            self.fill(offset, bytes(length), seg=seg)

    def close(self) -> None:
        if self._dev_ptrs:
            # 顺序：逐段 free → reset → finalize。
            # 进程退出期 CANN 可能已半拆（或 NDS 注册映射尚未解除），
            # aclrtFree/aclrtResetDevice 失败降级为 WARNING 不抛异常——
            # close 常在异常路径的 finally 中执行，再抛会掩盖原始错误。
            for p in self._dev_ptrs:
                rc = self._lib.aclrtFree(ctypes.c_void_p(p))
                if rc != 0:
                    print(f"警告: aclrtFree(0x{p:x}) rc={rc}"
                          "（忽略；若反复出现，请确认 ssd_pool_destroy/"
                          "nds_uninit 已在 aclrtFree 之前完成）",
                          file=sys.stderr)
            self._dev_ptrs = []
            rc = self._lib.aclrtResetDevice(ctypes.c_int32(self.dev_id))
            if rc != 0:
                print(f"警告: aclrtResetDevice rc={rc}（忽略，进程即将退出）",
                      file=sys.stderr)
            rc = self._lib.aclFinalize()
            if rc != 0:
                print(f"警告: aclFinalize rc={rc}（忽略，进程即将退出）",
                      file=sys.stderr)


def resolve_mem_src(requested: str, user_set_path: bool) -> str:
    """把 auto/host/npu 请求解析为最终形态 'host' | 'npu'。

    安全拦截（不满足即打印原因并退出）：
      * 桩库 + npu arena：桩库是 host pread/pwrite，device 指针会段错误；
      * 真实库 + host arena：NDS 会把 host 指针当 device 地址 DMA，有越界
        风险——auto 下拒绝；显式 host（专家模式）打印 WARNING 放行。
      * npu 形态要求 CANN 环境（libascendcl.so 可定位且可加载）。
    """
    if requested == "npu":
        if not user_set_path:
            print("错误: --mem-src npu 但 UMM_NDS_PATH 未显式提供（将使用"
                  "桩库）。桩库内部是 host pread/pwrite，把 NPU device"
                  "指针当 host 地址访问会段错误。\n"
                  "请 export UMM_NDS_PATH=/path/to/real/libnds_aiv.so 后"
                  "再使用 npu 模式。")
            sys.exit(2)
        if not ascendcl_available():
            print("错误: --mem-src npu 需要 CANN 环境（libascendcl.so "
                  "可定位且可加载），当前环境不满足。\n" + ASCENDCL_HINT)
            sys.exit(2)
        return "npu"

    if requested == "host":
        if user_set_path:
            print("!" * 68)
            print("WARNING: 专家模式 —— 真实 NDS 库 + HOST arena。")
            print("  NDS 后端会把 host 指针当作 NPU device 地址下发 DMA，")
            print("  真机上存在越界访问/数据损坏风险；仅供库提供方调试使用。")
            print("!" * 68)
        return "host"

    # auto：按 UMM_NDS_PATH 来源判定
    if not user_set_path:
        return "host"   # demo 自动指向桩库 → host arena
    if ascendcl_available():
        return "npu"    # 用户显式 export 真实库 + CANN 环境可用 → npu
    print("错误: 检测到用户显式 export UMM_NDS_PATH（视为真实 NDS 库），"
          "但当前环境无可用 CANN（libascendcl.so 找不到或加载失败）。\n"
          "真实库 + host arena 会把 host 指针当 device 地址 DMA，存在越界"
          "风险，auto 模式拒绝运行。\n"
          "需要 CANN 环境（libascendcl.so）：" + ASCENDCL_HINT + "；\n"
          "或确需调试时显式加 --mem-src host 进入专家模式。")
    sys.exit(2)


def parse_dev_id(spec: str) -> int:
    """从 "nds:<id>[+<base_off>]" 解析 device id。"""
    tail = spec.split(":", 1)[1]
    return int(tail.split("+", 1)[0], 0)


def parse_base_off(spec: str) -> int:
    """从 "nds:<id>[+<base_off>]" 解析窗口基址（无窗口返回 0）。"""
    tail = spec.split(":", 1)[1]
    if "+" not in tail:
        return 0
    return int(tail.split("+", 1)[1], 0)


# ---------------------------------------------------------------------------
# 位置编码 pattern：取代旧的 (seed+7i)&0xff 周期 pattern。
# 旧 pattern 周期 256B，真机 D1 位移 134B 与 134±256kB 无法区分，
# 也无法区分"盘上残留旧运行数据"。位置编码每 8B 单元唯一携带
# (tag, pos)，可直接测出精确位移并识别外来数据。
# ---------------------------------------------------------------------------

POS_UNIT = 8                  # 编码单元粒度（8B 小端 uint64）
POS_MASK = (1 << 48) - 1      # pos 域（低 48bit，字节偏移）


def make_pos_pattern(nbytes: int, tag: int, base: int = 0) -> bytes:
    """每 8B 一个小端 uint64：高 16bit=tag（运行标识），低 48bit=base+字节偏移。

    base 为 pattern 内部基准（字节），单元 i 的 pos = base + 8*i——
    pos 直接是"该单元应位于的 vaddr/arena 字节偏移"，位移测量单位即字节。
    nbytes 需 8 的倍数（页对齐自然满足）。
    """
    assert nbytes % POS_UNIT == 0, f"pos pattern 长度须 {POS_UNIT}B 对齐"
    n = nbytes // POS_UNIT
    hi = (tag & 0xFFFF) << 48
    return struct.pack(
        f"<{n}Q", *((hi | ((base + POS_UNIT * i) & POS_MASK))
                     for i in range(n)))


def analyze_pos_pattern(buf: bytes, tag, base: int = 0) -> dict:
    """逐 8B 单元解析 (t, pos)，与期望 (tag, base+8i) 对照。

    tag=None 表示未知运行标识（--probe-no-write 二次运行场景）：
    先从 buf 中统计 dominant tag 再按该 tag 分析。

    返回 dict：
      ok              全部单元 (t==tag 且 pos==期望)
      tag             实际用于分析的 tag（dominant tag）
      same_tag_ratio  t==tag 的单元占比
      dominant_delta  同 tag 单元中出现最多的 (pos - 期望pos) = 实际位移
                      （带符号字节数；方向约定见下）
      delta_histogram 前 5 个 (delta, 计数)
      first_bad_index 首个异常单元下标（None=全对）
      foreign_units   不同 tag 单元计数
      all_zero        buf 全零

    方向约定：delta = 实际读到的 pos − 该 buffer 位置的期望 pos。
    delta=+k 表示读回区第 i 单元装的是 vaddr+k 处的数据（DMA 源地址
    多算 k，数据被"前移"）；delta=-k 反之（源地址少算 k）。

    判读：
      dominant_delta==0 且 same_tag_ratio==1 → 数据正确；
      dominant_delta==k(≠0) 且 ratio≈1 → DMA 位移 k 字节（源地址域）；
      same_tag_ratio<1 → 读到旧运行/外来数据（写未落盘或落错位置）。
    """
    n = len(buf) // POS_UNIT
    units = struct.unpack(f"<{n}Q", buf[:n * POS_UNIT]) if n else ()
    all_zero = not any(buf)
    if tag is None:
        tag_hist = Counter(u >> 48 for u in units)
        # 全零 buf 的 dominant tag 会是 0，ratio=0 判读自然成立
        tag = tag_hist.most_common(1)[0][0] if tag_hist else 0
    hist = Counter()
    same = 0
    foreign = 0
    first_bad = None
    for i, u in enumerate(units):
        t = u >> 48
        pos = u & POS_MASK
        exp = (base + POS_UNIT * i) & POS_MASK
        if t == tag:
            same += 1
            d = pos - exp
            hist[d] += 1
            if d != 0 and first_bad is None:
                first_bad = i
        else:
            foreign += 1
            if first_bad is None:
                first_bad = i
    dominant = hist.most_common(1)[0][0] if hist else None
    return {
        # 全零 buf 的单元 0 pos=0 恰与 base 对齐、dominant 偶然为 0，
        # 须显式排除（全零 = DMA 未发生/未落盘，不是"数据正确"）
        "ok": n > 0 and same == n and dominant == 0 and not all_zero,
        "tag": tag,
        "same_tag_ratio": (same / n) if n else 0.0,
        "dominant_delta": dominant,
        "delta_histogram": hist.most_common(5),
        "first_bad_index": first_bad,
        "foreign_units": foreign,
        "all_zero": all_zero,
    }


def dump_pos_units(buf: bytes, count: int = 8, label: str = "") -> None:
    """打印前 count 个 (tag,pos) 单元（expected/actual 对照用）。"""
    n = min(count, len(buf) // POS_UNIT)
    units = struct.unpack(f"<{n}Q", buf[:n * POS_UNIT])
    text = " ".join(f"(0x{u >> 48:04x},0x{u & POS_MASK:x})" for u in units)
    print(f"  {label}[0:{n}单元]= {text}")


def pos_verdict(analysis: dict, nbytes: int, base: int,
                hints=()) -> str:
    """由 analyze 结果生成综合判定行文本。

    hints：形如 (描述, 字节值) 的候选参考值列表（如 probe_off、
    base_off）；dominant_delta 恰等于其一即在判定行指出域错误特征。
    """
    if analysis["ok"]:
        return "数据正确（delta==0, ratio==1）"
    ratio = analysis["same_tag_ratio"]
    delta = analysis["dominant_delta"]
    if analysis["all_zero"]:
        return ("读回区全零 → DMA 未发生（pread 未真正写入 device 内存，"
                "可能 nds_init/注册未生效或 DMA 静默失败；no-write 模式"
                "则为上次写未落盘）")
    if ratio >= 0.999 and delta is not None and delta != 0:
        v = (f"DMA 位移 {delta:+d} B（vaddr/offset 算术偏差；"
             f"delta=实际pos-期望pos，+k=源地址多算k）")
        for desc, val in hints:
            if delta == val:
                v += (f"；位移恰等于 {desc}(0x{val:x})——"
                      f"file offset 域错误特征（{desc} 被多加/少加一次）")
        return v
    if ratio < 0.999:
        return (f"读到旧/外来数据（same_tag_ratio={ratio:.3f}, "
                f"外来单元 {analysis['foreign_units']} 个）→ "
                "写未落盘或落错位置；配合 --probe-no-write 二次运行验证")
    return (f"无一致形态（delta 直方图 {analysis['delta_histogram']}）→ "
            "数据被打碎/多次错位混合")


def scenario_topology(client, spec: str, capacity: int):
    """D0：拓扑校验 —— ummD 应能返回本节点注册的 nds: 设备。"""
    print("\n=== D0：拓扑校验（get_device_list）===")
    devs = client.get_device_list()
    ssd = [d for d in devs if d["tier"] == 2]
    assert ssd, f"拓扑中未找到 SSD 资源: {devs}"
    d = ssd[0]
    print(f"[D0] SSD 资源: path={d['device_path']} capacity={d['capacity']}")
    assert d["device_path"] == spec, (
        f"设备路径不一致: topo={d['device_path']} spec={spec}")
    assert d["capacity"] == capacity, (
        f"容量不一致: topo={d['capacity']} spec={capacity}")
    assert spec.startswith("nds:")
    print("[D0] PASS：nds: 设备路径与容量和注册值一致")


def scenario_nds_loopback(client, dp: NdsDataPlane, arena, tag: int):
    """D1：NDS 数据面回环 —— register → pwrite → 清零 → pread → memcmp。"""
    print(f"\n=== D1：NDS 数据面回环（register_dev_mem → pool_pwrite/pread，"
          f"{arena.SRC_NAME}）===")

    # 负路径：未注册 device 内存前 I/O 必须被拒绝（NDS buf 语义是
    # device vaddr，未注册即非法）
    rc = dp.pwrite(0, PAGE, arena.addr)
    assert rc != 0, "未注册 device 内存时 pwrite 应被拒绝"
    print(f"[D1] 未注册时 pwrite → rc={rc}（预期拒绝）PASS")

    # 逐段注册（多段形态对齐提供方测试代码的连续 nds_register 用法；
    # 池级 API 每次调用向池内所有 nds 设备转发注册同一 dev_mem，
    # 多段场景逐段调用并传各段基址）
    for s in range(arena.n_segments):
        dp.register_dev_mem(arena.seg_addr(s), arena.seg_size)
        print(f"[D1] register_dev_mem(seg{s}=0x{arena.seg_addr(s):x}, "
              f"{arena.seg_size // 1024 // 1024}MB, {arena.SRC_NAME})")

    desc = client.create_chunk(D1_CHUNK, "ssd")
    # 单设备池：chunk gpa 偏移（mask node/tier 位）即池虚拟偏移
    chunk_voff = desc.base_gpa & GPA_OFFSET_MASK
    print(f"[D1] ssd chunk 已分配: base_gpa=0x{desc.base_gpa:x} "
          f"→ 池虚拟偏移 0x{chunk_voff:x}")

    # 异步安全结构：两个独立写源区 srcA/srcB（段0 内 [0,64K) 与
    # [64K,128K)），消除"覆写同源"竞态——NDS pool_pwrite 为 void 返回、
    # 无完成信号，若写为异步 DMA，pwrite 返回后立即覆写同一源缓冲，
    # DMA 会读到覆写后内容（真机 D1 失败根因之一，见 --probe-race）。
    # blk0←srcA(pat base=0)、blk1←srcB(pat base=64K)，两块均读回校验
    # （blk1 顺带验证偏移寻址，替代原"第二段不参与校验"语义）。
    # 读回区：多段形态放最后一段头部（段间交叉验证），单段形态
    # [16MB, 16MB+128KB)。
    w_seg = 0
    srcA_off, srcB_off = 0, D1_IO
    srcA = arena.seg_addr(w_seg) + srcA_off
    srcB = arena.seg_addr(w_seg) + srcB_off
    r_seg = arena.n_segments - 1
    r_off = 0 if r_seg != w_seg else 16 * 1024 * 1024
    r_dst0 = arena.seg_addr(r_seg) + r_off
    r_dst1 = r_dst0 + D1_IO
    pattern0 = make_pos_pattern(D1_IO, tag, base=0)
    pattern1 = make_pos_pattern(D1_IO, tag, base=D1_IO)

    arena.fill(srcA_off, pattern0, seg=w_seg)
    arena.fill(srcB_off, pattern1, seg=w_seg)
    t0 = time.perf_counter()
    rc = dp.pwrite(chunk_voff, D1_IO, srcA)
    assert rc == 0, f"pool_pwrite blk0 64KB 失败 rc={rc}"
    rc = dp.pwrite(chunk_voff + D1_IO, D1_IO, srcB)
    assert rc == 0, f"pool_pwrite blk1 64KB 失败 rc={rc}"
    w_ms = (time.perf_counter() - t0) * 1000

    # NDS void 返回无完成信号：200ms 是经验性 DMA 落盘等待，
    # 待库方确认完成语义（completion/poll API）后移除
    print("[D1] NOTE: NDS pool_pwrite 无完成信号，读回前 sleep 200ms "
          "等待 DMA 落盘（经验值，待库方确认完成语义后移除）")
    time.sleep(0.2)

    arena.clear(r_off, 2 * D1_IO, seg=r_seg)   # 清零读回区（blk0+blk1）
    t0 = time.perf_counter()
    rc = dp.pread(chunk_voff, D1_IO, r_dst0)
    assert rc == 0, f"pool_pread blk0 失败 rc={rc}"
    rc = dp.pread(chunk_voff + D1_IO, D1_IO, r_dst1)
    assert rc == 0, f"pool_pread blk1 失败 rc={rc}"
    r_ms = (time.perf_counter() - t0) * 1000
    for name, expected, base, back in (
            ("blk0", pattern0, 0,
             arena.readback(r_off, D1_IO, seg=r_seg)),
            ("blk1", pattern1, D1_IO,
             arena.readback(r_off + D1_IO, D1_IO, seg=r_seg))):
        if back != expected:
            # 位置编码诊断：dominant_delta/ratio/首坏单元为主，
            # 首 64B hex dump 保留辅助
            a = analyze_pos_pattern(back, tag, base=base)
            print(f"[D1] {name} 数据校验失败：tag=0x{tag:04x} "
                  f"dominant_delta={a['dominant_delta']} "
                  f"same_tag_ratio={a['same_tag_ratio']:.3f} "
                  f"foreign_units={a['foreign_units']} "
                  f"first_bad_unit={a['first_bad_index']} "
                  f"delta_histogram={a['delta_histogram']}")
            print(f"[D1] {name} 前 8 单元 (tag,pos) 对照：")
            dump_pos_units(expected, label="expected")
            dump_pos_units(back, label="actual  ")
            print(f"[D1] {name} expected[0:64]= {expected[:64].hex()}")
            print(f"[D1] {name} actual  [0:64]= {back[:64].hex()}")
            print(f"[D1] {name} 形态判定: {pos_verdict(a, D1_IO, base)}")
            raise AssertionError(
                f"D1 {name} 数据校验失败（位置编码分析见上）")
    print(f"[D1] 2x64KB 写（{w_ms:.2f}ms）→ sleep 200ms → 清零 → 读回"
          f"（{r_ms:.2f}ms）（seg{w_seg} 双源 → seg{r_seg}） blk0/blk1 "
          f"memcmp PASS（位置编码 tag=0x{tag:04x} 全匹配，"
          f"blk1 顺带验证偏移寻址）")

    # 负路径：非 page 对齐长度必须被拒绝
    rc = dp.pwrite(chunk_voff, 1000, srcA)
    assert rc != 0, "非页对齐 len 应被拒绝"
    print(f"[D1] len=1000 非页对齐 pwrite → rc={rc}（预期拒绝）PASS")

    client.delete_chunk(desc)
    print("[D1] PASS")


def scenario_batch_io(client, dp: NdsDataPlane, arena,
                      capacity: int, tag: int):
    """D2：batch 散聚 I/O（KV 页 offload）+ 串行基线对照 + 负路径。"""
    print(f"\n=== D2：batch 散聚 I/O（ssd_pool_batch_write/read，"
          f"KV 页 offload 场景，{arena.SRC_NAME}）===")

    desc = client.create_chunk(D2_CHUNK, "ssd")
    chunk_voff = desc.base_gpa & GPA_OFFSET_MASK
    print(f"[D2] ssd chunk 已分配: {D2_CHUNK // 1024 // 1024}MB, "
          f"池虚拟偏移 0x{chunk_voff:x}")

    # 128 个 8KB iov（对齐提供方 vLLM workload 样例：per-iov=NVMe 页
    # 8192 字节）。vaddr 按提供方样例风格散布各注册段：
    # 段选择 i % n_segments + 段内页偏移 i // n_segments；
    # disk offset 散布 chunk（种子固定可复现）
    n_chunk_pages = D2_CHUNK // D2_IOV_LEN
    # 异步安全结构：每段前一半为写源区、后一半为读回镜像区——
    # batch_write void 返回无完成信号，旧结构"写后 clear 同一批 vaddr
    # 再 batch_read 回同址"会在异步 DMA 下毁掉写源（真机 D1 同类竞态，
    # 见 --probe-race）。write iov vaddr 在写源区，read iov 镜像到
    # 读回区（段内偏移相同、offset 相同）；clear 只清读回区。
    half_seg = arena.seg_size // 2
    pages_per_half = half_seg // D2_IOV_LEN
    assert D2_NUM_IOV // arena.n_segments <= pages_per_half
    rng = random.Random(42)
    disk_slots = rng.sample(range(n_chunk_pages), D2_NUM_IOV)

    IOVecArray = UmmNdsIOVec * D2_NUM_IOV
    w_iovs = IOVecArray()
    r_iovs = IOVecArray()
    slots = []          # [(seg, offset_in_seg)] 写源区段内页号
    for i in range(D2_NUM_IOV):
        seg = i % arena.n_segments
        off_in_seg = (i // arena.n_segments) % pages_per_half
        slots.append((seg, off_in_seg))
        w_iovs[i].vaddr = arena.seg_addr(seg) + off_in_seg * D2_IOV_LEN
        w_iovs[i].length = D2_IOV_LEN
        w_iovs[i].offset = chunk_voff + disk_slots[i] * D2_IOV_LEN
        r_iovs[i].vaddr = (arena.seg_addr(seg) + half_seg
                           + off_in_seg * D2_IOV_LEN)
        r_iovs[i].length = D2_IOV_LEN
        r_iovs[i].offset = w_iovs[i].offset
        # 位置编码 base = 该 iov 写源在 arena 内的绝对字节偏移——
        # 校验失败时可精确区分"每个 iov 统一位移 k"（vaddr 域）与
        # "随 iov 线性变化的位移"（file offset 域）；读回区按镜像
        # 关系以同一 base 核对
        arena_off = seg * arena.seg_size + off_in_seg * D2_IOV_LEN
        arena.fill(off_in_seg * D2_IOV_LEN,
                   make_pos_pattern(D2_IOV_LEN, tag, base=arena_off),
                   seg=seg)

    total = D2_NUM_IOV * D2_IOV_LEN

    t0 = time.perf_counter()
    rc = dp.batch_write(w_iovs)
    w_ms = (time.perf_counter() - t0) * 1000
    assert rc == 0, f"batch_write 失败 rc={rc}"
    print(f"[D2] batch_write {D2_NUM_IOV}x{D2_IOV_LEN}B 一次下发: "
          f"{w_ms:.2f}ms ({total / w_ms / 1000:.1f} MB/s)")

    # NDS void 返回无完成信号：200ms 是经验性 DMA 落盘等待，
    # 待库方确认完成语义（completion/poll API）后移除
    print("[D2] NOTE: NDS batch_write 无完成信号，读回前 sleep 200ms "
          "等待 DMA 落盘（经验值，待库方确认完成语义后移除）")
    time.sleep(0.2)

    for i in range(D2_NUM_IOV):
        seg, off_in_seg = slots[i]
        arena.clear(half_seg + off_in_seg * D2_IOV_LEN, D2_IOV_LEN,
                    seg=seg)  # 清零读回镜像区（写源区保持不动）

    t0 = time.perf_counter()
    rc = dp.batch_read(r_iovs)
    r_ms = (time.perf_counter() - t0) * 1000
    assert rc == 0, f"batch_read 失败 rc={rc}"
    bad = 0
    bad_deltas = []     # [(iov_idx, dominant_delta)] 用于位移形态汇总
    for i in range(D2_NUM_IOV):
        seg, off_in_seg = slots[i]
        # 镜像核对：读回区数据应与写源区 pattern（base=写源 arena 偏移）
        # 完全一致
        arena_off = seg * arena.seg_size + off_in_seg * D2_IOV_LEN
        expected_iov = make_pos_pattern(D2_IOV_LEN, tag, base=arena_off)
        got = arena.readback(half_seg + off_in_seg * D2_IOV_LEN,
                             D2_IOV_LEN, seg=seg)
        if got != expected_iov:
            bad += 1
            a = analyze_pos_pattern(got, tag, base=arena_off)
            bad_deltas.append((i, a["dominant_delta"],
                               a["same_tag_ratio"]))
            if bad <= 4:    # 最多详打 4 个失败 iov，避免刷屏
                print(f"[D2] iov#{i} seg{seg}@0x{off_in_seg * D2_IOV_LEN:x}"
                      f"（arena_off=0x{arena_off:x}）校验失败："
                      f"dominant_delta={a['dominant_delta']} "
                      f"same_tag_ratio={a['same_tag_ratio']:.3f} "
                      f"first_bad_unit={a['first_bad_index']} "
                      f"delta_histogram={a['delta_histogram']}")
                dump_pos_units(expected_iov, label=f"iov#{i} expected")
                dump_pos_units(got, label=f"iov#{i} actual  ")
                print(f"[D2] iov#{i} expected[0:32]= "
                      f"{expected_iov[:32].hex()}")
                print(f"[D2] iov#{i} actual  [0:32]= {got[:32].hex()}")
    if bad:
        # 位移形态汇总：统一位移 k → vaddr 域偏差；随 iov（arena_off）
        # 线性变化 → file offset 域偏差；ratio<1 → 旧/外来数据
        same_tag = [(i, d) for i, d, r in bad_deltas if r >= 0.999]
        deltas = {d for _, d in same_tag}
        if len(deltas) == 1:
            d = deltas.pop()
            print(f"[D2] 位移形态: {bad} 个失败 iov 全部统一位移 "
                  f"{d:+d} B → vaddr 域偏差（DMA 源地址算术错误）")
        elif same_tag:
            print(f"[D2] 位移形态: 失败 iov 位移不一致（样本 "
                  f"{bad_deltas[:8]}）→ 随 iov 变化即 file offset 域偏差")
        else:
            print(f"[D2] 位移形态: same_tag_ratio<1 → 读到旧/外来数据"
                  f"（写未落盘或落错位置），样本 {bad_deltas[:8]}")
    assert bad == 0, f"{bad} 个 iov 数据校验失败（位置编码分析见上）"
    print(f"[D2] batch_read  {D2_NUM_IOV}x{D2_IOV_LEN}B: "
          f"{r_ms:.2f}ms ({total / r_ms / 1000:.1f} MB/s), "
          f"{D2_NUM_IOV} 个 iov 全部 memcmp PASS")

    # 串行基线：逐条 pool_pwrite/pread（写源区/读回镜像区同 batch 结构）
    t0 = time.perf_counter()
    for i in range(D2_NUM_IOV):
        rc = dp.pwrite(w_iovs[i].offset, D2_IOV_LEN, w_iovs[i].vaddr)
        assert rc == 0
    sw_ms = (time.perf_counter() - t0) * 1000
    time.sleep(0.2)   # 同上：经验性 DMA 落盘等待，不计入读计时
    t0 = time.perf_counter()
    for i in range(D2_NUM_IOV):
        rc = dp.pread(r_iovs[i].offset, D2_IOV_LEN, r_iovs[i].vaddr)
        assert rc == 0
    sr_ms = (time.perf_counter() - t0) * 1000
    print(f"[D2] 串行基线（逐条 pwrite/pread）: 写 {sw_ms:.2f}ms "
          f"读 {sr_ms:.2f}ms | batch 写加速 {sw_ms / w_ms:.2f}x "
          f"读加速 {sr_ms / r_ms:.2f}x")

    # 负路径 1：iov.length > max_io（1MB，UMM_NDS_MAX_IO）→ 后端拒绝
    bad1 = (UmmNdsIOVec * 1)(
        UmmNdsIOVec(vaddr=arena.addr, length=2 * 1024 * 1024,
                    offset=chunk_voff))
    rc = dp.batch_write(bad1)
    assert rc != 0, "length > max_io 的 iov 应被拒绝"
    print(f"[D2] 负路径：iov length=2MB > max_io 1MB → rc={rc}"
          "（预期拒绝）PASS")

    # 负路径 2：iov 越过设备末尾（池层按设备边界校验，跨设备/越界拒绝；
    # 注意池层不管 chunk 边界——越过 chunk 但落在设备内是合法的）
    bad2 = (UmmNdsIOVec * 1)(
        UmmNdsIOVec(vaddr=arena.addr, length=D2_IOV_LEN * 2,
                    offset=capacity - D2_IOV_LEN))
    rc = dp.batch_write(bad2)
    assert rc != 0, "越过设备边界的 iov 应被拒绝"
    print(f"[D2] 负路径：iov 越过设备末尾（跨设备/越界）→ rc={rc}"
          "（预期拒绝）PASS")

    client.delete_chunk(desc)
    print("[D2] PASS")


# ---------------------------------------------------------------------------
# --probe：最小数据面探针（不起 ummD/umms、不走 RPC client）。
# 用途：真机 D1 失败时隔离数据面——本模式只依赖 NDS 库 + arena，
# 序列 fill → pwrite → 清零 → pread → readback。pattern 为位置编码
# （tag=运行标识 + pos=字节偏移），analyze 直接给出精确位移
# dominant_delta 与同 tag 占比，区分"DMA 位移 / 旧运行残留 /
# 未落盘（全零）/ 位移在 vaddr 域还是 file offset 域"。
# 配合 --probe-no-write（跳过写，直接读盘 analyze，tag 自动取盘上
# dominant tag）可二次验证上次写是否真的落盘并可读出位移。
# ---------------------------------------------------------------------------

PROBE_W_OFF = 0               # 写源在 arena 头部
PROBE_R_OFF = 16 * 1024 * 1024  # 读回区偏移（远离写源，避免重叠）


def scenario_probe(dp: NdsDataPlane, arena, probe_off: int,
                   probe_len: int, no_write: bool,
                   base_off: int = 0) -> bool:
    """probe 序列；返回 True=PROBE PASS。

    pattern 为位置编码：tag=int(time.time())&0xFFFF（每次运行不同，
    打印出来便于与盘上残留对照），pos=单元字节偏移。写读回环后
    analyze_pos_pattern 精确测位移并识别外来数据。
    """
    assert arena.n_segments == 1 and arena.seg_size >= ARENA_SIZE
    if probe_len % PAGE or probe_off % PAGE:
        raise ValueError(
            f"probe-off/probe-len 必须按 page {PAGE} 对齐: "
            f"off=0x{probe_off:x} len=0x{probe_len:x}")
    if probe_len > PROBE_R_OFF - PROBE_W_OFF:
        raise ValueError(
            f"probe-len 0x{probe_len:x} 超出写源/读回区间隔 "
            f"0x{PROBE_R_OFF - PROBE_W_OFF:x}")

    tag = int(time.time()) & 0xFFFF
    pattern = make_pos_pattern(probe_len, tag, base=0)
    w_src = arena.seg_addr(0) + PROBE_W_OFF   # 段基址+段内偏移（devPtr）
    r_dst = arena.seg_addr(0) + PROBE_R_OFF
    print(f"[probe] 写源 vaddr=0x{w_src:x} 读回 vaddr=0x{r_dst:x} "
          f"（均为 arena 段基址 0x{arena.seg_addr(0):x}+偏移）")
    print(f"[probe] 位置编码 tag=0x{tag:04x}（本次运行标识，"
          f"二次运行/no-write 判读时对照）")
    print(f"[probe] pattern 前 8 单元 (tag,pos)：")
    dump_pos_units(pattern, label="expected")

    if not no_write:
        arena.fill(PROBE_W_OFF, pattern)
        t0 = time.perf_counter()
        rc = dp.pwrite(probe_off, probe_len, w_src)
        w_ms = (time.perf_counter() - t0) * 1000
        print(f"[probe] pool_pwrite(off=0x{probe_off:x}, "
              f"len={probe_len}) → rc={rc} ({w_ms:.2f}ms)")
        if rc != 0:
            print(f"PROBE FAIL(形态判定: pwrite 被拒绝 rc={rc}——"
                  "注册/对齐/容量问题，DMA 未下发)")
            return False
    else:
        print("[probe] --probe-no-write：跳过 fill 与 pwrite，"
              "直接读盘（验证上次写是否落盘；tag 自动取盘上 "
              "dominant tag，与本次运行无关）")

    arena.clear(PROBE_R_OFF, probe_len)   # 清零读回区
    t0 = time.perf_counter()
    rc = dp.pread(probe_off, probe_len, r_dst)
    r_ms = (time.perf_counter() - t0) * 1000
    print(f"[probe] pool_pread(off=0x{probe_off:x}, "
          f"len={probe_len}) → rc={rc} ({r_ms:.2f}ms)")
    if rc != 0:
        print(f"PROBE FAIL(形态判定: pread 被拒绝 rc={rc})")
        return False
    back = arena.readback(PROBE_R_OFF, probe_len)

    # no-write：本次 tag 与上次写不同，按盘上 dominant tag 分析
    a = analyze_pos_pattern(back, None if no_write else tag, base=0)
    hints = [("probe_off", probe_off)]
    if base_off:
        hints.append(("base_off(窗口基址)", base_off))
    print(f"[probe] analyze: tag=0x{a['tag']:04x} "
          f"dominant_delta={a['dominant_delta']} "
          f"same_tag_ratio={a['same_tag_ratio']:.3f} "
          f"foreign_units={a['foreign_units']} "
          f"first_bad_unit={a['first_bad_index']} "
          f"all_zero={a['all_zero']}")
    print(f"[probe] delta_histogram(前5)= {a['delta_histogram']}")
    print(f"[probe] 前 8 单元 (tag,pos) 对照：")
    dump_pos_units(pattern, label="expected")
    dump_pos_units(back, label="actual  ")
    print(f"[probe] expected[0:64]= {pattern[:64].hex()}")
    print(f"[probe] actual  [0:64]= {back[:64].hex()}")

    verdict = pos_verdict(a, probe_len, 0, hints=hints)
    if a["ok"]:
        if no_write:
            print("[probe] 读到完整位置编码 pattern —— 写确实落盘且 "
                  "读回无位移，D1 失败嫌疑转移到读回链路/一致性")
        print(f"PROBE PASS({verdict})")
        return True
    print(f"PROBE FAIL(形态判定: {verdict})")
    if no_write:
        if a["same_tag_ratio"] >= 0.999 and a["dominant_delta"]:
            print(f"[probe] 提示: no-write 模式读到上次写入数据且位移 "
                  f"{a['dominant_delta']:+d} B 可测 —— 写已落盘，"
                  "位移在 DMA/offset 算术，与缓存一致性无关")
        elif a["same_tag_ratio"] == 0 or a["all_zero"]:
            print("[probe] 提示: no-write 模式读回全零/无有效编码 = "
                  "盘上该窗口无数据，上次写未落盘（或本次读窗口与上次"
                  "写窗口不一致）")
    return False


# ---------------------------------------------------------------------------
# --probe-race：异步 DMA 源覆写竞态 vs file offset 截断 判别实验。
# 真机 D1 失败形态：同源覆写序列后 blk0 读回=覆写后内容（同 tag、恒
# +65536 位移）。两个候选根因：
#   A. nds 写为异步 DMA——pwrite 返回后 DMA 尚未读源，覆写同源导致
#      blk0 落盘为覆写后内容；
#   B. 第二次 pwrite 的 file offset 被库截断/忽略——blk1 从未被写，
#      blk0 被覆写。
# 判别关键在 blk1 内容：A → blk1=覆写后内容（两个 DMA 都读了覆写后的
# 源）；B → blk1=陈旧/外来（没人写过）。
# 复用 probe 的单池单设备直连结构（不起 ummD/umms、不走 RPC）。
# ---------------------------------------------------------------------------

RACE_IO = 64 * 1024         # 判别 IO 大小（对齐 D1）
RACE_SRC_A = 0              # 源区 A（arena 偏移）
RACE_SRC_B = RACE_IO        # 源区 B
RACE_RB = 16 * 1024 * 1024  # 读回区（远离源区）


def scenario_probe_race(dp: NdsDataPlane, arena, base_off: int = 0,
                        sync_semantics: bool = False) -> bool:
    """E1/E2/E3 三实验 + 综合判定；返回 True=E1/E3 基线检查 PASS。

    sync_semantics=True（桩库）时 pwrite 立即落盘，E2 预期读回覆写前
    内容——判定行只陈述实测事实并标注桩库语义，不武断下真机结论。
    """
    assert arena.n_segments == 1 and arena.seg_size >= ARENA_SIZE
    tag = int(time.time()) & 0xFFFF
    seg0 = arena.seg_addr(0)
    srcA = seg0 + RACE_SRC_A
    srcB = seg0 + RACE_SRC_B
    rb = seg0 + RACE_RB
    sync_note = "（桩库同步语义：pwrite 立即落盘）" if sync_semantics else ""
    print(f"[probe-race] 位置编码 tag=0x{tag:04x} 窗口基址 "
          f"base_off=0x{base_off:x}（真机必须带 +base_off，落盘于 "
          f"[base_off, base_off+capacity)）{sync_note}")
    print(f"[probe-race] srcA=0x{srcA:x} srcB=0x{srcB:x} "
          f"读回区=0x{rb:x}")

    def read_disk(pool_off: int, base: int):
        """清读回区 → pread(pool_off) → readback → analyze(base)。"""
        arena.clear(RACE_RB, RACE_IO)
        rc = dp.pread(pool_off, RACE_IO, rb)
        assert rc == 0, f"pread(off=0x{pool_off:x}) 失败 rc={rc}"
        return analyze_pos_pattern(
            arena.readback(RACE_RB, RACE_IO), tag, base=base)

    # ------------------------------------------------------------------
    # E1 基线：独立双源 + 双偏移寻址
    # ------------------------------------------------------------------
    print("\n=== E1 基线：独立源区 srcA/srcB + blk0/blk1 双偏移寻址 ===")
    arena.fill(RACE_SRC_A, make_pos_pattern(RACE_IO, tag, base=0))
    arena.fill(RACE_SRC_B, make_pos_pattern(RACE_IO, tag, base=RACE_IO))
    rc = dp.pwrite(0, RACE_IO, srcA)
    assert rc == 0, f"E1 pwrite(blk0) 失败 rc={rc}"
    rc = dp.pwrite(RACE_IO, RACE_IO, srcB)
    assert rc == 0, f"E1 pwrite(blk1) 失败 rc={rc}"
    time.sleep(0.2)   # 等待 DMA 落盘（经验值，同 D1 NOTE）
    arena.clear(RACE_RB, 2 * RACE_IO)
    rc = dp.pread(0, RACE_IO, rb)
    assert rc == 0, f"E1 pread(blk0) 失败 rc={rc}"
    rc = dp.pread(RACE_IO, RACE_IO, rb + RACE_IO)
    assert rc == 0, f"E1 pread(blk1) 失败 rc={rc}"
    e1_a0 = analyze_pos_pattern(arena.readback(RACE_RB, RACE_IO),
                                tag, base=0)
    e1_a1 = analyze_pos_pattern(arena.readback(RACE_RB + RACE_IO, RACE_IO),
                                tag, base=RACE_IO)
    e1_ok = e1_a0["ok"] and e1_a1["ok"]
    print(f"[E1] blk0: {'PASS' if e1_a0['ok'] else 'FAIL'} "
          f"(delta={e1_a0['dominant_delta']} "
          f"ratio={e1_a0['same_tag_ratio']:.3f}) | blk1: "
          f"{'PASS' if e1_a1['ok'] else 'FAIL'} "
          f"(delta={e1_a1['dominant_delta']} "
          f"ratio={e1_a1['same_tag_ratio']:.3f})")
    print(f"[E1] {'PASS：独立双源 + 双偏移寻址基线正常' if e1_ok else 'FAIL：基线异常，先排查注册/对齐/容量'}")

    # ------------------------------------------------------------------
    # E2 覆写竞态（核心判别）：fill(srcA,base=0) → pwrite(blk0) →
    # [sleep X] → 覆写 srcA=pat(base=64K, 同 tag) → 立即 pread(blk0)
    # ------------------------------------------------------------------
    print("\n=== E2 覆写竞态：pwrite 后按 0/50/200ms 时延覆写同源 ===")
    e2_new = {}     # delay_ms -> True=读到覆写后内容 / False=覆写前 / None=异常
    for delay_ms in (0, 50, 200):
        arena.fill(RACE_SRC_A, make_pos_pattern(RACE_IO, tag, base=0))
        rc = dp.pwrite(0, RACE_IO, srcA)
        assert rc == 0, f"E2 pwrite 失败 rc={rc}"
        if delay_ms:
            time.sleep(delay_ms / 1000.0)
        # 覆写同源（tag 相同，base=64K——读回 base=64K 即 DMA 读了覆写后的源）
        arena.fill(RACE_SRC_A, make_pos_pattern(RACE_IO, tag, base=RACE_IO))
        a = read_disk(0, base=0)
        if a["ok"]:
            e2_new[delay_ms] = False
            fact = "读回=pat(base=0)（覆写前内容）→ 该时延下同步（DMA 已在覆写前完成源读）"
        elif (a["same_tag_ratio"] >= 0.999
              and a["dominant_delta"] == RACE_IO):
            e2_new[delay_ms] = True
            fact = ("读回=pat(base=65536)（覆写后内容）→ 异步源读："
                    "pwrite 返回后 DMA 尚未读源，源缓冲被覆写")
        else:
            e2_new[delay_ms] = None
            fact = (f"读回形态异常：delta={a['dominant_delta']} "
                    f"ratio={a['same_tag_ratio']:.3f} "
                    f"foreign={a['foreign_units']} "
                    f"all_zero={a['all_zero']} histogram={a['delta_histogram']}")
        print(f"[E2] sleep={delay_ms}ms: {fact} {sync_note}")
    async_confirmed = e2_new.get(0) is True
    if async_confirmed:
        print("PROBE-RACE: ASYNC-WRITE 确认——NDS 写为异步，"
              "源缓冲在 DMA 完成前不可复用")

    # ------------------------------------------------------------------
    # E3 偏移寻址：先核验 E1 的 blk1（E2 只动 blk0，blk1 仍是 E1 写入的
    # srcB 内容），再打 0x10000 / 0x11000（非 64K 整数倍的页对齐偏移）
    # 两个写读点，排除低位截断。
    # ------------------------------------------------------------------
    print("\n=== E3 偏移寻址：blk1 内容核验 + 额外偏移点 ===")
    a_b1 = read_disk(RACE_IO, base=RACE_IO)
    offset_truncation = False
    if a_b1["ok"]:
        print("[E3] blk1=pat(base=65536) → 第二次 pwrite 确实落在 "
              "blk1，偏移寻址正常")
        e3_blk1_ok = True
    else:
        e3_blk1_ok = False
        offset_truncation = True
        kind = ("陈旧/外来（blk1 从未被写）"
                if (a_b1["all_zero"] or a_b1["same_tag_ratio"] < 0.999)
                else f"可解析但错位 delta={a_b1['dominant_delta']}")
        print(f"[E3] blk1 内容异常（{kind}）：delta={a_b1['dominant_delta']} "
              f"ratio={a_b1['same_tag_ratio']:.3f} "
              f"foreign={a_b1['foreign_units']} all_zero={a_b1['all_zero']} "
              f"histogram={a_b1['delta_histogram']} → file offset 域错误"
              "（疑似截断/忽略），报库方")
    e3_pts_ok = True
    for off in (0x10000, 0x11000):
        arena.fill(RACE_SRC_B, make_pos_pattern(RACE_IO, tag, base=off))
        rc = dp.pwrite(off, RACE_IO, srcB)
        assert rc == 0, f"E3 pwrite(off=0x{off:x}) 失败 rc={rc}"
        time.sleep(0.2)
        a = read_disk(off, base=off)
        ok = a["ok"]
        e3_pts_ok = e3_pts_ok and ok
        print(f"[E3] f_offset=0x{off:x} 写读: {'PASS' if ok else 'FAIL'} "
              f"(delta={a['dominant_delta']} "
              f"ratio={a['same_tag_ratio']:.3f})")
        if not ok:
            offset_truncation = True
            print(f"[E3]   histogram={a['delta_histogram']} "
                  f"foreign={a['foreign_units']} all_zero={a['all_zero']}"
                  " → file offset 域错误，报库方")
    e3_ok = e3_blk1_ok and e3_pts_ok

    # ------------------------------------------------------------------
    # 综合判定表
    # ------------------------------------------------------------------
    all_old = all(v is False for v in e2_new.values())
    print("\n=== PROBE-RACE 综合判定 ===")
    print(f"E1 基线: {'PASS（独立双源+双偏移正常）' if e1_ok else 'FAIL'}")
    if async_confirmed:
        e2_line = "异步源读实锤（@0ms 读回覆写后内容）"
    elif all_old:
        e2_line = ("各时延（0/50/200ms）均读回覆写前内容 → 同步或时延内完成"
                   + sync_note)
    else:
        e2_line = f"混合/异常形态 {e2_new}"
    print(f"E2 覆写竞态: {e2_line}")
    print(f"E3 偏移寻址: {'PASS（blk1 与额外偏移点均正常）' if e3_ok else 'FAIL（file offset 域错误特征）'}")
    if async_confirmed:
        cause = ("A：异步 DMA 源覆写竞态——pwrite void 返回无完成信号，"
                 "源缓冲复用导致先发的写读到覆写后内容；D1/D2 已改造为"
                 "异步安全结构（独立源区 + 200ms 经验等待）")
    elif offset_truncation:
        cause = ("B：file offset 截断/忽略——blk1 未被写或额外偏移点失败，"
                 "file offset 域错误，报库方")
    elif all_old and e1_ok and e3_ok:
        cause = ("未实锤 A/B：写路径同步（或时延内完成）且偏移寻址正常，"
                 "真机 D1 失败嫌疑转向 C（读偏移/一致性/其他）——"
                 "请在真机复跑 --probe-race 对照")
    else:
        cause = ("未实锤 A/B：形态不完整（见上各实验行），"
                 "建议 --probe 与 --probe-race 结果一并发库方")
    print(f"最可能根因: {cause}")
    ok = e1_ok and e3_ok
    print(f"PROBE-RACE {'PASS' if ok else 'FAIL'}"
          "（E1/E3 基线检查；E2 为诊断实验，如实报告不计入 PASS 判定）")
    return ok


# ---------------------------------------------------------------------------
# --probe-batch：batch I/O 阶梯探针（不起 ummD/umms、不走 RPC）。
# 用途：真机 D2（128 iov × 4 段 × 随机 offset）128/128 失败、而单发
# read/write（D1）全部正确时，逐级隔离 batch 路径的三个候选根因：
#   ① batch 的 IOVec.vaddr 寻址解释与单发不一致 → N=1 即失败可实锤；
#   ② kernel 要求 iov 按 SSD offset 有序/连续（提供方样例严格
#      offset=i*8192）→ 顺序过、--probe-batch-shuffled 挂可实锤；
#   ③ vaddr 跨注册段散布（提供方样例 4 段）→ 单段过、
#      --probe-batch-segments 4 挂可实锤。
# 结构复用 probe 骨架（本地池单设备直连 + AclArena/HostArena +
# register）与 D2 异步安全镜像（写源区/读回镜像区分离，clear 只清
# 读回区）。位置编码 base = 该 iov 的 SSD offset（池虚拟偏移）——
# 盘侧内容可直接反查"实际写到了哪"：读回区若出现 base=O' 的数据即
# 说明 offset/vaddr 域发生了 O'-O 的错配。
# ---------------------------------------------------------------------------


def scenario_probe_batch(dp: NdsDataPlane, arena, n_iov: int,
                         iov_len: int, probe_off: int, capacity: int,
                         shuffled: bool, base_off: int = 0) -> bool:
    """probe-batch 序列；返回 True=全部 iov PASS。"""
    n_seg = arena.n_segments
    half_seg = arena.seg_size // 2
    tag = int(time.time()) & 0xFFFF
    tag_x1 = (tag + 1) & 0xFFFF   # X1 相位独立 tag（防相位间残留污染判定）
    tag_x2 = (tag + 2) & 0xFFFF   # X2 相位独立 tag

    # disk slot 分配：缺省顺序（probe_off 起连续，对齐提供方样例
    # offset=i*8192）；--probe-batch-shuffled 随机散布整个窗口
    # （复现 D2 形态；种子固定 42 可复现，与 D2 一致）
    n_window_pages = (capacity - probe_off) // iov_len
    assert n_iov <= n_window_pages
    if shuffled:
        rng = random.Random(42)
        slots = rng.sample(range(n_window_pages), n_iov)
        off_desc = (f"随机散布窗口 [0x{probe_off:x}, 0x{capacity:x}) "
                    f"（种子 42，复现 D2 形态）")
    else:
        slots = list(range(n_iov))
        off_desc = f"顺序连续（probe_off 起，对齐提供方样例 offset=i*len）"

    # vaddr 布局（镜像 D2 异步安全结构）：iov i → 段 i%n_seg，段内页
    # i//n_seg；写源在段前半，读回镜像在段后半（段内偏移相同）
    print(f"[probe-batch] N={n_iov} iov_len={iov_len} vaddr段数={n_seg} "
          f"tag=0x{tag:04x}")
    print(f"[probe-batch] SSD offset: {off_desc}")
    IOVecArray = UmmNdsIOVec * n_iov
    w_iovs = IOVecArray()
    r_iovs = IOVecArray()
    for i in range(n_iov):
        seg = i % n_seg
        page = i // n_seg
        w_iovs[i].vaddr = arena.seg_addr(seg) + page * iov_len
        w_iovs[i].length = iov_len
        w_iovs[i].offset = probe_off + slots[i] * iov_len
        r_iovs[i].vaddr = arena.seg_addr(seg) + half_seg + page * iov_len
        r_iovs[i].length = iov_len
        r_iovs[i].offset = w_iovs[i].offset
        # 位置编码 base = 该 iov 的 SSD offset：盘侧/读回区出现
        # base=O' 的数据即反查"实际写/读到了 O'"
        arena.fill(page * iov_len,
                   make_pos_pattern(iov_len, tag, base=w_iovs[i].offset),
                   seg=seg)
    print(f"[probe-batch] 位置编码 base=各 iov 的 SSD offset"
          f"（如 iov#0 base=0x{w_iovs[0].offset:x}）")
    if base_off:
        print(f"[probe-batch] 窗口基址 base_off=0x{base_off:x}"
              f"（物理盘位置 = base_off + 池虚拟偏移）")

    t0 = time.perf_counter()
    rc = dp.batch_write(w_iovs)
    w_ms = (time.perf_counter() - t0) * 1000
    print(f"[probe-batch] ssd_pool_batch_write({n_iov} iov) → rc={rc} "
          f"({w_ms:.2f}ms)")
    if rc != 0:
        print(f"PROBE-BATCH FAIL(形态判定: batch_write 被拒绝 rc={rc}——"
              "注册/对齐/容量/max_io 问题，batch 未下发)")
        return False
    # NDS void 返回无完成信号：200ms 经验性 DMA 落盘等待（同 D1/D2 NOTE）
    print("[probe-batch] NOTE: batch_write 无完成信号，sleep 200ms "
          "等待 DMA 落盘（经验值，同 D1/D2）")
    time.sleep(0.2)

    for i in range(n_iov):
        arena.clear(half_seg + (i // n_seg) * iov_len, iov_len,
                    seg=i % n_seg)      # 只清读回镜像区

    t0 = time.perf_counter()
    rc = dp.batch_read(r_iovs)
    r_ms = (time.perf_counter() - t0) * 1000
    print(f"[probe-batch] ssd_pool_batch_read({n_iov} iov) → rc={rc} "
          f"({r_ms:.2f}ms)")
    if rc != 0:
        print(f"PROBE-BATCH FAIL(形态判定: batch_read 被拒绝 rc={rc})")
        return False

    # 盘侧反查暂存区：段 0 末尾一页（远离写源区/读回区）
    scratch = arena.seg_addr(0) + arena.seg_size - iov_len

    def disk_check(pool_off: int, base: int) -> dict:
        """清暂存区 → 单发 pread(pool_off) → readback → analyze(base)。"""
        arena.clear(arena.seg_size - iov_len, iov_len, seg=0)
        rc = dp.pread(pool_off, iov_len, scratch)
        if rc != 0:
            return {"ok": False, "rc": rc}
        return analyze_pos_pattern(
            arena.readback(arena.seg_size - iov_len, iov_len, seg=0),
            tag, base=base)

    # 逐 iov 分析
    bad = []        # [(i, analysis)]
    for i in range(n_iov):
        seg = i % n_seg
        got = arena.readback(half_seg + (i // n_seg) * iov_len, iov_len,
                             seg=seg)
        a = analyze_pos_pattern(got, tag, base=w_iovs[i].offset)
        if not a["ok"]:
            bad.append((i, a))
    n_pass = n_iov - len(bad)
    print(f"[probe-batch] PASS {n_pass}/{n_iov} iov")

    for i, a in bad[:4]:    # 最多详打 4 个失败 iov
        if a["all_zero"]:
            kind = "全零（DMA 未触达该 iov 的读回区）"
        elif a["same_tag_ratio"] < 0.999:
            kind = (f"外来数据/旧残留（same_tag_ratio="
                    f"{a['same_tag_ratio']:.3f}, foreign="
                    f"{a['foreign_units']}）")
        else:
            kind = f"delta={a['dominant_delta']:+d}（错位）"
        print(f"[probe-batch] 失败 iov#{i}: seg{i % n_seg} "
              f"offset=0x{w_iovs[i].offset:x} vaddr=0x{r_iovs[i].vaddr:x} "
              f"→ {kind} histogram={a['delta_histogram']}")

    # 盘侧反查（首个失败 iov）：单发 pread 期望 offset 与 delta 修正后
    # 的 offset 各读一次——区分"写侧落错位置"与"读侧取错位置"
    if bad:
        i, a = bad[0]
        oi = w_iovs[i].offset
        d = a["dominant_delta"]
        print(f"[probe-batch] 盘侧反查 iov#{i}（单发 pread 对照）：")
        c1 = disk_check(oi, base=oi)
        if c1.get("rc"):
            print(f"  盘@期望offset 0x{oi:x}: pread rc={c1['rc']}（拒绝）")
        elif c1["ok"]:
            print(f"  盘@期望offset 0x{oi:x}: 数据正确 → batch_write "
                  "落盘正常，异常在 batch_read 侧（读 offset/vaddr 域错配"
                  "或读回 DMA 未发生）")
        elif c1["all_zero"]:
            print(f"  盘@期望offset 0x{oi:x}: 全零 → batch_write 未触达"
                  "该 offset（该 iov 请求被丢弃/DMA 未发生）")
        else:
            print(f"  盘@期望offset 0x{oi:x}: 异常 delta="
                  f"{c1['dominant_delta']} ratio={c1['same_tag_ratio']:.3f}"
                  f" all_zero={c1['all_zero']} → batch_write 落错位置")
        if d and 0 <= oi + d and oi + d + iov_len <= capacity:
            c2 = disk_check(oi + d, base=oi + d)
            if c2.get("rc"):
                print(f"  盘@delta修正offset 0x{oi + d:x}: pread "
                      f"rc={c2['rc']}（拒绝）")
            elif c2["ok"]:
                print(f"  盘@delta修正offset 0x{oi + d:x}: 恰为读回区所见"
                      f"数据 → 读回 iov#{i} 实际取自/落于 0x{oi + d:x}"
                      f"（offset/vaddr 域偏差 {d:+d} B）")
            else:
                print(f"  盘@delta修正offset 0x{oi + d:x}: 对不上"
                      f"（delta={c2.get('dominant_delta')} ratio="
                      f"{c2.get('same_tag_ratio', 0):.3f}）——读回数据"
                      "非盘侧邻近内容，疑 vaddr 域错配（读了错误的 HBM 源）")
    # ------------------------------------------------------------------
    # X1/X2 交叉验证相位（真机证据驱动）：真实库 nds_batch_write 空转
    # （疑似未实现）、nds_batch_read 正确性未证实，而单发 read/write
    # 已验证完全正确——用单发做"金标准"交叉验证，精确定位 batch_read
    # 与 batch_write 各自的正确性（位置编码 pattern 复用上方写源区
    # 与读回镜像区，base=各 iov 的 SSD offset 不变）。
    #   X1（验证 batch_read）：逐 iov 单发 pool_pwrite 写 pattern →
    #       sleep 200ms → ssd_pool_batch_read → 逐 iov analyze；
    #   X2（验证 batch_write）：ssd_pool_batch_write → sleep 200ms →
    #       逐 iov 单发 pool_pread 读回 → 逐 iov analyze。
    # ------------------------------------------------------------------
    def analyze_iov(i: int, phase_tag: int = None) -> dict:
        """读回镜像区 iov#i → analyze(base=该 iov SSD offset)。
        phase_tag：X1/X2 相位须传各自 tag（相位间 tag 不同，
        防止上一相位写入的残留被误判为本相位 batch 写成功）。"""
        seg = i % n_seg
        got = arena.readback(half_seg + (i // n_seg) * iov_len, iov_len,
                             seg=seg)
        return analyze_pos_pattern(got, phase_tag or tag,
                                   base=w_iovs[i].offset)

    def report_cross(name: str, xbad) -> None:
        """打印交叉验证结论行 + 首坏 iov 详情（全零/外来/delta）。"""
        if not xbad:
            print(f"[{name}] PASS：{n_iov} 个 iov 全部一致"
                  f"（{'batch_read' if name == 'X1' else 'batch_write'} 正确）")
            return
        print(f"[{name}] FAIL：{len(xbad)}/{n_iov} iov 异常")
        i, a = xbad[0]
        if a["all_zero"]:
            kind = "全零（DMA 未触达读回区）"
        elif a["same_tag_ratio"] < 0.999:
            kind = (f"外来数据/旧残留（same_tag_ratio="
                    f"{a['same_tag_ratio']:.3f}, foreign="
                    f"{a['foreign_units']}）")
        else:
            kind = f"delta={a['dominant_delta']:+d}（错位）"
        print(f"[{name}] 首坏 iov#{i}: seg{i % n_seg} "
              f"offset=0x{w_iovs[i].offset:x} → {kind} "
              f"histogram={a['delta_histogram']}")

    # ---- X1：单发写 + batch_read（验证 batch_read）----
    print(f"\n=== X1 交叉验证：逐 iov 单发 pwrite（已验证金标准）→ "
          f"batch_read ===（tag=0x{tag_x1:04x}）")
    for i in range(n_iov):
        arena.fill((i // n_seg) * iov_len,
                   make_pos_pattern(iov_len, tag_x1, base=w_iovs[i].offset),
                   seg=i % n_seg)        # 重写源区 pattern（X1 独立 tag）
        rc = dp.pwrite(w_iovs[i].offset, iov_len, w_iovs[i].vaddr)
        assert rc == 0, f"X1 单发 pwrite iov#{i} 失败 rc={rc}"
    print("[X1] NOTE: sleep 200ms 等待 DMA 落盘（同 D1/D2 经验值）")
    time.sleep(0.2)
    for i in range(n_iov):
        arena.clear(half_seg + (i // n_seg) * iov_len, iov_len,
                    seg=i % n_seg)
    t0 = time.perf_counter()
    rc = dp.batch_read(r_iovs)
    x1_ms = (time.perf_counter() - t0) * 1000
    print(f"[X1] ssd_pool_batch_read({n_iov} iov) → rc={rc} "
          f"({x1_ms:.2f}ms)")
    x1_bad = []
    if rc != 0:
        print(f"[X1] batch_read 被拒绝 rc={rc}（视为全部失败）")
        x1_bad = [(i, {"all_zero": True}) for i in range(n_iov)]
    else:
        for i in range(n_iov):
            a = analyze_iov(i, tag_x1)
            if not a["ok"]:
                x1_bad.append((i, a))
    x1_ok = not x1_bad
    report_cross("X1", x1_bad)

    # ---- X2：batch_write + 单发读回（验证 batch_write）----
    print(f"\n=== X2 交叉验证：batch_write → 逐 iov 单发 pread"
          f"（已验证金标准）读回 ===（tag=0x{tag_x2:04x}）")
    for i in range(n_iov):
        arena.fill((i // n_seg) * iov_len,
                   make_pos_pattern(iov_len, tag_x2, base=w_iovs[i].offset),
                   seg=i % n_seg)   # X2 独立 tag 重填源区（与 X1 残留区分）
    t0 = time.perf_counter()
    rc = dp.batch_write(w_iovs)
    x2_ms = (time.perf_counter() - t0) * 1000
    print(f"[X2] ssd_pool_batch_write({n_iov} iov) → rc={rc} "
          f"({x2_ms:.2f}ms)")
    print("[X2] NOTE: sleep 200ms 等待 DMA 落盘（同 D1/D2 经验值）")
    time.sleep(0.2)
    x2_bad = []
    if rc != 0:
        print(f"[X2] batch_write 被拒绝 rc={rc}（视为全部失败）")
        x2_bad = [(i, {"all_zero": True}) for i in range(n_iov)]
    else:
        for i in range(n_iov):
            arena.clear(half_seg + (i // n_seg) * iov_len, iov_len,
                        seg=i % n_seg)
            rc = dp.pread(r_iovs[i].offset, iov_len, r_iovs[i].vaddr)
            assert rc == 0, f"X2 单发 pread iov#{i} 失败 rc={rc}"
            a = analyze_iov(i, tag_x2)
            if not a["ok"]:
                x2_bad.append((i, a))
    x2_ok = not x2_bad
    report_cross("X2", x2_bad)

    ok = not bad and x1_ok and x2_ok

    # ------------------------------------------------------------------
    # 判定表（逐级定位；标注本次运行所在行）
    # ------------------------------------------------------------------
    here = []
    if n_iov == 1:
        here.append(1)
    if not shuffled and n_seg == 1 and n_iov == 128 and ok:
        here.append(4)
    print("\n=== PROBE-BATCH 判定表（逐级定位 batch 根因）===")
    print(f"本次配置: N={n_iov} len={iov_len} "
          f"offset={'随机' if shuffled else '顺序'} vaddr段数={n_seg} "
          f"→ {'PASS' if ok else f'FAIL({len(bad)}/{n_iov})'}")
    rows = [
        "[1] N=1 失败 → batch 路径本身异常（即使单 iov）：IOVec 布局/"
        "vaddr 解释与库不符，报库方对照头文件",
        "[2] N=1 过、顺序 N>1 过、shuffled 挂 → kernel 要求有序/连续 "
        "offset，D2 改为有序或报库方",
        "[3] 单段过、多段挂 → 跨段 batch 在当前库版本异常，报库方"
        "（提供方样例即多段）",
        "[4] 顺序单段 N=128 全过 → D2 的随机散布是唯一触发条件",
        "[X] X1 过 X2 挂 → 库 batch_write 未实现/异常：export "
        "UMM_NDS_BATCH_WRITE_EMULATE=1 过渡（单发循环模拟，性能低于真 "
        "batch，见 ssd_backend_nds.c 头注释），并报库方",
        "[X] X1 挂 → batch_read 也有问题，报库方（附 X1 证据：首坏 iov "
        "全零/外来/delta 形态与 analyze 输出）",
    ]
    print(f"X1（单发写+batch_read 交叉验证）: "
          f"{'PASS' if x1_ok else f'FAIL({len(x1_bad)}/{n_iov})'}")
    print(f"X2（batch_write+单发读回交叉验证）: "
          f"{'PASS' if x2_ok else f'FAIL({len(x2_bad)}/{n_iov})'}")
    for idx, row in enumerate(rows, 1):
        mark = ""
        if idx == 1 and n_iov == 1:
            mark = f"  ← 本次：{'FAIL，命中此根因' if not ok else 'PASS，排除'}"
        elif idx == 2 and n_iov > 1 and n_seg == 1:
            if not shuffled and ok:
                mark = "  ← 本次：顺序 PASS（前半成立）"
            elif shuffled and not ok:
                mark = "  ← 本次：shuffled FAIL（若 N=1 与顺序均过则命中）"
        elif idx == 3 and n_seg > 1:
            mark = (f"  ← 本次：多段 {'FAIL（若单段过则命中）' if not ok else 'PASS，排除'}")
        elif idx == 4 and idx in here:
            mark = "  ← 本次：命中（顺序单段 N=128 全过）"
        print(row + mark)
    print("逐级命令序列：--probe-batch 1 → --probe-batch 8 → "
          "--probe-batch 128 → --probe-batch 128 --probe-batch-shuffled → "
          "--probe-batch 128 --probe-batch-segments 4 → "
          "--probe-batch 128 --probe-batch-shuffled "
          "--probe-batch-segments 4（完整复现 D2 形态）")
    print(f"PROBE-BATCH {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    parser = argparse.ArgumentParser(
        description="Phase 3 场景 D-NDS：NDS（NPU2SSD 直驱）后端 e2e")
    parser.add_argument(
        "--spec",
        default=os.environ.get("UMM_NDS_SPEC", "nds:0"),
        help='NDS 设备 spec "nds:<device_id>[+<base_off>]"'
             "（真机必须带 +base_off 窗口）",
    )
    parser.add_argument(
        "--capacity",
        default=os.environ.get("UMM_NDS_CAPACITY", "256M"),
        help="NDS 窗口容量（后缀 G/M/K 或字节数，缺省 256M）",
    )
    parser.add_argument(
        "--mem-src",
        choices=["auto", "host", "npu"],
        default=os.environ.get("UMM_NDS_MEM_SRC", "auto"),
        help="I/O arena 内存来源：host=ctypes buffer（桩库形态）；"
             "npu=aclrt(libascendcl.so) 申请的真实 HBM（真实库形态）；"
             "auto=按 UMM_NDS_PATH 是否为用户显式 export 自动判定"
             "（缺省 auto，env UMM_NDS_MEM_SRC）",
    )
    parser.add_argument(
        "--segments",
        type=int,
        default=int(os.environ.get("UMM_NDS_SEGMENTS", "1")),
        help="注册段数（缺省 1，env UMM_NDS_SEGMENTS）：N>1 时 arena "
             "均分为 N 个独立分配段（npu 形态为 N 次 aclrtMalloc），"
             "逐段 nds_register——对齐 NDS 提供方测试代码的多段注册"
             "模式（其样例为 4 段）",
    )
    parser.add_argument(
        "--probe",
        action="store_true",
        help="最小数据面探针模式：不起 ummD/umms、不走 RPC client，"
             "直接本地 ssd_pool + arena 做 fill→pwrite→清零→pread→"
             "readback 回环，输出差异诊断（区分全零/旧数据/部分匹配/"
             "错位），结尾 PROBE PASS/FAIL",
    )
    parser.add_argument(
        "--probe-len",
        default=os.environ.get("UMM_NDS_PROBE_LEN", "8192"),
        help="probe IO 长度（须页对齐，缺省 8192，env UMM_NDS_PROBE_LEN）",
    )
    parser.add_argument(
        "--probe-off",
        default=os.environ.get("UMM_NDS_PROBE_OFF", "0"),
        help="probe 池虚拟偏移（须页对齐，缺省 0，env UMM_NDS_PROBE_OFF）",
    )
    parser.add_argument(
        "--probe-race",
        action="store_true",
        help="异步判别探针（不起 ummD/umms、不走 RPC）：判别真机 D1 "
             "失败根因——E1 独立双源基线 / E2 同源覆写竞态（0/50/200ms "
             "时延变体，区分异步 DMA 源读与同步写）/ E3 偏移寻址核验"
             "（blk1 内容 + f_offset 0x10000/0x11000 额外点，排除 "
             "file offset 截断），结尾输出综合判定表与最可能根因",
    )
    parser.add_argument(
        "--probe-batch",
        type=int,
        nargs="?",
        const=8,
        default=None,
        metavar="N",
        help="batch I/O 阶梯探针（不起 ummD/umms、不走 RPC）：定位真机 "
             "batch I/O 全失败根因。N=iov 数量（缺省 8；1 为关键基线——"
             "单 iov 即挂说明 batch 路径本身异常）。序列：fill 各 iov 源"
             "（位置编码 base=SSD offset）→ batch_write → sleep 200ms → "
             "清读回镜像区 → batch_read → 逐 iov analyze + 盘侧反查；"
             "随后追加 X1/X2 交叉验证相位（单发金标准分别验证 "
             "batch_read/batch_write），结尾输出逐级判定表（含 "
             "UMM_NDS_BATCH_WRITE_EMULATE 过渡指引）",
    )
    parser.add_argument(
        "--probe-batch-len",
        default=os.environ.get("UMM_NDS_PROBE_BATCH_LEN", "8192"),
        help="probe-batch 每 iov 长度（须页对齐，缺省 8192 对齐提供方"
             "样例，env UMM_NDS_PROBE_BATCH_LEN）",
    )
    parser.add_argument(
        "--probe-batch-shuffled",
        action="store_true",
        help="probe-batch SSD offset 缺省顺序连续（probe_off 起，对齐"
             "提供方样例 offset=i*len）；加此 flag 后随机散布整个窗口"
             "（种子 42，复现 D2 形态）",
    )
    parser.add_argument(
        "--probe-batch-segments",
        type=int,
        default=int(os.environ.get("UMM_NDS_PROBE_BATCH_SEGMENTS", "1")),
        help="probe-batch vaddr 散布段数（缺省 1=单段隔离变量；4=复现 "
             "D2/提供方样例多段注册形态，env "
             "UMM_NDS_PROBE_BATCH_SEGMENTS）",
    )
    parser.add_argument(
        "--probe-no-write",
        action="store_true",
        help="跳过 fill 与 pwrite，直接 pread 读盘后 dump——二次运行"
             "验证上次 --probe 写是否真落盘",
    )
    args = parser.parse_args()

    spec = args.spec
    capacity = parse_size(args.capacity)
    if not spec.startswith("nds:"):
        print(f"错误: spec 必须以 nds: 开头: {spec}")
        sys.exit(2)
    segments = args.segments
    if segments < 1:
        print(f"错误: --segments 必须 >= 1: {segments}")
        sys.exit(2)
    if ARENA_SIZE % segments != 0:
        print(f"错误: arena {ARENA_SIZE} 不能均分为 {segments} 段")
        sys.exit(2)
    if (ARENA_SIZE // segments) % PAGE != 0:
        print(f"错误: 段大小 {ARENA_SIZE // segments} 未按 page {PAGE} "
              "对齐（nds_register 要求）")
        sys.exit(2)

    umm_root = find_umm_root()
    os.environ.setdefault("UMM_BUILD_DIR", os.path.join(umm_root, "build"))

    # NDS 库定位：真机用户 export UMM_NDS_PATH 覆盖；缺省指向
    # $UMM_ROOT/bin/libnds_aiv.so 桩库（文件模拟 SSD）。demo 进程自身
    # 与 umms 子进程（继承环境）都依赖该变量。
    # 注意：user_set_path 必须在 setdefault 之前采样——它同时是
    # mem-src auto 判定与安全拦截的依据（显式 export 即视为真实库）。
    stub_lib = os.path.join(umm_root, "bin", "libnds_aiv.so")
    user_set_path = bool(os.environ.get("UMM_NDS_PATH"))
    if not user_set_path:
        if os.path.isfile(stub_lib):
            os.environ.setdefault("UMM_NDS_PATH", stub_lib)
        else:
            print(f"错误: 使用 nds: 设备需要 libnds_aiv.so，但 UMM_NDS_PATH "
                  f"未设置且桩库不存在: {stub_lib}\n"
                  f"请先构建（make test）或 export UMM_NDS_PATH="
                  f"/path/to/libnds_aiv.so")
            sys.exit(2)
    is_stub = os.environ["UMM_NDS_PATH"] == stub_lib

    # 内存来源解析 + 安全拦截（在任何服务启动前完成，失败即退出）
    mem_src = resolve_mem_src(args.mem_src, user_set_path)

    print(f"[setup] NDS spec={spec}")
    print(f"[setup] 容量={capacity} ({capacity / 1024 / 1024:.0f}MB)")
    print(f"[setup] UMM_NDS_PATH={os.environ['UMM_NDS_PATH']} "
          f"({'stub 文件模拟盘' if is_stub else '真机库'}"
          f"{', 用户显式 export' if user_set_path else ', demo 默认指向'})")
    print(f"[setup] mem-src: --mem-src={args.mem_src} → {mem_src}")
    print(f"[setup] segments={segments} "
          f"（每段 {ARENA_SIZE // segments // 1024 // 1024}MB，"
          f"逐段 nds_register，对齐提供方多段注册模式）")
    print(f"[setup] UMM_ROOT={umm_root}")
    if not is_stub:
        # 真机库前置体检：RPC admin queue 环境变量（缺了 umms 会在
        # nds_init 内 abort，端口超时后才能在日志里看到，提前亮出来）
        if not os.environ.get("UMM_NDS_PRELOAD"):
            print("[setup] WARNING: 未 export UMM_NDS_PRELOAD——若 libnds_aiv.so "
                  "含未定义符号（如 readwrite_demo/libnvm_host 依赖），加载将走 "
                  "RTLD_LAZY 回退或直接失败")
        if not os.environ.get("UMM_NDS_RPC_SOCKET"):
            print("[setup] WARNING: 未 export UMM_NDS_RPC_SOCKET——若该 NDS 库采用 "
                  "RPC admin queue（双进程架构），nds_init 将 abort。"
                  "请先启动 umm_nds_rpc_server 再设置该变量")
    if not is_stub and "+" not in spec:
        print("警告: 真机模式 spec 未带 +<base_off> 窗口基址，"
              "将从盘首写起（有毁盘风险）", file=sys.stderr)

    if is_stub and os.path.exists(STUB_DISK):
        os.unlink(STUB_DISK)

    probe_batch = args.probe_batch is not None
    if args.probe or args.probe_race or probe_batch:
        # 探针模式：跳过 RPC 分配面（ummD/umms/client），纯本地数据面。
        # pool 容量缺省 1G（未显式给 --capacity/UMM_NDS_CAPACITY 时）。
        # probe/probe-race 的 arena 固定单段 32MB；probe-batch 按
        # --probe-batch-segments 分段（vaddr 散布变量隔离）。
        if ("--capacity" not in sys.argv
                and "UMM_NDS_CAPACITY" not in os.environ):
            capacity = 1 << 30
            print("[probe] 未显式指定容量，pool 容量缺省 1G")
        probe_off = parse_size(args.probe_off)
        if args.probe:
            probe_len = parse_size(args.probe_len)
            if probe_off + probe_len > capacity:
                print(f"错误: probe 窗口 [0x{probe_off:x}, "
                      f"0x{probe_off + probe_len:x}) 超出 pool 容量 "
                      f"0x{capacity:x}")
                sys.exit(2)
        pb_segments = 1
        pb_n = pb_len = 0
        if probe_batch:
            pb_n = args.probe_batch
            pb_len = parse_size(args.probe_batch_len)
            pb_segments = args.probe_batch_segments
            if pb_n < 1:
                print(f"错误: --probe-batch N 必须 >= 1: {pb_n}")
                sys.exit(2)
            if pb_segments < 1 or ARENA_SIZE % pb_segments != 0:
                print(f"错误: --probe-batch-segments 非法: {pb_segments}")
                sys.exit(2)
            if pb_len % PAGE or probe_off % PAGE:
                print(f"错误: probe-batch 的 off/len 必须按 page {PAGE} "
                      f"对齐: off=0x{probe_off:x} len=0x{pb_len:x}")
                sys.exit(2)
            if probe_off + pb_n * pb_len > capacity:
                print(f"错误: probe-batch 窗口需要 [0x{probe_off:x}, "
                      f"0x{probe_off + pb_n * pb_len:x}) 超出 pool 容量 "
                      f"0x{capacity:x}")
                sys.exit(2)
            seg_size = ARENA_SIZE // pb_segments
            pages_per_seg = (pb_n + pb_segments - 1) // pb_segments
            if pages_per_seg * pb_len > seg_size // 2:
                print(f"错误: {pb_n} iov x {pb_len}B 超出 arena "
                      f"{pb_segments} 段的写源半区容量（每段半区 "
                      f"{seg_size // 2}B，段内最多 "
                      f"{seg_size // 2 // pb_len} 页）；请减少 N 或增加"
                      "段数")
                sys.exit(2)
        dp = None
        arena = None
        try:
            dp = NdsDataPlane(
                os.path.join(os.environ["UMM_BUILD_DIR"], "libumm.so"),
                spec, capacity)
            if mem_src == "npu":
                dev_id = parse_dev_id(spec)
                arena = AclArena(ARENA_SIZE, dev_id, segments=pb_segments)
            else:
                arena = HostArena(ARENA_SIZE, segments=pb_segments)
            print(f"[probe] 本地 ssd_pool 已打开 {spec} "
                  f"（容量 {capacity // 1024 // 1024}MB）；"
                  f"{arena.SRC_NAME} {pb_segments} 段 x "
                  f"{arena.seg_size // 1024 // 1024}MB，无 RPC 分配面")
            for s in range(pb_segments):
                dp.register_dev_mem(arena.seg_addr(s), arena.seg_size)
                print(f"[probe] register_dev_mem(seg{s}="
                      f"0x{arena.seg_addr(s):x}, "
                      f"{arena.seg_size // 1024 // 1024}MB)")
            if args.probe_race:
                ok = scenario_probe_race(
                    dp, arena, base_off=parse_base_off(spec),
                    sync_semantics=is_stub)
            elif probe_batch:
                ok = scenario_probe_batch(
                    dp, arena, pb_n, pb_len, probe_off, capacity,
                    args.probe_batch_shuffled,
                    base_off=parse_base_off(spec))
            else:
                ok = scenario_probe(dp, arena, probe_off, probe_len,
                                    args.probe_no_write,
                                    base_off=parse_base_off(spec))
        finally:
            # 顺序保证：先销毁 ssd_pool（nds_uninit 解除注册映射），
            # 再释放 arena（aclrtFree）；与主场景 finally 一致
            if dp:
                dp.close()
            if arena:
                arena.close()
            if is_stub and os.path.exists(STUB_DISK):
                os.unlink(STUB_DISK)
        sys.exit(0 if ok else 1)

    # 双进程部署：真机模式下 umms 只做分配簿记，改用 nds-meta 纯分配
    # 后端（不 dlopen/不 nds_init/不占 RPC 连接），demo 进程本地
    # ssd_pool 是唯一 NDS 客户端——避免 umms 与 demo 双进程 nds_init
    # 同一设备（RPC server 单客户端串行/qp_id 冲突 → 卡死）。
    # 桩库模式保持现状（umms 仍开 nds 桩后端，保留该路径覆盖）。
    umms_spec = spec
    if not is_stub and spec.startswith("nds:"):
        umms_spec = "nds-meta:" + spec[len("nds:"):]
        print("[setup] umms 使用 nds-meta 纯分配后端"
              "（demo 进程为唯一 NDS 客户端）")

    cfg_path = write_umms_config(umms_spec, capacity)
    procs = start_servers(umm_root, cfg_path)
    dp = None
    arena = None
    try:
        from bmpclient.client import UMMServiceClient
        client = UMMServiceClient(
            meta_addr=f"127.0.0.1:{META_PORT}",
            mem_addr=f"127.0.0.1:{MEM_PORT}",
            node_id=0,
            tier_aware=True,
        )
        print("[client] 已连接（tier_aware：RPC 分配面）")

        client.enable_ssd(spec, capacity)
        print("[client] SSD tier 已注册（ummD 拓扑；NDS 数据面在本地 "
              "ssd_pool，不走 client.write_chunk，见文件头说明）")

        scenario_topology(client, spec, capacity)

        # 位置编码运行标识（D1/D2 pattern 共用；打印便于与盘上残留对照）
        run_tag = int(time.time()) & 0xFFFF
        print(f"[setup] 位置编码 tag=0x{run_tag:04x}（本次运行标识）")

        # 数据面：demo 进程本地 ssd_pool（NDS 后端）+ I/O arena
        # （mem-src=host：ctypes buffer 模拟 HBM；mem-src=npu：aclrt
        # 申请的真实 HBM，devPtr 直接注册进 ssd_pool）
        dp = NdsDataPlane(
            os.path.join(os.environ["UMM_BUILD_DIR"], "libumm.so"),
            spec, capacity)
        if mem_src == "npu":
            dev_id = parse_dev_id(spec)
            arena = AclArena(ARENA_SIZE, dev_id, segments=segments)
            print(f"[dataplane] 本地 ssd_pool 已打开 {spec}；"
                  f"NPU HBM arena {segments} 段 x "
                  f"{arena.seg_size // 1024 // 1024}MB "
                  f"（aclrtMalloc policy={arena.alloc_policy}, "
                  f"device {dev_id}）")
        else:
            arena = HostArena(ARENA_SIZE, segments=segments)
            print(f"[dataplane] 本地 ssd_pool 已打开 {spec}；"
                  f"HOST arena {segments} 段 x "
                  f"{arena.seg_size // 1024 // 1024}MB "
                  f"（ctypes buffer 模拟 NPU HBM）")

        scenario_nds_loopback(client, dp, arena, run_tag)
        scenario_batch_io(client, dp, arena, capacity, run_tag)

        client.close()
    finally:
        # 顺序保证：先销毁 ssd_pool（pool_destroy → nds_uninit 解除注册
        # 映射），再释放 arena（aclrtFree）——NPU 模式下 devPtr 必须
        # 存活至 pool_destroy 之后，否则 aclrtFree rc=107000。
        # 各自 try 包裹：异常路径下 dp.close 出错也不能跳过 arena.close。
        if dp:
            try:
                dp.close()
            except Exception as exc:
                print(f"警告: dp.close() 异常（忽略）: {exc}",
                      file=sys.stderr)
        if arena:
            try:
                arena.close()
            except Exception as exc:
                print(f"警告: arena.close() 异常（忽略）: {exc}",
                      file=sys.stderr)
        stop_servers(*procs)
        os.unlink(cfg_path)
        if is_stub and os.path.exists(STUB_DISK):
            os.unlink(STUB_DISK)

    print("\n*** 场景 D-NDS 全部 PASS ***")


if __name__ == "__main__":
    main()
