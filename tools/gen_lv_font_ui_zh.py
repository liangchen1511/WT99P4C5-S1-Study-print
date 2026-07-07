#!/usr/bin/env python3
# Generate lv_font_ui_zh_22.c / lv_font_ui_zh_30.c via lv_font_conv (npx).
import os
import shutil
import subprocess
import sys
import urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FONT_DIR = os.path.join(ROOT, "components", "lv_font_ui_zh")
WOFF = os.path.join(FONT_DIR, "noto.woff")
MATH_TTF = os.path.join(FONT_DIR, "noto_math.ttf")
MATH_URL = "https://cdn.jsdelivr.net/gh/googlefonts/noto-fonts@main/hinted/ttf/NotoSansMath/NotoSansMath-Regular.ttf"
PUNCT_TTF = os.path.join(FONT_DIR, "noto_sans.ttf")
PUNCT_URL = "https://cdn.jsdelivr.net/gh/googlefonts/noto-fonts@main/hinted/ttf/NotoSans/NotoSans-Regular.ttf"
PUNCT_SYMBOLS = "…—·"  # CJK woff 常缺，用 Noto Sans 补
SYMBOL_FILES = (
    "symbols.txt",
    "symbols_math.txt",
    "symbols_extra.txt",
    "symbols_hans_common.txt",
)
MATH_ONLY = ("symbols_math.txt",)


def _dedupe_non_ascii(text: str) -> str:
  seen: set[str] = set()
  out: list[str] = []
  for ch in text:
    if ord(ch) < 0x80 or ch in seen:
      continue
    seen.add(ch)
    out.append(ch)
  return "".join(out)


def _read_symbol_files(names: tuple[str, ...]) -> str:
  raw = []
  for name in names:
    path = os.path.join(FONT_DIR, name)
    if not os.path.isfile(path):
      if name == "symbols.txt":
        print("Missing required:", path, file=sys.stderr)
        sys.exit(1)
      continue
    for line in open(path, encoding="utf-8"):
      s = line.strip()
      if not s or s.startswith("#"):
        continue
      raw.append(s)
  return _dedupe_non_ascii("".join(raw))


def ensure_ttf(path: str, url: str) -> None:
  if os.path.isfile(path):
    return
  print("download", url, file=sys.stderr)
  urllib.request.urlretrieve(url, path)


def ensure_math_font() -> None:
  ensure_ttf(MATH_TTF, MATH_URL)


def ensure_punct_font() -> None:
  ensure_ttf(PUNCT_TTF, PUNCT_URL)


def main() -> int:
  if not os.path.isfile(WOFF):
    print("Missing font:", WOFF, file=sys.stderr)
    return 1
  ensure_math_font()
  ensure_punct_font()
  symbols = _read_symbol_files(SYMBOL_FILES)
  math_symbols = _read_symbol_files(MATH_ONLY)
  print("Han glyphs in font subset:", len(symbols), file=sys.stderr)
  os.chdir(FONT_DIR)
  common = [
    "--font", WOFF, "-r", "0x20-0x7F", "--symbols", symbols,
    "--font", PUNCT_TTF, "--symbols", PUNCT_SYMBOLS,
    "--font", MATH_TTF, "--symbols", math_symbols,
    "--bpp", "4", "--format", "lvgl", "--lv-include", "lvgl.h",
  ]
  npx = shutil.which("npx") or shutil.which("npx.cmd")
  if not npx:
    print("npx not found in PATH", file=sys.stderr)
    return 1
  for size, name in ((30, "lv_font_ui_zh_30.c"), (22, "lv_font_ui_zh_22.c")):
    cmd = [npx, "--yes", "lv_font_conv@1.5.2", *common, "--size", str(size), "-o", name]
    print("Running:", name, file=sys.stderr)
    subprocess.check_call(cmd)
  print("OK:", os.path.join(FONT_DIR, "lv_font_ui_zh_22.c"))
  print("OK:", os.path.join(FONT_DIR, "lv_font_ui_zh_30.c"))
  return 0


if __name__ == "__main__":
  sys.exit(main())
