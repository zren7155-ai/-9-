import argparse
import base64
import io
import json
import queue
import sys
import threading
import time
import urllib.request
from collections import deque
from pathlib import Path

LOCAL_DEPS = Path(__file__).resolve().with_name("pc_monitor_deps")
if LOCAL_DEPS.exists():
    sys.path.insert(0, str(LOCAL_DEPS))

try:
    import serial
    import matplotlib
    import matplotlib.animation as animation
    import matplotlib.pyplot as plt
    from PIL import Image
except ImportError as exc:
    raise SystemExit(
        "Missing PC dependencies. Run:\n"
        "d:\\Espressif\\python_env\\idf5.5_py3.13_env\\Scripts\\python.exe "
        "-m pip install pyserial matplotlib pillow\n"
        f"Original error: {exc}"
    )


def serial_reader(port: str, baud: int, out_queue: queue.Queue) -> None:
    try:
        ser = serial.Serial(port, baud, timeout=0.2)
        ser.setDTR(False)
        ser.setRTS(False)
    except Exception as exc:
        out_queue.put(("err", f"serial open failed: {exc}"))
        return

    time.sleep(0.2)
    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="ignore").strip()
        if line.startswith("BGDATA,"):
            try:
                out_queue.put(("data", json.loads(line[7:])))
            except json.JSONDecodeError:
                pass
        elif line.startswith("BGCAM,"):
            try:
                payload = json.loads(line[6:])
                jpg = base64.b64decode(payload["jpeg_b64"])
                img = Image.open(io.BytesIO(jpg)).convert("RGB")
                out_queue.put(("cam", img))
            except Exception as exc:
                out_queue.put(("err", f"camera decode failed: {exc}"))
        elif line.startswith("BGERR,"):
            out_queue.put(("err", line))


def mjpeg_reader(url: str, out_queue: queue.Queue) -> None:
    boundary = None
    while True:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "BodyGuard-PC-Monitor/1.0"})
            with urllib.request.urlopen(req, timeout=8) as resp:
                content_type = resp.headers.get("Content-Type", "")
                marker = "boundary="
                if marker in content_type:
                    boundary = content_type.split(marker, 1)[1].strip().strip('"')
                    boundary = boundary.encode("ascii", errors="ignore")
                    if not boundary.startswith(b"--"):
                        boundary = b"--" + boundary
                if not boundary:
                    boundary = b"--bodyguardframe"

                buf = b""
                while True:
                    chunk = resp.read(4096)
                    if not chunk:
                        raise ConnectionError("mjpeg stream closed")
                    buf += chunk

                    while True:
                        header_end = buf.find(b"\r\n\r\n")
                        if header_end < 0:
                            break
                        header = buf[:header_end].decode("latin1", errors="ignore")
                        content_length = None
                        for line in header.splitlines():
                            if line.lower().startswith("content-length:"):
                                content_length = int(line.split(":", 1)[1].strip())
                                break
                        if content_length is None:
                            next_boundary = buf.find(boundary, header_end + 4)
                            if next_boundary < 0:
                                break
                            jpg = buf[header_end + 4:next_boundary].strip()
                            buf = buf[next_boundary:]
                        else:
                            frame_start = header_end + 4
                            frame_end = frame_start + content_length
                            if len(buf) < frame_end:
                                break
                            jpg = buf[frame_start:frame_end]
                            buf = buf[frame_end:]

                        if jpg:
                            img = Image.open(io.BytesIO(jpg)).convert("RGB")
                            out_queue.put(("cam", img))
        except Exception as exc:
            out_queue.put(("err", f"mjpeg stream error: {exc}"))
            time.sleep(2)


