# UMM + bmpclient 完整项目包

## 目录
- `README.md`     —— **端到端搭建指南（先读这个）**
- `umm/`          UMM C 基座（含构建产物 build/libumm.so、libumm.a，bin/ 服务端与测试）
- `bmpclient/`    Python client 层（并发读写引擎 + e2e 场景 + docs/ 架构文档）
- `bmpclient/docs/` 架构文档：01 bmpclient层 / 02 umm层 / 03 管控面 / 04 数据面 /
                    R6 并发遗留问题 / 真机风险分析与测试流程

## 当前功能基线（umm 5c30ddb / bmpclient 4b00e6f）
1. P0 锁收窄：local transport memcpy 移出锁外，多线程真并行
2. SSD 数据面双路径：文件模拟（mmap+memcpy）/ libnvm 用户态驱动（dlopen 软依赖）
3. libnvm 窗口化路径：`libnvm:<ctrl>[@ns][+<base_off>]`，机制性避开 LBA0
4. Phase 3 已接线：RPC 模式 SSD tier 端到端（tier_aware + enable_ssd）
5. Phase 4 进行中：
   - 已修复 mem_service 全局锁贯穿 I/O（真机并发 0.88x 根因之一）
   - libnvm ctx 池 + 每 ctx 互斥锁 + 开机只读探针（多 init 自损防护）
   - **真机固定 UMM_LIBNVM_CTXS=1**（libnvm_host 同进程多 init 会破坏 ctx）
   - 遗留 R6：单队列并发提交安全性 + 单 ctx 多队列路线（见 docs/R6）
6. Makefile：`shared` 目标（libumm.so）纳入默认构建；make test 桩库隔离

## 真机验证进度（华为 A3 / CANN 8.5 / libnvm_host 真库）
- Phase 1 冒烟 / Phase 2 受控写入 / Phase 3 服务集成：全部 PASS
- Phase 4 并发：锁修复+探针就绪，待复测与多队列 API（docs/R6 §5）

## 快速开始
见 `README.md`（端到端搭建指南：编译 → 单测 → mock → 文件后端 → 真机四阶段）。

## 已知遗留
- R6：libnvm 单队列并发/多队列支持（docs/R6，含给同事的 API 草案）
- VirtualMedia 依赖 `umm_alloc_on_device`（libumm 未导出，上游 API 漂移，
  13 项单测失败为基线既有问题）
- 建议同事在 nvm_host_disk_info_t 补 NSZE 总容量字段
