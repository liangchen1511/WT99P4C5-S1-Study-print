# 家长控制台（学习守护）

与 SoTi 上传服务同进程部署，浏览器访问 **`http://<ECS公网IP>/parent/`**（未备案勿用域名 Host）。

## 相册传图

- 侧栏 **相册传图**：家长上传 JPG/PNG/WebP；ECS 用 Pillow **自动转 JPG 并缩小至 ≤1280×960**（对齐设备相册解码上限），再入队下发。
- 设备每约 12 秒轮询 `GET /parent/api/album/poll`，分片下载后写入 `/sdcard`。
- API：`POST /parent/api/album/upload`（家长登录）、`GET /parent/api/album/file/{id}?off=&len=`、`POST /parent/api/album/ack`（设备 Token）。
- HEIC 暂不支持（需第二期 `pillow-heif`）。

## 远程打印

- 侧栏 **远程打印**：家长上传 **图片**（JPG/PNG/WebP）或 **文字**（≤2000 字）；图片经 `print_preprocess.py` 缩放到热敏宽度 **≤384px**、高 ≤900，并做对比增强（保留蓝色线条）。
- 设备后台约 **12 秒**轮询 `GET /parent/api/print/poll`；在桌面 **打印** App 中查看队列并点 **打印** 确认出纸（**不写入 SD 相册**）。
- 图片 job：分片 `GET /parent/api/print/file/{id}?off=&len=` → SD 临时文件 → `escpos_printer_print_jpeg_file` → `POST /parent/api/print/ack`。
- 文字 job：`GET /parent/api/print/text/{id}` → `escpos_printer_print_utf8` → ack。
- API：`POST /parent/api/print/upload`（家长 Bearer 登录）、上述 poll/file/text/ack（设备 `UPLOAD_TOKEN` + `X-Device-Id`）。
- 环境变量（可选）：`PARENT_PRINT_MAX_WIDTH=384`、`PARENT_PRINT_MAX_PENDING=5`、`PARENT_PRINT_MAX_TEXT_CHARS=2000`。

## 亲子聊天

- 网页侧栏 **亲子聊天**：与家长端文字消息（轮询约 3 秒）。
- 板子桌面 **家长** App：孩子端界面；有未读时图标偏红，无未读偏蓝（约 12 秒刷新）。
- 消息在服务端按设备保留最近 **200 条**（环境变量 `PARENT_MAX_CHAT`）；设备界面只加载最近 50 条。
- API：`GET/POST /parent/api/chat/*`，鉴权与家长端登录或设备 `UPLOAD_TOKEN` 相同。

## 登录

- 设备码默认 `WT99-DEMO-01`；**记住本设备**（`parent_device`）与 **保存密码**（`parent_password` + `parent_token` 免登）可独立勾选，勾选框均在文字后方。

| 项 | 默认值 |
|----|--------|
| 设备码 | `WT99-DEMO-01`（与固件 `SOTI_DEVICE_ID` 一致） |
| 家长密码 | `parent2025`（环境变量 `PARENT_PASSWORD`） |

```bash
export PARENT_PASSWORD='你的强密码'
export PARENT_DEVICE_CODES='WT99-DEMO-01'   # 逗号分隔，大写
```

## Nginx（若浏览器见 `404 Not Found nginx/1.18.0`）

这是 **Nginx 未配置 `/parent` 反代**（不是 Python 报错）。在 ECS 上：

```bash
# 1) 确认 Python 本机可访问
curl -sI http://127.0.0.1:3000/parent/   # 须 HTTP/1.1 200

# 2) 若上一步 404，补传静态页与 parent_*.py 后 systemctl restart soti-upload

# 3) 更新 Nginx 并 reload（仓库配置含 location /parent/）
sudo cp /path/to/nginx-soti-upload-site.conf /etc/nginx/sites-available/soti-upload
sudo ln -sf /etc/nginx/sites-available/soti-upload /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t && sudo systemctl reload nginx

# 4) 经 Nginx 再测
curl -sI http://127.0.0.1/parent/ -H "Host: 你的公网IP"
```

一键脚本（需先把 `ecs/` 与 `nginx-soti-upload-site.conf` scp 到 `/tmp/soti-ecs/`）：

```bash
bash /tmp/soti-ecs/deploy-parent-to-ecs.sh
```

访问地址必须是 **`http://<公网IP>/parent/`**（带 `/parent/`），不要只打开 `http://<IP>/`（旧配置根路径会 404）。

## 界面与主题

