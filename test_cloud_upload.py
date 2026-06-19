import io
import os
import time

import requests

try:
    from PIL import Image
except ImportError:
    import sys
    from pathlib import Path

    deps = Path(__file__).resolve().parent / "pc_monitor_deps"
    if deps.exists():
        sys.path.insert(0, str(deps))
    from PIL import Image


def make_jpeg(width, height, color):
    image = Image.new("RGB", (width, height), color)
    buf = io.BytesIO()
    image.save(buf, format="JPEG", quality=85)
    return buf.getvalue()


event_id = f"SELFTEST-{int(time.time())}"
base_url = os.getenv("BODYGUARD_CLOUD_URL", "http://127.0.0.1:5000").rstrip("/")
url = f"{base_url}/v1/ai/event_upload"

files = [
    ("snapshot", ("bad_snapshot.jpg", make_jpeg(1, 1, (255, 0, 0)), "image/jpeg")),
    ("images", ("000.jpg", make_jpeg(80, 60, (32, 96, 180)), "image/jpeg")),
]
data = {
    "event_id": event_id,
    "prompt": "测试无效快照时使用连续帧兜底",
    "risk_pre": "75",
    "posture": "2",
    "timestamp": str(int(time.time() * 1000)),
}

resp = requests.post(url, data=data, files=files, timeout=40)
print(resp.status_code)
print(resp.text)
