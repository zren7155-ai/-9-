import time

import serial


ser = serial.Serial("COM3", 115200, timeout=0.2)
end = time.time() + 30
print("OPEN_OK repeat action now")
while time.time() < end:
    line = ser.readline()
    if not line:
        continue
    text = line.decode("utf-8", errors="replace").rstrip()
    if text:
        print(text)
ser.close()
print("READ_DONE")
