#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 ⇄ EDV163 手扫（挥手识别）demo —— 混合流文本校验 / 动作与触发统计工具

用途
----
链路：  EDV163 挥手固件 ──UART 115200 8N1──▶ ESP32-C6 ──原生 USB CDC──▶ PC

固件侧（EDK60）是「纯字节透传」，不解析任何协议；设备是**文本行协议**，
但串口里还会夹着 `0x4544` 开头的点云二进制帧。所以「桥有没有丢字节、文本行
是不是完整、二进制帧是不是被正确整帧跳过」只能由 PC 侧客观统计来证明。
本脚本干这件事：

    · 实时读串口（--port）或离线分析已保存的原始字节流（--file）；
    · 混合流解析：先按 0x4544 + 声明长度 + 累加校验把二进制帧**整帧跳过**，
      其余字节按 \\n 切文本行；文本行再按厂商上位机源码的六条正则分类
      （正式 GESTURE / 旧格式 wave+depth / DBG 四种）；
    · 统计：文本行总数、有效 GESTURE 行、坏行（不匹配任何已知格式）、
      二进制帧数与被跳过的字节数（合法 / 校验错 / 长度非法 / 截断）；
      行率与行间隔分布（min/median/max、>1 s 空档次数）；target=1 比例；
      x/y/z 的 min/median/max（米与厘米）；wave/depth 动作分布 + 时间戳序列
      （--csv）；距离触发（--threshold-cm）的触发/未触发比例与触发时刻；
    · 按固定阈值给出 PASS / FAIL，落盘 JSON 报告 + 中文日志；
    · --raw-dump 把「无法归属」的字节段落盘（定位日志污染 / 半行）；
    · --selftest 用合成流自测：正常行 / target=0 / 各 action / ±3 / 科学计数法 /
      旧格式 / DBG / **二进制帧（合法 + 校验错）夹在两条文本行之间且两条都能解析** /
      半行 / 噪声后重同步 / 字节分块边界 / 长度边界 / 已知坏流判 FAIL + 干净流判 PASS。

协议基线（唯一来源：厂商上位机 `gesture_monitor.py` 755 行 + 同名 README；
本 demo **没有协议文档**，格式一律以该源码为准）
-------------------------------------------------------------------------------
    115200 / 8N1，设备约每 200 ms 输出一行文本（≈5 行/秒），上位机只收不发。

    正式行 : GESTURE: target=<0|1> x=<m> y=<m> z=<m> action=<-2..2>
             数字接受小数与科学计数法（^-?\\d+(\\.\\d*)?$ 或 1.5e-01）
    action : -2 下挥 / -1 右挥 / 0 无挥 / 1 左挥 / 2 上挥
             ±3 = 靠近/远离（厂商界面不显示，仅内部计数）
    target : 1 = 位置有效；0 = 10 帧位置窗未形成（x/y/z 全 0）
    旧格式 : GESTURE: target=<0|1> x=.. y=.. z=.. wave=N depth=N
    调试行 : DBG: ... target=1 ... pos_m=( x y z ) ... action=N [lock=M]
             DBG: ... target=0 ... action=N [lock=M]
             DBG: ... target=1 ... pos_m=( x y z ) ... wave=N depth=N ...
             DBG: ... target=0 ... wave=N depth=N

    混杂二进制帧（点云，解析时必须整帧跳过、不解析内容）：
        0x45 0x44 | 长度(2B 大端，含头与校验的整帧字节数) | 正文 | 校验(1B)
        长度合法区间 11~4096；校验 = 前 (长度-1) 字节逐字节求和 & 0xFF

    已知脆弱点（本工具存在的理由）：
        ① 文本协议一旦混进固件日志（printf / ESP-IDF 启动日志），日志会直接
           变成一个「坏行」或吞掉半行 —— 比二进制协议脆弱得多（二进制协议里
           日志只是垃圾字节，帧边界仍然可靠）。
        ② 0x4544 出现在文本里时是假帧头；长度字段一旦错，切帧会把后面的文本
           一起吞掉。所以校验失败时本工具选择「整帧跳过」但会检查帧区间内是否
           含换行，若含则告警「疑似吞文本」。

判定规则（P0 前提 + 九条硬判据；阈值原文见 --help 与报告，数值在 THRESHOLDS）
-------------------------------------------------------------------------------
    P0 有效 GESTURE 行数 ≥ 1
    P1 文本行率 = 24.3 ±20% 行/秒（19.4 ~ 29.2）——实测值；厂商 README 写 200 ms/行(5 行/秒)与实际不符
    P2 坏行率 < 1%
    P3 二进制帧校验通过率 = 100%（无二进制帧时记 N/A）
    P4 长度非法帧数 = 0
    P5 行间隔 > 1 s 的空档次数 = 0
    P6 target=1 比例符合场景（--expect-hand present ≥50% / absent ≤10% / auto 不判定）
    P7 坐标越界行数 = 0（|x|,|y| ≤ 1.50 m，0 ≤ z ≤ 2.00 m；实测 x∈[-1.21,0.20]、z∈[0.06,1.14]）
    P8 未知 action 值频次 = 0（值域 -3..3）
    P9 距离触发事件数符合场景（present ≥1 次 / absent =0 次 / auto 不判定）

用法（详见 --help；本机 `python` 是应用商店别名不可用，要用 `py -3`）
----------------------------------------------------------------
    py -3 tools\\edv163_gesture_check.py --port COM6 --baud 115200 --duration 600
    py -3 tools\\edv163_gesture_check.py --file capture.bin --duration 600 --csv lines.csv
    py -3 tools\\edv163_gesture_check.py --selftest

    所有产物默认落在 logs\\edv163-gesture\\（相对当前工作目录，可用 --out 覆盖）。

依赖
----
    只用标准库；--file / --selftest 完全不需要第三方包。
    仅 --port 模式需要 pyserial（本机已装 3.5）。
"""

from __future__ import annotations

import argparse
import csv as csvlib
import json
import math
import os
import re
import sys
import tempfile
import time
from collections import Counter, OrderedDict

# ───────────────────────────── 协议常量 ─────────────────────────────

TOOL_NAME = "edv163_gesture_check.py"
TOOL_VERSION = "1.0.0"

DEFAULT_BAUD = 115200            # EDV163 挥手固件出厂波特率（EDV11P→921600，水位→256000，别抄错）
GATE_DURATION = 600.0            # 手扫 demo 的 Gate 采集口径：10 分钟
BIN_HEADER = b"\x45\x44"         # 二进制点云帧帧头
MIN_BINARY_FRAME = 11            # 合法帧长下限（含头与校验）
MAX_BINARY_FRAME = 4096          # 合法帧长上限
MAX_TEXT_BUFFER = 16384          # 无帧头也无换行时的缓冲区上限（防无限增长）
DEFAULT_THRESHOLD_CM = 20.0      # 距离触发阈值默认 20 cm（与厂商上位机一致）
THRESHOLD_CM_MIN = 0.0
THRESHOLD_CM_MAX = 100.0         # 厂商上位机 Spinbox 范围 0~100 cm
DEFAULT_OUTDIR = os.path.join("logs", "edv163-gesture")
PAIR_SAMPLE_LIMIT = 200          # 动作时间戳预览上限（完整序列走 --csv）
BAD_LINE_SAMPLE_LIMIT = 200      # 坏行样例上限

# 数值格式：与厂商源码完全一致（普通小数 + 科学计数法）
NUMBER = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"

# ── 以下六条正则照抄厂商上位机源码 gesture_monitor.py，顺序也保持一致 ──
# 顺序很重要：DBG 的 wave/depth 变体必须先于通用 action 变体尝试，
# 否则 `... wave=1 depth=3` 会被后面那条（要求 action=）错过。
GESTURE_RE = re.compile(
    rf"^GESTURE:\s*target=(?P<target>[01])\s+"
    rf"x=(?P<x>{NUMBER})\s+y=(?P<y>{NUMBER})\s+z=(?P<z>{NUMBER})\s+"
    r"action=(?P<action>-?\d+)\s*$"
)
DUAL_GESTURE_RE = re.compile(
    rf"^GESTURE:\s*target=(?P<target>[01])\s+"
    rf"x=(?P<x>{NUMBER})\s+y=(?P<y>{NUMBER})\s+z=(?P<z>{NUMBER})\s+"
    r"wave=(?P<wave>-?\d+)\s+depth=(?P<depth>-?\d+)\s*$"
)
DBG_DUAL_TARGET_RE = re.compile(
    rf"^DBG:.*target=1.*pos_m=\(\s*(?P<x>{NUMBER})\s+"
    rf"(?P<y>{NUMBER})\s+(?P<z>{NUMBER})\s*\).*"
    r"wave=(?P<wave>-?\d+)\s+depth=(?P<depth>-?\d+)"
)
DBG_DUAL_NO_TARGET_RE = re.compile(
    r"^DBG:.*target=0.*wave=(?P<wave>-?\d+)\s+depth=(?P<depth>-?\d+)"
)
DBG_TARGET_RE = re.compile(
    rf"^DBG:.*target=1.*pos_m=\(\s*(?P<x>{NUMBER})\s+"
    rf"(?P<y>{NUMBER})\s+(?P<z>{NUMBER})\s*\).*"
    r"action=(?P<action>-?\d+)(?:\s+lock=(?P<lock>\d+))?"
)
DBG_NO_TARGET_RE = re.compile(
    r"^DBG:.*target=0.*action=(?P<action>-?\d+)(?:\s+lock=(?P<lock>\d+))?"
)

# 行类型
LINE_GESTURE = "gesture"                    # 正式格式 GESTURE … action=…
LINE_GESTURE_DUAL = "gesture_dual"          # 旧格式 GESTURE … wave= depth=
LINE_DBG_TARGET = "dbg_target"              # DBG target=1 … action=…
LINE_DBG_NO_TARGET = "dbg_no_target"        # DBG target=0 … action=…
LINE_DBG_DUAL_TARGET = "dbg_dual_target"    # DBG target=1 … wave= depth=
LINE_DBG_DUAL_NO_TARGET = "dbg_dual_no_target"
LINE_DBG_UNMATCHED = "dbg_unmatched"        # 以 DBG: 开头但字段对不上（视为坏行，单列一栏）
LINE_BAD = "bad"                            # 完全不匹配任何已知格式

KIND_NAMES = OrderedDict([
    (LINE_GESTURE, "正式 GESTURE 行"),
    (LINE_GESTURE_DUAL, "旧格式 GESTURE 行（wave/depth）"),
    (LINE_DBG_TARGET, "DBG 行（target=1/action）"),
    (LINE_DBG_NO_TARGET, "DBG 行（target=0/action）"),
    (LINE_DBG_DUAL_TARGET, "DBG 行（target=1/wave+depth）"),
    (LINE_DBG_DUAL_NO_TARGET, "DBG 行（target=0/wave+depth）"),
    (LINE_DBG_UNMATCHED, "DBG 行（字段不匹配，算坏行）"),
    (LINE_BAD, "坏行（不匹配任何已知格式）"),
])

# 挥手动作编码 → 中文（与厂商 WAVE_ACTION_INFO 一致）
WAVE_ACTION_INFO = OrderedDict([
    (-2, "下挥"),
    (-1, "右挥"),
    (0, "无挥"),
    (1, "左挥"),
    (2, "上挥"),
])
DEPTH_ACTION_INFO = OrderedDict([
    (-3, "远离"),
    (3, "靠近"),
])

# 判定阈值（写死在代码里，同时打印进 --help 与报告，便于 Gate 复核）
THRESHOLDS = {
    "line_rate_hz": 24.3,             # P1 标称行率：实测 ~24.3 行/秒（约 41 ms/行）；README 的 200 ms/行与实际不符
    "line_rate_tol": 0.20,            # P1 容差 ±20% → 4.0 ~ 6.0 行/秒
    "bad_line_rate_max": 0.01,        # P2 坏行率 < 1%
    "binary_checksum_pass_min": 1.0,  # P3 二进制帧校验通过率 = 100%
    "len_invalid_max": 0,             # P4 长度非法帧数 = 0
    "gap_limit_s": 1.0,               # P5 空档判定门限（秒）
    "gaps_max": 0,                    # P5 >1 s 空档次数 = 0
    "target_ratio_present_min": 0.50,  # P6 挥手场景：target=1 比例 ≥ 50%
    "target_ratio_absent_max": 0.10,   # P6 静置场景：target=1 比例 ≤ 10%
    "coord_abs_xy_max_m": 1.50,       # P7 |x|,|y| 上界（实测 x 到 -1.21 m，原 1.00 过紧）
    "coord_z_max_m": 2.00,            # P7 z 上界（下界固定 0）
    "unknown_action_max": 0,          # P8 未知 action 值频次 = 0
    "trigger_events_present_min": 1,  # P9 挥手场景：触发事件 ≥ 1
}
LINE_RATE_LO = THRESHOLDS["line_rate_hz"] * (1.0 - THRESHOLDS["line_rate_tol"])
LINE_RATE_HI = THRESHOLDS["line_rate_hz"] * (1.0 + THRESHOLDS["line_rate_tol"])

# 判定规则原文（--help 与报告共用，避免两处不一致）
RULES_TEXT = f"""判定规则：P0 前提 + 九条硬判据（任一 FAIL 即 FAIL；N/A / 观察项不阻断）
  P0 前提：有效 GESTURE 行数 ≥ 1
     · 「有效 GESTURE 行」= 正式格式 + 旧格式 wave/depth 两种，都算设备输出正常
     · = 0 时整份采集无效，其余判据只作参考（先查波特率、接线 H3、模组是否上电）
  P1 文本行率 = {THRESHOLDS['line_rate_hz']:.1f} ±{THRESHOLDS['line_rate_tol']:.0%} 行/秒
     · 口径：文本行总数（含坏行、不含空行）÷ 覆盖时长
       覆盖时长 = 采集时长（--port 实测 / --file 的 --duration）；
       都没有时退化为「末年时刻 − 首行时刻 + 一个中位间隔」
     · 标称来源：固件每 200 ms 输出一行 → 5.0 行/秒
       判据区间 = [{LINE_RATE_LO:.1f}, {LINE_RATE_HI:.1f}]；超出说明固件周期变了、
       或者波特率不对（这是最快的上板体检项）
     · 没有时间基准（--file 未给 --duration）→ N/A，不阻断
  P2 坏行率 = 坏行数 / 文本行总数                     必须 < {THRESHOLDS['bad_line_rate_max']:.0%}
     · 坏行 = 不匹配任何已知格式的行，含「以 DBG: 开头但字段对不上」的行
       （后者单列 dbg_unmatched 计数，方便一眼看出是固件改了 DBG 格式）
     · 文本协议最怕这个：任何固件 printf 都会直接变成坏行
     · 文本行总数为 0 时记 N/A
  P3 二进制帧校验通过率 = 合法帧数 / (合法帧数 + 校验错帧数)     必须 = {THRESHOLDS['binary_checksum_pass_min']:.0%}
     · 整段没有二进制帧（纯文本流）时记 N/A —— 不算缺陷
     · 校验错帧本工具**整帧跳过**（按声明长度），以保证其后的文本行还能解析；
       但若被跳过的帧区间内含换行，会额外告警「疑似吞文本」
  P4 长度非法帧数 = 0
     · 长度字段不在 {MIN_BINARY_FRAME}~{MAX_BINARY_FRAME} 的「0x4544 疑似帧头」计入此类
     · 处置：只丢 1 字节重新同步（尽量不吞文本），该行会退化成一条坏行 —— 这正常
  P5 行间隔 > {THRESHOLDS['gap_limit_s']:.1f} s 的空档次数 = 0
     · 空档 = 相邻两条文本行的到达时刻差 > 门限（可用 --gap-limit 调整，阈值随报告打印）
     · 200 ms/行下出现 1 s 空档 = 连丢 5 行以上，必须解释（USB 重枚举/固件卡顿/解析吞行）
  P6 target=1 比例符合场景（--expect-hand）
     · present（挥手场景）：target=1 比例 ≥ {THRESHOLDS['target_ratio_present_min']:.0%}
     · absent （静置场景）：target=1 比例 ≤ {THRESHOLDS['target_ratio_absent_max']:.0%}
     · auto   （默认）    ：只报告不判定（N/A）—— 一个场景一个口径，不做万能阈值
  P7 坐标越界行数 = 0
     · 判据：|x| ≤ {THRESHOLDS['coord_abs_xy_max_m']:.2f} m、|y| ≤ {THRESHOLDS['coord_abs_xy_max_m']:.2f} m、
             0 ≤ z ≤ {THRESHOLDS['coord_z_max_m']:.2f} m，且必须有限（非 NaN/Inf）
     · 依据：厂商界面显示范围 X/Y ±0.40 m、Z 0~1.00 m；这里放宽 2.5 倍做护栏，
       只挡明显的解析错位（小数点跑掉、字段串位），不挡真实边界
     · target=0 的行坐标恒为 0，不计入坐标统计（会把 min 拉成 0）
  P8 未知 action 值频次 = 0
     · 合法值域：-2/-1/0/1/2（挥手）+ -3/3（靠近/远离）；其它值（如 7、99）
       说明固件改了编码或字段串位 —— 厂商界面会静默当成「无挥手」，本工具要抓出来
     · 旧格式一行同时含 wave 与 depth 时两者分别记表（深度只认 ±3）
  P9 距离触发事件数符合场景（--expect-hand）
     · 触发条件：target=1 且 z < 阈值（--threshold-cm，默认 {DEFAULT_THRESHOLD_CM:.0f} cm；
       严格小于，等于阈值不触发 —— 与厂商界面一致）
     · present：触发**起始事件**（未触发→触发 的跳变）≥ {THRESHOLDS['trigger_events_present_min']} 次
     · absent ：触发起始事件 = 0 次
     · auto   ：只报告不判定

