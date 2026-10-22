#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 ⇄ EDV11P 桥接帧校验 / 统计工具（Gate-2 关键证据工具）

用途
----
本项目链路是：  EDV11P 雷达模组 ──UART 921600 8N1──▶ ESP32-C6 ──原生 USB CDC──▶ PC
固件侧是「纯字节透传」，不做任何解析。所以「桥接有没有丢字节、有没有把帧搞乱」
只能由 PC 侧客观统计来证明。本脚本就干这件事：

    · 实时读串口（--port），或离线分析已保存的原始字节流（--file）；
    · 逐帧校验：帧头同步、长度字段、校验和、TLV 长度与帧长是否自洽；
    · 统计坏帧分类、假同步（重同步）次数、msgid 连续性、0x70 目标上报内容、
      帧率与字节率；
    · 按固定阈值给出 PASS / FAIL 结论，并落盘 JSON 报告 + 文本日志；
    · --raw-dump 把「解析不了」的字节段原样存成 unparsed_<n>.bin，
      这是排查「谁在往数据流里插日志」的关键手段。

协议基线（与 EDV11P 串口通用协议 V1.9 文档 + human_track_v1.4_side_mount.py 交叉核对）
----------------------------------------------------------------------------------
    波特率 921600 / 8N1，无流控，大端；帧结构（长度字段 = 整包字节数）：

    +--------+----------+------------+------+------+-----------+---------+------+
    | 0x45 44| 长度(2)  | msgid(2)   | op(1)|type(1)| TLV长(2) | value N | 校验1|
    +--------+----------+------------+------+------+-----------+---------+------+
    最短帧 11 字节（TLV 长为 0）。校验 = 从帧头起逐字节求和 & 0xFF。
    TLV 长 必须等于 「帧长 - 11」。
    op  : 0xA0 GET / 0xA1 SET / 0xA2 REPORT(应答) / 0xA3 STATUS(主动上报) / 0xA4 透传 / 0xA9 ACK
    msgid: 0 ~ 0xFFF0，到 0xFFF0 之后回绕到 0。注意：实测可能按命令类别各自独立计数，
           所以本工具同时给出「按 (op,type) 分组」的连续性视图，避免把分组计数误判成丢帧。

判定规则（P0 前提 + 六条硬判据；原文见 --help 与 RULES_TEXT，数值写死在 THRESHOLDS）
---------------------------------------------------------------------------------
    P0 有效帧数 ≥ 1                                        否则整份采集无效
    P1 未归属字节率 = (窗口内收字节 − 窗口内合法帧字节) / 窗口内收字节     = 0 才 PASS
                   0 < x < 0.1% → WARN；≥ 0.1% → FAIL（窗口从 T+10s 起算）
    P2 重同步绝对次数 ≤ max(1, round(3 × 时长/1800s))        30 分钟 → 3 次
    P3 坏帧率         = 坏帧数 / (好帧数 + 坏帧数)                 必须 < 0.1%
    P4 帧完整率       = 好帧数 / (好帧数 + 坏帧数 + 重同步)        必须 ≥ 99.9%
    P5 假同步丢帧估算率 = 重同步次数 / (好帧数 + 重同步次数)       必须 < 0.1%
    P6 msgid 跳号率   = 跳号次数 / 组内帧数（按 (op,type) 分组取最大组）必须 < 0.1%
    任一 FAIL 即 FAIL；WARN 不阻断但必须写进 Gate 记录；样本不足的项判 N/A。

另有静态门禁 --check-banner：扫源码/镜像/配置，输出文案里出现相邻字节 45 44
（ASCII "ED"，如 "EDK60"）即 FAIL —— 因为 45 44 就是帧头，会凭空造出假同步。

用法（详见 --help；本机 `python` 是应用商店别名不可用，要用 `py -3`）
--------------------------------------------------------------
    py -3 tools\\edv11p_frame_check.py --port COM6 --baud 921600 --duration 1800
    py -3 tools\\edv11p_frame_check.py --file capture.bin --duration 1800 --csv 人数.csv
    py -3 tools\\edv11p_frame_check.py --selftest          # 无硬件自测（合成流）
    py -3 tools\\edv11p_frame_check.py --check-banner       # 静态横幅门禁（不需要硬件）

    所有产物默认落在 logs\\edv11p\\（相对当前工作目录，可用 --out 覆盖）。

依赖
----
    只用标准库；--selftest / --file 完全不需要第三方包。
    仅 --port 模式需要 pyserial（py -3 -m pip install pyserial）。本机若没有会在报错里提示。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import struct
import sys
import tempfile
import time
from collections import Counter, OrderedDict

# ───────────────────────────── 协议常量 ─────────────────────────────

TOOL_NAME = "edv11p_frame_check.py"
TOOL_VERSION = "1.1.0"

HEADER = b"\x45\x44"          # 固定帧头
HEADER_B0 = 0x45              # 用于「末尾可能是半个帧头」的保留判断
MIN_FRAME_LEN = 11            # 2头 + 2长度 + 2msgid + 1op + 1type + 2TLV长 + 0值 + 1校验
MAX_FRAME_LEN = 4096          # 与参考上位机一致的上限，超出即视为非法长度
MSGID_MAX = 0xFFF0            # msgid 回绕点（到 0xFFF0 之后回到 0）
GATE2_DURATION = 1800.0       # Gate-2 要求：30 分钟连续运行（秒）
DEFAULT_OUTDIR = os.path.join("logs", "edv11p")   # 默认输出目录（相对当前工作目录，推荐在仓库根运行）

OP_NAMES = {
    0xA0: "GET",
    0xA1: "SET",
    0xA2: "REPORT",
    0xA3: "STATUS",
    0xA4: "透传",
    0xA9: "ACK",
}

TYPE_NAMES = {
    0x01: "产品信息", 0x02: "雷达模块信息", 0x05: "工作模式", 0x10: "上报频率",
    0x20: "检测范围", 0x21: "检测高度", 0x22: "挂高", 0x23: "运动灵敏度",
    0x24: "存在灵敏度", 0x25: "感应区数量", 0x26: "感应区设置", 0x27: "屏蔽区数量",
    0x28: "屏蔽区设置", 0x29: "目标消失延时", 0x2A: "存在开关", 0x2F: "出厂设置状态",
    0x30: "进入自学", 0x31: "退出自学", 0x32: "恢复自学参数", 0x33: "查询自学状态",
    0x70: "目标上报", 0x72: "跌倒上报", 0x73: "跌倒目标上报",
    0x80: "从机版本", 0x81: "同步MTU", 0x82: "OTA通知", 0x83: "OTA固件请求",
    0x84: "OTA升级结果", 0xF0: "错误码",
}

OP_TARGET_REPORT = 0xA3       # 0x70 目标上报是「主动状态上报」，op = 0xA3
TYPE_TARGET_REPORT = 0x70     # 目标上报 type = 0x70

# 判定阈值（写死在代码里，同时打印进 --help 与报告，便于 Gate 复核）
THRESHOLDS = {
    "unattributed_rate_max": 0.001,     # 未归属字节率 < 0.1%（=0 才 PASS；0<x<0.1% 记 WARN）
    "bad_frame_rate_max": 0.001,        # 坏帧率 < 0.1%
    "complete_rate_min": 0.999,         # 帧完整率 ≥ 99.9%（= 30 分钟 99.9% 的目标）
    "fake_sync_loss_rate_max": 0.001,   # 假同步丢帧估算率 < 0.1%
    "msgid_gap_rate_max": 0.001,        # msgid 跳号率 < 0.1%
}
RESYNC_LIMIT_30MIN = 3                 # 重同步绝对次数上限：3 次 / 30 分钟
DEFAULT_WARMUP_S = 10.0                # 未归属字节统计窗口的起算点（跳过上电 bootloader 文本）

# 判定规则原文（--help 与报告共用，避免两处不一致）
RULES_TEXT = f"""判定规则：P0 前提 + 六条硬判据（按优先级顺序判定，任一 FAIL 即 FAIL）
  P0 前提：有效帧数 ≥ 1                       否则整份采集无效，其余判据 SKIP
  P1 未归属字节率  = (窗口内收字节 − 窗口内合法帧字节) / 窗口内收字节
     · 窗口从 T+{DEFAULT_WARMUP_S:.0f}s 起算（跳过 bootloader/ROM 文本，可用 --warmup 调整）
     · 合法帧 = 帧头 + 长度 + 校验 + TLV 全通过；坏帧字节、垃圾字节、截断字节都算「未归属」
     · 判定：= 0 → PASS；0 < x < {THRESHOLDS['unattributed_rate_max']:.2%} → WARN（不判 FAIL，但必须写进 Gate 记录）；
             ≥ {THRESHOLDS['unattributed_rate_max']:.2%} → FAIL
  P2 重同步绝对次数 ≤ max(1, round({RESYNC_LIMIT_30MIN} × 时长/1800s))
     · 30 分钟 → 3 次；60 分钟 → 6 次；10 分钟 → 1 次。超出说明有字符在「持续」插入流里，
       不能靠比率摊薄掉（这是 P5 的相对率判据盖不住的情形）
  P3 坏帧率         = 坏帧数 / (好帧数 + 坏帧数)                 必须 < {THRESHOLDS['bad_frame_rate_max']:.2%}
     坏帧 = 长度非法 + 校验错 + TLV长度与帧长不符 + 截断帧（四类分别计数）
  P4 帧完整率       = 好帧数 / (好帧数 + 坏帧数 + 重同步)        必须 ≥ {THRESHOLDS['complete_rate_min']:.2%}
     ★ 这是 Gate-2「30 分钟帧完整率 ≥ 99.9%」的直接判定指标
  P5 假同步丢帧估算率 = 重同步次数 / (好帧数 + 重同步次数)       必须 < {THRESHOLDS['fake_sync_loss_rate_max']:.2%}
  P6 msgid 跳号率   = 跳号次数 / 组内帧数（按 (op,type) 分组取最大组） 必须 < {THRESHOLDS['msgid_gap_rate_max']:.2%}
     组内样本 < --msgid-min-samples 的组不参与判定；若「不分组」超标而「分组」全达标，
     报告会判定「msgid 按命令类别独立计数」，不算丢帧。

判据之间的关系与冲突处理（重要）
  · P1 比 P5 更严：P1 按「字节」精确归属，P5 按「事件」估算。未归属字节 > 0 时 P5 一定 > 0
    （反之不成立：字节可能插在帧尾对齐处，构不成重同步事件却仍污染流）→ 冲突时以 P1 为准。
  · 坏帧字节也算「未归属」，所以 P3 > 0 必然导致 P1 > 0 → P1 先判，P3 只用来说明「坏在哪一类」。
  · P2 与 P5 冲突时以 P2 为准：短采集里几次重同步能把比率摊到 0.1% 以下，但绝对次数照样是缺陷。
  · WARN 不改变 PASS，但必须在 Gate 记录里逐条列出（P1 的 WARN 尤其要说清来源字节段）。
  · Gate-2 引用判据（一句话，可直接抄）：「T-FC-01 的 未归属字节率 = 0 且 帧完整率 ≥ 99.9%，
    同时 重同步绝对次数 ≤ 3/30min、msgid 跳号率 < 0.1%，四项均由 tools\\edv11p_frame_check.py
    输出 JSON 的 checks[] 直接给出且退出码为 0。」

不影响 PASS/FAIL 但会进 warnings 的观察项
  · 采集中出现未识别的 op / type；0x70 载荷解析异常；采集时长不足 --duration 的 90%
  · msgid 重复（同一组内连续两次相同）；单段垃圾字节超过 1 KiB（疑似日志污染）
  · 未归属字节率的 WARN 区间（0 < x < 0.1%）
"""

# 疑似「日志污染」的特征串。
#   STRONG：≥5 字节的强特征，几乎不可能由二进制误码碰巧凑出来 → 命中即报，不看可打印比例。
#   WEAK  ：2~3 字节的弱特征（如 "I ("），随机二进制里也可能出现 → 仅当整段以文本为主时才报。
STRONG_POLLUTION_MARKERS = [b"EDK60", b"ESP-IDF", b"esp_idf", b"bridge:", b"radar_",
                            b"rst:0x", b"stack:"]
WEAK_POLLUTION_MARKERS = [b"I (", b"W (", b"E (", b"boot:"]


# ───────────────────────────── 小工具 ─────────────────────────────

def setup_stdout() -> None:
    """让中文能正常输出：真·GBK 控制台保持 GBK，其它（管道/重定向/UTF-8 控制台）统一 UTF-8。"""
    enc = (sys.stdout.encoding or "").lower().replace("-", "")
    if enc in ("utf8", "cp65001"):
        return
    target = "utf-8"
    if os.name == "nt" and sys.stdout.isatty():
        try:
            import ctypes
            cp = ctypes.windll.kernel32.GetConsoleOutputCP()   # 936 = 简体中文 GBK
            if cp in (936, 950):
                target = None                                   # cmd/conhost 按 GBK 显示才对
        except Exception:
            pass
    if target is None:
        return
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding=target, errors="replace")   # py3.7+
        except Exception:
            pass


