# bmpclient 设计文档

## 1. 概述

`bmpclient` 是 UMM（Unified Memory Management）系统的 Python 客户端库。它通过 `ctypes` 加载 UMM 的 C 动态库 `libumm.so`，将底层的 C API 封装为 Pythonic 的高层接口，提供从粗粒度 Chunk 申请到细粒度内存分配的内存管理能力。

**核心设计目标**：
- **分层抽象**：从底层 C API → Chunk 管理 → 细粒度分配，层层递进
- **多介质支持**：统一处理 CXL（含 DRAM mock）和 SSD 两种存储介质
- **多设备 SSD**：支持将多个物理 SSD 设备注册为统一的 SSD tier，并允许指定设备分配
- **稀疏 KV cache 卸载**：VirtualMedia + SparseKVStore 两层栈，为稀疏注意力
  LLM 推理提供位置哈希打散、super page 聚合写与 GPU 直通加载地址映射
  （详见 `docs/06_稀疏注意力KV卸载数据排布设计.md` 与
  `docs/07_VirtualMedia合并设计_稀疏KV专用介质.md`）
- **线程安全**：所有状态变更均受锁保护，支持多线程并发访问
- **自动资源管理**：空闲 Chunk 自动回收

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    推理框架 / GPU 直通算子（decode 图内）                       │
│                                                                             │
│   store.offload(blocks)        plan = store.plan(layer, topk_tokens)        │
│        CPU 写路径                   ↓ descriptor buffer                    │
│                               GPU 按 (ssd_id, lba_offset) 直通加载           │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        bmpclient 内部模块                                    │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────────┐
│  │ FineGrainedAllocator│  │ SparseKVStore       │  │    UMMServiceClient     │
│  │   (细粒度分配器)     │  │ (框架对接层)         │  │   (Chunk 生命周期管理)   │
│  │                     │  │ • KVBlockRef 展开    │  │                         │
│  │ • ChunkBuffer 池    │  │ • slot_table 元数据  │  │ • create_chunk()        │
│  │ • 空闲链表 + 首次适应│  │ • plan() 地址映射    │  │ • delete_chunk()        │
│  │ • 自动扩展 / 回收   │  │ • fetch() CPU 兜底   │  │ • read/write chunk      │
│  │ • 线程锁保护        │  │                     │  │ • get_topology()        │
│  └─────────────────────┘  └──────────┬──────────┘  └─────────────────────────┘
│                                      │
│                                      ▼
│                           ┌─────────────────────┐
│                           │    VirtualMedia     │
│                           │   （介质层）         │
│                           │ • 盘感知 / 拓扑      │
│                           │ • position_hash 打散 │
│                           │ • 段聚合写路径       │
│                           └──────────┬──────────┘
│                                      │
│                                      ▼
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                           UMMLib (ctypes)                            │   │
│  │  • 加载 libumm.so                                                    │   │
│  │  • C 结构体映射: UMMConfig, ChunkDescriptor, StorageTopology...     │   │
│  │  • 函数签名绑定: umm_init, umm_alloc, umm_free, umm_read...         │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼ ctypes CDLL
┌─────────────────────────────────────────────────────────────────────────────┐
│                           libumm.so (C 库)                                   │
│                                                                             │
│   ┌──────────────┐      ┌──────────────┐      ┌──────────────────────────┐ │
│   │  Direct Mode │      │   RPC Mode   │      │      Transport Layer     │ │
│   │  (本地调用)   │      │ (TCP/Unix)   │      │  (mock / cxl / tcp / ...)│ │
│   └──────────────┘      └──────────────┘      └──────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
                           UMM 元数据服务 / 内存服务
