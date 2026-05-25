"""将豆包返回的 Markdown/LaTeX 清洗为 ESP32 屏显纯文本（与固件 soti_answer_format 规则对齐）。"""
from __future__ import annotations

import re
import unicodedata

# 设备子集字库难以覆盖的符号 → 常用替代
_CHAR_MAP = {
    "\u201c": '"',
    "\u201d": '"',
    "\u2018": "'",
    "\u2019": "'",
    "\u2014": "——",
    "\u2026": "…",
    "\u00d7": "*",
    "\u00f7": "/",
    "\u2212": "-",
    "\u00b7": "*",
}

_LATEX_CMD_REPLACEMENTS = {
    "times": "*",
    "cdot": "*",
    "div": "/",
    "pm": "+",
    "mp": "-",
}

# \boldsymbol{67} / \mathbf / \mathrm / \text / \textbf / \textit → 内容
_STYLE_CMDS = frozenset(
    {"boldsymbol", "mathbf", "mathrm", "text", "textbf", "textit", "operatorname"}
)


def _normalize_quotes(s: str) -> str:
    for k, v in _CHAR_MAP.items():
        s = s.replace(k, v)
    return s


def _collapse_digit_spaces(s: str) -> str:
    """6 8 → 68（仅 ASCII 数字链之间的空格）。"""
    out: list[str] = []
    i = 0
    n = len(s)
    while i < n:
        if s[i].isdigit():
            while i < n and s[i].isdigit():
                out.append(s[i])
                i += 1
            while i < n and s[i] == " " and i + 1 < n and s[i + 1].isdigit():
                i += 1
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def _read_brace_content(s: str, i: int) -> tuple[str, int]:
    if i >= len(s) or s[i] != "{":
        return "", i
    i += 1
    depth = 1
    start = i
    while i < len(s) and depth > 0:
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
            if depth == 0:
                return s[start:i], i + 1
        i += 1
    return s[start:i], i


def _read_cmd_payload(s: str, i: int) -> tuple[str, int]:
    while i < len(s) and s[i] in " \t":
        i += 1
    if i < len(s) and s[i] == "{":
        inner, i = _read_brace_content(s, i)
        return inner, i
    start = i
    while i < len(s) and (s[i].isdigit() or s[i] in ".+- "):
        i += 1
    return s[start:i].replace(" ", ""), i


def _handle_latex_command(s: str, i: int) -> tuple[str, int]:
    """s[i] == '\\'。"""
    if i >= len(s) or s[i] != "\\":
        return "\\", i + 1
    i += 1
    while i < len(s) and s[i] in " \t":
        i += 1
    start = i
    while i < len(s) and (s[i].isalpha() or s[i] in "@*"):
        i += 1
    cmd = s[start:i].lower()
    while i < len(s) and s[i] in " \t":
        i += 1

    if cmd in _STYLE_CMDS:
        payload, i = _read_cmd_payload(s, i)
        return payload, i
    if cmd == "frac":
        a, i = _read_cmd_payload(s, i)
        while i < len(s) and s[i] in " \t":
            i += 1
        b, i = _read_cmd_payload(s, i)
        return f"{a}/{b}", i
    if cmd in _LATEX_CMD_REPLACEMENTS:
        return _LATEX_CMD_REPLACEMENTS[cmd], i
    if cmd in ("left", "right"):
        if i < len(s) and s[i] in "()[]":
            return "", i + 1
        return "", i
    if not cmd:
        return "", i
    if i < len(s) and s[i] == "{":
        inner, i = _read_brace_content(s, i)
        return inner, i
    return "", i


def _strip_latex_delimited(s: str, i: int, open_two: str, close_two: str) -> tuple[str, int]:
    """处理 \\( ... \\) 与 \\[ ... \\]。"""
    if not s.startswith(open_two, i):
        return "", i
    i += len(open_two)
    out: list[str] = []
    while i < len(s):
        if s.startswith(close_two, i):
            return "".join(out), i + len(close_two)
        if s[i] == "\\":
            chunk, i = _handle_latex_command(s, i)
            out.append(chunk)
            continue
        if s[i] in "{}":
            i += 1
            continue
        if s[i] == "*":
            out.append("*")
            i += 1
            continue
        out.append(s[i])
        i += 1
    return "".join(out), i


