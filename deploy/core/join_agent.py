#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import json
import os
import re
import ssl
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
from pathlib import Path

RELEASES = "/opt/forkmesh-next/app/releases"
SNAPSHOT_DIR = "/opt/forkmesh-next/snapshot"
CREDS = "/etc/forkmesh-next/creds"
IDS_KEY_PATH = "/var/lib/fmnext-ids/sign.key"
NAME_RE = re.compile(r"^[ab][1-9]$")
CRED_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_.-]{1,63}$")
HOST_RE = re.compile(r"^[a-z0-9.-]{3,253}$")
REV_RE = re.compile(r"^[0-9a-f]{12}$")


class JoinError(RuntimeError):
    pass


def log(message):
    print("%s fmnext-join: %s" % (time.strftime("%H:%M:%S"), message), flush=True)


def sh(script, stdin=None, check=True, timeout=1800):
    done = subprocess.run(["/bin/bash", "-euo", "pipefail", "-c", script],
                          input=stdin, capture_output=True, timeout=timeout)
    if check and done.returncode != 0:
        raise JoinError("step failed (exit %d): %s" % (
            done.returncode, done.stderr.decode("utf-8", "replace")[-600:]))
    return done


def b64u(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def ids_keypair():
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    os.makedirs("/var/lib/fmnext-ids", mode=0o700, exist_ok=True)
    if not os.path.exists(IDS_KEY_PATH):
        raw = Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
            serialization.NoEncryption())
        fd = os.open(IDS_KEY_PATH, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as handle:
            handle.write(b64u(raw) + "\n")
    text = open(IDS_KEY_PATH).read().strip()
    raw = base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))
    key = Ed25519PrivateKey.from_private_bytes(raw)
    return b64u(key.public_key().public_bytes(serialization.Encoding.Raw,
                                              serialization.PublicFormat.Raw))


def wireguard_key():
    done = sh("umask 077; install -d -m 0700 /etc/wireguard; "
              "[ -s /etc/wireguard/wg0.key ] || wg genkey >/etc/wireguard/wg0.key; "
              "wg pubkey </etc/wireguard/wg0.key")
    public = done.stdout.decode().strip()
    if not re.fullmatch(r"[A-Za-z0-9+/]{43}=", public):
        raise JoinError("unexpected WireGuard public key")
    return public


def call(app_host, path, payload=None, token=None, timeout=60, raw=False):
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request("https://%s%s" % (app_host, path), data=data,
                                     method="POST" if data is not None else "GET")
    request.add_header("Content-Type", "application/json")
    request.add_header("User-Agent", "fmnext-join/1")
    if token:
        request.add_header("x-fmnext-join-token", token)
    context = ssl.create_default_context()
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        body = response.read()
    return body if raw else json.loads(body or b"{}")


def with_retries(what, action, attempts=12, delay=10):
    for attempt in range(1, attempts + 1):
        try:
            return action()
        except urllib.error.HTTPError as error:
            detail = error.read(300).decode("utf-8", "replace")
            if error.code < 500 and error.code not in (409, 429):
                raise JoinError("%s refused: HTTP %d %s" % (what, error.code, detail))
            log("%s: HTTP %d (attempt %d)" % (what, error.code, attempt))
        except (urllib.error.URLError, OSError, ValueError) as error:
            log("%s: %s (attempt %d)" % (what, str(error)[:120], attempt))
        time.sleep(delay)
    raise JoinError("%s: giving up" % what)


def install_files(bundle):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        for path, (content, mode) in sorted(bundle["files"].items()):
            if not path.startswith("/") or ".." in path.split("/"):
                raise JoinError("bad file path %r" % path)
            data = base64.b64decode(content)
            info = tarfile.TarInfo(path.lstrip("/"))
            info.size = len(data)
            info.mode = int(mode)
            info.mtime = 0
            archive.addfile(info, io.BytesIO(data))
    target = "/tmp/fmnext-join-files.tar.gz"
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as handle:
        handle.write(buffer.getvalue())
    sh(bundle["scripts"]["files"])