```

---

## 3. 模块详细设计

### 3.1 `umm_client.py` — ctypes 底层绑定

**职责**：唯一与 `libumm.so` 直接交互的模块，负责 C 结构体映射、函数签名绑定、错误转换。

#### C 结构体映射

Python 侧使用 `ctypes.Structure` 逐字段映射 C 头文件中的结构体，**必须与 C 侧保持严格对齐**：

| 结构体 | 用途 | 关键字段 |
|--------|------|----------|
| `ChunkDescriptor` | Chunk 分配/释放/读写的句柄 | `chunk_id`, `base_gpa`, `user_size` |
| `ChunkMetadata` | Chunk 元数据查询结果 | `name`, `gpa`, `size`, `primary_tier`, `has_ssd_copy` |
| `SsdDeviceConfig` | 多 SSD 设备配置项 | `path[256]`, `size` |
| `StorageResource` | 拓扑中的单个设备 | `tier`, `device_path[256]`, `capacity`, `base_offset`, `online` |
| `StorageTopology` | 完整拓扑信息 | `node_id`, `num_resources`, `resources[20]` |
| `UMMConfig` | 客户端初始化配置 | transport, server 地址, ssd_devices[16], `num_ssd_devices` 等 |

> ⚠️ **对齐敏感性**：`UMMConfig` 在 Python 侧的 `sizeof` 必须与 C 侧完全一致（当前为 **7448 字节**）。任何字段增减都需要重新验证 ctypes 对齐（`umm_init` 内部按 `sizeof(UMMConfig)` memcpy，缺字段 = 越界读 Python 堆）。

#### `UMMLib` 类

- **动态库加载**：通过 `find_libumm_so()` 按优先级查找 `.so` 文件：
  1. `UMM_BUILD_DIR` 环境变量（**推荐**，本仓库指向 `umm/build`）
  2. `../UMM/build/libumm.so`（旧布局候选）
  3. `../../UMM/build/libumm.so`（旧布局候选）

  注意默认候选是历史遗留的 `UMM/` 大写目录；本仓库实际为 `umm/`，
  因此在仓库内直接使用时应显式设置 `UMM_BUILD_DIR=$PWD/umm/build`。
- **签名绑定**：在 `_setup_signatures()` 中为每个 C 函数声明 `argtypes` 和 `restype`，防止 ctypes 默认推断带来的类型不匹配问题。
- **可选符号绑定**：`umm_alloc_on_device` / `umm_get_topology` / `umm_invalidate`
  用 `getattr` 做可选绑定，缺失时调用处再报 `RuntimeError`，不影响其他 API 加载。
- **异常转换**：所有分配/读写操作在 `rc != UMM_OK` 时抛出 `RuntimeError`，并附加 `umm_error_string()` 的错误描述。

#### 支持的 C API 映射

| C API | Python 方法 | 说明 |
|-------|-------------|------|
| `umm_init` | `UMMLib.init(cfg)` | 初始化 UMM 会话 |
| `umm_deinit` | `UMMLib.deinit()` | 关闭会话 |
| `umm_alloc` | `UMMLib.alloc(size)` | 默认 tier（CXL）分配 |
| `umm_alloc_tiered` | `UMMLib.alloc_tiered(size, tier)` | 按 tier 分配（任意设备） |
| `umm_alloc_on_device` | `UMMLib.alloc_on_device(size, tier, device_idx)` | **指定设备分配**（可选绑定；当前 libumm.so 未导出该符号，属已知基线问题） |
| `umm_free` | `UMMLib.free(desc)` | 释放 Chunk |
| `umm_read` / `umm_write` | `UMMLib.read/write(desc, offset, size/data)` | 字节级读写 |
| `umm_read` / `umm_write` | `UMMLib.read_into/write_from(desc, offset, buf)` | 零拷贝读写（调用方 buffer 直取指针） |
| `umm_lookup_chunk` | `UMMLib.lookup_chunk(name)` | 按名称查找 |
| `umm_get_topology` | `UMMLib.get_topology()` | 查询拓扑（可选绑定） |
| `umm_register_storage_tier` | `UMMLib.register_storage_tier(tier, path, capacity)` | 注册 tier 存储设备（tier_router 重建） |
| `umm_fence` | `UMMLib.fence()` | 落盘屏障（共享盘读共享场景） |
| `umm_invalidate` | `UMMLib.invalidate(desc, offset, size)` | 丢弃缓存页视图（可选绑定） |

---

### 3.2 `client.py` — UMMServiceClient

**职责**：在 `UMMLib` 之上提供面向 Chunk 生命周期的高层封装，处理 tier 映射、多设备配置、拓扑查询。

#### 初始化流程

```python
client = UMMServiceClient(
    meta_addr="127.0.0.1:20001",
    mem_addr="127.0.0.1:20002",
    node_id=0,
    ssd_device="/tmp/ssd.raw",        # 兼容旧版：单 SSD
    ssd_devices=[("/tmp/ssd1.raw", 1<<30), ("/tmp/ssd2.raw", 1<<30)],  # 多 SSD
    tier_aware=True,                  # tier 路由模式（SSD 数据面走 tier_router）
    peer_nodes="0:10.0.0.11:20002",   # Phase 1 跨节点对等表
    rpc_token="lab-token",            # 共享密钥（须与服务端一致）
    ssd_owner_node=0xFF,              # 属主回退（0xFF=本节点）
    data_max_io=0,                    # 数据面 RPC payload 上限（0=默认 1MB）
    memory_size=0,                    # 本地内存数据面容量（0=默认 64MB）
    local_mem_as_dram=0,              # 1=本地内存注册 DRAM tier（Phase 2）
    mem_device="",                    # 内存层后备设备（Phase 2.5 共享窗口）
)
```

1. 实例化 `UMMLib`，加载 `libumm.so`
2. 构造 `UMMConfig`：
   - `transport = b"" if tier_aware else b"mock"`；`consistency_model = b"hardware"`
   - 填充 server 地址、node_id、Phase 1 远程数据面字段
     （`peer_nodes` / `rpc_token` / `ssd_owner_node` / `data_max_io`）
   - 若传 `ssd_devices`，最多取 16 个设备写入 `cfg.ssd_devices[]`，设置 `num_ssd_devices`
   - 若传 `ssd_device`（旧版单设备），写入 `cfg.ssd_device`
   - `mem_device` 非空时写入 `cfg.cxl_device`（两个 tier 分支复用该字段，避免 ABI 变更）
3. 调用 `umm_init()`，失败则抛出 `RuntimeError`
4. `tier_aware=True` 时还需在 init 之后、任何数据 I/O 之前调用
   `enable_ssd()`（即 `umm_register_storage_tier`）注册 SSD tier

#### `create_chunk()` 的 tier 路由逻辑

```
media_type="dram"  →  umm_alloc()           [CXL tier, 任意设备]
media_type="ssd" + device_idx=None   →  umm_alloc_tiered(SSD)   [SSD tier, 任意设备]
media_type="ssd" + device_idx=N      →  umm_alloc_on_device(SSD, N)  [指定 SSD 设备 N]
```

`device_idx` 仅在 `media_type="ssd"` 时有效，其他情况抛出 `ValueError`。

#### 拓扑查询

`get_device_list()` 调用 `umm_get_topology()`，将 C 结构体数组转换为 Python `dict` 列表：

```python
[
    {"tier": 1, "device_path": "/dev/cxl/mem0", "capacity": 67108864, "base_offset": 0, "online": True},
    {"tier": 2, "device_path": "/tmp/ssd1.raw", "capacity": 1073741824, "base_offset": 0, "online": True},
    {"tier": 2, "device_path": "/tmp/ssd2.raw", "capacity": 1073741824, "base_offset": 1073741824, "online": True},
]
```

---

### 3.3 `allocator.py` — FineGrainedAllocator

**职责**：在 UMM Chunk 之上实现细粒度内存分配器。向 UMM 申请大块 Chunk（如 2MB/4MB），在本地用空闲链表管理，将 Chunk 切分为用户所需的小块 `Block`。

#### 核心数据结构

**`Block`**（返回给用户）
```python
@dataclass
class Block:
    chunk_id: int       # 所属 UMM Chunk 的 ID
    offset: int         # 在 Chunk 内的字节偏移
    size: int           # 用户实际请求的大小
    gpa: int            # Chunk 的 base_gpa（全局物理地址）
    chunk_size: int     # UMM 实际分配的 page 对齐大小
