# bmpclient — UMM 客户端

> **首次部署请读 → [docs/00_端到端搭建指南.md](docs/00_端到端搭建指南.md)**
> 架构文档索引：docs/01（本层）· 02（umm层）· 03（管控面）· 04（数据面）· R6（并发遗留问题）
>
> 无加速卡单盘测试：[直接 I/O 后端与 trace 回放指南](docs/09_单盘直接IO与trace回放.md)
> · [真实排布路径、在线回放与实施计划](docs/10_真实排布路径回放与实施计划.md)
> · [八盘哈希分盘与窗口配置](docs/11_八盘哈希回放.md)

`bmpclient` 是 UMM 的 Python 客户端库，通过 `ctypes` 加载 `libumm.so`，将 UMM 的 C API 封装为 Pythonic 接口，提供从大块 Chunk 申请到细粒度 `Block` 分配、再到跨 SSD 设备定长存储的内存管理能力。

## 架构概述

```
┌─────────────────────────────────────────────────────────────┐
│                         bmpclient                            │
│  ┌──────────────┐  ┌──────────────────┐  ┌──────────────┐ │
│  │   UMMService │  │ FineGrained      │  │  VirtualMedia│ │
│  │   Client     │──│ Allocator        │  │(跨 SSD 条带) │ │
│  │ (ctypes 封装)│  │ (Chunk 内细粒度  │  │(定长存储)    │ │
│  └──────────────┘  │  分配器)         │  │ + 可插拔策略 │ │
│                    └──────────────────┘                    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼ ctypes
┌─────────────────────────────────────────────────────────────┐
│                         libumm.so                            │
└─────────────────────────────────────────────────────────────┘
```

- **UMMServiceClient**：封装对 UMM C API 的调用，管理 Chunk 生命周期
- **FineGrainedAllocator**：向 UMM 申请大块 Chunk，在本地用空闲链表 + 首次适应算法切分为小块 `Block`
- **VirtualMedia**：跨多块 SSD 设备提供定长数据存储，支持可插拔数据打散策略

## 目录结构

```
bmpclient/
├── __init__.py                  # 包入口，导出公共 API
├── client.py                    # UMMServiceClient
├── allocator.py                 # FineGrainedAllocator + ChunkBuffer + Block
├── virtual_media.py             # VirtualMedia（稀疏 KV 专用介质层）
├── _vm_segment.py               # 段（super page）分配器与聚合写缓冲
├── virtual_media_strategy.py    # 语义键打散策略抽象（position_hash 内建）
├── virtual_media_config.py      # VirtualMedia 配置加载
├── config/
│   └── virtual_media.json       # VirtualMedia 默认配置
├── sparse_kv/                   # 稀疏 KV 框架对接层
│   ├── __init__.py
│   ├── store.py                 # SparseKVStore + PlanView
│   ├── slot_table.py            # (layer, token) -> 偏移 packed entry
│   └── vllm_adapter.py          # KVBlockRef
├── scripts/                     # 手动验证脚本
│   ├── _common.py               # 服务状态检查辅助
│   ├── demo_allocator.py        # 分配器功能演示
│   ├── demo_sparse_kv.py        # 稀疏 KV 卸载/地址规划演示
│   └── run_all_demos.sh         # 一键运行所有演示
└── tests/                       # 单元测试
    ├── test_allocator.py        # 分配器测试
    ├── test_virtual_media.py    # VirtualMedia 介质层测试
    └── test_sparse_kv.py        # SparseKVStore 对接层测试
```

## 依赖

- Python 3
- UMM 已编译并生成 `UMM/build/libumm.so`
- UMM 服务已启动（RPC Mode 下需要 `umm-metadata-service` + `umm-memory-server`）

## 核心模块说明

### 1. UMMServiceClient

封装对 `libumm.so` 的 ctypes 调用，支持 Chunk 分配、释放、读写、拓扑查询等：

| 方法 | 说明 |
|------|------|
| `create_chunk(size, media_type, device_idx=None)` | 分配 Chunk |
| `delete_chunk(desc)` | 释放 Chunk |
| `read_chunk(desc, offset, size)` | 从 Chunk 读取 |
| `write_chunk(desc, offset, data)` | 向 Chunk 写入 |
| `get_topology()` | 查询存储拓扑 |
| `get_device_list()` | 获取设备列表 |

### 2. FineGrainedAllocator

细粒度分配器，对 DRAM 和 SSD 两种介质独立管理：

- **空闲链表 + 首次适应**：在现有 Chunk 中查找可容纳的空闲块
- **自动扩展**：现有 Chunk 不足时自动向 UMM 申请新 Chunk
- **Chunk 回收**：Block 完全释放后自动合并相邻空闲块，Chunk 完全空闲时归还给 UMM
- **批量预分配**：`preallocate_chunks()` 一次性申请多个 Chunk，减少 UMM 往返
- **线程安全**：内部使用 `threading.Lock`
- **指定设备分配**：SSD 介质支持 `device_idx` 参数，落盘到指定 SSD 设备

```python
from bmpclient import FineGrainedAllocator

allocator = FineGrainedAllocator(
    "127.0.0.1:20001",   # metadata service 地址
    "127.0.0.1:20002",   # memory service 地址
)
block = allocator.alloc(1024, media_type="dram")
print(block.chunk_id, block.offset, block.size)
allocator.free(block)
allocator.close()
```

