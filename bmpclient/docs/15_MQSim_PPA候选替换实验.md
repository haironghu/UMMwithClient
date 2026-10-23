# MQSim 模拟 PPA 感知的 Top-2400 候选替换

目标：在相同主机读取量、相同 NAND 读事务数量和相同初始映射下，验证候选替换能否缓解盘内资源竞争、降低每层整批 KV 加载时间。物理位置来自 MQSim 自己的 FTL，不是实际 SSD 的 PBA/PPA；当前算法利用准确位置，但不保证全局最优。

本实现只访问本机普通文件和模拟器，不访问 `/dev/nvme*`，无需 A800、CUDA、NPU、root。读取实际 `.pt` 需要 CPU 可用的 PyTorch；便携 JSON fixture 不需要 PyTorch。

## 已实现的范围

- 官方 MQSim 固定到 `51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16`，补丁在 `bmpclient/bench/mqsim/oracle.patch`。
- 使用原有 NVMe、主机 PCIe、FTL、TSU、PHY、NAND 离散事件路径；未用 Python 简化时延公式替代 MQSim。
- 仅单 SSD；位置哈希在单盘退化为盘 0，KV 按原始层号、token 顺序占用连续 4096 B 槽位；层间不补齐，整个 request 尾部补齐到 Superpage。这与 ordered 单盘 snapshot 的地址规则一致。
- 每个 Superpage 发一条真实模拟 NVMe 写命令，等待所有写请求完成后才导出映射或发读。FTL 将大写命令拆分到 NAND 页。
- 页级 FTL、理想映射表；关闭 SSD 数据缓存、磨损均衡和预处理。snapshot 不超过物理容量 60%，并校验没有 GC/映射/擦除事务；不是通过修改 GC 算法消除代价。
- 最后导出上下文的全部 KV 先准备好，再回放各层；固定 snapshot、固定原模型候选轨迹，没有 decode 在线写入。
- 不模拟历史 HBM 缓存；当前 token 直接可用，原生集合是否选中它保持不变，它不参与替换、不发 SSD 读。
- 每层等全部 NVMe 读完成后才推进到下一层；每步按原始层号递增执行。
- `--qd 16` 是在途 NVMe 请求数上限，不是 16 个 CPU 线程，不等同于原八盘实测配置。

## 1. 获取并编译

从 UMMwithClient 仓库根目录运行。依赖：Git、GNU make、支持 C++11 的 g++、Python 3.10+。

```bash
python3 scripts/setup_mqsim_oracle.py --jobs 4
```

默认下载到 `.deps/MQSim`，不将源码副本和二进制纳入本项目 Git。已有独立的该版本 checkout 可指定 `--source /path/to/MQSim`。脚本只接受干净官方源码或正好应用本补丁的源码，不会重置其他改动。

每次编译前执行 `make clean`：上游 Makefile 没有头文件依赖跟踪，修改类布局后增量构建可能导致堆损坏。构建标记记录官方版本、补丁和二进制 SHA256，实验启动时检查一致性。需要更新补丁时使用新的干净 checkout，避免覆盖已有修改。

## 2. 导出真实候选

输入原始 Top-2048 + Top-2400 dump，不用此前只包含 Miss 的 JSONL。原生索引缺失于参考候选的项仍保留，其分数设为 null，选择时保护。

```bash
python3 -u scripts/export_ppa_candidates.py /home/h00938526 \
  --out-dir /home/h00938526/ppa_candidates_l27_s8 \
  --layers 27 --max-steps 8 --expected-layers 78
```

输入应为一个 sample 的直接目录。`--expected-layers` 检查原始目录总层数，`--layers` 再选择子集；省略 `--layers/--max-steps` 才是全量。建议先一层、8 步，避免一上来处理 78 层全部轨迹。若要验证 100k 上下文，需要对应的 100k 原始 dump，增加 snapshot 大小不会制造新的模型候选轨迹。

默认段大小 26,640,384 B，NAND page 固定 16 KiB，KV slot 固定 4096 B。SQLite 暂存排序，输出按 step、原始 layer 数值顺序排列。数据目录必须不存在。manifest 只有全部成功才标记 complete，并保存 candidates.jsonl 的 SHA256。

候选记录包括：sample/layer/step/context/current_token、native 集合、所有参考候选及分数、device=0、offset_bytes。地址按整个 snapshot 计算，不按候选子集重新编号。

## 3. 执行模拟

```bash
python3 -u scripts/mqsim_ppa_oracle.py \
  --candidates /home/h00938526/ppa_candidates_l27_s8 \
  --out-dir /home/h00938526/ppa_oracle_l27_s8 \
  --budgets 0 32 64 128 --qd 16
```

输出目录必须不存在。默认 warmup=0；若导出 200 步并希望排除前 100 步，使用 `--warmup-steps 100`，被排除的步骤仍实际执行。`--protect-top N` 保护参考排名前 N 项；分数缺失的原生项及当前 token 始终保护。

默认使用明确的参考 SSD：4 channel、每 channel 1 chip、每 chip 2 die、每 die 2 plane、128 page/block、16 KiB/page、CWDP 分配。block 数量按 snapshot 自动扩充，保留容量余量。时延和接口参数来自固定版本官方配置，因此**不是用户真实 SSD 的校准模型**。最终生成的 `ssd.xml` 是完整有效参数。

可用 `--ssd-config /path/to/ssd.xml` 提供其他几何和时延参数；本阶段仍强制理想页映射、关闭预处理和磨损均衡、使用 OUT_OF_ORDER，必须为 NVMe 和 16 KiB page。快照大于 60% 物理容量时拒绝运行。非均匀的 271 个有效 die、六 plane 的具体命令并行约束尚未适配或验证；不要直接宣称当前 preset 模拟了那块硬盘。

