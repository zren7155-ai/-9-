import json
import math
import time

import serial


ser = serial.Serial("COM3", 115200, timeout=0.2)
end = time.time() + 12
print("OPEN_OK move_or_tilt_mpu_now")
count = 0
zero_count = 0
max_acc = 0.0
max_gyro = 0.0
while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    text = line.decode("utf-8", errors="replace").strip()
    if "MPU6050" in text or text.startswith("BGERR,"):
        print(text)
    if not text.startswith("BGDATA,"):
        continue
    try:
        packet = json.loads(text[7:])
    except Exception:
        continue
    mpu = packet.get("mpu", {})
    ax = float(mpu.get("ax", 0))
    ay = float(mpu.get("ay", 0))
    az = float(mpu.get("az", 0))
    gx = float(mpu.get("gx", 0))
    gy = float(mpu.get("gy", 0))
    gz = float(mpu.get("gz", 0))
    acc = math.sqrt(ax * ax + ay * ay + az * az)
    gyro = abs(gx) + abs(gy) + abs(gz)
    max_acc = max(max_acc, acc)
    max_gyro = max(max_gyro, gyro)
    count += 1
    if acc < 0.05 and gyro < 0.05:
        zero_count += 1
    if count % 10 == 0:
        risk = packet.get("risk", {})
        print(
            "MPU ax={:.3f} ay={:.3f} az={:.3f} acc={:.3f} gyro_sum={:.1f} angle={} pose={} sys={}".format(
                ax,
                ay,
                az,
                acc,
                gyro,
                mpu.get("angle"),
                risk.get("pose_state"),
                risk.get("sys_state"),
            )
        )
ser.close()
print(f"SUMMARY samples={count} zero_count={zero_count} max_acc={max_acc:.3f} max_gyro={max_gyro:.1f}")