- 正式页：`/parent/`（`index.html` + `styles.css` + `app.js`）— **Ultimate 视觉**（v3 圆角 shell、三条杠菜单、聚光灯悬停、数字滚动、GSAP 切页、四套配色×三主题）
- 设计预览 mock 仍保留：`design-preview-ultimate.html` 等（见下方历史预览列表）
- **设计预览 v3**（独立视觉稿，不引用 `styles.css`）：`/parent/design-preview（2）.html` + `design-preview-v3.css`
- **设计预览 Ultimate**（整合版，推荐）：`/parent/design-preview-ultimate.html` + `design-preview-ultimate.css/js`
  - **UI**：v3 双层圆角 shell/core + 三条杠全屏导航菜单
  - **交互**：Raycast 式卡片悬停（`.ix` 上浮 + `.spot` 聚光灯）+ 记录行侧滑
  - **动效**：v5 数字滚动 + `animation-timeline: view()` 列表入场 + v4 GSAP 切页 stagger + View Transitions 场景切换
  - **配色**（顶栏点击切换，各含浅色/深色/纯黑，**仅改色不改字体**）：暖琥珀（v2）、紫雾玻璃（v3）、青绿编辑（v4）、香槟金（v5）；◐ 切换明暗主题；写入 `parent_palette` / `parent_theme`
- **设计预览 v4**（gpt-taste 独立稿）：`/parent/design-preview-v4.html` + `design-preview-v4.css` + `design-preview-v4.js`（Cinematic 登录、密集 Bento、底栏 Dock、GSAP）
- **设计预览 v5**（高端前端技巧）：`/parent/design-preview-v5.html` + `design-preview-v5.css` + `design-preview-v5.js`
  - View Transitions API 场景切换
  - CSS `@property` 驱动动态网格背景 + SVG 噪点
  - 聚光灯卡片（鼠标追踪 radial-gradient）
  - 磁吸按钮、数字滚动计数、spring 抽屉/开关
  - `animation-timeline: view()` scroll-driven 列表入场
  - `@container` Bento 自适应、毛玻璃 Dock 滑块指示器
- **品牌炫技预览**（静态 mock，不改动 v2/v3/v4；共用 `design-preview-mock.js` + `design-preview-linear.css` 基座）：
  - **Linear**：`/parent/design-preview-linear.html` — 紫丁香 accent、侧栏导航、hairline 边框
  - **Bugatti**：`/parent/design-preview-bugatti.html` — 纯黑奢华、大写 display、outline pill 按钮
  - **PlayStation**：`/parent/design-preview-ps.html` — `#0070d1` 蓝带登录、顶栏 tab 导航
  - **Raycast**：`/parent/design-preview-raycast.html` — 命令面板侧栏、`⌘K` 提示、红色 hero 条
- **双主题**：默认跟随系统 `prefers-color-scheme`（映射为暖色 **深色**）；登录后点顶栏 **◐** 循环切换 **浅色 → 深色 → 纯黑**（写入 `localStorage` 键 `parent_theme`，取值 `light` / `dark` / `black`）
- **主色**：暖琥珀（亲子控制台），侧栏 SVG 图标
- **手机布局**（宽度 ≤768px，电脑端不变）：底部 Tab 栏（总览/记录/策略/聊天/相册/打印）替代三条杠菜单；顶栏紧凑；Bento 统计 2 列；聊天区 flex 满屏；记录抽屉底滑全宽；支持 `safe-area-inset`

## 功能

- **搜题历史**：每次识题成功写入 SQLite（`data/parent.db`），可选 320px 缩略图；详情抽屉底部可 **下载照片**（`搜题-{id}.jpg`）
- **管控策略**：应用开关 + 时段 JSON；设备每 5 分钟 `GET /parent/api/policy`

## 设备固件

- 上传 Header：`X-Device-Id: WT99-DEMO-01`
- 组件：`components/parent_policy/`（2048 / 音乐 / 视频启动拦截）；`components/parent_print_sync/`（远程打印 poll + 分片下载）

## 数据目录

默认 `tools/soti-standalone-server/data/`（可用 `PARENT_DATA_DIR` 覆盖）。最多保留 `PARENT_MAX_HISTORY=500` 条。

## 快速验证

```bash
curl -s http://127.0.0.1:3000/parent/api/demo | head
curl -s -X POST http://127.0.0.1:3000/parent/api/login \
  -H 'Content-Type: application/json' \
  -d '{"device_code":"WT99-DEMO-01","password":"parent2025"}'
```
