# ESP32-P4 NANO BodyGuard

ESP32-P4 NANO BodyGuard is an embedded safety-monitoring prototype built around an ESP32-P4-NANO board. The firmware combines motion sensing, camera snapshots, BrainLink EEG input, local risk scoring, audio alerts, BLE/serial telemetry, MJPEG preview, and cloud AI event review.

The repository also includes a small FastAPI cloud service that receives telemetry and event images, calls a multimodal AI model, stores event assets, and returns a concise safety result for the device.

## Project Layout

```text
bodyguard_esp32_p4/          ESP32-P4 main firmware
brainlink_wroom_gateway/     ESP32-WROOM Bluetooth SPP to UART EEG bridge
cloud_server/                FastAPI cloud event receiver and AI analyzer
BrainLinkParser-Python/      PC-side BrainLink helper scripts
sdcard_image/                SD-card voice prompt assets for local deployment
```

The firmware currently uses local ESP-IDF component paths for camera/video and hosted-WiFi related components. Keep those components available locally before building, or adjust `bodyguard_esp32_p4/CMakeLists.txt` to match your ESP-IDF component layout.

## Main Features

- MPU6050 posture and movement sampling.
- OV5647 camera capture with event image buffering.
- BrainLink Lite EEG input through BLE or a PC/ESP32-WROOM UART bridge.
- Lightweight local risk engine for fall/fatigue pre-checks.
- Cloud AI event upload through HTTP.
- Audio alerts with WAV files on SD card.
- BLE, serial telemetry, and optional MJPEG stream for debugging.
- FastAPI cloud API with event storage, health check, image viewing, and AI fallback logic.

## Hardware Notes

Default pins are defined in `bodyguard_esp32_p4/main/bodyguard_config.h`.

Common hardware used by this project:

- ESP32-P4-NANO development board.
- MPU6050 over I2C.
- OV5647 camera.
- ES8311/NS4150B audio chain.
- TF/SD card for cached events and voice prompts.
- BrainLink Lite EEG device.
- Optional ESP32-WROOM gateway for Bluetooth SPP to UART forwarding.

## Firmware Configuration

Create a private config file before compiling:

```powershell
Copy-Item bodyguard_esp32_p4\main\bodyguard_private_config.example.h bodyguard_esp32_p4\main\bodyguard_private_config.h
```

Then edit `bodyguard_esp32_p4/main/bodyguard_private_config.h`:

```c
#define BODYGUARD_WIFI_SSID                    "YOUR_WIFI_SSID"
#define BODYGUARD_WIFI_PASSWORD                "YOUR_WIFI_PASSWORD"
#define BODYGUARD_CLOUD_HOST                   "http://YOUR_SERVER_IP:5000"
```

Do not commit `bodyguard_private_config.h`; it is ignored by Git.

## Build Firmware

Install and export ESP-IDF first. This project has been worked on with ESP-IDF 5.x.

```powershell
cd bodyguard_esp32_p4
idf.py set-target esp32p4
idf.py build
idf.py -p COM3 flash monitor
```

If your local ESP-IDF components are not in the same place as this development machine, update `EXTRA_COMPONENT_DIRS` in `bodyguard_esp32_p4/CMakeLists.txt`.

## Build BrainLink Gateway

The gateway project is for an ESP32-WROOM style board that connects to BrainLink Lite through Bluetooth SPP and forwards compact EEG frames to the P4 over UART.

```powershell
cd brainlink_wroom_gateway
idf.py set-target esp32
idf.py build
idf.py -p COM8 flash monitor
```

## Run Cloud Server

```powershell
cd cloud_server
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
$env:DASHSCOPE_API_KEY="YOUR_DASHSCOPE_API_KEY"
$env:PUBLIC_BASE_URL="http://YOUR_SERVER_IP:5000"
uvicorn app:app --host 0.0.0.0 --port 5000
```

Useful endpoints:

- `GET /health`
- `POST /v1/data`
- `POST /v1/ai/event_upload`
- `GET /events/{event_id}/snapshot.jpg`
- `GET /events/{event_id}/video`

## SD Card Assets

Copy the WAV prompt files to the SD card so the firmware can play local alerts:

```text
/voice/analyzing.wav
/voice/fall_detected.wav
/voice/fatigue.wav
```

## GitHub Publishing Checklist

Before pushing to GitHub:

1. Keep real secrets out of the repository: WiFi passwords, cloud IP credentials, Tuya auth keys, API keys, and SSH private keys.
2. Do not commit `build/`, `build_*`, `*.log`, captured `*.jpg`, raw stream `*.bin`, Python dependency folders, or cloud runtime data.
3. Review `git status --short` before committing.
4. If a secret was already committed, rotate it before making the repository public.

Suggested first publish commands:

```powershell
git init
git add README.md .gitignore bodyguard_esp32_p4 brainlink_wroom_gateway cloud_server BrainLinkParser-Python build_p4_wifi.cmd pc_realtime_monitor.py verify_alarm_flow.py verify_p4_runtime.py
git status --short
git commit -m "Initial BodyGuard ESP32-P4 project"
git branch -M main
git remote add origin https://github.com/YOUR_NAME/YOUR_REPO.git
git push -u origin main
```

