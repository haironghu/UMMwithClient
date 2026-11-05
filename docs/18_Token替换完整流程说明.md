# Token 替换完整流程说明

> 本文档描述 PPA-aware Token 替换实验的全链路：从离线准备到在线替换、质量验证、带宽测量。
> 面向读者：参与本实验的工程师及论文撰写者。

## 总览：两个 run，一条分界线

| | run A（原生基线） | run B（在线替换） |
|---|---|---|
| 索引张量 | 原生 top-2048 → attention | 替换后集合 → attention |
| hook 角色 | 旁路记录（不干预） | 在线干预（替换索引） |
| dump 类型 | 第一代 trace | 第二代 trace |
| 用途 | 基线输出 + 基线分数 | 对照输出 + 替换记录 |

---

## 1 离线准备（一次性）

```
run A 任意一个样本 dump（78 层齐全）
        ↓
export_ppa_candidates.py --declared-stride-tokens <stride>
        ↓  产出布局清单：78 层 × stride token × 4KB
mqsim_ppa_oracle.py --prepare-only
        ↓  模拟写入全量 snapshot，FTL 按顺序分配物理页
ppa.csv（约 359 万行映射表）
```

`ppa.csv` 每一行记录：`逻辑页号 → 物理位置(channel, chip, die, plane, block, page)`

**关键性质**：映射只依赖层数和 stride，与数据内容无关 → run A/B 共用，不用重跑。仅在 stride 或层数变化时需重做。

### 1.1 stride 的选取

stride 必须 ≥ 实验数据集中最长 prompt 的 token 数（含 chat 模板开销）+ 生成上限。建议在 run A 完成后从 `results.jsonl` 的 `prompt_tokens` 字段取真实最大值并加余量。

---

## 2 run A：原生基线推理

**服务端配置**：`DSA_DUMP_ENABLE=1`（仅此一个开关，不设任何 `DSA_OVERRIDE_*`）

每个 decode step、每一层：

1. 原生 fused indexer → top-2048 token IDs
2. hook 独立计算 FP32 参考排名 → top-2400 + 分数
3. 落盘第一代 trace（`indices` + `candidate_indices`/`candidate_scores`）
4. **索引张量原样返回** → attention 用原生集合计算
5. LRU 缓存用原生集推进（为 run B 准备共享基线状态）

### 2.1 不变量

- 模型行为与完全无 hook 时逐位一致（budget=0 或 dump-only 模式）
- 读取量（SSD 请求的 token 数）与 run B 完全相同
- 命中/未命中数由共享 LRU 决定，两组一致

### 2.2 产出

| 产物 | 路径 | 内容 |
|---|---|---|
| 基线输出 | `runA/results.jsonl` | 每例的 output、tokens、logprobs、耗时 |
| 第一代 trace | `$DSA_DUMP_DIR/<样本id>/` | 每层每步的 `<layer>_s<seq>.pt` |

---

## 3 run B：在线替换推理

**服务端配置**：`DSA_DUMP_ENABLE=1` + 四个 `DSA_OVERRIDE_*` 变量

| 变量 | 示例值 | 含义 |
|---|---|---|
| `DSA_OVERRIDE_ENABLE` | `1` | 启用替换 |
| `DSA_OVERRIDE_MAPPING` | `/path/to/ppa.csv` | prepare 产物 |
| `DSA_OVERRIDE_STRIDE_TOKENS` | `184320` | 与 prepare 一致 |
| `DSA_OVERRIDE_BUDGET` | `128` | 每步每层最多替换对数 |

每个 decode step、每一层（与 run A 的差异用 **粗体** 标出）：

1. 原生 fused indexer → top-2048 token IDs
2. hook 独立计算 FP32 参考排名 → top-2400 + 分数
3. **rank 0 独自运行 `select()` 选择算法**
4. **rank 0 将结果广播给全部 16 个 TP rank**
5. **所有 rank 用替换后的索引张量 → attention 在替换集合上计算**
6. 落盘第二代 trace（新增 `selected`、`swaps`、`cache_state_sha256` 等字段）
7. LRU 缓存用原生集推进（与 run A 相同）

