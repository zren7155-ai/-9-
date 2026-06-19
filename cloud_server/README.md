# BodyGuard Cloud Server

FastAPI service for receiving ESP32-P4 telemetry and event images.

## Endpoints

- `GET /health`: health check.
- `POST /v1/data`: receive sensor telemetry JSON.
- `POST /v1/ai/event_upload`: receive event metadata and images, call the AI model, and return `AI_NORMAL`, `AI_MONITOR`, or `AI_DANGER`.
- `GET /events/{event_id}/snapshot.jpg`: view the first event snapshot.
- `GET /events/{event_id}/video`: view event frames.

## Environment

```bash
export DASHSCOPE_API_KEY="YOUR_DASHSCOPE_API_KEY"
export DASHSCOPE_MODEL="qwen-vl-max"
export PUBLIC_BASE_URL="http://YOUR_SERVER_IP:5000"
export FEISHU_WEBHOOK_URL=""
export FEISHU_SECRET=""
```

## Run

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app:app --host 0.0.0.0 --port 5000
```
