from pathlib import Path
import time
import urllib.request


def jpeg_dimensions(data: bytes):
    i = 2
    while i < len(data) - 9:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xC0, 0xC2):
            height = (data[i + 5] << 8) + data[i + 6]
            width = (data[i + 7] << 8) + data[i + 8]
            return width, height
        if marker in (0xD8, 0xD9):
            i += 2
            continue
        length = (data[i + 2] << 8) + data[i + 3]
        i += 2 + length
    return None, None


def fetch_frame():
    url = "http://172.20.10.9:8080/stream"
    with urllib.request.urlopen(url, timeout=8) as resp:
        data = resp.read(450_000)
    start = data.find(b"\xff\xd8")
    end = data.find(b"\xff\xd9", start)
    if start < 0 or end < 0:
        raise RuntimeError("MJPEG stream did not contain a complete JPEG")
    jpg = data[start:end + 2]
    out = Path("D:/ESP32_P4nano/latest_ov5647_frame.jpg")
    out.write_bytes(jpg)
    width, height = jpeg_dimensions(jpg)
    print(f"stream_jpeg_bytes={len(jpg)} width={width} height={height} saved={out}")


def read_serial():
    try:
        import serial
    except Exception as exc:
        print(f"serial_import_error={exc}")
        return
    try:
        ser = serial.Serial("COM3", 115200, timeout=0.2)
    except Exception as exc:
        print(f"serial_open_error={exc}")
        return
    deadline = time.time() + 4
    data = bytearray()
    while time.time() < deadline:
        data.extend(ser.read(4096))
    ser.close()
    text = data.decode("utf-8", "replace")
    print("serial_tail_begin")
    print(text[-3000:])
    print("serial_tail_end")


if __name__ == "__main__":
    fetch_frame()
    read_serial()