```

**`ChunkBuffer`**（内部 Chunk 状态）
```python
@dataclass
class ChunkBuffer:
    chunk_id: int
    base_gpa: int
    total_size: int          # UMM 实际分配的 page 对齐大小
    media_type: str          # "dram" 或 "ssd"
    device_idx: Optional[int]  # SSD 指定设备时使用
    free_list: List[(offset, size)]  # 空闲区间列表，按 offset 排序
```

空闲链表初始状态为 `[(0, total_size)]`，即整个 Chunk 可用。

#### 分配算法：首次适应 (First-Fit)

```python
def alloc(self, size: int) -> Optional[int]:
    for i, (offset, free_size) in enumerate(self.free_list):
        if free_size >= size:
            if free_size == size:
                self.free_list.pop(i)
            else:
                self.free_list[i] = (offset + size, free_size - size)
            return offset
    return None
```

- 遍历 `free_list`，找到第一个能容纳 `size` 的空闲区间
- 精确匹配则移除该区间，否则拆分区间（前部分分配，后部分保留）
- 时间复杂度 O(n)，n 为空闲区间数（通常很小）

#### 释放与合并

```python
def free(self, offset: int, size: int) -> None:
    # 1. 插入新区间
    # 2. 按 offset 排序
    # 3. 合并相邻区间（如果前区间的 end == 当前 offset）
