# 桌面专用分支说明

本仓库为 **WT99P4C5-S1 Brookesia 桌面性能优化验证基线**，不含小智语音/双固件切换能力。

## 定位

- 验证 Flash / RAM / 相机 / 显示等桌面端优化
- 日常构建默认使用 `sdkconfig.defaults` + `sdkconfig.defaults.release`
- 分区表：[`partitions.csv`](../partitions.csv)（9M 单 factory + 4M SPIFFS）

## 桌面 App 列表

| App | 说明 |
|-----|------|
| Calculator | 计算器 |
| MusicPlayer | 音乐播放 |
| AppSettings | 系统设置（WiFi 等） |
| Game2048 | 2048 游戏 |
| Camera | 相机预览 / 拍照（无 AI 检测） |
| AppVideoPlayer | 视频播放 |
| SoTi | 搜题 |
| PhotoAlbum | 相册 |
| Print | ESC/POS 打印 |
| ParentChat | 家长端聊天 |

## 已移除（不再维护）

| 项目 | 原因 |
|------|------|
| `components/xiaozhi_engine_disabled_for_path_c/` | 嵌入式小智，apps 无依赖 |
| `sdkconfig.defaults.xiaozhi` | 仅服务嵌入式 xiaozhi_engine |
| `components/dual_firmware/` | Path C 双固件切小智 |
| `components/apps/AiChat/` | 小智启动器 |
| `components/human_face_detect/`、`pedestrian_detect/` | AI 检测模型，Camera 已去除 |
| `PATH_C_DUAL_FIRMWARE.md`、`partitions_dual.csv` | 旧双固件文档/分区 |

## 构建

```bash
idf.py build
idf.py size
```

改动记录与最新体积见 [`OPTIMIZATION_CHANGELOG.md`](OPTIMIZATION_CHANGELOG.md)。

## 刷机注意

若设备曾刷过双固件版本，建议 `idf.py erase-flash` 后再烧录本桌面固件，避免 NVS 中旧启动目标干扰。