def install_credentials(bundle):
    os.makedirs(CREDS, mode=0o700, exist_ok=True)
    for name, value in sorted(bundle.get("credentials", {}).items()):
        if not CRED_RE.match(name):
            raise JoinError("bad credential name %r" % name)
        sh('umask 077; systemd-creds encrypt --with-key=host --name="%s" - '
           '"%s/%s.cred.new" && mv "%s/%s.cred.new" "%s/%s.cred"' % (
               name, CREDS, name, CREDS, name, CREDS, name),
           stdin=base64.b64decode(value))
    if bundle.get("curlrc"):
        sh("umask 077; cat >/etc/forkmesh-next/rqlite.curlrc.new && "
           "mv /etc/forkmesh-next/rqlite.curlrc.new /etc/forkmesh-next/rqlite.curlrc",
           stdin=bundle["curlrc"].encode())


def start_without_ids(script):
    lines = []
    for line in script.splitlines():
        if line.strip() in ("systemctl restart fmnext-ids.service",
                            "systemctl try-restart fmnext-ids.service",
                            "systemctl enable --now fmnext-ids.service"):
            continue
        if line.startswith("systemctl enable --now "):
            line = line.replace(" fmnext-ids.service", "")
        lines.append(line)
    return "\n".join(lines)


def install_daemon(bundle):
    daemon = bundle["daemon"]
    sh("rm -rf /tmp/fmnext-daemon && install -d -m 0700 /tmp/fmnext-daemon/tools && "
       "install -m 0755 %s/forkmesh-mirror-node /tmp/fmnext-daemon/forkmesh-mirror-node && "
       "cp -a %s/tools/. /tmp/fmnext-daemon/tools/" % (SNAPSHOT_DIR, SNAPSHOT_DIR))
    sh("umask 077; cat >/tmp/fmnext-router-key",
       stdin=daemon["routerPublicKey"].encode())
    sh(daemon["script"])


def runner_public_key():
    done = sh("/usr/local/bin/forkmesh-mirror-node --config "
              "/etc/forkmesh/mirror-node.json --public-key")
    public = done.stdout.decode().strip().splitlines()[-1].strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{43}", public):
        raise JoinError("unexpected runner public key")
    return public


def install_release(bundle, app_host, token):
    release = bundle["release"]
    rev = release["rev"]
    if not REV_RE.match(rev):
        raise JoinError("bad release id")
    data = with_retries("release download", lambda: call(
        app_host, "/api/fleet/release/%s" % rev, token=token, timeout=600, raw=True))
    if hashlib.sha256(data).hexdigest() != release["sha256"]:
        raise JoinError("release %s does not match its sha256" % rev)
    target = Path(RELEASES) / (".incoming-%s.tar.gz" % rev)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    sh(release["upload"])
    sh(release["activate"], timeout=1800)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="fmnext-join")
    parser.add_argument("--name", required=True)
    parser.add_argument("--app-host", required=True)
    parser.add_argument("--token", required=True)
    args = parser.parse_args(argv)
    if not NAME_RE.match(args.name) or not HOST_RE.match(args.app_host):
        raise SystemExit("bad arguments")
    marker = Path("/var/lib/fmnext-join.done")
    if marker.exists():
        log("already joined")
        return 0
    app = args.name.startswith("a")
    log("joining as %s via %s" % (args.name, args.app_host))
    payload = {"token": args.token, "name": args.name, "idsPublicKey": ids_keypair()}
    if app:
        payload["wgPublicKey"] = wireguard_key()
    bundle = with_retries("join", lambda: call(args.app_host, "/api/fleet/join",
                                               payload))
    if bundle.get("name") != args.name:
        raise JoinError("the bundle is for another node")
    log("bundle received (%d files)" % len(bundle["files"]))
    install_files(bundle)
    install_credentials(bundle)
    finish = {"token": args.token}
    if app:
        sh(start_without_ids(bundle["scripts"]["start"]))
        log("waiting for the cores to add this node's WireGuard peer")
        install_release(bundle, args.app_host, args.token)
    else:
        sh(start_without_ids(bundle["scripts"]["start"]))
        install_daemon(bundle)
        finish["runnerPublicKey"] = runner_public_key()
    sh("systemctl enable --now fmnext-ids.service")
    with_retries("joined", lambda: call(args.app_host, "/api/fleet/joined", finish))
    marker.write_text(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) + "\n")
    sh("rm -f /var/lib/cloud/instance/user-data.txt "
       "/var/lib/cloud/instance/user-data.txt.i /tmp/fmnext-join-files.tar.gz",
       check=False)
    log("joined")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except JoinError as error:
        log("FAILED: %s" % error)
        sys.exit(1)
