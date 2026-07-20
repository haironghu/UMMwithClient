# Transport 层清理计划

## 目标
删除 transport 层冗余代码，合并重复模块

## 分析结论

### 冗余项

| # | 文件 | 行数 | 问题 | 决策 |
|---|------|------|------|------|
| 1 | `transport_rdma.c` | 152 | 纯 stub，全部 no-op | **删除** |
| 2 | `transport_mock.c` + `transport_cxl.c` | 295+295 | 结构几乎完全相同（map_device→memcpy），仅 mock 多了 mutex 和 cross-node 检查不同 | **合并为 `transport_local.c`** |
| 3 | `ssd_backend.c` + `ssd_pool.c` | 365+360 | ssd_backend 是 ssd_pool 的内部实现细节，两者紧密耦合；ssd_backend 的 read/write/chunk 函数是死代码 | **合并为 `ssd_pool.c`** |

### 合并后结构

```
src/transport/
├── transport.h              # MemoryTransportVtbl 接口（不变）
├── transport_local.c        # 合并 mock + cxl（~300行）
├── transport_ssd.c          # SSD transport（不变）
├── transport_ssd.h          # SSD transport 头（不变）
├── ssd_pool.c               # 合并 ssd_backend + ssd_pool（~400行）
├── ssd_pool.h               # SSD pool 头（不变，加上原 ssd_backend 的公开 API）
├── tier_router.c            # Tier router（不变）
└── tier_router.h            # Tier router 头（不变）
```

## 执行步骤

### Stage 1: 删除 transport_rdma.c
- 删除文件
- 更新 Makefile（移除 transport_rdma.o 引用）
- 更新 umm_transport.c（移除 umm_rdma_vtbl_get 引用）
- 编译验证

### Stage 2: 合并 transport_mock.c + transport_cxl.c → transport_local.c
- 创建 transport_local.c（以 mock 为基础，加入 cxl 的 cross-node 检查）
- 删除 transport_mock.c 和 transport_cxl.c
- 更新 Makefile
- 更新 umm_transport.c（umm_mock_vtbl_get → umm_local_vtbl_get）
- 更新 umm_api.c（umm_mock_vtbl_get 引用）
- 编译验证

### Stage 3: 合并 ssd_backend.c + ssd_pool.c
- 将 ssd_backend.c 的内容内联到 ssd_pool.c
- 更新 ssd_pool.h（加入 ssd_backend 的公开 API）
- 删除 ssd_backend.c 和 ssd_backend.h
- 更新所有 include "ssd_backend.h" 的文件
- 更新 Makefile
- 编译验证

### Stage 4: 运行全部测试
