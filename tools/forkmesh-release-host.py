#!/usr/bin/env python3
"""Complete desktop releases with the assets only the release host can build.

The Actions release job (.forkmesh/release.yml) runs on Linux sandboxes that
have neither the Android toolchain and upload keystore nor a Windows
cross toolchain and Authenticode certificate. Every candidate it publishes
therefore lacks the host-built assets:

  forkmesh-linux-x86_64               Debian 12 container build (+ AppImage, .zsync)
  forkmesh-android-universal.apk      native Android app (native/android, Gradle),
                                      signed with the direct-distribution key
  forkmesh-windows-x86_64-setup.exe   MinGW cross build, NSIS, Authenticode
  forkmesh-macos-{arm64,x86_64}.dmg   one universal image, built on the office Mac
                                      (Codemagic `macos-desktop` when it is offline)

and the promotion gate (tools/forkmesh-release-promote.py) holds the release
back until they exist. This tool runs on the release host, which has the
toolchains and the signing material:

  1. finds the newest candidate that carries every sandbox-built required
     asset and lacks a host-built asset that is required or that `latest`
     already offers (a release the public channel already serves can be
     completed in place when a new host asset becomes required);
  2. builds and signs each missing asset at that release's build commit
     (the macOS image on Codemagic, from the release tag);
  3. publishes them into the candidate (same tag and build commit), runs the
     promotion gate, and verifies the signed manifest against the installer's
     trust anchor;
  4. seeds every asset of the promoted release into each managed mirror's
     release store, hash-verified, BEFORE the metadata is visible;
  5. merges the metadata onto main and moves the release tag onto that commit,
     because deployed desktops read `latest` at the tag commit.

Run it by hand for one release (`--tag v0.8.7`), or from the
forkmesh-release-host.timer user unit with `--auto`, which exits quietly when
no release is waiting. Exit status: 0 done or nothing to do, 1 a step failed
(the public channel is untouched unless the failure came after the merge),
2 configuration error. A failed build is not retried for an hour.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import struct
import bz2
import zlib
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from collections import Counter
from pathlib import Path


LINUX_ASSET = "forkmesh-linux-x86_64"
APPIMAGE_ASSET = "forkmesh-linux-x86_64.AppImage"
ZSYNC_ASSET = "forkmesh-linux-x86_64.AppImage.zsync"
LINUX_ASSETS = (LINUX_ASSET, APPIMAGE_ASSET)
APK_ASSET = "forkmesh-android-universal.apk"
APK_APPLICATION_ID = "com.forkmesh.forkmesh"
APK_OUTPUT = "native/android/app/build/outputs/apk/release/app-release.apk"
WINDOWS_ASSET = "forkmesh-windows-x86_64-setup.exe"
MACOS_ASSET = "forkmesh-macos-arm64.dmg"
MACOS_X86_64_ASSET = "forkmesh-macos-x86_64.dmg"
MACOS_ASSETS = (MACOS_ASSET, MACOS_X86_64_ASSET)
HOST_ASSETS = (LINUX_ASSET, APPIMAGE_ASSET, APK_ASSET, WINDOWS_ASSET, MACOS_ASSET,
               MACOS_X86_64_ASSET)
PLATFORM_ASSETS = {
    "android": (APK_ASSET,),
    "linux": (LINUX_ASSET, APPIMAGE_ASSET),
    "macos": MACOS_ASSETS,
    "windows": (WINDOWS_ASSET,),
}
PLATFORM_LABELS = {"android": "Android", "linux": "Linux", "macos": "macOS",
                   "windows": "Windows"}
MAX_BUILD = 99
LOCK_WAIT_SECONDS = 4 * 60 * 60
LOCK_POLL_SECONDS = 15
CANDIDATE_WAIT_SECONDS = 2 * 60 * 60
CANDIDATE_POLL_SECONDS = 30
HOMEPAGE_POLL_SECONDS = 30
CODEMAGIC_API = "https://api.codemagic.io"
CODEMAGIC_DEFAULT_APP_ID = "6aac6e1e7b35578e30ad5a76"
CODEMAGIC_WORKFLOW = "macos-desktop"
CODEMAGIC_FAILED = frozenset({"failed", "canceled", "cancelled", "timeout", "skipped"})
CODEMAGIC_POLL_SECONDS = 60
CODEMAGIC_TIMEOUT_SECONDS = 3 * 60 * 60
PUBLIC_REPOSITORY = "https://forkmesh.com/forkmesh/forkmesh"
DMG_MIN_BYTES = 10 * 1024 * 1024
UDIF_CHUNK_ZERO = frozenset({0x00000000, 0x00000002})
UDIF_CHUNK_RAW = 0x00000001
UDIF_CHUNK_ZLIB = 0x80000005
UDIF_CHUNK_BZIP2 = 0x80000006
UDIF_CHUNK_SKIP = frozenset({0x7FFFFFFE, 0xFFFFFFFF})
MACHO_CPU_X86_64 = 0x01000007
MACHO_CPU_ARM64 = 0x0100000C
MACHO_CPU_TYPES = frozenset({7, 12, 18, MACHO_CPU_X86_64, MACHO_CPU_ARM64, 0x01000012,
                             0x0200000C})
MACHO_LOADABLE = frozenset({2, 6, 8})
MACHO_FAT = b"\xca\xfe\xba\xbe"
MACHO_THIN_64 = b"\xcf\xfa\xed\xfe"
SCAN_OVERLAP = 256
RELEASES = ".forkmesh/releases"
BUILDS_FILE = f"{RELEASES}/builds.json"
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_paths  # noqa: E402  (sibling module; tools run as scripts)
import forkmesh_safe_delete  # noqa: E402

# Releases are cut from the ForkMesh home project, whichever project is open.
LAYOUT = forkmesh_paths.resolve(forkmesh_paths.HOME_PROJECT)
FORKMESH_DATA = LAYOUT.data
INSTALLER_KEY_RE = re.compile(r"MCowBQYDK2VwAyEA[A-Za-z0-9+/]{43}=")
VERSION_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
HASH_RE = re.compile(r"^[0-9a-f]{64}$")
COMMIT_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
FAILURE_BACKOFF_SECONDS = 60 * 60
MAC_BUILD_KEY_DIR = LAYOUT.ssh / "mac-build"
MAC_BUILD_HOST = ""
MAC_ADDRESS_FILE = LAYOUT.state / "mac-build" / "address"
MAC_BUILD_GIT_URL = "https://app.forkmesh.com/forkmesh/forkmesh"
MAC_HOST_KEY_ALIAS = "forkmesh-mac"
MAC_LOCK_WAIT_SECONDS = 2 * 60 * 60
MAC_DMG_TIMEOUT_SECONDS = 3 * 60 * 60
MAC_IOS_TIMEOUT_SECONDS = 2 * 60 * 60
MAC_UNREACHABLE = 255
CODEMAGIC_IOS_WORKFLOW = "ios-native-release"
CODEMAGIC_IOS_TIMEOUT_SECONDS = 2 * 60 * 60
IOS_FIRST_RELEASE = (1, 2, 6)
IOS_SUBMITTED = "for App Review"
HOMEPAGE_MANIFEST_URL = ("https://app.forkmesh.com/api/repo/forkmesh/forkmesh/raw?"
                         "path=.forkmesh%2Freleases%2Flatest%2Frelease.json&ref=main")
HOMEPAGE_WAIT_SECONDS = 30 * 60
DMG_DIGEST_RE = re.compile(r"^([0-9a-f]{64})\s+(/\S+\.dmg)$")
MIRROR_NODE_ASSETS = {"x86_64": "forkmesh-mirror-node-linux-x86_64",
                      "arm64": "forkmesh-mirror-node-linux-arm64"}
ROLLOUT_BLOB_BASE = "https://app.forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/"
ROLLOUT_SETTLED = frozenset({"current", "deployed", "next-managed"})
ROLLOUT_MAX_ATTEMPTS = 6
ROLLOUT_READY_WAIT_SECONDS = 120
ROLLOUT_HOST_TIMEOUT_SECONDS = 15 * 60
NEXT_HOST_PREFIX = "fmnext-"
# The v3 direct-distribution key (android-direct-distribution-v3.jks), created
# 2026-09-22 after the v2 keystore and its password were lost with the node's
# data folder. It signs every APK from v1.2.3; v2 installs must reinstall once.
DEFAULT_SIGNER_SHA256 = (
    "e8c028a49936350273a1bcb4e822c0d4f8fedb98c3417ba9eb9d3c617a33f49a"
)
# The ForkMesh Authenticode certificate (release/windows-authenticode.pfx,
# self-issued 2026-09-13, valid to 2036) that signs the Windows installer.
DEFAULT_WINDOWS_SIGNER_SHA256 = (
    "06e093ead70bbda10b4cb60aaeb22c8c52aaed076e21f4056e452d38d1be4573"
)


class CompletionError(Exception):
    """A step failed; nothing after it ran."""


class ConfigError(Exception):
    """The host is not configured to complete releases."""


def _load_gate():
    path = ROOT / "tools" / "forkmesh-release-promote.py"
    spec = importlib.util.spec_from_file_location("forkmesh_release_promote", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules.setdefault(spec.name, module)
    spec.loader.exec_module(module)
    return module


# --- pure decisions (unit-tested) -------------------------------------------


def parse_version(tag: str) -> tuple[int, int, int] | None:
    match = VERSION_RE.match((tag or "").strip())
    return tuple(int(part) for part in match.groups()) if match else None


def android_version_code(tag: str, build: int = 1) -> int:
    """Android versionCode for build N of a desktop release:
    (MAJOR*10000 + MINOR*100 + PATCH) * 100 + N.

    It only has to increase from build to build so each APK installs over
    the previous one; v1.2.8 shipped as 10208 before build numbers, and its
    successor v1.2.9 build 1 is 1020901.
    """
    version = parse_version(tag)
    if version is None:
        raise CompletionError(f"release tag {tag!r} is not vMAJOR.MINOR.PATCH")
    major, minor, patch = version
    if minor >= 100 or patch >= 100:
        raise CompletionError(
            f"release {tag} does not fit the MAJOR*10000+MINOR*100+PATCH "
            "versionCode scheme"
        )
    if not 1 <= build <= MAX_BUILD:
        raise CompletionError(f"build {build} is outside 1-{MAX_BUILD}")
    return (major * 10000 + minor * 100 + patch) * 100 + build


def asset_names(manifest: dict | None) -> frozenset[str]:
    return frozenset(
        str(asset.get("name"))
        for asset in (manifest or {}).get("assets", [])
        if isinstance(asset, dict) and asset.get("name")
    )


def wanted_host_assets(
    latest: dict | None, required: frozenset[str],
    host_assets: tuple[str, ...] = HOST_ASSETS,
) -> frozenset[str]:
    """Host-built assets a promotable candidate must carry.

    An asset is wanted when the gate requires it or when `latest` already
    offers it (promoting without it would withdraw a download).
    """
    offered = asset_names(latest)
    return frozenset(name for name in host_assets if name in required or name in offered)


def missing_host_assets(
    manifest: dict, required: frozenset[str], wanted: frozenset[str],
    host_assets: tuple[str, ...] = HOST_ASSETS,
) -> frozenset[str]:
    """Wanted host assets this candidate lacks, or nothing when the sandbox
    matrix itself is incomplete (that is not a host problem)."""
    names = asset_names(manifest)
    sandbox_required = required - frozenset(host_assets)
    if not sandbox_required <= names:
        return frozenset()
    return wanted - names


def pending_completion(
    manifests: dict[str, dict], required: frozenset[str],
    host_assets: tuple[str, ...] = HOST_ASSETS,
) -> tuple[str, dict, frozenset[str]] | None:
    """The newest candidate held back only for host-built assets.

    Candidates older than `latest` are never revisited. The candidate `latest`
    was promoted from is revisited when a host asset became required after
    the promotion, so the public release gains it in place.
    """
    latest = manifests.get("latest")
    wanted = wanted_host_assets(latest, required, host_assets)
    if not wanted:
        return None
    latest_version = parse_version(str((latest or {}).get("tag", "")))
    best = None
    for channel, manifest in manifests.items():
        if not channel.startswith("candidate-"):
            continue
        version = parse_version(str(manifest.get("tag", "")))
        if version is None:
            continue
        if latest_version is not None and version < latest_version:
            continue
        if (latest_version is not None and version == latest_version
                and manifest.get("tag") != latest.get("tag")):
            continue
        missing = missing_host_assets(manifest, required, wanted, host_assets)
        if not missing:
            continue
        if best is None or version > best[0]:
            best = (version, channel, manifest, missing)
    return None if best is None else (best[1], best[2], best[3])


def pending_android_completion(
    manifests: dict[str, dict], required: frozenset[str]
) -> tuple[str, dict] | None:
    """The newest candidate held back only because it lacks latest's APK
    (the pre-Windows decision, kept for its callers and tests)."""
    pending = pending_completion(manifests, required, (APK_ASSET,))
    if pending is None:
        return None
    channel, manifest, missing = pending
    latest_version = parse_version(str(manifests.get("latest", {}).get("tag", "")))
    if latest_version is not None and parse_version(str(manifest.get("tag"))) == latest_version:
        return None
    return (channel, manifest) if APK_ASSET in missing else None


def valid_build(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= MAX_BUILD


def parse_builds(text: str) -> dict:
    try:
        data = json.loads(text or "{}")
    except ValueError:
        data = {}
    if not isinstance(data, dict):
        data = {}
    tag = str(data.get("tag") or "")
    raw = data.get("builds") if isinstance(data.get("builds"), dict) else {}
    return {
        "tag": tag if parse_version(tag) else "",
        "builds": {platform: raw[platform] if valid_build(raw.get(platform)) else 1
                   for platform in PLATFORM_ASSETS},
    }


def render_builds(tag: str, builds: dict[str, int]) -> str:
    return json.dumps({"tag": tag,
                       "builds": {platform: builds.get(platform, 1)
                                  for platform in PLATFORM_ASSETS}}, indent=2) + "\n"


def recorded_builds(text: str, tag: str) -> dict[str, int]:
    state = parse_builds(text)
    if state["tag"] != tag:
        return {platform: 1 for platform in PLATFORM_ASSETS}
    return dict(state["builds"])


def platform_build(manifest: dict | None, platform: str) -> int:
    builds = [asset.get("build") for asset in (manifest or {}).get("assets", [])
              if isinstance(asset, dict) and asset.get("name") in PLATFORM_ASSETS[platform]]
    return max((build for build in builds if valid_build(build)), default=1)


def deploy_target(manifests: dict[str, dict], tag: str | None = None) -> tuple[str, dict]:
    if tag:
        tag = tag if tag.startswith("v") else f"v{tag}"
        channel = f"candidate-{tag.lstrip('v')}"
        candidate = manifests.get(channel)
        if candidate is None or candidate.get("tag") != tag:
            raise CompletionError(f"main has no {channel} manifest for {tag}")
        return channel, candidate
    latest_version = parse_version(str((manifests.get("latest") or {}).get("tag", "")))
    best = None
    for channel, manifest in manifests.items():
        if not channel.startswith("candidate-"):
            continue
        version = parse_version(str(manifest.get("tag", "")))
        if version is None or f"candidate-{'.'.join(map(str, version))}" != channel:
            continue
        if latest_version is not None and version < latest_version:
            continue
        if best is None or version > best[0]:
            best = (version, channel, manifest)
    if best is None:
        raise CompletionError("main has no release candidate to build for")
    return best[1], best[2]


def promotion_blockers(latest: dict | None, candidate_names: frozenset[str],
                       required: frozenset[str]) -> frozenset[str]:
    return (asset_names(latest) | required) - candidate_names


def plan_deploy(manifests: dict[str, dict], platform: str, builds_text: str,
                main_commit: str, tag: str | None = None,
                first_build: bool = False) -> dict:
    channel, candidate = deploy_target(manifests, tag)
    tag = str(candidate["tag"])
    names = PLATFORM_ASSETS[platform]
    published = set(names) <= asset_names(candidate)
    current = max(recorded_builds(builds_text, tag)[platform],
                  platform_build(candidate, platform)) if published else 0
    plan = {"platform": platform, "channel": channel, "tag": tag, "assets": names,
            "current": current, "skip": False, "rebuild": published}
    if published and first_build:
        return dict(plan, skip=True, build=current,
                    commit=str(candidate.get("build_commit", "")).lower())
    if not published:
        return dict(plan, build=1, commit=str(candidate.get("build_commit", "")).lower())
    if current >= MAX_BUILD:
        raise CompletionError(f"{PLATFORM_LABELS[platform]} {tag} already reached build "
                              f"{MAX_BUILD}; cut a new version instead")
    if not COMMIT_RE.match(main_commit or ""):
        raise CompletionError("main has no exact commit to build from")
    return dict(plan, build=current + 1, commit=main_commit.lower())


def describe_plan(plan: dict) -> str:
    label = PLATFORM_LABELS[plan["platform"]]
    source = ("a rebuild from main" if plan["rebuild"]
              else "the release's build commit")
    return (f"{label} {plan['tag']} build {plan['build']} from {plan['commit'][:12]} "
            f"({source}) into {plan['channel']}")


def installer_public_key(install_sh: str) -> str:
    keys = set(INSTALLER_KEY_RE.findall(install_sh))
    if len(keys) != 1:
        raise ConfigError(
            f"expected one Ed25519 release key in www/public/install.sh, found {len(keys)}"
        )
    return keys.pop()


def parse_hosts(text: str) -> list[tuple[str, str]]:
    hosts = []
    for line in text.splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if len(fields) < 2 or not re.fullmatch(r"[A-Za-z0-9.:-]+", fields[1]):
            raise ConfigError(f"unreadable mirror hosts line: {line!r}")
        hosts.append((fields[0], fields[1]))
    return hosts


def fleet_core_hosts() -> list[tuple[str, str]]:
    try:
        import next_common
        fleet = next_common.load_fleet()
    except (ImportError, OSError, RuntimeError, ValueError, KeyError):
        return []
    return [("fmnext-%s" % node["name"], str(node["public_ipv4"]))
            for node in fleet.get("nodes") or []
            if node.get("role", "core") == "core" and node.get("public_ipv4")]


def seed_hosts(text: str, cores: list[tuple[str, str]] | None = None) -> list[tuple[str, str]]:
    listed = parse_hosts(text)
    cores = fleet_core_hosts() if cores is None else cores
    if not cores:
        return listed
    return list(cores) + [(name, ip) for name, ip in listed
                          if not name.startswith("fmnext-")]


def failure_backoff_active(marker: Path, now: float,
                           backoff: float = FAILURE_BACKOFF_SECONDS) -> bool:
    """True while a recent failed attempt (recorded in `marker`) is fresh."""
    try:
        return now - float(marker.read_text().strip()) < backoff
    except (OSError, ValueError):
        return False


def actions_variable(conf_text: str, name: str) -> str:
    section = ""
    for line in conf_text.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            continue
        if section != "actions" or not stripped.startswith("variables="):
            continue
        raw = stripped[len("variables="):]
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1]
        raw = re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)),
                     raw)
        if raw.startswith("@ByteArray(") and raw.endswith(")"):
            raw = raw[len("@ByteArray("):-1]
        try:
            variables = json.loads(raw)
        except ValueError:
            return ""
        value = variables.get(name) if isinstance(variables, dict) else None
        return value.strip() if isinstance(value, str) else ""
    return ""


def ios_release_tag(manifests: dict[str, dict],
                    first: tuple[int, int, int] = IOS_FIRST_RELEASE) -> str:
    best = None
    for channel, manifest in manifests.items():
        tag = str((manifest or {}).get("tag", ""))
        version = parse_version(tag)
        if version is None or version < first or channel != f"candidate-{tag.lstrip('v')}":
            continue
        if best is None or version > best[0]:
            best = (version, tag)
    return best[1] if best else ""


def release_notes_text(raw: str, tag: str) -> str:
    lines = []
    for line in raw.splitlines():
        if line.startswith("-----BEGIN PGP SIGNATURE") or line.startswith("-----BEGIN SSH SIGNATURE"):
            break
        lines.append(line.rstrip())
    while lines and (not lines[0].strip() or lines[0].strip() in (tag, tag.lstrip("v"))):
        lines.pop(0)
    text = "\n".join(lines).strip()
    return text or f"ForkMesh {tag.lstrip('v')}."


def cm_env_values(text: str) -> dict[str, str]:
    values = {}
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        if sep and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            values[key] = value
    return values


def mac_dmg_result(build_log: str) -> tuple[str, str]:
    for line in reversed(build_log.splitlines()):
        match = DMG_DIGEST_RE.match(line.strip())
        if match:
            return match.group(1), match.group(2)
    raise CompletionError("the Mac build log names no finished .dmg (no sha256 line)")


def peeled_tag_commit(ls_remote: str, tag: str) -> str:
    direct = peeled = ""
    for line in ls_remote.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        if fields[1] == f"refs/tags/{tag}^{{}}":
            peeled = fields[0].lower()
        elif fields[1] == f"refs/tags/{tag}":
            direct = fields[0].lower()
    return peeled or direct


def dmg_artefact_name(tag: str) -> str:
    return f"ForkMesh-{tag.lstrip('v')}-macos-universal.dmg"


def codemagic_build_commit(build: dict) -> str:
    commit = build.get("commit")
    if isinstance(commit, dict):
        return str(commit.get("hash") or "").lower()
    return str(commit or "").lower()


def reusable_codemagic_build(builds: list, tag: str, build_commit: str,
                             workflow: str = CODEMAGIC_WORKFLOW) -> str:
    best = None
    for build in builds or []:
        if not isinstance(build, dict):
            continue
        if build.get("workflowId") not in (None, workflow):
            continue
        if build.get("tag") != tag:
            continue
        if str(build.get("status", "")).lower() in CODEMAGIC_FAILED:
            continue
        commit = codemagic_build_commit(build)
        if commit and build_commit and commit != build_commit.lower():
            continue
        started = str(build.get("createdAt") or build.get("startedAt") or "")
        if best is None or started > best[0]:
            best = (started, str(build.get("_id") or ""))
    return best[1] if best and best[1] else ""


def codemagic_build_finished(build: dict, build_commit: str,
                             workflow: str = CODEMAGIC_WORKFLOW) -> bool:
    status = str(build.get("status", "")).lower()
    if status in CODEMAGIC_FAILED:
        raise CompletionError(f"Codemagic {workflow} build {build.get('_id', '?')} "
                              f"ended {status}")
    if status != "finished":
        return False
    if not build_commit:
        return True
    commit = codemagic_build_commit(build)
    if commit != build_commit.lower():
        raise CompletionError(f"Codemagic built {commit[:12] or 'an unknown commit'}, "
                              f"not the release build commit {build_commit[:12]}")
    return True


def macos_artefact(build: dict, tag: str) -> dict:
    wanted = dmg_artefact_name(tag)
    artefacts = build.get("artefacts") or build.get("artifacts") or []
    matches = [a for a in artefacts if isinstance(a, dict) and a.get("name") == wanted]
    if len(matches) != 1 or not matches[0].get("url"):
        names = ", ".join(sorted(str(a.get("name")) for a in artefacts if isinstance(a, dict)))
        raise CompletionError(f"Codemagic build {build.get('_id', '?')} has no {wanted} "
                              f"artefact (has: {names or 'none'})")
    return matches[0]


def codemagic_url_allowed(url: str) -> bool:
    parsed = urllib.parse.urlparse(url)
    host = (parsed.hostname or "").lower()
    return parsed.scheme == "https" and (host == "codemagic.io" or host.endswith(".codemagic.io"))


def verify_dmg(path: Path, expected_size=None, expected_md5: str = "") -> None:
    size = path.stat().st_size
    if expected_size not in (None, "") and int(expected_size) != size:
        raise CompletionError(f"{path.name} is {size} bytes, Codemagic reported {expected_size}")
    if size < DMG_MIN_BYTES:
        raise CompletionError(f"{path.name} is only {size} bytes")
    with path.open("rb") as handle:
        handle.seek(-512, os.SEEK_END)
        trailer = handle.read(4)
    if trailer != b"koly":
        raise CompletionError(f"{path.name} is not a UDIF disk image (no koly trailer)")
    if expected_md5:
        digest = hashlib.md5()
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1 << 20), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected_md5.lower():
            raise CompletionError(f"{path.name} md5 does not match Codemagic's")


def udif_chunks(path: Path):
    with path.open("rb") as handle:
        handle.seek(-512, os.SEEK_END)
        koly = handle.read(512)
        if koly[:4] != b"koly":
            raise CompletionError(f"{path.name} is not a UDIF disk image (no koly trailer)")
        data_offset = struct.unpack(">Q", koly[24:32])[0]
        xml_offset, xml_length = struct.unpack(">QQ", koly[216:232])
        handle.seek(xml_offset)
        try:
            plist = plistlib.loads(handle.read(xml_length))
            tables = plist["resource-fork"]["blkx"]
        except (ValueError, KeyError, TypeError, plistlib.InvalidFileException):
            raise CompletionError(f"{path.name} has no readable UDIF block table") from None
        chunks = []
        for table in tables:
            mish = bytes(table.get("Data") or b"")
            if mish[:4] != b"mish" or len(mish) < 204:
                raise CompletionError(f"{path.name} has a malformed UDIF block table")
            first_sector = struct.unpack(">Q", mish[8:16])[0]
            table_offset = struct.unpack(">Q", mish[24:32])[0]
            count = struct.unpack(">I", mish[200:204])[0]
            for index in range(count):
                entry = mish[204 + 40 * index:244 + 40 * index]
                if len(entry) < 40:
                    raise CompletionError(f"{path.name} has a truncated UDIF block table")
                kind, _, sector, sectors, offset, length = struct.unpack(">IIQQQQ", entry)
                if kind in UDIF_CHUNK_SKIP:
                    continue
                chunks.append(((first_sector + sector) * 512, sectors * 512, kind,
                               data_offset + table_offset + offset, length))
        for position, size, kind, offset, length in sorted(chunks):
            if kind in UDIF_CHUNK_ZERO:
                yield position, None
                continue
            handle.seek(offset)
            raw = handle.read(length)
            if kind == UDIF_CHUNK_RAW:
                yield position, raw
            elif kind == UDIF_CHUNK_ZLIB:
                yield position, zlib.decompress(raw)
            elif kind == UDIF_CHUNK_BZIP2:
                yield position, bz2.decompress(raw)
            else:
                raise CompletionError(f"{path.name} uses UDIF chunk type {kind:#x}; build it "
                                      "with hdiutil -format UDZO")


def _fat_slices(buffer: bytes, at: int) -> list[tuple[int, int]] | None:
    count = struct.unpack(">I", buffer[at + 4:at + 8])[0]
    if not 1 <= count <= 8 or at + 8 + 20 * count > len(buffer):
        return None
    slices = []
    for index in range(count):
        cpu, _, offset, size, align = struct.unpack(
            ">iIIII", buffer[at + 8 + 20 * index:at + 28 + 20 * index])
        if cpu not in MACHO_CPU_TYPES or offset < 8 + 20 * count or size == 0 or align > 16:
            return None
        slices.append((cpu, offset))
    return slices


def inspect_macos_dmg(path: Path, build_commit: str) -> dict:
    fat, thin, slice_starts = [], {}, {}
    commit = build_commit.lower().encode("ascii")
    carries_commit = False
    tail, tail_position = b"", None

    def scan(buffer: bytes, base: int, limit: int) -> None:
        nonlocal carries_commit
        at = buffer.find(MACHO_FAT)
        while 0 <= at < limit:
            slices = _fat_slices(buffer, at)
            if slices:
                fat.append({cpu for cpu, _ in slices})
                slice_starts.update((base + at + offset, cpu) for cpu, offset in slices)
            at = buffer.find(MACHO_FAT, at + 1)
        at = buffer.find(MACHO_THIN_64)
        while 0 <= at < limit:
            if at + 16 <= len(buffer):
                cpu, _, filetype = struct.unpack("<iII", buffer[at + 4:at + 16])
                if cpu in (MACHO_CPU_X86_64, MACHO_CPU_ARM64) and filetype in MACHO_LOADABLE:
                    thin[base + at] = cpu
            at = buffer.find(MACHO_THIN_64, at + 1)
        if not carries_commit and commit and commit in buffer:
            carries_commit = True

    for position, data in udif_chunks(path):
        if tail_position is None or position != tail_position + len(tail) or data is None:
            if tail:
                scan(tail, tail_position, len(tail))
            tail, tail_position = b"", position
        if data is None:
            continue
        buffer = tail + data
        keep = min(SCAN_OVERLAP, len(buffer))
        scan(buffer, tail_position, len(buffer) - keep)
        tail_position += len(buffer) - keep
        tail = buffer[len(buffer) - keep:]
    if tail:
        scan(tail, tail_position, len(tail))
    universal_binaries = sum(1 for cpus in fat if {MACHO_CPU_X86_64, MACHO_CPU_ARM64} <= cpus)
    standalone = Counter(cpu for at, cpu in thin.items() if at not in slice_starts)
    displaced = Counter(cpu for at, cpu in slice_starts.items() if at not in thin)
    single_arch = sum((standalone - displaced).values())
    return {
        "universal": bool(universal_binaries) and not single_arch,
        "universal_binaries": universal_binaries,
        "single_arch_binaries": single_arch,
        "carries_commit": carries_commit,
    }


def local_macos_dmg(path: Path, build_commit: str) -> tuple[Path, frozenset[str]]:
    if not path.is_file():
        raise ConfigError(f"--macos-dmg {path} is not a file")
    verify_dmg(path)
    report = inspect_macos_dmg(path, build_commit)
    if not report["carries_commit"]:
        raise CompletionError(f"{path.name} does not carry the release build commit "
                              f"{build_commit[:12]}; it was built from other source")
    if not report["universal_binaries"] and not report["single_arch_binaries"]:
        raise CompletionError(f"{path.name} holds no Mach-O binary")
    names = frozenset(MACOS_ASSETS) if report["universal"] else frozenset({MACOS_ASSET})
    log(f"using {path} for the macOS image ({path.stat().st_size} bytes, sha256 "
        f"{sha256_file(path)[:12]}, {'universal' if report['universal'] else 'single-arch'}: "
        f"{report['universal_binaries']} universal and {report['single_arch_binaries']} "
        f"single-arch Mach-O files) as {describe(names)}")
    return path, names


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class CodemagicClient:
    def __init__(self, token: str, app_id: str, api: str = CODEMAGIC_API):
        self.token = token
        self.app_id = app_id
        self.api = api.rstrip("/")

    def _request(self, method: str, path: str, body: dict | None = None) -> dict:
        data = json.dumps(body).encode("utf-8") if body is not None else None
        request = urllib.request.Request(f"{self.api}{path}", data=data, method=method)
        request.add_header("x-auth-token", self.token)
        request.add_header("Accept", "application/json")
        if data is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                payload = response.read()
        except urllib.error.HTTPError as exc:
            raise CompletionError(f"Codemagic {method} {path} returned HTTP {exc.code}") from None
        except (urllib.error.URLError, OSError) as exc:
            raise CompletionError(f"Codemagic {method} {path} failed: {exc}") from None
        try:
            parsed = json.loads(payload or b"{}")
        except ValueError:
            raise CompletionError(f"Codemagic {method} {path} did not return JSON") from None
        return parsed if isinstance(parsed, dict) else {}

    def builds(self, workflow: str = CODEMAGIC_WORKFLOW) -> list:
        query = urllib.parse.urlencode({"appId": self.app_id, "workflowId": workflow})
        return list(self._request("GET", f"/builds?{query}").get("builds") or [])

    def start_build(self, tag: str, workflow: str = CODEMAGIC_WORKFLOW,
                    variables: dict[str, str] | None = None) -> str:
        body = {"appId": self.app_id, "workflowId": workflow, "tag": tag,
                "labels": ["forkmesh-release-host", tag]}
        if variables:
            body["environment"] = {"variables": dict(variables)}
        started = self._request("POST", "/builds", body)
        build_id = str(started.get("buildId") or "")
        if not build_id:
            raise CompletionError("Codemagic accepted the build request but returned no buildId")
        return build_id

    def build(self, build_id: str) -> dict:
        build = self._request("GET", f"/builds/{urllib.parse.quote(build_id)}").get("build")
        return build if isinstance(build, dict) else {}

    def download(self, url: str, dest: Path) -> None:
        if not codemagic_url_allowed(url):
            raise CompletionError(f"refusing to send the Codemagic token to {url}")
        opener = urllib.request.build_opener(_NoRedirect)
        request = urllib.request.Request(url)
        request.add_header("x-auth-token", self.token)
        try:
            response = opener.open(request, timeout=300)
        except urllib.error.HTTPError as exc:
            location = exc.headers.get("Location") if exc.code in (301, 302, 303, 307, 308) else None
            if not location:
                raise CompletionError(f"artefact download returned HTTP {exc.code}") from None
            target = urllib.parse.urljoin(url, location)
            if urllib.parse.urlparse(target).scheme != "https":
                raise CompletionError("artefact download redirected off HTTPS") from None
            response = urllib.request.urlopen(target, timeout=300)
        with response, dest.open("wb") as handle:
            shutil.copyfileobj(response, handle, 1 << 20)


def codemagic_client(needed_for: str = "") -> CodemagicClient:
    conf = LAYOUT.settings_file
    conf_text = conf.read_text(errors="replace") if conf.is_file() else ""
    token = (os.environ.get("CODEMAGIC_API_TOKEN", "").strip()
             or actions_variable(conf_text, "CODEMAGIC_API_TOKEN"))
    if not token:
        raise ConfigError(
            f"{needed_for or MACOS_ASSET + ' must be rebuilt for this release (latest offers it)'}"
            " but no Codemagic API token is configured: set CODEMAGIC_API_TOKEN in the service "
            "environment or in the desktop's Actions variables (Settings -> Secrets & "
            f"Coves, read from {conf}); Codemagic -> Teams -> Personal account -> "
            "Integrations -> Codemagic API issues one")
    app_id = (os.environ.get("CODEMAGIC_APP_ID", "").strip()
              or actions_variable(conf_text, "CODEMAGIC_APP_ID") or CODEMAGIC_DEFAULT_APP_ID)
    return CodemagicClient(token, app_id)


def public_tag_commit(repository: str, tag: str) -> str:
    listed = run(["git", "ls-remote", repository, f"refs/tags/{tag}", f"refs/tags/{tag}^{{}}"],
                 check=False, timeout=120)
    return peeled_tag_commit(listed.stdout, tag) if listed.returncode == 0 else ""


def start_macos_build(client, tag: str, build_commit: str, repository: str,
                      tag_lookup=None) -> str:
    reused = reusable_codemagic_build(client.builds(), tag, build_commit)
    if reused:
        log(f"reusing Codemagic {CODEMAGIC_WORKFLOW} build {reused} for {tag}")
        return reused
    peeled = (tag_lookup or public_tag_commit)(repository, tag)
    if peeled != build_commit.lower():
        raise CompletionError(
            f"{repository} serves tag {tag} at {peeled[:12] or 'nothing'}, not the build "
            f"commit {build_commit[:12]}; Codemagic builds from that mirror, so the macOS "
            "image would not match the release")
    build_id = client.start_build(tag)
    log(f"started Codemagic {CODEMAGIC_WORKFLOW} build {build_id} for {tag}")
    return build_id


def await_codemagic_build(client, build_id: str, build_commit: str,
                          workflow: str = CODEMAGIC_WORKFLOW,
                          timeout: float = CODEMAGIC_TIMEOUT_SECONDS,
                          poll: float = CODEMAGIC_POLL_SECONDS,
                          sleep=time.sleep, clock=time.monotonic) -> dict:
    deadline = clock() + timeout
    while True:
        build = client.build(build_id)
        if codemagic_build_finished(build, build_commit, workflow):
            return build
        if clock() >= deadline:
            raise CompletionError(f"Codemagic build {build_id} did not finish within "
                                  f"{int(timeout // 60)} minutes "
                                  f"(status {build.get('status', '?')})")
        sleep(poll)


def await_macos_dmg(client, build_id: str, tag: str, build_commit: str, dest_dir: Path,
                    timeout: float = CODEMAGIC_TIMEOUT_SECONDS,
                    poll: float = CODEMAGIC_POLL_SECONDS,
                    sleep=time.sleep, clock=time.monotonic) -> Path:
    build = await_codemagic_build(client, build_id, build_commit, CODEMAGIC_WORKFLOW,
                                  timeout, poll, sleep, clock)
    artefact = macos_artefact(build, tag)
    dest_dir.mkdir(parents=True, exist_ok=True)
    download = dest_dir / f"{artefact['name']}.part"
    download.unlink(missing_ok=True)
    client.download(str(artefact["url"]), download)
    dmg = dest_dir / MACOS_ASSET
    dmg.unlink(missing_ok=True)
    if zipfile.is_zipfile(download):
        with zipfile.ZipFile(download) as archive:
            members = [m for m in archive.namelist()
                       if Path(m).name == artefact["name"]]
            if len(members) != 1:
                raise CompletionError(f"the Codemagic archive does not hold {artefact['name']}")
            with archive.open(members[0]) as source, dmg.open("wb") as handle:
                shutil.copyfileobj(source, handle, 1 << 20)
        download.unlink()
        verify_dmg(dmg)
    else:
        download.rename(dmg)
        verify_dmg(dmg, artefact.get("size"), str(artefact.get("md5") or ""))
    log(f"downloaded {artefact['name']} from Codemagic build {build_id} "
        f"({dmg.stat().st_size} bytes, sha256 {sha256_file(dmg)[:12]})")
    return dmg


def start_codemagic_macos(tag: str, build_commit: str, repository: str):
    client = codemagic_client()
    try:
        return client, start_macos_build(client, tag, build_commit, repository), ""
    except CompletionError as exc:
        log(f"macOS image deferred ({exc}); building the other assets now")
        return client, "", str(exc)


def release_ios_on_codemagic(client, tag: str, notes: str, repository: str,
                             tag_lookup=None, sleep=time.sleep, clock=time.monotonic) -> dict:
    build_id = reusable_codemagic_build(client.builds(CODEMAGIC_IOS_WORKFLOW), tag, "",
                                        CODEMAGIC_IOS_WORKFLOW)
    if build_id:
        log(f"reusing Codemagic {CODEMAGIC_IOS_WORKFLOW} build {build_id} for {tag}")
    else:
        if not (tag_lookup or public_tag_commit)(repository, tag):
            raise CompletionError(f"{repository} does not serve tag {tag} yet; Codemagic "
                                  "builds the iOS app from that mirror")
        build_id = client.start_build(tag, CODEMAGIC_IOS_WORKFLOW, {
            "FORKMESH_IOS_APP_STORE": "1",
            "FORKMESH_IOS_WHATS_NEW": notes,
            "TESTFLIGHT_WHATS_NEW": notes,
        })
        log(f"started Codemagic {CODEMAGIC_IOS_WORKFLOW} build {build_id} for {tag}")
    await_codemagic_build(client, build_id, "", CODEMAGIC_IOS_WORKFLOW,
                          CODEMAGIC_IOS_TIMEOUT_SECONDS, CODEMAGIC_POLL_SECONDS, sleep, clock)
    return {"route": "codemagic", "codemagic_build": build_id}


# --- host operations ---------------------------------------------------------


def log(message: str) -> None:
    print(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {message}", flush=True)


def run(args, *, cwd=None, env=None, check=True, timeout=None, stdin=None,
        capture=True) -> subprocess.CompletedProcess:
    result = subprocess.run(
        [str(arg) for arg in args], cwd=cwd, env=env, stdin=stdin,
        capture_output=capture, text=True, timeout=timeout, check=False,
    )
    if check and result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()[-2000:]
        raise CompletionError(f"{' '.join(str(a) for a in args[:4])} failed "
                              f"({result.returncode}): {detail}")
    return result


def git(repo: Path, *args, check=True) -> str:
    return run(["git", "-C", repo, *args], check=check).stdout.strip()


class MacUnreachable(CompletionError):
    pass


class MacHost:
    def __init__(self, address: str, key: Path, known_hosts: Path):
        self.address = address
        self.key = key
        self.known_hosts = known_hosts

    def command(self, remote: str, connect_timeout: int = 15) -> list[str]:
        return ["ssh", "-i", str(self.key), "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes",
                "-o", f"ConnectTimeout={connect_timeout}", "-o", "StrictHostKeyChecking=yes",
                "-o", f"UserKnownHostsFile={self.known_hosts}",
                "-o", f"HostKeyAlias={MAC_HOST_KEY_ALIAS}",
                "-o", "ServerAliveInterval=30", "-o", "ServerAliveCountMax=10",
                f"build@{self.address}", remote]

    def reachable(self) -> bool:
        try:
            return subprocess.run(self.command("true", 5), stdin=subprocess.DEVNULL,
                                  capture_output=True, timeout=30).returncode == 0
        except (subprocess.TimeoutExpired, OSError):
            return False

    def run(self, script: str, *args: str, stdin=None, log_file: Path | None = None,
            output: Path | None = None, timeout: float = 600, lock: str = "") -> str:
        if lock:
            script = f"MAC_LOCK_NAME={shlex.quote(lock)}\n{MAC_OWNED}\n{script}"
        remote = " ".join(["bash", "-c", shlex.quote(script), "mac-step",
                           *(shlex.quote(str(arg)) for arg in args)])
        sink = (output.open("wb") if output else log_file.open("ab") if log_file
                else subprocess.PIPE)
        try:
            result = subprocess.run(
                self.command(remote), stdin=subprocess.DEVNULL if stdin is None else stdin,
                stdout=sink, stderr=subprocess.STDOUT if log_file else subprocess.PIPE,
                timeout=timeout)
        finally:
            if sink is not subprocess.PIPE:
                sink.close()
        captured = (result.stdout or b"").decode("utf-8", "replace")
        detail = (result.stderr or b"").decode("utf-8", "replace")
        if log_file:
            detail = log_file.read_text(errors="replace")
        if result.returncode == MAC_UNREACHABLE:
            raise MacUnreachable(f"lost the office Mac at {self.address}: {detail.strip()[-500:]}")
        if result.returncode != 0:
            raise CompletionError(f"the office Mac step failed ({result.returncode}): "
                                  f"{(detail or captured).strip()[-2000:]}")
        return captured


MAC_LOCK_SCRIPT = r"""
run="$1"; limit="$2"; waited=0
killtree() { local child; for child in $(pgrep -P "$1"); do killtree "$child"; done; kill "$1" 2>/dev/null || true; }
mkdir -p ci
until mkdir ci/lock 2>/dev/null && printf '%s\n' "$run" > ci/lock/run; do
  if [ "$(ps -o ppid= -p $$ | tr -d ' ')" = 1 ]; then
    exit 1
  fi
  if [ "$(cat ci/lock/run 2>/dev/null)" = "$run" ]; then
    echo "ending an earlier attempt of $run"
    pid="$(cat ci/lock/pid 2>/dev/null || true)"
    [ -z "$pid" ] || killtree "$pid"
    sleep 5
    rm -rf ci/lock
    continue
  fi
  if [ -n "$(find ci/lock -maxdepth 0 -mmin +240 2>/dev/null)" ]; then
    echo "removing a build lock older than four hours: $(cat ci/lock/run 2>/dev/null)"
    rm -rf ci/lock
    continue
  fi
  if [ "$waited" -ge "$limit" ]; then
    echo "another build held ~/ci/lock for $((limit / 60)) minutes: $(cat ci/lock/run 2>/dev/null)" >&2
    exit 1
  fi
  [ "$waited" = 0 ] && echo "waiting for the build lock, held by $(cat ci/lock/run 2>/dev/null)"
  sleep 5
  waited=$((waited + 5))
