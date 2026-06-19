import json
import math
import time

import serial


ser = serial.Serial("COM3", 115200, timeout=0.2)
end = time.time() + 35
print("OPEN_OK action: flat 1s -> flick/tap -> side 5s")
max_state = 0
max_risk = 0
max_conf = 0
max_angle = 0
events = []
while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    text = line.decode("utf-8", errors="replace").strip()
    if not text:
        continue
    interesting = (
        "candidate_event" in text
        or "risk decision" in text
        or "event captured" in text
        or "upload complete" in text
        or "audio" in text
        or "STATE" in text
        or "状态转换" in text
        or "MPU6050 recover" in text
        or "WiFi connected" in text
    )
    if interesting:
        print("LOG", text)
        events.append(text)
    if text.startswith("BGDATA,"):
        try:
            packet = json.loads(text[7:])
        except Exception:
            continue
        mpu = packet.get("mpu", {})
        risk = packet.get("risk", {})
        angle = float(mpu.get("angle", 0))
        ax = float(mpu.get("ax", 0))
        ay = float(mpu.get("ay", 0))
        az = float(mpu.get("az", 0))
        gx = float(mpu.get("gx", 0))
        gy = float(mpu.get("gy", 0))
        gz = float(mpu.get("gz", 0))
        acc = math.sqrt(ax * ax + ay * ay + az * az)
        gyro = abs(gx) + abs(gy) + abs(gz)
        state = int(risk.get("sys_state", 0))
        final = int(risk.get("final", 0))
        conf = int(risk.get("confidence", 0))
        max_state = max(max_state, state)
        max_risk = max(max_risk, final)
        max_conf = max(max_conf, conf)
        max_angle = max(max_angle, angle)
        if angle > 35 or gyro > 100 or acc > 1.5 or state >= 2:
            print(
                "BG angle={:.1f} acc={:.2f} gyro={:.1f} pose={} final={} conf={} sys={}".format(
                    angle,
                    acc,
                    gyro,
                    risk.get("pose_state"),
                    final,
                    conf,
                    state,
                )
            )
ser.close()
print(
    "SUMMARY max_angle={:.1f} max_risk={} max_conf={} max_sys_state={} events={}".format(
        max_angle, max_risk, max_conf, max_state, len(events)
    )
)
