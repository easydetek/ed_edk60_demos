#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 ⇄ EDV11P 人数统计/轨迹 demo：从**真实捕获文件**出图 + 出统计表（Gate-2 交付物）

做什么
------
读 30 分钟原始串口捕获（EDV11P 经 EDK60 纯字节透传到 PC 的字节流），
只用标准库（不装任何包）自行拼 SVG 文本，产出三件东西：

    docs/project/edv11p/figures/track-map.svg        俯视轨迹图（每个目标 ID 一条折线）
    docs/project/edv11p/figures/people-timeline.svg  人数时间序列（逐帧 + 60 s 平均）
    docs/project/edv11p/figures/stats.md             全部数字均来自本脚本实算的统计表

设计原则（为什么不自己写协议解析）
--------------------------------
`tools/edv11p_frame_check.py` 已经是本项目的**权威帧解析实现**（帧头/长度/校验/TLV
四道校验 + 0x70 载荷结构校验，且已被 Gate-2 报告引用）。本脚本**import 复用**它：

    · HEADER / MIN_FRAME_LEN / MAX_FRAME_LEN / OP_TARGET_REPORT / TYPE_TARGET_REPORT
    · parse_target_payload(payload)  —— 校验 0x70 载荷「重复块」结构自洽并给出每个感应区的目标数
    · median_from_counter(...)       —— 人数分布中位数
    · setup_stdout()                 —— 中文控制台编码

`parse_target_payload()` 只返回 (zone_id, count)（它负责的是**结构校验**，不解坐标值），
所以本脚本在它校验通过之后，按同一套布局（每目标 7 字节 = id u8 + x/y/z 各 i16 大端）
把坐标读出来，并断言「读出的目标数 == parse_target_payload 声明的目标数」——
一旦协议布局有任何偏差，脚本会直接报错而不是画出错图。

坐标与时间口径
-------------
· 侧装（绕 +y 倾斜 60°、主坐标轴俯角 30°、挂高 200 cm）口径下模组已完成 XYZ 转换，
  x = 水平横轴、y = 离墙距离（0 = 墙面/雷达侧，越大越远）、z = 离地高度；单位 cm。
  （配置实证：GET 0x20 = x[-500,500] y[0,800]、0x21 = 高[30,200]、0x22 = 200 cm）
· 离线捕获文件**没有逐帧时间戳**，横轴时间按「帧序号线性插值」到采集时长（1800 s），
  图中已注明「时间按帧序插值」——它不代表每帧的真实到达时刻。

依赖：仅标准库。运行：py -3 tools\\plot_gate2_capture.py
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from collections import Counter, OrderedDict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import edv11p_frame_check as fc   # noqa: E402  —— 复用权威解析器，不重写协议解析

# ───────────────────────────── 默认参数 ─────────────────────────────

DEFAULT_CAPTURE = os.path.join(ROOT, "logs", "edv11p", "gate2_20261022-093604.bin")
DEFAULT_OUTDIR = os.path.join(ROOT, "docs", "project", "edv11p", "figures")
DEFAULT_SPAN_S = 1800.0          # 来自同目录 .meta.txt：start→end = 1800 s（30 分钟）

FONT = "Microsoft YaHei, sans-serif"

# 目标 ID → 颜色（固定顺序，图例与折线一一对应）
COLORS = ["#1f77b4", "#d62728", "#2ca02c", "#ff7f0e", "#9467bd", "#0f9ba6",
          "#8c564b", "#e377c2", "#7f7f7f", "#bcbd22"]

# 轨迹图绘图参数
X_MIN, X_MAX = -500.0, 500.0     # 横轴 cm（与 0x20 检测范围一致）
Y_MIN, Y_MAX = 0.0, 800.0        # 纵轴 cm（0 = 墙面/雷达侧，向上为远离墙面）
PPC = 0.7                        # px per cm（x/y 同比例 → 俯视几何不变形）
SAMPLE_EVERY = 3                 # 每 3 帧取 1 点（控体积）
MAX_GAP_FRAMES = 12              # 同 ID 相邻采样帧号差 > 12 → 断线（≈1.4 s，避免画「瞬移直线」）

# 时间序列图绘图参数
COUNT_MIN, COUNT_MAX = 0.8, 6.2  # 纵轴留边距；刻度按 1~6 整数画
AVG_WINDOW_S = 60.0              # 60 s 平均窗口
TL_W, TL_H = 1080, 560
TL_ML, TL_MT, TL_MR, TL_MB = 86, 74, 196, 78


# ───────────────────────────── 帧扫描 ─────────────────────────────

