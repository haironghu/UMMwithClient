# UMM 多节点集群能力：多 Phase 规划与 Phase 1 设计

> 版本：v1.0（2026-08-03）
> 目标读者：参与 UMM 多节点化的工程师。
> 读完可完成：理解整体路线 → 部署 Phase 1 → 按验收清单执行两节点验证。

---

## 0. 现状评估结论（代码实证）

| 能力 | 状态 | 证据 |
|---|---|---|
| RPC 跨机连接（控制面） | ✅ 已完成 | `umm_tcp_connect` 支持任意 IP/DNS；`parse_host_port`；服务端 `-b 0.0.0.0` |
| 全局元数据目录（ummD） | ✅ 已完成 | meta 协议 op 1-10 |
| 集中式分配权威（umms 位图） | ✅ 已完成 | mem 协议 ALLOC_TIERED/FREE_TIERED |
| GPA 集群编址 | ✅ 已预留 | `gpa = [node:6][tier:2][offset:56]` |
| **节点路由（CIS）** | ❌ 空壳 | `cis_router.c` 是 placeholder，**且从未被 umm_init 调用**（死代码） |
| **GPA node 位路由** | ❌ 未接通 | `tier_router` 只按 tier 位分派；且 GPA node 位填的是**客户端自己的** `my_node_id`（`umm_api.c` `umm_alloc_tiered`），不是数据属主 |
| **跨节点数据面** | ❌ 不存在 | RPC 协议无 READ/WRITE op；SSD 数据面 = 各节点本地打开拓扑里的设备路径 |
| 鉴权/加密 | ❌ 无 | 明文 RPC，限可信内网 |

**核心语义缺陷**：当前 GPA 的 node 位标识的是"谁分配的"，而非"数据在谁那里"。
跨节点场景下节点 B 分配到 SSD chunk 后，GPA node=1（B 自己），数据实际在
节点 A 的池里——B 按 tier 位本地分派，打开自己机器上的同名设备，**静默读错盘**。

---

## 1. 多 Phase 路线总览

```
Phase 1  跨节点数据面打通（验证级）          ← 本次交付
  静态节点表 + TCP 同步数据面 RPC + CIDR 白名单/token
  验收：B 节点读写 A 节点盘字节一致；负路径全过
        │
Phase 2  性能与工程化
  I/O 流水化（多 outstanding）、独立数据面端口、
  性能基准（时延/吞吐 vs 本地基线）、ummD 侧白名单、
  混合版本集群兼容矩阵
        │
Phase 3  多节点（>2）与可靠性
  成员服务（心跳/租约/fencing）、ummD 拓扑持久化、
  多 umms 池的路由分配（cis_route_alloc_node 实做）、
  节点失联的脑裂写防护
        │
Phase 4  RDMA 数据面（评估后决策）
  前提：Phase 2 完成且网络 RTT 被证明是瓶颈；
  注意 R6 单队列瓶颈不解决，RDMA 收益会被吃掉
```

### 各 Phase 工作量与退出标准

| Phase | 主要交付 | 工作量估算 | 退出标准 |
|---|---|---|---|
| 1 | 数据面 op + 远端 transport + node 路由 + 静态节点表 + 白名单 | ~1400 行 C + ~500 行测试（**本次已交付**） | 两节点 e2e 全过（§4 清单） |
| 2 | 流水化、独立端口、基准报告 | ~800 行 + 基准 harness，3-5 天 | 远端读吞吐 ≥ 70% 本地基线；RTT 开销量化入盈亏线公式 |
| 3 | 租约/fencing、成员服务、多池路由 | ~2000 行，1-2 周 | 杀节点演练：无脑裂写、无静默错读；3 节点验证 |
| 4 | RDMA transport | 需硬件评估，2 周起 | 决策点：仅在 Phase 2 数据表明网络是主瓶颈时启动 |

---

## 2. Phase 1 设计（本次实现）

### 2.1 设计原则

1. **数据属主权威在服务端**：GPA node 位 = 数据属主节点（umms 的 node_id），
   不再填客户端自己的 id。CXL tier 维持现状（分配簿记在服务端，数据面是
   client 进程本地 malloc——node=my_node 语义本来就对）。
