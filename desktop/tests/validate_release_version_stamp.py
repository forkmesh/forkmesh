#!/usr/bin/env python3
"""Guard the release version stamp that "Draft a release" commits.

Drafting a release rewrites the version in several files across the repository.
Every stamp is a regular expression, so a renamed CMake project, a moved
version key or a reformatted Gradle block would silently turn one of them into
a no-op: the release would be tagged and published with a stale version in the
files the stamp missed. This test reads the stamp table out of the C++ source
and asserts each pattern still matches the file it targets.
"""

import re
import subprocess
from pathlib import Path


DESKTOP = Path(__file__).resolve().parents[1]
ROOT = DESKTOP.parents[0]
RELEASES = (DESKTOP / "src" / "MainWindowReleases.cpp").read_text(encoding="utf-8")

# Each stamped file, and why the release has to touch it.
REQUIRED = {
    "desktop/CMakeLists.txt": "the desktop project version",
    "native/android/app/build.gradle.kts": "the native Android app version",
}

def function_body(signature: str) -> str:
    start = RELEASES.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    brace = RELEASES.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body: {signature}")
    depth = 0
    for index in range(brace, len(RELEASES)):
        if RELEASES[index] == "{":
            depth += 1
        elif RELEASES[index] == "}":
            depth -= 1
            if depth == 0:
                return RELEASES[brace + 1 : index]
    raise AssertionError(f"unterminated body: {signature}")


def cpp_string(literal: str) -> str:
    """The runtime value of a C++ string literal's escaped source text."""
    return literal.replace("\\\\", "\x00").replace('\\"', '"').replace("\x00", "\\")


body = function_body("QList<ReleaseVersionFile> releaseVersionFiles(")

# The table lists a relative path, then the patterns stamped into that file, so
# reading the QStringLiteral values in source order pairs them up.
stamps: dict[str, list[str]] = {}
current: str | None = None
for match in re.finditer(r'QStringLiteral\(\s*"((?:[^"\\]|\\.)*)"\s*\)', body):
    value = cpp_string(match.group(1))
    if re.fullmatch(r"[\w./-]+\.\w+", value):
        current = value
        stamps.setdefault(current, [])
    else:
        assert current is not None, f"pattern {value!r} precedes any stamped file"
        stamps[current].append(value)

missing = sorted(set(REQUIRED) - set(stamps))
assert not missing, (
    "Draft a release must stamp the version into "
    + ", ".join(f"{path} ({REQUIRED[path]})" for path in missing)
)

captured: dict[str, list[str]] = {}
for path, patterns in stamps.items():
    assert patterns, f"{path} is listed with no version pattern"
    text = (ROOT / path).read_text(encoding="utf-8")
    for pattern in patterns:
        found = re.search(pattern, text, re.MULTILINE)
        assert found, (
            f"the release version stamp {pattern!r} no longer matches {path}; "
            "the version there would keep its old value through a release"
        )
        assert found.lastindex and found.lastindex >= 2, (
            f"the stamp {pattern!r} for {path} must capture the version text "
            "in group 2, which is what the release rewrites"
        )
        captured.setdefault(path, []).append(found.group(2))

# The native Android versionCode follows the published APK's scheme, so the two
# have to keep describing the same arithmetic.
host = (ROOT / "tools" / "forkmesh-release-host.py").read_text(encoding="utf-8")
assert "MAJOR*10000 + MINOR*100 + PATCH" in host
assert "parts.at(0).toInt() * 10000 + minor * 100 + patch" in RELEASES, (
    "releaseAndroidVersionCode must keep matching the release host's "
    "MAJOR*10000 + MINOR*100 + PATCH versionCode scheme"
)

# Nothing else in the repository may declare the release version without being
# stamped: a new file that carries it (another app, a package manifest) would
# otherwise fall one release behind the moment the next one is drafted. Test
# fixtures and recorded history are exempt; they only happen to use the value.
release_version = captured["desktop/CMakeLists.txt"][0]
declares_release = re.compile(
    r"(?i)version[^\n]{0,40}?(?<![0-9.])v?"
    + re.escape(release_version)
    + r"(?![0-9.])"
)
EXEMPT_PREFIXES = (".forkmesh/", "conformance/")
listed = subprocess.run(
    ["git", "ls-files", "-z"], cwd=ROOT, capture_output=True, check=False
)
if listed.returncode == 0:
    unstamped = []
    for path in listed.stdout.decode("utf-8", "replace").split("\0"):
        if (
            not path
            or path in stamps
            or path.startswith(EXEMPT_PREFIXES)
            or "test" in path.lower()
            or path == "desktop/src/MainWindowReleases.cpp"
        ):
            continue
        try:
            text = (ROOT / path).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if declares_release.search(text):
            unstamped.append(path)
    assert not unstamped, (
        f"these files declare the release version {release_version} but Draft a "
        "release does not stamp them; add them to releaseVersionFiles in "
        "desktop/src/MainWindowReleases.cpp: " + ", ".join(unstamped)
    )

# The drafting path has to run the stamp before it creates the tag, and on the
# release's target branch rather than only when that branch is checked out.
draft = function_body("void MainWindow::draftNewRelease(")
before_tag = draft.split('"tag",')[0]
assert '"--abbrev-ref", "HEAD"' not in before_tag, (
    "the version stamp must not depend on the target branch being checked out"
)
stamp_call = draft.find("stampReleaseTarget(dir, targetRef, tag")
tag_call = draft.find('"tag",')
assert stamp_call >= 0, "promptNewRelease must stamp the release version"
assert 0 <= stamp_call < tag_call, (
    "promptNewRelease must stamp the version before it creates the release tag, "
    "or the tagged commit will not carry it"
)

print("release version stamp OK")
