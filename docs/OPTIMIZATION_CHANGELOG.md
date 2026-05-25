# WT99P4C5-S1 优化改动记录

> 每次改动在此追加一条，格式见 [ENGINEERING_STANDARD.md](ENGINEERING_STANDARD.md)

## 当前状态摘要

| 项 | 值 |
|----|-----|
| 分支定位 | 桌面 Brookesia 性能验证基线（无小智/双固件/AI 检测） |
| 说明文档 | [`DESKTOP_BRANCH.md`](DESKTOP_BRANCH.md) |
| 默认配置 | `sdkconfig.defaults` + `sdkconfig.defaults.release`（根 `CMakeLists.txt` 已写入） |
| 最新 app binary | **0x5c6fb0** bytes（约 5.77 MB） |
| 9M factory 剩余 | **36%** |
| 最新 build | **成功**（2026-05-24） |

**构建命令：**

```bash
idf.py build
idf.py size
idf.py size-components
```

修改 `sdkconfig.defaults*` 或组件结构后建议 `idf.py fullclean build`。

---

## 验证基线（优化前）

- 日期：2026-05-24
- idf.py size 输出：（未记录原始数值）
- 功能状态：待硬件回归

---

## [阶段0] 工程标准与改动文档 — 2026-05-24

### 改了什么

建立工程改动标准与本文档。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `.cursor/rules/engineering-standards.mdc` | 全文 | Cursor 规则：每次改动必须更新 CHANGELOG |
| `docs/ENGINEERING_STANDARD.md` | 全文 | 完整工程标准 |
| `docs/OPTIMIZATION_CHANGELOG.md` | 全文 | 改动记录 |

### 原因

便于发布版优化追溯与 Code Review。

### 预期影响

无固件体积变化。

---

## [阶段1.1] 排除冗余 Flash 资源 — 2026-05-24

### 改了什么

CMake 排除未使用的 `*_large` 音乐资源、`breaking_news.c`。

> **历史说明**：本阶段曾短暂合并 App 图标为单一 `img_app_launcher`，已在 **[阶段4.1]** 撤销；当前仍保留 large / breaking_news 排除。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `components/apps/CMakeLists.txt` | `list(FILTER ...)` | 排除 `gui_music/assets/*_large.c`、`breaking_news.c` |

### 原因

GLOB 编译了未引用或与标准版重复的资源，占用 Flash。

### 预期影响

- Flash：**约 -800 KB ~ -1.2 MB**（large 资源 + breaking_news；不含图标，图标见 4.1）

---

## [阶段1.2] 中文字体单字号 — 2026-05-24

### 改了什么

发布版仅编译 `lv_font_ui_zh_22.c`；`lv_font_ui_zh_30` 宏别名到 22px。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `components/lv_font_ui_zh/CMakeLists.txt` | SRCS | 移除 `lv_font_ui_zh_30.c` |
| `components/lv_font_ui_zh/lv_font_ui_zh.h` | 全文 | `#define lv_font_ui_zh_30 lv_font_ui_zh_22` |

### 预期影响

- Flash：**约 -800 KB ~ -1.2 MB**（30px 字体 blob 不再链接）

---

## [阶段1.4–1.6] sdkconfig 发布裁剪 + 关蓝牙 + 调试移除 — 2026-05-24

### 改了什么

新增 `sdkconfig.defaults.release`；关闭蓝牙/NimBLE；降低日志与 LVGL Demo；移除 main 堆监控循环。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `sdkconfig.defaults.release` | 全文 | 发布版 Kconfig 覆盖项 |
| `sdkconfig.defaults` | BT / RAM / FreeRTOS | 关 BT；RAM/FreeRTOS 调优 |
| `main/main.cpp` | 显示/BLE/循环 | `buff_dma=true`；删 BLE init；删 5s MEM 循环 |
| `components/apps/setting/` | UI 条件编译 | `#if !CONFIG_BT_ENABLED` 隐藏蓝牙菜单与 BLE 屏 |

