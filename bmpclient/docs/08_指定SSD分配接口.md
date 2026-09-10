# 指定 SSD 分配接口恢复

当前基准为 UMMwithClient。参考旧仓库 huangrui125/UMM 的 `0f35b97`，
恢复指定设备分配功能，保留当前 SSD 池分配器、libnvm/NDS 后端和远程数据面。

## 接口与语义

```c
int umm_alloc_on_device(uint64_t size, tier_id_t tier,
                        uint32_t device_idx, ChunkDescriptor *out);
```

```python
desc = lib.alloc_on_device(size, UMM_TIER_SSD, device_idx)
lib.write_from(desc, offset, data)
lib.read_into(desc, offset, output)
lib.free(desc)
```

- 当前仅支持 SSD tier。有效的非 SSD tier 返回 `UMM_E_UNSUPPORTED`。
- `device_idx` 是分配服务端 SSD 池的注册序号，从 0 开始，与
  `get_topology()` 返回的 SSD 条目顺序一致。不是节点号、Linux NVMe 设备编号。
- size 向上对齐到 4096 B；成功分配的整个 extent 位于指定设备。
- 目标盘空间不足或碎片导致无连续区域时返回 `UMM_E_NO_MEMORY`，不换盘。
- 使用同一个池位图管理普通分配与指定设备分配，统一通过原有 free 回收。
- `umm_alloc_tiered()` 原有分配策略保留，仍允许跨设备区间。
- 元数据注册失败会撤销 SSD 空间预留。

## Direct 与 RPC

Direct 模式在本进程调用内存服务。RPC 模式由 umms 管理分配，新增
`MEM_OP_ALLOC_ON_DEVICE=12`；10、11 保持远程数据读写含义。
请求为 `tier:u8, device_idx:u32, size:u64, flags:u32`（17 B，flags 必须为零），
响应为 `status:i32, offset:u64, owner_node:u8`（13 B）。
客户端用属主节点构造 GPA，不把客户端节点误写成数据属主。
这只是分配控制面操作；数据读写继续使用已配置的数据面。
服务不支持指定设备操作时明确返回错误，禁止回退到任意设备分配。

共享盘模式下，客户端的 `ssd_devices` 顺序与容量必须匹配 umms 的池，
路径可为本机对应设备路径。远程模式通过现有 peer_nodes/属主路由读写。
Direct 模式先使用 `register_storage_tier` 注册设备，再创建 VirtualMedia。
运行期保持设备列表及顺序固定。

## 拓扑及编译兼容性

- C `StorageTopology.resources` 扩展到 20 项，与已有 Python ctypes 结构一致。
  **C ABI 大小发生变化：libumm、服务端及所有 C 调用者须一起重新编译。**
  不可把新库直接加载进按旧头文件编译的程序。
- umms 按实际池顺序返回每盘信息；base_offset 为该设备在 tier 地址空间的起点。
- 公共拓扑查询在 RPC 模式优先查询分配服务端；旧服务端不支持时保留 ummD
  摘要查询回退，但旧端不能执行新指定盘分配。
- 拓扑 op 9 保留响应格式，请求可追加 start:u32 取下一页；客户端自动合并，
  避免 16 盘列表超过 4 KiB 控制帧而被截断。分页期间要求拓扑不变。
- 配置解析行缓冲扩大至 8192 B，超过限制明确拒绝，避免设备列表被截断。
  umms 任一配置设备注册失败即启动失败，避免设备编号前移。

## 验证

从仓库根目录运行；全部使用临时文件或桩库，无需显卡或物理 SSD：

```bash
make -C umm clean
make -C umm -j4
make -C umm test
python3 -m unittest discover -s bmpclient/tests
python3 bmpclient/scripts/demo_alloc_on_device.py
```

新增 C 测试覆盖设备边界、盘满/碎片、不跨盘回退、混合分配、并发唯一性、
释放复用、整数溢出、实际文件落点和元数据失败回滚。
端到端脚本覆盖 direct、16 盘共享池 RPC（包括拓扑分页）、不同属主节点的
远程 RPC，以及 SparseKVStore 卸载/flush/fetch，并直接读取目标文件交叉校验。
脚本还先在每盘预留一页，确保测试覆盖非零 extent 基址。

### 本次验证结果

- `make -C umm test`：全部通过，包括新增指定盘分配及注册失败回滚测试。
- `demo_alloc_on_device.py`：direct、16 盘共享池 RPC、远程 RPC 全部通过；
  旧服务端不支持时明确报错，连接可继续使用；设备注册失败时启动终止。
- `simulate_shared_pool.py --dev-size 16M --chunks 2 --chunk-size 2M`：A1–A4 全部通过。
- `demo_e2e_remote_ssd.py`：既有 R0–R3 回归全部通过。
- Python 全套共 65 项，64 项通过。`test_block_read_write_ssd` 仍报
  `umm_write: rc=-8`：旧 FineGrainedAllocator 使用 legacy mock 数据面，
  未启用 SSD tier 路由。使用修改前 `41e8073` 的库及服务端可复现相同错误，
  属于既有问题，本次没有扩展该分配器的初始化接口。

## 本次范围之外

`VirtualMedia.extent_base()` 仍返回占位值；NPU 直通描述符的盘侧地址导出
不属于本次恢复范围。CPU `fetch` 通过 ChunkDescriptor 和现有 UMM 数据面
完成地址翻译，不依赖该占位函数。当前验证是功能正确性，不代表真盘性能结果。