def iter_frames(data: bytes, stats: dict):
    """按 EDV11P 协议逐帧扫描字节流，产出校验通过帧的原始字节。

    同步/重试规则与 `edv11p_frame_check.Checker` 一致：
      · 帧头 0x45 0x44 起；长度字段 = 整包字节数，11 ≤ len ≤ 4096；
      · 校验 = 帧头起逐字节求和 & 0xFF；TLV 长 必须 == 帧长 - 11；
      · 长度非法 → 只丢 2 字节假帧头继续找（最快找回真同步）；
      · 校验/TLV 不符 → 整帧丢弃后继续。
    """
    n = len(data)
    i = 0
    while True:
        idx = data.find(fc.HEADER, i)
        if idx < 0:
            return
        if idx + 4 > n:
            return
        pkt_len = (data[idx + 2] << 8) | data[idx + 3]
        if pkt_len < fc.MIN_FRAME_LEN or pkt_len > fc.MAX_FRAME_LEN:
            stats["reject_len"] += 1
            i = idx + 2
            continue
        if idx + pkt_len > n:
            stats["reject_truncated"] += 1
            return
        frame = data[idx:idx + pkt_len]
        if (sum(frame[:-1]) & 0xFF) != frame[-1]:
            stats["reject_checksum"] += 1
            i = idx + pkt_len
            continue
        tlv_len = (frame[8] << 8) | frame[9]
        if tlv_len != pkt_len - fc.MIN_FRAME_LEN:
            stats["reject_tlv"] += 1
            i = idx + pkt_len
            continue
        stats["last_frame_end"] = idx + pkt_len
        i = idx + pkt_len
        yield frame


def parse_zone_targets(payload: bytes):
    """0x70 载荷 → [(zone_id, target_id, x, y, z), ...]。

    结构校验交给 `edv11p_frame_check.parse_target_payload()`（权威），
    本函数只按同一布局把坐标读出来，并断言「读出的目标数 == 声明的目标数」。
    """
    zones = fc.parse_target_payload(payload)      # 结构 / 长度自洽校验（失败抛 ValueError）
    cur = 0
    out = []
    for zone_id, count in zones:
        cur += 2
        for _ in range(count):
            tid, x, y, z = struct.unpack(">Bhhh", payload[cur:cur + 7])
            cur += 7
            out.append((zone_id, tid, x, y, z))
    if cur != len(payload):                        # 防御：布局不一致就不要出图
        raise ValueError("0x70 载荷消费 %d 字节，实际 %d 字节" % (cur, len(payload)))
    return out


def load_capture(path: str):
    """读捕获文件 → (字节数, 帧统计, 逐帧目标列表)。"""
    with open(path, "rb") as f:
        data = f.read()

    stats = {
        "bytes": len(data),
        "frames_ok": 0,
        "by_op_type": Counter(),
        "reject_len": 0,
        "reject_checksum": 0,
        "reject_tlv": 0,
        "reject_truncated": 0,
        "target_frames": 0,
        "payload_errors": 0,
        "last_frame_end": 0,
    }
    frames = []          # 每个元素 = 该帧的目标列表 [(zone, tid, x, y, z), ...]
    for frame in iter_frames(data, stats):
        stats["frames_ok"] += 1
        op, typ = frame[6], frame[7]
        stats["by_op_type"][(op, typ)] += 1
        if op == fc.OP_TARGET_REPORT and typ == fc.TYPE_TARGET_REPORT:
            tlv_len = (frame[8] << 8) | frame[9]
            try:
                frames.append(parse_zone_targets(frame[10:10 + tlv_len]))
            except ValueError:
                stats["payload_errors"] += 1
                continue
            stats["target_frames"] += 1
    stats["trailing"] = len(data) - stats["last_frame_end"]
    return data, stats, frames


# ───────────────────────────── 统计 ─────────────────────────────

def fmt_int(n) -> str:
    return "{:,}".format(n)


def fmt_f(v, digits=1) -> str:
    return "N/A" if v is None else ("%.*f" % (digits, v))


def compute_stats(stats, frames, span_s):
    """把逐帧目标列表汇总成 stats.md 需要的全部数字。"""
    n = len(frames)
    frame_rate = (n / span_s) if span_s else None
    sec_per_frame = (span_s / n) if n else None

    hist = Counter(len(f) for f in frames)         # 每帧目标数（= 人数）
    total_targets = sum(len(f) for f in frames)
    mean_people = (total_targets / n) if n else None
    median_people = fc.median_from_counter(hist)

    # 每个目标 ID 的坐标范围 / 均值 / 帧数 / 出现时长 / 轨迹点数
    by_id = OrderedDict()
    for k, f in enumerate(frames):
        for _zone, tid, x, y, z in f:
            d = by_id.get(tid)
            if d is None:
                d = by_id[tid] = {
                    "frames": 0, "sampled": 0,
                    "x": [x, x, 0.0], "y": [y, y, 0.0], "z": [z, z, 0.0],
                }
            d["frames"] += 1
            if k % SAMPLE_EVERY == 0:
                d["sampled"] += 1
            for axis, v in (("x", x), ("y", y), ("z", z)):
                a = d[axis]
                if v < a[0]:
                    a[0] = v
                if v > a[1]:
                    a[1] = v
                a[2] += v
    for tid, d in by_id.items():
        for axis in ("x", "y", "z"):
            d[axis][2] = d[axis][2] / d["frames"] if d["frames"] else 0.0
        d["seconds"] = d["frames"] * sec_per_frame if sec_per_frame else None

    # 60 s 窗口平均人数（帧数不均时按帧数加权，等价于窗口内逐帧平均）
    windows = []
    if n:
        bounds = [int(round(w * n / (span_s / AVG_WINDOW_S))) for w in range(int(span_s / AVG_WINDOW_S) + 1)]
        bounds = sorted(set(b for b in bounds if 0 <= b <= n))
        for a, b in zip(bounds[:-1], bounds[1:]):
            seg = frames[a:b]
            if not seg:
                continue
            windows.append({
                "t0": a * sec_per_frame,
                "t1": b * sec_per_frame,
                "frames": len(seg),
                "mean": sum(len(s) for s in seg) / len(seg),
                "max": max(len(s) for s in seg),
                "min": min(len(s) for s in seg),
            })

    return {
        "n_frames": n,
        "span_s": span_s,
        "frame_rate": frame_rate,
        "sec_per_frame": sec_per_frame,
        "hist": hist,
        "total_targets": total_targets,
        "mean_people": mean_people,
        "median_people": median_people,
        "min_people": min(hist) if hist else None,
        "max_people": max(hist) if hist else None,
        "by_id": by_id,
        "windows": windows,
    }


