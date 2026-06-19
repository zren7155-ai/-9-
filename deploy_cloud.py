import sys
import time
import os
from pathlib import Path

LOCAL_ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(LOCAL_ROOT / "pydeps"))

import paramiko


HOST = os.getenv("BODYGUARD_DEPLOY_HOST", "")
USER = os.getenv("BODYGUARD_DEPLOY_USER", "admin")
KEY_PATH = Path(os.getenv("BODYGUARD_DEPLOY_KEY", str(LOCAL_ROOT / "deploy_key.pem")))
LOCAL_APP = LOCAL_ROOT / "cloud_server" / "app.py"


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
    if not HOST:
        raise SystemExit("Set BODYGUARD_DEPLOY_HOST before deploying.")
    if not KEY_PATH.exists():
        raise SystemExit(f"SSH key not found: {KEY_PATH}")

    key = paramiko.RSAKey.from_private_key_file(str(KEY_PATH))
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(HOST, username=USER, pkey=key, timeout=10)

    run(client, "pwd; hostname")
    run(client, "ps aux | grep -E 'uvicorn|gunicorn|python' | grep -v grep || true")
    code, out, _ = run(client, "find / -maxdepth 5 -name app.py 2>/dev/null | grep -E 'bodyguard|cloud|root|opt|www' | head -20")

    candidates = [line.strip() for line in out.splitlines() if line.strip()]
    remote_app = None
    for path in candidates:
        code, content, _ = run(client, f"grep -q 'BodyGuard Cloud Server' '{path}' && echo MATCH || true")
        if "MATCH" in content:
            remote_app = path
            break

    if remote_app is None:
        raise SystemExit("未找到服务器上的 BodyGuard app.py，请把 find 输出发回来。")

    remote_dir = str(Path(remote_app).parent).replace("\\", "/")
    backup = f"{remote_app}.bak.{int(time.time())}"
    print(f"REMOTE_APP={remote_app}")
    print(f"REMOTE_DIR={remote_dir}")

    sftp = client.open_sftp()
    tmp_app = f"/tmp/bodyguard_app_{int(time.time())}.py"
    sftp.put(str(LOCAL_APP), tmp_app)
    sftp.close()
    run(client, f"sudo cp '{remote_app}' '{backup}'")
    run(client, f"sudo cp '{tmp_app}' '{remote_app}'")
    run(client, f"sudo chown root:root '{remote_app}'")

    run(client, f"cd '{remote_dir}' && sudo .venv/bin/python -m py_compile '{remote_app}'")
    run(client, "sudo systemctl daemon-reload")
    run(client, "sudo systemctl restart bodyguard-cloud")
    time.sleep(2)
    run(client, "sudo systemctl status bodyguard-cloud --no-pager -l | head -60")
    run(client, "tail -80 /opt/bodyguard_cloud/logs/bodyguard.log 2>/dev/null || true")
    client.close()


if __name__ == "__main__":
    main()
