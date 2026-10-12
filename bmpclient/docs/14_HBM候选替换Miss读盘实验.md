# 固定 DSA 轨迹上的 HBM 候选替换与 Miss 读盘实验

本轮：A800 所在机器、原八盘配置、position_hash、ordered、26,640,384 B
superpage 写入、16 个主机读取线程。不是 NVIDIA GPU kernel 或真实 HBM cache
实现：HBM 驻留状态由离线回放模拟，计时数据面为 SSD → 主机 DRAM。
不包含 CPU Indexer 参考计算、缓存决策、H2D、attention，因此不是模型 TPOT。

## 输入与六组对照

每个样本目录包含 `<layer>_s<context_length>.pt`，同时有原生 `indices`、
`candidate_indices`、`candidate_scores`、`has_reference_candidates` 等字段。
传一个样本目录，不要混合多个样本、TP rank 或重复采集。
当前实验要求 78 层、每层相同且连续的 decode 序列，长度递增 1。

每层 cache 容量 4096 / 8192 行，各跑：

- baseline：原生 top-2048，缓存独立更新。
- same_state_replacement：baseline 缓存状态上的反事实选择；只读它的 miss。
  维持 baseline 驻留状态的额外成本被排除，不能解释为在线策略时延。
- independent_replacement：替换策略自己维护缓存，读取最终选中的 miss。

模拟器与导出器共用 BatchLRU/choose/load_record；默认最多替换 128 项，冷缓存开始，
前 100 步更新缓存但排除出测量指标。当前 token 的 KV 视为外部已生成、直接可用；
只有选中行才进入持久缓存。不模拟其他层共享、预取、DRAM 二级缓存。

## 导出实际 Miss ID

以下命令从 `UMMwithClient` 仓库根目录执行。
导出器依赖同目录的 `simulate_hbm_candidates.py`，读取 PT 需要 CPU PyTorch。
不需要 NPU 或 CUDA，也无需启动 vLLM。

```bash
python3 -u scripts/export_hbm_miss_traces.py /home/h00938526 \
  --out-dir /home/h00938526/hbm_miss_traces \
  --capacities 4096 8192 --max-replacements 128 \
  --warmup-steps 100 --expected-layers 78
```

若 PT 实际位于该目录的 sample 子目录，输入参数改为那个子目录。
输出目录必须不存在；导出失败时保留 `status=running` 的 manifest，不允许运行读盘。
SQLite 磁盘暂存排序，限制内存用量；输出按 step、原始模型 layer 数值顺序排列。
每条记录包含 `topk_token_indices`（现在是 miss IDs）、warmup、缓存统计。
保留零 miss 记录和原 context_length，不能只写实际 miss 的最大 token。

检查 manifest.json 的 measured_totals，应与原 summary.csv 的 misses 一致。
用户已测样本在默认参数下预期：

| 容量 | baseline | same-state | independent |
|---:|---:|---:|---:|
| 4096 | 9,138,218 | 3,456,112 | 4,895,994 |
| 8192 | 2,454,084 | 260,877 | 848,769 |

若不同，先核对 warmup、替换预算、样本、trace 完整性和参考分数；不要直接测盘。

## 查看服务器设备并预览

仓库配置 `bmpclient/bench/eight_devices.huawei.json` 记录 /dev/nvme80n1a 至
/dev/nvme87n1a，offset=0，每盘 window_bytes=15362386386944。
这只是现有配置，不能据此宣称服务器当前设备状态已经核验。
运行机器上可用只读命令检查：

```bash
lsblk -o NAME,PATH,SIZE,TYPE,MOUNTPOINTS,MODEL
nvidia-smi --query-gpu=name,memory.total,pci.bus_id --format=csv
```

```bash
python3 -u scripts/bench_hbm_misses.py \
  --traces-dir /home/h00938526/hbm_miss_traces \
  --repo . \
  --out-dir /home/h00938526/hbm_read_plan --workers 16
```

默认只预览，不打开设备；各组容量/窗口计划在六个 log 文件中。
需要已编译的原生 libumm.so 才能实际执行；构建/库查找沿用已有 bench_layout_replay。

## 实测

沿用已经确认可覆盖的测试设备窗口。每轮都会准备并写入完整的合成 KV snapshot，
之后仅读取 miss；不是读取此前未知映射的已有数据。准备写入时间不计入读取时延。
不格式化或 discard；不修改设备配置扩大窗口。

```bash
python3 -u scripts/bench_hbm_misses.py \
  --traces-dir /home/h00938526/hbm_miss_traces \
  --repo . \
  --out-dir /home/h00938526/hbm_read_results \
  --workers 16 --repeats 3 --allow-device-write
```

只串行运行六组，每个 case 一个独立进程；相邻重复反转 case 顺序。
保持 seed、全量 context、布局和原始 layer hash 一致；比较 mapping_sha256 和原生库哈希。
合成数据逐行可重建，默认验证读回内容。已有文档提过读回全零的问题，若仍出现，
本轮应先修复而不是关闭校验；wrapper 遇到失败即停止。
O_DIRECT 绕过主机 page cache，但不控制 SSD 内部缓存/FTL 或其它进程的设备负载。

每个 token 用 4096 B slot，有效主 KV 1152 B；不把 1152 B 当作真实读请求长度。
关闭 read merge，各 measured case 自动核对实际 read_bytes == miss_rows * 4096。
该结果不能外推为紧凑 1152 B 布局下的物理读量；FTL/NAND 内部读取也未测量。

## 指标

- runs.csv：每个 case 完成立即写入，记录时延、带宽、miss 数等。
- comparison.csv：同一缓存容量下，相对 baseline 的读取时间降低比例和 speedup。
- 每组 JSON：逐 batch/逐盘统计、库和映射哈希、环境、准备/释放时长。
- active_fetch_seconds：排除 warmup 后的所有层 miss fetch 时长之和。
- decode_step_read_us：每个 step 的逐层 fetch 时长之和，报告 P50/P95/P99；
  是磁盘加载分量，不含选址/计算的完整流水 TPOT。层内等所有并发读完成。
- fetch_us 包含零 miss 层（0），nonempty_fetch_us 单独统计有读取的层。
- effective_unique_kv_MiB_s：miss_rows * 1152 / measured fetch seconds。
- submitted_read_MiB_s：实际提交读取字节 / measured fetch seconds。
- 顶层/trial io 是包括准备、warmup 的全程计数；测量分母应使用 measured_read_bytes。

减少 miss 后队列可能更短，有效带宽不一定更高；优先判断读取总时长和 step 尾时延。
缓存 4096 / 8192 对应 78 层数据主体 351 / 702 MiB，但此基准没有分配对应 GPU HBM；
不能声称测过 SSD→A800 HBM 或真实缓存命中路径。

## 验证

```bash
python3 -m unittest discover -s scripts -p 'test_*hbm*.py' -v
UMM_TEST_DIRECT=1 python3 -m unittest bmpclient.tests.test_hbm_miss_replay -v
```

测试只读写临时普通文件。覆盖原模拟器统计一致性、按 step/layer 排序、零 miss、
warmup 排除、单文件与八文件 O_DIRECT 读取、完整 snapshot 容量和数据校验。
