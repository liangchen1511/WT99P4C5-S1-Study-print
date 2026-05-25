# SoTi 搜题（设备端）

WT99P4C5-S1 上拍照 → JPEG 上传 → 豆包视觉识题 → 屏幕显示答案。

## 相机与答案页

上传开始（加载遮罩）至用户点击 **返回** 之前：

- `Camera::stopPreviewStreamingIfRunning()` + 关闭帧处理
- 取消预览 canvas 绑定，隐藏预览与快门侧栏
- 串口不再刷 `camera: 13.x FPS`

点击 **返回** 后恢复预览。

## 答案界面说明

设备 **不渲染 LaTeX**，只显示纯文本。服务器在返回 JSON 前用 `soti_answer_sanitize.py` 清洗一遍；固件在 `soti_answer_format.cpp` 里再做兜底简化：

| 服务器返回 | 屏幕显示 |
|-----------|---------|
| `### 题目:` | `【题目:】` |
| `**粗体**` | 粗体标记去掉，保留文字 |
| `\( 712 - 48 = 664 \)` | `712 - 48 = 664` |
| `\boldsymbol{664}` | `664`（重点数字，无加粗字体） |
| `“ ”` 弯引号 | `"`（避免缺字方框） |

若仍出现 **方框 □**，说明该字不在字库子集内，需补字后重新生成字体（见下）。

## 字库

- 组件：`components/lv_font_ui_zh/`
- 配置：`symbols.txt`（UI 文案）、`symbols_math.txt`（数学符号）、`symbols_extra.txt`（AI 回复补充字）、`symbols_hans_common.txt`（常用汉字）
- 生成：

```bash
python tools/gen_lv_font_ui_zh.py
```

需要本机已安装 **Node.js**（`npx lv_font_conv`）。生成后重新 `idf.py build flash`。

## 上传配置

编辑 `soti_config.h`：

- `SOTI_R2_WORKER_URL`：上传地址（分片 query：`?st=init|part|commit`）
- `SOTI_R2_UPLOAD_TOKEN`：与服务器 `SOTI_UPLOAD_TOKEN` 一致
- `SOTI_R2_UPLOAD_USE_ECS_PUBLIC_IP`：备案未完成时可先用 ECS 公网 IP

服务器部署见：`tools/soti-standalone-server/README.md`

## 相关源文件

| 文件 | 作用 |
|------|------|
| `SoTi.cpp` | 预览、快门、答案页 UI |
| `soti_r2_upload.cpp` | 分片 HTTP 上传 |
| `soti_answer_format.cpp` | Markdown/LaTeX → 屏显文本 |
| `soti_config.h` | URL / Token |
