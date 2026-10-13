#!/usr/bin/env python3
"""Compatibility entry point.

Host-side release completion now builds every host-only asset (the Android
APK and the signed Windows installer) in tools/forkmesh-release-host.py; this
name is kept because a release host that already has the
forkmesh-release-android.timer unit installed still calls this path; the
unit in tools/release-host/ now runs forkmesh-release-host.py directly.
"""

import runpy
import sys
from pathlib import Path


if __name__ == "__main__":
    sys.argv[0] = str(Path(__file__).with_name("forkmesh-release-host.py"))
    runpy.run_path(sys.argv[0], run_name="__main__")
