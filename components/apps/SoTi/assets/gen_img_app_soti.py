#!/usr/bin/env python3
"""Generate img_app_soti.c (LVGL 8, TRUE_COLOR_ALPHA) from img_app_soti_112.png."""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    sys.exit(1)

W = H = 112


def rgb_to_lv8(r: int, g: int, b: int) -> int:
    """Match LV_COLOR_MAKE8 bitfield layout: blue (2b), green (3b), red (3b)."""
    blue2 = (b >> 6) & 0x03
    green3 = (g >> 5) & 0x07
    red3 = (r >> 5) & 0x07
    return blue2 | (green3 << 2) | (red3 << 5)


def rgb_to_lv16(r: int, g: int, b: int) -> int:
    """LVGL 16-bit: blue in bits 0-4, green 5-10, red 11-15 (LV_COLOR_16_SWAP==0)."""
    b5 = (b >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    r5 = (r >> 3) & 0x1F
    return b5 | (g6 << 5) | (r5 << 11)


def emit_rows(bytes_list: list[int], first_line_comment: str) -> list[str]:
    lines: list[str] = []
    n = len(bytes_list)
    i = 0
    while i < n:
        chunk = bytes_list[i : i + 16]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        if i == 0 and first_line_comment:
            lines.append(f"  /*{first_line_comment}*/\n  {hexes},")
        else:
            lines.append(f"  {hexes},")
        i += 16
    return lines


def main() -> None:
    root = Path(__file__).resolve().parent
    png = root / "img_app_soti_112.png"
    out = root / "img_app_soti.c"
    if not png.is_file():
        print(f"Missing {png}", file=sys.stderr)
        sys.exit(1)

    im = Image.open(png).convert("RGBA")
    if im.size != (W, H):
        im = im.resize((W, H), Image.Resampling.LANCZOS)

    px = list(im.getdata())

    d8: list[int] = []
    d16: list[int] = []
    d16s: list[int] = []
    d32: list[int] = []
    for r, g, b, a in px:
        d8.extend([rgb_to_lv8(r, g, b), a])
        v = rgb_to_lv16(r, g, b)
        lo, hi = v & 0xFF, (v >> 8) & 0xFF
        d16.extend([lo, hi, a])
        vs = ((v & 0xFF) << 8) | (v >> 8)
        lo2, hi2 = vs & 0xFF, (vs >> 8) & 0xFF
        d16s.extend([lo2, hi2, a])
        d32.extend([b & 0xFF, g & 0xFF, r & 0xFF, a & 0xFF])

    parts: list[str] = []
    parts.append(
        r"""#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif


#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_IMG_APP_SOTI
#define LV_ATTRIBUTE_IMG_IMG_APP_SOTI
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_IMG_APP_SOTI uint8_t img_app_soti_map[] = {
"""
    )

    parts.append("#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8\n")
    parts.append("  /*Pixel format: Alpha 8 bit, Red: 3 bit, Green: 3 bit, Blue: 2 bit*/\n")
    parts.extend(l + "\n" for l in emit_rows(d8, "112x112 embedded icon"))
    parts.append("#endif\n")

    parts.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0\n")
    parts.append("  /*Pixel format: Alpha 8 bit, Red: 5 bit, Green: 6 bit, Blue: 5 bit*/\n")
    parts.extend(l + "\n" for l in emit_rows(d16, "112x112 SoTi icon"))
    parts.append("#endif\n")

    parts.append("#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0\n")
    parts.append("  /*Pixel format: Alpha 8 bit, Red: 5 bit, Green: 6 bit, Blue: 5 bit  BUT the 2  color bytes are swapped*/\n")
    parts.extend(l + "\n" for l in emit_rows(d16s, ""))
    parts.append("#endif\n")

    parts.append("#if LV_COLOR_DEPTH == 32\n")
    parts.append("  /*Pixel format: Alpha 8 bit, Red: 8 bit, Green: 8 bit, Blue: 8 bit*/\n")
    parts.extend(l + "\n" for l in emit_rows(d32, ""))
    parts.append("#endif\n")

    parts.append("};\n\n")
    parts.append(
        """const lv_img_dsc_t img_app_soti = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 112,
  .header.h = 112,
  .data_size = 12544 * LV_IMG_PX_SIZE_ALPHA_BYTE,
  .data = img_app_soti_map,
};
"""
    )

    out.write_text("".join(parts), encoding="utf-8")
    print(f"Wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