```

释放后将相邻空闲块合并，避免碎片。

#### `FineGrainedAllocator.alloc()` 完整流程

```
1. 参数校验 (media_type ∈ {"dram", "ssd"}, device_idx 仅 SSD 支持)
2. 加锁 (self.lock)
3. 在现有 Chunk 中查找：
   - 只考虑同 media_type 且 device_idx 匹配的 ChunkBuffer
   - 对每个 ChunkBuffer 调用首次适应算法
   - 若命中，返回 Block
4. 现有 Chunk 不足，申请新 Chunk：
   - chunk_size = max(requested_size, default_chunk_size)
   - 调用 client.create_chunk(chunk_size, media_type, device_idx)
   - 创建 ChunkBuffer，插入 free_list
   - 从新 Chunk 中分配，返回 Block
5. 若 UMM 返回内存不足，捕获 RuntimeError → 转换为 MemoryExhaustedError
```

#### Chunk 回收策略

`free(block)` 时：
1. 找到 `block.chunk_id` 对应的 `ChunkBuffer`
2. 将 `(block.offset, block.size)` 归还到空闲链表
3. 若 `ChunkBuffer.is_fully_free()`（空闲链表仅剩 `[(0, total_size)]`）：
   - 构造 `ChunkDescriptor`，调用 `umm_free()` 归还给 UMM
   - 从 `chunks_by_type[media_type]` 中移除

#### 批量预分配

`preallocate_chunks(count, chunk_size, media_type, device_idx)`：
- 一次性申请多个 Chunk，减少 UMM 往返
- 每个 Chunk 以 `ChunkBuffer` 形式加入 `chunks_by_type`，处于完全空闲状态
- 后续 `alloc()` 可直接从中分配，无需再次调用 UMM

#### Block 级读写

`FineGrainedAllocator` 提供基于 `Block` 的 `read()` 和 `write()` 方法：
- 参数校验：offset/size 不能超出 Block 边界
- 构造 `ChunkDescriptor`（从 Block 的 `chunk_id`, `gpa`, `chunk_size`）
- 计算 `chunk_offset = block.offset + offset`
- 调用 `UMMLib.read()` / `UMMLib.write()` 进行底层 I/O

---

### 3.4 `virtual_media.py` / `_vm_segment.py` / `sparse_kv/store.py` — 稀疏 KV 专用介质与对接层

合并重构后，原 `virtual_media` 与 `sparse_kv/media.py` 合并为统一栈，
按职责拆为两层：

- **VirtualMedia（介质层）**：只负责"盘与数据路径"；
- **SparseKVStore（框架对接层）**：只负责"框架语义与地址映射输出"。

#### VirtualMedia 职责

- **盘感知**：通过 `umm_get_topology` 发现在线 SSD，用 `umm_alloc_on_device`
  为每盘分配 extent；
- **可插拔语义键打散策略**：策略接口从 `locate(index)` 升级为 `locate(key)`，
  稀疏 KV 场景 `key = (layer_id, token_idx)`；
- **段聚合写路径**：同盘单元在主机侧缓冲，满则以 super page 粒度一次连续
  IO 下盘；读路径只供地址规划与 CPU 兜底，不执行生产加载。

#### SparseKVStore 职责

- **vllm block 语义**：`offload()` 把 `KVBlockRef` 按 token 展开为定长单元；
- **slot_table 元数据**：`(layer, token) -> 8B packed entry`，只保留 `VALID`
  flag，IN_BUF/ON_SSD 判定下沉到 `VirtualMedia.is_buffered()`；
- **地址规划 `plan()`**：把 `(layer_id, topk_tokens)` 翻译成 descriptor buffer
  供 GPU 直通算子入图加载；
- **CPU 兜底 `fetch()`**：无直通硬件环境的数据校验与降级读。

#### 核心数据结构

**Descriptor buffer**（GPU 直通算子与本项目的唯一契约，32B/entry）：

```
header:  { count : u32, layer_id : u32, reserved : u64 }
entry[i]: { ssd_id : u32, flags : u32, lba_offset : u64,
            length : u64, dst_offset : u64 }
