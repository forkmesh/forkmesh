#!/usr/bin/env python3
"""Production's identity values into the Next vault's source (docs/CUTOVER.md)."""

from __future__ import annotations

import argparse
import hashlib
import hmac
import os
import re
import secrets
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import next_common as common  # noqa: E402

PROD_ENV = Path.home() / "projects" / "forkmesh" / "src" / "app" / ".env.production"
ADOPT = (
    "MIRROR_ROUTER_SIGNING_SEED", "MIRROR_ROUTER_PUBLIC_KEY",
    "DISCORD_BOT_TOKEN", "DISCORD_CLIENT_ID", "DISCORD_CLIENT_SECRET",
    "SENTRY_DSN", "TREASURY_SOLANA_ADDRESS",
    "APNS_AUTH_KEY", "APNS_KEY_ID", "APNS_TEAM_ID",
    "MAILTRAP_API_TOKEN", "ADMIN_PATH",
    "PRODUCT_ANALYSIS_USER", "PRODUCT_ANALYSIS_PASS",
    "SSH_GATEWAY_HOST", "SSH_GATEWAY_PORT", "SSH_GATEWAY_TOKEN",
    "SSH_GATEWAY_REPOSITORIES", "SSH_GATEWAY_NODE_HOSTS",
    "SSH_GATEWAY_TOKEN_NEXT", "SSH_GATEWAY_ALLOWED_ADDRESSES",
)
NEVER = ("DATA_KEY", "ADMIN_PASS", "FORKMESH_MESH_SECRET", "RQLITE_PASSWORD",
         "FORKMESH_CERTSYNC_SECRET", "FMNEXT_IDS_MASTER", "CLOUDFLARE_API_TOKEN",
         "CLOUDFLARE_ACCOUNT_ID", "VULTR_API_KEY")
ROTATE = ("WWW_EDGE_SECRET",)
BACKUP_NAME = "secrets.env.pre-cutover"
LINE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$")


class IdentityError(RuntimeError):
    pass


def log(message):
    print(message, flush=True)


def read_private_env(path):
    path = Path(path)
    if path.stat().st_mode & 0o077:
        raise IdentityError("%s is readable by group/other; chmod 0600 it" % path)
    return common.read_env_file(path)


def same(a, b):
    return hmac.compare_digest(hashlib.sha256(a.encode()).digest(),
                               hashlib.sha256(b.encode()).digest())


KEEP_KEY_ADOPT = ("ADMIN_PASS",)
NEXT_KEY_NAME = "DATA_KEY.next-pre-cutover"


def adopted(keep_key):
    return ADOPT + (KEEP_KEY_ADOPT if keep_key else ())


def plan(prod_env=PROD_ENV, keep_key=False):
    prod = read_private_env(prod_env)
    current = read_private_env(common.config_dir() / "secrets.env")
    rows = []
    for name in adopted(keep_key):
        if not prod.get(name):
            state = "absent in production (skipped)"
        elif not current.get(name):
            state = "new"
        elif same(prod[name], current[name]):
            state = "same"
        else:
            state = "replaced by production's"
        rows.append((name, state))
    for name in ROTATE:
        rows.append((name, "rotated (new random value)"))
    for name, state in rows:
        log("%-28s %s" % (name, state))
    return rows


def rewrite_env(text, updates):
    seen = set()
    out = []
    for line in text.splitlines():
        match = LINE.match(line)
        if match and match.group(1) in updates:
            name = match.group(1)
            if name in seen:
                continue
            seen.add(name)
            out.append("%s=%s" % (name, updates[name]))
        else:
            out.append(line)
    for name, value in updates.items():
        if name not in seen:
            out.append("%s=%s" % (name, value))
    return "\n".join(out) + "\n"


