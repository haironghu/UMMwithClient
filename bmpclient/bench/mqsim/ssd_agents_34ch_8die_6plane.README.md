# AGENTS.md 硬件拓扑近似配置

配置文件：`ssd_agents_34ch_8die_6plane.xml`。

已用当前补丁版 MQSim 完成小规模读写验证：写入一个 26,640,384 B 的 snapshot，导出 1626 个 NAND 页映射，覆盖 34 个 channel、8 个 die ID、6 个 plane ID；随后完成 14 条 4 KiB 读取，未出现 GC、映射或擦除事务。这只验证配置可运行，不是硬件时序或六 plane 独立并行行为的校准。

这是一份可运行的单 SSD 读路径实验配置，已填入 AGENTS.md 中明确的拓扑参数。它不是完整实测校准模型，不能把模拟时延当成真实设备预测。

后续用户纠正优先于 AGENTS.md 的旧描述：硬件术语为 `channel → die → chip → plane`，34 channel、每 channel 8 die、每 die 1 chip、每 chip 6 plane。读写组织中的 Block 有 6408 个 16 KiB Page，该 Block 不属于所述物理层级。它与 MQSim 物理 NAND 擦除块的对应关系尚未确认。

用户已决定其余参数采用本文件的实验默认值，可以直接运行，无需等待补充硬件参数。这些默认值用于固定实验条件，不代表真实硬件已经校准。

| 参数 | 配置值 | 来源或限制 |
|---|---:|---|
| Flash_Channel_Count | 34 | AGENTS.md |
| Chip_No_Per_Channel | 1 | MQSim 拓扑适配占位，不代表用户最新术语中的 chip 数 |
| Die_No_Per_Chip | 8 | 与上一项相乘，保持每 channel 8 个 die；物理层级适配待确认 |
| Plane_No_Per_Die | 6 | AGENTS.md；并行命令及独立 cache 行为仍需验证 |
| Page_Capacity | 16384 B | AGENTS.md |
| Page_No_Per_Block | 128 | 模拟容量占位值，不是硬件规格 |
| Block_No_Per_Plane | 32 | 模拟容量占位值，不是硬件规格 |
| Plane_Allocation_Scheme | CWDP | 本实验排布假设，不代表真实 FTL |
| PCIe_Lane_Count / PCIe_Lane_Bandwidth | 4 / 1 GB/s | 固定版本上游默认值，尚未校准 |
| Flash_Channel_Width / Channel_Transfer_Rate | 1 B / 333 MT/s | 固定版本上游默认值，尚未校准 |
| Page_Read_Latency_LSB/CSB/MSB | 各 75000 ns | 固定版本上游默认值，尚未校准 |
| Page_Program_Latency_LSB/CSB/MSB | 各 750000 ns | 固定版本上游默认值，尚未校准 |
| Block_Erase_Latency | 3800000 ns | 固定版本上游默认值，尚未校准 |
| Flash_Technology | TLC | 用户已确认；各页类型读时延仍暂用 75 μs，尚未校准 |

其余编程、擦除、DRAM 时序等参数也保留固定版本上游值。这个配置的作用是先对齐明确的拓扑数量，再逐步校准时序。

## 尚未精确表达的规格

- 实际有效 die 为 271，plane 为 1626。当前 MQSim 使用规则化拓扑，因此配置为 272 die、1632 plane，没有屏蔽坏 die。不能通过在 workload 中删掉一个 die ID 实现精确屏蔽，因为该过滤会作用于所有 channel。
- 用户已纠正：读写组织中的每个 Block 有 6408 个 Page，每 Page 16 KiB。一个这样的 Block 为 104,988,672 B（100.125 MiB）。由于用户说明它不是物理结构，不能直接填入 MQSim 的物理 `Page_No_Per_Block`；需先确认它与物理擦除块的对应关系。当前每 plane 为 4096 个模拟 page，block 划分只是避免 GC 的实验容量设置。
- 六 plane 的数量不等于已实现真实设备任意地址的六路独立读、cache 和 multi-plane 命令限制。当前文件只调整数量，没有修改 NAND 命令模型。
- 用户最新术语是 `channel → die → chip → plane`，不同于 MQSim 使用的 `channel → chip → die → plane`。目前适配保留 34 × 8 × 6 的资源数量，但不能据此宣称 chip 层级、共享关系和命令并行行为已经匹配。

物理模拟容量为 109,521,666,048 B（102 GiB），与真实整盘容量无关。运行器要求 snapshot 不超过其 60%；若容量检查失败，可增加 `Block_No_Per_Plane`，并留意模拟器内存开销。

## 使用

在仓库根目录执行：

```bash
python3 -u scripts/mqsim_ppa_oracle.py \
  --candidates "$HOME/ppa_candidates_all" \
  --out-dir "$HOME/ppa_agents_topology_results" \
  --ssd-config bmpclient/bench/mqsim/ssd_agents_34ch_8die_6plane.xml \
  --capacities 4096 8192 \
  --budgets 0 32 64 128 \
  --qd 16 \
  --warmup-steps 100
```

短样本需要将 warmup 改为 0。此配置无需重编译 MQSim，但改变几何后必须重新运行实验，不能复用旧几何下的 PPA 映射。运行目录中的 `ssd.xml` 保存最终生效参数。

每组仍共享原生 baseline LRU 状态，只允许 Miss → 额外候选 Miss，保持读取量一致。快照全部写完后才回放读取；没有并发 decode 写入。SSD 数据缓存关闭，使用理想映射表、关闭预处理和磨损均衡、使用 OUT_OF_ORDER 调度。这些控制条件继续沿用运行器设置。

用户已接受按 272 个 die 模拟，并采用 MQSim 原生 multi-plane 调度和 NAND 执行模型，不增加 plane 异步独立读功能。NAND 类型已确认为 TLC；其余参数按上述默认值固定。LSB/CSB/MSB 的读时延各为 75000 ns，不能由 TLC 类型直接推断真实时延。未来若需预测真实设备性能，再替换物理 block/page 数量、NAND 时序、通道和 PCIe 参数，并进行实测校准。

运行参数继续使用 HBM 容量 4096/8192、预算 0/32/64/128、主机在途请求上限 QD=16、warmup=100 步。XML 的 `IO_Queue_Depth=65535` 是接口队列容量，不会覆盖运行器的 16 请求并发限制。
