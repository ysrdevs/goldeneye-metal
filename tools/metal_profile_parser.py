#!/usr/bin/env python3
"""Parse GoldenEye's complete ``[metal-profile]`` windows without dependencies.

The renderer writes one command window followed by its command/wait ledgers.  This
tool deliberately keys every ledger on its inclusive swap range, rather than on
line order, so interleaved presenter output cannot corrupt a benchmark result.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


PROFILE = "[metal-profile]"
KV = re.compile(r"([A-Za-z_][A-Za-z0-9_-]*)=([^\s]+)")
RANGE = re.compile(r"(?:swaps|attempts)=(\d+)-(\d+)")
STAGES = ("draw", "copy", "swap", "wait_reg_mem")


def number(value: str) -> int | float | str:
    try:
        return float(value) if any(c in value.lower() for c in ".e") else int(value, 0)
    except ValueError:
        return value


def fields(line: str) -> dict[str, Any]:
    return {key: number(value) for key, value in KV.findall(line)}


def swap_range(line: str) -> tuple[int, int] | None:
    match = RANGE.search(line)
    return (int(match.group(1)), int(match.group(2))) if match else None


def empty_window(start: int, end: int, data: dict[str, Any]) -> dict[str, Any]:
    return {
        "swap_start": start,
        "swap_end": end,
        "swap_count": end - start + 1,
        "elapsed_ns": data.get("elapsed_ns"),
        "avg_frame_ns": data.get("avg_frame_ns"),
        "fps": data.get("fps"),
        "stages": {},
        "wait_reasons": {},
        "wait_reg_mem": [],
        "presenter": [],
        "dam_candidate": False,
        "selected": False,
    }


def parse_log(path: Path) -> tuple[list[dict[str, Any]], list[str], dict[str, int]]:
    windows: dict[tuple[int, int], dict[str, Any]] = {}
    ordered: list[tuple[int, int]] = []
    violations: list[str] = []
    counts: dict[str, int] = defaultdict(int)

    with path.open("r", encoding="utf-8", errors="replace") as log:
        for line_number, line in enumerate(log, 1):
            # Fallback counters are emitted by the resolver outside the profile ledger.
            values = fields(line)
            fallback = values.get("fallbacks")
            if isinstance(fallback, (int, float)) and fallback > 0:
                violations.append(f"line {line_number}: fallbacks={fallback}")
                counts["fallbacks"] += int(fallback)

            if PROFILE not in line:
                if re.search(r"drawable[^\n]*(?:nil|fail(?:ed|ure)?)", line, re.I):
                    violations.append(f"line {line_number}: drawable failure: {line.strip()}")
                    counts["drawable_failures"] += 1
                if re.search(
                    r"(?:direct guest output copy did not complete|"
                    r"GPU tiled resolve failed|shared-memory upload failed)",
                    line,
                    re.I,
                ):
                    violations.append(f"line {line_number}: Metal execution failure: {line.strip()}")
                    counts["metal_execution_failures"] += 1
                continue

            key = swap_range(line)
            if " window " in f" {line} " and key:
                if key not in windows:
                    windows[key] = empty_window(*key, values)
                    ordered.append(key)
                continue

            if " command " in f" {line} " and key:
                window = windows.get(key)
                if window:
                    event = str(values.get("event", "unknown"))
                    window["stages"][event] = values
                continue

            if " wait swaps=" in line and key:
                window = windows.get(key)
                if window:
                    window["wait_reasons"][str(values.get("reason", "unknown"))] = values
                continue

            if " wait-reg-mem " in f" {line} " and key:
                window = windows.get(key)
                if window:
                    window["wait_reg_mem"].append(values)
                continue

            if " presenter " in f" {line} " and key:
                # Presenter attempts are independent of swaps. Keep them for evidence,
                # and apply their failure counters globally below.
                drawable_nil = values.get("drawable_nil", 0)
                if isinstance(drawable_nil, (int, float)) and drawable_nil > 0:
                    violations.append(f"line {line_number}: drawable_nil={drawable_nil}")
                    counts["drawable_nil"] += int(drawable_nil)
                uploads, upload_bytes = values.get("uploads", 0), values.get("upload_bytes", 0)
                if isinstance(uploads, (int, float)) and uploads > 0:
                    violations.append(f"line {line_number}: presenter full-frame uploads={uploads}")
                    counts["presenter_uploads"] += int(uploads)
                if isinstance(upload_bytes, (int, float)) and upload_bytes > 0:
                    violations.append(
                        f"line {line_number}: presenter full-frame upload_bytes={upload_bytes}"
                    )
                    counts["presenter_upload_bytes"] += int(upload_bytes)
                continue

    result = [windows[key] for key in ordered]
    for window in result:
        copy = window["stages"].get("copy", {})
        average = copy.get("avg_calls_per_swap", 0)
        window["dam_candidate"] = isinstance(average, (int, float)) and average >= 10
    return result, violations, dict(counts)


def stage_value(window: dict[str, Any], stage: str, name: str) -> Any:
    return window["stages"].get(stage, {}).get(name, "")


def select_windows(windows: list[dict[str, Any]], warmup: int, measure: int) -> list[dict[str, Any]]:
    dam = [window for window in windows if window["dam_candidate"]]
    selected = dam[warmup : warmup + measure]
    for window in selected:
        window["selected"] = True
    return selected


def wait_reg_mem_violations(windows: list[dict[str, Any]]) -> list[str]:
    problems: list[str] = []
    for window in windows:
        for entry in window["wait_reg_mem"]:
            unmatched, timeouts = entry.get("unmatched", 0), entry.get("timeouts", 0)
            if isinstance(unmatched, (int, float)) and unmatched > 0:
                problems.append(f"swaps {window['swap_start']}-{window['swap_end']}: WAIT_REG_MEM unmatched={unmatched}")
            if isinstance(timeouts, (int, float)) and timeouts > 0:
                problems.append(f"swaps {window['swap_start']}-{window['swap_end']}: WAIT_REG_MEM timeouts={timeouts}")
    return problems


def aggregate(selected: list[dict[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {"windows": len(selected), "stages": {}}
    fps = [w["fps"] for w in selected if isinstance(w.get("fps"), (int, float))]
    output["real_window_fps"] = {"mean": sum(fps) / len(fps), "min": min(fps), "max": max(fps)} if fps else None
    for stage in STAGES:
        metrics = [w["stages"].get(stage, {}) for w in selected]
        calls = [m.get("calls", 0) for m in metrics if isinstance(m.get("calls", 0), (int, float))]
        total_ns = [m.get("total_ns", 0) for m in metrics if isinstance(m.get("total_ns", 0), (int, float))]
        avg_per_swap = [m.get("avg_ns_per_swap", 0) for m in metrics if isinstance(m.get("avg_ns_per_swap", 0), (int, float))]
        output["stages"][stage] = {
            "total_calls": sum(calls),
            "total_ns": sum(total_ns),
            "mean_avg_ns_per_swap": sum(avg_per_swap) / len(avg_per_swap) if avg_per_swap else None,
            "max_call_ns": max((m.get("max_call_ns", 0) for m in metrics if isinstance(m.get("max_call_ns", 0), (int, float))), default=0),
        }
    return output


def write_outputs(output: Path, windows: list[dict[str, Any]], selected: list[dict[str, Any]], violations: list[str], counts: dict[str, int], warmup: int, measure: int) -> bool:
    output.mkdir(parents=True, exist_ok=True)
    enough = len(selected) == measure
    # A timeout or unmatched compare indicates an execution-path failure, even
    # if it happened while the selected Dam samples were warming up.
    issues = list(violations) + wait_reg_mem_violations(windows)
    if not enough:
        issues.append(f"insufficient Dam windows: need {warmup + measure}, found {sum(w['dam_candidate'] for w in windows)}")
    summary = {
        "status": "pass" if not issues else "fail",
        "requirements": {"dam_copy_avg_calls_per_swap_at_least": 10, "discard_dam_windows": warmup, "measure_dam_windows": measure},
        "observed": {"profile_windows": len(windows), "dam_windows": sum(w["dam_candidate"] for w in windows), "selected_windows": len(selected), "failure_counts": counts},
        "aggregate": aggregate(selected),
        "failures": issues,
    }
    with (output / "summary.json").open("w", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2, sort_keys=True)
        handle.write("\n")
    columns = ["swap_start", "swap_end", "dam_candidate", "selected", "fps", "elapsed_ns", "avg_frame_ns"]
    for stage in STAGES:
        columns += [f"{stage}_{field}" for field in ("calls", "avg_calls_per_swap", "total_ns", "avg_ns_per_swap", "max_call_ns", "max_swap_ns")]
    with (output / "windows.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns)
        writer.writeheader()
        for window in windows:
            row = {key: window.get(key, "") for key in columns}
            for stage in STAGES:
                for field in ("calls", "avg_calls_per_swap", "total_ns", "avg_ns_per_swap", "max_call_ns", "max_swap_ns"):
                    row[f"{stage}_{field}"] = stage_value(window, stage, field)
            writer.writerow(row)
    with (output / "summary.txt").open("w", encoding="utf-8") as handle:
        handle.write(f"status: {summary['status']}\nDam windows: {summary['observed']['dam_windows']} (selected {len(selected)}/{measure})\n")
        if summary["aggregate"]["real_window_fps"]:
            fps = summary["aggregate"]["real_window_fps"]
            handle.write(f"real window FPS: mean={fps['mean']:.3f} min={fps['min']:.3f} max={fps['max']:.3f}\n")
        for issue in issues:
            handle.write(f"FAIL: {issue}\n")
    return not issues


def command_text(argv: list[str]) -> str:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    return result.stdout.strip()


def sha256(path: Path) -> str | None:
    if not path.is_file():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_metadata(args: argparse.Namespace) -> None:
    executable, dylib, xex, repo = Path(args.executable), Path(args.dylib), Path(args.xex), Path(args.repo)
    sysctls = {key: command_text(["sysctl", "-n", key]) for key in ("hw.model", "machdep.cpu.brand_string", "hw.physicalcpu", "hw.logicalcpu", "hw.nperflevels", "hw.perflevel0.physicalcpu", "hw.perflevel1.physicalcpu", "hw.memsize")}
    metadata = {
        "executable": str(executable), "dylib": str(dylib), "xex": str(xex), "game_data_root": args.data_root,
        "hashes_sha256": {"executable": sha256(executable), "dylib": sha256(dylib), "xex": sha256(xex)},
        "macos": command_text(["sw_vers"]), "hardware": {"model": sysctls["hw.model"], "chip": sysctls["machdep.cpu.brand_string"], "physical_cores": sysctls["hw.physicalcpu"], "logical_cores": sysctls["hw.logicalcpu"], "performance_cores": sysctls["hw.perflevel0.physicalcpu"], "efficiency_cores": sysctls["hw.perflevel1.physicalcpu"], "memory_bytes": sysctls["hw.memsize"], "raw_sysctl": sysctls},
        "power_source": command_text(["pmset", "-g", "batt"]) or command_text(["pmset", "-g", "ups"]),
        "git": {"commit": command_text(["git", "-C", str(repo), "rev-parse", "HEAD"]), "dirty": bool(command_text(["git", "-C", str(repo), "status", "--porcelain"]))},
        "environment": {key: os.environ.get(key) for key in sorted(os.environ) if key.startswith(("REX_", "GOLDENEYE_", "DYLD_LIBRARY_PATH"))},
    }
    destination = Path(args.metadata)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", nargs="?", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--measure", type=int, default=48)
    parser.add_argument("--ready", action="store_true", help="exit success once enough clean data exists")
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--executable")
    parser.add_argument("--dylib")
    parser.add_argument("--xex")
    parser.add_argument("--repo")
    parser.add_argument("--data-root")
    args = parser.parse_args()
    if args.metadata:
        required = (args.executable, args.dylib, args.xex, args.repo, args.data_root)
        if not all(required):
            parser.error("--metadata requires --executable, --dylib, --xex, --repo, and --data-root")
        write_metadata(args)
        return 0
    if not args.log:
        parser.error("log is required unless --metadata is used")
    windows, violations, counts = parse_log(args.log)
    selected = select_windows(windows, args.warmup, args.measure)
    issues = violations + wait_reg_mem_violations(windows)
    ready = len(selected) == args.measure and not issues
    if args.output:
        success = write_outputs(args.output, windows, selected, violations, counts, args.warmup, args.measure)
        return 0 if success else 1
    if args.ready:
        return 0 if ready else 1
    print(json.dumps({"ready": ready, "profile_windows": len(windows), "dam_windows": sum(w["dam_candidate"] for w in windows), "selected_windows": len(selected), "failures": issues}, sort_keys=True))
    return 0 if ready else 1


if __name__ == "__main__":
    raise SystemExit(main())
