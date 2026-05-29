"""Normalize parent print uploads: thermal width, optional contrast enhance, JPEG out."""
from __future__ import annotations

import os
from io import BytesIO
from typing import Any

MAX_PRINT_WIDTH = int(os.environ.get("PARENT_PRINT_MAX_WIDTH", "384"))
MAX_PRINT_HEIGHT = int(os.environ.get("PARENT_PRINT_MAX_HEIGHT", "900"))
PRINT_JPEG_QUALITY = int(os.environ.get("PARENT_PRINT_JPEG_QUALITY", "85"))


def _thermal_enhance_rgb(im) -> Any:
    """
    High-contrast grayscale for thermal, but lift blue strokes on dark backgrounds
    (avoid turning logo blue into solid black blobs).
    """
    from PIL import Image, ImageOps

    im = im.convert("RGB")
    gray = ImageOps.autocontrast(im.convert("L"), cutoff=1)
    src = im.load()
    out = gray.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            pr, pg, pb = src[x, y]
            lum = out[x, y]
            if pb > pr + 15 and pb > pg + 8 and lum < 160:
                lum = min(255, lum + 110)
            elif lum < 35:
                lum = 0
            elif lum > 210:
                lum = 255
            out[x, y] = lum
    return gray.convert("RGB")


def print_normalize_image(raw: bytes, binarize: bool = True) -> dict[str, Any]:
    """
    Accept JPEG/PNG/WebP. Returns JPEG bytes sized for thermal raster (width <= 384).
    binarize=True uses thermal enhance (not harsh 1-bit); False uses mild autocontrast only.
    """
    if not raw or len(raw) < 32:
        raise ValueError("file too small")
    try:
        from PIL import Image, ImageOps
    except ImportError as e:
        raise ValueError("server missing Pillow; contact admin") from e

    original_size = len(raw)
    try:
        im = Image.open(BytesIO(raw))
        im.load()
    except Exception as e:
        raise ValueError("cannot open image (use JPG/PNG/WebP)") from e

    src_w, src_h = im.size
    im = ImageOps.exif_transpose(im)
    im = im.convert("RGB")
    im.thumbnail((MAX_PRINT_WIDTH, MAX_PRINT_HEIGHT), Image.Resampling.LANCZOS)
    if binarize:
        im = _thermal_enhance_rgb(im)
    else:
        im = ImageOps.autocontrast(im.convert("L"), cutoff=1).convert("RGB")

    out_w, out_h = im.size

    buf = BytesIO()
    im.save(
        buf,
        format="JPEG",
        quality=PRINT_JPEG_QUALITY,
        optimize=True,
        progressive=False,
    )
    jpeg = buf.getvalue()
    if len(jpeg) < 256:
        raise ValueError("processed image too small")
    if jpeg[0] != 0xFF or jpeg[1] != 0xD8:
        raise ValueError("internal: not jpeg after encode")

    return {
        "jpeg": jpeg,
        "width": out_w,
        "height": out_h,
        "original_size": original_size,
        "processed_size": len(jpeg),
        "source_width": src_w,
        "source_height": src_h,
        "binarize": bool(binarize),
    }
