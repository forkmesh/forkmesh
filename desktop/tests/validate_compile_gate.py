#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

GATE = Path(__file__).resolve().parents[1] / "cmake" / "forkmesh-compile-gate.sh"

JOB = r"""
import fcntl, os, sys, time
state = sys.argv[1]
def bump(delta):
    with open(state + ".lock", "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            current, peak = map(int, open(state).read().split())
        except (FileNotFoundError, ValueError):
            current, peak = 0, 0
        current += delta
        peak = max(peak, current)
        open(state, "w").write("%d %d" % (current, peak))
bump(1)
time.sleep(0.4)
bump(-1)
sys.exit(int(sys.argv[2]))
"""


def peak_concurrency(jobs, extra_env):
    with tempfile.TemporaryDirectory() as tmp:
        state = os.path.join(tmp, "state")
        env = dict(os.environ)
        env.pop("FORKMESH_COMPILE_GATE_OFF", None)
        env.update({"FORKMESH_COMPILE_GATE_DIR": os.path.join(tmp, "slots")})
        env.update(extra_env)
        procs = [
            subprocess.Popen([str(GATE), sys.executable, "-c", JOB, state, "0"], env=env)
            for _ in range(jobs)
        ]
        codes = [p.wait(timeout=60) for p in procs]
        assert codes == [0] * jobs, codes
        current, peak = map(int, Path(state).read_text().split())
        assert current == 0
        return peak


def main():
    if not sys.platform.startswith("linux") or shutil.which("flock") is None:
        print("skipped: needs Linux and flock")
        return 0
    assert os.access(GATE, os.X_OK)

    peak = peak_concurrency(8, {"FORKMESH_COMPILE_SLOTS": "2", "FORKMESH_COMPILE_MIN_FREE_MB": "0"})
    assert 1 <= peak <= 2, peak

    peak = peak_concurrency(4, {"FORKMESH_COMPILE_SLOTS": "4", "FORKMESH_COMPILE_MIN_FREE_MB": str(1 << 30)})
    assert peak == 1, peak

    with tempfile.TemporaryDirectory() as tmp:
        env = dict(os.environ, FORKMESH_COMPILE_GATE_DIR=os.path.join(tmp, "slots"))
        code = subprocess.call([str(GATE), sys.executable, "-c", "import sys; sys.exit(7)"], env=env)
        assert code == 7, code

    print("compile gate ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