2. **远端路由是显式 opt-in 的增量路径**：remote transport 仅在配置了
   `peer_nodes` **或** `ssd_owner_node` 时挂载（2026-08 收紧：此前只要
   设了 mem_server_addr 就隐式挂载）。未配置集群时 node≠my_node 的 GPA
   仍走旧的 tier-local 分派，**现有单节点/NFS 部署行为零变化**；零配置
   RPC 部署即**共享盘模型**（多节点挂同一批盘、umms 只做分配、各节点
   本机 I/O，实验套件见 `lab_shared_ssd/`）——两种部署形态由配置区分，
   不再隐式猜测。
3. **协议向后兼容**：旧服务端响应 12 字节，新客户端按 body_len 自适应；
   新旧混跑时回退 `ssd_owner_node` 配置或 `my_node_id`（旧行为）。
4. **payload 走流式传输**：`UmmProtoBody` 上限 4KB（`body_len` 为 u16），
   数据帧不挤 body——header + 小 body(16B) + 原始字节流。
5. **同步 stop-and-wait**：验证级实现，每次 I/O 一个 RTT；流水化留给 Phase 2。

### 2.2 架构变更图

```
变更前（节点 B）：                          变更后（节点 B）：
umm_write(gpa[1|SSD|off])                   umm_write(gpa[0|SSD|off])  ← node=属主
  → tier_router 按 tier 分派                  → tier_router 先看 node 位：
  → transport_ssd 本地打开                      node(0)≠my(1) → transport_remote
  → /dev/libnvm_helper0（B 自己的盘！）         → DATA_WRITE RPC → umms@A
                                                → ssd_pool_pwrite → A 的盘

节点 A（my_node=0）：GPA node=0=my_node → 本地 transport_ssd，行为不变。
```

### 2.3 组件变更清单

| 组件 | 变更 | 文件 |
|---|---|---|
| 协议 | 新增 `MEM_OP_DATA_READ=10` / `MEM_OP_DATA_WRITE=11`；ALLOC_TIERED 响应追加属主 node 字节（13B，body_len 自适应）；header `reserved[6]` 承载 token digest | `mem_protocol.h/.c`、`protocol_common.c`（fnv1a digest） |
| 服务端 | DATA_READ/WRITE handler：校验（tier=SSD、len≤cap）→ `ssd_pool_pread/pwrite`；accept 时 CIDR 白名单；逐请求 token 校验 | `mem_service_rpc_server.c`、`mem_server.c/.h` |
| 远端 transport | 新 `transport_remote.c`：MemoryTransportVtbl 实现，分块（默认 1MB）、懒连接、断线重连、地址解析（cis 表 → umms 回退） | `transport_remote.c/.h`（新增） |
| 路由 | `tier_router` 增加 my_node + remote 槽位；get/put 先查 node 位；远端原子操作明确返回 `UMM_E_UNSUPPORTED` | `tier_router.c/.h` |
| CIS | `cis_router` 实做静态节点表（解析 `peer_nodes` 配置）；接入 `umm_init/deinit`（此前是死代码） | `cis_router.c`、`umm_api.c` |
| 客户端接线 | SSD alloc 响应带属主 node → GPA 用属主 node 组合；remote transport 创建与清理 | `umm_api.c`、`mem_service_rpc_client.c` |
| 配置 | 新键：`peer_nodes`、`rpc_token`、`allow_cidrs`、`ssd_owner_node`、`data_max_io` | `config_parser.c`、`include/umm.h`（UMMConfig 尾部追加） |
| Python | **修复存量 bug**：ctypes `UMMConfig` 缺 nds_rpc_server_* 尾部字段，`umm_init` 的 `memcpy(sizeof(UMMConfig))` 越界读 Python 堆 ~780B；同步追加 Phase 1 字段 | `bmpclient/umm_client.py`、`client.py` |
| 安全 | FNV-1a(64) token digest（header reserved 6B）+ CIDR 白名单（accept 拦截） | `umm_network.c/.h`、各处接线 |

### 2.4 新配置键

| 键 | 侧 | 语义 | 缺省 |
|---|---|---|---|
| `peer_nodes` | client | `"node:host:port,..."` 集群数据面对等表；与 `ssd_owner_node` 同为 remote transport 挂载门槛 | 空=不挂载（共享盘模型） |
| `rpc_token` | 双侧 | 共享密钥；服务端非空则逐请求校验 | 空=不校验 |
| `allow_cidrs` | server(umms) | accept 白名单 `"10.0.0.0/8,..."` | 空=全放行 |
| `ssd_owner_node` | client | 旧服务端（12B 响应）时的属主回退 | `0xFF`=用 my_node_id |
| `data_max_io` | 双侧 | 单 RPC payload 上限（钳位 [4KB,16MB]） | 1MB |