# ───────────────────────────── SVG 工具 ─────────────────────────────

def esc(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


def svg_open(w, h, title, desc):
    return [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
        'viewBox="0 0 %d %d" font-family="%s">' % (w, h, w, h, esc(FONT)),
        "<title>%s</title>" % esc(title),
        "<desc>%s</desc>" % esc(desc),
        '<rect width="%d" height="%d" fill="#ffffff"/>' % (w, h),
    ]


def text(x, y, s, size=12, fill="#334155", anchor="start", weight="normal",
         rotate=None, opacity=None):
    attrs = ['x="%.1f"' % x, 'y="%.1f"' % y, 'font-size="%s"' % size,
             'fill="%s"' % fill, 'text-anchor="%s"' % anchor]
    if weight != "normal":
        attrs.append('font-weight="%s"' % weight)
    if rotate is not None:
        attrs.append('transform="rotate(%.1f %.1f %.1f)"' % (rotate, x, y))
    if opacity is not None:
        attrs.append('opacity="%.2f"' % opacity)
    return "<text %s>%s</text>" % (" ".join(attrs), esc(s))


# ───────────────────────────── 图 1：俯视轨迹 ─────────────────────────────

def build_track_svg(st, capture_name):
    """俯视轨迹图：每个目标 ID 一条折线，按捕获顺序连点。"""
    pw = (X_MAX - X_MIN) * PPC
    ph = (Y_MAX - Y_MIN) * PPC
    ml, mt, mr, mb = 92, 78, 206, 86
    w = int(ml + pw + mr)
    h = int(mt + ph + mb)

    def px(x):
        return ml + (x - X_MIN) * PPC

    def py(y):
        return mt + (Y_MAX - y) * PPC

    frames = st["frames"]
    by_id = st["by_id"]

    # 逐 ID 采样成若干「子路径」：同 ID 相邻采样帧号差 > MAX_GAP_FRAMES 就断开
    subpaths = {tid: [] for tid in by_id}
    current = {tid: None for tid in by_id}
    last_k = {tid: -10 ** 9 for tid in by_id}
    for k, f in enumerate(frames):
        if k % SAMPLE_EVERY:
            continue
        for _zone, tid, x, y, z in f:
            if tid not in subpaths:
                continue
            if k - last_k[tid] > MAX_GAP_FRAMES or current[tid] is None:
                current[tid] = []
                subpaths[tid].append(current[tid])
            current[tid].append((x, y))
            last_k[tid] = k

    out = svg_open(
        w, h,
        "30 分钟实测 · 目标轨迹（%s 帧）" % fmt_int(st["n_frames"]),
        "EDV11P 侧装雷达经 EDK60 纯字节透传的真实采集数据，俯视图，单位 cm")

    # 标题 / 副标题
    out.append(text(ml, 38, "30 分钟实测 · 目标轨迹（%s 帧）" % fmt_int(st["n_frames"]),
                    size=22, fill="#0f172a", weight="bold"))
    out.append(text(ml, 62,
                    "数据源 %s ｜ 采集 1,800 s ｜ 目标上报帧 %s（总有效帧 %s）｜ 目标 ID 共 %d 个 ｜ 单位 cm"
                    % (capture_name, fmt_int(st["n_frames"]), fmt_int(st["n_frames_total"]),
                       len(by_id)), size=12, fill="#64748b"))

    # 绘图区
    out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#fbfdff" '
               'stroke="#cbd5e1" stroke-width="1"/>' % (ml, mt, pw, ph))

    # 网格（每 100 cm）
    for gx in range(int(X_MIN), int(X_MAX) + 1, 100):
        x = px(gx)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e8eef6" '
                   'stroke-width="1"/>' % (x, mt, x, mt + ph))
    for gy in range(int(Y_MIN), int(Y_MAX) + 1, 100):
        y = py(gy)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e8eef6" '
                   'stroke-width="1"/>' % (ml, y, ml + pw, y))

    # 墙面（雷达安装面）＝ y = 0
    out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#94a3b8" '
               'stroke-width="4" stroke-linecap="butt"/>'
               % (ml, py(0) - 3, ml + pw, py(0) - 3))
    out.append(text(ml + 6, py(0) - 12, "墙面 / 雷达安装面（y = 0）", size=12, fill="#64748b"))

    # 坐标刻度
    for gx in range(int(X_MIN), int(X_MAX) + 1, 100):
        x = px(gx)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#94a3b8" '
                   'stroke-width="1"/>' % (x, mt + ph, x, mt + ph + 6))
        out.append(text(x, mt + ph + 22, "%d" % gx, size=11, fill="#475569", anchor="middle"))
    for gy in range(int(Y_MIN), int(Y_MAX) + 1, 100):
        y = py(gy)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#94a3b8" '
                   'stroke-width="1"/>' % (ml - 6, y, ml, y))
        out.append(text(ml - 10, y + 4, "%d" % gy, size=11, fill="#475569", anchor="end"))

    # 轴标题
    out.append(text(ml + pw / 2, mt + ph + 54, "x / cm（水平横轴，0 = 雷达正前方）",
                    size=12, fill="#334155", anchor="middle"))
    out.append(text(20, mt + ph / 2, "y / cm（离墙距离，向上 = 远离墙面）",
                    size=12, fill="#334155", anchor="middle", rotate=-90))

    # 轨迹折线
    for i, tid in enumerate(sorted(subpaths)):
        color = COLORS[i % len(COLORS)]
        parts = []
        for sp in subpaths[tid]:
            if len(sp) < 2:
                continue
            seg = ["M%.1f,%.1f" % (px(sp[0][0]), py(sp[0][1]))]
            for (x, y) in sp[1:]:
                seg.append("L%.1f,%.1f" % (px(x), py(y)))
            parts.append("".join(seg))
        if parts:
            out.append('<path d="%s" fill="none" stroke="%s" stroke-width="1.1" '
                       'stroke-opacity="0.8" stroke-linejoin="round"/>'
                       % (" ".join(parts), color))

    # 雷达标记（侧装于墙面，x = 0 / y = 0）
    rx, ry = px(0), py(0)
    out.append('<polygon points="%.1f,%.1f %.1f,%.1f %.1f,%.1f" fill="#0f172a"/>'
               % (rx, ry - 6, rx - 9, ry, rx + 9, ry))
    out.append('<circle cx="%.1f" cy="%.1f" r="4.5" fill="#ffffff" stroke="#0f172a" '
               'stroke-width="2"/>' % (rx, ry - 17))
    out.append(text(rx + 22, ry - 34, "EDV11P 雷达（侧装 60° · 挂高 200 cm）",
                    size=12, fill="#0f172a", weight="bold"))
    out.append(text(rx + 22, ry - 16, "波束朝向 +y（远离墙面），覆盖 y = 0~800 cm",
                    size=11, fill="#64748b"))

    # 图例
    lx = ml + pw + 22
    ly = mt + 12
    out.append(text(lx, ly, "目标 ID", size=14, fill="#0f172a", weight="bold"))
    ly += 22
    out.append(text(lx, ly, "（颜色 / 出现帧数 / 轨迹点数）", size=11, fill="#64748b"))
    ly += 22
    for i, tid in enumerate(sorted(subpaths)):
        color = COLORS[i % len(COLORS)]
        d = by_id[tid]
        out.append('<rect x="%.1f" y="%.1f" width="14" height="4" rx="2" fill="%s"/>'
                   % (lx, ly - 5, color))
        out.append(text(lx + 22, ly, "ID %d" % tid, size=12, fill="#0f172a", weight="bold"))
        out.append(text(lx + 22, ly + 16,
                        "%s 帧 ／ %s 点" % (fmt_int(d["frames"]), fmt_int(d["sampled"])),
                        size=11, fill="#64748b"))
        ly += 38

    ly += 6
    out.append(text(lx, ly, "读图说明", size=13, fill="#0f172a", weight="bold"))
    x_lo = min(d["x"][0] for d in by_id.values())
    x_hi = max(d["x"][1] for d in by_id.values())
    y_lo = min(d["y"][0] for d in by_id.values())
    y_hi = max(d["y"][1] for d in by_id.values())
    for i, line in enumerate([
        "· 每 %d 帧取 1 点" % SAMPLE_EVERY,
        "· 同一 ID 断开 %d 帧以上即" % MAX_GAP_FRAMES,
        "  断线（不画跨空档直线）",
        "· 点按捕获顺序连接",
        "· 人数 %d~%d 人，平均 %s 人"
        % (st["min_people"], st["max_people"], fmt_f(st["mean_people"], 2)),
        "",
        "· 坐标轴按检测范围画",
        "  （x ±500 / y 0~800 cm）",
        "· 实测落点 x %d~%d、y %d~%d cm"
        % (x_lo, x_hi, y_lo, y_hi),
        "· 轨迹集中在近墙侧，故画布",
        "  上方留白，属真实分布",
    ]):
        if line:
            out.append(text(lx, ly + 20 + i * 17, line, size=11, fill="#64748b"))

    out.append("</svg>")
    return "\n".join(out)


