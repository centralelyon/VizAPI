from __future__ import annotations

import math
from io import BytesIO
from pathlib import Path

from PIL import (
    Image,
    ImageColor,
    ImageEnhance,
    ImageFilter,
    ImageOps,
    UnidentifiedImageError,
)

from app.modules.image_transform.repositories_image_transform import (
    EXTENSION_FORMATS,
    DATA_CONTENT_TYPES,
    list_images,
    list_datasets,
    resolve_request_image_path,
    resolve_request_data_path,
    data_content_type,
)


MAX_DIMENSION = 4000
MAX_PIXELS = 16_000_000

CONTENT_TYPES = {
    "JPEG": "image/jpeg",
    "PNG": "image/png",
    "WEBP": "image/webp",
}

Image.MAX_IMAGE_PIXELS = MAX_PIXELS


# ---------------------------------------------------------------------------
# Public API called by the route layer
# ---------------------------------------------------------------------------

def get_images() -> dict:
    return {"images": list_images()}


def get_data() -> dict:
    return {"datasets": list_datasets()}


def process_image(
    request_path: str,
    query: dict[str, list[str]],
) -> tuple[bytes, str] | tuple[None, None]:
    image_path = resolve_request_image_path(request_path)
    if image_path is None or not image_path.is_file():
        return None, None
    return render_local_image(image_path, query)


def get_data_file(file_path: str) -> tuple[bytes, str] | tuple[None, None]:
    data_path = resolve_request_data_path(file_path)
    if data_path is None or not data_path.is_file():
        return None, None
    return data_path.read_bytes(), data_content_type(data_path)


# ---------------------------------------------------------------------------
# Image rendering
# ---------------------------------------------------------------------------

def render_local_image(image_path: Path, query: dict[str, list[str]]) -> tuple[bytes, str]:
    with Image.open(image_path) as image:
        default_format = EXTENSION_FORMATS.get(image_path.suffix.lower()) or normalize_format(
            image.format
        )
        return render_open_image(image, query, default_format)


def render_open_image(
    image: Image.Image,
    query: dict[str, list[str]],
    default_format: str | None,
) -> tuple[bytes, str]:
    image = ImageOps.exif_transpose(image)
    image = normalize_image_mode(image)
    validate_size(image.width, image.height)

    image = crop_image(image, query)
    image = rotate_image(image, query)
    image = flip_image(image, query)
    image = resize_image(image, query)
    image = apply_color_adjustments(image, query)
    image = blur_image(image, query)
    image = pixelize_image(image, query)
    validate_size(image.width, image.height)

    output_format = parse_output_format(query, default_format)
    payload = encode_image(image, query, output_format)
    return payload, CONTENT_TYPES[output_format]


def normalize_image_mode(image: Image.Image) -> Image.Image:
    image = image.copy()
    if image.mode in ("RGB", "RGBA", "L"):
        return image
    if image.mode == "LA":
        return image.convert("RGBA")
    if image.mode == "P" and "transparency" in image.info:
        return image.convert("RGBA")
    if "A" in image.getbands():
        return image.convert("RGBA")
    return image.convert("RGB")


# ---------------------------------------------------------------------------
# Transform operations
# ---------------------------------------------------------------------------

def crop_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    bbox = parse_box(query, image.size)
    if bbox is None:
        return image
    cropped = image.crop(bbox)
    validate_size(cropped.width, cropped.height)
    return cropped


def parse_box(
    query: dict[str, list[str]],
    image_size: tuple[int, int],
) -> tuple[int, int, int, int] | None:
    value = first_query_value(query, "bbox")
    mode = (first_query_value(query, "bboxMode") or "xyxy").strip().lower()

    if value is None or value.strip() == "":
        value = first_query_value(query, "crop")
        mode = (first_query_value(query, "cropMode") or "xywh").strip().lower()

    if value is None or value.strip() == "":
        return None

    parts = [part.strip() for part in value.split(",")]
    if len(parts) != 4:
        raise ValueError("crop or bbox must contain four comma-separated numbers.")

    try:
        a, b, c, d = [float(part) for part in parts]
    except ValueError as exc:
        raise ValueError("crop or bbox values must be numbers.") from exc

    if mode in ("xyxy", "ltrb"):
        left, top, right, bottom = a, b, c, d
    elif mode in ("xywh", "rect"):
        left, top, right, bottom = a, b, a + c, b + d
    else:
        raise ValueError("bboxMode and cropMode must be xyxy or xywh.")

    left_i, top_i, right_i, bottom_i = [
        int(round(n)) for n in (left, top, right, bottom)
    ]
    image_width, image_height = image_size

    if left_i < 0 or top_i < 0 or right_i > image_width or bottom_i > image_height:
        raise ValueError(
            f"crop must be inside the image bounds 0,0,{image_width},{image_height}."
        )
    if left_i >= right_i or top_i >= bottom_i:
        raise ValueError("crop must define a non-empty area.")

    return left_i, top_i, right_i, bottom_i


