#!/usr/bin/env python3
"""
SoTi 上传服务（替代 Cloudflare Worker）：与固件 soti_r2_upload.cpp 协议一致。

  POST /upload
  Headers: Content-Type: image/jpeg, Authorization: Bearer <UPLOAD_TOKEN>
  Body: 原始 JPEG

  成功: HTTP 200 + JSON {"ok": true, "answer": "..."}
  失败: 非 200 或 JSON 含 error（固件会读 error/answer 字段）

  依赖: Python 3 标准库 + 可选 Pillow（用于上传图二值化后送豆包，利于搜题 OCR）。
  未装 Pillow 时自动跳过二值化，仍用原 JPEG。安装: pip install Pillow

  环境变量 SOTI_BINARIZE: 默认 1 启用二值化；设为 0/false 则关闭。

  分片上传（推荐 ESP32 SDIO）：POST /upload?mode=solve&st=init&total=N（body 空）→ part / commit。
  mode 可选：solve（默认）| translate | tutor | grade | summary。A5 每日一句：GET /upload/daily。

  多模式说明见 soti_modes.py；部署需同目录 soti_upload_server.py + soti_modes.py + soti_answer_sanitize.py。

  公网诊断（一条命令区分「阿里云 Beaver 备案拦」与「已打到本服务」）:
    curl -sS -D- -o /tmp/h.out http://souti.novaio.top/upload/health | head -n 5; head -c 200 /tmp/h.out
    Windows: curl.exe -sS -D- http://souti.novaio.top/upload/health -o -
  - 若首行 HTTP/1.1 200 且 body 含 "soti-upload" → 流量已到 Nginx+本进程。
  - 若 HTTP/1.1 403 且 HTML 含 Beaver / ICP Filing → 未到应用，先处理备案/解析/80 入口。
  （GET 无需 Authorization；仅用于连通性，勿暴露敏感信息在响应中。）

  用法:
  export DOUBAO_API_KEY='你的火山方舟 API Key'
  export DOUBAO_MODEL='ep-xxxx'   # 必填：方舟控制台「推理接入点」ID（ep- 开头），不要用旧版模型名字符串
  export UPLOAD_TOKEN='esp32souti'  # 与 soti_config.h 一致；不设则不校验
  python3 soti_upload_server.py

  默认监听 127.0.0.1:3000；Nginx SSL 反代到 http://127.0.0.1:3000/upload
  对外端口用 PORT=3000 LISTEN=0.0.0.0（不推荐直连公网，建议只给 Nginx 反代）

  ESP32 经 SDIO 上传大 JPEG 很慢：Nginx 默认 60s 超时易出现 499、设备一直等。
  见同目录 nginx-soti-upload-snippet.conf（client_body_timeout / proxy_read_timeout 等）。
"""
from __future__ import annotations

import base64
import json
import os
import sys
import time
import uuid
from io import BytesIO
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlparse
from urllib.request import Request, urlopen

from parent_routes import (
    handle_get as parent_handle_get,
    handle_options as parent_handle_options,
    handle_post as parent_handle_post,
    handle_put as parent_handle_put,
    record_upload_history,
)
from soti_answer_sanitize import format_answer_for_device, looks_like_raw_latex, split_print_sections
from soti_modes import (
    DAILY_TEXT_PROMPT,
    daily_cache_path,
    normalize_mode,
    should_binarize_for_mode,
    today_key,
    vision_prompt_for_mode,
)

LISTEN = os.environ.get("LISTEN", "127.0.0.1")
PORT = int(os.environ.get("PORT", "3000"))
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "esp32souti")
DOUBAO_API_KEY = os.environ.get("DOUBAO_API_KEY", "").strip()
ARK_API_URL = os.environ.get(
    "ARK_API_URL", "https://ark.cn-beijing.volces.com/api/v3/chat/completions"
)
DOUBAO_MODEL = os.environ.get("DOUBAO_MODEL", "").strip()
DOUBAO_MAX_TOKENS = int(os.environ.get("DOUBAO_MAX_TOKENS", "4096"))

MAX_UPLOAD_BYTES = 6 * 1024 * 1024
MAX_SEG_SESSIONS = 64
SEG_SESSION_TTL_SEC = 600.0
UPLOAD_SESSIONS: dict[str, dict[str, Any]] = {}