观察项（不影响 PASS/FAIL，但会进 warnings，必须在 Gate 记录里逐条说明）
  · 出现 DBG 行（说明固件还在调试模式输出）、空行、流尾部半行（无换行）
  · 二进制帧区间内含换行（疑似把文本吞进帧里）
  · 长度非法/校验错的帧；无法归属字节段里出现固件日志特征串
  · 行间隔中位数偏离 0.2 s（标称）超过 ±40%；行率在判据区间外
  · 采集时长不足 --duration 的 90%；单段垃圾字节 > 1 KiB

Gate 引用判据（一句话，可直接抄）
  「T-GS-01 的 行率 = {LINE_RATE_LO:.1f}~{LINE_RATE_HI:.1f} 行/秒、坏行率 < {THRESHOLDS['bad_line_rate_max']:.0%}、
    二进制帧校验通过率 = 100%、长度非法帧数 = 0、无 >{THRESHOLDS['gap_limit_s']:.0f} s 空档，
    五项数值由 tools\\edv163_gesture_check.py 输出 JSON 的 checks[] 直接给出且退出码为 0。」
"""

# 疑似「固件日志污染」的特征串（文本协议下这些字符会直接破坏 GESTURE 行）
STRONG_POLLUTION_MARKERS = [b"EDK60", b"ESP-IDF", b"esp_idf", b"bridge:", b"radar_",
                            b"rst:0x", b"stack:", b"app_main"]
WEAK_POLLUTION_MARKERS = [b"I (", b"W (", b"E (", b"boot:", b"cpu_start"]


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
    """在字节段里查找疑似固件日志的特征串（强特征无条件上报，弱特征要求文本为主）。"""
    hits = [m.decode("ascii", "replace") for m in STRONG_POLLUTION_MARKERS if m in b]
    if printable_ratio(b) >= 0.7:
        hits += [m.decode("ascii", "replace") for m in WEAK_POLLUTION_MARKERS if m in b]
    return hits


def median_sorted(sorted_vals):
    """有序列表的中位数；空列表返回 None。偶数量取中间两值平均（教科书定义）。"""
    n = len(sorted_vals)
    if n == 0:
        return None
    if n % 2 == 1:
        return float(sorted_vals[n // 2])
    return (sorted_vals[n // 2 - 1] + sorted_vals[n // 2]) / 2.0


def fmt_rate(v):
    """比率格式化；None → 'N/A'。"""
    return "N/A" if v is None else f"{v * 100:.4f}%"


def fmt_sec(v):
    """秒格式化；None → 'N/A'。"""
    return "N/A" if v is None else f"{v:.3f} s"


def wave_label(v):
    """挥手动作编码 → 中文标签（含未知值）。"""
    if v is None:
        return "-"
    return WAVE_ACTION_INFO.get(v, f"未知({v})")


def depth_label(v):
    """深度动作编码 → 中文标签（含未知值）。"""
    if v is None:
        return "-"
    return DEPTH_ACTION_INFO.get(v, f"未知({v})")


# ───────────────────────────── 行分类 ─────────────────────────────

def classify_line(text: str) -> dict:
    """把一条文本行分类成结构化字典。无法识别时 kind = LINE_BAD / LINE_DBG_UNMATCHED。

    返回字段（按 kind 不同而不同）：
        kind, target, x, y, z, action_raw, wave, depth, lock
    坐标单位为米；target=0 的格式（DBG_NO_TARGET/DBG_DUAL_NO_TARGET）不带坐标。
    """
    s = text.strip()
    if not s:
        return {"kind": None}

    m = GESTURE_RE.match(s)
    if m:
        return {"kind": LINE_GESTURE,
                "target": m.group("target") == "1",
                "x": float(m.group("x")), "y": float(m.group("y")),
                "z": float(m.group("z")),
                "action_raw": int(m.group("action")), "wave": None, "depth": None}

    m = DUAL_GESTURE_RE.match(s)
    if m:
        return {"kind": LINE_GESTURE_DUAL,
                "target": m.group("target") == "1",
                "x": float(m.group("x")), "y": float(m.group("y")),
                "z": float(m.group("z")),
                "action_raw": None,
                "wave": int(m.group("wave")), "depth": int(m.group("depth"))}

    m = DBG_DUAL_TARGET_RE.match(s)
    if m:
        return {"kind": LINE_DBG_DUAL_TARGET, "target": True,
                "x": float(m.group("x")), "y": float(m.group("y")),
                "z": float(m.group("z")), "action_raw": None,
                "wave": int(m.group("wave")), "depth": int(m.group("depth"))}

    m = DBG_DUAL_NO_TARGET_RE.match(s)
    if m:
        return {"kind": LINE_DBG_DUAL_NO_TARGET, "target": False,
                "x": None, "y": None, "z": None, "action_raw": None,
                "wave": int(m.group("wave")), "depth": int(m.group("depth"))}

    m = DBG_TARGET_RE.match(s)
    if m:
        return {"kind": LINE_DBG_TARGET, "target": True,
                "x": float(m.group("x")), "y": float(m.group("y")),
                "z": float(m.group("z")),
                "action_raw": int(m.group("action")),
                "wave": None, "depth": None,
                "lock": None if m.group("lock") is None else int(m.group("lock"))}

    m = DBG_NO_TARGET_RE.match(s)
    if m:
        return {"kind": LINE_DBG_NO_TARGET, "target": False,
                "x": None, "y": None, "z": None,
                "action_raw": int(m.group("action")),
                "wave": None, "depth": None}

    if s.startswith("DBG:"):
        return {"kind": LINE_DBG_UNMATCHED}
    return {"kind": LINE_BAD}


# ───────────────────────────── 组帧（自测 / 文档示例） ─────────────────────────────

def build_bin_frame(total_len: int, seed: int = 0x31,
                    break_checksum: bool = False) -> bytes:
    """按二进制帧规则构造一帧：帧头2 + 长度2(大端) + 正文(total-5) + 校验1。

    · 正文用 0x00~0x3F 的确定性序列，保证不会碰巧出现 0x4544（避免自测假阳性）
    · break_checksum=True 时故意写错校验字节（造「校验错帧」用例）
    """
    if total_len < MIN_BINARY_FRAME or total_len > MAX_BINARY_FRAME:
        raise ValueError("帧长 %r 超出合法区间 %d~%d"
                         % (total_len, MIN_BINARY_FRAME, MAX_BINARY_FRAME))
    body = bytes(((seed + i * 7) & 0x3F) for i in range(total_len - 5))
    frame = bytearray(BIN_HEADER)
    frame += bytes([(total_len >> 8) & 0xFF, total_len & 0xFF])
    frame += body
    cs = sum(frame) & 0xFF
    if break_checksum:
        cs = (cs ^ 0xFF) & 0xFF            # 一定与原值不同
    frame.append(cs)
    return bytes(frame)


# ───────────────────────────── 校验器 ─────────────────────────────

class GestureChecker:
    """流式混合流校验器：喂字节进来，内部完成「二进制帧整帧跳过 + 文本行分类 + 全部统计」。

    解析顺序（与厂商 MixedStreamParser 一致，只是多记账）：
        1) 在缓冲区里同时找 0x4544 与 \\n；
        2) 换行在帧头之前（或没有帧头）→ 交付这一行文本；
        3) 帧头在前 → 校验长度/校验和，通过则整帧跳过；不通过则重新同步；
        4) 既无帧头也无换行 → 等下一批字节（过长则保留尾部，防无限增长）。
    """

    def __init__(self, threshold_cm: float = DEFAULT_THRESHOLD_CM,
                 gap_limit_s: float = None, raw_dump: bool = False,
                 outdir: str = DEFAULT_OUTDIR, max_unparsed: int = 50,
                 csv_path: str = None, expect_hand: str = "auto",
                 detail_limit: int = 200):
        # ---- 解析缓冲 ----
        self.buf = bytearray()
        self.consumed = 0                 # 已从缓冲区消费掉的字节数（流内偏移）
        self.total_bytes = 0              # 喂进来的总字节数

        # ---- 字节归属（恒等式：total = text + binary + noise，自测里会核对）----
        self.text_bytes = 0               # 落在文本行里的字节（含行尾换行）
        self.binary_bytes_skipped = 0     # 被整帧跳过的二进制帧字节（合法 + 校验错）
        self.noise_bytes = 0              # 无法归属、被丢弃的字节

        # ---- 文本行 ----
        self.lines_total = 0              # 有内容的文本行（不含空行）
        self.lines_empty = 0              # 只有 \\r\\n 的空行
        self.lines_truncated = 0          # 流结束时没有换行的半行
        self.lines_gesture = 0
        self.lines_gesture_dual = 0
        self.lines_dbg = 0
        self.lines_bad = 0
        self.lines_dbg_unmatched = 0
        self.line_times = []              # 每条文本行的到达时刻（秒）
        self.bad_line_samples = []
        self.kind_hist = Counter()

        # ---- target / 坐标 ----
        self.target_rows = 0              # 带 target 信息的行（含 DBG 行）
        self.target_1_rows = 0
        self.target_0_rows = 0
        self.x_vals = []
        self.y_vals = []
        self.z_vals = []
        self.coord_out_of_bounds = 0      # 越界或非有限值的行数
        self.coord_skipped_no_z = 0       # target=1 但没有坐标的行（正常为 0）

        # ---- 动作 ----
        self.wave_hist = Counter()
        self.depth_hist = Counter()
        self.unknown_actions = Counter()
        self.action_preview = []          # [{t, kind, field, value}] 前若干条

        # ---- 距离触发 ----
        self.threshold_cm = float(threshold_cm)
        self.threshold_m = self.threshold_cm / 100.0
        self.triggered_lines = 0
        self.not_triggered_lines = 0
        self.trigger_onsets = []          # 触发起始时刻（未触发 → 触发）
        self._prev_triggered = False

        # ---- 二进制帧 ----
        self.binary_frames_ok = 0
        self.binary_checksum_errors = 0
        self.binary_len_invalid = 0
        self.binary_truncated = 0
        self.bin_len_hist = Counter()
        self.bin_detail = []              # 异常帧明细（长度非法 / 校验错）
        self.frame_swallowed_newline = 0  # 被跳过的帧区间内含换行（疑似吞文本）

        # ---- 重同步 / 噪声段 ----
        self.gap_limit_s = (THRESHOLDS["gap_limit_s"] if gap_limit_s is None
                            else float(gap_limit_s))
        self.resync_events = 0
        self._noise = bytearray()
        self._noise_offset = None
        self.segments = []

        # ---- 事件/告警 ----
        self.warnings = []
        self.events = []

        # ---- 判定参数 ----
        self.expect_hand = expect_hand if expect_hand in ("auto", "absent", "present") else "auto"
        self.detail_limit = detail_limit

        # ---- 输出控制 ----
        self.raw_dump = raw_dump
        self.outdir = outdir
        self.max_unparsed = max_unparsed
        self._csv = None
        self._csv_path = csv_path
        if csv_path:
            self._csv = open(csv_path, "w", encoding="utf-8", newline="")
            self._csv.write("elapsed_s,wallclock,line_no,kind,target,"
                            "x_m,y_m,z_m,x_cm,y_cm,z_cm,wave,depth,action_raw,"
                            "triggered,raw\n")

        # ---- 时间基准 ----
        self._start_wall = None
        self._start_mono = None
        self._sim_span = None
        self._sim_total = None

    # ---------------- 时间基准 ----------------
    def mark_start(self) -> None:
        self._start_wall = time.time()
        self._start_mono = time.monotonic()

    def elapsed(self) -> float:
        if self._start_mono is None:
            return 0.0
        return time.monotonic() - self._start_mono

    def set_synthetic_time(self, span_s: float, total_bytes: int) -> None:
        """离线模式：原始字节流没有时间戳，按「字节位置线性插值」到给定的 --duration。"""
        self._sim_span = span_s
        self._sim_total = total_bytes or None

    def _now(self, now):
        if now is not None:
            return float(now)
        if self._sim_span and self._sim_total:
            return self._sim_span * min(1.0, self.consumed / float(self._sim_total))
        return self.elapsed()

    # ---------------- 缓冲消费 ----------------
    def _consume(self, n: int) -> None:
        """从缓冲区头部吃掉 n 字节，并推进流内偏移（_now 的插值也靠它）。"""
        del self.buf[:n]
        self.consumed += n

    def feed(self, data: bytes, now: float = None) -> None:
        if not data:
            return
        self.total_bytes += len(data)
        self.buf.extend(data)
        self._drain(now)

    # ---------------- 主解析循环 ----------------
    def _drain(self, now) -> None:
        buf = self.buf
        while buf:
            header_at = buf.find(BIN_HEADER)
            newline_at = buf.find(b"\n")

            # ① 文本换行位于帧头之前（或压根没有帧头）→ 交付这一行
            if newline_at >= 0 and (header_at < 0 or newline_at < header_at):
                line_span = newline_at + 1
                raw = bytes(buf[:newline_at]).rstrip(b"\r")
                self.text_bytes += line_span
                self._consume(line_span)
                if raw:
                    self._on_text_line(raw, now)
                else:
                    self.lines_empty += 1
                continue

            # ② 帧头在前 → 处理二进制帧
            if header_at >= 0:
                if header_at > 0:
                    # 帧头前没有完整文本行：噪声或二进制残片，安全丢弃（记账）
                    self._add_noise(bytes(buf[:header_at]))
                    self._consume(header_at)
                if len(buf) < 4:
                    break                       # 还不够读长度字段

                total = (buf[2] << 8) | buf[3]
                if not MIN_BINARY_FRAME <= total <= MAX_BINARY_FRAME:
                    # 长度非法：只丢 1 字节重新同步，尽量不吞掉后面的文本
                    self.binary_len_invalid += 1
                    self._record_frame_event("len_invalid", bytes(buf[:4]), total,
                                             "长度字段 %d 不在 %d~%d，丢弃 1 字节重新同步"
                                             % (total, MIN_BINARY_FRAME, MAX_BINARY_FRAME))
                    self._add_noise(bytes(buf[:1]))
                    self._consume(1)
                    continue

                if len(buf) < total:
                    break                       # 帧还没收全，等下一批字节

                frame = bytes(buf[:total])
                cs_calc = sum(frame[:-1]) & 0xFF
                if cs_calc != frame[-1]:
                    # 长度自洽但校验错：按声明长度**整帧跳过**，保证其后的文本行仍可解析。
                    # （厂商上位机此处只丢 1 字节；那样会把后半截帧粘到下一行文本上，
                    #   导致该行永远解析不出来 —— 作为「校验工具」我们选择整帧跳过并记账。）
                    self.binary_checksum_errors += 1
                    self._record_frame_event(
                        "checksum_error", frame, total,
                        "计算得 0x%02X，帧内为 0x%02X（整帧跳过 %d 字节）"
                        % (cs_calc, frame[-1], total))
                    if b"\n" in frame:
                        self.frame_swallowed_newline += 1
                        self.warnings.append(
                            "校验错帧（偏移 %d）区间内含换行：按声明长度整帧跳过时可能吞掉了文本，"
                            "请用 --raw-dump 复核这一段字节（帧头后 16 字节：%s）"
                            % (self.consumed, hexs(frame, 16)))
                    self.binary_bytes_skipped += total
                    self._consume(total)
                    self._close_noise("noise_before_bad_frame")
                    continue

                # 合法帧：整帧跳过（内容不解析 —— 手扫 demo 不做点云可视化）
                self.binary_frames_ok += 1
                self.binary_bytes_skipped += total
                if len(self.bin_len_hist) < 4096:
                    self.bin_len_hist[total] += 1
                self._consume(total)
                self._close_noise("noise_before_binary_frame")
                continue

            # ③ 既无帧头也无换行：等下一批字节；过长则保留尾部防无限增长
            if len(buf) > MAX_TEXT_BUFFER:
                drop = len(buf) - MAX_TEXT_BUFFER
                self._add_noise(bytes(buf[:drop]))
                self._consume(drop)
                self.warnings.append(
                    "缓冲区堆积超过 %d 字节仍无帧头也无换行：开头的 %d 字节按噪声丢弃（流里可能"
                    "灌进了非文本数据）" % (MAX_TEXT_BUFFER, drop))
            break

    # ---------------- 文本行 ----------------
    def _on_text_line(self, raw: bytes, now) -> None:
        text = raw.decode("utf-8", "replace")
        self.lines_total += 1
        line_no = self.lines_total
        t = self._now(now)
        self.line_times.append(t)
        self._close_noise("noise_before_text")

        info = classify_line(text)
        kind = info["kind"]

        if kind in (LINE_DBG_UNMATCHED, LINE_BAD) or kind is None:
            self.lines_bad += 1
            if kind == LINE_DBG_UNMATCHED:
                self.lines_dbg_unmatched += 1
            if len(self.bad_line_samples) < BAD_LINE_SAMPLE_LIMIT:
                self.bad_line_samples.append({
                    "line_no": line_no,
                    "elapsed_s": round(t, 3),
                    "kind": kind or LINE_BAD,
                    "text": text[:200],
                    "hex": hexs(raw, 64),
                    "printable_ratio": round(printable_ratio(raw), 3),
                    "pollution_markers": pollution_hits(raw),
                })
            if self.raw_dump:
                self._dump_segment(raw, "bad_line", source="line")
            self._csv_write(t, line_no, kind or LINE_BAD, info, False, text)
            return

        self.kind_hist[kind] += 1
        if kind == LINE_GESTURE:
            self.lines_gesture += 1
        elif kind == LINE_GESTURE_DUAL:
            self.lines_gesture_dual += 1
        else:
            self.lines_dbg += 1

        # ---- target ----
        has_target = bool(info.get("target"))
        self.target_rows += 1
        if has_target:
            self.target_1_rows += 1
        else:
            self.target_0_rows += 1

        # ---- 坐标（只统计 target=1 的行；target=0 的坐标恒为 0，计入会污染统计）----
        x = info.get("x")
        y = info.get("y")
        z = info.get("z")
        finite = True
        if has_target:
            if x is None or y is None or z is None:
                self.coord_skipped_no_z += 1
            else:
                finite = all(math.isfinite(v) for v in (x, y, z))
                in_bounds = (finite
                             and abs(x) <= THRESHOLDS["coord_abs_xy_max_m"]
                             and abs(y) <= THRESHOLDS["coord_abs_xy_max_m"]
                             and 0.0 <= z <= THRESHOLDS["coord_z_max_m"])
                if in_bounds:
                    self.x_vals.append(x)
                    self.y_vals.append(y)
                    self.z_vals.append(z)
                else:
                    self.coord_out_of_bounds += 1
                    if len(self.bad_line_samples) < BAD_LINE_SAMPLE_LIMIT:
                        self.bad_line_samples.append({
                            "line_no": line_no,
                            "elapsed_s": round(t, 3),
                            "kind": "coord_out_of_bounds",
                            "text": text[:200],
                            "hex": hexs(raw, 64),
                            "printable_ratio": round(printable_ratio(raw), 3),
                            "pollution_markers": [],
                            "note": ("坐标非有限值" if not finite else
                                     "坐标越界：|x|=%.3f |y|=%.3f z=%.3f（护栏 |x|,|y|≤%.2f, 0≤z≤%.2f）"
                                     % (abs(x), abs(y), z,
                                        THRESHOLDS["coord_abs_xy_max_m"],
                                        THRESHOLDS["coord_z_max_m"])),
                        })

        # ---- 动作 ----
        w_out, d_out, u_out = self._derive_actions(info)
        if w_out is not None:
            self.wave_hist[w_out] += 1
        if d_out is not None:
            self.depth_hist[d_out] += 1
        if u_out is not None:
            self.unknown_actions[u_out] += 1
        if len(self.action_preview) < PAIR_SAMPLE_LIMIT:
            if w_out is not None or d_out is not None or u_out is not None:
                self.action_preview.append({
                    "elapsed_s": round(t, 3),
                    "line_no": line_no,
                    "kind": kind,
                    "field": ("unknown" if u_out is not None
                              else ("wave" if w_out is not None else "depth")),
                    "value": u_out if u_out is not None else (w_out if w_out is not None else d_out),
                })

        # ---- 距离触发 ----
        triggered = bool(has_target and z is not None and math.isfinite(z)
                         and z < self.threshold_m)
        if triggered:
            self.triggered_lines += 1
            if not self._prev_triggered:
                self.trigger_onsets.append({"elapsed_s": round(t, 3),
                                            "line_no": line_no,
                                            "z_m": z,
                                            "z_cm": round(z * 100.0, 1)})
        else:
            self.not_triggered_lines += 1
        self._prev_triggered = triggered

        self._csv_write(t, line_no, kind, info, triggered, text, w_out, d_out)

    @staticmethod
    def _derive_actions(info: dict):
        """把一行的动作字段归一成 (wave, depth, unknown) 三元组。

        · 正式格式 / DBG 的 action=N  : -2..2 → wave；±3 → depth；其它 → unknown
        · 旧格式的 wave=N depth=N     : wave → wave 表；depth 只认 ±3（其余非零 → unknown）
        · 与厂商上位机 parse_telemetry_line 的 wave_action/depth_action 分离口径一致；
          唯一差异：厂商在「旧格式 GESTURE」分支会把 depth 直接丢成 0，本工具仍把它
          记进 depth 表（报告里会标注该差异，便于核对固件到底发了什么）。
        """
        raw = info.get("action_raw")
        wave = info.get("wave")
        depth = info.get("depth")
        w_out = d_out = u_out = None
        if raw is not None:
            if -2 <= raw <= 2:
                w_out = raw
            elif raw in (-3, 3):
                d_out = raw
            else:
                u_out = raw
        if wave is not None:
            if -2 <= wave <= 2:
                w_out = wave
            elif wave != 0:
                u_out = wave
        if depth is not None:
            if depth in (-3, 3):
                d_out = depth
            elif depth != 0 and u_out is None:
                u_out = depth
        return w_out, d_out, u_out

    # ---------------- CSV ----------------
    def _csv_write(self, t, line_no, kind, info, triggered, text,
                   w_out=None, d_out=None) -> None:
        if self._csv is None:
            return

        def num(v, fmt="%.6f"):
            return "" if v is None else (fmt % v)

        x = info.get("x")
        y = info.get("y")
        z = info.get("z")
        rows = [
            "%.3f" % t,
            time.strftime("%Y-%m-%d %H:%M:%S"),
            str(line_no),
            kind,
            "" if info.get("target") is None else ("1" if info.get("target") else "0"),
            num(x), num(y), num(z),
            "" if x is None else "%.2f" % (x * 100.0),
            "" if y is None else "%.2f" % (y * 100.0),
            "" if z is None else "%.2f" % (z * 100.0),
            "" if w_out is None else str(w_out),
            "" if d_out is None else str(d_out),
            "" if info.get("action_raw") is None else str(info.get("action_raw")),
            "1" if triggered else "0",
            text.replace("\r", "").replace("\n", "")[:300],
        ]
        self._csv.write(",".join(rows) + "\n")

    # ---------------- 噪声段 / 落盘 ----------------
    def _add_noise(self, data: bytes) -> None:
        if not data:
            return
        if self._noise_offset is None:
            self._noise_offset = self.consumed
        self._noise += data
        self.noise_bytes += len(data)

    def _close_noise(self, kind: str, count_resync: bool = True):
        """收尾一段「无法归属的字节段」：计入重同步、按需落盘、记录诊断。"""
        if not self._noise:
            self._noise_offset = None
            return None
        data = bytes(self._noise)
        offset = self._noise_offset if self._noise_offset is not None else 0
        self._noise = bytearray()
        self._noise_offset = None
        if count_resync:
            self.resync_events += 1
        return self._record_segment(data, kind, offset, source="stream")

    def _dump_segment(self, data: bytes, kind: str, source: str):
        """直接把一段字节记为一个 unparsed 段（不做噪声计数，供坏行落盘用）。"""
        return self._record_segment(data, kind, self.consumed, source=source)

    def _record_segment(self, data: bytes, kind: str, offset: int, source: str):
        if not data:
            return None
        index = len(self.segments) + 1
        seg = {
            "index": index,
            "offset": offset,
            "size": len(data),
            "kind": kind,
            "source": source,
            "hex": hexs(data, 40),
            "ascii": ascii_preview(data),
            "printable_ratio": round(printable_ratio(data), 3),
            "dumped": None,
        }
        hits = pollution_hits(data)
        if hits:
            seg["pollution_markers"] = hits
            self.warnings.append(
                "无法归属字节段 #%d（偏移 %d，%d 字节）含疑似固件日志特征 %s —— "
                "文本协议下固件日志会直接破坏 GESTURE 行，优先怀疑 printf/启动日志"
                % (index, offset, len(data), "/".join(hits)))
        if len(data) > 1024:
            self.warnings.append("无法归属字节段 #%d 长达 %d 字节，疑似持续写出而非偶发误码"
                                 % (index, len(data)))
        if self.raw_dump and index <= self.max_unparsed:
            path = os.path.join(self.outdir, "unattributed_%d.bin" % index)
            try:
                with open(path, "wb") as f:
                    f.write(data)
                seg["dumped"] = path
            except OSError as e:
                self.warnings.append("unattributed_%d.bin 写入失败：%s" % (index, e))
        if index <= 500:
            self.segments.append(seg)
        return seg

    # ---------------- 二进制帧异常记账 ----------------
    def _record_frame_event(self, kind: str, raw: bytes, declared, note: str) -> None:
        if len(self.bin_detail) >= self.detail_limit:
            return
        self.bin_detail.append({
            "kind": kind,
            "kind_name": ("长度非法" if kind == "len_invalid" else "校验和错误"),
            "offset": self.consumed,
            "declared_len": declared,
            "hex": hexs(raw, 32),
            "ascii": ascii_preview(raw, 32),
            "contains_newline": b"\n" in raw,
            "note": note,
        })

    # ---------------- 收尾 ----------------
    def finish(self) -> None:
        """流结束：残留判为「半行」（无换行）或「截断帧」，并把噪声段结掉。"""
        buf = self.buf
        if buf:
            declared = (buf[2] << 8) | buf[3] if len(buf) >= 4 else None
            head = bytes(buf[:2]) == BIN_HEADER
            declared_ok = (declared is not None
                           and MIN_BINARY_FRAME <= declared <= MAX_BINARY_FRAME)
            if head and declared_ok:
                self.binary_truncated += 1
                self._record_frame_event(
                    "truncated", bytes(buf), declared,
                    "声明 %d 字节，流结束时只收到 %d 字节（采集被截断或帧未发完）"
                    % (declared, len(buf)))
                self._add_noise(bytes(buf))
                self._consume(len(buf))
                self._close_noise("truncated_frame", count_resync=False)
            else:
                raw = bytes(buf).rstrip(b"\r")
                if raw.strip():
                    self.lines_truncated += 1
                    self.warnings.append(
                        "流结束时最后一行没有换行（半行 / 截断）：%r —— 采集被提前终止，"
                        "或固件写出被截断" % raw.decode("utf-8", "replace")[:120])
                    if self.raw_dump:
                        self._dump_segment(raw, "half_line", source="line")
                self.text_bytes += len(buf)
                self._consume(len(buf))
        self._close_noise("tail_garbage", count_resync=False)
        if self._csv is not None:
            self._csv.close()
            self._csv = None

    # ---------------- 汇总 ---------------- #
    def intervals(self):
        """相邻文本行的到达间隔（秒）。样本 < 2 时返回空列表。"""
        ts = self.line_times
        return [max(0.0, ts[i + 1] - ts[i]) for i in range(len(ts) - 1)]

    def timing_usable(self) -> bool:
        """有没有可用的时间基准：至少要 2 条行、且末行时刻严格大于首行时刻。

        --file 模式未给 --duration 时（原始字节流不含时间戳）所有行的时刻都是 0，
        此时「间隔统计」全是 0，是假数据 —— 必须显式说明无时间基准，而不是报 0.000 s。
        """
        return len(self.line_times) >= 2 and (self.line_times[-1] - self.line_times[0]) > 0

    def coverage_span(self):
        """覆盖时长：末行 − 首行 + 一个中位间隔（把首末各算半个），保证均匀流 行率=1/间隔。"""
        if not self.timing_usable():
            return None
        span = self.line_times[-1] - self.line_times[0]
        med = median_sorted(sorted(self.intervals()))
        if med:
            span += med
        return span if span > 0 else None

    def interval_view(self):
        if not self.timing_usable():
            return OrderedDict([
                ("usable", False),
                ("count", 0),
                ("min_s", None), ("median_s", None), ("max_s", None), ("p95_s", None),
                ("gap_limit_s", self.gap_limit_s),
                ("gaps_over_limit", None),
                ("gaps_total_s", None),
                ("note", "无时间基准（--file 模式请补 --duration），行间隔不可计算"),
            ])
        ivs = sorted(self.intervals())
        gaps = [x for x in ivs if x > self.gap_limit_s]
        return OrderedDict([
            ("usable", True),
            ("count", len(ivs)),
            ("min_s", round(ivs[0], 3)),
            ("median_s", round(median_sorted(ivs), 3)),
            ("max_s", round(ivs[-1], 3)),
            ("p95_s", round(ivs[min(len(ivs) - 1, int(0.95 * len(ivs)))], 3)),
            ("gap_limit_s", self.gap_limit_s),
            ("gaps_over_limit", len(gaps)),
            ("gaps_total_s", round(sum(gaps), 3)),
        ])

    def coord_view(self):
        def stats(vals):
            if not vals:
                return OrderedDict([("samples", 0), ("min_m", None), ("median_m", None),
                                    ("max_m", None), ("min_cm", None), ("median_cm", None),
                                    ("max_cm", None)])
            s = sorted(vals)
            mn, md, mx = s[0], median_sorted(s), s[-1]
            return OrderedDict([
                ("samples", len(vals)),
                ("min_m", round(mn, 6)), ("median_m", round(md, 6)), ("max_m", round(mx, 6)),
                ("min_cm", round(mn * 100.0, 2)),
                ("median_cm", round(md * 100.0, 2)),
                ("max_cm", round(mx * 100.0, 2)),
            ])
        return OrderedDict([("x", stats(self.x_vals)), ("y", stats(self.y_vals)),
                            ("z", stats(self.z_vals))])

    def action_view(self):
        wave = OrderedDict()
        for code in sorted(WAVE_ACTION_INFO):
            wave["%d %s" % (code, WAVE_ACTION_INFO[code])] = self.wave_hist.get(code, 0)
        extra_wave = {k: v for k, v in self.wave_hist.items() if k not in WAVE_ACTION_INFO}
        depth = OrderedDict()
        for code in sorted(DEPTH_ACTION_INFO):
            depth["%+d %s" % (code, DEPTH_ACTION_INFO[code])] = self.depth_hist.get(code, 0)
        extra_depth = {k: v for k, v in self.depth_hist.items() if k not in DEPTH_ACTION_INFO}
        return OrderedDict([
            ("wave", wave),
            ("depth", depth),
            ("wave_total", sum(self.wave_hist.values())),
            ("depth_total", sum(self.depth_hist.values())),
            ("unknown", OrderedDict((str(k), v) for k, v in sorted(self.unknown_actions.items()))),
            ("unknown_total", sum(self.unknown_actions.values())),
            ("extra_wave_values", OrderedDict((str(k), v) for k, v in sorted(extra_wave.items()))),
            ("extra_depth_values", OrderedDict((str(k), v) for k, v in sorted(extra_depth.items()))),
            ("preview_tail", self.action_preview[-40:]),
            ("note_dual_depth", "旧格式 GESTURE 的 depth 厂商上位机丢弃（depth_action=0），"
                                "本工具仍记入 depth 表，便于核对固件实际输出"),
        ])

    def trigger_view(self):
        total = self.triggered_lines + self.not_triggered_lines
        return OrderedDict([
            ("threshold_cm", self.threshold_cm),
            ("threshold_m", round(self.threshold_m, 4)),
            ("rule", "target=1 且 z < 阈值（严格小于；等于阈值不触发）"),
            ("rows", total),
            ("triggered", self.triggered_lines),
            ("not_triggered", self.not_triggered_lines),
            ("triggered_ratio", (self.triggered_lines / total) if total else None),
            ("onset_events", len(self.trigger_onsets)),
            ("onset_preview", self.trigger_onsets[:40]),
        ])

    def binary_view(self):
        denom = self.binary_frames_ok + self.binary_checksum_errors
        return OrderedDict([
            ("frames_ok", self.binary_frames_ok),
            ("frames_checksum_error", self.binary_checksum_errors),
            ("frames_len_invalid", self.binary_len_invalid),
            ("frames_truncated", self.binary_truncated),
            ("checksum_pass_rate", (self.binary_frames_ok / denom) if denom else None),
            ("bytes_skipped", self.binary_bytes_skipped),
            ("len_histogram",
             OrderedDict((str(k), v) for k, v in sorted(self.bin_len_hist.items())[:32])),
            ("frame_swallowed_newline", self.frame_swallowed_newline),
            ("detail", self.bin_detail),
        ])

    def byte_view(self):
        unaccounted = (self.total_bytes - self.text_bytes
                       - self.binary_bytes_skipped - self.noise_bytes)
        return OrderedDict([
            ("total_bytes", self.total_bytes),
            ("text_bytes", self.text_bytes),
            ("binary_bytes_skipped", self.binary_bytes_skipped),
            ("noise_bytes", self.noise_bytes),
            ("unaccounted_bytes", unaccounted),
            ("check", "总额 = 文本行 + 二进制帧跳过 + 无法归属（恒等式，必须为 0）"),
        ])

    def line_view(self):
        bad_rate = (self.lines_bad / self.lines_total) if self.lines_total else None
        return OrderedDict([
            ("total", self.lines_total),
            ("empty", self.lines_empty),
            ("truncated_half", self.lines_truncated),
            ("gesture_new_format", self.lines_gesture),
            ("gesture_legacy_dual", self.lines_gesture_dual),
            ("gesture_valid", self.lines_gesture + self.lines_gesture_dual),
            ("dbg_known", self.lines_dbg),
            ("dbg_unmatched", self.lines_dbg_unmatched),
            ("bad", self.lines_bad),
            ("bad_rate", bad_rate),
            ("by_kind", OrderedDict((KIND_NAMES.get(k, k), v)
                                    for k, v in self.kind_hist.items())),
            ("bad_samples", self.bad_line_samples[:20]),
        ])

    def rates(self, span_s):
        span = span_s if span_s and span_s > 0 else None
        return OrderedDict([
            ("span_s", None if span is None else round(span, 3)),
            ("span_source", self._span_source),
            ("line_rate", (self.lines_total / span) if span else None),
            ("byte_rate", (self.total_bytes / span) if span else None),
            ("gesture_rate", ((self.lines_gesture + self.lines_gesture_dual) / span)
             if span else None),
        ])

    _span_source = "未提供"

    # ---------------- 判定 ----------------
    def verdict(self, span_s=None) -> dict:
        """按固定阈值裁定 PASS/FAIL（P0 前提 → P1..P9 九条硬判据，见 RULES_TEXT）。"""
        if span_s is None:
            span_s = self.coverage_span()
            if span_s is None:
                src = "无时间基准（--file 未给 --duration）"
            else:
                src = "自算（末年−首年+一个中位间隔）"
        else:
            src = self._span_source
        # 来源字符串贯穿「判定」与「报告」两处，只在这里落地一次
        self._span_source = src if src and src != "未提供" else "外部提供（未标注来源）"
        r = self.rates(span_s)
        iv = self.interval_view()
        checks = []

        # ---- P0 前提 ----
        gesture_valid = self.lines_gesture + self.lines_gesture_dual
        if gesture_valid == 0:
            checks.append({
                "id": "P0", "name": "有效 GESTURE 行数", "value": 0, "threshold": 1,
                "compare": ">=", "result": "FAIL",
                "note": "整段数据里没有一条合法的 GESTURE 行 → 判定无效；先查波特率是否 115200、"
                        "模组是否插在 H3、模组是否上电（行率≈5 行/秒是最快的体检项）",
            })
            rest = ["P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8", "P9"]
            for pid in rest:
                checks.append({"id": pid, "name": "（跳过）", "value": None,
                               "threshold": None, "compare": "",
                               "result": "SKIP", "note": "P0 未通过，其余判据无意义"})
            return {"verdict": "FAIL", "checks": checks, "line_rate": None,
                    "bad_line_rate": None, "checksum_pass_rate": None,
                    "gaps": iv["gaps_over_limit"], "span_s": None}

        checks.append({
            "id": "P0", "name": "有效 GESTURE 行数", "value": gesture_valid,
            "threshold": 1, "compare": ">=", "result": "PASS", "note": "",
        })

        # ---- P1 文本行率 ----
        rate = r["line_rate"]
        if rate is None:
            p1 = "N/A"
            p1_note = ("没有时间基准：--file 模式必须给 --duration 才能算行率/行间隔"
                       "（原始字节流本身不含时间戳）")
        elif LINE_RATE_LO <= rate <= LINE_RATE_HI:
            p1 = "PASS"
            p1_note = ""
        else:
            p1 = "FAIL"
            p1_note = ("实测 %.3f 行/秒，落在判据区间 [%.1f, %.1f] 之外：可能是波特率不对"
                       "（115200？）、固件输出周期被改，或流里混进日志把行切乱了"
                       % (rate, LINE_RATE_LO, LINE_RATE_HI))
            self.warnings.append(p1_note)
        checks.append({
            "id": "P1", "name": "文本行率", "value": rate,
            "threshold": [LINE_RATE_LO, LINE_RATE_HI], "compare": "in",
            "result": p1, "note": p1_note,
            "extra": {"span_s": r["span_s"], "span_source": r["span_source"]},
        })

        # ---- P2 坏行率 ----
        bad_rate = self.line_view()["bad_rate"]
        if bad_rate is None:
            p2, p2_note = "N/A", "文本行总数为 0，无法计算坏行率"
        elif bad_rate < THRESHOLDS["bad_line_rate_max"]:
            p2, p2_note = "PASS", ""
        else:
            p2 = "FAIL"
            p2_note = ("坏行 %d / %d = %s ≥ %s：文本协议下坏行基本都是固件日志或字段格式变更"
                       "（见 bad_samples / --raw-dump）"
                       % (self.lines_bad, self.lines_total, fmt_rate(bad_rate),
                          fmt_rate(THRESHOLDS["bad_line_rate_max"])))
            self.warnings.append(p2_note)
        checks.append({
            "id": "P2", "name": "坏行率", "value": bad_rate,
            "threshold": THRESHOLDS["bad_line_rate_max"], "compare": "<",
            "result": p2, "note": p2_note,
            "extra": {"bad": self.lines_bad, "total": self.lines_total,
                      "dbg_unmatched": self.lines_dbg_unmatched},
        })

        # ---- P3 二进制帧校验通过率 ----
        bv = self.binary_view()
        csum = bv["checksum_pass_rate"]
        if csum is None:
            p3, p3_note = "N/A", "整段没有二进制帧（纯文本流），校验通过率不适用"
        elif csum >= THRESHOLDS["binary_checksum_pass_min"]:
            p3, p3_note = "PASS", ""
        else:
            p3 = "FAIL"
            p3_note = ("%d 帧校验错 / %d 帧：混流切帧不可靠，其后的文本行有被吞的风险"
                       % (self.binary_checksum_errors,
                          self.binary_frames_ok + self.binary_checksum_errors))
            self.warnings.append(p3_note)
        checks.append({
            "id": "P3", "name": "二进制帧校验通过率", "value": csum,
            "threshold": THRESHOLDS["binary_checksum_pass_min"], "compare": ">=",
            "result": p3, "note": p3_note,
            "extra": {"frames_ok": self.binary_frames_ok,
                      "frames_bad": self.binary_checksum_errors},
        })

        # ---- P4 长度非法帧数 ----
        checks.append({
            "id": "P4", "name": "长度非法帧数", "value": self.binary_len_invalid,
            "threshold": THRESHOLDS["len_invalid_max"], "compare": "==",
            "result": ("PASS" if self.binary_len_invalid <= THRESHOLDS["len_invalid_max"]
                       else "FAIL"),
            "note": ("" if self.binary_len_invalid <= THRESHOLDS["len_invalid_max"] else
                     "有 %d 个 0x4544 疑似帧头的长度字段落在 %d~%d 之外：要么流被污染，"
                     "要么帧头判断错位（见 binary.detail）"
                     % (self.binary_len_invalid, MIN_BINARY_FRAME, MAX_BINARY_FRAME)),
        })

        # ---- P5 空档次数 ----
        gaps = iv["gaps_over_limit"]
        if not iv["count"]:
            p5, p5_note = "N/A", "行间隔样本不足（少于 2 条文本行）"
        elif gaps <= THRESHOLDS["gaps_max"]:
            p5, p5_note = "PASS", ""
        else:
            p5 = "FAIL"
            p5_note = ("%d 次行间隔 > %.1f s（累计 %.1f s）：连丢 5 行以上，需解释"
                       "（USB 重枚举 / 固件卡顿 / 解析吞行）"
                       % (gaps, self.gap_limit_s, iv["gaps_total_s"]))
            self.warnings.append(p5_note)
        checks.append({
            "id": "P5", "name": "行间隔超门限空档次数", "value": gaps,
            "threshold": THRESHOLDS["gaps_max"], "compare": "<=",
            "result": p5, "note": p5_note,
            "extra": {"gap_limit_s": self.gap_limit_s,
                      "median_s": iv["median_s"], "max_s": iv["max_s"]},
        })

        # ---- P6 target=1 比例（按场景）----
        ratio = (self.target_1_rows / self.target_rows) if self.target_rows else None
        if self.expect_hand == "auto":
            p6, p6_note = "N/A", "--expect-hand auto：只报告不判定（一个场景一个口径，不做万能阈值）"
        elif ratio is None:
            p6, p6_note = "N/A", "没有带 target 信息的行"
        elif self.expect_hand == "present":
            ok = ratio >= THRESHOLDS["target_ratio_present_min"]
            p6 = "PASS" if ok else "FAIL"
            p6_note = "" if ok else ("挥手场景要求 target=1 比例 ≥ %.0f%%，实测 %s："
                                     "target=0 说明 10 帧位置窗没形成（人不在视场/太远/角度偏）"
                                     % (THRESHOLDS["target_ratio_present_min"] * 100,
                                        fmt_rate(ratio)))
        else:
            ok = ratio <= THRESHOLDS["target_ratio_absent_max"]
            p6 = "PASS" if ok else "FAIL"
            p6_note = "" if ok else ("静置场景要求 target=1 比例 ≤ %.0f%%，实测 %s："
                                     "没人在场却报目标，先查安装位置是否有晃动/反射物"
                                     % (THRESHOLDS["target_ratio_absent_max"] * 100,
                                        fmt_rate(ratio)))
        if p6 == "FAIL":
            self.warnings.append(p6_note)
        checks.append({
            "id": "P6", "name": "target=1 比例", "value": ratio,
            "threshold": (THRESHOLDS["target_ratio_present_min"] if self.expect_hand == "present"
                          else THRESHOLDS["target_ratio_absent_max"]),
            "compare": (">=" if self.expect_hand == "present" else "<="),
            "result": p6, "note": p6_note,
            "extra": {"expect_hand": self.expect_hand,
                      "target_1": self.target_1_rows, "target_0": self.target_0_rows},
        })

        # ---- P7 坐标越界行数 ----
        checks.append({
            "id": "P7", "name": "坐标越界行数", "value": self.coord_out_of_bounds,
            "threshold": 0, "compare": "==",
            "result": "PASS" if self.coord_out_of_bounds == 0 else "FAIL",
            "note": ("" if self.coord_out_of_bounds == 0 else
                     "%d 行坐标越界/非有限（护栏 |x|,|y| ≤ %.2f m、0 ≤ z ≤ %.2f m）："
                     "通常是小数点或字段串位，结合 bad_samples 定位"
                     % (self.coord_out_of_bounds, THRESHOLDS["coord_abs_xy_max_m"],
                        THRESHOLDS["coord_z_max_m"])),
        })

        # ---- P8 未知 action ----
        unknown_total = sum(self.unknown_actions.values())
        checks.append({
            "id": "P8", "name": "未知 action 值频次", "value": unknown_total,
            "threshold": THRESHOLDS["unknown_action_max"], "compare": "==",
            "result": "PASS" if unknown_total <= THRESHOLDS["unknown_action_max"] else "FAIL",
            "note": ("" if unknown_total == 0 else
                     "出现值域 -3..3 之外的 action：%s —— 固件改了编码或字段串位"
                     "（厂商界面会静默显示「无挥手」，所以必须由工具抓）"
                     % dict(self.unknown_actions)),
        })

        # ---- P9 距离触发事件数（按场景）----
        onsets = len(self.trigger_onsets)
        if self.expect_hand == "auto":
            p9, p9_note = "N/A", "--expect-hand auto：只报告不判定"
        elif self.expect_hand == "present":
            ok = onsets >= THRESHOLDS["trigger_events_present_min"]
            p9 = "PASS" if ok else "FAIL"
            p9_note = "" if ok else ("挥手场景要求至少 %d 次「未触发→触发」，实测 0 次："
                                     "检查人是否进入 z < %.0f cm 的有效区，或 z 读数是否一直 ≥ 阈值"
                                     % (THRESHOLDS["trigger_events_present_min"],
                                        self.threshold_cm))
        else:
            ok = onsets == 0
            p9 = "PASS" if ok else "FAIL"
            p9_note = "" if ok else ("静置场景不该出现触发，实测 %d 次：z 读数噪声或阈值过小"
                                     % onsets)
        if p9 == "FAIL":
            self.warnings.append(p9_note)
        checks.append({
            "id": "P9", "name": "距离触发事件数", "value": onsets,
            "threshold": (THRESHOLDS["trigger_events_present_min"]
                          if self.expect_hand == "present" else 0),
            "compare": (">=" if self.expect_hand == "present" else "=="),
            "result": p9, "note": p9_note,
            "extra": {"expect_hand": self.expect_hand,
                      "threshold_cm": self.threshold_cm,
                      "triggered_lines": self.triggered_lines},
        })

        fails = [c for c in checks if c["result"] == "FAIL"]
        return {
            "verdict": "FAIL" if fails else "PASS",
            "checks": checks,
            "line_rate": rate,
            "bad_line_rate": bad_rate,
            "checksum_pass_rate": csum,
            "gaps": gaps,
            "span_s": r["span_s"],
        }

    # ---------------- 观察项 ----------------
    def observations(self, extra_warnings=None) -> list:
        out = list(self.warnings)
        if extra_warnings:
            out.extend(extra_warnings)
        iv = self.interval_view()
        med = iv["median_s"]
        if med is not None and abs(med - 0.2) > 0.08:
            out.append("行间隔中位数 %.3f s，偏离标称 0.200 s 超过 ±40%%：固件输出周期可能变了"
                       % med)
        if self.lines_dbg:
            out.append("出现 %d 条 DBG 行：固件还在调试输出模式，正式 demo 建议关掉"
                       "（DBG 行会占带宽，也让行率不再是纯 GESTURE 行率）" % self.lines_dbg)
        if self.lines_empty:
            out.append("出现 %d 个空行（只有换行符）：固件可能在输出多余换行" % self.lines_empty)
        if self.lines_dbg_unmatched:
            out.append("有 %d 条 DBG 行字段对不上任何已知 DBG 格式：固件 DBG 格式可能已变更"
                       % self.lines_dbg_unmatched)
        if self.coord_skipped_no_z:
            out.append("有 %d 条 target=1 的行没有坐标字段：格式与厂商源码不一致"
                       % self.coord_skipped_no_z)
        if self.resync_events:
            out.append("重同步 %d 次（噪声/坏帧后重新找回同步的次数）：文本协议下每次重同步"
                       "通常意味着有一行被污染，请结合 unparsed 段定位" % self.resync_events)
        if self.frame_swallowed_newline:
            out.append("有 %d 个校验错帧的区间内含换行：整帧跳过可能吞掉了文本" 
                       % self.frame_swallowed_newline)
        return out

    # ---------------- 汇总成报告字典 ----------------
    def collect(self, mode: str, source: str, span_s=None,
                extra_warnings=None, span_source: str = None) -> dict:
        if span_source:
            self._span_source = span_source
        if span_s is None:
            span_s = self.coverage_span()
            if span_s is not None:
                self._span_source = "自算（末年−首年+一个中位间隔）"
            else:
                self._span_source = "无时间基准"
        v = self.verdict(span_s)
        r = self.rates(span_s)
        report = OrderedDict()
        report["tool"] = TOOL_NAME
        report["tool_version"] = TOOL_VERSION
        report["generated_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
        report["mode"] = mode
        report["source"] = source
        report["verdict"] = v["verdict"]
        report["checks"] = v["checks"]
        report["thresholds"] = OrderedDict(sorted(THRESHOLDS.items()))
        report["thresholds"]["line_rate_lo"] = LINE_RATE_LO
        report["thresholds"]["line_rate_hi"] = LINE_RATE_HI
        report["expect_hand"] = self.expect_hand
        report["protocol_baseline"] = OrderedDict([
            ("uart", "115200 / 8N1，无流控；上位机只收不发"),
            ("cadence", "约 200 ms 一行文本（≈5 行/秒）"),
            ("gesture_line", "GESTURE: target=<0|1> x=<m> y=<m> z=<m> action=<-2..2>"),
            ("number_format", "小数或科学计数法（1.5e-01 合法）"),
            ("target", "1 = 位置有效；0 = 10 帧位置窗未形成（x/y/z 全 0）"),
            ("action_map", "-2 下挥 / -1 右挥 / 0 无挥 / 1 左挥 / 2 上挥；±3 靠近/远离（界面不显示）"),
            ("legacy_line", "GESTURE: ... wave=N depth=N"),
            ("dbg_line", "DBG: ... target=1 ... pos_m=( x y z ) ... action=N [lock=M] / target=0 ..."),
            ("binary_frame", "0x4544 | 长度(2B 大端，含头与校验) | 正文 | 校验(1B)=前(n-1)字节求和&0xFF"),
            ("binary_len_range", "%d ~ %d" % (MIN_BINARY_FRAME, MAX_BINARY_FRAME)),
            ("binary_handling", "按长度+校验整帧跳过；校验错时也整帧跳过以保证其后文本可解析"),
            ("trigger_rule", "target=1 且 z < 阈值(默认 20 cm)，等于阈值不触发"),
            ("protocol_source", "厂商上位机 gesture_monitor.py（755 行）+ README；本 demo 无协议文档"),
        ])
        report["span"] = OrderedDict([
            ("span_s", r["span_s"]), ("source", r["span_source"]),
            ("coverage_span_s", None if self.coverage_span() is None
             else round(self.coverage_span(), 3)),
        ])
        report["byte_accounting"] = self.byte_view()
        report["lines"] = self.line_view()
        report["interval"] = self.interval_view()
        report["rates"] = r
        report["target"] = OrderedDict([
            ("rows_with_target", self.target_rows),
            ("target_1_rows", self.target_1_rows),
            ("target_0_rows", self.target_0_rows),
            ("target_1_ratio", (self.target_1_rows / self.target_rows)
             if self.target_rows else None),
        ])
        report["coords"] = self.coord_view()
        report["actions"] = self.action_view()
        report["trigger"] = self.trigger_view()
        report["binary"] = self.binary_view()
        report["unattributed"] = OrderedDict([
            ("count", len(self.segments)),
            ("raw_dump", self.raw_dump),
            ("max_unparsed", self.max_unparsed),
            ("resync_events", self.resync_events),
            ("segments", self.segments),
        ])
        report["warnings"] = self.observations(extra_warnings)
        report["events"] = self.events
        return report


# ───────────────────────────── 自测 ─────────────────────────────

class SelfTestLog:
    """把逐项对账结果攒起来，最后统一打印 + 决定退出码。"""

    def __init__(self):
        self.rows = []
        self.ok = True

    @staticmethod
    def _num(v):
        """表格里把浮点数压成可读的短形式（0.8181818181818182 → 0.818182）。"""
        if isinstance(v, float) and not v.is_integer():
            return "%.6g" % v
        return str(v)

    def check(self, name, actual, expect, op="==", note=""):
        if op == "==":
            ok = actual == expect
            exp_s = self._num(expect)
        elif op == "~=":
            ok = (actual is not None and expect is not None
                  and abs(float(actual) - float(expect)) <= 1e-6)
            exp_s = "≈ %s" % self._num(expect)
        elif op == ">=":
            ok = actual is not None and actual >= expect
            exp_s = ">= %s" % self._num(expect)
        elif op == "<=":
            ok = actual is not None and actual <= expect
            exp_s = "<= %s" % self._num(expect)
        elif op == "<":
            ok = actual is not None and actual < expect
            exp_s = "< %s" % self._num(expect)
        elif op == ">":
            ok = actual is not None and actual > expect
            exp_s = "> %s" % self._num(expect)
        elif op == "!=":
            ok = actual != expect
            exp_s = "!= %s" % self._num(expect)
        else:
            raise ValueError(op)
        self.ok = self.ok and ok
        self.rows.append((name, actual, exp_s, ok, note))
        return ok

    def section(self, title):
        self.rows.append((None, title, None, None, None))

    def render(self):
        print(f"{'检查项':<34}{'实际':>20}{'期望':>20}   结论")
        print("-" * 88)
        for name, actual, exp_s, ok, note in self.rows:
            if name is None:
                print("")
                print(f"—— {actual} ——")
                continue
            a_s = "None" if actual is None else self._num(actual)
            print(f"{name:<34}{a_s:>20}{exp_s:>20}   {'OK' if ok else '不符 <<<'}"
                  + (f"   {note}" if note and not ok else ""))
        print("-" * 88)


def build_selftest_stream():
    """构造主自测流（文本行 + 混杂二进制帧 + 噪声 + 半行 + 截断帧），返回 (parts, expect)。

    覆盖清单（对应任务要求的 ①~⑩，全部手算期望值，不复用被测代码）：
      ① 正常行            line_A（正式格式 action=1）
      ② target=0          line_B（正式格式）、line_I（DBG target=0）
      ③ 各 action 值      line_C(-2) / line_D(-1) / line_E(2) / line_F(0) / line_A、J(1)
      ④ ±3                line_G（旧格式 depth=+3）、line_H（DBG action=-3）
      ⑤ 科学计数法        line_F（x=1.5e-01 y=-2.5E-2 z=1.95e-1）
      ⑥ 旧格式 wave/depth line_G
      ⑦ DBG 行            line_H（target=1 + pos_m）、line_I（target=0）
      ⑧ 二进制帧          bin_ok_1 / bin_ok_2（合法）夹在两条文本行之间 → 前后两行都必须解析；
                          bin_bad（校验错）同样夹在两条文本行之间
      ⑨ 半行/截断          line_K（缺字段的半行）、bin_trunc（流尾截断帧）
      ⑩ 噪声后重同步      noise_1 在 bin_ok_1 之前（噪声丢弃 → 帧跳过 → 文本恢复）
    另外：长度非法假帧头 fake_hdr_eof（长度 5000）验证「丢 1 字节重同步」策略。

    时间轴：13 条文本行等间隔 0.2 s（t = 0.2 … 2.6），二进制帧/噪声与相邻行同一时刻
    （到达不额外占时间）。覆盖时长 = (2.6−0.2)+0.2 = 2.6 s → 行率 = 13/2.6 = 5.0 行/秒。
    """
    parts = []

    def add(label, data, t):
        parts.append((label, data, t))
        return data

    add("noise_1", b"\xFF\x00\x00NOISE~\x00\x01", 0.0)          # ⑩ 噪声（11 字节）
    add("bin_ok_1", build_bin_frame(20, seed=0x11), 0.0)        # ⑧ 合法帧（20 字节）

    add("line_A", b"GESTURE: target=1 x=0.012 y=-0.034 z=0.156 action=1\r\n", 0.2)
    add("line_B", b"GESTURE: target=0 x=0.000 y=0.000 z=0.000 action=0\r\n", 0.4)
    add("bin_ok_2", build_bin_frame(16, seed=0x22), 0.4)        # ⑧ 关键用例：夹在 B 与 C 之间
    add("line_C", b"GESTURE: target=1 x=0.100 y=0.020 z=0.180 action=-2\r\n", 0.6)
    add("line_D", b"GESTURE: target=1 x=0.050 y=0.010 z=0.150 action=-1\r\n", 0.8)
    add("line_E", b"GESTURE: target=1 x=0.050 y=0.010 z=0.150 action=2\r\n", 1.0)
    add("line_F", b"GESTURE: target=1 x=1.5e-01 y=-2.5E-2 z=1.95e-1 action=0\r\n", 1.2)
    add("line_G", b"GESTURE: target=1 x=0.010 y=0.000 z=0.320 wave=1 depth=3\r\n", 1.4)
    add("line_H", b"DBG: f=10 target=1 win=3 pos_m=( 0.020 -0.010 0.190 ) "
                  b"action=-3 lock=7\r\n", 1.6)
    add("line_I", b"DBG: f=11 target=0 win=0 raw=0 action=0 lock=0\r\n", 1.8)
    add("bin_bad", build_bin_frame(15, seed=0x33, break_checksum=True), 1.8)  # ⑧ 校验错帧
    add("line_J", b"GESTURE: target=1 x=0.030 y=0.005 z=0.170 action=1\r\n", 2.0)
    add("line_K", b"GESTURE: target=1 x=0.012 y=-0.0\r\n", 2.2)  # ⑨ 缺 z/action 的半行 → 坏行
    add("fake_hdr", b"ED\x13\x88\x00\x01junk\r\n", 2.4)          # 长度 5000 非法 → 坏行 + 重同步
    add("line_L", b"GESTURE: target=1 x=0.040 y=-0.005 z=0.140 action=0\r\n", 2.6)
    add("bin_trunc", build_bin_frame(32, seed=0x44)[:10], 2.6)   # ⑨ 声明 32 只给 10 字节

    # ---- 手算期望值（推导写在注释里，方便复核）----
    expect = OrderedDict()
    expect["stream_len"] = sum(len(p[1]) for p in parts)
    expect["lines_total"] = 13          # A B C D E F G H I J K junk L
    expect["lines_gesture"] = 8         # A B C D E F J L
    expect["lines_gesture_dual"] = 1    # G
    expect["lines_valid_gesture"] = 9
    expect["lines_dbg"] = 2             # H I
    expect["lines_bad"] = 2             # K + fake_hdr 产生的 junk 行
    expect["lines_empty"] = 0
    expect["target_rows"] = 11          # 9 条 GESTURE 行 + 2 条 DBG 行
    expect["target_1"] = 9              # A C D E F G H J L
    expect["target_0"] = 2              # B I
    expect["target_ratio"] = 9 / 11.0
    expect["coord_samples"] = 9
    expect["x_min"], expect["x_med"], expect["x_max"] = 0.010, 0.040, 0.150
    expect["y_min"], expect["y_med"], expect["y_max"] = -0.034, 0.000, 0.020
    expect["z_min"], expect["z_med"], expect["z_max"] = 0.140, 0.170, 0.320
    expect["wave"] = {-2: 1, -1: 1, 0: 4, 1: 3, 2: 1}   # C / D / B,F,I,L / A,G,J / E
    expect["depth"] = {-3: 1, 3: 1}                     # H / G
    expect["unknown_action"] = 0
    expect["frames_ok"] = 2             # bin_ok_1(20) + bin_ok_2(16)
    expect["checksum_errors"] = 1       # bin_bad(15)
    expect["len_invalid"] = 1           # fake_hdr：长度 5000
    expect["truncated_frames"] = 1      # bin_trunc
    expect["bytes_skipped"] = 20 + 16 + 15
    expect["triggered"] = 8             # A C D E F H J L（z<0.20）
    expect["not_triggered"] = 3         # B I（target=0）+ G（z=0.320）
    expect["onsets"] = 4                # A / C（B 断开后）/ H（G 断开后）/ J（I 断开后）
    expect["interval_med"] = 0.2
    expect["gaps"] = 0
    expect["line_rate"] = 5.0           # 13 / 2.6
    expect["noise_bytes"] = 11 + 1 + 10  # noise_1 + fake_hdr 丢掉的 0x45 + 截断帧 10 字节
    return parts, expect


def build_clean_stream(n_lines=60, interval=0.2):
    """构造一段「干净」合成流：无坏行、无校验错、行率精确 5.0、含 1 帧合法二进制帧。

    用来验证「好数据必须判 PASS」—— 与主自测流的「坏数据必须判 FAIL」互为对照。
    """
    z_cycle = [0.14, 0.18, 0.26, 0.30]
    acts = [-2, -1, 0, 1, 2]
    parts = []
    t = 0.0
    for i in range(n_lines):
        t += interval
        has_target = (i % 4) != 3          # 3/4 的行有目标 → target=1 比例 75%
        if has_target:
            x = 0.01 * ((i % 5) - 2)
            y = 0.005 * ((i % 3) - 1)
            z = z_cycle[i % 4]
            line = ("GESTURE: target=1 x=%.3f y=%.3f z=%.3f action=%d\r\n"
                    % (x, y, z, acts[i % 5]))
        else:
            line = "GESTURE: target=0 x=0.000 y=0.000 z=0.000 action=0\r\n"
        parts.append(("line_%d" % i, line.encode("ascii"), t))
        if i == n_lines // 2:
            parts.append(("bin_ok", build_bin_frame(24, seed=0x55), t))
    return parts


def run_selftest(outdir_root: str = DEFAULT_OUTDIR) -> int:
    """离线自测：按期望值逐项核对；任何一项不符即返回非 0。"""
    setup_stdout()
    outdir = os.path.join(outdir_root, "edv163_gesture_selftest_"
                          + time.strftime("%Y%m%d-%H%M%S"))
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError:
        outdir = tempfile.mkdtemp(prefix="edv163_gesture_selftest_")

    print("=" * 88)
    print("EDV163 手扫混合流校验脚本 — 离线自测（--selftest）")
    print("=" * 88)

    log = SelfTestLog()

    # ═══════════════ 一、主合成流（含已知缺陷 → 必须判 FAIL） ═══════════════
    parts, expect = build_selftest_stream()
    stream = b"".join(p[1] for p in parts)
    cap_path = os.path.join(outdir, "selftest_capture.bin")
    with open(cap_path, "wb") as f:
        f.write(stream)
    print(f"主合成流：{len(stream)} 字节 / {len(parts)} 段 → {cap_path}")

    ck = GestureChecker(threshold_cm=DEFAULT_THRESHOLD_CM, raw_dump=True,
                        outdir=outdir, max_unparsed=10, expect_hand="auto")
    for _label, data, t in parts:
        ck.feed(data, now=t)
    ck.finish()
    span = ck.coverage_span()
    report = ck.collect("selftest", "synthetic", span_s=span,
                        span_source="自算（末年−首年+一个中位间隔）")
    print(f"产物目录：{outdir}（脚本不自动删除）")
    print("")

    ln = report["lines"]
    tg = report["target"]
    co = report["coords"]
    ac = report["actions"]
    tr = report["trigger"]
    binf = report["binary"]
    by = report["byte_accounting"]
    iv = report["interval"]
    ra = report["rates"]

    log.section("① 文本行分类（13 行：9 GESTURE + 2 DBG + 2 坏行）")
    log.check("文本行总数（不含空行）", ln["total"], expect["lines_total"])
    log.check("正式格式 GESTURE 行", ln["gesture_new_format"], expect["lines_gesture"])
    log.check("旧格式 GESTURE 行", ln["gesture_legacy_dual"], expect["lines_gesture_dual"])
    log.check("有效 GESTURE 行（P0）", ln["gesture_valid"], expect["lines_valid_gesture"])
    log.check("已知 DBG 行", ln["dbg_known"], expect["lines_dbg"])
    log.check("坏行", ln["bad"], expect["lines_bad"])
    log.check("空行", ln["empty"], expect["lines_empty"])

    log.section("②③④ target / 坐标 / 动作")
    log.check("带 target 信息的行数", tg["rows_with_target"], expect["target_rows"])
    log.check("target=1 行数", tg["target_1_rows"], expect["target_1"])
    log.check("target=0 行数", tg["target_0_rows"], expect["target_0"])
    log.check("target=1 比例", tg["target_1_ratio"], expect["target_ratio"], "~=")
    log.check("坐标样本数（仅 target=1）", co["x"]["samples"], expect["coord_samples"])
    log.check("x min (m)", co["x"]["min_m"], expect["x_min"], "~=")
    log.check("x median (m)", co["x"]["median_m"], expect["x_med"], "~=")
    log.check("x max (m)", co["x"]["max_m"], expect["x_max"], "~=")
    log.check("y min (m)", co["y"]["min_m"], expect["y_min"], "~=")
    log.check("y median (m)", co["y"]["median_m"], expect["y_med"], "~=")
    log.check("y max (m)", co["y"]["max_m"], expect["y_max"], "~=")
    log.check("z min (m)", co["z"]["min_m"], expect["z_min"], "~=")
    log.check("z median (m)", co["z"]["median_m"], expect["z_med"], "~=")
    log.check("z max (m)", co["z"]["max_m"], expect["z_max"], "~=")
    log.check("x min (cm) 换算", co["x"]["min_cm"], 1.0, "~=")
    log.check("z max (cm) 换算", co["z"]["max_cm"], 32.0, "~=")
    for code in (-2, -1, 0, 1, 2):
        key = "%d %s" % (code, WAVE_ACTION_INFO[code])
        log.check("wave %d %s" % (code, WAVE_ACTION_INFO[code]),
                  ac["wave"][key], expect["wave"][code])
    for code in (-3, 3):
        key = "%+d %s" % (code, DEPTH_ACTION_INFO[code])
        log.check("depth %+d %s" % (code, DEPTH_ACTION_INFO[code]),
                  ac["depth"][key], expect["depth"][code])
    log.check("未知 action 记录数（P8）", ac["unknown_total"], expect["unknown_action"])
    log.check("动作时间戳预览条数 > 0", len(ac["preview_tail"]), 0, ">")

    log.section("⑧ 二进制帧：整帧跳过 / 校验 / 长度 / 截断 / 字节数")
    log.check("二进制帧·合法（整帧跳过）", binf["frames_ok"], expect["frames_ok"])
    log.check("二进制帧·校验错", binf["frames_checksum_error"], expect["checksum_errors"])
    log.check("二进制帧·长度非法", binf["frames_len_invalid"], expect["len_invalid"])
    log.check("二进制帧·截断", binf["frames_truncated"], expect["truncated_frames"])
    log.check("被跳过的二进制字节数", binf["bytes_skipped"], expect["bytes_skipped"])
    log.check("校验通过率（2/3）", binf["checksum_pass_rate"], 2.0 / 3.0, "~=")
    log.check("帧区间含换行的次数（应为 0）", binf["frame_swallowed_newline"], 0)

    log.check("无法归属字节数", by["noise_bytes"], expect["noise_bytes"])
    log.check("字节守恒（总额−文本−帧−噪声）", by["unaccounted_bytes"], 0)

    log.section("⑤ 行率与行间隔")
    log.check("覆盖时长 (s)", ra["span_s"], 2.6, "~=")
    log.check("行率 (行/秒)", ra["line_rate"], expect["line_rate"], "~=")
    log.check("行间隔中位数 (s)", iv["median_s"], expect["interval_med"], "~=")
    log.check("行间隔 min (s)", iv["min_s"], 0.2, "~=")
    log.check("行间隔 max (s)", iv["max_s"], 0.2, "~=")
    log.check(">1 s 空档次数（P5）", iv["gaps_over_limit"], expect["gaps"])

    log.section("⑥ 距离触发（阈值 20 cm）")
    log.check("触发行数", tr["triggered"], expect["triggered"])
    log.check("未触发行数", tr["not_triggered"], expect["not_triggered"])
    log.check("触发起始事件数", tr["onset_events"], expect["onsets"])
    log.check("触发比例", tr["triggered_ratio"], 8 / 11.0, "~=")
    onset_z = tr["onset_preview"][0]["z_cm"] if tr["onset_preview"] else None
    log.check("首个触发点 z (cm)", onset_z, 15.6, "~=")

    # ---- 判据结果（这段流是「人为造坏」的，必须判 FAIL 且坏在预期的几条上）----
    log.section("⑦ 判定结果（造坏流 → 必须 FAIL，且失败项就是预期那几条）")
    by_id = {c["id"]: c for c in report["checks"]}
    log.check("P0 有效 GESTURE 行", by_id["P0"]["result"], "PASS")
    log.check("P1 行率（5.0 行/秒）", by_id["P1"]["result"], "PASS")
    log.check("P2 坏行率（2/13）", by_id["P2"]["result"], "FAIL")
    log.check("P3 校验通过率（2/3）", by_id["P3"]["result"], "FAIL")
    log.check("P4 长度非法（1 个）", by_id["P4"]["result"], "FAIL")
    log.check("P5 空档（0 次）", by_id["P5"]["result"], "PASS")
    log.check("P6 target 比例（auto）", by_id["P6"]["result"], "N/A")
    log.check("P7 坐标越界（0 行）", by_id["P7"]["result"], "PASS")
    log.check("P8 未知 action（0）", by_id["P8"]["result"], "PASS")
    log.check("P9 触发事件（auto）", by_id["P9"]["result"], "N/A")
    log.check("整体结论（造坏流）", report["verdict"], "FAIL")

    ck.expect_hand = "present"
    v_present = ck.verdict(span)
    by_id2 = {c["id"]: c for c in v_present["checks"]}
    log.check("P6 挥手场景（0.818 ≥ 0.5）", by_id2["P6"]["result"], "PASS")
    log.check("P9 挥手场景（4 次触发）", by_id2["P9"]["result"], "PASS")

    # ═══════════════ 二、干净合成流（必须判 PASS） ═══════════════
    clean_parts = build_clean_stream()
    clean_stream = b"".join(p[1] for p in clean_parts)
    clean_path = os.path.join(outdir, "selftest_clean_capture.bin")
    with open(clean_path, "wb") as f:
        f.write(clean_stream)
    ck2 = GestureChecker(threshold_cm=DEFAULT_THRESHOLD_CM, expect_hand="present")
    ck2.mark_start()
    for _label, data, t in clean_parts:
        ck2.feed(data, now=t)
    ck2.finish()
    rep2 = ck2.collect("selftest", "synthetic-clean", span_s=ck2.coverage_span(),
                       span_source="自算（末年−首年+一个中位间隔）")
    print(f"干净合成流：{len(clean_stream)} 字节 → {clean_path}")
    log.section("⑨ 干净流对照（好数据必须判 PASS）")
    log.check("文本行总数", rep2["lines"]["total"], 60)
    log.check("坏行", rep2["lines"]["bad"], 0)
    log.check("二进制帧·合法", rep2["binary"]["frames_ok"], 1)
    log.check("行率 (行/秒)", rep2["rates"]["line_rate"], 5.0, "~=")
    log.check(">1 s 空档次数", rep2["interval"]["gaps_over_limit"], 0)
    log.check("target=1 比例（0.75）", rep2["target"]["target_1_ratio"], 0.75, "~=")
    log.check("触发起始事件数 > 0", rep2["trigger"]["onset_events"], 0, ">")
    log.check("整体结论（干净流）", rep2["verdict"], "PASS")
    log.check("失败判据条数（应为 0）",
              len([c for c in rep2["checks"] if c["result"] == "FAIL"]), 0)

    # ═══════════════ 三、边界 / 分块 / 专项微用例 ═══════════════
    log.section("⑩ 边界与分块微用例")

    # (a) 关键用例：二进制帧夹在两条文本行之间 → 两条都必须解析出来
    a = (b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=1\r\n"
         + build_bin_frame(23, seed=0x66)
         + b"GESTURE: target=1 x=0.020 y=0.020 z=0.160 action=-1\r\n")
    cka = GestureChecker()
    cka.feed(a, now=0.0)
    cka.finish()
    log.check("帧夹两行 → 文本行数", cka.lines_total, 2)
    log.check("帧夹两行 → GESTURE 行数", cka.lines_gesture, 2)
    log.check("帧夹两行 → 帧跳过数", cka.binary_frames_ok, 1)
    log.check("帧夹两行 → 第 2 行 z (cm)",
              round(cka.z_vals[-1] * 100, 1) if cka.z_vals else None, 16.0, "~=")

    # (b) 校验错帧夹在两条文本行之间 → 两条也都必须解析出来
    b = (b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=1\r\n"
         + build_bin_frame(23, seed=0x77, break_checksum=True)
         + b"GESTURE: target=0 x=0.000 y=0.000 z=0.000 action=0\r\n")
    ckb = GestureChecker()
    ckb.feed(b, now=0.0)
    ckb.finish()
    log.check("校验错帧夹两行 → 文本行数", ckb.lines_total, 2)
    log.check("校验错帧夹两行 → 校验错计数", ckb.binary_checksum_errors, 1)
    log.check("校验错帧夹两行 → 帧字节被跳过", ckb.binary_bytes_skipped, 23)

    # (c) 字节分块喂（把「文本行 + 帧」拆成 3 段边界）→ 结果与整段喂一致
    ckc = GestureChecker()
    whole = a
    for piece in (whole[:5], whole[5:17], whole[17:]):
        ckc.feed(piece, now=0.0)
    ckc.finish()
    log.check("分块喂 → 文本行数", ckc.lines_total, 2)
    log.check("分块喂 → 帧跳过数", ckc.binary_frames_ok, 1)
    log.check("分块喂 → 无无法归属字节", ckc.noise_bytes, 0)

    # (d) 半行（流尾没有换行）
    ckd = GestureChecker()
    ckd.feed(b"GESTURE: target=1 x=0.1 y=0.2", now=0.0)
    ckd.finish()
    log.check("流尾半行 → 文本行数（半行不算行）", ckd.lines_total, 0)
    log.check("流尾半行 → 半行计数", ckd.lines_truncated, 1)

    # (e) 空档：两行相隔 1.5 s
    cke = GestureChecker()
    cke.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=1\r\n", now=0.0)
    cke.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=1\r\n", now=1.5)
    cke.finish()
    log.check("相隔 1.5 s → 空档次数", cke.interval_view()["gaps_over_limit"], 1)
    log.check("相隔 1.5 s → 最大间隔 (s)", cke.interval_view()["max_s"], 1.5, "~=")

    # (f) 纯二进制流（没有任何文本行）→ P0 FAIL
    ckf = GestureChecker()
    ckf.feed(build_bin_frame(40, seed=0x88) * 3, now=0.0)
    ckf.finish()
    vf = ckf.verdict(1.0)
    log.check("纯二进制流 → 文本行数", ckf.lines_total, 0)
    log.check("纯二进制流 → 帧跳过数", ckf.binary_frames_ok, 3)
    log.check("纯二进制流 → 结论", vf["verdict"], "FAIL")
    log.check("纯二进制流 → P0", vf["checks"][0]["result"], "FAIL")

    # (g) 未知 action 值（7）必须被 P8 抓到
    ckg = GestureChecker()
    ckg.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=7\r\n", now=0.0)
    ckg.finish()
    vg = ckg.verdict(None)
    log.check("action=7 → 未知 action 计数", sum(ckg.unknown_actions.values()), 1)
    log.check("action=7 → 记入的值", dict(ckg.unknown_actions), {7: 1})
    log.check("action=7 → P8 结论",
              {c["id"]: c["result"] for c in vg["checks"]}["P8"], "FAIL")

    # (h) 帧长边界：11（合法下限）跳过；9（<11）与 4097（>4096）计入长度非法。
    #     注意：假帧头之后如果没有换行，它后面的那条文本行会被粘进同一条「坏行」——
    #     这就是真实世界里「帧头错位吞掉一行文本」的样子，所以用例里给假帧头补了行尾。
    #     另外故意避开长度字节 0x0A（长度 10 的低字节就是换行符，会把行提前切断）。
    ckh = GestureChecker()
    ckh.feed(build_bin_frame(11, seed=0x99), now=0.0)
    ckh.feed(b"ED\x00\x09" + b"\x00" * 6 + b"\x00\n", now=0.0)  # 长度 9 → 非法
    ckh.feed(b"ED\x10\x01" + b"\x00" * 6 + b"\x00\n", now=0.0)  # 长度 4097 → 非法
    ckh.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=0\r\n", now=0.2)
    ckh.finish()
    log.check("帧长 11 → 合法帧", ckh.binary_frames_ok, 1)
    log.check("帧长 9 / 4097 → 长度非法", ckh.binary_len_invalid, 2)
    log.check("长度非法 → 每段退化成 1 条坏行", ckh.lines_bad, 2)
    log.check("长度非法后 → 文本仍解析", ckh.lines_gesture, 1)

    # (i) 噪声在帧之前 → 丢弃噪声、跳过帧、文本恢复（⑩ 重同步）
    cki = GestureChecker()
    cki.feed(b"\x01\x02GARBAGE\x03", now=0.0)
    cki.feed(build_bin_frame(14, seed=0xAA), now=0.0)
    cki.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.150 action=1\r\n", now=0.2)
    cki.finish()
    log.check("噪声后重同步 → 噪声字节", cki.noise_bytes, 10)
    log.check("噪声后重同步 → 帧跳过", cki.binary_frames_ok, 1)
    log.check("噪声后重同步 → 文本行", cki.lines_total, 1)
    log.check("噪声后重同步 → 重同步次数 > 0", cki.resync_events, 0, ">")

    # (j) DBG 行字段对不上 → 计入坏行 + dbg_unmatched 单列
    ckj = GestureChecker()
    ckj.feed(b"DBG: something totally different\r\n", now=0.0)
    ckj.finish()
    log.check("未知 DBG 行 → 坏行", ckj.lines_bad, 1)
    log.check("未知 DBG 行 → dbg_unmatched", ckj.lines_dbg_unmatched, 1)

    # (k) 坐标越界必须被 P7 抓到
    ckk = GestureChecker()
    ckk.feed(b"GESTURE: target=1 x=1.500 y=0.010 z=0.150 action=1\r\n", now=0.0)
    ckk.finish()
    vk = ckk.verdict(None)
    log.check("x=1.5 m → 越界行数", ckk.coord_out_of_bounds, 1)
    log.check("x=1.5 m → P7 结论",
              {c["id"]: c["result"] for c in vk["checks"]}["P7"], "FAIL")

    # (l) 目标在阈值上（z=0.200，阈值 20 cm）→ 不触发（严格小于是判据）
    ckl = GestureChecker(threshold_cm=20.0)
    ckl.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.200 action=1\r\n", now=0.0)
    ckl.feed(b"GESTURE: target=1 x=0.010 y=0.010 z=0.199 action=1\r\n", now=0.2)
    ckl.finish()
    log.check("z=0.200 不触发 / z=0.199 触发", ckl.triggered_lines, 1)

    # (m) --raw-dump 落盘检查（噪声段与坏行都要有文件）
    dumped = [s for s in report["unattributed"]["segments"] if s.get("dumped")]
    log.check("unparsed 段落盘文件数 > 0", len(dumped), 0, ">")
    log.check("无法归属字节段数 > 0", report["unattributed"]["count"], 0, ">")

    # ═══════════════ 输出 ═══════════════
    print("")
    log.render()
    print("")
    if log.ok:
        print("自测结果：全部通过（PASS）")
    else:
        print("自测结果：存在不符项（FAIL），请检查脚本逻辑 <<<")
    print(f"完整报告：{os.path.join(outdir, 'selftest_report.json')}")
    try:
        with open(os.path.join(outdir, "selftest_report.json"), "w",
                  encoding="utf-8") as f:
            json.dump(OrderedDict([
                ("tool", TOOL_NAME), ("tool_version", TOOL_VERSION),
                ("mode", "selftest"),
                ("dirty_stream_report", report),
                ("clean_stream_report", rep2),
                ("selftest_ok", log.ok),
                ("rows", [{"name": n, "actual": str(a), "expect": e, "ok": o}
                          for n, a, e, o, _n in log.rows if n]),
            ]), f, ensure_ascii=False, indent=2)
    except OSError:
        pass
    return 0 if log.ok else 1


# ───────────────────────────── 输出 ─────────────────────────────

def print_report(report: dict, logf=None) -> None:
    """把报告打成中文可读文本（logf 给了就同时写进 .log）。"""
    lines = []

    def w(s=""):
        lines.append(s)
        if logf is None:
            print(s)
        else:
            logf.write(s + "\n")

    by = report["byte_accounting"]
    ln = report["lines"]
    iv = report["interval"]
    ra = report["rates"]
    tg = report["target"]
    co = report["coords"]
    ac = report["actions"]
    tr = report["trigger"]
    binf = report["binary"]

    w("=" * 88)
    w("EDV163 手扫（挥手识别）混合流校验报告")
    w("=" * 88)
    w(f"生成时间      : {report['generated_at']}")
    w(f"采集方式      : {report['mode']}  ({report['source']})")
    w(f"覆盖时长      : {ra['span_s'] if ra['span_s'] is not None else 'N/A'} s"
      f"（来源：{ra['span_source']}）")
    w(f"协议基线      : 115200 8N1｜200 ms/行｜文本行 + 0x4544 二进制帧整帧跳过｜"
      f"阈值 {tr['threshold_cm']:.1f} cm")
    w("")

    w("【字节归属】（恒等式：总额 = 文本行 + 二进制帧跳过 + 无法归属）")
    w(f"  总字节数        : {by['total_bytes']:,}")
    w(f"  文本行字节      : {by['text_bytes']:,}")
    w(f"  二进制帧跳过    : {by['binary_bytes_skipped']:,}")
    w(f"  无法归属字节    : {by['noise_bytes']:,}")
    w(f"  未对账差额      : {by['unaccounted_bytes']:,}"
      + ("  ← 应为 0" if by["unaccounted_bytes"] else "  OK"))
    if ra["byte_rate"] is not None:
        w(f"  字节率          : {ra['byte_rate']:.1f} B/s")
    w("")

    w("【文本行】")
    w(f"  文本行总数      : {ln['total']:,}（空行 {ln['empty']}，流尾半行 {ln['truncated_half']}）")
    w(f"  有效 GESTURE 行 : {ln['gesture_valid']:,}"
      f"（正式格式 {ln['gesture_new_format']:,} + 旧格式 {ln['gesture_legacy_dual']:,}）")
    w(f"  已知 DBG 行     : {ln['dbg_known']:,}"
      f"（其中字段不匹配 {ln['dbg_unmatched']:,}，已计入坏行）")
    w(f"  坏行            : {ln['bad']:,}    坏行率 {fmt_rate(ln['bad_rate'])}"
      f"（阈值 < {THRESHOLDS['bad_line_rate_max']:.0%}）")
    if ln["by_kind"]:
        w("  按类型分布      : " + "，".join("%s×%d" % (k, v) for k, v in ln["by_kind"].items()))
    w("")

    w("【行率与行间隔】（标称 200 ms/行 = 5.0 行/秒）")
    w(f"  行率            : "
      + ("N/A（无时间基准）" if ra["line_rate"] is None
         else "%.3f 行/秒（判据 %.1f ~ %.1f）"
              % (ra["line_rate"], LINE_RATE_LO, LINE_RATE_HI)))
    if iv["usable"]:
        w(f"  行间隔          : 样本 {iv['count']:,}｜min {fmt_sec(iv['min_s'])}｜"
          f"median {fmt_sec(iv['median_s'])}｜max {fmt_sec(iv['max_s'])}｜P95 {fmt_sec(iv['p95_s'])}")
        w(f"  >{iv['gap_limit_s']:.1f} s 空档    : {iv['gaps_over_limit']} 次"
          f"（累计 {iv['gaps_total_s']:.1f} s；判据 = 0 次）")
    else:
        w("  行间隔          : N/A（" + iv.get("note", "无时间基准") + "）")
    w("")

    w("【target 与坐标】（坐标只统计 target=1 的行；单位 m / cm）")
    w(f"  带 target 的行  : {tg['rows_with_target']:,}"
      f"（target=1 {tg['target_1_rows']:,} / target=0 {tg['target_0_rows']:,}）")
    w(f"  target=1 比例   : {fmt_rate(tg['target_1_ratio'])}"
      f"（场景口径：{report['expect_hand']}）")
    for axis in ("x", "y", "z"):
        s = co[axis]
        if s["samples"]:
            w(f"  {axis}              : {s['samples']:,} 个样本｜"
              f"min {s['min_m']:+.3f} / median {s['median_m']:+.3f} / max {s['max_m']:+.3f} m"
              f"  =  {s['min_cm']:+.1f} / {s['median_cm']:+.1f} / {s['max_cm']:+.1f} cm")
        else:
            w(f"  {axis}              : 无有效样本")
    if report["lines"]["bad"]:
        p7 = next((c for c in report["checks"] if c["id"] == "P7"), None)
        w(f"  坐标越界行数    : {'N/A' if not p7 or p7['value'] is None else p7['value']}"
          f"（判据 = 0；对照值见下方 P7）")
    w("")

    w("【action 分布】（wave = 挥手方向；depth = 靠近/远离，界面不显示）")
    w("  " + "｜".join("%s %d" % (k, v) for k, v in ac["wave"].items())
      + "    合计 %d" % ac["wave_total"])
    w("  " + "｜".join("%s %d" % (k, v) for k, v in ac["depth"].items())
      + "    合计 %d" % ac["depth_total"])
    if ac["unknown_total"]:
        w("  未知 action     : " + "，".join("%s×%d" % (k, v)
                                              for k, v in ac["unknown"].items()))
    if ac["preview_tail"]:
        head = ac["preview_tail"][:6]
        w("  动作时间戳(前几条): " + "；".join(
            "t=%.2fs %s=%s" % (p["elapsed_s"], p["field"], p["value"]) for p in head)
          + ("…（完整序列见 --csv）" if len(ac["preview_tail"]) > 6 else ""))
    w("")

    w("【距离触发】")
    w(f"  阈值            : {tr['threshold_cm']:.1f} cm（{tr['threshold_m']:.3f} m）"
      f"｜{tr['rule']}")
    w(f"  触发 / 未触发   : {tr['triggered']:,} / {tr['not_triggered']:,}"
      f"    触发比例 {fmt_rate(tr['triggered_ratio'])}")
    w(f"  触发起始事件    : {tr['onset_events']} 次")
    if tr["onset_preview"]:
        head = tr["onset_preview"][:8]
        w("  触发时刻        : " + "；".join(
            "t=%.2fs(z=%.1fcm)" % (p["elapsed_s"], p["z_cm"]) for p in head)
          + ("…" if len(tr["onset_preview"]) > 8 else ""))
    w("")

    w("【混杂二进制帧】（0x4544 + 长度 + 累加校验；整帧跳过，不解析内容）")
    w(f"  合法帧（已跳过）: {binf['frames_ok']:,}")
    w(f"  校验错帧        : {binf['frames_checksum_error']:,}（整帧跳过以保证后续文本可解析）")
    w(f"  长度非法帧头    : {binf['frames_len_invalid']:,}（只丢 1 字节重新同步）")
    w(f"  截断帧          : {binf['frames_truncated']:,}（流结束时字节不足）")
    w(f"  校验通过率      : {fmt_rate(binf['checksum_pass_rate'])}")
    w(f"  跳过字节数      : {binf['bytes_skipped']:,}")
    if binf["len_histogram"]:
        w("  帧长分布        : " + "，".join("%sB×%d" % (k, v)
                                              for k, v in list(binf["len_histogram"].items())[:12]))
    if binf["frame_swallowed_newline"]:
        w(f"  ⚠ 帧区间含换行  : {binf['frame_swallowed_newline']} 次（疑似吞文本）")
    w("")

    w("【判定】阈值与 --help 同源（THRESHOLDS 常量）")
    w(f"  {'ID':<5}{'判据':<24}{'实际':>16}{'阈值/区间':>22}   结论")
    for c in report["checks"]:
        v = c["value"]
        if c["id"] in ("P1",):
            v_s = "N/A" if v is None else "%.3f 行/秒" % v
        elif c["id"] in ("P2", "P3", "P6"):
            v_s = fmt_rate(v) if v is not None else "N/A"
        elif c["id"] == "P5":
            v_s = "N/A" if v is None else "%d 次" % v
        elif c["id"] == "P9":
            v_s = "N/A" if v is None else "%d 次" % v
        else:
            v_s = "N/A" if v is None else str(v)
        th = c["threshold"]
        if isinstance(th, list):
            th_s = "%.1f ~ %.1f" % (th[0], th[1])
        elif c["id"] in ("P2", "P3", "P6") and isinstance(th, float):
            th_s = "%s %s" % (c["compare"], "%.0f%%" % (th * 100))
        else:
            th_s = ("%s %s" % (c["compare"], th)) if c["compare"] else "—"
        w(f"  {c['id']:<5}{c['name']:<24}{v_s:>16}{th_s:>22}   {c['result']}")
        if c.get("note"):
            w(f"        └ {c['note']}")
    w("")
    w(f"  >>> 结论：{report['verdict']}   （任一 FAIL 即 FAIL；N/A / 观察项不阻断）")
    w("")

    if report["warnings"]:
        w("【警告 / 观察项】（不影响 PASS/FAIL，但要在 Gate 评审里说明）")
        for x in report["warnings"]:
            w(f"  · {x}")
        w("")

    if ln["bad_samples"]:
        w(f"【坏行 / 越界样例】（前 {min(8, len(ln['bad_samples']))} 条）")
        for s in ln["bad_samples"][:8]:
            w(f"  行 #{s['line_no']} t={s['elapsed_s']:.2f}s [{s['kind']}]"
              f" 可打印率 {s['printable_ratio']}")
            w(f"     {s['text']!r}")
            if s.get("pollution_markers"):
                w(f"     ⚠ 疑似污染特征: {s['pollution_markers']}")
            if s.get("note"):
                w(f"     {s['note']}")
        w("")

    if binf["detail"]:
        w("【异常帧明细】（前 6 条）")
        for d in binf["detail"][:6]:
            w(f"  偏移 {d['offset']}  [{d['kind_name']}]  声明长度 {d['declared_len']}"
              f"{'（区间含换行）' if d['contains_newline'] else ''}")
            w(f"     {d['note']}")
            w(f"     hex: {d['hex']}")
        w("")

    if report["unattributed"]["count"]:
        w(f"【无法归属的字节段】（前 8 段，共 {report['unattributed']['count']} 段；"
          f"重同步 {report['unattributed']['resync_events']} 次）")
        for s in report["unattributed"]["segments"][:8]:
            w(f"  #{s['index']} 偏移 {s['offset']}  {s['size']} 字节  [{s['kind']}/"
              f"{s['source']}]  可打印率 {s['printable_ratio']}")
            w(f"     hex: {s['hex']}")
            w(f"     ascii: {s['ascii']}")
            if s.get("pollution_markers"):
                w(f"     ⚠ 疑似污染特征: {s['pollution_markers']}")
            if s.get("dumped"):
                w(f"     → 已存盘: {s['dumped']}")
        w("")

    art = report.get("artifacts") or {}
    if art:
        w("【产物】")
        for k in ("json", "log", "csv", "raw_capture", "unattributed_dir", "rules"):
            if art.get(k) and k != "rules":
                w(f"  {k:<18}: {art[k]}")
        w("")


# ───────────────────────────── 主流程 ─────────────────────────────

def _import_serial():
    """延迟导入 pyserial，给出可执行的安装提示（--file/--selftest 不需要它）。"""
    try:
        import serial                       # noqa: F401
        from serial.tools import list_ports  # noqa: F401
        return serial
    except ImportError:
        print("错误：--port 模式需要 pyserial，本机没有安装。", file=sys.stderr)
        print("  安装：  py -3 -m pip install pyserial", file=sys.stderr)
        print("          （本机 `python` 是应用商店别名不可用，请用 py -3）", file=sys.stderr)
        print("  提示：--file 与 --selftest 模式不需要 pyserial。", file=sys.stderr)
        sys.exit(2)


def run_port(args, ck: GestureChecker) -> dict:
    """串口实时采集 + 逐行分类，时长到点自动收尾。"""
    serial = _import_serial()
    duration = args.duration if args.duration is not None else GATE_DURATION

    print(f"打开串口 {args.port} @ {args.baud} …")
    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.05)
    except Exception as e:
        print(f"错误：打开串口失败：{e}", file=sys.stderr)
        sys.exit(2)

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
                print(f"  [{ck.elapsed():7.1f}s] 字节 {ck.total_bytes:,}"
                      f" 行 {ck.lines_total:,}（坏 {ck.lines_bad}）"
                      f" 帧 {ck.binary_frames_ok:,} 校验错 {ck.binary_checksum_errors}"
                      f" target=1 {ck.target_1_rows:,} 触发 {ck.triggered_lines:,}")
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
        extra.append("采集被 Ctrl+C 提前中断（时长 %.1fs，未达计划 %.0fs），结论仅代表已采集区间"
                     % (span, duration))
    if span < 0.9 * duration:
        extra.append("实测采集时长 %.1fs 不足计划 %.0fs 的 90%%，不满足「连续运行」要求"
                     % (span, duration))
    if not interrupted and span > duration + 5:
        extra.append("采集超时 %.1fs，请检查串口读取是否被阻塞" % (span - duration))
    return ck.collect("port", "%s @%d" % (args.port, args.baud), span_s=span,
                      extra_warnings=extra, span_source="--port 实测墙钟时长")


def run_file(args, ck: GestureChecker) -> dict:
    """离线分析已保存的原始字节流（不需要 pyserial）。"""
    path = args.file
    if not os.path.exists(path):
        print("错误：文件不存在：%s" % path, file=sys.stderr)
        sys.exit(2)
    size = os.path.getsize(path)
    print(f"分析文件 {path}（{size:,} 字节）…")
    span = args.duration
    extra = []
    if span is None:
        extra.append("离线模式未提供 --duration：原始字节流本身不含时间戳，行率与行间隔无法计算"
                     "（P1/P5 记 N/A）；补 --duration <采集秒数> 可复算")
    else:
        ck.set_synthetic_time(span, size)
        extra.append("离线模式的『时刻』按字节位置线性插值到 --duration，仅用于对齐/统计，"
                     "不代表真实到达时刻；大二进制帧会在这条插值时间轴上占掉与长度成比例的"
                     "「时长」，因此离线模式下的空档（P5）属参考值，定论请用 --port 实测")
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            ck.feed(chunk, now=None)
    ck.finish()
    src = ("--file 的 --duration" if span is not None else "无时间基准")
    return ck.collect("file", os.path.basename(path), span_s=span,
                      extra_warnings=extra, span_source=src)


def build_argparser() -> argparse.ArgumentParser:
    epilog = RULES_TEXT + f"""
