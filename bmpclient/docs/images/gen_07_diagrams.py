# -*- coding: utf-8 -*-
"""
生成 docs/07_VirtualMedia合并设计_稀疏KV专用介质.md 所需的两幅示意图。
输出 SVG（文本格式），不依赖第三方绘图库。
"""

import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


def write_svg(name: str, content: str) -> None:
    path = os.path.join(OUT_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"generated {path}")


def svg_header(width: int, height: int) -> str:
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">\n'
        '  <rect width="100%" height="100%" fill="#ffffff"/>\n'
    )


def svg_footer() -> str:
    return "</svg>\n"


def rect(x, y, w, h, fill, stroke="#333", sw=1, label="", label_color="#000"):
    s = f'  <rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>\n'
    if label:
        s += f'  <text x="{x + w/2}" y="{y + h/2 + 4}" text-anchor="middle" font-size="12" fill="{label_color}">{label}</text>\n'
    return s


def text(x, y, label, size=14, color="#000", anchor="start", weight="normal"):
    return (
        f'  <text x="{x}" y="{y}" text-anchor="{anchor}" font-size="{size}" '
        f'fill="{color}" font-weight="{weight}">{label}</text>\n'
    )


def arrow(x1, y1, x2, y2, color="#333"):
    return (
        f'  <line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="1.5" marker-end="url(#arrowhead)"/>\n'
    )


def marker_def():
    return (
        '  <defs>\n'
        '    <marker id="arrowhead" markerWidth="10" markerHeight="7" refX="9" refY="3.5" orient="auto">\n'
        '      <polygon points="0 0, 10 3.5, 0 7" fill="#333"/>\n'
        '    </marker>\n'
        '  </defs>\n'
    )


def gen_full_kv_scatter():
    W, H = 900, 520
    colors = ["#a8d5e5", "#f9d89c", "#b8e6b8", "#e2b8e2",
              "#f7b7a3", "#c9c9ff", "#ffdfba", "#b5e7a0"]
    n_ssd = 8
    units_per_seg = 4
    n_tokens = 32
    token_w = 24
    token_h = 28
    start_x = 60
    start_y = 60
    gap = 6

    svg = svg_header(W, H)
    svg += marker_def()
    svg += text(W/2, 30, "图 1：全量 KV cache 按位置哈希打散到 8 块 SSD，并以 super page 对齐",
                size=16, anchor="middle", weight="bold")

    # token sequence row
    svg += text(start_x, start_y - 15, "layer 0 的 token 序列（按位置哈希决定目标盘）", size=12)
    for i in range(n_tokens):
        dev = ((i * 17) % 2229299) % n_ssd
        x = start_x + i * (token_w + gap)
        y = start_y
        svg += rect(x, y, token_w, token_h, colors[dev], label=str(i), label_color="#333")

    # arrows down to SSD stacks
    seg_h = 22
    seg_w = 70
    ssd_y = start_y + token_h + 70
    ssd_x_start = 50
    ssd_gap = (W - 2 * ssd_x_start - n_ssd * seg_w) / (n_ssd - 1)

    for dev in range(n_ssd):
        x = ssd_x_start + dev * (seg_w + ssd_gap)
        # label
        svg += text(x + seg_w/2, ssd_y - 10, f"SSD{dev}", size=13, anchor="middle", weight="bold")
        # show up to 3 segments, each with 4 units
        for seg in range(3):
            svg += rect(x, ssd_y + seg * (seg_h + 4), seg_w, seg_h, "#f5f5f5",
                        label=f"seg@{seg*units_per_seg*4096}", label_color="#666")
            # 4 unit ticks
            for u in range(units_per_seg):
                ux = x + u * (seg_w / units_per_seg)
                svg += f'  <line x1="{ux}" y1="{ssd_y + seg*(seg_h+4)}" x2="{ux}" y2="{ssd_y + seg*(seg_h+4)+seg_h}" stroke="#aaa" stroke-width="1"/>\n'

    # arrows from tokens to devices (sample first few)
    for i in (0, 1, 7, 8, 15, 16, 23, 31):
        dev = ((i * 17) % 2229299) % n_ssd
        tx = start_x + i * (token_w + gap) + token_w/2
        ty = start_y + token_h
        dx = ssd_x_start + dev * (seg_w + ssd_gap) + seg_w/2
        dy = ssd_y - 10
        svg += arrow(tx, ty, dx, dy, color=colors[dev])

    # legend / formula
    svg += text(start_x, H - 60,
                "哈希公式：device = ((token_idx * STEP_IDX + layer_id * STEP_LAYER) % PRIME) % N_SSD",
                size=12)
    svg += text(start_x, H - 40,
                "同盘内连续 token 按追加顺序填入 segment，满 super page 后以整段连续 IO 下盘",
                size=12)
    svg += text(start_x, H - 20,
                "super page 对齐保证 topk 加载时能从不同 channel/die/plane 并行读取",
                size=12)
    svg += svg_footer()
    write_svg("07_full_kv_scatter.svg", svg)