### 2.5 线协议（数据面 op）

```
DATA_WRITE 请求:  header(op=11, body_len=16) + body[gpa u64 | len u64] + payload[len]
DATA_WRITE 响应:  header + body[status i32]
DATA_READ  请求:  header(op=10, body_len=16) + body[gpa u64 | len u64]
DATA_READ  响应:  header + body[status i32 | len u64] + payload[len]（仅 status==OK）

约束：tier 必须 = UMM_TIER_SSD；len ≤ 服务端 data_max_io；
      len=0 或 tier 错误 → UMM_E_INVALID_ARG。
```

### 2.6 地址解析顺序（transport_remote）

```
1. cis_router_get_node_addr(owner)   ← peer_nodes 静态表
2. owner == 已知 umms node ?         ← alloc 响应学习 / ssd_owner_node 配置
   → mem_server_addr
3. 都 miss → UMM_E_NOT_FOUND + 明确日志（负路径用例 N1）
```

### 2.7 已知边界（Phase 1 明确不做）

- 远端原子操作（CAS/fetch_add）：返回 `UMM_E_UNSUPPORTED`；
- I/O 流水化：同步 1-RTT/次，性能数字预期 = 本地时延 + RTT；
- fencing/脑裂防护：ummD 无租约的既有边界在跨节点后被放大，文档钉死，
  Phase 3 解决；
- ummD（meta 端口）白名单：Phase 2；
- 数据路径不校验位图分配状态（与本地路径语义一致）：越出池容量报错，
  池内未分配偏移读到旧数据——与现状相同，不在 Phase 1 改变。

---

## 3. 两节点部署手册（Phase 1）

### 3.1 拓扑

```
VM-A（server，node 0）                VM-B（client，node 1）
├─ ummD   0.0.0.0:20001               └─ libumm + bmpclient
└─ umms   0.0.0.0:20002                      │ TCP 20001/20002
    └─ SSD: 文件后端 /data/ssd0.raw           │（或 libnvm 真盘，
       （或 libnvm:/dev/libnvm_helper0@1+0x40000000） 数据面全部走 RPC）
```

### 3.2 VM-A：启动服务

```bash
# 安全组放行 20001/20002 入方向（限 B 的内网 IP）
cd umm && make -j

# ummD（元数据）
./bin/ummd -p 20001 -b 0.0.0.0

# umms（内存+SSD 池+数据面），文件后端示例：
cat > /etc/umms_node0.yaml <<'EOF'
node_id: 0
listen_addr: "0.0.0.0"
listen_port: 20002
memory_size: 268435456
ssd_devices: "/data/ssd0.raw:8G"
rpc_token: "s3cr3t-phase1"            # 可选；配置后所有 mem 请求需带 token
allow_cidrs: "10.0.0.0/8"             # 可选；按实际内网网段改
EOF
./bin/umms -c /etc/umms_node0.yaml

# libnvm 真盘换成（延用既有约束）：
#   export UMM_LIBNVM_PATH=.../libnvm_host.so LD_LIBRARY_PATH=... UMM_LIBNVM_CTXS=1
#   ssd_devices: "libnvm:/dev/libnvm_helper0@1+0x40000000:16G"
```

### 3.3 VM-B：客户端

```python
from bmpclient.client import UMMServiceClient

c = UMMServiceClient(
    meta_addr="<A_IP>:20001",
    mem_addr="<A_IP>:20002",
    node_id=1,                      # B 自己的节点号
    peer_nodes="0:<A_IP>:20002",    # 数据面对等表：node0 在 A
    rpc_token="s3cr3t-phase1",      # 与服务端一致（若启用）
    tier_aware=True,
)

desc = c.create_chunk(32*1024*1024, media_type="ssd")   # GPA node=0（属主）
c.write_chunk(desc, 0, b"hello-remote")                  # → DATA_WRITE RPC 到 A
data = c.read_chunk(desc, 0, 12)                         # → DATA_READ RPC 到 A
assert data == b"hello-remote"
```