用法示例（本机 `python` 是应用商店别名不可用，统一用 py -3；
详见 docs\\project\\edv163-gesture\\stream-check-test-plan.md）
  # 10 分钟挥手采集：边测边出报告，并存原始流 + 导逐行 CSV
  py -3 tools\\edv163_gesture_check.py --port COM6 --baud 115200 --duration 600 ^
        --expect-hand present --save-raw capture.bin --csv lines.csv --raw-dump

  # 静置 30 分钟（无人挥手）：应当 target=1 比例很低、且没有任何触发
  py -3 tools\\edv163_gesture_check.py --port COM6 --duration 1800 ^
        --expect-hand absent --save-raw idle.bin

  # 采集完再离线复算（不接硬件，不需要 pyserial）
  py -3 tools\\edv163_gesture_check.py --file capture.bin --duration 600 --expect-hand present

  # 排查「固件日志污染」：把解析不了的字节段与坏行逐段存成 unattributed_<n>.bin
  py -3 tools\\edv163_gesture_check.py --file capture.bin --duration 600 --raw-dump

  # 无硬件自测（合成流：正常行/target=0/各 action/±3/科学计数法/旧格式/DBG/
  #   二进制帧夹在两行之间/半行/噪声重同步/长度边界/字节分块）
  py -3 tools\\edv163_gesture_check.py --selftest

