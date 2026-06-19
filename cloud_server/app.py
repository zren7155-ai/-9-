import base64
import hashlib
import hmac
import html
import json
import logging
import os
import struct
import time
from pathlib import Path
from typing import List, Optional

import requests
from fastapi import FastAPI, File, Form, UploadFile
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, PlainTextResponse, StreamingResponse


APP_ROOT = Path(__file__).resolve().parent
DATA_DIR = APP_ROOT / "data" / "events"
DATA_DIR.mkdir(parents=True, exist_ok=True)
LOG_DIR = APP_ROOT / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
    handlers=[
        logging.FileHandler(LOG_DIR / "bodyguard.log", encoding="utf-8"),
        logging.StreamHandler(),
    ],
)
logger = logging.getLogger("bodyguard_cloud")

DASHSCOPE_API_KEY = os.getenv("DASHSCOPE_API_KEY", "")
PUBLIC_BASE_URL = os.getenv("PUBLIC_BASE_URL", "http://127.0.0.1:5000").rstrip("/")
FEISHU_WEBHOOK_URL = os.getenv("FEISHU_WEBHOOK_URL", "")
FEISHU_SECRET = os.getenv("FEISHU_SECRET", "")
DASHSCOPE_URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
DASHSCOPE_MODEL = os.getenv("DASHSCOPE_MODEL", "qwen-vl-max")

app = FastAPI(title="BodyGuard Cloud Server", version="1.0.2")


def _safe_event_id(event_id: str) -> str:
    keep = [ch for ch in event_id[:64] if ch.isalnum() or ch in ("-", "_")]
    return "".join(keep) or str(int(time.time() * 1000))


def _event_dir(event_id: str) -> Path:
    return DATA_DIR / _safe_event_id(event_id)


def _event_frame_paths(event_id: str) -> list[Path]:
    image_dir = _event_dir(event_id) / "images"
    if not image_dir.exists():
        return []
    return [path for path in sorted(image_dir.glob("*.jpg")) if _valid_model_image(path)]


def _event_video_url(event_id: str) -> str:
    return f"{PUBLIC_BASE_URL}/events/{event_id}/video"


def _event_strip_url(event_id: str) -> str:
    return f"{PUBLIC_BASE_URL}/events/{event_id}/video-strip"


def _event_mjpeg_url(event_id: str) -> str:
    return f"{PUBLIC_BASE_URL}/events/{event_id}/video.mjpeg"


def _event_frame_url(event_id: str, frame_name: str) -> str:
    return f"{PUBLIC_BASE_URL}/events/{event_id}/frames/{frame_name}"


def _url_with_version(url: str, version: str) -> str:
    if not url:
        return ""
    sep = "&" if "?" in url else "?"
    return f"{url}{sep}v={version}"




def _trim_30(text: str) -> str:
    text = "".join(str(text).splitlines()).strip().replace(" ", "")
    fallback = "\u7efc\u5408\u5224\u65ad:\u5173\u6ce8;\u5904\u7406\u5efa\u8bae:\u53ca\u65f6\u67e5\u770b"
    return text[:30] if text else fallback


def _fallback_report(risk_pre: int, posture: int) -> tuple[str, str, str]:
    if risk_pre >= 90 or posture == 3:
        return "AI_DANGER", "FALL_DETECTED", "AI\u8c03\u7528\u5931\u8d25;\u672c\u5730\u5224\u65ad\u5371\u9669"
    if risk_pre >= 70:
        return "AI_MONITOR", "FATIGUE", "AI\u8c03\u7528\u5931\u8d25;\u672c\u5730\u5efa\u8bae\u5173\u6ce8"
    return "AI_NORMAL", "NORMAL", "AI\u8c03\u7528\u5931\u8d25;\u672c\u5730\u5224\u65ad\u6b63\u5e38"


def _extract_ai_level(text: str, risk_pre: int, posture: int) -> tuple[str, str]:
    text = str(text)
    lowered = text.lower()
    danger = "\u5371\u9669"
    monitor = "\u5173\u6ce8"
    abnormal = "\u5f02\u5e38"
    normal = "\u6b63\u5e38"
    if danger in text or "danger" in lowered:
        return "AI_DANGER", "FALL_DETECTED"
    if monitor in text or abnormal in text or "monitor" in lowered:
        return "AI_MONITOR", "FATIGUE"
    if normal in text or "normal" in lowered:
        return "AI_NORMAL", "NORMAL"
    level, tts_code, _ = _fallback_report(risk_pre, posture)
    return level, tts_code

def _learning_feedback(level: str, risk_pre: int, posture: int) -> dict:
    if level == "AI_DANGER":
        return {
            "danger_type": "fall",
            "confidence": 0.93,
            "false_alarm": False,
            "recommendation": "emergency",
            "pose_reliable": 0.86,
            "eeg_reliable": 0.24,
            "recommended_pose_weight": 0.72,
            "recommended_eeg_weight": 0.18,
        }
    if level == "AI_MONITOR":
        return {
            "danger_type": "fatigue",
            "confidence": 0.78,
            "false_alarm": bool(risk_pre < 70 and posture != 3),
            "recommendation": "monitor",
            "pose_reliable": 0.68,
            "eeg_reliable": 0.42,
            "recommended_pose_weight": 0.66,
            "recommended_eeg_weight": 0.24,
        }
    return {
        "danger_type": "normal",
        "confidence": 0.88,
        "false_alarm": bool(risk_pre >= 70 or posture == 3),
        "recommendation": "normal",
        "pose_reliable": 0.74,
        "eeg_reliable": 0.18,
        "recommended_pose_weight": 0.60,
        "recommended_eeg_weight": 0.30,
    }