### 3. VirtualMedia + SparseKVStore（稀疏 KV cache 专用）

合并重构后，`VirtualMedia` 只服务稀疏注意力 KV cache 卸载/加载场景，
按职责拆为两层：

- **VirtualMedia（介质层）**：盘感知 + 可插拔语义键打散策略
  （默认 `position_hash`）+ 段（super page）聚合写路径。
- **SparseKVStore（框架对接层）**：vllm `KVBlockRef` 逐 token 展开、
  `slot_table` 元数据、`plan()` 地址映射输出。

关键架构决策：

- 卸载（写）由 CPU 执行，**不入图**；
- 加载（读）不经过本项目，只由 `SparseKVStore.plan()` 产出固定地址的
  **descriptor buffer**（`(ssd_id, lba_offset, length, dst_offset)`），
  供 GPU 直通存储算子在图内直接加载。

#### 数据打散策略

`VirtualMedia` 的策略从 "索引 -> 盘号" 升级为 "语义键 -> 盘号"。
稀疏 KV 场景的键为 `(layer_id, token_idx)`，内建策略为确定性位置哈希：

```
device = ((token_idx * STEP_IDX + layer_id * STEP_LAYER) % PRIME) % N_SSD
```

配置示例 `bmpclient/config/virtual_media.json`：

```json
{
  "strategy": "position_hash",
  "step_idx": 17,
  "step_layer": 23,
  "prime": 2229299,
  "super_page_bytes": 2097152,
  "max_topk": 512
}
```

#### 使用示例

```python
from bmpclient.sparse_kv import KVBlockRef, SparseKVStore
from bmpclient.testing import FakeUMMLib
from bmpclient.virtual_media import VirtualMedia

lib = FakeUMMLib(num_ssd_devices=4)
vm = VirtualMedia(
    lib,
    unit_size=4096,
    capacity_per_device=64 * 1024 * 1024,
    sp_bytes=16 * 4096,
)
store = SparseKVStore(vm, num_layers=4, max_tokens=256, max_topk=64)

# 构造 vllm block：layer 0, token [0,16), 每 token 4KB
buf = bytearray(16 * 4096)
block = KVBlockRef(layer_id=0, block_idx=0, token_start=0,
                   token_count=16, buffer=buf)
store.offload([block])
store.flush()

# decode 阶段：对 layer 0 的 topk token 做地址规划
plan = store.plan(0, [0, 4, 7, 12])
print(f"descriptor buffer 地址: 0x{plan.address:x}")
for i in range(plan.count):
    ssd_id, flags, lba_off, length, dst_off = plan.entry(i)
    print(f"entry[{i}]: ssd={ssd_id} flags={flags} lba={lba_off} len={length}")

# CPU 兜底读（无 GPU 直通硬件的验证/降级场景）
out = [memoryview(bytearray(4096)) for _ in range(4)]
store.fetch(0, [0, 4, 7, 12], out)

store.close()
```

## 运行测试

前置条件：UMM 服务不需要手动启动，测试会自动在后台启动服务。

```bash
cd bmpclient

# 运行分配器测试
PYTHONPATH=.. python3 tests/test_allocator.py

# 运行虚拟介质 / 稀疏 KV 测试（均使用 FakeUMMLib，无需真实服务）
PYTHONPATH=.. python3 tests/test_virtual_media.py
PYTHONPATH=.. python3 tests/test_sparse_kv.py
```

测试覆盖：
- 单 Chunk 内多次细粒度分配与释放
- 跨 Chunk 自动扩容、Chunk 回收与相邻空闲块合并
- DRAM / SSD 隔离与容量耗尽、批量预分配、多线程并发 alloc/free
- Block 级 read/write（含偏移、越界检查、SSD 介质）
- VirtualMedia 位置哈希性质、段聚合写 IO 连续性、段回收复用、异构段大小
- VirtualMedia `is_buffered` / `extent_base` / `read_batch` 正确性
- SparseKVStore `offload/flush/release/fetch` 数据一致性
- `plan()` 产出的 descriptor buffer 地址映射、HOST_READY 兜底、`max_topk` 语义

## 手动验证

### 一键运行（自动启停 UMM 服务）

```bash
cd bmpclient
./scripts/run_all_demos.sh
```

### 手动启动服务后单独运行演示

```bash
# 1. 启动 UMM 服务
cd UMM
./scripts/start_services.sh

# 2. 运行演示
cd ../bmpclient
python3 scripts/demo_allocator.py
python3 scripts/demo_sparse_kv.py

# 3. 停止服务
cd ../UMM
./scripts/stop_services.sh
```

如果服务未启动就运行演示脚本，会自动报错并提示启动方式。

## 与 UMM 的关系

`bmpclient` 依赖 UMM 的 `libumm.so` 与服务端：

- UMM 负责管理底层物理设备（CXL / SSD）的空间配额
- `bmpclient` 负责在申请到的 Chunk 内部做细粒度的逻辑划分
- `bmpclient` 不直接暴露底层设备信息给用户，通过 `Block` 和 `VirtualMedia` 提供访问接口
