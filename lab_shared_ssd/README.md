# lab_shared_ssd — QEMU 共享 SSD 双虚拟机实验环境

用 QEMU 模拟两块 SSD 盘、实例化两个虚拟机；两块盘作为**共享存储资源**同时
挂给两个 VM；UMM 服务（ummD + umms）跑在 VM1；两个 VM 各自通过 UMM 申请
SSD 资源区间，然后**在本机盘上直接读写**。

**不做跨节点读写，不做数据搬运**：客户端不设 `peer_nodes`，remote
transport 不挂载（配置门槛，见下），一切 I/O 落在本机看到的同两块盘上。

## 拓扑

```
                 宿主机（qemu 工作目录 lab_shared_ssd/work/）
   ┌───────────────────────────────────────────────────────────────┐
   │  ssd0.raw (8G) ═══════════════╦═══════════════ ssd0.raw       │
   │  ssd1.raw (8G) ═══同一后端文件═╩═══ file.locking=off ═════════  │
   │        ▲ NVMe 模拟                     ▲ NVMe 模拟             │
   │  ┌─────┴───────────┐  socket netdev  ┌┴─────────────────┐     │
   │  │ VM1 umm-vm1     │◄════ L2 直连 ══►│ VM2 umm-vm2      │     │
   │  │ 10.0.0.11       │   10.0.0.0/24   │ 10.0.0.12        │     │
   │  │ · ummD  :20001  │                 │ · UMM 客户端     │     │
   │  │ · umms  :20002  │◄── RPC 申请 ────┤   node-id 1      │     │
   │  │ · UMM 客户端    │                 │ · /dev/nvme[01]n1│     │
   │  │   node-id 0     │                 │   本机盘读写     │     │
   │  │ · /dev/nvme[01]n1                 │                  │     │
   │  │   本机盘读写    │                 │                  │     │
   │  └─ NAT ssh :2221 ─┘                 └─ NAT ssh :2222 ──┘     │
   └───────────────────────────────────────────────────────────────┘
```

- **共享盘**：QEMU NVMe 模拟设备，两个 QEMU 进程以同一 raw 后端文件
  + `file.locking=off` 打开。两 VM 里都是 `/dev/nvme0n1`、`/dev/nvme1n1`
  （serial UMMSSD0/1，启动参数保证顺序一致）。
- **分配权威**：umms（VM1）以 `ssd_devices` 打开两块盘建立 ssd_pool，
  两个 VM 的客户端都向它 RPC 申请/释放 offset 区间 —— 区间集群级唯一。
- **数据面**：客户端用 `ssd_devices` 在本机直接打开这两块盘
  （mmap/pread/pwrite），数据不过网络。
- **管控面**：socket netdev 直连链路（无需 root/网桥/TAP），
  ummD `10.0.0.11:20001`、umms `10.0.0.11:20002`。

## 前提

- Linux 宿主机，QEMU >= 6（NVMe 模拟 + socket netdev），cloud-init 镜像
  制作工具（cloud-localds 或 genisoimage/xorriso），openssh 客户端，curl。
- `/dev/kvm` 可写则自动用 KVM；否则回退 TCG（慢但功能一致）。
- 磁盘建议 >= 16G 可用（镜像全部稀疏，按需增长）。
- 宿主机无需 root。

## 使用步骤（顺序执行）

```bash
cd lab_shared_ssd
./00_check_prereqs.sh     # 依赖检查
./01_make_images.sh       # 下载 Ubuntu 24.04 云镜像；建 2 系统盘 + 2 共享 SSD(8G)
./02_make_seed.sh         # cloud-init：umm 用户 + ssh key + 双网卡网络配置
./03_launch_vms.sh        # 起两个 VM（自动等 cloud-init、自检网卡和共享盘）
./04_provision.sh         # 源码同步到两 VM 并 make；udev 放行 nvme 设备访问
./05_start_services.sh    # VM1 起 ummD + umms（pool=2x8G，token，CIDR 白名单）
./06_run_experiment.sh    # 两 VM 并行跑实验客户端 + 集群级断言
./07_teardown.sh          # 关机；--purge 连镜像一起删
```

