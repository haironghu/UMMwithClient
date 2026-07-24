# UMM -- 统一内存管理系统

支持多层级存储管理（CXL 共享内存 + SSD 用户态 NVMe 驱动），通过统一的 GPA 地址编码实现跨节点、跨介质的内存分配与访问。

---

## 文档导航（docs/）

| 文档 | 内容 |
|---|---|
| [02_架构总览_umm层.md](docs/02_架构总览_umm层.md) | GPA 编址、三种初始化模式、模块清单、设计决策 |
| [03_架构总览_管控面.md](docs/03_架构总览_管控面.md) | ummD/umms、RPC 协议全 op 表、配置键全表 |
| [04_架构总览_数据面.md](docs/04_架构总览_数据面.md) | I/O 全旅程、SSD 双后端、锁纪律、性能档案 |
| [R6_单队列并发安全性遗留问题.md](docs/R6_单队列并发安全性遗留问题.md) | libnvm 并发天花板根因 + 单 ctx 多队列路线 |
| [真机风险分析与测试流程.md](docs/真机风险分析与测试流程.md) | R1-R14 风险清单 + 五阶段真机验证流程 |

> 端到端搭建指南与 Python 客户端架构见配套仓库 bmpclient 的 `docs/`。

---

## 架构

```
+------------------------------------------------------------------+
|                          UMM 架构                                 |
+------------------------------------------------------------------+
|                                                                   |
|  +----------+          +----------+          +----------+       |
|  |  Client  |<-- RPC ->|   ummd   |<-- RPC ->|   umms   |       |
|  |  (lib)   |          |(元数据)  |          |(内存服务)|       |
|  +----+-----+          +----------+          +----+-----+       |
|       |                                            |              |
|       |  +--------------------------------------+  |              |
|       |  |   Tier Router (gpa_to_tier(gpa))     |  |              |
|       |  |   +-- CXL: transport_local -> memcpy |  |              |
|       |  |   +-- SSD: transport_ssd -> ssd_pool |  |              |
|       |  +--------------------------------------+  |              |
|       |                                            |              |
|       +----------- GPA 地址空间 --------------------+              |
|                   [node:6][tier:2][offset:56]                     |
|                                                                   |
|  核心设计：                                                       |
|  - ummd 只存元数据（名字 -> GPA 映射）                            |
|  - umms 管理分配（每个 tier 独立的 bitmap 分配器）                |
|  - umm_read/write 根据 GPA tier 自动路由到对应设备                |
|  - CXL 数据路径：transport_local -> mmap -> memcpy                |
|  - SSD 数据路径：transport_ssd -> ssd_pool -> 设备文件            |
+------------------------------------------------------------------+
```

### GPA 编码

```
+--------------+----------+----------------------------------------------+
|  node_id:6   | tier_id:2|            offset:56                         |
|  [63:58]     | [57:56]  |            [55:0]                            |
+--------------+----------+----------------------------------------------+
|  0..63       | 0..3     |  0..0x00FFFFFFFFFFFFFF (每个 tier 64 PB)     |
+--------------+----------+----------------------------------------------+

gpa_to_node(gpa)   = gpa >> 58
gpa_to_tier(gpa)   = (gpa >> 56) & 0x03
gpa_to_offset(gpa) = gpa & 0x00FFFFFFFFFFFFFF
make_gpa(node, tier, offset) = ((gpa_t)node << 58) | ((gpa_t)tier << 56) | offset
```

---

## 快速开始

### 1. 编译

```bash
cd /path/to/um
make all           # 编译全部（服务端、客户端库、测试）
```

编译目标：
- `make server` -- 编译 ummd + umms
- `make client` -- 编译 libumm.a 静态库
- `make test`   -- 编译并运行全部 65 个测试
- `make tools`  -- 编译 ssd_sim_test
- `make clean`  -- 清理全部构建产物

### 2. 运行全部测试

