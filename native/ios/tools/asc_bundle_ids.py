#!/usr/bin/env python3
"""Make sure the native app's bundle ids exist in the Apple developer account,
and register test devices.

The native app ships as the App Store app ForkMesh (the App Store app id
com.forkmesh.forkmesh, App Store Connect app 6803060735);
docs/native/IOS_RELEASE.md. The app and its notification extension ids already
exist; the watch ids are made here. Cloud signing at export mints certificates
and profiles on its own, but only for an App ID that already exists, and the
app's needs Push Notifications switched on or the exported build carries no
aps-environment. Running it again changes nothing.

No id needs an App Group: the Home Screen widgets share the app's keychain
group, and the watch app and its widgets share the watch's
(docs/native/IOS_RELEASE.md, "Widgets, Live Activities and the watch").

The API cannot switch Increased Memory Limit on: its capabilityType
list has no INCREASED_MEMORY_LIMIT and answers 409. ensure prints a reminder
instead; tick "Increased Memory Limit" for com.forkmesh.forkmesh once under
Certificates, Identifiers & Profiles > Identifiers in the developer portal.

Reads ASC_KEY_ID and ASC_ISSUER_ID from the environment and the key from
~/.appstoreconnect/private_keys/AuthKey_<ASC_KEY_ID>.p8 (where the
"Check the App Store Connect credentials" step leaves it). Standard library
and the openssl binary only.

    asc_bundle_ids.py ensure                  register what is missing
    asc_bundle_ids.py app-id                  print the App Store Connect app's id, or nothing
    asc_bundle_ids.py devices                 list registered devices (id, class, status, UDID, name)
    asc_bundle_ids.py register-device NAME UDID
                                              register an iPhone, iPad or Apple Watch for ad hoc
                                              builds (a watch lists as platform IOS, class
                                              APPLE_WATCH); prints the existing row when the
                                              UDID is already registered
                                              (add-device is the same command)
    asc_bundle_ids.py has-watch               exit 0 when an enabled Apple Watch is registered
    asc_bundle_ids.py latest-build            print the highest build number uploaded for the app
                                              (0 when there is none)
    asc_bundle_ids.py next-build VERSION      print the next free build number for VERSION: one
                                              above its highest upload, 0 for a version with none
"""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.appstoreconnect.apple.com"
APP_BUNDLE_ID = "com.forkmesh.forkmesh"
# (identifier, name shown in the developer portal, capabilities)
BUNDLE_IDS = [
    (APP_BUNDLE_ID, "ForkMesh Mobile", [
        "PUSH_NOTIFICATIONS", "ASSOCIATED_DOMAINS", "APP_GROUPS", "HEALTHKIT",
        "AUTOFILL_CREDENTIAL_PROVIDER",
    ]),
    (APP_BUNDLE_ID + ".NotificationService", "ForkMesh Notification Service", []),
    (APP_BUNDLE_ID + ".watchkitapp", "ForkMesh Watch", []),
    (APP_BUNDLE_ID + ".watchkitapp.widgets", "ForkMesh Watch Widgets", []),
    (APP_BUNDLE_ID + ".Widgets", "ForkMesh Widgets", []),
    (APP_BUNDLE_ID + ".Share", "ForkMesh Share", ["APP_GROUPS"]),
    (APP_BUNDLE_ID + ".AutoFill", "ForkMesh AutoFill", ["AUTOFILL_CREDENTIAL_PROVIDER"]),
]
PORTAL_ONLY_CAPABILITIES = {"INCREASED_MEMORY_LIMIT": "Increased Memory Limit"}


def b64url(raw: bytes) -> str:
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")


def der_to_raw(der: bytes) -> bytes:
    """An ECDSA signature as openssl writes it (DER SEQUENCE of two INTEGERs)
    to the fixed 64 byte r||s form a JWT carries."""

    def read_length(at: int):
        first = der[at]
        if first < 0x80:
            return first, at + 1
        size = first & 0x7F
        return int.from_bytes(der[at + 1 : at + 1 + size], "big"), at + 1 + size

    if der[0] != 0x30:
        raise ValueError("not a DER sequence")
    _, at = read_length(1)
    parts = []
    for _ in range(2):
        if der[at] != 0x02:
            raise ValueError("not a DER integer")
        length, at = read_length(at + 1)
        parts.append(int.from_bytes(der[at : at + length], "big"))
        at += length
    return parts[0].to_bytes(32, "big") + parts[1].to_bytes(32, "big")


