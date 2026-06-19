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
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--brainlink-port", default="COM5")
    ap.add_argument("--brainlink-baud", type=int, default=115200)
    ap.add_argument("--p4-port", default="COM3")
    ap.add_argument("--p4-baud", type=int, default=115200)
    args = ap.parse_args()

    brain = serial.Serial(args.brainlink_port, args.brainlink_baud, timeout=0.1)
    p4 = serial.Serial(args.p4_port, args.p4_baud, timeout=0.1)

    buf = bytearray()
    last_sent = 0.0
    latest = {"attention": 0, "meditation": 0, "signal": 200}

    print(f"bridge started: {args.brainlink_port} -> {args.p4_port}")
    try:
        while True:
            data = brain.read(256)
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

                parsed = parse_payload(payload)
                if "attention" in parsed:
                    latest["attention"] = parsed["attention"]
                if "meditation" in parsed:
                    latest["meditation"] = parsed["meditation"]
                if "signal" in parsed:
                    latest["signal"] = parsed["signal"]

            now = time.time()
            if now - last_sent >= 1.0:
                line = f"EEG,{latest['attention']},{latest['meditation']},{latest['signal']}\n"
                p4.write(line.encode("ascii"))
                p4.flush()
                print(line.strip())
                last_sent = now
    finally:
        brain.close()
        p4.close()


if __name__ == "__main__":
    main()
