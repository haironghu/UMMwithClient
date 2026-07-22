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