def hexs(b: bytes, limit: int = 48) -> str:
    """bytes → '45 44 00 0B …'（超长截断，用于日志/JSON）。"""
    s = " ".join(f"{x:02X}" for x in b[:limit])
    if len(b) > limit:
        s += f" …(共 {len(b)} 字节)"
    return s


def ascii_preview(b: bytes, limit: int = 64) -> str:
    """bytes → 可打印预览（不可打印显示为 .），用于判断垃圾段是不是 ASCII 日志。"""
    out = []
    for x in b[:limit]:
        out.append(chr(x) if 32 <= x < 127 else ".")
    s = "".join(out)
    if len(b) > limit:
        s += "…"
    return s


def printable_ratio(b: bytes) -> float:
    """可打印 ASCII 占比，用于区分「二进制垃圾」与「文本日志」。"""
    if not b:
        return 0.0
    ok = sum(1 for x in b if 32 <= x < 127 or x in (9, 10, 13))
    return ok / len(b)


def pollution_hits(b: bytes) -> list:
    """在字节段里查找疑似日志污染的特征串。

    强特征（≥5 字节，如 EDK60 / ESP-IDF）无条件上报——二进制误码凑不出这种串；
    弱特征（如 "I ("）只在整段可打印比例 ≥ 0.7 时上报，避免误报。
    """
    hits = [m.decode("ascii", "replace") for m in STRONG_POLLUTION_MARKERS if m in b]
    if printable_ratio(b) >= 0.7:
        hits += [m.decode("ascii", "replace") for m in WEAK_POLLUTION_MARKERS if m in b]
    return hits


def fmt_rate(v):
    """比率格式化；None → 'N/A'。"""
    return "N/A" if v is None else f"{v * 100:.4f}%"


def median_from_counter(counter: Counter):
    """从频次表算中位数（避免为几十万帧保存完整列表）。"""
    total = sum(counter.values())
    if total == 0:
        return None
    half = total / 2.0
    acc = 0
    prev_key = None
    for key in sorted(counter):
        acc += counter[key]
        if acc == half and total % 2 == 0 and prev_key is not None:
            # 偶数个样本且正好落在边界：取上下两个值的中值
            return (prev_key + key) / 2.0
        if acc >= half:
            return float(key)
        prev_key = key
    return float(max(counter))


def op_label(op: int) -> str:
    return OP_NAMES.get(op, "未识别")


def type_label(t: int) -> str:
    return TYPE_NAMES.get(t, "未识别")


def build_frame(msgid: int, op: int, typ: int, payload: bytes = b"",
                break_checksum: bool = False, force_len: int = None) -> bytes:
    """按协议构造一帧（自测与文档示例用）。

    break_checksum=True 时故意写错校验和；force_len 可强改长度字段（造坏帧）。
    """
    body = bytearray()
    body += HEADER
    body += struct.pack(">H", 0)                 # 长度占位
    body += struct.pack(">H", msgid & 0xFFFF)
    body += bytes([op & 0xFF, typ & 0xFF])
    body += struct.pack(">H", len(payload))
    body += payload
    total = len(body) + 1                        # +1 校验字节
    body[2:4] = struct.pack(">H", total if force_len is None else force_len)
    cs = sum(body) & 0xFF
    if break_checksum:
        cs ^= 0xFF                               # 一定与原值不同（0x00/0xFF 也不会撞上）
    return bytes(body) + bytes([cs])


def target_payload(zones) -> bytes:
    """按 0x70 载荷格式打包: 重复 { zone_id u8, count u8, count × {id u8,x i16,y i16,z i16} }"""
    out = bytearray()
    for zone_id, targets in zones:
        out.append(zone_id & 0xFF)
        out.append(len(targets) & 0xFF)
        for (tid, x, y, z) in targets:
            out += struct.pack(">Bhhh", tid & 0xFF, x, y, z)
    return bytes(out)


def parse_target_payload(payload: bytes):
    """解析 0x70 载荷 → [(zone_id, count), …]；格式不合法时抛 ValueError。

    与 human_track_v1.4_side_mount.py 的 parse_target_report_v1_4 语义一致：
    每个目标 7 字节（id u8 + x/y/z 各 i16 大端）。
    """
    zones = []
    i, n = 0, len(payload)
    while i < n:
        if i + 2 > n:
            raise ValueError("感应区头不完整（剩 %d 字节，至少要 2 字节）" % (n - i))
        zone_id = payload[i]
        count = payload[i + 1]
        i += 2
        need = count * 7
        if i + need > n:
            raise ValueError("感应区 %d 声明 %d 个目标需要 %d 字节，实际只剩 %d 字节"
                             % (zone_id, count, need, n - i))
        i += need
        zones.append((zone_id, count))
    return zones


# ───────────────────────────── 校验器 ─────────────────────────────

