"""Portable public-dashboard replica shared by the Worker and mirror nodes.

The replica is deliberately a set of validated public API documents, not a
copy of D1.  Passwords, sessions, encrypted account records, private inboxes,
payments, and administrative state therefore never leave the controller.
Repository content and collaboration ledgers remain Git-authoritative.
"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile
import threading
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

import public_snapshots


REPLICA_SCHEMA_VERSION = 1
REPLICA_KIND = "forkmesh.public-dashboard-replica"
REPLICA_SOURCE_KIND = "forkmesh.public-dashboard-replica-source"
REPLICA_SOURCE_PATH = "/api/mirror-replica"
REPLICA_REPOSITORY_STATS_PATHS = (
    "/api/repo/forkmesh/forkmesh/star",
    "/api/repo/forkmesh/forkmesh/forks",
    "/api/repo/forkmesh/forkmesh/about",
)
REPLICA_PATHS = public_snapshots.SNAPSHOT_PATHS + REPLICA_REPOSITORY_STATS_PATHS
REPLICA_PATH_MAX_BYTES = {
    **public_snapshots.SNAPSHOT_PATH_MAX_BYTES,
    "/api/repo/forkmesh/forkmesh/star": 16 * 1024,
    "/api/repo/forkmesh/forkmesh/forks": 256 * 1024,
    "/api/repo/forkmesh/forkmesh/about": 512 * 1024,
}
# The individual public caps sum to just under 6 MiB. Keep a hard bound with
# enough envelope/headroom for the atomic wrapper so a valid worst-case bundle
# is not rejected after all documents have independently passed validation.
REPLICA_MAX_BYTES = 8 * 1024 * 1024
REPLICA_FETCH_TIMEOUT_SECONDS = 6
REPLICA_USER_AGENT = "ForkMesh-Mirror-Replica/1"


class ReplicaError(RuntimeError):
    """A replica could not be validated or updated safely."""


def normalize_source(value: Any) -> str:
    """Return an exact HTTPS origin suitable for public snapshot fetching."""
    source = str(value or "").strip().rstrip("/")
    try:
        parsed = urlsplit(source)
        port_ok = parsed.port is None or 1 <= parsed.port <= 65535
    except ValueError as error:
        raise ReplicaError("replica source must be an HTTPS origin") from error
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or parsed.path not in ("", "/")
        or parsed.query
        or parsed.fragment
        or parsed.port not in (None, 443)
        or not port_ok
    ):
        raise ReplicaError("replica source must be an HTTPS origin")
    hostname = parsed.hostname.lower()
    netloc = "[" + hostname + "]" if ":" in hostname else hostname
    return "https://" + netloc


def _url_origin(value: Any) -> str:
    try:
        parsed = urlsplit(str(value or "").strip())
        port_ok = parsed.port in (None, 443)
    except ValueError as error:
        raise ReplicaError("controller replica response changed origin") from error
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or parsed.password is not None
        or not port_ok
    ):
        raise ReplicaError("controller replica response changed origin")
    hostname = parsed.hostname.lower()
    netloc = "[" + hostname + "]" if ":" in hostname else hostname
    return "https://" + netloc


def _valid_timestamp(value: Any) -> bool:
    return (
        isinstance(value, int)
        and not isinstance(value, bool)
        and 0 < value <= public_snapshots.MAX_SAFE_INTEGER
    )


def _canonical_json(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        raise ReplicaError("replica contains invalid JSON") from error


def _bounded_text(value: Any, maximum: int) -> str:
    if not isinstance(value, str) or len(value) > maximum:
        raise ReplicaError("replica repository stats failed validation")
    return value


def _bounded_count(value: Any) -> int:
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or not 0 <= value <= public_snapshots.MAX_SAFE_INTEGER
    ):
        raise ReplicaError("replica repository stats failed validation")
    return value


def _project_repository_stats(path: str, payload: Any) -> dict[str, Any]:
    """Return the exact public subset an independent repo page consumes.

    These three controller-backed counters/cards are not Git records, so the
    mirror retains a bounded last-good projection beside its Git-authoritative
    repository.  Projection (rather than copying the response wholesale) keeps
    a future controller field from silently entering the portable bundle.
    """
    if not isinstance(payload, dict) or payload.get("ok") is not True:
        raise ReplicaError("replica repository stats failed validation")
    if path.endswith("/star"):
        return {
            "ok": True,
            "count": _bounded_count(payload.get("count")),
            # Replica refreshes are anonymous. Never retain viewer state even
            # if an intermediary accidentally attaches a controller cookie.
            "starred": False,
        }
    if path.endswith("/forks"):
        raw_forks = payload.get("forks")
        if not isinstance(raw_forks, list) or len(raw_forks) > 100:
            raise ReplicaError("replica repository stats failed validation")
        forks = []
        for raw in raw_forks:
            if not isinstance(raw, dict):
                raise ReplicaError("replica repository stats failed validation")
            item = {
                "owner": _bounded_text(raw.get("owner"), 80),
                "name": _bounded_text(raw.get("name"), 160),
                "url": _bounded_text(raw.get("url"), 512),
                "createdAt": _bounded_count(raw.get("createdAt", 0)),
            }
            profile = raw.get("profileUrl")
            if profile is not None:
                item["profileUrl"] = _bounded_text(profile, 2048)
            forks.append(item)
        return {
            "ok": True,
            "count": _bounded_count(payload.get("count")),
            "forks": forks,
        }
    if path.endswith("/about"):
        raw_fediverse = payload.get("fediverse")
        if not isinstance(raw_fediverse, dict):
            raise ReplicaError("replica repository stats failed validation")
        raw_followers = raw_fediverse.get("followersList")
        if not isinstance(raw_followers, list) or len(raw_followers) > 50:
            raise ReplicaError("replica repository stats failed validation")
        followers = []
        for raw in raw_followers:
            if not isinstance(raw, dict):
                raise ReplicaError("replica repository stats failed validation")
            item = {
                "handle": _bounded_text(raw.get("handle", ""), 320),
                "url": _bounded_text(raw.get("url", ""), 2048),
                "name": _bounded_text(raw.get("name", ""), 160),
                "avatarUrl": _bounded_text(raw.get("avatarUrl", ""), 4096),
                "profileUrl": _bounded_text(raw.get("profileUrl", ""), 2048),
                "followedAt": _bounded_count(raw.get("followedAt", 0)),
            }
            followers.append(item)
        raw_settings = raw_fediverse.get("settings")
        if not isinstance(raw_settings, dict):
            raise ReplicaError("replica repository stats failed validation")
        settings = {}
        for key in ("federate", "broadcastEvents", "acceptComments"):
            value = raw_settings.get(key)
            if not isinstance(value, bool):
                raise ReplicaError("replica repository stats failed validation")
            settings[key] = value
        enabled = raw_fediverse.get("enabled")
        if not isinstance(enabled, bool):
            raise ReplicaError("replica repository stats failed validation")
        return {
            "ok": True,
            "description": _bounded_text(payload.get("description", ""), 240),
            "website": _bounded_text(payload.get("website", ""), 240),
            "logoUrl": _bounded_text(payload.get("logoUrl", ""), 32 * 1024),
            "bannerUrl": _bounded_text(payload.get("bannerUrl", ""), 32 * 1024),
            "fediverse": {
                "enabled": enabled,
                "handle": _bounded_text(raw_fediverse.get("handle", ""), 320),
                "actorUrl": _bounded_text(raw_fediverse.get("actorUrl", ""), 2048),
                "followers": _bounded_count(raw_fediverse.get("followers")),
                "followersList": followers,
                "settings": settings,
            },
        }
    raise ReplicaError("replica document path is not allowed")


def _document_generated_at(path: str, payload: dict[str, Any], now: int) -> int:
    value = (
        payload.get("generatedAt")
        if path == "/api/repo/forkmesh/forkmesh/mirrors"
        else payload.get("now")
        if path in public_snapshots.SNAPSHOT_STATUS_PATHS
        else now
    )
    try:
        value = int(value)
    except (TypeError, ValueError, OverflowError):
        value = 0
    return value if value > 0 else now


def build_document(path: str, payload: Any, fetched_at: int) -> dict[str, Any]:
    """Validate one controller response and return its bounded disk record."""
    if (
        path not in REPLICA_PATHS
        or not isinstance(payload, dict)
        or not _valid_timestamp(fetched_at)
    ):
        raise ReplicaError("replica document path is not allowed")
    if path in REPLICA_REPOSITORY_STATS_PATHS:
        payload = _project_repository_stats(path, payload)
    generated_at = _document_generated_at(path, payload, fetched_at)
    if (
        path in public_snapshots.SNAPSHOT_PATHS
        and not public_snapshots.validate_snapshot_payload(
            path, payload, generated_at
        )
    ):
        raise ReplicaError("replica document failed public schema validation")
    body = _canonical_json(payload)
    if not 1 < len(body) <= REPLICA_PATH_MAX_BYTES[path]:
        raise ReplicaError("replica document exceeds its public size cap")
    return {
        "fetchedAt": fetched_at,
        "generatedAt": generated_at,
        "sha256": hashlib.sha256(body).hexdigest(),
        "payload": payload,
    }


def validate_bundle(value: Any) -> dict[str, dict[str, Any]]:
    """Return validated documents; reject the entire file on any corruption."""
    if not isinstance(value, dict) or set(value) != {
        "schemaVersion", "type", "source", "updatedAt", "documents"
    }:
        raise ReplicaError("replica bundle shape is invalid")
    if (
        value.get("schemaVersion") != REPLICA_SCHEMA_VERSION
        or value.get("type") != REPLICA_KIND
        or not isinstance(value.get("source"), str)
        or not _valid_timestamp(value.get("updatedAt"))
        or not isinstance(value.get("documents"), dict)
        or not set(value["documents"]).issubset(REPLICA_PATHS)
    ):
        raise ReplicaError("replica bundle metadata is invalid")
    try:
        if normalize_source(value["source"]) != value["source"]:
            raise ReplicaError("replica bundle metadata is invalid")
    except ReplicaError as error:
        raise ReplicaError("replica bundle metadata is invalid") from error
    documents: dict[str, dict[str, Any]] = {}
    for path, document in value["documents"].items():
        if not isinstance(document, dict) or set(document) != {
            "fetchedAt", "generatedAt", "sha256", "payload"
        }:
            raise ReplicaError("replica document shape is invalid")
        if (
            not _valid_timestamp(document.get("fetchedAt"))
            or not _valid_timestamp(document.get("generatedAt"))
            or not isinstance(document.get("sha256"), str)
        ):
            raise ReplicaError("replica document metadata is invalid")
        rebuilt = build_document(
            path, document.get("payload"), document["fetchedAt"]
        )
        if (
            rebuilt["generatedAt"] != document.get("generatedAt")
            or rebuilt["sha256"] != document.get("sha256")
        ):
            raise ReplicaError("replica document digest is invalid")
        documents[path] = rebuilt
    return documents


def load_bundle(path: Path) -> tuple[dict[str, Any], dict[str, dict[str, Any]]]:
    descriptor = -1
    try:
        flags = os.O_RDONLY
        if hasattr(os, "O_CLOEXEC"):
            flags |= os.O_CLOEXEC
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.geteuid()
            or stat.S_IMODE(info.st_mode) & 0o077
            or not 0 < info.st_size <= REPLICA_MAX_BYTES
        ):
            raise ReplicaError("replica file permissions are unsafe")
        raw = bytearray()
        while len(raw) <= REPLICA_MAX_BYTES:
            chunk = os.read(descriptor, min(65536, REPLICA_MAX_BYTES + 1 - len(raw)))
            if not chunk:
                break
            raw.extend(chunk)
        if len(raw) != info.st_size:
            raise ReplicaError("replica file changed while reading")
        value = json.loads(raw)
    except ReplicaError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ReplicaError("replica file is not readable valid JSON") from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    return value, validate_bundle(value)


def validate_destination(path: Path) -> Path:
    if not path.is_absolute() or path != Path(os.path.normpath(path)):
        raise ReplicaError("replica path must be absolute and normalized")
    parent = path.parent
    try:
        info = parent.lstat()
    except OSError as error:
        raise ReplicaError("replica parent directory is unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o022
    ):
        raise ReplicaError("replica parent directory is unsafe")
    if path.exists() or path.is_symlink():
        existing = path.lstat()
        if (
            not stat.S_ISREG(existing.st_mode)
            or stat.S_ISLNK(existing.st_mode)
            or existing.st_uid != os.geteuid()
            or stat.S_IMODE(existing.st_mode) & 0o077
        ):
            raise ReplicaError("replica destination is unsafe")
    return path


def _write_bundle(path: Path, value: dict[str, Any]) -> None:
    destination = validate_destination(path)
    raw = _canonical_json(value) + b"\n"
    if len(raw) > REPLICA_MAX_BYTES:
        raise ReplicaError("replica bundle exceeds its disk cap")
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=".dashboard-replica-", dir=destination.parent
    )
    temporary = Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "wb", closefd=True) as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    except Exception:
        try:
            os.close(descriptor)
        except OSError:
            pass
        try:
            temporary.unlink()
        except OSError:
            pass
        raise


def _default_fetch(url: str, expected_source: str) -> tuple[int, Any]:
    request = Request(
        url,
        headers={"Accept": "application/json", "User-Agent": REPLICA_USER_AGENT},
        method="GET",
    )
    try:
        with urlopen(request, timeout=REPLICA_FETCH_TIMEOUT_SECONDS) as response:
            if _url_origin(response.geturl()) != expected_source:
                raise ReplicaError("controller replica response changed origin")
            status = int(response.status)
            maximum = (
                REPLICA_MAX_BYTES
                if urlsplit(url).path == REPLICA_SOURCE_PATH
                else max(REPLICA_PATH_MAX_BYTES.values())
            )
            raw = response.read(maximum + 1)
            if len(raw) > maximum:
                raise ReplicaError("controller replica response is too large")
            return status, json.loads(raw)
    except HTTPError as error:
        return int(error.code), None
    except (URLError, TimeoutError, OSError, json.JSONDecodeError) as error:
        raise ReplicaError("controller replica response is unavailable") from error


def refresh_bundle(
    path: Path,
    source: str,
    *,
    fetch: Callable[[str], tuple[int, Any]] | None = None,
    now_ms: Callable[[], int] = lambda: int(time.time() * 1000),
) -> dict[str, Any]:
    """Refresh public documents independently, retaining last-good failures."""
    source = normalize_source(source)
    if fetch is None:
        fetch = lambda url: _default_fetch(url, source)
    documents: dict[str, dict[str, Any]] = {}
    existing_bundle: dict[str, Any] | None = None
    if path.exists():
        try:
            existing_bundle, documents = load_bundle(path)
            if existing_bundle.get("source") != source:
                existing_bundle = None
                documents = {}
        except ReplicaError:
            documents = {}
    fetched_at = now_ms()
    refreshed = 0
    failures = 0
    legacy_fallback = False
    missing_fallback: list[str] = []
    try:
        source_status, source_payload = fetch(source + REPLICA_SOURCE_PATH)
    except Exception:
        source_status, source_payload = 0, None
    if source_status == 404:
        # Mixed-version rollout only. A current controller returns the bundled
        # endpoint; an unavailable or rate-limited one must not be amplified
        # into eight more retries.
        legacy_fallback = True
    elif source_status == 200 and (
        isinstance(source_payload, dict)
        and set(source_payload) == {
            "ok", "schemaVersion", "type", "generatedAt", "documents"
        }
        and source_payload.get("ok") is True
        and source_payload.get("schemaVersion") == REPLICA_SCHEMA_VERSION
        and source_payload.get("type") == REPLICA_SOURCE_KIND
        and _valid_timestamp(source_payload.get("generatedAt"))
        and isinstance(source_payload.get("documents"), dict)
        and set(source_payload["documents"]).issubset(REPLICA_PATHS)
    ):
        source_documents = source_payload["documents"]
        for replica_path in REPLICA_PATHS:
            try:
                if replica_path not in source_documents:
                    missing_fallback.append(replica_path)
                    raise ReplicaError("controller replica source is incomplete")
                documents[replica_path] = build_document(
                    replica_path, source_documents[replica_path], fetched_at
                )
                refreshed += 1
            except Exception:
                failures += 1
    else:
        # One failed bundle request is one failed refresh. Preserve the local
        # last-good projection and wait for the socket or slow safety cadence.
        failures = len(REPLICA_PATHS)

    fallback_paths = (
        REPLICA_PATHS if legacy_fallback else tuple(missing_fallback)
    )
    if fallback_paths:
        with ThreadPoolExecutor(max_workers=len(fallback_paths)) as executor:
            pending = {
                executor.submit(fetch, source + replica_path): replica_path
                for replica_path in fallback_paths
            }
            for future in as_completed(pending):
                replica_path = pending[future]
                try:
                    status, payload = future.result()
                    if status != 200:
                        raise ReplicaError(
                            "controller returned a non-success status")
                    documents[replica_path] = build_document(
                        replica_path, payload, fetched_at
                    )
                    refreshed += 1
                except Exception:
                    failures += 1
    if not documents:
        raise ReplicaError("no valid public replica document is available")
    if refreshed == 0 and existing_bundle is not None:
        return {
            "ok": True,
            "refreshed": 0,
            "retained": len(documents),
            "failed": failures,
        }
    bundle = {
        "schemaVersion": REPLICA_SCHEMA_VERSION,
        "type": REPLICA_KIND,
        "source": source,
        "updatedAt": fetched_at,
        "documents": documents,
    }
    _write_bundle(path, bundle)
    return {
        "ok": True,
        "refreshed": refreshed,
        "retained": len(documents) - refreshed,
        "failed": failures,
    }


class ReplicaStore:
    """Validated memory-first reader explicitly refreshed by node events.

    The old request path called ``stat(2)`` for every API read. The daemon now
    signals the gateway immediately after its atomic bundle replacement, so a
    process can retain every public projection in RAM and perform no disk I/O
    at all between revisions.
    """

    def __init__(self, path: Path):
        self.path = path
        self._lock = threading.Lock()
        self._documents: dict[str, dict[str, Any]] = {}
        self._loaded_at = 0.0
        self._bytes = 0
        self._hits = 0
        self._misses = 0
        self._reloads = 0
        self._invalidations = 0
        self._disk_reads = 0
        self.invalidate(preload=True, initial=True)

    def invalidate(self, *, preload: bool = False, initial: bool = False) -> None:
        with self._lock:
            if not initial:
                self._invalidations += 1
            self._documents = {}
            self._loaded_at = 0.0
            self._bytes = 0
            if not preload:
                return
            self._disk_reads += 1
            try:
                _bundle, documents = load_bundle(self.path)
            except ReplicaError:
                return
            self._documents = documents
            self._loaded_at = time.monotonic()
            self._bytes = len(_canonical_json(documents))
            self._reloads += 1

    def document(self, path: str) -> dict[str, Any] | None:
        with self._lock:
            document = self._documents.get(path)
            if document is None:
                self._misses += 1
                return None
            self._hits += 1
            return dict(document)

    def diagnostics(self) -> dict[str, Any]:
        with self._lock:
            return {
                "loaded": bool(self._documents),
                "documents": len(self._documents),
                "bytes": self._bytes,
                "hits": self._hits,
                "misses": self._misses,
                "reloads": self._reloads,
                "invalidations": self._invalidations,
                "diskReads": self._disk_reads,
                "diskReadsAvoided": self._hits,
                "ageSeconds": (
                    max(0, int(time.monotonic() - self._loaded_at))
                    if self._loaded_at else 0
                ),
            }
