#!/usr/bin/env python3
"""生成 06 设计文档的补充示意图（§2 排布/I/O 单元分离、§3 位置哈希、
§4 super page 聚合机制、§5 元数据与分层槽位页表）。"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

plt.rcParams["font.sans-serif"] = ["Noto Sans CJK JP", "DejaVu Sans", "sans-serif"]
plt.rcParams["axes.unicode_minus"] = False

C_GRID = "#dbeafe"; C_BLOCK = "#fbbf24"; C_HASH = "#dcfce7"; C_META = "#fee2e2"
C_SSD = "#e5e7eb"; C_SEG = "#bfdbfe"; C_BUF = "#fde68a"; C_ARROW = "#374151"
C_VIOLET = "#ede9fe"; C_CYAN = "#e0f2fe"; C_GREEN = "#d1fae5"
OUT = "bmpclient/docs/images"


def box(ax, x, y, w, h, text, fc="white", fs=10, ec=C_ARROW, lw=1.2,
        weight="normal", rounding=0.6):
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                boxstyle=f"round,pad=0.15,rounding_size={rounding}",
                                fc=fc, ec=ec, lw=lw))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center",
            fontsize=fs, weight=weight)


def arrow(ax, x1, y1, x2, y2, text=None, fs=9, color=C_ARROW, lw=1.6,
          style="-|>", ls="-", tx=None, ty=None):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle=style,
                                 mutation_scale=14, color=color, lw=lw, linestyle=ls))
    if text:
        ax.text(tx if tx is not None else (x1 + x2) / 2,
                ty if ty is not None else (y1 + y2) / 2 + 0.6,
                text, ha="center", va="bottom", fontsize=fs, color=color)


# ======================================================================
# 图 A：排布单元与 I/O 单元分离
# ======================================================================
fig, ax = plt.subplots(figsize=(15, 8.5))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.0, "排布单元与 I/O 单元分离：去哪块盘 与 怎么下盘 是两个独立决策",
        ha="center", fontsize=15, weight="bold")

# ---- 左：排布单元 ----
ax.text(22.0, 47.5, "排布单元（决定 去哪块盘）", ha="center", fontsize=12.5, weight="bold")
box(ax, 4.0, 38.5, 36.0, 6.0,
    "一个 token 一层的 K+V\nkey = (layer_id, token_idx)",
    fc=C_VIOLET, fs=11.5, weight="bold")
box(ax, 4.0, 31.5, 36.0, 5.0,
    "定长 unit_size = 2×n_kv_heads×head_dim×dtype_bytes\n向上对齐到 4KB（盘内长度无需入元数据）",
    fc="white", fs=10)
box(ax, 4.0, 24.5, 36.0, 5.0,
    "用途：位置哈希输入 + slot_table 元数据 key\n一个 vllm block 逐 token 展开 → 打散到多盘（有意为之）",
    fc="white", fs=10)

# 左下：vllm block 展开示意
bx, by = 6.0, 15.5
ax.add_patch(Rectangle((bx, by), 20.0, 4.2, fc=C_BLOCK, ec=C_ARROW, lw=1.2))
for i in range(1, 8):
    ax.plot([bx + i * 2.5] * 2, [by, by + 4.2], color="#92400e", lw=0.7)
for i in range(8):
    ax.text(bx + i * 2.5 + 1.25, by + 2.1, f"t{i}", ha="center", va="center", fontsize=8.5)
ax.text(bx + 10.0, by - 1.4, "vllm block（8 token，token 主序连续 buffer）",
        ha="center", fontsize=9.5)
arrow(ax, bx + 10.0, by + 4.6, 22.0, 24.0, text="逐 token 展开\n（memoryview 切片，零拷贝）",
      fs=9, tx=27.5, ty=21.0)

# ---- 右：I/O 单元 ----
ax.text(74.0, 47.5, "I/O 单元（决定 怎么下盘）", ha="center", fontsize=12.5, weight="bold")
box(ax, 56.0, 38.5, 36.0, 6.0,
    "super page（段）\n按盘可配置：super_page_bytes[_per_device]",
    fc=C_SEG, fs=11.5, weight="bold")
box(ax, 56.0, 31.5, 36.0, 5.0,
    "约束：2 的幂、4KB 整数倍、≥ unit_size\n适配不同盘 FTL 条带/擦除块（1MB ~ 16MB）",
    fc="white", fs=10)
box(ax, 56.0, 24.5, 36.0, 5.0,
    "用途：同盘 token 聚合写的下发粒度 + 读合并粒度\n只是写侧管理单位，不参与读路径寻址",
    fc="white", fs=10)

# 右下：段内槽位示意
sx, sy = 58.0, 15.5
ax.add_patch(Rectangle((sx, sy), 30.0, 4.2, fc=C_SEG, ec=C_ARROW, lw=1.2))
n_slot = 6
for i in range(1, n_slot):
    ax.plot([sx + i * 30.0 / n_slot] * 2, [sy, sy + 4.2], color="#1d4ed8", lw=0.7)
ax.text(sx + 15.0, sy + 2.1, "unit | unit | …（同盘 token 按到达顺序追加聚合）",
        ha="center", va="center", fontsize=9)
ax.text(sx + 15.0, sy - 1.4, "段基址 = segment_id × 本盘 super_page_bytes；槽位偏移 = 段基址 + i × unit_size",
        ha="center", fontsize=9)

# ---- 中间：分离说明 ----
arrow(ax, 40.5, 41.5, 55.5, 41.5, lw=2.0)
ax.text(48.0, 43.6, "互不耦合", ha="center", fontsize=11, color="#b91c1c", weight="bold")

# 底部结论
box(ax, 15.0, 3.0, 70.0, 6.0,
    "分离的收益：打散策略只管盘间并行，段聚合只管盘内连续；\n"
    "段大小可按盘异构而不影响定位；元数据 entry 只需存偏移（长度定长隐含、盘号哈希现算）",
    fc=C_GREEN, fs=10.5)

fig.savefig(f"{OUT}/06_排布单元与IO单元分离.png", dpi=170, bbox_inches="tight")
plt.close(fig)

# ======================================================================
# 图 B：确定性位置哈希
# ======================================================================
fig, ax = plt.subplots(figsize=(15, 8.5))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.0, "确定性位置哈希：无状态、可复现的跨盘打散", ha="center",
        fontsize=15, weight="bold")

# 顶部公式
box(ax, 20.0, 45.5, 60.0, 5.5,
    "device(t, l) = ((t × STEP_IDX + l × STEP_LAYER) % PRIME) % N_SSD",
    fc=C_HASH, fs=14, weight="bold")
ax.text(50, 44.0, "t = token_idx（上下文内全局索引）　l = layer_id　N_SSD = 在线 SSD 数（umm_get_topology）",
        ha="center", fontsize=10)

# ---- 左：性质一 ----
ax.text(22.0, 39.0, "性质一：连续 N_SSD 个 token 恰好覆盖全部盘", ha="center",
        fontsize=11.5, weight="bold")
ax.text(22.0, 37.0, "（topk 局部性 → 聚集窗口最大化打散）", ha="center", fontsize=9.5, color="#6b7280")
ax.text(22.0, 34.8, "token 行（t=100..107）→ 盘号行（SSD0..SSD7）", ha="center",
        fontsize=9.5, color="#6b7280")
colors8 = ["#fca5a5", "#fdba74", "#fde047", "#86efac", "#67e8f9", "#93c5fd", "#c4b5fd", "#f0abfc"]
tx0, ty0, tw = 6.0, 28.5, 2.6
for i in range(8):
    d = (i * 17) % 8  # STEP_IDX=17, N_SSD=8 的简化示意
    ax.add_patch(Rectangle((tx0 + i * tw, ty0), tw - 0.15, 3.2, fc="white", ec=C_ARROW, lw=1.0))
    ax.text(tx0 + i * tw + tw / 2, ty0 + 1.6, f"{100 + i}", ha="center", va="center", fontsize=9)
    arrow(ax, tx0 + i * tw + tw / 2, ty0 - 0.1, tx0 + i * tw + tw / 2, 24.6, lw=1.1)
    ax.add_patch(Rectangle((tx0 + i * tw, 21.4), tw - 0.15, 3.2, fc=colors8[d], ec=C_ARROW, lw=1.0))
    ax.text(tx0 + i * tw + tw / 2, 23.0, f"SSD{d}", ha="center", va="center", fontsize=8.5)
ax.text(tx0 + 8 * tw / 2, 19.6, "token 每加 1 盘号步进 STEP_IDX mod N_SSD（≠0 且互质）→ 完全剩余系",
        ha="center", fontsize=9, color="#065f46")

# ---- 右：性质二 ----
ax.text(72.0, 39.0, "性质二：同一 token 连续 N_SSD 层恰好覆盖全部盘", ha="center",
        fontsize=11.5, weight="bold")
ax.text(72.0, 37.0, "（层间相似性 → 相邻层 topk 不挤同一盘）", ha="center", fontsize=9.5, color="#6b7280")
lx0, ly0, lh = 62.0, 32.8, 3.4
for l in range(4):
    d = (37 * 17 + l * 23) % 8  # t=37, STEP_LAYER=23 的简化示意
    y = ly0 - l * (lh + 0.5)
    ax.add_patch(Rectangle((lx0, y), 8.0, lh, fc="white", ec=C_ARROW, lw=1.0))
    ax.text(lx0 + 4.0, y + lh / 2, f"layer {l}", ha="center", va="center", fontsize=9)
    arrow(ax, lx0 + 8.3, y + lh / 2, lx0 + 11.5, y + lh / 2, lw=1.1)
    ax.add_patch(Rectangle((lx0 + 11.8, y), 6.0, lh, fc=colors8[d], ec=C_ARROW, lw=1.0))
    ax.text(lx0 + 14.8, y + lh / 2, f"SSD{d}", ha="center", va="center", fontsize=9)
ax.text(lx0 + 10.0, 18.8,
        "同一 t=37：layer 每加 1 盘号步进 STEP_LAYER mod N_SSD ≠ 0 → 逐层错位",
        ha="center", fontsize=9, color="#065f46")

# ---- 底部：性质三 + 参数约束 ----
box(ax, 4.0, 10.5, 44.0, 7.0,
    "性质三：二维均匀性\n模 PRIME 使 (t, l) 网格到盘号近似均匀，\n长期运行各盘容量与负载均衡（≈1/N_SSD）",
    fc="white", fs=10.5)
box(ax, 52.0, 10.5, 44.0, 7.0,
    "参数约束\nSTEP_IDX / STEP_LAYER：大于 N_SSD 的互异质数\nPRIME：更大质数，建议 > max_t×STEP_IDX + max_l×STEP_LAYER\n（定义域内无回绕，性质一/二严格成立）",
    fc=C_META, fs=10)

box(ax, 15.0, 2.0, 70.0, 5.0,
    "无状态性的价值：任何人凭 (t, l) 即可算出盘号——盘号不占元数据位、不查表；\n"
    "多实例/多进程参数一致即排布可复现；元数据只需存盘内偏移",
    fc=C_GREEN, fs=10.5)

fig.savefig(f"{OUT}/06_确定性位置哈希.png", dpi=170, bbox_inches="tight")
plt.close(fig)

# ======================================================================
# 图 C：盘内 super page 聚合写机制
# ======================================================================
fig, ax = plt.subplots(figsize=(15, 8.5))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.0, "盘内 super page 聚合写：同盘 token 攒批成连续大 IO", ha="center",
        fontsize=15, weight="bold")

# ---- 上：写路径时间线 ----
ax.text(30.0, 48.0, "写路径（单盘视角）", ha="center", fontsize=12, weight="bold")
steps = [
    ("① 哈希分盘", "token 粒度写请求\n到达本盘", C_VIOLET),
    ("② 追加聚合", "写入当前段缓冲\n连续槽位（主机内存）", C_BUF),
    ("③ 写满下盘", "一次 sp_bytes 连续 IO\n（write_from）", C_SEG),
    ("④ 推进新段", "从空闲段池取\n下一个段继续聚合", C_GREEN),
]
for i, (t, d, c) in enumerate(steps):
    x = 4.0 + i * 23.5
    box(ax, x, 39.5, 19.0, 7.0, f"{t}\n{d}", fc=c, fs=10.5, weight="bold")
    if i < 3:
        arrow(ax, x + 19.3, 43.0, x + 23.2, 43.0, lw=1.6)

# ---- 中：段缓冲 → extent 映射 ----
bufx, bufy = 6.0, 27.5
ax.add_patch(Rectangle((bufx, bufy), 26.0, 5.0, fc=C_BUF, ec=C_ARROW, lw=1.3))
n_slot = 5
for i in range(1, n_slot):
    ax.plot([bufx + i * 26.0 / n_slot] * 2, [bufy, bufy + 5.0], color="#92400e", lw=0.8)
for i in range(n_slot):
    ax.text(bufx + i * 26.0 / n_slot + 26.0 / n_slot / 2, bufy + 2.5,
            f"unit{i}", ha="center", va="center", fontsize=9)
ax.text(bufx + 13.0, bufy + 6.6, "当前段写缓冲（大小 = 本盘 super_page_bytes）",
        ha="center", fontsize=10, weight="bold")
ax.text(bufx + 13.0, bufy - 1.5, "槽位偏移 = 段基址 + i × unit_size（与 extent 偏移 1:1 映射）",
        ha="center", fontsize=9)

extx, exty = 52.0, 27.5
ax.add_patch(Rectangle((extx, exty), 42.0, 5.0, fc="white", ec=C_ARROW, lw=1.3))
nseg = 5
for s in range(nseg):
    sx = extx + s * 42.0 / nseg
    ax.add_patch(Rectangle((sx, exty), 42.0 / nseg, 5.0,
                 fc=C_SEG if s == 1 else "white", ec="#6b7280", lw=0.8))
    ax.text(sx + 42.0 / nseg / 2, exty + 2.5, f"seg{s}", ha="center", va="center", fontsize=9)
ax.text(extx + 21.0, exty + 6.6, "extent（umm_alloc_on_device 分配的连续区间）",
        ha="center", fontsize=10, weight="bold")
ax.text(extx + 21.0, exty - 1.5, "段只是写侧分配/回收单位：segment_id = offset >> log2(本盘段大小)；读路径只用绝对偏移",
        ha="center", fontsize=9)
arrow(ax, bufx + 26.5, bufy + 2.5, extx + 1.1 * 42.0 / nseg, exty + 2.5,
      text="写满：整段一次连续大 IO", fs=9.5, tx=44.0, ty=33.6, lw=2.0, color="#1d4ed8")

# ---- 下：为什么有效 + flush 策略 ----
box(ax, 4.0, 15.0, 44.0, 8.5,
    "为什么能降低读冲突\n"
    "· 盘侧 FTL 不可控、无法「指定」物理位置；\n"
    "· 但连续 LBA 大 IO 大概率被 FTL 条带化到跨\n"
    "  channel/die/plane 的连续物理空间；\n"
    "· topk 局部性 ⇒ 相邻 token 写入时间接近 ⇒ 聚合后\n"
    "  偏移相邻，读时可合并为区间读、跨段并行",
    fc="white", fs=10)
box(ax, 52.0, 15.0, 42.0, 8.5,
    "flush 策略（半满段的收益/延迟权衡）\n"
    "· 段写满自动下盘（主路径，无延迟开销）\n"
    "· eager：每个 block 写完即刷（低延迟，聚合弱）\n"
    "· on_request_end（默认）：请求级刷盘\n"
    "· watermark：缓冲驻留超阈值即刷\n"
    "（半满段也按整段长度下发，未用槽位为零页）",
    fc=C_CYAN, fs=10)

box(ax, 15.0, 5.5, 70.0, 6.0,
    "边界条件：段大小按盘独立配置（2 的幂、4KB 整数倍、≥ unit_size），同一盘内固定；\n"
    "跨盘逻辑不受异构段大小影响——哈希只管盘号，读路径只管绝对偏移",
    fc=C_GREEN, fs=10.5)

fig.savefig(f"{OUT}/06_盘内superpage聚合机制.png", dpi=170, bbox_inches="tight")
plt.close(fig)

# ======================================================================
# 图 D：元数据结构与分层槽位页表
# ======================================================================
fig, ax = plt.subplots(figsize=(15, 8.5))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.0, "元数据：分层槽位页表 + packed entry，O(1) 定位打散后的 token",
        ha="center", fontsize=15, weight="bold")

# ---- 左：分层扁平数组 ----
ax.text(24.0, 48.5, "slot_table[layer][token_idx]（单请求作用域）", ha="center",
        fontsize=12, weight="bold")
ly0, rowh = 41.0, 3.6
for l in range(3):
    y = ly0 - l * (rowh + 1.2)
    ax.add_patch(Rectangle((6.0, y), 7.0, rowh, fc=C_VIOLET, ec=C_ARROW, lw=1.0))
    ax.text(9.5, y + rowh / 2, f"layer {l}", ha="center", va="center", fontsize=9.5, weight="bold")
    # 每层一行扁平数组
    for t in range(10):
        cx = 13.5 + t * 2.9
        hit = (l == 1 and t in (2, 3, 6))
        ax.add_patch(Rectangle((cx, y), 2.9, rowh, fc=C_META if hit else "white",
                     ec="#9ca3af", lw=0.6))
        ax.text(cx + 1.45, y + rowh / 2, str(t) if t < 9 else "…",
                ha="center", va="center", fontsize=8)
    if l == 1:
        ax.text(13.5 + 10 * 2.9 + 1.0, y + rowh / 2, "topk 命中（红）→ 一次批量 gather",
                ha="left", va="center", fontsize=9, color="#b91c1c")
ax.text(24.0, ly0 - 3 * (rowh + 1.2) - 0.2,
        "token_idx 请求内天然稠密（0..T-1）→ 数组直接下标而非哈希表：O(1)、缓存友好、支持批量 gather",
        ha="center", fontsize=9.5, color="#065f46")

# ---- 中：packed entry 位段图 ----
ax.text(66.0, 48.5, "packed entry（8B 定长，8B 对齐原子读写）", ha="center",
        fontsize=12, weight="bold")
mx, my, mw, mh = 52.0, 40.0, 42.0, 4.0
ax.add_patch(Rectangle((mx, my), mw * 48 / 64, mh, fc="#fecaca", ec=C_ARROW, lw=1.2))
ax.add_patch(Rectangle((mx + mw * 48 / 64, my), mw * 8 / 64, mh, fc="#fed7aa", ec=C_ARROW, lw=1.2))
ax.add_patch(Rectangle((mx + mw * 56 / 64, my), mw * 8 / 64, mh, fc="#fef08a", ec=C_ARROW, lw=1.2))
ax.text(mx + mw * 24 / 64, my + mh / 2, "offset : 48 bit", ha="center", va="center", fontsize=11, weight="bold")
ax.text(mx + mw * 52 / 64, my + mh / 2, "rsv:8", ha="center", va="center", fontsize=9)
ax.text(mx + mw * 60 / 64, my + mh / 2, "flags:8", ha="center", va="center", fontsize=9)
ax.text(mx + mw / 2, my + mh + 1.0, "63 ……………………… 16 | 15 … 8 | 7 … 0",
        ha="center", fontsize=9, color="#6b7280")
box(ax, 52.0, 31.5, 42.0, 5.5,
    "offset：本盘 extent 内字节偏移（4KB 对齐）\nflags：仅 VALID；IN_BUF/ON_SSD 由 is_buffered() 现场判定\nrsv：保留（校验和/多副本扩展），当前置 0",
    fc="white", fs=9.5)

# ---- 下：定位流水线 ----
ax.text(50.0, 24.5, "decode 快速定位（每 step、每层、topk 批量）", ha="center",
        fontsize=12, weight="bold")
pipe = [
    ("① hash(t, layer)", "盘号现场算\n零元数据", C_HASH),
    ("② gather entry", "slot_table[layer][t]\n一次数组批量索引", C_META),
    ("③ is_buffered 判定", "缓冲命中 → CPU 兜底\n否则走 descriptor", C_BUF),
    ("④ (extent, offset, unit)", "定位结果即 umm_read /\n直通算子入参形态", C_GREEN),
]
for i, (t, d, c) in enumerate(pipe):
    x = 4.0 + i * 23.5
    box(ax, x, 15.5, 19.0, 7.0, f"{t}\n{d}", fc=c, fs=9.5, weight="bold")
    if i < 3:
        arrow(ax, x + 19.3, 19.0, x + 23.2, 19.0, lw=1.6)

# ---- 底部：容量/位宽 + 并发 ----
box(ax, 4.0, 4.5, 44.0, 8.0,
    "容量与位宽\n"
    "· 128K token × 48 层 × 8B ≈ 48 MB/请求（驻留 DRAM）\n"
    "· offset 48 bit = 256 TB，只须覆盖单盘 extent；\n"
    "  1M token × 48 层 × 4KB ÷ 4 盘 ≈ 48 GiB（36 bit），富余 3 个数量级",
    fc="white", fs=9.5)
box(ax, 52.0, 4.5, 42.0, 8.0,
    "并发模型\n"
    "· 同一 (layer, token) 生命周期只分配一次 → 单写者；\n"
    "· set/invalidate 细粒度锁，get/gather 读路径无锁；\n"
    "· entry 整体原子写入：读者见 VALID 即 offset 可见；\n"
    "· seg_dir（段目录/空闲池）仅写侧回收，读路径不查",
    fc="white", fs=9.5)

fig.savefig(f"{OUT}/06_元数据分层槽位页表.png", dpi=170, bbox_inches="tight")
plt.close(fig)
print("done")