```

- `ssd_id`：目标 SSD 设备索引；
- `flags`：`0=DISK`（算子从盘直通读），`1=HOST_READY`（CPU 已兜底回填 staging，
  算子跳过该条 IO）；
- `lba_offset`：盘侧字节地址 = `extent_base(ssd_id) + slot_table offset`；
- `dst_offset`：固定 staging buffer 内偏移 = `i * unit_size`。

#### 写路径流程

```python
# 1. 分盘：key=(layer, token)
device_idx = vm.locate(key)

# 2. 追加到该盘段缓冲，返回 extent 内偏移
offset = vm.write(key, unit_data)

# 3. 记录到 slot_table
slot_table.set(layer, token, offset, FLAG_VALID)

# 4. 请求边界 / prefill->decode 切换时 flush
vm.flush()   # 半满段也整段下盘
```

#### 地址规划（读路径）流程

```python
# 1. 算盘号（零元数据，纯哈希）
devices = vm.locate_batch([(layer, t) for t in topk])

# 2. 批量取 slot_table 偏移
entries = slot_table.gather(layer, topk)

# 3. 组装 descriptor buffer
for i, (t, e) in enumerate(zip(topk, entries)):
    off = entry_offset(e)
    if vm.is_buffered(devices[i], off):
        # CPU 兜底：数据 memcpy 到 staging，entry 标记 HOST_READY
        staging[i*unit_size:] = vm.read_buffered(devices[i], off)
        flags = HOST_READY
    else:
        flags = DISK
    pack entry(devices[i], flags, extent_base + off, unit_size, i*unit_size)
```

#### 线程安全

- `VirtualMedia.write()` 单写者串行化（`self._write_lock`），保证同盘段缓冲
  追加与 flush 的原子性；
- `slot_table` 读路径无锁，写路径（`set/invalidate`）在细粒度锁内完成；
- `SparseKVStore.plan()` 仅在 CPU 侧做 ALU 与内存访问，µs 级，可在 graph
  replay 之前同步完成。

---

### 3.5 `virtual_media_strategy.py` — 数据打散策略

**职责**：定义语义键到 SSD 设备的打散策略抽象，稀疏 KV 场景内建
`position_hash`。

#### 抽象基类

```python
class PlacementStrategy(ABC):
    @abstractmethod
    def locate(self, key) -> int:
        """根据语义键返回目标 SSD 设备索引。"""
