import io
import os
import time

import requests

try:
    from PIL import Image, ImageDraw
except ImportError:
    import sys
    from pathlib import Path

    deps = Path(__file__).resolve().parent / "pc_monitor_deps"
    if deps.exists():
        sys.path.insert(0, str(deps))
    from PIL import Image, ImageDraw


def make_frame(index: int) -> bytes:
    image = Image.new("RGB", (640, 480), (28, 32, 38))
    draw = ImageDraw.Draw(image)
    draw.rectangle((80, 280, 560, 430), fill=(60, 60, 64))
    draw.ellipse((250 + index * 4, 300, 330 + index * 4, 380), fill=(210, 210, 200))
    draw.line((290 + index * 4, 380, 420, 420), fill=(220, 70, 70), width=18)
    draw.line((330 + index * 4, 380, 220, 420), fill=(220, 70, 70), width=18)
    draw.text((24, 24), "BodyGuard danger event simulation", fill=(255, 255, 255))
    draw.text((24, 54), f"risk_pre=95 posture=FALL frame={index}", fill=(255, 220, 120))
    buf = io.BytesIO()
    image.save(buf, format="JPEG", quality=90)
    return buf.getvalue()


event_id = f"DANGERTEST-{int(time.time())}"
base_url = os.getenv("BODYGUARD_CLOUD_URL", "http://127.0.0.1:5000").rstrip("/")
url = f"{base_url}/v1/ai/event_upload"

snapshot = make_frame(7)
files = [("snapshot", (f"{event_id}_snapshot.jpg", snapshot, "image/jpeg"))]
for i in range(8):
    files.append(("images", (f"{i:03d}.jpg", make_frame(i), "image/jpeg")))

data = {
    "event_id": event_id,
    "prompt": "比赛测试：模拟跌倒危险预警，请严格30字内返回综合判断和处理建议",
    "risk_pre": "95",
    "posture": "3",
    "timestamp": str(int(time.time() * 1000)),
}

resp = requests.post(url, data=data, files=files, timeout=60)
print(resp.status_code)
print(resp.text)