### 配置变更

| CONFIG | 旧 → 新 |
|--------|---------|
| CONFIG_BT_ENABLED | y → n |
| CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE | y → n |
| CONFIG_LV_LAYER_SIMPLE_BUF_SIZE | 102400 → 51200 |
| CONFIG_LV_IMG_CACHE_DEF_SIZE | 20 → 10 |
| CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM | n → y |

### 预期影响

- Flash：**约 -100 ~ -400 KB**（BT/NimBLE + LVGL demo/字体）
- Internal RAM：**约 +10 ~ 30 KB**

---

## [阶段2] 移除 AI 检测管线 — 2026-05-24

### 改了什么

移除 Camera 中 PPA/feed_pipeline/detect 及 apps 对 detect 组件的依赖。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `components/apps/camera/Camera.cpp` | init/run/close/frame | 删除 AI 检测与 PPA 初始化 |
| `components/apps/camera/Camera.hpp` | 成员/声明 | 删除 detect task |
| `components/apps/CMakeLists.txt` | REQUIRES | 移除 human_face_detect、pedestrian_detect |
| `sdkconfig.defaults.release` | AI model | 关闭 MODEL_IN_FLASH_RODATA |

### 预期影响

- PSRAM：**约 -10 MB**（运行时不再分配 feed pipeline）
- Flash：**约 -200 KB ~ -1 MB**（模型不再链接）

> 阶段4 已物理删除 `human_face_detect` / `pedestrian_detect` 组件及 camera 下 detect 源文件。

---

## [阶段3.1–3.2] 相机帧回调解耦 — 2026-05-24

### 改了什么

JPEG/SD 快照移至 `cam_worker` 任务；预览用 `lv_obj_invalidate` 替代 `lv_refr_now`；提高 video stream 优先级。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `components/apps/camera/Camera.cpp` | worker / frame 回调 | 异步 worker + 轻量回调 |
| `components/apps/camera/app_video.c` | 任务参数 | prio 3→6，stack 4K→8K |
| `components/apps/camera/app_video.h` | EXAMPLE_CAM_BUF_NUM | 4→3 |

### 预期影响

- 相机预览 FPS：**预计 +30% ~ +100%**
- PSRAM：**约 -2.5 MB**（少 1 帧 buffer）

---

## [阶段3.3–3.4] 显示 DMA + SD SPI — 2026-05-24

### 改了什么

LVGL 缓冲启用 DMA；SD SPI `max_transfer_sz` 增大。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `main/main.cpp` | bsp_display_cfg | `buff_dma = true` |
| `components/wt99p4c5_s1_board/wt99p4c5_s1_board.c` | spi_bus_config | `max_transfer_sz` 4000→32768 |

### 预期影响

- 显示 flush CPU 拷贝减少
- SD 读写吞吐提升

---

## [阶段4] 桌面专用分支 — 去小智 — 2026-05-24

### 改了什么

移除已确认不再使用的小智/双固件/AI 检测组件；修复 CMake 因缺失 `xiaozhi-esp32-2.2.4` 导致的 configure 失败；默认构建切换为 release 配置。

### 改动位置

