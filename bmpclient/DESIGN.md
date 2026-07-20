# bmpclient 设计文档

## 1. 概述

`bmpclient` 是 UMM（Unified Memory Management）系统的 Python 客户端库。它通过 `ctypes` 加载 UMM 的 C 动态库 `libumm.so`，将底层的 C API 封装为 Pythonic 的高层接口，提供从粗粒度 Chunk 申请到细粒度内存分配的内存管理能力。

**核心设计目标**：
- **分层抽象**：从底层 C API → Chunk 管理 → 细粒度分配，层层递进
- **多介质支持**：统一处理 CXL（含 DRAM mock）和 SSD 两种存储介质
- **多设备 SSD**：支持将多个物理 SSD 设备注册为统一的 SSD tier，并允许指定设备分配
- **线程安全**：所有状态变更均受锁保护，支持多线程并发访问
- **自动资源管理**：空闲 Chunk 自动回收

---

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          用户层 (User Code)                                  │
│                                                                             │
│   allocator.alloc(size, "ssd", device_idx=1)                                │
│   vm.save(data)              vm.read(index)                                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                      │
                                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        bmpclient 内部模块                                    │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────────┐
│  │ FineGrainedAllocator│  │    VirtualMedia     │  │    UMMServiceClient     │
│  │   (细粒度分配器)     │  │ (跨 SSD 定长存储)   │  │   (Chunk 生命周期管理)   │
│  │                     │  │                     │  │                         │
│  │ • ChunkBuffer 池    │  │ • 多设备条带化      │  │ • create_chunk()        │
│  │ • 空闲链表 + 首次适应│  │ • 可插拔打散策略    │  │ • delete_chunk()        │
│  │ • 自动扩展 / 回收   │  │ • round-robin /     │  │ • read/write chunk      │
│  │ • 线程锁保护        │  │   consistent_hash   │  │ • get_topology()        │
│  │                     │  │ • 定长粒度          │  │                         │
│  │                     │  │ • 线程锁保护        │  │                         │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────────┘
│           │                         │                                      │
│           └─────────────────────────┘                                      │
│                         │                                                  │
│                         ▼                                                  │
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

> ⚠️ **对齐敏感性**：`UMMConfig` 在 Python 侧的 `sizeof` 必须与 C 侧完全一致（当前为 **5312 字节**）。任何字段增减都需要重新验证 ctypes 对齐。

#### `UMMLib` 类

- **动态库加载**：通过 `find_libumm_so()` 按优先级查找 `.so` 文件：
  1. `UMM_BUILD_DIR` 环境变量
  2. `../UMM/build/libumm.so`
  3. `../../UMM/build/libumm.so`
- **签名绑定**：在 `_setup_signatures()` 中为每个 C 函数声明 `argtypes` 和 `restype`，防止 ctypes 默认推断带来的类型不匹配问题。
- **异常转换**：所有分配/读写操作在 `rc != UMM_OK` 时抛出 `RuntimeError`，并附加 `umm_error_string()` 的错误描述。

#### 支持的 C API 映射

