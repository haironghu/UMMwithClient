#!/usr/bin/env python3
"""生成 06 设计文档的两幅示意图（写路径排布 / topk 读路径定位）。"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Rectangle

plt.rcParams["font.sans-serif"] = ["Noto Sans CJK JP", "DejaVu Sans", "sans-serif"]
plt.rcParams["axes.unicode_minus"] = False

C_GRID = "#dbeafe"; C_BLOCK = "#fbbf24"; C_HASH = "#dcfce7"; C_META = "#fee2e2"
C_SSD = "#e5e7eb"; C_SEG = "#bfdbfe"; C_BUF = "#fde68a"; C_ARROW = "#374151"
OUT = "bmpclient/docs/images"

def box(ax, x, y, w, h, text, fc="white", fs=10, ec=C_ARROW, lw=1.2, weight="normal", rounding=0.6):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0.15,rounding_size={rounding}",
                                fc=fc, ec=ec, lw=lw))
    ax.text(x + w / 2, y + h / 2, text, ha="center", va="center", fontsize=fs, weight=weight)

def arrow(ax, x1, y1, x2, y2, text=None, fs=9, color=C_ARROW, lw=1.6, style="-|>", ls="-", tx=None, ty=None):
    ax.add_patch(FancyArrowPatch((x1, y1), (x2, y2), arrowstyle=style, mutation_scale=14,
                                 color=color, lw=lw, linestyle=ls))
    if text:
        ax.text(tx if tx is not None else (x1 + x2) / 2,
                ty if ty is not None else (y1 + y2) / 2 + 0.6,
                text, ha="center", va="bottom", fontsize=fs, color=color)

# ======================================================================
# 图 1：全量 KV cache 跨盘打散 + 盘内 super page 对齐
# ======================================================================
fig, ax = plt.subplots(figsize=(16, 9))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.3, "全量 KV cache 的多盘打散与盘内 super page 对齐保存（N_SSD = 8）",
        ha="center", fontsize=15, weight="bold")

# ---- 左：全量 KV 网格 ----
ax.text(12.5, 48.5, "① 全量 KV cache（layer × token）", ha="center", fontsize=12, weight="bold")
gx, gy, cw, ch = 4.5, 28.0, 1.55, 1.9   # 网格原点（左下）、单元宽高
ROWS, COLS = 8, 10
for r in range(ROWS):          # r=0 在最上面 = layer 0
    for c in range(COLS):
        y = gy + (ROWS - 1 - r) * ch
        fc = C_BLOCK if (r == 2 and 3 <= c <= 10 - 1) else C_GRID
        if r == 2 and c >= 3:
            fc = C_BLOCK
        ax.add_patch(Rectangle((gx + c * cw, y), cw - 0.12, ch - 0.15, fc=fc, ec="#6b7280", lw=0.6))
ax.text(gx - 1.2, gy + ROWS * ch / 2, "layer_id", rotation=90, ha="center", va="center", fontsize=10)
ax.text(gx + COLS * cw / 2, gy - 1.6, "token_idx", ha="center", fontsize=10)
# 高亮 block 标注
bx0, by0 = gx + 3 * cw, gy + (ROWS - 1 - 2) * ch
ax.annotate("一个 vllm block（8 个 token，单层）\n→ 逐 token 展开为排布单元",
            xy=(bx0 + 3.5, by0 + ch), xytext=(2.0, 46.5), fontsize=10,
            arrowprops=dict(arrowstyle="->", color=C_ARROW), va="center")
box(ax, 2.0, 22.6, 21.0, 3.4,
    "排布单元 = (layer_id, token_idx) 的 K+V\n定长 unit_size（4KB 对齐）",
    fc="white", fs=10)

# ---- 中：位置哈希 ----
ax.text(35.0, 48.5, "② 确定性位置哈希分盘", ha="center", fontsize=12, weight="bold")
box(ax, 26.0, 40.0, 18.0, 5.2,
    "device(t, l) =\n((t×STEP_IDX + l×STEP_LAYER)\n% PRIME) % 8",
    fc=C_HASH, fs=11, weight="bold")
box(ax, 26.0, 33.4, 18.0, 4.6,
    "性质一：连续 8 个 token\n恰好覆盖全部 8 盘（完全剩余系）\n→ topk 聚集窗口被最大化打散",
    fc="white", fs=9.5)
box(ax, 26.0, 26.6, 18.0, 4.6,
    "性质二：同一 token 相邻层\n盘号错位（步进 STEP_LAYER）\n→ 层间相似访问不挤同一盘",
    fc="white", fs=9.5)
arrow(ax, 23.5, 36.5, 26.0, 42.0, fs=10, text="逐单元计算", tx=21.6, ty=39.8)

# ---- 右：8 盘 extent ----
ax.text(74.0, 48.5, "③ 8 块 SSD：extent 内按段（super page）对齐", ha="center", fontsize=12, weight="bold")
sp_sizes = ["2MB", "2MB", "4MB", "2MB", "1MB", "2MB", "4MB", "2MB"]
ex, ew = 56.0, 26.0
ey0, eh, egap = 8.6, 3.0, 1.35
disk_y = []
for d in range(8):
    y = ey0 + (7 - d) * (eh + egap)
    disk_y.append(y)
    ax.text(ex - 1.0, y + eh / 2, f"SSD{d}", ha="right", va="center", fontsize=10, weight="bold")
    ax.add_patch(Rectangle((ex, y), ew, eh, fc="white", ec=C_ARROW, lw=1.2))
    nseg = 4
    for s in range(nseg):
        sx = ex + s * (ew / nseg)
        ax.add_patch(Rectangle((sx, y), ew / nseg, eh, fc=C_SEG if s < 2 else "white",
                               ec="#6b7280", lw=0.7))
    ax.text(ex + ew / nseg, y + eh / 2, "段0", ha="center", va="center", fontsize=8)
    ax.text(ex + 2 * ew / nseg, y + eh / 2, "段1", ha="center", va="center", fontsize=8)
    ax.text(ex + ew + 1.0, y + eh / 2, f"段={sp_sizes[d]}", ha="left", va="center", fontsize=8.5, color="#1d4ed8")
ax.text(ex + ew + 1.0, ey0 + 8 * (eh + egap) - 0.4, "段大小按盘配置",
        ha="left", va="bottom", fontsize=8.5, color="#1d4ed8")

# 哈希 → 各盘的扇出箭头（从哈希框到 8 盘）
for d in range(8):
    arrow(ax, 44.3, 42.0, ex - 0.2, disk_y[d] + eh / 2, lw=1.0,
          color="#059669" if d % 2 == 0 else "#0891b2")
ax.text(49.5, 44.6, "同一 block 的 8 个 token\n一盘一个，连续 token 覆盖全盘", ha="center", fontsize=9, color="#065f46")

# ---- 右下放大：单盘聚合写 ----
zx, zy = 56.0, 0.8
box(ax, zx, zy + 2.6, 12.5, 3.2, "段写缓冲（主机内存）\n同盘 token 追加聚合", fc=C_BUF, fs=9)
arrow(ax, zx + 12.8, zy + 4.2, zx + 16.2, zy + 4.2,
      text="写满后一次连续大 IO（write_from）", fs=8.5, ty=zy + 6.1)
ax.add_patch(Rectangle((zx + 16.5, zy + 2.6), 14.0, 3.2, fc=C_SEG, ec=C_ARROW, lw=1.1))
for i in range(1, 7):
    ax.plot([zx + 16.5 + i * 2.0] * 2, [zy + 2.6, zy + 5.8], color="#6b7280", lw=0.6)
ax.text(zx + 23.5, zy + 1.5,
        "段内槽位 offset = 段基址 + i × unit_size；\nextent 内连续 LBA 区间 → FTL 大概率映射连续物理空间",
        ha="center", fontsize=8.5)

# ---- 底部：元数据条 ----
ax.text(24.0, 19.8, "④ 元数据（offload 时同步写入）", ha="center", fontsize=12, weight="bold")
# packed entry 位段
mx, my, mw, mh = 3.0, 13.4, 26.0, 3.4
ax.add_patch(Rectangle((mx, my), mw * 48 / 64, mh, fc="#fecaca", ec=C_ARROW, lw=1.1))
ax.add_patch(Rectangle((mx + mw * 48 / 64, my), mw * 8 / 64, mh, fc="#fed7aa", ec=C_ARROW, lw=1.1))
ax.add_patch(Rectangle((mx + mw * 56 / 64, my), mw * 8 / 64, mh, fc="#fef08a", ec=C_ARROW, lw=1.1))
ax.text(mx + mw * 24 / 64, my + mh / 2, "offset : 48 bit（extent 内字节偏移）", ha="center", va="center", fontsize=9.5)
ax.text(mx + mw * 52 / 64, my + mh / 2, "rsv:8", ha="center", va="center", fontsize=8)
ax.text(mx + mw * 60 / 64, my + mh / 2, "flags:8", ha="center", va="center", fontsize=8)
ax.text(mx + mw / 2, my + mh + 1.0, "slot_table[layer][token_idx] = 8B entry（扁平数组，O(1) 下标）",
        ha="center", fontsize=10, weight="bold")
box(ax, 2.0, 8.6, 13.5, 3.4, "flags：VALID | IN_BUF\n→（下盘后）ON_SSD", fc="white", fs=8.5)
box(ax, 16.5, 8.6, 13.5, 3.4, "段目录 seg_dir[d][seg]\n= (refcount, state)\n仅写侧回收用", fc="white", fs=8.5)
ax.text(15.5, 5.0, "定位坐标 = ( extent[hash(t,l)] , entry.offset , unit_size ) —— 直接作为 umm_read 入参",
        ha="center", fontsize=10, color="#b91c1c", weight="bold")

fig.savefig(f"{OUT}/06_写路径_打散与superpage对齐.png", dpi=170, bbox_inches="tight")
plt.close(fig)

# ======================================================================
# 图 2：稀疏 topk 加载时的定位与并行读
# ======================================================================
fig, ax = plt.subplots(figsize=(16, 9))
ax.set_xlim(0, 100); ax.set_ylim(0, 56); ax.axis("off")
ax.text(50, 54.3, "稀疏 topk 加载：定位流水线与多盘并行读（元数据机制全程体现）",
        ha="center", fontsize=15, weight="bold")

# ---- 输入 ----
box(ax, 2.0, 42.0, 17.0, 8.0,
    "输入\nlayer_id = 3\ntopk = {37,38,41,102,\n103,900}\n（索引局部聚集）",
    fc="#ede9fe", fs=10.5, weight="bold")

# ---- ① 哈希算盘号 ----
box(ax, 24.0, 45.0, 20.0, 5.0,
    "① 算盘号（零元数据）\ndevice = hash(t, 3)\n纯 ALU，可向量化",
    fc=C_HASH, fs=10, weight="bold")
arrow(ax, 19.3, 47.0, 23.8, 47.2)
box(ax, 24.0, 38.6, 20.0, 4.4,
    "t=37→SSD2   t=38→SSD5\nt=41→SSD0   t=102→SSD7\nt=103→SSD3  t=900→SSD5",
    fc="white", fs=9)

# ---- ② gather slot_table ----
box(ax, 49.0, 45.0, 24.0, 5.0,
    "② gather 元数据（一次数组批量索引）\nentry = slot_table[3][t]\n检查 flags 介质位",
    fc=C_META, fs=10, weight="bold")
arrow(ax, 44.3, 47.2, 48.8, 47.2)
# slot_table 数组示意
sx, sy = 49.0, 36.6
ax.add_patch(Rectangle((sx, sy), 24.0, 3.0, fc="white", ec=C_ARROW, lw=1.1))
for i, lab in enumerate(["t=0", "…", "37", "38", "…", "102", "103", "…", "900", "…"]):
    cx = sx + i * 2.4
    ax.add_patch(Rectangle((cx, sy), 2.4, 3.0,
                 fc="#fecaca" if lab in ("37", "38", "102", "103", "900") else "white",
                 ec="#9ca3af", lw=0.6))
    ax.text(cx + 1.2, sy + 1.5, lab, ha="center", va="center", fontsize=8)
ax.text(sx + 12.0, sy - 1.2, "slot_table[3][·]：扁平数组，8B entry = {offset:48, rsv:8, flags:8}",
        ha="center", fontsize=9, color="#b91c1c")
arrow(ax, 61.0, 39.6, 61.0, 44.8, lw=1.2)

# flags 分支
box(ax, 75.5, 45.0, 22.5, 5.0,
    "flags = IN_BUF → 数据还在主机段缓冲\n按同一 offset 直接 memcpy（不走盘）\nflags = ON_SSD → 走盘读",
    fc=C_BUF, fs=9.5)
arrow(ax, 73.3, 47.2, 75.3, 47.2)

# ---- ③ 分桶排序合并 ----
box(ax, 24.0, 26.5, 24.0, 6.0,
    "③ 按盘分桶 + 桶内 offset 排序\n计数排序（8 桶）\n相邻 [off, off+unit) 合并为区间读",
    fc="#e0f2fe", fs=10, weight="bold")
arrow(ax, 34.0, 38.4, 34.0, 32.8, text="(device, offset) 对", fs=9, tx=40.5, ty=35.2)

# 桶示意
for d in range(8):
    bx = 3.0 + d * 2.5
    ax.add_patch(Rectangle((bx, 27.5), 2.2, 4.0, fc=C_SEG if d in (0, 2, 3, 5, 7) else "white",
                           ec="#6b7280", lw=0.8))
    ax.text(bx + 1.1, 26.6, f"D{d}", ha="center", fontsize=8)
ax.text(12.0, 33.0, "8 桶（按盘号）", ha="center", fontsize=9)
box(ax, 3.0, 21.4, 21.0, 3.6,
    "例：SSD5 桶内 t=38(off=a) 与 t=900(off=a+4KB)\n偏移相邻 → 合并为一次 8KB 区间读",
    fc="white", fs=8.5)

# ---- ④ 并行读 ----
box(ax, 53.0, 26.5, 20.0, 6.0,
    "④ 每盘一个读任务\nConcurrentIOEngine 并行下发\nIOAddress = (desc, offset, len)",
    fc="#d1fae5", fs=10, weight="bold")
arrow(ax, 48.3, 29.5, 52.8, 29.5)

ssd_y = []
for d in range(8):
    y = 4.5 + (7 - d) * 2.6
    ssd_y.append(y)
    ax.add_patch(Rectangle((56.0, y), 14.0, 2.0, fc=C_SSD, ec=C_ARROW, lw=1.0))
    ax.text(55.2, y + 1.0, f"SSD{d}", ha="right", va="center", fontsize=8.5)
    if d in (0, 2, 3, 5, 7):
        ax.add_patch(Rectangle((58.0 + (d % 4) * 2.5, y + 0.35), 3.5, 1.3, fc="#fca5a5", ec="#b91c1c", lw=0.7))
arrow(ax, 63.0, 26.3, 63.0, 24.2, lw=1.4)
ax.text(75.0, 13.5, "8 盘并行读\n命中区间（红）\n跨段/跨盘无冲突",
        ha="left", fontsize=9.5, color="#065f46")

# 输出
box(ax, 80.0, 26.5, 18.0, 6.0,
    "输出：按偏移切分\n填入各 topk 对应的\nout buffers（GPU/CPU）",
    fc="#ede9fe", fs=10, weight="bold")
arrow(ax, 73.3, 29.5, 79.8, 29.5)

# ---- 底部：元数据/并发注解 ----
box(ax, 3.0, 12.0, 46.0, 6.5,
    "元数据机制小结\n"
    "· 盘号不入元数据：hash(t, layer) 现场算（性质一：聚集 topk 被打到多盘并行）\n"
    "· slot_table[layer][token] 扁平数组：O(1) gather，entry 直接给出 extent 内偏移\n"
    "· 单写者 + VALID 发布屏障：decode 读路径全程无锁；seg_dir 仅写侧回收，读路径不查",
    fc="white", fs=9.5)
ax.text(26.0, 9.4, "定位开销 ≈ k 次哈希 ALU + k 次数组 gather + 8 桶计数排序 ≪ SSD 读延迟",
        ha="center", fontsize=10, color="#b91c1c", weight="bold")
# 层间预取
box(ax, 3.0, 2.0, 46.0, 4.6,
    "层间流水预取（利用层间相似性）：第 l 层 topk 结果 → prefetch(l+1, topk_l)\n"
    "与第 l 层注意力计算重叠，隐藏 SSD 读延迟",
    fc="#f5f5f4", fs=9.5)

fig.savefig(f"{OUT}/06_读路径_topk定位加载.png", dpi=170, bbox_inches="tight")
plt.close(fig)
print("done")