done
"""

MAC_OWNED = r"""
[ "$(cat ci/lock/run 2>/dev/null)" = "$MAC_LOCK_NAME" ] || { echo "lost the build lock" >&2; exit 1; }
echo $$ > ci/lock/pid
"""

MAC_UNLOCK_SCRIPT = r"""
if [ "$(cat ci/lock/run 2>/dev/null)" = "$1" ]; then rm -rf ci/lock; fi
"""

MAC_DESKTOP_SOURCE_SCRIPT = r"""
set -euo pipefail
mkdir -p desktop-release
cd desktop-release
rm -rf src-* build-src-* stage-src-* dist/ForkMesh-*-macos-universal.dmg
mkdir "src-$1"
tar -x -C "src-$1"
printf '%s\n' "$2" > "src-$1.commit"
"""

MAC_DESKTOP_BUILD_SCRIPT = r"""
set -euo pipefail
cd desktop-release
FORKMESH_BUILD_COMMIT="$2" bash "src-$1/tools/package/macos_universal_on_mac.sh" "src-$1" .
"""

MAC_IOS_FETCH_SCRIPT = r"""
set -euo pipefail
mkdir -p ci
cd ci
if [ ! -d tree/.git ]; then
  rm -rf tree
  git init -q tree
fi
cd tree
git config remote.origin.url "$1"
git config remote.origin.promisor true
git config remote.origin.partialclonefilter blob:none
git config extensions.partialClone origin
git sparse-checkout set --no-cone '/native/ios/' '/codemagic.yaml' '/conformance/' \
  '/changelog/' '/tools/changelog.py' '/app/src/apns.py' '/app/src/app_runtime.py' \
  '/app/public/assets/call-segmentation/' '/marketplace/packages/vault/tests/fixtures/' \
  '/marketplace/packages/people/tests/fixtures/' '/marketplace/packages/secure-drive/tests/fixtures/' \
  '/docs/native/PERFORMANCE_CHECKS.md' '/docs/marketplace/video-mix-dj-studio-performance.md'
