import argparse
import time

import serial


def checksum(payload: bytes) -> int:
    return (~sum(payload)) & 0xFF


def parse_payload(payload: bytes) -> dict:
    out = {}
    i = 0
    while i < len(payload):
        code = payload[i]
        i += 1
        if code >= 0x80:
            if i >= len(payload):
                break
            length = payload[i]
            i += 1
            value = payload[i:i + length]
            i += length
        else:
            value = payload[i:i + 1]
            i += 1

        if code == 0x02 and value:
            out["signal"] = value[0]
        elif code == 0x04 and value:
            out["attention"] = value[0]
        elif code == 0x05 and value:
            out["meditation"] = value[0]
        elif code == 0x16 and value:
            out["blink"] = value[0]
        elif code == 0x80 and len(value) >= 2:
            raw = int.from_bytes(value[:2], "big", signed=True)
            out["raw"] = raw
        elif code == 0x83 and len(value) >= 24:
            names = [
                "delta", "theta", "low_alpha", "high_alpha",
                "low_beta", "high_beta", "low_gamma", "high_gamma",
            ]
            for idx, name in enumerate(names):
                start = idx * 3
                out[name] = int.from_bytes(value[start:start + 3], "big")
        else:
            out[f"code_0x{code:02X}"] = value.hex()
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=30.0)
    args = parser.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    end = time.time() + args.seconds
    buf = bytearray()
    packets = 0
    eeg_packets = 0

    print(f"reading {args.port} at {args.baud} baud ...")
    while time.time() < end:
        data = ser.read(256)
        if data:
            buf.extend(data)

        while True:
            start = buf.find(b"\xAA\xAA")
            if start < 0:
                if len(buf) > 256:
                    del buf[:-2]
                break
            if start > 0:
                del buf[:start]
            if len(buf) < 4:
                break
            plen = buf[2]
            frame_len = 2 + 1 + plen + 1
            if len(buf) < frame_len:
                break

            payload = bytes(buf[3:3 + plen])
            recv_sum = buf[3 + plen]
            del buf[:frame_len]

            if checksum(payload) != recv_sum:
                continue

            packets += 1
            parsed = parse_payload(payload)
            if any(k in parsed for k in ("attention", "meditation", "signal", "delta")):
                eeg_packets += 1
                print(f"packet={packets} eeg={eeg_packets} {parsed}")
            elif packets <= 10:
                print(f"packet={packets} {parsed}")

    ser.close()
    print(f"done packets={packets} eeg_packets={eeg_packets}")


if __name__ == "__main__":
    main()
