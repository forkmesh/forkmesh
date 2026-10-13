#!/usr/bin/env python3
"""Nightly rqlite backup of ForkMesh Next, pulled to the operator box.

    next_backup.py keygen [--execute]   # age key pair: config/backup-age.{pub,key}
    next_backup.py run    [--execute]   # back up, verify, encrypt, then apply retention
    next_backup.py verify FILE [--identity KEY]   # check one backup file
    next_backup.py restore FILE --out DB [--identity KEY]   # decrypt + gunzip
    next_backup.py encrypt-existing [--execute] [--identity KEY]
    next_backup.py prune  [--execute]   # retention only

No object storage: the operator box opens an SSH tunnel to the Raft leader's
``127.0.0.1:4001`` and downloads ``GET /db/backup?compress`` (rqlite v10: a
gzip-compressed SQLite file; checked against rqlited v10.3.6) into
a private work directory (``data/backups/.work``, 0700; never ``/tmp``).
A backup is kept only after it verifies: it decompresses, opens with
sqlite3, ``PRAGMA integrity_check`` returns ``ok`` and the core tables
(``d1_migrations`` required; ``accounts``, ``repositories`` counted when
present) can be counted.

**Encrypted at rest** (audit 2026-09-24 M9): the verified backup is then
encrypted with ``age`` to the recipient in ``config/backup-age.pub`` and kept
only as ``rqlite-<UTC stamp>.sqlite3.gz.age`` (0600); the plaintext never
lands in the backups directory. ``run`` refuses when there is no recipient.
The matching private key (``config/backup-age.key``, written 0400 by
``keygen``) belongs **offline**: the nightly run needs only the public key.
``verify``/``restore`` of a ``.age`` file take ``--identity`` (default
``config/backup-age.key``). ``encrypt-existing`` encrypts plaintext backups
written before encryption existed, checks each round trip with the identity
and only then deletes the plaintext file, matched by exact name.

Retention keeps the newest backup of each of the last 7 days that have one
and the newest of each of the last 4 ISO weeks. It deletes **only** regular
files in the backups directory whose whole name matches
``rqlite-YYYYMMDDTHHMMSSZ.sqlite3.gz[.age]`` -- never anything else, never
the newest backup.

**Dry-run is the default**: without ``--execute`` it only prints the plan.
"""

from __future__ import annotations

import argparse
import datetime as dt
import gzip
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import urllib.request
from base64 import b64encode
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

NAME_RE = re.compile(r"^rqlite-(\d{8}T\d{6}Z)\.sqlite3\.gz(?:\.age)?$")
PLAIN_RE = re.compile(r"^rqlite-(\d{8}T\d{6}Z)\.sqlite3\.gz$")
AGE_SUFFIX = ".age"
AGE_MAGIC = b"age-encryption.org/v1\n"
RECIPIENT_FILE = "backup-age.pub"
IDENTITY_FILE = "backup-age.key"
KEEP_DAILY = 7
KEEP_WEEKLY = 4
REQUIRED_TABLES = ("d1_migrations",)
COUNTED_TABLES = ("d1_migrations", "accounts", "repositories")


def backups_dir() -> Path:
    configured = os.environ.get("FMNEXT_BACKUPS_DIR")
    if configured:
        return Path(configured)
    return common.data_dir() / "backups"


def backup_name(now: dt.datetime) -> str:
    return "rqlite-%s.sqlite3.gz" % now.astimezone(dt.timezone.utc).strftime(
        "%Y%m%dT%H%M%SZ")


def work_dir(directory: Path) -> Path:
    """0700 scratch directory for plaintext during download/verify."""
    work = Path(directory) / ".work"
    work.mkdir(parents=True, exist_ok=True)
    os.chmod(work, 0o700)
    return work


# --------------------------------------------------------------------------
# age


def age_binary(name="age") -> str:
    """The age CLI: $FMNEXT_AGE_DIR/<name>, ~/.local/bin/<name>, then PATH."""
    candidates = []
    if os.environ.get("FMNEXT_AGE_DIR"):
        candidates.append(Path(os.environ["FMNEXT_AGE_DIR"]) / name)
    candidates.append(Path.home() / ".local" / "bin" / name)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    found = shutil.which(name)
    if not found:
        raise ToolError("%s not found (install it in ~/.local/bin)" % name)
    return found


def recipient_path() -> Path:
    return common.config_dir() / RECIPIENT_FILE