```bash
# 容器环境（使用 ld-linux 直接加载）：
make test

# 普通环境：
for t in bin/test_*; do ./$t; done
```

期望输出：每个测试套件显示 `Results: X run, X passed, 0 failed`。

### 3. SSD 模拟测试

不启动服务端，直接测试 SSD 块设备 I/O：

```bash
# 128MB 模拟 SSD，20 个测试 chunk
/lib64/ld-linux-x86-64.so.2 ./bin/ssd_sim_test -s 128 -n 20

# 自定义设备文件和大小
/lib64/ld-linux-x86-64.so.2 ./bin/ssd_sim_test -f /data/my_ssd.raw -s 1024 -n 100
```

---

## 配置

两个服务端均使用 YAML 配置文件。仅需 **2 个配置文件**：

### 元数据服务 (`config/ummd.yaml`)

```yaml
listen_addr: "0.0.0.0"
listen_port: 20001
```

### 内存服务 (`config/umms.yaml`)

```yaml
node_id: 0
listen_addr: "0.0.0.0"
listen_port: 20002
memory_size: 67108864
base_gpa: 0

# 多 SSD 设备列表（格式：path:size,path:size,...）
# 大小后缀：G(GB), M(MB), K(KB)
ssd_devices: "/data/ssd0.raw:250G,/data/ssd1.raw:250G"
```

### 启动服务

```bash
# 终端 1 -- 元数据服务
./bin/ummd -c config/ummd.yaml

# 终端 2 -- 内存服务（支持多 SSD 设备）
./bin/umms -c config/umms.yaml
```

命令行模式仍受支持（可选）：

```bash
./bin/ummd -p 20001 -b 0.0.0.0
./bin/umms -p 20002 -n 0 -s 64M -d /tmp/umm_ssd
```

---

## 统一读写接口

`umm_read()` 和 `umm_write()` 根据 GPA 的 tier 字段自动路由到对应的存储设备：

```c
#include "umm.h"

// 初始化（单节点测试使用 direct 模式）
UMMConfig cfg = {0};
cfg.memory_size = 64ULL * 1024 * 1024;  // 64MB
umm_init(&cfg);

// 注册 SSD 存储（可选，用于 SSD tier）
umm_register_storage_tier(UMM_TIER_SSD, "/tmp/umm_ssd", 64ULL*1024*1024);

// 在 CXL tier 分配（默认）
ChunkDescriptor cx1;
umm_alloc(4096, &cx1);
umm_write(&cx1, 0, 13, "Hello CXL!");

// 在 SSD tier 分配
ChunkDescriptor ssd;
umm_alloc_tiered(4096, UMM_TIER_SSD, &ssd);
umm_write(&ssd, 0, 13, "Hello SSD!");

// 读回 -- 路由自动完成
char buf[64];
umm_read(&cx1, 0, 13, buf);  // -> transport_local -> CXL 内存
umm_read(&ssd, 0, 13, buf);  // -> transport_ssd -> SSD 设备文件
```

### 数据路径流程

```
umm_write(&desc, offset, len, buf)
  -> desc.base_gpa + offset = GPA
  -> tier_router: gpa_to_tier(gpa)
     -> tier=CXL (1): transport_local -> map_device -> memcpy
     -> tier=SSD (2): transport_ssd  -> map_device -> memcpy -> msync
```

---

## 自动化测试套件