def gen_topk_load():
    W, H = 900, 560
    svg = svg_header(W, H)
    svg += marker_def()
    svg += text(W/2, 30, "图 2：decode 阶段 topk token 经 slot_table + plan() 定位并输出 descriptor buffer",
                size=16, anchor="middle", weight="bold")

    y = 70
    # topk tokens
    svg += text(50, y, "sparse topk tokens (layer=L)", size=13, weight="bold")
    tokens = [23, 45, 46, 78, 79, 80, 151]
    tx = 50
    for t in tokens:
        svg += rect(tx, y + 10, 36, 30, "#fff3cd", label=str(t), label_color="#333")
        tx += 46

    # slot table
    y += 70
    svg += text(50, y, "slot_table[layer][token] → packed entry (offset | VALID)", size=13, weight="bold")
    table_x = 50
    table_y = y + 15
    for i, t in enumerate(tokens):
        off = 0x10000 + i * 4096
        entry = f"0x{off:012x}"
        svg += rect(table_x + i * 100, table_y, 90, 34, "#e2e6ea", label=f"t={t}", label_color="#333")
        svg += text(table_x + i * 100 + 45, table_y + 48, entry, size=9, anchor="middle")

    # locate_batch
    y = table_y + 80
    svg += text(50, y, "locate_batch()：按位置哈希确定目标 SSD", size=13, weight="bold")
    devs = [3, 0, 5, 2, 7, 1, 6]
    for i, d in enumerate(devs):
        svg += rect(50 + i * 100, y + 15, 90, 30, "#d4edda", label=f"SSD{d}", label_color="#333")

    # descriptor buffer
    y += 90
    svg += text(50, y, "plan() 输出 descriptor buffer（固定地址，逐 step 覆写）", size=13, weight="bold")
    hdr_w = 120
    svg += rect(50, y + 20, hdr_w, 40, "#f8d7da", label="header", label_color="#333")
    svg += text(50 + hdr_w/2, y + 75, "count, layer_id", size=10, anchor="middle")
    entry_w = 110
    entry_h = 70
    for i in range(len(tokens)):
        ex = 50 + hdr_w + 10 + i * (entry_w + 6)
        svg += rect(ex, y + 20, entry_w, entry_h, "#d1ecf1", label=f"entry[{i}]", label_color="#333")
        lines = [
            f"ssd_id={devs[i]}",
            "flags=DISK",
            f"lba=0x{0x10000+i*4096:x}",
            f"dst={i*4096}",
        ]
        for j, line in enumerate(lines):
            svg += text(ex + 5, y + 40 + j * 13, line, size=9)

    # GPU direct read
    y += entry_h + 70
    svg += text(50, y, "GPU 直通存储算子按 descriptor 并行发起加载", size=13, weight="bold")
    for i, d in enumerate(devs):
        ex = 50 + hdr_w + 10 + i * (entry_w + 6) + entry_w/2
        dy = y + 70
        svg += rect(ex - 20, dy, 40, 30, "#cce5ff", label="SSD", label_color="#333")
        svg += arrow(ex, y + 20, ex, dy - 5)

    svg += text(50, H - 25,
                "说明：若 token 仍在主机段缓冲（IN_BUF），plan() 直接 memcpy 到 staging 并标记 HOST_READY，GPU 跳过该条 IO。",
                size=11)
    svg += svg_footer()
    write_svg("07_topk_load.svg", svg)


if __name__ == "__main__":
    gen_full_kv_scatter()
    gen_topk_load()
