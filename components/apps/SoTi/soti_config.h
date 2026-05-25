/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
 * ┃ 【搜题 / 上传】设备侧主要改 SOTI_R2_WORKER_* 宏；豆包 Key 在服务器环境变量。              ┃
 * ┃ 按量 ECS 无法做 ICP 备案时域名会被 Beaver 拦：见 SOTI_R2_WORKER_USE_ECS_PUBLIC_IP。        ┃
 * ┃ 固件在拿到 IP 后已用公共 DNS，减轻解析失败。                                              ┃
 * ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛
 */
#pragma once

/**
 * 【必填】上传 URL，必须以 /upload 结尾。
 *
 * 备案与按量 ECS：阿里云规定按量计费 ECS 不能作为备案接入方，域名（如 souti.novaio.top）解析到
 * 该类实例时会被 Beaver 返回「ICP Non-compliance」。永久做法：实例改为包年包月并完成备案。
 *
 * 开发阶段可直连公网 EIP：将 SOTI_R2_WORKER_USE_ECS_PUBLIC_IP 改为 1，并核对
 * SOTI_R2_WORKER_URL_ECS_PUBLIC 与当前 EIP 一致（EIP 变更后须改）。HTTP 请求的 Host 须为 EIP，
 * 不要用域名 Host 访问 EIP（公网边缘会按域名做备案拦截）。Nginx 的 server_name 须包含该 EIP。
 * 上线后再改回 0 使用域名。
 */
#ifndef SOTI_R2_WORKER_USE_ECS_PUBLIC_IP
#define SOTI_R2_WORKER_USE_ECS_PUBLIC_IP 1
#endif
#ifndef SOTI_R2_WORKER_URL_ECS_PUBLIC
#define SOTI_R2_WORKER_URL_ECS_PUBLIC "http://8.154.20.8/upload"
#endif

#ifndef SOTI_R2_WORKER_URL
#if SOTI_R2_WORKER_USE_ECS_PUBLIC_IP
#define SOTI_R2_WORKER_URL SOTI_R2_WORKER_URL_ECS_PUBLIC
#else
#define SOTI_R2_WORKER_URL "http://souti.novaio.top/upload"
#endif
#endif

/**
 * 【可选】与 Cloudflare Worker Secret「UPLOAD_TOKEN」完全一致；Worker 未设置该项则保持 "" 。
 */
#ifndef SOTI_R2_UPLOAD_TOKEN
#define SOTI_R2_UPLOAD_TOKEN "esp32souti"
#endif

/** 家长控制台绑定的设备码（与网页登录一致） */
#ifndef SOTI_DEVICE_ID
#define SOTI_DEVICE_ID "WT99-DEMO-01"
#endif

/** 大于此字节的 JPEG 走分片上传（多 TCP 短连接），减轻 ESP-Hosted SDIO 单连接 ~16KB 卡死。 */
#ifndef SOTI_UPLOAD_SEGMENT_THRESHOLD
#define SOTI_UPLOAD_SEGMENT_THRESHOLD 8192
#endif

/** 超过此大小的 JPEG 拒绝上传（SDIO 传 260KB+ 易卡死）；约 1280×960 强压画质后典型 <120KB。 */
#ifndef SOTI_MAX_UPLOAD_JPEG_BYTES
#define SOTI_MAX_UPLOAD_JPEG_BYTES (160 * 1024)
#endif