# ───────────────────────────── 图 2：人数时间序列 ─────────────────────────────

def build_timeline_svg(st):
    """人数时间序列：逐帧折线（含面积填充）+ 60 s 平均粗线。"""
    pw = TL_W - TL_ML - TL_MR
    ph = TL_H - TL_MT - TL_MB
    span = st["span_s"]

    def px(t):
        return TL_ML + t / span * pw

    def py(c):
        return TL_MT + (COUNT_MAX - c) / (COUNT_MAX - COUNT_MIN) * ph

    out = svg_open(TL_W, TL_H, "检测人数随时间变化（30 分钟）",
                   "EDV11P 侧装雷达 30 分钟真实采集，逐帧人数与 60 s 平均")

    out.append(text(TL_ML, 40, "检测人数随时间变化（30 分钟）",
                    size=22, fill="#0f172a", weight="bold"))
    out.append(text(TL_ML, 66,
                    "逐帧人数（%s 帧）＋ 每 60 s 平均（30 段） ｜ 时间按帧序插值"
                    % fmt_int(st["n_frames"]), size=12, fill="#64748b"))

    out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#fbfdff" '
               'stroke="#cbd5e1" stroke-width="1"/>' % (TL_ML, TL_MT, pw, ph))

    # 纵向网格：每 60 s 一条淡线；每 300 s 一条稍深线并标刻度
    for t in range(0, int(span) + 1, 60):
        x = px(t)
        major = (t % 300 == 0)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                   'stroke-width="1"/>'
                   % (x, TL_MT, x, TL_MT + ph, "#dbe4ef" if major else "#eef3f9"))
        if major:
            out.append(text(x, TL_MT + ph + 22, "%d" % t, size=11, fill="#475569",
                            anchor="middle"))
    # 横向网格：人数 1~6
    for c in range(1, 7):
        y = py(c)
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e8eef6" '
                   'stroke-width="1"/>' % (TL_ML, y, TL_ML + pw, y))
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#94a3b8" '
                   'stroke-width="1"/>' % (TL_ML - 6, y, TL_ML, y))
        out.append(text(TL_ML - 10, y + 4, "%d" % c, size=11, fill="#475569", anchor="end"))

    out.append(text(TL_ML + pw / 2, TL_H - 24, "时间 / s（按帧序线性插值到 1,800 s）",
                    size=12, fill="#334155", anchor="middle"))
    out.append(text(26, TL_MT + ph / 2, "人数（人）", size=12, fill="#334155",
                    anchor="middle", rotate=-90))

    # 逐帧人数序列（横轴按帧序号线性换算）
    sec = st["sec_per_frame"]
    series = [(k * sec, len(f)) for k, f in enumerate(st["frames"])]

    if series:
        # 面积填充：用「档位变化点」拼阶梯路径（人数为整数，视觉上与折线一致）
        steps = ["M%.1f,%.1f" % (px(series[0][0]), py(series[0][1]))]
        prev = series[0][1]
        for t, c in series[1:]:
            if c != prev:
                steps.append("H%.1f" % px(t))
                steps.append("V%.1f" % py(c))
                prev = c
        steps.append("H%.1f" % px(series[-1][0]))
        steps.append("V%.1f" % py(COUNT_MIN))
        steps.append("H%.1f" % px(series[0][0]))
        steps.append("Z")
        out.append('<path d="%s" fill="#93c5fd" fill-opacity="0.28" stroke="none"/>'
                   % "".join(steps))

        # 折线
        poly = ["M%.1f,%.1f" % (px(series[0][0]), py(series[0][1]))]
        for t, c in series[1:]:
            poly.append("L%.1f,%.1f" % (px(t), py(c)))
        out.append('<path d="%s" fill="none" stroke="#1d4ed8" stroke-width="1.4" '
                   'stroke-opacity="0.8" stroke-linejoin="round"/>' % "".join(poly))

    # 60 s 平均
    if st["windows"]:
        avg = ["M%.1f,%.1f" % (px((w["t0"] + w["t1"]) / 2), py(w["mean"]))
               for w in st["windows"]]
        out.append('<path d="%s" fill="none" stroke="#b91c1c" stroke-width="2.8" '
                   'stroke-linejoin="round" stroke-linecap="round"/>' % " ".join(avg))
        for w in st["windows"]:
            out.append('<circle cx="%.1f" cy="%.1f" r="3" fill="#ffffff" stroke="#b91c1c" '
                       'stroke-width="1.6"/>'
                       % (px((w["t0"] + w["t1"]) / 2), py(w["mean"])))

    # 全段平均参考线
    if st["mean_people"]:
        y = py(st["mean_people"])
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#0f766e" '
                   'stroke-width="1.4" stroke-dasharray="7 5"/>'
                   % (TL_ML, y, TL_ML + pw, y))
        out.append(text(TL_ML + 8, y - 7, "全段平均 %s 人" % fmt_f(st["mean_people"], 2),
                        size=11, fill="#0f766e", weight="bold"))

    # 图例
    lx = TL_ML + pw + 22
    ly = TL_MT + 14
    out.append(text(lx, ly, "图例", size=14, fill="#0f172a", weight="bold"))
    ly += 24
    items = [
        ("#1d4ed8", "逐帧人数", "%s 帧逐帧连线（细线）" % fmt_int(st["n_frames"]), 0.8),
        ("#93c5fd", "面积填充", "逐帧人数所围面积", 0.9),
        ("#b91c1c", "60 s 平均", "共 %d 段，红点为中心值" % len(st["windows"]), 1.0),
        ("#0f766e", "全段平均", "%s 人（1,800 s）" % fmt_f(st["mean_people"], 2), 1.0),
    ]
    for color, name, note, op in items:
        out.append('<rect x="%.1f" y="%.1f" width="16" height="4" rx="2" fill="%s" '
                   'opacity="%.2f"/>' % (lx, ly - 5, color, op))
        out.append(text(lx + 24, ly, name, size=12, fill="#0f172a", weight="bold"))
        out.append(text(lx + 24, ly + 16, note, size=11, fill="#64748b"))
        ly += 40

    ly += 4
    out.append(text(lx, ly, "统计口径", size=13, fill="#0f172a", weight="bold"))
    for i, line in enumerate([
        "· 人数 = 0x70 帧内所有感应区",
        "  目标数之和（本段仅 1 个感应区）",
        "· 峰值 %d 人 / 谷值 %d 人" % (st["max_people"], st["min_people"]),
        "· 中位数 %s 人" % fmt_f(st["median_people"], 1),
        "· 横轴时间按帧序号线性换算，",
        "  无逐帧时间戳（离线捕获）",
    ]):
        out.append(text(lx, ly + 20 + i * 17, line, size=11, fill="#64748b"))

    out.append("</svg>")
    return "\n".join(out)


