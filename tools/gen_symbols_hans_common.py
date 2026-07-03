#!/usr/bin/env python3
# 生成 symbols_hans_common.txt（GB2312-80 一级汉字 3755 字，按拼音首字母分组）
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FONT_DIR = os.path.join(ROOT, "components", "lv_font_ui_zh")
OUT = os.path.join(FONT_DIR, "symbols_hans_common.txt")
PINYIN = os.path.join(os.path.dirname(__file__), "pinyin_data_kMandarin_8105.txt")
_U = re.compile(r"U\+([0-9A-F]+):\s*([^#]+)")
_ACCENT = str.maketrans("āáǎàēéěèīíǐìōóǒòūúǔùǖǘǚǜü", "aaaaeeeeiiiioooouuuuvvvvv")

def gb2312_l1():
    o = []
    for b1 in range(0xB0, 0xD8):
        for b2 in range(0xA1, 0xFF):
            try: o.append(bytes([b1, b2]).decode("gb2312"))
            except UnicodeDecodeError: pass
    return o

def pinyin_table():
    t = {}
    for line in open(PINYIN, encoding="utf-8"):
        m = _U.match(line.strip())
        if not m: continue
        ch = chr(int(m.group(1), 16))
        pys = [x.strip().translate(_ACCENT).rstrip("12345") for x in m.group(2).split(",") if x.strip()]
        if pys: t[ch] = pys[0]
    return t

def main():
    chars = gb2312_l1()
    if len(chars) != 3755:
        print("warn: GB2312 L1 count", len(chars), file=sys.stderr)
    py = pinyin_table()
    groups = {}
    for ch in chars:
        p = py.get(ch, "")
        k = p[0].upper() if p and p[0].isalpha() else "#"
        groups.setdefault(k, []).append((p or "~", ch))
    lines = [
        "# GB2312-80 一级常用汉字 3755 字（按拼音首字母分组）",
        "# 生成: python tools/gen_symbols_hans_common.py  改后: python tools/gen_lv_font_ui_zh.py",
    ]
    for k in sorted(groups.keys()):
        items = sorted(groups[k], key=lambda x: (x[0], x[1]))
        lines.append(f"# {k}")
        lines.append("".join(ch for _, ch in items))
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    print(f"OK: {OUT} ({len(chars)} Han)", file=sys.stderr)

if __name__ == "__main__":
    sys.exit(main())
