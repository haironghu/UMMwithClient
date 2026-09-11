# 单盘直接 I/O 与 trace 回放

这套工具用于没有 GPU/NPU 的 Linux 主机：从其他机器导出 top-k token 列表，在真实 SSD 文件后端上生成可校验的模拟 KV，比较 4 KiB 小写与大段聚合写之后的读取表现。

需要覆盖实际 SparseKVStore/VirtualMedia、排布顺序对照和在线追加时，使用新增的 [真实排布路径回放](10_真实排布路径回放与实施计划.md)。本文对应原 SSD pool 微基准。

## 实现范围

调用路径是 `bench_single_ssd.py → ctypes → libumm SSD pool → O_DIRECT pread/pwrite`。不启动 RPC 服务，不加载模型，不依赖 CANN/CUDA/libnvm，不经过 Python KV 缓存。它是 SSD 数据面微基准，没有覆盖 SparseKVStore 的完整 offload/fetch、槽位页表或 NPU 直通链路，因此结果不能称为模型 TPOT。

新增 `ssd_backend_direct.c`，设备字符串格式为 `direct:<窗口起点字节数>:/绝对路径`。例如 `direct:4096:/data/bench.raw`，注册容量是从起点开始可访问的窗口大小。

- 后端只打开已经存在的普通文件或块设备，不创建、不截断，不 mmap，无应用层 buffered I/O 回退。旧文件后端行为保留。
- 起点、容量、每次 I/O 偏移及长度必须按 4096 B 对齐。指针按 `max(4096, 主机页大小)` 对齐，以兼容 aarch64 的大页配置；不对齐指针使用线程私有、可复用的中转缓冲。基准主动使用对齐缓冲，并检查 `bounce_bytes == 0`。
- 检查窗口边界、整数溢出，处理 EINTR、短读写、EOF；同步调用 fdatasync 并返回错误。
- 记录实际成功的 pread/pwrite 调用数、字节数及中转缓冲字节数。调用统计不是 NVMe 命令数；内核可能拆分大请求。统计重置应在没有在途请求时进行。
- 拒绝 tmpfs/ramfs。文件系统或设备不支持当前直接 I/O 对齐时会失败，不能改成 buffered 后端继续生成同名性能结果。