def _prune_upload_sessions() -> None:
    now = time.monotonic()
    dead = [k for k, v in UPLOAD_SESSIONS.items() if now - float(v["t0"]) > SEG_SESSION_TTL_SEC]
    for k in dead:
        del UPLOAD_SESSIONS[k]


def preprocess_image_for_vision(jpeg: bytes, mode: str = "solve") -> tuple[bytes, str]:
    """
    将 JPEG 转为高对比黑白图再送豆包，通常更利印刷体/手写题 OCR；PNG 二值常比高画质 JPEG 更小。
    返回 (字节, data URL 子类型 "png"|"jpeg")。
    """
    if not should_binarize_for_mode(mode):
        return jpeg, "jpeg"
    v = os.environ.get("SOTI_BINARIZE", "1").strip().lower()
    if v in ("0", "false", "no"):
        return jpeg, "jpeg"
    try:
        from PIL import Image, ImageFilter, ImageOps
    except ImportError:
        sys.stderr.write("soti-upload: Pillow 未安装，跳过二值化。ECS: pip install Pillow\n")
        return jpeg, "jpeg"
    try:
        im = Image.open(BytesIO(jpeg)).convert("L")
        im = ImageOps.autocontrast(im, cutoff=1)
        im = im.filter(ImageFilter.UnsharpMask(radius=1, percent=110, threshold=3))
        lo, hi = im.getextrema()
        th = 128 if hi <= lo else (lo + hi) // 2
        bw = im.point(lambda p, t=th: 255 if p > t else 0).convert("1")
        out = BytesIO()
        bw.save(out, format="PNG", optimize=True)
        png = out.getvalue()
        if len(png) > len(jpeg) * 14 // 10:
            sys.stderr.write(
                "soti-upload: binarized PNG (%d) > 1.4×JPEG (%d), keep JPEG\n" % (len(png), len(jpeg))
            )
            return jpeg, "jpeg"
        sys.stderr.write("soti-upload: binarize jpeg %d -> png %d bytes\n" % (len(jpeg), len(png)))
        return png, "png"
    except Exception as e:
        sys.stderr.write("soti-upload: binarize failed %r; use raw JPEG\n" % (e,))
        return jpeg, "jpeg"


def json_bytes(obj: Any, status: int = 200) -> bytes:
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return body


def vision_failure_json_bytes(msg: str) -> bytes:
    """豆包/预处理异常时返回 502；answer 含具体原因便于设备小屏显示，stderr 打全文便于 ECS 排查。"""
    raw = msg or ""
    sys.stderr.write("soti-upload: vision failure: %s\n" % raw[:4000])
    shown = raw.strip()
    if len(shown) > 900:
        shown = shown[:900] + "…"
    return json_bytes(
        {
            "ok": False,
            "error": "搜题失败: " + raw,
            "answer": "搜题失败：" + shown,
        }
    )


def finalize_device_answer(text: str, mode: str) -> str:
    """豆包原文 → 屏显纯文本；失败类文案不二次处理。"""
    if not text:
        return text
    t = text.strip()
    if t.startswith("（演示）") or t.startswith("搜题失败"):
        return t
    out = format_answer_for_device(t, mode)
    if looks_like_raw_latex(out):
        sys.stderr.write("soti-upload: answer still has LaTeX residue mode=%s\n" % mode)
    return out


def device_upload_response(answer: str, mode: str) -> dict[str, Any]:
    """成功上传 JSON：answer + 可选 print 小节（热敏错题贴）。"""
    m = normalize_mode(mode)
    payload: dict[str, Any] = {"ok": True, "key": None, "answer": answer, "mode": m}
    if answer and not answer.startswith("搜题失败") and not answer.startswith("（演示）"):
        payload["print"] = split_print_sections(answer, m)
    return payload