def action_variables() -> dict[str, str]:
    home = Path.home()
    config_dirs = [home / "projects" / "forkmesh" / "config"]
    configured = os.environ.get("FORKMESH_CONFIG_DIR", "").strip()
    if configured:
        config_dirs.insert(0, Path(configured))
    xdg = os.environ.get("XDG_CONFIG_HOME", "").strip()
    config_dirs.append(Path(xdg) if xdg else home / ".config")
    seen = set()
    for config_dir in config_dirs:
        path = config_dir / "ForkMesh" / "ForkMesh.conf"
        if path in seen:
            continue
        seen.add(path)
        try:
            lines = path.read_text().splitlines()
        except OSError:
            continue
        section = ""
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("[") and stripped.endswith("]"):
                section = stripped[1:-1]
                continue
            if section != "actions" or not stripped.startswith("variables="):
                continue
            raw = stripped[len("variables="):]
            if raw.startswith('"') and raw.endswith('"'):
                raw = raw[1:-1]
            raw = re.sub(r"\\(.)", lambda match: {"n": "\n", "t": "\t"}.get(
                match.group(1), match.group(1)), raw)
            if raw.startswith("@ByteArray(") and raw.endswith(")"):
                raw = raw[len("@ByteArray("):-1]
            try:
                parsed = json.loads(raw)
            except ValueError:
                break
            if isinstance(parsed, dict):
                return {name: value.strip() for name, value in parsed.items()
                        if isinstance(value, str)}
    return {}


def credentials() -> tuple[str, str]:
    stored = action_variables()
    values = {
        name: os.environ.get(name, "").strip() or stored.get(name, "")
        for name in ("ASC_KEY_ID", "ASC_ISSUER_ID")
    }
    missing = [name for name, value in values.items() if not value]
    if missing:
        raise SystemExit(
            "ERROR: missing App Store Connect credential "
            f"{' and '.join(missing)}. Set it in ForkMesh Desktop under Settings -> Secrets & Coves, "
            "or export it in this shell."
        )
    return values["ASC_KEY_ID"], values["ASC_ISSUER_ID"]


def token() -> str:
    key_id, issuer = credentials()
    key_path = Path.home() / ".appstoreconnect" / "private_keys" / f"AuthKey_{key_id}.p8"
    if not key_path.is_file():
        raise SystemExit(f"ERROR: no App Store Connect key at {key_path}")
    header = {"alg": "ES256", "kid": key_id, "typ": "JWT"}
    now = int(time.time())
    claims = {"iss": issuer, "iat": now, "exp": now + 600, "aud": "appstoreconnect-v1"}
    signing_input = (
        b64url(json.dumps(header, separators=(",", ":")).encode())
        + "."
        + b64url(json.dumps(claims, separators=(",", ":")).encode())
    )
    der = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(key_path)],
        input=signing_input.encode("ascii"),
        stdout=subprocess.PIPE,
        check=True,
    ).stdout
    return signing_input + "." + b64url(der_to_raw(der))