attempt=1
until git fetch -q --depth=1 --no-tags --filter=blob:none origin "$2" &&
    git -c advice.detachedHead=false checkout -q --force --detach FETCH_HEAD; do
  if [ "$attempt" -ge 6 ]; then
    echo "could not fetch $2 from $1" >&2
    exit 1
  fi
  echo "fetch or checkout attempt $attempt of 6 failed; retrying in 20 s" >&2
  sleep 20
  attempt=$((attempt + 1))
done
git clean -ffdxq
git log -1 --format='checked out %h: %s'
"""

MAC_IOS_STEPS_SCRIPT = r"""
set -euo pipefail
case "$1" in release/ios-?*) ;; *) echo "refusing run folder $1" >&2; exit 64 ;; esac
mkdir -p release
rm -rf "$1"
mkdir -p "$1"
ls -1dt release/ios-* 2>/dev/null | tail -n +3 | xargs rm -rf
tar -x -C "$1"
"""


@contextlib.contextmanager
def mac_lock(mac: MacHost, name: str):
    mac.run(MAC_LOCK_SCRIPT, name, str(MAC_LOCK_WAIT_SECONDS),
            timeout=MAC_LOCK_WAIT_SECONDS + 600)
    log(f"holding the office Mac's build lock as {name!r}")
    try:
        yield name
    finally:
        try:
            mac.run(MAC_UNLOCK_SCRIPT, name, timeout=120)
        except (CompletionError, subprocess.TimeoutExpired, OSError) as exc:
            log(f"could not release the Mac's build lock ({exc}); it expires after four hours")


def mac_setting(name: str, default: str) -> str:
    conf = LAYOUT.settings_file
    conf_text = conf.read_text(errors="replace") if conf.is_file() else ""
    return os.environ.get(name, "").strip() or actions_variable(conf_text, name) or default


def lan_ssh_hosts(exclude: set[str]) -> list[str]:
    route = run(["ip", "-4", "-o", "route", "get", "1.1.1.1"], check=False)
    match = re.search(r"\bsrc (\d+\.\d+\.\d+)\.(\d+)\b", route.stdout)
    if not match:
        return []
    prefix, own = match.groups()
    candidates = [f"{prefix}.{host}" for host in range(1, 255)
                  if str(host) != own and f"{prefix}.{host}" not in exclude]

    def open_ssh(address: str) -> str:
        try:
            socket.create_connection((address, 22), timeout=2).close()
            return address
        except OSError:
            return ""

    with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
        return [address for address in pool.map(open_ssh, candidates) if address]


def remember_build_mac(address: str) -> None:
    try:
        MAC_ADDRESS_FILE.parent.mkdir(parents=True, exist_ok=True)
        MAC_ADDRESS_FILE.write_text(address + "\n")
    except OSError as exc:
        log(f"could not remember the office Mac's address in {MAC_ADDRESS_FILE} ({exc})")


def find_build_mac(key_dir: Path = MAC_BUILD_KEY_DIR) -> MacHost | None:
    key, known_hosts = key_dir / "id_ed25519", key_dir / "known_hosts"
    if not key.is_file() or not known_hosts.is_file():
        log(f"no office Mac key in {key_dir} (docs/native/MAC_BUILD.md); using Codemagic")
        return None
    try:
        remembered = MAC_ADDRESS_FILE.read_text().strip()
    except OSError:
        remembered = ""
    hint = mac_setting("MAC_BUILD_HOST", MAC_BUILD_HOST)
    tried = list(dict.fromkeys(address for address in (remembered, hint) if address))
    for address in tried:
        mac = MacHost(address, key, known_hosts)
        if mac.reachable():
            remember_build_mac(address)
            return mac
    log(f"the office Mac is not at {', '.join(tried)}; scanning the LAN for its pinned host key")
    for address in lan_ssh_hosts(set(tried)):
        mac = MacHost(address, key, known_hosts)
        if mac.reachable():
            log(f"found the office Mac at {address}")
            remember_build_mac(address)
            return mac
    log("the office Mac is not on the LAN; using Codemagic")
    return None


def build_macos_on_mac(mac: MacHost, repo: Path, work: Path, tag: str,
                       build_commit: str) -> Path:
    version = tag.lstrip("v")
    dest = work / f"macos-{tag}"
    dest.mkdir(parents=True, exist_ok=True)
    build_log = dest / "mac-build.log"
    build_log.write_text("")
    dmg = dest / dmg_artefact_name(tag)
    with mac_lock(mac, f"desktop-release {tag}") as lock:
        archive = subprocess.Popen(["git", "-C", str(repo), "archive", "--format=tar",
                                    build_commit], stdout=subprocess.PIPE)
        try:
            mac.run(MAC_DESKTOP_SOURCE_SCRIPT, version, build_commit, stdin=archive.stdout,
                    timeout=1800, lock=lock)
        finally:
            archive.stdout.close()
            archive.wait()
        if archive.returncode != 0:
            raise CompletionError(f"git archive {build_commit[:12]} failed ({archive.returncode})")
        log(f"building the universal macOS image for {tag} on the office Mac "
            f"({mac.address}); log: {build_log}")
        mac.run(MAC_DESKTOP_BUILD_SCRIPT, version, build_commit, log_file=build_log,
                timeout=MAC_DMG_TIMEOUT_SECONDS, lock=lock)
        digest, remote = mac_dmg_result(build_log.read_text(errors="replace"))
        mac.run('cat "$1"', remote, output=dmg, timeout=1800)
    if sha256_file(dmg) != digest:
        raise CompletionError(f"{dmg.name} changed on its way from the Mac")
    log(f"the office Mac built {dmg.name} ({dmg.stat().st_size} bytes, sha256 {digest[:12]})")
    return dmg


def ios_steps(repo: Path, commit: str, dest: Path, overrides: dict[str, str]) -> None:
    import yaml

    config_text = run(["git", "-C", repo, "show", f"{commit}:codemagic.yaml"]).stdout
    workflow = (yaml.safe_load(config_text).get("workflows") or {}).get(CODEMAGIC_IOS_WORKFLOW)
    if not workflow:
        raise CompletionError(f"codemagic.yaml at {commit[:12]} has no {CODEMAGIC_IOS_WORKFLOW}")
    names = [step.get("name", "") for step in workflow.get("scripts") or []
             if isinstance(step, dict)]
    if "Submit to the App Store" not in names:
        raise CompletionError(f"{CODEMAGIC_IOS_WORKFLOW} at {commit[:12]} cannot submit to "
                              "the App Store")
    watch = str(((workflow.get("environment") or {}).get("vars") or {})
                .get("FORKMESH_NATIVE_WATCH", "1"))
    with tempfile.TemporaryDirectory(prefix="codemagic-") as scratch:
        config = Path(scratch) / "codemagic.yaml"
        config.write_text(config_text)
        helper = Path(scratch) / "codemagic_steps.py"
        helper.write_text(run(["git", "-C", repo, "show",
                               f"{commit}:native/ios/tools/codemagic_steps.py"]).stdout)
        skip = ["--skip", "watchOS platform"] if watch == "0" else []
        run([sys.executable, helper, CODEMAGIC_IOS_WORKFLOW, dest, "--config", config, *skip])
    with (dest / "env.sh").open("a") as env:
        for key, value in overrides.items():
            env.write(f"export {key}={shlex.quote(value)}\n")


def release_ios_on_mac(mac: MacHost, repo: Path, work: Path, tag: str, commit: str,
                       notes: str) -> dict:
    run_dir = f"release/ios-{tag}"
    build_log = work / f"ios-{tag}.log"
    build_log.write_text("")
    with mac_lock(mac, f"ios-release {tag}") as lock:
        fetched = mac.run(MAC_IOS_FETCH_SCRIPT, mac_setting("MAC_BUILD_GIT_URL", MAC_BUILD_GIT_URL),
                          commit, timeout=600, lock=lock)
        log(f"office Mac {fetched.strip()}")
        with tempfile.TemporaryDirectory(dir=work, prefix="ios-steps-") as scratch:
            ios_steps(repo, commit, Path(scratch) / "steps", {
                "FORKMESH_IOS_APP_STORE": "1",
                "FORKMESH_IOS_WHATS_NEW": notes,
                "TESTFLIGHT_WHATS_NEW": notes,
            })
            packed = subprocess.Popen(["tar", "-C", scratch, "-c", "steps"],
                                      stdout=subprocess.PIPE)
            try:
                mac.run(MAC_IOS_STEPS_SCRIPT, run_dir, stdin=packed.stdout, timeout=300,
                        lock=lock)
            finally:
                packed.stdout.close()
                packed.wait()
        log(f"building and submitting the iOS app for {tag} on the office Mac "
            f"({mac.address}); log: {build_log}")
        mac.run('bash ci/tree/native/ios/tools/mac_adhoc_build.sh "$1/steps" ci/tree "$1/out"',
                run_dir, log_file=build_log, timeout=MAC_IOS_TIMEOUT_SECONDS, lock=lock)
        values = cm_env_values(mac.run('cat "$1/steps/cm_env"', run_dir, timeout=60))
    if IOS_SUBMITTED not in build_log.read_text(errors="replace"):
        raise CompletionError(f"the iOS build for {tag} finished without an App Store "
                              f"submission (see {build_log})")
    return {"route": "mac", "version": values.get("FORKMESH_IOS_VERSION", ""),
            "build": values.get("FORKMESH_IOS_BUILD", "")}


def release_notes(repo: Path, tag: str) -> str:
    raw = git(repo, "tag", "-l", "--format=%(contents)", tag, check=False)
    return release_notes_text(raw, tag)


def fetch_homepage_manifest(url: str = HOMEPAGE_MANIFEST_URL) -> dict:
    request = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
    with urllib.request.urlopen(request, timeout=60) as response:
        parsed = json.loads(response.read())
    return parsed if isinstance(parsed, dict) else {}


def manifests_at(repo: Path, ref: str) -> dict[str, dict]:
    manifests = {}
    listing = run(["git", "-C", repo, "ls-tree", "--name-only", f"{ref}:{RELEASES}"],
                  check=False)
    for name in listing.stdout.split():
        shown = run(["git", "-C", repo, "show", f"{ref}:{RELEASES}/{name}/release.json"],
                    check=False)
        if shown.returncode != 0:
            continue
        try:
            manifest = json.loads(shown.stdout)
        except ValueError:
            continue
        if isinstance(manifest, dict):
            manifests[name] = manifest
    return manifests


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def release_signing_key(dest: Path, installer_key: str) -> None:
    """Write the Ed25519 release key that matches the installer's anchor, 0600."""
    explicit = os.environ.get("FORKMESH_RELEASE_SIGNING_KEY", "").strip()
    candidates = []
    if explicit:
        candidates.append(Path(explicit).read_text())
    conf = LAYOUT.settings_file
    if conf.is_file():
        text = conf.read_text(errors="replace")
        for raw in re.findall(
            r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?-----END [A-Z ]*PRIVATE KEY-----",
            text, re.S,
        ):
            begin = re.match(r"-----BEGIN [A-Z ]*PRIVATE KEY-----", raw).group(0)
            end = re.search(r"-----END [A-Z ]*PRIVATE KEY-----$", raw).group(0)
            # The app's variables blob stores PEM newlines as (doubly) escaped \n.
            body = re.sub(r"[^A-Za-z0-9+/=]", "",
                          re.sub(r"\\+n", "", raw[len(begin):-len(end)]))
            candidates.append(
                begin + "\n" + "\n".join(body[i:i + 64] for i in range(0, len(body), 64))
                + "\n" + end + "\n"
            )
    for pem in candidates:
        public = subprocess.run(["openssl", "pkey", "-pubout"], input=pem,
                                capture_output=True, text=True, check=False)
        lines = public.stdout.splitlines()
        if public.returncode == 0 and len(lines) >= 2 and lines[1] == installer_key:
            fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as handle:
                handle.write(pem)
            return
    raise ConfigError("no release signing key matches the installer's public key "
                      "(set FORKMESH_RELEASE_SIGNING_KEY or store "
                      "FORKMESH_RELEASE_SIGNING_KEY_PEM in the app variables)")