class Checker:
    """流式 EDV11P 帧校验器：喂字节进来，内部完成帧同步、校验与全部统计。"""

    # 坏帧分类的中文名
    BAD_KINDS = OrderedDict([
        ("len_invalid", "长度非法（<11 或 >4096）"),
        ("checksum_error", "校验和错误"),
        ("tlv_len_mismatch", "TLV 长度与帧长不符"),
        ("truncated", "截断帧（流结束时字节不足）"),
    ])

    def __init__(self, raw_dump: bool = False, outdir: str = DEFAULT_OUTDIR,
                 max_unparsed: int = 50, csv_path: str = None,
                 msgid_min_samples: int = 20, detail_limit: int = 200,
                 warmup_s: float = DEFAULT_WARMUP_S):
        self.buf = bytearray()
        self.abs_offset = 0            # 已从缓冲区消费掉的字节数（= buf[0] 在流中的偏移）

        # ---- 基础统计 ----
        self.total_bytes = 0
        self.frames_ok = 0
        self.by_op = Counter()                        # op → 帧数
        self.by_type = Counter()                      # type → 帧数
        self.by_op_type = Counter()                   # (op, type) → 帧数
        self.unknown_op_frames = 0

        # ---- 未归属字节（洁净判据 H1 / 章程 R1 口径）----
        # 未归属字节 = 统计窗口内的收字节 − 窗口内「校验通过帧」的字节。
        # 窗口从 warmup_s 秒起算，跳过上电瞬间的 bootloader/ROM 文本（口径见 RULES_TEXT）。
        self.warmup_s = float(warmup_s)
        self.window_started = bool(warmup_s <= 0)
        self.window_start_offset = 0
        self.frame_bytes_total = 0                    # 全程：校验通过帧的字节合计
        self.frame_bytes_window = 0                   # 窗口内：校验通过帧的字节合计

        # ---- 坏帧 ----
        self.bad = Counter({k: 0 for k in self.BAD_KINDS})
        self.bad_detail = []           # 明细（有上限）
        self.detail_limit = detail_limit

        # ---- 假同步 / 重同步 ----
        self.resync_events = 0
        self.garbage_bytes = 0
        self._garbage = bytearray()
        self._garbage_offset = None
        self.segments = []             # 每个无法解析的字节段（用于 --raw-dump 与反查污染）

        # ---- 目标上报 ----
        self.target_frames = 0
        self.target_count_hist = Counter()            # 每帧目标总数 → 次数
        self.zone_seen = set()
        self.zone_max = Counter()                     # zone_id → 该区最大目标数
        self.target_payload_errors = 0

        # ---- msgid 连续性 ----
        self.msgid_min_samples = msgid_min_samples
        self.msg_groups = OrderedDict()               # (op,type) → dict(last, count, jumps, dups)
        self.msg_overall = {"last": None, "count": 0, "jumps": 0, "dups": 0}
        self.jump_log = []                            # 明细（有上限，按时间顺序）
        self.dup_log = []

        # ---- 事件/告警 ----
        self.warnings = []
        self.events = []                              # 读串口异常等

        # ---- 输出控制 ----
        self.raw_dump = raw_dump
        self.outdir = outdir
        self.max_unparsed = max_unparsed
        self._csv = None
        self._csv_path = csv_path
        if csv_path:
            self._csv = open(csv_path, "w", encoding="utf-8", newline="")
            self._csv.write("elapsed_s,wallclock,frame_no,zone_id,target_count,frame_total_targets\n")
        self._start_wall = None
        self._start_mono = None
        self._sim_span = None          # 离线模式的时间插值跨度（见 set_synthetic_time）
        self._sim_total = None

    # ---------------- 时间基准 ----------------
    def mark_start(self) -> None:
        self._start_wall = time.time()
        self._start_mono = time.monotonic()

    def set_synthetic_time(self, span_s: float, total_bytes: int) -> None:
        """离线模式：没有真实时间戳时，按「字节位置线性插值」到给定的 --duration。

        仅用于让帧率/人数时间序列可以对齐与画图，**不代表真实到达时刻**（报告里会注明）。
        """
        self._sim_span = span_s
        self._sim_total = total_bytes or None

    def disable_warmup(self) -> None:
        """离线且没有时基时调用：无法判断 T+10 s，就退化为「从流首开始全程统计」。

        报告里会注明 warmup = 0（此时头部的 bootloader 文本会被算作未归属字节，
        读到非 0 的未归属率要先排除这个原因）。
        """
        self.warmup_s = 0.0
        self.window_started = True
        self.window_start_offset = 0

    def _maybe_start_window(self, now) -> None:
        """一旦越过 warmup，就把当前流位置记为统计窗口起点（只做一次）。"""
        if self.window_started:
            return
        if self._now(now) >= self.warmup_s:
            self.window_started = True
            self.window_start_offset = self.abs_offset

    def window_stats(self) -> dict:
        """未归属字节统计（H1 判据的数值来源）。"""
        window_bytes = max(0, self.abs_offset - self.window_start_offset)
        unattr = window_bytes - self.frame_bytes_window
        if unattr < 0:                      # 理论上不可能，防御性收敛
            unattr = 0
        rate = (unattr / window_bytes) if window_bytes > 0 else None
        return {
            "warmup_s": self.warmup_s,
            "window_started": self.window_started,
            "window_start_offset": self.window_start_offset,
            "window_bytes": window_bytes,
            "frame_bytes_in_window": self.frame_bytes_window,
            "frame_bytes_total": self.frame_bytes_total,
            "unattributed_bytes": unattr,
            "unattributed_rate": rate,
        }

    def elapsed(self) -> float:
        if self._start_mono is None:
            return 0.0
        return time.monotonic() - self._start_mono

    def _now(self, now):
        if now is not None:
            return now
        if self._sim_span and self._sim_total:
            return self._sim_span * min(1.0, self.abs_offset / self._sim_total)
        return self.elapsed()

    # ---------------- 喂数据 ----------------
    def feed(self, data: bytes, now: float = None) -> None:
        if not data:
            return
        self.total_bytes += len(data)
        self.buf.extend(data)
        self._drain(now)

    def _drain(self, now) -> None:
        buf = self.buf
        while True:
            self._maybe_start_window(now)
            idx = buf.find(HEADER)
            if idx < 0:
                # 没有帧头：整段就是垃圾，但末尾若正好是 0x45 可能是半个帧头，留 1 字节
                keep = 1 if buf[-1:] == b"\x45" else 0
                drop = len(buf) - keep
                if drop > 0:
                    self._add_garbage(bytes(buf[:drop]), now)
                    del buf[:drop]
                return
            if idx > 0:
                self._add_garbage(bytes(buf[:idx]), now)
                del buf[:idx]
            # 到这里缓冲区一定以帧头开始
            if len(buf) < 4:
                return
            pkt_len = (buf[2] << 8) | buf[3]
            if pkt_len < MIN_FRAME_LEN or pkt_len > MAX_FRAME_LEN:
                self._reject("len_invalid", bytes(buf[:4]), pkt_len, now,
                             extra="声明长度 %d，合法范围 %d~%d" % (pkt_len, MIN_FRAME_LEN, MAX_FRAME_LEN))
                # 只丢这 2 字节假帧头，剩下的重新找帧头（能最快找回真同步）
                self._add_garbage(bytes(buf[:2]), now)
                del buf[:2]
                continue
            if len(buf) < pkt_len:
                return                       # 帧还没收全，等下一批数据
            frame = bytes(buf[:pkt_len])
            cs = sum(frame[:-1]) & 0xFF
            if cs != frame[-1]:
                self._reject("checksum_error", frame, pkt_len, now,
                             extra="计算得 0x%02X，帧内为 0x%02X" % (cs, frame[-1]))
                self._add_garbage(frame, now)
                del buf[:pkt_len]
                continue
            tlv_len = (frame[8] << 8) | frame[9]
            if tlv_len != pkt_len - MIN_FRAME_LEN:
                self._reject("tlv_len_mismatch", frame, pkt_len, now,
                             extra="TLV 声明 %d，按帧长应为 %d" % (tlv_len, pkt_len - MIN_FRAME_LEN))
                self._add_garbage(frame, now)
                del buf[:pkt_len]
                continue
            # 合法帧
            frame_start = self.abs_offset      # 本帧在字节流中的起始偏移（归属统计用）
            del buf[:pkt_len]
            self.abs_offset += pkt_len
            self._on_good_frame(frame, now, frame_start)

    # ---------------- 垃圾段 / 重同步 ----------------
    def _add_garbage(self, data: bytes, now) -> None:
        if not data:
            return
        if self._garbage_offset is None:
            self._garbage_offset = self.abs_offset
        self._garbage += data
        self.garbage_bytes += len(data)
        self.abs_offset += len(data)

    def _close_garbage(self, kind: str, now, count_resync: bool = True) -> None:
        """收尾一段「无法解析的字节段」：计入重同步、落 unparsed_<n>.bin、记录诊断。"""
        if not self._garbage:
            self._garbage_offset = None
            return
        data = bytes(self._garbage)
        offset = self._garbage_offset if self._garbage_offset is not None else 0
        self._garbage = bytearray()
        self._garbage_offset = None
        if count_resync:
            self.resync_events += 1

        seg = {
            "index": len(self.segments) + 1,
            "offset": offset,
            "size": len(data),
            "kind": kind,
            "hex": hexs(data, 40),
            "ascii": ascii_preview(data),
            "printable_ratio": round(printable_ratio(data), 3),
            "dumped": None,
        }
        hits = pollution_hits(data)
        if hits:
            seg["pollution_markers"] = hits
            ratio = seg["printable_ratio"]
            self.warnings.append(
                "unparsed 段 #%d（偏移 %d，%d 字节，可打印比例 %.2f）含疑似日志/文本特征 %s"
                " —— 数据流里混入了非协议字节，优先怀疑固件日志/printf（详见测试计划第五节）"
                % (seg["index"], offset, len(data), ratio, "/".join(hits)))
        if len(data) > 1024:
            self.warnings.append("unparsed 段 #%d 长达 %d 字节，疑似持续写出而非偶发误码"
                                 % (seg["index"], len(data)))
        # --raw-dump：把这段字节原样存盘（超过 --max-unparsed 后只记账不落盘）
        if self.raw_dump and seg["index"] <= self.max_unparsed:
            path = os.path.join(self.outdir, "unparsed_%d.bin" % seg["index"])
            try:
                with open(path, "wb") as f:
                    f.write(data)
                seg["dumped"] = path
            except OSError as e:
                self.warnings.append("unparsed_%d.bin 写入失败：%s" % (seg["index"], e))
        if seg["index"] <= 500:
            self.segments.append(seg)
        return seg

    # ---------------- 坏帧 ----------------
    def _reject(self, kind: str, raw: bytes, pkt_len: int, now, extra: str = "") -> None:
        self.bad[kind] += 1
        if len(self.bad_detail) < self.detail_limit:
            self.bad_detail.append({
                "kind": kind,
                "offset": self.abs_offset,
                "elapsed_s": round(self._now(now), 3),
                "declared_len": pkt_len,
                "hex": hexs(raw, 32),
                "ascii": ascii_preview(raw, 32),
                "note": extra,
            })

    # ---------------- 合法帧 ----------------
    def _on_good_frame(self, frame: bytes, now, frame_start: int = 0) -> None:
        # 一帧成功解析 = 之前那段无法解析的字节就此结束 → 记一次重同步
        self._close_garbage("garbage_or_badframe", now, count_resync=True)

        msgid = (frame[4] << 8) | frame[5]
        op = frame[6]
        typ = frame[7]
        tlv_len = (frame[8] << 8) | frame[9]
        payload = frame[10:10 + tlv_len]

        # 归属统计：只有「校验 + 长度 + TLV 全通过」的帧才算已归属字节
        self.frame_bytes_total += len(frame)
        if self.window_started and frame_start >= self.window_start_offset:
            self.frame_bytes_window += len(frame)

        self.frames_ok += 1
        self.by_op[op] += 1
        self.by_type[typ] += 1
        self.by_op_type[(op, typ)] += 1
        if op not in OP_NAMES:
            self.unknown_op_frames += 1

        self._track_msgid(msgid, op, typ, self._now(now))

        if op == OP_TARGET_REPORT and typ == TYPE_TARGET_REPORT:
            self._on_target_report(payload, now)

    # ---------------- msgid 连续性 ----------------
    @staticmethod
    def _expected_next(prev: int) -> int:
        return 0 if prev >= MSGID_MAX else prev + 1

    def _track_msgid(self, msgid: int, op: int, typ: int, now: float) -> None:
        # 1) 全局（不分组）视图
        ov = self.msg_overall
        if ov["last"] is not None and msgid != self._expected_next(ov["last"]):
            ov["jumps"] = ov.get("jumps", 0) + 1
            self._log_jump(None, op, typ, ov["last"], msgid, now)
        ov["last"] = msgid
        ov["count"] += 1

        # 2) 按 (op,type) 分组视图 —— 若 msgid 按命令类别独立计数，只有这个视图才是对的
        key = (op, typ)
        g = self.msg_groups.get(key)
        if g is None:
            g = {"op": op, "type": typ, "last": None, "count": 0}
            self.msg_groups[key] = g
        if g["last"] is not None:
            exp = self._expected_next(g["last"])
            if msgid == g["last"]:
                g["dups"] = g.get("dups", 0) + 1
                if len(self.dup_log) < 50:
                    self.dup_log.append({"op": op, "type": typ, "msgid": msgid,
                                         "elapsed_s": round(now, 3)})
            elif msgid != exp:
                g["jumps"] = g.get("jumps", 0) + 1
                g["missing"] = g.get("missing", 0) + (
                    ((msgid - exp) % (MSGID_MAX + 1)) if msgid > exp else 0)
                self._log_jump(key, op, typ, g["last"], msgid, now)
        g["last"] = msgid
        g["count"] += 1

    def _log_jump(self, group, op, typ, prev, cur, now) -> None:
        if len(self.jump_log) >= self.detail_limit:
            return
        self.jump_log.append({
            "op": op, "type": typ,
            "grouped": group is not None,
            "prev_msgid": prev,
            "cur_msgid": cur,
            "delta": cur - prev,
            "expected": self._expected_next(prev),
            "elapsed_s": round(now, 3),
        })

    # ---------------- 0x70 目标上报 ----------------
    def _on_target_report(self, payload: bytes, now) -> None:
        self.target_frames += 1
        try:
            zones = parse_target_payload(payload)
        except ValueError as e:
            self.target_payload_errors += 1
            if self.target_payload_errors <= 10:
                self.warnings.append("0x70 载荷解析异常（第 %d 次）：%s；载荷 %s"
                                     % (self.target_payload_errors, e, hexs(payload, 24)))
            return
        total = sum(c for _, c in zones)
        self.target_count_hist[total] += 1
        for zone_id, c in zones:
            self.zone_seen.add(zone_id)
            if zone_id not in self.zone_max or c > self.zone_max[zone_id]:
                self.zone_max[zone_id] = c
        if self._csv is not None:
            wall = time.strftime("%Y-%m-%d %H:%M:%S")
            t = self._now(now)
            for zone_id, c in zones:
                self._csv.write("%.3f,%s,%d,%d,%d,%d\n"
                                % (t, wall, self.frames_ok, zone_id, c, total))

    # ---------------- 收尾 ----------------
    def finish(self) -> None:
        """流结束：把残留判定为「截断帧」或尾部垃圾。"""
        buf = self.buf
        if buf:
            pkt_len = (buf[2] << 8) | buf[3] if len(buf) >= 4 else None
            head = buf.startswith(HEADER)
            declared_ok = pkt_len is not None and MIN_FRAME_LEN <= pkt_len <= MAX_FRAME_LEN
            if head and declared_ok and len(buf) < pkt_len:
                # 帧头与长度都像真的，只是字节没到齐 → 截断帧（不算重同步）
                self.bad["truncated"] += 1
                if len(self.bad_detail) < self.detail_limit:
                    self.bad_detail.append({
                        "kind": "truncated",
                        "offset": self.abs_offset,
                        "elapsed_s": 0.0,
                        "declared_len": pkt_len,
                        "actual_len": len(buf),
                        "hex": hexs(bytes(buf), 32),
                        "ascii": ascii_preview(buf, 32),
                        "note": "声明 %d 字节，实际只收到 %d 字节" % (pkt_len, len(buf)),
                    })
                self._add_garbage(bytes(buf), None)
                self._close_garbage("truncated_tail", None, count_resync=False)
            else:
                self._add_garbage(bytes(buf), None)
                self._close_garbage("tail_garbage", None, count_resync=False)
            del buf[:]
        if self._csv is not None:
            self._csv.close()
            self._csv = None

    # ---------------- 结果汇总 ----------------
    def rates(self, span_s):
        span = span_s if span_s and span_s > 0 else None
        return {
            "span_s": round(span, 3) if span else None,
            "frame_rate": (self.frames_ok / span) if span else None,
            "byte_rate": (self.total_bytes / span) if span else None,
        }

    def msgid_view(self):
        """msgid 连续性：不分组 vs 按 (op,type) 分组（含每组跳号率）。"""
        groups = []
        for (op, typ), g in self.msg_groups.items():
            n = g["count"]
            jumps = g.get("jumps", 0)
            rate = (jumps / (n - 1)) if n > 1 else 0.0
            groups.append({
                "op": op, "op_name": op_label(op),
                "type": typ, "type_name": type_label(typ),
                "frames": n,
                "jumps": jumps,
                "duplicates": g.get("dups", 0),
                "missing_estimate": g.get("missing", 0),
                "jump_rate": rate,
                "eligible": n >= self.msgid_min_samples,
            })
        groups.sort(key=lambda d: (-d["frames"], d["op"], d["type"]))
        ov_n = self.msg_overall["count"]
        ov_j = self.msg_overall.get("jumps", 0)
        ov_rate = (ov_j / (ov_n - 1)) if ov_n > 1 else 0.0
        eligible = [g for g in groups if g["eligible"]]
        if eligible:
            primary = max(eligible, key=lambda d: d["jump_rate"])
            primary_rate = primary["jump_rate"]
            basis = "分组(op,type) 最大组：op=0x%02X type=0x%02X" % (primary["op"], primary["type"])
        elif ov_n >= self.msgid_min_samples:
            primary_rate = ov_rate
            basis = "不分组（无组内样本达标的组）"
        else:
            primary_rate = None
            basis = "样本不足（总共 %d 帧 < %d）" % (ov_n, self.msgid_min_samples)
        return {
            "basis": basis,
            "primary_rate": primary_rate,
            "overall": {"frames": ov_n, "jumps": ov_j, "jump_rate": ov_rate},
            "groups": groups,
            "grouped_pass_ungrouped_fail": (primary_rate is not None
                                            and primary_rate < THRESHOLDS["msgid_gap_rate_max"]
                                            and ov_rate >= THRESHOLDS["msgid_gap_rate_max"]),
            "jumps": self.jump_log,
            "duplicates": self.dup_log,
            "min_samples": self.msgid_min_samples,
        }

    def resync_limit(self, span_s) -> int:
        """重同步绝对次数上限：30 分钟 3 次，按时长线性缩放，最少给 1 次。

        30 min → 3；60 min → 6；10 min → 1；5 min → 1（避免短采集因 1 次抖动直接判死）。
        """
        if not span_s or span_s <= 0:
            return RESYNC_LIMIT_30MIN
        return max(1, int(round(RESYNC_LIMIT_30MIN * span_s / GATE2_DURATION)))

    def verdict(self, span_s) -> dict:
        """按固定阈值裁定 PASS/FAIL（P0 前提 → P1..P6 六条硬判据，见 RULES_TEXT）。"""
        bad_total = sum(self.bad.values())
        checks = []

        # ---- P0 前提：必须有有效帧，否则后面全部无意义 ----
        if self.frames_ok == 0:
            checks.append({
                "priority": "P0", "name": "有效帧数", "value": 0, "threshold": 1,
                "compare": ">=", "result": "FAIL",
                "note": "整段数据里没有一帧合法 EDV11P 帧 → 判定无效（其余判据 SKIP）",
            })
            return {
                "verdict": "FAIL", "checks": checks,
                "bad_frame_rate": None, "complete_rate": None,
                "fake_sync_loss_rate": None, "msgid_gap_rate": None,
                "bad_frames_total": bad_total,
                "unattributed_rate": None, "resync_limit": self.resync_limit(span_s),
            }
        checks.append({
            "priority": "P0", "name": "有效帧数", "value": self.frames_ok, "threshold": 1,
            "compare": ">=", "result": "PASS", "note": "",
        })

        # ---- P1 未归属字节率（洁净判据；R1 验收口径，最严）----
        ws = self.window_stats()
        u_rate = ws["unattributed_rate"]
        if u_rate is None:
            u_result = "N/A"
            u_note = "统计窗口内没有收到字节（采集太短或全在 warmup 内）"
        elif ws["unattributed_bytes"] == 0:
            u_result = "PASS"
            u_note = "干净：窗口内每个字节都能归属到某一帧合法帧"
        elif u_rate < THRESHOLDS["unattributed_rate_max"]:
            u_result = "WARN"
            u_note = ("存在 %d 字节未归属（%.4f%% < 0.1%%）：不会判 FAIL，但必须在 Gate 记录里"
                      "说明来源（见 unparsed 段）" % (ws["unattributed_bytes"], u_rate * 100))
        else:
            u_result = "FAIL"
            u_note = ("存在 %d 字节未归属（%.4f%% ≥ 0.1%%）：数据流里混进了非协议字节"
                      % (ws["unattributed_bytes"], u_rate * 100))
        checks.append({
            "priority": "P1", "name": "未归属字节率", "value": u_rate,
            "threshold": THRESHOLDS["unattributed_rate_max"], "compare": "==",
            "result": u_result, "note": u_note,
            "extra": {"unattributed_bytes": ws["unattributed_bytes"],
                      "window_bytes": ws["window_bytes"], "warmup_s": ws["warmup_s"]},
        })
        if u_result == "WARN":
            self.warnings.append("未归属字节率 = %.4f%%（%d 字节）：属 WARN 级，需在 Gate 记录里说明来源"
                                 % (u_rate * 100, ws["unattributed_bytes"]))

        # ---- P2 重同步绝对次数（硬门槛，防止「少量但持续」的字符插入混过去）----
        limit = self.resync_limit(span_s)
        if self.resync_events <= limit:
            rs_result, rs_note = "PASS", ""
        else:
            rs_result = "FAIL"
            rs_note = ("%d 次 > 上限 %d 次（%s）：说明有字符在持续插入流里，不能只靠比率通过"
                       % (self.resync_events, limit,
                          "30 分钟 3 次" if span_s and abs(span_s - GATE2_DURATION) < 1
                          else "按 3 次/30 分钟线性折算"))
        checks.append({
            "priority": "P2", "name": "重同步绝对次数", "value": self.resync_events,
            "threshold": limit, "compare": "<=", "result": rs_result, "note": rs_note,
        })

        # ---- P3 坏帧率 ----
        denom = self.frames_ok + bad_total
        bad_rate = (bad_total / denom) if denom else None
        checks.append({
            "priority": "P3", "name": "坏帧率",
            "value": bad_rate,
            "threshold": THRESHOLDS["bad_frame_rate_max"],
            "compare": "<",
            "result": ("N/A" if bad_rate is None else
                       ("PASS" if bad_rate < THRESHOLDS["bad_frame_rate_max"] else "FAIL")),
            "note": "",
        })

        # ---- P4 帧完整率（= 章程 G1 主指标）----
        cdenom = self.frames_ok + bad_total + self.resync_events
        complete = (self.frames_ok / cdenom) if cdenom else None
        checks.append({
            "priority": "P4", "name": "帧完整率",
            "value": complete,
            "threshold": THRESHOLDS["complete_rate_min"],
            "compare": ">=",
            "result": ("N/A" if complete is None else
                       ("PASS" if complete >= THRESHOLDS["complete_rate_min"] else "FAIL")),
            "note": "",
        })

        # ---- P5 假同步丢帧估算率（P1 的宽松版，用于定位「丢了几帧」）----
        sdenom = self.frames_ok + self.resync_events
        fake = (self.resync_events / sdenom) if sdenom else None
        checks.append({
            "priority": "P5", "name": "假同步丢帧估算率",
            "value": fake,
            "threshold": THRESHOLDS["fake_sync_loss_rate_max"],
            "compare": "<",
            "result": ("N/A" if fake is None else
                       ("PASS" if fake < THRESHOLDS["fake_sync_loss_rate_max"] else "FAIL")),
            "note": "",
        })

        # ---- P6 msgid 跳号率 ----
        mv = self.msgid_view()
        gap = mv["primary_rate"]
        checks.append({
            "priority": "P6", "name": "msgid 跳号率",
            "value": gap,
            "threshold": THRESHOLDS["msgid_gap_rate_max"],
            "compare": "<",
            "result": ("N/A" if gap is None else
                       ("PASS" if gap < THRESHOLDS["msgid_gap_rate_max"] else "FAIL")),
            "note": mv["basis"],
        })

        fails = [c for c in checks if c["result"] == "FAIL"]
        verdict = "FAIL" if fails else "PASS"

        return {
            "verdict": verdict,
            "checks": checks,
            "bad_frame_rate": bad_rate,
            "complete_rate": complete,
            "fake_sync_loss_rate": fake,
            "msgid_gap_rate": gap,
            "bad_frames_total": bad_total,
            "unattributed_rate": u_rate,
            "resync_limit": limit,
        }

    # ---------------- 汇总成报告字典 ----------------
    def collect(self, mode: str, source: str, span_s, extra_warnings=None) -> dict:
        bad_total = sum(self.bad.values())
        v = self.verdict(span_s)
        r = self.rates(span_s)
        warnings = list(self.warnings)
        if extra_warnings:
            warnings.extend(extra_warnings)

        if self.target_frames and self.target_count_hist:
            hist = self.target_count_hist
            tmin = min(hist)
            tmax = max(hist)
            tmed = median_from_counter(hist)
        else:
            tmin = tmed = tmax = None

        report = OrderedDict()
        report["tool"] = TOOL_NAME
        report["tool_version"] = TOOL_VERSION
        report["generated_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
        report["mode"] = mode
        report["source"] = source
        report["verdict"] = v["verdict"]
        report["checks"] = v["checks"]
        report["thresholds"] = dict(THRESHOLDS)
        report["protocol_baseline"] = OrderedDict([
            ("uart", "921600 / 8N1，无流控，大端"),
            ("header", "0x45 0x44"),
            ("frame_layout", "头2 | 长度2(整包字节数) | msgid2 | op1 | type1 | TLV长2 | value N | 校验1"),
            ("min_frame_len", MIN_FRAME_LEN),
            ("max_frame_len", MAX_FRAME_LEN),
            ("checksum", "从帧头起逐字节求和 & 0xFF"),
            ("msgid_range", "0 ~ 0x%04X，之后回绕到 0" % MSGID_MAX),
            ("target_report", "op=0xA3 STATUS，type=0x70，载荷 = 重复{zone_id u8, count u8, count×(id u8, x i16, y i16, z i16)}"),
        ])
        report["metrics"] = OrderedDict([
            ("total_bytes", self.total_bytes),
            ("span_s", r["span_s"]),
            ("byte_rate", r["byte_rate"]),
            ("frames_ok", self.frames_ok),
            ("frame_rate", r["frame_rate"]),
            ("bad_frames", OrderedDict([("total", bad_total)] + list(self.bad.items()))),
            ("bad_frame_rate", v["bad_frame_rate"]),
            ("complete_rate", v["complete_rate"]),
            ("resync_events", self.resync_events),
            ("resync_limit", v["resync_limit"]),
            ("fake_sync_loss_rate", v["fake_sync_loss_rate"]),
            ("garbage_bytes", self.garbage_bytes),
            ("unattributed", self.window_stats()),
            ("unattributed_rate", v["unattributed_rate"]),
            ("msgid_gap_rate", v["msgid_gap_rate"]),
            ("unknown_op_frames", self.unknown_op_frames),
        ])
        report["by_op"] = OrderedDict(
            ("0x%02X %s" % (op, op_label(op)), n) for op, n in sorted(self.by_op.items()))
        report["by_type"] = OrderedDict(
            ("0x%02X %s" % (t, type_label(t)), n) for t, n in sorted(self.by_type.items()))
        report["by_op_type"] = OrderedDict(
            ("0x%02X/0x%02X %s·%s" % (op, t, op_label(op), type_label(t)), n)
            for (op, t), n in sorted(self.by_op_type.items()))
        report["bad_frame_detail"] = self.bad_detail
        report["unparsed"] = OrderedDict([
            ("count", len(self.segments)),
            ("raw_dump", self.raw_dump),
            ("max_unparsed", self.max_unparsed),
            ("segments", self.segments),
        ])
        report["msgid"] = self.msgid_view()
        report["target_report"] = OrderedDict([
            ("frames", self.target_frames),
            ("targets_per_frame", OrderedDict([
                ("min", tmin), ("median", tmed), ("max", tmax),
                ("histogram", OrderedDict((str(k), v2) for k, v2 in sorted(self.target_count_hist.items()))),
            ])),
            ("zone_ids", sorted(self.zone_seen)),
            ("zone_max_targets", OrderedDict(
                (str(z), self.zone_max[z]) for z in sorted(self.zone_max))),
            ("payload_errors", self.target_payload_errors),
        ])
        report["warnings"] = warnings
        report["events"] = self.events
        return report


# ───────────────────────────── 自测 ─────────────────────────────

def build_selftest_stream():
    """构造一段带「已知缺陷」的合成字节流，并给出期望统计值。

    覆盖：合法帧（含 0 目标/多目标/多感应区）、msgid 跳号、校验错的帧、
    含 ASCII "EDK60" 的日志污染段（会触发假帧头 → 长度非法）、末尾截断帧。
    """
    parts = []
    expect = {}

    counts_cycle = [1, 2, 1, 3, 1, 1, 2, 1, 1, 2, 1, 1]   # 12 帧 0x70 每帧目标总数
    # 特例帧：多感应区（第二区 0 目标）/ 全空场景（人数 0）/ 一个区 2 个目标
    special = {
        4: [(1, [(2, -120, 111, 149)]), (2, [])],
        7: [(1, [])],
        9: [(1, [(3, 10, 20, 30), (4, -40, -50, -60)])],
    }
    target_frames = 0
    zone_ids = set()
    total_targets_sum = 0

    # 1) 12 帧合法 0x70 目标上报，msgid 0x0100 起
    mid = 0x0100
    for i in range(12):
        if i in special:
            zones = special[i]
        else:
            cnt = counts_cycle[i]
            zones = [(1, [(i + 1, 5 * i, -3 * i, 100 + i) for _ in range(cnt)])]
        total_targets_sum += sum(len(tlist) for _, tlist in zones)
        for z, _tlist in zones:
            zone_ids.add(z)
        parts.append(build_frame(mid, OP_TARGET_REPORT, TYPE_TARGET_REPORT, target_payload(zones)))
        mid += 1
        target_frames += 1

    # 2) msgid 故意跳号：上一帧 0x010B → 这一帧 0x0110（该组跳号 +1）
    parts.append(build_frame(0x0110, OP_TARGET_REPORT, TYPE_TARGET_REPORT,
                             target_payload([(1, [(9, 1, 2, 3)])])))
    target_frames += 1
    total_targets_sum += 1

    # warmup 分界点：这里之后的字节落在「未归属字节」统计窗口内（见 run_selftest 分两次喂）
    warmup_split = sum(len(p) for p in parts)
    warmup_clean_bytes = 0        # 分界点之后的合法帧字节（窗口内）
    warmup_dirty_bytes = 0        # 分界点之后的未归属字节（窗口内）

    # 3) 3 帧另一个命令类别（0xA2 / 0x05 工作模式上报），用自己的 msgid 序列
    for k in range(3):
        parts.append(build_frame(0x0001 + k, 0xA2, 0x05, bytes([1])))

    # 4) 1 帧校验和故意写错
    bad_frame = build_frame(0x0004, 0xA2, 0x10, b"\x00\x02", break_checksum=True)
    parts.append(bad_frame)

    # 5) 垃圾字节段：真实日志行，含 ASCII "EDK60"（= 0x45 0x44，会触发假帧头）
    logline = b"\x00\x01\x02\x03LOG:EDK60 bridge boot\r\n"
    parts.append(logline)

    # 6) 再恢复 3 帧合法 0x70（msgid 接着跳号钳 0x0111 起），检验重同步后能继续
    for k in range(3):
        parts.append(build_frame(0x0111 + k, OP_TARGET_REPORT, TYPE_TARGET_REPORT,
                                 target_payload([(1, [(1, 0, 0, 100)])])))
        target_frames += 1
        total_targets_sum += 1

    # 7) 末尾截断帧：只有 12 字节，但长度字段声明 24 字节
    full = build_frame(0x0120, OP_TARGET_REPORT, TYPE_TARGET_REPORT,
                       target_payload([(1, [(1, 0, 0, 100), (2, 1, 1, 101)])]))
    trunc = full[:12]
    parts.append(trunc)

    stream = b"".join(parts)

    # 未归属字节（窗口 = warmup_split 之后）：
    #   窗口内未归属字节 = 坏帧整帧 + 整条日志行 + 截断帧 12 字节
    window_bytes = len(stream) - warmup_split
    dirty_bytes = len(bad_frame) + len(logline) + len(trunc)
    clean_bytes = window_bytes - dirty_bytes

    expect.update({
        "total_bytes": len(stream),
        "frames_ok": 12 + 1 + 3 + 3,            # 19
        "by_op_A3": 12 + 1 + 3,                # 16
        "by_op_A2": 3,
        "checksum_error": 1,
        "len_invalid_min": 1,                  # "EDK60" 里的 0x45 0x44 → 长度 0x4B36
        "tlv_len_mismatch": 0,
        "truncated": 1,
        "resync_events": 1,                    # 校验错帧 + 污染段 + 假帧头 → 一段被下一帧收尾
        # 不分组视图必然出现 3 次跳号：0x010B→0x0110、0x0110→0x0001、0x0003→0x0111
        # 其中后两次是「0x70 与 0x05 各自独立计数」的正常现象 —— 自测同时验证「分组视图」能把它拆干净
        "msgid_jumps_overall": 3,
        "msgid_jumps_group_max": 1,            # 分组后只有 0x70 组真的丢了一段（0x010C~0x010F）
        "target_frames": target_frames,
        "target_min": 0,
        "target_max": 3,
        "target_sum": total_targets_sum,
        "zone_ids": sorted(zone_ids),
        "pollution_marker": "EDK60",
        "warmup_split": warmup_split,
        "window_bytes": window_bytes,
        "unattributed_bytes": dirty_bytes,
        "frame_bytes_window": clean_bytes,
        "resync_limit_60s": 1,                 # 60 s 采集 → max(1, round(3*60/1800)) = 1
    })
    return stream, expect


def run_selftest(outdir_root: str = DEFAULT_OUTDIR) -> int:
    """离线自测：按期望值逐项核对；任何一项不符即返回非 0。"""
    setup_stdout()
    stream, expect = build_selftest_stream()
    # 产物放在 --out 下的独立子目录（便于取证与复查；脚本不做任何删除）
    outdir = os.path.join(outdir_root, "edv11p_selftest_" + time.strftime("%Y%m%d-%H%M%S"))
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError:
        outdir = tempfile.mkdtemp(prefix="edv11p_selftest_")
    cap_path = os.path.join(outdir, "selftest_capture.bin")
    with open(cap_path, "wb") as f:
        f.write(stream)

    print("=" * 74)
    print("EDV11P 帧校验脚本 — 离线自测（--selftest）")
    print("=" * 74)
    print(f"合成流：{len(stream)} 字节 → {cap_path}")
    print(f"产物目录：{outdir}（不自动删除）")
    print("")

    # 用 5 帧作为分组最小样本，让小样本自测也能触发 msgid 分组判定
    ck = Checker(raw_dump=True, outdir=outdir, max_unparsed=10, msgid_min_samples=5)
    # 分两次喂，让「未归属字节」统计窗口（T+10s）正好落在 warmup_split 这个帧边界上：
    #   前段 now=0（在 warmup 内，不计入窗口）；后段 now=30（窗口从这里起算）
    split = expect["warmup_split"]
    ck.feed(stream[:split], now=0.0)
    ck.feed(stream[split:], now=30.0)
    ck.finish()
    report = ck.collect("selftest", "synthetic", span_s=60.0)

    m = report["metrics"]
    got = {
        "total_bytes": m["total_bytes"],
        "frames_ok": m["frames_ok"],
        "by_op_A3": report["by_op"].get("0xA3 STATUS", 0),
        "by_op_A2": report["by_op"].get("0xA2 REPORT", 0),
        "checksum_error": m["bad_frames"]["checksum_error"],
        "len_invalid_min": m["bad_frames"]["len_invalid"],
        "tlv_len_mismatch": m["bad_frames"]["tlv_len_mismatch"],
        "truncated": m["bad_frames"]["truncated"],
        "resync_events": m["resync_events"],
        "resync_limit": m["resync_limit"],
        "msgid_jumps": report["msgid"]["overall"]["jumps"],
        "msgid_jumps_group_max": max([g["jumps"] for g in report["msgid"]["groups"]] or [0]),
        "target_frames": report["target_report"]["frames"],
        "target_min": report["target_report"]["targets_per_frame"]["min"],
        "target_max": report["target_report"]["targets_per_frame"]["max"],
        "target_sum": sum(int(k) * v for k, v in
                          report["target_report"]["targets_per_frame"]["histogram"].items()),
        "zone_ids": report["target_report"]["zone_ids"],
        "window_bytes": m["unattributed"]["window_bytes"],
        "frame_bytes_window": m["unattributed"]["frame_bytes_in_window"],
        "unattributed_bytes": m["unattributed"]["unattributed_bytes"],
    }

    # (名称, 实际值, 期望值, 比较方式)
    rows = [
        ("总字节数", got["total_bytes"], expect["total_bytes"], "=="),
        ("合法帧数", got["frames_ok"], expect["frames_ok"], "=="),
        ("op=0xA3 帧数", got["by_op_A3"], expect["by_op_A3"], "=="),
        ("op=0xA2 帧数", got["by_op_A2"], expect["by_op_A2"], "=="),
        ("坏帧·校验错", got["checksum_error"], expect["checksum_error"], "=="),
        ("坏帧·长度非法", got["len_invalid_min"], expect["len_invalid_min"], ">="),
        ("坏帧·TLV长度不符", got["tlv_len_mismatch"], expect["tlv_len_mismatch"], "=="),
        ("坏帧·截断帧", got["truncated"], expect["truncated"], "=="),
        ("重同步次数", got["resync_events"], expect["resync_events"], "=="),
        ("重同步上限(60s)", got["resync_limit"], expect["resync_limit_60s"], "=="),
        ("msgid 跳号·不分组", got["msgid_jumps"], expect["msgid_jumps_overall"], "=="),
        ("msgid 跳号·分组最大组", got["msgid_jumps_group_max"], expect["msgid_jumps_group_max"], "=="),
        ("0x70 帧数", got["target_frames"], expect["target_frames"], "=="),
        ("每帧目标数 min", got["target_min"], expect["target_min"], "=="),
        ("每帧目标数 max", got["target_max"], expect["target_max"], "=="),
        ("目标总数合计", got["target_sum"], expect["target_sum"], "=="),
        ("zone_id 集合", got["zone_ids"], expect["zone_ids"], "=="),
        ("窗口内收字节", got["window_bytes"], expect["window_bytes"], "=="),
        ("窗口内合法帧字节", got["frame_bytes_window"], expect["frame_bytes_window"], "=="),
        ("未归属字节", got["unattributed_bytes"], expect["unattributed_bytes"], "=="),
    ]

    ok_all = True
    print(f"{'检查项':<18}{'实际':>14}{'期望':>16}   结论")
    print("-" * 74)
    for name, actual, exp, op in rows:
        if op == ">=":
            ok = actual is not None and actual >= exp
            exp_s = f">= {exp}"
        else:
            ok = actual == exp
            exp_s = str(exp)
        ok_all = ok_all and ok
        print(f"{name:<18}{str(actual):>14}{exp_s:>16}   {'OK' if ok else '不符 <<<'}")
    print("-" * 74)

    # 额外检查：--raw-dump 是否落盘、落盘的段里是否有 ASCII "EDK60"
    dumped = [s for s in report["unparsed"]["segments"] if s["dumped"]]
    print(f"unparsed 段数        : {report['unparsed']['count']}（落盘 {len(dumped)} 个）")
    marker_ok = False
    if not dumped:
        print("落盘检查             : 不符 <<< 没有生成 unparsed_*.bin")
        ok_all = False
    else:
        blob = b""
        for s in dumped:
            with open(s["dumped"], "rb") as f:
                blob += f.read()
        marker_ok = expect["pollution_marker"].encode() in blob
        print(f"落盘内容含 '{expect['pollution_marker']}' : {'OK（污染反查可用）' if marker_ok else '不符 <<<'}")
        ok_all = ok_all and marker_ok

    # 额外检查：报告（JSON/日志/警告）本身要把污染特征点出来 —— 这是排「日志污染」的关键能力
    reported = [s for s in report["unparsed"]["segments"] if s.get("pollution_markers")]
    warned = [w for w in report["warnings"] if "疑似日志/文本特征" in w]
    diag_ok = bool(reported) and bool(warned)
    print(f"报告点出污染特征     : {'OK ' + str(reported[0]['pollution_markers']) if diag_ok else '不符 <<<'}")
    ok_all = ok_all and diag_ok

    # 判定结果：这份人为造坏的数据必须判 FAIL（否则说明阈值形同虚设）
    print(f"结论判定             : {report['verdict']}（人为造坏的数据应判 FAIL）")
    verdict_ok = report["verdict"] == "FAIL"
    ok_all = ok_all and verdict_ok

    print("")
    if ok_all:
        print("自测结果：全部通过（PASS）")
    else:
        print("自测结果：存在不符项（FAIL），请检查脚本逻辑")
    print(f"完整报告：{os.path.join(outdir, 'selftest_report.json')}")
    try:
        with open(os.path.join(outdir, "selftest_report.json"), "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
    except OSError:
        pass
    return 0 if ok_all else 1


# ───────────────────────────── 输出 ─────────────────────────────

def print_report(report: dict, logf=None) -> None:
    """把报告打成中文可读文本（同时写进 .log）。"""
    lines = []

    def w(s=""):
        lines.append(s)
        print(s)

    m = report["metrics"]
    tr = report["target_report"]
    mv = report["msgid"]

    w("=" * 74)
    w("EDV11P 桥接帧完整性校验报告")
    w("=" * 74)
    w(f"生成时间      : {report['generated_at']}")
    w(f"采集方式      : {report['mode']}  ({report['source']})")
    w(f"采集时长      : {m['span_s'] if m['span_s'] is not None else 'N/A'} s")
    w("")
    byte_rate_s = "N/A" if m["byte_rate"] is None else "%.1f B/s" % m["byte_rate"]
    frame_rate_s = "N/A" if m["frame_rate"] is None else "%.3f 帧/s" % m["frame_rate"]

    w("【流量】")
    w(f"  总字节数    : {m['total_bytes']:,}")
    w(f"  字节率      : {byte_rate_s}")
    w(f"  合法帧数    : {m['frames_ok']:,}")
    w(f"  帧率        : {frame_rate_s}")
    w("")
    w("【帧分类】")
    for k, n in report["by_op"].items():
        w(f"  op  {k:<16}: {n:,}")
    for k, n in report["by_type"].items():
        w(f"  type {k:<24}: {n:,}")
    if m["unknown_op_frames"]:
        w(f"  ⚠ 未识别 op 的帧: {m['unknown_op_frames']}")
    w("")
    w("【坏帧】")
    for k, label in Checker.BAD_KINDS.items():
        w(f"  {label:<26}: {m['bad_frames'][k]:,}")
    w(f"  {'坏帧合计':<26}: {m['bad_frames']['total']:,}   （坏帧率 {fmt_rate(m['bad_frame_rate'])}）")
    w("")
    w("【假同步 / 重同步】")
    w(f"  重同步次数  : {m['resync_events']:,} / 上限 {m['resync_limit']}"
      f"   （假同步丢帧估算率 {fmt_rate(m['fake_sync_loss_rate'])}）")
    w(f"  无法解析字节: {m['garbage_bytes']:,} 字节，分 {report['unparsed']['count']} 段"
      f"{'（已落盘 unparsed_*.bin）' if report['unparsed']['raw_dump'] else ''}")
    w("")
    ua = m["unattributed"]
    w("【未归属字节】（洁净判据 P1 / 章程 R1 口径）")
    if not ua["window_started"]:
        w(f"  统计窗口    : 未启动（采集时长未超过 warmup {ua['warmup_s']:.0f}s）→ 本项 N/A")
    else:
        w(f"  统计窗口    : 从 T+{ua['warmup_s']:.0f}s（流偏移 {ua['window_start_offset']}）起算")
        w(f"  窗口内收字节: {ua['window_bytes']:,}   其中合法帧 {ua['frame_bytes_in_window']:,}")
        w(f"  未归属字节  : {ua['unattributed_bytes']:,}   （未归属率 {fmt_rate(ua['unattributed_rate'])}）")
        if ua["unattributed_bytes"] == 0:
            w("  判读        : 窗口内每个字节都能归属到合法帧 → 数据流洁净")
        else:
            w("  判读        : 有字节不属于任何合法帧 → 见下方 unparsed 段与 warnings（坏帧/日志/误码）")
    w("")
    w("【msgid 连续性】")
    w(f"  判定依据    : {mv['basis']}")
    w(f"  不分组视图  : {mv['overall']['frames']:,} 帧，跳号 {mv['overall']['jumps']} 次"
      f"（{fmt_rate(mv['overall']['jump_rate'])}）")
    for g in mv["groups"]:
        flag = "" if g["eligible"] else "  [样本不足，不参与判定]"
        w(f"    op=0x{g['op']:02X} type=0x{g['type']:02X} {g['type_name']:<10}"
          f" {g['frames']:>7,} 帧  跳号 {g['jumps']:>4}  重复 {g['duplicates']:>3}"
          f"  {fmt_rate(g['jump_rate'])}{flag}")
    if mv["grouped_pass_ungrouped_fail"]:
        w("  说明        : 不分组视图跳号明显、分组视图正常 → msgid 按命令类别独立计数，不是丢帧")
    jumped = [j for j in mv["jumps"] if j["grouped"]]      # 只列分组视图的跳号（判定依据）
    for j in jumped[:10]:
        w(f"    跳号 @ {j['elapsed_s']}s  op=0x{j['op']:02X} type=0x{j['type']:02X}"
          f"  {j['prev_msgid']} → {j['cur_msgid']}（期望 {j['expected']}）")
    if len(jumped) > 10:
        w(f"    …（共 {len(jumped)} 条，完整明细见 JSON）")
    w("")
    w("【目标上报 0x70】")
    w(f"  帧数        : {tr['frames']:,}   载荷异常: {tr['payload_errors']}")
    if tr["frames"]:
        w(f"  每帧目标数  : min={tr['targets_per_frame']['min']}"
          f"  median={tr['targets_per_frame']['median']}"
          f"  max={tr['targets_per_frame']['max']}")
        hist = tr["targets_per_frame"]["histogram"]
        w(f"  分布        : " + "  ".join(f"{k}人×{v}" for k, v in hist.items()))
        w(f"  zone_id 集合: {tr['zone_ids']}")
        w(f"  各区最大目标: {dict(tr['zone_max_targets'])}")
    w("")
    w("【判定】（P0 前提 + 六条硬判据；任一 FAIL 即 FAIL，WARN 不阻断但须记录）")
    for c in report["checks"]:
        val = c["value"]
        if isinstance(val, float):
            val_s = fmt_rate(val)
        elif c["name"] == "未归属字节率" and c.get("extra"):
            val_s = "%s (%d B)" % (fmt_rate(val), c["extra"]["unattributed_bytes"])
        else:
            val_s = str(val)
        thr = c["threshold"]
        thr_s = fmt_rate(thr) if isinstance(thr, float) and c["name"] != "重同步绝对次数" else str(thr)
        note = f"   ({c['note']})" if c.get("note") else ""
        w(f"  {c.get('priority',''):<3}{c['name']:<12}: {val_s:>16}  需 {c['compare']} "
          f"{thr_s:>12}   → {c['result']}{note}")
    w("")
    w(f"  最终结论    : {report['verdict']}")
    if report["warnings"]:
        w("")
        w("【警告】（不影响 PASS/FAIL，但要在 Gate 评审里说明）")
        for s in report["warnings"]:
            w(f"  · {s}")
    if report["events"]:
        w("")
        w("【运行事件】")
        for s in report["events"]:
            w(f"  · {s}")
    if report["unparsed"]["segments"]:
        w("")
        w("【无法解析的字节段】（前 10 段）")
        for s in report["unparsed"]["segments"][:10]:
            w(f"  #{s['index']} 偏移 {s['offset']}  {s['size']} 字节  [{s['kind']}]"
              f"  可打印率 {s['printable_ratio']}")
            w(f"     hex: {s['hex']}")
            w(f"     ascii: {s['ascii']}")
            if s.get("pollution_markers"):
                w(f"     ⚠ 疑似污染特征: {s['pollution_markers']}")
            if s["dumped"]:
                w(f"     → 已存盘: {s['dumped']}")
    w("=" * 74)

    if logf is not None:
        logf.write("\n".join(lines) + "\n")


# ─────────────────── 静态「启动横幅门禁」（--check-banner） ───────────────────
#
# 背景（Gate-1 P1-07）：ASCII 文本 "EDK60" 的前两个字母就是 0x45 0x44 = 协议帧头。
# 只要固件把含 "ED" 的文案打到数据串口，解析器就会把它当帧头，后面 "K6"(0x4B36)
# 被当长度 → 长度非法 → 假同步。这个门禁不需要硬件，扫源码/镜像/配置即可。

CFG_BANNER = "CONFIG_APP_EDK60_STARTUP_BANNER"
CFG_PINMAP = "CONFIG_APP_EDK60_PRINT_PINMAP"
CFG_LOG_NONE = "CONFIG_LOG_DEFAULT_LEVEL_NONE"
CFG_LOG_LEVEL = "CONFIG_LOG_DEFAULT_LEVEL"
CFG_SEC_USB = "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG"
CFG_SEC_NONE = "CONFIG_ESP_CONSOLE_SECONDARY_NONE"

# 会往控制台写字的调用（不含 vsnprintf/snprintf 这类只拼缓冲区的函数）
LOG_CALL_RE = re.compile(
    r"(?<![A-Za-z0-9_])(printf|vprintf|puts|fputs|fwrite|putchar|"
    r"ESP_LOG[IEWDV]|ESP_EARLY_LOG[IEWDV]|esp_rom_printf)\s*\(")

# 用于「门禁自检」的样例：证明 45 44 规则真的生效
BANNER_SELFTEST_CASES = [("  EDK60 (ESP32-C6)", True), ("esp32c6 bridge", False)]


def _strip_c_comments(text: str) -> str:
    """去掉 C 注释但保留字符串字面量与换行（长度不变，便于按行号定位）。"""
    out = []
    i, n = 0, len(text)
    state = "code"
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if c == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if c == '"':
                state = "string"
            out.append(c)
            i += 1
            continue
        if state == "line":
            if c == "\n":
                state = "code"
                out.append(c)
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        # state == "string"
        if c == "\\" and i + 1 < n:
            out.append(c)
            out.append(text[i + 1])
            i += 2
            continue
        if c == '"':
            state = "code"
        out.append(c)
        i += 1
    return "".join(out)


def _decode_c_escapes(s: str) -> str:
    """把 C 字符串字面量里的转义还原成「真正会写出去的字节」（够用即可）。"""
    def repl(mobj):
        e = mobj.group(1)
        simple = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'",
                  "0": "\0"}
        if e in simple:
            return simple[e]
        if e.startswith("x") and len(e) > 1:
            try:
                return chr(int(e[1:], 16))
            except ValueError:
                return ""
        try:
            return chr(int(e, 8) & 0xFF)
        except ValueError:
            return ""
    return re.sub(r"\\(x[0-9A-Fa-f]{1,2}|[0-7]{1,3}|.)", repl, s)


FUNC_DEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*\)\s*\{?\s*$")


def scan_c_source(path: str) -> dict:
    """扫一个 C 源文件。

    返回 {'strings': [...], 'log_calls': [...]}；
    每条 string 带上下文：所在函数名 + 所属输出调用（printf/ESP_LOGx…）。
    这样才能区分「会被打到串口的文案」和「只是数据/断言的字符串」。
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        return {"error": str(e), "strings": [], "log_calls": []}
    clean = _strip_c_comments(text)

    strings, log_calls = [], []
    current_func = ""
    open_call = None          # 跨行的输出调用（例如 printf( "..." \n "..." ); ）
    for lineno, line in enumerate(clean.splitlines(), 1):
        mdef = FUNC_DEF_RE.match(line)
        if mdef and mdef.group(1) not in ("if", "for", "while", "switch", "return",
                                          "sizeof", "do", "else"):
            current_func = mdef.group(1)

        calls = [(m.group(1), m.start()) for m in LOG_CALL_RE.finditer(line)]
        for name, col in calls:
            log_calls.append({"line": lineno, "call": name, "func": current_func,
                              "snippet": line.strip()[:110]})
        if calls and ");" not in line[calls[0][1]:]:
            open_call = calls[0][0]
        elif open_call and ");" in line:
            open_call = None

        for mobj in re.finditer(r'"((?:[^"\\]|\\.)*)"', line):
            col = mobj.start()
            owner = None
            for name, ccol in calls:
                if ccol < col:
                    owner = name
            if owner is None and open_call is not None:
                owner = open_call
            strings.append({
                "line": lineno, "col": col + 1,
                "text": _decode_c_escapes(mobj.group(1)),
                "func": current_func,
                "call": owner,
            })
    return {"strings": strings, "log_calls": log_calls}


def scan_binary_strings(path: str, min_len: int = 4):
    """从 .bin/.elf 里抽出可打印字符串（含位置），返回 [{offset, text}]。"""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        return {"error": str(e), "strings": []}
    strings, start = [], None
    for idx, b in enumerate(data):
        printable = 32 <= b < 127 or b in (9, 10, 13)
        if printable:
            if start is None:
                start = idx
        else:
            if start is not None and idx - start >= min_len:
                strings.append({"offset": start,
                                "text": data[start:idx].decode("latin-1")})
            start = None
    if start is not None and len(data) - start >= min_len:
        strings.append({"offset": start, "text": data[start:].decode("latin-1")})
    return {"strings": strings}


def read_config_flags(paths) -> dict:
    """读 sdkconfig / sdkconfig.defaults* 里的关键开关；后读的覆盖先读的。"""
    flags = {}
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    if line.startswith("#"):
                        # "# CONFIG_X is not set" 表示 =n
                        mobj = re.match(r"#\s*(CONFIG_[A-Z0-9_]+)\s+is not set", line)
                        if mobj and mobj.group(1) in (
                                CFG_BANNER, CFG_PINMAP, CFG_LOG_NONE, CFG_LOG_LEVEL,
                                CFG_SEC_USB, CFG_SEC_NONE,
                                "CONFIG_APP_EDK60_DEMO_RADAR_BRIDGE_EDV11P"):
                            flags[mobj.group(1)] = "n"
                        continue
                    if "=" in line and line.startswith("CONFIG_"):
                        k, v = line.split("=", 1)
                        flags[k] = v
        except OSError as e:
            flags.setdefault("_errors", []).append("%s: %s" % (p, e))
    return flags


def _yn(flags, key: str) -> bool:
    return flags.get(key) == "y"


def default_banner_targets(root: str = ".", demo: str = "radar_bridge_edv11p"):
    """默认扫描范围：本次桥接构建的配置 + 源码 + 镜像。

    配置优先级（后者覆盖前者）：sdkconfig.defaults → sdkconfig.<demo> → sdkconfig.defaults.<demo>
    刻意**不**读仓库根 sdkconfig（那是上一次 menuconfig 的残留，可能属于别的 demo）。
    """
    cfg = []
    for name in ("sdkconfig.defaults", "sdkconfig." + demo, "sdkconfig.defaults." + demo):
        if os.path.isfile(os.path.join(root, name)):
            cfg.append(os.path.join(root, name))

    src = []
    for pat in ("components/edk60_board/*.c", "components/edk60_app/src/demo_dispatch.c",
                "main/*.c", "components/edk60_app/src/*%s*.c" % demo):
        src.extend(sorted(glob.glob(os.path.join(root, pat))))

    bins = []
    for d in sorted(glob.glob(os.path.join(root, "build-*%s*" % demo))):
        for name in ("edk60_demos.bin", "merged-binary.bin"):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                bins.append(p)
    return {"config": cfg, "source": src, "binary": bins, "demo": demo,
            "flags": read_config_flags(cfg)}


def _role_of(path: str, demo: str) -> str:
    base = os.path.basename(path).lower()
    if demo and demo in base:
        return "bridge_demo"
    if "edk60_board" in base:
        return "bsp"
    if "dispatch" in base:
        return "dispatch"
    return "other"


# 直接把字符写到 stdout 的调用（与 ESP_LOG* 不同：不会被日志级别编译期消掉）
PRINTF_CALLS = ("printf", "vprintf", "puts", "fputs", "fwrite", "putchar", "esp_rom_printf")


def _classify_source_string(rec, role: str) -> dict:
    """判断一条源码字符串「会不会被打到串口」，以及受哪个开关控制。"""
    text, func, call = rec["text"], rec.get("func", ""), rec.get("call")
    in_banner_fn = bool(re.search(r"(print_banner|print_pinmap|banner|pinmap)", func or "",
                                 re.I))
    if call in PRINTF_CALLS:
        kind = "banner_text" if in_banner_fn else "printf_text"
        active = "banner" if in_banner_fn else True
    elif call and call.startswith("ESP_"):
        kind = "log_text"
        active = "log"
    else:
        kind = "literal_text"
        active = False
    return {"kind": kind, "active": active}


def run_banner_check(args) -> int:
    """静态横幅门禁主流程：扫源码/镜像/配置，判定输出文案里是否有 45 44。"""
    demo = getattr(args, "banner_demo", None) or "radar_bridge_edv11p"
    targets = default_banner_targets(".", demo)
    flags = targets["flags"]
    strict = not args.banner_warn_only

    explicit = []
    for p in (args.banner_path or []):
        if os.path.isdir(p):
            explicit.extend(sorted(x for x in glob.glob(os.path.join(p, "*"))
                                   if os.path.isfile(x)))
        elif os.path.isfile(p):
            explicit.append(p)
        else:
            print("警告：--banner-path 不存在，已跳过：%s" % p, file=sys.stderr)

    src_files = list(dict.fromkeys(
        [p for p in (targets["source"] + explicit)
         if p.lower().endswith((".c", ".h", ".cc", ".cpp"))]))
    bin_files = list(dict.fromkeys(
        [p for p in (targets["binary"] + explicit)
         if p.lower().endswith((".bin", ".elf"))]))
    cfg_files = list(targets["config"])
    for p in explicit:
        if os.path.basename(p).startswith("sdkconfig") or p.lower().endswith(".config"):
            cfg_files.append(p)
    cfg_files = list(dict.fromkeys(cfg_files))
    if explicit:
        flags = read_config_flags(cfg_files)

    banner_on = _yn(flags, CFG_BANNER)
    pinmap_on = _yn(flags, CFG_PINMAP)
    log_none = _yn(flags, CFG_LOG_NONE) or flags.get(CFG_LOG_LEVEL) in ("0",)
    sec_usb = _yn(flags, CFG_SEC_USB)
    sec_none = _yn(flags, CFG_SEC_NONE)
    banner_active = banner_on or pinmap_on
    log_active = (not log_none) and sec_usb

    findings = []

    def add(kind, path, loc, text, active, note, sev=None):
        if sev is None:
            sev = "FAIL" if (strict or active) else "WARN"
        findings.append({"kind": kind, "file": path, "loc": loc, "text": text,
                         "active": bool(active), "severity": sev, "note": note})

    # ---- 1) 源码：字符串字面量里的 45 44（按输出上下文分级）----
    for p in src_files:
        sc = scan_c_source(p)
        role = _role_of(p, targets["demo"])
        if sc.get("error"):
            findings.append({"kind": "read_error", "file": p, "loc": "-", "text": "",
                             "active": False, "severity": "WARN",
                             "note": "读取失败：" + sc["error"]})
            continue
        for s in sc["strings"]:
            if "ED" not in s["text"]:
                continue
            cls = _classify_source_string(s, role)
            if cls["active"] == "banner":
                active, extra = banner_active, "属启动横幅/引脚表，开关当前%s" % (
                    "已打开" if banner_active else "关闭（潜在风险）")
            elif cls["active"] == "log":
                active, extra = log_active, "ESP_LOG 文案（日志级别当前%s）" % (
                    "开" if not log_none else "已关，编译期被消掉")
            elif cls["active"] is True:
                active, extra = True, "printf 类文案（不受日志级别影响，只要被调用就会写出）"
            else:
                active, extra = False, "非输出上下文（数据/断言字符串），当前不会写出"
            where = "在 %s() 里" % s["func"] if s.get("func") else ""
            # 非输出上下文的字符串不是「输出文案」，严格模式下也只记 WARN
            sev = None
            if cls["kind"] == "literal_text":
                sev = "WARN"
            elif cls["kind"] == "log_text" and not log_active:
                sev = "WARN"
            add(cls["kind"], p, "第 %d 行第 %d 列" % (s["line"], s["col"]),
                s["text"], active,
                "输出文案含相邻 45 44（ASCII \"ED\"）%s；%s" % (where, extra), sev=sev)

        # ---- 2) 源码：会写控制台的调用本身 ----
        # 桥接 demo 逐个报（工程门禁）；共享文件按类聚合成一条，避免刷屏
        printf_calls = [c for c in sc["log_calls"] if c["call"] in PRINTF_CALLS]
        esp_calls = [c for c in sc["log_calls"] if c["call"] not in PRINTF_CALLS]
        if role == "bridge_demo":
            for c in sc["log_calls"]:
                add("log_call", p, "第 %d 行" % c["line"], c["snippet"], True,
                    "%s() —— 桥接 demo 必须「零输出调用」（源码级门禁）" % c["call"],
                    sev="FAIL")
        else:
            if printf_calls:
                banner_pf = [c for c in printf_calls
                             if re.search(r"(print_banner|print_pinmap)", c.get("func", ""),
                                          re.I)]
                other_pf = [c for c in printf_calls if c not in banner_pf]
                if banner_pf:
                    lines = ",".join(str(c["line"]) for c in banner_pf[:8])
                    add("printf_call_group", p, "第 %s 行" % lines,
                        "%d 处 printf（横幅/引脚表函数内）" % len(banner_pf), banner_active,
                        "横幅/引脚表开关%s → 这些 printf %s" % (
                            "已开" if banner_active else "关闭",
                            "会真的把含 45 44 的文案写到串口"
                            if banner_active else "当前不会被调用"))
                if other_pf:
                    lines = ",".join(str(c["line"]) for c in other_pf[:8])
                    add("printf_call_group", p, "第 %s 行" % lines,
                        "%d 处 printf（其它函数内）" % len(other_pf), False,
                        "这些函数（复位原因/深睡唤醒打印等）只有其它 demo 会调用；"
                        "桥接构建不会走到 → 仅提示", sev="WARN")
            if esp_calls:
                lines = ",".join(str(c["line"]) for c in esp_calls[:8])
                add("log_call_group", p, "第 %s 行" % lines,
                    "%d 处 ESP_LOGx 调用" % len(esp_calls), log_active,
                    "日志级别当前%s → 这些调用%s" % (
                        "已关" if log_none else "开着",
                        "在编译期被消掉，不会写出字符" if log_none else "会真的写到串口"),
                    sev=("WARN" if log_none else "FAIL"))

    # ---- 3) 镜像：rodata 里的字符串（存在 ≠ 会输出，按类别分档）----
    for p in bin_files:
        bs = scan_binary_strings(p)
        if bs.get("error"):
            findings.append({"kind": "read_error", "file": p, "loc": "-", "text": "",
                             "active": False, "severity": "WARN",
                             "note": "读取失败：" + bs["error"]})
            continue
        seen = {}
        for s in bs["strings"]:
            if "ED" not in s["text"]:
                continue
            seen.setdefault(s["text"], s["offset"])
        for text, off in seen.items():
            is_banner = "EDK60" in text
            add("binary_text", p, "偏移 0x%X" % off, text,
                banner_active if is_banner else False,
                "镜像 rodata 存在含 45 44 的文案" +
                ("（横幅类，开关当前%s）" % ("开" if banner_active else "关")
                 if is_banner else "（是否输出取决于调用路径，需人工确认）"))

    # 4) 配置开关
    cfg_rows = [
        (CFG_BANNER, "y" if banner_on else "not set", banner_on,
         "启动横幅会 printf 一整套多行文案（含 \"  EDK60\"）"),
        (CFG_PINMAP, "y" if pinmap_on else "not set", pinmap_on,
         "引脚表会 printf 多行文案（首行含 \"[EDK60 引脚映射]\"）"),
        (CFG_LOG_NONE, "y" if log_none else "not set", not log_none,
         "关闭后 ESP_LOG* 在编译期被消掉，一个字符都不写"),
        (CFG_SEC_USB, "y" if sec_usb else "not set", sec_usb,
         "第二控制台挂在原生 USB 上：它输出的每个字符都会混进数据流"),
        (CFG_SEC_NONE, "y" if sec_none else "not set", sec_none,
         "关闭第二控制台 → 历史坑：系统启动卡死、USB 不枚举"),
    ]
    if sec_usb and not log_none:
        add("config", "sdkconfig", CFG_LOG_LEVEL, "%s=%s" % (CFG_LOG_LEVEL,
            flags.get(CFG_LOG_LEVEL, "?")), True,
            "日志级别未关闭且第二控制台是 USB → 日志会直接污染数据流", sev="FAIL")
    if sec_none:
        add("config", "sdkconfig", CFG_SEC_NONE, "y", True,
            "禁止关闭第二控制台（会让系统启动卡死、USB 不枚举）", sev="WARN")

    fails = [f for f in findings if f["severity"] == "FAIL"]
    warns = [f for f in findings if f["severity"] == "WARN"]
    verdict = "FAIL" if fails else "PASS"

    # ---- 打印 ----
    lines = []

    def w(s=""):
        lines.append(s)
        print(s)

    w("=" * 78)
    w("EDV11P 启动横幅门禁（静态检查，不需要硬件、不需要 pyserial）")
    w("=" * 78)
    w("规则：任何会写到数据串口的文案里，只要出现相邻字节 45 44（ASCII \"ED\"），")
    w("      解析器就会把它当帧头 → 假同步。识别到即判 FAIL 并给出位置。")
    chk = []
    for sample, should_hit in BANNER_SELFTEST_CASES:
        hit = "ED" in sample
        chk.append("%s→%s" % (repr(sample), "命中" if hit else "无命中"))
        if hit != should_hit:
            chk.append("(自检异常!)")
    w("门禁自检：" + "；".join(chk))
    w("")
    w("扫描范围：源码 %d 个、镜像 %d 个、配置 %d 个；当前 demo = %s"
      % (len(src_files), len(bin_files), len(cfg_files), targets["demo"] or "未识别"))
    for p in src_files:
        w("    src: %s" % p)
    for p in bin_files:
        w("    bin: %s" % p)
    if not bin_files:
        w("    ⚠ 未找到 build-%s 的镜像（.bin）→ 本次只做源码+配置检查；" % targets["demo"])
        w("      先跑 build_all.ps1 -Demo %s 再复测，才能覆盖「镜像 rodata」这一层。" % targets["demo"])
    w("")
    w("【关键开关】")
    for key, val, is_on, note in cfg_rows:
        w(f"  {key:<48}{val:>8}   {note}")
    w("")
    if log_none:
        w("  判读：日志宏已关闭 + 第二控制台在 USB → 当前构建不会输出日志字符；")
    w("        但横幅/引脚表文案仍在镜像里，一旦有人打开对应开关就会立刻污染。")
    w("")
    kind_names = {
        "banner_text": "启动横幅/引脚表文案（受 CONFIG 开关控制）",
        "printf_text": "printf 类输出文案（不受日志级别控制）",
        "log_text": "ESP_LOG 文案（日志级别关掉即编译期消失）",
        "literal_text": "非输出上下文字符串（数据/断言，不会写出）",
        "log_call": "桥接 demo 里的输出调用（源码零输出门禁）",
        "printf_call_group": "共享代码里的 printf 类调用（按文件聚合）",
        "log_call_group": "共享代码里的 ESP_LOGx 调用（按文件聚合）",
        "binary_text": "镜像 rodata 里的文案",
        "config": "构建配置风险项",
        "read_error": "读取失败",
    }
    per_kind = {}
    for f in findings:
        per_kind.setdefault(f["kind"], []).append(f)
    w(f"【命中】共 {len(findings)} 处（FAIL {len(fails)} / WARN {len(warns)}）")
    for k, items in sorted(per_kind.items(),
                           key=lambda kv: (-sum(1 for x in kv[1] if x["severity"] == "FAIL"),
                                           -len(kv[1]))):
        nf = sum(1 for x in items if x["severity"] == "FAIL")
        w(f"  {kind_names.get(k, k):<34} 共 {len(items):>3}（FAIL {nf}）")

    # 只详列「会输出 / 属横幅」的命中，其余折叠（JSON 里有全量）
    detail = [f for f in findings
              if f["kind"] in ("banner_text", "printf_text", "printf_call_group",
                               "config", "read_error", "log_call")
              or (f["kind"] == "log_call_group" and f["active"])
              or (f["kind"] == "log_text" and f["active"])
              or (f["kind"] == "binary_text" and "EDK60" in f["text"])]
    if detail:
        w("")
        w("【重点命中】（会被写到数据串口，或属启动横幅/引脚表）")
        for f in detail[:25]:
            w(f"  {f['severity']:<4} [{f['kind']}] {f['file']}  {f['loc']}")
            w(f"       文案/片段: {f['text']}")
            w(f"       说明: {f['note']}  (当前会被输出: {'是' if f['active'] else '否（潜在）'})")
        if len(detail) > 25:
            w(f"  …（重点命中 {len(detail)} 处，完整清单见 JSON）")
    others = [f for f in findings if f not in detail]
    if others:
        w("")
        w("【其它命中】（当前不会写出，列前 5 条示意；完整清单见 JSON）")
        for f in others[:5]:
            w(f"  {f['severity']:<4} [{f['kind']}] {f['file']}  {f['loc']}  {f['text'][:40]}")
    w("")
    w("【判定】")
    w(f"  模式        : {'严格（任何 45 44 命中即 FAIL）' if strict else '宽松（仅「会被输出」的命中判 FAIL）'}")
    w(f"  最终结论    : {verdict}")
    if verdict == "FAIL":
        w("  修复方向    : ⑧ 把横幅/引脚表里的 \"EDK60\" 改成等义但无 45 44 的写法")
        w("                （例如 \"ED-K60\"/\"EDK 60\"/\"E D K 6 0\" 或整行改写）；")
        w("                ⑨ 桥接构建保持 CONFIG_APP_EDK60_STARTUP_BANNER/PRINT_PINMAP 关闭；")
        w("                ⑩ 保持 CONFIG_LOG_DEFAULT_LEVEL_NONE=y，且不要关第二控制台。")
    w("=" * 78)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    try:
        os.makedirs(args.out, exist_ok=True)
        jp = os.path.join(args.out, "edv11p_banner_check_%s.json" % stamp)
        lp = os.path.join(args.out, "edv11p_banner_check_%s.log" % stamp)
        with open(lp, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        with open(jp, "w", encoding="utf-8") as f:
            json.dump(OrderedDict([
                ("tool", TOOL_NAME), ("tool_version", TOOL_VERSION),
                ("mode", "check-banner"),
                ("generated_at", time.strftime("%Y-%m-%d %H:%M:%S")),
                ("verdict", verdict),
                ("strict", strict),
                ("demo", targets["demo"]),
                ("scanned", {"source": src_files, "binary": bin_files, "config": cfg_files}),
                ("config_flags", OrderedDict(
                    (k, flags.get(k, "not set")) for k, _v, _o, _n in cfg_rows)),
                ("findings", findings),
                ("rule", "输出文案中出现相邻字节 45 44（ASCII \"ED\"）即判 FAIL；"
                         "45 44 是 EDV11P 帧头，会触发假同步"),
            ]), f, ensure_ascii=False, indent=2)
        print(f"JSON 报告: {jp}")
        print(f"文本日志: {lp}")
    except OSError as e:
        print("警告：报告写入失败：%s" % e, file=sys.stderr)

    return 0 if verdict == "PASS" else 1


# ───────────────────────────── 主流程 ─────────────────────────────

def _import_serial():
    """延迟导入 pyserial，给出可执行的安装提示（--file/--selftest 不需要它）。"""
    try:
        import serial                      # noqa: F401
        from serial.tools import list_ports  # noqa: F401
        return serial
    except ImportError:
        print("错误：--port 模式需要 pyserial，本机没有安装。", file=sys.stderr)
        print("  安装：  py -3 -m pip install pyserial", file=sys.stderr)
        print("          （本机 `python` 是应用商店别名不可用，请用 py -3）", file=sys.stderr)
        print("  或直接用 ESP-IDF 自带的 Python（里面已经有 pyserial）：", file=sys.stderr)
        print(r"          C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe "
              r"tools\edv11p_frame_check.py --port COM6", file=sys.stderr)
        print("  提示：--file 与 --selftest 模式不需要 pyserial。", file=sys.stderr)
        sys.exit(2)


def run_port(args, ck: Checker, outdir: str) -> dict:
    """串口实时采集 + 逐帧校验，时长到点自动收尾。"""
    serial = _import_serial()
    duration = args.duration if args.duration is not None else GATE2_DURATION

    print(f"打开串口 {args.port} @ {args.baud} …")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except Exception as e:
        print(f"错误：打开串口失败：{e}", file=sys.stderr)
        sys.exit(3)

    rawf = None
    if args.save_raw:
        rawf = open(args.save_raw, "wb")
        print(f"原始字节流同时存盘：{args.save_raw}")

    ck.mark_start()
    print(f"开始校验，计划采集 {duration:.0f} 秒（Ctrl+C 可提前结束并照样出报告）…")
    last_status = 0.0
    interrupted = False
    read_errors = []
    try:
        while ck.elapsed() < duration:
            try:
                data = ser.read(max(1, ser.in_waiting or 4096))
            except Exception as e:
                read_errors.append(str(e))
                ck.events.append("串口读取异常：%s" % e)
                if len(read_errors) > 20:
                    ck.events.append("串口连续异常，提前结束采集")
                    break
                time.sleep(0.2)
                continue
            if data:
                if rawf is not None:
                    rawf.write(data)
                ck.feed(data, now=ck.elapsed())
            if ck.elapsed() - last_status >= args.status_every:
                last_status = ck.elapsed()
                print(f"  [{ck.elapsed():7.1f}s] 字节 {ck.total_bytes:,}  帧 {ck.frames_ok:,}"
                      f"  坏帧 {sum(ck.bad.values())}  重同步 {ck.resync_events}")
    except KeyboardInterrupt:
        interrupted = True
        print("\n收到 Ctrl+C，提前收尾并照常出报告。")
    finally:
        try:
            ser.close()
        except Exception:
            pass
        if rawf is not None:
            rawf.close()
    ck.finish()
    span = ck.elapsed()

    extra = []
    if interrupted:
        extra.append("采集被 Ctrl+C 提前中断（时长 %.1fs，未达计划 %.0fs），结论仅代表已采集区间" % (span, duration))
    if span < 0.9 * duration:
        extra.append("实测采集时长 %.1fs 不足计划 %.0fs 的 90%%，不满足 Gate-2「30 分钟连续运行」要求" % (span, duration))
    if not interrupted and span > duration + 5:
        extra.append("采集超时 %.1fs，请检查串口读取是否被阻塞" % (span - duration))
    return ck.collect("port", "%s @%d" % (args.port, args.baud), span, extra)


def run_file(args, ck: Checker) -> dict:
    """离线分析已保存的原始字节流（不需要 pyserial）。"""
    path = args.file
    if not os.path.exists(path):
        print("错误：文件不存在：%s" % path, file=sys.stderr)
        sys.exit(4)
    size = os.path.getsize(path)
    print(f"分析文件 {path}（{size:,} 字节）…")
    span = args.duration      # 离线模式必须显式给 --duration 才能算速率
    extra = []
    if span is None:
        extra.append("离线模式未提供 --duration，帧率/字节率无法计算（原始字节流本身不含时间戳）")
        ck.disable_warmup()
        extra.append("离线且无时基：未归属字节统计退化为「从流首起算」（头部 bootloader 文本会被计入）")
    else:
        ck.set_synthetic_time(span, size)
        extra.append("离线模式的『时间点』按字节位置线性插值到 --duration，仅用于对齐/画图，不代表真实到达时刻")
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            ck.feed(chunk, now=None)
    ck.finish()
    if span is not None and span < 0.9 * GATE2_DURATION:
        extra.append("提供的 --duration（%.0fs）不足 30 分钟的 90%%，不满足 Gate-2 时长要求" % span)
    return ck.collect("file", os.path.basename(path), span, extra)


def build_argparser() -> argparse.ArgumentParser:
    epilog = RULES_TEXT + """
示例（本机 python 不在 PATH，用 py -3 启动器；详见 docs\\project\\edv11p\\frame-check-test-plan.md）
  # 实时跑 30 分钟，边测边出报告与日志（Gate-2 主证据）
  py -3 tools\\edv11p_frame_check.py --port COM6 --baud 921600 --duration 1800

  # 同时把原始流存盘 + 把人数时间序列导出 CSV
  py -3 tools\\edv11p_frame_check.py --port COM6 --save-raw capture.bin --csv people.csv

  # 采集完再离线复算（不接硬件，不需要 pyserial）
  py -3 tools\\edv11p_frame_check.py --file capture.bin --duration 1800

  # 排查「日志污染」：把解析不了的字节段逐段存成 unparsed_<n>.bin
  py -3 tools\\edv11p_frame_check.py --file capture.bin --duration 1800 --raw-dump

  # 无硬件自测（合成流，含校验错帧 / 截断帧 / 垃圾字节段）
  py -3 tools\\edv11p_frame_check.py --selftest

  # 静态「启动横幅门禁」：不需要硬件，扫源码/镜像/配置里的 45 44 文案（Gate-1 P1-07）
  py -3 tools\\edv11p_frame_check.py --check-banner
  # 只把「确实会被输出」的命中判 FAIL（已知文案不会被调用时的降级复核）
  py -3 tools\\edv11p_frame_check.py --check-banner --banner-warn-only
  # 指定要扫的文件或目录（可多次）
  py -3 tools\\edv11p_frame_check.py --check-banner --banner-path components\\edk60_board

横幅门禁规则
  · 任何会写到数据串口的文案里出现相邻字节 45 44（ASCII "ED"）→ FAIL 并给出
    文件:行:列 / 镜像偏移。因为 45 44 就是帧头，后面 "K6"(0x4B36) 会被当长度 → 假同步。
  · 默认严格模式；--banner-warn-only 时只有「当前确实会被输出」的命中判 FAIL。
  · 另外检查：桥接 demo 里出现 printf/ESP_LOGx（源码零输出门禁）、
    日志级别未关 + 第二控制台在 USB（日志会直接污染数据流）、关闭第二控制台（历史坑）。

产出（默认全部落在 logs\\edv11p\\，可用 --out 改）
  logs\\edv11p\\edv11p_frame_check_<时间戳>.json   机器可读全量报告（Gate 归档用）
  logs\\edv11p\\edv11p_frame_check_<时间戳>.log    同内容的中文文本日志
  logs\\edv11p\\edv11p_banner_check_<时间戳>.json  --check-banner 的报告
  logs\\edv11p\\unparsed_<n>.bin                   仅在 --raw-dump 时生成
  logs\\edv11p\\edv11p_selftest_<时间戳>\\          仅 --selftest 生成（合成流 + 报告）
  退出码：0 = PASS（或自测通过）；1 = FAIL（或自测不符）；2/3/4 = 环境错误

装 pyserial（只有 --port 模式需要；本机当前未安装）
  py -3 -m pip install pyserial
  或直接用 ESP-IDF 自带 Python（里面已有 pyserial）：
  C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools\\edv11p_frame_check.py --port COM6
"""
    p = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="EDK60 ⇄ EDV11P 桥接帧完整性校验工具：逐帧校验 + 坏帧/假同步/msgid/目标上报统计 + PASS/FAIL 裁定",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    src = p.add_mutually_exclusive_group()
    src.add_argument("--port", metavar="COMx", help="实时读取该串口（需要 pyserial）")
    src.add_argument("--file", metavar="capture.bin", help="离线分析已保存的原始字节流（不需要 pyserial）")
    src.add_argument("--selftest", action="store_true", help="离线自测：跑一段含已知缺陷的合成流并核对统计值")
    src.add_argument("--check-banner", action="store_true",
                     help="静态「启动横幅门禁」：扫源码/镜像/配置，输出文案里出现 45 44（\"ED\"）即 FAIL（不需要硬件）")

    p.add_argument("--baud", type=int, default=921600, help="波特率，默认 921600（EDV11P 出厂值）")
    p.add_argument("--duration", type=float, default=None,
                   help="采集时长/秒，--port 模式默认 1800（30 分钟）；--file 模式必须显式给出才能算帧率")
    p.add_argument("--warmup", type=float, default=DEFAULT_WARMUP_S,
                   help="未归属字节统计窗口的起算点/秒，默认 10（跳过上电 bootloader 文本）")
    p.add_argument("--save-raw", metavar="capture.bin", default=None,
                   help="--port 模式同时把原始字节流存盘，便于事后离线复算")
    p.add_argument("--csv", metavar="out.csv", default=None,
                   help="把人数时间序列导出 CSV（elapsed_s,wallclock,frame_no,zone_id,target_count,frame_total_targets）")
    p.add_argument("--raw-dump", action="store_true",
                   help="把无法解析的字节段存成 unparsed_<n>.bin（排查日志污染的关键手段）")
    p.add_argument("--max-unparsed", type=int, default=50,
                   help="--raw-dump 最多落盘多少段，默认 50（超出只记账不落盘，防止刷爆磁盘）")
    p.add_argument("--banner-path", action="append", metavar="PATH", default=None,
                   help="--check-banner 额外要扫的文件或目录（可多次）；默认自动发现 sdkconfig*/build-*/*.bin/桥接源码")
    p.add_argument("--banner-demo", metavar="NAME", default="radar_bridge_edv11p",
                   help="--check-banner 针对哪个 demo 构建，默认 radar_bridge_edv11p")
    p.add_argument("--banner-warn-only", action="store_true",
                   help="--check-banner 宽松模式：只有「当前确实会被输出」的 45 44 命中才判 FAIL")
    p.add_argument("--out", "--outdir", dest="out", default=DEFAULT_OUTDIR,
                   help="报告 / 日志 / unparsed_*.bin 的输出目录，默认 %s（相对当前工作目录，"
                        "推荐在仓库根运行）；--outdir 为等价别名" % DEFAULT_OUTDIR)
    p.add_argument("--status-every", type=float, default=10.0, help="--port 模式每隔多少秒打一行进度，默认 10")
    p.add_argument("--msgid-min-samples", type=int, default=20,
                   help="msgid 分组判定的最小样本帧数，默认 20（少于此数的组只报告不判定）")
    return p


def main(argv=None) -> int:
    setup_stdout()
    args = build_argparser().parse_args(argv)

    if not (args.port or args.file or args.selftest or args.check_banner):
        build_argparser().print_help()
        print("\n错误：必须指定 --port / --file / --selftest / --check-banner 之一。", file=sys.stderr)
        return 2

    if args.selftest:
        return run_selftest(args.out)

    if args.check_banner:
        return run_banner_check(args)

    outdir = args.out
    # 默认目录（logs\edv11p）不存在时自动创建；用户显式指定的目录同样自动创建
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError as e:
        print("错误：无法创建输出目录 %s：%s" % (outdir, e), file=sys.stderr)
        return 4
    stamp = time.strftime("%Y%m%d-%H%M%S")
    json_path = os.path.join(outdir, "edv11p_frame_check_%s.json" % stamp)
    log_path = os.path.join(outdir, "edv11p_frame_check_%s.log" % stamp)

    ck = Checker(raw_dump=args.raw_dump, outdir=outdir, max_unparsed=args.max_unparsed,
                 csv_path=args.csv, msgid_min_samples=args.msgid_min_samples,
                 warmup_s=args.warmup)
    report = run_port(args, ck, outdir) if args.port else run_file(args, ck)

    report["artifacts"] = OrderedDict([
        ("json", os.path.abspath(json_path)),
        ("log", os.path.abspath(log_path)),
        ("csv", os.path.abspath(args.csv) if args.csv else None),
        ("raw_capture", os.path.abspath(args.save_raw) if args.save_raw else None),
        ("unparsed_dir", os.path.abspath(outdir) if args.raw_dump else None),
        ("rules", RULES_TEXT),
    ])

    print("")
    with open(log_path, "w", encoding="utf-8") as logf:
        print_report(report, logf)
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    print(f"JSON 报告: {json_path}")
    print(f"文本日志: {log_path}")
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