def call(bearer: str, method: str, path: str, body=None):
    request = urllib.request.Request(
        API + path,
        method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Authorization": "Bearer " + bearer, "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read()
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        raise SystemExit(f"ERROR: App Store Connect {method} {path} answered {error.code}: {detail}")
    return json.loads(raw) if raw else {}


def download(bearer: str, path: str) -> bytes | None:
    request = urllib.request.Request(
        API + path, headers={"Authorization": "Bearer " + bearer, "Accept": "application/a-gzip"}
    )
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", "replace")
        if error.code == 404:
            return None
        raise SystemExit(f"ERROR: App Store Connect GET {path} answered {error.code}: {detail}")


def find_bundle_id(bearer: str, identifier: str):
    query = urllib.parse.urlencode(
        {"filter[identifier]": identifier, "include": "bundleIdCapabilities", "limit": "200"}
    )
    found = call(bearer, "GET", "/v1/bundleIds?" + query)
    # The filter is a prefix match: the app's id also matches its extension's.
    for row in found.get("data", []):
        if row["attributes"]["identifier"] == identifier:
            wanted = {
                ref["id"]
                for ref in row.get("relationships", {})
                .get("bundleIdCapabilities", {})
                .get("data", [])
            }
            capabilities = {
                item["attributes"]["capabilityType"]
                for item in found.get("included", [])
                if item["type"] == "bundleIdCapabilities" and item["id"] in wanted
            }
            return row["id"], capabilities
    return None, set()


def ensure() -> None:
    bearer = token()
    for identifier, name, capabilities in BUNDLE_IDS:
        resource_id, present = find_bundle_id(bearer, identifier)
        if resource_id is None:
            created = call(
                bearer,
                "POST",
                "/v1/bundleIds",
                {
                    "data": {
                        "type": "bundleIds",
                        "attributes": {"identifier": identifier, "name": name, "platform": "IOS"},
                    }
                },
            )
            resource_id = created["data"]["id"]
            print(f"Registered {identifier}")
        else:
            print(f"{identifier} is already registered")
        for capability in capabilities:
            if capability in present:
                continue
            if capability in PORTAL_ONLY_CAPABILITIES:
                print(
                    f'NOTE: the App Store Connect API cannot switch {capability} on. Tick '
                    f'"{PORTAL_ONLY_CAPABILITIES[capability]}" for {identifier} once in the developer portal '
                    "(Certificates, Identifiers & Profiles > Identifiers) if it is not on yet."
                )
                continue
            call(
                bearer,
                "POST",
                "/v1/bundleIdCapabilities",
                {
                    "data": {
                        "type": "bundleIdCapabilities",
                        "attributes": {"capabilityType": capability},
                        "relationships": {
                            "bundleId": {"data": {"type": "bundleIds", "id": resource_id}}
                        },
                    }
                },
            )
            print(f"Enabled {capability} on {identifier}")


def app_id() -> None:
    query = urllib.parse.urlencode({"filter[bundleId]": APP_BUNDLE_ID, "limit": "200"})
    found = call(token(), "GET", "/v1/apps?" + query)
    for row in found.get("data", []):
        if row["attributes"]["bundleId"] == APP_BUNDLE_ID:
            print(row["id"])
            return


def uploaded_builds(marketing_version: str = "") -> list[int]:
    bearer = token()
    query = urllib.parse.urlencode({"filter[bundleId]": APP_BUNDLE_ID, "limit": "200"})
    apps = [row["id"] for row in call(bearer, "GET", "/v1/apps?" + query).get("data", [])
            if row["attributes"]["bundleId"] == APP_BUNDLE_ID]
    if not apps:
        return []
    params = {"filter[app]": apps[0], "sort": "-uploadedDate",
              "fields[builds]": "version", "limit": "200"}
    if marketing_version:
        params["filter[preReleaseVersion.version]"] = marketing_version
    rows = call(bearer, "GET", "/v1/builds?" + urllib.parse.urlencode(params)).get("data", [])
    return [int(str(row["attributes"].get("version", ""))) for row in rows
            if str(row["attributes"].get("version", "")).isdigit()]


def latest_build() -> None:
    print(max(uploaded_builds(), default=0))


def next_build(marketing_version: str) -> None:
    if not re.fullmatch(r"[0-9]+(\.[0-9]+){0,2}", marketing_version):
        raise SystemExit(f"ERROR: {marketing_version!r} is not a MAJOR.MINOR.PATCH version")
    builds = uploaded_builds(marketing_version)
    print(max(builds) + 1 if builds else 0)


def list_devices(bearer: str):
    rows = []
    path = "/v1/devices?limit=200"
    while path:
        page = call(bearer, "GET", path)
        rows += page.get("data", [])
        following = page.get("links", {}).get("next")
        path = following[len(API):] if following else None
    return rows


def devices() -> None:
    for row in list_devices(token()):
        a = row["attributes"]
        print(f'{row["id"]}\t{a.get("deviceClass")}\t{a.get("status")}\t{a.get("udid")}\t{a.get("name")}')


def register_device(name: str, udid: str) -> None:
    udid = udid.strip()
    compact = udid.replace("-", "")
    if not name.strip() or not compact or any(c not in "0123456789abcdefABCDEF" for c in compact):
        raise SystemExit("ERROR: usage: register-device NAME UDID (UDID: hex digits and dashes)")
    bearer = token()
    for row in list_devices(bearer):
        if row["attributes"].get("udid", "").lower() == udid.lower():
            a = row["attributes"]
            print(f'{udid} is already registered: {row["id"]} {a.get("deviceClass")} {a.get("status")} "{a.get("name")}"')
            return
    created = call(
        bearer,
        "POST",
        "/v1/devices",
        {"data": {"type": "devices", "attributes": {"name": name.strip(), "udid": udid, "platform": "IOS"}}},
    )
    a = created["data"]["attributes"]
    print(f'Registered {udid} as {created["data"]["id"]} ({a.get("deviceClass")}, {a.get("status")}).')
    print("Run ios-native-adhoc again: the ad hoc profile only covers devices registered when it is minted.")


def has_watch() -> None:
    watches = [
        row
        for row in list_devices(token())
        if row["attributes"].get("deviceClass") == "APPLE_WATCH" and row["attributes"].get("status") == "ENABLED"
    ]
    print(len(watches))
    raise SystemExit(0 if watches else 1)


if __name__ == "__main__":
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "ensure":
        ensure()
    elif command == "app-id":
        app_id()
    elif command == "devices":
        devices()
    elif command in ("register-device", "add-device") and len(sys.argv) == 4:
        register_device(sys.argv[2], sys.argv[3])
    elif command == "has-watch":
        has_watch()
    elif command == "latest-build":
        latest_build()
    elif command == "next-build" and len(sys.argv) == 3:
        next_build(sys.argv[2])
    else:
        raise SystemExit(__doc__)