def _jpeg_size(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    if len(data) < 4 or data[:2] != b"\xff\xd8":
        return 0, 0
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
        seg_len = struct.unpack(">H", data[i:i + 2])[0]
        if seg_len < 2 or i + seg_len > len(data):
            break
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h = struct.unpack(">H", data[i + 3:i + 5])[0]
            w = struct.unpack(">H", data[i + 5:i + 7])[0]
            return w, h
        i += seg_len
    return 0, 0


def _valid_model_image(path: Optional[Path]) -> bool:
    if path is None or not path.exists() or path.stat().st_size < 1024:
        return False
    width, height = _jpeg_size(path)
    logger.info("image check: path=%s size=%s width=%s height=%s", path, path.stat().st_size, width, height)
    return width > 10 and height > 10


def _select_ai_image(snapshot_path: Optional[Path], frame_paths: list[Path]) -> Optional[Path]:
    if _valid_model_image(snapshot_path):
        return snapshot_path
    for frame_path in frame_paths:
        if _valid_model_image(frame_path):
            logger.info("ai image fallback to frame: %s", frame_path)
            return frame_path
    logger.warning("no valid ai image: snapshot=%s frames=%u", snapshot_path, len(frame_paths))
    return None


def _snapshot_data_url(snapshot_path: Path) -> str:
    return "data:image/jpeg;base64," + base64.b64encode(snapshot_path.read_bytes()).decode("ascii")



def call_dashscope(snapshot_path: Optional[Path], prompt: str, risk_pre: int, posture: int) -> tuple[str, str, str, str]:
    image_ok = _valid_model_image(snapshot_path)
    if not DASHSCOPE_API_KEY or not image_ok:
        logger.warning("dashscope fallback: key_configured=%s valid_image=%s risk_pre=%s posture=%s", bool(DASHSCOPE_API_KEY), image_ok, risk_pre, posture)
        level, tts_code, report = _fallback_report(risk_pre, posture)
        return level, tts_code, report, "fallback"

    text_prompt = (
        "You are a multimodal safety monitoring model. "
        "Analyze the image plus sensor summary. "
        "Return ONLY one short Chinese sentence within 30 Chinese characters. "
        "The sentence MUST use this exact style: 综合判断:正常/关注/危险;处理建议:xxx. "
        "Do not apologize. Do not mention missing information. "
        f"risk_pre={risk_pre}, posture={posture}. Extra note: {prompt or ''}"
    )
    payload = {
        "model": DASHSCOPE_MODEL,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": text_prompt},
                    {"type": "image_url", "image_url": {"url": _snapshot_data_url(snapshot_path)}},
                ],
            }
        ],
        "max_tokens": 64,
    }
    headers = {"Authorization": f"Bearer {DASHSCOPE_API_KEY}", "Content-Type": "application/json"}

    try:
        logger.info("dashscope request start: model=%s risk_pre=%s posture=%s", DASHSCOPE_MODEL, risk_pre, posture)
        resp = requests.post(DASHSCOPE_URL, headers=headers, json=payload, timeout=20)
        logger.info("dashscope http status=%s", resp.status_code)
        if resp.status_code >= 400:
            logger.error("dashscope error body=%s", resp.text[:1000])
        resp.raise_for_status()
        data = resp.json()
        content = data["choices"][0]["message"]["content"]
        text = content if isinstance(content, str) else json.dumps(content, ensure_ascii=False)
        report = _trim_30(text)
        level, tts_code = _extract_ai_level(report, risk_pre, posture)
        logger.info("dashscope success: level=%s tts=%s report=%s", level, tts_code, report)
        return level, tts_code, report, "dashscope"
    except Exception as exc:
        logger.exception("dashscope fallback after error: %s", exc)
        level, tts_code, report = _fallback_report(risk_pre, posture)
        return level, tts_code, report, "fallback"

def _feishu_signed_payload(payload: dict) -> dict:
    if not FEISHU_SECRET:
        return payload
    timestamp = str(int(time.time()))
    string_to_sign = f"{timestamp}\n{FEISHU_SECRET}".encode("utf-8")
    sign = base64.b64encode(hmac.new(string_to_sign, b"", hashlib.sha256).digest()).decode("utf-8")
    payload["timestamp"] = timestamp
    payload["sign"] = sign
    return payload


def send_feishu(event_id: str, level: str, report: str, snapshot_url: str, video_url: str, strip_url: str = "", ai_source: str = "unknown") -> None:
    if not FEISHU_WEBHOOK_URL:
        logger.warning("feishu skipped: webhook not configured event_id=%s", event_id)
        return

    source_text = "通义千问多模态" if ai_source == "dashscope" else "本地兜底/AI未成功"
    actions = []
    if snapshot_url:
        actions.append({
            "tag": "button",
            "text": {"tag": "plain_text", "content": "查看照片"},
            "url": snapshot_url,
            "type": "default",
        })
    if video_url:
        actions.append({
            "tag": "button",
            "text": {"tag": "plain_text", "content": "查看视频"},
            "url": video_url,
            "type": "primary",
        })
    if strip_url:
        actions.append({
            "tag": "button",
            "text": {"tag": "plain_text", "content": "静态帧预览"},
            "url": strip_url,
            "type": "default",
        })

    elements = [
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**事件ID**：{event_id}\n**等级**：{level}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**AI分析报告**：{report}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**分析来源**：{source_text}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**照片链接**：{snapshot_url or '未收到快照'}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**视频链接**：{video_url or '未生成事件视频'}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**帧预览**：{strip_url or '未生成帧预览'}"}},
    ]
    if actions:
        elements.append({"tag": "action", "actions": actions})
    elements.extend([
        {"tag": "hr"},
        {"tag": "note", "elements": [{"tag": "plain_text", "content": "如果飞书内置浏览器不播放视频，请点“静态帧预览”。"}]},
    ])

    payload = {
        "msg_type": "interactive",
        "card": {
            "config": {"wide_screen_mode": True},
            "header": {
                "template": "red" if level == "AI_DANGER" else "orange",
                "title": {"tag": "plain_text", "content": "BodyGuard 安全监护告警"},
            },
            "elements": elements,
        },
    }
    try:
        resp = requests.post(FEISHU_WEBHOOK_URL, headers={"Content-Type": "application/json"}, json=_feishu_signed_payload(payload), timeout=5)
        logger.info("feishu webhook status=%s event_id=%s body=%s", resp.status_code, event_id, resp.text[:300])
    except Exception as exc:
        logger.exception("feishu webhook error event_id=%s error=%s", event_id, exc)