`env.sh` 集中所有可调参数（盘容量、IP、端口、token、chunk 大小等），
改完重跑对应步骤即可。

## 实验内容（06 脚本，每个 VM 上 demo_shared_pool.py 执行）

| 阶段 | 内容 | 通过判据 |
|---|---|---|
| S0 | 查 ummD 全局拓扑 | 信息性（不影响数据面） |
| S1 | 申请 2×3G chunk → 1MB piece 流式写确定性 pattern → 读回逐字节校验 + 交叉复查 | 全量字节一致 |
| S2 | 写出结果 JSON（chunk/GPA/offset/size） | 文件生成 |
| S3 | 负路径：超额申请→-3 NO_MEMORY；越界写→-1；重复释放→-1 | 三者均被拒 |

编排侧（宿主）最后做**集群级断言**：两 VM 共 4 个 chunk 的 offset 区间
两两不相交 —— 这是「分配权威唯一」的直接证据。

默认 `EXP_CHUNK_SIZE=3G` 的用意：pool 为 2×8G 的连续虚拟空间，VM2 分到的
`6G→9G` 区间跨越 nvme0n1/nvme1n1 边界，**必然走跨设备分段 I/O 路径**。

## 关键配置约束（踩坑点）

1. **设备顺序/容量三方一致**：`POOL_DEVICES`（默认
   `/dev/nvme0n1:8G,/dev/nvme1n1:8G`）同时用于 umms 和两个客户端，顺序
   必须一致 —— pool 虚拟 offset 空间按此顺序拼接。03 脚本固定 serial
   （UMMSSD0/1）保证两 VM 设备命名一致；05 脚本启动前会校验容量。
2. **块设备显式放行**：UMM 默认拒绝打开块设备（防误写系统盘）。实验里
   umms 与所有客户端都需要 `UMM_ALLOW_BLOCK_DEVICE=1`（05/06 已内置）。
3. **不要分区/格式化/挂载**这两块盘 —— UMM 裸设备读写。
4. **udev 0666 规则仅实验用**（04 写入，按 UMMSSD* serial 匹配）；生产
   请用专用用户组。
5. **socket netdev 有顺序依赖**：VM1 listen 必须先于 VM2 connect，03 已
   编排；若单独重启 VM2 需保证 VM1 在线。
6. **共享盘没有跨 VM 一致性语义**：两个 QEMU 进程各自有独立的 guest 页
   缓存。本实验两个 VM 的分配区间互不相交、各自只读写自己的区间，所以
   不受影响；但不要用这个环境验证「一个 VM 写、另一个 VM 立刻读」。
7. **token 只是实验值**（lab-token）；`allow_cidrs` 限定 10.0.0.0/24。

## 与单机仿真的关系

`bmpclient/scripts/simulate_shared_pool.py` 在单台宿主上仿真同一拓扑
（ummD+umms+两客户端进程、2×16M raw 文件），断言 A1-A4 含盘上字节级校验；
`simulate_s5_flush_invalidate.py` 仿真 S5 落盘/刷盘时序（B1-B4）。
改 UMM 代码后建议先跑仿真回归（秒级），再进 QEMU 环境做端到端。

## 与 Phase 1 远程数据面的关系

本实验是**共享盘模型**，与 Phase 1 的 **shared-nothing 跨节点远程数据面**
（每节点独占本地 SSD、跨节点走 RPC 数据面）是两个正交的部署形态。
代码上用配置区分：只有显式配置 `peer_nodes`（或 `ssd_owner_node`）时
remote transport 才挂载；本实验刻意不配，I/O 全部本机盘。

## 混合池模式（内存层 + SSD，无 CXL 硬件）

默认配置即混合池：umms 除 SSD 池外还注册 `MEM_TIER_SIZE`（默认 512M）
的内存层，客户端额外跑 S1m（内存层分配+写读校验）与 S4（tier 隔离）。