def identity_path() -> Path:
    return common.config_dir() / IDENTITY_FILE


def read_recipient(path=None) -> str:
    path = Path(path or recipient_path())
    try:
        lines = [l.strip() for l in path.read_text("utf-8").splitlines()]
    except FileNotFoundError:
        raise ToolError("no backup recipient %s: run `next_backup.py keygen "
                        "--execute` first (backups are never stored in "
                        "plaintext)" % path) from None
    keys = [l for l in lines if l and not l.startswith("#")]
    if len(keys) != 1 or not re.fullmatch(r"age1[0-9a-z]{58}", keys[0]):
        raise ToolError("%s must hold exactly one age1... recipient" % path)
    return keys[0]


def is_encrypted(path: Path) -> bool:
    with open(path, "rb") as handle:
        return handle.read(len(AGE_MAGIC)) == AGE_MAGIC


def age_encrypt(source: Path, target: Path, recipient: str):
    """Encrypt source to a new 0600 target (O_EXCL; removed on failure)."""
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as out, open(source, "rb") as src:
            subprocess.run([age_binary(), "--encrypt", "--recipient", recipient],
                           stdin=src, stdout=out, stderr=subprocess.PIPE, check=True)
        if not is_encrypted(target):
            raise ToolError("age produced no age header for %s" % target.name)
    except BaseException:
        target.unlink(missing_ok=True)
        raise


