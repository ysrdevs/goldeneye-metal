#!/usr/bin/env python3
"""Repeat GoldenEye boot/menu/Dam/shutdown cycles in isolated state roots."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace
from typing import Any


ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import metal_profile_parser as profile  # noqa: E402
import render_regression as rendering  # noqa: E402


FATAL_PATTERNS = (
    re.compile(r"libc\+\+abi: terminating", re.I),
    re.compile(r"segmentation fault", re.I),
    re.compile(r"EXC_BAD_ACCESS", re.I),
    re.compile(r"pure virtual method called", re.I),
    re.compile(r"uncaught exception", re.I),
    re.compile(r"fatal error", re.I),
    re.compile(r"direct guest output copy did not complete", re.I),
    re.compile(r"GPU tiled resolve failed", re.I),
    re.compile(r"shared-memory upload failed", re.I),
)
ACTIVE_PROCESS: subprocess.Popen[bytes] | None = None


def positive_integer(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return parsed


def finite_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise argparse.ArgumentTypeError("must be a finite number")
    return parsed


def timestamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def safe_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def detect_fatal_logs(cycle_root: Path) -> list[str]:
    matches: list[str] = []
    candidates = [cycle_root / "raw.log"]
    candidates.extend((cycle_root / "user-data" / "Logs").glob("*.log"))
    for candidate in candidates:
        text = safe_text(candidate)
        for pattern in FATAL_PATTERNS:
            match = pattern.search(text)
            if match:
                matches.append(f"{candidate.name}: {match.group(0)}")
    return sorted(set(matches))


def collect_crash_reports(cycle_root: Path, started_epoch: float) -> list[str]:
    source = Path.home() / "Library/Logs/DiagnosticReports"
    if not source.is_dir():
        return []
    destination = cycle_root / "crash-reports"
    copied: list[str] = []
    for candidate in source.iterdir():
        if not candidate.is_file() or candidate.suffix.lower() not in (".ips", ".crash"):
            continue
        if not re.match(r"(?:GoldenEye|ge)[-_ ]", candidate.name, re.I):
            continue
        try:
            if candidate.stat().st_mtime < started_epoch - 2:
                continue
            destination.mkdir(parents=True, exist_ok=True)
            target = destination / candidate.name
            shutil.copy2(candidate, target)
            copied.append(str(target))
        except OSError:
            continue
    return copied


def terminate_process(
    process: subprocess.Popen[bytes], method: str, timeout: float
) -> dict[str, Any]:
    try:
        process_group = os.getpgid(process.pid)
        if process_group != process.pid:
            process_group = None
    except ProcessLookupError:
        process_group = process.pid

    def group_alive() -> bool:
        if process_group is None:
            return False
        try:
            os.killpg(process_group, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True

    def send_signal_to_test(signal_number: int) -> None:
        if process_group is not None:
            try:
                os.killpg(process_group, signal_number)
                return
            except ProcessLookupError:
                return
        if process.poll() is None:
            process.send_signal(signal_number)

    result: dict[str, Any] = {
        "requested_method": method,
        "native_request_succeeded": False,
        "forced_signal": None,
        "timed_out": False,
        "process_group": process_group,
        "descendant_cleanup_required": False,
    }
    if process.poll() is not None:
        result["process_exited_before_request"] = True
        result["exit_code"] = process.returncode
    else:
        if method == "native":
            try:
                rendering.macos_app_control(ROOT, "terminate", process.pid)
                result["native_request_succeeded"] = True
            except (rendering.ImageError, OSError, subprocess.TimeoutExpired) as error:
                result["native_request_error"] = str(error)
                send_signal_to_test(signal.SIGTERM)
                result["forced_signal"] = "SIGTERM (native request fallback)"
        else:
            send_signal_to_test(signal.SIGTERM)
            result["forced_signal"] = "SIGTERM (requested harness mode)"

        deadline = time.monotonic() + timeout
        while process.poll() is None and time.monotonic() < deadline:
            time.sleep(0.1)

        if process.poll() is None:
            result["timed_out"] = True
            send_signal_to_test(signal.SIGTERM)
            result["forced_signal"] = "SIGTERM"
            fallback_deadline = time.monotonic() + min(5.0, timeout)
            while process.poll() is None and time.monotonic() < fallback_deadline:
                time.sleep(0.1)
        if process.poll() is None:
            send_signal_to_test(signal.SIGKILL)
            result["forced_signal"] = "SIGKILL"
            process.wait(timeout=5)
        else:
            process.wait()
        result["exit_code"] = process.returncode

    # start_new_session=True makes the child its process-group leader. Clean up
    # descendants even if a launcher exits before the game or a helper survives
    # the normal AppKit quit request.
    if group_alive():
        result["descendant_cleanup_required"] = True
        send_signal_to_test(signal.SIGTERM)
        descendant_deadline = time.monotonic() + min(5.0, timeout)
        while group_alive() and time.monotonic() < descendant_deadline:
            time.sleep(0.1)
        if group_alive():
            send_signal_to_test(signal.SIGKILL)
            result["descendant_force_killed"] = True
    return result


def readiness(
    mode: str,
    log_path: Path,
    elapsed: float,
    menu_settle_seconds: float,
    warmup: int,
    observe: int,
) -> tuple[bool, dict[str, Any]]:
    windows, violations, counts = profile.parse_log(log_path)
    details: dict[str, Any] = {
        "profile_windows": len(windows),
        "dam_windows": sum(window["dam_candidate"] for window in windows),
        "profile_failures": violations + profile.wait_reg_mem_violations(windows),
        "failure_counts": counts,
    }
    if details["profile_failures"]:
        return False, details
    if mode == "menu":
        injected = "GOLDENEYE_AUTO_START=menu injecting Start" in safe_text(log_path)
        details["auto_start_seen"] = injected
        return injected and elapsed >= menu_settle_seconds and bool(windows), details
    selected = profile.select_windows(windows, warmup, observe)
    details["selected_windows"] = len(selected)
    if len(selected) == observe:
        details["aggregate"] = profile.aggregate(selected)
        return True, details
    return False, details


def compare_capture(
    reference: Path, actual: Path, cycle_root: Path, args: argparse.Namespace
) -> dict[str, Any]:
    report, difference = rendering.compare_images(
        rendering.read_png(reference),
        rendering.read_png(actual),
        pixel_threshold=args.render_pixel_threshold,
        max_changed_ratio=args.render_max_changed_ratio,
        max_mae=args.render_max_mae,
        max_coarse_mae=args.render_max_coarse_mae,
        min_luma_stddev=args.render_min_luma_stddev,
        rectangles=args.render_ignore,
    )
    report["reference"] = {
        "path": str(reference),
        "sha256": rendering.file_sha256(reference),
    }
    report["actual"] = {"path": str(actual), "sha256": rendering.file_sha256(actual)}
    difference_path = cycle_root / "frame-diff.png"
    rendering.write_png(difference_path, difference)
    report["difference_image"] = str(difference_path)
    (cycle_root / "render-comparison.json").write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return report


def build_environment(
    args: argparse.Namespace, cycle_root: Path, *, create_directories: bool = True
) -> dict[str, str]:
    user_data = cycle_root / "user-data"
    cache = cycle_root / "cache"
    home = cycle_root / "home"
    temporary = cycle_root / "tmp"
    if create_directories:
        for directory in (user_data, cache, home, temporary):
            directory.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.update(
        {
            "HOME": str(home),
            "TMPDIR": str(temporary),
            "REX_GPU": "metal",
            "REX_INPUT_BACKEND": "none",
            "REX_MNK_MODE": "false",
            "REX_USER_DATA_ROOT": str(user_data),
            "REX_CACHE_PATH": str(cache),
            "REX_WINDOW_WIDTH": "1280",
            "REX_WINDOW_HEIGHT": "720",
            "REX_VIDEO_MODE_WIDTH": "1280",
            "REX_VIDEO_MODE_HEIGHT": "720",
            "REX_GPU_VSYNC": "false",
            "REX_MAX_FPS": "60",
            "REX_FULLSCREEN": "false",
            "REX_METAL_SHOW_FPS": "false",
            "GOLDENEYE_AUTO_START": "menu",
            "GOLDENEYE_METAL_PROFILE": "1",
            "GOLDENEYE_LAUNCHER_BYPASS_UI": "1",
        }
    )
    if args.mode == "dam":
        environment["GOLDENEYE_AUTO_MISSION"] = "dam"
    else:
        environment.pop("GOLDENEYE_AUTO_MISSION", None)
    previous_libraries = environment.get("DYLD_LIBRARY_PATH")
    environment["DYLD_LIBRARY_PATH"] = str(args.runtime_dir) + (
        f":{previous_libraries}" if previous_libraries else ""
    )
    return environment


def run_cycle(args: argparse.Namespace, suite_root: Path, number: int) -> dict[str, Any]:
    global ACTIVE_PROCESS
    cycle_root = suite_root / f"cycle-{number:03d}"
    cycle_root.mkdir(parents=True)
    log_path = cycle_root / "raw.log"
    environment = build_environment(args, cycle_root)
    command = [str(args.executable), "--game_data_root", str(args.game_data), "--gpu", "metal"]
    started = time.monotonic()
    started_epoch = time.time()
    ready = False
    ready_elapsed: float | None = None
    exited_before_ready: int | None = None
    readiness_details: dict[str, Any] = {}
    launch_error: str | None = None
    with log_path.open("wb") as log:
        try:
            process = subprocess.Popen(
                command,
                cwd=args.game_data,
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            ACTIVE_PROCESS = process
        except OSError as error:
            launch_error = str(error)
            process = None

        if process is not None:
            deadline = started + args.ready_timeout
            while time.monotonic() < deadline:
                log.flush()
                elapsed = time.monotonic() - started
                if process.poll() is not None:
                    exited_before_ready = process.returncode
                    break
                ready, readiness_details = readiness(
                    args.mode,
                    log_path,
                    elapsed,
                    args.menu_settle_seconds,
                    args.warmup_windows,
                    args.observe_windows,
                )
                if ready:
                    ready_elapsed = elapsed
                    break
                time.sleep(args.poll_seconds)

            capture: str | None = None
            capture_error: str | None = None
            capture_validation: dict[str, Any] | None = None
            regression: dict[str, Any] | None = None
            if ready and args.capture:
                capture_path = cycle_root / f"{args.mode}.png"
                try:
                    capture_validation = rendering.capture_window(
                        ROOT,
                        process.pid,
                        capture_path,
                        args.capture_delay,
                        args.capture_retries,
                        min_luma_stddev=args.render_min_luma_stddev,
                    )
                    capture = str(capture_path)
                    if args.reference:
                        regression = compare_capture(args.reference, capture_path, cycle_root, args)
                except (rendering.ImageError, OSError, subprocess.TimeoutExpired) as error:
                    capture_error = str(error)
            shutdown = terminate_process(process, args.quit_method, args.shutdown_timeout)
            ACTIVE_PROCESS = None
        else:
            capture = None
            capture_error = None
            capture_validation = None
            regression = None
            shutdown = {"exit_code": None}

    elapsed = time.monotonic() - started
    fatal_logs = detect_fatal_logs(cycle_root)
    crash_reports = collect_crash_reports(cycle_root, started_epoch)
    profile_artifacts: str | None = None
    final_profile_passed: bool | None = None
    final_profile_failures: list[str] = []
    final_windows, final_violations, final_counts = profile.parse_log(log_path)
    final_profile_failures = final_violations + profile.wait_reg_mem_violations(final_windows)
    if args.mode == "dam":
        final_selected = profile.select_windows(
            final_windows, args.warmup_windows, args.observe_windows
        )
        profile_root = cycle_root / "profile"
        final_profile_passed = profile.write_outputs(
            profile_root,
            final_windows,
            final_selected,
            final_violations,
            final_counts,
            args.warmup_windows,
            args.observe_windows,
        )
        profile_artifacts = str(profile_root)
    else:
        # Menu readiness was observed before capture and shutdown. Parse the
        # completed log too so a late fallback, malformed profile window, or
        # renderer failure cannot hide behind that earlier clean snapshot.
        final_profile_passed = not final_profile_failures
    failures: list[str] = []
    if launch_error:
        failures.append(f"launch failed: {launch_error}")
    if not ready:
        if exited_before_ready is not None:
            failures.append(
                f"application exited with code {exited_before_ready} before {args.mode} readiness"
            )
        else:
            failures.append(
                f"{args.mode} readiness was not reached within {args.ready_timeout:.1f} seconds"
            )
    failures.extend(readiness_details.get("profile_failures", []))
    failures.extend(fatal_logs)
    if final_profile_passed is False:
        detail = "; see profile/summary.txt" if profile_artifacts else ""
        failures.append(f"final Metal profile validation failed{detail}")
        for profile_failure in final_profile_failures:
            if profile_failure not in failures:
                failures.append(profile_failure)
    failures.extend(
        f"macOS crash report captured: {Path(report).name}" for report in crash_reports
    )
    if capture_error:
        failures.append(f"capture failed: {capture_error}")
    if regression and regression["status"] != "pass":
        failures.extend(f"render regression: {item}" for item in regression["failures"])
    if shutdown.get("timed_out"):
        failures.append("application did not exit before the shutdown deadline")
    if shutdown.get("descendant_cleanup_required"):
        failures.append("application left a child process running after shutdown")
    if shutdown.get("process_exited_before_request"):
        failures.append(
            f"application exited before the requested shutdown (code {shutdown.get('exit_code')})"
        )
    if (
        args.quit_method == "native"
        and not shutdown.get("process_exited_before_request")
        and not shutdown.get("native_request_succeeded")
    ):
        failures.append("native clean-quit request was not accepted")
    if args.quit_method == "native" and shutdown.get("exit_code") not in (None, 0):
        failures.append(f"native clean quit returned {shutdown.get('exit_code')}")

    result = {
        "cycle": number,
        "status": "fail" if failures else "pass",
        "mode": args.mode,
        "command": command,
        "pid": process.pid if process is not None else None,
        "elapsed_seconds": elapsed,
        "ready": ready,
        "ready_seconds": ready_elapsed,
        "paths": {
            "user_data": str(cycle_root / "user-data"),
            "cache": str(cycle_root / "cache"),
            "raw_log": str(log_path),
        },
        "readiness": readiness_details,
        "capture": capture,
        "capture_validation": capture_validation,
        "render_comparison": regression,
        "profile_artifacts": profile_artifacts,
        "final_profile_passed": final_profile_passed,
        "final_profile_failures": final_profile_failures,
        "shutdown": shutdown,
        "fatal_log_matches": fatal_logs,
        "crash_reports": crash_reports,
        "failures": failures,
    }
    (cycle_root / "cycle.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return result


def write_suite_summary(suite_root: Path, cycles: list[dict[str, Any]], args: argparse.Namespace) -> bool:
    passed = sum(cycle["status"] == "pass" for cycle in cycles)
    readiness_times = [
        float(cycle["ready_seconds"])
        for cycle in cycles
        if cycle.get("ready_seconds") is not None
    ]
    mean_fps = []
    for cycle in cycles:
        fps = (
            cycle.get("readiness", {})
            .get("aggregate", {})
            .get("real_window_fps")
        )
        if fps:
            mean_fps.append(float(fps["mean"]))
    summary = {
        "status": "pass" if passed == len(cycles) else "fail",
        "mode": args.mode,
        "cycles_requested": len(cycles),
        "cycles_passed": passed,
        "native_clean_shutdowns": sum(
            cycle["shutdown"].get("native_request_succeeded", False)
            and cycle["shutdown"].get("exit_code") == 0
            for cycle in cycles
        ),
        "readiness_seconds": {
            "mean": statistics.fmean(readiness_times) if readiness_times else None,
            "min": min(readiness_times) if readiness_times else None,
            "max": max(readiness_times) if readiness_times else None,
        },
        "mean_window_fps_across_cycles": statistics.fmean(mean_fps) if mean_fps else None,
        "isolated_state": True,
        "game_data": str(args.game_data),
        "reference": str(args.reference) if args.reference else None,
        "cycles": cycles,
    }
    (suite_root / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    lines = [
        f"status: {summary['status']}",
        f"mode: {args.mode}",
        f"cycles: {passed}/{len(cycles)} passed",
        f"native clean shutdowns: {summary['native_clean_shutdowns']}/{len(cycles)}",
    ]
    if mean_fps:
        lines.append(f"mean 64-frame-window FPS across cycles: {statistics.fmean(mean_fps):.3f}")
    for cycle in cycles:
        if cycle["failures"]:
            lines.append(f"cycle {cycle['cycle']}: " + "; ".join(cycle["failures"]))
    (suite_root / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return summary["status"] == "pass"


def main() -> int:
    global ACTIVE_PROCESS
    default_app_contents = (
        ROOT
        / "vendor/GoldenEye-Recomp/out/build/macos-arm64-release/dist"
        / "GoldenEye Metal.app/Contents"
    )
    default_executable = default_app_contents / "MacOS/GoldenEye"
    default_runtime = default_app_contents / "Frameworks"
    default_game_data = Path.home() / "Library/Application Support/GoldenEye Metal/Game Data"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=positive_integer, default=3)
    parser.add_argument("--mode", choices=("menu", "dam"), default="dam")
    parser.add_argument("--executable", type=Path, default=default_executable)
    parser.add_argument("--runtime-dir", type=Path, default=default_runtime)
    parser.add_argument("--game-data", type=Path, default=default_game_data)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--ready-timeout", type=finite_float, default=150.0)
    parser.add_argument("--menu-settle-seconds", type=finite_float, default=20.0)
    parser.add_argument("--poll-seconds", type=finite_float, default=1.0)
    parser.add_argument("--warmup-windows", type=int, default=1)
    parser.add_argument("--observe-windows", type=positive_integer, default=3)
    parser.add_argument("--shutdown-timeout", type=finite_float, default=20.0)
    parser.add_argument(
        "--quit-method",
        choices=("native", "signal"),
        default="native",
        help="native validates normal AppKit quit; signal is for harness self-tests",
    )
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--capture-delay", type=finite_float, default=0.0)
    parser.add_argument("--capture-retries", type=positive_integer, default=10)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--render-pixel-threshold", type=int, default=16)
    parser.add_argument("--render-max-changed-ratio", type=finite_float, default=0.02)
    parser.add_argument("--render-max-mae", type=finite_float, default=3.0)
    parser.add_argument("--render-max-coarse-mae", type=finite_float, default=4.0)
    parser.add_argument("--render-min-luma-stddev", type=finite_float, default=1.0)
    parser.add_argument(
        "--render-ignore",
        type=rendering.parse_rectangle,
        action="append",
        default=[],
        metavar="X,Y,W,H",
    )
    parser.add_argument("--skip-metadata", action="store_true")
    parser.add_argument(
        "--allow-stale-build",
        action="store_true",
        help="allow artifacts older than tracked source files (automation self-tests only)",
    )
    args = parser.parse_args()

    args.executable = args.executable.expanduser().resolve()
    args.runtime_dir = args.runtime_dir.expanduser().resolve()
    args.game_data = args.game_data.expanduser().resolve()
    requested_output = args.output.expanduser().resolve() if args.output else None
    args.reference = args.reference.expanduser().resolve() if args.reference else None
    if args.reference and not args.capture:
        parser.error("--reference requires --capture")
    if args.reference and args.mode != "menu":
        parser.error(
            "Dam gameplay is not frame-synchronized; --reference is supported only with --mode menu"
        )
    if not args.executable.is_file() or not os.access(args.executable, os.X_OK):
        parser.error(f"executable is not runnable: {args.executable}")
    dylib = args.runtime_dir / "librexruntime.dylib"
    if not dylib.is_file():
        parser.error(f"runtime library does not exist: {dylib}")
    if not (args.game_data / "default.xex").is_file():
        parser.error(f"game-data directory has no default.xex: {args.game_data}")
    if args.reference and not args.reference.is_file():
        parser.error(f"reference does not exist: {args.reference}")
    if args.warmup_windows < 0:
        parser.error("--warmup-windows must not be negative")
    if not 0 <= args.render_pixel_threshold <= 255:
        parser.error("--render-pixel-threshold must be between 0 and 255")
    if not 0 <= args.render_max_changed_ratio <= 1:
        parser.error("--render-max-changed-ratio must be between 0 and 1")
    if min(
        args.render_max_mae,
        args.render_max_coarse_mae,
        args.render_min_luma_stddev,
    ) < 0:
        parser.error("render comparison thresholds must not be negative")
    if (
        args.ready_timeout <= 0
        or args.poll_seconds <= 0
        or args.shutdown_timeout <= 0
        or args.menu_settle_seconds < 0
        or args.capture_delay < 0
    ):
        parser.error("timeouts/polling must be positive and delays must not be negative")
    if not args.allow_stale_build:
        freshness = profile.build_freshness_report(ROOT, dylib, args.executable)
        if not freshness["fresh"]:
            parser.error("; ".join(freshness["failures"]))
    if args.capture or args.quit_method == "native":
        try:
            rendering.prepare_macos_app_control(ROOT)
        except (rendering.ImageError, OSError, subprocess.TimeoutExpired) as error:
            parser.error(f"macOS application helper is unavailable: {error}")
    if requested_output:
        suite_root = requested_output
        suite_root.mkdir(parents=True, exist_ok=False)
    else:
        stability_root = ROOT / "out/stability"
        stability_root.mkdir(parents=True, exist_ok=True)
        suite_root = Path(tempfile.mkdtemp(prefix=f"{timestamp()}.", dir=stability_root))

    if not args.skip_metadata:
        profile.write_metadata(
            SimpleNamespace(
                executable=str(args.executable),
                dylib=str(dylib),
                xex=str(args.game_data / "default.xex"),
                repo=str(ROOT),
                data_root=str(args.game_data),
                metadata=str(suite_root / "metadata.json"),
                effective_environment=build_environment(
                    args, suite_root / "cycle-001", create_directories=False
                ),
            )
        )
    print(f"stability-cycle: {args.cycles} {args.mode} cycle(s) -> {suite_root}")
    cycles: list[dict[str, Any]] = []
    try:
        for number in range(1, args.cycles + 1):
            cycle = run_cycle(args, suite_root, number)
            cycles.append(cycle)
            print(f"cycle {number}/{args.cycles}: {cycle['status']}")
    except BaseException:
        if ACTIVE_PROCESS is not None and ACTIVE_PROCESS.poll() is None:
            try:
                terminate_process(ACTIVE_PROCESS, "signal", min(5.0, args.shutdown_timeout))
            except BaseException:
                ACTIVE_PROCESS.kill()
        ACTIVE_PROCESS = None
        raise
    success = write_suite_summary(suite_root, cycles, args)
    print(f"{'PASS' if success else 'FAIL'}: {suite_root / 'summary.txt'}")
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