| 测试套件 | 用例数 | 测试内容 |
|-----------|-------|---------|
| `test_allocator` | 6 | Bitmap 分配器（分配/释放/连续/大页） |
| `test_transport_mock` | 10 | 本地传输（get/put/原子操作/fence/barrier） |
| `test_metadata` | 9 | 元数据服务（注册/查询/引用计数/注销） |
| `test_integration` | 8 | 完整客户端 API（初始化/分配/写/读/释放） |
| `test_cross_node` | 3 | 跨节点的 GPA 编码正确性 |
| `test_cxl` | 6 | CXL 传输及设备配置 |
| `test_tier` | 8 | 多层级：ssd_pool、transport_ssd、tier_router |
| `test_ssd_dist` | 2 | Tier-aware 分配 + 路由分发 |
| `test_ssd_simple_dist` | 2 | 持久性测试：umms 重启后数据不丢 |
| `test_ssd_multi_device` | 5 | 多设备 SSD 连续虚拟地址空间 |
| **`test_rw`** | **6** | **统一读写测试：CXL + SSD + 隔离 + 大传输 + 偏移** |

**总计：65 个测试，全部通过。**

### 单独运行测试

```bash
# 单个测试
/lib64/ld-linux-x86-64.so.2 ./bin/test_rw

# 只看摘要
/lib64/ld-linux-x86-64.so.2 ./bin/test_rw 2>&1 | grep -E "Testing|PASSED|FAILED|Results"
```

---

## 目录结构

```
.
+-- bin/                          # 可执行文件
|   +-- ummd                      # 元数据服务
|   +-- umms                      # 内存服务
|   +-- ssd_sim_test              # SSD 独立基准测试
|   +-- test_*                    # 测试二进制
|
+-- config/                       # YAML 配置文件
|   +-- ummd.yaml                 # 元数据服务配置
|   +-- umms.yaml                 # 内存服务配置（多设备 SSD）
|
+-- include/
|   +-- umm.h                     # 公共 API 头文件
|
+-- src/
|   +-- client_lib/               # 客户端库（libumm.a）
|   |   +-- umm_api.c             # umm_alloc/umm_write/umm_read
|   |   +-- umm_transport.c       # 传输层初始化
|   |
|   +-- transport/                # 传输层
|   |   +-- transport.h           # MemoryTransportVtbl 接口
|   |   +-- transport_local.c     # 本地传输（合并 mock+cxl）
|   |   +-- transport_ssd.c       # SSD 传输 vtable
|   |   +-- transport_ssd.h       # SSD 传输头文件
|   |   +-- ssd_pool.c            # SSD 多设备池（合并 ssd_backend）
|   |   +-- ssd_pool.h            # SSD 池头文件
|   |   +-- tier_router.c         # GPA -> tier 路由
|   |   +-- tier_router.h         # 路由头文件
|   |
|   +-- memory_service/           # 内存分配服务（umms）
|   |   +-- mem_service_direct.c  # 每个 tier 独立的 bitmap 分配器
|   |   +-- mem_service_rpc_*.c   # RPC 服务端/客户端
|   |
|   +-- metadata_service/         # Chunk 目录服务（ummd）
|   |   +-- meta_service_direct.c # 哈希表实现
|   |   +-- meta_service_rpc_*.c  # RPC 服务端/客户端
|   |
|   +-- protocol/                 # 网络协议
|   |   +-- mem_protocol.c        # 内存操作码 + 打包/解包
|   |   +-- meta_protocol.c       # 元数据操作码 + 打包/解包
|   |
|   +-- common/                   # 共享工具
|   |   +-- bitmap_allocator.c    # 页级 bitmap 分配器
|   |   +-- config_parser.c       # YAML 配置解析器
|   |
|   +-- server/                   # 服务端入口
|       +-- meta_server.c
|       +-- mem_server.c
|
+-- test/                         # 测试套件
|   +-- test_rw.c                 # 统一读写测试
|   +-- test_tier.c               # 多层级存储测试
|   +-- test_ssd_*.c              # SSD 相关测试
|   +-- test_*.c                  # 其他测试套件
|   +-- test_framework.h          # 最小化测试框架
|
+-- scripts/
|   +-- test_ssd_dist.sh          # 自动化多终端测试脚本
|
+-- Makefile
+-- README.md                     # 本文件
```

---

## 核心概念

### SSD vs CXL 数据路径对比