def remove_worktree(repo: Path, tree: Path) -> None:
    if tree.exists():
        run(["git", "-C", repo, "worktree", "remove", "--force", tree], check=False)
        forkmesh_safe_delete.remove_tree(tree, tree.parent, worktree=True, ignore_errors=True)
    run(["git", "-C", repo, "worktree", "prune"], check=False)


def discard_relocated_build_tree(build_dir: Path, source_dir: Path) -> bool:
    """Drop a CMake build tree that was configured somewhere else.

    CMake records absolute paths: the source folder in CMakeCache.txt and the
    toolchain file in CMakeSystem.cmake. When the checkout or the work folder
    has moved since (the projects layout moved both), configure fails on the
    stale include before it can correct anything, and every later run repeats
    it. A build tree is disposable, so a relocated one starts over.
    """
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return False
    stale = ""
    recorded = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$",
                         cache.read_text(errors="replace"), re.M)
    if recorded and Path(recorded.group(1).strip()) != source_dir:
        stale = f"it was configured for {recorded.group(1).strip()}"
    for system in build_dir.glob("CMakeFiles/*/CMakeSystem.cmake"):
        for included in re.findall(r'^include\("([^"]+)"\)', system.read_text(errors="replace"),
                                   re.M):
            if not Path(included).is_file():
                stale = f"its toolchain file {included} no longer exists"
    if not stale:
        return False
    log(f"discarding build tree {build_dir}: {stale}")
    forkmesh_safe_delete.remove_tree(build_dir, build_dir.parent, ignore_errors=True)
    return True


def build_tree(repo: Path, work: Path, label: str, tag: str, commit: str) -> Path:
    """A pristine detached worktree at the release's build commit."""
    tree = work / f"{label}-{tag}"
    if tree.exists() and git(tree, "rev-parse", "HEAD", check=False) != commit:
        remove_worktree(repo, tree)
    if not tree.exists():
        git(repo, "worktree", "add", "--detach", str(tree), commit)
    if git(tree, "status", "--porcelain", "--untracked-files=no"):
        raise CompletionError(f"build tree {tree} has tracked changes")
    return tree


def run_build(script: str, tree: Path, env: dict, build_log: Path,
              timeout: int) -> str:
    with build_log.open("w") as handle:
        result = subprocess.run(["bash", "-c", script], cwd=tree, env=env,
                                stdout=handle, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False)
    output = build_log.read_text(errors="replace")
    if result.returncode != 0:
        raise CompletionError(f"build failed ({result.returncode}): "
                              f"{output.strip()[-1500:]}")
    return output


BUILD_APK_SCRIPT = r"""
set -euo pipefail
. tools/android-toolchain.sh >/dev/null
if [ "${ANDROID_TOOLCHAIN_READY:-0}" != 1 ]; then
  echo "Android toolchain unavailable: ${ANDROID_TOOLCHAIN_REASON:-unknown}" >&2
  exit 1
fi
if [ ! -x native/android/gradlew ]; then
  echo "this release has no native Android app (native/android/gradlew)" >&2
  exit 1
fi
changelog=""
notes="$(mktemp "${TMPDIR:-/tmp}/forkmesh-changelog-XXXXXX")"
trap 'rm -f "$notes"' EXIT
if [ -f tools/changelog.py ] && python3 tools/changelog.py export --platform android --out "$notes"; then
  changelog="$notes"
else
  echo "No changelog for this APK: it shows its version with no notes." >&2
fi
cd native/android
nice -n 10 ../../tools/loopback-netns.sh ./gradlew --no-daemon --console=plain \
  -Dorg.gradle.jvmargs="-Xmx4g -XX:MaxMetaspaceSize=1g -Dfile.encoding=UTF-8" \
  -Pkotlin.compiler.execution.strategy=in-process \
  -Pforkmesh.appId="$FORKMESH_ANDROID_APPLICATION_ID" \
  -Pforkmesh.versionCode="$FORKMESH_ANDROID_VERSION_CODE" \
  -Pforkmesh.versionName="$FORKMESH_ANDROID_VERSION_NAME" \
  ${changelog:+-Pforkmesh.changelog="$changelog"} \
  :app:assembleRelease
apk=app/build/outputs/apk/release/app-release.apk
bt="$(ls -d "$ANDROID_HOME"/build-tools/*/ | sort -V | tail -n1)"
"${bt}apksigner" verify --print-certs "$apk" | sed 's/^/FORKMESH_APK_CERT /'
echo "FORKMESH_APK_BADGING $("${bt}aapt2" dump badging "$apk" | head -n1)"
"""


# apksigner names the signer differently across build-tools releases: 36 and
# older print "Signer #1 certificate SHA-256 digest: <hex>", 37 prints
# "V2 Signer: certificate SHA-256 digest: <hex>". Matching only the old form
# read an empty signer from a correctly signed APK and held v1.2.0 and v1.2.1
# back from promotion, which also stopped their mirror rollout.
APK_SIGNER_DIGEST = re.compile(
    r"^FORKMESH_APK_CERT (?:Signer #\d+|V\d+(?:\.\d+)? Signer:) "
    r"certificate SHA-256 digest: ([0-9A-Fa-f]{64})\s*$", re.M)


def apk_signer_sha256(output: str) -> str:
    """The one certificate digest apksigner reported, or "" for none or several."""
    digests = {digest.lower() for digest in APK_SIGNER_DIGEST.findall(output)}
    return digests.pop() if len(digests) == 1 else ""


APK_BADGING = re.compile(
    r"^FORKMESH_APK_BADGING package: name='([^']+)' versionCode='(\d+)'", re.M)


