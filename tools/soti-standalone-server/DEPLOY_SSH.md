# SoTi 多模式（A1–A5）— ECS SSH 部署

在**你本机**（Windows，项目已下载）和 **ECS**（阿里云，示例 IP `8.154.20.8`）上按顺序执行。  
未备案时设备仍用 **公网 IP** 访问，不要用域名作 `Host`（见 `soti_config.h`）。

## 1. 本机：上传代码到 ECS

把 `你的ECS公网IP` 换成实际 IP；用户一般为 `root`。

```powershell
cd C:\Users\1\Desktop\WT99P4C5-S1-master

scp tools\soti-standalone-server\soti_upload_server.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\soti_modes.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\soti_answer_sanitize.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\parent_store.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\parent_routes.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\album_preprocess.py root@8.154.20.8:/opt/soti/
scp tools\soti-standalone-server\print_preprocess.py root@8.154.20.8:/opt/soti/
ssh root@8.154.20.8 "mkdir -p /opt/soti/static"
scp -r tools\soti-standalone-server\static\parent root@8.154.20.8:/opt/soti/static/
scp -r tools\soti-standalone-server\ecs root@8.154.20.8:/tmp/soti-ecs/
scp tools\soti-standalone-server\nginx-soti-upload-site.conf root@8.154.20.8:/tmp/soti-ecs/
```

## 2. SSH 登录 ECS

```powershell
ssh root@8.154.20.8
```

## 3. 配置环境变量（首次或改 Key 时）

```bash
# 若还没有配置文件
cp /tmp/soti-ecs/soti-upload.env.example /etc/soti-upload.env
chmod 600 /etc/soti-upload.env
nano /etc/soti-upload.env
```

必填：

```bash
DOUBAO_API_KEY=你的火山方舟Key
DOUBAO_MODEL=ep-xxxxxxxx          # 方舟「推理接入点」ID，必须 ep- 开头
UPLOAD_TOKEN=esp32souti           # 与固件 soti_config.h 里 SOTI_R2_UPLOAD_TOKEN 一致
```

保存后执行部署脚本：

```bash
bash /tmp/soti-ecs/deploy-on-ecs.sh
```

成功时会打印 `http://127.0.0.1:3000/upload/health` 的 JSON，且含 `"modes"` 字段。

## 4. 公网自检（在 ECS 或本机）

```bash
# 健康检查（无需 Token）
curl -sS http://127.0.0.1:3000/upload/health
curl -sS http://8.154.20.8/upload/health

# 每日一句（需 Token，与 UPLOAD_TOKEN 一致）
curl -sS -H "Authorization: Bearer esp32souti" http://8.154.20.8/upload/daily
```

若域名返回 **403 Beaver**，说明备案拦截，继续用 IP 即可。

## 5. 家长页 + Nginx（`/parent/` 若 404 必做）

```bash
bash /tmp/soti-ecs/deploy-parent-to-ecs.sh
```

或手动：`cp /tmp/soti-ecs/nginx-soti-upload-site.conf /etc/nginx/sites-available/soti-upload`，`ln -sf` 到 `sites-enabled`，`nginx -t && systemctl reload nginx`。

浏览器打开 **`http://8.154.20.8/parent/`**（勿只用根路径 `/`）。

## 6. 设备固件

本机改完固件后需重新编译烧录：

1. 确认 `components/apps/SoTi/soti_config.h` 中  
   `SOTI_R2_WORKER_USE_ECS_PUBLIC_IP` 为 `1`，`SOTI_R2_WORKER_URL_ECS_PUBLIC` 为你的 EIP。
2. 若改了 `symbols.txt`，运行：`python tools/gen_lv_font_ui_zh.py`
3. `idf.py build flash`

搜题 App 右侧：**滚轮选模式**（搜题/翻译/讲解/批改/摘要）→ 快门上传；**每日** 按钮拉取 A5。

## 7. 模式与 API 对照

| 功能 | 设备操作 | 服务器 |
|------|----------|--------|
| A1 拍照翻译 | 滚轮「翻译」+ 拍照 | `POST /upload?mode=translate&st=…` |
| A2 错题讲解 | 「讲解」 | `mode=tutor` |
| A3 作业批改 | 「批改」 | `mode=grade` |
| A4 文档摘要 | 「摘要」 | `mode=summary` |
| A5 每日一句 | 「每日」按钮 | `GET /upload/daily` |
| 原搜题 | 「搜题」 | `mode=solve`（默认） |

## 8. 常用运维命令

```bash
systemctl restart soti-upload
journalctl -u soti-upload -f --no-pager
```

每日一句缓存文件默认：`/opt/soti/daily_cache.json`（同一天只调一次豆包）。

## 9. 仅更新 Python、不重装 systemd

```bash
# 本机再次 scp 两个 py 文件后，在 ECS：
python3 -m py_compile /opt/soti/soti_upload_server.py /opt/soti/soti_modes.py
systemctl restart soti-upload
```
