#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 ⇄ EDV163 桥接帧校验 / 水位统计工具（水位 demo 的关键证据工具）

用途
----
链路：  EDV163 水位雷达 ──UART 256000 8N1──▶ ESP32-C6 ──原生 USB CDC──▶ PC
固件侧是「纯字节透传」，不解析任何协议。所以「桥有没有丢字节、有没有破坏帧、
转义有没有处理对」只能由 PC 侧客观统计来证明。本脚本干这件事：

    · 实时读串口（--port），或离线分析已保存的原始字节流（--file）；
    · 先做转义还原（0x09 0x01 → 0x0A、0x09 0x00 → 0x09），再逐帧校验：
      帧头同步、声明长度、校验和、TLV 长度与帧长是否自洽、帧尾 0x0A；
    · 统计转义错误（非法序列 / 帧中途裸 0x0A / 悬空 0x09）、坏帧五分类、
      重同步次数、未归属字节率（窗口 T+10 s 起算）、msgid 分组连续性；
    · 0x81 上报专表：帧数、通道数分布、target_distance 的 min/median/max（mm 与 cm）、
      按 `--mount-mm` 换算的水位序列（level = 挂高 − 距离），可选 --csv 落盘；
    · 按固定阈值给出 PASS / FAIL，落盘 JSON 报告 + 中文日志；
    · --raw-dump 把「解析不了」的字节段原样存盘（查日志污染的关键手段）；
    · --selftest 用合成流自测（含「正文里带 0x0A/0x09 的帧」转义还原对账）。

协议基线（`radar/edv163.h` / `radar/edv163.c` + EDV163串口通用协议.md 交叉核对）
--------------------------------------------------------------------------------
    256000 / 8N1，大端。帧（长度字段 = 整包字节数，按**转义前**计）：

    0x45 0x44 | 长度(2) | msgid(2) | op(1) | type(1) | TLV长(2) | data N | 校验(1) | 0x0A
      头部 2  + 长度 2 + msgid 2 + op 1 + type 1 + TLV 2 = 10，+ data N + 校验 1 + 帧尾 1
      ⇒ 长度 = 12 + N（N=0 时最短 12，最大 256）

    校验 = 转义前从帧头到 data 末逐字节求和 & 0xFF（不含校验自身与帧尾）
    转义 = 先算长度与校验 → 对「帧头…校验」整段做转义（0x0A→0x09 0x01、0x09→0x09 0x00）
           → 最后贴帧尾 0x0A（帧尾本身不转义）
    op   : 0xA0 GET / 0xA1 SET / 0xA2 REPORT(应答) / 0xA3 STATUS(主动上报) / 0xA4 透传 / 0xA9 ACK
    type : 0x01 产品信息 / 0x02 模块信息 / 0x11 检测延时 / 0x17 上报频率(100ms) / 0x18 工作模式
           0x25 挂高(mm) / 0x26 检测高度 / 0x27 检测范围 / 0x28 自动测高 / 0x29 屏蔽区
           0x81 上报数据（op=0xA3 周期上报）
    0x81 载荷 : count(1B) + count × 7B{channel, delay_presence, target_state,
               motion_direction, target_distance(2B 大端 mm), target_angle(1B 有符号)}
               注：①`edv163_channel_data_t` 是 __attribute__((packed))，6 个字段 = 7 字节，
                  与「8 字节/通道」的文档写法不符，本工具默认 --channel-stride auto
                  用实收长度反推并告警；②驱动解析时也不校验 tlv_len。
    水位     : level_mm = mount_mm − target_distance_mm（distance=0 表示无目标，不计入统计）

判定规则（P0 前提 + 八条硬判据；原文见 --help 与报告，数值在 THRESHOLDS）
------------------------------------------------------------------------
    P0 有效帧数 ≥ 1
    P1 转义错误数 = 0（非法转义序列 / 帧中途裸 0x0A / 悬空 0x09）
    P2 未归属字节率 = 0（0~0.1% 记 WARN，≥0.1% FAIL；窗口从 T+10 s 起算）
    P3 重同步绝对次数 ≤ max(1, round(3 × 时长/1800s))（30 分钟 → 3 次）
    P4 坏帧率 < 0.1%
    P5 帧完整率 ≥ 99.9%
    P6 msgid 跳号率 < 0.1%（按 (op,type) 分组）
    P7 0x81 载荷自洽率 = 100%（载荷长度必须等于 1 + 步长×count）
    P8 0x81 上报帧数 ≥ 1

用法（详见 --help；本机 `python` 是应用商店别名不可用，要用 `py -3`）
----------------------------------------------------------------
    py -3 tools\\edv163_frame_check.py --port COM6 --baud 256000 --duration 1800
    py -3 tools\\edv163_frame_check.py --file capture.bin --duration 1800 --csv level.csv
    py -3 tools\\edv163_frame_check.py --selftest

    所有产物默认落在 logs\\edv163\\（相对当前工作目录，可用 --out 覆盖）。

依赖
----
    只用标准库；--file / --selftest 完全不需要第三方包。
    仅 --port 模式需要 pyserial。
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import tempfile
import time
from collections import Counter, OrderedDict

# ───────────────────────────── 协议常量 ─────────────────────────────

TOOL_NAME = "edv163_frame_check.py"
TOOL_VERSION = "1.0.0"

HEADER = b"\x45\x44"          # 固定帧头
HEADER_B0 = 0x45              # 末尾可能是半个帧头时保留 1 字节
FRAME_TAIL = 0x0A             # 帧尾（不转义）
ESC_BYTE = 0x09               # 转义前缀字节
ESC_CODE_0A = 0x01            # 0x09 0x01 表示正文里的 0x0A
ESC_CODE_09 = 0x00            # 0x09 0x00 表示正文里的 0x09
MIN_FRAME_LEN = 12            # 2头 + 2长度 + 2msgid + 1op + 1type + 2TLV长 + 0数据 + 1校验 + 1帧尾
MAX_FRAME_LEN = 256           # 协议上限（长度字段是转义前的长度）
MSGID_MAX = 0xFFF0            # msgid 回绕点
TYPE_REPORT_DATA = 0x81       # 上报数据
MAX_CHANNELS = 40             # edv163.h 里 channels[40]
DEFAULT_BAUD = 256000         # EDV163 出厂波特率（EDV11P 是 921600，别抄错）
DEFAULT_MOUNT_MM = 2000       # 水位 demo 默认挂高 2 m（量程 0~2 m）
GATE_DURATION = 1800.0        # 30 分钟采集口径
DEFAULT_OUTDIR = os.path.join("logs", "edv163")
DEFAULT_WARMUP_S = 10.0       # 未归属字节统计窗口起算点（跳过上电日志）

OP_NAMES = OrderedDict([
    (0xA0, "GET"),
    (0xA1, "SET"),
    (0xA2, "REPORT"),
    (0xA3, "STATUS"),
    (0xA4, "透传"),
    (0xA9, "ACK"),
])

TYPE_NAMES = OrderedDict([
    (0x01, "产品信息"), (0x02, "模块信息"), (0x04, "波特率"), (0x11, "检测延时"),
    (0x12, "闭锁时间"), (0x17, "上报频率"), (0x18, "工作模式"), (0x19, "雷达频率"),
    (0x1A, "强制无人"), (0x1B, "触发使能"), (0x1C, "存在保持时间"), (0x1D, "30min强制无人"),
    (0x1F, "恢复出厂"), (0x24, "触发模式"), (0x25, "挂高"), (0x26, "检测高度"),
    (0x27, "检测范围"), (0x28, "自动测高"), (0x29, "屏蔽区"),
    (0x81, "上报数据"),
])

# 判定阈值（写死在代码里，同时打印进 --help 与报告，便于 Gate 复核）
THRESHOLDS = {
    "escape_errors_max": 0,             # P1 转义错误数必须为 0
    "unattributed_rate_max": 0.001,     # P2 未归属字节率 < 0.1%（=0 才 PASS；0<x<0.1% 记 WARN）
    "bad_frame_rate_max": 0.001,        # P4 坏帧率 < 0.1%
    "complete_rate_min": 0.999,         # P5 帧完整率 ≥ 99.9%
    "fake_sync_loss_rate_max": 0.001,   # 假同步丢帧估算率 < 0.1%（观察项，随 P5 报告）
    "msgid_gap_rate_max": 0.001,        # P6 msgid 跳号率 < 0.1%
    "report_payload_ok_min": 1.0,       # P7 0x81 载荷自洽率 = 100%
    "report_frames_min": 1,             # P8 至少 1 帧 0x81
}
RESYNC_LIMIT_30MIN = 3
DEFAULT_STRIDE = 7                  # 通道步长（驱动实测 7B/通道；文档写 8B）
STRIDE_CHOICES = ("auto", "7", "8")

# 判定规则原文（--help 与报告共用，避免两处不一致）
RULES_TEXT = f"""判定规则：P0 前提 + 八条硬判据（按优先级顺序判定，任一 FAIL 即 FAIL）
  P0 前提：有效帧数 ≥ 1                       否则整份采集无效，其余判据 SKIP
  P1 转义错误数 = 0
     · 转义错误 = ①非法转义序列（0x09 后面既不是 0x00 也不是 0x01，例如 0x09 0x05）
                  ②帧中途出现的裸 0x0A（转义前 0x0A 只可能出现在帧尾）
                  ③流在转义序列中途结束（悬空 0x09：最后一个字节是 0x09 但没有转义码）
     · 计数按「事件」：一次非法序列 / 一次裸 0x0A 各记 1；同一帧命中多类分别计数
     · 出现转义错误说明「转义层」被破坏，此时长度/校验统计都不可信 → 优先修它
     · 判定：= {THRESHOLDS['escape_errors_max']} → PASS；> 0 → FAIL
  P2 未归属字节率  = (窗口内收字节 − 窗口内合法帧字节) / 窗口内收字节
     · 窗口从 T+{DEFAULT_WARMUP_S:.0f}s 起算（跳过 bootloader/日志，可用 --warmup 调整）
     · 「合法帧字节」按**转义后的在线字数**计（一个字节都不许多、不许少）
     · 判定：= 0 → PASS；0 < x < {THRESHOLDS['unattributed_rate_max']:.2%} → WARN（不判 FAIL，
             但必须写进 Gate 记录并点名来源）；≥ {THRESHOLDS['unattributed_rate_max']:.2%} → FAIL
  P3 重同步绝对次数 ≤ max(1, round({RESYNC_LIMIT_30MIN} × 时长/1800s))
     · 30 分钟 → 3 次；60 分钟 → 6 次；10 分钟 → 1 次。超出说明有字符在「持续」插入流里
  P4 坏帧率         = 坏帧数 / (好帧数 + 坏帧数)                 必须 < {THRESHOLDS['bad_frame_rate_max']:.2%}
     坏帧 = 长度非法 + 校验错 + TLV长度与帧长不符 + 转义错误 + 帧尾错误 + 截断帧（六类分别计数）
  P5 帧完整率       = 好帧数 / (好帧数 + 坏帧数 + 重同步)        必须 ≥ {THRESHOLDS['complete_rate_min']:.2%}
     ★ 这是「水位 demo 30 分钟帧完整率 ≥ 99.9%」的直接判定指标
  P6 msgid 跳号率   = 跳号次数 / 组内帧数（按 (op,type) 分组取最大组） 必须 < {THRESHOLDS['msgid_gap_rate_max']:.2%}
     组内样本 < --msgid-min-samples 的组不参与判定；若「不分组」超标而「分组」全达标，
     说明 msgid 按命令类别独立计数，不算丢帧
  P7 0x81 载荷自洽率 = 长度自洽的 0x81 帧 / 0x81 帧总数         必须 = {THRESHOLDS['report_payload_ok_min']:.0%}
     自洽判据：载荷长度 == 1 + 步长 × count（步长默认按实收长度自动锁定 7 或 8，
     也可用 --channel-stride 7/8 强制）；count > {MAX_CHANNELS} 视为异常
  P8 0x81 上报帧数 ≥ {THRESHOLDS['report_frames_min']}
     · 水位 demo 没有 0x81 就没有距离数据；只收到 ACK/SET 应答说明雷达没在上报

转义还原口径（P1 与全部统计的前提）
  · 发送侧顺序：算长度 → 算校验 → 转义「帧头…校验」整段 → 贴帧尾 0x0A（帧尾不转义）
  · 接收侧必须反序：先按转义规则还原，再按声明的（转义前）长度切帧、再验校验
  · 因此「在线字数」≠「声明长度」：转义后每出现一个 0x09 0x0? 就多 1 字节
  · 帧内绝不允许出现裸 0x0A（它是唯一的分帧标志）；收到裸 0x0A 即判帧提前结束
  · 提示：0x81 单通道帧的 TLV 长度字节是 0x08（1+7），双通道 0x0F；而距离字段的
    高/低字节完全可能是 0x09/0x0A（如 2569 mm = 0x0A09），所以转义层每天都在这条链路上工作

判据之间的关系与冲突处理（重要）
  · P1 优先级最高：转义一错，后面的长度/校验统计都在错误切分上算出来的，先修转义
  · P2 比「假同步丢帧估算率」更严：P2 按字节精确归属，后者按事件估算 → 冲突时以 P2 为准
  · 坏帧字节也算「未归属」，所以 P4 > 0 必然 P2 > 0 → 先判 P2，P4 用来说明「坏在哪一类」
  · P3 与比率类冲突时以 P3 为准：短采集里几次重同步能把比率摊到 0.1% 以下，绝对次数照样是缺陷
  · P7/P8 只在水位场景生效；没有 0x81 帧时 P7 记 N/A，但 P8 会 FAIL（水位 demo 必须有距离数据）
  · WARN 不改变 PASS，但必须在 Gate 记录里逐条列出（P2 的 WARN 尤其要说清来源字节段）

不影响 PASS/FAIL 但会进 warnings 的观察项
  · 出现未识别 op/type；0x81 通道数=0；距离样本为 0（全程无目标）
  · 0x81 帧间隔明显异常（> 5× 中位间隔，疑似大面积丢帧）
  · 采集时长不足 --duration 的 90%；单段垃圾字节 > 1 KiB（疑似日志污染）
  · 0x81 通道步长锁定为 7 而协议文档写 8（文档与实现不一致，需更正文档）

Gate 引用判据（一句话，可直接抄）
  「T-FC-01 的 转义错误 = 0 且 未归属字节率 = 0 且 帧完整率 ≥ 99.9%，同时 0x81 载荷自洽率 = 100%、
    0x81 帧数 ≥ 1，四项数值由 tools\\edv163_frame_check.py 输出 JSON 的 checks[] 直接给出且退出码为 0。」
"""

