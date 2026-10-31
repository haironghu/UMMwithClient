# 在线质量实验工具（采集 runner 与分析）

围绕 dsa_dump_hook 的采集/override 能力，把质量验证流程串成脚本：
数据准备 → 串行采集（A 原生 / B override）→ A/B 分析 → 错题与 swaps 对账。
全部纯标准库；reconcile_swaps 额外需要本地 tokenizer 与 torch（推理机上都有）。

## 0. 数据格式（normalized JSONL）

```json
{"id":"passkey131072_d30_0003","task":"passkey131072","prompt":"...",
 "reference":["40361"],"gold_text":"40361","max_tokens":32,"match":"contain"}
```

- `id`：会净化成 dump 子目录名，必须唯一（与 dsa_dump_hook 的净化规则一致）。
- `reference`：字符串或可接受答案列表；`match`: `contain`（默认，归一化包含）或 `exact`。
- `gold_text`：**字面出现在 prompt 里**的关键文本（如 passkey 数字）；供 swaps 对账。
  省略时取第一个能在 prompt 中找到的 reference。

## 1. 准备数据

```bash
# 自造 passkey（100k+ 即刻可得；无 tokenizer 时按字符近似）
python3 scripts/quality/make_passkey_dataset.py --out passkey131k.jsonl \
    --count 20 --target-tokens 131072 --tokenizer $MODEL_DIR

# 三个基准的原始文件自行下载后转换（过滤超长样本；服务需在线）
python3 scripts/quality/convert_datasets.py --format longbench-v2 \
    --input longbench_v2.json --out lbv2.jsonl \
    --tokenize-endpoint http://推理机:8088 --model glm-5
# 同法：--format ruler（jsonl） / --format infinitebench（每任务一个 json）
```

注意：转换器的 prompt 包装是自定的——本实验只做 **A/B 同 prompt 对比**，
分数与各基准官方榜单不可比。

## 2. 采集（跑两遍，同数据集）

服务端独占、单请求串行、greedy（runner 固定 temperature=0）。每例自动写
`current_sample` marker，hook 把 trace 路由到 `<DSA_DUMP_DIR>/<id>/`。

```bash
# run A：原生（或服务端 DSA_OVERRIDE_BUDGET=0）
python3 scripts/quality/run_quality_collection.py \
    --dataset passkey131k.jsonl --out-dir runA --label A \
    --endpoint http://推理机:8088/v1 --model glm-5 \
    --dump-root /共享挂载/DSA_DUMP_DIR --max-tokens 32

# run B：服务端切 override 后重启，再跑一次（--label B --out-dir runB）
```

断点续跑：中断后用**同参数**重跑即可（同 label/dataset 自动跳过已完成）；
换 label 复用目录需 `--append-ok`。超长样本经 `/tokenize` 预过滤记为
`filtered`，不占请求数。`manifest.json` 原子更新进度。

## 3. A/B 分析

```bash
python3 scripts/quality/analyze_quality.py \
    --dataset passkey131k.jsonl --results-a runA/results.jsonl \
    --results-b runB/results.jsonl --out-dir analysis
```

输出 `summary.json` + `per_example.csv`：总分/分任务分、A/B 一致率、
首分歧 token 位置分布、**分歧前逐 token logprob 差**（teacher-forced
反事实信号，采集时 `--logprobs 1` 即可）、回归/改善样本清单。

## 4. 错题对账（regression ↔ swaps）

```bash
python3 scripts/quality/reconcile_swaps.py \
    --dataset passkey131k.jsonl --results-a runA/results.jsonl \
    --results-b runB/results.jsonl \
    --dump-root /共享挂载/DSA_DUMP_DIR --tokenizer $MODEL_DIR \
    --out-dir analysis
```

对每个回归样本（A 对 B 错）+ 对照组（都对）：定位 gold 文本在 prompt 中的
token（offset 对齐），逐层逐步统计 gold token 在 native 集合中出现却从
applied 选择中消失/被显式换出的次数。`reconcile.json` 里的
`regressions_stats` vs `controls_stats` 就是"质量下降可否归因于关键 token
被替换"的证据。

## 5. 后接离线管线

每个 `<DSA_DUMP_DIR>/<id>/` 就是一个独立样本目录：逐个跑
`check_dumps.py` 校验、`export_ppa_candidates.py` 导出（第二代字段自动
透传）、`mqsim_ppa_oracle.py --applied` 做同轨迹时延回放。

## 已知边界

- 采集/分析工具不做模型质量与官方榜单的对齐；结论只取 A/B 差值。
- passkey 生成器在无 tokenizer 时按 `--chars-per-token`（默认 4.0）近似，
  中英混合场景建议带 `--tokenizer`。
- reconcile 只处理 gold 文本**字面出现**在 prompt 里的样本（检索类天然
  满足；抽象回答的样本跳过并记录原因）。
- runner 每例两个 HTTP 调用（/tokenize 预过滤 + /completions），弱网下
  失败自动重试一次；服务端 4xx 不重试。
