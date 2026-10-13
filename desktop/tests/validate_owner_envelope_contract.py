#!/usr/bin/env python3
"""Validate a Qt-emitted owner envelope with the Worker's real validator."""

import base64
import importlib.util
import json
from pathlib import Path
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: validate_owner_envelope_contract.py BINARY")
    binary = Path(sys.argv[1]).resolve()
    emitted = subprocess.run(
        [str(binary), "--emit-owner-envelope"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    payload = json.loads(emitted.stdout.decode("utf-8"))
    envelope = payload["envelope"]

    root = Path(__file__).resolve().parents[2]
    module_path = root / "app" / "src" / "security_controls.py"
    spec = importlib.util.spec_from_file_location(
        "qt_worker_owner_envelope_contract", module_path)
    security = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(security)

    validated = security.validate_owner_envelope(envelope)
    assert validated == envelope, "Worker rewrote/rejected Qt envelope"
    assert security.owner_envelope_key_id(validated) == payload["keyId"]
    assert len(validated["recipients"]) == 1
    plaintext = base64.b64decode(payload["plaintext"])
    assert plaintext not in json.dumps(envelope, sort_keys=True).encode("utf-8")
    print("Qt owner envelope accepted verbatim by Worker validator")


if __name__ == "__main__":
    main()