def rotate_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    angle = parse_optional_float(query, "rotate", minimum=-3600, maximum=3600)
    if angle is None or angle % 360 == 0:
        return image

    expand = parse_bool_query(query, "expand", default=True)
    fill = background_color_for_image(image, query)
    rotated = image.rotate(
        -angle,
        resample=Image.Resampling.BICUBIC,
        expand=expand,
        fillcolor=fill,
    )
    validate_size(rotated.width, rotated.height)
    return rotated


def background_color_for_image(
    image: Image.Image,
    query: dict[str, list[str]],
) -> tuple[int, int, int] | tuple[int, int, int, int]:
    value = first_present_query_value(query, ("background", "bg"))
    has_alpha = image.mode in ("RGBA", "LA") or "A" in image.getbands()

    if value:
        rgba = parse_color(value)
    elif has_alpha:
        rgba = (0, 0, 0, 0)
    else:
        rgba = (255, 255, 255, 255)

    return rgba if has_alpha else rgba[:3]


def flip_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    if parse_bool_query(query, "flip", default=False):
        image = ImageOps.flip(image)
    if parse_bool_query(query, "flop", default=False) or parse_bool_query(
        query, "mirror", default=False
    ):
        image = ImageOps.mirror(image)
    return image


def resize_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    width = parse_optional_dimension(query, "width")
    height = parse_optional_dimension(query, "height")

    if width is None and height is None:
        return image

    if width is None:
        target_height = require_int(height)
        target_width = max(1, round(image.width * target_height / image.height))
        validate_size(target_width, target_height)
        return image.resize((target_width, target_height), Image.Resampling.LANCZOS)

    if height is None:
        target_width = width
        target_height = max(1, round(image.height * target_width / image.width))
        validate_size(target_width, target_height)
        return image.resize((target_width, target_height), Image.Resampling.LANCZOS)

    fit = (first_query_value(query, "fit") or "stretch").strip().lower()
    validate_size(width, height)

    if fit in ("stretch", "exact"):
        if image.size == (width, height):
            return image
        return image.resize((width, height), Image.Resampling.LANCZOS)

    if fit in ("cover", "crop"):
        return ImageOps.fit(
            image,
            (width, height),
            method=Image.Resampling.LANCZOS,
            centering=parse_gravity(query),
        )

    if fit in ("contain", "inside"):
        scale = min(width / image.width, height / image.height)
        if fit == "inside":
            scale = min(scale, 1.0)
        target_width = max(1, round(image.width * scale))
        target_height = max(1, round(image.height * scale))
        validate_size(target_width, target_height)
        if image.size == (target_width, target_height):
            return image
        return image.resize((target_width, target_height), Image.Resampling.LANCZOS)

    raise ValueError("fit must be stretch, exact, cover, crop, contain, or inside.")


def parse_gravity(query: dict[str, list[str]]) -> tuple[float, float]:
    value = (first_query_value(query, "gravity") or "center").strip().lower()
    values = {
        "center": (0.5, 0.5),
        "top": (0.5, 0.0),
        "bottom": (0.5, 1.0),
        "left": (0.0, 0.5),
        "right": (1.0, 0.5),
        "top-left": (0.0, 0.0),
        "top-right": (1.0, 0.0),
        "bottom-left": (0.0, 1.0),
        "bottom-right": (1.0, 1.0),
    }
    if value not in values:
        raise ValueError(
            "gravity must be center, top, bottom, left, right, top-left, top-right, "
            "bottom-left, or bottom-right."
        )
    return values[value]


# ---------------------------------------------------------------------------
# Color adjustments
# ---------------------------------------------------------------------------

def apply_color_adjustments(
    image: Image.Image,
    query: dict[str, list[str]],
) -> Image.Image:
    if parse_bool_query(query, "invert", default=False):
        image = invert_preserve_alpha(image)
    if parse_bool_query(query, "grayscale", default=False):
        image = grayscale_preserve_alpha(image)
    if parse_bool_query(query, "sepia", default=False):
        image = sepia_image(image)

    recolor = first_query_value(query, "recolor")
    if recolor:
        strength = parse_fraction(query, "recolorStrength", default=1.0)
        image = recolor_image(image, parse_color(recolor)[:3], strength)

    tint = first_query_value(query, "tint")
    if tint:
        strength = parse_fraction(query, "tintStrength", default=0.35)
        image = tint_image(image, parse_color(tint)[:3], strength)

    brightness = parse_optional_float(query, "brightness", minimum=0.0, maximum=10.0)
    if brightness is not None:
        image = ImageEnhance.Brightness(image).enhance(brightness)

    contrast = parse_optional_float(query, "contrast", minimum=0.0, maximum=10.0)
    if contrast is not None:
        image = ImageEnhance.Contrast(image).enhance(contrast)

    saturation = parse_optional_float(query, "saturation", minimum=0.0, maximum=10.0)
    if saturation is not None:
        image = ImageEnhance.Color(image).enhance(saturation)

    sharpness = parse_optional_float(query, "sharpness", minimum=0.0, maximum=10.0)
    if sharpness is not None:
        image = ImageEnhance.Sharpness(image).enhance(sharpness)

    return image


