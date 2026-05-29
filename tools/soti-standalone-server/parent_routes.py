"""HTTP handlers for /parent/* — used by soti_upload_server.Handler."""
from __future__ import annotations

import json
import mimetypes
import os
import secrets
import time
from http.server import BaseHTTPRequestHandler
from typing import Any
from urllib.parse import parse_qs, urlparse

from album_preprocess import album_normalize_image
from print_preprocess import print_normalize_image
from parent_store import (
    DEFAULT_DEVICE_ID,
    album_ack,
    album_insert,
    album_list_pending,
    album_read_chunk,
    print_ack,
    print_insert_image,
    print_insert_text,
    print_list_pending,
    print_read_chunk,
    print_read_text,
    chat_insert,
    chat_list_recent,
    chat_mark_read,
    chat_poll,
    chat_unread,
    get_history_item,
    get_policy,
    insert_history,
    list_history,
    save_policy,
    stats,
    thumb_path,
    touch_device,
)

STATIC_DIR = os.path.join(os.path.dirname(__file__), "static", "parent")
PARENT_PASSWORD = os.environ.get("PARENT_PASSWORD", "parent2025")
ALLOWED_DEVICE_CODES = {
    x.strip().upper()
    for x in os.environ.get("PARENT_DEVICE_CODES", "WT99-DEMO-01,WT99-DEMO").split(",")
    if x.strip()
}
UPLOAD_TOKEN = os.environ.get("UPLOAD_TOKEN", "esp32souti")

_sessions: dict[str, dict[str, Any]] = {}
SESSION_TTL = 86400.0


def _prune_sessions() -> None:
    now = time.monotonic()
    dead = [k for k, v in _sessions.items() if now - float(v["t0"]) > SESSION_TTL]
    for k in dead:
        del _sessions[k]


def _json_response(handler: BaseHTTPRequestHandler, code: int, obj: Any) -> None:
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    handler.send_response(code)
    handler.send_header("Content-Type", "application/json; charset=utf-8")
    handler.send_header("Content-Length", str(len(body)))
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.end_headers()
    handler.wfile.write(body)


