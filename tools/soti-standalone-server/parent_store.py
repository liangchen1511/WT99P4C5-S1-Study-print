"""SQLite storage for parent dashboard: history + device policy."""
from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
from io import BytesIO
from typing import Any

_lock = threading.Lock()
_conn: sqlite3.Connection | None = None

DATA_DIR = os.environ.get("PARENT_DATA_DIR", os.path.join(os.path.dirname(__file__), "data"))
DB_PATH = os.path.join(DATA_DIR, "parent.db")
THUMB_DIR = os.path.join(DATA_DIR, "thumbs")
ALBUM_INBOX_DIR = os.path.join(DATA_DIR, "album_inbox")
MAX_ALBUM_FILE_BYTES = int(os.environ.get("PARENT_ALBUM_MAX_FILE", str(4 * 1024 * 1024)))
MAX_ALBUM_PENDING = int(os.environ.get("PARENT_ALBUM_MAX_PENDING", "10"))
ALBUM_TTL_SEC = float(os.environ.get("PARENT_ALBUM_TTL_SEC", "86400"))
MAX_HISTORY_ROWS = int(os.environ.get("PARENT_MAX_HISTORY", "500"))
MAX_CHAT_ROWS = int(os.environ.get("PARENT_MAX_CHAT", "200"))
CHAT_MAX_BODY_LEN = 500
DEFAULT_DEVICE_ID = os.environ.get("PARENT_DEFAULT_DEVICE_ID", "WT99-DEMO-01")

DEFAULT_POLICY: dict[str, Any] = {
    "version": 1,
    "timezone": "Asia/Shanghai",
    "demo_force_study": False,
    "rules": [
        {
            "name": "周末自由",
            "days": [6, 7],
            "start": "00:00",
            "end": "23:59",
            "allow_apps": ["*"],
            "deny_apps": [],
        },
        {
            "name": "工作日学习",
            "days": [1, 2, 3, 4, 5],
            "start": "19:00",
            "end": "21:00",
            "allow_apps": ["*"],
            "deny_apps": ["game_2048", "video", "music"],
        },
    ],
    "device_lock_outside_rules": False,
    "app_toggles": {
        "game_2048": True,
        "video": True,
        "music": True,
        "camera": True,
        "print": True,
    },
}


def _migrate_study_deny_camera_print(policy: dict[str, Any]) -> dict[str, Any]:
    """旧版学习时段用窄 allow 白名单会误拦视频/音乐；统一为 allow * + deny 列表。"""
    rules = policy.get("rules")
    if not isinstance(rules, list):
        return policy
    out = dict(policy)
    new_rules: list[Any] = []
    changed = False
    for rule in rules:
        if not isinstance(rule, dict):
            new_rules.append(rule)
            continue
        r = dict(rule)
        allow = list(r.get("allow_apps") or [])
        if allow and "*" not in allow:
            r["allow_apps"] = ["*"]
            changed = True
        new_rules.append(r)
    if changed:
        out["rules"] = new_rules
    return out