- **路线A（默认，零代码改动）**：`EXP_MEM_TIER=1`，内存层占 mock CXL
  槽位——服务端/客户端的"CXL 层"在无 `/dev/cxl/mem0` 时自动回退
  malloc 后备，物理上就是 DRAM，只是 GPA tier 位为 1。
- **路线B（真 DRAM tier）**：`MEM_TIER_KIND=dram EXP_MEM_TIER=0`，
  umms 把内存层注册为 DRAM（tier=0，注册即 malloc 后备），客户端
  本地数据面挂进 tier_router 的 DRAM 槽位（`local_mem_as_dram`），
  GPA tier 位为 0。实现见 docs/05 的 Phase 2 小节。
  切换：先 `./04_provision.sh`（代码需含 Phase 2），再
  `MEM_TIER_KIND=dram EXP_MEM_TIER=0 ./05_start_services.sh && ./06_run_experiment.sh`。

关键语义差异（务必理解再下结论）：

| | SSD 层 | 内存层 |
|---|---|---|
| 分配权威 | umms 全局位图 | umms 全局位图 |
| 数据面 | 两 VM **物理共享**（同一后备文件） | 各 VM **私有** malloc buffer |
| 跨 VM 可见性 | 区间不相交前提下各自读写 | 互不可见（本就不共享） |

DRAM 不可能跨机共享，"全局分配 + 私有数据面"是池化 DRAM 配额的合理
语义；但它和共享盘是两回事，实验报告里不要混用两者的结论。

旋钮（env.sh）：`MEM_TIER_SIZE`（内存层容量；VM1 上 umms 与客户端各
malloc 一份，`VM_MEM_MB` 默认已提到 4G）、`EXP_MEM_TIER/-CHUNKS/-CHUNK_SIZE`。
关掉混合池：`EXP_MEM_TIER=-1 ./06_run_experiment.sh`。

## S5：落盘/刷盘语义（A 写 → fence 落盘 → B invalidate → 读回）

`EXP_S5=1`（默认开）时 06 在 S0-S4 之后追加 S5 阶段，验证"写入端落盘 +
读取端刷盘"这对跨页缓存语义。背景：UMM 的 SSD 数据面是 `mmap(MAP_SHARED)`
+ memcpy，写只进本 VM 页缓存；两台 VM 经各自 QEMU 看到同一后备文件，
**B 的页缓存不会因 A 落盘而自动失效**——不落盘/不刷盘就会读到陈旧数据。

S5 时序（三角色由 06 分时启动，经各 VM `~/s5_*` 标记文件带外同步）：

1. vmA `writer`：分配 SSD chunk → 等 vmB 预读 → 写确定性 pattern →
   `flush()`（fence = CPU 屏障 + 全池 `msync(MS_SYNC)`，数据压过后备文件）
   → **进程保活**等 vmB 验完才释放（fence 而非退出时 munmap 才是被验证
   的落盘路径）。
2. vmB `preread`：预读目标区域，用旧内容填充 vmB 页缓存（制造陈旧视图）。
3. vmB `verify`：先不刷盘读——若 digest 与写入 pattern 不符即打印
   `负路径证据：刷盘前读到陈旧数据`（页缓存陷阱复现）；随后
   `invalidate_chunk()`（`msync(MS_INVALIDATE)` 丢弃本机缓存页）再读，
   digest 必须与写入端**逐字节一致**（正路径断言，不过则 06 失败）。

B 未参与分配，chunk 描述符由 GPA/chunk_id/size 手工构造（06 从 vmA 的
`~/s5_alloc.json` 带外取回）。旋钮：`EXP_S5_SIZE`（默认 16M）、
`EXP_S5_TIMEOUT`（默认 300s）；`EXP_S5=0` 跳过。

负路径是否复现取决于 vmB 页缓存命中情况：若 verify 打印"提示：刷盘前
读到的已是新数据"而非负路径证据，说明本次缓存未命中旧页，**不是失败**，
正路径依然有效。想提高复现率可把 `EXP_S5_SIZE` 调大（页缓存压力更可控）。