def _strip_dollar_math(s: str) -> str:
    """$...$ 与 $$...$$。"""
    def repl_block(m: re.Match[str]) -> str:
        return _sanitize_plain(m.group(1))

    s = re.sub(r"\$\$([^$]+)\$\$", repl_block, s, flags=re.DOTALL)
    s = re.sub(r"\$([^$\n]+)\$", repl_block, s)
    return s


def _sanitize_plain(s: str) -> str:
    out: list[str] = []
    i = 0
    n = len(s)
    while i < n:
        if s.startswith("\\(", i) or s.startswith("\\[", i):
            close = "\\)" if s[i + 1] == "(" else "\\]"
            chunk, i = _strip_latex_delimited(s, i, s[i : i + 2], close)
            out.append(chunk)
            continue
        if s[i] == "\\":
            chunk, i = _handle_latex_command(s, i)
            out.append(chunk)
            continue
        if s[i] == "{" or s[i] == "}":
            i += 1
            continue
        out.append(s[i])
        i += 1
    return "".join(out)


def _strip_markdown(s: str) -> str:
    lines = s.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out_lines: list[str] = []
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith("#"):
            p = stripped
            while p.startswith("#"):
                p = p[1:]
            p = p.lstrip()
            p = re.sub(r"\*\*([^*]+)\*\*", r"\1", p)
            p = p.replace("**", "").replace("*", "")
            if p:
                out_lines.append(f"【{p}】")
            continue
        line = re.sub(r"\*\*([^*]+)\*\*", r"\1", line)
        line = line.replace("**", "")
        if line.strip().startswith("* ") or line.strip().startswith("- "):
            line = "· " + line.strip()[2:]
        out_lines.append(line)
    return "\n".join(out_lines)


def _squeeze_blank_lines(s: str, max_run: int = 2) -> str:
    lines = s.split("\n")
    out: list[str] = []
    run = 0
    for line in lines:
        if not line.strip():
            run += 1
            if run <= max_run:
                out.append("")
        else:
            run = 0
            out.append(line.rstrip())
    text = "\n".join(out).strip()
    return text


def _remove_stray_backslashes(s: str) -> str:
    """去掉未形成命令的孤立反斜杠。"""
    s = re.sub(r"\\(?![a-zA-Z(@*])", "", s)
    return s


def _strip_wrapping_quotes(s: str) -> str:
    s = s.strip()
    pairs = ('"', '"'), ("'", "'"), ("「", "」"), ("『", "』")
    for a, b in pairs:
        if s.startswith(a) and s.endswith(b) and len(s) > 2:
            s = s[1:-1].strip()
    return s


def _ensure_section(s: str, title: str, body_if_missing: str = "") -> str:
    """若缺少【title】则在文首补上。"""
    mark = f"【{title}】"
    if mark in s:
        return s
    if body_if_missing:
        return f"{mark}\n{body_if_missing}\n\n{s}".strip()
    return f"{mark}\n{s}".strip()


def _format_daily(s: str) -> str:
    s = _strip_wrapping_quotes(s)
    s = re.sub(r"^\d+[\.\)、]\s*", "", s)
    if len(s) > 120:
        s = s[:120].rstrip() + "…"
    if "【每日一句】" not in s:
        s = _ensure_section(s, "每日一句")
    return _squeeze_blank_lines(s, max_run=1)


def _format_translate(s: str) -> str:
    if "【原文】" not in s and "【译文】" not in s:
        parts = s.split("\n\n", 1)
        if len(parts) == 2:
            s = f"【原文】\n{parts[0].strip()}\n\n【译文】\n{parts[1].strip()}"
        else:
            s = _ensure_section(s, "译文")
    return _squeeze_blank_lines(s)


def _format_grade(s: str) -> str:
    if "【批改结果】" not in s and "【说明】" not in s:
        s = _ensure_section(s, "批改结果")
    return _squeeze_blank_lines(s)


def _format_summary(s: str) -> str:
    if "【标题】" not in s and "【要点】" not in s:
        lines = [ln.strip() for ln in s.split("\n") if ln.strip()]
        if lines:
            s = f"【标题】\n{lines[0]}\n\n【要点】\n" + "\n".join(
                f"· {ln.lstrip('·').strip()}" if not ln.startswith("·") else ln for ln in lines[1:]
            )
    return _squeeze_blank_lines(s)


def _format_by_mode(s: str, mode: str) -> str:
    m = (mode or "solve").strip().lower()
    if m == "daily":
        return _format_daily(s)
    if m == "translate":
        return _format_translate(s)
    if m == "grade":
        return _format_grade(s)
    if m == "summary":
        return _format_summary(s)
    return _squeeze_blank_lines(s)