**B 节点不需要任何 SSD 设备、不需要 enable_ssd、不需要 libnvm**——
数据面全部经 RPC 落在 A 的池上。

### 3.4 一键验证（单机双进程模拟两节点，CI 可跑）

```bash
UMM_ROOT=$PWD/umm python3 bmpclient/scripts/demo_e2e_remote_ssd.py
# 场景 R0 远端读写一致性 / R1 双客户端分配唯一性 /
#       R2 负路径（未知 node、超限帧、错 token、远端原子）/ R3 断线重连
# 真机两 VM：--mem-addr <A_IP>:20002 --meta-addr <A_IP>:20001 --no-server
```

---

## 4. 验收清单（全部通过才算 Phase 1 完成）

| # | 用例 | 预期 |
|---|---|---|
| L0 | B `nc -z A 20001/20002` | 通 |
| L1 | B 跨机 ALLOC/FREE/STATS/HEARTBEAT | 全成功 |
| L2 | A 注册 chunk → B lookup 命中 | GPA node=0 |
| L3 | **B 写 32MB pattern → B 远端读回 / A 上直接读池文件，字节一致** | sha256 一致 |
| L4 | 双客户端并发 alloc ×100 | GPA 全局唯一无重叠 |
| L5a | 未知 node GPA（手工构造 node=7） | `UMM_E_NOT_FOUND`，明确日志 |
| L5b | 原始 socket 发超限 DATA_WRITE（>data_max_io） | 服务端拒绝，不崩 |
| L5c | 错 token 连接 | 拒绝；服务端 warn 日志 |
| L5d | 白名单外 IP 连接 | accept 即断开 |
| L5e | 远端 GPA 原子操作 | `UMM_E_UNSUPPORTED` |
| L6 | 传输中 kill umms → 重启 → B 重试 | 自动重连，后续 I/O 成功 |
| L6b | **回归**：A 本机（node 0）跑原 demo_e2e_ssd | 行为与改动前完全一致 |

## 5. 风险登记（继承约束，不随 Phase 1 消失）

1. **R6 单队列**：libnvm `UMM_LIBNVM_CTXS=1`，远端 I/O 共享服务端单 ctx 锁，
   吞吐天花板 = 现状单队列水平；远程化不解决并发，先放大时延。
2. **RTT 进关键路径**：同步 RPC 每次 I/O +1 RTT；盈亏线公式需重标（Phase 2 实测）。
3. **契约变更**："RPC 仅控制面"原则正式打破，docs/03/04 评审红线需同步更新。
4. **暴露面扩大**：数据 op 上线后端口能力 = 整盘读写；token+白名单是底线，
   仍限可信内网；生产化需 TLS/mTLS（Phase 3 评估）。
5. **脑裂写无防护**：两节点同时认为自己是属主时无 fencing——Phase 1 靠
   "静态配置只配一个 umms" 规避，Phase 3 根治。

---

## 6. Phase 2：SSD+DRAM 混合池（无 CXL 硬件）

### 6.1 目标与语义

在共享盘实验（lab_shared_ssd）之上验证混合池：umms 同时作为 **DRAM 层**
与 **SSD 层** 的唯一分配权威；SSD 层两 VM 物理共享（同一后备文件），
DRAM 层是"全局分配、私有数据面"——umms 全局发 offset，各 VM 写自己的
malloc buffer（DRAM 不可能跨机共享，这是池化 DRAM 配额语义）。

### 6.2 实现（本仓库已落地）

| 组件 | 改动 |
|---|---|
| `mem_service_direct` | 新增 `mem_service_direct_create_tiered()`：内存层可注册为 CXL（默认）或 DRAM（注册即 malloc 后备，DRAM 分支原本就在） |
| `mem_server` | 重构出 `create_impl/create_multi_impl`；新增 `mem_server_create_multi_tiered()`，旧接口签名与行为不变 |
| `umms` | yaml 新增 `memory_tier: cxl|dram`（umms 专属 key，config_parser 静默忽略未知 key，umms 自行行级扫描，不进 UMMConfig ABI） |
| `UMMConfig` | `_reserved_phase1[3]` 首字节改为 `local_mem_as_dram`——sizeof/偏移全不变（ctypes 镜像同步，已验证 7448） |
| `tier_router` | 新增 `tier_router_set_dram()`：填充 create 时留空的 DRAM 槽位 |
| `transport_local` | `local_init` 按 `local_mem_as_dram` 选择注册 DRAM（create_v2 不带 CXL mock）或 CXL（旧行为） |
| `umm_api` | `configure_transports_from_topology` 在 DRAM 模式下把本地 transport 挂进 DRAM 槽位；CXL 槽位保留同一 transport（tier=1 GPA 会因 CXL 未注册干净报错，属预期） |