| 项目 | 变更说明 |
|------|----------|
| `components/xiaozhi_engine_disabled_for_path_c/` | **删除** — 嵌入式小智 |
| `sdkconfig.defaults.xiaozhi` | **删除** |
| `CMakeLists.txt` | `SDKCONFIG_DEFAULTS` → `sdkconfig.defaults;sdkconfig.defaults.release` |
| `PATH_C_DUAL_FIRMWARE.md` | **删除** |
| `components/dual_firmware/` | **删除** |
| `components/apps/AiChat/` | **删除** |
| `partitions_dual.csv` | **删除** |
| `components/human_face_detect/`、`pedestrian_detect/` | **删除** |
| `components/apps/camera/app_*detect*`、`app_camera_pipeline.*` | **删除** |
| `main/main.cpp` | 移除 `dual_firmware_early_check()`、AiChat 安装 |
| `components/apps/apps.h` | 移除 AiChat include |
| `main/CMakeLists.txt`、`components/apps/CMakeLists.txt` | 移除 `dual_firmware` REQUIRES |
| `docs/DESKTOP_BRANCH.md` | **新增** |
| `main/idf_component.yml` | 新增 `espressif/esp_jpeg` |
| `components/usb_escpos_printer/CMakeLists.txt` | `esp_jpeg` → `espressif__esp_jpeg` |
| `components/apps/camera/Camera.cpp` | 修复多余 `}` 语法错误 |

### 原因

本仓库专注桌面 Brookesia 性能验证；小智独立版本已删除。

### 预期影响

- CMake configure 成功
- Flash 下降（去掉 esp-dl 模型及 78__esp-* 依赖链）
- 桌面无 AiChat；相机无 AI 检测框

### 验证

见文档末尾 **[当前验证记录]**（去小智后、恢复图标前：0x574440）。

---

## [阶段4.1] 恢复各 App 独立桌面图标 — 2026-05-24

### 改了什么

撤销阶段1.1 中的「共享计算器图标」方案，各 App 恢复独立 `img_app_*` 资源。

### 改动位置

| 文件 | 变更说明 |
|------|----------|
| `components/apps/CMakeLists.txt` | 取消排除 `img_app_*.c` |
| 各 App `*.cpp` | `img_app_launcher` → 各自 `img_app_*` |
| `parent_chat/parent_chat_launcher.cpp` | 未读红点查找 `img_app_parent_chat` |
| `common/assets/img_app_launcher.c` | **删除** |

### 原因

共享图标导致桌面全部显示为计算器。

### 预期影响

- 桌面图标恢复区分
- Flash：**+约 330 KB**（0x574440 → 0x5c6fb0）

### 验证

见文档末尾 **[当前验证记录]**。

---

## [功能] 家长站相册传图（替代局域网传图）— 2026-05-24

### 改了什么

