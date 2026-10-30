#!/usr/bin/env python3
"""Convert the StopWatch GIF sources to JPEG/MJPEG plus bounded frame indexes."""

import argparse
import hashlib
import io
import json
import re
import struct
import tempfile
from pathlib import Path

from PIL import Image, ImageChops


EMOTIONS = frozenset((
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving",
    "embarrassed", "surprised", "shocked", "thinking", "winking", "cool", "relaxed",
    "delicious", "kissy", "confident", "sleepy", "silly", "confused",
))
HEADER = struct.Struct("<4sHHHHIII")
FRAME = struct.Struct("<III")
MAX_FRAMES = 64
MAX_CLIP_BYTES = 1024 * 1024
MAX_FRAME_BYTES = 128 * 1024


def convert(source, background, output, quality=82):
    source, background, output = Path(source), Path(background), Path(output)
    if not 1 <= quality <= 95:
        raise ValueError("JPEG quality must be between 1 and 95")
    images = {}
    for path in sorted(source.glob("*.gif")):
        name = re.sub(r"^\d+_", "", path.stem)
        if name not in EMOTIONS or name in images:
            raise ValueError(f"Unknown or duplicate emotion: {path.name}")
        images[name] = path
    if images.keys() != EMOTIONS:
        raise ValueError(f"Missing emotions: {', '.join(sorted(EMOTIONS - images.keys()))}")

    with Image.open(background) as image:
        if image.size != (466, 466):
            raise ValueError("The background must be 466x466")
        backdrop = image.convert("RGBA")
    coverage = Image.new("L", backdrop.size)
    for path in images.values():
        with Image.open(path) as image:
            if image.size != backdrop.size or not 1 <= image.n_frames <= MAX_FRAMES:
                raise ValueError(f"Unsupported size or frame count: {path.name}")
            for frame in range(image.n_frames):
                image.seek(frame)
                coverage = ImageChops.lighter(coverage, image.convert("RGBA").getchannel("A"))
    bounds = coverage.getbbox()
    if bounds is None:
        raise ValueError("All input frames are transparent")
    x, y = bounds[0] // 16 * 16, bounds[1] // 16 * 16
    right = min(466, (bounds[2] + 15) // 16 * 16)
    bottom = min(466, (bounds[3] + 15) // 16 * 16)
    width, height = right - x, bottom - y
    report = {
        "version": 1, "canvas": [466, 466], "crop": [x, y, width, height],
        "quality": quality, "subsampling": "4:4:4",
        "background_sha256": hashlib.sha256(background.read_bytes()).hexdigest(),
        "clips": [],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="mjpeg-convert-", dir=output.parent) as directory:
        staging = Path(directory)
        for name, path in images.items():
            stream, table, durations = bytearray(), bytearray(), []
            with Image.open(path) as image:
                count = image.n_frames
                repeats = image.info.get("loop")
                loops = 1 if count == 1 or repeats is None else (0 if repeats == 0 else repeats + 1)
                if not 0 <= loops <= 10000:
                    raise ValueError(f"Unsupported loop count: {path.name}")
                for frame in range(count):
                    image.seek(frame)
                    duration = image.info.get("duration", 100) or 100
                    if not 10 <= duration <= 10000:
                        raise ValueError(f"Unsupported frame duration: {path.name}")
                    composite = Image.alpha_composite(backdrop, image.convert("RGBA"))
                    cropped = composite.crop((x, y, right, bottom)).convert("RGB")
                    encoded = io.BytesIO()
                    cropped.save(encoded, "JPEG", quality=quality, subsampling=0,
                                 optimize=True, progressive=False)
                    data = encoded.getvalue()
                    if len(data) > MAX_FRAME_BYTES:
                        raise ValueError(f"JPEG frame exceeds {MAX_FRAME_BYTES} bytes: {path.name}")
                    table.extend(FRAME.pack(len(stream), len(data), duration))
                    stream.extend(data)
                    durations.append(duration)
            if len(stream) > MAX_CLIP_BYTES:
                raise ValueError(f"Clip exceeds {MAX_CLIP_BYTES} bytes: {path.name}; reduce quality")
            filename = f"{name}.jpg" if count == 1 else f"{name}.mjpeg"
            (staging / filename).write_bytes(stream)
            (staging / f"{name}.idx").write_bytes(
                HEADER.pack(b"MJI1", width, height, x, y, count, len(stream), loops) + table)
            report["clips"].append({
                "name": name, "file": filename, "frames": count, "durations_ms": durations,
                "loops": loops, "bytes": len(stream),
                "source_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                "sha256": hashlib.sha256(stream).hexdigest(),
            })
        report["stream_bytes"] = sum(clip["bytes"] for clip in report["clips"])
        (staging / "conversion.json").write_text(json.dumps(report, indent=2) + "\n")
        output.mkdir(parents=True, exist_ok=True)
        for name in images:
            for suffix in (".jpg", ".mjpeg"):
                previous = output / (name + suffix)
                if previous.exists() and not (staging / previous.name).exists():
                    previous.unlink()
        for path in staging.iterdir():
            path.replace(output / path.name)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--background", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--quality", type=int, default=82)
    args = parser.parse_args()
    try:
        report = convert(args.source, args.background, args.output, args.quality)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(f"Converted {len(report['clips'])} emotions; crop {report['crop']}; "
          f"JPEG/MJPEG streams {report['stream_bytes']} bytes")


if __name__ == "__main__":
    main()
