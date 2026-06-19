import sys
import os
from pathlib import Path

LOCAL_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(LOCAL_ROOT / "pydeps"))

import paramiko


def run(client, command):
    stdin, stdout, stderr = client.exec_command(command)
    code = stdout.channel.recv_exit_status()
    out = stdout.read().decode("utf-8", errors="replace")
    err = stderr.read().decode("utf-8", errors="replace")
    print(f"$ {command}\nexit={code}")
    if out:
        print(out)
    if err:
        print(err)
    return code, out, err


def main():
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

    run(client, "sudo systemctl cat bodyguard-cloud")
    run(client, "sudo ls -la /opt/bodyguard_cloud")
    run(client, "sudo find /opt/bodyguard_cloud -maxdepth 2 -type f -name '*.env' -o -name '.env'")
    run(client, "sudo systemctl stop bodyguard-cloud || true")
    run(client, "sudo pkill -f 'uvicorn app:app' || true")
    run(client, "sudo rm -rf /opt/bodyguard_cloud/__pycache__")
    run(client, "cd /opt/bodyguard_cloud && sudo .venv/bin/python -m py_compile app.py")
    run(client, "sudo systemctl daemon-reload")
    run(client, "sudo systemctl restart bodyguard-cloud")
    run(client, "sleep 2; sudo systemctl status bodyguard-cloud --no-pager -l | head -60")
    run(client, "tail -80 /tmp/bodyguard_cloud.log 2>/dev/null || true")
    client.close()


if __name__ == "__main__":
    main()
