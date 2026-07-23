#!/usr/bin/env python3

import argparse
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import render_regression as rendering  # noqa: E402


def patterned_image(width: int = 80, height: int = 60) -> rendering.Image:
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels.extend(((x * 3) % 256, (y * 5) % 256, (x + y * 2) % 256, 255))
    return rendering.Image(width, height, bytes(pixels))


def compare(reference: rendering.Image, actual: rendering.Image, rectangles=None):
    return rendering.compare_images(
        reference,
        actual,
        pixel_threshold=16,
        max_changed_ratio=0.02,
        max_mae=3.0,
        max_coarse_mae=4.0,
        min_luma_stddev=1.0,
        rectangles=rectangles or [],
    )


class RenderRegressionTest(unittest.TestCase):
    def test_png_round_trip_and_identical_comparison(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "frame.png"
            expected = patterned_image()
            rendering.write_png(path, expected)
            actual = rendering.read_png(path)
            self.assertEqual(actual, expected)
            report, _ = compare(expected, actual)
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["metrics"]["changed_pixels"], 0)

    def test_large_visual_change_fails_and_writes_diff(self):
        reference = patterned_image()
        changed = bytearray(reference.rgba)
        for y in range(10, 30):
            for x in range(15, 45):
                offset = (y * reference.width + x) * 4
                changed[offset : offset + 4] = b"\xff\x00\xff\xff"
        report, difference = compare(
            reference, rendering.Image(reference.width, reference.height, bytes(changed))
        )
        self.assertEqual(report["status"], "fail")
        self.assertGreater(report["metrics"]["changed_pixel_ratio"], 0.02)
        self.assertEqual((difference.width, difference.height), (80, 60))

    def test_dynamic_region_can_be_explicitly_ignored(self):
        reference = patterned_image()
        changed = bytearray(reference.rgba)
        for y in range(10, 30):
            for x in range(15, 45):
                offset = (y * reference.width + x) * 4
                changed[offset : offset + 4] = b"\xff\x00\xff\xff"
        report, _ = compare(
            reference,
            rendering.Image(reference.width, reference.height, bytes(changed)),
            rectangles=[(15, 10, 30, 20)],
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["ignored_pixels"], 600)

    def test_title_bar_can_be_removed_from_window_capture(self):
        content = patterned_image(80, 45)
        framed_pixels = bytearray(80 * 65 * 4)
        framed_pixels[: 80 * 20 * 4] = b"\x20\x20\x20\xff" * (80 * 20)
        framed_pixels[80 * 20 * 4 :] = content.rgba
        framed = rendering.Image(80, 65, bytes(framed_pixels))
        self.assertEqual(rendering.crop_game_window_content(framed), content)

    def test_non_finite_threshold_is_rejected(self):
        with self.assertRaises(argparse.ArgumentTypeError):
            rendering.finite_float("nan")
        with self.assertRaises(argparse.ArgumentTypeError):
            rendering.finite_float("inf")

    def test_blank_capture_quality_is_rejected_without_a_reference(self):
        blank = rendering.Image(80, 60, b"\x00\x00\x00\xff" * (80 * 60))
        blank_report = rendering.image_quality_report(blank)
        self.assertEqual(blank_report["status"], "fail")
        self.assertTrue(any("blank" in failure for failure in blank_report["failures"]))

        patterned_report = rendering.image_quality_report(patterned_image())
        self.assertEqual(patterned_report["status"], "pass")

    def test_capture_cannot_reuse_an_existing_destination(self):
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary) / "capture.png"
            rendering.write_png(destination, patterned_image())
            completed = subprocess.CompletedProcess([], 0, "", "")
            with mock.patch.object(rendering.sys, "platform", "darwin"), mock.patch.object(
                rendering, "macos_app_control", return_value="123"
            ), mock.patch.object(rendering.subprocess, "run", return_value=completed):
                with self.assertRaises(rendering.ImageError):
                    rendering.capture_window(ROOT, 100, destination, 0.0, 1)
            self.assertFalse(destination.exists())


if __name__ == "__main__":
    unittest.main()