# 疑似「日志污染」的特征串（与 EDV11P 工具保持一致）
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


def hex_full(b: bytes) -> str:
    """bytes → 完整十六进制串（不截断），用于自测的逐字节对账。"""
    return " ".join(f"{x:02X}" for x in b)


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
    """在字节段里查找疑似日志污染的特征串（强特征无条件上报，弱特征要求文本为主）。"""
    hits = [m.decode("ascii", "replace") for m in STRONG_POLLUTION_MARKERS if m in b]
    if printable_ratio(b) >= 0.7:
        hits += [m.decode("ascii", "replace") for m in WEAK_POLLUTION_MARKERS if m in b]
    return hits


def fmt_rate(v):
    """比率格式化；None → 'N/A'。"""
    return "N/A" if v is None else f"{v * 100:.4f}%"


def median_from_counter(counter: Counter):
    """从频次表算中位数（避免为几十万个样本保存完整列表）。

    注意：偶数量样本时取「第 n/2 个与第 n/2+1 个」两值的平均（教科书定义），
    不要写成「相邻两个频次的边界值」——那会整体偏小一档。
    """
    total = sum(counter.values())
    if total == 0:
        return None
    keys = sorted(counter)

    def value_at(pos: int) -> float:
        """返回第 pos 个样本（1-based）的取值。"""
        acc = 0
        for k in keys:
            acc += counter[k]
            if acc >= pos:
                return float(k)
        return float(keys[-1])

    if total % 2 == 1:
        return value_at((total + 1) // 2)
    return (value_at(total // 2) + value_at(total // 2 + 1)) / 2.0


def percentile(sorted_vals, p: float):
    """线性插值分位数；空列表返回 None。"""
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p
    f = int(k)
    c = min(f + 1, len(sorted_vals) - 1)
    if f == c:
        return float(sorted_vals[f])
    return sorted_vals[f] + (sorted_vals[c] - sorted_vals[f]) * (k - f)


def op_label(op: int) -> str:
    return OP_NAMES.get(op, "未识别")


def type_label(t: int) -> str:
    return TYPE_NAMES.get(t, "未识别")


# ───────────────────────────── 组帧（自测 / 文档示例） ─────────────────────────────

def escape_body(raw: bytes) -> bytes:
    """按协议转义：0x0A→0x09 0x01，0x09→0x09 0x00（帧尾不参与）。"""
    out = bytearray()
    for b in raw:
        if b == FRAME_TAIL:
            out += bytes([ESC_BYTE, ESC_CODE_0A])
        elif b == ESC_BYTE:
            out += bytes([ESC_BYTE, ESC_CODE_09])
        else:
            out.append(b)
    return bytes(out)


def unescape_body(raw: bytes) -> bytes:
    """按协议还原（仅用于自测对账，主解析器是逐字节状态机）。"""
    out = bytearray()
    i = 0
    while i < len(raw):
        b = raw[i]
        if b == ESC_BYTE and i + 1 < len(raw):
            nxt = raw[i + 1]
            if nxt == ESC_CODE_0A:
                out.append(FRAME_TAIL)
                i += 2
                continue
            if nxt == ESC_CODE_09:
                out.append(ESC_BYTE)
                i += 2
                continue
        out.append(b)
        i += 1
    return bytes(out)


def _frame_body(msgid: int, op: int, typ: int, payload: bytes,
                force_len: int = None, force_tlv: int = None) -> bytearray:
    """构造**转义前**的帧体（帧头 … data），不含校验与帧尾。"""
    body = bytearray()
    body += HEADER
    body += struct.pack(">H", 0)                       # 长度占位
    body += struct.pack(">H", msgid & 0xFFFF)
    body += bytes([op & 0xFF, typ & 0xFF])
    body += struct.pack(">H", len(payload) if force_tlv is None else (force_tlv & 0xFFFF))
    body += payload
    total = len(body) + 2                              # + 校验 + 帧尾
    body[2:4] = struct.pack(">H", total if force_len is None else (force_len & 0xFFFF))
    return body


def frame_checksum(msgid: int, op: int, typ: int, payload: bytes) -> int:
    """按协议算校验和（转义前、不含校验自身与帧尾）。"""
    return sum(_frame_body(msgid, op, typ, payload)) & 0xFF


def msgid_hitting_checksum(base_msgid: int, op: int, typ: int, payload: bytes, target: int) -> int:
    """找一个 msgid，使该帧校验和恰好等于 target（用来造「校验字节也必须转义」的用例）。"""
    for d in range(256):
        m = (base_msgid + d) & 0xFFFF
        if frame_checksum(m, op, typ, payload) == target:
            return m
    raise RuntimeError("找不到能命中校验和 0x%02X 的 msgid" % target)


def build_frame(msgid: int, op: int, typ: int, payload: bytes = b"",
                break_checksum: bool = False, force_len: int = None,
                force_tlv: int = None, insert_after_len: bytes = None,
                truncate_to: int = None) -> bytes:
    """按协议构造**在线字节**（转义后 + 帧尾），自测与文档示例用。

    break_checksum     ：故意写错校验和（造坏帧）
    force_len          ：强改长度字段（造「长度非法」或「截断」）
    force_tlv          ：强改 TLV 长度字段（造「TLV 长度不符」）
    insert_after_len   ：在「帧头+长度」之后插入原始字节（造转义错误用例）
    truncate_to        ：把整帧截断成前 N 字节（造截断帧）
    """
    body = _frame_body(msgid, op, typ, payload, force_len=force_len, force_tlv=force_tlv)
    cs = sum(body) & 0xFF
    if break_checksum:
        cs ^= 0xFF                                     # 一定与原值不同
    pre = bytes(body) + bytes([cs])
    wire = escape_body(pre) + bytes([FRAME_TAIL])
    if insert_after_len:
        wire = wire[:4] + insert_after_len + wire[4:]
    if truncate_to is not None:
        wire = wire[:truncate_to]
    return wire


def report_payload(channels, stride: int = DEFAULT_STRIDE) -> bytes:
    """按 0x81 载荷格式打包：count(1B) + count × {channel, delay, state, motion, dist(2B), angle(1B)}。"""
    out = bytearray()
    out.append(len(channels) & 0xFF)
    for c in channels:
        channel, delay, state, motion, dist, angle = c
        out += bytes([channel & 0xFF, delay & 0xFF, state & 0xFF, motion & 0xFF])
        out += struct.pack(">H", dist & 0xFFFF)
        out.append(angle & 0xFF)
        if stride == 8:                                # 兼容「文档口径」的 8B/通道
            out.append(0x00)
    return bytes(out)


# ───────────────────────────── 校验器 ─────────────────────────────

class Checker:
    """流式 EDV163 帧校验器：喂字节进来，内部完成转义还原、帧同步、校验与全部统计。"""

    BAD_KINDS = OrderedDict([
        ("len_invalid", "长度非法（<12 或 >256）"),
        ("checksum_error", "校验和错误"),
        ("tlv_len_mismatch", "TLV 长度与帧长不符"),
        ("escape_error", "转义错误（非法序列 / 帧内裸 0x0A）"),
        ("tail_error", "帧尾不是 0x0A"),
        ("truncated", "截断帧（流结束时字节不足）"),
    ])

    ESCAPE_ERROR_KINDS = OrderedDict([
        ("illegal_sequence", "非法转义序列（0x09 后不是 0x00/0x01）"),
        ("bare_0a", "帧中途出现裸 0x0A"),
        ("dangling_09", "悬空 0x09（流在转义序列中途结束）"),
    ])

    def __init__(self, raw_dump: bool = False, outdir: str = DEFAULT_OUTDIR,
                 max_unparsed: int = 50, csv_path: str = None,
                 msgid_min_samples: int = 20, detail_limit: int = 200,
                 warmup_s: float = DEFAULT_WARMUP_S, mount_mm: int = DEFAULT_MOUNT_MM,
                 level_channel="auto", channel_stride="auto"):
        self.buf = bytearray()
        self.abs_offset = 0            # 已从缓冲区消费掉的字节数（= buf[0] 在流中的偏移）

        # ---- 基础统计 ----
        self.total_bytes = 0
        self.frames_ok = 0
        self.by_op = Counter()
        self.by_type = Counter()
        self.by_op_type = Counter()
        self.unknown_op_frames = 0

        # ---- 未归属字节（窗口 T+warmup 起算）----
        self.warmup_s = float(warmup_s)
        self.window_started = bool(warmup_s <= 0)
        self.window_start_offset = 0
        self.frame_bytes_total = 0     # 全程：合法帧的**在线**字节合计
        self.frame_bytes_window = 0    # 窗口内：合法帧的**在线**字节合计

        # ---- 坏帧 / 转义错误 ----
        self.bad = Counter({k: 0 for k in self.BAD_KINDS})
        self.bad_detail = []
        self.detail_limit = detail_limit
        self.escape_errors = 0
        self.escape_error_kinds = Counter({k: 0 for k in self.ESCAPE_ERROR_KINDS})
        self.escape_pairs = 0          # 成功还原的转义对
        self.escape_pairs_0a = 0       # 0x09 0x01 → 0x0A
        self.escape_pairs_09 = 0       # 0x09 0x00 → 0x09
        self.frames_with_escape = 0    # 含转义还原的合法帧数
        self.escaped_samples = []      # 前若干个含转义的帧（正文还原对账用）

        # ---- 重同步 / 垃圾段 ----
        self.resync_events = 0
        self.garbage_bytes = 0
        self._garbage = bytearray()
        self._garbage_offset = None
        self.segments = []

        # ---- 0x81 上报 / 水位 ----
        self.mount_mm = int(mount_mm)
        self.level_channel = level_channel
        self.level_channel_missing = 0
        self.report_frames = 0
        self.report_by_op = Counter()
        self.report_payload_errors = 0
        self.channel_count_hist = Counter()
        self.distance_hist = Counter()      # 距离样本（mm，仅 distance>0）
        self.distance_zero_samples = 0
        self.level_hist = Counter()         # 水位样本（mm）
        self.level_preview = []             # 末尾若干个水位点（JSON 里给一段预览）
        self.level_preview_max = 20
        self.report_intervals = []          # 0x81 帧间隔（秒）
        self.report_intervals_max = 200000
        self._last_report_t = None
        self.channel_stride = None          # 锁定后为 7 或 8
        self._stride_pref = channel_stride

        # ---- msgid 连续性 ----
        self.msgid_min_samples = msgid_min_samples
        self.msg_groups = OrderedDict()
        self.msg_overall = {"last": None, "count": 0, "jumps": 0, "dups": 0}
        self.jump_log = []
        self.dup_log = []

        # ---- 事件/告警 ----
        self.warnings = []
        self.events = []
        self._warned_unattr = False
        self._warned_stride = False

        # ---- 输出控制 ----
        self.raw_dump = raw_dump
        self.outdir = outdir
        self.max_unparsed = max_unparsed
        self._csv = None
        self._csv_path = csv_path
        if csv_path:
            self._csv = open(csv_path, "w", encoding="utf-8", newline="")
            self._csv.write("elapsed_s,wallclock,frame_no,msgid,op,channels,level_channel,"
                            "distance_mm,distance_cm,mount_mm,level_mm,level_cm,"
                            "target_state,motion_direction,target_angle\n")
        self._start_wall = None
        self._start_mono = None
        self._sim_span = None
        self._sim_total = None

    # ---------------- 时间基准 ----------------
    def mark_start(self) -> None:
        self._start_wall = time.time()
        self._start_mono = time.monotonic()

    def set_synthetic_time(self, span_s: float, total_bytes: int) -> None:
        """离线模式：没有时间戳时，按「字节位置线性插值」到给定的 --duration。"""
        self._sim_span = span_s
        self._sim_total = total_bytes or None

    def disable_warmup(self) -> None:
        """离线且没有时基时调用：无法判断 T+10 s，退化为从流首起算。"""
        self.warmup_s = 0.0
        self.window_started = True
        self.window_start_offset = 0

    def _maybe_start_window(self, now) -> None:
        if self.window_started:
            return
        if self._now(now) >= self.warmup_s:
            self.window_started = True
            self.window_start_offset = self.abs_offset

    def window_stats(self) -> dict:
        """未归属字节统计（P2 判据的数值来源）。"""
        window_bytes = max(0, self.abs_offset - self.window_start_offset)
        unattr = window_bytes - self.frame_bytes_window
        if unattr < 0:
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
                keep = 1 if buf[-1:] == bytes([HEADER_B0]) else 0
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
            declared = (buf[2] << 8) | buf[3]
            if declared < MIN_FRAME_LEN or declared > MAX_FRAME_LEN:
                self._reject("len_invalid", bytes(buf[:4]), declared, now,
                             extra="声明长度 %d，合法范围 %d~%d（转义前口径）"
                                   % (declared, MIN_FRAME_LEN, MAX_FRAME_LEN),
                             raw_kind="wire")
                # 只丢这 2 字节假帧头，剩下的重新找帧头（能最快找回真同步）
                self._add_garbage(bytes(buf[:2]), now)
                del buf[:2]
                continue

            # ---- 转义还原（边还原边数转义错误）----
            decoded = bytearray(buf[:4])          # 帧头 2 + 长度 2（长度字节内含 0x09/0x0A 时为非法帧）
            i = 4
            esc_used = 0
            esc_error = None
            while len(decoded) < declared - 1:    # 目标：还原出「帧头…校验」共 declared-1 字节
                if i >= len(buf):
                    return                        # 数据还没到齐，等下一批
                b = buf[i]
                if b == ESC_BYTE:
                    if i + 1 >= len(buf):
                        return                    # 转义对还没收全
                    nxt = buf[i + 1]
                    if nxt == ESC_CODE_0A:
                        decoded.append(FRAME_TAIL)
                        self.escape_pairs += 1
                        self.escape_pairs_0a += 1
                        esc_used += 1
                        i += 2
                    elif nxt == ESC_CODE_09:
                        decoded.append(ESC_BYTE)
                        self.escape_pairs += 1
                        self.escape_pairs_09 += 1
                        esc_used += 1
                        i += 2
                    else:
                        esc_error = ("illegal_sequence", i,
                                     "0x09 之后是 0x%02X（只允许 0x00/0x01）" % nxt)
                        break
                elif b == FRAME_TAIL:
                    esc_error = ("bare_0a", i, "帧中途出现裸 0x0A（它是唯一分帧标志，正文里必须转义）")
                    break
                else:
                    decoded.append(b)
                    i += 1

            if esc_error is not None:
                kind, pos, desc = esc_error
                self.escape_errors += 1
                self.escape_error_kinds[kind] += 1
                self._reject("escape_error", bytes(buf[:pos + 2]), declared, now,
                             extra="[%s] %s（偏移 %d，字节 %s）"
                                   % (kind, desc, pos, hexs(bytes(buf[max(0, pos - 2):pos + 3]), 5)),
                             raw_kind="wire")
                # 只丢这 2 字节假帧头，让后面的字节重新参与同步
                self._add_garbage(bytes(buf[:2]), now)
                del buf[:2]
                continue

            if i >= len(buf):
                return                            # 还差帧尾字节

            if buf[i] != FRAME_TAIL:
                self._reject("tail_error", bytes(buf[:i + 1]), declared, now,
                             extra="帧尾应为 0x0A，实收 0x%02X（声明长度 %d，还原后 %d 字节）"
                                   % (buf[i], declared, len(decoded)),
                             raw_kind="wire")
                self._add_garbage(bytes(buf[:i + 1]), now)
                del buf[:i + 1]
                continue

            n_wire = i + 1                        # 本帧在线上实际占用的字节数（含转义膨胀）
            frame = bytes(decoded)                # 长度 == declared
            cs_calc = sum(frame[:declared - 2]) & 0xFF
            cs_in = frame[declared - 2]
            if cs_calc != cs_in:
                self._reject("checksum_error", frame, declared, now,
                             extra="计算得 0x%02X，帧内为 0x%02X（在线占用 %d 字节）"
                                   % (cs_calc, cs_in, n_wire),
                             raw_kind="decoded")
                self._add_garbage(bytes(buf[:n_wire]), now)
                del buf[:n_wire]
                continue

            tlv = (frame[8] << 8) | frame[9]
            expect_tlv = declared - MIN_FRAME_LEN
            if tlv != expect_tlv:
                self._reject("tlv_len_mismatch", frame, declared, now,
                             extra="TLV 声明 %d，按帧长应为 %d" % (tlv, expect_tlv),
                             raw_kind="decoded")
                self._add_garbage(bytes(buf[:n_wire]), now)
                del buf[:n_wire]
                continue

            frame_start = self.abs_offset
            del buf[:n_wire]
            self.abs_offset += n_wire
            self._on_good_frame(frame, now, frame_start, n_wire, esc_used)

    # ---------------- 垃圾段 / 重同步 ----------------
    def _add_garbage(self, data: bytes, now) -> None:
        if not data:
            return
        if self._garbage_offset is None:
            self._garbage_offset = self.abs_offset
        self._garbage += data
        self.garbage_bytes += len(data)
        self.abs_offset += len(data)

    def _close_garbage(self, kind: str, now, count_resync: bool = True):
        """收尾一段「无法解析的字节段」：计入重同步、落 unparsed_<n>.bin、记录诊断。"""
        if not self._garbage:
            self._garbage_offset = None
            return None
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
            self.warnings.append(
                "unparsed 段 #%d（偏移 %d，%d 字节，可打印比例 %.2f）含疑似日志/文本特征 %s"
                " —— 数据流里混入了非协议字节，优先怀疑固件日志/printf（详见测试计划第五节）"
                % (seg["index"], offset, len(data), seg["printable_ratio"], "/".join(hits)))
        if len(data) > 1024:
            self.warnings.append("unparsed 段 #%d 长达 %d 字节，疑似持续写出而非偶发误码"
                                 % (seg["index"], len(data)))
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
    def _reject(self, kind: str, raw: bytes, declared: int, now, extra: str = "",
                raw_kind: str = "wire") -> None:
        self.bad[kind] += 1
        if len(self.bad_detail) < self.detail_limit:
            self.bad_detail.append({
                "kind": kind,
                "kind_name": self.BAD_KINDS.get(kind, kind),
                "offset": self.abs_offset,
                "elapsed_s": round(self._now(now), 3),
                "declared_len": declared,
                "raw_kind": raw_kind,           # wire = 在线字节（仍带转义）；decoded = 已还原
                "hex": hexs(raw, 32),
                "ascii": ascii_preview(raw, 32),
                "note": extra,
            })

    # ---------------- 合法帧 ----------------
    def _on_good_frame(self, frame: bytes, now, frame_start: int, n_wire: int, esc_used: int) -> None:
        # 一帧成功解析 = 之前那段无法解析的字节就此结束 → 记一次重同步
        self._close_garbage("garbage_or_badframe", now, count_resync=True)

        declared = len(frame)
        msgid = (frame[4] << 8) | frame[5]
        op = frame[6]
        typ = frame[7]
        tlv = (frame[8] << 8) | frame[9]
        payload = frame[10:10 + tlv]

        # 归属统计：只有「转义 + 长度 + 校验 + TLV 全通过」的帧才算已归属字节（按在线字节数）
        self.frame_bytes_total += n_wire
        if self.window_started and frame_start >= self.window_start_offset:
            self.frame_bytes_window += n_wire

        self.frames_ok += 1
        self.by_op[op] += 1
        self.by_type[typ] += 1
        self.by_op_type[(op, typ)] += 1
        if op not in OP_NAMES:
            self.unknown_op_frames += 1

        if esc_used:
            self.frames_with_escape += 1
            if len(self.escaped_samples) < 5:
                self.escaped_samples.append({
                    "msgid": msgid,
                    "op": op, "type": typ,
                    "escapes": esc_used,
                    "wire_hex": hexs(frame, 0),      # 占位，下面覆盖成在线字节
                    "payload_hex": hex_full(payload),
                    "payload": payload,
                })
                # 记录线上去转义膨胀：在线字节数 - 声明长度
                self.escaped_samples[-1]["wire_len"] = n_wire
                self.escaped_samples[-1]["declared_len"] = declared
                self.escaped_samples[-1].pop("wire_hex", None)

        self._track_msgid(msgid, op, typ, self._now(now))

        if typ == TYPE_REPORT_DATA:
            self._on_report_frame(op, payload, now, msgid)

    # ---------------- msgid 连续性 ----------------
    @staticmethod
    def _expected_next(prev: int) -> int:
        return 0 if prev >= MSGID_MAX else prev + 1

    def _track_msgid(self, msgid: int, op: int, typ: int, now: float) -> None:
        ov = self.msg_overall
        if ov["last"] is not None and msgid != self._expected_next(ov["last"]):
            ov["jumps"] = ov.get("jumps", 0) + 1
            self._log_jump(None, op, typ, ov["last"], msgid, now)
        ov["last"] = msgid
        ov["count"] += 1

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

    # ---------------- 0x81 上报 / 水位 ----------------
    def _lock_stride(self, payload: bytes):
        """确定「每通道几字节」：优先按实收长度反推，锁定后一直用。"""
        if self.channel_stride is not None:
            return self.channel_stride
        if self._stride_pref in ("7", "8"):
            self.channel_stride = int(self._stride_pref)
            return self.channel_stride
        if not payload:
            return None
        count = payload[0]
        if count == 0:
            return None                     # count=0 时无法区分 7/8，等下一帧
        n = len(payload)
        cands = []
        if n == 1 + 7 * count:
            cands.append(7)
        if n == 1 + 8 * count:
            cands.append(8)
        if len(cands) == 1:
            self.channel_stride = cands[0]
            # 锁定 7 = 正常（驱动实测口径），不打告警；只有锁定 8 才说明固件真的按「文档口径」发，
            # 那与 radar/edv163.c 的解析步长冲突，必须告警让上层（网页/网关）别按 7 写死。
            if self.channel_stride == 8 and not self._warned_stride:
                self._warned_stride = True
                self.warnings.append(
                    "0x81 载荷步长锁定为 8 字节/通道：与 radar/edv163.c L372-390 的 7 字节/通道解析冲突。"
                    "协议文档 §3「每通道 8 字节」属笔误，正常固件应为 7；若实测真是 8，说明固件/协议已变更，"
                    "必须先与组件库维护者对齐再改上层解析与本文档")
        elif not self._warned_stride and payload[0] and len(payload) not in (
                1 + 7 * payload[0], 1 + 8 * payload[0]):
            self._warned_stride = True
            self.warnings.append(
                "0x81 载荷长度 %d 既不符合 1+7×count(%d) 也不符合 1+8×count(%d)（count=%d）："
                "无法锁定通道步长，按载荷异常计（疑似帧被截断或协议变更）"
                % (len(payload), 1 + 7 * payload[0], 1 + 8 * payload[0], payload[0]))
        return self.channel_stride

    def _on_report_frame(self, op: int, payload: bytes, now, msgid: int) -> None:
        self.report_frames += 1
        self.report_by_op[op] += 1
        t = self._now(now)

        if self._last_report_t is not None and len(self.report_intervals) < self.report_intervals_max:
            self.report_intervals.append(max(0.0, t - self._last_report_t))
        self._last_report_t = t

        self._lock_stride(payload)          # 按实收长度反推并锁定通道步长（7 或 8）

        bad_reason = None
        chans = []
        if not payload:
            bad_reason = "载荷为空"
        elif self.channel_stride is None:
            bad_reason = ("无法确定通道步长：count=%d，载荷 %d 字节（7B 口径应为 %d，8B 口径应为 %d）"
                          % (payload[0], len(payload), 1 + 7 * payload[0], 1 + 8 * payload[0]))
        else:
            count = payload[0]
            need = 1 + self.channel_stride * count
            if count > MAX_CHANNELS:
                bad_reason = "通道数 %d 超过上限 %d" % (count, MAX_CHANNELS)
            elif need != len(payload):
                bad_reason = ("载荷长度与 count 不符：count=%d，步长=%d，应有 %d 字节，实收 %d 字节"
                              % (count, self.channel_stride, need, len(payload)))
            else:
                for k in range(count):
                    base = 1 + self.channel_stride * k
                    angle_raw = payload[base + 6]
                    chans.append({
                        "channel": payload[base],
                        "delay_presence": payload[base + 1],
                        "target_state": payload[base + 2],
                        "motion_direction": payload[base + 3],
                        "distance": (payload[base + 4] << 8) | payload[base + 5],
                        "angle": angle_raw - 256 if angle_raw >= 128 else angle_raw,
                    })
        if bad_reason:
            self.report_payload_errors += 1
            if self.report_payload_errors <= 5:
                self.warnings.append("0x81 载荷解析异常（第 %d 次，msgid=0x%04X）：%s；载荷 %s"
                                     % (self.report_payload_errors, msgid, bad_reason, hexs(payload, 24)))
            self._csv_row(t, msgid, op, None, None, None)
            return

        self.channel_count_hist[len(chans)] += 1
        for c in chans:
            if c["distance"] > 0:
                self.distance_hist[c["distance"]] += 1
            else:
                self.distance_zero_samples += 1

        # 选水位通道：auto = 第一个有距离的通道，否则通道 0；也可以指定通道号
        sel = None
        if self.level_channel == "auto":
            for c in chans:
                if c["distance"] > 0:
                    sel = c
                    break
            if sel is None and chans:
                sel = chans[0]
        else:
            for c in chans:
                if c["channel"] == int(self.level_channel):
                    sel = c
                    break
            if sel is None:
                self.level_channel_missing += 1

        level = None
        if sel is not None and sel["distance"] > 0:
            level = self.mount_mm - sel["distance"]
            self.level_hist[level] += 1
            self.level_preview.append({"elapsed_s": round(t, 3), "level_mm": level})
            if len(self.level_preview) > self.level_preview_max:
                self.level_preview.pop(0)
        self._csv_row(t, msgid, op, len(chans), sel, level)

    def _csv_row(self, t, msgid, op, channels, sel, level) -> None:
        if self._csv is None:
            return
        def num(v):
            return "" if v is None else str(v)
        dist = sel["distance"] if sel else None
        vals = [
            "%.3f" % t,
            time.strftime("%Y-%m-%d %H:%M:%S"),
            str(self.frames_ok),
            "0x%04X" % msgid,
            "0x%02X" % op,
            num(channels),
            num(sel["channel"] if sel else None),
            num(dist),
            "" if dist is None else "%.1f" % (dist / 10.0),
            str(self.mount_mm),
            num(level),
            "" if level is None else "%.1f" % (level / 10.0),
            num(sel["target_state"] if sel else None),
            num(sel["motion_direction"] if sel else None),
            num(sel["angle"] if sel else None),
        ]
        self._csv.write(",".join(vals) + "\n")

    # ---------------- 收尾 ----------------
    def finish(self) -> None:
        """流结束：把残留判定为「截断帧」或尾部垃圾；顺带查悬空 0x09。"""
        buf = self.buf
        if buf:
            declared = (buf[2] << 8) | buf[3] if len(buf) >= 4 else None
            head = buf[:2] == HEADER
            declared_ok = declared is not None and MIN_FRAME_LEN <= declared <= MAX_FRAME_LEN
            if head and declared_ok:
                self.bad["truncated"] += 1
                if len(self.bad_detail) < self.detail_limit:
                    self.bad_detail.append({
                        "kind": "truncated",
                        "kind_name": self.BAD_KINDS["truncated"],
                        "offset": self.abs_offset,
                        "elapsed_s": 0.0,
                        "declared_len": declared,
                        "actual_len": len(buf),
                        "raw_kind": "wire",
                        "hex": hexs(bytes(buf), 32),
                        "ascii": ascii_preview(buf, 32),
                        "note": "声明 %d 字节（转义前），流结束时只收到 %d 字节在线数据"
                                % (declared, len(buf)),
                    })
                if buf[-1:] == bytes([ESC_BYTE]):
                    self.escape_errors += 1
                    self.escape_error_kinds["dangling_09"] += 1
                    self.warnings.append("流在转义序列中途结束（最后的字节是 0x09，缺少转义码），"
                                         "已计 1 次转义错误")
                self._add_garbage(bytes(buf), None)
                self._close_garbage("truncated_tail", None, count_resync=False)
            else:
                self._add_garbage(bytes(buf), None)
                self._close_garbage("tail_garbage", None, count_resync=False)
            del buf[:]
        # 无论残留是否为空，都要把「最后一段还没收尾的垃圾」结掉：
        # 纯垃圾流（整段没有帧头）在 _drain 里就已经被吃进 garbage，不在这里收尾会漏掉明细
        self._close_garbage("tail_garbage", None, count_resync=False)
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

    def report_view(self) -> dict:
        """0x81 上报专表 + 水位统计。"""
        dist_n = sum(self.distance_hist.values())
        dist_min = min(self.distance_hist) if self.distance_hist else None
        dist_max = max(self.distance_hist) if self.distance_hist else None
        dist_med = median_from_counter(self.distance_hist)
        lvl_n = sum(self.level_hist.values())
        lvl_min = min(self.level_hist) if self.level_hist else None
        lvl_max = max(self.level_hist) if self.level_hist else None
        lvl_med = median_from_counter(self.level_hist)
        if lvl_n:
            lvl_sum = sum(k * v for k, v in self.level_hist.items())
            lvl_mean = lvl_sum / lvl_n
        else:
            lvl_mean = None
        ivs = sorted(self.report_intervals)
        iv_med = median_from_counter(Counter(round(x, 3) for x in self.report_intervals))
        return {
            "frames": self.report_frames,
            "by_op": OrderedDict(("0x%02X %s" % (op, op_label(op)), n)
                                 for op, n in sorted(self.report_by_op.items())),
            "payload_errors": self.report_payload_errors,
            "payload_ok_rate": ((self.report_frames - self.report_payload_errors) / self.report_frames
                                if self.report_frames else None),
            "channel_stride": self.channel_stride,
            "channel_stride_pref": self._stride_pref,
            "channel_count_histogram": OrderedDict((str(k), v)
                                                   for k, v in sorted(self.channel_count_hist.items())),
            "distance": OrderedDict([
                ("samples", dist_n),
                ("zero_samples", self.distance_zero_samples),
                ("min_mm", dist_min), ("median_mm", dist_med), ("max_mm", dist_max),
                ("min_cm", None if dist_min is None else round(dist_min / 10.0, 1)),
                ("median_cm", None if dist_med is None else round(dist_med / 10.0, 1)),
                ("max_cm", None if dist_max is None else round(dist_max / 10.0, 1)),
            ]),
            "level": OrderedDict([
                ("mount_mm", self.mount_mm),
                ("formula", "level_mm = mount_mm - target_distance_mm"),
                ("level_channel", self.level_channel),
                ("level_channel_missing", self.level_channel_missing),
                ("samples", lvl_n),
                ("min_mm", lvl_min), ("median_mm", lvl_med), ("max_mm", lvl_max),
                ("mean_mm", None if lvl_mean is None else round(lvl_mean, 1)),
                ("min_cm", None if lvl_min is None else round(lvl_min / 10.0, 1)),
                ("median_cm", None if lvl_med is None else round(lvl_med / 10.0, 1)),
                ("max_cm", None if lvl_max is None else round(lvl_max / 10.0, 1)),
                ("min_m", None if lvl_min is None else round(lvl_min / 1000.0, 3)),
                ("max_m", None if lvl_max is None else round(lvl_max / 1000.0, 3)),
                ("preview_tail", list(self.level_preview)),
            ]),
            "interval": OrderedDict([
                ("count", len(self.report_intervals)),
                ("median_s", None if iv_med is None else round(iv_med, 3)),
                ("p99_s", None if percentile(ivs, 0.99) is None else round(percentile(ivs, 0.99), 3)),
                ("max_s", None if not ivs else round(ivs[-1], 3)),
            ]),
        }

    def escape_view(self) -> dict:
        """转义层统计（P1 判据的全部输入，口径见 RULES_TEXT）。"""
        return OrderedDict([
            ("rule", "正文 0x0A→0x09 0x01、0x09→0x09 0x00；帧尾 0x0A 不转义"),
            ("pairs_restored", self.escape_pairs),
            ("pairs_0a_restored", self.escape_pairs_0a),
            ("pairs_09_restored", self.escape_pairs_09),
            ("frames_with_escape", self.frames_with_escape),
            ("errors", self.escape_errors),
            ("error_kinds", OrderedDict(
                (k + " " + name, self.escape_error_kinds.get(k, 0))
                for k, name in self.ESCAPE_ERROR_KINDS.items())),
            ("error_rule",
             "①非法转义序列（0x09 后既非 0x00 也非 0x01）②帧中途出现裸 0x0A "
             "③悬空 0x09（流在转义序列中途结束）—— 每发生一次记 1，故 errors ≥ 各类之和的下限"),
            ("samples", [OrderedDict([
                ("msgid", s["msgid"]), ("op", s["op"]), ("type", s["type"]),
                ("escapes", s["escapes"]),
                ("declared_len", s["declared_len"]), ("wire_len", s["wire_len"]),
                ("payload_hex", s["payload_hex"]),
            ]) for s in self.escaped_samples]),
        ])

    def resync_limit(self, span_s) -> int:
        """重同步绝对次数上限：30 分钟 3 次，按时长线性缩放，最少给 1 次。"""
        if not span_s or span_s <= 0:
            return RESYNC_LIMIT_30MIN
        return max(1, int(round(RESYNC_LIMIT_30MIN * span_s / GATE_DURATION)))

    def verdict(self, span_s) -> dict:
        """按固定阈值裁定 PASS/FAIL（P0 前提 → P1..P8 八条硬判据，见 RULES_TEXT）。"""
        bad_total = sum(self.bad.values())
        checks = []

        # ---- P0 前提 ----
        if self.frames_ok == 0:
            checks.append({
                "priority": "P0", "name": "有效帧数", "value": 0, "threshold": 1,
                "compare": ">=", "result": "FAIL",
                "note": "整段数据里没有一帧合法 EDV163 帧 → 判定无效（其余判据 SKIP）；"
                        "先排波特率是否 256000、接线是否 H3、模组是否上电",
            })
            return {
                "verdict": "FAIL", "checks": checks,
                "bad_frame_rate": None, "complete_rate": None,
                "fake_sync_loss_rate": None, "msgid_gap_rate": None,
                "bad_frames_total": bad_total, "unattributed_rate": None,
                "resync_limit": self.resync_limit(span_s),
                "report_payload_ok_rate": None,
            }
        checks.append({
            "priority": "P0", "name": "有效帧数", "value": self.frames_ok, "threshold": 1,
            "compare": ">=", "result": "PASS", "note": "",
        })

        # ---- P1 转义错误数（EDV163 新增层，最高优先级）----
        kinds_txt = " / ".join("%s %d" % (name.split("（")[0], self.escape_error_kinds.get(k, 0))
                               for k, name in self.ESCAPE_ERROR_KINDS.items())
        checks.append({
            "priority": "P1", "name": "转义错误数", "value": self.escape_errors,
            "threshold": THRESHOLDS["escape_errors_max"], "compare": "==",
            "result": "PASS" if self.escape_errors == 0 else "FAIL",
            "note": ("" if self.escape_errors == 0
                     else "转义层被破坏（%s）：长度/校验统计都建立在错误切分之上，先修转义" % kinds_txt),
            "extra": OrderedDict(self.escape_error_kinds),
        })

        # ---- P2 未归属字节率 ----
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
            u_note = ("存在 %d 字节未归属（%.4f%% < 0.1%%）：不判 FAIL，但必须在 Gate 记录里"
                      "说明来源（见 unparsed 段）" % (ws["unattributed_bytes"], u_rate * 100))
        else:
            u_result = "FAIL"
            u_note = ("存在 %d 字节未归属（%.4f%% ≥ 0.1%%）：数据流里混进了非协议字节"
                      % (ws["unattributed_bytes"], u_rate * 100))
        checks.append({
            "priority": "P2", "name": "未归属字节率", "value": u_rate,
            "threshold": THRESHOLDS["unattributed_rate_max"], "compare": "==",
            "result": u_result, "note": u_note,
            "extra": {"unattributed_bytes": ws["unattributed_bytes"],
                      "window_bytes": ws["window_bytes"], "warmup_s": ws["warmup_s"]},
        })
        if u_result == "WARN" and not self._warned_unattr:
            self._warned_unattr = True
            self.warnings.append("未归属字节率 = %.4f%%（%d 字节）：属 WARN 级，需在 Gate 记录里说明来源"
                                 % (u_rate * 100, ws["unattributed_bytes"]))

        # ---- P3 重同步绝对次数 ----
        limit = self.resync_limit(span_s)
        if self.resync_events <= limit:
            rs_result, rs_note = "PASS", ""
        else:
            rs_result = "FAIL"
            rs_note = ("%d 次 > 上限 %d 次（%s）：说明有字符在持续插入流里，不能只靠比率通过"
                       % (self.resync_events, limit,
                          "30 分钟 3 次" if span_s and abs(span_s - GATE_DURATION) < 1
                          else "按 3 次/30 分钟线性折算"))
        checks.append({
            "priority": "P3", "name": "重同步绝对次数", "value": self.resync_events,
            "threshold": limit, "compare": "<=", "result": rs_result, "note": rs_note,
        })

        # ---- P4 坏帧率 / P5 帧完整率 / 假同步估算 ----
        denom = self.frames_ok + bad_total
        bad_rate = (bad_total / denom) if denom else None
        checks.append({
            "priority": "P4", "name": "坏帧率", "value": bad_rate,
            "threshold": THRESHOLDS["bad_frame_rate_max"], "compare": "<",
            "result": ("N/A" if bad_rate is None else
                       ("PASS" if bad_rate < THRESHOLDS["bad_frame_rate_max"] else "FAIL")),
            "note": "坏帧合计 %d（%s）" % (
                bad_total, " / ".join("%s %d" % (self.BAD_KINDS[k].split("（")[0], n)
                                      for k, n in self.bad.items() if n)),
        })

        cdenom = self.frames_ok + bad_total + self.resync_events
        complete = (self.frames_ok / cdenom) if cdenom else None
        checks.append({
            "priority": "P5", "name": "帧完整率", "value": complete,
            "threshold": THRESHOLDS["complete_rate_min"], "compare": ">=",
            "result": ("N/A" if complete is None else
                       ("PASS" if complete >= THRESHOLDS["complete_rate_min"] else "FAIL")),
            "note": "",
        })

        sdenom = self.frames_ok + self.resync_events
        fake = (self.resync_events / sdenom) if sdenom else None

        # ---- P6 msgid 跳号率 ----
        mv = self.msgid_view()
        gap = mv["primary_rate"]
        checks.append({
            "priority": "P6", "name": "msgid 跳号率", "value": gap,
            "threshold": THRESHOLDS["msgid_gap_rate_max"], "compare": "<",
            "result": ("N/A" if gap is None else
                       ("PASS" if gap < THRESHOLDS["msgid_gap_rate_max"] else "FAIL")),
            "note": mv["basis"],
        })

        # ---- P7 0x81 载荷自洽率 ----
        rv = self.report_view()
        ok_rate = rv["payload_ok_rate"]
        if self.report_frames == 0:
            p7_result, p7_note = "N/A", "没有 0x81 上报帧，无法评估载荷自洽性（见 P8）"
        else:
            p7_result = "PASS" if ok_rate >= THRESHOLDS["report_payload_ok_min"] else "FAIL"
            p7_note = "" if p7_result == "PASS" else (
                "%d 帧载荷自洽性不满足（步长=%s）：说明帧被截断或长度字段被改"
                % (self.report_payload_errors, rv["channel_stride"]))
        checks.append({
            "priority": "P7", "name": "0x81 载荷自洽率", "value": ok_rate,
            "threshold": THRESHOLDS["report_payload_ok_min"], "compare": ">=",
            "result": p7_result, "note": p7_note,
            "extra": {"report_frames": self.report_frames,
                      "payload_errors": self.report_payload_errors,
                      "channel_stride": rv["channel_stride"]},
        })

        # ---- P8 0x81 上报帧数 ----
        checks.append({
            "priority": "P8", "name": "0x81 上报帧数", "value": self.report_frames,
            "threshold": THRESHOLDS["report_frames_min"], "compare": ">=",
            "result": "PASS" if self.report_frames >= THRESHOLDS["report_frames_min"] else "FAIL",
            "note": ("" if self.report_frames >= THRESHOLDS["report_frames_min"]
                     else "水位 demo 没有 0x81 就没有距离数据；检查雷达工作模式=1、上报频率(0x17)非 0"),
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
            "report_payload_ok_rate": ok_rate,
        }

    # ---------------- 汇总成报告字典 ----------------
    def collect(self, mode: str, source: str, span_s, extra_warnings=None) -> dict:
        bad_total = sum(self.bad.values())
        v = self.verdict(span_s)
        r = self.rates(span_s)
        warnings = list(self.warnings)
        if extra_warnings:
            warnings.extend(extra_warnings)

        # 观察项：帧间隔异常
        rv = self.report_view()
        iv_med = rv["interval"]["median_s"]
        iv_max = rv["interval"]["max_s"]
        if iv_med and iv_max and iv_max > 5 * iv_med:
            warnings.append("0x81 帧间隔最大 %.3f s，超过中位间隔 %.3f s 的 5 倍：疑似大面积丢帧或雷达暂停上报"
                            % (iv_max, iv_med))
        if self.report_frames == 0:
            warnings.append("整段没有 0x81 上报帧：水位数据缺失（检查工作模式/上报频率/雷达是否上电）")
        if rv["distance"]["samples"] == 0 and self.report_frames:
            warnings.append("所有 0x81 帧的 target_distance 都是 0：当前没有水面回波（空罐/超量程/安装角度问题）")
        if self.level_channel_missing:
            warnings.append("--level-channel %s：有 %d 帧里找不到该通道号，水位样本缺失（可用 --level-channel auto）"
                            % (self.level_channel, self.level_channel_missing))
        if rv["level"]["samples"] == 0 and self.report_frames:
            warnings.append("水位无有效样本：0x81 帧在，但所选通道的 distance 全为 0（无目标）或通道不存在")

        report = OrderedDict()
        report["tool"] = TOOL_NAME
        report["tool_version"] = TOOL_VERSION
        report["generated_at"] = time.strftime("%Y-%m-%d %H:%M:%S")
        report["mode"] = mode
        report["source"] = source
        report["verdict"] = v["verdict"]
        report["checks"] = v["checks"]
        report["thresholds"] = dict(THRESHOLDS)
        report["resync_limit_30min"] = RESYNC_LIMIT_30MIN
        report["protocol_baseline"] = OrderedDict([
            ("uart", "256000 / 8N1，无流控，大端"),
            ("header", "0x45 0x44"),
            ("frame_layout", "头2 | 长度2(转义前整包字节数) | msgid2 | op1 | type1 | TLV长2 | data N | 校验1 | 0x0A"),
            ("frame_len_rule", "长度 = 12 + data，最小 12，最大 256"),
            ("min_frame_len", MIN_FRAME_LEN),
            ("max_frame_len", MAX_FRAME_LEN),
            ("checksum", "转义前从帧头到 data 末逐字节求和 & 0xFF"),
            ("escape", "正文 0x0A→0x09 0x01、0x09→0x09 0x00；帧尾 0x0A 不转义"),
            ("msgid_range", "0 ~ 0x%04X，之后回绕到 0" % MSGID_MAX),
            ("report_0x81", "count(1B) + count × 7B{channel,delay_presence,target_state,"
                            "motion_direction,distance(2B 大端 mm),angle(1B 有符号)}"),
            ("level_formula", "level_mm = mount_mm - target_distance_mm"),
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
            ("escape_errors", self.escape_errors),
        ])
        report["escape"] = self.escape_view()
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
        report["report_0x81"] = rv
        report["warnings"] = warnings
        report["events"] = self.events
        return report


# ───────────────────────────── 自测 ─────────────────────────────

def build_selftest_stream():
    """构造一段带「已知缺陷」的合成字节流，并给出全部期望统计值。

    覆盖（每条都是 EDV163 特有的风险点）：
      ① 12 帧正常 0x81（含一帧正文带 0x0A/0x09 数据字节 → 必须靠转义还原才对）
      ② 一帧 SET 屏蔽区，正文密集含 0x09/0x0A，且**校验字节本身**也被迫转义
      ③ 2 通道 0x81 帧（第二通道无目标 distance=0）
      ④ msgid 跳号
      ⑤ 校验和错误帧
      ⑥ ASCII "EDK60" 日志污染（45 44 = 帧头 → 假同步 + 长度非法 + 污染特征）
      ⑦ 非法转义序列（0x09 0x05）
      ⑧ 帧中途裸 0x0A
      ⑨ 末尾截断帧
    """
    parts = []

    def add(b, good=False, fully=False):
        parts.append({"b": b, "good": good, "fully": fully})
        return b

    def ch(dist, channel=0, delay=1, state=2, motion=1, angle=0):
        return (channel, delay, state, motion, dist, angle)

    expect = Counter()
    ch_hist = Counter()
    dist_samples = []
    level_samples = []
    esc_total = 0
    frames_with_esc = 0
    by_op = Counter()
    by_type = Counter()

    def note_good(wire, op, typ, chans=None):
        nonlocal esc_total, frames_with_esc
        by_op[op] += 1
        by_type[typ] += 1
        esc = wire.count(bytes([ESC_BYTE]))
        esc_total += esc
        if esc:
            frames_with_esc += 1
        if chans is not None:
            ch_hist[len(chans)] += 1
            sel = None
            for c in chans:
                if c[4] > 0:
                    sel = c
                    break
            if sel is None and chans:
                sel = chans[0]
            for c in chans:
                if c[4] > 0:
                    dist_samples.append(c[4])
            if sel is not None and sel[4] > 0:
                level_samples.append(DEFAULT_MOUNT_MM - sel[4])

    # ── 1) warmup 段：6 帧单通道 0x81，距离稳定在 1500 mm 附近（水位 500 mm）
    for i, d in enumerate([1500, 1502, 1498, 1500, 1501, 1499]):
        chans = [ch(d)]
        w = build_frame(0x0100 + i, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
        add(w, good=True, fully=True)
        note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # ── 2) 关键用例：正文含 0x0A / 0x09 数据字节的帧（distance = 0x0A09 = 2569 mm）
    escaped_chans = [ch(0x0A09, angle=-3)]
    escaped_payload = report_payload(escaped_chans)
    w = build_frame(0x0106, 0xA3, TYPE_REPORT_DATA, escaped_payload)
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, escaped_chans)

    # ── 3) SET 屏蔽区：正文密集含 0x09/0x0A，并让校验字节恰好是 0x0A（也被迫转义）
    shield = bytes([0x01, 0x00, 0x0A, 0x09, 0x00, 0x0A, 0x00, 0x09,
                    0x0A, 0x09, 0x0A, 0x09, 0x0A])
    shield_payload = bytes([0x01]) + shield          # 1 个屏蔽区 = 1B count + 13B
    mid = msgid_hitting_checksum(0x0200, 0xA1, 0x29, shield_payload, FRAME_TAIL)
    w = build_frame(mid, 0xA1, 0x29, shield_payload)
    assert frame_checksum(mid, 0xA1, 0x29, shield_payload) == FRAME_TAIL
    add(w, good=True, fully=True)
    note_good(w, 0xA1, 0x29, None)

    # warmup 分界点：这里之后的字节落在「未归属字节」统计窗口内（run_selftest 分两次喂）
    warmup_split = sum(len(p["b"]) for p in parts)

    # ── 4) 窗口段：3 帧单通道 + 1 帧 2 通道 + 1 帧跳号
    for k, d in enumerate([1100, 1102, 1098]):
        chans = [ch(d)]
        w = build_frame(0x0107 + k, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
        add(w, good=True, fully=True)
        note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    chans = [ch(900, channel=0, delay=1, state=2, motion=1, angle=7),
             ch(0, channel=1, delay=0, state=0, motion=0, angle=0)]
    w = build_frame(0x010A, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # msgid 故意跳号：0x010A → 0x0110（0x010B~0x010F 缺失）
    chans = [ch(1200)]
    w = build_frame(0x0110, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # ── 5) 【垃圾段 A】校验和错误帧（整帧仍能被完整还原，只是校验不对）
    #      紧跟一帧正常上报来「收尾」这段垃圾 → 记 1 次重同步（与真实场景一致：
    #      噪声/坏帧之后靠下一个合法帧头重新同步）
    bad_cs = build_frame(0x0111, 0xA3, TYPE_REPORT_DATA, report_payload([ch(1200)]),
                         break_checksum=True)
    add(bad_cs, good=False, fully=True)
    esc_total += bad_cs.count(bytes([ESC_BYTE]))
    chans = [ch(1206)]
    w = build_frame(0x0111, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # ── 6) 【垃圾段 B】日志污染：ASCII "EDK60" 里的 45 44 会造出假帧头 → 长度非法 + 重同步
    logline = b"\x00\x01LOG:EDK60 bridge boot\r\n"
    add(logline)
    chans = [ch(1204)]
    w = build_frame(0x0112, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # ── 7) 【垃圾段 C】两帧转义层被破坏的帧
    #      ⑦-1 非法转义序列（0x09 0x05）
    bad_esc = build_frame(0x0113, 0xA3, TYPE_REPORT_DATA, report_payload([ch(1206)]),
                          insert_after_len=bytes([ESC_BYTE, 0x05]))
    add(bad_esc)
    #      ⑦-2 帧中途裸 0x0A
    bare_0a = build_frame(0x0114, 0xA3, TYPE_REPORT_DATA, report_payload([ch(1204)]),
                          insert_after_len=bytes([FRAME_TAIL]))
    add(bare_0a)
    #      收尾：再恢复 1 帧正常上报（证明「转义错完还能继续」）
    chans = [ch(1202)]
    w = build_frame(0x0115, 0xA3, TYPE_REPORT_DATA, report_payload(chans))
    add(w, good=True, fully=True)
    note_good(w, 0xA3, TYPE_REPORT_DATA, chans)

    # ── 8) 末尾截断帧（声明 20 字节，只给 8 字节；截断不额外计重同步）
    full = build_frame(0x0116, 0xA3, TYPE_REPORT_DATA, report_payload([ch(1300)]))
    add(full[:8])

    stream = b"".join(p["b"] for p in parts)

    # 未归属字节（窗口 = warmup_split 之后）：窗口内收字节 − 窗口内合法帧在线字节
    window_bytes = len(stream) - warmup_split
    # 精确算法：窗口内合法帧字节 = 分界点之后、good=True 的部件在线字节之和
    clean_bytes = 0
    seen = 0
    for p in parts:
        if p["good"] and seen >= warmup_split:
            clean_bytes += len(p["b"])
        seen += len(p["b"])

    dist_samples_sorted = sorted(dist_samples)
    level_sorted = sorted(level_samples)

    expect.update({
        "total_bytes": len(stream),
        "frames_ok": sum(by_op.values()),
        "by_op_A3": by_op[0xA3],
        "by_op_A1": by_op[0xA1],
        "by_type_81": by_type[TYPE_REPORT_DATA],
        "by_type_29": by_type[0x29],
        "checksum_error": 1,
        "len_invalid_min": 1,
        "tlv_len_mismatch": 0,
        "escape_error": 2,
        "escape_illegal_sequence": 1,
        "escape_bare_0a": 1,
        "tail_error": 0,
        "truncated": 1,
        "resync_events": 3,
        "resync_limit_60s": 1,
        "escape_pairs": esc_total,
        "frames_with_escape": frames_with_esc,
        "msgid_jumps_group_max": 2,
        "msgid_jumps_overall": 4,      # 0106→SET(0x02xx)、SET→0107、010A→0110、0112→0115
        "report_frames": ch_hist.total(),
        "channel_hist": dict(sorted(ch_hist.items())),
        "distance_samples": len(dist_samples),
        "distance_min": dist_samples_sorted[0] if dist_samples else None,
        "distance_max": dist_samples_sorted[-1] if dist_samples else None,
        "distance_median": median_from_list(dist_samples_sorted),
        "distance_2569_count": dist_samples.count(0x0A09),
        "level_samples": len(level_samples),
        "level_min": level_sorted[0] if level_sorted else None,
        "level_max": level_sorted[-1] if level_sorted else None,
        "level_median": median_from_list(level_sorted),
        "payload_ok_rate": 1.0,
        "warmup_split": warmup_split,
        "window_bytes": window_bytes,
        "frame_bytes_window": clean_bytes,
        "unattributed_bytes": window_bytes - clean_bytes,
        "escaped_payload_hex": hex_full(escaped_payload),
        "escaped_msgid": 0x0106,
        "pollution_marker": "EDK60",
    })
    return stream, expect


def median_from_list(sorted_vals):
    """有序列表的中位数（自测对账用）。"""
    n = len(sorted_vals)
    if n == 0:
        return None
    if n % 2 == 1:
        return float(sorted_vals[n // 2])
    return (sorted_vals[n // 2 - 1] + sorted_vals[n // 2]) / 2.0


def run_selftest(outdir_root: str = DEFAULT_OUTDIR) -> int:
    """离线自测：按期望值逐项核对；任何一项不符即返回非 0。"""
    setup_stdout()
    stream, expect = build_selftest_stream()
    outdir = os.path.join(outdir_root, "edv163_selftest_" + time.strftime("%Y%m%d-%H%M%S"))
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError:
        outdir = tempfile.mkdtemp(prefix="edv163_selftest_")
    cap_path = os.path.join(outdir, "selftest_capture.bin")
    with open(cap_path, "wb") as f:
        f.write(stream)

    print("=" * 78)
    print("EDV163 帧校验脚本 — 离线自测（--selftest）")
    print("=" * 78)
    print(f"合成流：{len(stream)} 字节 → {cap_path}")
    print(f"产物目录：{outdir}（脚本不自动删除）")
    print("")

    ck = Checker(raw_dump=True, outdir=outdir, max_unparsed=10, msgid_min_samples=5,
                 mount_mm=DEFAULT_MOUNT_MM, level_channel="auto", channel_stride="auto")
    split = expect["warmup_split"]
    ck.feed(stream[:split], now=0.0)
    ck.feed(stream[split:], now=30.0)
    ck.finish()
    report = ck.collect("selftest", "synthetic", span_s=60.0)

    m = report["metrics"]
    esc = report["escape"]
    r81 = report["report_0x81"]
    lvl = r81["level"]
    dst = r81["distance"]

    esc_sample = [s for s in esc["samples"] if s["msgid"] == expect["escaped_msgid"]]
    esc_roundtrip = bool(esc_sample) and esc_sample[0]["payload_hex"] == expect["escaped_payload_hex"]

    got = {
        "total_bytes": m["total_bytes"],
        "frames_ok": m["frames_ok"],
        "by_op_A3": report["by_op"].get("0xA3 STATUS", 0),
        "by_op_A1": report["by_op"].get("0xA1 SET", 0),
        "by_type_81": report["by_type"].get("0x81 上报数据", 0),
        "by_type_29": report["by_type"].get("0x29 屏蔽区", 0),
        "checksum_error": m["bad_frames"]["checksum_error"],
        "len_invalid": m["bad_frames"]["len_invalid"],
        "tlv_len_mismatch": m["bad_frames"]["tlv_len_mismatch"],
        "escape_error": m["bad_frames"]["escape_error"],
        "tail_error": m["bad_frames"]["tail_error"],
        "truncated": m["bad_frames"]["truncated"],
        "escape_illegal_sequence": ck.escape_error_kinds.get("illegal_sequence", 0),
        "escape_bare_0a": ck.escape_error_kinds.get("bare_0a", 0),
        "escape_pairs": esc["pairs_restored"],
        "frames_with_escape": esc["frames_with_escape"],
        "resync_events": m["resync_events"],
        "resync_limit": m["resync_limit"],
        "msgid_jumps_overall": report["msgid"]["overall"]["jumps"],
        "msgid_jumps_group_max": max([g["jumps"] for g in report["msgid"]["groups"]] or [0]),
        "report_frames": r81["frames"],
        "channel_hist": {int(k): v for k, v in r81["channel_count_histogram"].items()},
        "distance_samples": dst["samples"],
        "distance_min": dst["min_mm"],
        "distance_max": dst["max_mm"],
        "distance_median": dst["median_mm"],
        "distance_2569_count": ck.distance_hist.get(0x0A09, 0),
        "level_samples": lvl["samples"],
        "level_min": lvl["min_mm"],
        "level_max": lvl["max_mm"],
        "level_median": lvl["median_mm"],
        "level_channel_used": lvl["level_channel"],
        "payload_ok_rate": r81["payload_ok_rate"],
        "channel_stride": r81["channel_stride"],
        "window_bytes": m["unattributed"]["window_bytes"],
        "frame_bytes_window": m["unattributed"]["frame_bytes_in_window"],
        "unattributed_bytes": m["unattributed"]["unattributed_bytes"],
    }

    # (名称, 实际值, 期望值, 比较方式)
    rows = [
        ("总字节数", got["total_bytes"], expect["total_bytes"], "=="),
        ("合法帧数", got["frames_ok"], expect["frames_ok"], "=="),
        ("op=0xA3 帧数", got["by_op_A3"], expect["by_op_A3"], "=="),
        ("op=0xA1 帧数", got["by_op_A1"], expect["by_op_A1"], "=="),
        ("type=0x81 帧数", got["by_type_81"], expect["by_type_81"], "=="),
        ("type=0x29 帧数", got["by_type_29"], expect["by_type_29"], "=="),
        ("坏帧·校验错", got["checksum_error"], expect["checksum_error"], "=="),
        ("坏帧·长度非法", got["len_invalid"], expect["len_invalid_min"], ">="),
        ("坏帧·TLV长度不符", got["tlv_len_mismatch"], expect["tlv_len_mismatch"], "=="),
        ("坏帧·转义错误", got["escape_error"], expect["escape_error"], "=="),
        ("坏帧·帧尾错误", got["tail_error"], expect["tail_error"], "=="),
        ("坏帧·截断帧", got["truncated"], expect["truncated"], "=="),
        ("转义错误·非法序列", got["escape_illegal_sequence"], expect["escape_illegal_sequence"], "=="),
        ("转义错误·帧内裸0x0A", got["escape_bare_0a"], expect["escape_bare_0a"], "=="),
        ("转义对还原总数", got["escape_pairs"], expect["escape_pairs"], "=="),
        ("含转义还原的帧数", got["frames_with_escape"], expect["frames_with_escape"], "=="),
        ("重同步次数", got["resync_events"], expect["resync_events"], "=="),
        ("重同步上限(60s)", got["resync_limit"], expect["resync_limit_60s"], "=="),
        ("msgid 跳号·不分组", got["msgid_jumps_overall"], expect["msgid_jumps_overall"], "=="),
        ("msgid 跳号·分组最大组", got["msgid_jumps_group_max"], expect["msgid_jumps_group_max"], "=="),
        ("0x81 帧数", got["report_frames"], expect["report_frames"], "=="),
        ("通道数分布", got["channel_hist"], expect["channel_hist"], "=="),
        ("通道步长(锁定)", got["channel_stride"], DEFAULT_STRIDE, "=="),
        ("距离样本数", got["distance_samples"], expect["distance_samples"], "=="),
        ("距离 min (mm)", got["distance_min"], expect["distance_min"], "=="),
        ("距离 max (mm)", got["distance_max"], expect["distance_max"], "=="),
        ("距离 median (mm)", got["distance_median"], expect["distance_median"], "=="),
        ("距离=2569(0x0A09) 样本数", got["distance_2569_count"], expect["distance_2569_count"], "=="),
        ("水位样本数", got["level_samples"], expect["level_samples"], "=="),
        ("水位 min (mm)", got["level_min"], expect["level_min"], "=="),
        ("水位 max (mm)", got["level_max"], expect["level_max"], "=="),
        ("水位 median (mm)", got["level_median"], expect["level_median"], "=="),
        ("0x81 载荷自洽率", got["payload_ok_rate"], expect["payload_ok_rate"], "=="),
        ("窗口内收字节", got["window_bytes"], expect["window_bytes"], "=="),
        ("窗口内合法帧字节", got["frame_bytes_window"], expect["frame_bytes_window"], "=="),
        ("未归属字节", got["unattributed_bytes"], expect["unattributed_bytes"], "=="),
    ]

    ok_all = True
    print(f"{'检查项':<26}{'实际':>16}{'期望':>18}   结论")
    print("-" * 78)
    for name, actual, exp, op in rows:
        if op == ">=":
            ok = actual is not None and actual >= exp
            exp_s = f">= {exp}"
        else:
            ok = actual == exp
            exp_s = str(exp)
        ok_all = ok_all and ok
        print(f"{name:<26}{str(actual):>16}{exp_s:>18}   {'OK' if ok else '不符 <<<'}")
    print("-" * 78)

    # 关键专项：转义还原的「逐字节对账」——证明 0x0A/0x09 数据字节被原样还原
    print(f"转义还原逐字节对账       : {'OK ' + expect['escaped_payload_hex'] if esc_roundtrip else '不符 <<<'}")
    print(f"  （msgid=0x{expect['escaped_msgid']:04X} 的正文，含 0x0A/0x09 数据字节）")
    ok_all = ok_all and esc_roundtrip

    # --raw-dump 落盘检查 + 污染特征反查
    dumped = [s for s in report["unparsed"]["segments"] if s["dumped"]]
    print(f"unparsed 段数            : {report['unparsed']['count']}（落盘 {len(dumped)} 个）")
    marker_ok = False
    if not dumped:
        print("落盘检查                 : 不符 <<< 没有生成 unparsed_*.bin")
        ok_all = False
    else:
        blob = b""
        for s in dumped:
            with open(s["dumped"], "rb") as f:
                blob += f.read()
        marker_ok = expect["pollution_marker"].encode() in blob
        print(f"落盘内容含 '{expect['pollution_marker']}'      : "
              f"{'OK（污染反查可用）' if marker_ok else '不符 <<<'}")
        ok_all = ok_all and marker_ok

    reported = [s for s in report["unparsed"]["segments"] if s.get("pollution_markers")]
    warned = [w for w in report["warnings"] if "疑似日志/文本特征" in w]
    diag_ok = bool(reported) and bool(warned)
    print(f"报告点出污染特征         : "
          f"{'OK ' + str(reported[0]['pollution_markers']) if diag_ok else '不符 <<<'}")
    ok_all = ok_all and diag_ok

    print(f"结论判定                 : {report['verdict']}（人为造坏的数据应判 FAIL）")
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
    """把报告打成中文可读文本（logf 给了就同时写进 .log，控制台只输出一次）。"""
    lines = []

    def w(s=""):
        lines.append(s)
        if logf is None:
            print(s)
        else:
            logf.write(s + "\n")

    m = report["metrics"]
    esc = report["escape"]
    r81 = report["report_0x81"]
    dst = r81["distance"]
    lvl = r81["level"]
    mv = report["msgid"]

    w("=" * 78)
    w("EDV163 桥接帧完整性 / 水位校验报告")
    w("=" * 78)
    w(f"生成时间      : {report['generated_at']}")
    w(f"采集方式      : {report['mode']}  ({report['source']})")
    w(f"采集时长      : {m['span_s'] if m['span_s'] is not None else 'N/A'} s")
    w(f"协议基线      : 256000 8N1｜帧头 45 44｜长度=12+data(转义前)｜校验=求和&0xFF｜帧尾 0x0A｜有转义")
    w("")

    byte_rate_s = "N/A" if m["byte_rate"] is None else "%.1f B/s" % m["byte_rate"]
    frame_rate_s = "N/A" if m["frame_rate"] is None else "%.3f 帧/s" % m["frame_rate"]
    w("【流量】")
    w(f"  总字节数    : {m['total_bytes']:,}")
    w(f"  字节率      : {byte_rate_s}")
    w(f"  合法帧数    : {m['frames_ok']:,}")
    w(f"  帧率        : {frame_rate_s}")
    w("")

    w("【转义层】（EDV163 特有；P1 判据的全部输入）")
    w(f"  还原的转义对: {esc['pairs_restored']:,}"
      f"（0x0A←09 01: {esc['pairs_0a_restored']:,}；0x09←09 00: {esc['pairs_09_restored']:,}）")
    w(f"  含转义的帧  : {esc['frames_with_escape']:,}")
    w(f"  转义错误    : {esc['errors']}")
    for k, v in esc["error_kinds"].items():
        if v:
            w(f"      · {k}: {v}")
    w(f"  统计口径    : {esc['error_rule']}")
    w("")

    w("【帧分类】")
    if report["by_op"]:
        for k, n in report["by_op"].items():
            w(f"  op  {k:<12}: {n:>8,}")
    if report["by_type"]:
        for k, n in report["by_type"].items():
            w(f"  type {k:<14}: {n:>8,}")
    if not report["by_op"]:
        w("  （没有任何合法帧）")
    w("")

    w("【坏帧分类】")
    for k, name in Checker.BAD_KINDS.items():
        w(f"  {name:<30}: {m['bad_frames'][k]:>6}")
    w(f"  {'合计':<30}: {m['bad_frames']['total']:>6}"
      f"   坏帧率 {fmt_rate(m['bad_frame_rate'])}（阈值 < 0.1%）")
    w("")

    ws = m["unattributed"]
    w("【假同步 / 未归属字节】（P2/P3 判据）")
    w(f"  重同步次数  : {m['resync_events']} / 上限 {m['resync_limit']}"
      f"（3 次/30 分钟线性折算）")
    w(f"  垃圾字节    : {m['garbage_bytes']:,}")
    w(f"  统计窗口    : 从 T+{ws['warmup_s']:.0f}s 起算（窗口内收 {ws['window_bytes']:,} 字节）")
    w(f"  窗口内合法帧字节 : {ws['frame_bytes_in_window']:,}")
    w(f"  未归属字节  : {ws['unattributed_bytes']:,}    未归属率 {fmt_rate(ws['unattributed_rate'])}")
    w(f"  假同步丢帧估算率 : {fmt_rate(m['fake_sync_loss_rate'])}")
    w("")

    w("【msgid 连续性】（按 (op,type) 分组，组内样本 ≥ %d 才判定）" % mv["min_samples"])
    w(f"  判定依据    : {mv['basis']}")
    w(f"  {'op/type':<14}{'帧数':>8}{'跳号':>6}{'重复':>6}{'估算丢帧':>10}{'跳号率':>10}")
    for g in mv["groups"][:8]:
        w(f"  0x{g['op']:02X}/0x{g['type']:02X} {g['type_name'][:6]:<6}"
          f"{g['frames']:>8}{g['jumps']:>6}{g['duplicates']:>6}"
          f"{g['missing_estimate']:>10}{fmt_rate(g['jump_rate']):>12}")
    if not mv["groups"]:
        w("  （没有分组数据）")
    w("")

    w("【0x81 上报专表】（水位数据）")
    w(f"  帧数        : {r81['frames']}")
    if r81["by_op"]:
        w(f"  按 op 分布  : " + "，".join("%s×%d" % (k, v) for k, v in r81["by_op"].items()))
    w(f"  通道步长    : {r81['channel_stride']} 字节/通道（驱动实测 7；文档写 8）")
    hist = "，".join("%s 通道×%d" % (k, v) for k, v in r81["channel_count_histogram"].items())
    w(f"  通道数分布  : {hist if hist else 'N/A'}")
    w(f"  载荷异常    : {r81['payload_errors']}    载荷自洽率 {fmt_rate(r81['payload_ok_rate'])}")
    if dst["samples"]:
        w(f"  target_distance : {dst['samples']} 个样本"
          f"（无目标 {dst['zero_samples']}）")
        w(f"      min / median / max = {dst['min_mm']} / {dst['median_mm']} / {dst['max_mm']} mm"
          f"  =  {dst['min_cm']} / {dst['median_cm']} / {dst['max_cm']} cm")
    else:
        w(f"  target_distance : 无有效样本（无目标 {dst['zero_samples']}）")
    if lvl["samples"]:
        w(f"  水位 = 挂高({lvl['mount_mm']} mm) − 距离  [{lvl['level_channel']} 通道]")
        w(f"      样本 {lvl['samples']}｜min / median / mean / max = "
          f"{lvl['min_mm']} / {lvl['median_mm']} / {lvl['mean_mm']} / {lvl['max_mm']} mm")
        w(f"      换算        = {lvl['min_cm']} / {lvl['median_cm']} / {lvl['max_cm']} cm"
          f"  =  {lvl['min_m']} ~ {lvl['max_m']} m")
    else:
        w("  水位        : 无有效样本（距离全为 0 或通道缺失）")
    iv = r81["interval"]
    if iv["count"]:
        w(f"  帧间隔      : 中位 {iv['median_s']} s，P99 {iv['p99_s']} s，最大 {iv['max_s']} s")
    w("")

    w("【判定】阈值与 --help 同源（THRESHOLDS 常量）")
    w(f"  {'优先级':<8}{'判据':<18}{'实际':>14}{'阈值':>16}   结论")
    for c in report["checks"]:
        v = c["value"]
        if isinstance(v, float):
            v_s = "%.4f%%" % (v * 100) if c["compare"] in ("<", ">=", "==") and v <= 1.0 else "%.3f" % v
            if c["name"] in ("未归属字节率", "坏帧率", "帧完整率", "msgid 跳号率",
                             "0x81 载荷自洽率", "假同步丢帧估算率"):
                v_s = fmt_rate(v)
        else:
            v_s = str(v) if v is not None else "N/A"
        th = c["threshold"]
        if c["name"] == "未归属字节率":
            th_s = "= 0（<0.10% 记 WARN）"
            cmp_s = ""
        elif (isinstance(th, float) and th <= 1.0
              and c["name"] in ("坏帧率", "帧完整率", "msgid 跳号率", "0x81 载荷自洽率",
                                "假同步丢帧估算率")):
            th_s = "%.2f%%" % (th * 100)
            cmp_s = c["compare"]
        else:
            th_s = str(th)
            cmp_s = c["compare"]
        w(f"  {c['priority']:<8}{c['name']:<18}{v_s:>14}{cmp_s + ' ' + th_s:>22}   {c['result']}")
        if c.get("note"):
            w(f"          └ {c['note']}")
    w("")
    w(f"  >>> 结论：{report['verdict']}   （任一 FAIL 即 FAIL；WARN/N/A 不阻断）")
    w("")

    if report["warnings"]:
        w("【警告】（不影响 PASS/FAIL，但要在 Gate 评审里说明）")
        for x in report["warnings"]:
            w(f"  · {x}")
        w("")

    if report["unparsed"]["count"]:
        w(f"【无法解析的字节段】（前 10 段，共 {report['unparsed']['count']} 段）")
        for s in report["unparsed"]["segments"][:10]:
            w(f"  #{s['index']} 偏移 {s['offset']}  {s['size']} 字节  [{s['kind']}]"
              f"  可打印率 {s['printable_ratio']}")
            w(f"     hex: {s['hex']}")
            w(f"     ascii: {s['ascii']}")
            if s.get("pollution_markers"):
                w(f"     ⚠ 疑似污染特征: {s['pollution_markers']}")
            if s.get("dumped"):
                w(f"     → 已存盘: {s['dumped']}")
        w("")

    bad_det = report["bad_frame_detail"]
    if bad_det:
        w("【坏帧明细】（前 10 条）")
        for d in bad_det[:10]:
            w(f"  偏移 {d['offset']}  [{d['kind_name']}]  声明长度 {d['declared_len']}"
              f"（{d.get('raw_kind', 'wire')} 视角）")
            if d.get("note"):
                w(f"     {d['note']}")
            w(f"     hex: {d['hex']}")
        w("")

    art = report.get("artifacts") or {}
    if art:
        w("【产物】")
        for k in ("json", "log", "csv", "raw_capture", "unparsed_dir"):
            if art.get(k):
                w(f"  {k:<12}: {art[k]}")
    w("")


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
              r"tools\edv163_frame_check.py --port COM6", file=sys.stderr)
        print("  提示：--file 与 --selftest 模式不需要 pyserial。", file=sys.stderr)
        sys.exit(2)


def run_port(args, ck: Checker, outdir: str) -> dict:
    """串口实时采集 + 逐帧校验，时长到点自动收尾。"""
    serial = _import_serial()
    duration = args.duration if args.duration is not None else GATE_DURATION

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
                      f"  0x81 {ck.report_frames:,}  坏帧 {sum(ck.bad.values())}"
                      f"  转义错 {ck.escape_errors}  重同步 {ck.resync_events}")
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
        extra.append("实测采集时长 %.1fs 不足计划 %.0fs 的 90%%，不满足「30 分钟连续运行」要求"
                     % (span, duration))
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
    span = args.duration
    extra = []
    if span is None:
        extra.append("离线模式未提供 --duration，帧率/字节率无法计算（原始字节流本身不含时间戳）")
        ck.disable_warmup()
        extra.append("离线且无时基：未归属字节统计退化为「从流首起算」（头部开机日志会被计入）")
    else:
        ck.set_synthetic_time(span, size)
        extra.append("离线模式的『时间点』按字节位置线性插值到 --duration，仅用于对齐/画图，"
                     "不代表真实到达时刻")
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            ck.feed(chunk, now=None)
    ck.finish()
    if span is not None and span < 0.9 * GATE_DURATION:
        extra.append("提供的 --duration（%.0fs）不足 30 分钟的 90%%，不满足时长要求" % span)
    return ck.collect("file", os.path.basename(path), span, extra)


def build_argparser() -> argparse.ArgumentParser:
    epilog = RULES_TEXT + """
用法示例（本机 `python` 是应用商店别名不可用，统一用 py -3；
详见 docs\\project\\edv163\\frame-check-test-plan.md）
  # 30 分钟静置采集（水位稳定）：边测边出报告，并存原始流 + 导出水位 CSV
  py -3 tools\\edv163_frame_check.py --port COM6 --baud 256000 --duration 1800 ^
        --save-raw capture.bin --csv level.csv --raw-dump --mount-mm 2000

  # 采集完再离线复算（不接硬件，不需要 pyserial）
  py -3 tools\\edv163_frame_check.py --file capture.bin --duration 1800 --mount-mm 2000

  # 排查「日志污染」：把解析不了的字节段逐段存成 unparsed_<n>.bin
  py -3 tools\\edv163_frame_check.py --file capture.bin --duration 1800 --raw-dump

  # 无硬件自测（合成流：含转义字节的正常帧 / 坏校验 / 截断 / 噪声后重同步 / 2 通道 0x81）
  py -3 tools\\edv163_frame_check.py --selftest

  # 只知道「每通道 8 字节」的文档口径时，可以强制步长（默认 auto 按实收长度反推）
  py -3 tools\\edv163_frame_check.py --file capture.bin --channel-stride 7

产出（默认全部落在 logs\\edv163\\，可用 --out 改）
  logs\\edv163\\edv163_frame_check_<时间戳>.json   机器可读全量报告（Gate 归档用）
  logs\\edv163\\edv163_frame_check_<时间戳>.log    同内容的中文文本日志
  logs\\edv163\\unparsed_<n>.bin                   仅在 --raw-dump 时生成
  logs\\edv163\\edv163_selftest_<时间戳>\\          仅 --selftest 生成（合成流 + 报告）
  退出码：0 = PASS（或自测通过）；1 = FAIL（或自测不符）；2/3/4 = 参数或环境错误

装 pyserial（只有 --port 模式需要）
  py -3 -m pip install pyserial
  或直接用 ESP-IDF 自带 Python（里面已有 pyserial）：
  C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe tools\\edv163_frame_check.py --port COM6
"""
    p = argparse.ArgumentParser(
        prog=TOOL_NAME,
        description="EDK60 ⇄ EDV163 桥接帧完整性校验工具：转义还原 + 逐帧校验 + 坏帧/重同步/msgid/0x81 水位统计 + PASS/FAIL 裁定",
        epilog=epilog,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    src = p.add_mutually_exclusive_group()
    src.add_argument("--port", metavar="COMx", help="实时读取该串口（需要 pyserial）")
    src.add_argument("--file", metavar="capture.bin", help="离线分析已保存的原始字节流（不需要 pyserial）")
    src.add_argument("--selftest", action="store_true",
                     help="离线自测：跑一段含已知缺陷的合成流并逐项核对统计值")

    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"波特率，默认 {DEFAULT_BAUD}（EDV163 出厂值；EDV11P 才是 921600）")
    p.add_argument("--duration", type=float, default=None,
                   help="采集时长/秒，--port 模式默认 1800（30 分钟）；--file 模式给出后才能算帧率")
    p.add_argument("--warmup", type=float, default=DEFAULT_WARMUP_S,
                   help="未归属字节统计窗口的起算点/秒，默认 10（跳过上电日志）")
    p.add_argument("--mount-mm", type=int, default=DEFAULT_MOUNT_MM,
                   help=f"挂高（mm），水位 = 挂高 − 距离，默认 {DEFAULT_MOUNT_MM}（2 m）")
    p.add_argument("--level-channel", default="auto",
                   help="用哪个通道的距离算水位：auto（默认，取第一个有距离的通道）或通道号（0/1/…）")
    p.add_argument("--channel-stride", choices=STRIDE_CHOICES, default="auto",
                   help="0x81 每通道字节数：auto（默认，按实收长度反推 7 或 8）/7/8")
    p.add_argument("--save-raw", metavar="capture.bin", default=None,
                   help="--port 模式同时把原始字节流存盘，便于事后离线复算")
    p.add_argument("--csv", metavar="out.csv", default=None,
                   help="把水位时间序列导出 CSV（elapsed_s,wallclock,…,distance_mm,level_mm,…）")
    p.add_argument("--raw-dump", action="store_true",
                   help="把无法解析的字节段存成 unparsed_<n>.bin（排查日志污染的关键手段）")
    p.add_argument("--max-unparsed", type=int, default=50,
                   help="--raw-dump 最多落盘多少段，默认 50（超出只记账不落盘，防止刷爆磁盘）")
    p.add_argument("--out", "--outdir", dest="out", default=DEFAULT_OUTDIR,
                   help="报告 / 日志 / unparsed_*.bin 的输出目录，默认 %s（相对当前工作目录，"
                        "推荐在仓库根运行）；--outdir 为等价别名" % DEFAULT_OUTDIR)
    p.add_argument("--status-every", type=float, default=10.0,
                   help="--port 模式每隔多少秒打一行进度，默认 10")
    p.add_argument("--msgid-min-samples", type=int, default=20,
                   help="msgid 分组判定的最小样本帧数，默认 20（少于此数的组只报告不判定）")
    return p


def main(argv=None) -> int:
    setup_stdout()
    parser = build_argparser()
    args = parser.parse_args(argv)

    if not (args.port or args.file or args.selftest):
        parser.print_help()
        print("\n错误：必须指定 --port / --file / --selftest 之一。", file=sys.stderr)
        return 2

    if args.level_channel not in ("auto", None):
        try:
            lc = int(args.level_channel)
            if lc < 0 or lc > MAX_CHANNELS:
                raise ValueError
            args.level_channel = lc
        except (TypeError, ValueError):
            print("错误：--level-channel 只接受 auto 或 0~%d 的通道号，收到 %r"
                  % (MAX_CHANNELS, args.level_channel), file=sys.stderr)
            return 2

    if args.mount_mm < 0 or args.mount_mm > 10000:
        print("错误：--mount-mm 应在 0~10000 之间（协议 0x25 挂高范围 1200~5000 mm），收到 %d"
              % args.mount_mm, file=sys.stderr)
        return 2

    if args.selftest:
        return run_selftest(args.out)

    outdir = args.out
    try:
        os.makedirs(outdir, exist_ok=True)
    except OSError as e:
        print("错误：无法创建输出目录 %s：%s" % (outdir, e), file=sys.stderr)
        return 4
    stamp = time.strftime("%Y%m%d-%H%M%S")
    json_path = os.path.join(outdir, "edv163_frame_check_%s.json" % stamp)
    log_path = os.path.join(outdir, "edv163_frame_check_%s.log" % stamp)

    ck = Checker(raw_dump=args.raw_dump, outdir=outdir, max_unparsed=args.max_unparsed,
                 csv_path=args.csv, msgid_min_samples=args.msgid_min_samples,
                 warmup_s=args.warmup, mount_mm=args.mount_mm,
                 level_channel=args.level_channel, channel_stride=args.channel_stride)
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
