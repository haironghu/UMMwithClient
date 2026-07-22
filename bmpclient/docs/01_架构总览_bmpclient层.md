# 架构总览：bmpclient 层（Python 客户端）

> 代码位置：`bmpclient/`（Python 包，约 2000 行）　基线：commit `4ce30e6`
> 角色定位：UMM 的用户入口。通过 ctypes 加载 `libumm.so`，向上为 sglang/vllm
> KV cache 卸载提供**统一并发读写接口**。

---

## 1. 模块全景

```
┌────────────────────────── 用户应用（sglang/vllm 卸载引擎） ──────────────────────────┐
│                                                                                      │
│  ConcurrentIOEngine ──────────────┐   VirtualMedia（多设备定长存储，见 §5 已知缺口）  │
│  （并发读写引擎，concurrent_io.py）│   FineGrainedAllocator（Chunk 内细粒度分配）     │
└───────────────────────────────────┼──────────────────────────────────────────────────┘
                                    │ 只依赖
                                    ▼
                        UMMServiceClient（client.py）
                        · Chunk 生命周期（create/delete/read/write）
                        · tier_aware 模式 + enable_ssd（SSD tier 接线）
                        · 拓扑查询（get_device_list）
                                    │ 只依赖
                                    ▼
                        UMMLib（umm_client.py，ctypes 绑定层）
                        · 加载 libumm.so、结构体镜像、rc→异常
                        · 零拷贝 read_into/write_from
                                    │ ctypes / C ABI
                                    ▼
                          libumm.so（见 02_架构总览_umm层.md）
```

## 2. 模块职责与依赖项

| 文件 | 行数 | 职责 | 依赖 |
|---|---|---|---|
| `umm_client.py` | 383 | **UMMLib**：ctypes 绑定层。查找/加载 libumm.so，镜像 C 结构体（UMMConfig/ChunkDescriptor/StorageTopology 等），函数签名声明，错误码转异常；零拷贝 `read_into/write_from`（直接读写用户 buffer，避免二次拷贝） | libumm.so（**必须与 C 侧结构体布局严格一致**，ABI 耦合点） |
| `client.py` | 138 | **UMMServiceClient**：对 UMMLib 的面向对象封装。两种模式：`tier_aware=False`（legacy mock，仅 CXL/DRAM）/ `tier_aware=True`（tier 路由，SSD 数据面）；`enable_ssd()` 注册 SSD tier | UMMLib |
| `concurrent_io.py` | 472 | **ConcurrentIOEngine**：统一并发读写引擎。`IOAddress`（描述符+偏移+长度）、`IORequest`、批量提交（分片+组级聚合消除小 IO 分发开销）、异步 future、背压、保序选项、统计 | UMMLib（不依赖 UMMServiceClient，可直接复用同一底层句柄） |
| `allocator.py` | 277 | **FineGrainedAllocator**：申请大块 Chunk，本地空闲链表+首次适应切分为 Block | UMMServiceClient |
| `virtual_media*.py` | 542 | **VirtualMedia**：跨多 SSD 设备的定长存储 + 可插拔打散策略（§5 有已知缺口） | UMMLib |
| `testing.py` | 145 | **FakeUMMLib**：慢设备模拟（包级工具，e2e 场景 C 用） | 无 |

## 3. 关键使用路径

### 3.1 并发读写主路径（生产推荐）
```python
from bmpclient.client import UMMServiceClient
from bmpclient.concurrent_io import ConcurrentIOEngine, IOAddress, IORequest

client = UMMServiceClient(meta_addr="127.0.0.1:20001",
                          mem_addr="127.0.0.1:20002",
                          node_id=0, tier_aware=True)
client.enable_ssd("libnvm:/dev/libnvm_helper0@1+0x40000000", 16 << 30)

descs = [client.create_chunk(8 << 20, "ssd") for _ in range(4)]
engine = ConcurrentIOEngine(client.lib, num_workers=8, num_queues=4)
engine.submit_write(reqs).result()   # 批量并发写（future）
engine.read_batch(rreqs)             # 批量并发读
```
**引擎并行粒度 = chunk**：N 个 chunk 落入 N 个队列，队列间真并行；
同 chunk 内 IO 保序。批量接口内部做分片提交+组级聚合，避免小 IO 的
任务分发开销主导（e2e 实测优化点，commit `8e10acf`）。

### 3.2 模式选择
| 模式 | 构造 | 数据面 | 适用 |
|---|---|---|---|
| legacy | `tier_aware=False`（默认） | mock transport（本机 malloc memcpy） | 功能联调、CI |
| tier 路由 | `tier_aware=True` + `enable_ssd()` | tier_router → local/ssd transport | 生产（SSD tier） |

## 4. 测试与演示

| 入口 | 内容 |
|---|---|
| `scripts/demo_e2e_servers.py` | 场景 A（基础读写）/B（引擎并发）/C（慢设备模拟），mock 介质 |
| `scripts/demo_e2e_ssd.py` | **场景 D（真实 SSD tier）**：D0 拓扑校验/D1 基础读写/D2 引擎并发+串行对照；文件后端（默认）或 libnvm 后端 |
| `tests/` | test_concurrent_io（引擎单测）、test_allocator、test_virtual_media |
| `scripts/run_all_demos.sh` | 全部 demo 一键回归 |

## 5. 已知缺口与边界

1. **ABI 耦合**：ctypes 结构体是 C 侧的手工镜像。C 侧修改 UMMConfig 等布局时**必须同步** umm_client.py，否则静默错位（无编译期检查）。
2. **VirtualMedia 缺口**：依赖 `umm_alloc_on_device`（按设备放置），当前 libumm 未导出该符号（上游 API 漂移）——13 项相关单测失败为**基线既有问题**。需要时补：MEM_OP 协议 + 服务端处理 + client API + 绑定。
3. **GIL**：ctypes 调用期间 GIL 释放，引擎多线程可真并行进入 C 层。
4. **可选符号**：UMMLib 对新符号（如 `umm_get_topology`）做可选绑定，旧库给出明确报错而非崩溃。
