# UMM × NDS（NPU2SSD 直驱）适配与端到端使用指南

本文档面向需要在昇腾 NPU 上通过 NDS 库（`libnds_aiv.so`，NPU 直驱 NVMe SSD、
绕过 host CPU 数据面）做 KV cache / 数据卸载的用户，说明：

1. UMM 侧做了什么适配、为什么这样设计；
2. 如何构建、部署（三进程形态）；
3. 如何**端到端**调用适配接口完成：设备内存注册 → 单发读/写 → **批量读** → 关闭；
4. 已验证的测试过程与结果、库方确认的已知限制、常见故障排查。

> 适用范围：本文档对应提交基线 `0bb5b6b` 之后的 NDS 适配补丁
> （`npu2ssd_nds.patch`）。上午完成的 host2SSD（`libnvm_host.so`）集成是
> 另一套后端，本文不涉及。

---

## 1. 架构总览

### 1.1 NDS 库的两个事实

`libnds_aiv.so` 提供的是 C++ 单例类 `NDS`（见 `include/nds_aiv.h`）：

| 接口 | 语义 |
|---|---|
| `NDS::Instance()` | 全局单例 |
| `nds_init(deviceId, queueDepth, coreNum, page_size, max_page_num)` | 初始化（进程内**只能成功一次**） |
| `nds_register(dev_mem, aligned_read_size)` | 注册 NPU device 内存段（可多次、多段） |
| `nds_single_read / nds_single_write(vaddr, bytes, f_offset)` | 单发同步读写 |
| `nds_batch_read / nds_batch_write(IOVec*, n_iov)` | 批量读写（见 §6 限制） |
| `nds_uninit()` | 反初始化 |

关键事实：

- **控制面走 RPC**：`nds_init` 内部需要向 NVMe 控制器申请 admin queue，
  通过 `libnvm_host.so` 的 RPC 通道完成（`nvm_host_bind_remote` /
  `nvm_host_set_rpc_context`）。因此必须有一个特权进程先
  `nvm_host_init` + `nvm_host_enable_rpc_server(ctx, socket)` 占住控制器。
- **数据面零 RPC**：读写由 NPU 直接 DMA，host CPU 不参与数据拷贝。
  缓冲必须是 **NPU device 内存**（`aclrtMalloc`），且必须先 `nds_register`。

### 1.2 UMM 侧的分层适配

```
┌─────────────────────────────────────────────────────────┐
│ 用户代码 / demo_e2e_nds.py / KV cache 卸载层             │
├─────────────────────────────────────────────────────────┤
│ 池层   ssd_pool_register_dev_mem / ssd_pool_pwrite       │
│        ssd_pool_pread / ssd_pool_batch_read / batch_write│
│        （多设备虚拟地址空间簿记 + 按 voffset 路由设备）    │
├─────────────────────────────────────────────────────────┤
│ 后端层 ssd_backend_nds.c（纯 C，dlopen 软依赖）           │
│        ssd_nds_open / register_mem / read / write        │
│        batch_read / batch_write / close                  │
│        · dlopen libnds_aiv.so + dlsym Itanium mangled 符号│
│        · 每 device 单例 registry（引用计数 + io_lock）     │
│        · RPC 引导（bind_remote → set_rpc_context）        │
│        · 多段注册（≤16 段）、对齐/范围/未注册前置校验        │
├─────────────────────────────────────────────────────────┤
│ libnds_aiv.so（C++）→ libnvm_host.so（RPC 控制面）        │
└─────────────────────────────────────────────────────────┘
```

**为什么 dlopen 而不是直接链接**：UMM 核心保持纯 C、不引入 C++ 链接；
无 NPU 的开发机可用 C++ 桩库（`test/stub_nds_aiv.cpp`）跑完整单测。

### 1.3 三进程部署形态（真机必须）

库方确认 **当前只有一个 qp（单客户端）**，因此同一时刻只允许一个进程做
NDS 数据面：

```
进程 1  umm_nds_rpc_server   特权控制面（每控制器一个，常驻）
        nvm_host_init → enable_rpc_server(/tmp/nvm_host_rpc.sock) → pause()

进程 2  umms                 分配服务，yaml 中设备写 "nds-meta:..."
        纯簿记（bitmap 分配/容量管理），不 dlopen、不 nds_init、不占用 qp

进程 3  worker / demo        唯一 NDS 客户端
        设备写 "nds:..."，负责 register + read/write/batch_read
```

**常见踩坑**：umms 与 worker 都配 `nds:` → 两个客户端抢单 qp，第二个卡在
`qp_id=4`；worker 进程内若既有 umm_api 客户端库又直连数据面 → 两次
`nds_init`，第二次卡死。适配层已处理：`nds-meta:` 不做任何 NDS 调用；
umm_api 客户端库遇到 `nds:` 设备会跳过本地数据面（tier 返回 UNSUPPORTED）。

