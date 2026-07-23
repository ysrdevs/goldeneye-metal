#!/usr/bin/env python3
"""Capture and compare GoldenEye window frames without third-party packages.

References and captures belong under ``out/`` because they may contain game
assets. A reference is replaced only by the explicit ``accept`` subcommand.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ImageError(RuntimeError):
    pass


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    rgba: bytes


def finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite number")
    return parsed


def _paeth(left: int, up: int, upper_left: int) -> int:
    estimate = left + up - upper_left
    left_distance = abs(estimate - left)
    up_distance = abs(estimate - up)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= up_distance and left_distance <= upper_left_distance:
        return left
    if up_distance <= upper_left_distance:
        return up
    return upper_left


def read_png(path: Path) -> Image:
    payload = path.read_bytes()
    if not payload.startswith(PNG_SIGNATURE):
        raise ImageError(f"{path} is not a PNG")

    offset = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    palette: bytes | None = None
    transparency: bytes | None = None
    saw_end = False
    while offset + 12 <= len(payload):
        length = struct.unpack_from(">I", payload, offset)[0]
        kind = payload[offset + 4 : offset + 8]
        start = offset + 8
        end = start + length
        if end + 4 > len(payload):
            raise ImageError(f"{path} has a truncated PNG chunk")
        body = payload[start:end]
        expected_crc = struct.unpack_from(">I", payload, end)[0]
        actual_crc = binascii.crc32(kind + body) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            raise ImageError(f"{path} has a corrupt {kind.decode(errors='replace')} chunk")
        offset = end + 4

        if kind == b"IHDR":
            if len(body) != 13:
                raise ImageError(f"{path} has an invalid IHDR")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if not width or not height or compression != 0 or filtering != 0:
                raise ImageError(f"{path} has unsupported PNG header values")
        elif kind == b"PLTE":
            palette = bytes(body)
        elif kind == b"tRNS":
            transparency = bytes(body)
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            saw_end = True
            break

    if not saw_end or width is None or height is None or not compressed:
        raise ImageError(f"{path} is missing required PNG chunks")
    if bit_depth != 8 or interlace != 0:
        raise ImageError(f"{path} must be a non-interlaced 8-bit PNG")
    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_by_type:
        raise ImageError(f"{path} uses unsupported PNG color type {color_type}")
    channels = channels_by_type[color_type]
    stride = width * channels
    try:
        filtered = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        raise ImageError(f"{path} has invalid compressed pixels: {error}") from error
    expected_size = height * (stride + 1)
    if len(filtered) != expected_size:
        raise ImageError(
            f"{path} decoded to {len(filtered)} bytes; expected {expected_size}"
        )

    rows: list[bytearray] = []
    source_offset = 0
    previous = bytearray(stride)
    for _ in range(height):
        filter_kind = filtered[source_offset]
        source_offset += 1
        row = bytearray(filtered[source_offset : source_offset + stride])
        source_offset += stride
        if filter_kind == 1:
            for index in range(stride):
                row[index] = (row[index] + (row[index - channels] if index >= channels else 0)) & 0xFF
        elif filter_kind == 2:
            for index in range(stride):
                row[index] = (row[index] + previous[index]) & 0xFF
        elif filter_kind == 3:
            for index in range(stride):
                left = row[index - channels] if index >= channels else 0
                row[index] = (row[index] + ((left + previous[index]) // 2)) & 0xFF
        elif filter_kind == 4:
            for index in range(stride):
                left = row[index - channels] if index >= channels else 0
                upper_left = previous[index - channels] if index >= channels else 0
                row[index] = (row[index] + _paeth(left, previous[index], upper_left)) & 0xFF
        elif filter_kind != 0:
            raise ImageError(f"{path} uses invalid PNG filter {filter_kind}")
        rows.append(row)
        previous = row

    rgba = bytearray(width * height * 4)
    destination = 0
    for row in rows:
        for x in range(width):
            source = x * channels
            if color_type == 6:
                red, green, blue, alpha = row[source : source + 4]
            elif color_type == 2:
                red, green, blue = row[source : source + 3]
                alpha = 255
            elif color_type == 0:
                red = green = blue = row[source]
                alpha = 255
            elif color_type == 4:
                red = green = blue = row[source]
                alpha = row[source + 1]
            else:
                palette_index = row[source]
                palette_offset = palette_index * 3
                if palette is None or palette_offset + 3 > len(palette):
                    raise ImageError(f"{path} has an invalid indexed-colour palette")
                red, green, blue = palette[palette_offset : palette_offset + 3]
                alpha = transparency[palette_index] if transparency and palette_index < len(transparency) else 255
            rgba[destination : destination + 4] = bytes((red, green, blue, alpha))
            destination += 4
    return Image(width, height, bytes(rgba))


def _chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", binascii.crc32(kind + body) & 0xFFFFFFFF)


def write_png(path: Path, image: Image) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    rows = bytearray()
    stride = image.width * 4
    for y in range(image.height):
        rows.append(0)
        start = y * stride
        rows.extend(image.rgba[start : start + stride])
    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)
    path.write_bytes(
        PNG_SIGNATURE
        + _chunk(b"IHDR", header)
        + _chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + _chunk(b"IEND", b"")
    )


def crop_image(image: Image, x: int, y: int, width: int, height: int) -> Image:
    if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > image.width or y + height > image.height:
        raise ImageError("crop rectangle is outside the image")
    pixels = bytearray(width * height * 4)
    destination = 0
    source_stride = image.width * 4
    copied_stride = width * 4
    for row in range(y, y + height):
        source = row * source_stride + x * 4
        pixels[destination : destination + copied_stride] = image.rgba[
            source : source + copied_stride
        ]
        destination += copied_stride
    return Image(width, height, bytes(pixels))


def crop_game_window_content(image: Image) -> Image:
    """Strip only a plausible macOS title bar from the fixed 16:9 test window."""
    expected_height = round(image.width * 9 / 16)
    title_bar_height = image.height - expected_height
    if 20 <= title_bar_height <= 80:
        return crop_image(image, 0, title_bar_height, image.width, expected_height)
    return image


def parse_rectangle(value: str) -> tuple[int, int, int, int]:
    try:
        rectangle = tuple(int(component.strip()) for component in value.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("rectangle must be x,y,width,height") from error
    if len(rectangle) != 4 or any(component < 0 for component in rectangle):
        raise argparse.ArgumentTypeError("rectangle must contain four non-negative integers")
    if rectangle[2] == 0 or rectangle[3] == 0:
        raise argparse.ArgumentTypeError("rectangle width and height must be positive")
    return rectangle  # type: ignore[return-value]


def ignored_pixels(width: int, height: int, rectangles: Iterable[tuple[int, int, int, int]]) -> bytearray:
    ignored = bytearray(width * height)
    for x, y, rectangle_width, rectangle_height in rectangles:
        x_end = min(width, x + rectangle_width)
        y_end = min(height, y + rectangle_height)
        for row in range(min(y, height), y_end):
            start = row * width + min(x, width)
            ignored[start : row * width + x_end] = b"\x01" * max(0, x_end - min(x, width))
    return ignored


def compare_images(
    reference: Image,
    actual: Image,
    *,
    pixel_threshold: int,
    max_changed_ratio: float,
    max_mae: float,
    max_coarse_mae: float,
    min_luma_stddev: float,
    rectangles: list[tuple[int, int, int, int]],
    grid_width: int = 64,
    grid_height: int = 36,
) -> tuple[dict[str, object], Image]:
    if (reference.width, reference.height) != (actual.width, actual.height):
        return (
            {
                "status": "fail",
                "failures": [
                    f"dimensions differ: reference {reference.width}x{reference.height}, "
                    f"actual {actual.width}x{actual.height}"
                ],
            },
            Image(actual.width, actual.height, bytes(actual.width * actual.height * 4)),
        )

    width, height = actual.width, actual.height
    ignored = ignored_pixels(width, height, rectangles)
    compared = changed = maximum = 0
    absolute_sum = squared_sum = 0.0
    luma_sum = luma_squared_sum = 0.0
    coarse_reference = [0.0] * (grid_width * grid_height)
    coarse_actual = [0.0] * (grid_width * grid_height)
    coarse_count = [0] * (grid_width * grid_height)
    difference = bytearray(width * height * 4)

    for pixel in range(width * height):
        byte_offset = pixel * 4
        if ignored[pixel]:
            difference[byte_offset : byte_offset + 4] = b"\x00\x00\x00\x00"
            continue
        ref_channels = reference.rgba[byte_offset : byte_offset + 3]
        actual_channels = actual.rgba[byte_offset : byte_offset + 3]
        channel_differences = [abs(int(a) - int(b)) for a, b in zip(ref_channels, actual_channels)]
        pixel_maximum = max(channel_differences)
        maximum = max(maximum, pixel_maximum)
        if pixel_maximum > pixel_threshold:
            changed += 1
        absolute_sum += sum(channel_differences)
        squared_sum += sum(value * value for value in channel_differences)
        compared += 1

        actual_luma = 0.2126 * actual_channels[0] + 0.7152 * actual_channels[1] + 0.0722 * actual_channels[2]
        reference_luma = 0.2126 * ref_channels[0] + 0.7152 * ref_channels[1] + 0.0722 * ref_channels[2]
        luma_sum += actual_luma
        luma_squared_sum += actual_luma * actual_luma
        x, y = pixel % width, pixel // width
        grid_x = min(grid_width - 1, x * grid_width // width)
        grid_y = min(grid_height - 1, y * grid_height // height)
        grid_index = grid_y * grid_width + grid_x
        coarse_reference[grid_index] += reference_luma
        coarse_actual[grid_index] += actual_luma
        coarse_count[grid_index] += 1

        heat = min(255, pixel_maximum * 4)
        context = int(actual_luma * 0.12)
        difference[byte_offset : byte_offset + 4] = bytes((max(heat, context), context, context, 255))

    if not compared:
        raise ImageError("ignore rectangles cover the entire image")
    changed_ratio = changed / compared
    mae = absolute_sum / (compared * 3)
    rmse = math.sqrt(squared_sum / (compared * 3))
    luma_mean = luma_sum / compared
    luma_variance = max(0.0, luma_squared_sum / compared - luma_mean * luma_mean)
    luma_stddev = math.sqrt(luma_variance)
    coarse_differences = [
        abs(coarse_actual[index] / count - coarse_reference[index] / count)
        for index, count in enumerate(coarse_count)
        if count
    ]
    coarse_mae = sum(coarse_differences) / len(coarse_differences)

    failures: list[str] = []
    if changed_ratio > max_changed_ratio:
        failures.append(
            f"changed-pixel ratio {changed_ratio:.6f} exceeds {max_changed_ratio:.6f}"
        )
    if mae > max_mae:
        failures.append(f"RGB mean absolute error {mae:.3f} exceeds {max_mae:.3f}")
    if coarse_mae > max_coarse_mae:
        failures.append(
            f"coarse luminance error {coarse_mae:.3f} exceeds {max_coarse_mae:.3f}"
        )
    if luma_stddev < min_luma_stddev:
        failures.append(
            f"actual-frame luminance stddev {luma_stddev:.3f} is below {min_luma_stddev:.3f}; "
            "capture may be blank"
        )

    report: dict[str, object] = {
        "status": "fail" if failures else "pass",
        "dimensions": {"width": width, "height": height},
        "compared_pixels": compared,
        "ignored_pixels": len(ignored) - compared,
        "metrics": {
            "changed_pixels": changed,
            "changed_pixel_ratio": changed_ratio,
            "rgb_mean_absolute_error": mae,
            "rgb_root_mean_square_error": rmse,
            "maximum_channel_error": maximum,
            "coarse_luminance_mean_absolute_error": coarse_mae,
            "actual_luminance_mean": luma_mean,
            "actual_luminance_stddev": luma_stddev,
        },
        "thresholds": {
            "per_channel_change": pixel_threshold,
            "max_changed_pixel_ratio": max_changed_ratio,
            "max_rgb_mean_absolute_error": max_mae,
            "max_coarse_luminance_mean_absolute_error": max_coarse_mae,
            "min_actual_luminance_stddev": min_luma_stddev,
        },
        "ignored_rectangles": [list(rectangle) for rectangle in rectangles],
        "failures": failures,
    }
    return report, Image(width, height, bytes(difference))


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def image_quality_report(image: Image, min_luma_stddev: float = 1.0) -> dict[str, object]:
    """Reject captures that decoded successfully but contain no useful image."""
    pixel_count = image.width * image.height
    if pixel_count <= 0:
        raise ImageError("capture has no pixels")
    luma_sum = 0.0
    luma_squared_sum = 0.0
    for offset in range(0, len(image.rgba), 4):
        red, green, blue = image.rgba[offset : offset + 3]
        luma = 0.2126 * red + 0.7152 * green + 0.0722 * blue
        luma_sum += luma
        luma_squared_sum += luma * luma
    luma_mean = luma_sum / pixel_count
    luma_variance = max(0.0, luma_squared_sum / pixel_count - luma_mean * luma_mean)
    luma_stddev = math.sqrt(luma_variance)
    failures: list[str] = []
    if luma_stddev < min_luma_stddev:
        failures.append(
            f"capture luminance stddev {luma_stddev:.3f} is below "
            f"{min_luma_stddev:.3f}; capture may be blank"
        )
    return {
        "status": "fail" if failures else "pass",
        "dimensions": {"width": image.width, "height": image.height},
        "actual_luminance_mean": luma_mean,
        "actual_luminance_stddev": luma_stddev,
        "min_actual_luminance_stddev": min_luma_stddev,
        "failures": failures,
    }


def compare_command(args: argparse.Namespace) -> int:
    report, difference = compare_images(
        read_png(args.reference),
        read_png(args.actual),
        pixel_threshold=args.pixel_threshold,
        max_changed_ratio=args.max_changed_ratio,
        max_mae=args.max_mae,
        max_coarse_mae=args.max_coarse_mae,
        min_luma_stddev=args.min_luma_stddev,
        rectangles=args.ignore,
    )
    report["reference"] = {"path": str(args.reference), "sha256": file_sha256(args.reference)}
    report["actual"] = {"path": str(args.actual), "sha256": file_sha256(args.actual)}
    if args.diff:
        write_png(args.diff, difference)
        report["difference_image"] = str(args.diff)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if report["status"] == "pass" else 1


def prepare_macos_app_control(root: Path, timeout: float = 30.0) -> Path:
    import fcntl

    source = root / "tools/macos_app_control.m"
    executable = root / "out/tools/macos_app_control"
    if sys.platform != "darwin":
        raise ImageError("the macOS application helper is available only on macOS")
    if not source.is_file():
        raise ImageError(f"macOS application-helper source is missing: {source}")
    executable.parent.mkdir(parents=True, exist_ok=True)
    lock_path = executable.with_suffix(".lock")
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        if not executable.is_file() or executable.stat().st_mtime_ns < source.stat().st_mtime_ns:
            temporary = executable.with_name(f"{executable.name}.tmp.{os.getpid()}")
            try:
                temporary.unlink(missing_ok=True)
                compilation_environment = os.environ.copy()
                compilation_environment["CLANG_MODULE_CACHE_PATH"] = str(
                    executable.parent / "clang-module-cache"
                )
                compilation = subprocess.run(
                    [
                        "/usr/bin/xcrun",
                        "clang",
                        "-fobjc-arc",
                        "-framework",
                        "AppKit",
                        "-framework",
                        "CoreGraphics",
                        str(source),
                        "-o",
                        str(temporary),
                    ],
                    check=False,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=timeout,
                    env=compilation_environment,
                )
                if compilation.returncode:
                    raise ImageError(
                        compilation.stderr.strip()
                        or f"macOS window-helper compilation exited {compilation.returncode}"
                    )
                os.replace(temporary, executable)
            finally:
                temporary.unlink(missing_ok=True)
    if not os.access(executable, os.X_OK):
        raise ImageError(f"macOS application helper is not executable: {executable}")
    return executable


def macos_app_control(root: Path, command: str, pid: int, timeout: float = 30.0) -> str:
    executable = prepare_macos_app_control(root, timeout)
    result = subprocess.run(
        [str(executable), command, str(pid)],
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    if result.returncode:
        raise ImageError(result.stderr.strip() or f"window helper exited {result.returncode}")
    return result.stdout.strip()


def capture_window(
    root: Path,
    pid: int,
    destination: Path,
    delay: float,
    retries: int,
    *,
    min_luma_stddev: float = 1.0,
) -> dict[str, object]:
    if sys.platform != "darwin":
        raise ImageError("window capture is available only on macOS")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if delay:
        time.sleep(delay)
    last_error = "window not found"
    for attempt in range(retries):
        try:
            # Never let a previous capture masquerade as a successful new one.
            destination.unlink(missing_ok=True)
            window_id = macos_app_control(root, "window-id", pid)
            result = subprocess.run(
                ["/usr/sbin/screencapture", "-x", "-o", "-l", window_id, str(destination)],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=30,
            )
            if result.returncode == 0 and destination.is_file():
                image = read_png(destination)
                # `screencapture -l` includes the macOS title bar.
                cropped = crop_game_window_content(image)
                if cropped is not image:
                    write_png(destination, cropped)
                validation = image_quality_report(cropped, min_luma_stddev)
                if validation["status"] != "pass":
                    destination.unlink(missing_ok=True)
                    raise ImageError("; ".join(str(item) for item in validation["failures"]))
                return validation
            last_error = result.stderr.strip() or f"screencapture exited {result.returncode}"
        except (ImageError, subprocess.TimeoutExpired) as error:
            last_error = str(error)
        if attempt + 1 < retries:
            time.sleep(1)
    raise ImageError(
        f"could not capture PID {pid}: {last_error}. Allow Screen Recording access for Terminal/Codex."
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    compare_parser = subparsers.add_parser("compare", help="compare a capture with a reference")
    compare_parser.add_argument("reference", type=Path)
    compare_parser.add_argument("actual", type=Path)
    compare_parser.add_argument("--report", type=Path, help="write the JSON result here")
    compare_parser.add_argument("--diff", type=Path, help="write a red heat-map PNG here")
    compare_parser.add_argument(
        "--pixel-threshold", type=int, default=16, help="channel delta that marks a pixel changed"
    )
    compare_parser.add_argument(
        "--max-changed-ratio",
        type=finite_float,
        default=0.02,
        help="allowed changed-pixel fraction",
    )
    compare_parser.add_argument(
        "--max-mae", type=finite_float, default=3.0, help="allowed RGB mean error"
    )
    compare_parser.add_argument(
        "--max-coarse-mae",
        type=finite_float,
        default=4.0,
        help="allowed low-frequency luminance error",
    )
    compare_parser.add_argument(
        "--min-luma-stddev",
        type=finite_float,
        default=1.0,
        help="reject likely blank captures",
    )
    compare_parser.add_argument(
        "--ignore", type=parse_rectangle, action="append", default=[], metavar="X,Y,W,H"
    )

    capture_parser = subparsers.add_parser("capture", help="capture the largest window for a PID")
    capture_parser.add_argument("--pid", type=int, required=True, help="GoldenEye process ID")
    capture_parser.add_argument("--output", type=Path, required=True)
    capture_parser.add_argument(
        "--delay", type=finite_float, default=0.0, help="seconds before capture"
    )
    capture_parser.add_argument("--retries", type=int, default=10, help="window lookup attempts")

    terminate_parser = subparsers.add_parser(
        "terminate", help="request a normal AppKit termination for a process"
    )
    terminate_parser.add_argument("--pid", type=int, required=True, help="GoldenEye process ID")

    accept_parser = subparsers.add_parser(
        "accept", help="explicitly replace a local reference with a reviewed capture"
    )
    accept_parser.add_argument("capture", type=Path)
    accept_parser.add_argument("reference", type=Path)

    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    if args.command == "compare":
        if not 0 <= args.pixel_threshold <= 255:
            parser.error("--pixel-threshold must be between 0 and 255")
        if not 0 <= args.max_changed_ratio <= 1:
            parser.error("--max-changed-ratio must be between 0 and 1")
        if min(args.max_mae, args.max_coarse_mae, args.min_luma_stddev) < 0:
            parser.error("comparison thresholds must not be negative")
    elif args.command in ("capture", "terminate"):
        if args.pid <= 0:
            parser.error("--pid must be positive")
        if args.command == "capture" and (args.delay < 0 or args.retries <= 0):
            parser.error("--delay must not be negative and --retries must be positive")
    try:
        if args.command == "compare":
            return compare_command(args)
        if args.command == "capture":
            capture_window(root, args.pid, args.output, args.delay, args.retries)
            print(args.output)
            return 0
        if args.command == "terminate":
            macos_app_control(root, "terminate", args.pid)
            print(args.pid)
            return 0
        image = read_png(args.capture)
        if image.width < 64 or image.height < 64:
            raise ImageError("refusing to accept an implausibly small capture")
        args.reference.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.capture, args.reference)
        print(args.reference)
        return 0
    except (ImageError, OSError, subprocess.TimeoutExpired) as error:
        print(f"render-regression: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
