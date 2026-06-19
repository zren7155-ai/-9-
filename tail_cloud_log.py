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
stdin, stdout, stderr = client.exec_command("sudo tail -40 /opt/bodyguard_cloud/logs/bodyguard.log")
print(stdout.read().decode("utf-8", errors="replace"))
print(stderr.read().decode("utf-8", errors="replace"))
client.close()
