import json
import time

import serial


ser = serial.Serial("COM3", 115200, timeout=0.2)
end = time.time() + 15
print("OPEN_OK")
while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    text = line.decode("utf-8", errors="replace").strip()
    if not text:
        continue
    if any(
        key in text
        for key in (
            "risk decision",
            "candidate_event",
            "T5 connected",
            "T5 notify",
            "WROOM EEG",
            "WiFi",
            "BGDATA",
        )
    ):
        if text.startswith("BGDATA,"):
            try:
                packet = json.loads(text[7:])
                mpu = packet.get("mpu", {})
                risk = packet.get("risk", {})
                print(
                    "BG angle={} pose={} final={} conf={} sys={}".format(
                        mpu.get("angle"),
                        risk.get("pose_state"),
                        risk.get("final"),
                        risk.get("confidence"),
                        risk.get("sys_state"),
                    )
                )
            except Exception:
                pass
        else:
            print(text)
ser.close()
print("READ_DONE")
