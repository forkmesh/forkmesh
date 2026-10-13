#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import forkmesh_safe_delete  # noqa: E402

INDEX_TYPES = (
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
)
MANIFEST_TYPES = (
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
)


def split_reference(reference: str) -> tuple[str, str, str]:
    ref = reference
    for prefix in ("docker.io/", "index.docker.io/", "registry-1.docker.io/"):
        if ref.startswith(prefix):
            ref = ref[len(prefix):]
    name, _, tag = ref.partition(":")
    if "/" not in name:
        name = f"library/{name}"
    return "registry-1.docker.io", name, tag or "latest"


class Registry:
    def __init__(self, host: str, name: str):
        self.host = host
        self.name = name
        query = urllib.parse.urlencode(
            {"service": "registry.docker.io", "scope": f"repository:{name}:pull"})
        with urllib.request.urlopen(f"https://auth.docker.io/token?{query}", timeout=60) as r:
            self.token = json.load(r)["token"]

    def get(self, path: str, accept: tuple[str, ...] = ()) -> tuple[bytes, str]:
        request = urllib.request.Request(f"https://{self.host}/v2/{self.name}/{path}")
        request.add_header("Authorization", f"Bearer {self.token}")
        for kind in accept:
            request.add_header("Accept", kind)
        with urllib.request.urlopen(request, timeout=600) as response:
            return response.read(), response.headers.get("Content-Type", "")

    def blob_to(self, digest: str, dest: Path) -> None:
        request = urllib.request.Request(f"https://{self.host}/v2/{self.name}/blobs/{digest}")
        request.add_header("Authorization", f"Bearer {self.token}")
        hasher = hashlib.sha256()
        with urllib.request.urlopen(request, timeout=600) as response, dest.open("wb") as out:
            for chunk in iter(lambda: response.read(1 << 20), b""):
                hasher.update(chunk)
                out.write(chunk)
        if f"sha256:{hasher.hexdigest()}" != digest:
            dest.unlink(missing_ok=True)
            raise SystemExit(f"blob {digest} failed its digest check")


def resolve_manifest(registry: Registry, tag: str, digest: str) -> tuple[str, dict]:
    body, kind = registry.get(f"manifests/{digest or tag}", INDEX_TYPES + MANIFEST_TYPES)
    parsed = json.loads(body)
    if kind.split(";")[0] in INDEX_TYPES or "manifests" in parsed:
        if digest and f"sha256:{hashlib.sha256(body).hexdigest()}" != digest:
            raise SystemExit("image index failed its digest check")
        chosen = [m for m in parsed["manifests"]
                  if m.get("platform", {}).get("architecture") == "amd64"
                  and m.get("platform", {}).get("os") == "linux"]
        if not chosen:
            raise SystemExit("image has no linux/amd64 manifest")
        return resolve_manifest(registry, tag, chosen[0]["digest"])
    actual = f"sha256:{hashlib.sha256(body).hexdigest()}"
    if digest and actual != digest:
        raise SystemExit("image manifest failed its digest check")
    return actual, parsed


def safe_members(archive: tarfile.TarFile, root: Path):
    for member in archive:
        name = member.name.lstrip("./") if member.name.startswith("./") else member.name
        if not name or name.startswith("/") or ".." in Path(name).parts:
            continue
        base = os.path.basename(name)
        parent = os.path.dirname(name)
        if base == ".wh..wh..opq":
            target = root / parent
            if target.is_dir():
                for child in target.iterdir():
                    remove(child, root)
            continue
        if base.startswith(".wh."):
            remove(root / parent / base[len(".wh."):], root)
            continue
        if member.isdev() or member.isfifo():
            continue
        if not _inside(root, root / parent):
            continue
        if member.islnk() and (member.linkname.startswith("/")
                               or ".." in Path(member.linkname).parts
                               or not _inside(root, (root / member.linkname).parent)):
            continue
        existing = root / name
        if existing.is_symlink() or (existing.exists() and not existing.is_dir()):
            existing.unlink()
        member.name = name
        member.uid = member.gid = 0
        member.uname = member.gname = ""
        member.mode &= 0o777
        if member.isdir():
            member.mode |= 0o700
        elif member.isfile():
            member.mode |= 0o600
        yield member


def _inside(root: Path, path: Path) -> bool:
    base = root.resolve()
    resolved = path.resolve()
    return resolved == base or base in resolved.parents


def remove(path: Path, root: Path) -> None:
    forkmesh_safe_delete.remove_tree(path, root)


def pull(reference: str, digest: str, dest: Path) -> str:
    host, name, tag = split_reference(reference)
    registry = Registry(host, name)
    manifest_digest, manifest = resolve_manifest(registry, tag, digest)
    if dest.exists():
        raise SystemExit(f"{dest} already exists")
    staging = Path(tempfile.mkdtemp(prefix=".rootfs-", dir=dest.parent))
    try:
        rootfs = staging / "rootfs"
        rootfs.mkdir()
        for layer in manifest["layers"]:
            blob = staging / "layer"
            registry.blob_to(layer["digest"], blob)
            with tarfile.open(blob) as archive:
                archive.extractall(rootfs, members=safe_members(archive, rootfs),
                                   filter="fully_trusted")
            blob.unlink()
        (rootfs / ".forkmesh-image").write_text(f"{reference}@{manifest_digest}\n")
        rootfs.rename(dest)
    finally:
        forkmesh_safe_delete.remove_tree(staging, dest.parent, ignore_errors=True)
    return manifest_digest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("dest")
    parser.add_argument("--digest", default="")
    args = parser.parse_args(argv)
    dest = Path(args.dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(pull(args.reference, args.digest, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