def _num(value, default=0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _latest_sensor_summary(sensor_payload: dict, risk_pre: int, risk_final: int, confidence: int, posture: int) -> str:
    """生成飞书用的核心传感器摘要，字段名保持和 P4 上传 JSON 兼容。"""
    if not isinstance(sensor_payload, dict):
        sensor_payload = {}
    mpu = sensor_payload.get("mpu") or sensor_payload.get("last_sensor") or {}
    eeg = sensor_payload.get("eeg") or sensor_payload.get("last_eeg") or {}
    location = sensor_payload.get("location") or sensor_payload.get("gps") or {}
    series = sensor_payload.get("series") or sensor_payload
    risk = sensor_payload.get("risk") or {}

    angle = _num(mpu.get("angle", mpu.get("tilt_angle", 0.0)))
    accel = mpu.get("accel") or [0, 0, 0]
    gyro = mpu.get("gyro") or [0, 0, 0]
    if not isinstance(accel, list):
        accel = [0, 0, 0]
    if not isinstance(gyro, list):
        gyro = [0, 0, 0]
    accel = (accel + [0, 0, 0])[:3]
    gyro = (gyro + [0, 0, 0])[:3]

    eeg_connected = int(_num(eeg.get("connected", 0)))
    attention = int(_num(eeg.get("attention", 0)))
    meditation = int(_num(eeg.get("meditation", eeg.get("relax", 0))))
    fatigue = int(_num(eeg.get("fatigue", 0)))
    signal_quality = int(_num(eeg.get("signal_quality", eeg.get("noise", 0))))
    eeg_state = int(_num(risk.get("eeg_state", sensor_payload.get("eeg_state", 0))))
    pose_state = int(_num(risk.get("pose_state", sensor_payload.get("pose_state", posture))))
    sys_state = int(_num(risk.get("sys_state", sensor_payload.get("sys_state", 0))))
    if isinstance(location, dict):
        latitude = location.get("latitude", location.get("lat"))
        longitude = location.get("longitude", location.get("lng", location.get("lon")))
        address = location.get("address") or location.get("name") or location.get("desc") or ""
        if latitude is not None and longitude is not None:
            location_text = f"纬度={_num(latitude):.6f}，经度={_num(longitude):.6f}"
            if address:
                location_text += f"，位置={address}"
        else:
            location_text = address or "未接入定位"
    elif isinstance(location, str) and location.strip():
        location_text = location.strip()
    else:
        location_text = "未接入定位"

    return (
        f"**风险数据**：初判风险分={risk_pre}，最终风险分={risk_final}，置信度={confidence}%\n"
        f"**系统状态**：系统状态码={sys_state}，姿态状态码={pose_state}，脑电状态码={eeg_state}\n"
        f"**姿态数据**：姿态角度={angle:.1f}°，加速度=[{_num(accel[0]):.3f}, {_num(accel[1]):.3f}, {_num(accel[2]):.3f}]g，"
        f"角速度=[{_num(gyro[0]):.1f}, {_num(gyro[1]):.1f}, {_num(gyro[2]):.1f}]°/s\n"
        f"**脑电数据**：脑电连接={eeg_connected}，注意力={attention}，放松度={meditation}，疲劳值={fatigue}，信号质量={signal_quality}\n"
        f"**实时位置**：{location_text}\n"
        f"**事件缓存**：姿态数据={int(_num(series.get('sensor_count', 0)))}条，脑电数据={int(_num(series.get('eeg_count', 0)))}条，"
        f"风险数据={int(_num(series.get('risk_count', 0)))}条，图像={int(_num(series.get('image_count', 0)))}帧"
    )


def send_feishu(
    event_id: str,
    level: str,
    report: str,
    snapshot_url: str,
    video_url: str,
    strip_url: str = "",
    ai_source: str = "unknown",
    sensor_payload: Optional[dict] = None,
    risk_pre: int = 0,
    risk_final: int = 0,
    confidence: int = 0,
    posture: int = 0,
) -> None:
    if not FEISHU_WEBHOOK_URL:
        logger.warning("feishu skipped: webhook not configured event_id=%s", event_id)
        return

    source_text = "通义千问多模态大模型" if ai_source == "dashscope" else "本地兜底/AI未成功"
    sensor_summary = _latest_sensor_summary(sensor_payload or {}, risk_pre, risk_final, confidence, posture)
    actions = []
    if snapshot_url:
        actions.append({
            "tag": "button",
            "text": {"tag": "plain_text", "content": "查看照片"},
            "url": snapshot_url,
            "type": "default",
        })

    elements = [
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**事件ID**：{event_id}\n**等级**：{level}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**AI分析报告**：{report}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": sensor_summary}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**分析来源**：{source_text}"}},
        {"tag": "div", "text": {"tag": "lark_md", "content": f"**照片链接**：{snapshot_url or '未收到快照'}"}},
    ]
    if actions:
        elements.append({"tag": "action", "actions": actions})
    elements.append({"tag": "hr"})

    payload = {
        "msg_type": "interactive",
        "card": {
            "config": {"wide_screen_mode": True},
            "header": {
                "template": "red" if level == "AI_DANGER" else "orange",
                "title": {"tag": "plain_text", "content": "BodyGuard 安全监护告警"},
            },
            "elements": elements,
        },
    }
    try:
        resp = requests.post(FEISHU_WEBHOOK_URL, headers={"Content-Type": "application/json"}, json=_feishu_signed_payload(payload), timeout=5)
        logger.info("feishu webhook status=%s event_id=%s body=%s", resp.status_code, event_id, resp.text[:300])
    except Exception as exc:
        logger.exception("feishu webhook error event_id=%s error=%s", event_id, exc)