| C API | Python 方法 | 说明 |
|-------|-------------|------|
| `umm_init` | `UMMLib.init(cfg)` | 初始化 UMM 会话 |
| `umm_deinit` | `UMMLib.deinit()` | 关闭会话 |
| `umm_alloc` | `UMMLib.alloc(size)` | 默认 tier（CXL）分配 |
| `umm_alloc_tiered` | `UMMLib.alloc_tiered(size, tier)` | 按 tier 分配（任意设备） |
| `umm_alloc_on_device` | `UMMLib.alloc_on_device(size, tier, device_idx)` | **指定设备分配**（SSD 多设备） |
| `umm_free` | `UMMLib.free(desc)` | 释放 Chunk |
| `umm_read` / `umm_write` | `UMMLib.read/write(desc, offset, size/data)` | 字节级读写 |
| `umm_lookup_chunk` | `UMMLib.lookup_chunk(name)` | 按名称查找 |
| `umm_get_topology` | `UMMLib.get_topology()` | **查询拓扑** |

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
    ssd_devices=[("/tmp/ssd1.raw", 1<<30), ("/tmp/ssd2.raw", 1<<30)]  # 新版：多 SSD
)
```

1. 实例化 `UMMLib`，加载 `libumm.so`
2. 构造 `UMMConfig`：
   - `transport = b"mock"`（本地 mock 模式，也支持 `"rpc"` 等）
   - 填充 server 地址、node_id
   - 若传 `ssd_devices`，最多取 16 个设备写入 `cfg.ssd_devices[]`，设置 `num_ssd_devices`
   - 若传 `ssd_device`（旧版单设备），写入 `cfg.ssd_device`
3. 调用 `umm_init()`，失败则抛出 `RuntimeError`

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

### 3.4 `virtual_media.py` — VirtualMedia

**职责**：抽象底层多段 SSD 设备，提供定长数据的条带化存储，并通过可插拔策略决定数据落盘位置。

#### 设计要点

- **直接调用底层 UMM API**：不经过 `FineGrainedAllocator` 的 `Block` 层，直接使用 `UMLLib.alloc_on_device()`、`UMLLib.read()`、`UMLLib.write()`、`UMLLib.free()`
- **均匀分配**：创建时按 `size` 在所有在线 SSD 设备间均分，要求 `size % num_devices == 0`
- **定长条目**：每条数据长度固定为 `granularity`，总槽位数 `slot_count = size // granularity`
- **可插拔数据打散策略**：第 `i` 条数据写入哪个设备由 `DataPlacementStrategy.locate(i)` 决定，默认 `round_robin`
- **索引映射表**：维护 `_index_map`，记录每个全局索引对应的 `(device_idx, slot_within_device)`，保证任意策略下都能正确定位读取
- **元数据仅内存保存**：每个设备段保存 `DeviceExtent`（含 `device_idx` 和 `ChunkDescriptor`）
- **写满保护**：所有槽位写满后再次 `save()` 抛出 `VirtualMediaFullError`

#### 核心数据结构

```python
@dataclass
class DeviceExtent:
    device_idx: int           # SSD 设备索引
    desc: ChunkDescriptor     # UMM ChunkDescriptor（含 GPA、size）
```

#### 索引机制

VirtualMedia 的索引分为两层：**全局逻辑索引**和**物理设备内索引**。

**1. 全局逻辑索引**

`save()` 每次写入返回一个从 0 开始递增的全局索引 `index`；`read(index)` 基于该索引读取。

```python
index = self._next_slot
self._next_slot += 1
return index
```

**2. 设备选择：策略定位**

全局索引通过数据打散策略映射到目标 SSD 设备：

```python
device_idx = self._strategy.locate(index)
```

策略可以是 `round_robin`、`consistent_hash` 或用户自定义策略，策略只返回设备索引，不感知设备内部写入进度。

**3. 设备内槽位：顺序填充**

每个设备容量相同，可容纳的条目数也相同：

```python
slots_per_device = self.slot_count // num_devices
```

`VirtualMedia` 维护 `_device_write_counts` 数组记录每个设备已写入的条数。新数据在该设备内的槽位即为当前计数：

```python
slot_within_device = self._device_write_counts[device_idx]
offset_within_device = slot_within_device * self._granularity
```

写入后该设备计数加 1：

```python
self._device_write_counts[device_idx] += 1
```

**4. 索引映射表 `_index_map`**

由于策略可以是任意映射（尤其是一致性哈希等非均匀策略），不能仅靠数学公式反推设备内槽位。因此 `save()` 时将全局索引到物理位置的映射持久化到内存：

```python
self._index_map.append((device_idx, slot_within_device))
```

`read(index)` 直接查表定位：

```python
device_idx, slot_within_device = self._index_map[index]
offset_within_device = slot_within_device * self._granularity
extent = self._extents[device_idx]
return self._lib.read(extent.desc, offset_within_device, self._granularity)
```

**5. 写满保护**

每个设备容量固定，写入前检查该设备是否还有空槽：