def apk_badging(output: str) -> tuple[str, int] | None:
    match = APK_BADGING.search(output)
    return (match.group(1), int(match.group(2))) if match else None


def android_version_name(tag: str) -> str:
    version = parse_version(tag)
    if version is None:
        raise CompletionError(f"release tag {tag!r} is not vMAJOR.MINOR.PATCH")
    return ".".join(str(part) for part in version)


def build_apk(repo: Path, work: Path, tag: str, commit: str, version_code: int,
              signer_sha256: str) -> Path:
    release_dir = FORKMESH_DATA / "release"
    keystore = Path(os.environ.get(
        "FORKMESH_ANDROID_KEYSTORE", release_dir / "android-direct-distribution-v3.jks"))
    password_file = Path(os.environ.get(
        "FORKMESH_ANDROID_KEYSTORE_PASSWORD_FILE", release_dir / ".apk-keystore-password"))
    if not keystore.is_file() or not password_file.is_file():
        raise ConfigError(f"Android keystore or its password file is missing under {release_dir}")
    password = password_file.read_text().strip()
    tree = build_tree(repo, work, "apk", tag, commit)
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env = dict(
        os.environ,
        FORKMESH_ANDROID_KEYSTORE=str(keystore),
        FORKMESH_ANDROID_KEYSTORE_PASSWORD=password,
        FORKMESH_ANDROID_KEY_ALIAS=os.environ.get("FORKMESH_ANDROID_KEY_ALIAS", "forkmesh"),
        FORKMESH_ANDROID_KEY_PASSWORD=password,
        FORKMESH_ANDROID_VERSION_CODE=str(version_code),
        FORKMESH_ANDROID_VERSION_NAME=android_version_name(tag),
        FORKMESH_ANDROID_APPLICATION_ID=APK_APPLICATION_ID,
        TMPDIR=str(tmp),
    )
    build_log = work / f"apk-{tag}.log"
    log(f"building the signed Android APK for {tag} (versionCode {version_code}); "
        f"log: {build_log}")
    output = run_build(BUILD_APK_SCRIPT, tree, env, build_log, 60 * 60)
    signer = apk_signer_sha256(output)
    badging = apk_badging(output)
    if signer != signer_sha256.lower():
        raise CompletionError(f"APK is not signed by {signer_sha256[:12]}...: "
                              f"{signer or 'no single signer'}")
    if not badging or badging[0] != APK_APPLICATION_ID:
        raise CompletionError(f"APK package is {badging[0] if badging else '?'}, "
                              f"expected {APK_APPLICATION_ID}")
    if badging[1] != version_code:
        raise CompletionError(f"APK versionCode is {badging[1]}, expected {version_code}")
    apk = tree / APK_OUTPUT
    if not apk.is_file():
        raise CompletionError(f"APK was not produced at {apk}")
    return apk


# The packaging tooling (toolchain locator, CMake toolchain file, packager)
# comes from this tool's own checkout: releases cut before it existed have no
# copy at their build commit, and packaging is build infrastructure, not the
# provenance-stamped product source that the pristine tree supplies.
BUILD_WINDOWS_SCRIPT = r"""
set -euo pipefail
tools="$FORKMESH_HOST_TOOLS_ROOT"
. "$tools/tools/package/windows-toolchain.sh"
if [ "${FORKMESH_WIN_TOOLCHAIN_READY:-0}" != 1 ]; then
  echo "Windows toolchain unavailable: ${FORKMESH_WIN_TOOLCHAIN_REASON:-unknown}" >&2
  exit 1
fi
build="${FORKMESH_WIN_BUILD_DIR:?}"
# FindOpenSSL caches what it found, so a tree configured against another
# OpenSSL (the Qt tools kit's 3.0, before the toolchain cross-built its own)
# would keep compiling against it: start that tree over.
if [ -f "$build/CMakeCache.txt" ] \
   && ! grep -qxF "OPENSSL_ROOT_DIR:PATH=$FORKMESH_WIN_OPENSSL_ROOT" "$build/CMakeCache.txt"; then
  echo "discarding $build: configured for another OpenSSL" >&2
  rm -rf "$build"
fi
launcher=""
command -v ccache >/dev/null 2>&1 && launcher=-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake -S desktop -B "$build" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$tools/desktop/cmake/mingw-w64-x86_64.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=OFF -DFORKMESH_REQUIRE_OPENSSL_3_5=ON \
  -DFORKMESH_BUILD_COMMIT_OVERRIDE="$FORKMESH_WIN_BUILD_COMMIT" \
  -DFORKMESH_VERSION_OVERRIDE="$FORKMESH_WIN_VERSION" $launcher
nice -n 10 cmake --build "$build" -j"${FORKMESH_WIN_JOBS:-4}"
rm -rf "$build/install"
cmake --install "$build" --prefix "$build/install"
[ -x "$build/install/bin/forkmesh.exe" ] || { echo "no forkmesh.exe was installed" >&2; exit 1; }
# Windows binaries cannot run here, so prove the provenance stamp by content:
# the header bakes the exact build commit into the executable as a literal.
grep -q -a "$FORKMESH_WIN_BUILD_COMMIT" "$build/install/bin/forkmesh.exe" \
  || { echo "forkmesh.exe does not carry build commit $FORKMESH_WIN_BUILD_COMMIT" >&2; exit 1; }
export FORKMESH_REQUIRE_NATIVE_SIGNING=1
export FORKMESH_WIN_CXX="$FORKMESH_WIN_MINGW_ROOT/usr/bin/x86_64-w64-mingw32-g++-posix"
installer="$("$tools/tools/package/package-release.sh" \
  --os windows --arch x86_64 \
  --binary "$build/install/bin/forkmesh.exe" \
  --install-root "$build/install" \
  --channel "$FORKMESH_WIN_CHANNEL" --tag "$FORKMESH_WIN_TAG" \
  --outdir "$build" | tail -n1)"
[ -s "$installer" ] || { echo "no installer was produced" >&2; exit 1; }
echo "FORKMESH_WIN_INSTALLER $installer"
"""


def windows_signer_sha256(installer: Path, scratch: Path) -> str:
    """SHA-256 fingerprint of the certificate that signed an Authenticode PE."""
    pem = scratch / "signature.pem"
    toolchain = Path(os.environ.get("FORKMESH_WIN_TOOLCHAIN", LAYOUT.windows_toolchain))
    osslsigncode = shutil.which("osslsigncode") or str(toolchain / "sysroot/usr/bin/osslsigncode")
    # The build dir is reused across attempts, and osslsigncode refuses to
    # overwrite: a previous run's signature.pem failed every later check.
    pem.unlink(missing_ok=True)
    run([osslsigncode, "extract-signature", "-pem", "-in", installer, "-out", pem])
    certs = run(["openssl", "pkcs7", "-print_certs", "-in", pem]).stdout
    first = re.search(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                      certs, re.S)
    if not first:
        raise CompletionError(f"{installer.name} carries no signing certificate")
    fingerprint = subprocess.run(
        ["openssl", "x509", "-noout", "-fingerprint", "-sha256"],
        input=first.group(0), capture_output=True, text=True, check=False).stdout
    match = re.search(r"Fingerprint=([0-9A-Fa-f:]+)", fingerprint)
    if not match:
        raise CompletionError(f"could not fingerprint the signer of {installer.name}")
    return match.group(1).replace(":", "").lower()


def build_windows_installer(repo: Path, work: Path, tag: str, commit: str,
                            channel: str, signer_sha256: str) -> Path:
    release_dir = FORKMESH_DATA / "release"
    pfx = Path(os.environ.get(
        "FORKMESH_WIN_CERT_PFX_FILE", release_dir / "windows-authenticode.pfx"))
    password_file = Path(os.environ.get(
        "FORKMESH_WIN_CERT_PASSWORD_FILE", release_dir / ".windows-cert-password"))
    if not pfx.is_file() or not password_file.is_file():
        raise ConfigError("the Windows Authenticode certificate or its password "
                          f"file is missing under {release_dir}")
    tree = build_tree(repo, work, "win", tag, commit)
    build_dir = work / f"win-build-{tag}"
    discard_relocated_build_tree(build_dir, tree / "desktop")
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    jobs = max(1, min(4, (os.cpu_count() or 4) // 2))
    env = dict(
        os.environ,
        FORKMESH_HOST_TOOLS_ROOT=str(ROOT),
        FORKMESH_WIN_BUILD_DIR=str(build_dir),
        FORKMESH_WIN_BUILD_COMMIT=commit,
        FORKMESH_WIN_VERSION=tag.lstrip("v"),
        FORKMESH_WIN_TAG=tag,
        FORKMESH_WIN_CHANNEL=channel,
        FORKMESH_WIN_JOBS=str(jobs),
        FORKMESH_WIN_CERT_PFX=base64.b64encode(pfx.read_bytes()).decode("ascii"),
        FORKMESH_WIN_CERT_PASSWORD=password_file.read_text().strip(),
        TMPDIR=str(tmp),
    )
    build_log = work / f"win-{tag}.log"
    log(f"cross-building the signed Windows installer for {tag}; log: {build_log}")
    output = run_build(BUILD_WINDOWS_SCRIPT, tree, env, build_log, 3 * 60 * 60)
    produced = re.search(r"^FORKMESH_WIN_INSTALLER (.+)$", output, re.M)
    installer = Path(produced.group(1).strip()) if produced else None
    if installer is None or not installer.is_file() or installer.name != WINDOWS_ASSET:
        raise CompletionError(f"{WINDOWS_ASSET} was not produced")
    signer = windows_signer_sha256(installer, build_dir)
    if signer != signer_sha256.lower():
        raise CompletionError(f"{WINDOWS_ASSET} is not signed by {signer_sha256[:12]}...: "
                              f"{signer}")
    return installer


BUILD_LINUX_SCRIPT = r"""
set -euo pipefail
"$FORKMESH_LINUX_DRIVER" build --source . --out "$FORKMESH_LINUX_OUT" \
  --version "$FORKMESH_LINUX_VERSION" --commit "$FORKMESH_LINUX_COMMIT" \
  --channel "$FORKMESH_LINUX_CHANNEL"
"$FORKMESH_LINUX_DRIVER" smoke --out "$FORKMESH_LINUX_OUT" --version "$FORKMESH_LINUX_VERSION"
"""


def build_linux_assets(repo: Path, work: Path, tag: str, commit: str,
                       channel: str) -> dict[str, Path]:
    tree = build_tree(repo, work, "linux", tag, commit)
    out = work / f"linux-out-{tag}"
    forkmesh_safe_delete.remove_tree(out, work, ignore_errors=True)
    out.mkdir(parents=True)
    driver = ROOT / "tools" / "package" / "linux-baseline.sh"
    version = tag.lstrip("v")
    tmp = work / "tmp"
    tmp.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, TMPDIR=str(tmp))
    for name in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
        env.pop(name, None)
    env["FORKMESH_LINUX_BASELINE_CACHE"] = os.environ.get(
        "FORKMESH_LINUX_BASELINE_CACHE", str(LAYOUT.home_dir / "toolchain-linux"))
    build_log = work / f"linux-{tag}.log"
    log(f"building the Linux binary and AppImage for {tag} in the Debian 12 baseline "
        f"container; log: {build_log}")
    env.update(FORKMESH_LINUX_DRIVER=str(driver), FORKMESH_LINUX_OUT=str(out),
               FORKMESH_LINUX_VERSION=version, FORKMESH_LINUX_COMMIT=commit,
               FORKMESH_LINUX_CHANNEL=channel)
    run_build(BUILD_LINUX_SCRIPT, tree, env, build_log, 4 * 60 * 60)
    built = {name: out / name for name in (LINUX_ASSET, APPIMAGE_ASSET, ZSYNC_ASSET)}
    for name, path in built.items():
        if not path.is_file() or path.stat().st_size == 0:
            raise CompletionError(f"{name} was not produced by the Linux baseline build")
    return built


SEED_REMOTE = r"""
set -e
D="$BASE/sha256/${H:0:2}/$H"
if [ -f "$D/data" ] && [ "$(sha256sum < "$D/data" | cut -d' ' -f1)" = "$H" ]; then
  cat >/dev/null; echo present; exit 0
fi
install -d -o forkmesh-node -g forkmesh-node -m 0755 "$BASE" "$BASE/sha256" "$BASE/sha256/${H:0:2}" "$D"
cat > "$D/data.tmp"
if [ "$(sha256sum < "$D/data.tmp" | cut -d' ' -f1)" != "$H" ]; then
  rm -f "$D/data.tmp"; echo mismatch; exit 1
fi
chown forkmesh-node:forkmesh-node "$D/data.tmp"; chmod 0644 "$D/data.tmp"; mv "$D/data.tmp" "$D/data"
echo seeded
"""


def seed_targets(hosts: list[tuple[str, str]], ssh_key: Path,
                 known_hosts: Path | None = None) -> list[tuple[str, str, list[str]]]:
    options = ["-i", str(ssh_key), "-o", "IdentitiesOnly=yes"]
    if known_hosts is not None:
        options += ["-o", "StrictHostKeyChecking=yes",
                    "-o", f"UserKnownHostsFile={known_hosts}"]
    return [(name, ip, options) for name, ip in hosts]


def seed_mirrors(cas: Path, hashes: list[str],
                 hosts: list[tuple[str, str, list[str]]], remote_repo: str) -> None:
    for digest in hashes:
        if not HASH_RE.match(digest):
            raise CompletionError(f"invalid asset hash {digest!r}")
        if not (cas / "sha256" / digest[:2] / digest / "data").is_file():
            raise CompletionError(f"local release store lacks blob {digest[:12]}")
    base = f"{remote_repo}/forkmesh-releases"
    # The blob travels on stdin, so the remote script rides in the command
    # line: base64 keeps its newlines intact through the remote login shell
    # (a JSON-quoted script arrives as one line with literal "\n" and breaks).
    script = base64.b64encode(SEED_REMOTE.encode("utf-8")).decode("ascii")
    remote = f'BASE={base} H={{digest}} bash -c "$(printf %s {script} | base64 -d)"'

    def seed(host: tuple[str, str, list[str]]) -> str:
        name, ip, options = host
        for digest in hashes:
            src = cas / "sha256" / digest[:2] / digest / "data"
            with src.open("rb") as handle:
                result = subprocess.run(
                    ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options,
                     f"root@{ip}", remote.format(digest=digest)],
                    stdin=handle, capture_output=True, text=True, timeout=30 * 60, check=False)
            if result.returncode != 0:
                raise CompletionError(f"{name}: blob {digest[:12]} did not seed: "
                                      f"{(result.stderr or result.stdout).strip()[-300:]}")
        return name

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(hosts))) as pool:
        for name in pool.map(seed, hosts):
            log(f"{name}: every release blob present and hash-verified")


