#!/usr/bin/env python3
from __future__ import annotations

import argparse
import email.utils
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APT_FILES = ROOT / "tools" / "package" / "apt"
KEYRING_NAME = "forkmesh-archive-keyring.asc"
SOURCES_NAME = "forkmesh.sources"
SUITE = "stable"
COMPONENT = "main"
PACKAGE = "forkmesh"
KEY_UID = "ForkMesh Archive Signing Key <security@forkmesh.com>"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:\+[0-9]+)?$")
ARCH_RE = re.compile(r"^[a-z0-9]+$")
FIELD_ORDER = ("Package", "Version", "Architecture", "Maintainer", "Installed-Size",
               "Depends", "Section", "Priority", "Homepage", "Description")


class AptError(Exception):
    pass


def read_control(deb: Path) -> str:
    with deb.open("rb") as data:
        if data.read(8) != b"!<arch>\n":
            raise AptError(f"{deb.name} is not a Debian package (no ar header)")
        while header := data.read(60):
            if len(header) != 60 or header[58:60] != b"`\n":
                raise AptError(f"{deb.name} has a truncated ar member header")
            name = header[:16].decode("ascii", "replace").strip().rstrip("/")
            size = int(header[48:58].decode("ascii").strip())
            if not name.startswith("control.tar"):
                data.seek(size + (size & 1), os.SEEK_CUR)
                continue
            member = io.BytesIO(data.read(size))
            with tarfile.open(fileobj=member, mode="r:*") as archive:
                for entry in archive.getmembers():
                    if entry.name in ("./control", "control") and entry.isfile():
                        return archive.extractfile(entry).read().decode("utf-8")
            raise AptError(f"{deb.name} has no control file")
    raise AptError(f"{deb.name} has no control archive")