def age_decrypt(source: Path, target: Path, identity: Path):
    identity = Path(identity)
    if not identity.is_file():
        raise ToolError("%s is encrypted; decrypting needs the offline identity "
                        "(--identity, default %s)" % (source.name, identity))
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as out, open(source, "rb") as src:
        result = subprocess.run([age_binary(), "--decrypt", "--identity", str(identity)],
                                stdin=src, stdout=out, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise ToolError("%s does not decrypt with %s" % (source.name, identity.name))


def stamp_of(name: str):
    match = NAME_RE.match(name)
    if not match:
        return None
    return dt.datetime.strptime(match.group(1), "%Y%m%dT%H%M%SZ").replace(
        tzinfo=dt.timezone.utc)


# --------------------------------------------------------------------------
# verify


def verify(path: Path, required=REQUIRED_TABLES, identity=None, scratch=None) -> dict:
    """Return {table: rows}; raise ToolError when the backup is unusable.

    Plaintext only ever goes to a 0700 directory next to the backup (or
    ``scratch``), never to /tmp. An age-encrypted backup is decrypted with
    ``identity`` (default config/backup-age.key) first.
    """
    path = Path(path)
    scratch = Path(scratch) if scratch else work_dir(path.resolve().parent)
    with tempfile.TemporaryDirectory(prefix="verify-", dir=scratch) as tmp:
        os.chmod(tmp, 0o700)
        plain = Path(tmp) / "db.sqlite3"
        compressed = path
        try:
            encrypted = is_encrypted(path)
        except OSError as error:
            raise ToolError("%s: %s" % (path.name, error))
        if encrypted:
            compressed = Path(tmp) / "db.sqlite3.gz"
            age_decrypt(path, compressed, identity or identity_path())
        try:
            with gzip.open(compressed, "rb") as source, open(plain, "wb") as target:
                shutil.copyfileobj(source, target)
        except (OSError, EOFError) as error:
            raise ToolError("%s does not decompress: %s" % (path.name, error))
        try:
            connection = sqlite3.connect("file:%s?mode=ro" % plain, uri=True)
            try:
                result = connection.execute("PRAGMA integrity_check").fetchall()
                if result != [("ok",)]:
                    raise ToolError("%s integrity_check: %s" % (
                        path.name, "; ".join(r[0] for r in result[:5])))
                tables = {row[0] for row in connection.execute(
                    "SELECT name FROM sqlite_master WHERE type='table'")}
                missing = [t for t in required if t not in tables]
                if missing:
                    raise ToolError("%s lacks table(s) %s" % (path.name, ", ".join(missing)))
                counts = {}
                for table in COUNTED_TABLES:
                    if table in tables:
                        counts[table] = connection.execute(
                            'SELECT count(*) FROM "%s"' % table).fetchone()[0]
                return counts
            finally:
                connection.close()
        except sqlite3.DatabaseError as error:
            raise ToolError("%s is not a SQLite database: %s" % (path.name, error))


# --------------------------------------------------------------------------
# retention


def retention_plan(names, now=None):
    """(keep, delete) among names matching NAME_RE; other names are ignored."""
    stamped = sorted(((stamp_of(n), n) for n in names if stamp_of(n)), reverse=True)
    keep = set()
    days, weeks = [], []
    for stamp, name in stamped:
        day = stamp.date()
        week = stamp.isocalendar()[:2]
        if day not in days and len(days) < KEEP_DAILY:
            days.append(day)
            keep.add(name)
        if week not in weeks and len(weeks) < KEEP_WEEKLY:
            weeks.append(week)
            keep.add(name)
    if stamped:
        keep.add(stamped[0][1])  # never the newest
    delete = [n for _, n in stamped if n not in keep]
    return sorted(keep), sorted(delete)


def prune(directory: Path, runner):
    directory = Path(directory)
    if not directory.is_dir():
        return []
    names = [entry.name for entry in directory.iterdir()
             if entry.is_file() and not entry.is_symlink() and NAME_RE.match(entry.name)]
    keep, delete = retention_plan(names)
    for name in delete:
        target = directory / name
        # Belt and braces: exactly our pattern, directly inside the directory.
        if not NAME_RE.match(target.name) or target.parent.resolve() != directory.resolve() \
                or target.is_symlink() or not target.is_file():
            raise GuardError("refusing to delete %s" % target)
        runner.step("delete old backup %s" % name, lambda target=target: target.unlink())
    runner.say("retention: keep %d, delete %d" % (len(keep), len(delete)))
    return delete


# --------------------------------------------------------------------------
# download


def download(url, user, password, target: Path, opener=None):
    request = urllib.request.Request(url + "/db/backup?compress", headers={
        "Authorization": "Basic " + b64encode(
            ("%s:%s" % (user, password)).encode()).decode()})
    opener = opener or urllib.request.urlopen
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as handle, opener(request, timeout=600) as response:
        if response.status != 200:
            raise ToolError("backup request returned HTTP %d" % response.status)
        shutil.copyfileobj(response, handle, 1 << 20)


def run_backup(fleet, ssh, runner, directory=None, now=None, opener=None):
    import next_deploy
    directory = Path(directory or backups_dir())
    now = now or dt.datetime.now(dt.timezone.utc)
    final = directory / (backup_name(now) + AGE_SUFFIX)

    def action():
        recipient = read_recipient()  # refuse before downloading anything
        directory.mkdir(parents=True, exist_ok=True)
        os.chmod(directory, 0o700)
        values = common.read_env_file(common.config_dir() / "secrets.env")
        user = (fleet.get("rqlite") or {}).get("user", "fmnext")
        leader = next_deploy.find_leader(fleet, ssh)
        partial = work_dir(directory) / (backup_name(now) + ".part")
        url, close = next_deploy.open_rqlite_tunnel(ssh, leader)
        try:
            download(url, user, values.get("RQLITE_PASSWORD", ""), partial, opener)
        finally:
            close()
        encrypted = directory / (final.name + ".part")
        try:
            counts = verify(partial, scratch=partial.parent)
            age_encrypt(partial, encrypted, recipient)
            os.replace(encrypted, final)
        finally:
            partial.unlink(missing_ok=True)  # the plaintext never stays
            encrypted.unlink(missing_ok=True)
        runner.say("backup %s from %s verified: %s" % (final.name, leader["name"], ", ".join(
            "%s=%d" % item for item in sorted(counts.items()))))
    runner.step("tunnel to the rqlite leader, GET /db/backup?compress, verify "
                "(integrity_check + table counts), age-encrypt -> %s" % final, action)
    prune(directory, runner)
    return final


# --------------------------------------------------------------------------
# keygen, restore, encrypt-existing


def keygen(runner, config=None):
    """Write config/backup-age.key (0400) and config/backup-age.pub."""
    config = Path(config or common.config_dir())
    key, pub = config / IDENTITY_FILE, config / RECIPIENT_FILE
    if key.exists() or pub.exists():
        raise GuardError("%s or %s already exists; move the old pair away first "
                         "(old backups need the old key)" % (key, pub))

    def action():
        subprocess.run([age_binary("age-keygen"), "-o", str(key)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.chmod(key, 0o400)
        public = subprocess.run([age_binary("age-keygen"), "-y", str(key)], check=True,
                                capture_output=True, text=True).stdout.strip()
        fd = os.open(pub, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        with os.fdopen(fd, "w") as handle:
            handle.write(public + "\n")
        read_recipient(pub)
        runner.say("recipient %s written to %s" % (public, pub))
        runner.say("IMPORTANT: move %s OFFLINE now (e.g. a USB key or password "
                   "manager) and delete it here. Nightly backups need only the "
                   ".pub; verify/restore take --identity <offline copy>." % key)
    runner.step("age-keygen -> %s (0400) and %s" % (key, pub), action)


def restore(path: Path, out: Path, identity=None) -> dict:
    """Decrypt + gunzip a backup into a new 0600 SQLite file after verifying."""
    path, out = Path(path), Path(out)
    counts = verify(path, identity=identity)
    scratch = work_dir(path.resolve().parent)
    with tempfile.TemporaryDirectory(prefix="restore-", dir=scratch) as tmp:
        compressed = path
        if is_encrypted(path):
            compressed = Path(tmp) / "db.sqlite3.gz"
            age_decrypt(path, compressed, identity or identity_path())
        fd = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as target, gzip.open(compressed, "rb") as source:
            shutil.copyfileobj(source, target)
    return counts


def encrypt_existing(directory: Path, runner, identity=None):
    """Encrypt plaintext backups this tool wrote; delete each plaintext only
    after its .age copy decrypts and verifies with the identity."""
    directory = Path(directory)
    if not directory.is_dir():
        return []
    names = sorted(entry.name for entry in directory.iterdir()
                   if entry.is_file() and not entry.is_symlink()
                   and PLAIN_RE.match(entry.name))
    if not names:
        runner.say("no plaintext backups in %s" % directory)
        return []
    recipient = read_recipient()
    identity = Path(identity or identity_path())
    if not identity.is_file():
        raise ToolError("encrypt-existing checks every round trip before deleting "
                        "the plaintext, so it needs the identity (%s)" % identity)
    for name in names:
        source = directory / name
        target = directory / (name + AGE_SUFFIX)

        def action(source=source, target=target):
            if not PLAIN_RE.match(source.name) or source.is_symlink() \
                    or source.parent.resolve() != directory.resolve() \
                    or is_encrypted(source):
                raise GuardError("refusing to touch %s" % source)
            before = verify(source)
            if target.exists():
                raise GuardError("%s already exists" % target.name)
            partial = directory / (target.name + ".part")
            try:
                age_encrypt(source, partial, recipient)
                after = verify(partial, identity=identity)
                if after != before:
                    raise ToolError("%s: round trip differs" % source.name)
                os.replace(partial, target)
            finally:
                partial.unlink(missing_ok=True)
            source.unlink()
            runner.say("%s -> %s (round trip verified; plaintext deleted)" % (
                source.name, target.name))
        runner.step("age-encrypt %s -> %s, verify the round trip, delete the "
                    "plaintext" % (name, name + AGE_SUFFIX), action)
    return names


def main(argv=None, ssh=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    parser.add_argument("--dir", help="backups directory (default data/backups)")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "prune", "keygen"):
        common.add_execute_flags(sub.add_parser(name))
    p = sub.add_parser("encrypt-existing")
    common.add_execute_flags(p)
    p.add_argument("--identity")
    p = sub.add_parser("verify")
    p.add_argument("file")
    p.add_argument("--identity")
    p = sub.add_parser("restore")
    p.add_argument("file")
    p.add_argument("--out", required=True)
    p.add_argument("--identity")
    args = parser.parse_args(argv)
    try:
        if args.command == "verify":
            counts = verify(Path(args.file), identity=args.identity)
            print("ok: %s" % ", ".join("%s=%d" % i for i in sorted(counts.items())))
            return 0
        if args.command == "restore":
            counts = restore(Path(args.file), Path(args.out), args.identity)
            print("restored %s: %s" % (args.out, ", ".join(
                "%s=%d" % i for i in sorted(counts.items()))))
            return 0
        runner = common.runner_from_args(args)
        if args.command == "prune":
            prune(Path(args.dir or backups_dir()), runner)
        elif args.command == "keygen":
            keygen(runner)
        elif args.command == "encrypt-existing":
            encrypt_existing(Path(args.dir or backups_dir()), runner, args.identity)
        else:
            fleet = common.load_fleet(args.fleet)
            run_backup(fleet, ssh or common.Ssh(), runner, args.dir)
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
    except (ToolError, OSError, KeyError, subprocess.SubprocessError) as error:
        print("next_backup: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
