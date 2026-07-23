#!/usr/bin/env python3

import json
import os
import subprocess
import tempfile
import textwrap
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def fake_game_source(*, crash: bool, spawn_child: bool = False) -> str:
    ending = "sys.exit(7)" if crash else "while True:\n            time.sleep(0.05)"
    child_setup = ""
    if spawn_child:
        child_setup = textwrap.dedent(
            """\
            child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
            pathlib.Path(os.environ["FAKE_CHILD_PID_FILE"]).write_text(str(child.pid))
            """
        )
    return textwrap.dedent(
        f"""\
        #!/usr/bin/env python3
        import os, pathlib, signal, subprocess, sys, time
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        {textwrap.indent(child_setup, "        ").lstrip()}
        for index in range(2):
            start = index * 64 + 1
            end = start + 63
            print(f"[metal-profile] window swaps={{start}}-{{end}} elapsed_ns=1066666688 avg_frame_ns=16666667 fps=60.0", flush=True)
            for event, calls, average in (("draw", 640, 10), ("copy", 768, 12), ("swap", 64, 1), ("wait_reg_mem", 64, 1)):
                print(f"[metal-profile] command swaps={{start}}-{{end}} event={{event}} calls={{calls}} avg_calls_per_swap={{average}} total_ns=64000000 avg_ns_per_swap=1000000 max_call_ns=2000000 max_swap_ns=3000000", flush=True)
            print(f"[metal-profile] wait-reg-mem swaps={{start}}-{{end}} rank=1 source=memory calls=64 polls=64 total_ns=8000000 unmatched=0 timeouts=0", flush=True)
        {ending}
        """
    )


class BenchmarkDamTest(unittest.TestCase):
    def run_benchmark(
        self, root: Path, *, crash: bool, silent: bool = False, spawn_child: bool = False
    ) -> tuple[subprocess.CompletedProcess[str], Path]:
        executable = root / "fake-game.py"
        source = fake_game_source(crash=crash, spawn_child=spawn_child)
        if silent:
            source = textwrap.dedent(
                """\
                #!/usr/bin/env python3
                import signal, sys, time
                signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
                while True:
                    time.sleep(0.05)
                """
            )
        executable.write_text(source, encoding="utf-8")
        executable.chmod(0o755)
        runtime = root / "runtime"
        runtime.mkdir()
        (runtime / "librexruntime.dylib").touch()
        game_data = root / "game-data"
        game_data.mkdir()
        (game_data / "default.xex").write_bytes(b"fixture")
        output = root / "benchmarks"
        environment = os.environ.copy()
        environment.update(
            {
                "GOLDENEYE_BENCH_EXECUTABLE": str(executable),
                "GOLDENEYE_BENCH_RUNTIME_DIR": str(runtime),
                "GOLDENEYE_BENCH_GAME_DATA": str(game_data),
                "GOLDENEYE_BENCH_OUTPUT_ROOT": str(output),
                "GOLDENEYE_BENCH_CACHE_DIR": str(root / "cache"),
                "GOLDENEYE_BENCH_ALLOW_STALE_BUILD": "1",
                "GOLDENEYE_BENCH_WARMUP_WINDOWS": "0",
                "GOLDENEYE_BENCH_MEASURE_WINDOWS": "2",
                "GOLDENEYE_BENCH_TIMEOUT_S": "1" if silent else "5",
                "GOLDENEYE_BENCH_POLL_S": "0.05",
                "GOLDENEYE_BENCH_NATIVE_QUIT": "0",
                "FAKE_CHILD_PID_FILE": str(root / "child.pid"),
                "PYTHONDONTWRITEBYTECODE": "1",
            }
        )
        result = subprocess.run(
            ["/bin/bash", str(ROOT / "tools/benchmark-dam.sh")],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=20,
        )
        runs = list(output.iterdir())
        self.assertEqual(len(runs), 1, result.stdout + result.stderr)
        return result, runs[0]

    def test_requested_termination_passes(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, output = self.run_benchmark(Path(temporary), crash=False)
            detail = result.stdout + result.stderr
            if (output / "summary.txt").is_file():
                detail += (output / "summary.txt").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 0, detail)
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "pass")

    def test_unexpected_process_exit_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, output = self.run_benchmark(Path(temporary), crash=True)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "fail")
            self.assertTrue(
                any("status 7" in failure for failure in summary["failures"]),
                summary["failures"],
            )

    def test_timeout_terminates_process_and_is_reported(self):
        with tempfile.TemporaryDirectory() as temporary:
            result, output = self.run_benchmark(
                Path(temporary), crash=False, silent=True
            )
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            summary = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            self.assertTrue(
                any("timed out" in failure for failure in summary["failures"]),
                summary["failures"],
            )

    def test_requested_termination_cleans_the_entire_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result, output = self.run_benchmark(root, crash=False, spawn_child=True)
            detail = result.stdout + result.stderr
            if (output / "summary.txt").is_file():
                detail += (output / "summary.txt").read_text(encoding="utf-8")
            self.assertEqual(result.returncode, 0, detail)
            child_pid = int((root / "child.pid").read_text(encoding="utf-8"))
            for _ in range(50):
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.02)
            else:
                self.fail(f"benchmark left child process {child_pid} running")

    def test_invalid_numeric_options_fail_before_launch(self):
        cases = {
            "GOLDENEYE_BENCH_TIMEOUT_S": "1.5",
            "GOLDENEYE_BENCH_POLL_S": "nan",
            "GOLDENEYE_BENCH_WARMUP_WINDOWS": "-1",
            "GOLDENEYE_BENCH_MEASURE_WINDOWS": "0",
            "GOLDENEYE_BENCH_MIN_FPS": "nan",
            "GOLDENEYE_BENCH_MAX_P99_FRAME_MS": "-1",
            "GOLDENEYE_BENCH_MAX_FPS_CV_PERCENT": "inf",
            "GOLDENEYE_BENCH_NATIVE_QUIT": "2",
        }
        for name, value in cases.items():
            with self.subTest(name=name):
                environment = {
                    **os.environ,
                    name: value,
                    "PYTHONDONTWRITEBYTECODE": "1",
                }
                started = time.monotonic()
                result = subprocess.run(
                    ["/bin/bash", str(ROOT / "tools/benchmark-dam.sh")],
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    env=environment,
                    timeout=3,
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertLess(time.monotonic() - started, 2.0)


if __name__ == "__main__":
    unittest.main()
