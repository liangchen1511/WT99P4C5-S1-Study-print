#!/usr/bin/env python3
"""Generate img_app_aichat.c: 112x112 from user-cropped src, rounded corners only."""
from __future__ import annotations
import sys
from pathlib import Path
try:
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
except ImportError:
    print("Pillow required: pip install Pillow", file=sys.stderr)
    sys.exit(1)
W = H = 112
CORNER_RADIUS = 22

def rgb_to_lv8(r: int, g: int, b: int) -> int:
    return ((b >> 6) & 3) | (((g >> 5) & 7) << 2) | (((r >> 5) & 7) << 5)

def rgb_to_lv16(r: int, g: int, b: int) -> int:
    return ((b >> 3) & 0x1F) | (((g >> 2) & 0x3F) << 5) | (((r >> 3) & 0x1F) << 11)

def emit_rows(bs: list[int], first: str) -> list[str]:
    out, i, n = [], 0, len(bs)
    while i < n:
        hx = ", ".join(f"0x{b:02x}" for b in bs[i:i + 16])
        out.append(f"  /*{first}*/\n  {hx}," if i == 0 and first else f"  {hx},")
        i += 16
    return out

def resize_cover(im: Image.Image, size: int) -> Image.Image:
    im = im.convert("RGBA")
    tw, th = im.size
    sc = max(size / tw, size / th)
    nw, nh = max(1, int(round(tw * sc))), max(1, int(round(th * sc)))
    im = im.resize((nw, nh), Image.Resampling.LANCZOS)
    l, t = (nw - size) // 2, (nh - size) // 2
    return im.crop((l, t, l + size, t + size))

def rounded_alpha(im: Image.Image) -> Image.Image:
    im = im.convert("RGBA")
    w, h = im.size
    r = min(CORNER_RADIUS, w // 2, h // 2)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1), radius=r, fill=255)
    mask = mask.filter(ImageFilter.MinFilter(3))
    rc, gc, bc, ac = im.split()
    return Image.merge("RGBA", (rc, gc, bc, ImageChops.multiply(ac, mask)))

def load_src(root: Path) -> Image.Image:
    for n in ("img_app_aichat_src.png", "img_app_aichat.png"):
        p = root / n
        if p.is_file():
            return Image.open(p)
    raise FileNotFoundError(f"No source png in {root}")

def write_c(im: Image.Image, out: Path) -> None:
    d8, d16, d16s, d32 = [], [], [], []
    for r, g, b, a in im.getdata():
        d8.extend([rgb_to_lv8(r, g, b), a])
        v = rgb_to_lv16(r, g, b)
        d16.extend([v & 0xFF, (v >> 8) & 0xFF, a])
        vs = ((v & 0xFF) << 8) | (v >> 8)
        d16s.extend([vs & 0xFF, (vs >> 8) & 0xFF, a])
        d32.extend([b, g, r, a])
    p = [r"""#ifdef __has_include
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
#ifndef LV_ATTRIBUTE_IMG_IMG_APP_AICHAT
#define LV_ATTRIBUTE_IMG_IMG_APP_AICHAT
#endif
const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_IMG_APP_AICHAT uint8_t img_app_aichat_map[] = {
""", "#if LV_COLOR_DEPTH == 1 || LV_COLOR_DEPTH == 8\n  /*Pixel format: Alpha 8 bit*/\n"]
    p.extend(l + "\n" for l in emit_rows(d8, "AI chat icon"))
    p += ["#endif\n#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0\n"]
    p.extend(l + "\n" for l in emit_rows(d16, ""))
    p += ["#endif\n#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP != 0\n"]
    p.extend(l + "\n" for l in emit_rows(d16s, ""))
    p += ["#endif\n#if LV_COLOR_DEPTH == 32\n"]
    p.extend(l + "\n" for l in emit_rows(d32, ""))
    p += ["""#endif
};
const lv_img_dsc_t img_app_aichat = {
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .header.always_zero = 0,
  .header.reserved = 0,
  .header.w = 112,
  .header.h = 112,
  .data_size = 12544 * LV_IMG_PX_SIZE_ALPHA_BYTE,
  .data = img_app_aichat_map,
};
"""]
    out.write_text("".join(p), encoding="utf-8")

def main() -> None:
    root = Path(__file__).resolve().parent
    im = rounded_alpha(resize_cover(load_src(root), W))
    im.save(root / "img_app_aichat_112.png")
    write_c(im, root / "img_app_aichat.c")
    print(f"Wrote {root / 'img_app_aichat.c'}")

if __name__ == "__main__":
    main()