---

## 2. 构建

```bash
cd umm
make                 # 只编译：server / client / tools / shared（不会跑测试）
make test-build      # 只编译测试（含 C++ 桩库 bin/libnds_aiv.so）
make test            # 编译 + 运行单测（运行行已 env 隔离真机变量）
make test-bootstrap  # umms 托管拉起 RPC server 的引导测试
```

产物：`bin/umms`、`bin/libumm_client.so`、`bin/umm_nds_rpc_server`、
`bin/libnds_aiv.so`（桩，仅自测用）。

---

## 3. 环境变量全表

| 变量 | 缺省 | 说明 |
|---|---|---|
| `UMM_NDS_PATH` | `libnds_aiv.so` | NDS 库路径（绝对路径推荐） |
| `UMM_NDS_PRELOAD` | 空 | 冒号分隔的预载库列表（RTLD_NOW\|RTLD_GLOBAL），用于补齐 NDS 库未声明的依赖（如 `libread-write_kernel.so`） |
| `UMM_NDS_RPC_SOCKET` | 空 | RPC socket 路径；**设置后**才会执行 RPC 引导（bind→set_ctx） |
| `UMM_NDS_RPC_WAIT_MS` | 3000 | 等 RPC server 就绪的重试总时长（指数退避） |
| `UMM_NDS_MAX_IO` | 1MB | 单次 IO 长度上限（与 nds_init 的 max_page_num 无关） |
| `UMM_NDS_MAX_PAGE_NUM` | 7340032 | 传给 nds_init 的内部 IO 跟踪资源池规模（对齐提供方样例） |
| `UMM_NDS_BATCH_WRITE_EMULATE` | 0 | =1 时 batch_write 降级为单发 write 循环（**真机当前必须开**，见 §6） |
| `UMM_NDS_BATCH_READ_EMULATE` | 0 | =1 时 batch_read 降级为单发 read 循环（调试用） |
| `UMM_NDS_RPC_SERVER_BIN` | 自动 | umms 托管拉起时 rpc_server 二进制路径（默认同目录查找） |
| `UMM_NDS_RPC_READY_TIMEOUT_MS` | 10000 | umms 等 rpc_server 端口就绪的超时 |
| `UMM_TEST_NDS_INITONLY` | 0 | 单测真机冒烟：只做 open/init/close，不做 IO |

真机最小三件套：

```bash
export UMM_NDS_PATH=/home/l00938527/SSDdriver/NPU_Direct_Storage/build/lib/libnds_aiv.so
export UMM_NDS_PRELOAD=/home/l00938527/SSDdriver/NPU_Direct_Storage/build/lib/libread-write_kernel.so
export UMM_NDS_RPC_SOCKET=/tmp/nvm_host_rpc.sock
export UMM_NDS_BATCH_WRITE_EMULATE=1   # batch_write 库未实现，必须单发模拟
```

---

## 4. 启动顺序（真机）

```bash
# 0) Ascend 环境
source /usr/local/Ascend/ascend-toolkit/set_env.sh
export LD_LIBRARY_PATH=/usr/local/Ascend/ascend-toolkit/latest/runtime/lib64:$LD_LIBRARY_PATH

# 1) 控制面守护进程（每控制器一个）
bin/umm_nds_rpc_server --ctrl /dev/libnvm_helper0 --ns 1 --qd 64 \
    --socket /tmp/nvm_host_rpc.sock &
# 看到 "rpc server ready" 即可；stale socket 会自动 unlink

# 2) 分配服务（簿记，不占用 qp）
bin/umms -c config/umms_ssd_nds.yaml &    # yaml 中设备为 nds-meta:0+0x40000000:16G

# 3) 数据面进程（唯一 NDS 客户端）
python3 ../bmpclient/scripts/demo_e2e_nds.py --real ...
# 或你自己的进程，按 §5 的 C API 调用
```

umms 也可以**托管拉起** rpc server（Phase 2）：yaml 中配置
`nds_rpc_server_enable: true` + `nds_rpc_server_ctrl/ns/qd/socket`，
umms 会 fork+exec 并等待端口就绪后自动 `setenv UMM_NDS_RPC_SOCKET`。

---

## 5. 端到端使用：读写与批量读

### 5.1 C API（后端层，`ssd_backend_nds.h`）

