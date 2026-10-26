#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""EDX106 门锁雷达 —— 触发事件抓取与测距统计

用途：现场测「探测距离 / 高电平宽度 / 闭锁行为」时，把 USB 文本流里的
`EDX106: t=... trig=1 ...` / `trig=0 width=...` 事件抓下来，出表 + 落 CSV。

典型用法：
    py -3 tools\\edx106_capture.py --port COM8 --duration 30
    py -3 tools\\edx106_capture.py --port COM8 --duration 30 --label "thr70 normal 1.0m"
    py -3 tools\\edx106_capture.py --file logs\\edx106\\xxx.txt

判据（每次触发）：
    - rise→fall 的宽度 = 实测高电平时间（应 ≈ delay，允许因二次触发顺延）
    - 两次 trig=1 的间隔应 ≥ delay + lockout（否则说明闭锁没生效）
"""
import argparse
import csv
import os
import re
import sys
import time

RISE_RE = re.compile(r"EDX106:\s*t=(?P<t>\d+)\s+trig=1\b")
FALL_RE = re.compile(r"EDX106:\s*t=(?P<t>\d+)\s+trig=0\s+width=(?P<w>\d+)")
MODE_RE = re.compile(r"EDX106:\s*run mode=(?P<mode>\w+).*?thr=(?P<thr>\d+).*?delay=(?P<delay>\d+).*?lockout=(?P<lockout>\d+)")
CFG_RE = re.compile(r"EDX106:\s*cfg ok\s+thr=(?P<thr>\d+)\s+delay=(?P<delay>\d+)\s+lockout=(?P<lockout>\d+)")


def parse_lines(text):
    """从文本流里抽出：触发事件、配置行"""
    events = []
    cfg = {}
    rise = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        m = MODE_RE.search(line) or CFG_RE.search(line)
        if m and not cfg:
            d = m.groupdict()
            cfg = {
                "mode": d.get("mode", "?"),
                "thr": int(d.get("thr", 0)),
                "delay_ms": int(d.get("delay", 0)),
                "lockout_ms": int(d.get("lockout", 0)),
            }
        m = RISE_RE.search(line)
        if m:
            rise = int(m.group("t"))
            events.append({"kind": "rise", "t_ms": rise})
            continue
        m = FALL_RE.search(line)
        if m:
            t = int(m.group("t"))
            w = int(m.group("w"))
            events.append({"kind": "fall", "t_ms": t, "width_ms": w, "rise_t_ms": rise})
            rise = None
    return cfg, events


def add_relative(events, t0_device_ms):
    """把设备侧 t(ms，开机起算) 转成"相对本次采集开始"的秒，便于对距离分段。"""
    for e in events:
        if t0_device_ms is None:
            e["rel_s"] = None
        else:
            e["rel_s"] = round((e["t_ms"] - t0_device_ms) / 1000.0, 2)
    return events


def summarize(cfg, events, t0_device_ms=None, segments=None):
    rises = [e for e in events if e["kind"] == "rise"]
    falls = [e for e in events if e["kind"] == "fall"]
    if t0_device_ms is None:
        for e in events:
            e["rel_s"] = None
    else:
        add_relative(events, t0_device_ms)
    widths = [e["width_ms"] for e in falls]
    gaps = []
    for a, b in zip(rises, rises[1:]):
        gaps.append(b["t_ms"] - a["t_ms"])

    print("=" * 74)
    print("EDX106 触发事件统计")
    print("=" * 74)
    if cfg:
        print(f"配置        : mode={cfg['mode']} thr={cfg['thr']} delay={cfg['delay_ms']}ms "
              f"lockout={cfg['lockout_ms']}ms")
    print(f"触发次数    : {len(rises)}")
    print(f"完整脉冲    : {len(falls)}（有上升沿记录 {len(rises)}）")
    if widths:
        print(f"高电平宽度  : min={min(widths)}ms  max={max(widths)}ms  "
              f"avg={sum(widths) / len(widths):.0f}ms")
        if cfg and cfg["delay_ms"]:
            over = [w - cfg["delay_ms"] for w in widths]
            print(f"相对 delay  : 偏差 min={min(over):+d}ms max={max(over):+d}ms"
                  f"（>0 说明二次触发/持续动作把高电平顺延了）")
    if gaps:
        print(f"触发间隔    : min={min(gaps)}ms max={max(gaps)}ms（按 delay+lockout 判断闭锁是否生效）")
        if cfg:
            floor = cfg["delay_ms"] + cfg["lockout_ms"]
            bad = [g for g in gaps if g < floor]
            print(f"闭锁判定    : 理论下限 {floor}ms，违例 {len(bad)} 次"
                  + ("" if not bad else f" → {bad}"))
    if rises:
        print("-" * 74)
        print("触发明细（rel_s = 相对本次采集开始的秒；用于对距离分段）")
        print(f"{'rel_s':>8} {'t_dev_ms':>12} {'width_ms':>9}")
        for e in falls:
            rel = e.get("rel_s")
            print(f"{'' if rel is None else f'{rel:8.2f}'} {e['t_ms']:12d} {e['width_ms']:9d}")
        if len(falls) < len(rises):
            last = rises[-1]
            rel = last.get("rel_s")
            print(f"{'' if rel is None else f'{rel:8.2f}'} {last['t_ms']:12d} {'(未回落)':>9}")
    print("-" * 74)
    if not rises:
        print("结论：本次未捕获到任何触发。可尝试 SET mode=normal / 降低 thr / 靠近模组。")
    return rises, falls


def main():
    ap = argparse.ArgumentParser(description="EDX106 门锁雷达触发抓取与统计")
    ap.add_argument("--port", help="串口，如 COM8")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--duration", type=float, default=30.0, help="采集秒数（默认 30）")
    ap.add_argument("--file", help="改为离线分析一个已保存的文本/二进制文件")
    ap.add_argument("--label", default="", help="给本次采集打个标签（写进 CSV 文件名与内容）")
    ap.add_argument("--outdir", default=os.path.join("logs", "edx106"))
    ap.add_argument("--quiet", action="store_true", help="不回显原始行")
    args = ap.parse_args()

    text = ""
    raw_bytes = b""
    if args.file:
        with open(args.file, "rb") as f:
            raw_bytes = f.read()
        text = raw_bytes.decode("utf-8", errors="replace")
        print(f"离线分析：{args.file}（{len(raw_bytes)} 字节）")
    else:
        if not args.port:
            ap.error("需要 --port 或 --file")
        try:
            import serial  # noqa: F401
        except ImportError:
            print("错误：需要 pyserial（py -3 -m pip install pyserial）", file=sys.stderr)
            return 2
        import serial
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
        print(f"采集 {args.port} @{args.baud}，{args.duration:.0f} 秒…（Ctrl+C 提前结束）")
        chunks = []
        t0 = time.time()
        t0_device_ms = None   # 设备侧首个时间戳，用于算相对秒
        try:
            while time.time() - t0 < args.duration:
                data = ser.read(4096)
                if data:
                    chunks.append(data)
                    if t0_device_ms is None:
                        m0 = re.search(rb"EDX106:\s*t=(\d+)\s+trig=", data)
                        if m0:
                            t0_device_ms = int(m0.group(1))
                    if not args.quiet:
                        sys.stdout.write(data.decode("utf-8", errors="replace"))
                        sys.stdout.flush()
        except KeyboardInterrupt:
            print("\n（提前结束）")
        finally:
            ser.close()
        raw_bytes = b"".join(chunks)
        text = raw_bytes.decode("utf-8", errors="replace")

    cfg, events = parse_lines(text)
    if not args.file:
        try:
            t0_device_ms
        except NameError:
            t0_device_ms = None
    else:
        t0_device_ms = None
        for e in events:
            if e["kind"] == "rise":
                t0_device_ms = e["t_ms"]
                break
    rises, falls = summarize(cfg, events, t0_device_ms)

    os.makedirs(args.outdir, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    log_path = os.path.join(args.outdir, f"cap_{stamp}.txt")
    with open(log_path, "wb") as f:
        f.write(raw_bytes)
    csv_path = os.path.join(args.outdir, f"cap_{stamp}.csv")
    with open(csv_path, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.writer(f)
        w.writerow(["label", "kind", "t_ms", "width_ms", "rise_t_ms",
                    "mode", "thr", "delay_ms", "lockout_ms"])
        for e in events:
            w.writerow([args.label, e["kind"], e["t_ms"], e.get("width_ms", ""),
                        e.get("rise_t_ms", ""), cfg.get("mode", ""), cfg.get("thr", ""),
                        cfg.get("delay_ms", ""), cfg.get("lockout_ms", "")])
    print(f"原始流  : {log_path}")
    print(f"事件 CSV: {csv_path}")
    return 0 if rises else 1


if __name__ == "__main__":
    sys.exit(main())