```

稀疏 KV 场景键为 `(layer_id, token_idx)`。

#### 内建策略

| 策略类 | 策略名 | 说明 |
|--------|--------|------|
| `PositionHashStrategy` | `position_hash` | 确定性位置哈希：`((t*STEP_IDX + l*STEP_LAYER) % PRIME) % N_SSD` |

#### 位置哈希参数约束

- `STEP_IDX`、`STEP_LAYER` 为大于 `N_SSD` 的互异质数；
- `PRIME` 为更大质数，推荐在最大 `token_idx` / `layer_id` 范围内无回绕；
- 性质一：任意连续 `N_SSD` 个 token 恰好覆盖全部盘；
- 性质二：同一 token 连续 `N_SSD` 层恰好覆盖全部盘。

#### 策略工厂

```python
create_strategy("position_hash", num_devices=8, {
    "step_idx": 17,
    "step_layer": 23,
    "prime": 2229299,
})
```

内建策略注册在 `_STRATEGY_REGISTRY` 中，用户可通过 `register_strategy()`
在运行时注册自定义 `PlacementStrategy` 子类。

---

### 3.6 `__init__.py` — 包入口

统一导出公共 API，使用者只需：

```python
from bmpclient import (
    FineGrainedAllocator, Block,
    VirtualMedia, SparseKVStore, KVBlockRef,
    PlacementStrategy, PositionHashStrategy, create_strategy,
)
```

---

## 4. 多 SSD 设备支持

### 4.1 设计动机

UMM 内存服务支持注册多个 SSD 设备（如 `/tmp/ssd1.raw`, `/tmp/ssd2.raw`），每个设备有独立的容量和虚拟地址偏移。客户端需要：
1. 能够查询所有 SSD 设备的拓扑信息
2. 能够将数据定向分配到指定 SSD 设备
3. 保持与单 SSD 设备的向后兼容

### 4.2 实现机制

**C 侧**：
- `UMMConfig` 新增 `ssd_devices[16]` 数组和 `num_ssd_devices` 字段
- `umm_get_topology()` 已实现并导出，返回每个设备作为独立的 `StorageResource`
- `umm_alloc_on_device(size, tier, device_idx, desc)`：**当前 libumm.so 未导出
  （已知基线问题，见 `README_交付说明.md`）**；Python 侧按 `getattr` 可选绑定，
  缺失时调用 `alloc_on_device()` 才报错

**Python 侧扩展**：
- `UMMServiceClient.__init__()` 新增 `ssd_devices` 参数：
  ```python
  ssd_devices=[("/tmp/ssd1.raw", 1<<30), ("/tmp/ssd2.raw", 1<<30)]
  ```
- `create_chunk(size, "ssd", device_idx=N)` 路由到 `umm_alloc_on_device()`
- `get_device_list()` 遍历拓扑全部槽位并过滤 `online`（ummD 按 tier 稀疏
  存放、mem_service 返回紧凑数组，故不能仅取前 `num_resources` 项）

**虚拟地址布局**：
- 设备 0 的虚拟偏移从 0 开始
- 设备 N 的虚拟偏移为 `sum(capacity[0..N-1])`
- `base_offset` 字段反映该设备在虚拟地址空间中的起始位置

### 4.3 向后兼容

- 旧版 `ssd_device="/tmp/ssd.raw"` 参数仍然有效，走 legacy 单设备路径
- 新版 `ssd_devices` 参数优先；若两者同时传入，`ssd_devices` 生效

---

## 5. 线程安全

| 模块 | 同步机制 | 保护范围 |
|------|----------|----------|
| `FineGrainedAllocator` | `threading.Lock` | `chunks_by_type` 的增删改、`alloc()` / `free()` 的完整流程 |
| `VirtualMedia` | `threading.Lock` | 段缓冲追加与 flush 串行化 |
| `SparseKVStore` | `threading.Lock` | `offload` 写路径串行化；`plan/fetch` 读路径无锁（slot_table 写锁保护 set/invalidate） |
| `UMMLib` | 无（C 库内部同步） | `libumm.so` 内部使用 `pthread_mutex` 保护全局状态 |

---

## 6. 异常体系

```
RuntimeError            ← UMMLib 在 C API 返回错误时抛出
    ├── MemoryExhaustedError  ← FineGrainedAllocator 在 UMM 内存不足时转换抛出
    └── SegmentFullError      ← VirtualMedia 段耗尽时抛出

ValueError              ← 参数校验失败（如 device_idx 用于 DRAM、读写越界、slot 越界等）
FileNotFoundError       ← find_libumm_so() 找不到 libumm.so
```

---

## 7. 测试架构

### 7.1 两类测试形态

**FakeUMMLib 形态（无依赖，CI 可跑）**：`test_concurrent_io.py`、
`test_virtual_media.py`、`test_sparse_kv.py` 全部基于
`bmpclient/testing.py` 的 `FakeUMMLib`（内存 dict 模拟 C 库，含
`alloc_on_device` / `get_topology` 模拟与读写日志），不需要启动任何服务。

**真实服务形态**：`test_allocator.py` 在 `setUpClass()` 中通过
`subprocess.Popen` 自动启动 UMM 服务端：

```python
# 启动 metadata service（注意：当前用例仍引用旧二进制名/旧目录布局，
# 属已知基线问题，实际二进制是 umm/bin/ummd 与 umm/bin/umms）
ummd_proc = Popen(["umm-metadata-service", "-p", "20001", "-b", "127.0.0.1"])

# 启动 memory service
umms_proc = Popen(["umm-memory-server", "-p", "20002", "-b", "127.0.0.1",
                   "-n", "0", "-s", str(128*1024*1024), "-d", "/tmp/umm_test_ssd"])

