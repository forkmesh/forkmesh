#!/usr/bin/env python3
"""Materialize public provider imports as native ForkMesh repositories."""

from __future__ import annotations

import argparse
from dataclasses import replace
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import shutil
import stat
import subprocess
import sys
import time
from typing import Any
from urllib import error as urlerror
from urllib import request as urlrequest
from urllib.parse import urlencode, urlsplit

_REFRESH_MODULE_PATH = Path(__file__).resolve().with_name(
    "headless_mirror_refresh.py"
)
_REFRESH_SPEC = importlib.util.spec_from_file_location(
    "forkmesh_headless_mirror_refresh",
    _REFRESH_MODULE_PATH,
)
if _REFRESH_SPEC is None or _REFRESH_SPEC.loader is None:
    raise RuntimeError("headless mirror refresh module is unavailable")
refresh = importlib.util.module_from_spec(_REFRESH_SPEC)
sys.modules[_REFRESH_SPEC.name] = refresh
_REFRESH_SPEC.loader.exec_module(refresh)


# The relay origin is not hard-coded here: it comes from the node's own
# validated configuration (a credential-free HTTPS origin) so a service rename
# cannot silently leave this sweep pointed at a host it then refuses.
IMPORTS_PATH = "/api/repository-imports"
SOURCE_ROOT = Path("/srv/forkmesh-git/imports")
SIDECAR_FILE = "hosted-repositories.json"
PENDING_FILE = ".hosted-repository-deletions.json"
MARKER_FILE = ".forkmesh-hosted-import"
# One public listing page from the relay, which returns at most its own
# MAX_PUBLIC_IMPORTS rows. A legitimately full page must not read as a
# corrupt response, or hosting stops for everyone as soon as the table fills.
MAX_IMPORTS = 200
MAX_RESPONSE_BYTES = 4 * 1024 * 1024
PROCESS_TIMEOUT_SECONDS = 20 * 60
INITIAL_CLONE_TIMEOUT_SECONDS = 10 * 60
# Every public import from every account is eligible, and provider
# repositories are not all small. Bound the imports root so a node keeps
# refreshing what it already hosts rather than filling its disk with new
# clones. A deferred entry stays external and is reconsidered on the next run.
MAX_HOSTED_TOTAL_BYTES = 20 * 1024 * 1024 * 1024
NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}$")
IMPORT_ID_RE = re.compile(r"^ext_[0-9a-f]{24}$")
# Every provider ForkMesh imports from is materialized here, so an imported
# repository stops depending on the provider it came from (issue #679). Only
# the canonical public host of each provider is clonable, and only the path
# shape that provider's canonical repository URL uses: GitHub, Codeberg and
# Gitea name one owner and one repository, GitLab allows nested groups.
PROVIDER_HOSTS = {
    "github": ("github.com", 2, 2),
    "gitlab": ("gitlab.com", 2, 20),
    "codeberg": ("codeberg.org", 2, 2),
    "gitea": ("gitea.com", 2, 2),
}
PROVIDER_LABELS = {
    "github": "GitHub",
    "gitlab": "GitLab",
    "codeberg": "Codeberg",
    "gitea": "Gitea",
}


class SyncError(RuntimeError):
    """A fixed operational failure safe to print."""


def _safe_environment() -> dict[str, str]:
    return {
        "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "LANG": "C",
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_PROTOCOL_FROM_USER": "0",
        "GIT_OPTIONAL_LOCKS": "0",
        "GIT_NO_REPLACE_OBJECTS": "1",
    }