def parse_control(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    current = ""
    for line in text.splitlines():
        if not line.strip():
            if fields:
                break
            continue
        if line[0] in " \t":
            if not current:
                raise AptError("control continuation line without a field")
            fields[current] += "\n" + line
            continue
        key, sep, value = line.partition(":")
        if not sep or not key or " " in key:
            raise AptError(f"malformed control line: {line!r}")
        current = key.strip()
        fields[current] = value.strip()
    for required in ("Package", "Version", "Architecture", "Description"):
        if not fields.get(required):
            raise AptError(f"control file has no {required}")
    if fields["Package"] != PACKAGE:
        raise AptError(f"package is {fields['Package']!r}, not {PACKAGE!r}")
    if not VERSION_RE.match(fields["Version"]):
        raise AptError(f"package version {fields['Version']!r} is not X.Y.Z[+N]")
    if not ARCH_RE.match(fields["Architecture"]):
        raise AptError(f"package architecture {fields['Architecture']!r} is invalid")
    return fields


def pool_path(fields: dict[str, str], sha256: str) -> str:
    return (f"pool/sha256/{sha256}/"
            f"{fields['Package']}_{fields['Version']}_{fields['Architecture']}.deb")


def packages_stanza(fields: dict[str, str], sha256: str, size: int) -> str:
    if not SHA256_RE.match(sha256):
        raise AptError(f"{sha256!r} is not a sha256")
    ordered = [key for key in FIELD_ORDER if key in fields]
    ordered += sorted(key for key in fields if key not in FIELD_ORDER)
    lines = [f"{key}: {fields[key]}" for key in ordered if key != "Description"]
    lines += [f"Filename: {pool_path(fields, sha256)}", f"Size: {size}", f"SHA256: {sha256}"]
    lines.append(f"Description: {fields['Description']}")
    return "\n".join(lines) + "\n"


def release_text(files: dict[str, bytes], architectures: list[str], date: float) -> str:
    lines = [
        "Origin: ForkMesh",
        "Label: ForkMesh",
        f"Suite: {SUITE}",
        f"Codename: {SUITE}",
        f"Date: {email.utils.formatdate(date, usegmt=True)}",
        f"Architectures: {' '.join(sorted(architectures))}",
        f"Components: {COMPONENT}",
        "Description: ForkMesh desktop releases",
        "SHA256:",
    ]
    for name in sorted(files):
        body = files[name]
        lines.append(f" {hashlib.sha256(body).hexdigest()} {len(body):>16} {name}")
    return "\n".join(lines) + "\n"


def repository_files(debs: list[tuple[dict[str, str], str, int]],
                     date: float) -> dict[str, bytes]:
    by_arch: dict[str, list[str]] = {}
    for fields, sha256, size in debs:
        by_arch.setdefault(fields["Architecture"], []).append(
            packages_stanza(fields, sha256, size))
    if not by_arch:
        raise AptError("no packages to publish")
    indexes = {f"{COMPONENT}/binary-{arch}/Packages": "\n".join(stanzas).encode()
               for arch, stanzas in by_arch.items()}
    release = release_text(indexes, list(by_arch), date).encode()
    out = {f"dists/{SUITE}/{name}": body for name, body in indexes.items()}
    out[f"dists/{SUITE}/Release"] = release
    return out


def gpg(home: Path, *args: str, stdin: bytes | None = None) -> bytes:
    command = ["gpg", "--homedir", str(home), "--batch", "--yes", "--no-tty",
               "--pinentry-mode", "loopback", "--passphrase", "", *args]
    result = subprocess.run(command, input=stdin, capture_output=True, check=False)
    if result.returncode != 0:
        raise AptError(f"gpg {' '.join(args[:2])} failed: "
                       f"{result.stderr.decode(errors='replace').strip()[-800:]}")
    return result.stdout


class GnupgHome:
    def __init__(self, parent: str | None = None):
        self._dir = tempfile.TemporaryDirectory(prefix="forkmesh-apt-gpg-", dir=parent)
        self.path = Path(self._dir.name)
        os.chmod(self.path, 0o700)

    def __enter__(self) -> Path:
        return self.path

    def __exit__(self, *exc) -> None:
        subprocess.run(["gpgconf", "--homedir", str(self.path), "--kill", "all"],
                       capture_output=True, check=False)
        self._dir.cleanup()


def fingerprints(home: Path, secret: bool = False) -> list[str]:
    listing = gpg(home, "--with-colons", "--list-secret-keys" if secret else "--list-keys")
    found = []
    for line in listing.decode().splitlines():
        parts = line.split(":")
        if parts[0] == "fpr" and len(parts) > 9:
            found.append(parts[9])
    return found


def sign_release(release: bytes, key: Path, keyring: Path) -> tuple[bytes, bytes]:
    with GnupgHome() as home:
        gpg(home, "--import", str(key))
        secret = fingerprints(home, secret=True)
        if not secret:
            raise AptError(f"{key} holds no secret key")
        with GnupgHome() as check:
            gpg(check, "--import", str(keyring))
            published = fingerprints(check)
        if secret[0] not in published:
            raise AptError(f"the signing key {secret[0]} is not the published key in {keyring}")
        common = ("--local-user", secret[0], "--digest-algo", "SHA512")
        inrelease = gpg(home, *common, "--clearsign", "--output", "-", stdin=release)
        detached = gpg(home, *common, "--armor", "--detach-sign", "--output", "-",
                       stdin=release)
    return inrelease, detached


def verify_signatures(directory: Path, keyring: Path) -> None:
    dists = directory / "dists" / SUITE
    with GnupgHome() as home:
        binary = home / "keyring.gpg"
        binary.write_bytes(gpg(home, "--dearmor", "--output", "-", stdin=keyring.read_bytes()))
        for args in (["InRelease"], ["Release.gpg", "Release"]):
            result = subprocess.run(["gpgv", "--keyring", str(binary),
                                     *[str(dists / name) for name in args]],
                                    capture_output=True, check=False)
            if result.returncode != 0:
                raise AptError(f"{args[0]} does not verify against {keyring.name}: "
                               f"{result.stderr.decode(errors='replace').strip()[-500:]}")
    release = (dists / "Release").read_text()
    listed = re.findall(r"^ ([0-9a-f]{64})\s+(\d+) (\S+)$", release, re.M)
    if not listed:
        raise AptError("Release lists no index")
    for digest, size, name in listed:
        body = (dists / name).read_bytes()
        if hashlib.sha256(body).hexdigest() != digest or len(body) != int(size):
            raise AptError(f"{name} does not match its Release entry")


def write_tree(directory: Path, files: dict[str, bytes]) -> None:
    for relative, body in files.items():
        target = directory / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        staged = target.with_name(target.name + ".tmp")
        staged.write_bytes(body)
        staged.replace(target)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as data:
        for chunk in iter(lambda: data.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def publish(debs: list[Path], out: Path, key: Path, keyring: Path,
            sources: Path, digests: list[str] | None = None,
            date: float | None = None) -> dict[str, bytes]:
    if digests is not None and len(digests) != len(debs):
        raise AptError("give one sha256 per package")
    entries = []
    for index, deb in enumerate(debs):
        digest = sha256_file(deb)
        if digests is not None and digests[index] != digest:
            raise AptError(f"{deb} does not hash to {digests[index]}")
        entries.append((parse_control(read_control(deb)), digest, deb.stat().st_size))
    files = repository_files(entries, time.time() if date is None else date)
    inrelease, detached = sign_release(files[f"dists/{SUITE}/Release"], key, keyring)
    files[f"dists/{SUITE}/InRelease"] = inrelease
    files[f"dists/{SUITE}/Release.gpg"] = detached
    files[KEYRING_NAME] = keyring.read_bytes()
    files[SOURCES_NAME] = sources.read_bytes()
    out.mkdir(parents=True, exist_ok=True)
    write_tree(out, files)
    verify_signatures(out, keyring)
    return files


def init_key(key: Path, public: Path) -> str:
    if key.exists():
        raise AptError(f"{key} already exists; refusing to replace the apt signing key")
    key.parent.mkdir(parents=True, exist_ok=True)
    with GnupgHome() as home:
        gpg(home, "--quick-generate-key", KEY_UID, "ed25519", "sign", "never")
        fingerprint = fingerprints(home, secret=True)[0]
        secret = gpg(home, "--armor", "--export-secret-keys", fingerprint)
        exported = gpg(home, "--armor", "--export-options", "export-minimal",
                       "--export", fingerprint)
    descriptor = os.open(key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(secret)
    public.parent.mkdir(parents=True, exist_ok=True)
    public.write_bytes(exported)
    return fingerprint


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Publish the signed ForkMesh apt repository metadata.")
    commands = parser.add_subparsers(dest="command", required=True)
    publish_cmd = commands.add_parser("publish", help="write and sign dists/ for .deb files")
    publish_cmd.add_argument("--deb", action="append", required=True, type=Path)
    publish_cmd.add_argument("--sha256", action="append", default=[],
                             help="expected sha256 of each --deb, in order")
    publish_cmd.add_argument("--out", required=True, type=Path)
    publish_cmd.add_argument("--signing-key", required=True, type=Path)
    publish_cmd.add_argument("--keyring", type=Path, default=APT_FILES / KEYRING_NAME)
    publish_cmd.add_argument("--sources", type=Path, default=APT_FILES / SOURCES_NAME)
    verify_cmd = commands.add_parser("verify", help="check a published repository")
    verify_cmd.add_argument("--repo", required=True, type=Path)
    verify_cmd.add_argument("--keyring", type=Path, default=APT_FILES / KEYRING_NAME)
    key_cmd = commands.add_parser("init-key", help="create the apt signing key once")
    key_cmd.add_argument("--signing-key", required=True, type=Path)
    key_cmd.add_argument("--public", type=Path, default=APT_FILES / KEYRING_NAME)
    args = parser.parse_args(argv)
    for tool in ("gpg", "gpgv", "gpgconf"):
        if shutil.which(tool) is None:
            print(f"error: {tool} is required", file=sys.stderr)
            return 2
    try:
        if args.command == "init-key":
            fingerprint = init_key(args.signing_key, args.public)
            print(f"created apt signing key {fingerprint}\n"
                  f"  secret: {args.signing_key}\n  public: {args.public}")
        elif args.command == "publish":
            if args.sha256 and len(args.sha256) != len(args.deb):
                parser.error("give one --sha256 per --deb, or none")
            publish(args.deb, args.out, args.signing_key, args.keyring, args.sources,
                    [value.lower() for value in args.sha256] or None)
            print(f"published {len(args.deb)} package(s) to {args.out}")
        else:
            verify_signatures(args.repo, args.keyring)
            print(f"{args.repo} verifies against {args.keyring.name}")
    except (AptError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
