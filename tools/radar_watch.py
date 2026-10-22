#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 雷达体征验收工具 —— 一直测到达标为止

达标判定（可在 --ok 里改）：
    distance > 0  且  heart > 0  且  breath > 0
    （即：雷达确实检测到人，并且心率、呼吸率都测出来了）

用法：
    python radar_watch.py                 # 默认阈值，跑到达标为止
    python radar_watch.py --max 300       # 最多跑 300 秒
    python radar_watch.py --ok distance   # 只要检测到人就算达标（快速验证）
    python radar_watch.py --port COM6

它做什么：
    · 打开板子的 USB 串口，过滤掉 ESP-IDF 日志，只解析雷达 5A 帧；
    · 每秒打印一行状态（距离/体动/心率/呼吸率/可信度）；
    · 一旦某项达到历史最好值，立刻高亮提示；
    · 达标后打印 OK 摘要并退出（退出码 0）；超时未达标退出码 1；
    · 全过程写入 radar_watch_<时间>.log，便于事后回看。

前置条件（否则永远出不了数）：
    1. 模组插在 H3，丝印 RADAR 红灯亮（= 雷达已上电）；
    2. 板子跑的是 radar_bridge（固件会自动发 turn_on 打开射频）；
    3. **有人坐在雷达正前方 30~150cm 内、接近静止**，并保持 30~60 秒。
