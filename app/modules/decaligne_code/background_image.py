"""Background decoding, normalization, layout and palette extraction (server only)."""
import base64
import hashlib
from io import BytesIO
import json
import warnings

from fastapi import APIRouter, HTTPException, Request
from starlette.concurrency import run_in_threadpool

router = APIRouter()
MAX_IMAGE_UPLOAD = 10 * 1024 * 1024
MAX_PIXELS = 24_000_000
MAX_EMBEDDED_BYTES = 900 * 1024


def prepare_background(data):
    # Local import keeps other module endpoints available if deployment omitted Pillow.
    from PIL import Image, ImageOps, UnidentifiedImageError

    if not data or len(data) > MAX_IMAGE_UPLOAD:
        raise ValueError("Choose an image up to 10 MB")
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('error', Image.DecompressionBombWarning)
            with Image.open(BytesIO(data)) as source:
                if source.format not in {'PNG', 'JPEG', 'WEBP'}:
                    raise ValueError('Supported background formats: PNG, JPEG and WebP')
                if source.width * source.height > MAX_PIXELS:
                    raise ValueError('Background image must contain at most 24 million pixels')
                source.seek(0)
                image = ImageOps.exif_transpose(source).convert('RGBA')
    except (UnidentifiedImageError, OSError, Image.DecompressionBombError,
            Image.DecompressionBombWarning) as exc:
        raise ValueError('Invalid or oversized background image') from exc

    # Fit within the page, preserving aspect ratio; all layout defaults are server-owned.
    scale = min(1200 / image.width, 800 / image.height)
    width, height = image.width * scale, image.height * scale
    sample = image.copy()
    sample.thumbnail((160, 160), Image.Resampling.LANCZOS)
    visible = [(r, g, b) for r, g, b, a in sample.getdata() if a >= 128]
    if visible:
        strip = Image.new('RGB', (len(visible), 1))
        strip.putdata(visible)
        quantized = strip.quantize(colors=8, method=Image.Quantize.MEDIANCUT)
        palette = quantized.getpalette()
        colors = ['#%02x%02x%02x' % tuple(palette[i*3:i*3+3])
                  for count, i in sorted(quantized.getcolors(), reverse=True)]
        colors = list(dict.fromkeys(colors))
    else:
        colors = ['#ffffff', '#000000']

    image.thumbnail((1600, 1600), Image.Resampling.LANCZOS)
    while True:
        output = BytesIO()
        image.save(output, format='PNG', optimize=True)
        png = output.getvalue()
        if len(png) <= MAX_EMBEDDED_BYTES:
            break
        image = image.resize((max(1, int(image.width * .8)), max(1, int(image.height * .8))), Image.Resampling.LANCZOS)
    return {'values': {
        'background-image-base64': base64.b64encode(png).decode('ascii'),
        'background-image-id': hashlib.sha256(png).hexdigest(),
        'background-image-width': str(width), 'background-image-height': str(height),
        'background-image-x': str((1200-width)/2), 'background-image-y': str((800-height)/2),
        'background-image-scale': '100',
        'background-offset-x': '0', 'background-offset-y': '0',
        'image-color-group': json.dumps(colors),
    }}


@router.post('/background/image')
async def background_image(request: Request):
    data = bytearray()
    async for chunk in request.stream():
        data.extend(chunk)
        if len(data) > MAX_IMAGE_UPLOAD:
            raise HTTPException(413, 'Background image must be at most 10 MB')
    try:
        return await run_in_threadpool(prepare_background, bytes(data))
    except ImportError as exc:
        raise HTTPException(503, 'Install decaligne_code/requirements-background.txt and restart VizAPI') from exc
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