### 6.3 路径核验（RPC 模式，tier=0）

- alloc：`mem_rpc_alloc_tiered2(tier=0)` → umms DRAM 位图；GPA node 位 =
  my_node_id（数据面本就本地）
- free：`tier != CXL → mem_rpc_free_tiered(tier=0)` ✓
- read/write：纯 tier_router 按 GPA tier 位分派 → DRAM 槽 → transport_local
  → 本地 mem_service DRAM tier（malloc）✓
- 已知限制：DRAM 模式下 legacy `umm_alloc()`（tier 硬编码 CXL）不可用，
  一律用 `umm_alloc_tiered`（Python 侧 `create_chunk_on_tier`）

### 6.4 实验切换

```bash
# 路线A（默认）：EXP_MEM_TIER=1（mock CXL 槽位，零代码改动）
./05_start_services.sh && ./06_run_experiment.sh
# 路线B（真 DRAM）：
MEM_TIER_KIND=dram EXP_MEM_TIER=0 ./05_start_services.sh && \
    EXP_MEM_TIER=0 ./06_run_experiment.sh
```

demo 追加场景：S1m（内存层分配+流式写读校验+交叉复查）、S4（tier 容量
隔离双向互测 + 内存层负路径三件套）；集群断言按 tier 分别做 offset
不相交校验。

## 7. Phase 3：SSD 落盘/刷盘语义（fence 持久化 + invalidate）

背景：SSD 数据面是 `open + mmap(MAP_SHARED) + memcpy`，写只进本机页缓存。
共享盘双 VM 形态下，A 的写入要能被 B 可靠读到，需要一对显式语义：
**写入端落盘（fence）+ 读取端刷盘（invalidate）**——B 的页缓存不会因
A 落盘而自动失效（跨内核无解，必须由读端主动丢弃陈旧页）。

### 7.1 语义修正与新增 API

| 改动 | 内容 |
|---|---|
| `ssd_transport` fence 修正 | 原为纯 CPU 屏障（`__sync_synchronize`），不落盘；现为 屏障 + `ssd_pool_sync(0, 全池容量)`（逐设备 `msync(MS_SYNC)`，含跨设备分段循环）。经新增的 `mem_service_direct_ssd_pool()` 访问器取 pool；libnvm/NDS 等 MAP_FAILED 回退后端天然跳过 |
| `ssd_backend_invalidate` | 新增：`msync(MS_INVALIDATE)` 丢弃区间缓存页（会丢脏页，调用方须保证先 fence） |
| `ssd_pool_invalidate` | 新增：跨设备分段循环，镜像 `ssd_pool_sync` |
| `ssd_transport_invalidate` | 新增：GPA → offset → `ssd_pool_invalidate` |
| `umm_invalidate(desc, off, len)` | 新增公共 API：越界检查；非 SSD tier 为 no-op 返回 OK；无 SSD transport 返回 UNSUPPORTED |
| bmpclient | `client.flush()`（umm_fence）与 `client.invalidate_chunk(...)` 封装 |

### 7.2 验证场景 S5（共享盘实验 06 编排）

时序：vmA writer（分配→写 pattern→`flush()` 落盘→**进程保活**）⇄
vmB preread（旧内容填充页缓存=陈旧视图）→ vmB verify（刷盘前读=负路径
观察 → `invalidate_chunk` → 再读=正路径断言 digest 逐字节一致）。
writer 保活是关键设计：fence 而非退出时 munmap 才是被验证的落盘路径。
B 侧 chunk 描述符由 GPA/chunk_id/size 手工构造（带外传递，无需参与分配）。

单机仿真 `simulate_s5_flush_invalidate.py`（B1-B4）回归时序与正路径；
负路径（陈旧读）需两个独立内核页缓存，只能在双 VM 上复现。

## 8. Phase 2.5：共享内存窗口（virtio-pmem，DRAM 层硬件一致）

