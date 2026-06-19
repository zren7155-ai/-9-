import sys
import os
from pathlib import Path

LOCAL_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(LOCAL_ROOT / "pydeps"))

import paramiko

host = os.getenv("BODYGUARD_DEPLOY_HOST", "")
key_path = Path(os.getenv("BODYGUARD_DEPLOY_KEY", str(LOCAL_ROOT / "deploy_key.pem")))
if not host:
    raise SystemExit("Set BODYGUARD_DEPLOY_HOST before connecting.")
if not key_path.exists():
    raise SystemExit(f"SSH key not found: {key_path}")

key = paramiko.RSAKey.from_private_key_file(str(key_path))
client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect(host, username=os.getenv("BODYGUARD_DEPLOY_USER", "admin"), pkey=key, timeout=10)

cmd = r"""
python3 - <<'PY'
from pathlib import Path
import json, struct

base = Path('/opt/bodyguard_cloud/data/events')

def jpeg_size(path):
    data = path.read_bytes()
    if len(data) < 4 or data[:2] != b'\xff\xd8':
        return 0, 0, len(data)
    i = 2
    while i + 9 < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker in (0xD8, 0xD9):
            continue
        if i + 2 > len(data):
            break
        seg_len = struct.unpack('>H', data[i:i + 2])[0]
        if seg_len < 2 or i + seg_len > len(data):
            break
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h = struct.unpack('>H', data[i + 3:i + 5])[0]
            w = struct.unpack('>H', data[i + 5:i + 7])[0]
            return w, h, len(data)
        i += seg_len
    return 0, 0, len(data)

events = sorted([p for p in base.iterdir() if p.is_dir()], key=lambda p: p.stat().st_mtime, reverse=True)[:12]
for ev in events:
    meta_path = ev / 'meta.json'
    meta = {}
    if meta_path.exists():
        try:
            meta = json.loads(meta_path.read_text())
        except Exception:
            pass
    snap = ev / 'snapshot.jpg'
    snap_info = jpeg_size(snap) if snap.exists() else None
    frames = sorted((ev / 'images').glob('*.jpg'))[:5]
    frame_info = [(f.name, *jpeg_size(f)) for f in frames]
    print(ev.name, 'mtime', int(ev.stat().st_mtime), 'frame_count_meta', meta.get('frame_count'), 'ai_image', meta.get('ai_image'), 'snapshot', snap_info, 'frames', frame_info)
PY
"""

stdin, stdout, stderr = client.exec_command(cmd)
print(stdout.read().decode("utf-8", errors="replace"))
print(stderr.read().decode("utf-8", errors="replace"))
client.close()
