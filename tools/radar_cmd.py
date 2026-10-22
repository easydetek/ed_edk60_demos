#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EDV151 雷达命令工具 —— 通过 EDK60 板子与雷达模组对话

原理：
    PC --USB--> ESP32-C6 --UART(IO5)--> 雷达模组
    板子跑 radar_bridge 时是**双向透传**，所以从 PC 发的字节会原样送到模组，
    模组的回包也会原样回到 PC。于是本脚本可以直接对模组下命令、读应答。

帧格式（EDV151 协议）：
    +------+--------+--------+---------------+--------+
    | 0x5A |  LEN   |   ID   |    PAYLOAD    |  XOR   |
    +------+--------+--------+---------------+--------+
    LEN = 整帧字节数；ID 0x00~0x7F 雷达发，0x80~0xFF 主控发；XOR 为前面各字节异或

用法：
    python radar_cmd.py get_model get_version get_gain get_boost get_band
    python radar_cmd.py "get_para 4,5,6,7,8,9,10,11"
    python radar_cmd.py "set_gain 0.5" "set_para 4:1"
    python radar_cmd.py --listen 10            # 只监听 10 秒，打印全部帧

注意：
    模组回包与雷达主动上报共用同一条 USB 通路，本脚本会按 ID 匹配应答。
"""

import argparse
import io
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("缺少 pyserial，请用 ESP-IDF 的 Python 运行。")

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    try:
        sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    except Exception:
        pass

MAGIC = 0x5A


def xor_of(b):
    v = 0
    for x in b:
        v ^= x
    return v


def build(msg_id, payload: bytes):
    body = bytes([MAGIC, 3 + len(payload) + 1, msg_id]) + payload
    return body + bytes([xor_of(body)])


def read_frames(ser, buf, timeout=0.8):
    """从串口读一段，返回解析出的合法帧列表。"""
    out = []
    t_end = time.time() + timeout
    while time.time() < t_end:
        chunk = ser.read(512)
        if chunk:
            buf.extend(chunk)
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
                continue
            if len(buf) < L:
                break
            f = bytes(buf[:L])
            del buf[:L]
            if xor_of(f[:-1]) == f[-1]:
                out.append(f)
        time.sleep(0.02)
    return out


def main():
    ap = argparse.ArgumentParser(description="EDV151 雷达命令工具（经 EDK60 板子透传）")
    ap.add_argument("--port", default="COM6", help="板子的 USB 串口（默认 COM6）")
    ap.add_argument("--listen", type=float, default=0,
                    help="仅监听 N 秒并打印所有帧，不发命令")
    ap.add_argument("commands", nargs="*", help="要发送的 ASCII 命令，如 get_gain")
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    try:
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception:
        pass
    time.sleep(0.2)
    ser.reset_input_buffer()
    buf = bytearray()

    # 只监听模式
    if args.listen:
        print(f"监听 {args.listen} 秒……")
        t0 = time.time()
        seen = {}
        while time.time() - t0 < args.listen:
            for f in read_frames(ser, buf, timeout=0.3):
                mid, payload = f[2], f[3:-1]
                try:
                    txt = payload.decode("utf-8")
                except Exception:
                    txt = payload.hex(" ")
                key = (mid, txt)
                seen[key] = seen.get(key, 0) + 1
                if seen[key] == 1:
                    print(f"  ID=0x{mid:02X} LEN={f[1]:3d}  {txt}")
        ser.close()
        print("\n=== 汇总 ===")
        for (mid, txt), c in sorted(seen.items(), key=lambda x: -x[1]):
            print(f"  ID=0x{mid:02X}  x{c:4d}  {txt}")
        return

    if not args.commands:
        ap.print_help()
        ser.close()
        return

    msg_id = 0x81
    for cmd in args.commands:
        payload = cmd.encode("utf-8")
        frame = build(msg_id, payload)
        ser.write(frame)
        ser.flush()
        print(f">>> [{msg_id:#04x}] {cmd}")
        frames = read_frames(ser, buf, timeout=2.0)
        matched = [f for f in frames if f[2] == msg_id]
        others = [f for f in frames if f[2] != msg_id and f[2] >= 0x80]
        if matched:
            for f in matched:
                try:
                    txt = f[3:-1].decode("utf-8")
                except Exception:
                    txt = f[3:-1].hex(" ")
                print(f"<<< ID=0x{f[2]:02X}  {txt}")
        else:
            print("<<< 无匹配应答（可能命令不合法，或模组未上电/未接 H3）")
        for f in others:
            try:
                txt = f[3:-1].decode("utf-8")
            except Exception:
                txt = f[3:-1].hex(" ")
            print(f"    （其它应答 ID=0x{f[2]:02X}: {txt}）")
        msg_id += 1
        if msg_id > 0xFF:
            msg_id = 0x81
        time.sleep(0.15)

    ser.close()


if __name__ == "__main__":
    main()