def main() -> None:
    parser = argparse.ArgumentParser(description="BodyGuard ESP32-P4 realtime monitor")
    parser.add_argument("--port", default="COM3", help="P4 USB serial port")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--seconds", type=int, default=30, help="Curve time window")
    parser.add_argument("--stream-url", default="", help="Optional P4 MJPEG URL, for example http://192.168.1.23:8080/stream")
    args = parser.parse_args()

    q: queue.Queue = queue.Queue(maxsize=300)
    t = threading.Thread(target=serial_reader, args=(args.port, args.baud, q), daemon=True)
    t.start()
    if args.stream_url:
        vt = threading.Thread(target=mjpeg_reader, args=(args.stream_url, q), daemon=True)
        vt.start()

    matplotlib.rcParams["font.sans-serif"] = ["Microsoft YaHei", "SimHei", "Arial Unicode MS", "DejaVu Sans"]
    matplotlib.rcParams["axes.unicode_minus"] = False

    max_points = max(100, args.seconds * 20)
    ts = deque(maxlen=max_points)
    attention = deque(maxlen=max_points)
    meditation = deque(maxlen=max_points)
    fatigue = deque(maxlen=max_points)
    signal = deque(maxlen=max_points)
    gx = deque(maxlen=max_points)
    gy = deque(maxlen=max_points)
    gz = deque(maxlen=max_points)
    angle = deque(maxlen=max_points)
    risk = deque(maxlen=max_points)

    latest_img = None
    latest_text = "Waiting for ESP32-P4 data..."
    cam_title = f"OV5647 live preview ({args.stream_url or 'serial BGCAM'})"
    err_text = ""
    start_ts = None

    fig = plt.figure(figsize=(13, 8))
    fig.canvas.manager.set_window_title("BodyGuard realtime monitor")
    grid = fig.add_gridspec(2, 2)
    ax_eeg = fig.add_subplot(grid[0, 0])
    ax_mpu = fig.add_subplot(grid[1, 0])
    ax_cam = fig.add_subplot(grid[:, 1])

    def animate(_):
        nonlocal latest_img, latest_text, err_text, start_ts
        drained = 0
        while drained < 100:
            try:
                kind, payload = q.get_nowait()
            except queue.Empty:
                break
            drained += 1

            if kind == "data":
                now = payload.get("ts", 0) / 1000.0
                if start_ts is None:
                    start_ts = now
                x = now - start_ts
                eeg = payload.get("eeg", {})
                mpu = payload.get("mpu", {})
                r = payload.get("risk", {})

                ts.append(x)
                attention.append(eeg.get("attention", 0))
                meditation.append(eeg.get("meditation", 0))
                fatigue.append(eeg.get("fatigue", 0))
                signal.append(eeg.get("signal", 0))
                gx.append(mpu.get("gx", 0))
                gy.append(mpu.get("gy", 0))
                gz.append(mpu.get("gz", 0))
                angle.append(mpu.get("angle", 0))
                risk.append(r.get("pre", 0))

                latest_text = (
                    f"EEG connected={eeg.get('connected', 0)} "
                    f"attention={eeg.get('attention', 0)} meditation={eeg.get('meditation', 0)} "
                    f"fatigue={eeg.get('fatigue', 0)} signal={eeg.get('signal', 0)}\n"
                    f"MPU angle={mpu.get('angle', 0):.1f} posture={mpu.get('posture', 0)} "
                    f"accel=({mpu.get('ax', 0):.2f},{mpu.get('ay', 0):.2f},{mpu.get('az', 0):.2f}) "
                    f"gyro=({mpu.get('gx', 0):.1f},{mpu.get('gy', 0):.1f},{mpu.get('gz', 0):.1f})\n"
                    f"risk_pre={r.get('pre', 0)} risk_final={r.get('final', 0)} "
                    f"sys_state={r.get('sys_state', 0)}"
                )
            elif kind == "cam":
                latest_img = payload
            elif kind == "err":
                err_text = str(payload)

        ax_eeg.clear()
        ax_eeg.set_title("BrainLink EEG trend")
        ax_eeg.set_ylim(-5, 105)
        ax_eeg.grid(True, alpha=0.25)
        if ts:
            ax_eeg.plot(ts, attention, label="attention")
            ax_eeg.plot(ts, meditation, label="meditation")
            ax_eeg.plot(ts, fatigue, label="fatigue")
            ax_eeg.plot(ts, signal, label="signal")
        ax_eeg.legend(loc="upper left")

        ax_mpu.clear()
        ax_mpu.set_title("MPU6050 pose / risk")
        ax_mpu.grid(True, alpha=0.25)
        if ts:
            ax_mpu.plot(ts, angle, label="tilt angle")
            ax_mpu.plot(ts, risk, label="risk_pre")
            ax_mpu.plot(ts, gx, label="gyro_x", alpha=0.55)
            ax_mpu.plot(ts, gy, label="gyro_y", alpha=0.55)
            ax_mpu.plot(ts, gz, label="gyro_z", alpha=0.55)
        ax_mpu.legend(loc="upper left")

        ax_cam.clear()
        ax_cam.set_title(cam_title)
        ax_cam.axis("off")
        if latest_img is not None:
            ax_cam.imshow(latest_img)
        ax_cam.text(
            0.01,
            0.01,
            latest_text + (f"\n{err_text}" if err_text else ""),
            transform=ax_cam.transAxes,
            color="white" if latest_img is not None else "black",
            fontsize=9,
            va="bottom",
            bbox={
                "facecolor": "black" if latest_img is not None else "white",
                "alpha": 0.65,
                "pad": 6,
            },
        )

    # 必须保存动画对象引用，否则 matplotlib 会在 plt.show() 前后回收动画，
    # 表现为窗口一闪而过或画面不刷新。
    anim = animation.FuncAnimation(fig, animate, interval=100, cache_frame_data=False)
    fig._bodyguard_animation = anim
    plt.tight_layout()
    print(f"BodyGuard realtime monitor started on {args.port} @ {args.baud}. Close the chart window to exit.")
    plt.show()


if __name__ == "__main__":
    main()
