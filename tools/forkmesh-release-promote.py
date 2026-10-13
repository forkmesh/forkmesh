#!/usr/bin/env python3
"""Promote a signed release candidate channel to the public channel.

Native runners publish into .forkmesh/releases/candidate-<version>/ (see
.forkmesh/release.yml and tools/forkmesh-release-publish.sh). This decides
whether that candidate may replace the public channel (default: latest), and
if so copies its signed metadata over and regenerates the appcast.

A partially failed matrix must never replace a working latest release, so a
candidate is held back when:

  * any REQUIRED asset is missing from the candidate manifest. The required set
    comes from $FORKMESH_RELEASE_REQUIRED_ASSETS (comma- and/or
    whitespace-separated asset names). When the variable is unset it defaults
    to what the fleet builds today; when it is set but names nothing the gate
    fails closed rather than promoting an arbitrary candidate.
  * the current public manifest offers assets the candidate lacks (for
    example the hand-signed Android APK). Promotion would silently withdraw
    them, so it is refused unless $FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS=1 (or
    --allow-dropped-assets) says the drop is deliberate.

Only metadata moves: the candidate's release.json, release.json.sig and
SHASUMS256.txt are copied byte-for-byte (the signature covers the exact
release.json bytes, so nothing is rewritten) and
tools/package/generate-appcast.sh derives the public appcast.xml from the
promoted release.json. Asset bytes already live in the content-addressed store.

Usage (run from the repository root):
  tools/forkmesh-release-promote.py --candidate candidate-0.8.6 [--channel latest]
      [--allow-dropped-assets] [--dry-run]
  tools/forkmesh-release-promote.py --check-config

Exit status: 0 promoted (or, with --dry-run / --check-config, would promote /
configuration is valid); 3 held back, the public channel is untouched; 2 usage,
configuration or manifest error (fail closed).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


REQUIRED_ASSETS_ENV = "FORKMESH_RELEASE_REQUIRED_ASSETS"
ALLOW_DROPPED_ENV = "FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS"
# What the fleet actually builds today: one Linux x86_64 runner produces the
# desktop binary, its AppImage, and the native mirror-node daemon. Add
# Windows/macOS/arm64 installers here (or in the node variable) once a runner
# for that platform exists.
DEFAULT_REQUIRED_ASSETS = (
    "forkmesh-linux-x86_64",
    "forkmesh-linux-x86_64.AppImage",
    "forkmesh-mirror-node-linux-x86_64",
)
METADATA_FILES = ("SHASUMS256.txt", "release.json", "release.json.sig")
RELEASES_DIR = Path(".forkmesh") / "releases"
APPCAST = Path(__file__).resolve().parent / "package" / "generate-appcast.sh"
PUBLISHER = "tools/forkmesh-release-publish.sh"

EXIT_PROMOTE = 0
EXIT_CONFIG = 2
EXIT_HOLD = 3


class GateError(Exception):
    """Configuration or manifest problem: the gate fails closed."""


def parse_required_assets(value: str | None) -> frozenset[str]:
    """Return the required asset names; None (unset) means the default set."""
    if value is None:
        return frozenset(DEFAULT_REQUIRED_ASSETS)
    names = frozenset(token for token in re.split(r"[\s,]+", value) if token)
    if not names:
        raise GateError(
            f"{REQUIRED_ASSETS_ENV} is set but names no assets; refusing to "
            "promote a release without a required asset set"
        )
    return names


def truthy(value: str | None) -> bool:
    return (value or "").strip().lower() in {"1", "true", "yes", "on"}


def load_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise GateError(f"no release manifest at {path}") from None
    except (OSError, ValueError) as exc:
        raise GateError(f"unreadable release manifest {path}: {exc}") from None
    if not isinstance(manifest, dict) or not isinstance(
        manifest.get("assets", []), list
    ):
        raise GateError(f"release manifest {path} has no asset list")
    return manifest


def asset_names(manifest: dict) -> frozenset[str]:
    return frozenset(
        str(asset.get("name"))
        for asset in manifest.get("assets", [])
        if isinstance(asset, dict) and asset.get("name")
    )


@dataclass
class Decision:
    promote: bool
    missing_required: list[str] = field(default_factory=list)
    dropped: list[str] = field(default_factory=list)
    dropped_allowed: bool = False


def decide(
    candidate: dict,
    current: dict | None,
    required: frozenset[str],
    allow_dropped: bool,
) -> Decision:
    if not required:
        raise GateError("the required asset set is empty")
    names = asset_names(candidate)
    missing = sorted(required - names)
    dropped = sorted(asset_names(current) - names) if current is not None else []
    blocked_by_drop = bool(dropped) and not allow_dropped
    return Decision(
        promote=not missing and not blocked_by_drop,
        missing_required=missing,
        dropped=dropped,
        dropped_allowed=bool(dropped) and allow_dropped,
    )


def explain(
    decision: Decision,
    candidate_channel: str,
    channel: str,
    candidate: dict,
    current: dict | None,
) -> list[str]:
    lines = []
    tag = candidate.get("tag", "?")
    current_tag = (current or {}).get("tag", "?")
    if decision.missing_required:
        lines.append(
            f"Candidate {candidate_channel} ({tag}) is not promoted to {channel}: "
            f"required asset(s) missing: {', '.join(decision.missing_required)}."
        )
        lines.append(
            f"  The required set comes from {REQUIRED_ASSETS_ENV}; {channel} keeps "
            f"serving {current_tag} until every required asset lands."
        )
    if decision.dropped and not decision.dropped_allowed:
        lines.append(
            f"Candidate {candidate_channel} ({tag}) is not promoted to {channel}: "
            f"{channel} ({current_tag}) offers asset(s) the candidate lacks: "
            f"{', '.join(decision.dropped)}."
        )
        lines.append(
            "  Publish them into the candidate for the same tag and build commit "
            "(the publisher merges same-provenance assets by name), e.g.:"
        )
        lines.append(
            f"    {PUBLISHER} --channel {candidate_channel} --tag {tag} "
            f"--build-commit {candidate.get('build_commit', '<build_commit>')} "
            "--signing-key <key.pem> --cas-dir <mirror>/forkmesh-releases "
            f"{' '.join(decision.dropped)}"
        )
        lines.append(
            f"  then run: tools/forkmesh-release-promote.py --candidate "
            f"{candidate_channel} --channel {channel}"
        )
        lines.append(
            f"  To retire them deliberately, set {ALLOW_DROPPED_ENV}=1 "
            "(or pass --allow-dropped-assets)."
        )
    if decision.dropped_allowed:
        lines.append(
            f"WARNING: promoting {candidate_channel} withdraws asset(s) {channel} "
            f"offered ({ALLOW_DROPPED_ENV} is set): {', '.join(decision.dropped)}."
        )
    return lines


def validate_channel(name: str, flag: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name or ""):
        raise GateError(f"{flag} must be a plain release channel name, got {name!r}")
    return name


def promote(candidate_channel: str, channel: str) -> None:
    source = RELEASES_DIR / candidate_channel
    target = RELEASES_DIR / channel
    for name in METADATA_FILES:
        if not (source / name).is_file():
            raise GateError(f"candidate is missing {source / name}")
    target.mkdir(parents=True, exist_ok=True)
    # Stage every file first so a failure part-way never leaves the public
    # channel with a release.json that its release.json.sig does not cover.
    staged = []
    for name in METADATA_FILES:
        tmp = target / f".{name}.promote.tmp"
        tmp.write_bytes((source / name).read_bytes())
        staged.append((tmp, target / name))
    for tmp, final in staged:
        os.replace(tmp, final)
    env = dict(os.environ, RELEASE_CHANNEL=channel)
    subprocess.run(
        ["bash", str(APPCAST), "--channel", channel], check=True, env=env
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__.split("\n\n", 1)[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--candidate", help="candidate channel, e.g. candidate-0.8.6")
    parser.add_argument("--channel", default="latest", help="public channel (default: latest)")
    parser.add_argument("--allow-dropped-assets", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="decide only; write nothing")
    parser.add_argument(
        "--check-config", action="store_true",
        help=f"only validate {REQUIRED_ASSETS_ENV} and exit",
    )
    args = parser.parse_args(argv)

    try:
        required = parse_required_assets(os.environ.get(REQUIRED_ASSETS_ENV))
        if args.check_config:
            print(f"Release promotion requires: {', '.join(sorted(required))}")
            return EXIT_PROMOTE
        if not args.candidate:
            raise GateError("--candidate is required")
        candidate_channel = validate_channel(args.candidate, "--candidate")
        channel = validate_channel(args.channel, "--channel")
        if candidate_channel == channel:
            raise GateError("--candidate and --channel must differ")
        allow_dropped = args.allow_dropped_assets or truthy(
            os.environ.get(ALLOW_DROPPED_ENV)
        )
        candidate = load_manifest(RELEASES_DIR / candidate_channel / "release.json")
        current_path = RELEASES_DIR / channel / "release.json"
        # A missing public manifest (first release) has nothing to drop. A
        # present but unreadable one fails closed like any other bad manifest.
        current = load_manifest(current_path) if current_path.exists() else None
        decision = decide(candidate, current, required, allow_dropped)
        for line in explain(decision, candidate_channel, channel, candidate, current):
            print(line)
        if not decision.promote:
            return EXIT_HOLD
        if args.dry_run:
            print(f"Candidate {candidate_channel} would be promoted to {channel}.")
            return EXIT_PROMOTE
        promote(candidate_channel, channel)
    except GateError as exc:
        print(f"ERROR: release promotion: {exc}", file=sys.stderr)
        return EXIT_CONFIG
    except subprocess.CalledProcessError as exc:
        print(f"ERROR: appcast generation failed ({exc.returncode})", file=sys.stderr)
        return 1
    print(f"Promoted {candidate_channel} ({candidate.get('tag', '?')}) to channel: {channel}")
    return EXIT_PROMOTE


if __name__ == "__main__":
    sys.exit(main())
