#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDK60 雷达透传 —— PC 侧上位机测试工具

用途：
    板子跑 demo 5.1 (radar_bridge) 时，雷达原始字节经 ESP32-C6 的原生 USB 串口
    送到 PC。本脚本打开该串口，把「雷达帧」和「ESP-IDF 日志」分开显示，
    并校验帧格式、统计帧率，用来确认整条链路是否正常。

链路：
    雷达模组(EDV151) ──UART(IO4,921600)──► ESP32-C6 ──USB──► PC

为什么需要这个脚本（而不是随便开个串口助手）：
    因为本工程的「控制台第二输出口」也接到了 USB（关掉会让系统启动卡死，
    详见 sdkconfig.defaults.radar_bridge 的说明），所以 COM 口上同时有
    ①ESP-IDF 日志（形如 "I (1234) tag: ..."）②雷达二进制帧。
    直接用串口助手会看到两者混在一起。本脚本自动把日志行挑出来单列，
    再按 5A 帧头解析雷达数据。

帧格式（2026-10-15 实测反解）：
    +------+--------+--------+---------------+--------+
    | 0x5A |  LEN   |  TYPE  |    PAYLOAD    |  XOR   |
    +------+--------+--------+---------------+--------+
    LEN   = 整帧字节数（含帧头与校验）
    TYPE  = 0x00 / 0x01 ...（消息类型）
    XOR   = 前面所有字节依次异或

用法：
    python radar_monitor.py                 # 自动找端口（优先 COM6）
    python radar_monitor.py --port COM6
    python radar_monitor.py --raw           # 只显示原始十六进制，不解析
    python radar_monitor.py --quiet-log     # 不显示 ESP-IDF 日志
    python radar_monitor.py --seconds 10    # 跑 10 秒后自动退出

依赖：pyserial（ESP-IDF 的 Python 环境里已有）
    C:\\Espressif\\tools\\python\\v6.1\\venv\\Scripts\\python.exe radar_monitor.py
