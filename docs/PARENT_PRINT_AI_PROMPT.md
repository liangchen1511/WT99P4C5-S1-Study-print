# 任务：WT99P4C5-S1 远程热敏 JPEG 图片打印（从零整理/重做）

> 复制本文件全文到新 AI 对话，并附加：「请按实施顺序，先做 host 工具对齐参数，再改固件」

## 项目与硬件

- 板子：**WT99P4C5-S1** = ESP32-**P4** 主控 + ESP32-**C5** WiFi 协处理器（**ESP-Hosted SDIO**，复位 GPIO**54**，SDIO CLK/CMD/D0-D3 = 18/19/14-17）
- **不要**把 `sdkconfig` 配成 SPI + ESP32-H2 + GPIO12，否则 WiFi 挂、`prt_sync` 无法 poll
- 打印机：**58mm 热敏**，有效宽度约 **384 dot**；当前走 **UART TTL**（非 USB 打印路径）
  - UART2 TX=**GPIO4** → 打印机 RX，RX=**GPIO5** ← 打印机 TX，**115200 8N1**
  - **CTS**：打印机丝印 CTS（RTS busy）→ **GPIO6** UART CTS 硬件流控
  - 机型接近 **EM5820H / 廉价 UART ESC/POS**，中文页 **ESC t / PAGE_936**
- P4 **无原生 WiFi**，远程打印依赖 C5 SDIO 链路正常

## 已有架构（可复用，勿推倒重来）

### 服务端（ECS `tools/soti-standalone-server/`）

- 家长端上传 → `POST /parent/api/print/upload`
- 设备轮询 → `GET /parent/api/print/poll`
- 分片下载 → `GET /parent/api/print/file/{id}?off=&len=`
- 确认 → `POST /parent/api/print/ack`
- 图片预处理：`tools/soti-standalone-server/print_preprocess.py`
  - 缩放到宽 ≤384、高 ≤900，JPEG quality ~85
  - `_thermal_enhance_rgb`：增强对比度，**蓝色笔画在深色底上提亮**（避免校徽蓝色变死黑块）
  - **不要**简单 harsh 二值化

### 设备端

- `components/parent_print_sync/`：HTTP 下载到 SD `/.print_{id}.part` → 调用 `escpos_printer_print_jpeg_file()` → ack
- 核心光栅：`components/usb_escpos_printer/escpos_jpeg_raster.c`
  - JPEG decode → RGB565 → gray downsample → mono threshold(140) → ESC/POS 发送
  - 链路类型：`escpos_jpeg_raster_set_link_kind(ESCPOS_LINK_UART)`（uart 驱动里设置）
- 配置：`components/usb_escpos_printer/Kconfig` + `sdkconfig.defaults`
- 上位机调参（优先用它试参再改固件）：`tools/printer_host_test/print_logo.py`

## 目标行为

1. 家长网页上传校徽/照片 → 设备 Print App 或后台 poll 收到 job → **完整打出**（高度不截断）
2. 图像**对齐**（无水平锯齿错位）、**无底部乱码**（二进制被当文字打出）
3. 允许迭代优化横纹，但首要：**全高 + 可辨认 + 无乱码**

## 实机已验证教训（必须遵守）

### GS v 0 分条（默认推荐 UART 路径）

- 廉价 UART 机 **单条 GS v 0 高度过大或连续发送** → 只印 1/3~1/2 或尾部乱码
- **必须分条**发送 `GS v 0 m xL xH yL yH [data]`，条间留 **足够延时**（社区经验 ~**800–1200ms**/条，115200 下按字节数再加算）
- 条间 **禁止** 额外 `LF` / `CRLF` / `ESC J` / `escpos_feed_dots`（会 **双走纸** → 固定横白条）
- 结束 **禁止** `ESC d n`（会把缓冲当文本印出 → **底部乱码**）；安全结束：`ESC J n` + `ESC @`
- XPrinter 类固件：高度为 **48 倍数** 可能异常 → 可选末条 **PAD +8 行白**（`CONFIG_ESC_POS_RASTER_PAD_48`）
- 推荐起始参数：`band_h=24`，`post_band_delay=1000ms`，UART min delay 1000ms，**不要**条间 LF

### ESC * 24 点列（仅作备选）

- 部分 UART 机 GS v 0 缓冲差时 ESC* 能打全高，但易：
  - **水平错位/锯齿**（LF 时未 `ESC a 0` 左对齐）
  - **横纹**（`ESC 3` 行距与条高 24 不一致）
  - 长 `vTaskDelay` 导致 **走纸电机断续**
- 若用 ESC*：`ESC 3 24` 与 strip 高度一致；条前 `ESC a 0`；条间小延时（≤120ms）+ 精确 `ESC J n`，**不要** CR+LF 代替走纸
- UART 默认应 **GS v 0 分条**（`CONFIG_ESC_POS_UART_USE_ESC_STAR=n`），ESC* 仅 menuconfig 显式开启

### 其他

- 改 `sdkconfig.defaults` 后若 WiFi 仍坏：检查是否误选 H2/SPI；必要时删 `sdkconfig` 重建，**保留** Hosted SDIO+C5 项（见 `sdkconfig.defaults` 中 `CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5`、`CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE`）
- 每次阶段改动写 `docs/OPTIMIZATION_CHANGELOG.md`；跑 `idf.py build` + `idf.py size`
- **Git 几乎无打印历史 commit**，重要节点请 `git commit`

## 串口验收日志（必须出现）

**WiFi 正常：**

```
Reset slave using GPIO[54]
sdio_wrapper: SDIO master
EUI_Setting: wifi_init done
prt_sync: background poll resumed
```

**打印 job（GS v 0 路径）：**

```
print_image_job download_done ...
escpos_raster: raster_print start /sdcard/.print_N.part
escpos_raster: image mode GS v0 banded (link=1)
escpos_raster: banded_print total_h=384 band_h=24 bands=16 link=1
escpos_raster: raster_print done ... err=ESP_OK
```

**不应出现：** `spi: Resetting slave on SPI bus with pin 12`、`image mode ESC*`（除非你刻意测 ESC*）

## 实施顺序（建议）

1. **先**用 `tools/printer_host_test/print_logo.py` 在 PC 直连 COM 口试 GS v0 band_h/delay/finish，锁定参数
2. **再**把同样参数写入 `escpos_jpeg_raster.c` + `sdkconfig.defaults`
3. **然后**端到端：网页上传 → 设备远程打印校徽
4. 每次只改 **一个变量**（条高 / 延时 / 结束方式），对比实机

## 约束

- 最小 diff，不重构无关模块（Brookesia、相机、WiFi Hosted 配置）
- 不破坏现有文本打印（`escpos_text_print.c`）与家长站 API
- 工程标准：改动必须记入 `docs/OPTIMIZATION_CHANGELOG.md`

## 当前代码状态说明

仓库里已有 Parent-Print-4~7 迭代痕迹和 Hosted-SDIO-1 修复，但 **git 未提交**。你可以：

- 在现有 `escpos_jpeg_raster.c` 上 **精简重写** 打印路径；或
- 保留 `parent_print_sync` + 服务端，**只重写** 光栅发送层

请先阅读 `escpos_jpeg_raster.c`、`Kconfig`、`print_preprocess.py`、`parent_print_sync.cpp`，再给出改动方案和首版参数。
