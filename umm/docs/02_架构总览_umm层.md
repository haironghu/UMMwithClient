# 架构总览：UMM 层（C 统一内存管理基座）

> 代码位置：`umm/`（C11，约 1.4 万行）　基线：commit `5c30ddb`
> 角色定位：统一编址 + 分层存储 + 分配/元数据服务。对上暴露 `libumm.so`
> C API（被 bmpclient ctypes 加载），对下管理 CXL/DRAM 与 SSD tier。

---

## 1. 分层视图

```
┌─────────────────────────── 公开 C API（include/umm.h） ───────────────────────────┐
│ umm_init/deinit  umm_alloc[_tiered]/umm_free  umm_read/umm_write                   │
│ umm_register_storage_tier  umm_get_topology   原子操作 / persist / fence           │
└────────────────────────────────────────────────────────────────────────────────────┘
        │ 实现于 src/client_lib/umm_api.c（1245 行，模式路由中枢）
        ▼
┌─────────────── 服务接口层（vtbl 双实现：direct 本地 / rpc 远端） ──────────────────┐
│ MemoryServiceVtbl：alloc/free[_tiered]、register_storage、map_device、             │
│                   ssd_read/ssd_write（可选，回退通路）、get_topology               │
│ MetaServiceVtbl ：register/lookup/unregister chunk、add_ref/release_ref、          │
│                   register_storage_resource、get_storage_topology                  │
└────────────────────────────────────────────────────────────────────────────────────┘
        │                                    │
        ▼ direct（单进程）                    ▼ rpc（客户端 ↔ umms/ummD）
┌──────────────────────┐          ┌────────────────────────────────────┐
│ mem_service_direct.c │          │ mem/meta_service_rpc_client.c      │
│ meta_service_direct.c│          │ mem/meta_service_rpc_server.c      │
└──────────────────────┘          └────────────────────────────────────┘
        │                                    │ 线协议（src/protocol/）
        ▼                                    ▼
┌───────────────── 数据面：tier_router + transports ─────────────────┐
│ local transport（CXL/DRAM：malloc/mmap memcpy，P0 锁收窄）          │
│ ssd transport（SSD：文件 mmap / libnvm 回退，见 04_数据面文档）       │
└────────────────────────────────────────────────────────────────────┘
```

## 2. 统一编址（GPA）

64 位全局物理地址，位域编码 `node | tier | offset`：

```
[63:56] node_id   [55:48] tier   [47:0] tier 内偏移
例：0x200000000000000 = node 0, tier=SSD(2), offset 0
```

- `gpa_to_node/tier/offset()` 解码（src/common/types.h）；
- tier 枚举：DRAM=0 / CXL=1 / SSD=2 /（UMM_NUM_TIERS=4）；
- 数据面按 tier 路由（tier_router），分配面按 tier 选择池。

## 3. 三种初始化模式（umm_api.c）

| 模式 | 触发条件 | 分配面 | 数据面 |
|---|---|---|---|
| direct（单进程） | 无 server 地址 | 本地 mem_service_direct | 本地 transports |
| RPC + legacy | 配了 meta/mem_server_addr，`transport="mock"` | RPC 至 umms | mock（malloc memcpy） |
| RPC + tier 路由 | 配了 server 地址，`transport=""`（空） | RPC 至 umms | tier_router → 本地 transports（SSD 设备由 `umm_register_storage_tier` 或 ummD 拓扑配置） |

**架构要点**：RPC 模式下分配权威在 umms（服务端 ssd_pool 位图），但
**SSD 数据面在 client 进程本地**（tier_router → transport_ssd → 本地
ssd_pool → 设备）。两端打开同一设备、容量一致，服务端分配的偏移经 GPA
传到 client，client 池仅作 I/O 路由（不查位图）。

## 4. 模块清单与依赖项

| 目录 | 关键文件 | 职责 |
|---|---|---|
| `include/` | umm.h | 公开 API、UMMConfig、ChunkDescriptor、StorageTopology、错误码 |
| `src/common/` | types.h、log、error_codes、bitmap_allocator、umm_network、config_parser | GPA 编解码、位图分配器、TCP 封装、yaml 简化解析 |
| `src/protocol/` | protocol_common、meta_protocol、mem_protocol | RPC 帧格式 + 序列化（pack/unpack 严格对称） |
| `src/memory_service/` | mem_service_direct / rpc_client / rpc_server | 分配服务三形态 |
| `src/metadata_service/` | meta_service_direct / rpc_client / rpc_server、meta_hash_table | 元数据服务三形态 + chunk 名→描述符哈希表 |
| `src/transport/` | transport_local、transport_ssd、tier_router、ssd_pool、ssd_backend_libnvm | 数据面（详见 04 文档） |
| `src/client_lib/` | umm_api（模式路由）、umm_transport、umm_router、umm_descriptor | libumm 入口 |
| `src/cis/` | cis_router | compute/in-storage 路由（预留） |
| `src/server/` | mem_server（→umms）、meta_server（→ummD） | 服务端进程主体 |
| `bin/` | ummD.c、umms.c | 可执行入口 |
| `test/` | 15 个测试 + stub_libnvm_host.c | 见 §6 |

**外部依赖（极少）**：pthread、libdl（dlopen libnvm）、标准 C 库。
**可选依赖**：`libnvm_host.so`（生产 SSD 数据面，dlopen 软依赖，缺席时
文件后端照常工作）。

## 5. 构建产物

| 产物 | 目标 | 用途 |
|---|---|---|
| `build/libumm.a` | `make client`（默认含） | C 静态链接 |
| `build/libumm.so` | `make shared`（默认含） | **bmpclient ctypes 加载** |
| `bin/ummd` / `bin/umms` | 默认 | 元数据服务 / 内存服务 |
| `bin/libnvm_host.so` | 默认 | **测试桩**（同名符号替代真库） |
| `bin/test_*` | 默认 | 15 个测试程序 |

## 6. 测试体系

| 层级 | 测试 | 覆盖 |
|---|---|---|
| 单元 | test_allocator / transport_mock / metadata / integration / cross_node / cxl / tier / rw | 基础服务 |
| 并发 | test_p0_concurrent | P0 锁收窄正确性 |
| SSD 池 | test_ssd_pool_io（13 项）/ ssd_dist / simple_dist / multi_device | 池化、跨设备分段 |
| libnvm | test_ssd_libnvm（**24 项**，桩库驱动） | 分派/分段/池级/注册层/探针剔除降级/全灭报错 |
| e2e | bmpclient scripts（场景 A-D） | 全链路 |

`make test` 对 libnvm 用例**钉死桩库**（屏蔽真机环境变量），单测在任何
环境下可重复；真机验证走显式环境变量（见 00_端到端搭建指南.md）。

## 7. 关键设计决策（速查）

1. **vtbl 双实现**：direct/rpc 同一接口，上层无感；
2. **RPC 仅承载控制面**（分配/元数据），数据面永远本地——决定了 SSD tier
   的"服务端分配、客户端 I/O"分工；
3. **可选 vtbl 扩展**：`ssd_read/ssd_write` 置于 vtbl 尾部，NULL 兼容旧实现；
4. **dlopen 软依赖 + 同名桩库**：无真硬件/真库环境全量可测；
5. **窗口化设备路径**：`libnvm:<ctrl>[@ns][+<base_off>]`，机制性避开 LBA0；
6. **每 ctx 互斥锁 + 开机探针**：不假设 libnvm 线程安全/多 init 可用
   （见 R6 文档）。