ROLLOUT_REMOTE = r"""
set -eu
unit=/etc/systemd/system/forkmesh-mirror-node.service
bin=/usr/local/bin/forkmesh-mirror-node
api=http://127.0.0.1:8791
[ "$(id -u)" = 0 ] || { echo 'rollout: native mirror updates require root' >&2; exit 77; }
if [ -f "$unit" ] && grep -q '^# Managed by deploy/core/render.py' "$unit"; then
  echo 'rollout: next-managed'; exit 0
fi
[ -f "$unit" ] && [ ! -L "$unit" ] && [ "$(stat -c %u "$unit")" = 0 ] && [ -x "$bin" ] && [ ! -L "$bin" ] || {
  echo 'rollout: the managed native mirror service is missing or unsafe' >&2; exit 76; }
grep -Fqx 'User=forkmesh-node' "$unit" && grep -Fqx 'Group=forkmesh-node' "$unit" && id forkmesh-node >/dev/null 2>&1 || {
  echo 'rollout: the managed mirror service account is invalid' >&2; exit 76; }
if grep -Fqx "ExecStart=$bin --config /etc/forkmesh/mirror-node.json" "$unit"; then
  config=/etc/forkmesh/mirror-node.json
elif grep -Fqx "ExecStart=$bin --config /var/lib/forkmesh/mirror-node.json" "$unit"; then
  config=/var/lib/forkmesh/mirror-node.json
else
  echo 'rollout: the mirror service command is not controller managed' >&2; exit 76
fi
grep -Fqx "ExecStartPre=$bin --config $config --check" "$unit" && [ -f "$config" ] && [ ! -L "$config" ] || {
  echo 'rollout: the managed mirror configuration is unsafe' >&2; exit 76; }
case "$(uname -m)" in
  x86_64|amd64) expected="$X86" ;;
  aarch64|arm64) expected="$ARM" ;;
  *) echo "rollout: no native mirror-node release for $(uname -m)" >&2; exit 75 ;;
esac
[ -n "$expected" ] || { echo "rollout: the release has no mirror-node asset for $(uname -m)" >&2; exit 75; }
digest() { sha256sum "$1" | cut -d' ' -f1; }
reports() { curl -fsS --connect-timeout 1 --max-time 3 "$api/v1/status" 2>/dev/null | grep -Fq "\"version\":\"$VERSION\""; }
await_ready() {
  n=0
  while [ "$n" -lt "$READY_WAIT" ]; do
    curl -fsS --connect-timeout 1 --max-time 3 -o /dev/null "$api/readyz" 2>/dev/null && return 0
    n=$((n + 2)); sleep 2
  done
  return 1
}
if [ "$(digest "$bin")" = "$expected" ] && reports; then
  await_ready || { echo "rollout: $VERSION is installed but $api/readyz did not pass within ${READY_WAIT}s" >&2; exit 71; }
  echo "rollout: current $VERSION"; exit 0
fi
confdir="$(dirname "$config")"
runuser -u forkmesh-node -- test -w "$confdir" || {
  echo "rollout: $confdir is not writable by forkmesh-node, so --set-version would fail after the swap; chown forkmesh-node:forkmesh-node $confdir" >&2
  exit 73; }
previous="$(python3 -I -c 'import json, sys; print(json.load(open(sys.argv[1])).get("version", ""))' "$config" 2>/dev/null || true)"
candidate="$(mktemp /usr/local/bin/.forkmesh-mirror-node.XXXXXX)" || exit 70
trap 'rm -f -- "$candidate"' EXIT HUP INT TERM
seeded="$STORE/sha256/$(printf %s "$expected" | cut -c1-2)/$expected/data"
if [ -f "$seeded" ] && cp -- "$seeded" "$candidate" && [ "$(digest "$candidate")" = "$expected" ]; then
  source=store
else
  curl -fsSL --connect-timeout 20 --max-time 180 -H 'Cache-Control: no-cache' "$BLOB_BASE$expected" -o "$candidate" || {
    echo 'rollout: mirror-node download failed' >&2; exit 70; }
  source=download
fi
[ "$(digest "$candidate")" = "$expected" ] || { echo 'rollout: mirror-node checksum verification failed' >&2; exit 74; }
chown root:root "$candidate"; chmod 0755 "$candidate"
cp -p -- "$bin" "$bin.bak-update"
restore() {
  mv -f -- "$bin.bak-update" "$bin"
  if [ -n "$previous" ]; then
    runuser -u forkmesh-node -- "$bin" --config "$config" --set-version "$previous" >/dev/null 2>&1 || true
  fi
}
mv -f -- "$candidate" "$bin"
[ "$(digest "$bin")" = "$expected" ] || { restore; echo 'rollout: the installed binary does not match' >&2; exit 74; }
runuser -u forkmesh-node -- "$bin" --config "$config" --set-version "$VERSION" >/dev/null || {
  restore; echo 'rollout: --set-version failed; the previous binary is back in place' >&2; exit 73; }
systemctl restart forkmesh-mirror-node.service
up=0; n=0
while [ "$n" -lt 30 ]; do
  if curl -fsS --connect-timeout 1 --max-time 2 -o /dev/null "$api/healthz" 2>/dev/null && reports; then up=1; break; fi
  n=$((n + 1)); sleep 1
done
if [ "$up" != 1 ]; then
  restore; systemctl restart forkmesh-mirror-node.service || true
  echo "rollout: $VERSION did not come up healthy; rolled back to ${previous:-the previous binary}" >&2; exit 72
fi
await_ready || { echo "rollout: $VERSION is running but $api/readyz did not pass within ${READY_WAIT}s" >&2; exit 71; }
echo "rollout: deployed $VERSION sha256 $expected from $source"
"""


def rollout_release(manifest: dict | None) -> dict[str, str]:
    if not isinstance(manifest, dict) or manifest.get("schema") != "forkmesh-release-v2":
        raise CompletionError("latest is not a forkmesh-release-v2 manifest")
    tag = str(manifest.get("tag", "")).strip()
    if not tag.startswith("v") or not VERSION_RE.match(tag):
        raise CompletionError(f"latest names an unusable tag {tag!r}")
    digests: dict[str, str] = {}
    for asset in manifest.get("assets") or []:
        if not isinstance(asset, dict):
            raise CompletionError("latest has an invalid asset entry")
        name = str(asset.get("name", "")).strip()
        if name not in MIRROR_NODE_ASSETS.values():
            continue
        digest = str(asset.get("blob_sha256", "")).strip().lower()
        if not HASH_RE.match(digest):
            raise CompletionError(f"latest has an invalid {name} digest")
        if digests.setdefault(name, digest) != digest:
            raise CompletionError(f"latest assigns conflicting blobs to {name}")
    if not digests:
        raise CompletionError(f"{tag} has no Linux mirror-node asset to roll out")
    return {"tag": tag, "version": tag[1:],
            "x86_64": digests.get(MIRROR_NODE_ASSETS["x86_64"], ""),
            "arm64": digests.get(MIRROR_NODE_ASSETS["arm64"], "")}


def rollout_hosts(hosts: list[tuple[str, str]]) -> list[tuple[str, str]]:
    return [(name, ip) for name, ip in hosts if not name.startswith(NEXT_HOST_PREFIX)]


def rollout_remote_command(release: dict[str, str], blob_base: str, store: str,
                           ready_wait: int = ROLLOUT_READY_WAIT_SECONDS) -> str:
    script = base64.b64encode(ROLLOUT_REMOTE.encode("utf-8")).decode("ascii")
    env = {"X86": release["x86_64"], "ARM": release["arm64"], "VERSION": release["version"],
           "BLOB_BASE": blob_base, "STORE": store, "READY_WAIT": str(int(ready_wait))}
    assignments = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    return f'{assignments} bash -c "$(printf %s {script} | base64 -d)"'


def rollout_outcome(returncode: int, stdout: str, stderr: str) -> tuple[str, str]:
    lines = [line.strip() for line in (stdout or "").splitlines() if line.strip()]
    last = lines[-1] if lines else ""
    if returncode == 0 and last.startswith("rollout: "):
        words = last[len("rollout: "):].split()
        if words and words[0] in ROLLOUT_SETTLED:
            return words[0], last[len("rollout: "):]
    detail = (stderr or stdout or "").strip()[-300:] or "no output"
    return "failed", f"exit {returncode}: {detail}"


def load_rollout_state(path: Path, tag: str) -> dict:
    try:
        state = json.loads(path.read_text())
    except (OSError, ValueError):
        state = None
    if not isinstance(state, dict) or state.get("tag") != tag:
        state = {"tag": tag, "attempts": 0, "last_attempt": 0, "hosts": {}}
    if not isinstance(state.get("hosts"), dict):
        state["hosts"] = {}
    return state


def rollout_targets(state: dict, hosts: list[tuple[str, str]]) -> list[tuple[str, str]]:
    recorded = state.get("hosts", {})
    return [(name, ip) for name, ip in hosts
            if not (isinstance(recorded.get(name), dict)
                    and recorded[name].get("ip") == ip
                    and recorded[name].get("status") in ROLLOUT_SETTLED)]


def rollout_due(state: dict, now: float, forced: bool = False,
                backoff: float = FAILURE_BACKOFF_SECONDS,
                max_attempts: int = ROLLOUT_MAX_ATTEMPTS) -> str:
    if forced:
        return ""
    attempts = int(state.get("attempts", 0) or 0)
    if attempts >= max_attempts:
        return f"gave up after {attempts} failed attempts"
    if attempts and now - float(state.get("last_attempt", 0) or 0) < backoff:
        return "failed recently"
    return ""


def record_rollout(state: dict, results: dict[str, tuple[str, str, str]],
                   now: float) -> dict:
    hosts = dict(state.get("hosts", {}))
    for name, (ip, status, detail) in results.items():
        hosts[name] = {"ip": ip, "status": status, "detail": detail, "at": int(now)}
    failed = any(status not in ROLLOUT_SETTLED for _, status, _ in results.values())
    attempts = int(state.get("attempts", 0) or 0) + 1 if failed else 0
    return {**state, "hosts": hosts, "attempts": attempts, "last_attempt": int(now)}


def roll_out_hosts(targets: list[tuple[str, str, list[str]]], remote: str,
                   runner=subprocess.run) -> dict[str, tuple[str, str, str]]:
    def roll(host: tuple[str, str, list[str]]) -> tuple[str, tuple[str, str, str]]:
        name, ip, options = host
        try:
            result = runner(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", *options,
                             f"root@{ip}", remote],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True,
                            timeout=ROLLOUT_HOST_TIMEOUT_SECONDS, check=False)
        except (subprocess.TimeoutExpired, OSError) as exc:
            return name, (ip, "failed", str(exc)[-300:])
        status, detail = rollout_outcome(result.returncode, result.stdout, result.stderr)
        return name, (ip, status, detail)

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(targets))) as pool:
        return dict(pool.map(roll, targets))


def move_tag(repo: Path, tag: str, target: str) -> None:
    kind = git(repo, "for-each-ref", "--format=%(objecttype)", f"refs/tags/{tag}")
    if kind == "tag":
        message = git(repo, "for-each-ref",
                      "--format=%(contents:subject)%0a%0a%(contents:body)", f"refs/tags/{tag}")
        message = re.split(r"-----BEGIN [A-Z ]*SIGNATURE-----", message)[0].strip() or tag
        git(repo, "-c", "user.name=ForkMesh Actions", "-c", "user.email=actions@forkmesh.local",
            "tag", "-f", "-a", tag, "-m", message, target)
    else:
        git(repo, "tag", "-f", tag, target)


def land_on_main(repo: Path, branch: str, message: str, attempts: int = 60) -> None:
    for _ in range(attempts):
        if (repo / ".git" / "index.lock").exists():
            time.sleep(10)
            continue
        if git(repo, "rev-parse", "--abbrev-ref", "HEAD") != "main":
            raise CompletionError(f"{repo} is not on main")
        if git(repo, "status", "--porcelain", "--untracked-files=no"):
            time.sleep(10)
            continue
        merged = run(["git", "-C", repo, "merge", "--no-ff", "-m", message, branch],
                     check=False)
        if merged.returncode == 0:
            return
        if (repo / ".git" / "MERGE_HEAD").exists():
            run(["git", "-C", repo, "merge", "--abort"], check=False)
            raise CompletionError(f"merging {branch} into main conflicted: "
                                  f"{merged.stdout.strip()[-500:]}")
        time.sleep(10)
    raise CompletionError(f"main stayed busy; {branch} was not merged")


def describe(missing: frozenset[str]) -> str:
    return ", ".join(sorted(missing))


def parked_tag_target(repo: Path, tag: str, tag_commit: str) -> str | None:
    peeled = git(repo, "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}", check=False)
    if peeled.lower() == tag_commit:
        return None
    # A previous completion moved the tag onto its metadata commit (so
    # updaters read the promoted manifest there). The publisher insists
    # the tag still peels to the manifest's tag commit, so park it there
    # for the publish and put it back afterwards; the served mirror never
    # takes a non-forced tag move, so nothing public changes meanwhile.
    descends = run(["git", "-C", repo, "merge-base", "--is-ancestor", tag_commit, peeled],
                   check=False).returncode == 0
    if not peeled or not descends:
        raise CompletionError(f"tag {tag} points at {peeled[:12] or 'nothing'}, "
                              f"not the candidate's tag commit {tag_commit[:12]}")
    return peeled


def release_hosts(args: argparse.Namespace) -> list:
    hosts_file = Path(args.hosts_file).expanduser()
    seed_file = Path(args.seed_hosts_file).expanduser()
    hosts = seed_targets(
        parse_hosts(hosts_file.read_text()) if hosts_file.is_file() else [],
        Path(args.ssh_key).expanduser())
    hosts += seed_targets(
        seed_hosts(seed_file.read_text() if seed_file.is_file() else ""),
        Path(args.seed_ssh_key).expanduser(),
        Path(args.seed_known_hosts).expanduser())
    if not hosts:
        raise ConfigError(f"no release hosts listed in {hosts_file} or {seed_file}")
    return hosts