def _read_json_body(handler: BaseHTTPRequestHandler) -> dict[str, Any] | None:
    try:
        clen = int(handler.headers.get("Content-Length", "0"))
    except ValueError:
        clen = 0
    if clen <= 0:
        return {}
    raw = handler.rfile.read(clen)
    try:
        return json.loads(raw.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None


def _bearer_token(handler: BaseHTTPRequestHandler) -> str:
    auth = handler.headers.get("Authorization") or ""
    if auth.startswith("Bearer "):
        return auth[7:].strip()
    return ""


def _session_ok(handler: BaseHTTPRequestHandler) -> str | None:
    _prune_sessions()
    tok = _bearer_token(handler)
    if not tok:
        return None
    sess = _sessions.get(tok)
    if sess is None:
        return None
    return str(sess.get("device_id") or DEFAULT_DEVICE_ID)


def _device_auth_ok(handler: BaseHTTPRequestHandler) -> bool:
    if not UPLOAD_TOKEN:
        return True
    return _bearer_token(handler) == UPLOAD_TOKEN


def _chat_device_id(handler: BaseHTTPRequestHandler, qs: dict, body: dict | None) -> str:
    sess = _session_ok(handler)
    if sess:
        return sess
    if body and body.get("device_id"):
        return str(body["device_id"]).strip().upper()
    hdr = handler.headers.get("X-Device-Id") or ""
    if hdr:
        return hdr.strip().upper()
    return str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()


def _chat_auth_ok(handler: BaseHTTPRequestHandler) -> bool:
    return _session_ok(handler) is not None or _device_auth_ok(handler)


def record_upload_history(
    device_id: str,
    mode: str,
    answer: str,
    jpeg: bytes | None,
    *,
    ok: bool = True,
    latency_ms: int | None = None,
    client_ip: str | None = None,
) -> None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    touch_device(device_id, client_ip)
    if not answer or answer.startswith("（演示）"):
        return
    if answer.startswith("搜题失败"):
        ok = False
    insert_history(
        device_id,
        mode,
        answer,
        ok=ok,
        latency_ms=latency_ms,
        jpeg=jpeg,
    )


def handle_options(handler: BaseHTTPRequestHandler) -> bool:
    handler.send_response(204)
    handler.send_header("Access-Control-Allow-Origin", "*")
    handler.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS")
    handler.send_header(
        "Access-Control-Allow-Headers",
        "Content-Type, Authorization, X-Device-Id",
    )
    handler.send_header("Access-Control-Max-Age", "86400")
    handler.end_headers()
    return True


def handle_get(handler: BaseHTTPRequestHandler) -> bool:
    parsed = urlparse(handler.path)
    path = parsed.path or "/"
    qs = parse_qs(parsed.query)

    if path in ("/parent", "/parent/"):
        return _serve_static(handler, "index.html")

    if path.startswith("/parent/static/"):
        name = path[len("/parent/static/") :]
        return _serve_static(handler, name)

    for name in ("styles.css", "app.js", "demo_data.json"):
        if path == "/parent/" + name:
            return _serve_static(handler, name)

    if path == "/parent/api/demo":
        return _serve_static(handler, "demo_data.json")

    if path == "/parent/api/stats":
        device_id = _session_ok(handler)
        if not device_id:
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        st = stats(device_id)
        pol = get_policy(device_id)
        _json_response(
            handler,
            200,
            {
                "ok": True,
                "stats": st,
                "policy_name": _active_rule_name(pol),
                "device_id": device_id,
            },
        )
        return True

    if path == "/parent/api/history":
        device_id = _session_ok(handler)
        if not device_id:
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        limit = int(qs.get("limit", ["30"])[0])
        offset = int(qs.get("offset", ["0"])[0])
        items = list_history(device_id, limit=limit, offset=offset)
        _json_response(handler, 200, {"ok": True, "items": items})
        return True

    if path.startswith("/parent/api/history/"):
        device_id = _session_ok(handler)
        if not device_id:
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        try:
            rid = int(path.split("/")[-1])
        except ValueError:
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        item = get_history_item(rid, device_id)
        if item is None:
            _json_response(handler, 404, {"ok": False, "error": "not found"})
        else:
            _json_response(handler, 200, {"ok": True, "item": item})
        return True

    if path.startswith("/parent/api/thumb/"):
        name = path[len("/parent/api/thumb/") :]
        fpath = thumb_path(name)
        if fpath is None:
            handler.send_error(404)
            return True
        try:
            with open(fpath, "rb") as f:
                data = f.read()
        except OSError:
            handler.send_error(404)
            return True
        handler.send_response(200)
        handler.send_header("Content-Type", "image/jpeg")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Cache-Control", "public, max-age=3600")
        handler.end_headers()
        handler.wfile.write(data)
        return True

    if path == "/parent/api/policy":
        device_id = qs.get("device_id", [DEFAULT_DEVICE_ID])[0].strip().upper()
        if not _session_ok(handler) and not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        touch_device(device_id, handler.client_address[0])
        pol = get_policy(device_id)
        _json_response(
            handler,
            200,
            {"ok": True, "device_id": device_id, "policy": pol, "server_time": time.time()},
        )
        return True

    if path == "/parent/api/chat/messages":
        if not _chat_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        device_id = _chat_device_id(handler, qs, None)
        limit = int(qs.get("limit", ["50"])[0])
        items = chat_list_recent(device_id, limit=limit)
        unread = chat_unread(device_id)
        _json_response(
            handler,
            200,
            {"ok": True, "items": items, "unread": unread, "server_time": time.time()},
        )
        return True

    if path == "/parent/api/chat/poll":
        if not _chat_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        device_id = _chat_device_id(handler, qs, None)
        since_id = int(qs.get("since", ["0"])[0])
        limit = int(qs.get("limit", ["50"])[0])
        items = chat_poll(device_id, since_id=since_id, limit=limit)
        unread = chat_unread(device_id)
        _json_response(
            handler,
            200,
            {
                "ok": True,
                "items": items,
                "unread": unread,
                "latest_id": unread.get("latest_id", 0),
                "server_time": time.time(),
            },
        )
        return True

    if path == "/parent/api/chat/unread":
        if not _chat_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        device_id = _chat_device_id(handler, qs, None)
        unread = chat_unread(device_id)
        _json_response(handler, 200, {"ok": True, "unread": unread})
        return True

    if path == "/parent/api/album/poll":
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        device_id = str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()
        if handler.headers.get("X-Device-Id"):
            device_id = handler.headers.get("X-Device-Id", device_id).strip().upper()
        touch_device(device_id, handler.client_address[0])
        items = album_list_pending(device_id)
        _json_response(handler, 200, {"ok": True, "items": items})
        return True

    if path.startswith("/parent/api/album/file/"):
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        try:
            album_id = int(path.split("/")[-1])
        except ValueError:
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        device_id = str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()
        if handler.headers.get("X-Device-Id"):
            device_id = handler.headers.get("X-Device-Id", device_id).strip().upper()
        off = int(qs.get("off", ["0"])[0])
        length = int(qs.get("len", ["4096"])[0])
        data = album_read_chunk(album_id, device_id, off, length)
        if data is None:
            handler.send_error(404)
            return True
        handler.send_response(200)
        handler.send_header("Content-Type", "application/octet-stream")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Connection", "close")
        handler.end_headers()
        handler.wfile.write(data)
        return True

    if path == "/parent/api/print/poll":
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        device_id = str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()
        if handler.headers.get("X-Device-Id"):
            device_id = handler.headers.get("X-Device-Id", device_id).strip().upper()
        touch_device(device_id, handler.client_address[0])
        items = print_list_pending(device_id)
        _json_response(handler, 200, {"ok": True, "items": items})
        return True

    if path.startswith("/parent/api/print/file/"):
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        try:
            print_id = int(path.split("/")[-1])
        except ValueError:
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        device_id = str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()
        if handler.headers.get("X-Device-Id"):
            device_id = handler.headers.get("X-Device-Id", device_id).strip().upper()
        off = int(qs.get("off", ["0"])[0])
        length = int(qs.get("len", ["4096"])[0])
        data = print_read_chunk(print_id, device_id, off, length)
        if data is None:
            handler.send_error(404)
            return True
        handler.send_response(200)
        handler.send_header("Content-Type", "application/octet-stream")
        handler.send_header("Content-Length", str(len(data)))
        handler.send_header("Connection", "close")
        handler.end_headers()
        handler.wfile.write(data)
        return True

    if path.startswith("/parent/api/print/text/"):
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        try:
            print_id = int(path.split("/")[-1])
        except ValueError:
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        device_id = str(qs.get("device_id", [DEFAULT_DEVICE_ID])[0]).strip().upper()
        if handler.headers.get("X-Device-Id"):
            device_id = handler.headers.get("X-Device-Id", device_id).strip().upper()
        body = print_read_text(print_id, device_id)
        if body is None:
            _json_response(handler, 404, {"ok": False, "error": "not found"})
            return True
        _json_response(handler, 200, {"ok": True, "body": body})
        return True

    return False


def handle_post(handler: BaseHTTPRequestHandler) -> bool:
    parsed = urlparse(handler.path)
    path = parsed.path or "/"

    if path == "/parent/api/login":
        body = _read_json_body(handler)
        if body is None:
            _json_response(handler, 400, {"ok": False, "error": "bad json"})
            return True
        password = str(body.get("password") or "")
        device_code = str(body.get("device_code") or "").strip().upper()
        if password != PARENT_PASSWORD:
            _json_response(handler, 401, {"ok": False, "error": "密码错误"})
            return True
        if device_code not in ALLOWED_DEVICE_CODES:
            _json_response(handler, 403, {"ok": False, "error": "设备码无效"})
            return True
        tok = secrets.token_urlsafe(24)
        _sessions[tok] = {"device_id": device_code, "t0": time.monotonic()}
        _json_response(
            handler,
            200,
            {
                "ok": True,
                "token": tok,
                "device_id": device_code,
                "allowed_devices": sorted(ALLOWED_DEVICE_CODES),
            },
        )
        return True

    if path == "/parent/api/heartbeat":
        body = _read_json_body(handler) or {}
        device_id = str(
            body.get("device_id") or handler.headers.get("X-Device-Id") or DEFAULT_DEVICE_ID
        ).strip().upper()
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        touch_device(device_id, handler.client_address[0])
        _json_response(handler, 200, {"ok": True})
        return True

    if path == "/parent/api/chat/send":
        if not _chat_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        body = _read_json_body(handler)
        if body is None:
            _json_response(handler, 400, {"ok": False, "error": "bad json"})
            return True
        device_id = _chat_device_id(handler, {}, body)
        touch_device(device_id, handler.client_address[0])
        sender = "parent" if _session_ok(handler) else "child"
        try:
            msg = chat_insert(
                device_id,
                sender,
                str(body.get("body") or ""),
                client_msg_id=str(body.get("client_msg_id") or "") or None,
            )
            unread = chat_unread(device_id)
            _json_response(
                handler,
                200,
                {"ok": True, "message": msg, "unread": unread},
            )
        except ValueError as e:
            _json_response(handler, 400, {"ok": False, "error": str(e)})
        return True

    if path == "/parent/api/chat/read":
        if not _chat_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        body = _read_json_body(handler) or {}
        device_id = _chat_device_id(handler, {}, body)
        role = "parent" if _session_ok(handler) else "child"
        if body.get("role") in ("parent", "child"):
            role = str(body["role"])
        try:
            chat_mark_read(device_id, role, int(body.get("up_to_id", 0)))
            unread = chat_unread(device_id)
            _json_response(handler, 200, {"ok": True, "unread": unread})
        except ValueError as e:
            _json_response(handler, 400, {"ok": False, "error": str(e)})
        return True

    if path == "/parent/api/album/upload":
        device_id = _session_ok(handler)
        if not device_id:
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        ctype = handler.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype:
            _json_response(handler, 400, {"ok": False, "error": "need multipart"})
            return True
        try:
            import cgi

            env = {
                "REQUEST_METHOD": "POST",
                "CONTENT_TYPE": ctype,
                "CONTENT_LENGTH": handler.headers.get("Content-Length", "0"),
            }
            form = cgi.FieldStorage(fp=handler.rfile, headers=handler.headers, environ=env)
            if "file" not in form:
                _json_response(handler, 400, {"ok": False, "error": "no file"})
                return True
            file_item = form["file"]
            if not file_item.file:
                _json_response(handler, 400, {"ok": False, "error": "empty file"})
                return True
            raw = file_item.file.read()
            fname = file_item.filename or "photo.jpg"
            if "name" in form and form["name"].value:
                fname = str(form["name"].value)
            meta = album_normalize_image(raw)
            jpeg = meta["jpeg"]
            row = album_insert(device_id, fname, jpeg)
            row["width"] = meta["width"]
            row["height"] = meta["height"]
            row["original_size"] = meta["original_size"]
            row["processed_size"] = meta["processed_size"]
            row["source_width"] = meta["source_width"]
            row["source_height"] = meta["source_height"]
            _json_response(handler, 200, {"ok": True, "item": row})
        except ValueError as e:
            _json_response(handler, 400, {"ok": False, "error": str(e)})
        return True

    if path == "/parent/api/album/ack":
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        body = _read_json_body(handler)
        if body is None:
            _json_response(handler, 400, {"ok": False, "error": "bad json"})
            return True
        device_id = str(
            body.get("device_id") or handler.headers.get("X-Device-Id") or DEFAULT_DEVICE_ID
        ).strip().upper()
        try:
            album_id = int(body.get("id", 0))
        except (TypeError, ValueError):
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        if not album_ack(album_id, device_id):
            _json_response(handler, 404, {"ok": False, "error": "not found"})
        else:
            _json_response(handler, 200, {"ok": True})
        return True

    if path == "/parent/api/print/upload":
        device_id = _session_ok(handler)
        if not device_id:
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        ctype = handler.headers.get("Content-Type", "")
        if "multipart/form-data" not in ctype:
            _json_response(handler, 400, {"ok": False, "error": "need multipart"})
            return True
        try:
            import cgi

            env = {
                "REQUEST_METHOD": "POST",
                "CONTENT_TYPE": ctype,
                "CONTENT_LENGTH": handler.headers.get("Content-Length", "0"),
            }
            form = cgi.FieldStorage(fp=handler.rfile, headers=handler.headers, environ=env)
            job_type = "image"
            if "type" in form and form["type"].value:
                job_type = str(form["type"].value).strip().lower()
            if job_type == "text":
                body_field = form.getvalue("body") if "body" in form else ""
                if not body_field:
                    _json_response(handler, 400, {"ok": False, "error": "empty text"})
                    return True
                title = ""
                if "name" in form and form["name"].value:
                    title = str(form["name"].value).strip()
                text_bytes = body_field.encode("utf-8") if isinstance(body_field, str) else body_field
                row = print_insert_text(device_id, title, text_bytes)
                _json_response(handler, 200, {"ok": True, "item": row})
                return True
            if job_type != "image":
                _json_response(handler, 400, {"ok": False, "error": "bad type"})
                return True
            if "file" not in form:
                _json_response(handler, 400, {"ok": False, "error": "no file"})
                return True
            file_item = form["file"]
            if not file_item.file:
                _json_response(handler, 400, {"ok": False, "error": "empty file"})
                return True
            raw = file_item.file.read()
            fname = file_item.filename or "print.jpg"
            if "name" in form and form["name"].value:
                fname = str(form["name"].value)
            binarize = True
            if "binarize" in form and form["binarize"].value is not None:
                bv = str(form["binarize"].value).strip().lower()
                binarize = bv not in ("0", "false", "no", "off")
            meta = print_normalize_image(raw, binarize=binarize)
            jpeg = meta["jpeg"]
            row = print_insert_image(
                device_id,
                fname,
                jpeg,
                int(meta["width"]),
                int(meta["height"]),
                meta={"binarize": meta.get("binarize", binarize)},
            )
            row["width"] = meta["width"]
            row["height"] = meta["height"]
            row["original_size"] = meta["original_size"]
            row["processed_size"] = meta["processed_size"]
            row["source_width"] = meta["source_width"]
            row["source_height"] = meta["source_height"]
            _json_response(handler, 200, {"ok": True, "item": row})
        except ValueError as e:
            _json_response(handler, 400, {"ok": False, "error": str(e)})
        return True

    if path == "/parent/api/print/ack":
        if not _device_auth_ok(handler):
            _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
            return True
        body = _read_json_body(handler)
        if body is None:
            _json_response(handler, 400, {"ok": False, "error": "bad json"})
            return True
        device_id = str(
            body.get("device_id") or handler.headers.get("X-Device-Id") or DEFAULT_DEVICE_ID
        ).strip().upper()
        try:
            print_id = int(body.get("id", 0))
        except (TypeError, ValueError):
            _json_response(handler, 400, {"ok": False, "error": "bad id"})
            return True
        if not print_ack(print_id, device_id):
            _json_response(handler, 404, {"ok": False, "error": "not found"})
        else:
            _json_response(handler, 200, {"ok": True})
        return True

    return False


def handle_put(handler: BaseHTTPRequestHandler) -> bool:
    parsed = urlparse(handler.path)
    if parsed.path != "/parent/api/policy":
        return False
    device_id = _session_ok(handler)
    if not device_id:
        _json_response(handler, 401, {"ok": False, "error": "unauthorized"})
        return True
    body = _read_json_body(handler)
    if body is None or "policy" not in body:
        _json_response(handler, 400, {"ok": False, "error": "need policy"})
        return True
    save_policy(device_id, body["policy"])
    _json_response(handler, 200, {"ok": True})
    return True


def _active_rule_name(policy: dict[str, Any]) -> str:
    if policy.get("demo_force_study"):
        return "演示·学习时段"
    rules = policy.get("rules") or []
    if not rules:
        return "默认"
    return str(rules[-1].get("name") or "已配置")


def _serve_static(handler: BaseHTTPRequestHandler, name: str) -> bool:
    if ".." in name or name.startswith("/"):
        handler.send_error(403)
        return True
    fpath = os.path.join(STATIC_DIR, name)
    if not os.path.isfile(fpath):
        handler.send_error(404)
        return True
    ctype, _ = mimetypes.guess_type(fpath)
    if ctype is None:
        ctype = "application/octet-stream"
    with open(fpath, "rb") as f:
        data = f.read()
    handler.send_response(200)
    handler.send_header("Content-Type", ctype)
    handler.send_header("Content-Length", str(len(data)))
    if name.endswith((".css", ".js", ".json", ".html")):
        handler.send_header("Cache-Control", "no-cache")
    handler.end_headers()
    handler.wfile.write(data)
    return True
