# MQSim PPA 映射与候选替换问答


## 问题一：MQSim 模拟器里 PPA 的映射是怎么做的？

**回答：分两步。先根据逻辑页号选择 channel/chip/die/plane，再在目标 plane 内分配空闲 block/page，最后记录到映射表。**

PPA 是 Physical Page Address，即物理页地址。我们使用的 MQSim 版本为 `51f0f2d3fed92d88ef4a0fa61a38024b07bf9d16`，当前实验采用页级 FTL 和 `CWDP` 分配方式。

### 1. 将主机地址换算为逻辑页号

当前主机 LBA 单位为 512 B，NAND page 为 16 KiB。对某个字节偏移：

```text
LBA          = 字节偏移 // 512
逻辑页号 LPA = 字节偏移 // 16384
页内偏移     = 字节偏移 % 16384
```

LPA 是 Logical Page Address，表示逻辑页。MQSim 源码中也使用 `lpn` 命名表示逻辑页号。

本实验每个 KV 槽位为 4096 B。因此，对齐且连续的四个相邻 KV 槽位属于同一个逻辑页，并对应同一个物理页；它们在页内的偏移不同。

### 2. 根据 LPA 选择盘内资源

`CWDP` 表示按 Channel → Chip → Die → Plane 的顺序展开逻辑页号，其中 W 对应 chip/way 维度。假设该流可用的各级编号从 0 连续排列，记：

- C：channel 数量。
- W：每 channel 的 chip 数量。
- D：每 chip 的 die 数量。
- P：每 die 的 plane 数量。

选择公式为：

```text
channel = LPA % C
chip    = (LPA // C) % W
die     = (LPA // (C × W)) % D
plane   = (LPA // (C × W × D)) % P
```

实际源码通过流的 `Channel_ids`、`Chip_ids`、`Die_ids`、`Plane_ids` 数组取得资源编号；若流只分配到部分资源，公式算出的是这些数组的下标。

当前参考 SSD 配置为 4 channel、每 channel 1 chip、每 chip 2 die、每 die 2 plane，分配示例如下：

| LPA | Channel | Chip | Die | Plane |
|---|---|---|---|---|
| 0～3 | 依次 0～3 | 0 | 0 | 0 |
| 4～7 | 依次 0～3 | 0 | 1 | 0 |
| 8～11 | 依次 0～3 | 0 | 0 | 1 |
| 12～15 | 依次 0～3 | 0 | 1 | 1 |
| 16～19 | 依次 0～3 | 0 | 0 | 0 |

也就是先轮转 channel，再轮转 die，最后轮转 plane，16 个逻辑页覆盖一轮资源组合。LPA 0 和 16 会选到相同的 channel/chip/die/plane，但完整物理页地址还要看下一步的分配。

### 3. 在选中的 plane 内分配 block/page

每个 plane 为写入流维护当前写入块（write frontier）：

```text
选中 plane
  → 找到该流的当前写入块
  → 分配该块的下一个空闲页
  → 页号递增
  → 块写满后，换一个空闲块
```

因此，channel/chip/die/plane 主要由分配规则决定，具体 block/page 取决于写入历史和空闲块状态。不能仅根据上面的取模公式确定完整 PPA。

### 4. 保存映射，读取时查表

MQSim 将分配的完整物理位置编码成 PPA，并记录：

```text
映射表[LPA] = PPA
```

读请求通过映射表找到物理位置。覆盖写会分配新页、使旧页失效；GC 搬移也可能更新映射，所以完整 PPA 可以随设备状态变化。

当前实验在 snapshot 全部写入完成后，通过 FTL 的只读映射访问器导出：

```text
LPA → PPA → channel / chip / die / plane / block / page
```

`Ideal_Mapping_Table=true` 表示查询映射无需额外读盘，不表示 LPA 等于 PPA，也不表示能够任意指定物理位置。我们的补丁查询已写入页面的映射，不调用自动为未映射读地址创建物理页的路径。

**我们能够知道模拟 PPA，是因为写入由 MQSim 分配位置，再从它的映射表查询结果。这是模拟器自己的映射，不是实际硬盘映射的还原。**

### 对应代码

- [资源选择及地址映射](../../.deps/MQSim/src/ssd/Address_Mapping_Unit_Page_Level.cpp)：`allocate_plane_for_user_write()`、`allocate_page_in_plane_for_user_write()`、`Convert_address_to_ppa()`。
- [块页分配](../../.deps/MQSim/src/ssd/Flash_Block_Manager.cpp)：`Allocate_block_and_page_in_plane_for_user_write()`。
- [写完后导出 PPA](../../.deps/MQSim/src/host/IO_Flow_Trace_Based.cpp)：补丁新增的 `dsa_dump_mapping()`。
- [可复现补丁](../bench/mqsim/oracle.patch)与[构建脚本](../../scripts/setup_mqsim_oracle.py)。上面的 `.deps` 源码链接需要先运行构建脚本才存在；源码副本不随本项目提交。

## 问题二：候选替换策略是怎么做的？

**回答：各组使用相同的 baseline LRU 缓存状态，从原生 Top-2048 的 Miss 项中做替换，只允许加入 Top-2400 额外候选中同样 Miss 的项，让实际 SSD 读取更均衡。最终选中总数和每步 Miss 数均不变。**

### 1. 查询每个候选的模拟 PPA

根据候选 KV 的 snapshot 字节偏移查到 LPA，再查询写入后导出的 PPA，得到 channel、chip、die、plane。

### 2. 统计原生选择的资源负载

