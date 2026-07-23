#!/usr/bin/env python3

import argparse
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import metal_profile_parser as parser  # noqa: E402


def window_lines(index: int, fps: float, frame_ns: int) -> str:
    start = index * 64 + 1
    end = start + 63
    elapsed = frame_ns * 64
    return "\n".join(
        (
            f"[metal-profile] window swaps={start}-{end} elapsed_ns={elapsed} "
            f"avg_frame_ns={frame_ns} fps={fps}",
            f"[metal-profile] command swaps={start}-{end} event=draw calls=640 "
            "avg_calls_per_swap=10 total_ns=64000000 avg_ns_per_swap=1000000 "
            "max_call_ns=2000000 max_swap_ns=3000000",
            f"[metal-profile] command swaps={start}-{end} event=copy calls=768 "
            "avg_calls_per_swap=12 total_ns=32000000 avg_ns_per_swap=500000 "
            "max_call_ns=1000000 max_swap_ns=2000000",
            f"[metal-profile] command swaps={start}-{end} event=swap calls=64 "
            "avg_calls_per_swap=1 total_ns=16000000 avg_ns_per_swap=250000 "
            "max_call_ns=500000 max_swap_ns=500000",
            f"[metal-profile] command swaps={start}-{end} event=wait_reg_mem calls=128 "
            "avg_calls_per_swap=2 total_ns=8000000 avg_ns_per_swap=125000 "
            "max_call_ns=200000 max_swap_ns=400000",
            f"[metal-profile] wait swaps={start}-{end} reason=swap calls=64 "
            "waited_calls=64 waited_submissions=64 total_ns=16000000 max_call_ns=500000",
            f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory "
            "address=0x100 reference=0 mask=0xffffffff operation=3 wait=0x100 "
            "calls=64 polls=640 avg_polls_per_call=10 max_polls=12 total_ns=4000000 "
            "avg_ns_per_swap=62500 max_ns=100000 unmatched=0 timeouts=0",
        )
    )


