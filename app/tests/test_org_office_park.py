import subprocess
import sys
from pathlib import Path

import org_office as o

ROOT = Path(__file__).resolve().parents[2]


def test_generated_park_layout_is_current():
    result = subprocess.run(
        [sys.executable, str(ROOT / "office" / "tools" / "build_park_map.py"), "--check"],
        capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr


def test_park_sits_east_of_the_office_without_moving_it():
    assert o.OFFICE_W == 66 and o.PARK_X0 > o.OFFICE_W and o.W == o.PARK_X0 + o.PARK_W
    for x in range(o.OFFICE_W, o.PARK_X0):
        for y in range(o.H):
            assert not o.is_walkable(x, y)
    for ride in o.RIDES.values():
        if ride["exit"]:
            assert o.is_walkable(*ride["exit"])