```c
#include "transport/ssd_backend_nds.h"

SsdNdsBackend *b = NULL;

/* 1. 打开设备：spec = "<device_id>[+<base_off>]"，容量由上层显式管理 */
ssd_nds_open("0+0x40000000", &b);          /* NPU0，窗口基址 1GB（避开 LBA0） */

/* 2. 注册 NPU device 内存段（aclrtMalloc 得来，可多段，≤16 段） */
void *seg0; aclrtMalloc(&seg0, 256UL<<20, 1 /*ACL_MEM_MALLOC_HUGE_ONLY*/);
ssd_nds_register_mem(b, seg0, 256UL<<20);

/* 3. 单发写：把 device 内存 seg0 起始 64KB 写到窗口内偏移 0 */
ssd_nds_write(b, /*offset=*/0, seg0, 64UL<<10);

/* 4. 单发读：从窗口内偏移 0 读 64KB 到 seg0+1MB 处 */
ssd_nds_read(b, 0, (char*)seg0 + (1UL<<20), 64UL<<10);

/* 5. 批量读：128 条 × 8192B，每条 IOVec 指定 device vaddr + 窗口内 offset */
UmmNdsIOVec iovs[128];
for (int i = 0; i < 128; i++) {
    iovs[i].vaddr  = (char*)seg0 + (2UL<<20) + i * 8192;  /* 读回区 */
    iovs[i].length = 8192;
    iovs[i].offset = (uint64_t)i * 8192;                  /* 盘上窗口内偏移 */
}
ssd_nds_batch_read(b, iovs, 128);

/* 6. 关闭（引用计数归零时自动 nds_uninit → rpc_disconnect → dlclose） */
ssd_nds_close(b);
aclrtFree(seg0);   /* 必须在 close 之后；rc=107000 告警可忽略，见 §6 */
```

约束（适配层会在调用前校验并拒绝，NDS 接口 void 返回、无法运行期报错）：

- `offset`、`len` 必须 **page_size（4096）对齐**；`len ≤ UMM_NDS_MAX_IO`（缺省 1MB）；
- 每条 IO 的 `vaddr` 必须**完整落在某一个已注册段内**，跨段拒绝；
- 未注册任何段时，read/write/batch 一律返回 `UMM_E_INVALID_ARG`；
- 批量写：`ssd_nds_batch_write` 接口存在，但**库未实现**（空转不落盘），
  真机必须 `UMM_NDS_BATCH_WRITE_EMULATE=1` 走单发循环兜底（见 §6）。

### 5.2 池层（多设备统一簿记，`ssd_pool.h`）

```c
SsdPool *pool = ssd_pool_create();
ssd_pool_add_device(pool, "nds:0+0x40000000", 16UL<<30);   /* 容量必填 */

ssd_pool_register_dev_mem(pool, seg0, 256UL<<20);

uint64_t voff;
ssd_pool_alloc(pool, 1UL<<20, &voff);        /* 虚拟地址空间分配 1MB */
ssd_pool_pwrite(pool, voff, 64UL<<10, seg0); /* 按 voff 路由到设备写 */
ssd_pool_batch_read(pool, iovs, 128);        /* iovs[].offset 为虚拟空间偏移 */
ssd_pool_free(pool, voff, 1UL<<20);
ssd_pool_destroy(pool);
```

### 5.3 Python 参考实现（完整可跑）

`bmpclient/scripts/demo_e2e_nds.py` 就是端到端参考：

- `AclArena`：ctypes 直调 `libascendcl.so`（`aclrtMalloc` HUGE_ONLY /
  `aclrtMemcpy` / `aclrtMemset` / `aclrtSynchronizeDevice`），多段分配并
  逐段 `register_dev_mem`；无需 torch。
- `NdsDataPlane`：ctypes 直连 `ssd_pool_*` API。
- 位置编码 pattern 校验：每 8B 小端 uint64 = `(tag<<48)|pos`，
  读回后 `analyze_pos_pattern` 输出 dominant_delta / same_tag_ratio，
  能精确区分 DMA 位移、旧数据残留、全零——D1/D2/X1/X2 全靠它定位。

常用命令：

```bash
# 无 NPU 自测（桩库）
python3 bmpclient/scripts/demo_e2e_nds.py

# 真机安全冒烟（只 init 不 IO）
UMM_TEST_NDS_INITONLY=1 python3 bmpclient/scripts/demo_e2e_nds.py --real

# 真机完整 D1（64KB 单发回环）+ D2（128×8192B batch）
python3 bmpclient/scripts/demo_e2e_nds.py --real --device 0 \
    --base-off 0x40000000 --capacity 16G --segments 4

# 诊断探针
... --probe                 # 单发写读 + hex dump（--probe-no-write 只读盘）
... --probe-batch 8         # batch 阶梯 + 盘侧反查 + X1/X2 交叉验证
```