class MetalProfileParserTest(unittest.TestCase):
    def make_log(self, root: Path) -> Path:
        log = root / "raw.log"
        samples = ((60.0, 16_666_667), (58.0, 17_241_379), (56.0, 17_857_143), (54.0, 18_518_519))
        log.write_text(
            "\n".join(window_lines(index, *sample) for index, sample in enumerate(samples))
            + "\n",
            encoding="utf-8",
        )
        return log

    def test_frame_pacing_and_wait_aggregation(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = self.make_log(Path(temporary))
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(counts, {})
            selected = parser.select_windows(windows, warmup=1, measure=3)
            aggregate = parser.aggregate(selected)
            self.assertAlmostEqual(aggregate["real_window_fps"]["mean"], 56.0)
            self.assertGreater(aggregate["real_window_fps"]["coefficient_of_variation_percent"], 2)
            self.assertEqual(aggregate["frame_pacing"]["sampled_frames"], 192)
            self.assertGreater(
                aggregate["frame_pacing"]["window_p99_frame_ms"],
                aggregate["frame_pacing"]["window_p50_frame_ms"],
            )
            self.assertEqual(aggregate["wait_reasons"]["swap"]["calls"], 192)
            self.assertGreater(aggregate["wait_reasons"]["swap"]["selected_elapsed_percent"], 1)
            waits = aggregate["wait_reg_mem"]
            self.assertEqual(waits["command_calls"], 384)
            self.assertEqual(waits["captured_ranked_calls"], 192)
            self.assertEqual(waits["unique_sites"], 1)
            self.assertEqual(waits["sites"][0]["address"], 0x100)
            self.assertAlmostEqual(waits["sites"][0]["average_polls_per_call"], 10)

    def test_optional_performance_gates_fail_summary(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            log = self.make_log(root)
            windows, violations, counts = parser.parse_log(log)
            selected = parser.select_windows(windows, warmup=1, measure=3)
            output = root / "result"
            passed = parser.write_outputs(
                output,
                windows,
                selected,
                violations,
                counts,
                warmup=1,
                measure=3,
                min_mean_fps=59.0,
                max_window_p99_ms=18.0,
                max_window_fps_cv_percent=1.0,
            )
            self.assertFalse(passed)
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "fail")
            self.assertTrue(any("mean FPS" in failure for failure in summary["failures"]))
            self.assertTrue(any("p99 frame time" in failure for failure in summary["failures"]))
            text_summary = (output / "summary.txt").read_text(encoding="utf-8")
            self.assertIn("WAIT_REG_MEM: command_calls=384", text_summary)
            self.assertIn("WAIT_REG_MEM site memory:0x00000100", text_summary)

    def test_incomplete_non_64_and_invalid_numeric_windows_are_rejected(self):
        cases = {
            "non-64": window_lines(0, 60.0, 16_666_667).replace("swaps=1-64", "swaps=1-63"),
            "missing-ledger": "\n".join(
                line
                for line in window_lines(0, 60.0, 16_666_667).splitlines()
                if "event=wait_reg_mem" not in line
            ),
            "non-finite": window_lines(0, 60.0, 16_666_667).replace("fps=60.0", "fps=nan"),
            "zero-header": window_lines(0, 60.0, 16_666_667).replace(
                "avg_frame_ns=16666667", "avg_frame_ns=0"
            ),
            "zero-elapsed": window_lines(0, 60.0, 16_666_667).replace(
                "elapsed_ns=1066666688", "elapsed_ns=0"
            ),
            "zero-fps": window_lines(0, 60.0, 16_666_667).replace("fps=60.0", "fps=0"),
            "short-swap-ledger": window_lines(0, 60.0, 16_666_667).replace(
                "event=swap calls=64", "event=swap calls=63"
            ),
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name, contents in cases.items():
                with self.subTest(name=name):
                    log = root / f"{name}.log"
                    log.write_text(contents + "\n", encoding="utf-8")
                    windows, violations, counts = parser.parse_log(log)
                    self.assertEqual(len(windows), 1)
                    self.assertFalse(windows[0]["complete"])
                    self.assertFalse(windows[0]["dam_candidate"])
                    self.assertTrue(violations)
                    self.assertEqual(counts["invalid_profile_windows"], 1)

    def test_performance_gates_never_use_a_metric_subset(self):
        with tempfile.TemporaryDirectory() as temporary:
            windows, violations, _ = parser.parse_log(self.make_log(Path(temporary)))
            self.assertEqual(violations, [])
            selected = parser.select_windows(windows, warmup=0, measure=3)
            selected[1]["fps"] = None
            issues = parser.performance_violations(
                parser.aggregate(selected),
                min_mean_fps=1.0,
                max_window_p99_ms=None,
                max_window_fps_cv_percent=100.0,
            )
            self.assertEqual(sum("incomplete samples" in issue for issue in issues), 2)

    def test_dam_selection_cannot_skip_a_complete_bad_window(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            samples = [window_lines(index, 60.0, 16_666_667) for index in range(4)]
            samples[1] = samples[1].replace(
                "event=copy calls=768 avg_calls_per_swap=12",
                "event=copy calls=0 avg_calls_per_swap=0",
            )
            log = root / "raw.log"
            log.write_text("\n".join(samples) + "\n", encoding="utf-8")
            windows, violations, counts = parser.parse_log(log)
            self.assertEqual(violations, [])
            self.assertEqual(sum(window["dam_candidate"] for window in windows), 3)
            selected = parser.select_windows(windows, warmup=0, measure=3)
            self.assertEqual(
                [(window["swap_start"], window["swap_end"]) for window in selected],
                [(1, 64)],
            )
            output = root / "result"
            self.assertFalse(
                parser.write_outputs(
                    output,
                    windows,
                    selected,
                    violations,
                    counts,
                    warmup=0,
                    measure=3,
                )
            )
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["observed"]["contiguous_dam_windows"], 1)
            self.assertTrue(
                any("insufficient contiguous Dam windows" in item for item in summary["failures"])
            )

    def test_threshold_converter_rejects_non_finite_values(self):
        for value in ("nan", "NaN", "inf", "-inf"):
            with self.subTest(value=value):
                with self.assertRaises(argparse.ArgumentTypeError):
                    parser.finite_float(value)
        self.assertEqual(parser.finite_float("0"), 0.0)
        self.assertEqual(parser.finite_float("59.5"), 59.5)

    def test_cli_rejects_non_finite_threshold(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = self.make_log(Path(temporary))
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/metal_profile_parser.py"),
                    str(log),
                    "--min-mean-fps",
                    "nan",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
                check=False,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("must be finite", result.stderr)

    def test_external_failure_is_written_to_both_summaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            windows, violations, counts = parser.parse_log(self.make_log(root))
            selected = parser.select_windows(windows, warmup=1, measure=3)
            output = root / "result"
            self.assertFalse(
                parser.write_outputs(
                    output,
                    windows,
                    selected,
                    violations,
                    counts,
                    warmup=1,
                    measure=3,
                    external_failures=["game process exited with status 9"],
                )
            )
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertIn("game process exited with status 9", summary["failures"])
            self.assertIn(
                "FAIL: game process exited with status 9",
                (output / "summary.txt").read_text(encoding="utf-8"),
            )

    def test_tracked_source_freshness_covers_code_headers_and_config_only(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            files = {
                "CMakeLists.txt": "runtime config",
                "src/runtime.cpp": "runtime code",
                "include/rex/runtime.h": "runtime header",
                "tests/runtime_test.cpp": "ignored test",
                "docs/status.md": "ignored docs",
                "thirdparty/CMakeLists.txt": "third-party config",
                "thirdparty/tiny-aes-c/aes.c": "third-party runtime code",
                "thirdparty/tiny-aes-c/aes.h": "third-party runtime header",
                "thirdparty/generated/generated.c": "ignored generated dependency",
                "thirdparty/out/output.c": "ignored output dependency",
                "thirdparty/README.md": "ignored third-party prose",
                "vendor/GoldenEye-Recomp/CMakeLists.txt": "app config",
                "vendor/GoldenEye-Recomp/ge_config.toml": "app config",
                "vendor/GoldenEye-Recomp/src/main.cpp": "app code",
                "vendor/GoldenEye-Recomp/src/ge_app.h": "app header",
                "vendor/GoldenEye-Recomp/tests/app_test.cpp": "ignored test",
            }
            for relative, contents in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
                os.utime(path, ns=(100, 100))
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            dylib, executable = root / "build/runtime.dylib", root / "build/GoldenEye"
            dylib.parent.mkdir()
            dylib.write_bytes(b"runtime")
            executable.write_bytes(b"app")
            os.utime(dylib, ns=(200, 200))
            os.utime(executable, ns=(200, 200))

            groups = parser.tracked_build_sources(root)
            runtime_relative = {str(path.relative_to(root)) for path in groups["runtime"]}
            app_relative = {str(path.relative_to(root)) for path in groups["goldeneye_app"]}
            self.assertIn("include/rex/runtime.h", runtime_relative)
            self.assertIn("thirdparty/CMakeLists.txt", runtime_relative)
            self.assertIn("thirdparty/tiny-aes-c/aes.c", runtime_relative)
            self.assertIn("thirdparty/tiny-aes-c/aes.h", runtime_relative)
            self.assertIn("vendor/GoldenEye-Recomp/src/ge_app.h", app_relative)
            self.assertIn("vendor/GoldenEye-Recomp/ge_config.toml", app_relative)
            self.assertNotIn("tests/runtime_test.cpp", runtime_relative)
            self.assertNotIn("docs/status.md", runtime_relative)
            self.assertNotIn("thirdparty/generated/generated.c", runtime_relative)
            self.assertNotIn("thirdparty/out/output.c", runtime_relative)
            self.assertNotIn("thirdparty/README.md", runtime_relative)
            self.assertNotIn("vendor/GoldenEye-Recomp/tests/app_test.cpp", app_relative)
            self.assertTrue(parser.build_freshness_report(root, dylib, executable)["fresh"])
            cli = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/metal_profile_parser.py"),
                    "--check-freshness",
                    "--repo",
                    str(root),
                    "--dylib",
                    str(dylib),
                    "--executable",
                    str(executable),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
                check=False,
            )
            self.assertEqual(cli.returncode, 0, cli.stderr)
            self.assertTrue(json.loads(cli.stdout)["fresh"])

            dependency_source = root / "thirdparty/tiny-aes-c/aes.c"
            os.utime(dependency_source, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertFalse(report["artifacts"]["runtime"]["fresh"])
            self.assertTrue(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "thirdparty/tiny-aes-c/aes.c",
                report["artifacts"]["runtime"]["stale_sources"],
            )
            os.utime(dependency_source, ns=(100, 100))

            app_header = root / "vendor/GoldenEye-Recomp/src/ge_app.h"
            os.utime(app_header, ns=(300, 300))
            report = parser.build_freshness_report(root, dylib, executable)
            self.assertTrue(report["artifacts"]["runtime"]["fresh"])
            self.assertFalse(report["artifacts"]["goldeneye_app"]["fresh"])
            self.assertIn(
                "vendor/GoldenEye-Recomp/src/ge_app.h",
                report["artifacts"]["goldeneye_app"]["stale_sources"],
            )

    def test_metadata_uses_explicit_effective_environment(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable, dylib, xex = root / "GoldenEye", root / "runtime.dylib", root / "default.xex"
            for path in (executable, dylib, xex):
                path.write_bytes(path.name.encode())
            destination = root / "metadata.json"
            args = SimpleNamespace(
                executable=str(executable),
                dylib=str(dylib),
                xex=str(xex),
                repo=str(root),
                data_root=str(root),
                metadata=str(destination),
            )
            effective = {
                "REX_GPU": "metal",
                "GOLDENEYE_BENCH_CACHE_MODE": "warm",
                "DYLD_LIBRARY_PATH": "/effective/runtime",
                "HOME": "/isolated/home",
                "TMPDIR": "/isolated/tmp",
                "IGNORED": "no",
            }
            with mock.patch.object(parser, "command_text", return_value=""):
                parser.write_metadata(args, effective_environment=effective)
            metadata = json.loads(destination.read_text(encoding="utf-8"))
            self.assertEqual(metadata["environment"]["REX_GPU"], "metal")
            self.assertEqual(metadata["environment"]["DYLD_LIBRARY_PATH"], "/effective/runtime")
            self.assertEqual(metadata["environment"]["HOME"], "/isolated/home")
            self.assertEqual(metadata["environment"]["TMPDIR"], "/isolated/tmp")
            self.assertNotIn("IGNORED", metadata["environment"])
            self.assertEqual(metadata["benchmark"]["cache_mode"], "warm")

    def test_metric_formatting_handles_zero_missing_and_non_finite(self):
        self.assertEqual(parser.format_metric(0), "0.000")
        self.assertEqual(parser.format_metric(None), "n/a")
        self.assertEqual(parser.format_metric(float("nan")), "n/a")


if __name__ == "__main__":
    unittest.main()