| | CXL | SSD |
|--|-----|-----|
| **分配方式** | `umm_alloc()` -> CXL bitmap | `umm_alloc_tiered(SSD)` -> SSD bitmap |
| **GPA** | `[node][CXL][offset]` | `[node][SSD][offset]` |
| **传输层** | `transport_local` -> memcpy | `transport_ssd` -> ssd_pool -> memcpy |
| **物理介质** | CXL 设备（或 malloc 回退） | `*.raw` 设备文件 |
| **持久性** | 易失（类 DRAM） | 持久（msync -> 磁盘） |
| **多设备** | 每节点单设备 | 每池多设备 |

### Chunk 命名规则

Chunk 自动命名为：`chunk_<节点>_<偏移量>_tier<tier_id>`

示例：`chunk_0_0_tier2` = 节点 0、偏移 0、SSD tier。

### 多设备 SSD

内存服务支持将多个 SSD 设备作为单一连续虚拟地址空间：

```yaml
ssd_devices: "/data/ssd0.raw:250G,/data/ssd1.raw:250G,/data/ssd2.raw:500G"
```

- 总虚拟容量：1TB（250+250+500）
- Bitmap 分配器管理合并后的空间
- `ssd_pool_translate()` 将虚拟偏移映射到（设备, 物理偏移）

### NDS（NPU2SSD 直驱）后端

除常规文件与 `libnvm:` 后端外，SSD 池还支持 NDS 直驱后端（dlopen 软依赖
`libnds_aiv.so`，数据通路为 NPU device 内存 ↔ SSD，UMM 核心保持纯 C，
经 dlsym Itanium mangled 符号调用 C++ 单例）：

生产部署（双进程形态，推荐）：

```yaml
# umms（CPU 侧分配/元数据服务）：纯分配簿记，不 nds_init
ssd_devices: "nds-meta:0+0x40000000:16G"
# worker 进程（sglang/demo，唯一 NDS 客户端）：spec "nds:0+0x40000000"
# ——同窗口同容量，本地 ssd_pool 直驱 DMA
```

**为何 umms 要用 `nds-meta:`**：真机 RPC server 单客户端串行（qp_id
全局唯一），若 umms 与 worker 双进程都用 `nds:` 后端 nds_init 同一
设备，第二个客户端会卡死在 CREATE_CQ（真机实证）。`nds-meta:` 是纯
分配后端：不 dlopen、不 `nds_init`、不占 RPC 连接、不 mmap，只做
bitmap 分配簿记（spec 语义/容量/页对齐截断与 `nds:` 完全一致，
`get_ptr` 返回 NULL，`pread/pwrite/register_dev_mem/batch_*` 一律
`UMM_E_UNSUPPORTED`）。umms 用 `nds-meta:` 时**无需**
`UMM_NDS_PATH`/`UMM_NDS_PRELOAD`/`UMM_NDS_RPC_SOCKET`（这些全在
worker 侧设置）。单机自测/桩库形态 umms 仍可用 `nds:`（进程内
refcount 复用单例，无此问题）：

```yaml
ssd_devices: "nds:0+0x40000000:16G"   # nds:<device_id>[+<base_off>]
```

**客户端库行为（单次 nds_init 纪律）**：客户端库（libumm.so）在按
拓扑/`enable_ssd` 构建本地数据面时，遇到 `nds:` 设备**不建本地
backend**——全进程仅 worker 直连池 API（`ssd_pool_*`）持有设备，
避免 `client = UMMServiceClient(tier_aware)` 与 worker 数据面两次
`nds_init` 同一设备（真机提供方库不支持 uninit 后重 init，第二次
会卡在 qp 分配）。跳过时打一行 info 日志；对该 tier 的
`write_chunk`/`read_chunk` 返回 `UMM_E_UNSUPPORTED`（host buffer
语义对 NDS 本就错误），RPC 分配面（`create_chunk`）与拓扑可见性
（`get_device_list`）不受影响。`nds-meta:`/`libnvm:`/文件设备行为
不变。

