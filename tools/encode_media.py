#!/usr/bin/env python3
"""Encode local WebM files into JCOS's small streaming media format."""

import argparse
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

WIDTH = 192
HEIGHT = 108
FPS = 12
SAMPLE_RATE = 48000
MAGIC = 0x31414944454D434A
HEADER = struct.Struct("<Q10IQQ")
FRAME_HEADER = struct.Struct("<II")


def run_ffmpeg(ffmpeg: str, source: Path, video: Path, audio: Path) -> None:
    video_filter = (
        f"fps={FPS},scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,"
        f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2:black"
    )
    subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-i", str(source), "-an",
         "-vf", video_filter, "-pix_fmt", "rgb8", "-f", "rawvideo", str(video)],
        check=True,
    )
    subprocess.run(
        [ffmpeg, "-y", "-loglevel", "error", "-i", str(source), "-vn",
         "-ac", "1", "-ar", str(SAMPLE_RATE), "-f", "u8", str(audio)],
        check=True,
    )


def delta_rle(frame: bytes, previous: bytes) -> bytes:
    encoded = bytearray()
    position = 0
    size = len(frame)
    while position < size:
        unchanged = 0
        while unchanged < 64 and position + unchanged < size and (
            frame[position + unchanged] == previous[position + unchanged]
        ):
            unchanged += 1
        if unchanged >= 3:
            encoded.append(0x80 | (unchanged - 1))
            position += unchanged
            continue

        repeated = 1
        while repeated < 64 and position + repeated < size and (
            frame[position + repeated] == frame[position]
        ):
            repeated += 1
        if repeated >= 3:
            encoded.extend((0xC0 | (repeated - 1), frame[position]))
            position += repeated
            continue

        literal_start = position
        position += 1
        while position - literal_start < 128 and position < size:
            unchanged = 0
            while unchanged < 3 and position + unchanged < size and (
                frame[position + unchanged] == previous[position + unchanged]
            ):
                unchanged += 1
            repeated = 1
            while repeated < 3 and position + repeated < size and (
                frame[position + repeated] == frame[position]
            ):
                repeated += 1
            if unchanged >= 3 or repeated >= 3:
                break
            position += 1
        literal = frame[literal_start:position]
        encoded.append(len(literal) - 1)
        encoded.extend(literal)
    return bytes(encoded)


def encode(ffmpeg: str, source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="jcos-media-") as temporary:
        video_path = Path(temporary) / "video.rgb8"
        audio_path = Path(temporary) / "audio.u8"
        run_ffmpeg(ffmpeg, source, video_path, audio_path)
        video = video_path.read_bytes()
        audio = audio_path.read_bytes()

    frame_bytes = WIDTH * HEIGHT
    frame_count = len(video) // frame_bytes
    video = video[:frame_count * frame_bytes]
    audio_limit = (frame_count * SAMPLE_RATE) // FPS
    audio = audio[:audio_limit]
    payload = bytearray()
    previous = bytes(frame_bytes)
    for index in range(frame_count):
        frame = video[index * frame_bytes:(index + 1) * frame_bytes]
        compressed = delta_rle(frame, previous)
        sample_start = (index * SAMPLE_RATE) // FPS
        sample_end = ((index + 1) * SAMPLE_RATE) // FPS
        samples = audio[sample_start:min(sample_end, len(audio))]
        payload.extend(FRAME_HEADER.pack(len(compressed), len(samples)))
        payload.extend(compressed)
        payload.extend(samples)
        previous = frame

    duration_us = (frame_count * 1_000_000) // FPS
    header = HEADER.pack(
        MAGIC, 1, HEADER.size, WIDTH, HEIGHT, FPS, frame_count, SAMPLE_RATE,
        1, 1, 0, duration_us, len(payload)
    )
    temporary_output = destination.with_suffix(destination.suffix + ".tmp")
    temporary_output.write_bytes(header + payload)
    temporary_output.replace(destination)
    print(f"encoded {destination.name}: {frame_count} frames, {destination.stat().st_size} bytes")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    args = parser.parse_args()

    sources = list(args.input_dir.glob("*.webm"))
    jobs = {}
    for source in sources:
        lowered = source.name.lower()
        if "bad apple" in lowered:
            jobs[source] = args.output_dir / "badapple.jmv"
        elif "caramelldansen" in lowered:
            jobs[source] = args.output_dir / "caramel.jmv"
    if len(jobs) != 2:
        raise SystemExit("expected Bad Apple and Caramelldansen WebM files in media/")
    for source, destination in jobs.items():
        encode(args.ffmpeg, source, destination)


if __name__ == "__main__":
    main()
