# bmpclient — UMM 客户端

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
├── virtual_media.py             # VirtualMedia + DeviceExtent + VirtualMediaFullError
├── virtual_media_strategy.py    # 数据打散策略抽象与内建实现
├── virtual_media_config.py      # 策略配置文件加载
├── config/
│   └── virtual_media.json       # 默认策略配置
├── scripts/                     # 手动验证脚本
│   ├── _common.py               # 服务状态检查辅助
│   ├── demo_allocator.py        # 分配器功能演示
│   ├── demo_virtual_media.py    # 虚拟介质功能演示
│   └── run_all_demos.sh         # 一键运行所有演示
└── tests/                       # 单元测试
    ├── test_allocator.py        # 分配器测试
    └── test_virtual_media.py    # 虚拟介质测试
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

### 3. VirtualMedia

跨多块 SSD 设备的定长数据存储抽象：

- **均匀分配**：创建时按 `size` 在所有在线 SSD 设备间均分空间
- **定长条目**：每条数据长度固定为 `granularity`，总条目数 `size // granularity`
- **可插拔打散策略**：通过策略决定第 `i` 条数据写入哪个 SSD 设备
- **按索引读取**：`save(data)` 返回条目索引，`read(index)` 按索引读取
- **直接调用 UMM**：不经过 `FineGrainedAllocator`，直接调用 `umm_alloc_on_device` / `umm_read` / `umm_write` / `umm_free`
- **写满保护**：所有槽位写满后再次 `save()` 抛出 `VirtualMediaFullError`

#### 数据打散策略

VirtualMedia 的数据打散策略通过 `bmpclient/config/virtual_media.json` 配置，也可在构造函数中显式指定。

内建策略：

| 策略名 | 说明 | 配置项 |
|--------|------|--------|
| `round_robin` | 顺序 round-robin 打散（默认） | 无 |
| `consistent_hash` | 一致性哈希打散 | `virtual_nodes`（默认 150） |
| `custom` | 动态加载用户自定义策略类 | `module`、`class`、`options` |

默认配置文件 `bmpclient/config/virtual_media.json`：

```json
{
  "strategy": "round_robin"
}
```

一致性哈希配置示例：

```json
{
  "strategy": "consistent_hash",
  "virtual_nodes": 150
}
```

自定义策略配置示例：

```json
{
  "strategy": "custom",
  "module": "my_package.my_strategy",
  "class": "MyStrategy",
  "options": {}
}
```

自定义策略类需要继承 `DataPlacementStrategy` 并实现 `locate(index) -> device_idx`：

```python
from bmpclient.virtual_media_strategy import DataPlacementStrategy

class MyStrategy(DataPlacementStrategy):
    def locate(self, index: int) -> int:
        # 返回目标 SSD 设备索引
        return index % self._num_devices
```

#### 使用示例

```python
from bmpclient.client import UMMServiceClient
from bmpclient.virtual_media import VirtualMedia

# 客户端需要配置与 umms 服务端相同的 SSD 设备列表，
# 以便本地 transport 能够正确映射 SSD 地址空间
ssd_devices = [
    ("/tmp/umm_ssd0.raw", 10 * 1024 * 1024),
    ("/tmp/umm_ssd1.raw", 10 * 1024 * 1024),
    ("/tmp/umm_ssd2.raw", 10 * 1024 * 1024),
]

client = UMMServiceClient(
    "127.0.0.1:20001",
    "127.0.0.1:20002",
    ssd_devices=ssd_devices,
)

# 30M 总容量，3 块 SSD 各 10M，单条 1M
# 不传 strategy 时读取 bmpclient/config/virtual_media.json
vm = VirtualMedia(client.lib, size=30 * 1024 * 1024, granularity=1024 * 1024)
print(vm.slot_count)  # 30

# 也可显式指定策略名
vm_ch = VirtualMedia(
    client.lib,
    size=30 * 1024 * 1024,
    granularity=1024 * 1024,
    strategy="consistent_hash",
)

# 顺序写入
for i in range(30):
    data = f"record-{i:04d}".encode().ljust(1024 * 1024, b'\0')
    idx = vm.save(data)

# 读取第 5 条
print(vm.read(5))

vm.close()
client.close()
```

## 运行测试

前置条件：UMM 服务不需要手动启动，测试会自动在后台启动服务。

```bash
cd bmpclient

# 运行分配器测试
PYTHONPATH=.. python3 tests/test_allocator.py

# 运行虚拟介质测试
PYTHONPATH=.. python3 tests/test_virtual_media.py
```

测试覆盖：
- 单 Chunk 内多次细粒度分配与释放
- 跨 Chunk 自动扩容
- Chunk 完全空闲后自动回收
- 相邻空闲块合并
- DRAM / SSD 隔离与容量耗尽
- 批量预分配
- 多线程并发 alloc/free
- Block 级 read/write（含偏移、越界检查、SSD 介质）
- VirtualMedia 多设备均匀分配、round-robin 写入、按索引读取、写满异常
- VirtualMedia 数据打散策略抽象（round-robin、consistent_hash、自定义策略）
- VirtualMedia 配置文件驱动策略加载

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
python3 scripts/demo_virtual_media.py

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
