# SoTi 上传服务（ECS / 本机）

Python 服务接收 ESP32 分片 JPEG，二值化后调用火山 **豆包** 视觉 API，返回 JSON `{ "answer": "..." }`。

**多模式 A1–A5**（`?mode=` / `GET /upload/daily`）与 **SSH 部署步骤** 见 [DEPLOY_SSH.md](./DEPLOY_SSH.md)。

## 家长控制台

浏览器打开 **`http://<公网IP>/parent/`**（与 `/upload` 同机）。设备码 `WT99-DEMO-01`，默认密码 `parent2025`。支持四套配色（顶栏文字）+ 浅色/深色/纯黑主题（◐）；三条杠全屏导航。详见 [PARENT_PORTAL.md](./PARENT_PORTAL.md)。

**部署家长端 UI：** 本机 `powershell -File tools/soti-standalone-server/deploy-parent-ui.ps1`

## 快速检查

```bash
curl -s http://127.0.0.1:3000/upload/health
curl -s http://127.0.0.1:3000/parent/
curl -s http://8.154.20.8/upload/health
```

## 环境变量

复制 `ecs/soti-upload.env.example` 为 `/etc/soti-upload.env`（或本机 `.env`）：

| 变量 | 说明 |
|------|------|
| `SOTI_UPLOAD_TOKEN` | 与固件 `SOTI_R2_UPLOAD_TOKEN` 一致 |
| `DOUBAO_API_KEY` | 火山方舟 API Key |
| `DOUBAO_MODEL` | **端点 ID** `ep-xxxx`，不是公开模型名 |
| `DOUBAO_VISION_PROMPT` | 可选，识题提示词 |

修改后：`sudo systemctl restart soti-upload`

## 分片协议（与固件一致）

同一 `POST` URL，Query 区分阶段（`Content-Type: image/jpeg`）：

1. `?st=init&total=N` — body 空，返回 `session_id`
2. `?st=part&sid=…&off=…` — body 为 JPEG 片段（建议 ≤4KB）
3. `?st=commit&sid=…` — body 空，触发识题，返回 JSON

也支持路径式 `/upload/init` 等（见 `soti_upload_server.py`）。

## Nginx / 域名备案

- 未备案域名经阿里云可能被 **403（Beaver）** 拦截。
- 包年包月 ECS 可做 ICP 备案后使用 `souti.novaio.top`。
- 过渡期：`server_name` 需包含公网 IP，`Host: 8.154.20.8` 可访问；`Host: 域名` 可能仍 403。

配置示例：`nginx-soti-upload-site.conf`

## 部署

```bash
# 在 ECS 上
bash ecs/deploy-on-ecs.sh
```

日志：`journalctl -u soti-upload -f`

## 答案格式与设备显示

豆包常返回 **Markdown + LaTeX**。设备端在 `components/apps/SoTi/soti_answer_format.cpp` 转为纯文本；**不会**渲染 `\boldsymbol` 等命令，仅保留括号内数字/公式字符。

若答案含罕见汉字出现方框，在 `components/lv_font_ui_zh/symbols*.txt` 补字后运行 `python tools/gen_lv_font_ui_zh.py` 并重新烧录固件。