### 3.1 为什么 rank 0 独自选择并广播

NPU 浮点运算（matmul、topk）跨核不保证 bit 级一致。如果 16 个 rank 各自独立跑 `select()`，微小的分数差异可能导致不同 rank 选出不同的 token 集合——而 DSA 的注意力集合是全头共享的，不一致会破坏模型语义。

解决方案：rank 0 算完 `select()` 后将结果（2048 个 int64，约 16KB）广播给所有 rank，保证一致性且消除其他 rank 的选择计算开销。

### 3.2 R0 惰性对照

`DSA_OVERRIDE_BUDGET=0` 时 `select()` 不做任何交换，返回的集合与原生完全相同——run B 的输出应与 run A 逐 token 一致。这是插桩安全性的证明（机制不改变模型行为）。

---

## 4 select() 选择算法

### 4.1 输入

| 参数 | 来源 | 含义 |
|---|---|---|
| `native` | 融合 indexer 的实际输出 | 原生 top-2048 token 集合 |
| `candidates` | FP32 参考计算 | top-2400 token + 分数 |
| `mapping` | `ppa.csv` | 每个 token 的 KV 在盘内的物理位置 |
| `cached` | BatchLRU 当前状态 | 哪些 token 已在 HBM 缓存中 |
| `budget` | `DSA_OVERRIDE_BUDGET` | 最多替换多少对 |
| `current` | 当前 decode 的 token ID | 始终保护 |

### 4.2 约束规则

- **保护**：缓存命中的 token 不动；当前 token 不动；无参考分数的原生 token 不动
- **前提**：只能换"Miss → 同样是 Miss 的额外候选"（保持读盘量不变）
- **不变量**：替换前后集合大小始终 = 2048

### 4.3 贪心循环（最多 budget 次）

```
1. 统计当前选中集合在每个 channel/chip/die/plane 上的请求数
2. 计算拥挤度 = 各级请求数的平方和（越低越分散）
3. 尝试：移出一个低分的原生 Miss token，
        换入一个在其他物理位置的高分候选
4. 只接受拥挤度严格下降的交换
   （平局按分数损失较小者，再按 token ID 破平）
5. 更新负载分布，进入下一轮
```

### 4.4 通俗理解

如果原生选择里有 800 个 token 的 KV 都挤在 channel 0 上（读它们会在 channel 0 排队），`select()` 会把其中分数较低的换成分数较高但位于 channel 3/5/7 的候选——**读同样多的数据，但分散到更多并行资源上**。

---

## 5 质量分析（run A vs run B 对比）

使用 `scripts/quality/analyze_quality.py`：

| 指标 | 含义 | 判读 |
|---|---|---|
| `score_a` vs `score_b` | 任务准确率差 | 替换是否伤质量 |
| `agreement` | 输出一致率 | 替换的影响面 |
| `first_divergence` | 首个分歧 token 位置 | 越晚越好 |
| `lp_delta_mean` | 分歧前逐 token logprob 差 | teacher-forced 信号 |
| `regressions` | A 对 B 错的题列表 | 质量受损的具体样本 |

### 5.1 错题归因（reconcile_swaps.py）

对每个回归样本（A 对 B 错），定位 gold 答案在 prompt 中的 token（offset 对齐），然后逐层逐步统计 gold token 在原生集合中出现却从替换后集合中消失的次数。

- 回归组的 `gold_swapped_out` 占比显著高于对照组 → 质量下降可归因于关键 token 被换出
- 回归组无 gold swap → 注意力边缘效应（与替换无关）

---

## 6 MQSim 时延回放

使用 `mqsim_ppa_oracle.py --applied`：

```
run B 的第二代 dump（每个样本目录）
        ↓
export_ppa_candidates.py（自动识别第二代字段并透传）
        ↓
mqsim_ppa_oracle.py --applied
        ↓  MQSim 模拟读取"实际应用的选择"
comparison.csv：read_seconds, speedup, step_P50/P95, NAND 事务数
```