# 轮询等待端口就绪（最多 4 秒）
```

### 7.2 测试覆盖

**`test_allocator.py`**：
- 基本分配/释放
- 同一 Chunk 内多次分配
- 跨 Chunk 自动扩展
- Chunk 完全空闲后自动回收
- 批量预分配
- DRAM/SSD 隔离与容量耗尽
- 多线程并发 alloc/free
- Block 级 read/write（含偏移、越界检查、SSD 介质）

**`test_virtual_media.py`**（`FakeUMMLib`）：
- 初始化与默认 `position_hash` 策略
- 位置哈希性质（确定性、连续 N_SSD token/层覆盖全盘）
- `locate` 与 `locate_batch` 一致性
- 写返回 offset、flush 后段粒度连续 IO
- `read_batch` 数据一致性、IN_BUF 未下盘读零盘 IO
- 相邻 offset 合并为一次读
- `release` 段回收复用
- 段满抛 `SegmentFullError`
- 按盘异构段大小
- close 后写操作失败

**`test_sparse_kv.py`**（`FakeUMMLib`）：
- `offload/flush/release/fetch` 数据一致性
- `plan()` descriptor buffer 地址映射正确性
- `HOST_READY` 兜底与 staging 数据回填
- `max_topk` 限制、未卸载 token 报错
- `prefetch` 缓存命中避免盘读

---

## 8. 目录结构

```
bmpclient/
├── __init__.py                  # 包入口，导出公共 API
├── umm_client.py                # ctypes 绑定：C 结构体 + UMMLib
├── client.py                    # UMMServiceClient：Chunk 生命周期 + 拓扑查询
├── allocator.py                 # FineGrainedAllocator + ChunkBuffer + Block
├── virtual_media.py             # VirtualMedia（稀疏 KV 专用介质层）
├── _vm_segment.py               # 段（super page）分配器与聚合写缓冲
├── virtual_media_strategy.py    # 语义键打散策略抽象（position_hash 内建）
├── virtual_media_config.py      # VirtualMedia 配置加载
├── config/
│   └── virtual_media.json       # 默认策略配置
├── sparse_kv/                   # 稀疏 KV 框架对接层
│   ├── __init__.py
│   ├── store.py                 # SparseKVStore + PlanView
│   ├── slot_table.py            # (layer, token) -> 偏移 packed entry
│   └── vllm_adapter.py          # KVBlockRef
├── README.md                    # 用户文档
├── DESIGN.md                    # 本设计文档
├── scripts/
│   ├── _common.py               # 服务状态检查辅助
│   ├── demo_allocator.py        # 分配器功能演示
│   ├── demo_sparse_kv.py        # 稀疏 KV 卸载/地址规划演示
│   └── run_all_demos.sh         # 一键运行所有演示
└── tests/
    ├── test_allocator.py        # 分配器测试
    ├── test_virtual_media.py    # VirtualMedia 介质层测试
    └── test_sparse_kv.py        # SparseKVStore 对接层测试
```

---

## 9. 关键设计决策

### 9.1 为什么用 ctypes 而不是 HTTP REST？

`bmpclient` 的前身使用 HTTP 调用 `basemempool` 的 REST API。迁移到 ctypes 的原因：
- **性能**：C API 直接绕过 HTTP 序列化/反序列化开销，分配延迟更低
- **功能完整性**：UMM 的 C API 提供更细粒度的控制（如 `umm_read`/`umm_write` 直接操作 GPA）
- **统一性**：与 C 侧工具链共享同一套 `libumm.so`

### 9.2 为什么 ChunkBuffer 用空闲链表而不是 bitmap？

- **简单性**：空闲链表实现简单，分配/释放/合并逻辑清晰
- **碎片容忍**：Chunk 回收策略（完全空闲才归还 UMM）意味着单个 Chunk 内的碎片不会长期积累
- **规模小**：每个 Chunk 通常 2MB~4MB，用户请求的 Block 通常在 KB~MB 级别，空闲区间数量不会膨胀

### 9.3 device_idx 的设计

- **语义清晰**：`device_idx` 是 SSD pool 中设备的**索引**（0-based），而非设备路径
- **不混淆 0**：`device_idx=0` 明确指向第一个 SSD 设备，不会与 "未指定" 混淆
- **仅 SSD 支持**：CXL tier 是统一地址空间，无需指定设备

### 9.4 为什么读路径不经过本项目而是输出地址映射？

- **图模式兼容**：decode 加载必须在 CUDA/ACL Graph 内执行，CPU 侧无法入图；
- **性能**：GPU 直通存储（NDS / GPUDirect 等）延迟远低于 CPU 转发；
- **契约最小化**：与 GPU 算子之间只有一块固定地址 descriptor buffer 的内存布局契约，
  便于算子侧固化到 kernel 元数据；
- **写路径仍由 CPU 完成**：KV cache 卸载对延迟不敏感，保留 CPU 接口并按打散策略落盘。