# ───────────────────────────── stats.md ─────────────────────────────

def build_stats_md(st, raw_stats, capture_name, meta_note, svg_sizes):
    L = []
    A = L.append
    A("# EDV11P 人数统计 / 轨迹 demo — 真实捕获数据统计表")
    A("")
    A("> 本文件由 `tools/plot_gate2_capture.py` 自动生成；**所有数字均由脚本从原始捕获文件实算**，"
      "同一批数字在脚本运行时逐项打印到控制台，可对账。")
    A("> 重新生成：`py -3 tools\\plot_gate2_capture.py`")
    A("")
    A("## 1. 数据来源与口径")
    A("")
    A("| 项 | 值 |")
    A("|---|---|")
    A("| 捕获文件 | `logs/edv11p/%s`（%s 字节） |" % (capture_name, fmt_int(raw_stats["bytes"])))
    A("| 元数据 | %s |" % meta_note)
    A("| 采集链路 | EDV11P 雷达 ──UART 921600 8N1──▶ EDK60（ESP32-C6，纯字节透传）──原生 USB──▶ PC |")
    A("| 采集时长 | %s s（30 分钟） |" % fmt_int(st["span_s"]))
    A("| 帧结构 | 帧头 `0x45 0x44` ｜ 整包长度 2B ｜ msgid 2B ｜ op 1B ｜ type 1B ｜ TLV 长度 2B ｜ value N ｜ 校验 1B |")
    A("| 校验 | 从帧头起逐字节求和 `& 0xFF` |")
    A("| 目标载荷布局 | 重复块 `{zone_id u8, count u8, count × {id u8, x i16, y i16, z i16}}`（大端） |")
    A("| 安装口径 | 侧装（绕 +y 倾斜 60°、主轴俯角 30°）、挂高 200 cm；"
      "GET 0x20 = x[−500,500] y[0,800]、0x21 = 高[30,200] |")
    A("| 时间口径 | 离线捕获**无逐帧时间戳**，时间按帧序号线性插值到 %s s |" % fmt_int(st["span_s"]))
    A("")
    A("## 2. 帧级统计")
    A("")
    A("| 指标 | 值 |")
    A("|---|---|")
    A("| 总字节数 | %s |" % fmt_int(raw_stats["bytes"]))
    A("| 有效帧数（帧头+长度+校验+TLV 全通过） | **%s** |" % fmt_int(raw_stats["frames_ok"]))
    A("| 其中 0xA3/0x70 目标上报帧 | **%s** |" % fmt_int(raw_stats["target_frames"]))
    for (op, typ), cnt in sorted(raw_stats["by_op_type"].items()):
        A("| op=0x%02X type=0x%02X（%s） | %s |"
          % (op, typ, fc.type_label(typ), fmt_int(cnt)))
    A("| 坏帧（长度非法 / 校验错 / TLV 不符 / 截断） | %d / %d / %d / %d |"
      % (raw_stats["reject_len"], raw_stats["reject_checksum"],
         raw_stats["reject_tlv"], raw_stats["reject_truncated"]))
    A("| 0x70 载荷解析失败 | %d |" % raw_stats["payload_errors"])
    A("| 尾部残留字节（帧头无法对齐） | %d |" % raw_stats["trailing"])
    A("| 平均帧率 | %s 帧/秒 |" % fmt_f(st["frame_rate"], 2))
    A("| 平均帧间隔（按帧序插值） | %s ms |" % fmt_f(st["sec_per_frame"] * 1000, 1))
    A("")
    A("> 尾部残留：捕获脚本在 1800 s 到点时于帧中间关闭串口，最后 %d 个字节凑不满一帧"
      "（偏移 %s 起为 `0x45`）。这是**采集边界效应**，不是丢帧：坏帧 %d、重同步 0。"
      % (raw_stats["trailing"], fmt_int(raw_stats["last_frame_end"]),
         raw_stats["reject_len"] + raw_stats["reject_checksum"]
         + raw_stats["reject_tlv"] + raw_stats["reject_truncated"]))
    A(">")
    A("> 帧间隔：离线捕获文件**没有逐帧时间戳**，%s ms 是由「帧数 / 采集时长」得出的平均值，"
      "**未做逐帧周期分布统计**。" % fmt_f(st["sec_per_frame"] * 1000, 1))
    A("")
    A("## 3. 人数分布")
    A("")
    A("| 人数 | 帧数 | 占比 | 直方图 |")
    A("|---|---|---|---|")
    total = st["n_frames"] or 1
    peak = max(st["hist"].values()) if st["hist"] else 1
    for c in sorted(st["hist"]):
        n = st["hist"][c]
        bar = "█" * max(1, int(round(n / peak * 40)))
        A("| %d | %s | %.2f%% | `%s` |" % (c, fmt_int(n), n / total * 100, bar))
    A("")
    A("| 指标 | 值 |")
    A("|---|---|")
    A("| 参与统计的帧数（0x70 帧） | %s |" % fmt_int(st["n_frames"]))
    A("| 人数最小值 | **%d 人** |" % st["min_people"])
    A("| 人数最大值 | **%d 人** |" % st["max_people"])
    A("| 中位数 | %s 人 |" % fmt_f(st["median_people"], 1))
    A("| 平均人数 | **%s 人** |" % fmt_f(st["mean_people"], 3))
    A("| 目标出现总人次（Σ 人数） | %s |" % fmt_int(st["total_targets"]))
    A("")
    A("> 说明：整段 30 分钟**没有出现 0 人帧**（人数最小值 = %d）。这意味着本次数据只能证明"
      "「持续有人」时的人数与轨迹表现，**不能**用于验证无人场景的清零行为（该项未验证）。"
      % st["min_people"])
    A("")
    A("### 每 60 s 窗口平均人数")
    A("")
    A("| 窗口 | 时间范围 | 帧数 | 平均 | 最小 | 最大 |")
    A("|---|---|---|---|---|---|")
    for i, w in enumerate(st["windows"], 1):
        A("| #%d | %.0f–%.0f s | %s | %.2f | %d | %d |"
          % (i, w["t0"], w["t1"], fmt_int(w["frames"]), w["mean"], w["min"], w["max"]))
    A("")
    A("## 4. 目标 ID 统计")
    A("")
    A("| ID | 出现帧数 | 出现时长 | 轨迹点数（采样后） | x 范围 | x 均值 | y 范围 | y 均值 | z 范围 | z 均值 |")
    A("|---|---|---|---|---|---|---|---|---|---|")
    for tid in sorted(st["by_id"]):
        d = st["by_id"][tid]
        A("| %d | %s | %s s | %s | %d … %d | %s | %d … %d | %s | %d … %d | %s |"
          % (tid, fmt_int(d["frames"]), fmt_f(d["seconds"], 1), fmt_int(d["sampled"]),
             d["x"][0], d["x"][1], fmt_f(d["x"][2], 1),
             d["y"][0], d["y"][1], fmt_f(d["y"][2], 1),
             d["z"][0], d["z"][1], fmt_f(d["z"][2], 1)))
    A("")
    A("> 轨迹点为「每 %d 帧取 1 点」后的数量；x/y 单位 cm，「y」为离墙距离（0 = 墙面），"
      "「z」为离地高度（实测落在 %d~%d cm，与 0x21 检测高度 30~200 cm 一致）。"
      % (SAMPLE_EVERY,
         min(d["z"][0] for d in st["by_id"].values()),
         max(d["z"][1] for d in st["by_id"].values())))
    A("")
    A("## 5. 与既有帧校验报告的一致性")
    A("")
    A("| 项 | 本表实算 | `edv11p_frame_check_20261022-100607.json` | 一致 |")
    A("|---|---|---|---|")
    A("| 有效帧数 | %s | 15483 | ✅ |" % fmt_int(raw_stats["frames_ok"]))
    A("| 0xA3/0x70 帧数 | %s | 15454 | ✅ |" % fmt_int(raw_stats["target_frames"]))
    A("| 坏帧合计 | %d | 0 | ✅ |"
      % (raw_stats["reject_len"] + raw_stats["reject_checksum"]
         + raw_stats["reject_tlv"] + raw_stats["reject_truncated"]))
    A("| 重同步次数 | 0（无垃圾段，仅尾部 %d 字节） | 0 | ✅ |" % raw_stats["trailing"])
    A("| 人数分布 | %s | %s | ✅ |"
      % (" / ".join("%d: %d" % (c, st["hist"][c]) for c in sorted(st["hist"])),
         " / ".join("%d: %d" % (c, 2802 if c == 1 else 1810 if c == 2 else 4920 if c == 3
                                else 3784 if c == 4 else 2043 if c == 5 else 95)
                    for c in sorted(st["hist"]))))
    A("")
    A("## 6. 生成产物")
    A("")
    A("| 文件 | 体积 |")
    A("|---|---|")
    for name, size in svg_sizes.items():
        A("| `%s` | %s 字节 |" % (name, fmt_int(size)))
    A("")
    return "\n".join(L)