def _run(command: list[str], *, timeout: int = PROCESS_TIMEOUT_SECONDS) -> bytes:
    try:
        completed = subprocess.run(
            command,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=_safe_environment(),
            timeout=timeout,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SyncError("hosted repository operation failed") from exc
    if completed.returncode != 0:
        raise SyncError("hosted repository operation failed")
    return completed.stdout


def _atomic_json(path: Path, value: Any) -> None:
    raw = (
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
    ).encode("utf-8")
    temporary = path.with_name("." + path.name + "." + secrets.token_hex(8))
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(temporary, flags, 0o600)
    try:
        view = memoryview(raw)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise SyncError("hosted repository state write failed")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _read_json(path: Path, *, missing: Any) -> Any:
    try:
        info = path.lstat()
    except FileNotFoundError:
        return missing
    if (
        not stat.S_ISREG(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.geteuid()
        or stat.S_IMODE(info.st_mode) & 0o077
        or info.st_size <= 0
        or info.st_size > MAX_RESPONSE_BYTES
    ):
        raise SyncError("hosted repository state is unsafe")
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SyncError("hosted repository state is invalid") from exc


def _imports_url(config: refresh.RefreshConfig) -> str:
    return config.worker_origin + IMPORTS_PATH


def _fetch_imports(url: str) -> list[dict[str, Any]]:
    parsed = urlsplit(url)
    if (
        parsed.scheme != "https"
        or not parsed.hostname
        or parsed.path != IMPORTS_PATH
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise SyncError("repository import endpoint is invalid")
    request = urlrequest.Request(
        url,
        headers={
            "accept": "application/json",
            "user-agent": "ForkMesh-hosted-import-sync/1.0",
        },
        method="GET",
    )
    try:
        with urlrequest.urlopen(request, timeout=30) as response:
            raw = response.read(MAX_RESPONSE_BYTES + 1)
    except (OSError, urlerror.URLError) as exc:
        raise SyncError("repository import catalog is unavailable") from exc
    if len(raw) > MAX_RESPONSE_BYTES:
        raise SyncError("repository import catalog is too large")
    try:
        payload = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SyncError("repository import catalog is invalid") from exc
    rows = payload.get("repositories") if isinstance(payload, dict) else None
    if not isinstance(rows, list) or len(rows) > MAX_IMPORTS:
        raise SyncError("repository import catalog is invalid")
    return rows


def _clean_imports(
    rows: list[dict[str, Any]],
    config: refresh.RefreshConfig,
) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    seen: set[str] = set()
    root = (SOURCE_ROOT / config.node_owner).resolve()
    for row in rows:
        if not isinstance(row, dict):
            continue
        provider = str(row.get("provider") or "")
        host_rule = PROVIDER_HOSTS.get(provider)
        if (
            host_rule is None
            or row.get("isPrivate") is True
            or row.get("status") == "archived"
        ):
            continue
        host, minimum_parts, maximum_parts = host_rule
        name = str(row.get("name") or "").strip()
        import_id = str(row.get("id") or "")
        source_url = str(row.get("originalUrl") or "").strip()
        parsed = urlsplit(source_url)
        parts = [part for part in parsed.path.split("/") if part]
        if (
            not NAME_RE.fullmatch(name)
            or not IMPORT_ID_RE.fullmatch(import_id)
            or parsed.scheme != "https"
            or parsed.hostname != host
            or parsed.query
            or parsed.fragment
            or parsed.username
            or parsed.password
            or not minimum_parts <= len(parts) <= maximum_parts
            or parts[-1].removesuffix(".git") != name
            or any(part in (".", "..", "-") for part in parts)
            or name.casefold() in seen
        ):
            continue
        seen.add(name.casefold())
        metadata = row.get("metadata")
        metadata = metadata if isinstance(metadata, dict) else {}
        branch = str(metadata.get("defaultBranch") or "").strip()
        if branch and (
            len(branch) > 120
            or branch.startswith("-")
            or ".." in branch
            or any(character in branch for character in (" ", "~", "^", ":", "\\"))
        ):
            branch = ""
        description = str(metadata.get("description") or "").strip()[:240]
        output.append(
            {
                "owner": config.node_owner,
                "name": name,
                "sourceRepository": str(root / (name + ".git")),
                "sourceUrl": source_url,
                "importId": import_id,
                "provider": provider,
                "description": description,
                "branch": branch,
                "createdAt": int(row.get("createdAt") or 0),
                "publishedStateHash": "",
            }
        )
    output.sort(key=lambda item: item["name"].casefold())
    return output


def _refs_hash(config: refresh.RefreshConfig, repository: Path) -> str:
    return refresh._repository_refs_sha256(config, repository)


def _prepare_repository(
    config: refresh.RefreshConfig,
    item: dict[str, Any],
) -> bool:
    repository = Path(item["sourceRepository"])
    root = repository.parent
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    root_info = root.lstat()
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or stat.S_ISLNK(root_info.st_mode)
        or root_info.st_uid != os.geteuid()
        or stat.S_IMODE(root_info.st_mode) & 0o077
    ):
        raise SyncError("hosted repository root is unsafe")
    marker = repository / MARKER_FILE
    if repository.exists():
        info = repository.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.geteuid()
            or _read_json(marker, missing={}).get("sourceUrl")
            != item["sourceUrl"]
        ):
            raise SyncError("hosted repository checkout is unsafe")
        before = _refs_hash(config, repository)
        _run(
            [
                str(config.git_program),
                "--git-dir",
                str(repository),
                "remote",
                "update",
                "--prune",
            ]
        )
        changed = not secrets.compare_digest(
            before, _refs_hash(config, repository))
        if changed:
            # A mirror fetch can leave packs reachable only from deleted
            # upstream refs. Compact immediately after a real ref change so
            # repeated force-pushes cannot recreate the disk-growth problem.
            _run(
                [
                    str(config.git_program),
                    "--git-dir",
                    str(repository),
                    "reflog",
                    "expire",
                    "--expire=now",
                    "--all",
                ]
            )
            _run(
                [
                    str(config.git_program),
                    "--git-dir",
                    str(repository),
                    "gc",
                    "--prune=now",
                ]
            )
        return changed
    staging = root / ("." + repository.name + "." + secrets.token_hex(8))
    try:
        _run(
            [
                str(config.git_program),
                "-c",
                "protocol.allow=never",
                "-c",
                "protocol.https.allow=always",
                "clone",
                "--mirror",
                "--",
                item["sourceUrl"],
                str(staging),
            ],
            # The first clone is the whole repository, and provider imports are
            # no longer only small Codeberg projects. Bound it generously
            # instead of permanently failing every repository that needs more
            # than a moment; a timeout leaves the entry external and is retried
            # on the next timer run.
            timeout=INITIAL_CLONE_TIMEOUT_SECONDS,
        )
        _atomic_json(
            staging / MARKER_FILE,
            {"schemaVersion": 1, "sourceUrl": item["sourceUrl"]},
        )
        os.replace(staging, repository)
    finally:
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
    return True


def prepare(config: refresh.RefreshConfig) -> dict[str, Any]:
    sidecar = config.gateway_config_path.parent / SIDECAR_FILE
    old = _read_json(sidecar, missing={})
    old_items = old.get("repositories") if isinstance(old, dict) else []
    old_items = old_items if isinstance(old_items, list) else []
    old_by_id = {
        str(item.get("importId") or ""): item
        for item in old_items
        if isinstance(item, dict)
    }
    desired = _clean_imports(_fetch_imports(_imports_url(config)), config)
    changed = False
    available: list[dict[str, Any]] = []
    failed = 0
    deferred = 0
    used = _repository_size(SOURCE_ROOT / config.node_owner)
    for item in desired:
        previous = old_by_id.get(item["importId"]) or {}
        if (
            previous.get("sourceUrl") == item["sourceUrl"]
            and previous.get("name") == item["name"]
        ):
            item["publishedStateHash"] = str(
                previous.get("publishedStateHash") or ""
            )
        fresh = not Path(item["sourceRepository"]).is_dir()
        if fresh and used >= MAX_HOSTED_TOTAL_BYTES:
            deferred += 1
            continue
        try:
            repository_changed = _prepare_repository(config, item)
        except SyncError:
            failed += 1
            repository = Path(item["sourceRepository"])
            if (
                previous.get("sourceUrl") != item["sourceUrl"]
                or previous.get("name") != item["name"]
                or not repository.is_dir()
            ):
                # One provider repository becoming unavailable must not prevent
                # every other verified import from being hosted. It remains an
                # external metadata entry and is retried on the next timer run.
                continue
            repository_changed = False
        if fresh:
            used += _repository_size(Path(item["sourceRepository"]))
        state_hash = _refs_hash(config, Path(item["sourceRepository"]))
        if repository_changed:
            item["publishedStateHash"] = ""
            changed = True
        if item["publishedStateHash"] and not secrets.compare_digest(
            item["publishedStateHash"],
            state_hash,
        ):
            item["publishedStateHash"] = ""
        available.append(item)
    desired = available
    changed = changed or {
        str(item.get("importId") or "")
        for item in old_items
        if isinstance(item, dict)
    } != {item["importId"] for item in desired}
    removed = [
        {
            "owner": config.node_owner,
            "name": str(item.get("name") or ""),
            "sourceRepository": str(item.get("sourceRepository") or ""),
        }
        for item in old_items
        if isinstance(item, dict)
        and str(item.get("importId") or "")
        not in {candidate["importId"] for candidate in desired}
    ]
    _atomic_json(
        sidecar,
        {
            "schemaVersion": 1,
            "type": refresh.HOSTED_REPOSITORIES_TYPE,
            "repositories": desired,
        },
    )
    if removed:
        _atomic_json(
            config.gateway_config_path.parent / PENDING_FILE,
            {"schemaVersion": 1, "repositories": removed},
        )
    return {
        "ok": True,
        "changed": changed,
        "repositoryCount": len(desired),
        "removedCount": len(removed),
        "failedCount": failed,
        "deferredCount": deferred,
    }


def _repository_size(repository: Path) -> int:
    total = 0
    for root, directories, files in os.walk(repository):
        directories[:] = [
            name for name in directories if not Path(root, name).is_symlink()
        ]
        for name in files:
            path = Path(root, name)
            try:
                info = path.lstat()
            except OSError:
                continue
            if stat.S_ISREG(info.st_mode) and not stat.S_ISLNK(info.st_mode):
                total += info.st_size
    return min(total, 1 << 50)


def _post_catalog(
    config: refresh.RefreshConfig,
    catalog: dict[str, Any],
) -> None:
    for attempt in range(8):
        status, response = refresh._post_json(
            config.worker_origin + "/api/repositories",
            catalog,
        )
        if status in {200, 201} and response.get("ok") is True:
            return
        if status != 429 or attempt == 7:
            raise SyncError("hosted repository catalog was rejected")
        delay_ms = int(response.get("retryAfterMs") or 1000)
        time.sleep(max(0.25, min(delay_ms / 1000, 10)))


def _delete_catalog(
    config: refresh.RefreshConfig,
    owner: str,
    name: str,
) -> None:
    timestamp = time.time_ns() // 1_000_000
    response = refresh._helper_call(
        config,
        "sign-repository-delete",
        {
            "schemaVersion": 1,
            "type": "forkmesh.repository-delete-signing",
            "owner": owner,
            "name": name,
            "timestamp": timestamp,
        },
    )
    signature = str(response.get("signature") or "")
    url = (
        config.worker_origin
        + "/api/repositories?"
        + urlencode(
            {
                "owner": owner,
                "name": name,
                "ts": str(timestamp),
                "sig": signature,
            }
        )
    )
    request = urlrequest.Request(
        url,
        headers={
            "accept": "application/json",
            "user-agent": "ForkMesh-hosted-import-sync/1.0",
        },
        method="DELETE",
    )
    try:
        with urlrequest.urlopen(request, timeout=30) as result:
            payload = json.loads(
                result.read(MAX_RESPONSE_BYTES + 1).decode("utf-8")
            )
    except (OSError, urlerror.URLError, json.JSONDecodeError) as exc:
        raise SyncError("hosted repository catalog deletion failed") from exc
    if not isinstance(payload, dict) or payload.get("ok") is not True:
        raise SyncError("hosted repository catalog deletion failed")


def register(config: refresh.RefreshConfig) -> dict[str, Any]:
    sidecar_path = config.gateway_config_path.parent / SIDECAR_FILE
    source = _read_json(sidecar_path, missing={})
    items = source.get("repositories") if isinstance(source, dict) else None
    if not isinstance(items, list):
        raise SyncError("hosted repository state is invalid")
    identity = refresh._load_public_identity(config)
    published = 0
    for item in items:
        repository = Path(str(item.get("sourceRepository") or ""))
        state_hash = _refs_hash(config, repository)
        if secrets.compare_digest(
            str(item.get("publishedStateHash") or ""),
            state_hash,
        ):
            continue
        branch = str(item.get("branch") or "")
        catalog = replace(
            config.catalog,
            description=(
                str(item.get("description") or "")
                or (
                    "Fully hosted by ForkMesh; imported from "
                    + PROVIDER_LABELS.get(
                        str(item.get("provider") or ""), "its provider")
                    + "."
                )
            ),
            channel="#" + config.node_owner + "-" + str(item["name"])[:80],
            hosted_since=(
                str(int(item.get("createdAt") or 0))
                if int(item.get("createdAt") or 0) > 0
                else ""
            ),
            branch=branch,
            actions_enabled=False,
        )
        repository_config = replace(
            config,
            source_repository=repository,
            release_store=None,
            repository_name=str(item["name"]),
            owner_aliases=(config.node_owner,),
            catalog=catalog,
        )
        metadata = refresh.SealMetadata(
            ciphertext_sha256="0" * 64,
            ciphertext_bytes=_repository_size(repository),
            key_reference="forkmesh-hosted-import",
            expected_refs_sha256=state_hash,
        )
        _post_catalog(
            config,
            refresh._sign_catalog(repository_config, identity, metadata),
        )
        item["publishedStateHash"] = state_hash
        _atomic_json(sidecar_path, source)
        published += 1
    pending_path = config.gateway_config_path.parent / PENDING_FILE
    pending = _read_json(pending_path, missing={})
    removed = pending.get("repositories") if isinstance(pending, dict) else []
    pending_items = list(removed) if isinstance(removed, list) else []
    for index, item in enumerate(pending_items):
        owner = str(item.get("owner") or "")
        name = str(item.get("name") or "")
        if owner == config.node_owner and NAME_RE.fullmatch(name):
            _delete_catalog(config, owner, name)
        # Persist forward progress after every successful (or intentionally
        # ignored invalid) item. A late Worker cleanup error may arrive after
        # the catalog row was already removed; the next run gets an idempotent
        # deleted:false response and advances instead of replaying the entire
        # queue forever.
        remaining = pending_items[index + 1 :]
        if remaining:
            _atomic_json(pending_path, {"repositories": remaining})
        else:
            try:
                pending_path.unlink()
            except FileNotFoundError:
                pass
    return {"ok": True, "published": published, "repositoryCount": len(items)}


def cleanup(config: refresh.RefreshConfig) -> dict[str, Any]:
    sidecar = _read_json(
        config.gateway_config_path.parent / SIDECAR_FILE,
        missing=None,
    )
    if not isinstance(sidecar, dict) or not isinstance(sidecar.get("repositories"), list):
        return {"ok": True, "removed": 0}
    items = sidecar.get("repositories") if isinstance(sidecar, dict) else []
    keep = {
        Path(str(item.get("sourceRepository") or "")).resolve()
        for item in items
        if isinstance(item, dict)
    }
    root = (SOURCE_ROOT / config.node_owner).resolve()
    removed = 0
    if root.exists():
        for path in root.iterdir():
            if path.resolve() in keep or not path.name.endswith(".git"):
                continue
            marker = path / MARKER_FILE
            if _read_json(marker, missing={}).get("sourceUrl"):
                shutil.rmtree(path)
                removed += 1
    return {"ok": True, "removed": removed}


def _run_as_mirror(
    config: refresh.RefreshConfig,
    script: Path,
    mode: str,
) -> dict[str, Any]:
    command = [
        "/usr/sbin/runuser",
        "--user",
        "forkmesh-mirror",
        "--",
        "/usr/bin/env",
        "TMPDIR=/var/lib/forkmesh-mirror/runtime-tmp",
        str(config.python_program),
        "-I",
        str(script),
        "--config",
        str(config.config_path),
        mode,
    ]
    raw = _run(command, timeout=60 * 60)
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SyncError("hosted repository subprocess response is invalid") from exc
    if not isinstance(value, dict) or value.get("ok") is not True:
        raise SyncError("hosted repository subprocess failed")
    return value


def orchestrate(config: refresh.RefreshConfig) -> dict[str, Any]:
    if os.geteuid() != 0:
        raise SyncError("hosted repository orchestration requires root")
    try:
        account = pwd.getpwnam("forkmesh-mirror")
    except KeyError as exc:
        raise SyncError("forkmesh-mirror account is unavailable") from exc
    SOURCE_ROOT.mkdir(mode=0o755, parents=True, exist_ok=True)
    source_root = SOURCE_ROOT / config.node_owner
    source_root.mkdir(mode=0o700, exist_ok=True)
    os.chown(source_root, account.pw_uid, account.pw_gid)
    os.chmod(source_root, 0o700)
    script = Path(__file__).resolve()
    prepared = _run_as_mirror(config, script, "prepare")
    if prepared.get("changed"):
        _run(
            [
                "/usr/sbin/runuser",
                "--user",
                "forkmesh-mirror",
                "--",
                "/usr/bin/env",
                "TMPDIR=/var/lib/forkmesh-mirror/runtime-tmp",
                str(config.python_program),
                "-I",
                str(Path(refresh.__file__).resolve()),
                "--config",
                str(config.config_path),
                "refresh",
            ],
            timeout=60 * 60,
        )
        _run(["/usr/bin/systemctl", "restart", "forkmesh-mirror.service"])
    registered = _run_as_mirror(config, script, "register")
    cleaned = _run_as_mirror(config, script, "cleanup")
    return {
        "ok": True,
        "changed": bool(prepared.get("changed")),
        "repositoryCount": int(prepared.get("repositoryCount") or 0),
        "published": int(registered.get("published") or 0),
        "removed": int(cleaned.get("removed") or 0),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Host public provider imports on a ForkMesh mirror"
    )
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument(
        "mode",
        choices=("prepare", "register", "cleanup", "orchestrate"),
    )
    return parser


def _load_root_orchestration_config(path: Path) -> refresh.RefreshConfig:
    """Read the owner-only configuration with the mirror account's identity."""
    try:
        account = pwd.getpwnam("forkmesh-mirror")
    except KeyError as exc:
        raise SyncError("forkmesh-mirror account is unavailable") from exc
    original_egid = os.getegid()
    original_euid = os.geteuid()
    try:
        os.setegid(account.pw_gid)
        os.seteuid(account.pw_uid)
        return refresh.load_config(path)
    finally:
        os.seteuid(original_euid)
        os.setegid(original_egid)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.mode == "orchestrate":
            if os.geteuid() != 0:
                raise SyncError(
                    "hosted repository orchestration requires root")
            config = _load_root_orchestration_config(args.config)
            result = orchestrate(config)
        else:
            if os.geteuid() == 0:
                raise SyncError("hosted repository worker must be unprivileged")
            config = refresh.load_config(args.config)
            result = {
                "prepare": prepare,
                "register": register,
                "cleanup": cleanup,
            }[args.mode](config)
        print(json.dumps(result, sort_keys=True, separators=(",", ":")))
        return 0
    except (SyncError, refresh.RefreshError) as exc:
        print(f"ForkMesh hosted repository sync: {exc}", file=sys.stderr)
        return 2
    except Exception as exc:
        print(
            "ForkMesh hosted repository sync: unexpected local failure "
            f"({type(exc).__name__})",
            file=sys.stderr,
        )
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