先按每层 baseline LRU 筛选实际需要读盘的 Miss，再统计它们在每个 channel、chip、die、plane 上对应的读取请求数。资源按完整路径区分，例如：

```text
channel 键 = (channel)
chip 键    = (channel, chip)
die 键     = (channel, chip, die)
plane 键   = (channel, chip, die, plane)
```

不同 die 下相同编号的 plane 不会混在一起统计。

默认使用每层 4096/8192 行的冷启动 BatchLRU，前 100 步更新和读取但排除出测量。每一步各组使用相同的 baseline 缓存，原生命中项及当前 token 的 KV 直接可用，不计入盘侧负载。

缓存始终由原生 Top-2048 更新，替换后的选择不影响下一步缓存。原型早期的无 HBM 对照仍可通过 `--capacities 0` 显式运行，不是当前正式实验口径。

### 3. 尝试一对一替换

每次从原生集合移除一个允许替换的 Miss，再加入一个原生集合之外、有参考分数且同样不在 buffer 中的候选。例如，暂只看 plane 负载：

```text
原生选择中的 Miss：A、B、C、D
所在位置：plane 0、0、0、1
额外候选：E 位于 plane 2，且也不在 baseline buffer 中

尝试 C → E
请求分布：3、1、0 → 2、1、1
读取数量：仍为 4
```

示例中只展示 plane 分布；实际算法同时计算全部资源层级。

### 4. 只接受集中程度降低的交换

当前使用静态平方和指标：

```text
拥挤指标 = 各 channel 请求数²之和
         + 各 chip 请求数²之和
         + 各 die 请求数²之和
         + 各 plane 请求数²之和
```

上面仅看 plane 的例子，平方和从 `3² + 1² + 0² = 10` 降到 `2² + 1² + 1² = 6`。

算法只接受完整指标严格下降的交换，并优先选择下降最多的一对。完整指标下降相同时，优先选择参考分数损失较小的一对，再按 token ID 确定性地打破平局。

计算交换影响时，移除项和加入项共享的资源会相互抵消。例如在同一个 channel 内换 plane，不会改变该 channel 的请求数。

### 5. 用贪心搜索控制计算量

每轮按完整 plane 路径分组：

- 每个可移除组保留参考分数最低的原生项作为代表。
- 每个可加入组保留参考分数最高的额外候选作为代表。
- 按移除收益选前 16 个代表，按加入代价选前 16 个代表。
- 枚举最多 16 × 16 个交换对，选一个指标严格下降的最佳交换。

交换后更新选中集合和负载，再进行下一轮。达到预算或当前搜索范围内没有改善时停止。已加入的额外候选不会在后续轮次被重新移除。

例如 `--budgets 128` 表示最多交换 128 对，不代表必须交换 128 对；最终仍选 2048 项。它属于静态贪心启发式，不保证得到全局最优选择。

### 6. 保护规则和参考分数

- 原生集合中命中 baseline buffer 的所有项始终保护。
- 当前 token 始终保护，保持它是否被原生选中的状态。
- 缺失参考分数的原生项始终保护。
- `--protect-top N` 额外保护参考排名前 N 的原生项。
- 新加入的项必须是有参考分数的额外候选，且在当步 baseline buffer 中同样 Miss；已驻留的额外候选也不能加入。
- 当前未设置严格的总分数损失上限；通过替换预算和保护项控制偏移，并单独记录参考分数损失。

参考分数损失是被移除项与加入项的参考分数之差的累加，不能代替模型质量评估。

### 7. 通过 MQSim 判断实际读取收益

拥挤指标只用于选择，是否真的更快由 MQSim 的 NVMe/FTL/NAND 时序决定。不同资源仍可能共享通道和 die，不能认为覆盖更多 plane 就必然更快。

实验设置三组对照：

| 策略 | 方法 |
|---|---|
| baseline | 保留原生 Top-2048 |
| ppa_aware | 按上述静态指标进行贪心替换 |
| random_matched | 每层、每步随机替换与 ppa_aware 实际相同数量的项，遵守相同保护规则 |

随机组匹配实际替换数量，不匹配参考分数损失，同样只允许 Miss 换 Miss。各组从同一 snapshot 写入序列开始，并逐步核对 baseline 缓存状态（含 LRU 顺序）、原生命中项和原生 Miss 集合的哈希；映射、每步主机读取量和 NAND 读事务数量保持一致。零 Miss 层不发读请求，但保留在逐层结果中，时延为 0。

因此同一容量下，各组 `cache_hit_rate` 和 `available_hit_rate` 相同，比较的是相同读量的物理位置变化。共享 baseline 是反事实对照，维持未被替换策略选中的原生项的额外缓存填充成本不计入结果。

当前同页的多个 4 KiB 请求保留为独立读请求，不做主机合并、不启用数据缓存；指标按请求计数，而不是按不同物理页计数。不同物理页数单独记录。

**当前策略同时改善 channel/chip/die/plane 分布，收益不能单独归因为 plane 冲突减少。模拟读取时间也不包含 Python 选择开销，选择耗时单独记录；模型质量和端到端收益尚需进一步验证。**

### 对应代码

- [候选替换与回放](../../scripts/mqsim_ppa_oracle.py)：`resources()` 构造资源路径，`pressure()` 统计负载，`objective()` 计算平方和，`select()` 执行 Miss 换 Miss 的贪心替换，`baseline_states()` 按原生选择生成公共 LRU 轨迹，`selections_with_baseline()` 应用筛选并审计不变量，`main()` 生成对照并运行 MQSim。
- [完整候选导出](../../scripts/export_ppa_candidates.py)：保留原生集合、参考候选与分数、当前 token 及 snapshot 字节偏移。