def apply(execute=False, prod_env=PROD_ENV, token=secrets.token_urlsafe,
          keep_key=False):
    rows = plan(prod_env, keep_key)
    if not execute:
        log("dry-run: pass --execute")
        return None
    prod = read_private_env(prod_env)
    updates = {name: prod[name] for name, _ in rows
               if name in adopted(keep_key) and prod.get(name)}
    for name in NEVER:
        if not (keep_key and name in KEEP_KEY_ADOPT):
            updates.pop(name, None)
    for name, value in updates.items():
        if "\n" in value or "\r" in value:
            raise IdentityError("%s has a line break; refusing" % name)
    for name in ROTATE:
        updates[name] = token(48)
    target = common.config_dir() / "secrets.env"
    backup = common.config_dir() / BACKUP_NAME
    if backup.exists():
        raise IdentityError("%s exists (a previous apply); roll back or remove it first"
                            % backup)
    fd = os.open(str(backup), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(target.read_text("utf-8"))
    new_text = rewrite_env(target.read_text("utf-8"), updates)
    tmp = target.with_name(target.name + ".tmp")
    fd = os.open(str(tmp), os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(new_text)
    os.replace(tmp, target)
    log("secrets.env: %d value(s) written (%s); previous copy in %s" % (
        len(updates), ", ".join(sorted(updates)), backup.name))
    return sorted(updates)


def rollback(execute=False):
    target = common.config_dir() / "secrets.env"
    backup = common.config_dir() / BACKUP_NAME
    if not backup.is_file():
        raise IdentityError("no %s to restore" % backup)
    log("restore %s -> secrets.env" % backup.name)
    if not execute:
        log("dry-run: pass --execute")
        return
    shutil.copyfile(backup, target)
    os.chmod(target, 0o600)
    backup.unlink()


def write_key_file(path, text):
    tmp = path.with_name(path.name + ".tmp")
    fd = os.open(str(tmp), os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o400)
    with os.fdopen(fd, "w") as handle:
        handle.write(text + "\n")
    os.replace(tmp, path)
    os.chmod(path, 0o400)


def key_id(text):
    sys.path.insert(0, str(HERE.parent / "app" / "host"))
    from forkmesh_host.vault_crypto import VaultKey
    return VaultKey(text).key_id


def install_data_key(execute=False, prod_env=PROD_ENV):
    config = common.config_dir()
    current, saved = config / "DATA_KEY", config / NEXT_KEY_NAME
    prod = read_private_env(prod_env).get("DATA_KEY", "").strip()
    if len(prod) < 16:
        raise IdentityError("production DATA_KEY missing from %s" % prod_env)
    if "DATA_KEY" in read_private_env(config / "secrets.env"):
        raise IdentityError("secrets.env has a DATA_KEY= line; remove it first")
    now = current.read_text("utf-8").strip()
    if same(now, prod):
        log("config/DATA_KEY is production's already (key_id %s)" % key_id(prod))
        return
    if saved.exists():
        raise IdentityError("%s exists; refusing to overwrite it" % saved)
    log("config/DATA_KEY key_id %s -> %s (production); Next's key kept as %s "
        "(0400)" % (key_id(now), key_id(prod), NEXT_KEY_NAME))
    if not execute:
        log("dry-run: pass --execute")
        return
    write_key_file(saved, now)
    write_key_file(current, prod)
    log("install-data-key: done")


def restore_data_key(execute=False):
    config = common.config_dir()
    current, saved = config / "DATA_KEY", config / NEXT_KEY_NAME
    if not saved.is_file():
        raise IdentityError("no %s to restore" % saved)
    log("config/DATA_KEY -> Next's key_id %s (from %s)" % (
        key_id(saved.read_text("utf-8").strip()), NEXT_KEY_NAME))
    if not execute:
        log("dry-run: pass --execute")
        return
    write_key_file(current, saved.read_text("utf-8").strip())


def put_www_secret(execute=False, prod_env=PROD_ENV, run=subprocess.run,
                   wrangler=None, worker="forkmesh-www"):
    value = read_private_env(common.config_dir() / "secrets.env").get("WWW_EDGE_SECRET")
    if not value:
        raise IdentityError("WWW_EDGE_SECRET is not in secrets.env (run apply)")
    command = (wrangler or ["npx", "--yes", "wrangler@4.136.3"]) + [
        "secret", "put", "WWW_EDGE_SECRET", "--name", worker]
    log("put-www-secret: %s (value on stdin)" % " ".join(command))
    if not execute:
        log("dry-run: pass --execute")
        return None
    creds = read_private_env(prod_env)
    env = {k: v for k, v in os.environ.items()
           if k not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    env["CLOUDFLARE_API_TOKEN"] = creds["CLOUDFLARE_API_TOKEN"]
    env["CLOUDFLARE_ACCOUNT_ID"] = creds["CLOUDFLARE_ACCOUNT_ID"]
    result = run(command, input=value.encode(), env=env, capture_output=True,
                 timeout=300, check=False)
    if result.returncode != 0:
        raise IdentityError("wrangler secret put failed (exit %d)" % result.returncode)
    log("put-www-secret: done")
    return result.returncode


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--keep-production-key", action="store_true")
    for name in ("apply", "rollback", "put-www-secret", "install-data-key",
                 "restore-data-key"):
        p = sub.add_parser(name)
        p.add_argument("--execute", action="store_true")
        if name == "apply":
            p.add_argument("--keep-production-key", action="store_true")
    parser.add_argument("--production-env", default=str(PROD_ENV))
    args = parser.parse_args(argv)
    try:
        if args.command == "plan":
            plan(Path(args.production_env), args.keep_production_key)
        elif args.command == "apply":
            apply(args.execute, Path(args.production_env),
                  keep_key=args.keep_production_key)
        elif args.command == "install-data-key":
            install_data_key(args.execute, Path(args.production_env))
        elif args.command == "restore-data-key":
            restore_data_key(args.execute)
        elif args.command == "rollback":
            rollback(args.execute)
        else:
            put_www_secret(args.execute, Path(args.production_env))
        return 0
    except (IdentityError, OSError, KeyError) as error:
        print("cutover_identity: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