- **删除** 设备端局域网 HTTP 传图（`components/sd_web_upload/` 整包移除，省 Flash/运行时）。
- **新增** 家长控制台 [http://8.154.20.8/parent/](http://8.154.20.8/parent/) 侧栏「相册传图」：家长上传 JPG/PNG/WebP，ECS **自动转 JPG 并缩小至 ≤1280×960** 后入队。
- **新增** 设备端 `parent_album_sync`：约 12 秒后台轮询 + 相册「同步」按钮，分片下载写入 `/sdcard`；同步时暂停相机预览/亲子聊天轮询/策略轮询。

### 改动位置

| 文件 | 位置 | 变更说明 |
|------|------|----------|
| `components/sd_web_upload/` | **删除** | 原局域网 `esp_http_server` 传图方案（未再使用） |
| `components/parent_album_sync/` | 新建 | `parent_album_sync.cpp` 轮询/分片下载/ack；`parent_album_fs.c` 写 SD |
| `components/apps/photo_album/PhotoAlbum.*` | UI | 「同步」按钮与状态；移除 URL/Token 传图面板 |
| `components/apps/CMakeLists.txt` | `REQUIRES` | `parent_album_sync` 替代 `sd_web_upload` |
| `main/main.cpp` | `app_main` | `parent_album_sync_bg_start()` |
| `main/CMakeLists.txt` | `REQUIRES` | `parent_album_sync` |
| `tools/soti-standalone-server/parent_store.py` | `album_*` | 收件箱表与文件存储 |
| `tools/soti-standalone-server/parent_routes.py` | `/parent/api/album/*` | upload / poll / file / ack |
| `tools/soti-standalone-server/album_preprocess.py` | `album_normalize_image` | Pillow 缩放+转 JPEG（对齐设备相册上限） |
| `tools/soti-standalone-server/static/parent/` | 侧栏+面板 | 相册传图 UI |
| `components/parent_policy/` | `parent_policy_poll_pause` | 保留，供同步专注模式 |
| `sdkconfig.defaults` | — | 移除 `CONFIG_SD_UPLOAD_*` |

### 原因

家长希望用公网家长站传图，不必与设备同一 WiFi；局域网 HTTP 服务占用 Flash 且与产品入口不统一。

### 预期影响

- Flash：较局域网方案 **减少**（无 `esp_http_server` + 内嵌 HTML）
- 传图延迟：先上云再下发，依赖设备联网与 ECS
- 相册根目录新增家长站下发的 `.jpg`

### 验证

- [ ] `idf.py build` 通过
- [ ] 家长站登录 → 相册传图 → 上传 JPEG
- [ ] 设备 WiFi 联网后自动或点「同步」→ `/sdcard` 可见
- [ ] 工程内无 `sd_web_upload` / `sd_upload_*` 引用

---

## 当前验证记录

- 日期：2026-05-24
- build：**成功**
- app binary：**0x5c6fb0** bytes（约 5.77 MB）
- 9M factory 分区剩余：**36%**（0x339050 free）
- 去小智后（共享图标时）：0x574440 bytes，剩余 39%
- Total image size（共享图标 build）：5718700 bytes

**功能回归（待硬件）：**

- [ ] 启动 / Brookesia 桌面滑动
- [ ] 桌面各 App 图标区分正确
- [ ] 相机预览 / 拍照
- [ ] SoTi 上传
- [ ] WiFi 设置
- [ ] 音乐 / 视频
- [ ] USB/UART 打印
- [ ] SoTi 答案页「仅题目 / 含答案」错题贴打印

---

## [阶段 SoTi-Print-1] 搜题答案页错题贴文本打印

| 字段 | 内容 |
|------|------|
| 改了什么 | SoTi 答案页增加「仅题目」「含答案」热敏打印；服务器返回 `print` 小节；新增 ESC/POS UTF-8→GBK 文本模块 |
| 改动位置 | `tools/soti-standalone-server/soti_answer_sanitize.py` `split_print_sections()`；`soti_upload_server.py` `device_upload_response()`；`components/usb_escpos_printer/escpos_text_print.c`、`utf8_gbk_map.inc`、`tools/gen_utf8_gbk_table.py`；`components/apps/SoTi/soti_print_sections.cpp`、`SoTi.cpp`、`soti_r2_upload.cpp` |
| 原因 | 热敏机打照片效果差；错题贴需横向长条文本，题目/含步骤答案分开或合并打印 |
| 预期影响 | Flash +~25KB（GBK 映射表）；PSRAM/Internal 打印任务 8KB 栈；性能：文本打印 UART 负载低于 JPEG 光栅 |
| 验证 | build；搜题后两钮打印；旧服务器无 `print` 时本地切分兜底 |

**配置：** `CONFIG_UART_ESC_POS_PRINT_COLS=22`（menuconfig USB/BLE ESC/POS printer）

**服务器 JSON 扩展：**

```json
{ "ok": true, "answer": "...", "mode": "solve", "print": { "question": "...", "with_answer": "..." } }
```

**验证记录：**

- build：待 `idf.py build`
- size：待 `idf.py size`
- 功能：待硬件 — 搜题 → 仅题目 / 含答案 各打一条长条

---

## [阶段 SoTi-Print-2] 热敏文本行宽与英文断词

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Print-2]` |
| 改了什么 | 58mm Font B 行宽 22→42 列；英文按空格/连字符断词换行；弯引号/长破折号归一为 ASCII；GBK 缓冲 512B |
| 改动位置 | `components/usb_escpos_printer/Kconfig` `UART_ESC_POS_PRINT_COLS` default/range；`escpos_text_print.c` `normalize_print_cp()`、`encode_utf8_range()`、换行回退；`sdkconfig`/`sdkconfig.defaults` `CONFIG_UART_ESC_POS_PRINT_COLS=42`；`tools/gen_utf8_gbk_table.py` 补充标点 |
| 原因 | 用户打印：中文右侧约 1/3 空白、英文约 1/2 空白；英文单词被 22 列硬切；段间偶发乱码 |
| 预期影响 | Flash 不变；PSRAM 打印时 +256B 临时缓冲；性能无感；纸面利用率约 90% |
| 验证 | build + size；硬件：翻译/搜题长文打印，英文不再 develop- ment 式断词 |

**配置：** `CONFIG_UART_ESC_POS_PRINT_COLS` **22 → 42**

**验证记录：**

- build：`escpos_text_print.c` 编译通过；全量 `idf.py build` 待清 bootloader CMake 缓存后重跑
- size：待 `idf.py size`
- 功能：待硬件 — 翻译模式 【原文】【译文】各打一条，右侧空白明显缩小

---

## [阶段 SoTi-Fix-1] 搜题黑屏与界面卡死修复

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Fix-1]` |
| 改了什么 | 修复进入搜题黑屏、全屏无响应：CSI 停流等待加超时；关闭 loading 恢复预览；相册 JPEG 后台读取；识别层增加「取消」 |
| 改动位置 | `components/apps/camera/app_video.c` `app_video_stream_wait_stop(timeout_ms)`、`VIDIOC_STREAMOFF` 失败仍置 DONE；`Camera.cpp` `CAMERA_STREAM_STOP_WAIT_MS=5000`、陈旧 `s_camera_stream_running` 校正；`SoTi.cpp` `showLoadingUi`/`run`/`pause`/`tryConsumeSdCardUpload`/`on_loading_cancel`；`SoTi.hpp` |
| 原因 | LVGL 线程在 `app_video_stream_wait_stop(portMAX_DELAY)` 与 SD 大图 `fread` 上阻塞；loading 停预览后 `showLoadingUi(false)` 未 `setPreviewPaused(false)`；STREAMOFF 失败导致永久等待 |
| 预期影响 | Flash 不变；Internal RAM 不变；PSRAM 不变；性能：进入搜题/取消识别 UI 响应更快 |
| 验证 | 待 `idf.py build` + `idf.py size`；硬件：进搜题预览正常、识别中可点取消、相册进搜题不卡 UI |

