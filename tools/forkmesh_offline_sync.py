#!/usr/bin/env python3
"""Offline, low-bandwidth, and removable-media repository sync (issue #504).

Implements forkmesh-offline-media-v1 (www/docs/offline-sync.md): a signed
export of refs and collaboration history to a directory (a USB stick, a shared
folder, or any static HTTP host), a resumable chunked fetch of that directory
over an unreliable link, and an import that verifies provenance before it
reconciles refs in one transaction that can be rolled back.

  forkmesh_offline_sync.py keygen SEED_FILE
  forkmesh_offline_sync.py export REPO MEDIA_DIR --signing-key SEED_FILE \\
      [--ref GLOB]... [--path PATH]... [--metadata-only] \\
      [--defer-blobs-over SIZE] [--since PREVIOUS_MEDIA_DIR] [--chunk-size SIZE]
  forkmesh_offline_sync.py verify MEDIA_DIR [--trusted-key KEY]... [--json]
  forkmesh_offline_sync.py fetch SOURCE DEST_DIR [--rate SIZE] [--budget SIZE] \\
      [--schedule HH:MM-HH:MM]... [--wait] [--retries N] [--restart] [--json]
  forkmesh_offline_sync.py status DEST_DIR [--json]
  forkmesh_offline_sync.py import MEDIA_DIR REPO (--trusted-key KEY... | --allow-unpinned) \\
      [--allow-limited] [--json]
  forkmesh_offline_sync.py rollback REPO IMPORT_ID [--json]
  forkmesh_offline_sync.py replica REPO [--json]

SOURCE is a media directory or an http(s) URL of one; the server must honour
Range requests. `export`, `fetch`, `import`, and `rollback` accept
`--telemetry FILE`, which appends one count-only JSON line per run.

Exit codes: 0 done; 2 usage or unreadable input; 3 verification, integrity, or
missing-object failure (nothing was changed); 4 paused with a checkpoint kept
(data budget, schedule, cancellation, or the link stayed down); 5 imported
with conflicts kept aside for review.
"""

from __future__ import annotations

import argparse
import base64
import datetime as _dt
import fnmatch
import hashlib
import http.client
import json
import math
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_migration_report as _ed25519  # noqa: E402


MEDIA_SCHEMA = "forkmesh-offline-media-v1"
SIGNED_SCHEMA = "forkmesh-offline-media-signed-v1"
CHECKPOINT_SCHEMA = "forkmesh-offline-transfer-checkpoint-v1"
RESULT_SCHEMA = "forkmesh-offline-transfer-result-v1"
IMPORT_SCHEMA = "forkmesh-offline-import-journal-v1"
REPLICA_SCHEMA = "forkmesh-replica-scope-v1"
TELEMETRY_SCHEMA = "forkmesh-offline-telemetry-v1"

MANIFEST_FILE = "forkmesh-media.json"
BUNDLE_FILE = "repository.bundle"
TRANSFER_DIR = ".forkmesh-transfer"
STATE_DIR = "forkmesh-offline"
RESERVED_REF_PREFIX = "refs/forkmesh-offline/"
PROMISOR_REMOTE = "forkmesh-offline"
COLLABORATION_PATH = "/.forkmesh/"

DEFAULT_CHUNK_SIZE = 1024 * 1024
MIN_CHUNK_SIZE = 1024
MAX_CHUNK_SIZE = 64 * 1024 * 1024
MAX_MANIFEST_BYTES = 16 * 1024 * 1024
BLOCK_SIZE = 64 * 1024
CHUNK_INTEGRITY_ATTEMPTS = 3

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_FAILED = 3
EXIT_PAUSED = 4
EXIT_CONFLICTS = 5

# Ordered: the order is part of the replica advertisement.
LIMITATIONS = (
    "metadata_only", "path_filter", "branch_filter", "deferred_large_objects",
    "missing_objects",
)
SCOPES = ("full", "filtered", "metadata-only")
PAUSE_REASONS = ("budget_exhausted", "outside_schedule", "cancelled", "link_down")
DECISIONS = (
    "create", "fast_forward", "unchanged", "local_ahead", "conflict", "checked_out",
)

_HEX = re.compile(r"^[0-9a-f]{40}([0-9a-f]{24})?$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_SIZE = re.compile(r"^(\d+)\s*([kmg]?)(i?b)?$", re.I)
_WINDOW = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)-([01]\d|2[0-3]):([0-5]\d)$")
_REFNAME = re.compile(r"^refs/[\x21-\x7e]+$")


class UsageError(Exception):
    """Bad arguments or unreadable input (exit 2)."""


class VerificationError(Exception):
    """Provenance, integrity, or object checks failed; nothing changed (exit 3)."""

    def __init__(self, problems):
        self.problems = list(problems)
        super().__init__("; ".join(self.problems))


class Disconnect(Exception):
    """The link dropped mid-read; the transfer can resume."""


class GitError(Exception):
    pass


# --- Small shared helpers ---------------------------------------------------

def canonical_json(value) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def _b64encode(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def _b64decode(value) -> bytes:
    value = str(value or "").strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]*", value):
        raise ValueError("not base64url")
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def _now_ms() -> int:
    return int(time.time() * 1000)


def parse_size(text) -> int:
    """Parse `512k`, `10M`, `1GiB`, or a plain byte count (binary units)."""
    match = _SIZE.match(str(text or "").strip())
    if not match:
        raise UsageError("not a size: %r (use bytes, or k/M/G)" % (text,))
    return int(match.group(1)) * {"": 1, "k": 1 << 10, "m": 1 << 20, "g": 1 << 30}[
        match.group(2).lower()]


def _write_json_atomic(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(value, handle, indent=2, sort_keys=True)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)


def _read_json(path: Path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read %s: %s" % (path, exc)) from exc


def _append_telemetry(path, event, fields) -> None:
    """Append one count-only record. Never names repositories, refs, or keys."""
    if not path:
        return
    record = {"schema": TELEMETRY_SCHEMA, "event": event, "at": _now_ms()}
    record.update(fields)
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, sort_keys=True) + "\n")


# --- Ed25519 provenance -----------------------------------------------------

def _encode_point(point) -> bytes:
    x, y = point
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


def ed25519_public_key(seed: bytes) -> bytes:
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    return _encode_point(_ed25519._mult(_ed25519._BASE, scalar))


def ed25519_sign(seed: bytes, message: bytes) -> bytes:
    """RFC 8032 signing. Uses `cryptography` when present, else pure Python."""
    try:
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    except ImportError:
        pass
    else:
        return Ed25519PrivateKey.from_private_bytes(seed).sign(message)
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar &= (1 << 254) - 8
    scalar |= 1 << 254
    public = _encode_point(_ed25519._mult(_ed25519._BASE, scalar))
    nonce = int.from_bytes(hashlib.sha512(digest[32:] + message).digest(), "little") % _ed25519._L
    encoded_nonce = _encode_point(_ed25519._mult(_ed25519._BASE, nonce))
    challenge = int.from_bytes(
        hashlib.sha512(encoded_nonce + public + message).digest(), "little") % _ed25519._L
    return encoded_nonce + ((nonce + challenge * scalar) % _ed25519._L).to_bytes(32, "little")


def signing_message(manifest_sha256: str) -> bytes:
    return (MEDIA_SCHEMA + "\n" + manifest_sha256).encode("ascii")