- **路径规范**：`nds:<device_id>[+<base_off>]`（纯分配变体
  `nds-meta:<device_id>[+<base_off>]`）；`+<base_off>` 窗口语义与
  libnvm 相同（落盘偏移 = base_off + 窗口内偏移，避开 LBA0）。NDS 无容量
  查询接口，容量必须显式。
- **环境变量**：`UMM_NDS_PATH`（库路径，缺省 `libnds_aiv.so`）、
  `UMM_NDS_QUEUE_DEPTH`(64)、`UMM_NDS_CORE_NUM`(1，库当前单 qp；支持多 qp 后调大)、
  `UMM_NDS_PAGE_SIZE`(4096)、`UMM_NDS_MAX_PAGE_NUM`(7340032)、
  `UMM_NDS_MAX_IO`(1048576)；RPC 引导/托管相关：
  `UMM_NDS_RPC_SOCKET`（客户端 RPC socket，设置即启用引导）、
  `UMM_NDS_RPC_WAIT_MS`(3000，bind 重试预算，0=不重试)、
  `UMM_NDS_RPC_READY_TIMEOUT_MS`(10000，umms 托管 wait_ready 超时)、
  `UMM_NDS_RPC_SERVER_BIN`（RPC server helper 路径覆盖，缺省
  /proc/self/exe 同目录 `umm_nds_rpc_server`）。
  注意 `UMM_NDS_MAX_PAGE_NUM` 是 NDS
  内部 IO 跟踪资源池规模（对齐 NDS 提供方测试代码的
  `1024*1024*7`，支撑 ~10000 iov 在途），**不是**单次 IO 上限；
  单次上限由 `UMM_NDS_MAX_IO` 独立控制（缺省 1MB，保守可调——
  提供方样例 per-iov 为 8192，真实上限待库方文档确认）。
  另：`UMM_NDS_PRELOAD`（冒号分隔的 .so 完整路径列表）用于预载
  NDS 库的未声明依赖提供方库（RTLD_NOW|RTLD_GLOBAL，任一失败
  fail-fast 返回 `UMM_E_NOT_FOUND`，预载句柄常驻不 dlclose）。
  真实案例：`libnds_aiv.so` 引用 `libread-write_kernel.so` 的
  `readwrite_demo` 但 DT_NEEDED 未声明它，`dlopen(RTLD_NOW)` 报
  `undefined symbol: _Z14readwrite_demojPvPhS0_mjjmS0_`——此问题
  LD_LIBRARY_PATH 无法解决，配置示例：
  `export UMM_NDS_PRELOAD=/path/to/libread-write_kernel.so`。
  未预载时后端对 "undefined symbol" 失败自动回退 RTLD_LAZY 懒加载
  （打 WARN），仅作应急兜底：运行期真实调用到未定义符号会在调用点
  崩溃。定位：`ldd -r <lib>` 查全部未定义符号，`nm -D --defined-only`
  在提供方库目录定位符号后预载。