def _latest_event_summary() -> dict:
    event_dirs = [path for path in DATA_DIR.iterdir() if path.is_dir()] if DATA_DIR.exists() else []
    if not event_dirs:
        return {"event_id": "", "frame_count": 0, "has_snapshot": False, "mtime": 0}
    latest = max(event_dirs, key=lambda path: path.stat().st_mtime)
    frames = _event_frame_paths(latest.name)
    snapshot = latest / "snapshot.jpg"
    return {
        "event_id": latest.name,
        "frame_count": len(frames),
        "has_snapshot": snapshot.exists() or bool(frames),
        "mtime": latest.stat().st_mtime,
    }


def _read_json_file(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        logger.warning("json read failed path=%s error=%s", path, exc)
        return {}


def _event_detail(event_id: str) -> dict:
    event_id = _safe_event_id(event_id)
    event_dir = _event_dir(event_id)
    meta = _read_json_file(event_dir / "meta.json")
    sensor = _read_json_file(event_dir / "sensor.json")
    frames = _event_frame_paths(event_id)
    snapshot = event_dir / "snapshot.jpg"
    series = sensor.get("series", {}) if isinstance(sensor.get("series", {}), dict) else {}
    checks = {
        "images": len(frames) > 0,
        "snapshot": _valid_model_image(snapshot) or len(frames) > 0,
        "sensor_json": bool(sensor),
        "mpu": bool(sensor.get("mpu") or sensor.get("last_sensor") or series.get("sensor_count") or sensor.get("sensor_count")),
        "eeg": bool(sensor.get("eeg") or sensor.get("last_eeg") or series.get("eeg_count") or sensor.get("eeg_count")),
        "risk": any(k in meta for k in ("risk_pre", "risk_final", "confidence")),
        "ai": bool(meta.get("report") or meta.get("level")),
        "feishu": bool(meta.get("feishu_status")),
    }
    mtime = event_dir.stat().st_mtime if event_dir.exists() else 0
    return {
        "event_id": event_id,
        "mtime": mtime,
        "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(mtime)) if mtime else "",
        "frame_count": len(frames),
        "snapshot_size": snapshot.stat().st_size if snapshot.exists() else 0,
        "meta": meta,
        "sensor": sensor,
        "series": series,
        "checks": checks,
        "snapshot_url": f"/events/{event_id}/snapshot.jpg" if checks["snapshot"] else "",
        "video_url": f"/events/{event_id}/video" if frames else "",
        "strip_url": f"/events/{event_id}/video-strip" if frames else "",
    }


def _recent_events(limit: int = 20) -> list[dict]:
    if not DATA_DIR.exists():
        return []
    event_dirs = sorted(
        [path for path in DATA_DIR.iterdir() if path.is_dir()],
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    return [_event_detail(path.name) for path in event_dirs[:limit]]


def _ok_badge(ok: bool) -> str:
    if ok:
        return '<span class="badge ok">已收到</span>'
    return '<span class="badge bad">未收到</span>'


def _cloud_console_html(status: dict) -> str:
    events = _recent_events(20)
    latest = events[0] if events else None
    rows = []
    for item in events:
        meta = item["meta"]
        checks = item["checks"]
        event_id = html.escape(item["event_id"])
        report = html.escape(str(meta.get("report", "等待AI分析...")))
        level = html.escape(str(meta.get("level", "UNKNOWN")))
        links = []
        if item["snapshot_url"]:
            links.append(f'<a href="{item["snapshot_url"]}">照片</a>')
        if item["video_url"]:
            links.append(f'<a href="{item["video_url"]}">视频</a>')
        links.append(f'<a href="/api/events/{event_id}">JSON</a>')
        rows.append(
            "<tr>"
            f"<td><b>{event_id}</b><br><span>{html.escape(item['time'])}</span></td>"
            f"<td>{level}<br><span>{report}</span></td>"
            f"<td>{int(meta.get('risk_pre', 0))}/{int(meta.get('risk_final', 0))}<br><span>confidence={int(meta.get('confidence', 0))}</span></td>"
            f"<td>{_ok_badge(checks['images'])}<br><span>{item['frame_count']}帧</span></td>"
            f"<td>{_ok_badge(checks['snapshot'])}<br><span>{item['snapshot_size']}B</span></td>"
            f"<td>{_ok_badge(checks['mpu'])}</td>"
            f"<td>{_ok_badge(checks['eeg'])}</td>"
            f"<td>{_ok_badge(checks['ai'])}</td>"
            f"<td>{_ok_badge(checks['feishu'])}</td>"
            f"<td>{' / '.join(links)}</td>"
            "</tr>"
        )

    if latest:
        latest_json = html.escape(json.dumps({"meta": latest["meta"], "sensor": latest["sensor"]}, ensure_ascii=False, indent=2))
        latest_detail = f"""
        <section class="panel wide">
          <div class="section-title">最新事件详情</div>
          <div class="detail-grid">
            <div><span>事件ID</span><b>{html.escape(latest["event_id"])}</b></div>
            <div><span>风险分</span><b>{int(latest["meta"].get("risk_pre", 0))} / {int(latest["meta"].get("risk_final", 0))}</b></div>
            <div><span>置信度</span><b>{int(latest["meta"].get("confidence", 0))}%</b></div>
            <div><span>图片</span><b>{latest["frame_count"]} 帧</b></div>
            <div><span>姿态序列</span><b>{latest["series"].get("sensor_count", latest["sensor"].get("sensor_count", 0))}</b></div>
            <div><span>脑电序列</span><b>{latest["series"].get("eeg_count", latest["sensor"].get("eeg_count", 0))}</b></div>
          </div>
          <pre>{latest_json}</pre>
        </section>
        """
    else:
        latest_detail = '<section class="panel wide empty">还没有收到 ESP32-P4 的报警事件。触发 DANGER 后，这里会自动出现照片、姿态、脑电、风险分和 AI 分析结果。</section>'

    table_body = "\n".join(rows) if rows else '<tr><td colspan="10" class="empty">暂无事件</td></tr>'
    now = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
    dashscope_text = "已配置" if status["dashscope_configured"] else "未配置"
    feishu_text = "已配置" if status["feishu_configured"] else "未配置"
    latest_id = html.escape(latest["event_id"]) if latest else "暂无"
    latest_frames = latest["frame_count"] if latest else 0
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="5">
  <title>BodyGuard 云端控制台</title>
  <style>
    :root {{ --bg:#f6f7fb; --panel:#fff; --text:#172033; --muted:#657083; --line:#e3e8f0; --green:#0f8f67; --red:#c73535; --blue:#2563eb; }}
    * {{ box-sizing:border-box; }}
    body {{ margin:0; background:var(--bg); color:var(--text); font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",Arial,sans-serif; }}
    main {{ width:min(1280px, calc(100% - 28px)); margin:0 auto; padding:24px 0 42px; }}
    header {{ display:flex; justify-content:space-between; gap:16px; align-items:flex-start; margin-bottom:16px; }}
    h1 {{ margin:0 0 6px; font-size:28px; letter-spacing:0; }}
    .sub, span {{ color:var(--muted); font-size:13px; }}
    .status {{ display:flex; gap:8px; flex-wrap:wrap; justify-content:flex-end; }}
    .pill {{ padding:8px 10px; border-radius:999px; background:#eef6ff; color:#174ea6; font-weight:700; }}
    .grid {{ display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:12px; margin-bottom:12px; }}
    .panel {{ background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:16px; box-shadow:0 8px 24px rgba(20,31,51,.06); }}
    .wide {{ grid-column:1 / -1; }}
    .metric-label {{ color:var(--muted); font-size:13px; margin-bottom:8px; }}
    .metric {{ font-size:24px; font-weight:800; }}
    .section-title {{ font-size:18px; font-weight:800; margin-bottom:12px; }}
    .badge {{ display:inline-flex; padding:4px 8px; border-radius:999px; font-size:12px; font-weight:800; }}
    .badge.ok {{ background:#e8f7f0; color:var(--green); }}
    .badge.bad {{ background:#fff1f1; color:var(--red); }}
    table {{ width:100%; border-collapse:collapse; font-size:14px; }}
    th, td {{ text-align:left; vertical-align:top; border-bottom:1px solid var(--line); padding:10px 8px; }}
    th {{ color:var(--muted); font-size:12px; font-weight:800; background:#f8fafc; }}
    a {{ color:var(--blue); font-weight:700; text-decoration:none; }}
    .detail-grid {{ display:grid; grid-template-columns:repeat(6,minmax(0,1fr)); gap:10px; margin-bottom:12px; }}
    .detail-grid div {{ border:1px solid var(--line); border-radius:7px; padding:10px; background:#fbfcff; }}
    .detail-grid b {{ display:block; margin-top:5px; font-size:18px; }}
    pre {{ max-height:360px; overflow:auto; background:#0f172a; color:#dbeafe; border-radius:8px; padding:12px; font-size:12px; }}
    .empty {{ color:var(--muted); text-align:center; padding:28px; }}
    @media (max-width:900px) {{ .grid,.detail-grid {{ grid-template-columns:1fr; }} header {{ flex-direction:column; }} }}
  </style>
</head>
<body>
<main>
  <header>
    <div>
      <h1>BodyGuard 云端控制台</h1>
      <div class="sub">实时查看 ESP32-P4 是否把照片、姿态、脑电、风险分和 AI 分析上传成功。自动刷新 5 秒。当前时间：{html.escape(now)}</div>
    </div>
    <div class="status"><span class="pill">服务在线</span><span class="pill">大模型：{dashscope_text}</span><span class="pill">飞书：{feishu_text}</span></div>
  </header>
  <section class="grid">
    <div class="panel"><div class="metric-label">最近事件数</div><div class="metric">{len(events)}</div></div>
    <div class="panel"><div class="metric-label">最新事件</div><div class="metric">{latest_id}</div></div>
    <div class="panel"><div class="metric-label">最新图片帧</div><div class="metric">{latest_frames}</div></div>
    <div class="panel"><div class="metric-label">调试接口</div><div><a href="/api/events">/api/events</a><br><a href="/debug/log_tail">/debug/log_tail</a></div></div>
    <div class="panel wide">
      <div class="section-title">最近上传事件</div>
      <table>
        <thead><tr><th>事件</th><th>AI报告</th><th>风险</th><th>图片</th><th>快照</th><th>姿态</th><th>脑电</th><th>AI</th><th>飞书</th><th>操作</th></tr></thead>
        <tbody>{table_body}</tbody>
      </table>
    </div>
    {latest_detail}
  </section>
</main>
</body>
</html>"""


@app.get("/health")
def health(format: str = ""):
    status = {
        "ok": True,
        "dashscope_configured": bool(DASHSCOPE_API_KEY),
        "feishu_configured": bool(FEISHU_WEBHOOK_URL),
        "version": app.version,
        "latest_event": _latest_event_summary(),
    }
    if format.lower() == "json":
        return JSONResponse(status)
    return HTMLResponse(_cloud_console_html(status))

    latest = status["latest_event"]
    latest_event_id = html.escape(str(latest.get("event_id") or "暂无事件"))
    latest_time = "暂无"
    if latest.get("mtime"):
        latest_time = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(latest["mtime"])))
    latest_links = ""
    if latest.get("event_id"):
        event_id = html.escape(str(latest["event_id"]))
        latest_links = f"""
            <a class="btn" href="/events/{event_id}/video">查看事件视频</a>
            <a class="btn secondary" href="/events/{event_id}/snapshot.jpg">查看快照</a>
            <a class="btn secondary" href="/events/{event_id}/video-strip">查看帧预览</a>
        """
    else:
        latest_links = '<span class="muted">触发一次报警后，这里会出现视频和照片入口。</span>'

    dashscope_badge = "ok" if status["dashscope_configured"] else "warn"
    feishu_badge = "ok" if status["feishu_configured"] else "warn"
    html_body = f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="15">
  <title>BodyGuard 云端状态</title>
  <style>
    :root {{
      color-scheme: light;
      --bg: #f6f7fb;
      --panel: #ffffff;
      --text: #172033;
      --muted: #697386;
      --line: #e5e9f2;
      --green: #0f9f6e;
      --orange: #d97706;
      --blue: #2563eb;
      --red: #dc2626;
    }}
    * {{ box-sizing: border-box; }}
    body {{
      margin: 0;
      min-height: 100vh;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif;
      background: var(--bg);
      color: var(--text);
    }}
    .wrap {{
      width: min(1040px, calc(100% - 32px));
      margin: 0 auto;
      padding: 28px 0 42px;
    }}
    header {{
      display: flex;
      justify-content: space-between;
      gap: 16px;
      align-items: flex-start;
      margin-bottom: 18px;
    }}
    h1 {{
      margin: 0 0 8px;
      font-size: 28px;
      line-height: 1.2;
      letter-spacing: 0;
    }}
    .subtitle {{ color: var(--muted); font-size: 14px; }}
    .pill {{
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 8px 12px;
      border-radius: 999px;
      background: #e9f9f2;
      color: var(--green);
      font-weight: 700;
      white-space: nowrap;
    }}
    .dot {{
      width: 9px;
      height: 9px;
      border-radius: 50%;
      background: currentColor;
    }}
    .grid {{
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 14px;
      margin-bottom: 14px;
    }}
    .panel {{
      background: var(--panel);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 18px;
      box-shadow: 0 8px 24px rgba(20, 31, 51, 0.06);
    }}
    .label {{ color: var(--muted); font-size: 13px; margin-bottom: 8px; }}
    .value {{ font-size: 24px; font-weight: 800; line-height: 1.2; }}
    .badge {{
      display: inline-flex;
      padding: 5px 9px;
      border-radius: 999px;
      font-size: 13px;
      font-weight: 700;
      margin-top: 12px;
    }}
    .badge.ok {{ background: #e9f9f2; color: var(--green); }}
    .badge.warn {{ background: #fff7ed; color: var(--orange); }}
    .wide {{ grid-column: 1 / -1; }}
    .actions {{
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 14px;
    }}
    .btn {{
      display: inline-flex;
      align-items: center;
      justify-content: center;
      min-height: 38px;
      padding: 9px 12px;
      border-radius: 7px;
      background: var(--blue);
      color: #fff;
      text-decoration: none;
      font-weight: 700;
      font-size: 14px;
    }}
    .btn.secondary {{
      background: #eef2ff;
      color: #263a7a;
    }}
    .muted {{ color: var(--muted); }}
    code {{
      background: #f1f5f9;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 2px 6px;
      font-family: ui-monospace, SFMono-Regular, Consolas, monospace;
      font-size: 13px;
    }}
    .checklist {{
      display: grid;
      gap: 8px;
      margin-top: 10px;
      color: var(--muted);
      font-size: 14px;
    }}
    @media (max-width: 760px) {{
      header {{ flex-direction: column; }}
      .grid {{ grid-template-columns: 1fr; }}
      h1 {{ font-size: 24px; }}
    }}
  </style>
</head>
<body>
  <main class="wrap">
    <header>
      <div>
        <h1>BodyGuard 云端 AI 监护状态</h1>
        <div class="subtitle">自动刷新：15 秒 · 当前时间：{time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())}</div>
      </div>
      <div class="pill"><span class="dot"></span> 服务在线</div>
    </header>

    <section class="grid">
      <div class="panel">
        <div class="label">云端服务</div>
        <div class="value">正常</div>
        <span class="badge ok">HTTP 200</span>
      </div>
      <div class="panel">
        <div class="label">通义千问多模态</div>
        <div class="value">{"已配置" if status["dashscope_configured"] else "未配置"}</div>
        <span class="badge {dashscope_badge}">{"可调用" if status["dashscope_configured"] else "需要配置"}</span>
      </div>
      <div class="panel">
        <div class="label">飞书机器人</div>
        <div class="value">{"已配置" if status["feishu_configured"] else "未配置"}</div>
        <span class="badge {feishu_badge}">{"可推送" if status["feishu_configured"] else "需要配置"}</span>
      </div>

      <div class="panel wide">
        <div class="label">最近事件</div>
        <div class="value">{latest_event_id}</div>
        <div class="checklist">
          <div>更新时间：{html.escape(latest_time)}</div>
          <div>关键帧数量：{int(latest.get("frame_count") or 0)} 张</div>
          <div>快照状态：{"已生成" if latest.get("has_snapshot") else "暂无"}</div>
        </div>
        <div class="actions">{latest_links}</div>
      </div>

      <div class="panel wide">
        <div class="label">调试入口</div>
        <div class="actions">
          <a class="btn" href="/debug/log_tail">实时日志</a>
          <a class="btn secondary" href="/health?format=json">JSON 状态</a>
          <a class="btn secondary" href="/docs">接口文档</a>
        </div>
        <div class="checklist">
          <div>报警上传后，在日志里看 <code>dashscope success</code> 代表大模型调用成功。</div>
          <div>看到 <code>feishu webhook status=200</code> 代表飞书推送成功。</div>
          <div>看到 <code>event upload received</code> 代表 ESP32-P4 已把事件传到云端。</div>
        </div>
      </div>
    </section>
  </main>
</body>
</html>"""
    return HTMLResponse(html_body)


@app.get("/debug/log_tail")
def log_tail():
    path = LOG_DIR / "bodyguard.log"
    if not path.exists():
        return PlainTextResponse("", media_type="text/plain; charset=utf-8")
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()[-120:]
    return PlainTextResponse("\n".join(lines), media_type="text/plain; charset=utf-8")


@app.get("/api/events")
def api_events():
    return JSONResponse({"ok": True, "events": _recent_events(50)})


@app.get("/api/events/{event_id}")
def api_event_detail(event_id: str):
    detail = _event_detail(event_id)
    if not detail["mtime"]:
        return JSONResponse({"ok": False, "msg": "event not found"}, status_code=404)
    return JSONResponse({"ok": True, "event": detail})


@app.post("/debug/test_feishu")
def test_feishu():
    event_id = f"TEST-{int(time.time())}"
    report = "综合判断:关注异常;处理建议:及时查看"
    send_feishu(event_id, "AI_MONITOR", report, f"{PUBLIC_BASE_URL}/health", f"{PUBLIC_BASE_URL}/health", f"{PUBLIC_BASE_URL}/health", "dashscope")
    return {"ok": True, "event_id": event_id, "report": report}


@app.post("/v1/data")
async def upload_data(payload: dict):
    event_id = _safe_event_id(str(payload.get("event_id", "unknown")))
    event_dir = _event_dir(event_id)
    event_dir.mkdir(parents=True, exist_ok=True)
    (event_dir / "data.json").write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    return {"code": 200, "event_id": event_id, "status": "ok"}


@app.post("/v1/ai/event_upload")
async def event_upload(
    images: List[UploadFile] = File(default=[]),
    snapshot: Optional[UploadFile] = File(default=None),
    event_id: str = Form(...),
    prompt: str = Form(""),
    sensor_json: str = Form("{}"),
    risk_pre: int = Form(0),
    risk_final: int = Form(0),
    confidence: int = Form(0),
    posture: int = Form(0),
    timestamp: str = Form(""),
):
    event_id = _safe_event_id(event_id)
    upload_seq = str(int(time.time() * 1000))
    logger.info(
        "event upload received: event_id=%s upload_seq=%s risk_pre=%s risk_final=%s confidence=%s posture=%s images=%s snapshot=%s sensor_len=%s",
        event_id,
        upload_seq,
        risk_pre,
        risk_final,
        confidence,
        posture,
        len(images),
        snapshot is not None,
        len(sensor_json or ""),
    )
    event_dir = _event_dir(event_id)
    image_dir = event_dir / "images"
    image_dir.mkdir(parents=True, exist_ok=True)

    try:
        sensor_payload = json.loads(sensor_json) if sensor_json else {}
    except Exception as exc:
        logger.warning("sensor_json parse failed event_id=%s error=%s raw=%s", event_id, exc, (sensor_json or "")[:500])
        sensor_payload = {"parse_error": str(exc), "raw": sensor_json}
    (event_dir / "sensor.json").write_text(json.dumps(sensor_payload, ensure_ascii=False, indent=2), encoding="utf-8")

    snapshot_path: Optional[Path] = None
    if snapshot is not None:
        snapshot_path = event_dir / f"snapshot_{upload_seq}.jpg"
        snapshot_path.write_bytes(await snapshot.read())
        (event_dir / "snapshot.jpg").write_bytes(snapshot_path.read_bytes())

    ai_prompt = prompt
    if sensor_payload:
        ai_prompt = (
            f"{prompt or ''}\n"
            f"传感器摘要JSON: {json.dumps(sensor_payload, ensure_ascii=False)[:1200]}"
        )

    for idx, image in enumerate(images):
        image_data = await image.read()
        (image_dir / f"{upload_seq}_{idx:03d}.jpg").write_bytes(image_data)
        (image_dir / f"{idx:03d}.jpg").write_bytes(image_data)

    frame_paths = _event_frame_paths(event_id)
    frame_count = len(frame_paths)
    ai_image_path = _select_ai_image(snapshot_path, frame_paths)
    level, tts_code, report, ai_source = call_dashscope(ai_image_path, ai_prompt, risk_pre, posture)
    snapshot_url = _url_with_version(f"{PUBLIC_BASE_URL}/events/{event_id}/snapshot.jpg", upload_seq) if snapshot_path and snapshot_path.exists() else ""
    video_url = _url_with_version(_event_video_url(event_id), upload_seq) if frame_count > 0 else ""
    strip_url = _url_with_version(_event_strip_url(event_id), upload_seq) if frame_count > 0 else ""
    meta = {
        "event_id": event_id,
        "upload_seq": upload_seq,
        "timestamp": timestamp,
        "risk_pre": risk_pre,
        "risk_final": risk_final,
        "confidence": confidence,
        "posture": posture,
        "sensor_json_received": bool(sensor_payload),
        "sensor_count": sensor_payload.get("sensor_count", 0),
        "eeg_count": sensor_payload.get("eeg_count", 0),
        "risk_count": sensor_payload.get("risk_count", 0),
        "ai_result": {"AI_NORMAL": 1, "AI_MONITOR": 2, "AI_DANGER": 3}[level],
        "level": level,
        "tts_code": tts_code,
        "report": report,
        "snapshot_url": snapshot_url,
        "video_url": video_url,
        "mjpeg_url": _event_mjpeg_url(event_id) if frame_count > 0 else "",
        "strip_url": strip_url,
        "frame_count": frame_count,
        "ai_image": ai_image_path.name if ai_image_path else "",
        "ai_source": ai_source,
        **_learning_feedback(level, risk_pre, posture),
    }
    (event_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    send_feishu(
        event_id,
        level,
        report,
        snapshot_url,
        video_url,
        strip_url,
        ai_source,
        sensor_payload=sensor_payload,
        risk_pre=risk_pre,
        risk_final=risk_final,
        confidence=confidence,
        posture=posture,
    )
    meta["feishu_status"] = bool(FEISHU_WEBHOOK_URL)
    (event_dir / "meta.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
    return JSONResponse(meta)


@app.get("/events/{event_id}/snapshot.jpg")
def event_snapshot(event_id: str):
    path = _event_dir(event_id) / "snapshot.jpg"
    if not _valid_model_image(path):
        frames = _event_frame_paths(event_id)
        if frames:
            return FileResponse(frames[0], media_type="image/jpeg")
        return JSONResponse({"code": 404, "msg": "snapshot not found"}, status_code=404)
    return FileResponse(path, media_type="image/jpeg")


@app.head("/events/{event_id}/snapshot.jpg")
def event_snapshot_head(event_id: str):
    path = _event_dir(event_id) / "snapshot.jpg"
    if not _valid_model_image(path) and not _event_frame_paths(event_id):
        return JSONResponse({"code": 404}, status_code=404)
    return PlainTextResponse("", media_type="image/jpeg")


@app.get("/events/{event_id}/frames/{frame_name}")
def event_frame(event_id: str, frame_name: str):
    safe_name = "".join(ch for ch in frame_name if ch.isdigit() or ch in (".", "_", "-"))
    path = _event_dir(event_id) / "images" / safe_name
    if not _valid_model_image(path):
        return JSONResponse({"code": 404, "msg": "frame not found"}, status_code=404)
    return FileResponse(path, media_type="image/jpeg")


@app.head("/events/{event_id}/frames/{frame_name}")
def event_frame_head(event_id: str, frame_name: str):
    safe_name = "".join(ch for ch in frame_name if ch.isdigit() or ch in (".", "_", "-"))
    path = _event_dir(event_id) / "images" / safe_name
    if not _valid_model_image(path):
        return JSONResponse({"code": 404}, status_code=404)
    return PlainTextResponse("", media_type="image/jpeg")


@app.get("/events/{event_id}/video.mjpeg")
def event_video_mjpeg(event_id: str):
    frame_paths = _event_frame_paths(event_id)
    if not frame_paths:
        return JSONResponse({"code": 404, "msg": "event video frames not found"}, status_code=404)

    def frame_stream():
        while True:
            for path in frame_paths:
                data = path.read_bytes()
                yield b"--bodyguard\r\n"
                yield b"Content-Type: image/jpeg\r\n"
                yield f"Content-Length: {len(data)}\r\n\r\n".encode("ascii")
                yield data
                yield b"\r\n"
                time.sleep(0.10)

    return StreamingResponse(frame_stream(), media_type="multipart/x-mixed-replace; boundary=bodyguard")


@app.head("/events/{event_id}/video.mjpeg")
def event_video_mjpeg_head(event_id: str):
    if not _event_frame_paths(event_id):
        return JSONResponse({"code": 404}, status_code=404)
    return PlainTextResponse("", media_type="multipart/x-mixed-replace; boundary=bodyguard")


def _load_meta(event_id: str) -> dict:
    meta_path = _event_dir(event_id) / "meta.json"
    if not meta_path.exists():
        return {}
    try:
        return json.loads(meta_path.read_text(encoding="utf-8"))
    except Exception:
        return {}


@app.get("/events/{event_id}/video")
def event_video_page(event_id: str):
    event_id = _safe_event_id(event_id)
    frame_paths = _event_frame_paths(event_id)
    if not frame_paths:
        return JSONResponse({"code": 404, "msg": "event video frames not found"}, status_code=404)

    frames = [_event_frame_url(event_id, path.name) for path in frame_paths]
    meta = _load_meta(event_id)
    report = html.escape(str(meta.get("report", "等待AI分析...")))
    level = html.escape(str(meta.get("level", "UNKNOWN")))
    first = frames[0]
    frames_json = json.dumps(frames, ensure_ascii=False)
    html_body = f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BodyGuard 事件视频 {event_id}</title>
  <style>
    body {{ margin:0; font-family:Arial,sans-serif; background:#101418; color:#eef3f7; }}
    main {{ max-width:980px; margin:0 auto; padding:14px; }}
    img {{ width:100%; max-height:72vh; object-fit:contain; background:#000; border-radius:6px; }}
    .meta {{ margin-top:12px; line-height:1.7; color:#cbd5df; }}
    .badge {{ color:#fff; background:#c2410c; padding:2px 8px; border-radius:4px; }}
    a {{ color:#93c5fd; word-break:break-all; }}
  </style>
</head>
<body>
<main>
  <img id="player" src="{first}" alt="BodyGuard event video">
  <noscript><p><a href="{_event_strip_url(event_id)}">打开静态帧预览</a></p></noscript>
  <div class="meta">
    <div><b>事件ID：</b>{event_id}</div>
    <div><b>AI报告：</b>{report}</div>
    <div><b>风险等级：</b><span class="badge">{level}</span></div>
    <div><b>帧数：</b>{len(frames)}，按 10fps 循环播放</div>
    <div><b>静态帧预览：</b><a href="{_event_strip_url(event_id)}">{_event_strip_url(event_id)}</a></div>
  </div>
</main>
<script>
const frames = {frames_json};
let i = 0;
const img = document.getElementById('player');
setInterval(() => {{
  i = (i + 1) % frames.length;
  img.src = frames[i] + '?t=' + Date.now();
}}, 100);
</script>
</body>
</html>"""
    return HTMLResponse(html_body)


@app.head("/events/{event_id}/video")
def event_video_page_head(event_id: str):
    if not _event_frame_paths(event_id):
        return JSONResponse({"code": 404}, status_code=404)
    return PlainTextResponse("", media_type="text/html; charset=utf-8")


@app.get("/events/{event_id}/video-strip")
def event_video_strip(event_id: str):
    event_id = _safe_event_id(event_id)
    frame_paths = _event_frame_paths(event_id)
    if not frame_paths:
        return JSONResponse({"code": 404, "msg": "event video frames not found"}, status_code=404)
    meta = _load_meta(event_id)
    imgs = "\n".join(
        f'<a href="{_event_frame_url(event_id, p.name)}"><img src="{_event_frame_url(event_id, p.name)}" loading="lazy"></a>'
        for p in frame_paths
    )
    html_body = f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>BodyGuard 静态帧预览 {event_id}</title>
  <style>
    body {{ margin:0; font-family:Arial,sans-serif; background:#101418; color:#eef3f7; }}
    main {{ max-width:980px; margin:0 auto; padding:14px; }}
    img {{ width:100%; margin:8px 0; background:#000; border-radius:6px; }}
    .meta {{ line-height:1.7; color:#cbd5df; }}
  </style>
</head>
<body>
<main>
  <div class="meta">
    <div><b>事件ID：</b>{event_id}</div>
    <div><b>AI报告：</b>{html.escape(str(meta.get("report", "等待AI分析...")))}</div>
    <div><b>帧数：</b>{len(frame_paths)}</div>
  </div>
  {imgs}
</main>
</body>
</html>"""
    return HTMLResponse(html_body)


@app.head("/events/{event_id}/video-strip")
def event_video_strip_head(event_id: str):
    if not _event_frame_paths(event_id):
        return JSONResponse({"code": 404}, status_code=404)
    return PlainTextResponse("", media_type="text/html; charset=utf-8")