```python
if slot_within_device >= slots_per_device:
    raise VirtualMediaFullError(
        f"device {device_idx} is full: {slot_within_device}/{slots_per_device} slots used"
    )
```

对于非均匀策略，某些设备可能先满，此时会提前抛出 `VirtualMediaFullError`。

#### 写入流程

```python
index = self._next_slot
device_idx = self._strategy.locate(index)
slot_within_device = self._device_write_counts[device_idx]

if slot_within_device >= slots_per_device:
    raise VirtualMediaFullError(...)

offset_within_device = slot_within_device * self._granularity
extent = self._extents[device_idx]
self._lib.write(extent.desc, offset_within_device, data)

self._device_write_counts[device_idx] += 1
self._index_map.append((device_idx, slot_within_device))
self._next_slot += 1
return index
```

#### 读取流程

```python
device_idx, slot_within_device = self._index_map[index]
offset_within_device = slot_within_device * self._granularity
extent = self._extents[device_idx]
return self._lib.read(extent.desc, offset_within_device, self._granularity)
```

#### 策略解析流程

`VirtualMedia.__init__` 的 `strategy` 参数支持三种形式：

```python
strategy = None                         # 读取配置文件
strategy = "round_robin"                # 策略名字符串
strategy = DataPlacementStrategy(...)   # 策略实例
```

优先级：显式传入 > 配置文件 `bmpclient/config/virtual_media.json` > 默认 `round_robin`。

#### 线程安全

使用 `threading.Lock` 保护 `next_slot`、`extents`、`_index_map`、`_device_write_counts`。`save()` 的索引分配、策略定位、写入、映射表更新必须原子完成，防止多线程下同一槽位被重复写入或映射表不一致。

---

### 3.5 `virtual_media_strategy.py` — 数据打散策略

**职责**：定义 VirtualMedia 数据打散策略的抽象，提供内建策略实现和插件加载机制。

#### 抽象基类

```python
class DataPlacementStrategy(ABC):
    @abstractmethod
    def locate(self, index: int) -> int:
        """根据条目索引返回目标 SSD 设备索引。"""
```

#### 内建策略

| 策略类 | 策略名 | 说明 |
|--------|--------|------|
| `RoundRobinStrategy` | `round_robin` | 顺序打散，第 `i` 条写入设备 `i % num_devices` |
| `ConsistentHashStrategy` | `consistent_hash` | 一致性哈希，支持 `virtual_nodes` 配置 |
| `CustomStrategyPlugin` | `custom` | 通过 `importlib` 加载用户模块/类 |

#### 一致性哈希

- 为每个设备创建 `virtual_nodes` 个虚拟节点（默认 150）
- 构造 32 位无符号整数哈希环
- 对 `index` 计算哈希后顺时针找最近的虚拟节点，返回对应设备
- 相同 `index` 永远映射到同一设备，保证读写一致性

#### 插件机制

配置示例：

```json
{
  "strategy": "custom",
  "module": "my_package.my_strategy",
  "class": "MyStrategy",
  "options": {}
}
```

`CustomStrategyPlugin` 使用 `importlib.import_module()` 加载模块，反射获取类，校验其为 `DataPlacementStrategy` 子类后实例化。自定义策略类必须实现 `locate(index)`。

#### 策略工厂

```python
create_strategy("consistent_hash", num_devices=8, {"virtual_nodes": 200})
```

内建策略注册在 `_STRATEGY_REGISTRY` 中，用户也可通过 `register_strategy()` 在运行时注册自定义类。

---

### 3.6 `__init__.py` — 包入口

统一导出公共 API，使用者只需：