def complete_release(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    work.mkdir(parents=True, exist_ok=True)
    bare = Path(args.served_mirror).expanduser()
    cas = bare / "forkmesh-releases"
    gate = _load_gate()
    try:
        required = gate.parse_required_assets(os.environ.get(gate.REQUIRED_ASSETS_ENV))
    except gate.GateError as exc:
        raise ConfigError(str(exc)) from None
    only = frozenset(args.only) if args.only else frozenset(HOST_ASSETS)

    manifests = manifests_at(repo, "main")
    local_dmg = None
    local_names: frozenset[str] = frozenset()
    if args.tag:
        channel = f"candidate-{args.tag.lstrip('v')}"
        candidate = manifests.get(channel)
        if candidate is None or candidate.get("tag") != args.tag:
            raise CompletionError(f"main has no {channel} manifest for {args.tag}")
        if args.macos_dmg:
            local_dmg, local_names = local_macos_dmg(
                Path(args.macos_dmg).expanduser(), str(candidate.get("build_commit", "")))
        producible = frozenset(HOST_ASSETS)
        if local_dmg:
            producible = (producible - frozenset(MACOS_ASSETS)) | local_names
        wanted = wanted_host_assets(manifests.get("latest"), required) | (only & producible)
        missing = missing_host_assets(candidate, required, wanted)
        if not missing:
            log(f"{channel} already carries every host-built asset; nothing to build")
            return 0
    else:
        pending = pending_completion(manifests, required)
        if pending is None:
            return 0
        channel, candidate, missing = pending
        if args.macos_dmg:
            local_dmg, local_names = local_macos_dmg(
                Path(args.macos_dmg).expanduser(), str(candidate.get("build_commit", "")))
            missing = missing | (local_names - asset_names(candidate))
    missing = missing & only
    if not missing:
        log(f"{channel}: nothing selected to build")
        return 0
    tag = str(candidate["tag"])
    tag_commit = str(candidate.get("tag_commit", "")).lower()
    build_commit = str(candidate.get("build_commit", "")).lower()
    if not COMMIT_RE.match(tag_commit) or not COMMIT_RE.match(build_commit):
        raise CompletionError(f"{channel} manifest has no exact tag/build commit")
    parked_tag = parked_tag_target(repo, tag, tag_commit)
    marker = work / f"failed-{channel}"
    if not args.tag and failure_backoff_active(marker, time.time()):
        log(f"{channel} failed recently; waiting before another attempt")
        return 0
    log(f"{channel} ({tag}) is held back for {describe(missing)}; completing it")
    from_local = missing & local_names
    macos_wanted = frozenset() if local_dmg else missing & frozenset(MACOS_ASSETS)
    to_build = missing - from_local
    if args.dry_run:
        if from_local:
            log(f"dry run: would publish {local_dmg} as {describe(from_local)} "
                "instead of building the macOS image")
        if macos_wanted:
            log(f"dry run: would build {describe(macos_wanted)} from one universal image on "
                f"the office Mac, or on Codemagic {CODEMAGIC_WORKFLOW} when the Mac is offline")
        log(f"dry run: would build {describe(to_build) or 'nothing'} at {build_commit[:12]}, "
            "publish, promote, seed mirrors, merge to main, and move the tag")
        return 0

    installer_key = installer_public_key(git(repo, "show", "main:www/public/install.sh"))
    hosts = release_hosts(args)

    built: dict[str, Path] = {name: local_dmg for name in from_local}
    mac = find_build_mac() if macos_wanted else None
    macos_client, macos_build, macos_deferred = None, "", ""
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        mac_job = (pool.submit(build_macos_on_mac, mac, repo, work, tag, build_commit)
                   if mac else None)
        try:
            if macos_wanted and not mac:
                macos_client, macos_build, macos_deferred = start_codemagic_macos(
                    tag, build_commit, args.public_repository)
            if missing & frozenset(LINUX_ASSETS):
                linux = build_linux_assets(repo, work, tag, build_commit, channel)
                for name in LINUX_ASSETS:
                    if name in missing:
                        built[name] = linux[name]
                if APPIMAGE_ASSET in missing:
                    built[ZSYNC_ASSET] = linux[ZSYNC_ASSET]
            if APK_ASSET in missing:
                built[APK_ASSET] = build_apk(repo, work, tag, build_commit,
                                             android_version_code(tag), args.signer_sha256)
            if WINDOWS_ASSET in missing:
                built[WINDOWS_ASSET] = build_windows_installer(
                    repo, work, tag, build_commit, channel, args.windows_signer_sha256)
            dmg = None
            if mac_job:
                try:
                    dmg = mac_job.result()
                except MacUnreachable as exc:
                    log(f"{exc}; building the macOS image on Codemagic instead")
                    macos_client, macos_build, macos_deferred = start_codemagic_macos(
                        tag, build_commit, args.public_repository)
            if macos_build:
                dmg = await_macos_dmg(macos_client, macos_build, tag, build_commit,
                                      work / f"macos-{tag}")
            if dmg:
                _, names = local_macos_dmg(dmg, build_commit)
                for name in macos_wanted & names:
                    built[name] = dmg
        except (CompletionError, subprocess.TimeoutExpired):
            marker.write_text(str(time.time()))
            raise
    if macos_deferred:
        marker.write_text(str(time.time()))
        if not built:
            raise CompletionError(f"only the macOS image is missing and it is deferred: "
                                  f"{macos_deferred}")
    else:
        marker.unlink(missing_ok=True)
    held_for = frozenset(name for name in MACOS_ASSETS if name in missing and name not in built)
    held_back = bool(held_for)
    digests = {name: sha256_file(path) for name, path in built.items()}

    branch = f"release/{tag}-host-assets"
    promote_tree = work / f"promote-{tag}"
    if parked_tag:
        log(f"parking tag {tag} on {tag_commit[:12]} for the publisher "
            f"(currently {parked_tag[:12]})")
        move_tag(repo, tag, tag_commit)
    try:
        metadata_commit, assets = publish_and_promote(
            repo, work, branch, promote_tree, channel, tag, tag_commit, build_commit,
            cas, built, digests, missing, installer_key, promote=not held_back,
            held_for=describe(held_for) or MACOS_ASSET)
    finally:
        if parked_tag:
            move_tag(repo, tag, parked_tag)

    # Blobs first: install.sh and updaters resolve assets the moment the
    # metadata reaches a mirror.
    seed_mirrors(cas, sorted(set(assets.values())), hosts, args.remote_repository)
    if args.no_land:
        log(f"metadata committed on {branch} ({metadata_commit[:12]}); not merged or tagged")
        return 0
    if held_back:
        land_on_main(repo, branch, f"Merge {branch}: publish {tag} "
                     f"{describe(frozenset(built))} into {channel} (macOS image deferred)")
        remove_worktree(repo, promote_tree)
        log(f"{channel} now carries {describe(frozenset(built))}; held back for "
            f"{describe(held_for)} until it is built "
            f"({macos_deferred or 'the macOS image was not universal'})")
        return 0
    land_on_main(repo, branch, f"Merge {branch}: publish {tag} to latest with {describe(missing)}")
    move_tag(repo, tag, metadata_commit)
    if bare.is_dir():
        # The served mirror syncs heads from the checkout without forcing tags.
        run(["git", "-C", bare, "fetch", "--quiet", repo, f"+refs/tags/{tag}:refs/tags/{tag}"],
            check=False)
    remove_worktree(repo, promote_tree)
    for label in ("linux", "apk", "win"):
        remove_worktree(repo, work / f"{label}-{tag}")
    forkmesh_safe_delete.remove_tree(work / f"linux-out-{tag}", work, ignore_errors=True)
    for name in (MACOS_ASSET, dmg_artefact_name(tag)):
        (work / f"macos-{tag}" / name).unlink(missing_ok=True)
    log(f"{tag} is live in latest with {describe(missing)}; tag -> {metadata_commit[:12]}")
    return 0


def complete_ios(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    manifests = manifests_at(repo, "main")
    if args.tag:
        tag = args.tag if args.tag.startswith("v") else f"v{args.tag}"
    else:
        tag = ios_release_tag(manifests)
        if not tag:
            return 0
    candidate = manifests.get(f"candidate-{tag.lstrip('v')}") or {}
    if candidate.get("tag") != tag:
        raise CompletionError(f"main has no candidate manifest for {tag}")
    state = work / f"ios-{tag}.json"
    if state.is_file():
        if args.tag:
            log(f"{tag}: the iOS app was already submitted to the App Store ({state})")
        return 0
    marker = work / f"failed-ios-{tag}"
    if not args.tag and failure_backoff_active(marker, time.time()):
        return 0
    build_commit = str(candidate.get("build_commit", "")).lower()
    if not COMMIT_RE.match(build_commit):
        raise CompletionError(f"the {tag} candidate manifest has no exact build commit")
    if args.dry_run:
        log(f"dry run: would build the iOS app for {tag} at {build_commit[:12]} on the office "
            f"Mac, or on Codemagic {CODEMAGIC_IOS_WORKFLOW} when the Mac is offline, upload it "
            "and submit it for App Store review")
        return 0
    notes = release_notes(repo, tag)
    log(f"{tag}: rolling the iOS app out to the App Store")
    try:
        result = None
        mac = find_build_mac()
        if mac:
            try:
                result = release_ios_on_mac(mac, repo, work, tag, build_commit, notes)
            except MacUnreachable as exc:
                log(f"{exc}; building the iOS app on Codemagic instead")
        if result is None:
            client = codemagic_client(f"the {tag} iOS release needs Codemagic while the "
                                      "office Mac is offline,")
            result = release_ios_on_codemagic(client, tag, notes, args.public_repository)
    except (CompletionError, ConfigError, subprocess.TimeoutExpired):
        marker.write_text(str(time.time()))
        raise
    marker.unlink(missing_ok=True)
    result.update(tag=tag, build_commit=build_commit, submitted_at=int(time.time()))
    state.write_text(json.dumps(result, indent=2) + "\n")
    log(f"{tag}: iOS {result.get('version') or ''} ({result.get('build') or '?'}) submitted "
        f"for App Store review via {result['route']}")
    return 0


def check_homepage(args: argparse.Namespace, fetch=None, now=None) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    latest = manifests_at(repo, "main").get("latest") or {}
    tag = str(latest.get("tag", ""))
    if not tag or args.dry_run:
        return 0
    confirmed = work / f"homepage-{tag}"
    if confirmed.is_file():
        return 0
    try:
        served = (fetch or fetch_homepage_manifest)()
    except (OSError, ValueError) as exc:
        served, reason = {}, str(exc)
    else:
        reason = f"it serves {served.get('tag') or 'no tag'}"
    if (served.get("tag") == tag
            and served.get("checksums_sha256") == latest.get("checksums_sha256")):
        confirmed.write_text(f"{int(now or time.time())}\n")
        (work / f"homepage-{tag}.pending").unlink(missing_ok=True)
        log(f"the homepage offers {tag}: app.forkmesh.com serves the promoted manifest")
        return 0
    pending = work / f"homepage-{tag}.pending"
    if not pending.is_file():
        pending.write_text(f"{int(now or time.time())}\n")
    if failure_backoff_active(pending, now or time.time(), HOMEPAGE_WAIT_SECONDS):
        return 0
    raise CompletionError(f"the homepage still offers an older release: {HOMEPAGE_MANIFEST_URL} "
                          f"does not serve the {tag} manifest on main ({reason})")


def build_platform(plan: dict, repo: Path, work: Path, args: argparse.Namespace) -> dict[str, Path]:
    platform, tag, commit, channel = plan["platform"], plan["tag"], plan["commit"], plan["channel"]
    if platform == "linux":
        linux = build_linux_assets(repo, work, tag, commit, channel)
        return {name: linux[name] for name in (*LINUX_ASSETS, ZSYNC_ASSET)}
    if platform == "android":
        return {APK_ASSET: build_apk(repo, work, tag, commit,
                                     android_version_code(tag, plan["build"]),
                                     args.signer_sha256)}
    if platform == "windows":
        return {WINDOWS_ASSET: build_windows_installer(repo, work, tag, commit, channel,
                                                       args.windows_signer_sha256)}
    if args.macos_dmg:
        dmg = Path(args.macos_dmg).expanduser()
    else:
        mac = find_build_mac()
        dmg = None
        if mac:
            try:
                dmg = build_macos_on_mac(mac, repo, work, tag, commit)
            except MacUnreachable as exc:
                log(str(exc))
        if dmg is None:
            if plan["rebuild"]:
                raise CompletionError("a macOS rebuild needs the office Mac (Codemagic builds "
                                      "only the release tag); start it and run this again")
            client, build_id, deferred = start_codemagic_macos(tag, commit,
                                                               args.public_repository)
            if deferred:
                raise CompletionError(f"the macOS image is deferred: {deferred}")
            dmg = await_macos_dmg(client, build_id, tag, commit, work / f"macos-{tag}")
    _, names = local_macos_dmg(dmg, commit)
    if set(MACOS_ASSETS) - names:
        raise CompletionError(f"{dmg.name} is not a universal image; "
                              f"it can only be {describe(frozenset(names))}")
    return {name: dmg for name in MACOS_ASSETS}


def deploy_build(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    work.mkdir(parents=True, exist_ok=True)
    cas = Path(args.served_mirror).expanduser() / "forkmesh-releases"
    gate = _load_gate()
    try:
        required = gate.parse_required_assets(os.environ.get(gate.REQUIRED_ASSETS_ENV))
    except gate.GateError as exc:
        raise ConfigError(str(exc)) from None
    manifests = manifests_at(repo, "main")
    builds_text = run(["git", "-C", repo, "show", f"main:{BUILDS_FILE}"], check=False).stdout
    plan = plan_deploy(manifests, args.deploy_build, builds_text,
                       git(repo, "rev-parse", "--verify", "main"), args.tag, args.first_build)
    label = PLATFORM_LABELS[plan["platform"]]
    tag, channel = plan["tag"], plan["channel"]
    candidate = manifests[channel]
    if plan["skip"]:
        log(f"{label} {tag} already has build {plan['build']}; nothing to build")
        return 0
    built_names = frozenset(plan["assets"]) | (
        frozenset({ZSYNC_ASSET}) if plan["platform"] == "linux" else frozenset())
    blockers = promotion_blockers(manifests.get("latest"),
                                  asset_names(candidate) | built_names, required)
    if args.dry_run:
        log(f"plan: {describe_plan(plan)}")
        log(f"plan: then {'promote it to latest' if not blockers else 'hold it back until ' + describe(blockers) + ' are built'}")
        return 0
    tag_commit = str(candidate.get("tag_commit", "")).lower()
    build_commit = str(candidate.get("build_commit", "")).lower()
    if not COMMIT_RE.match(tag_commit) or not COMMIT_RE.match(build_commit):
        raise CompletionError(f"{channel} manifest has no exact tag/build commit")
    installer_key = installer_public_key(git(repo, "show", "main:www/public/install.sh"))
    hosts = release_hosts(args)
    log(f"deploying {describe_plan(plan)}")
    built = build_platform(plan, repo, work, args)
    digests = {name: sha256_file(path) for name, path in built.items()}
    builds = recorded_builds(builds_text, tag)
    builds[plan["platform"]] = plan["build"]
    publish_args = (("--build", str(plan["build"]), "--asset-commit", plan["commit"])
                    if plan["rebuild"] else ())
    branch = f"release/{tag}-{plan['platform']}-build-{plan['build']}"
    promote_tree = work / f"promote-{tag}"
    parked_tag = parked_tag_target(repo, tag, tag_commit)
    if parked_tag:
        move_tag(repo, tag, tag_commit)
    try:
        metadata_commit, assets = publish_and_promote(
            repo, work, branch, promote_tree, channel, tag, tag_commit, build_commit,
            cas, built, digests, frozenset(built), installer_key, promote=not blockers,
            held_for=describe(blockers), publish_args=publish_args,
            extra_files={BUILDS_FILE: render_builds(tag, builds)}, built_at=plan["commit"])
    finally:
        if parked_tag:
            move_tag(repo, tag, parked_tag)
    seed_mirrors(cas, sorted(set(assets.values())), hosts, args.remote_repository)
    if args.no_land:
        log(f"metadata committed on {branch} ({metadata_commit[:12]}); not merged or tagged")
        return 0
    land_on_main(repo, branch, f"Merge {branch}: publish {label} {tag} build {plan['build']}")
    remove_worktree(repo, promote_tree)
    if blockers:
        log(f"{label} {tag} build {plan['build']} is in {channel}; it goes public once "
            f"{describe(blockers)} are built")
        return 0
    move_tag(repo, tag, metadata_commit)
    bare = Path(args.served_mirror).expanduser()
    if bare.is_dir():
        run(["git", "-C", bare, "fetch", "--quiet", repo, f"+refs/tags/{tag}:refs/tags/{tag}"],
            check=False)
    log(f"{label} {tag} build {plan['build']} is live in latest; tag -> {metadata_commit[:12]}")
    return 0


def reset_build_numbers(args: argparse.Namespace) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    _, candidate = deploy_target(manifests_at(repo, "main"), args.tag)
    tag = str(candidate["tag"])
    current = run(["git", "-C", repo, "show", f"main:{BUILDS_FILE}"], check=False).stdout
    text = render_builds(tag, {platform: 1 for platform in PLATFORM_ASSETS})
    summary = ", ".join(f"{PLATFORM_LABELS[p]} 1" for p in PLATFORM_ASSETS)
    if current == text:
        log(f"{tag} build numbers are already reset: {summary}")
        return 0
    if args.dry_run:
        log(f"dry run: would reset {tag} build numbers to {summary}")
        return 0
    branch = f"release/{tag}-reset-builds"
    tree = work / f"reset-builds-{tag}"
    remove_worktree(repo, tree)
    if git(repo, "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}", check=False):
        git(repo, "branch", "-D", branch)
    git(repo, "worktree", "add", "-q", "-b", branch, str(tree), "main")
    try:
        (tree / BUILDS_FILE).write_text(text)
        git(tree, "add", "--", BUILDS_FILE)
        git(tree, "-c", "user.name=ForkMesh Actions", "-c", "user.email=actions@forkmesh.local",
            "commit", "-q", "-m", f"release: reset {tag} build numbers to 1")
    finally:
        remove_worktree(repo, tree)
    land_on_main(repo, branch, f"Merge {branch}: reset {tag} build numbers to 1")
    log(f"{tag} build numbers reset: {summary}")
    return 0


def wait_for_candidate(args: argparse.Namespace, sleep=time.sleep,
                       clock=time.monotonic) -> int:
    repo = Path(args.repo).resolve()
    tag = args.tag if args.tag.startswith("v") else f"v{args.tag}"
    deadline = clock() + CANDIDATE_WAIT_SECONDS
    announced = False
    while True:
        try:
            channel, _ = deploy_target(manifests_at(repo, "main"), tag)
        except CompletionError:
            if clock() >= deadline:
                raise CompletionError(f"main still has no candidate for {tag} after "
                                      f"{CANDIDATE_WAIT_SECONDS // 60} minutes; check the "
                                      "release workflow on the Actions page") from None
            if not announced:
                log(f"waiting for the release workflow to publish {tag}'s candidate on main")
                announced = True
            sleep(CANDIDATE_POLL_SECONDS)
            continue
        log(f"{channel} is on main; {tag} is ready for its builds")
        return 0


def check_build(args: argparse.Namespace, fetch=None, sleep=time.sleep,
                clock=time.monotonic) -> int:
    repo = Path(args.repo).resolve()
    platform = args.check_build
    label = PLATFORM_LABELS[platform]
    manifests = manifests_at(repo, "main")
    channel, candidate = deploy_target(manifests, args.tag)
    latest = manifests.get("latest") or {}
    tag = str(candidate.get("tag", ""))
    if latest.get("tag") != tag:
        log(f"{label} {tag} waits in {channel}; the homepage keeps "
            f"{latest.get('tag') or 'its release'} until every platform is built")
        return 0
    build = platform_build(latest, platform)
    if args.dry_run:
        log(f"dry run: would wait for the homepage to offer {label} {tag} build {build}")
        return 0
    deadline = clock() + HOMEPAGE_WAIT_SECONDS
    while True:
        try:
            served = (fetch or fetch_homepage_manifest)()
            reason = f"it serves {served.get('tag') or 'no tag'}"
        except (OSError, ValueError) as exc:
            served, reason = {}, str(exc)
        if (served.get("tag") == tag
                and served.get("checksums_sha256") == latest.get("checksums_sha256")):
            log(f"the homepage offers {label} {tag} build {build}")
            return 0
        if clock() >= deadline:
            raise CompletionError(f"the homepage does not offer {label} {tag} build {build} "
                                  f"yet ({reason})")
        sleep(HOMEPAGE_POLL_SECONDS)


def acquire_lock(lock, wait: bool, sleep=time.sleep, clock=time.monotonic) -> bool:
    deadline = clock() + (LOCK_WAIT_SECONDS if wait else 0)
    announced = False
    while True:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            if clock() >= deadline:
                return False
            if not announced:
                log("waiting for the release host's current run to finish")
                announced = True
            sleep(LOCK_POLL_SECONDS)


def save_rollout_state(path: Path, state: dict) -> None:
    staged = path.with_name(path.name + ".tmp")
    staged.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n")
    staged.replace(path)


def roll_out_release(args: argparse.Namespace, runner=subprocess.run, now=None) -> int:
    repo = Path(args.repo).resolve()
    work = Path(args.work_dir).expanduser()
    latest = manifests_at(repo, "main").get("latest")
    if not latest:
        return 0
    tag = str(latest.get("tag", ""))
    if args.tag and args.tag != tag:
        if args.rollout_only:
            raise CompletionError(f"latest on main is {tag or 'empty'}, not {args.tag}; "
                                  "only the promoted release is rolled out")
        return 0
    path = work / f"rollout-{tag}.json"
    state = load_rollout_state(path, tag)
    hosts_file = Path(args.hosts_file).expanduser()
    listed = parse_hosts(hosts_file.read_text()) if hosts_file.is_file() else []
    hosts = rollout_hosts(listed)
    pending = rollout_targets(state, hosts)
    if not pending:
        if not state.get("settled") and not args.dry_run:
            state["settled"] = int(now or time.time())
            save_rollout_state(path, state)
            log(f"{tag}: every managed mirror-node host runs it "
                f"({len(hosts)} in {hosts_file}"
                + (f"; {len(listed) - len(hosts)} ForkMesh Next core(s) left to next_fleet"
                   if len(listed) != len(hosts) else "") + ")")
        return 0
    release = rollout_release(latest)
    waiting = rollout_due(state, now or time.time(), forced=bool(args.tag))
    if waiting:
        if state.get("reported") != waiting and not args.dry_run:
            state["reported"] = waiting
            save_rollout_state(path, state)
            log(f"{tag}: mirror-node rollout {waiting}; still pending on "
                f"{', '.join(name for name, _ in pending)} (run --rollout-only --tag {tag} "
                "to retry now)")
        return 0
    if args.dry_run:
        log(f"dry run: would roll out {tag} mirror-node to {', '.join(n for n, _ in pending)}")
        return 0
    remote = rollout_remote_command(release, args.rollout_blob_base,
                                    f"{args.remote_repository}/forkmesh-releases")
    log(f"{tag}: rolling out the mirror node to {', '.join(name for name, _ in pending)}")
    results = roll_out_hosts(seed_targets(pending, Path(args.ssh_key).expanduser()), remote,
                             runner=runner)
    for name, (_, status, detail) in sorted(results.items()):
        log(f"{name}: {'failed, ' if status == 'failed' else ''}{detail}")
    state = record_rollout(state, results, now or time.time())
    state.pop("reported", None)
    state.pop("settled", None)
    failed = sorted(name for name, (_, status, _) in results.items() if status == "failed")
    if not failed:
        state["settled"] = int(now or time.time())
    save_rollout_state(path, state)
    if failed:
        raise CompletionError(f"{tag}: the mirror-node rollout failed on {', '.join(failed)} "
                              f"(attempt {state['attempts']} of {ROLLOUT_MAX_ATTEMPTS})")
    log(f"{tag}: the mirror node runs it on every managed host")
    return 0


def run_step(step, args: argparse.Namespace) -> int:
    try:
        return step(args)
    except ConfigError as exc:
        log(f"ERROR (configuration): {exc}")
        return 2
    except (CompletionError, subprocess.TimeoutExpired, OSError) as exc:
        log(f"ERROR: {exc}")
        return 1


def publish_and_promote(repo: Path, work: Path, branch: str, promote_tree: Path,
                        channel: str, tag: str, tag_commit: str, build_commit: str,
                        cas: Path, built: dict[str, Path], digests: dict[str, str],
                        missing: frozenset[str], installer_key: str,
                        promote: bool = True,
                        held_for: str = MACOS_ASSET,
                        publish_args: tuple = (),
                        extra_files: dict[str, str] | None = None,
                        built_at: str = "") -> tuple[str, dict[str, str]]:
    """Publish the built assets into the candidate, promote it, verify the
    promoted manifest, and commit the metadata on `branch`."""
    remove_worktree(repo, promote_tree)
    if git(repo, "rev-parse", "--verify", "--quiet", f"refs/heads/{branch}", check=False):
        git(repo, "branch", "-D", branch)
    git(repo, "worktree", "add", "-q", "-b", branch, str(promote_tree), "main")
    with tempfile.TemporaryDirectory(dir=work, prefix="sign-") as secret_dir:
        os.chmod(secret_dir, 0o700)
        secrets = Path(secret_dir)
        key = secrets / "release-key.pem"
        release_signing_key(key, installer_key)
        staged = []
        for name, path in built.items():
            target = secrets / name
            shutil.copyfile(path, target)
            staged.append(target)
        run([promote_tree / "tools/forkmesh-release-publish.sh", "--channel", channel,
             "--signing-key", key, "--tag", tag, "--tag-commit", tag_commit,
             "--build-commit", build_commit, "--cas-dir", cas, *publish_args, *staged],
            cwd=promote_tree)
        for relative, text in (extra_files or {}).items():
            (promote_tree / relative).write_text(text)
            git(promote_tree, "add", "--", relative)
        built_at = built_at or build_commit
        if not promote:
            candidate_dir = promote_tree / RELEASES / channel
            published = json.loads((candidate_dir / "release.json").read_text())
            assets = {a["name"]: a["blob_sha256"] for a in published.get("assets", [])}
            if (published.get("tag") != tag
                    or any(assets.get(name) != digest for name, digest in digests.items())):
                raise CompletionError(f"the {channel} manifest does not describe the built assets")
            git(promote_tree, "add", "--", f"{RELEASES}/{channel}")
            summary = "\n".join(f"{name} {digest[:12]}" for name, digest in sorted(digests.items()))
            git(promote_tree, "-c", "user.name=ForkMesh Actions",
                "-c", "user.email=actions@forkmesh.local", "commit", "-q", "-m",
                f"release: publish {tag} {describe(frozenset(built))} into {channel}\n\n"
                f"Held back for {held_for}. Built at {built_at[:12]} by "
                f"tools/forkmesh-release-host.py:\n{summary}")
            return git(promote_tree, "rev-parse", "HEAD"), assets
        promoted = run([sys.executable, "tools/forkmesh-release-promote.py",
                        "--candidate", channel], cwd=promote_tree, check=False)
        if promoted.returncode != 0:
            raise CompletionError(f"promotion gate refused {channel}: "
                                  f"{(promoted.stdout + promoted.stderr).strip()[-1500:]}")
        public_pem = secrets / "installer.pub.pem"
        public_pem.write_text(f"-----BEGIN PUBLIC KEY-----\n{installer_key}\n"
                              "-----END PUBLIC KEY-----\n")
        latest_dir = promote_tree / RELEASES / "latest"
        run(["openssl", "pkeyutl", "-verify", "-pubin", "-rawin", "-inkey", public_pem,
             "-in", latest_dir / "release.json", "-sigfile", latest_dir / "release.json.sig"])
    latest = json.loads((latest_dir / "release.json").read_text())
    shasums = (latest_dir / "SHASUMS256.txt").read_bytes()
    assets = {a["name"]: a["blob_sha256"] for a in latest.get("assets", [])}
    if (latest.get("tag") != tag
            or any(assets.get(name) != digest for name, digest in digests.items())
            or hashlib.sha256(shasums).hexdigest() != latest.get("checksums_sha256")):
        raise CompletionError("the promoted latest manifest does not describe the built assets")
    git(promote_tree, "add", "--", f"{RELEASES}/{channel}", f"{RELEASES}/latest")
    summary = "\n".join(f"{name} {digest[:12]}" for name, digest in sorted(digests.items()))
    git(promote_tree, "-c", "user.name=ForkMesh Actions",
        "-c", "user.email=actions@forkmesh.local", "commit", "-q", "-m",
        f"release: publish {tag} {describe(missing)} and promote to latest\n\n"
        f"Built at {built_at[:12]} by tools/forkmesh-release-host.py:\n{summary}")
    metadata_commit = git(promote_tree, "rev-parse", "HEAD")
    return metadata_commit, assets


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--auto", action="store_true",
                        help="complete the newest held-back release, if any")
    target.add_argument("--tag", help="complete this release, e.g. v0.8.7")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--deploy-build", choices=sorted(PLATFORM_ASSETS), metavar="PLATFORM",
                       help="build the next build number of one platform (android, linux, "
                            "macos, windows) and publish it to the homepage; the newest "
                            "candidate's version unless --tag names one")
    modes.add_argument("--check-build", choices=sorted(PLATFORM_ASSETS), metavar="PLATFORM",
                       help="wait until the homepage offers that platform's newest build")
    modes.add_argument("--reset-builds", action="store_true",
                       help="reset every platform's build number to 1 for the newest "
                            "candidate's version (or --tag)")
    modes.add_argument("--wait-candidate", action="store_true",
                       help="wait until main carries the --tag release's candidate manifest")
    parser.add_argument("--first-build", action="store_true",
                        help="with --deploy-build: build only when the platform has no "
                             "build of this version yet")
    parser.add_argument("--only", action="append", choices=HOST_ASSETS,
                        help="build only this host asset (repeatable)")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--macos-dmg", metavar="PATH",
                        help="publish this locally built .dmg instead of building one on "
                             f"the office Mac or Codemagic: as {MACOS_ASSET}, and also as "
                             f"{MACOS_X86_64_ASSET} when it is universal")
    steps = parser.add_mutually_exclusive_group()
    steps.add_argument("--skip-ios", action="store_true",
                       help="do not build the iOS app or submit it to the App Store")
    steps.add_argument("--ios-only", action="store_true",
                       help="only build the iOS app and submit it to the App Store")
    rollout = parser.add_mutually_exclusive_group()
    rollout.add_argument("--skip-rollout", action="store_true",
                         help="do not roll the promoted mirror node out to the managed hosts")
    rollout.add_argument("--rollout-only", action="store_true",
                         help="only roll the promoted mirror node out to the managed hosts; "
                              "with --tag, retry now even after failed attempts")
    parser.add_argument("--rollout-blob-base", default=ROLLOUT_BLOB_BASE,
                        help="where a host downloads the mirror-node blob when its own "
                             "release store lacks it")
    parser.add_argument("--no-land", action="store_true",
                        help="publish, promote and seed, but leave the metadata on its "
                             "release branch instead of merging to main and moving the tag")
    parser.add_argument("--repo", default=str(ROOT),
                        help="primary checkout on main (default: this checkout)")
    parser.add_argument("--work-dir", default=str(LAYOUT.release_work))
    parser.add_argument("--served-mirror",
                        default=str(FORKMESH_DATA / "mirrors/forkmesh-forkmesh.git"))
    parser.add_argument("--hosts-file", default=str(LAYOUT.fanout_hosts))
    parser.add_argument("--ssh-key", default=str(LAYOUT.ssh / "vultr_mirror_ed25519"))
    parser.add_argument("--seed-hosts-file", default=str(LAYOUT.release_seed_hosts))
    parser.add_argument("--seed-ssh-key", default=str(LAYOUT.fleet_ssh_key))
    parser.add_argument("--seed-known-hosts",
                        default=str(LAYOUT.fleet_config / "known_hosts"))
    parser.add_argument("--public-repository", default=PUBLIC_REPOSITORY,
                        help="the public clone URL Codemagic builds the macOS image from")
    parser.add_argument("--remote-repository",
                        default="/var/lib/forkmesh/repositories/forkmesh-forkmesh.git")
    parser.add_argument("--signer-sha256",
                        default=os.environ.get("FORKMESH_ANDROID_SIGNER_SHA256",
                                               DEFAULT_SIGNER_SHA256))
    parser.add_argument("--windows-signer-sha256",
                        default=os.environ.get("FORKMESH_WINDOWS_SIGNER_SHA256",
                                               DEFAULT_WINDOWS_SIGNER_SHA256))
    args = parser.parse_args(argv)
    build_mode = args.deploy_build or args.reset_builds
    if args.auto and (build_mode or args.check_build or args.wait_candidate):
        parser.error("--auto completes held-back releases; give --tag or nothing instead")
    if not (args.auto or args.tag or build_mode or args.check_build or args.wait_candidate):
        parser.error("give --auto, --tag, --deploy-build, --check-build or --reset-builds")
    if args.wait_candidate and not args.tag:
        parser.error("--wait-candidate needs --tag")
    if args.first_build and not args.deploy_build:
        parser.error("--first-build goes with --deploy-build")
    if args.check_build:
        return run_step(check_build, args)
    if args.wait_candidate:
        return run_step(wait_for_candidate, args)

    work = Path(args.work_dir).expanduser()
    work.mkdir(parents=True, exist_ok=True)
    with (work / "lock").open("w") as lock:
        if not acquire_lock(lock, bool(build_mode)):
            if build_mode:
                log(f"the release host stayed busy for {LOCK_WAIT_SECONDS // 3600} hours")
                return 1
            log("another completion run holds the lock; exiting")
            return 0
        if args.deploy_build:
            return run_step(deploy_build, args)
        if args.reset_builds:
            return run_step(reset_build_numbers, args)
        codes = []
        if args.rollout_only:
            return run_step(roll_out_release, args)
        if not args.ios_only:
            codes.append(run_step(complete_release, args))
            if not args.skip_rollout and not args.no_land:
                codes.append(run_step(roll_out_release, args))
        if not args.skip_ios:
            codes.append(run_step(complete_ios, args))
        if not args.ios_only:
            codes.append(run_step(check_homepage, args))
        return max(codes)


if __name__ == "__main__":
    sys.exit(main())
