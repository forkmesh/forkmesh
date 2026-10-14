import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import plugin_data  # noqa: E402


def _entry(**extra):
    return {"accountSync": True, "dependencies": (), "settings": (), "files": (), **extra}


def test_packages_the_account_never_chose_are_marked_unchosen(monkeypatch):
    monkeypatch.setattr(plugin_data, "REGISTRY", {
        "voice-changer": _entry(),
        "chosen": _entry(),
        "defaulted": _entry(default=True),
        "dated": _entry(installedBefore="2026-10-01"),
        "health": _entry(accountData=True),
    })
    rows = {"chosen": {"installed": False, "settings": {}, "places": ()}}
    own = {"voice-changer": False, "chosen": False, "defaulted": True, "dated": False, "health": False}
    packages = {row["id"]: row for row in plugin_data._account_payload(rows, own)["packages"]}
    assert packages["voice-changer"].get("unchosen") is True
    assert "unchosen" not in packages["chosen"]
    assert "unchosen" not in packages["defaulted"]
    assert "unchosen" not in packages["dated"]
    assert "unchosen" not in packages["health"]
