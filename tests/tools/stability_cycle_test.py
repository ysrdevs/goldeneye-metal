#!/usr/bin/env python3

import importlib.util
import json
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "stability_cycle", ROOT / "tools/stability-cycle.py"
)
assert SPEC and SPEC.loader
stability = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stability)


class StabilityCycleTest(unittest.TestCase):
    def test_cli_rejects_non_finite_timeout(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "tools/stability-cycle.py"), "--ready-timeout", "nan"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("finite number", result.stderr)

    def test_process_group_cleanup_detects_and_kills_surviving_child(self):
        with tempfile.TemporaryDirectory() as temporary:
            child_ready = Path(temporary) / "child-ready"
            parent_source = textwrap.dedent(
                """\
                import subprocess, sys, time
                child_code = (
                    "import pathlib,signal,sys,time; "
                    "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                    "pathlib.Path(sys.argv[1]).write_text('ready'); "
                    "time.sleep(60)"
                )
                subprocess.Popen([sys.executable, "-c", child_code, sys.argv[1]])
                while True:
                    time.sleep(0.05)
                """
            )
            process = subprocess.Popen(
                [sys.executable, "-c", parent_source, str(child_ready)],
                start_new_session=True,
            )
            try:
                deadline = time.monotonic() + 3
                while not child_ready.is_file() and time.monotonic() < deadline:
                    time.sleep(0.02)
                self.assertTrue(child_ready.is_file())
                result = stability.terminate_process(process, "signal", 0.2)
                self.assertTrue(result["descendant_cleanup_required"])
                self.assertTrue(result["descendant_force_killed"])
            finally:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=3)

    def test_cycle_uses_private_state_and_collects_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import os, signal, sys, time
                    user = os.environ["REX_USER_DATA_ROOT"]
                    os.makedirs(user, exist_ok=True)
                    open(os.path.join(user, "state-root-seen.txt"), "w").write(
                        "\\n".join((user, os.environ["REX_CACHE_PATH"], os.environ["HOME"]))
                    )
                    for index in range(2):
                        start = index * 64 + 1
                        end = start + 63
                        print(f"[metal-profile] window swaps={start}-{end} elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=copy calls=768 avg_calls_per_swap=12 total_ns=32000000 avg_ns_per_swap=500000 max_call_ns=1000000 max_swap_ns=2000000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=draw calls=640 avg_calls_per_swap=10 total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=swap calls=64 avg_calls_per_swap=1 total_ns=16000000 avg_ns_per_swap=250000 max_call_ns=500000 max_swap_ns=500000", flush=True)
                        print(f"[metal-profile] command swaps={start}-{end} event=wait_reg_mem calls=64 avg_calls_per_swap=1 total_ns=8000000 avg_ns_per_swap=125000 max_call_ns=250000 max_swap_ns=250000", flush=True)
                        print(f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory unmatched=0 timeouts=0", flush=True)
                    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            command = [
                sys.executable,
                str(ROOT / "tools/stability-cycle.py"),
                "--cycles",
                "2",
                "--mode",
                "dam",
                "--executable",
                str(executable),
                "--runtime-dir",
                str(runtime),
                "--game-data",
                str(game_data),
                "--output",
                str(suite),
                "--ready-timeout",
                "5",
                "--poll-seconds",
                "0.05",
                "--warmup-windows",
                "0",
                "--observe-windows",
                "2",
                "--shutdown-timeout",
                "2",
                "--quit-method",
                "signal",
                "--skip-metadata",
                "--allow-stale-build",
            ]
            environment = os.environ.copy()
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            result = subprocess.run(
                command, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment
            )
            detail = result.stdout + result.stderr
            if (suite / "summary.txt").is_file():
                detail += (suite / "summary.txt").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 0, detail)
            summary = json.loads((suite / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "pass")
            self.assertEqual(summary["cycles_passed"], 2)
            self.assertTrue(summary["isolated_state"])
            for number in (1, 2):
                cycle_root = suite / f"cycle-{number:03d}"
                state_root = cycle_root / "user-data"
                observed_roots = (state_root / "state-root-seen.txt").read_text(
                    encoding="utf-8"
                ).splitlines()
                self.assertEqual(observed_roots[0], str(state_root.resolve()))
                self.assertEqual(observed_roots[1], str((cycle_root / "cache").resolve()))
                self.assertEqual(observed_roots[2], str((cycle_root / "home").resolve()))
                self.assertTrue((cycle_root / "raw.log").is_file())
                self.assertTrue((cycle_root / "cycle.json").is_file())
            self.assertEqual(sorted(path.name for path in game_data.iterdir()), ["default.xex"])

    def test_failure_logged_during_shutdown_fails_final_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import signal, sys, time
                    def stop(*_):
                        print("GPU tiled resolve failed during shutdown", flush=True)
                        sys.exit(0)
                    signal.signal(signal.SIGTERM, stop)
                    for index in range(2):
                        start = index * 64 + 1
                        end = start + 63
                        print(f"[metal-profile] window swaps={start}-{end} elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                        for event, calls, average in (("draw", 640, 10), ("copy", 768, 12), ("swap", 64, 1), ("wait_reg_mem", 64, 1)):
                            print(f"[metal-profile] command swaps={start}-{end} event={event} calls={calls} avg_calls_per_swap={average} total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                        print(f"[metal-profile] wait-reg-mem swaps={start}-{end} rank=1 source=memory unmatched=0 timeouts=0", flush=True)
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/stability-cycle.py"),
                    "--cycles",
                    "1",
                    "--mode",
                    "dam",
                    "--executable",
                    str(executable),
                    "--runtime-dir",
                    str(runtime),
                    "--game-data",
                    str(game_data),
                    "--output",
                    str(suite),
                    "--ready-timeout",
                    "5",
                    "--poll-seconds",
                    "0.05",
                    "--warmup-windows",
                    "0",
                    "--observe-windows",
                    "2",
                    "--shutdown-timeout",
                    "2",
                    "--quit-method",
                    "signal",
                    "--skip-metadata",
                    "--allow-stale-build",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            cycle = json.loads((suite / "cycle-001/cycle.json").read_text(encoding="utf-8"))
            self.assertFalse(cycle["final_profile_passed"])
            self.assertTrue(
                any("GPU tiled resolve failed" in match for match in cycle["fatal_log_matches"])
            )
            self.assertTrue(
                any("final Metal profile validation failed" in failure for failure in cycle["failures"])
            )

    def test_menu_failure_logged_after_readiness_fails_final_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            executable = root / "fake-game.py"
            executable.write_text(
                textwrap.dedent(
                    """\
                    #!/usr/bin/env python3
                    import signal, sys, time
                    def stop(*_):
                        print("[metal] async probe summary fallbacks=1", flush=True)
                        sys.exit(0)
                    signal.signal(signal.SIGTERM, stop)
                    print("[ge] GOLDENEYE_AUTO_START=menu injecting Start", flush=True)
                    print("[metal-profile] window swaps=1-64 elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
                    for event, calls, average in (("draw", 640, 10), ("copy", 64, 1), ("swap", 64, 1), ("wait_reg_mem", 64, 1)):
                        print(f"[metal-profile] command swaps=1-64 event={event} calls={calls} avg_calls_per_swap={average} total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
                    while True:
                        time.sleep(0.05)
                    """
                ),
                encoding="utf-8",
            )
            executable.chmod(0o755)
            runtime = root / "runtime"
            runtime.mkdir()
            (runtime / "librexruntime.dylib").touch()
            game_data = root / "game-data"
            game_data.mkdir()
            (game_data / "default.xex").write_bytes(b"fixture")
            suite = root / "suite"
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/stability-cycle.py"),
                    "--cycles",
                    "1",
                    "--mode",
                    "menu",
                    "--executable",
                    str(executable),
                    "--runtime-dir",
                    str(runtime),
                    "--game-data",
                    str(game_data),
                    "--output",
                    str(suite),
                    "--ready-timeout",
                    "5",
                    "--menu-settle-seconds",
                    "0",
                    "--poll-seconds",
                    "0.05",
                    "--shutdown-timeout",
                    "2",
                    "--quit-method",
                    "signal",
                    "--skip-metadata",
                    "--allow-stale-build",
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=15,
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            cycle = json.loads((suite / "cycle-001/cycle.json").read_text(encoding="utf-8"))
            self.assertFalse(cycle["final_profile_passed"])
            self.assertTrue(
                any("fallbacks=1" in failure for failure in cycle["final_profile_failures"]),
                cycle["final_profile_failures"],
            )


if __name__ == "__main__":
    unittest.main()
