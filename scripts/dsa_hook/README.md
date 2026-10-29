# dsa_dump_hook：vllm-ascend 0.18 Top-2400 参考候选采集插桩

重建此前丢失的 DSA trace 采集插桩。每个 decode step、每层同时落盘两组数据：

1. **native indices**：融合 `npu_lightning_indexer` 实际选出并喂给
   `npu_sparse_flash_attention` 的 top-2048；
2. **reference candidates**：用 FP32 独立重算 indexer 分数后取 top-2400。
   公式按 CANN Lightning Indexer 文档：
   `I_j = Σ_h w_h · ReLU(q_h · k_j)`，其中 q/k 为 RoPE 之后的 indexer
   向量（stash 点在 `indexer_select_post_process` 的 rope 之后、sparse-c8
   量化之前，与融合内核看到的输入完全一致），w 为 `weights_proj` 输出。

输出 schema 与 `scripts/simulate_hbm_candidates.py::load_record` 和
`scripts/export_ppa_candidates.py` 完全兼容：

```
<DSA_DUMP_DIR>/<layer_name>_s<seq_len>.pt
  seq_lens_key=[S], seq_lens_query=[1], indices（native，升序）,
  candidate_indices, candidate_scores（FP32，降序）,
  has_reference_candidates=True, layer=<layer_name>
manifest.jsonl   每次落盘一行（layer/seq/native 数/重叠数/耗时）
_summary.json    进程退出时的计数与配置
```

## 1. 前置校验

插桩补丁针对 vllm-ascend **v0.18.0 官方** `vllm_ascend/attention/sfa_v1.py`
（SHA256 `63849e10e82f8bee4a9f0fb21ab0d606852cd6cf86a0a0fb983c8187980189a7`）。
在推理容器内定位并核对：

```bash
SFA=$(python -c "import vllm_ascend.attention.sfa_v1 as m; print(m.__file__)")
sha256sum "$SFA"      # 不一致则按第 2 节手工插入三段代码
```

## 2. 安装

```bash
cd /path/to/UMMwithClient/scripts/dsa_hook
cp "$SFA" "$SFA.orig.bak"                       # 务必备份
git apply sfa_v1_top2400.patch                   # 在 sfa_v1.py 所在仓库根目录执行
cp dsa_dump_hook.py "$(dirname "$SFA")/"         # hook 模块放进 attention 包
cp ../ppa_selection.py "$(dirname "$SFA")/"      # 仅 override 模式需要（第 4 节）
```

补丁只有三处插入（import 守卫、post_process 内 stash、forward 内 dump），
文件不匹配时手工插入同样内容即可。`dsa_dump_hook.py` 缺失时 sfa_v1 完全
正常（import 走 except 分支），插桩零生效。

## 3. 环境变量

| 变量 | 缺省 | 说明 |
|---|---|---|
| `DSA_DUMP_ENABLE` | 未设 | 必须显式 `=1` 才启用 |
| `DSA_DUMP_DIR` | 无 | 落盘目录（建议每轮采集用新目录） |
| `DSA_DUMP_RANK` | 0 | 只有该 TP rank 写盘；其余 rank 零开销 |
| `DSA_DUMP_REF_TOPK` | 2400 | 参考候选数 |
| `DSA_DUMP_NATIVE_TOPK` | 2048 | 仅用于校验融合 top-k 数量 |
| `DSA_DUMP_MIN_SEQ` | 2049 | 小于该上下文不落盘（兼滤掉 dummy/profile 步） |
| `DSA_DUMP_MAX_STEPS_PER_LAYER` | 0（不限） | 控制落盘文件数 |
| `DSA_DUMP_INDEXER_RELU` | 1 | 公式中 ReLU 开关（0=纯点积加权），对照实验用 |
| `DSA_DUMP_SCALE` | 1.0 | 分数均匀缩放，不改变排序 |
| `DSA_DUMP_LOG_EVERY` | 100 | 计数日志间隔 |

## 4. 在线 override（第二代 trace）

`DSA_OVERRIDE_ENABLE=1` 时，hook 在落盘前额外做**在线 PPA-aware 替换**：
用与离线 oracle 完全相同的共享模块 `ppa_selection.py`（**必须一并拷进容器**，
放在 `dsa_dump_hook.py` 同目录）跑 `select()`，把替换后的索引张量返回给
sfa_v1（补丁第 3 处已改为赋值返回值），并把这些字段追加进每条 dump：
`selected / swaps / overridden / cache_mode / cache_capacity /
cache_state_sha256 / layer_rank / stride_tokens`。缓存语义为 shared-baseline
（BatchLRU 只用原生 Top-2048 推进）；`budget=0` 时集合不变、返回原张量，
输出与原生运行逐位一致——这是插桩惰性对照（R0）。

**TP 语义（重要）**：DSA 的注意力集合是全头共享的，因此替换必须在**所有
TP rank 上得到完全相同的集合**。复制式 indexer 保证了各 rank 输入位一致，
`select()` 是确定性纯函数，所以 hook 在 override 模式下让每个 rank 都执行
同一套计算（包括 FP32 参考重算——这会显著拖慢生成，属实验预期）；落盘
I/O 仍只在 `DSA_DUMP_RANK`。每层每步还通过 all_reduce 比对各 rank 选中集
的稳定摘要，不一致立即抛错拒绝继续（`DSA_OVERRIDE_VERIFY_RANKS=0` 可关，
不建议）。`DSA_OVERRIDE_MAPPING` 指向的文件必须对所有 rank 进程可读
（同一容器/共享挂载即可）。