Phase 3（S5）回答了"SSD 层跨页缓存如何可靠读"——需要 fence+invalidate
一对显式语义。Phase 2.5 是它的对照实验：**DRAM 层能否像 SSD 那样在两台
VM 之间共享？** 答案是可以，而且语义干净得多：把后备从"各 VM 私有
malloc"换成"宿主共享物理页"，一致性由 x86 硬件 cacheline 协议保证，
**无需 fence+invalidate**。这正是 CXL 3.x G-FAM 的软件替身。

### 8.1 设计：mem_device 旋钮 + virtio-pmem

| 层 | 默认（不变） | `MEM_TIER_BACKING=/dev/pmem0` 时 |
|---|---|---|
| 后备 | 各 VM 私有 malloc（路线B）/ mock + malloc 回退（路线A） | 宿主共享文件经 `-object memory-backend-file,share=on` 挂成两台 VM 的 `/dev/pmem0`，`open+mmap(MAP_SHARED)`（DAX 语义） |
| 一致性 | 谈不上（各自私有） | 两 vCPU = SMP 两线程，x86 硬件一致 |
| 跨 VM 读语义 | — | 直接读，无需 invalidate（对照 S5） |

关键决策：

- **复用 `UMMConfig.cxl_device` 字段传递设备路径**（config_parser/ctypes
  镜像早已就绪），ABI 不变；libumm local transport 的 DRAM 分支从
  `cfg->cxl_device` 读路径（注释明示该字段在此形态下表示内存层设备）。
- **`mem_service_direct_create_tiered` 新增 `mem_device` 参数**：
  非空时 CXL/DRAM 两个分支都用显式设备后备，空则完全保持旧行为
  （mock / malloc）。
- **DRAM 设备后备分支 fail-hard，绝不回退 malloc**：
  open 失败 `UMM_E_NOT_FOUND`、mmap 失败 `UMM_E_NO_MEMORY`。
  静默回退会把共享窗口无声退化成私有内存——实验结论直接作废，
  宁可硬失败。（CXL 分支的 lazy malloc 回退属路线A 遗产行为，保留，
  由实验编排层用日志守卫兜底。）
- `destroy` 泛化为 `mmap_fd >= 0 ? munmap+close : free`，兼容两种后备。
- umms 端：`--memory-device` / yaml `memory_device`（行级扫描，仿
  `memory_tier`），透传到 `create_multi_tiered`；05 在
  `MEM_TIER_BACKING` 非空时把 `mem_bytes` 取 `SHM_SIZE`，防 SIGBUS。

### 8.2 验证场景 S6（共享盘实验 06 编排）

时序：vmA `s6-writer`（内存层分配→写 pattern→`flush()`=纯 CPU 屏障
→保活）⇄ vmB `s6-verify`（带外 GPA 手工构造描述符，**直接读，不执行
任何 invalidate/刷盘**，digest 逐字节一致即 PASS）。
06 收尾对三份日志 grep `falling back to malloc backing`，命中即判失败
（静默退化守卫）。

与 S5 的对照关系：

| | S5（共享 SSD） | S6（共享内存窗口） |
|---|---|---|
| 共享介质 | 同一 raw 文件，两个 QEMU 各开一份 | 同一组宿主物理页（virtio-pmem） |
| 读写路径 | mmap+memcpy，隔着 guest 页缓存 | DAX mmap，直达物理页 |
| 跨 VM 一致性 | 软件保证：fence（落盘）+ invalidate（刷盘） | 硬件保证：cacheline 协议 |
| B 端读前动作 | 必须 `invalidate_chunk`，否则可能读到陈旧页 | 什么都不用做 |
| 对应硬件形态 | NVMe 共享盘（无多主机一致性） | CXL 3.x G-FAM（硬件 back-invalidate） |

路线A/B 通吃：`MEM_TIER_KIND=cxl EXP_MEM_TIER=1`（原 CXL 设备分支）或
`MEM_TIER_KIND=dram EXP_MEM_TIER=0`（新 DRAM 设备后备分支）。
单机仿真 `simulate_s6_shared_mem.py`（C1-C3：两进程 exit 0 / PASS 行
含"未执行任何 invalidate" / 无 malloc 回退无 remote）已在沙箱回归；
双 VM 实跑待实验机执行（需 `MEM_TIER_BACKING=/dev/pmem0` 重启 VM 后
重跑 03→06）。