API 语义（libumm 新增，bmpclient 已封装）：
- `umm_fence()` / `client.flush()`：SSD tier 的 fence 现在是**真落盘**
  （CPU 屏障 + 全池 msync），不再只是编译器/CPU 屏障。
- `umm_invalidate(desc, off, len)` / `client.invalidate_chunk(...)`：
  读取前丢弃本机该 GPA 区间的缓存页；非 SSD tier 为 no-op。
- 配套单机仿真：`bmpclient/scripts/simulate_s5_flush_invalidate.py`
  （单机共享页缓存，负路径不复现属预期，只断言正路径与时序）。

## S6：共享内存窗口（virtio-pmem，内存层硬件一致）

S5 的对照实验：DRAM 层从"各 VM 私有 malloc"换成**宿主共享物理页**——
03 给两台 VM 挂同一份 virtio-pmem 后备文件（`-object memory-backend-file,
share=on`），VM 内出现 `/dev/pmem0`；mem_service 对它 `open+mmap(MAP_SHARED)`
（DAX 语义），两 vCPU 的读写直达同一组物理页，x86 硬件 cacheline 一致，
**无需 S5 的 fence+invalidate**——这正是 CXL 3.x G-FAM 的软件替身。

启用（**注意：运行中的 VM 没有这块设备，必须重启 VM**）：

```bash
export MEM_TIER_BACKING=/dev/pmem0      # 共享窗口；留空=私有 malloc（默认，06 跳过 S6）
./07_teardown.sh                        # 或手工杀掉两个 qemu
./03_launch_vms.sh && ./04_provision.sh && ./05_start_services.sh
./06_run_experiment.sh                  # 跑完 S0-S5 后追加 S6 阶段
```

S6 时序（两角色，`~/s6_*` 标记文件带外同步）：

1. vmA `s6-writer`：在**内存层**分配 chunk → 写确定性 pattern →
   `flush()`（内存层 fence = 纯 CPU 屏障，无 msync）→ 保活等 vmB。
2. vmB `s6-verify`：按带外 GPA 手工构造描述符**直接读，不执行任何
   invalidate/刷盘**，digest 与写入端逐字节一致即 PASS——证明内存层
   跨 VM 读写天然一致。

工程防线（防"共享无声退化成私有"）：

- DRAM 设备后备路径 **fail-hard**：open/mmap 失败直接报错，**绝不回退
  malloc**（回退会把共享窗口静默变成私有内存，实验就白跑了）。
- 06 收尾对 `s6-writer.log`/`s6-verify.log`/`umms.log` grep
  `falling back to malloc backing`，命中即判失败（覆盖 CXL 分支遗留的
  lazy 回退路径）。
- 04 对 `/dev/pmem0` 做容量校验（`blockdev --getsize64` 须 ≥ `SHM_SIZE`），
  防止 umms 注册容量超出后备文件引发 SIGBUS。

旋钮：`SHM_SIZE`（后备文件大小，默认 1G）、`SHM_FILE`、`EXP_S6_SIZE`、
`EXP_S6_TIMEOUT`；`EXP_S6=0` 强制跳过。路线A/B 通吃：
`MEM_TIER_KIND=cxl EXP_MEM_TIER=1`（走原 CXL 设备分支）或
`MEM_TIER_KIND=dram EXP_MEM_TIER=0`（走新的 DRAM 设备后备分支），
umms yaml 由 05 写入 `memory_device`，libumm 经 `cxl_device` 字段传给
local transport（复用现有字段，ABI 不变）。

配套单机仿真：`bmpclient/scripts/simulate_s6_shared_mem.py`
（同一后备文件跑两进程 C1-C3：exit 0 / PASS 行含"未执行任何 invalidate"
/ 无 malloc 回退无 remote）。

实验验证内容与拓扑的总结见 `验证内容与拓扑总结.md` + `混合池拓扑图.png`。

## 故障排查