---

## 6. 库方确认的四个已知限制（适配方式）

| # | 限制 | UMM 适配 |
|---|---|---|
| 1 | **`nds_batch_write` 未实现**：调用空转（0.06ms 无内核轨迹、盘侧全零） | `UMM_NDS_BATCH_WRITE_EMULATE=1` 单发 write 循环兜底；**批量读 `nds_batch_read` 可用且经 X1 交叉验证正确** |
| 2 | **无 unregister**：`aclrtFree` 报 rc=107000 | HBM arena 按进程级一次分配不释放设计；cleanup 顺序 = 先 `close()`（uninit）后 `aclrtFree`，107000 降级 WARNING |
| 3 | **单 qp / 单客户端** | `CORE_NUM` 缺省改为 1；三进程形态（§1.3），umms 用 `nds-meta:` 纯簿记；进程内单次 `nds_init`（引用计数单例） |
| 4 | **全部 IO 同步** | D1/D2 中的 200ms sleep 为防御性保留；`io_lock` 串行化 |

---

## 7. 测试过程与已验证结论

| 测试 | 内容 | 结果 |
|---|---|---|
| 单测 81 项 | 分派/容量/对齐拒绝/未注册拒绝/batch 超限/混合池/并发/EMULATE 兜底（桩库） | PASS |
| INITONLY 冒烟 | 真机 open → RPC bind → nds_init → close，4/4 | PASS |
| D1 | 64KB 单发写 → 单发读 → 位置编码校验 | PASS |
| D2 | 128×8192B：写源区/读回区镜像分离，batch_read 回读校验 | PASS（写路径走 EMULATE） |
| X1 交叉验证 | 单发写 → batch_read 读 → 校验 | PASS（证明 batch_read 正确） |
| X2 交叉验证 | batch_write → 单发读 → 校验 | 证实库 batch_write 空转（后用 EMULATE 闭环） |
| probe-race | E1 基线 / E2 覆写 / E3 偏移寻址 | 排除异步竞态（库方确认同步） |

诊断方法学（可复用）：数据校验失败时不要只看"不匹配"，用位置编码
pattern + `analyze_pos_pattern` 的 dominant_delta 判定错位字节数
（本案例曾精确定位 delta=+65536 的 DMA 位移，最终确认为测试结构问题）。

---

## 8. 故障排查表

| 现象 | 根因 | 处理 |
|---|---|---|
| dlopen 报 `undefined symbol: _Z14readwrite_demo...` | NDS 库未声明依赖 `libread-write_kernel.so` | `UMM_NDS_PRELOAD=<该库路径>`；适配层会自动 RTLD_NOW→RTLD_LAZY 回退 |
| `nvm_host_admin_cq_create: no RPC context` abort | 未做 RPC 引导 | 先起 `umm_nds_rpc_server`，设 `UMM_NDS_RPC_SOCKET` |
| bind failed rc=-6 | RPC server 未起 / socket 路径不对 | 检查进程与 `ls -l $UMM_NDS_RPC_SOCKET` |
| 卡 `qp_id=4` | 两个 NDS 客户端抢单 qp / 进程内两次 nds_init | umms 改 `nds-meta:`；确认进程内只有一个数据面入口 |
| D2 全挂、盘侧全零 | batch_write 未实现 | `UMM_NDS_BATCH_WRITE_EMULATE=1` |
| `aclrtFree rc=107000` | 库无 unregister | 先 close 后 free；WARNING 可忽略 |
| umms 端口一直未就绪 | rpc server 没起 / env 缺失 | demo 已落盘服务器日志，看日志末尾；前置体检 WARNING 提示缺哪个 env |

---

## 9. 相关文件索引

| 文件 | 作用 |
|---|---|
| `umm/src/transport/ssd_backend_nds.c/.h` | NDS 后端（dlopen + RPC 引导 + 多段注册 + EMULATE） |
| `umm/src/transport/ssd_pool.c` | `nds:` / `nds-meta:` 分派、池级 register/batch API |
| `umm/bin/umm_nds_rpc_server.c` | 控制面守护进程 |
| `umm/bin/umms.c` / `umm/src/common/config_parser.c` | `nds_rpc_server_*` 配置 + 托管拉起 |
| `umm/test/test_ssd_nds.c` | 81 项单测（含 INITONLY 真机冒烟） |
| `umm/test/stub_nds_aiv.cpp` / `stub_nvm_host_rpc.c` | 无 NPU 自测桩 |
| `bmpclient/scripts/demo_e2e_nds.py` | 端到端 demo + 诊断探针（§5.3） |
| `umm/config/umms_ssd_nds.yaml` | 真机部署样例（含 env 全表注释） |