def grayscale_preserve_alpha(image: Image.Image) -> Image.Image:
    if "A" not in image.getbands():
        return ImageOps.grayscale(image).convert("RGB")
    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    gray = ImageOps.grayscale(rgba.convert("RGB")).convert("RGBA")
    gray.putalpha(alpha)
    return gray


def invert_preserve_alpha(image: Image.Image) -> Image.Image:
    if "A" not in image.getbands():
        return ImageOps.invert(image.convert("RGB"))
    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    inverted = ImageOps.invert(rgba.convert("RGB")).convert("RGBA")
    inverted.putalpha(alpha)
    return inverted


def sepia_image(image: Image.Image) -> Image.Image:
    return recolor_image(grayscale_preserve_alpha(image), (112, 66, 20), 0.75)


def recolor_image(
    image: Image.Image,
    color: tuple[int, int, int],
    strength: float,
) -> Image.Image:
    has_alpha = "A" in image.getbands()
    source = image.convert("RGBA") if has_alpha else image.convert("RGB")
    alpha = source.getchannel("A") if has_alpha else None
    base_rgb = source.convert("RGB")
    luma = ImageOps.grayscale(base_rgb)
    colorized = ImageOps.colorize(luma, black=(0, 0, 0), white=color)
    if strength < 1.0:
        colorized = Image.blend(base_rgb, colorized, strength)
    if alpha is not None:
        colorized = colorized.convert("RGBA")
        colorized.putalpha(alpha)
    return colorized


def tint_image(
    image: Image.Image,
    color: tuple[int, int, int],
    strength: float,
) -> Image.Image:
    has_alpha = "A" in image.getbands()
    source = image.convert("RGBA") if has_alpha else image.convert("RGB")
    alpha = source.getchannel("A") if has_alpha else None
    base_rgb = source.convert("RGB")
    overlay = Image.new("RGB", base_rgb.size, color)
    tinted = Image.blend(base_rgb, overlay, strength)
    if alpha is not None:
        tinted = tinted.convert("RGBA")
        tinted.putalpha(alpha)
    return tinted


def blur_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    radius = parse_optional_float(query, "blur", minimum=0.0, maximum=100.0)
    if radius is None or radius == 0:
        return image
    return image.filter(ImageFilter.GaussianBlur(radius=radius))