def run_doubao_chat_text(prompt: str, max_tokens: int | None = None) -> str:
    if not DOUBAO_API_KEY:
        return "（演示）未配置 DOUBAO_API_KEY。"
    if not DOUBAO_MODEL:
        raise RuntimeError("未设置 DOUBAO_MODEL（须为 ep-xxxx 接入点 ID）")
    mt = max_tokens if max_tokens is not None else DOUBAO_MAX_TOKENS
    payload = {
        "model": DOUBAO_MODEL,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": mt if mt > 0 else 512,
    }
    data = json.dumps(payload).encode("utf-8")
    req = Request(
        ARK_API_URL,
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "Authorization": "Bearer " + DOUBAO_API_KEY,
        },
    )
    try:
        with urlopen(req, timeout=120) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")[:800]
        raise RuntimeError(f"Doubao HTTP {e.code}: {err_body}") from e
    except URLError as e:
        raise RuntimeError(f"Doubao 网络错误: {e}") from e
    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        return text[:4000]
    err = obj.get("error")
    if isinstance(err, dict) and err.get("message"):
        raise RuntimeError(str(err["message"]))
    choices = obj.get("choices") or []
    if not choices:
        return json.dumps(obj, ensure_ascii=False)[:4000]
    msg = (choices[0] or {}).get("message") or {}
    content = msg.get("content")
    if isinstance(content, str):
        return finalize_device_answer(content.strip(), "daily")
    return json.dumps(obj, ensure_ascii=False)[:4000]


def get_daily_line_cached() -> str:
    path = daily_cache_path()
    key = today_key()
    try:
        with open(path, "r", encoding="utf-8") as f:
            obj = json.load(f)
        if obj.get("date") == key and isinstance(obj.get("answer"), str) and obj["answer"].strip():
            return finalize_device_answer(obj["answer"].strip(), "daily")
    except (OSError, json.JSONDecodeError, TypeError):
        pass
    line = run_doubao_chat_text(DAILY_TEXT_PROMPT, max_tokens=256)
    line = finalize_device_answer(line.strip(), "daily")
    try:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"date": key, "answer": line}, f, ensure_ascii=False)
    except OSError as e:
        sys.stderr.write("soti-upload: daily cache write failed %r\n" % (e,))
    return line