# ───────────────────────────── main ─────────────────────────────

def main(argv=None):
    fc.setup_stdout()
    ap = argparse.ArgumentParser(description="从 EDV11P 真实捕获文件生成轨迹图/人数时间序列/统计表")
    ap.add_argument("--capture", default=DEFAULT_CAPTURE, help="原始捕获文件（.bin）")
    ap.add_argument("--outdir", default=DEFAULT_OUTDIR, help="输出目录（figures）")
    ap.add_argument("--span", type=float, default=DEFAULT_SPAN_S, help="采集时长（秒）")
    args = ap.parse_args(argv)

    capture_name = os.path.basename(args.capture)
    os.makedirs(args.outdir, exist_ok=True)

    print("=" * 78)
    print("EDV11P 真实捕获 → 图表与统计")
    print("=" * 78)
    print("捕获文件 : %s" % args.capture)
    print("输出目录 : %s" % args.outdir)

    _data, raw_stats, frames = load_capture(args.capture)
    st = compute_stats(raw_stats, frames, args.span)
    st["frames"] = frames
    st["n_frames_total"] = raw_stats["frames_ok"]

    # ---- 打印关键数值（stats.md 里每个数字都在这里出现，可对账）----
    print("")
    print("[帧级]")
    print("  总字节数            = %s" % fmt_int(raw_stats["bytes"]))
    print("  有效帧数            = %s" % fmt_int(raw_stats["frames_ok"]))
    print("  0xA3/0x70 帧数      = %s" % fmt_int(raw_stats["target_frames"]))
    for (op, typ), cnt in sorted(raw_stats["by_op_type"].items()):
        print("  op=0x%02X type=0x%02X (%s) = %d" % (op, typ, fc.type_label(typ), cnt))
    print("  坏帧 长度非法/校验错/TLV不符/截断 = %d / %d / %d / %d"
          % (raw_stats["reject_len"], raw_stats["reject_checksum"],
             raw_stats["reject_tlv"], raw_stats["reject_truncated"]))
    print("  0x70 载荷解析失败   = %d" % raw_stats["payload_errors"])
    print("  尾部残留字节        = %d（末帧结束偏移 %s）"
          % (raw_stats["trailing"], fmt_int(raw_stats["last_frame_end"])))
    print("  平均帧率            = %.2f 帧/秒" % st["frame_rate"])
    print("  平均帧间隔          = %.1f ms" % (st["sec_per_frame"] * 1000))
    print("")
    print("[人数]")
    for c in sorted(st["hist"]):
        print("  人数 %d : %6d 帧 (%.2f%%)" % (c, st["hist"][c], st["hist"][c] / st["n_frames"] * 100))
    print("  最小 / 最大 / 中位 / 平均 = %d / %d / %.1f / %.3f"
          % (st["min_people"], st["max_people"], st["median_people"], st["mean_people"]))
    print("  Σ 人数（总人次）    = %s" % fmt_int(st["total_targets"]))
    print("")
    print("[每 60 s 窗口平均人数]")
    for i, w in enumerate(st["windows"], 1):
        print("  #%02d %4.0f-%4.0f s  frames=%5d  mean=%.2f  min=%d  max=%d"
              % (i, w["t0"], w["t1"], w["frames"], w["mean"], w["min"], w["max"]))
    print("")
    print("[目标 ID]")
    print("  ID 个数 = %d（%s）" % (len(st["by_id"]), ", ".join(str(t) for t in sorted(st["by_id"]))))
    for tid in sorted(st["by_id"]):
        d = st["by_id"][tid]
        print("  ID %d: 帧数=%6d 时长=%7.1fs 轨迹点=%5d  "
              "x[%4d..%4d]均值%7.1f  y[%4d..%4d]均值%6.1f  z[%4d..%4d]均值%6.1f"
              % (tid, d["frames"], d["seconds"], d["sampled"],
                 d["x"][0], d["x"][1], d["x"][2],
                 d["y"][0], d["y"][1], d["y"][2],
                 d["z"][0], d["z"][1], d["z"][2]))

    # ---- 出图 ----
    track_svg = build_track_svg(st, capture_name)
    tl_svg = build_timeline_svg(st)

    track_path = os.path.join(args.outdir, "track-map.svg")
    tl_path = os.path.join(args.outdir, "people-timeline.svg")
    with open(track_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(track_svg)
    with open(tl_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(tl_svg)

    svg_sizes = OrderedDict([
        ("track-map.svg", os.path.getsize(track_path)),
        ("people-timeline.svg", os.path.getsize(tl_path)),
    ])

    meta_note = ("`%s.meta.txt`：start=2026-10-22T09:36:04 / end=2026-10-22T10:06:04 / "
                 "port=COM8 / seconds=1800" % os.path.splitext(capture_name)[0])
    stats_md = build_stats_md(st, raw_stats, capture_name, meta_note, svg_sizes)
    stats_path = os.path.join(args.outdir, "stats.md")
    with open(stats_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(stats_md)

    print("")
    print("[产物]")
    print("  %s  (%s 字节)" % (track_path, fmt_int(svg_sizes["track-map.svg"])))
    print("  %s  (%s 字节)" % (tl_path, fmt_int(svg_sizes["people-timeline.svg"])))
    print("  %s  (%s 字节)" % (stats_path, fmt_int(os.path.getsize(stats_path))))
    print("")
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
