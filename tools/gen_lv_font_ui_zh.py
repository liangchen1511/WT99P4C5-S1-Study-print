#!/usr/bin/env python3
# Generate lv_font_ui_zh_22.c / lv_font_ui_zh_30.c via lv_font_conv (npx).
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FONT_DIR = os.path.join(ROOT, "components", "lv_font_ui_zh")
WOFF = os.path.join(FONT_DIR, "noto.woff")
SYMBOL_FILES = (
    "symbols.txt",
    "symbols_math.txt",
    "symbols_extra.txt",
    "symbols_hans_common.txt",
)


def load_symbols() -> str:
  """Merge symbol files; dedupe Han while keeping order. ASCII comes from --range 0x20-0x7F."""
  raw = []
  for name in SYMBOL_FILES:
    path = os.path.join(FONT_DIR, name)
    if not os.path.isfile(path):
      if name == "symbols.txt":
        print("Missing required:", path, file=sys.stderr)
        sys.exit(1)
      continue
    lines = []
    for line in open(path, encoding="utf-8"):
      s = line.strip()
      if not s or s.startswith("#"):
        continue
      lines.append(s)
    raw.append("".join(lines))
  text = "".join(raw)
  seen: set[str] = set()
  out: list[str] = []
  for ch in text:
    if ord(ch) < 0x80:
      continue
    if ch not in seen:
      seen.add(ch)
      out.append(ch)
  merged = "".join(out)
  print("Han glyphs in font subset:", len(out), file=sys.stderr)
  return merged


def main() -> int:
  if not os.path.isfile(WOFF):
    print("Missing font:", WOFF, file=sys.stderr)
    return 1
  symbols = load_symbols()
  os.chdir(FONT_DIR)
  common = [
    "--font",
    WOFF,
    "-r",
    "0x20-0x7F",
    "--symbols",
    symbols,
    "--bpp",
    "4",
    "--format",
    "lvgl",
    "--lv-include",
    "lvgl.h",
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