- **双进程架构 / RPC context 引导**：真实 `libnds_aiv.so` 内部复用
  nvm_host 的 admin queue。提供方为双进程架构——Process A（RPC
  server，持有 NVMe 控制器）与 NDS 客户端进程分离，客户端必须先建立
  RPC context 再 `nds_init`，否则 `nds_init` 内
  `nvm_host_admin_cq_create` 报 `no RPC context` 失败。设置
  `UMM_NDS_RPC_SOCKET`（如 `/tmp/nvm_host_rpc.sock`）后，后端在首个
  open 的 `nds_init` 之前自动执行 `nvm_host_bind_remote` +
  `nvm_host_set_rpc_context`（符号解析先 `libnds_aiv.so` 句柄再
  `RTLD_DEFAULT`，覆盖 libnvm_host.so 经 `UMM_NDS_PRELOAD` 预载的
  场景）；同 device 后续 open 复用 context 不重复 bind。bind 失败
  → `UMM_E_IO`（提示先启动 Process A）；符号缺失 → `UMM_E_NOT_FOUND`
  （提示把 libnvm_host.so 加入 `UMM_NDS_PRELOAD`）。不设该 env 则
  完全保持本地 admin queue 旧行为。
  **RPC 连接生命周期**：每条 RPC 连接随 device 注册表 entry——首个
  open bind 建连，同 device 多 backend 共享 entry 期间不断开，最后
  一个 close（refcount 归零）先 `nds_uninit` 停队列、再调
  `nvm_host_rpc_disconnect(rpc_ctx)` 释放 server 侧连接（info 日志
  一行），然后才 dlclose 主库（disconnect 符号可能解析自 NDS 主库
  句柄，顺序不能反）。`nvm_host_rpc_disconnect` 为可选符号（同 bind
  的查找顺序解析；老版本库未导出则记 NULL + debug 日志，close 跳过
  断开，不视为错误）。此前 close 从不断开，反复 open/close 会把真实
  RPC server 的僵尸连接占满导致后续 bind 阻塞（真机实证）。真机完整配置：

  ```bash
  # 1. 先启动 Process A（本仓库配套守护进程 umm_nds_rpc_server，
  #    本地持有 NVMe 控制器 + 对外提供 RPC admin queue 服务）：
  make tools   # 产出 bin/umm_nds_rpc_server（仅依赖 -ldl -pthread）
  UMM_LIBNVM_PATH=/path/libnvm_host.so \
    ./bin/umm_nds_rpc_server --ctrl /dev/libnvm_helper0 --ns 1 --qd 64 \
    --socket /tmp/nvm_host_rpc.sock &
  #    启动成功打印 "RPC server ready on <socket> ..."；真机残留
  #    stale socket 会自动 unlink（warn 一行）；SIGINT/SIGTERM 优雅
  #    退出（free + 清理 socket，退出码 0）。参数均有 env 等价物
  #    （UMM_NVM_CTRL/UMM_NVM_NS/UMM_NVM_QD/UMM_NVM_RPC_SOCKET）。
  # 2. UMM 侧（Process B，NDS 客户端）：
  export UMM_NDS_PRELOAD=/path/libread-write_kernel.so:/path/libnvm_host.so
  export UMM_NDS_PATH=/path/libnds_aiv.so
  export UMM_NDS_RPC_SOCKET=/tmp/nvm_host_rpc.sock
  ./bin/umms -c config/umms_ssd_nds.yaml &
  # 3. UMM 客户端照常连接 ummD/umms（三进程部署形态：
  #    umm_nds_rpc_server → ummD/umms → 客户端）
  ```

- **运维加固 Phase 1 — 客户端 bind 重试**：`nvm_host_bind_remote`
  失败（多为 Process A 启动窗口期的暂态失败）时在预算内指数退避
  重试（50→100→200→400→800ms 封顶），由 env `UMM_NDS_RPC_WAIT_MS`
  控制（缺省 3000；`0` = 不重试，保持单次失败立即返回的旧行为）。
  每次重试打 debug 日志（含剩余预算），预算耗尽报 error（沿用
  "Process A 未启动" 提示 + 已等待毫秒数/尝试次数）。重试只针对
  bind 失败；符号缺失等确定性错误仍 fail-fast 不重试。提供方库
  bind 每次失败会自行打印 `Failed to connect...`，属可观测暂态
  噪声，未抑制。