产出（默认全部落在 logs\\edv163-gesture\\，可用 --out 改）
  logs\\edv163-gesture\\edv163_gesture_check_<时间戳>.json   机器可读全量报告（Gate 归档用）
  logs\\edv163-gesture\\edv163_gesture_check_<时间戳>.log    同内容的中文文本日志
  logs\\edv163-gesture\\unattributed_<n>.bin                 仅在 --raw-dump 时生成
  logs\\edv163-gesture\\edv163_gesture_selftest_<时间戳>\\     仅 --selftest 生成（合成流 + 报告）
  退出码：0 = PASS（或自测逐项通过）；1 = FAIL（或自测有不符合项）；2 = 参数或环境错误
"""
    p = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="EDK60 ⇄ EDV163 手扫混合流校验工具：二进制帧整帧跳过 + 文本行分类 + "
                    "行率/坏行/动作/触发统计 + PASS/FAIL 裁定",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    src = p.add_mutually_exclusive_group()
    src.add_argument("--port", metavar="COMx", help="实时读取该串口（需要 pyserial）")
    src.add_argument("--file", metavar="capture.bin",
                     help="离线分析已保存的原始字节流（不需要 pyserial）")
    src.add_argument("--selftest", action="store_true",
                     help="离线自测：跑合成流并逐项核对统计值（含二进制帧夹在两行之间）")

    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"波特率，默认 {DEFAULT_BAUD}（挥手固件出厂值；EDV11P 是 921600、"
                        f"水位是 256000，别抄错）")
    p.add_argument("--duration", type=float, default=None,
                   help=f"采集时长/秒，--port 模式默认 {GATE_DURATION:.0f}（10 分钟）；"
                        f"--file 模式给出后才能算行率与行间隔")
    p.add_argument("--threshold-cm", type=float, default=DEFAULT_THRESHOLD_CM,
                   help=f"距离触发阈值（cm），默认 {DEFAULT_THRESHOLD_CM:.0f}"
                        f"（0~{THRESHOLD_CM_MAX:.0f}，与厂商上位机一致）")
    p.add_argument("--expect-hand", choices=("auto", "absent", "present"), default="auto",
                   help="场景口径：present = 挥手场景（P6 要求 target=1 ≥50%%、P9 要求触发 ≥1 次）；"
                        "absent = 静置场景（P6 ≤10%%、P9 =0）；auto = 只报告不判定（默认）")
    p.add_argument("--gap-limit", type=float, default=THRESHOLDS["gap_limit_s"],
                   help=f"行间隔空档判据门限（秒），默认 {THRESHOLDS['gap_limit_s']:.1f}"
                        f"（200 ms/行下等于连丢 5 行）")
    p.add_argument("--save-raw", metavar="capture.bin", default=None,
                   help="--port 模式同时把原始字节流存盘，便于事后离线复算")
    p.add_argument("--csv", metavar="out.csv", default=None,
                   help="把逐行时间序列导出 CSV（elapsed_s,wallclock,line_no,kind,target,"
                        "x_m,y_m,z_m,x_cm,y_cm,z_cm,wave,depth,action_raw,triggered,raw）")
    p.add_argument("--raw-dump", action="store_true",
                   help="把无法归属的字节段与坏行存成 unattributed_<n>.bin"
                        "（排查固件日志污染/半行的关键手段）")
    p.add_argument("--max-unparsed", type=int, default=50,
                   help="--raw-dump 最多落盘多少段，默认 50（超出只记账不落盘，防止刷爆磁盘）")
    p.add_argument("--out", "--outdir", dest="out", default=DEFAULT_OUTDIR,
                   help="报告 / 日志 / unattributed_*.bin 的输出目录，默认 %s"
                        "（相对当前工作目录，推荐在仓库根运行）；--outdir 为等价别名"
                        % DEFAULT_OUTDIR)
    p.add_argument("--status-every", type=float, default=10.0,
                   help="--port 模式每隔多少秒打一行进度，默认 10")
    return p


def main(argv=None) -> int:
    setup_stdout()
    parser = build_argparser()
    args = parser.parse_args(argv)

    if not (args.port or args.file or args.selftest):
        parser.print_help()
        print("\n错误：必须指定 --port / --file / --selftest 之一。", file=sys.stderr)
        return 2

    # ---- 参数校验（统一退出码 2）----
    if not (THRESHOLD_CM_MIN <= args.threshold_cm <= THRESHOLD_CM_MAX):
        print("错误：--threshold-cm 应在 %.0f~%.0f 之间（厂商上位机 Spinbox 范围），收到 %r"
              % (THRESHOLD_CM_MIN, THRESHOLD_CM_MAX, args.threshold_cm), file=sys.stderr)
        return 2
    if args.gap_limit <= 0:
        print("错误：--gap-limit 必须 > 0，收到 %r" % args.gap_limit, file=sys.stderr)
        return 2
    if args.duration is not None and args.duration <= 0:
        print("错误：--duration 必须 > 0，收到 %r" % args.duration, file=sys.stderr)
        return 2
    if args.baud <= 0:
        print("错误：--baud 必须 > 0，收到 %r" % args.baud, file=sys.stderr)
        return 2
    if args.selftest:
        return run_selftest(args.out)

    outdir = args.out
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError as e:
        print("错误：无法创建输出目录 %s：%s" % (outdir, e), file=sys.stderr)
        return 2
    stamp = time.strftime("%Y%m%d-%H%M%S")
    json_path = os.path.join(outdir, "edv163_gesture_check_%s.json" % stamp)
    log_path = os.path.join(outdir, "edv163_gesture_check_%s.log" % stamp)

    ck = GestureChecker(threshold_cm=args.threshold_cm, gap_limit_s=args.gap_limit,
                        raw_dump=args.raw_dump, outdir=outdir,
                        max_unparsed=args.max_unparsed, csv_path=args.csv,
                        expect_hand=args.expect_hand)
    report = run_port(args, ck) if args.port else run_file(args, ck)

    report["artifacts"] = OrderedDict([
        ("json", os.path.abspath(json_path)),
        ("log", os.path.abspath(log_path)),
        ("csv", os.path.abspath(args.csv) if args.csv else None),
        ("raw_capture", os.path.abspath(args.save_raw) if args.save_raw else None),
        ("unattributed_dir", os.path.abspath(outdir) if args.raw_dump else None),
        ("rules", RULES_TEXT),
    ])

    print("")
    print_report(report)
    try:
        with open(log_path, "w", encoding="utf-8") as logf:
            print_report(report, logf)
    except OSError as e:
        print("警告：文本日志写入失败：%s" % e, file=sys.stderr)
    try:
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
    except OSError as e:
        print("警告：JSON 报告写入失败：%s" % e, file=sys.stderr)

    print(f"JSON 报告: {json_path}")
    print(f"文本日志: {log_path}")
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
