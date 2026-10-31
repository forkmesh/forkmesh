#!/usr/bin/env python3
import base64
import re
from pathlib import Path


DESKTOP = Path(__file__).resolve().parents[1]
ROOT = DESKTOP.parents[0]
SETUP = (DESKTOP / "src" / "MainWindowSetup.cpp").read_text(encoding="utf-8")
SETTINGS = (DESKTOP / "src" / "MainWindowSettings.cpp").read_text(encoding="utf-8")
TRUST = (DESKTOP / "src" / "ReleaseTrust.cpp").read_text(encoding="utf-8")
INSTALLER = (ROOT / "www" / "public" / "install.sh").read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    end = text.find("\n}\n", start)
    return text[start:end]


auto = function_body(SETTINGS, "void MainWindow::maybeAutoUpdate(")
assert "creatordate" not in auto, "release tags must be ordered by version"
assert "newestReleaseTag(" in auto
assert "compareVersions(" in auto and "FORKMESH_VERSION" in auto
assert "updateRebuildRestart()" not in auto, (
    "auto-update must never fall back to building whatever git pull returns")

prebuilt = function_body(SETUP, "bool MainWindow::tryPrebuiltAutoUpdate(")
assert "release.json.sig" in prebuilt
assert "checkManifest(" in prebuilt
assert "updateRebuildRestart()" not in prebuilt
assert prebuilt.index("checkManifest(") < prebuilt.index("assets")
assert "autoUpdateFromVerifiedSource(tag, verifiedTagCommit)" in prebuilt

staged = function_body(SETUP, "void MainWindow::installPrebuiltAndRelaunch(")
assert "updateRebuildRestart()" not in staged

source = function_body(SETUP, "void MainWindow::autoUpdateFromVerifiedSource(")
assert "isCommitId(verifiedCommit)" in source
assert '"--ff-only"' in source and "verifiedCommit}" in source
assert "!= verifiedCommit" in source
assert '"pull"' not in source

pem = re.search(r"'(MCowBQYDK2VwAyEA[A-Za-z0-9+/=]+)'", INSTALLER)
assert pem, "install.sh pins one Ed25519 release key"
raw = re.search(r'QByteArrayLiteral\("([A-Za-z0-9+/=]+)"\)', TRUST)
assert raw, "ReleaseTrust.cpp pins one release key"
assert base64.b64decode(pem.group(1))[-32:] == base64.b64decode(raw.group(1)), (
    "the desktop and install.sh must pin the same release key")
assert 'QStringLiteral("forkmesh/forkmesh")' in TRUST

print("auto-update release trust OK")