"""

import argparse
import io
import os
import sys
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺少 pyserial，请用 ESP-IDF 的 Python 运行本脚本。")

# 让中文在 GBK 控制台也能输出
if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    try:
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    except Exception:
        pass

MAGIC = 0x5A
FIELDS = ["睡眠状态", "距离", "体动", "心率", "呼吸率",
          "心率状态", "呼吸状态", "心率可信度", "呼吸可信度"]
# 字段下标
IDX_DIST, IDX_MOTION, IDX_HR, IDX_BR = 1, 2, 3, 4


def xor_of(b: bytes) -> int:
    v = 0
    for x in b:
        v ^= x
    return v


def find_port(preferred=None):
    ports = list(list_ports.comports())
    if preferred:
        return preferred
    cand = [p for p in ports if p.vid == 0x303A]
    pool = cand if cand else ports
    if not pool:
        sys.exit("没有找到任何串口，请确认板子已插 USB。")
    if len(pool) > 1:
        print("检测到多个串口，请用 --port 指定：")
        for p in pool:
            print(f"  {p.device:8s} {p.description}")
        sys.exit(1)
    return pool[0].device


def parse_vital(payload: bytes):
    """把 vital_signs 载荷解析成 9 个整数；不是体征帧返回 None。"""
    try:
        s = payload.decode("utf-8")
    except Exception:
        return None
    if not s.startswith("vital_signs"):
        return None
    nums = s[len("vital_signs"):].strip().split(",")
    if len(nums) != len(FIELDS):
        return None
    try:
        return [int(n) for n in nums]
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser(description="EDK60 雷达体征验收工具（测到达标为止）")
    ap.add_argument("--port", help="串口号，缺省自动检测")
    ap.add_argument("--max", type=float, default=600, help="最长运行秒数（默认 600）")
    ap.add_argument("--ok", default="all",
                    choices=["all", "distance", "heart", "breath"],
                    help="达标条件：all=距离+心率+呼吸都>0；distance=只要检测到人")
    args = ap.parse_args()

    port = find_port(args.port)
    logname = f"radar_watch_{datetime.now():%Y%m%d_%H%M%S}.log"
    logf = open(logname, "w", encoding="utf-8")
    print(f"端口 {port}   日志 {logname}")
    print("=" * 76)
    print("请坐到雷达正前方 30~150cm，保持接近静止，等待心率/呼吸率出现（30~60 秒）")
    print("=" * 76)

    ser = serial.Serial(port, 115200, timeout=0.1)
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass

    buf = bytearray()
    t0 = time.time()
    last_line = t0
    frames = 0
    bad = 0
    vital_count = 0
    best = {"distance": 0, "motion": 0, "heart": 0, "breath": 0}
    latest = None
    announced = set()
    ok_result = None

    def log(msg):
        logf.write(f"{time.time() - t0:8.2f}s  {msg}\n")
        logf.flush()

    try:
        while True:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)

            # 解析帧
            while True:
                i = buf.find(MAGIC)
                if i < 0:
                    buf.clear()
                    break
                if i > 0:
                    del buf[:i]
                if len(buf) < 2:
                    break
                L = buf[1]
                if L < 4 or L > 512:
                    del buf[0]
                    bad += 1
                    continue
                if len(buf) < L:
                    break
                frame = bytes(buf[:L])
                del buf[:L]
                frames += 1
                if xor_of(frame[:-1]) != frame[-1]:
                    bad += 1
                    continue
                vals = parse_vital(frame[3:-1])
                if vals is None:
                    continue
                vital_count += 1
                latest = vals

                # 记录历史最好值并高亮
                for key, idx in (("distance", IDX_DIST), ("motion", IDX_MOTION),
                                 ("heart", IDX_HR), ("breath", IDX_BR)):
                    if vals[idx] > best[key]:
                        best[key] = vals[idx]
                        log(f"新最好值 {key}={vals[idx]}  完整帧={vals}")

                # 达标判定
                cond = False
                if args.ok == "distance":
                    cond = vals[IDX_DIST] > 0
                elif args.ok == "heart":
                    cond = vals[IDX_HR] > 0
                elif args.ok == "breath":
                    cond = vals[IDX_BR] > 0
                else:
                    cond = (vals[IDX_DIST] > 0 and vals[IDX_HR] > 0 and vals[IDX_BR] > 0)
                if cond:
                    ok_result = list(vals)
                    break

            if ok_result:
                break

            now = time.time()
            if now - last_line >= 1.0:
                last_line = now
                el = now - t0
                if latest:
                    d, m, hr, br = latest[IDX_DIST], latest[IDX_MOTION], latest[IDX_HR], latest[IDX_BR]
                    hint = ""
                    if d == 0:
                        hint = "  ← 没检测到人，请坐近一点/正对雷达"
                    elif hr == 0 or br == 0:
                        hint = "  ← 已检测到人，保持静止等心率/呼吸率"
                    else:
                        hint = "  ← 已出数"
                    print(f"[{el:5.1f}s] 距离={d:3d}cm  体动={m:4d}  "
                          f"心率={hr:3d}bpm  呼吸={br:3d}bpm{hint}")
                else:
                    print(f"[{el:5.1f}s] 尚未收到体征帧（已收 {frames} 帧）"
                          f"{'  ← 检查模组是否插 H3、红灯是否亮' if el > 5 else ''}")

            if args.max and (time.time() - t0) >= args.max:
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        logf.close()

    print("=" * 76)
    print(f"共收到 {frames} 帧（体征帧 {vital_count}，校验失败 {bad}）")
    print(f"历史最好值：距离={best['distance']}cm  体动={best['motion']}  "
          f"心率={best['heart']}bpm  呼吸={best['breath']}bpm")
    if ok_result:
        print("✅ 达标！最终体征帧：")
        for name, v in zip(FIELDS, ok_result):
            print(f"   {name:10s} = {v}")
        print("=" * 76)
        sys.exit(0)
    else:
        print("❌ 未达标。按下面顺序排查：")
        if best["distance"] == 0:
            print("   · 雷达没检测到人：坐到 30~150cm 内、正对雷达、保持静止")
            print("   · 确认模组插在 H3、丝印 RADAR 红灯亮")
            print("   · 确认固件是 radar_bridge（会自动发 turn_on 开射频）")
        else:
            print("   · 已检测到人（距离>0）但心率/呼吸未出：再多等 30~60 秒，")
            print("     并尽量不动（体动越大越难算），必要时调大增益/换 boost 模式")
        print(f"   · 详细日志见 {logname}")
        print("=" * 76)
        sys.exit(1)


if __name__ == "__main__":
    main()
