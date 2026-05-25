"""Normalize parent-album uploads: decode, fix orientation, resize, emit JPEG."""
from __future__ import annotations

import os
from io import BytesIO
from typing import Any

# Match PhotoAlbum::kMaxDecodePixels (1280 * 960) on device.
MAX_ALBUM_WIDTH = int(os.environ.get("PARENT_ALBUM_MAX_WIDTH", "1280"))
MAX_ALBUM_HEIGHT = int(os.environ.get("PARENT_ALBUM_MAX_HEIGHT", "960"))
ALBUM_JPEG_QUALITY = int(os.environ.get("PARENT_ALBUM_JPEG_QUALITY", "85"))


def album_normalize_image(raw: bytes) -> dict[str, Any]:
    """
    Accept JPEG/PNG/WebP/GIF (Pillow). Returns JPEG bytes + metadata.
    HEIC is phase-2 (needs pillow-heif).
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
    im.thumbnail((MAX_ALBUM_WIDTH, MAX_ALBUM_HEIGHT), Image.Resampling.LANCZOS)
    out_w, out_h = im.size

    buf = BytesIO()
    im.save(
        buf,
        format="JPEG",
        quality=ALBUM_JPEG_QUALITY,
        optimize=True,
        progressive=False,
    )
    jpeg = buf.getvalue()
    if len(jpeg) < 512:
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
    }
