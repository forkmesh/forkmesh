#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import secrets
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


CONTEXT = "forkmesh-ecosystem-import-v1"
DEFAULT_ORIGIN = "https://app.forkmesh.com"
IMPORT_PATH = "/api/ecosystem/import"
USER_AGENT = "forkmesh-ecosystem-seed/1"


def canonical(node, ts, request_id, payload):
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()
    return "\n".join((CONTEXT, node, ts, request_id, digest)).encode("utf-8")


def b64url(data):
    return base64.urlsafe_b64encode(data).decode("ascii").rstrip("=")


def default_key_path():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import forkmesh_paths
    return forkmesh_paths.resolve().identity_key


def load_key(path):
    try:
        from cryptography.hazmat.primitives import serialization
    except ImportError:
        raise SystemExit("ecosystem_seed: this needs the cryptography package "
                         "(python3 -m pip install cryptography)")
    return serialization.load_pem_private_key(
        Path(path).read_bytes(), password=None)


def signed_body(key, node, payload, now_ms=None, request_id=None):
    ts = str(int(time.time() * 1000) if now_ms is None else int(now_ms))
    request_id = request_id or secrets.token_urlsafe(24)
    signature = key.sign(canonical(node, ts, request_id, payload))
    return {"node": node, "ts": ts, "requestId": request_id,
            "payload": payload, "sig": b64url(signature)}


def post(origin, body, timeout=120):
    request = urllib.request.Request(
        origin.rstrip("/") + IMPORT_PATH,
        data=json.dumps(body).encode("utf-8"), method="POST",
        headers={"content-type": "application/json",
                 "accept": "application/json", "user-agent": USER_AGENT})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.status, response.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as error:
        return error.code, error.read().decode("utf-8", "replace")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Load ForkMesh Ecosystem projects from a JSON file into a "
                    "relay through its signed admin import API "
                    "(POST /api/ecosystem/import). Projects merge by slug and "
                    "nothing is written unless every project is valid.")
    parser.add_argument(
        "file", help="projects as JSON: one object, a list, or an admin export")
    parser.add_argument(
        "--node", required=True,
        help="the admin account whose node key signs the import")
    parser.add_argument(
        "--key", help="that account's Ed25519 identity key (default: this "
                      "machine's ForkMesh identity key)")
    parser.add_argument(
        "--origin", default=DEFAULT_ORIGIN,
        help="the App origin (default: %(default)s)")
    args = parser.parse_args(argv)
    payload = Path(args.file).read_text(encoding="utf-8")
    try:
        json.loads(payload)
    except ValueError as error:
        print("ecosystem_seed: %s is not valid JSON: %s" % (args.file, error),
              file=sys.stderr)
        return 2
    key = load_key(args.key or default_key_path())
    status, text = post(
        args.origin, signed_body(key, args.node.strip().lower(), payload))
    try:
        result = json.loads(text)
    except ValueError:
        result = {"error": text.strip()[:500]}
    if status == 200 and result.get("ok"):
        added, updated, unchanged = (
            int(result.get(name) or 0)
            for name in ("added", "updated", "unchanged"))
        print("Imported %d projects: %d added, %d updated, %d unchanged." % (
            added + updated + unchanged, added, updated, unchanged))
        return 0
    print("ecosystem_seed: HTTP %d %s" % (status, result.get("error", "")),
          file=sys.stderr)
    for line in result.get("errors") or []:
        print("  " + str(line), file=sys.stderr)
    if status == 401:
        print("  Check that --node is an admin account and --key is that "
              "account's node key.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