| 变量 | 缺省 | 说明 |
|---|---|---|
| `DSA_OVERRIDE_ENABLE` | 未设 | `=1` 启用（隐含要求 DSA_DUMP_ENABLE） |
| `DSA_OVERRIDE_MAPPING` | 无 | prepare 产物 `ppa.csv` 路径 |
| `DSA_OVERRIDE_STRIDE_TOKENS` | 无 | 声明式布局 v2 的每层 stride（token 数），须与 prepare 导出一致 |
| `DSA_OVERRIDE_CAPACITY` | 4096 | 每层 LRU 行数（≥2048） |
| `DSA_OVERRIDE_BUDGET` | 128 | 每步每层最多替换数（0–352） |
| `DSA_OVERRIDE_PROTECT_TOP` | 0 | 参考排名前 N 保护 |
| `DSA_OVERRIDE_LAYER_RANKS` | 无 | 层号→rank 的 JSON；缺省 rank=数字层号（78 层连续时正确） |
| `DSA_OVERRIDE_VERIFY_RANKS` | 1 | 每步 all_reduce 校验各 rank 选中集一致，不一致即抛错 |

完整闭环流程（质量与带宽同轨迹会合）：

```bash
# ① prepare：任一同层集 dump（如原 first-gen 样本）导出声明式布局并只跑写入
python3 scripts/export_ppa_candidates.py "$SAMPLE" --out-dir "$HOME/ppa_prep" \
    --declared-stride-tokens 120000
python3 scripts/mqsim_ppa_oracle.py --candidates "$HOME/ppa_prep" \
    --out-dir "$HOME/ppa_prepare_run" --prepare-only        # 产物 prepare/ppa.csv

# ② 在线：容器内启用 override，映射指向 ① 的 ppa.csv，stride 与 ① 相同
export DSA_OVERRIDE_ENABLE=1 DSA_OVERRIDE_MAPPING=.../ppa.csv \
       DSA_OVERRIDE_STRIDE_TOKENS=120000 DSA_OVERRIDE_BUDGET=128

# ③ 第二代导出（自动识别 stride/applied 字段并校验一致性）
python3 scripts/export_ppa_candidates.py "$DUMP2" --out-dir "$HOME/ppa_second"

# ④ 同轨迹时延回放：applied 组 = 在线实际应用的选择；baseline 组 = 同一条
#    分叉轨迹上的原生 Miss（反事实）。映射 sha、逐步 LRU 摘要自动交叉校验。
python3 scripts/mqsim_ppa_oracle.py --candidates "$HOME/ppa_second" \
    --out-dir "$HOME/ppa_second_results" --applied --capacities 4096 --qd 16
```

护栏：某步替换回退（`overridden=false`）会让第二代导出直接报错，拒绝混合
轨迹；`prepare` 映射 sha 与 hook 实际用的一致性在 ④ 自动核对；在线 LRU
与回放 LRU 的逐层摘要哈希逐步核对，不一致即中止。

## 5. 采集运行（仅 dump 模式）

沿用 `scripts/start_glm51_w4a8_trace.sh` 的启动方式，另加导出：

```bash
export DSA_DUMP_ENABLE=1
export DSA_DUMP_DIR=$HOME/dsa_dump_dir/sample_xxx
bash scripts/start_glm51_w4a8_trace.sh
```

口径要求（与离线管线一致）：**单请求 decode**（`--max-num-seqs 1` 或只发
一个请求）、greedy、`--enforce-eager`（启动脚本默认已带，保证 decode 批
无 padding）。多请求 batch、prefill、DSA-CP、sparse-c8 路径会被跳过并
计入 `_summary.json` 计数。dump rank 每层每步同步两次，采集时生成速度
明显变慢属预期。看到 `[IndexerTrace] hook ready` 只代表初始化，不代表
落盘成功——以第 5 节校验为准。

## 6. 采集后校验

```bash
# 容器内即可跑（CPU torch）：
python3 /path/to/UMMwithClient/scripts/dsa_hook/check_dumps.py "$DSA_DUMP_DIR"
```

通过（exit 0）后拷回工作机，走既有管线：

```bash
python3 -u scripts/simulate_hbm_candidates.py "$SAMPLE_DIR" --expected-layers 78 ...
python3 -u scripts/export_ppa_candidates.py "$SAMPLE_DIR" --out-dir ...
```

重点看 check_dumps 输出的 `mean_native_in_reference`：FP32 参考与融合
内核排名本就允许少量分歧（limitations 中已声明），但均值明显低于
native 的 90% 时，先核对公式假设（`DSA_DUMP_INDEXER_RELU`）再使用数据。

## 7. 边界与假设

- 仅支持普通 TP 部署的**复制式 indexer**（每 rank 全部 64 头；这正是各
  rank top-k 一致的前提）。q_li 头数与 `indexer.n_head` 不符会告警并跳过。
- indexer k cache 按 `[num_blocks, block_size, 1, 128]` 分页布局 gather；
  形状不符会告警跳过（不写错误数据）。
- 参考分数是 FP32 累加，输入与融合内核相同；分歧来源仅为累加精度与
  topk 平局处理，正是离线管线 `native_covered` 指标要统计的对象。
- 当前 token 的 KV 在 scatter 之后、top-k 之前已写入 k cache，参考分数
  覆盖 `[0, S)` 全体，与内核视角一致。
- 只读不写模型路径；落盘为原子替换；进程退出写 `_summary.json`。