def run_doubao_vision(image_bytes: bytes, media_subtype: str = "jpeg", mode: str = "solve") -> str:
    if not DOUBAO_API_KEY:
        return (
            "（演示）未配置 DOUBAO_API_KEY：在服务器上 export DOUBAO_API_KEY=你的火山方舟 Key 后重启本服务。"
        )

    if not DOUBAO_MODEL:
        raise RuntimeError(
            "未设置 DOUBAO_MODEL。请在火山方舟控制台「推理接入点」创建豆包视觉模型接入点，"
            "将接入点 ID（形如 ep-xxxx）写入环境变量：export DOUBAO_MODEL=ep-xxxx 后重启服务。"
            "（方舟 chat/completions 的 model 字段须填接入点 ID；填 doubao-xxx 公开名会返回 404。）"
        )

    b64 = base64.b64encode(image_bytes).decode("ascii")
    image_data_url = "data:image/%s;base64,%s" % (media_subtype, b64)
    payload = {
        "model": DOUBAO_MODEL,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "image_url": {"url": image_data_url}},
                    {"type": "text", "text": vision_prompt_for_mode(mode)},
                ],
            }
        ],
        "max_tokens": DOUBAO_MAX_TOKENS if DOUBAO_MAX_TOKENS > 0 else 4096,
    }
    data = json.dumps(payload).encode("utf-8")
    req = Request(
        ARK_API_URL,
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/json; charset=utf-8",
            "Authorization": "Bearer " + DOUBAO_API_KEY,
        },
    )
    try:
        with urlopen(req, timeout=240) as resp:
            text = resp.read().decode("utf-8", errors="replace")
    except HTTPError as e:
        err_body = e.read().decode("utf-8", errors="replace")[:800]
        raise RuntimeError(f"Doubao HTTP {e.code}: {err_body}") from e
    except URLError as e:
        raise RuntimeError(f"Doubao 网络错误: {e}") from e

    try:
        obj = json.loads(text)
    except json.JSONDecodeError:
        return text[:8000]

    err = obj.get("error")
    if isinstance(err, dict) and err.get("message"):
        raise RuntimeError(str(err["message"]))

    choices = obj.get("choices") or []
    if not choices:
        return json.dumps(obj, ensure_ascii=False)[:8000]
    msg = (choices[0] or {}).get("message") or {}
    content = msg.get("content")
    if isinstance(content, str):
        return finalize_device_answer(content.strip(), mode)
    if isinstance(content, list):
        parts = []
        for p in content:
            if isinstance(p, dict):
                if isinstance(p.get("text"), str):
                    parts.append(p["text"])
                elif isinstance(p.get("content"), str):
                    parts.append(p["content"])
        joined = "\n".join(parts).strip()
        if joined:
            return finalize_device_answer(joined, mode)
    return json.dumps(obj, ensure_ascii=False)[:8000]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - - [%s] %s\n" % (self.address_string(), self.log_date_time_string(), fmt % args))

    def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _device_id_header(self) -> str:
        return (self.headers.get("X-Device-Id") or "").strip()

    def _maybe_record_history(
        self, mode: str, answer: str, jpeg: bytes | None, t0: float | None = None
    ) -> None:
        try:
            latency = int((time.monotonic() - t0) * 1000) if t0 is not None else None
            record_upload_history(
                self._device_id_header(),
                mode,
                answer,
                jpeg,
                ok=not answer.startswith("搜题失败"),
                latency_ms=latency,
                client_ip=self.client_address[0],
            )
        except Exception as e:
            sys.stderr.write("soti-upload: parent history %r\n" % (e,))

    def do_OPTIONS(self) -> None:
        path = self.path.split("?", 1)[0]
        if path.startswith("/parent"):
            parent_handle_options(self)
            return
        if not path.startswith("/upload"):
            self.send_error(404)
            return
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS")
        self.send_header(
            "Access-Control-Allow-Headers",
            "Content-Type, Authorization, X-SoTi-Session, X-SoTi-Offset, X-SoTi-Phase, X-Device-Id",
        )
        self.send_header("Access-Control-Max-Age", "86400")
        self.end_headers()

    def do_PUT(self) -> None:
        if parent_handle_put(self):
            return
        self.send_error(404)

    def do_GET(self) -> None:
        """GET /upload/health | /upload/daily | /parent/*"""
        if parent_handle_get(self):
            return
        path = urlparse(self.path).path or "/"
        if path == "/upload/health":
            self._send(
                200,
                json_bytes(
                    {
                        "ok": True,
                        "service": "soti-upload",
                        "modes": ["solve", "translate", "tutor", "grade", "summary", "daily"],
                        "hint": "POST /upload?mode=… ; GET /upload/daily",
                    }
                ),
            )
            return
        if path == "/upload/daily":
            self._handle_daily_request()
            return
        self.send_error(404)

    def _handle_daily_request(self) -> None:
        if not self._auth_ok():
            self._send(401, b"Unauthorized", "text/plain; charset=utf-8")
            return
        try:
            answer = get_daily_line_cached()
        except Exception as e:
            self._send(502, vision_failure_json_bytes(str(e)))
            return
        self._send(200, json_bytes(device_upload_response(answer, "daily")))

    def _auth_ok(self) -> bool:
        if not UPLOAD_TOKEN:
            return True
        auth = self.headers.get("Authorization") or ""
        return auth == "Bearer " + UPLOAD_TOKEN

    def _read_body(self, clen: int, max_len: int) -> bytes | None:
        if clen <= 0 or clen > max_len:
            self._send(
                400,
                json_bytes({"ok": False, "error": "bad Content-Length", "answer": "Content-Length 无效。"}),
            )
            return None
        body = self.rfile.read(clen)
        if len(body) != clen:
            self._send(
                408,
                json_bytes(
                    {
                        "ok": False,
                        "error": "short read",
                        "answer": "上传中断。",
                    }
                ),
            )
            return None
        return body

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path or "/"
        if path.startswith("/parent"):
            if parent_handle_post(self):
                return
            self.send_error(404)
            return
        if not path.startswith("/upload"):
            self.send_error(404)
            return
        if path == "/upload/daily":
            self._handle_daily_request()
            return
        if not self._auth_ok():
            self._send(401, b"Unauthorized", "text/plain; charset=utf-8")
            return

        try:
            clen = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            clen = 0

        qs = parse_qs(parsed.query)

        def qs_first(key: str) -> str:
            for v in qs.get(key) or []:
                if isinstance(v, str) and v.strip():
                    return v.strip()
            return ""

        st_q = qs_first("st").lower()
        req_mode = normalize_mode(qs_first("mode") or (self.headers.get("X-SoTi-Mode") or ""))
        path_norm = path.rstrip("/") or "/"
        phase = st_q or (self.headers.get("X-SoTi-Phase") or "").strip().lower()
        is_upload_root = path_norm == "/upload"
        seg_init = path == "/upload/init" or (phase == "init" and is_upload_root)
        seg_part = path == "/upload/part" or (phase == "part" and is_upload_root)
        seg_commit = path == "/upload/commit" or (phase == "commit" and is_upload_root)

        # A5：与 st=init 相同 — POST /upload?mode=daily 空 body（设备 SDIO 上更稳）
        if is_upload_root and clen == 0 and req_mode == "daily" and not (seg_init or seg_part or seg_commit):
            self._handle_daily_request()
            return

        if seg_init:
            _prune_upload_sessions()
            if len(UPLOAD_SESSIONS) >= MAX_SEG_SESSIONS:
                self._send(503, json_bytes({"ok": False, "error": "busy", "answer": "会话过多，请稍后。"}))
                return
            total = 0
            if clen > 0:
                body = self._read_body(clen, 4096)
                if body is None:
                    return
                try:
                    o = json.loads(body.decode("utf-8"))
                    total = int(o.get("total", 0))
                except (json.JSONDecodeError, ValueError, TypeError):
                    self._send(400, json_bytes({"ok": False, "error": "bad json", "answer": "JSON 无效。"}))
                    return
            else:
                try:
                    total = int(qs_first("total"))
                except ValueError:
                    total = 0
            if total < 64 or total > MAX_UPLOAD_BYTES:
                self._send(
                    400,
                    json_bytes({"ok": False, "error": "bad total", "answer": "total 超出范围。"}),
                )
                return
            sid = str(uuid.uuid4())
            UPLOAD_SESSIONS[sid] = {
                "total": total,
                "buf": bytearray(total),
                "off": 0,
                "t0": time.monotonic(),
                "mode": req_mode,
            }
            self._send(200, json_bytes({"ok": True, "sid": sid}))
            return

        if seg_part:
            sid = qs_first("sid") or (self.headers.get("X-SoTi-Session") or "").strip()
            off_s = qs_first("off") or (self.headers.get("X-SoTi-Offset") or "").strip()
            sess = UPLOAD_SESSIONS.get(sid)
            if sess is None:
                self._send(404, json_bytes({"ok": False, "error": "unknown sid", "answer": "会话无效。"}))
                return
            try:
                off = int(off_s)
            except ValueError:
                self._send(400, json_bytes({"ok": False, "error": "bad offset", "answer": "offset 无效。"}))
                return
            body = self._read_body(clen, 65536)
            if body is None:
                return
            if len(body) == 0:
                self._send(400, json_bytes({"ok": False, "error": "empty part", "answer": "空分片。"}))
                return
            if int(sess["off"]) != off:
                self._send(
                    409,
                    json_bytes(
                        {
                            "ok": False,
                            "error": "out of order",
                            "answer": "请按顺序上传，期望 offset=%d" % int(sess["off"]),
                        }
                    ),
                )
                return
            if off + len(body) > int(sess["total"]):
                self._send(400, json_bytes({"ok": False, "error": "overflow", "answer": "超出 total。"}))
                return
            sess["buf"][off : off + len(body)] = body
            sess["off"] = off + len(body)
            sess["t0"] = time.monotonic()
            self._send(200, json_bytes({"ok": True, "received": int(sess["off"])}))
            return

        if seg_commit:
            sid = ""
            if clen > 0:
                body = self._read_body(clen, 4096)
                if body is None:
                    return
                try:
                    o = json.loads(body.decode("utf-8"))
                    sid = str(o.get("sid", "")).strip()
                except (json.JSONDecodeError, TypeError):
                    self._send(400, json_bytes({"ok": False, "error": "bad json", "answer": "JSON 无效。"}))
                    return
            else:
                sid = qs_first("sid")
            if not sid:
                self._send(400, json_bytes({"ok": False, "error": "no sid", "answer": "缺少会话 id。"}))
                return
            sess = UPLOAD_SESSIONS.pop(sid, None)
            if sess is None:
                self._send(404, json_bytes({"ok": False, "error": "unknown sid", "answer": "会话无效或已提交。"}))
                return
            if int(sess["off"]) != int(sess["total"]):
                self._send(
                    400,
                    json_bytes({"ok": False, "error": "incomplete", "answer": "分片未传齐。"}),
                )
                return
            jpg = bytes(sess["buf"])
            if len(jpg) < 4 or jpg[0] != 0xFF or jpg[1] != 0xD8:
                self._send(400, json_bytes({"ok": False, "error": "Not a JPEG", "answer": "合成后不是 JPEG。"}))
                return
            seg_mode = normalize_mode(str(sess.get("mode") or "solve"))
            t0 = time.monotonic()
            try:
                img_bytes, media = preprocess_image_for_vision(jpg, seg_mode)
                answer = run_doubao_vision(img_bytes, media, seg_mode)
            except Exception as e:
                msg = str(e)
                self._send(502, vision_failure_json_bytes(msg))
                return
            self._maybe_record_history(seg_mode, answer, jpg, t0)
            self._send(200, json_bytes(device_upload_response(answer, seg_mode)))
            return

        if not is_upload_root:
            self.send_error(404)
            return

        te = (self.headers.get("Transfer-Encoding") or "").lower()
        if clen <= 0:
            sys.stderr.write(
                "soti-upload: bad POST clen=%r TE=%r from %s (use Nginx "
                "client_max_body_size + proxy_request_buffering on; avoid "
                "stripping Content-Length)\n"
                % (self.headers.get("Content-Length"), te, self.address_string())
            )
            self._send(400, json_bytes({"ok": False, "error": "Empty body", "answer": "Empty body"}))
            return

        body = self.rfile.read(clen)
        got = len(body)
        if got != clen:
            sys.stderr.write(
                "soti-upload: short read got=%d expected=%d from %s (client likely reset or timed out)\n"
                % (got, clen, self.address_string())
            )
            self._send(
                408,
                json_bytes(
                    {
                        "ok": False,
                        "error": "Incomplete JPEG upload",
                        "answer": "上传中断（仅收到 %d/%d 字节）。请检查 Wi‑Fi / Nginx client_body_timeout。"
                        % (got, clen),
                    }
                ),
            )
            return
        head = body[:16] if body else b""
        sys.stderr.write(
            "soti-upload: body got=%d clen=%d head=%s\n"
            % (got, clen, head.hex() if head else "")
        )
        if len(body) < 4 or body[0] != 0xFF or body[1] != 0xD8:
            self._send(
                400,
                json_bytes({"ok": False, "error": "Not a JPEG", "answer": "请上传 JPEG 图片。"}),
            )
            return

        t0 = time.monotonic()
        try:
            img_bytes, media = preprocess_image_for_vision(body, req_mode)
            answer = run_doubao_vision(img_bytes, media, req_mode)
        except Exception as e:
            msg = str(e)
            self._send(502, vision_failure_json_bytes(msg))
            return

        self._maybe_record_history(req_mode, answer, body, t0)
        self._send(200, json_bytes(device_upload_response(answer, req_mode)))