def pixelize_image(image: Image.Image, query: dict[str, list[str]]) -> Image.Image:
    value = first_present_query_value(query, ("pixelize", "pixelate"))
    if value is None or value.strip() == "":
        return image

    try:
        block_size = int(value)
    except ValueError as exc:
        raise ValueError("pixelize must be an integer.") from exc

    if block_size < 1 or block_size > 512:
        raise ValueError("pixelize must be between 1 and 512.")
    if block_size == 1:
        return image

    small_size = (
        max(1, image.width // block_size),
        max(1, image.height // block_size),
    )
    return image.resize(small_size, Image.Resampling.BILINEAR).resize(
        image.size, Image.Resampling.NEAREST
    )


# ---------------------------------------------------------------------------
# Encoding
# ---------------------------------------------------------------------------

def encode_image(
    image: Image.Image,
    query: dict[str, list[str]],
    output_format: str,
) -> bytes:
    output = BytesIO()

    if output_format == "JPEG":
        image = flatten_for_jpeg(image)
        image.save(
            output,
            output_format,
            quality=parse_quality(query),
            optimize=True,
            progressive=parse_bool_query(query, "progressive", default=False),
        )
    elif output_format == "WEBP":
        image.save(
            output,
            output_format,
            quality=parse_quality(query),
            method=6,
            lossless=parse_bool_query(query, "lossless", default=False),
        )
    else:
        image.save(
            output,
            output_format,
            optimize=True,
            compress_level=parse_png_compress_level(query),
        )

    return output.getvalue()


def flatten_for_jpeg(image: Image.Image) -> Image.Image:
    if image.mode in ("RGB", "L"):
        return image.convert("RGB")
    background = Image.new("RGB", image.size, (255, 255, 255))
    if "A" in image.getbands():
        rgba = image.convert("RGBA")
        background.paste(rgba.convert("RGB"), mask=rgba.getchannel("A"))
    else:
        background.paste(image.convert("RGB"))
    return background


# ---------------------------------------------------------------------------
# Query parameter parsers
# ---------------------------------------------------------------------------

def parse_output_format(query: dict[str, list[str]], default_format: str | None) -> str:
    requested = first_query_value(query, "format")
    if requested:
        normalized = requested.strip().lower()
        if normalized == "jpg":
            normalized = "jpeg"
        output_format = normalized.upper()
    else:
        output_format = normalize_format(default_format) or "PNG"

    if output_format not in CONTENT_TYPES:
        raise ValueError("format must be one of jpg, jpeg, png, or webp.")
    return output_format


def normalize_format(output_format: str | None) -> str | None:
    if not output_format:
        return None
    normalized = output_format.upper()
    if normalized == "JPG":
        normalized = "JPEG"
    return normalized if normalized in CONTENT_TYPES else None


def parse_optional_dimension(query: dict[str, list[str]], name: str) -> int | None:
    value = first_query_value(query, name)
    if value is None or value == "":
        return None
    try:
        parsed = int(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be an integer.") from exc
    if parsed < 1 or parsed > MAX_DIMENSION:
        raise ValueError(f"{name} must be between 1 and {MAX_DIMENSION}.")
    return parsed


def parse_optional_float(
    query: dict[str, list[str]],
    name: str,
    *,
    minimum: float,
    maximum: float,
) -> float | None:
    value = first_query_value(query, name)
    if value is None or value == "":
        return None
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be a number.") from exc
    if not math.isfinite(parsed):
        raise ValueError(f"{name} must be finite.")
    if parsed < minimum or parsed > maximum:
        raise ValueError(f"{name} must be between {minimum:g} and {maximum:g}.")
    return parsed


def parse_fraction(
    query: dict[str, list[str]],
    name: str,
    *,
    default: float,
) -> float:
    value = first_query_value(query, name)
    if value is None or value == "":
        return default
    try:
        parsed = float(value)
    except ValueError as exc:
        raise ValueError(f"{name} must be a number.") from exc
    if parsed < 0 or parsed > 1:
        raise ValueError(f"{name} must be between 0 and 1.")
    return parsed


def parse_quality(query: dict[str, list[str]]) -> int:
    value = first_query_value(query, "quality")
    if value is not None and value != "":
        try:
            quality = int(value)
        except ValueError as exc:
            raise ValueError("quality must be an integer.") from exc
        if quality < 1 or quality > 95:
            raise ValueError("quality must be between 1 and 95.")
        return quality

    compression = parse_compression(query)
    if compression is None:
        return 90
    return max(1, min(95, round(95 - (compression * 94 / 100))))


def parse_png_compress_level(query: dict[str, list[str]]) -> int:
    value = first_present_query_value(query, ("pngCompression", "pngCompress"))
    if value is not None and value != "":
        try:
            level = int(value)
        except ValueError as exc:
            raise ValueError("pngCompression must be an integer.") from exc
        if level < 0 or level > 9:
            raise ValueError("pngCompression must be between 0 and 9.")
        return level

    compression = parse_compression(query)
    if compression is None:
        return 6
    return max(0, min(9, round(compression * 9 / 100)))


def parse_compression(query: dict[str, list[str]]) -> float | None:
    value = first_query_value(query, "compression")
    if value is None or value == "":
        return None
    try:
        compression = float(value)
    except ValueError as exc:
        raise ValueError("compression must be a number.") from exc
    if compression < 0 or compression > 100:
        raise ValueError("compression must be between 0 and 100.")
    return compression


def parse_bool_query(query: dict[str, list[str]], name: str, *, default: bool) -> bool:
    value = first_query_value(query, name)
    if value is None or value == "":
        return default
    normalized = value.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"{name} must be a boolean.")


def parse_color(value: str) -> tuple[int, int, int, int]:
    normalized = value.strip()
    if normalized.lower() in {"transparent", "none"}:
        return (0, 0, 0, 0)
    try:
        return ImageColor.getcolor(normalized, "RGBA")
    except ValueError as exc:
        raise ValueError(f"invalid color: {value}.") from exc


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def first_query_value(query: dict[str, list[str]], name: str) -> str | None:
    values = query.get(name)
    return values[0] if values else None


def first_present_query_value(
    query: dict[str, list[str]],
    names: tuple[str, ...],
) -> str | None:
    for name in names:
        value = first_query_value(query, name)
        if value is not None:
            return value
    return None


def require_int(value: int | None) -> int:
    if value is None:
        raise ValueError("internal dimension value is missing.")
    return value


def validate_size(width: int, height: int) -> None:
    if width < 1 or height < 1:
        raise ValueError("image dimensions must be positive.")
    if width * height > MAX_PIXELS:
        raise ValueError(f"image area must be at most {MAX_PIXELS} pixels.")