```python
from bmpclient import (
    FineGrainedAllocator, Block, VirtualMedia,
    DataPlacementStrategy, RoundRobinStrategy,
    ConsistentHashStrategy, create_strategy,
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

**C 侧扩展**：
- `UMMConfig` 新增 `ssd_devices[16]` 数组和 `num_ssd_devices` 字段
- 新增 `umm_alloc_on_device(size, tier, device_idx, desc)` API
- 新增 `umm_get_topology()` API，返回每个设备作为独立的 `StorageResource`

**Python 侧扩展**：
- `UMMServiceClient.__init__()` 新增 `ssd_devices` 参数：
  ```python
  ssd_devices=[("/tmp/ssd1.raw", 1<<30), ("/tmp/ssd2.raw", 1<<30)]
  ```
- `create_chunk(size, "ssd", device_idx=N)` 路由到 `umm_alloc_on_device()`
- `get_device_list()` 返回所有设备的详细信息

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
| `VirtualMedia` | `threading.Lock` | `extents`、`next_slot`，保证 `save()` 索引分配与写入的原子性 |
| `UMMLib` | 无（C 库内部同步） | `libumm.so` 内部使用 `pthread_mutex` 保护全局状态 |

---

## 6. 异常体系

```
RuntimeError            ← UMMLib 在 C API 返回错误时抛出
    ├── MemoryExhaustedError  ← FineGrainedAllocator 在 UMM 内存不足时转换抛出
    └── VirtualMediaFullError ← VirtualMedia 写满时抛出

ValueError              ← 参数校验失败（如 device_idx 用于 DRAM、读写越界、VirtualMedia 索引越界等）
FileNotFoundError       ← find_libumm_so() 找不到 libumm.so
```

---

## 7. 测试架构

### 7.1 自包含测试

测试不依赖外部已启动的服务，而是在 `setUpClass()` 中通过 `subprocess.Popen` 自动启动 UMM 服务端：

```python
# 启动 metadata service
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

**`test_virtual_media.py`**：
- 创建参数校验（非法 size/granularity、无 SSD 设备、不能整除）
- 多设备均匀分配
- save 返回索引递增
- round-robin 条带化写入
- read 按索引读取一致性
- 写满后抛 `VirtualMediaFullError`
- close 释放底层 Chunk
- 多线程并发 save
- 默认策略为 round-robin
- 显式指定 round_robin / consistent_hash 策略
- 传入自定义策略实例
- 配置文件驱动策略加载
- 策略工厂构造与未知策略异常

---

## 8. 目录结构

```
bmpclient/
├── __init__.py                  # 包入口，导出公共 API
├── umm_client.py                # ctypes 绑定：C 结构体 + UMMLib
├── client.py                    # UMMServiceClient：Chunk 生命周期 + 拓扑查询
├── allocator.py                 # FineGrainedAllocator + ChunkBuffer + Block
├── virtual_media.py             # VirtualMedia + DeviceExtent + VirtualMediaFullError
├── virtual_media_strategy.py    # 数据打散策略抽象与内建实现
├── virtual_media_config.py      # 策略配置文件加载
├── config/
│   └── virtual_media.json       # 默认策略配置
├── README.md                    # 用户文档
├── DESIGN.md                    # 本设计文档
├── scripts/
│   ├── _common.py               # 服务状态检查辅助
│   ├── demo_allocator.py        # 分配器功能演示
│   ├── demo_virtual_media.py    # 虚拟介质功能演示
│   └── run_all_demos.sh         # 一键运行所有演示
└── tests/
    ├── test_allocator.py        # 分配器测试
    └── test_virtual_media.py    # 虚拟介质测试
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

### 9.4 为什么 VirtualMedia 要抽象数据打散策略？

- **解耦**：将 "数据如何分布" 与 "如何读写 Chunk" 分离，策略变更不影响核心读写逻辑
- **可扩展**：用户可通过配置文件切换策略，或实现 `DataPlacementStrategy` 插入自定义策略
- **向后兼容**：默认 round-robin 策略与改造前行为完全一致，不破坏现有 API
- **策略与容量的关系**：当前 VirtualMedia 仍按 `size / num_devices` 为每个设备分配固定 Chunk。非均匀策略（如 consistent_hash）可能导致某些设备先满，此时 save() 会抛出 `VirtualMediaFullError`。若需支持高度不均匀策略，应配合策略提供设备权重/容量比例，当前版本保持简单。