| 现象 | 排查 |
|---|---|
| 03 等 ssh 超时 | 看 `work/vm1-console.log`；TCG 下首次启动慢，耐心或加超时 |
| VM 内看不到 nvme 盘 | `lsblk -d -o NAME,SERIAL`；确认 03 的 SSD_ARGS 与 QEMU 版本（>=6 建议） |
| 04 报 make: command not found | 新 VM 的 cloud-init apt 失败（典型：实验室出网需代理）。VM 内 `tail -30 /var/log/cloud-init-output.log` 确认；修复：手工带代理安装 `sudo apt-get -o Acquire::http::Proxy=http://代理:端口 -o Acquire::https::Proxy=http://代理:端口 install -y build-essential python3 python3-dev make` 后重跑 04；下次重建前 `export APT_PROXY=http://代理:端口`（02 会注入 cloud-config apt 段） |
| 06 客户端 rc=-1 打不开设备 | VM 内 `ls -l /dev/nvme*n1`；udev 规则是否生效（04 的输出） |
| S6 被跳过/VM 内没有 /dev/pmem0 | `MEM_TIER_BACKING=/dev/pmem0` 只对**新启动**的 VM 生效；运行中的 VM 不会热插这块设备。先 `./07_teardown.sh`（或杀 qemu）再重跑 03→06。VM 内 `ls -l /dev/pmem0` 与 `dmesg \| grep pmem` 确认 |
| 03 报 `not prepared for memory devices, consider specifying the maxmem option` | virtio-pmem 是内存设备，`-m` 必须带 maxmem。03 已修（挂 pmem 时自动用 `size=…M,slots=2,maxmem=VM_MEM+SHM大小`）；旧版脚本手工改 `-m` 即可 |
| PCI 里有 virtio-pmem（`Device 105b`）但没有 /dev/pmem0 | guest 内核缺驱动：Ubuntu 云镜像把 `virtio_pmem`/nvdimm 放在 `linux-modules-extra` 包。04 已自动处理（缺则经 `APT_PROXY` 安装+modprobe，失败即拦截）；手工修复：VM 内 `sudo apt-get -o Acquire::http::Proxy=$APT_PROXY install -y linux-modules-extra-$(uname -r) && sudo modprobe virtio_pmem` |
| ssh 报 `Permission denied (publickey)` | 两个坑都踩过：① 用户名是 `umm`（`work` 是目录名）；② `work/lab_key` 权限须 600（`cp -a` 恢复后可能变 711，ssh 拒用，`chmod 600` 即可） |
| umms 注册容量虚增/设备重复 | 看 VM1 `~/umms.log`，应恰好 device[0]/device[1] 各一条 registered |
| 管控面不可达 | VM2 上 `ip a` 确认 10.0.0.12；VM1 `ss -ltn`（或看日志）确认监听 |
| 实验后想清零重跑 | `./07_teardown.sh --purge && ./01_make_images.sh ...` 全流程重来 |
| 重跑 06 报 rc=-3 空间不足 | 上次 `--keep` 的分配还占在 umms 位图里；先 `./05_start_services.sh` 重启 umms 重置分配器 |
| 客户端行为像旧代码（行号对不上/偷挂 remote） | **陈旧 `build_pic` 事故**：宿主机源码树里的 `umm/build_pic/*.o` 若被 tar 带进 VM，其 mtime 比源码新会让 make 跳过编译，`libumm.so` 链出旧代码（服务端 `build/*.o` 全新所以正常，极具迷惑性）。04 已排除并自检；手工修复：VM 上 `cd ~/UMM/umm && rm -rf build build_pic && make -j$(nproc)`，再 `nm -D build/libumm.so \| grep ssd_transport_create_multi` 验证 |

### 版本甄别技巧：用日志行号当"二进制指纹"

`__LINE__` 是编译期常量。客户端日志形如 `umm_api.c:539 - ...no topology...`，
行号直接暴露二进制由哪版源码编译。本实验各版本的锚点行号（`no topology` 消息）：
pristine=448，Phase-1-only=505，当前共享盘版=539/540。
行为与源码对不上时，先对行号，再对 `nm -D` 符号表，可快速定位"新壳旧芯"。