def format_answer_for_device(raw: str, mode: str = "solve") -> str:
    """主入口：供 soti_upload_server 在返回 JSON 前调用。"""
    if not raw or not isinstance(raw, str):
        return raw or ""
    s = raw.strip()
    if s.startswith("（演示）") or s.startswith("搜题失败"):
        return s

    s = unicodedata.normalize("NFKC", s)
    s = _normalize_quotes(s)
    s = _strip_dollar_math(s)
    s = _sanitize_plain(s)
    s = _strip_markdown(s)
    s = _remove_stray_backslashes(s)
    s = re.sub(r"[ \t]+", " ", s)
    s = re.sub(r"\s*\*\s*", "*", s)
    s = re.sub(r"\s*/\s*", "/", s)
    s = _collapse_digit_spaces(s)

    # 常见 OCR/模型笔误
    s = s.replace("\\*", "*")
    s = s.replace("×", "*").replace("÷", "/")
    return _format_by_mode(s, mode)


_SECTION_HEADER_RE = re.compile(r"【([^】]+)】")


def _parse_section_map(text: str) -> dict[str, str]:
    """按【小节名】切分；值为该节正文（不含标题行）。"""
    matches = list(_SECTION_HEADER_RE.finditer(text))
    out: dict[str, str] = {}
    for i, m in enumerate(matches):
        name = m.group(1).strip()
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end].strip()
        if name:
            out[name] = body
    return out


def _join_section_bodies(section_map: dict[str, str], names: list[str]) -> str:
    parts: list[str] = []
    for name in names:
        body = section_map.get(name, "").strip()
        if not body:
            continue
        parts.append(f"【{name}】\n{body}")
    return "\n\n".join(parts).strip()


def _fallback_split(text: str) -> tuple[str, str]:
    parts = text.split("\n\n", 1)
    q = parts[0].strip()
    wa = parts[1].strip() if len(parts) > 1 else ""
    return q, wa


def split_print_sections(text: str, mode: str = "solve") -> dict[str, str]:
    """
    从屏显 answer 文本切出热敏打印用字段（同一次识题结果，非二次 OCR）。
    返回 {"question": "...", "with_answer": "..."}。
    """
    if not text or not isinstance(text, str):
        return {"question": "", "with_answer": ""}
    s = text.strip()
    if s.startswith("（演示）") or s.startswith("搜题失败"):
        return {"question": "", "with_answer": ""}

    m = (mode or "solve").strip().lower()
    sm = _parse_section_map(s)

    if m == "daily":
        block = _join_section_bodies(sm, ["每日一句"]) or s
        return {"question": block, "with_answer": block}

    if m == "translate":
        q = _join_section_bodies(sm, ["原文"])
        wa = _join_section_bodies(sm, ["译文", "说明"])
        if not q and not wa and not sm:
            q, wa = _fallback_split(s)
        return {"question": q, "with_answer": wa}

    if m == "tutor":
        q = _join_section_bodies(sm, ["题目"])
        wa = _join_section_bodies(sm, ["考点", "分步讲解", "答案", "易错提醒"])
        if not q and not wa and not sm:
            q, wa = _fallback_split(s)
        return {"question": q, "with_answer": wa}

    if m == "grade":
        q = _join_section_bodies(sm, ["题目"])
        wa = _join_section_bodies(sm, ["批改结果", "说明"])
        if not wa and not sm:
            _, wa = _fallback_split(s)
        if not wa:
            wa = s
        return {"question": q, "with_answer": wa}

    if m == "summary":
        q = _join_section_bodies(sm, ["标题"])
        wa = _join_section_bodies(sm, ["要点", "关键词"])
        if not q and not wa and not sm:
            q, wa = _fallback_split(s)
        return {"question": q, "with_answer": wa}

    # solve (default)
    q = _join_section_bodies(sm, ["题目"])
    wa = _join_section_bodies(sm, ["解题步骤", "答案"])
    if not q and not wa and not sm:
        q, wa = _fallback_split(s)
    return {"question": q, "with_answer": wa}


def looks_like_raw_latex(s: str) -> bool:
    """若仍含明显 LaTeX 痕迹，可打日志便于调 prompt。"""
    if not s:
        return False
    patterns = (
        r"\\boldsymbol",
        r"\\mathbf",
        r"\\\(",
        r"\\\)",
        r"\\frac",
        r"\$\$?",
    )
    return any(re.search(p, s) for p in patterns)
