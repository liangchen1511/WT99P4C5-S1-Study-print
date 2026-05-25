#!/usr/bin/env python3
"""Generate blue WeChat-style launcher icon (112x112) -> img_app_parent_chat.c"""
from __future__ import annotations

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    sys.exit(1)

W = H = 112
BLUE = (37, 99, 235, 255)
BLUE_DARK = (29, 78, 216, 255)
BG = (232, 240, 254, 255)
WHITE = (255, 255, 255, 240)


def rgb_to_lv8(r: int, g: int, b: int) -> int:
    blue2 = (b >> 6) & 0x03
    green3 = (g >> 5) & 0x07
    red3 = (r >> 5) & 0x07
    return blue2 | (green3 << 2) | (red3 << 5)


def rgb_to_lv16(r: int, g: int, b: int) -> int:
    b5 = (b >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    r5 = (r >> 3) & 0x1F
    return b5 | (g6 << 5) | (r5 << 11)


def emit_rows(bytes_list: list[int], first: str) -> list[str]:
    lines: list[str] = []
    n = len(bytes_list)
    i = 0
    while i < n:
        chunk = bytes_list[i : i + 16]
        hexes = ", ".join(f"0x{b:02x}" for b in chunk)
        if i == 0 and first:
            lines.append(f"  /*{first}*/\n  {hexes},")
        else:
            lines.append(f"  {hexes},")
        i += 16
    return lines


def draw_icon() -> Image.Image:
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((4, 4, W - 5, H - 5), radius=22, fill=BG)
    # back bubble (WeChat-like)
    d.ellipse((18, 28, 78, 82), fill=BLUE)
    d.rounded_rectangle((22, 32, 74, 76), radius=18, fill=BLUE)
    # front bubble
    d.ellipse((48, 42, 94, 88), fill=BLUE_DARK)
    d.rounded_rectangle((52, 46, 90, 84), radius=16, fill=BLUE_DARK)
    # highlight dots
    d.ellipse((34, 48, 42, 56), fill=WHITE)
    d.ellipse((50, 48, 58, 56), fill=WHITE)
    d.ellipse((66, 58, 74, 66), fill=WHITE)
    return im


def write_c_file(im: Image.Image, out: Path) -> None:
    px = list(im.getdata())
    d8: list[int] = []
    d16: list[int] = []
    d16s: list[int] = []
    d32: list[int] = []
    for r, g, b, a in px:
        d8.extend([rgb_to_lv8(r, g, b), a])
        v = rgb_to_lv16(r, g, b)
        d16.extend([v & 0xFF, (v >> 8) & 0xFF, a])
        vs = ((v & 0xFF) << 8) | (v >> 8)
        d16s.extend([vs & 0xFF, (vs >> 8) & 0xFF, a])
        d32.extend([b & 0xFF, g & 0xFF, r & 0xFF, a & 0xFF])

    parts = [
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

#ifndef LV_ATTRIBUTE_IMG_IMG_APP_PARENT_CHAT
#define LV_ATTRIBUTE_IMG_IMG_APP_PARENT_CHAT
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_IMG_APP_PARENT_CHAT uint8_t img_app_parent_chat_map[] = {
""",
        "#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8\n",
        "  /*Pixel format: Alpha 8 bit*/\n",
    ]
    parts.extend(l + "\n" for l in emit_rows(d8, "blue WeChat-style icon"))
    parts.append("#endif\n#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0\n")
    parts.extend(l + "\n" for l in emit_rows(d16, ""))
    parts.append("#endif\n#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0\n")
    parts.extend(l + "\n" for l in emit_rows(d16s, ""))
    parts.append("#endif\n#if LV_COLOR_DEPTH == 32\n")
    parts.extend(l + "\n" for l in emit_rows(d32, ""))
    parts.append(
        """#endif
};

const lv_img_dsc_t img_app_parent_chat = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 112,
  .header.h = 112,
  .data_size = 12544 * LV_IMG_PX_SIZE_ALPHA_BYTE,
  .data = img_app_parent_chat_map,
};
"""
    )
    out.write_text("".join(parts), encoding="utf-8")


def main() -> None:
    root = Path(__file__).resolve().parent
    png = root / "img_app_parent_chat_112.png"
    out_c = root / "img_app_parent_chat.c"
    im = draw_icon()
    im.save(png)
    write_c_file(im, out_c)
    print(f"Wrote {png} and {out_c}")


if __name__ == "__main__":
    main()
