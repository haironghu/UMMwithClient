# concurrent_io：bmpclient 统一并发读写模块设计说明

## 1. 设计动机

bmpclient 现有读写路径有两个不适合上层系统（如 sglang / vllm 的 KV cache 卸载）的限制：

1. **每次 I/O 都有临时拷贝**：`UMMLib.read()` 内部 `create_string_buffer` 分配临时内存再返回
   `bytes`；`UMMLib.write()` 每次 `create_string_buffer(data)` 拷贝。对 GB/s 级 KV cache
   卸载，这一次额外拷贝直接成为吞吐瓶颈。
2. **无并发能力**：`VirtualMedia.save/read` 在全局锁内做 I/O，完全串行。而 ctypes CDLL
   调用本身会释放 GIL，多线程调用 `umm_read`/`umm_write` 可以真正并行。

`concurrent_io` 模块在不改动 UMM C 侧、不改动已有代码行为的前提下，提供**抽象的、
统一的"地址 + buffer"并发读写引擎**。模块不引入任何 KV cache 语义（无 token/page/layer
概念），介质差异（DRAM/CXL/SSD tier）由 UMM 底层处理，引擎不感知。

## 2. 接口说明

### 2.1 UMMLib 零拷贝新增（`umm_client.py`，向后兼容）

```python
def read_into(self, desc: ChunkDescriptor, offset: int, buf) -> int
def write_from(self, desc: ChunkDescriptor, offset: int, buf) -> int
```

- `read_into`：直接读入调用方提供的**可写** buffer（bytearray / memoryview / ctypes 数组），
  通过 `(ctypes.c_char * n).from_buffer(mv)` 取得 buffer 原生指针，全程无中间拷贝。
  buffer 不可写或非 C 连续抛 `ValueError`。
- `write_from`：直接消费调用方 buffer。可写 buffer 走 `from_buffer`；`bytes` 及
  "包住 bytes 的完整 memoryview" 由 `ctypes.c_char_p` 直接引用其内部 buffer（ctypes 在
  调用期间保持引用，不复制数据）；只读子视图等无法安全取指针的场景退化为一次
  `tobytes()` 拷贝（唯一例外，调用方可传 bytes 本体避免）。
- 两方法均为同步语义：返回时数据已落盘 / 已就位；`mv.nbytes == 0` 直接返回 0。

### 2.2 数据模型

```python
@dataclass(frozen=True)
class IOAddress:
    chunk_id: int; base_gpa: int; offset: int; length: int
    # IOAddress.from_descriptor(desc, offset, length)
    # IOAddress.from_block(block, offset=0, length=None)   # 从 allocator.Block 构造

@dataclass
class IORequest:
    address: IOAddress
    buffer: memoryview    # 传 bytes/bytearray 会自动收敛为 memoryview
    opaque: Any = None    # 调用方透传上下文（如 request_id），结果原样带回
```

### 2.3 IOHandle（异步句柄，future 语义）

| 方法 | 语义 |
| --- | --- |
| `done()` | 整批是否完成 |
| `wait(timeout)` | 阻塞至整批完成，返回是否按时完成 |
| `result(timeout)` | 返回逐条字节数列表（提交顺序）；任一失败**抛首个异常** |
| `results(timeout)` | 逐条返回 `Union[int, BaseException]`，供细粒度检查 |
| `exception(timeout)` | 首个异常或 None |
| `add_done_callback(fn)` | 整批完成时回调（已完成则立即调用） |
| `requests` | 原请求列表（含 opaque） |

### 2.4 ConcurrentIOEngine

```python
ConcurrentIOEngine(lib, num_workers=4, num_queues=1, max_inflight=None)
read_batch(requests) / write_batch(requests)        # 同步批量 = submit + result
submit_read / submit_write / submit_mixed -> IOHandle
ConcurrentIOEngine.wait_all(handles, timeout) / wait_any(handles, timeout)
stats() -> {"submitted", "completed", "failed", "inflight"}
close(wait=True)；支持 with 上下文管理器
```

## 3. 语义契约

1. **并发模型**：`num_queues == 1` 时共享 `ThreadPoolExecutor(num_workers)`，不保证跨请求
   顺序；`num_queues > 1` 时维护 num_queues 个**单线程**执行器，请求按
   `chunk_id % num_queues` 路由，**同一 chunk 的 I/O 严格 FIFO 保序**，不同 chunk 并行。
2. **零拷贝**：读直接落在 `request.buffer`（必须可写，否则 ValueError），写直接消费
   `request.buffer`；引擎内部**绝不**分配与数据等大的临时 buffer。
3. **每请求独立成败**：一批中某条失败不影响其他条执行；`result()` 抛首个异常，
   `results()` 逐条可见。
4. **线程安全**：引擎可被多线程共享提交；stats 计数由锁保护。
5. **背压**：`max_inflight` 用 `threading.Semaphore` 实现，submit 时按条 acquire（超限
   阻塞），请求完成时 release。
6. **buffer 生命周期**：同步 API 返回时数据已就位；异步 API 在 handle 完成前，调用方
   不得复用 / 释放 buffer。
7. **边界校验与前缀语义**：offset/length 必须非负；buffer.nbytes 必须 >= length；
   buffer 大于 length 时只读写**前 length 字节**（memoryview 切片不产生拷贝）。
8. **不感知 tier/设备**：地址即 chunk+offset。

## 4. 与 sglang / vllm 的对接示例

```python
engine = ConcurrentIOEngine(client.lib, num_workers=8, num_queues=4)

# sglang hisparse 式卸载：一次提交整批离散写（host_buf 为 pinned host 内存）
reqs = [IORequest(IOAddress.from_descriptor(desc, off, PAGE),
                  memoryview(host_buf)[i*PAGE:(i+1)*PAGE])
        for i, off in enumerate(slot_offsets)]
h = engine.submit_write(reqs)      # 立即返回，不阻塞调度器
schedule_next_batch()              # 做别的事
h.result()                         # 需要时等待；逐条检查用 h.results()

# vllm 式分层拉回：按 chunk 保序读回
reads = [IORequest(IOAddress.from_descriptor(desc, off, PAGE),
                   memoryview(gpu_staging_buf)[i*PAGE:(i+1)*PAGE])
         for i, off in enumerate(hit_offsets)]
engine.submit_read(reads).result()
```

选择 `num_queues > 1` 的场景：同一 chunk 上存在"先写后读 / 覆盖写"等顺序敏感操作
（如先落盘再读取校验）。顺序不敏感的大批量离散 I/O 用 `num_queues == 1` 即可获得
最大并行度。

## 5. 测试

`tests/test_concurrent_io.py` 用 FakeUMMLib（内存 dict 模拟 chunk 存储 + threading 安全，
在 ctypes 指针层面复刻 `umm_read`/`umm_write` 签名）替代真实 libumm.so，因此
`UMMLib.read_into/write_from` 的真实代码路径（含 from_buffer 零拷贝）被完整覆盖，
并通过"设备侧收到的指针 == 调用方 buffer 地址"断言零拷贝成立。覆盖：零拷贝正确性、
2048 条 4KB 大批量一致性、异步/callback、8 线程并发无串扰、num_queues FIFO 保序、
逐条错误传播、背压上限、buffer 前缀语义，以及 wait_all/wait_any/submit_mixed/
from_block/上下文管理器等 API 完整性用例。