---

## [阶段 UART-1] 关闭上电 UART 测试打印

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 UART-1]` |
| 改了什么 | 关闭每次上电后约 5 秒自动打印的 Dongwei 测试小票 |
| 改动位置 | `components/usb_escpos_printer/Kconfig` `UART_ESC_POS_BOOT_TEST` default `y`→`n`；`sdkconfig` `CONFIG_UART_ESC_POS_BOOT_TEST` `y`→未设置；`sdkconfig.defaults` 追加 `CONFIG_UART_ESC_POS_BOOT_TEST=n` |
| 原因 | 用户不需要每次上电自动打测试条 |
| 预期影响 | Flash/PSRAM/RAM/性能无变化；上电少 3 次 UART 打印任务 |
| 验证 | `idf.py build`；上电后打印机不再自动出 hello/123/demo 测试条 |

---

## [阶段 SoTi-Upload-1] 搜题上传 SDIO mempool 耗尽修复

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Upload-1]` |
| 改了什么 | 缓解分片上传时 `STA TX: mempool_alloc failed` 导致 HTTP 失败 |
| 改动位置 | `soti_r2_upload.cpp` 分片 4096→1024、1KB 分块 write、part 间隔 120ms；`sdkconfig`/`sdkconfig.defaults` 启用 `CONFIG_ESP_HOSTED_USE_MEMPOOL`、`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM`、`CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE`/`RX_Q_SIZE` 20→32 |
| 原因 | 串口：大量 mempool OOM → TLS/HTTP 超时 → `segment part off=0 st=-1` |
| 预期影响 | PSRAM 略增（mempool）；上传略慢但更稳 |
| 验证 | 待 build；硬件：WiFi 已连、搜题拍照上传成功 |