"""

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("缺少 pyserial。请用 ESP-IDF 的 Python 运行本脚本，例如：\n"
             r"  C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe radar_monitor.py")

MAGIC = 0x5A



VITAL_NAMES = ["睡眠状态", "距离(cm)", "体动", "心率(bpm)", "呼吸率(bpm)",
               "心率状态", "呼吸状态", "心率可信度", "呼吸可信度"]


def decode_vital(payload: bytes):
    """把 vital_signs 载荷转成中文可读行；失败返回 None。"""
    try:
        s = payload.decode("utf-8")
    except Exception:
        return None
    if not s.startswith("vital_signs"):
        return None
    nums = s[len("vital_signs"):].strip().split(",")
    if len(nums) != len(VITAL_NAMES):
        return f"vital_signs 字段数异常({len(nums)}): {s}"
    parts = []
    for name, v in zip(VITAL_NAMES, nums):
        v = v.strip()
        # 心率/呼吸率为 0 表示未识别
        if name in ("心率(bpm)", "呼吸率(bpm)") and v == "0":
            v = "0(未识别)"
        parts.append(f"{name}={v}")
    return "呼吸心跳 | " + "  ".join(parts)

def xor_of(data: bytes) -> int:
    v = 0
    for b in data:
        v ^= b
    return v


def find_port(preferred=None):
    """优先用指定端口；否则找 Espressif(303A) 的 USB 串口。"""
    ports = list(list_ports.comports())
    if preferred:
        for p in ports:
            if p.device.upper() == preferred.upper():
                return p.device
        return preferred          # 端口存在但 pyserial 没枚举到，直接试
    cand = [p for p in ports if (p.vid == 0x303A)]
    if len(cand) == 1:
        return cand[0].device
    if not cand:
        cand = ports
    if not cand:
        sys.exit("没有找到任何串口。请确认板子已插上 USB。")
    if len(cand) > 1:
        print("检测到多个串口，请用 --port 指定其一：")
        for p in cand:
            print(f"  {p.device:8s} {p.description}")
        sys.exit(1)
    return cand[0].device


def main():
    ap = argparse.ArgumentParser(description="EDK60 雷达透传上位机测试工具")
    ap.add_argument("--port", help="串口号，如 COM6；缺省自动检测")
    ap.add_argument("--baud", type=int, default=115200,
                    help="串口波特率。注意：USB CDC 不受它影响，随便填（默认 115200）")
    ap.add_argument("--raw", action="store_true", help="只打印原始十六进制，不解析帧")
    ap.add_argument("--quiet-log", action="store_true", help="不显示 ESP-IDF 日志")
    ap.add_argument("--seconds", type=float, default=0, help="运行多少秒后退出（0=一直跑）")
    args = ap.parse_args()

    port = find_port(args.port)
    print(f"打开 {port} @ {args.baud}（USB CDC 实际不受波特率限制）")
    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except Exception as e:
        sys.exit(f"打开 {port} 失败：{e}")

    # 不要动 DTR/RTS：这两个信号在 ESP32 上会触发复位，一动板子就重启
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass

    print("=" * 78)
    print("等待雷达帧……  Ctrl+C 结束")
    print("=" * 78)

    buf = bytearray()
    frames = 0
    bad = 0
    by_type = {}
    t0 = time.time()
    last_report = t0
    pending_log = bytearray()

    def dump_log(chunk: bytes):
        """把 ESP-IDF 日志行挑出来打印（以 \\r\\n 分行）。"""
        pending_log.extend(chunk)

    try:
        while True:
            data = ser.read(4096)
            if data:
                if args.raw:
                    print(" ".join(f"{b:02X}" for b in data))
                else:
                    buf.extend(data)

            # 处理日志行
            if not args.raw and not args.quiet_log and pending_log:
                while b"\n" in pending_log:
                    line, _, rest = pending_log.partition(b"\n")
                    pending_log = bytearray(rest)
                    s = line.decode("utf-8", errors="replace").strip()
                    if s:
                        print(f"  [日志] {s}")

            # 解析帧
            if not args.raw:
                while True:
                    i = buf.find(MAGIC)
                    if i < 0:
                        if buf:
                            buf.clear()
                        break
                    if i > 0:
                        dumped = bytes(buf[:i])
                        if not args.quiet_log:
                            s = dumped.decode("utf-8", errors="replace").strip()
                            if s:
                                print(f"  [日志] {s}")
                        del buf[:i]
                    if len(buf) < 2:
                        break
                    length = buf[1]
                    if length < 4 or length > 512:
                        del buf[0]
                        bad += 1
                        continue
                    if len(buf) < length:
                        break
                    frame = bytes(buf[:length])
                    del buf[:length]

                    calc = xor_of(frame[:-1])
                    ok = (calc == frame[-1])
                    ftype = frame[2]
                    payload = frame[3:-1]
                    frames += 1
                    by_type[ftype] = by_type.get(ftype, 0) + 1
                    if not ok:
                        bad += 1

                    try:
                        txt = payload.decode("utf-8")
                        shown = txt if txt.isprintable() else payload.hex(" ")
                    except Exception:
                        shown = payload.hex(" ")

                    flag = "OK " if ok else "BAD"
                    vital = decode_vital(payload)
                    if vital:
                        print(f"  [{flag}] {vital}")
                    else:
                        print(f"  [{flag}] TYPE=0x{ftype:02X} LEN={length:3d}  {shown}")

            now = time.time()
            if now - last_report >= 5.0:
                dt = now - t0
                print(f"  ---- {dt:6.1f}s  帧数 {frames}  解析失败 {bad}  "
                      f"平均 {frames/dt:5.2f} 帧/秒  类型分布 "
                      f"{ {hex(k): v for k, v in by_type.items()} } ----")
                last_report = now

            if args.seconds and (time.time() - t0) >= args.seconds:
                break
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        total = time.time() - t0
        print("=" * 78)
        print(f"结束：{total:.1f}s  收到 {frames} 帧  解析失败 {bad}  "
              f"平均 {frames / total if total else 0:.2f} 帧/秒")
        if frames == 0:
            print("没有收到任何雷达帧。请按顺序检查：")
            print("  1) 模组是否插在 H3（不是 H4），且红灯(RADAR)亮 = 雷达已上电")
            print("  2) 板子是否在跑 radar_bridge（menuconfig 选 5.1 后烧录）")
            print("  3) 用 --raw 看是否有原始字节进来（区分「没数据」和「解析不了」）")
        print("=" * 78)


if __name__ == "__main__":
    main()