- **运维加固 Phase 2 — umms 托管拉起 RPC server（推荐部署形态，
  三进程 → 单命令）**：umms 解析配置后、storage 注册前，若
  `nds_rpc_server_enable: true` 且 `ssd_devices` 含 `nds:` 设备，
  自动 fork+exec 同目录（或 `UMM_NDS_RPC_SERVER_BIN` 指定）的
  `umm_nds_rpc_server`，wait_ready（每 100ms `connect()` 探测
  unix socket，缺省超时 10000ms，env
  `UMM_NDS_RPC_READY_TIMEOUT_MS` 可调；子进程早夭/超时均 fatal
  并提示检查 `UMM_LIBNVM_PATH`/控制器权限）。配置 socket 后若
  `UMM_NDS_RPC_SOCKET` 未设置则自动 `setenv`（保证同进程
  ssd_backend_nds 引导用同一 socket）；已设置且不一致则 warn
  （以 env 为准）。`nds_rpc_server_keep_alive`（缺省 true）：
  true = umms 退出后 server 保留（日志注明 PID）；false = 注册
  atexit 钩子，umms 退出（含 SIGTERM/SIGINT 优雅退出）时 SIGTERM
  子进程。设计取舍：库层（libumm.so）绝不拉起特权进程（多进程
  加载竞态/权限/生命周期倒挂），托管只在 umms 服务层。配置示例
  见 `config/umms_ssd_nds.yaml`。
  **systemd 终态建议**：生产上更推荐 systemd 表达依赖——
  `umm-nds-rpc.service`（`umm_nds_rpc_server`）+ `umms.service`
  `After=umm-nds-rpc.service`，由 systemd 保证启动顺序与
  重启策略；umms 托管模式面向无 systemd 的容器/手工部署。
  即便有 systemd 编排，Phase 1 的 bind 重试仍建议保留
  （server 重启窗口期的天然容错）。
- **与 libnvm 的差异**：
  - I/O 缓冲是 NPU device 内存（device vaddr），不是 host buffer；
    I/O 前必须 `ssd_pool_register_dev_mem()` 注册 device 内存段，
    未注册一律拒绝（`UMM_E_INVALID_ARG`）。支持多段注册（至多
    16 段，与 NDS 提供方测试代码的连续多段 `nds_register` 用法
    一致）；单条 IO 的 vaddr 必须完整落在某一个段内（跨段拒绝）。
    注册段建议用 `aclrtMalloc(ACL_MEM_MALLOC_HUGE_ONLY)` 分配
    （提供方测试代码即用 HUGE 页）。
  - offset/len 必须 page_size 对齐；单次 I/O 上限 =
    `UMM_NDS_MAX_IO`（缺省 1MB），超限自动分段。
  - NDS 全部接口 void 返回：无法调用后感知错误，只能调用前校验。
  - 新增批量 I/O：`ssd_pool_batch_read/write`（iov.offset 为池虚拟
    偏移，每个 iov 须完整落在单设备内）。
- **构建/测试**：桩库 `test/stub_nds_aiv.cpp`（g++ -shared 产出
  `bin/libnds_aiv.so`）+ `bin/test_ssd_nds`，纳入 `make test`。

---

## 常见问题排查

### `umm_init failed: -7` (UMM_E_TRANSPORT_ERROR)
- 检查 ummd/umms 是否运行且可达：`nc -z 127.0.0.1 20001`
- 确认 YAML 配置中的 `listen_addr` 和 `listen_port` 正确

### `offset X exceeds device size Y`
- SSD `base_offset` 不匹配。确保 umms 注册 SSD 时 `base_offset=0`
- 检查 `ssd_pool.c` 调试日志中的实际设备大小

### `Chunk not found in ummd`
- 写入端必须保持 chunk 注册状态（读取端查询前不要调用 `umm_free`）
- 确保读取端连接的是**同一个** ummd 实例

### `open(device.raw) failed: No such file`
- 启动 umms 前确保 SSD 设备父目录存在
- `ssd_pool_add_device()` 自动创建文件，但父目录必须已存在

### 测试报权限错误（容器环境）
```bash
# FUSE 文件系统可能不允许直接执行
/lib64/ld-linux-x86-64.so.2 ./bin/test_rw   # 直接用动态链接器加载
```
