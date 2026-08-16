#!/usr/bin/env python3
"""Resize still images and animated GIFs to 64x64 pixels."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageSequence


SIZE = (64, 64)
MAX_GIF_FRAMES = 48


def default_output_path(source: Path, animated: bool) -> Path:
    suffix = ".gif" if animated else ".png"
    return source.with_name(f"{source.stem}_64x64{suffix}")


def resize_still(image: Image.Image, output: Path) -> None:
    resized = image.convert("RGBA").resize(SIZE, Image.Resampling.LANCZOS)
    output.parent.mkdir(parents=True, exist_ok=True)
    resized.save(output)


def sample_ranges(frame_count: int, wanted: int) -> list[tuple[int, int]]:
    """Split every source frame into evenly distributed, non-empty ranges."""
    return [
        (index * frame_count // wanted, (index + 1) * frame_count // wanted)
        for index in range(wanted)
    ]


def resize_gif(image: Image.Image, output: Path) -> None:
    frames = [
        frame.convert("RGBA").resize(SIZE, Image.Resampling.LANCZOS)
        for frame in ImageSequence.Iterator(image)
    ]
    durations = [
        int(frame.info.get("duration", image.info.get("duration", 100)))
        for frame in ImageSequence.Iterator(image)
    ]

    wanted = min(len(frames), MAX_GIF_FRAMES)
    ranges = sample_ranges(len(frames), wanted)

    # Pick a representative frame from each part of the animation. Summing the
    # durations of that part preserves the GIF's original total playback time.
    sampled_frames = [frames[(start + end - 1) // 2] for start, end in ranges]
    sampled_durations = [sum(durations[start:end]) for start, end in ranges]

    output.parent.mkdir(parents=True, exist_ok=True)
    sampled_frames[0].save(
        output,
        save_all=True,
        append_images=sampled_frames[1:],
        duration=sampled_durations,
        loop=image.info.get("loop", 0),
        disposal=2,
        optimize=False,
    )


def convert(source: Path, output: Path | None = None) -> Path:
    if not source.is_file():
        raise FileNotFoundError(f"Input image does not exist: {source}")

    with Image.open(source) as image:
        animated = bool(getattr(image, "is_animated", False))
        destination = output or default_output_path(source, animated)

        if animated:
            if destination.suffix.lower() != ".gif":
                raise ValueError("The output path for an animated image must end in .gif")
            resize_gif(image, destination)
        else:
            resize_still(image, destination)

    return destination


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Resize an image to 64x64; animated GIFs are limited to 48 frames."
    )
    parser.add_argument("input", type=Path, help="input image or GIF")
    parser.add_argument(
        "output",
        type=Path,
        nargs="?",
        help="output path (defaults to <name>_64x64.png or .gif)",
    )
    args = parser.parse_args()

    try:
        destination = convert(args.input, args.output)
    except (FileNotFoundError, OSError, ValueError) as error:
        parser.error(str(error))

    print(f"Saved {destination}")


if __name__ == "__main__":
    main()
