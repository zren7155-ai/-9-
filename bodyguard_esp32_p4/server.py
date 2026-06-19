import dashscope
from flask import Flask, request, jsonify
import os
from datetime import datetime
import json

app = Flask(__name__)

# ====================== 【配置区】 ======================
# 你的阿里云 DashScope API Key
dashscope.api_key = "sk-68d2b17c424742daa779aa071b930817"

# 自动创建本地图片保存目录，用于法律溯源与留档
UPLOAD_FOLDER = "uploads"
if not os.path.exists(UPLOAD_FOLDER):
    os.makedirs(UPLOAD_FOLDER)

# ====================== 【逻辑区】 ======================

@app.route('/upload', methods=['POST'])
def handle_upload():
    """
    处理 ESP32-P4 上传的图片并进行多模态 AI 风险分析
    """
    # 1. 接收硬件端数据
    img_data = request.data
    # 尝试从 Header 获取 event-id，如果硬件没传则生成一个临时 ID
    event_id = request.headers.get("event-id", datetime.now().strftime("%Y%m%d_%H%M%S"))
    timestamp = request.headers.get("timestamp", str(datetime.now().timestamp()))

    if not img_data:
        print(f"[{event_id}] 错误：未接收到图片数据")
        return jsonify({"code": 400, "event_id": event_id, "msg": "no image data"}), 400

    # 2. 图片本地存档（工业级取证逻辑）
    img_path = os.path.join(UPLOAD_FOLDER, f"event_{event_id}.jpg")
    with open(img_path, "wb") as f:
        f.write(img_data)
    print(f"[{event_id}] 图片已存档: {img_path}")

    # 3. 构造三模态 AI 判研提示词 (融合视觉、脑电、姿态概念)
    # 强制要求返回 JSON 格式，方便 ESP32-P4 解析
    messages = [
        {
            "role": "user",
            "content": [
                {"image": f"file://{os.path.abspath(img_path)}"},
                {"text": "你是专业的人体健康与姿态安全AI监测引擎。请结合图像分析人体姿态（坐/站/歪斜）、疲劳风险及危险动作。请严格输出JSON格式，禁止任何解释性文字：{\"risk_score\":0-100,\"risk_level\":\"低/中/高\",\"posture_state\":\"标准坐姿/不良坐姿/站姿/躺卧/躯干歪斜\",\"eeg_state\":\"正常/疲劳/困倦\",\"risk_type\":\"无风险/姿态异常/疲劳风险/危险姿态\",\"description\":\"风险描述\",\"suggestion\":\"改善建议\"}"}
            ]
        }
    ]

    # 4. 调用阿里云视觉大模型 qwen-vl-max
    try:
        print(f"[{event_id}] 正在请求阿里云 AI 判研...")
        response = dashscope.MultiModalConversation.call(
            model='qwen-vl-max',
            messages=messages
        )

        if response.status_code == 200:
            # 获取 AI 返回的文本
            raw_content = response.output.choices[0].message.content[0]['text']
            
            # 容错处理：去除可能存在的 Markdown 格式标记
            clean_json = raw_content.replace("```json", "").replace("```", "").strip()
            
            try:
                ai_report = json.loads(clean_json)
                print(f"[{event_id}] AI 判研完成: {ai_report['risk_level']}风险")
                
                return jsonify({
                    "code": 200,
                    "event_id": event_id,
                    "timestamp": timestamp,
                    "ai_risk_report": ai_report
                })
            except json.JSONDecodeError:
                print(f"[{event_id}] JSON 解析失败，原始输出: {raw_content}")
                return jsonify({"code": 500, "error": "AI返回格式异常", "raw": raw_content}), 500
        else:
            print(f"[{event_id}] API 调用失败: {response.code} - {response.message}")
            return jsonify({"code": 500, "error": response.message}), 500

    except Exception as e:
        print(f"[{event_id}] 系统异常: {str(e)}")
        return jsonify({"code": 500, "error": str(e)}), 500

@app.route('/data', methods=['POST'])
def receive_data():
    """
    独立接收传感器数据接口（脑电/MPU6050）
    """
    try:
        data = request.get_json()
        event_id = data.get("event_id", "unknown")
        print(f"[{event_id}] 接收到传感器数据: {data}")
        return jsonify({"code": 200, "status": "success", "event_id": event_id})
    except Exception as e:
        return jsonify({"code": 400, "error": str(e)}), 400

if __name__ == '__main__':
    # host='0.0.0.0' 允许局域网内的 ESP32-P4 访问
    # 建议关闭 debug 模式以获得更稳定的连接
    print("法律 AI 助手后端已启动，等待 ESP32-P4 连接...")
    app.run(host='0.0.0.0', port=5000, debug=False)