## 4. 三组选择方法

1. **baseline**：完全保留原生 Top-2048。
2. **ppa_aware**：最多替换 budget 个原生项；额外项必须有参考分数。按真实映射构造 channel/chip/die/plane 的多级前缀键，用各组请求数平方和衡量集中度。贪心尝试移除原生项、加入额外项，仅接受集中度严格降低的交换。同资源组优先移除低分原生项、加入高分候选；每轮考察移除/加入各自排名前 16 个资源组代表项。它是静态启发式，未查询未来时延或重复调用 MQSim 搜索最优解。
3. **random_matched**：每个层/step 随机交换与 ppa_aware **实际相同数量**的项，遵守相同保护规则，用确定性 seed 重现。只匹配替换数量，不匹配分数损失；单独报告参考分数差。

每个策略在独立进程中重放相同 snapshot 写命令，检查 PPA 导出文件完全相同，再执行读取。当前直接向 FTL 的公开只读映射访问器查询条目，不调用“未映射读地址自动分配”路径。

保留最终选中数量、当前 token 是否被选中及历史读取数量。每组按相同 token-ID 排序规则提交读。不合并同页的多个 4 KiB 请求、不开数据缓存；脚本逐批检查每个读请求恰好产生一个用户 NAND 读事务且 payload 为 4096 B。它刻意固定此 I/O 规则以隔离竞争收益，不代表所有真实控制器都会这样处理同页请求。

## 5. 结果与核查

- `summary.json`：status、输入摘要、构建与配置哈希、限制及结果；未完成时保留 running。
- `comparison.csv`：读取时间、加速比、Step P50/P95、请求数、NAND 事务数、参考分数损失、Python 选择时间。
- `prepare/ppa.csv`：所有写完成后从 FTL 导出的 `LPA,PPA,channel,chip,die,plane,block,page`。
- 各策略 `ppa.csv`：与 prepare 的 SHA256 必须相同。
- 各策略 `batches.csv`：batch 0 是准备写，后续每批对应一个层/step。记录首个提交到最后完成的时长、完成请求数；下一批开始晚于上一批结束。
- 各策略 `layers.csv`：时延、静态资源负载峰值、不同物理页数、NAND 事务数及驻留/执行/传输时间总和。静态负载峰值不是测量得到的资源利用率。
- 各策略 `selections.jsonl`：选中集合、具体交换对和选择开销。
- 各策略 `nand.csv`：真实模拟事务的发起/完成时刻、类型、来源、地址、payload 和 PHY 时长。所有读地址再次核对准备好的映射，不能跨层 barrier。
- `mqsim.log` 和 `workload_scenario_1.xml`：保留官方日志和报告。

`read_seconds` 是测量层整批 NVMe 读取时间之和，**不含 Python 候选选择/映射查询开销**；`mean_decision_ms` 单独记录原型 CPU 用时，random 中包含计算匹配替换数量的开销。原型未做实时优化，读取收益不能直接解释为包含决策成本后的净收益。

`nand_read_payload_bytes` 是事务传输 payload，不是 NAND 实际感测字节；`nand_page_sense_bytes_estimate` 为事务数乘 16 KiB 的估计。`mean_nand_residual_us` 是事务驻留减执行和传输的剩余时间，不能等同于精确的某一级排队时间。多 plane 命令可能包含多个事务，因此事务数也不等于命令数。

相同读量是运行时强制检查，不靠事后解释。空间分散但延迟没降甚至变差也是有效结果，应保留；不要根据静态集中度预先宣称收益。参考分数差不能代替模型质量测试。固定候选轨迹仍不包含注意力集合变化对未来模型行为的影响。

当前策略同时改善 channel/chip/die/plane 分布，收益不能单独归因为 plane 冲突减少。若要隔离 plane 层，后续需要约束每个 channel/chip/die 的请求数不变，仅在其内部换 plane。该静态贪心策略没有收益，也不能证明最优 PPA 感知策略不存在收益。

## 6. 便携冒烟测试与回归

没有真实 dump 时可以构造明确标记的合成轨迹：

```bash
python3 scripts/make_ppa_oracle_fixture.py --out-dir /tmp/ppa_fixture
python3 scripts/export_ppa_candidates.py /tmp/ppa_fixture --out-dir /tmp/ppa_export
python3 scripts/mqsim_ppa_oracle.py --candidates /tmp/ppa_export \
  --out-dir /tmp/ppa_results --budgets 0 32 128 --qd 16
```

fixture 故意让原生集合集中在参考配置的 channel 0，额外候选位于其他 channel。只能验证机制及代码链路，不是 GLM 数据、100k 上下文或硬件实测结果。

```bash
MQSIM_SOURCE="$PWD/.deps/MQSim" python3 -m unittest discover -s scripts -p 'test_mqsim_ppa_oracle.py' -v
python3 -m unittest discover -s scripts -p 'test_*hbm*.py' -v
```

包括：完整候选/分数、数字层顺序、snapshot 地址、映射连续性和无别名、未知分数/当前 token 保护、替换预算、资源已均衡不交换；以及真实 MQSim 的等量读取、写后查询、映射一致、层完成屏障、集中/分散地址对比。

后续再扩展八盘同一时钟的多设备协调、固定盘间负载、缓存存在时 Miss 换 Miss、GC/在线写入、映射误差与真实硬件校准。目前没有把这些因素混入首轮机制实验。