def load_seed(path) -> bytes:
    try:
        seed = _b64decode(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError("cannot read signing key %s: %s" % (path, exc)) from exc
    if len(seed) != 32:
        raise UsageError("the signing key must be a base64url 32-byte Ed25519 seed")
    return seed


def parse_trusted_keys(values):
    keys = set()
    for value in values or ():
        try:
            raw = _b64decode(value)
        except ValueError:
            raw = b""
        if len(raw) != 32:
            raise UsageError("a trusted key must be a base64url 32-byte Ed25519 public key")
        keys.add(raw)
    return keys


def sign_manifest(manifest, seed: bytes, key_id: str = ""):
    manifest_sha256 = hashlib.sha256(canonical_json(manifest)).hexdigest()
    public = ed25519_public_key(seed)
    return {
        "schema": SIGNED_SCHEMA,
        "manifest": manifest,
        "manifestSha256": manifest_sha256,
        "signature": {
            "algorithm": "ed25519",
            "keyId": key_id or _b64encode(public)[:16],
            "publicKey": _b64encode(public),
            "signature": _b64encode(ed25519_sign(seed, signing_message(manifest_sha256))),
        },
    }


def _manifest_problems(manifest):
    if not isinstance(manifest, dict) or manifest.get("schema") != MEDIA_SCHEMA:
        return ["not a %s manifest" % MEDIA_SCHEMA]
    problems = []
    bundle = manifest.get("bundle")
    if not isinstance(bundle, dict):
        return ["the manifest has no bundle section"]
    size, chunk_size, chunks = bundle.get("size"), bundle.get("chunkSize"), bundle.get("chunks")
    if bundle.get("file") != BUNDLE_FILE:
        problems.append("the bundle file name must be %s" % BUNDLE_FILE)
    if not (isinstance(size, int) and size > 0):
        problems.append("the bundle size is invalid")
    if not (isinstance(chunk_size, int) and MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE):
        problems.append("the chunk size is invalid")
    if not _SHA256.match(str(bundle.get("sha256") or "")):
        problems.append("the bundle digest is invalid")
    if not problems and (
            not isinstance(chunks, list) or len(chunks) != math.ceil(size / chunk_size)
            or not all(_SHA256.match(str(c)) for c in chunks)):
        problems.append("the chunk digests do not cover the bundle")
    refs = manifest.get("refs")
    if not isinstance(refs, dict) or not refs:
        problems.append("the manifest lists no refs")
    else:
        for name, oid in refs.items():
            if not _REFNAME.match(name) or ".." in name or not _HEX.match(str(oid)):
                problems.append("invalid ref entry %r" % (name,))
                break
    if not all(_HEX.match(str(o)) for o in manifest.get("prerequisites") or []):
        problems.append("invalid prerequisite")
    replica = manifest.get("replica")
    if not isinstance(replica, dict) or replica != replica_scope(manifest.get("selection")):
        problems.append("the replica advertisement does not match the selection")
    return problems


def verify_envelope(envelope, trusted_keys=frozenset()):
    """Return (manifest, provenance, problems) for a signed media envelope."""
    if not isinstance(envelope, dict) or envelope.get("schema") != SIGNED_SCHEMA:
        return None, None, ["not a %s document" % SIGNED_SCHEMA]
    manifest = envelope.get("manifest")
    problems = _manifest_problems(manifest)
    manifest_sha256 = hashlib.sha256(canonical_json(manifest)).hexdigest()
    if envelope.get("manifestSha256") != manifest_sha256:
        problems.append("manifestSha256 does not match the manifest")
    signature = envelope.get("signature") if isinstance(envelope.get("signature"), dict) else {}
    provenance = {
        "algorithm": signature.get("algorithm"),
        "keyId": str(signature.get("keyId") or ""),
        "publicKey": str(signature.get("publicKey") or ""),
        "signatureValid": False,
        "trusted": False,
        "manifestSha256": manifest_sha256,
        "exportId": manifest_sha256[:16],
    }
    if signature.get("algorithm") != "ed25519":
        return manifest, provenance, problems + ["unsupported signature algorithm"]
    try:
        public = _b64decode(signature.get("publicKey"))
        raw_signature = _b64decode(signature.get("signature"))
    except ValueError:
        return manifest, provenance, problems + ["signature fields are not base64url"]
    provenance["signatureValid"] = _ed25519.ed25519_verify(
        public, raw_signature, signing_message(manifest_sha256))
    if not provenance["signatureValid"]:
        problems.append("the signature does not verify")
    provenance["trusted"] = bool(provenance["signatureValid"] and public in trusted_keys)
    if trusted_keys and provenance["signatureValid"] and not provenance["trusted"]:
        problems.append("the export was signed by a key that is not trusted")
    return manifest, provenance, problems


def bundle_problems(path: Path, manifest):
    bundle = manifest["bundle"]
    try:
        actual = path.stat().st_size
    except OSError:
        return ["%s is missing" % BUNDLE_FILE]
    if actual != bundle["size"]:
        return ["%s is %d bytes, the manifest says %d" % (BUNDLE_FILE, actual, bundle["size"])]
    problems = []
    whole = hashlib.sha256()
    with open(path, "rb") as handle:
        for index, expected in enumerate(bundle["chunks"]):
            data = handle.read(bundle["chunkSize"])
            whole.update(data)
            if hashlib.sha256(data).hexdigest() != expected:
                problems.append("chunk %d is corrupt" % index)
    if whole.hexdigest() != bundle["sha256"]:
        problems.append("the bundle digest does not match")
    return problems


# --- Selection and replica advertisement ------------------------------------

def normalize_selection(refs=(), paths=(), metadata_only=False, defer_blobs_over=None):
    clean_paths = []
    for path in paths or ():
        text = str(path).replace("\\", "/").strip().strip("/")
        if not text or "\n" in text or text.startswith(("!", "#")) or ".." in text.split("/"):
            raise UsageError("invalid path filter %r" % (path,))
        clean_paths.append(text)
    if defer_blobs_over is not None and defer_blobs_over < 0:
        raise UsageError("--defer-blobs-over must not be negative")
    return {
        "refs": sorted(set(refs or ())),
        "paths": sorted(set(clean_paths)),
        "metadataOnly": bool(metadata_only),
        "deferBlobsOver": defer_blobs_over,
    }


def replica_scope(selection, missing_objects=0):
    """What a replica built from this selection may claim. Fails closed."""
    selection = selection if isinstance(selection, dict) else None
    if selection is None:
        return {"schema": REPLICA_SCHEMA, "scope": "filtered", "limitations": ["missing_objects"],
                "claimsFullAvailability": False}
    found = set()
    if selection.get("metadataOnly"):
        found.add("metadata_only")
    if selection.get("paths"):
        found.add("path_filter")
    if selection.get("refs"):
        found.add("branch_filter")
    if selection.get("deferBlobsOver") is not None:
        found.add("deferred_large_objects")
    if missing_objects:
        found.add("missing_objects")
    limitations = [code for code in LIMITATIONS if code in found]
    if not limitations:
        scope = "full"
    elif "metadata_only" in found:
        scope = "metadata-only"
    else:
        scope = "filtered"
    return {
        "schema": REPLICA_SCHEMA,
        "scope": scope,
        "limitations": limitations,
        "claimsFullAvailability": scope == "full",
    }


def sparse_patterns(selection):
    """Non-cone sparse patterns; collaboration records are always kept."""
    if selection["metadataOnly"]:
        return [COLLABORATION_PATH]
    if selection["paths"]:
        return [COLLABORATION_PATH] + ["/" + path for path in selection["paths"]]
    return []


# --- Bandwidth schedule and rate --------------------------------------------

def parse_window(text):
    match = _WINDOW.match(str(text or "").strip())
    if not match:
        raise UsageError("a schedule window is HH:MM-HH:MM, got %r" % (text,))
    h1, m1, h2, m2 = (int(g) for g in match.groups())
    return h1 * 60 + m1, h2 * 60 + m2


def window_allows(windows, moment: _dt.datetime) -> bool:
    if not windows:
        return True
    minute = moment.hour * 60 + moment.minute
    for start, end in windows:
        if start == end:
            return True
        if start < end and start <= minute < end:
            return True
        if start > end and (minute >= start or minute < end):
            return True
    return False


def next_window_start(windows, moment: _dt.datetime) -> _dt.datetime:
    base = moment.replace(second=0, microsecond=0)
    candidates = []
    for start, _ in windows:
        candidate = base.replace(hour=start // 60, minute=start % 60)
        if candidate <= moment:
            candidate += _dt.timedelta(days=1)
        candidates.append(candidate)
    return min(candidates)


# --- Git ----------------------------------------------------------------------

def _git_env():
    env = dict(os.environ)
    env.update({"GIT_TERMINAL_PROMPT": "0", "GIT_NO_LAZY_FETCH": "1", "LC_ALL": "C"})
    return env


def git(repo, *args, stdin=None, check=True):
    completed = subprocess.run(
        ["git", "-C", str(repo), *args], input=stdin, capture_output=True,
        env=_git_env(), check=False,
    )
    if check and completed.returncode != 0:
        raise GitError("git %s failed: %s" % (
            " ".join(args[:2]), completed.stderr.decode("utf-8", "replace").strip()))
    return completed


def _git_text(repo, *args, stdin=None):
    return git(repo, *args, stdin=stdin).stdout.decode("utf-8", "replace")


def git_dir(repo) -> Path:
    try:
        return Path(_git_text(repo, "rev-parse", "--absolute-git-dir").strip())
    except (GitError, OSError) as exc:
        raise UsageError("%s is not a Git repository" % repo) from exc


def list_refs(repo):
    refs = {}
    for line in _git_text(repo, "for-each-ref", "--format=%(refname) %(objectname)").splitlines():
        name, _, oid = line.partition(" ")
        if name and oid:
            refs[name] = oid
    return refs


def missing_objects(repo, oids, *, filter_blobs=False):
    if not oids:
        return []
    args = ["rev-list", "--objects", "--missing=print", "--stdin"]
    if filter_blobs:
        args.append("--filter=blob:none")
    out = _git_text(repo, *args, stdin="\n".join(oids).encode() + b"\n")
    return [line[1:].strip() for line in out.splitlines() if line.startswith("?")]


def _present(repo, oids):
    if not oids:
        return set()
    out = _git_text(repo, "cat-file", "--batch-check=%(objectname) %(objecttype)",
                    stdin="\n".join(oids).encode() + b"\n")
    return {line.split()[0] for line in out.splitlines() if not line.endswith(" missing")}


def _is_ancestor(repo, ancestor, descendant) -> bool:
    return git(repo, "merge-base", "--is-ancestor", ancestor, descendant, check=False).returncode == 0


def read_bundle_header(path: Path):
    heads, prerequisites, capabilities = {}, [], {}
    with open(path, "rb") as handle:
        first = handle.readline().decode("utf-8", "replace").strip()
        if first not in ("# v2 git bundle", "# v3 git bundle"):
            raise VerificationError(["%s is not a Git bundle" % BUNDLE_FILE])
        while True:
            line = handle.readline()
            if not line or line == b"\n":
                break
            text = line.decode("utf-8", "replace").rstrip("\n")
            if text.startswith("@"):
                key, _, value = text[1:].partition("=")
                capabilities[key] = value
            elif text.startswith("-"):
                prerequisites.append(text[1:].split(" ", 1)[0])
            else:
                oid, _, name = text.partition(" ")
                heads[name] = oid
    return heads, prerequisites, capabilities


# --- Export -----------------------------------------------------------------

def _select_refs(all_refs, patterns):
    selected = {}
    for name, oid in sorted(all_refs.items()):
        if name.startswith(RESERVED_REF_PREFIX) or name.startswith("refs/remotes/") or name == "refs/stash":
            continue
        if patterns and not any(fnmatch.fnmatchcase(name, p) for p in patterns):
            continue
        selected[name] = oid
    return selected


def export_media(repo, media_dir, seed, *, selection, since=None, chunk_size=DEFAULT_CHUNK_SIZE,
                 repository="", key_id="", now_ms=None):
    repo, media_dir = Path(repo), Path(media_dir)
    git_dir(repo)
    if not MIN_CHUNK_SIZE <= chunk_size <= MAX_CHUNK_SIZE:
        raise UsageError("--chunk-size must be between 1k and 64M")
    if (media_dir / MANIFEST_FILE).exists():
        raise UsageError("%s already holds an export; use a new directory" % media_dir)
    refs = _select_refs(list_refs(repo), selection["refs"])
    if not refs:
        raise UsageError("no refs match the selection")

    previous_digest, prerequisites = None, []
    if since is not None:
        previous, _, problems = verify_envelope(_read_json(Path(since) / MANIFEST_FILE))
        if problems:
            raise UsageError("the previous export does not verify: " + "; ".join(problems))
        previous_digest = hashlib.sha256(canonical_json(previous)).hexdigest()
        prerequisites = sorted(_present(repo, sorted(set(previous["refs"].values()))))
        if refs == previous["refs"]:
            return None
        new_objects = _git_text(repo, "rev-list", "--count", "--stdin", stdin=(
            "\n".join(list(refs.values()) + ["^" + oid for oid in prerequisites]) + "\n").encode())
        if int(new_objects.strip() or 0) == 0:
            raise UsageError("refs changed without new commits; make a full export (omit --since)")

    filters = []
    patterns = sparse_patterns(selection)
    if patterns:
        spec = _git_text(repo, "hash-object", "-w", "--stdin",
                         stdin=("\n".join(patterns) + "\n").encode()).strip()
        filters.append("sparse:oid=" + spec)
    if selection["deferBlobsOver"] is not None:
        filters.append("blob:limit=%d" % selection["deferBlobsOver"])
    filter_spec = None
    if len(filters) == 1:
        filter_spec = filters[0]
    elif filters:
        filter_spec = "combine:" + "+".join(filters)

    revisions = "\n".join(list(refs) + ["^" + oid for oid in prerequisites]) + "\n"
    omitted = {"objects": 0, "bytes": 0}
    if filter_spec:
        listed = _git_text(repo, "rev-list", "--objects", "--stdin", "--filter=" + filter_spec,
                           "--filter-print-omitted", stdin=revisions.encode())
        omitted_oids = [line[1:].strip() for line in listed.splitlines() if line.startswith("~")]
        if omitted_oids:
            sizes = _git_text(repo, "cat-file", "--batch-check=%(objectsize)",
                              stdin="\n".join(omitted_oids).encode() + b"\n")
            omitted = {"objects": len(omitted_oids),
                       "bytes": sum(int(s) for s in sizes.split() if s.isdigit())}

    media_dir.mkdir(parents=True, exist_ok=True)
    partial = media_dir / (BUNDLE_FILE + ".partial")
    args = ["bundle", "create", "--quiet", str(partial.resolve())]
    if filter_spec:
        args.append("--filter=" + filter_spec)
    git(repo, *args, "--stdin", stdin=revisions.encode())

    chunks, whole, size = [], hashlib.sha256(), 0
    with open(partial, "rb") as handle:
        while True:
            data = handle.read(chunk_size)
            if not data:
                break
            chunks.append(hashlib.sha256(data).hexdigest())
            whole.update(data)
            size += len(data)
    _, bundle_prerequisites, _ = read_bundle_header(partial)
    head = git(repo, "symbolic-ref", "-q", "HEAD", check=False).stdout.decode().strip()
    manifest = {
        "schema": MEDIA_SCHEMA,
        "repository": str(repository or ""),
        "createdAt": now_ms if now_ms is not None else _now_ms(),
        "objectFormat": _git_text(repo, "rev-parse", "--show-object-format").strip() or "sha1",
        "head": head if head in refs else None,
        "refs": refs,
        "prerequisites": sorted(set(bundle_prerequisites)),
        "previousExport": previous_digest,
        "selection": selection,
        "replica": replica_scope(selection),
        "filter": filter_spec,
        "omitted": omitted,
        "bundle": {"file": BUNDLE_FILE, "size": size, "sha256": whole.hexdigest(),
                   "chunkSize": chunk_size, "chunks": chunks},
    }
    envelope = sign_manifest(manifest, seed, key_id)
    os.replace(partial, media_dir / BUNDLE_FILE)
    _write_json_atomic(media_dir / MANIFEST_FILE, envelope)
    return envelope


# --- Resumable transfer -----------------------------------------------------

class LocalSource:
    """A media directory on removable or network-mounted storage."""

    def __init__(self, root):
        self.root = Path(root)
        self.label = "local"

    def read_manifest(self) -> bytes:
        try:
            with open(self.root / MANIFEST_FILE, "rb") as handle:
                return handle.read(MAX_MANIFEST_BYTES + 1)
        except FileNotFoundError as exc:
            raise VerificationError(["%s has no %s" % (self.root, MANIFEST_FILE)]) from exc
        except OSError as exc:
            raise Disconnect(str(exc)) from exc

    def read_range(self, start, end):
        try:
            with open(self.root / BUNDLE_FILE, "rb") as handle:
                handle.seek(start)
                remaining = end - start + 1
                while remaining > 0:
                    data = handle.read(min(BLOCK_SIZE, remaining))
                    if not data:
                        raise Disconnect("the media ended early")
                    remaining -= len(data)
                    yield data
        except OSError as exc:
            raise Disconnect(str(exc)) from exc


class HttpSource:
    """A media directory on any static HTTP host that honours Range."""

    def __init__(self, url, timeout=30.0):
        self.base = url if url.endswith("/") else url + "/"
        self.timeout = timeout
        self.label = "http"

    def _open(self, name, headers):
        request = urllib.request.Request(self.base + name, headers=headers)
        try:
            return urllib.request.urlopen(request, timeout=self.timeout)  # noqa: S310 - http(s) only
        except urllib.error.HTTPError as exc:
            if exc.code >= 500 or exc.code in (408, 429):
                raise Disconnect("HTTP %d" % exc.code) from exc
            raise VerificationError(["%s returned HTTP %d" % (name, exc.code)]) from exc
        except (urllib.error.URLError, OSError, http.client.HTTPException) as exc:
            raise Disconnect(str(exc)) from exc

    def read_manifest(self) -> bytes:
        response = self._open(MANIFEST_FILE, {"Accept-Encoding": "identity"})
        try:
            with response:
                return response.read(MAX_MANIFEST_BYTES + 1)
        except (OSError, http.client.HTTPException) as exc:
            raise Disconnect(str(exc)) from exc

    def read_range(self, start, end):
        response = self._open(BUNDLE_FILE, {
            "Range": "bytes=%d-%d" % (start, end), "Accept-Encoding": "identity"})
        with response:
            content_range = response.headers.get("Content-Range", "")
            if response.status != 206 or not content_range.startswith("bytes %d-%d/" % (start, end)):
                raise VerificationError([
                    "the server ignored the Range request; resumable fetch needs Range support"])
            remaining = end - start + 1
            while remaining > 0:
                try:
                    data = response.read(min(BLOCK_SIZE, remaining))
                except (OSError, http.client.HTTPException) as exc:
                    raise Disconnect(str(exc)) from exc
                if not data:
                    raise Disconnect("the connection closed mid-chunk")
                remaining -= len(data)
                yield data


def open_source(location):
    text = str(location)
    scheme = urllib.parse.urlparse(text).scheme.lower()
    if scheme in ("http", "https"):
        return HttpSource(text)
    if scheme and len(scheme) > 1:
        raise UsageError("unsupported source scheme %r" % scheme)
    return LocalSource(text)


def _new_counters():
    return {"sessions": 0, "resumes": 0, "disconnects": 0, "bytesTransferred": 0,
            "manifestBytes": 0, "bytesResumed": 0, "integrityFailures": 0,
            "retransferredBytes": 0}


class Transfer:
    """Fetch one media directory into DEST in verified, checkpointed chunks.

    Progress survives disconnects, crashes, and cancellation: a chunk is
    appended to its `.part` file as bytes arrive, resumes from the byte it
    stopped at, and is renamed to `.done` only after its digest matches the
    signed manifest. Verified chunks are never requested again.
    """

    def __init__(self, source, dest, *, trusted_keys=frozenset(), allow_unpinned=True,
                 rate=None, budget=None, windows=(), wait=False, retries=8, restart=False,
                 clock=time.monotonic, sleep=time.sleep, now=_dt.datetime.now,
                 should_stop=lambda: False, backoff_cap=30.0):
        self.source = source
        self.dest = Path(dest)
        self.work = self.dest / TRANSFER_DIR
        self.trusted_keys = trusted_keys
        self.allow_unpinned = allow_unpinned
        self.rate = rate
        self.budget = budget
        self.windows = list(windows)
        self.wait = wait
        self.retries = retries
        self.restart = restart
        self.clock, self.sleep, self.now = clock, sleep, now
        self.should_stop = should_stop
        self.backoff_cap = backoff_cap
        self.checkpoint = None
        self.manifest = None
        self.provenance = None
        self._run_bytes = 0
        self._rate_bytes = 0
        self._rate_started = None

    # -- persistence
    def _checkpoint_path(self):
        return self.work / "checkpoint.json"

    def _save(self):
        self.checkpoint["updatedAt"] = _now_ms()
        _write_json_atomic(self._checkpoint_path(), self.checkpoint)

    def _chunk_path(self, index, suffix):
        return self.work / "chunks" / ("%08d.%s" % (index, suffix))

    def _chunk_bounds(self, index):
        bundle = self.manifest["bundle"]
        start = index * bundle["chunkSize"]
        return start, min(bundle["size"], start + bundle["chunkSize"]) - 1

    # -- flow control
    def _pause_reason(self):
        if self.should_stop():
            return "cancelled"
        if self.windows and not window_allows(self.windows, self.now()):
            if not self.wait:
                return "outside_schedule"
            moment = self.now()
            self.sleep(max(1.0, (next_window_start(self.windows, moment) - moment).total_seconds()))
            return self._pause_reason()
        if self.budget is not None and self._run_bytes >= self.budget:
            return "budget_exhausted"
        return None

    def _throttle(self, count):
        if not self.rate:
            return
        if self._rate_started is None:
            self._rate_started = self.clock()
        self._rate_bytes += count
        due = self._rate_started + self._rate_bytes / float(self.rate)
        delay = due - self.clock()
        if delay > 0:
            self.sleep(delay)

    def _backoff(self, failures):
        self.sleep(min(self.backoff_cap, 0.5 * (2 ** min(failures - 1, 10))))

    # -- manifest
    def _load_manifest(self):
        stored = self.work / MANIFEST_FILE
        if self.restart and self.work.exists():
            shutil.rmtree(self.work)
        if stored.exists() and self._checkpoint_path().exists():
            # Never re-request the manifest once it is on disk: on a metered
            # link that would be a duplicate transfer every session.
            raw = stored.read_bytes()
            envelope = _read_json(stored)
            self.checkpoint = _read_json(self._checkpoint_path())
        else:
            failures = 0
            while True:
                try:
                    raw = self.source.read_manifest()
                    break
                except Disconnect:
                    failures += 1
                    if failures > self.retries:
                        return "link_down"
                    self._backoff(failures)
            if len(raw) > MAX_MANIFEST_BYTES:
                raise VerificationError(["%s is too large" % MANIFEST_FILE])
            try:
                envelope = json.loads(raw.decode("utf-8"))
            except ValueError as exc:
                raise VerificationError(["%s is not JSON" % MANIFEST_FILE]) from exc
            self.checkpoint = None
        manifest, provenance, problems = verify_envelope(envelope, self.trusted_keys)
        if not problems and not provenance["trusted"] and not self.allow_unpinned:
            problems.append("the export is not signed by a trusted key")
        if problems:
            raise VerificationError(problems)
        self.manifest, self.provenance = manifest, provenance
        if self.checkpoint is None or self.checkpoint.get("manifestSha256") != provenance["manifestSha256"]:
            self.checkpoint = {
                "schema": CHECKPOINT_SCHEMA,
                "manifestSha256": provenance["manifestSha256"],
                "state": "in_progress",
                "reason": None,
                "verifiedChunks": [],
                "counters": _new_counters(),
                "lastRateBytesPerSecond": None,
            }
            self.checkpoint["counters"]["manifestBytes"] = len(raw)
            self.work.mkdir(parents=True, exist_ok=True)
            _write_json_atomic(stored, envelope)
            self._save()
        return None

    # -- chunks
    def _fetch_chunk(self, index):
        """Append the rest of one chunk to its part file. Returns True when full."""
        start, end = self._chunk_bounds(index)
        part = self._chunk_path(index, "part")
        part.parent.mkdir(parents=True, exist_ok=True)
        have = part.stat().st_size if part.exists() else 0
        length = end - start + 1
        if have > length:
            os.truncate(part, 0)
            have = 0
        if have == length:
            return True
        counters = self.checkpoint["counters"]
        request_end = end
        if self.budget is not None:
            request_end = min(end, start + have + (self.budget - self._run_bytes) - 1)
        received = 0
        try:
            with open(part, "ab") as handle:
                for block in self.source.read_range(start + have, request_end):
                    handle.write(block)
                    handle.flush()
                    received += len(block)
                    self._run_bytes += len(block)
                    counters["bytesTransferred"] += len(block)
                    self._throttle(len(block))
                    if self.should_stop():
                        break
        finally:
            self._last_received = received
        return have + received == length

    def _verify_chunk(self, index):
        part = self._chunk_path(index, "part")
        with open(part, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        if digest == self.manifest["bundle"]["chunks"][index]:
            os.replace(part, self._chunk_path(index, "done"))
            return True
        counters = self.checkpoint["counters"]
        counters["integrityFailures"] += 1
        counters["retransferredBytes"] += part.stat().st_size
        part.unlink()
        return False

    def _adopt_existing(self):
        """Count work already on disk; re-check every finished chunk's digest."""
        verified = set(self.checkpoint["verifiedChunks"])
        resumed = 0
        for index, expected in enumerate(self.manifest["bundle"]["chunks"]):
            done = self._chunk_path(index, "done")
            if done.exists():
                with open(done, "rb") as handle:
                    ok = hashlib.sha256(handle.read()).hexdigest() == expected
                if ok:
                    verified.add(index)
                    resumed += done.stat().st_size
                    continue
                done.unlink()
                self.checkpoint["counters"]["integrityFailures"] += 1
            verified.discard(index)
            part = self._chunk_path(index, "part")
            if part.exists():
                resumed += part.stat().st_size
        self.checkpoint["verifiedChunks"] = sorted(verified)
        return resumed

    def _assemble(self):
        partial = self.dest / (BUNDLE_FILE + ".partial")
        whole = hashlib.sha256()
        with open(partial, "wb") as output:
            for index in range(len(self.manifest["bundle"]["chunks"])):
                with open(self._chunk_path(index, "done"), "rb") as handle:
                    data = handle.read()
                whole.update(data)
                output.write(data)
            output.flush()
            os.fsync(output.fileno())
        if whole.hexdigest() != self.manifest["bundle"]["sha256"]:
            partial.unlink()
            raise VerificationError(["the assembled bundle digest does not match"])
        os.replace(partial, self.dest / BUNDLE_FILE)
        shutil.copyfile(self.work / MANIFEST_FILE, self.dest / MANIFEST_FILE)
        shutil.rmtree(self.work / "chunks", ignore_errors=True)

    def run(self):
        self.dest.mkdir(parents=True, exist_ok=True)
        reason = self._pause_reason() if self.windows or self.should_stop() else None
        if reason:
            return self._result("paused", reason)
        link = self._load_manifest()
        if link:
            return self._result("paused", link)
        counters = self.checkpoint["counters"]
        if self.checkpoint["state"] == "complete" and (self.dest / BUNDLE_FILE).exists():
            return self._result("complete", None)
        counters["sessions"] += 1
        resumed = self._adopt_existing()
        if counters["sessions"] > 1 or resumed:
            counters["resumes"] += 1
            counters["bytesResumed"] += resumed
        self.checkpoint["state"] = "in_progress"
        started, session_bytes = self.clock(), counters["bytesTransferred"]
        try:
            outcome = self._run_chunks()
        finally:
            elapsed = self.clock() - started
            sent = counters["bytesTransferred"] - session_bytes
            if elapsed > 0 and sent:
                self.checkpoint["lastRateBytesPerSecond"] = round(sent / elapsed, 1)
            self._save()
        return outcome

    def _run_chunks(self):
        verified = set(self.checkpoint["verifiedChunks"])
        counters = self.checkpoint["counters"]
        attempts = {}
        failures = 0
        for index in range(len(self.manifest["bundle"]["chunks"])):
            while index not in verified:
                reason = self._pause_reason()
                if reason:
                    return self._result("paused", reason)
                try:
                    full = self._fetch_chunk(index)
                except Disconnect:
                    counters["disconnects"] += 1
                    failures = 1 if self._last_received else failures + 1
                    self._save()
                    if failures > self.retries:
                        return self._result("paused", "link_down")
                    self._backoff(failures)
                    continue
                if self._last_received:
                    failures = 0
                if not full:
                    continue
                if self._verify_chunk(index):
                    verified.add(index)
                    self.checkpoint["verifiedChunks"] = sorted(verified)
                    self._save()
                    continue
                attempts[index] = attempts.get(index, 0) + 1
                self._save()
                if attempts[index] >= CHUNK_INTEGRITY_ATTEMPTS:
                    return self._result(
                        "failed", "integrity",
                        ["chunk %d failed its digest %d times; the source export changed "
                         "or is corrupt (rerun with --restart)" % (index, attempts[index])])
        self._assemble()
        return self._result("complete", None)

    def _result(self, state, reason, problems=()):
        if self.checkpoint is not None:
            self.checkpoint["state"] = state
            self.checkpoint["reason"] = reason
            if self.work.exists():
                self._save()
        result = {
            "schema": RESULT_SCHEMA,
            "state": state,
            "reason": reason,
            "problems": list(problems),
            "provenance": self.provenance,
            "replica": self.manifest["replica"] if self.manifest else None,
            "estimate": estimate(self.dest) if self.manifest else None,
            "counters": dict(self.checkpoint["counters"]) if self.checkpoint else _new_counters(),
            "nextWindowAt": None,
        }
        if reason == "outside_schedule":
            result["nextWindowAt"] = next_window_start(self.windows, self.now()).isoformat()
        return result


def estimate(dest):
    """Transfer estimate from what is on disk; never contacts the source."""
    dest = Path(dest)
    work = dest / TRANSFER_DIR
    if (dest / MANIFEST_FILE).exists() and not (work / "chunks").exists():
        envelope = _read_json(dest / MANIFEST_FILE)
        size = envelope["manifest"]["bundle"]["size"]
        count = len(envelope["manifest"]["bundle"]["chunks"])
        return {"totalBytes": size, "bytesOnDisk": size, "remainingBytes": 0,
                "chunks": count, "verifiedChunks": count, "percent": 100.0,
                "etaSeconds": 0, "checkpoint": None}
    checkpoint = _read_json(work / "checkpoint.json")
    envelope = _read_json(work / MANIFEST_FILE)
    bundle = envelope["manifest"]["bundle"]
    on_disk = 0
    for index in range(len(bundle["chunks"])):
        for suffix in ("done", "part"):
            path = work / "chunks" / ("%08d.%s" % (index, suffix))
            if path.exists():
                on_disk += path.stat().st_size
                break
    remaining = max(0, bundle["size"] - on_disk)
    rate = checkpoint.get("lastRateBytesPerSecond")
    return {
        "totalBytes": bundle["size"],
        "bytesOnDisk": on_disk,
        "remainingBytes": remaining,
        "chunks": len(bundle["chunks"]),
        "verifiedChunks": len(checkpoint.get("verifiedChunks") or []),
        "percent": round(100.0 * on_disk / bundle["size"], 1),
        "etaSeconds": math.ceil(remaining / rate) if rate else None,
        "checkpoint": str(work / "checkpoint.json"),
    }


# --- Import, reconciliation, and rollback -----------------------------------

def _state_dir(repo) -> Path:
    return git_dir(repo) / STATE_DIR


def replica_state(repo):
    """The replica advertisement this repository may publish."""
    path = _state_dir(repo) / "replica.json"
    recorded = _read_json(path) if path.exists() else None
    limitations = set(recorded.get("limitations") or []) if isinstance(recorded, dict) else set()
    refs = list_refs(repo)
    oids = sorted({oid for name, oid in refs.items() if not name.startswith(RESERVED_REF_PREFIX)})
    missing = len(missing_objects(repo, oids)) if oids else 0
    if missing:
        limitations.add("missing_objects")
    else:
        limitations.discard("missing_objects")
    selection = {
        "refs": ["*"] if "branch_filter" in limitations else [],
        "paths": ["*"] if "path_filter" in limitations else [],
        "metadataOnly": "metadata_only" in limitations,
        "deferBlobsOver": 0 if "deferred_large_objects" in limitations else None,
    }
    advertisement = replica_scope(selection, missing)
    advertisement["missingObjects"] = missing
    return advertisement


def _reconcile(repo, manifest, local_refs, checked_out):
    plan = []
    for name, incoming in sorted(manifest["refs"].items()):
        if name.startswith(RESERVED_REF_PREFIX):
            continue
        local = local_refs.get(name)
        if local is None:
            decision = "create"
        elif local == incoming:
            decision = "unchanged"
        elif name.startswith("refs/tags/"):
            decision = "conflict"
        elif _is_ancestor(repo, local, incoming):
            decision = "checked_out" if name == checked_out else "fast_forward"
        elif _is_ancestor(repo, incoming, local):
            decision = "local_ahead"
        else:
            decision = "conflict"
        plan.append({"ref": name, "local": local, "incoming": incoming, "decision": decision})
    return plan


def _conflict_ref(export_id, name):
    return "%sconflicts/%s/%s" % (RESERVED_REF_PREFIX, export_id, name[len("refs/"):])


def import_media(media_dir, repo, *, trusted_keys=frozenset(), allow_unpinned=False,
                 allow_limited=False, now_ms=None):
    media_dir, repo = Path(media_dir), Path(repo)
    state = _state_dir(repo)
    manifest, provenance, problems = verify_envelope(
        _read_json(media_dir / MANIFEST_FILE), trusted_keys)
    if not problems and not provenance["trusted"] and not allow_unpinned:
        problems.append("the export is not signed by a trusted key (pass --trusted-key)")
    if problems:
        raise VerificationError(problems)
    problems = bundle_problems(media_dir / BUNDLE_FILE, manifest)
    if problems:
        raise VerificationError(problems)
    limited = manifest["replica"]["scope"] != "full"
    if limited and not allow_limited:
        raise VerificationError([
            "this export is %s (%s); pass --allow-limited to import it and advertise "
            "the same limitations" % (
                manifest["replica"]["scope"], ", ".join(manifest["replica"]["limitations"]))])
    object_format = _git_text(repo, "rev-parse", "--show-object-format").strip() or "sha1"
    if object_format != manifest["objectFormat"]:
        raise VerificationError(["the export uses %s objects; this repository uses %s" % (
            manifest["objectFormat"], object_format)])
    absent = sorted(set(manifest["prerequisites"]) - _present(repo, manifest["prerequisites"]))
    if absent:
        raise VerificationError([
            "missing %d prerequisite commit(s); import the earlier export first "
            "(previousExport %s)" % (len(absent), manifest["previousExport"])])

    # Unbundle rather than fetch: fetch skips a pack whose tips are already
    # present, which would leave a partial repository's missing files missing.
    # Completeness is checked below instead of by fetch's connectivity check.
    local_before = list_refs(repo)
    partial = bool(git(repo, "config", "--get", "extensions.partialClone", check=False).stdout.strip())
    packs = git_dir(repo) / "objects" / "pack"
    before = set(packs.glob("*.pack")) if packs.exists() else set()
    git(repo, "bundle", "unbundle", str((media_dir / BUNDLE_FILE).resolve()))
    if limited:
        for pack in set(packs.glob("*.pack")) - before:
            pack.with_suffix(".promisor").touch()
        if not partial:
            git(repo, "config", "core.repositoryformatversion", "1")
            git(repo, "config", "extensions.partialClone", PROMISOR_REMOTE)
            git(repo, "config", "remote.%s.promisor" % PROMISOR_REMOTE, "true")
    incoming = sorted(set(manifest["refs"].values()))
    broken = missing_objects(repo, incoming, filter_blobs=True)
    if broken:
        raise VerificationError([
            "%d commit or tree object(s) are missing after unpacking" % len(broken)])
    if limited:
        deferred = len(missing_objects(repo, incoming))
    else:
        # A partial repository cannot vouch for objects behind its own refs.
        known = [] if partial else ["^" + oid for oid in sorted(set(local_before.values()))]
        absent_objects = missing_objects(repo, incoming + known)
        if absent_objects:
            raise VerificationError([
                "the export claims to be full but %d object(s) are missing" % len(absent_objects)])
        deferred = 0

    local_refs = local_before
    bare = _git_text(repo, "rev-parse", "--is-bare-repository").strip() == "true"
    checked_out = None if bare else (
        git(repo, "symbolic-ref", "-q", "HEAD", check=False).stdout.decode().strip() or None)
    plan = _reconcile(repo, manifest, local_refs, checked_out)
    export_id = provenance["exportId"]
    stamp = now_ms if now_ms is not None else _now_ms()
    import_id = "%s-%d" % (export_id, stamp)

    replica_path = state / "replica.json"
    previous_replica = _read_json(replica_path) if replica_path.exists() else None
    limitations = set((previous_replica or {}).get("limitations") or [])
    if not limited and not manifest["prerequisites"]:
        # A complete full export replaces earlier limits; objects that are
        # still missing are re-detected by replica_state().
        limitations = set()
    limitations.update(manifest["replica"]["limitations"])
    journal = {
        "schema": IMPORT_SCHEMA,
        "importId": import_id,
        "exportId": export_id,
        "manifestSha256": provenance["manifestSha256"],
        "repository": manifest["repository"],
        "signer": {k: provenance[k] for k in ("keyId", "publicKey", "trusted")},
        "importedAt": stamp,
        "state": "pending",
        "plan": plan,
        "updates": [],
        "previousReplica": previous_replica,
        "limitations": [code for code in LIMITATIONS if code in limitations],
        "deferredObjects": deferred,
    }
    commands = ["start"]
    for item in plan:
        if item["decision"] in ("create", "fast_forward"):
            commands.append("update %s %s %s" % (
                item["ref"], item["incoming"], item["local"] or "0" * len(item["incoming"])))
            journal["updates"].append({"ref": item["ref"], "old": item["local"], "new": item["incoming"]})
        elif item["decision"] in ("conflict", "checked_out"):
            aside = _conflict_ref(export_id, item["ref"])
            item["keptAt"] = aside
            old = local_refs.get(aside)
            commands.append("update %s %s %s" % (aside, item["incoming"], old or "0" * len(item["incoming"])))
            journal["updates"].append({"ref": aside, "old": old, "new": item["incoming"]})
    commands += ["prepare", "commit"]
    journal_path = state / "imports" / (import_id + ".json")
    _write_json_atomic(journal_path, journal)
    if journal["updates"]:
        git(repo, "update-ref", "--stdin", stdin=("\n".join(commands) + "\n").encode())
    journal["state"] = "applied"
    _write_json_atomic(journal_path, journal)
    if limited or previous_replica is not None:
        _write_json_atomic(replica_path, {"schema": REPLICA_SCHEMA,
                                          "limitations": journal["limitations"]})
    counts = {decision: 0 for decision in DECISIONS}
    for item in plan:
        counts[item["decision"]] += 1
    return {
        "schema": IMPORT_SCHEMA,
        "importId": import_id,
        "exportId": export_id,
        "provenance": provenance,
        "decisions": counts,
        "plan": plan,
        "replica": replica_state(repo),
        "deferredObjects": deferred,
        "journal": str(journal_path),
    }


def rollback_import(repo, import_id):
    repo = Path(repo)
    if not re.fullmatch(r"[0-9a-f]{16}-\d+", str(import_id)):
        raise UsageError("invalid import id %r" % (import_id,))
    journal_path = _state_dir(repo) / "imports" / (import_id + ".json")
    if not journal_path.exists():
        raise UsageError("no import %s in this repository" % import_id)
    journal = _read_json(journal_path)
    if journal.get("state") == "rolled_back":
        return journal
    commands = ["start"]
    for update in journal["updates"]:
        if update["old"] is None:
            commands.append("delete %s %s" % (update["ref"], update["new"]))
        else:
            commands.append("update %s %s %s" % (update["ref"], update["old"], update["new"]))
    commands += ["prepare", "commit"]
    if journal["updates"]:
        completed = git(repo, "update-ref", "--stdin", stdin=("\n".join(commands) + "\n").encode(),
                        check=False)
        if completed.returncode != 0:
            raise VerificationError([
                "refs changed after the import, so nothing was rolled back: "
                + completed.stderr.decode("utf-8", "replace").strip()])
    replica_path = _state_dir(repo) / "replica.json"
    if journal.get("previousReplica") is None:
        if replica_path.exists():
            replica_path.unlink()
    else:
        _write_json_atomic(replica_path, journal["previousReplica"])
    journal["state"] = "rolled_back"
    journal["rolledBackAt"] = _now_ms()
    _write_json_atomic(journal_path, journal)
    return journal


# --- CLI ----------------------------------------------------------------------

def _emit(value, as_json, lines):
    if as_json:
        print(json.dumps(value, indent=2, sort_keys=True))
    else:
        for line in lines:
            print(line)


def _describe_replica(replica):
    if replica["scope"] == "full":
        return "replica: full"
    return "replica: %s (%s); never claims full availability" % (
        replica["scope"], ", ".join(replica["limitations"]))


def _telemetry_counts(result):
    counters = result.get("counters") or {}
    return {"outcome": result.get("state"), "reason": result.get("reason"),
            **{k: counters.get(k, 0) for k in _new_counters()}}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    keygen = commands.add_parser("keygen", help="write a new Ed25519 signing seed")
    keygen.add_argument("seed_file")

    export = commands.add_parser("export", help="write a signed export to a directory")
    export.add_argument("repo")
    export.add_argument("media_dir")
    export.add_argument("--signing-key", required=True, help="base64url Ed25519 seed file")
    export.add_argument("--key-id", default="")
    export.add_argument("--repository", default="", help="owner/name recorded in the manifest")
    export.add_argument("--ref", action="append", default=[], help="ref glob, e.g. refs/heads/main")
    export.add_argument("--path", action="append", default=[], help="keep only this path's contents")
    export.add_argument("--metadata-only", action="store_true",
                        help="history, trees, and collaboration records; no other file contents")
    export.add_argument("--defer-blobs-over", help="leave out file contents larger than SIZE")
    export.add_argument("--since", help="previous media directory; export only what is new")
    export.add_argument("--chunk-size", default=str(DEFAULT_CHUNK_SIZE))
    export.add_argument("--telemetry")
    export.add_argument("--json", action="store_true")

    verify = commands.add_parser("verify", help="check a media directory's signature and digests")
    verify.add_argument("media_dir")
    verify.add_argument("--trusted-key", action="append", default=[])
    verify.add_argument("--json", action="store_true")

    fetch = commands.add_parser("fetch", help="resumable, bounded fetch of a media directory")
    fetch.add_argument("source")
    fetch.add_argument("dest")
    fetch.add_argument("--rate", help="bytes per second, e.g. 64k")
    fetch.add_argument("--budget", help="stop after this many bytes this run")
    fetch.add_argument("--schedule", action="append", default=[], help="allowed window HH:MM-HH:MM")
    fetch.add_argument("--wait", action="store_true", help="sleep until the next window")
    fetch.add_argument("--retries", type=int, default=8)
    fetch.add_argument("--restart", action="store_true", help="discard the checkpoint")
    fetch.add_argument("--trusted-key", action="append", default=[])
    fetch.add_argument("--telemetry")
    fetch.add_argument("--json", action="store_true")

    status = commands.add_parser("status", help="estimate a transfer from its checkpoint")
    status.add_argument("dest")
    status.add_argument("--json", action="store_true")

    importer = commands.add_parser("import", help="verify and reconcile an export into a repository")
    importer.add_argument("media_dir")
    importer.add_argument("repo")
    importer.add_argument("--trusted-key", action="append", default=[])
    importer.add_argument("--allow-unpinned", action="store_true")
    importer.add_argument("--allow-limited", action="store_true")
    importer.add_argument("--telemetry")
    importer.add_argument("--json", action="store_true")

    rollback = commands.add_parser("rollback", help="undo one import's ref changes")
    rollback.add_argument("repo")
    rollback.add_argument("import_id")
    rollback.add_argument("--telemetry")
    rollback.add_argument("--json", action="store_true")

    replica = commands.add_parser("replica", help="print this repository's replica advertisement")
    replica.add_argument("repo")
    replica.add_argument("--json", action="store_true")

    args = parser.parse_args(argv)
    try:
        return _dispatch(args)
    except UsageError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE
    except VerificationError as exc:
        if getattr(args, "json", False):
            print(json.dumps({"state": "failed", "problems": exc.problems}, indent=2))
        for problem in exc.problems:
            print("verification failed: %s" % problem, file=sys.stderr)
        return EXIT_FAILED
    except GitError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_FAILED


def _dispatch(args):
    if args.command == "keygen":
        path = Path(args.seed_file)
        if path.exists():
            raise UsageError("%s already exists" % path)
        seed = os.urandom(32)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(_b64encode(seed) + "\n")
        print(_b64encode(ed25519_public_key(seed)))
        return EXIT_OK

    if args.command == "export":
        selection = normalize_selection(
            args.ref, args.path, args.metadata_only,
            parse_size(args.defer_blobs_over) if args.defer_blobs_over else None)
        envelope = export_media(
            args.repo, args.media_dir, load_seed(args.signing_key), selection=selection,
            since=args.since, chunk_size=parse_size(args.chunk_size),
            repository=args.repository, key_id=args.key_id)
        if envelope is None:
            _emit({"state": "unchanged"}, args.json, ["nothing changed since the previous export"])
            return EXIT_OK
        manifest = envelope["manifest"]
        _append_telemetry(args.telemetry, "export", {
            "outcome": "complete", "bundleBytes": manifest["bundle"]["size"],
            "refs": len(manifest["refs"]), "omittedBytes": manifest["omitted"]["bytes"],
            "scope": manifest["replica"]["scope"]})
        _emit(envelope, args.json, [
            "exported %d refs, %d bytes in %d chunks" % (
                len(manifest["refs"]), manifest["bundle"]["size"], len(manifest["bundle"]["chunks"])),
            _describe_replica(manifest["replica"]),
            "left out %d objects (%d bytes)" % (manifest["omitted"]["objects"], manifest["omitted"]["bytes"]),
            "signer public key: %s" % envelope["signature"]["publicKey"],
        ])
        return EXIT_OK

    if args.command == "verify":
        media = Path(args.media_dir)
        manifest, provenance, problems = verify_envelope(
            _read_json(media / MANIFEST_FILE), parse_trusted_keys(args.trusted_key))
        if not problems:
            problems = bundle_problems(media / BUNDLE_FILE, manifest)
        report = {"ok": not problems, "problems": problems, "provenance": provenance,
                  "replica": manifest.get("replica") if isinstance(manifest, dict) else None}
        lines = ["verified: signature, manifest, and %s digests match" % BUNDLE_FILE] if not problems else [
            "verification failed: %s" % p for p in problems]
        if provenance and not problems:
            lines.append("signed by %s (%s)" % (
                provenance["publicKey"], "trusted" if provenance["trusted"] else "not pinned"))
            lines.append(_describe_replica(manifest["replica"]))
        _emit(report, args.json, lines)
        return EXIT_OK if not problems else EXIT_FAILED

    if args.command == "fetch":
        stop = {"requested": False}

        def _request_stop(signum, frame):
            stop["requested"] = True

        previous = signal.signal(signal.SIGINT, _request_stop)
        trusted = parse_trusted_keys(args.trusted_key)
        try:
            transfer = Transfer(
                open_source(args.source), args.dest, trusted_keys=trusted,
                allow_unpinned=not trusted,
                rate=parse_size(args.rate) if args.rate else None,
                budget=parse_size(args.budget) if args.budget else None,
                windows=[parse_window(w) for w in args.schedule], wait=args.wait,
                retries=args.retries, restart=args.restart,
                should_stop=lambda: stop["requested"])
            result = transfer.run()
        finally:
            signal.signal(signal.SIGINT, previous)
        _append_telemetry(args.telemetry, "fetch", _telemetry_counts(result))
        est = result["estimate"] or {}
        lines = ["%s%s" % (result["state"], " (%s)" % result["reason"] if result["reason"] else "")]
        if est:
            lines.append("%s of %s bytes on disk, %s remaining%s" % (
                est["bytesOnDisk"], est["totalBytes"], est["remainingBytes"],
                ", about %ss left" % est["etaSeconds"] if est.get("etaSeconds") else ""))
        if result["nextWindowAt"]:
            lines.append("next allowed window starts %s" % result["nextWindowAt"])
        lines += ["problem: %s" % p for p in result["problems"]]
        if result["replica"]:
            lines.append(_describe_replica(result["replica"]))
        _emit(result, args.json, lines)
        return {"complete": EXIT_OK, "paused": EXIT_PAUSED}.get(result["state"], EXIT_FAILED)

    if args.command == "status":
        dest = Path(args.dest)
        if not (dest / MANIFEST_FILE).exists() and not (dest / TRANSFER_DIR / "checkpoint.json").exists():
            raise UsageError("%s has no transfer" % dest)
        est = estimate(dest)
        _emit(est, args.json, ["%s of %s bytes (%s%%), %d of %d chunks verified" % (
            est["bytesOnDisk"], est["totalBytes"], est["percent"],
            est["verifiedChunks"], est["chunks"])])
        return EXIT_OK

    if args.command == "import":
        try:
            report = import_media(
                args.media_dir, args.repo, trusted_keys=parse_trusted_keys(args.trusted_key),
                allow_unpinned=args.allow_unpinned, allow_limited=args.allow_limited)
        except VerificationError:
            _append_telemetry(args.telemetry, "import", {"outcome": "failed"})
            raise
        conflicts = report["decisions"]["conflict"] + report["decisions"]["checked_out"]
        _append_telemetry(args.telemetry, "import", {
            "outcome": "conflicts" if conflicts else "applied", "conflicts": conflicts,
            "refsChanged": report["decisions"]["create"] + report["decisions"]["fast_forward"],
            "deferredObjects": report["deferredObjects"]})
        lines = ["import %s: %s" % (report["importId"], ", ".join(
            "%d %s" % (n, d) for d, n in report["decisions"].items() if n))]
        for item in report["plan"]:
            if item["decision"] in ("conflict", "checked_out"):
                lines.append("%s %s: kept yours; incoming at %s" % (
                    item["decision"], item["ref"], item["keptAt"]))
        lines.append(_describe_replica(report["replica"]))
        lines.append("undo with: forkmesh_offline_sync.py rollback %s %s" % (args.repo, report["importId"]))
        _emit(report, args.json, lines)
        return EXIT_CONFLICTS if conflicts else EXIT_OK

    if args.command == "rollback":
        journal = rollback_import(args.repo, args.import_id)
        _append_telemetry(args.telemetry, "rollback", {"outcome": journal["state"],
                                                        "refsRestored": len(journal["updates"])})
        _emit(journal, args.json, ["rolled back %d ref change(s)" % len(journal["updates"])])
        return EXIT_OK

    if args.command == "replica":
        git_dir(args.repo)
        advertisement = replica_state(args.repo)
        _emit(advertisement, args.json, [_describe_replica(advertisement),
                                         "missing objects: %d" % advertisement["missingObjects"]])
        return EXIT_OK
    raise UsageError("unknown command")


if __name__ == "__main__":
    raise SystemExit(main())