def _connect() -> sqlite3.Connection:
    global _conn
    if _conn is not None:
        return _conn
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(THUMB_DIR, exist_ok=True)
    _conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    _conn.row_factory = sqlite3.Row
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at REAL NOT NULL,
            device_id TEXT NOT NULL,
            mode TEXT NOT NULL,
            answer_text TEXT NOT NULL,
            ok INTEGER NOT NULL DEFAULT 1,
            latency_ms INTEGER,
            thumb_file TEXT
        )
        """
    )
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS policy (
            device_id TEXT PRIMARY KEY,
            json TEXT NOT NULL,
            updated_at REAL NOT NULL
        )
        """
    )
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS device_heartbeat (
            device_id TEXT PRIMARY KEY,
            last_seen REAL NOT NULL,
            ip TEXT
        )
        """
    )
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS chat_message (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            created_at REAL NOT NULL,
            device_id TEXT NOT NULL,
            sender TEXT NOT NULL,
            body TEXT NOT NULL,
            client_msg_id TEXT
        )
        """
    )
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS chat_read (
            device_id TEXT PRIMARY KEY,
            parent_last_read_id INTEGER NOT NULL DEFAULT 0,
            child_last_read_id INTEGER NOT NULL DEFAULT 0
        )
        """
    )
    _conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_chat_device_id ON chat_message(device_id, id)"
    )
    _conn.execute(
        """
        CREATE TABLE IF NOT EXISTS album_inbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            device_id TEXT NOT NULL,
            safe_name TEXT NOT NULL,
            file_path TEXT NOT NULL,
            size INTEGER NOT NULL,
            created_at REAL NOT NULL,
            acked_at REAL
        )
        """
    )
    _conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_album_device_pending ON album_inbox(device_id, acked_at)"
    )
    os.makedirs(ALBUM_INBOX_DIR, exist_ok=True)
    _conn.execute("PRAGMA journal_mode=WAL")
    _conn.commit()
    return _conn


def _upsert_policy_row(c: sqlite3.Connection, device_id: str, policy: dict[str, Any]) -> None:
    """须在已持有 _lock 时调用，不可再调 save_policy（避免死锁）。"""
    c.execute(
        """
        INSERT INTO policy (device_id, json, updated_at) VALUES (?, ?, ?)
        ON CONFLICT(device_id) DO UPDATE SET json=excluded.json, updated_at=excluded.updated_at
        """,
        (device_id, json.dumps(policy, ensure_ascii=False), time.time()),
    )


def ensure_default_policy(device_id: str) -> None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        row = c.execute("SELECT 1 FROM policy WHERE device_id=?", (device_id,)).fetchone()
        if row is None:
            _upsert_policy_row(c, device_id, DEFAULT_POLICY)
            c.commit()


def save_thumb(jpeg: bytes, record_id: int) -> str | None:
    if not jpeg or len(jpeg) < 4:
        return None
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        im = Image.open(BytesIO(jpeg))
        im.thumbnail((320, 320))
        name = "h%d.jpg" % record_id
        path = os.path.join(THUMB_DIR, name)
        im.save(path, format="JPEG", quality=72, optimize=True)
        return name
    except Exception:
        return None


def insert_history(
    device_id: str,
    mode: str,
    answer_text: str,
    *,
    ok: bool = True,
    latency_ms: int | None = None,
    jpeg: bytes | None = None,
) -> int:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        now = time.time()
        cur = c.execute(
            """
            INSERT INTO history (created_at, device_id, mode, answer_text, ok, latency_ms, thumb_file)
            VALUES (?, ?, ?, ?, ?, ?, NULL)
            """,
            (now, device_id, mode, answer_text[:32000], 1 if ok else 0, latency_ms),
        )
        rid = int(cur.lastrowid)
        c.commit()
    thumb = save_thumb(jpeg, rid) if jpeg else None
    if thumb:
        with _lock:
            c = _connect()
            c.execute("UPDATE history SET thumb_file=? WHERE id=?", (thumb, rid))
            c.commit()
    with _lock:
        c = _connect()
        count = c.execute("SELECT COUNT(*) FROM history").fetchone()[0]
        if count > MAX_HISTORY_ROWS:
            excess = count - MAX_HISTORY_ROWS
            olds = c.execute(
                "SELECT id, thumb_file FROM history ORDER BY created_at ASC LIMIT ?",
                (excess,),
            ).fetchall()
            for row in olds:
                if row["thumb_file"]:
                    try:
                        os.remove(os.path.join(THUMB_DIR, row["thumb_file"]))
                    except OSError:
                        pass
                c.execute("DELETE FROM history WHERE id=?", (row["id"],))
        c.commit()
    return rid


def list_history(device_id: str, limit: int = 50, offset: int = 0) -> list[dict[str, Any]]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        rows = c.execute(
            """
            SELECT id, created_at, device_id, mode, answer_text, ok, latency_ms, thumb_file
            FROM history WHERE device_id=?
            ORDER BY created_at DESC LIMIT ? OFFSET ?
            """,
            (device_id, limit, offset),
        ).fetchall()
    out = []
    for r in rows:
        d = dict(r)
        d["preview"] = (d["answer_text"] or "")[:120]
        d["thumb_url"] = (
            "/parent/api/thumb/" + d["thumb_file"] if d.get("thumb_file") else None
        )
        del d["thumb_file"]
        out.append(d)
    return out


def get_history_item(record_id: int, device_id: str | None = None) -> dict[str, Any] | None:
    with _lock:
        c = _connect()
        if device_id:
            row = c.execute(
                "SELECT * FROM history WHERE id=? AND device_id=?",
                (record_id, device_id),
            ).fetchone()
        else:
            row = c.execute("SELECT * FROM history WHERE id=?", (record_id,)).fetchone()
    if row is None:
        return None
    d = dict(row)
    d["thumb_url"] = (
        "/parent/api/thumb/" + d["thumb_file"] if d.get("thumb_file") else None
    )
    return d


def thumb_path(filename: str) -> str | None:
    if not filename or "/" in filename or "\\" in filename:
        return None
    path = os.path.join(THUMB_DIR, filename)
    return path if os.path.isfile(path) else None


def stats(device_id: str) -> dict[str, Any]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    now = time.time()
    day_start = now - (now % 86400) - 28800
    week_start = now - 7 * 86400
    with _lock:
        c = _connect()
        today = c.execute(
            "SELECT COUNT(*) FROM history WHERE device_id=? AND created_at>=? AND ok=1",
            (device_id, day_start),
        ).fetchone()[0]
        week = c.execute(
            "SELECT COUNT(*) FROM history WHERE device_id=? AND created_at>=? AND ok=1",
            (device_id, week_start),
        ).fetchone()[0]
        hb = c.execute(
            "SELECT last_seen FROM device_heartbeat WHERE device_id=?",
            (device_id,),
        ).fetchone()
    online = hb is not None and (now - float(hb["last_seen"])) < 600
    return {"today": today, "week": week, "online": online}


def touch_device(device_id: str, ip: str | None = None) -> None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        c.execute(
            """
            INSERT INTO device_heartbeat (device_id, last_seen, ip) VALUES (?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET last_seen=excluded.last_seen, ip=excluded.ip
            """,
            (device_id, time.time(), ip or ""),
        )
        c.commit()


def get_policy(device_id: str) -> dict[str, Any]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    ensure_default_policy(device_id)
    with _lock:
        c = _connect()
        row = c.execute("SELECT json FROM policy WHERE device_id=?", (device_id,)).fetchone()
    if row is None:
        return dict(DEFAULT_POLICY)
    try:
        pol = json.loads(row["json"])
    except json.JSONDecodeError:
        return dict(DEFAULT_POLICY)
    return _migrate_study_deny_camera_print(pol)


def save_policy(device_id: str, policy: dict[str, Any]) -> None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    policy = _migrate_study_deny_camera_print(policy)
    with _lock:
        c = _connect()
        _upsert_policy_row(c, device_id, policy)
        c.commit()


def _ensure_chat_read_row(c: sqlite3.Connection, device_id: str) -> None:
    c.execute(
        """
        INSERT INTO chat_read (device_id, parent_last_read_id, child_last_read_id)
        VALUES (?, 0, 0)
        ON CONFLICT(device_id) DO NOTHING
        """,
        (device_id,),
    )


def _prune_chat(c: sqlite3.Connection, device_id: str) -> None:
    count = c.execute(
        "SELECT COUNT(*) FROM chat_message WHERE device_id=?", (device_id,)
    ).fetchone()[0]
    if count <= MAX_CHAT_ROWS:
        return
    excess = count - MAX_CHAT_ROWS
    olds = c.execute(
        """
        SELECT id FROM chat_message WHERE device_id=?
        ORDER BY id ASC LIMIT ?
        """,
        (device_id, excess),
    ).fetchall()
    for row in olds:
        c.execute("DELETE FROM chat_message WHERE id=?", (row["id"],))


def chat_insert(
    device_id: str,
    sender: str,
    body: str,
    *,
    client_msg_id: str | None = None,
) -> dict[str, Any]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    sender = (sender or "").strip().lower()
    if sender not in ("parent", "child"):
        raise ValueError("invalid sender")
    body = (body or "").strip()
    if not body:
        raise ValueError("empty body")
    if len(body) > CHAT_MAX_BODY_LEN:
        body = body[:CHAT_MAX_BODY_LEN]
    with _lock:
        c = _connect()
        if client_msg_id:
            dup = c.execute(
                """
                SELECT id, created_at, device_id, sender, body
                FROM chat_message
                WHERE device_id=? AND client_msg_id=?
                """,
                (device_id, client_msg_id),
            ).fetchone()
            if dup is not None:
                return dict(dup)
        now = time.time()
        cur = c.execute(
            """
            INSERT INTO chat_message (created_at, device_id, sender, body, client_msg_id)
            VALUES (?, ?, ?, ?, ?)
            """,
            (now, device_id, sender, body, client_msg_id or None),
        )
        rid = int(cur.lastrowid)
        _ensure_chat_read_row(c, device_id)
        _prune_chat(c, device_id)
        c.commit()
        row = c.execute(
            "SELECT id, created_at, device_id, sender, body FROM chat_message WHERE id=?",
            (rid,),
        ).fetchone()
    return dict(row)


def chat_list_recent(device_id: str, limit: int = 50) -> list[dict[str, Any]]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    limit = max(1, min(limit, 100))
    with _lock:
        c = _connect()
        rows = c.execute(
            """
            SELECT id, created_at, device_id, sender, body
            FROM chat_message WHERE device_id=?
            ORDER BY id DESC LIMIT ?
            """,
            (device_id, limit),
        ).fetchall()
    items = [dict(r) for r in reversed(rows)]
    return items


def chat_poll(device_id: str, since_id: int = 0, limit: int = 50) -> list[dict[str, Any]]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    since_id = max(0, int(since_id))
    limit = max(1, min(limit, 100))
    with _lock:
        c = _connect()
        rows = c.execute(
            """
            SELECT id, created_at, device_id, sender, body
            FROM chat_message
            WHERE device_id=? AND id>?
            ORDER BY id ASC LIMIT ?
            """,
            (device_id, since_id, limit),
        ).fetchall()
    return [dict(r) for r in rows]


def chat_latest_id(device_id: str) -> int:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        row = c.execute(
            "SELECT MAX(id) AS mid FROM chat_message WHERE device_id=?",
            (device_id,),
        ).fetchone()
    return int(row["mid"] or 0) if row else 0


def chat_mark_read(device_id: str, role: str, up_to_id: int) -> None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    role = (role or "").strip().lower()
    up_to_id = max(0, int(up_to_id))
    col = "parent_last_read_id" if role == "parent" else "child_last_read_id"
    if role not in ("parent", "child"):
        raise ValueError("invalid role")
    with _lock:
        c = _connect()
        _ensure_chat_read_row(c, device_id)
        c.execute(
            f"""
            UPDATE chat_read SET {col}=MAX({col}, ?) WHERE device_id=?
            """,
            (up_to_id, device_id),
        )
        c.commit()


def chat_unread(device_id: str) -> dict[str, int]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip() or DEFAULT_DEVICE_ID
    with _lock:
        c = _connect()
        _ensure_chat_read_row(c, device_id)
        rd = c.execute(
            "SELECT parent_last_read_id, child_last_read_id FROM chat_read WHERE device_id=?",
            (device_id,),
        ).fetchone()
        parent_lr = int(rd["parent_last_read_id"]) if rd else 0
        child_lr = int(rd["child_last_read_id"]) if rd else 0
        parent_unread = c.execute(
            """
            SELECT COUNT(*) FROM chat_message
            WHERE device_id=? AND sender='child' AND id>?
            """,
            (device_id, parent_lr),
        ).fetchone()[0]
        child_unread = c.execute(
            """
            SELECT COUNT(*) FROM chat_message
            WHERE device_id=? AND sender='parent' AND id>?
            """,
            (device_id, child_lr),
        ).fetchone()[0]
        latest_row = c.execute(
            "SELECT MAX(id) AS mid FROM chat_message WHERE device_id=?",
            (device_id,),
        ).fetchone()
        latest_id = int(latest_row["mid"] or 0) if latest_row else 0
    return {
        "parent_unread": int(parent_unread),
        "child_unread": int(child_unread),
        "latest_id": latest_id,
    }


def _sanitize_album_name(name: str) -> str:
    """Normalize any client filename (incl. Chinese/wechat names) to a safe .jpg name."""
    import re

    base = os.path.basename(name or "photo.jpg").strip()
    if ".." in base or not base:
        raise ValueError("invalid name")
    stem, ext = os.path.splitext(base)
    # Stored file is always JPEG after album_preprocess; accept common upload extensions.
    if ext.lower() not in (".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp"):
        if re.search(r"\.(jpe?g|png|webp|gif|bmp)$", base, re.I):
            stem = re.sub(r"\.(jpe?g|png|webp|gif|bmp)$", "", base, flags=re.I)
        else:
            stem = stem or "photo"
    stem = re.sub(r"[^A-Za-z0-9._-]+", "_", stem).strip("._-")
    if not stem:
        stem = "photo"
    if len(stem) > 48:
        stem = stem[:48]
    return f"{stem}.jpg"


def album_prune() -> None:
    now = time.time()
    with _lock:
        c = _connect()
        rows = c.execute(
            "SELECT id, file_path FROM album_inbox WHERE acked_at IS NOT NULL OR created_at < ?",
            (now - ALBUM_TTL_SEC,),
        ).fetchall()
        for row in rows:
            try:
                os.remove(row["file_path"])
            except OSError:
                pass
            c.execute("DELETE FROM album_inbox WHERE id=?", (row["id"],))
        c.commit()


def album_pending_count(device_id: str) -> int:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip().upper()
    with _lock:
        c = _connect()
        n = c.execute(
            "SELECT COUNT(*) FROM album_inbox WHERE device_id=? AND acked_at IS NULL",
            (device_id,),
        ).fetchone()[0]
    return int(n)


def album_list_pending(device_id: str) -> list[dict[str, Any]]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip().upper()
    album_prune()
    with _lock:
        c = _connect()
        rows = c.execute(
            """
            SELECT id, safe_name, size, created_at, file_path FROM album_inbox
            WHERE device_id=? AND acked_at IS NULL
            ORDER BY id ASC
            """,
            (device_id,),
        ).fetchall()
        out: list[dict[str, Any]] = []
        for r in rows:
            path = (r["file_path"] or "").strip()
            if not path or not os.path.isfile(path):
                c.execute("DELETE FROM album_inbox WHERE id=?", (r["id"],))
                continue
            out.append(
                {
                    "id": int(r["id"]),
                    "name": str(r["safe_name"]),
                    "size": int(r["size"]),
                    "created_at": float(r["created_at"]),
                }
            )
        c.commit()
        return out


def album_insert(device_id: str, safe_name: str, jpeg: bytes) -> dict[str, Any]:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip().upper()
    if not jpeg or len(jpeg) < 512:
        raise ValueError("file too small")
    if len(jpeg) > MAX_ALBUM_FILE_BYTES:
        raise ValueError("file too large")
    if jpeg[0] != 0xFF or jpeg[1] != 0xD8:
        raise ValueError("not jpeg (若为 HEIC/PNG 请先转为 JPG)")
    safe_name = _sanitize_album_name(safe_name)
    album_prune()
    with _lock:
        c = _connect()
        pending = c.execute(
            "SELECT COUNT(*) FROM album_inbox WHERE device_id=? AND acked_at IS NULL",
            (device_id,),
        ).fetchone()[0]
        if pending >= MAX_ALBUM_PENDING:
            raise ValueError("too many pending")
        dev_dir = os.path.join(ALBUM_INBOX_DIR, device_id)
        os.makedirs(dev_dir, exist_ok=True)
        now = time.time()
        cur = c.execute(
            """
            INSERT INTO album_inbox (device_id, safe_name, file_path, size, created_at)
            VALUES (?, ?, '', ?, ?)
            """,
            (device_id, safe_name, len(jpeg), now),
        )
        rid = int(cur.lastrowid)
        fpath = os.path.join(dev_dir, f"{rid}_{safe_name}")
        with open(fpath, "wb") as f:
            f.write(jpeg)
        c.execute("UPDATE album_inbox SET file_path=? WHERE id=?", (fpath, rid))
        c.commit()
    return {"id": rid, "name": safe_name, "size": len(jpeg)}


def album_read_chunk(album_id: int, device_id: str, offset: int, length: int) -> bytes | None:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip().upper()
    length = min(max(length, 1), 65536)
    with _lock:
        c = _connect()
        row = c.execute(
            "SELECT file_path, size FROM album_inbox WHERE id=? AND device_id=? AND acked_at IS NULL",
            (album_id, device_id),
        ).fetchone()
    if row is None:
        return None
    fpath = row["file_path"] or ""
    size = int(row["size"])
    if not fpath or not os.path.isfile(fpath):
        return None
    if offset < 0 or offset >= size:
        return b""
    to_read = min(length, size - offset)
    try:
        with open(fpath, "rb") as f:
            f.seek(offset)
            return f.read(to_read)
    except OSError:
        return None


def album_ack(album_id: int, device_id: str) -> bool:
    device_id = (device_id or DEFAULT_DEVICE_ID).strip().upper()
    with _lock:
        c = _connect()
        row = c.execute(
            "SELECT file_path FROM album_inbox WHERE id=? AND device_id=? AND acked_at IS NULL",
            (album_id, device_id),
        ).fetchone()
        if row is None:
            return False
        try:
            os.remove(row["file_path"])
        except OSError:
            pass
        c.execute(
            "UPDATE album_inbox SET acked_at=? WHERE id=?",
            (time.time(), album_id),
        )
        c.commit()
    return True