---

## [阶段 SoTi-Upload-2] 澄清单次上传 vs 多次 HTTP；上传时暂停相册同步

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Upload-2]` |
| 改了什么 | 搜题上传期间 `parent_album_sync_pause(true)`；分片日志标明 parts 次数；`upload task start (single session)` |
| 改动位置 | `parent_album_sync.cpp/.h`、`SoTi.cpp`、`soti_r2_upload.cpp` |
| 原因 | 用户点一次搜题；分片模式一张图=N 次 TCP+后台 alb_sync 抢 SDIO mempool，易被误认为「同一张传了多次」 |
| 预期影响 | 上传时少并行 HTTP；日志可区分单次任务与多分片 |
| 验证 | monitor 仅一条 `upload task start`；见 `parts=` 与 `segment part ok` 计数 |

---

## [阶段 SoTi-Crash-1] 搜题上传中家长聊天 OOM 崩溃修复

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Crash-1]` |
| 改了什么 | 修复上传第 10 片后 `abort()`：`pchat_bg` 在堆紧张时 `std::string::resize(8192)` → `bad_alloc` |
| 改动位置 | `parent_chat_api.cpp` `http_read_body`/`fetch_unread` 改 SPIRAM 缓冲；`parent_chat_bg_pause`；`SoTi.cpp` 上传期间 `parent_chat_bg_pause(true)` |
| 原因 | 串口：10/10 `segment part ok` 后崩溃栈在 `parent_chat_fetch_unread` → `operator new` |
| 预期影响 | Internal RAM 上传期少并发 HTTP；家长消息角标 poll 上传时暂停 |
| 验证 | build；搜题上传至 commit 完成不 reboot |

---

## [阶段 SoTi-Daily-1] 每日一句栈溢出崩溃修复

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 SoTi-Daily-1]` |
| 改了什么 | 修复点「每日一句」后 `soti_daily` Stack protection fault |
| 改动位置 | `soti_r2_upload.cpp` `soti_fetch_daily_line`：`body[4096]` 改 SPIRAM 堆；HTTP buf 2048；`SoTi.cpp` `soti_daily` 栈 16KB→24KB，上传同款暂停 alb_sync/pchat_bg |
| 原因 | 串口：SP 越界，`soti_fetch_daily_line` 栈上 4KB body + esp_http_client 超 16KB 任务栈 |
| 预期影响 | PSRAM +4KB 临时；Internal 任务栈 +8KB |
| 验证 | build；点每日一句/快门 daily 不 panic |

---

## [阶段 Album-1] 移除相册 Tx 测试钮；息屏暂停 SDIO 后台 HTTP

| 字段 | 内容 |
|------|------|
| 阶段编号 | `[阶段 Album-1]` |
| 改了什么 | 相册去掉「Tx」ESC/POS 测试按钮及相关任务；息屏时暂停 policy/chat/album 后台轮询 |
| 改动位置 | `PhotoAlbum.cpp/.hpp` 删除 `_btn_test_print`、`album_test_print_task`；`camera_power_bridge.cpp` `power_manager_on_screen_off/on` |
| 原因 | 用户不再使用 Tx 测试；串口 74KB 搜题成功后约 2min `parent_pol: policy synced` 随即 `H_SDIO_DRV` 不可恢复 → `esp_restart` |
| 预期影响 | Flash 略减（相册 UI/任务代码）；息屏后 SDIO 负载降低，降低无故重启概率 |
| 验证 | build + size；相册无 Tx；息屏后无 policy/chat HTTP（亮屏恢复） |