def main() -> None:
    addr = (LISTEN, PORT)
    # 多线程：设备搜题（豆包）耗时时，家长页 API 仍可响应，避免 Nginx 504
    httpd = ThreadingHTTPServer(addr, Handler)
    print(
        "SoTi: GET http://%s:%s/upload/health （无 Token，公网 curl 判是否打到本进程）"
        % (LISTEN, PORT),
        file=sys.stderr,
    )
    print(
        "Parent: http://%s:%s/parent/ （家长控制台；PARENT_PASSWORD 默认 parent2025）"
        % (LISTEN, PORT),
        file=sys.stderr,
    )
    print(
        "SoTi: 分片 POST /upload?mode=solve|translate|tutor|grade|summary&st=init|part|commit"
        "（JPEG>%d 字节）; GET /upload/daily",
        file=sys.stderr,
    )
    if not DOUBAO_API_KEY:
        print("WARN: DOUBAO_API_KEY 未设置，将返回演示文案。", file=sys.stderr)
    elif not DOUBAO_MODEL:
        print(
            "WARN: 已设置 DOUBAO_API_KEY 但未设置 DOUBAO_MODEL；豆包调用将失败。"
            "请 export DOUBAO_MODEL=ep-你的方舟推理接入点ID",
            file=sys.stderr,
        )
    try:
        import PIL  # noqa: F401

        if os.environ.get("SOTI_BINARIZE", "1").strip().lower() not in ("0", "false", "no"):
            print("SoTi: 已启用二值化预处理（Pillow），SOTI_BINARIZE=0 可关闭。", file=sys.stderr)
    except ImportError:
        if os.environ.get("SOTI_BINARIZE", "1").strip().lower() not in ("0", "false", "no"):
            print(
                "WARN: 默认开启二值化但未安装 Pillow；将使用原 JPEG。安装: pip install Pillow",
                file=sys.stderr,
            )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)


if __name__ == "__main__":
    main()