**与 run A 的 baseline 回放对比** → 得出"PPA-aware 替换将整层 KV 加载时间降低了 X%"。

---

## 7 整条因果链

```
prepare 产出映射 → run B 用映射做在线替换 → 模型输出可能偏离 run A
                                     ↓
                    质量验证：偏离是否在可接受范围内？
                                     ↓
                    带宽验证：偏离（如果可接受）带来了多少读带宽提升？
                                     ↓
                              论文结论
```

---

## 8 运行手册速查

```bash
# ① prepare（一次性，在 MQSim 工作机上）
python3 scripts/export_ppa_candidates.py $SAMPLE_DIR \
    --out-dir ~/prep --declared-stride-tokens 184320
python3 scripts/mqsim_ppa_oracle.py --candidates ~/prep \
    --out-dir ~/prep_run --prepare-only --qd 16
# 产物：~/prep_run/prepare/ppa.csv

# ② run A（推理机，服务端仅 DSA_DUMP_ENABLE=1）
python3 scripts/quality/run_quality_collection.py \
    --dataset lbv2_b01.jsonl --out-dir runA --label A \
    --endpoint http://...:8088/v1 --model glm-5 \
    --dump-root $DSA_DUMP_DIR --max-tokens 32 --style chat --append-ok

# ③ run B（推理机，服务端加四个 DSA_OVERRIDE_* 变量）
#    先 DSA_OVERRIDE_BUDGET=0 做 R0 对照 → 再改 128 正式跑
python3 scripts/quality/run_quality_collection.py \
    --dataset lbv2_b01.jsonl --out-dir runB --label B \
    --endpoint http://...:8088/v1 --model glm-5 \
    --dump-root $DSA_DUMP_DIR --max-tokens 32 --style chat --append-ok

# ④ 分析
python3 scripts/quality/analyze_quality.py --dataset lbv2.jsonl \
    --results-a runA/results.jsonl --results-b runB/results.jsonl --out-dir an

# ⑤ 对账
python3 scripts/quality/reconcile_swaps.py --dataset lbv2.jsonl \
    --results-a runA/results.jsonl --results-b runB/results.jsonl \
    --dump-root $DSA_DUMP_DIR --tokenizer $MODEL_DIR --out-dir an

# ⑥ MQSim 回放（工作机）
python3 scripts/export_ppa_candidates.py $DSA_DUMP_DIR/<id> --out-dir ~/second
python3 scripts/mqsim_ppa_oracle.py --candidates ~/second \
    --out-dir ~/results --applied --capacities 4096 --qd 16
```

---

## 9 代码模块对照表

| 文件 | 职责 |
|---|---|
| `scripts/ppa_selection.py` | 共享选择算法（BatchLRU / select / 映射加载），纯标准库 |
| `scripts/dsa_hook/dsa_dump_hook.py` | 在线 hook（dump + override），部署到 vllm-ascend 容器 |
| `scripts/dsa_hook/sfa_v1_top2400.patch` | vllm-ascend v0.18.0 sfa_v1.py 最小补丁（三处插入） |
| `scripts/export_ppa_candidates.py` | dump → candidates.jsonl 导出（第一/二代自适应） |
| `scripts/mqsim_ppa_oracle.py` | MQSim 离线时延回放（baseline / ppa_aware / applied） |
| `scripts/quality/run_quality_collection.py` | 在线采集 runner（marker 轮转 + logprobs + 断点续跑） |
| `scripts/quality/analyze_quality.py` | A/B 对比分析（准确率 / 一致率 / 分歧 / logprob 差） |
| `scripts/quality/reconcile_swaps.py` | 错题归因（gold token ↔ swaps 对账） |
| `scripts/quality/convert_datasets.py` | LongBench v2 / RULER / InfiniteBench → 归一化格式 |
| `scripts/quality/make_passkey_dataset.py` | passkey/NIAH 生成器（可配长度和深度） |
| `scripts/quality/sample_dataset.py` | 分层抽样 / 分批切分 |