Linux 的 O_DIRECT 对齐要求依赖文件系统及内核；它也不等同于 O_SYNC，不能用它证明 SSD 固件缓存已关闭。参见 [Linux open(2)](https://man7.org/linux/man-pages/man2/open.2.html)。本工具写后显式同步，但不控制 FTL、控制器读缓存或 NAND 物理位置。

## aarch64 / EulerOS 编译与功能检查

在服务器源码仓库根目录执行，需要 gcc、g++、GNU make 和 Python 3.7 或以上版本及标准库：

```bash
make -C umm -j4
export UMM_BUILD_DIR="$PWD/umm/build"
python3 -m unittest bmpclient.tests.test_single_ssd_bench -v
make -C umm test-direct
```

`test-direct` 创建并删除 `/tmp/umm_direct_XXXXXX` 小文件，要求 `/tmp` 所在文件系统支持 O_DIRECT；如果服务器 `/tmp` 是 tmpfs，应使用下方目标 SSD 文件回放做功能检查。不要把这个测试的失败解释为硬件性能问题。完整 C 回归可执行 `make -C umm test`，其中部分测试使用本机 socket。

请在 aarch64 主机重新编译，不复用 x86 的 build/bin。库搜索优先使用 `UMM_BUILD_DIR`，随后使用当前仓库 `umm/build`。

## trace 格式

JSONL，每行一次 request、step、layer 的读取批次，按文件行顺序回放：

```json
{"request_id":"request-0","step_id":0,"layer_id":0,"context_length":32768,"topk_token_indices":[19,2,4096,7]}
```

`request_id` 必须是非空字符串；其余标识及 token 下标是非负整数，token 下标必须小于该行 `context_length`。默认每批最多 2048 项，可以少于 2048。重复 token 只读取一次，但按原始次序恢复输出，包括重复位置。导出时使用 **request 内 token 下标**，不要直接填 vLLM 物理 block ID。

基准先取每个 `(request_id, layer_id)` 的最大 context_length，按键排序分配连续区间。全部历史 KV 预先写入，之后回放读取。这是快照回放，不模拟在线 append、淘汰、请求释放或计算与 I/O 重叠。不同生命周期的请求应使用不同 request_id。

每个 token 占一个 4096 B 槽，有效 KV 按 1152 B 计算。槽内全部字节由固定种子的伪随机模式填充，可按 row 重建校验；避免全零数据让压缩/去重影响对比。原始所需容量是 `所有 request/layer 的最大 context_length 之和 × 4096`，再向上补齐至最大写分段大小。trace 全量加载进内存。

## 先运行小样例

先由设备管理方提供确实位于目标单盘上的可写测试目录。下例 `/mnt/test-ssd` 是占位路径，需要替换。你提供的磁盘列表含 RAID/ext4 签名，不能仅因没有 MOUNTPOINT 就把其中某盘作为空盘。

```bash
python3 bmpclient/scripts/bench_single_ssd.py \
  --trace bmpclient/bench/traces/example.jsonl \
  --file /mnt/test-ssd/umm-example.raw \
  --segments 64K 1M \
  --workers 4 --repeats 2 \
  --output /mnt/test-ssd/umm-example.json
```

`--file` 默认必须是新文件，由工具 posix_fallocate 预分配；结果文件也必须是新文件。重跑使用新文件名，或者对专用测试数据文件显式添加 `--reuse-file`。复用不会调整文件大小，文件不足时拒绝执行。样例仅 4 批，用于校验路径，不能据此判断收益或尾延迟。

## 真实 top-2048 回放

```bash
python3 bmpclient/scripts/bench_single_ssd.py \
  --trace /data/traces/top2048.jsonl \
  --file /mnt/test-ssd/umm-top2048.raw \
  --segments 1M 4M \
  --workers 8 --max-read-bytes 1M \
  --repeats 5 \
  --output /mnt/test-ssd/umm-top2048.json
```

4 KiB 小写基线自动加入。每轮依次写入完整数据、同步、回放全部 trace；下一轮倒转方案执行顺序。所有方案共享相同文件、数据、偏移、填充总字节数和数据生成分块大小。这里只改变写入分段大小，基线也是按偏移顺序小写，不代表实际系统中的任意离散分配布局。

按项目的 `1626 planes × 16 KiB` 参数计算，候选分段为 **26,640,384 B**（约 25.40625 MiB）。可以另跑 `--segments 26640384` 与 4 KiB 基线比较。分段须为 4096 的倍数，且所有候选分段都能整除最大分段；因此不要将这个值直接与 1M/4M 放在同一次运行。不同运行需关注数据补齐量的变化。

默认读侧对相邻槽合并，单请求大小不超过 `--max-read-bytes`，不跨空洞多读。`--no-merge` 可单独验证逐槽读取；每组写入方案的读策略保持相同。`--workers` 是同步 pread 的最大并发线程数，不是硬件队列深度。工作线程与缓冲在计时前创建。

默认每批在计时结束后全量校验，可发现映射、顺序、重复 token 或 I/O 数据错误。校验会造成批次间空闲。正确性通过后可另跑 `--no-verify`，所有对比项保持相同设置。`--warmup-batches N` 额外回放前 N 批，暖身不进入统计；这不是清除 SSD 缓存的操作。

## 如何读结果

JSON 保存 trace 与动态库 SHA256、操作系统/内核/架构、目标窗口、参数、每轮写入和逐批读取结果。确认 `status` 为 `complete`；执行过程中已完成的 trial 会保存，进入试验后的异常标为 `failed`。打开库或创建池之前失败时结果文件可能为空，不能当作完成结果。

| 字段 | 含义 |
|---|---|
| `prepare.io_call_ms` / `sync_ms` | 写 API 调用耗时之和 / 最后同步耗时；wall_ms 还含数据生成和复制 |
| `read.batch_us` | 每批规划、线程提交/等待、读取及恢复原顺序的总耗时 p50/p95/p99，排除校验 |
| `read.io_batch_us` | 提交任务到全部读完并复制到暂存区的时间，含调度开销 |
| `batches[].plan_us` / `gather_us` | 读取计划 / 恢复原始 top-k 次序的耗时 |
| `batches[].sum_io_call_us` | 并发线程 I/O 调用耗时之和，不能当成批次墙钟耗时 |
| `effective_unique_kv_MiB_s` | 去重后有效 KV 字节数除以所有批次总耗时 |
| `host_read_MiB_s` | 实际读取字节数除以所有批次总耗时，不是设备峰值带宽 |
| `read_amplification` | 实际读取字节 / 去重后有效 KV 字节；本版固定约 3.56（4096/1152） |
| `io.read_calls/write_calls/*_bytes` | 原生后端实际调用与传输统计；bounce_bytes 应为 0 |
| `comparisons` | 小写与聚合写的总批次耗时中位数之比，以及逐轮配对比值；大于 1 表示聚合写后读取更快 |

有效带宽的分母是各批次活跃时间之和，不含写入准备、暖身、校验和轮间时间。Python 任务提交、排序、内存复制都会影响结果，尤其是大量不相邻的 4 KiB 读；低带宽不能直接归因于 SSD。不要仅看某一轮 speedup；同时看多轮离散程度、实际 I/O 数及字节数是否一致。

正式测试还需记录单盘归属/文件系统、SSD 型号和固件、运行时是否有其他负载，保持相同 CPU/NUMA 配置，并对 worker 数及多个 trace 分别测量。连续文件偏移不保证连续 LBA；即使裸盘连续 LBA 也不保证连续 NAND 地址或特定 die 映射。这组试验只能验证聚合写对后续读表现的影响，不能独立证明 die 打散或模型 TPOT 收益。

## 可选裸设备窗口

仅在已确认可覆盖的专用设备/分区上使用 `--device`。必须同时给出 `--allow-device-write`、`--window-offset`、`--window-bytes`；不能仅指定盘名运行。工具只写窗口开头的 dataset_bytes，窗口须足够容纳全部数据。

底层还要求 `UMM_ALLOW_BLOCK_DEVICE=1`（基准验证上述参数后设置），并以 O_EXCL 打开块设备。O_EXCL 不能代替管理员确认存储软件的占用关系。此模式会覆盖目标区域，不创建文件系统，不做 discard/格式化。

裸设备目前用于基准的单进程单池路径；UMM 完整部署可能由多个池重复打开设备，独占打开会冲突，不能照搬文件后端配置。普通文件的 UMM API 分配、写入、同步、读取已经由 `test-direct` 覆盖；实际服务器、裸设备及固件上的效果仍需现场验证。

## 本地验证记录

- C 后端测试：窗口外保护字节、对齐/中转缓冲、越界、非法长度、EOF、mmap 绕过、统计，以及正常 UMM API 的指定设备读写。
- Python 测试：trace 合法性、最大上下文快照、读合并边界、重复 token 次序、已有文件保护及 trace 路径别名保护。
- 完整 C 回归通过；SparseKVStore、VirtualMedia 与基准 Python 测试通过。
- 临时文件上完成 merge/no-merge、多轮、暖身和 2048-token 批次回放，数据校验通过且中转字节为零。

以上是本机功能验证，没有访问所列 Huawei NVMe 盘，没有在 aarch64 / EulerOS 上执行，也没有真实 SSD 收益结论。
