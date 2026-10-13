"""``python -m forkmesh_host ops ...``: one-off operator actions on a node.

ForkMesh Next runs without outbound mail, so the email-verification step
and the website's admin flows that assume a mailbox or an admin node key
need a node-side way in. These commands run the app's own record helpers
(``entry._account_row`` / ``_save_account``) against the live database, with
the app's own environment and credentials. On a node, run them through
``fmnext-app-ops``, which starts this under the forkmesh-app unit's user,
environment files and encrypted credentials.

    python -m forkmesh_host ops verify-email <account>
    python -m forkmesh_host ops show-account <account>
    python -m forkmesh_host ops service-account <account>
    python -m forkmesh_host ops early-ios-testers all

With the vault on (``FORKMESH_VAULT=1``) this process never builds its own
runtime (it would have no key): the action runs inside the running app,
sent over the unseal socket as ``ops <action> <account>`` (vault.py).
"""

from __future__ import annotations

import asyncio
import json
import os
import re


def _runtime(args):
    from .serve import _config
    from .runtime import HostRuntime
    return HostRuntime(_config(args))


async def _verify_email(runtime, name):
    entry, env = runtime.entry, runtime.env
    name = name.strip().lower()
    name_bi, rec = await entry._account_row(env, name)
    if not rec or rec.get("status") != "active":
        return {"ok": False, "error": "no_such_account", "account": name}
    if entry._account_kind(rec) != "user":
        return {"ok": False, "error": "not_a_user", "account": name}
    if not str(rec.get("email") or "").strip():
        return {"ok": False, "error": "account_has_no_email", "account": name}
    if rec.get("email_verified") is True:
        return {"ok": True, "account": name, "alreadyVerified": True}
    rec["email_verified"] = True
    rec["email_verified_at"] = int(entry.Date.now())
    # Same marker as the admin override: nobody proved the mailbox.
    rec["email_verification_method"] = "admin_override"
    rec["email_verified_by"] = "node-operator"
    await entry._save_account(env, name_bi, rec)
    await entry.d1_run(
        env, "DELETE FROM pending_verifications WHERE name_bi=?", name_bi)
    return {"ok": True, "account": name, "verified": True}


async def _show_account(runtime, name):
    entry, env = runtime.entry, runtime.env
    name = name.strip().lower()
    _bi, rec = await entry._account_row(env, name)
    if not rec:
        return {"ok": False, "error": "no_such_account", "account": name}
    return {"ok": True, "account": name, "kind": entry._account_kind(rec),
            "status": rec.get("status"),
            "emailVerified": rec.get("email_verified") is True,
            "serviceAccount": rec.get("service_account") is True,
            "admin": await entry._is_admin(env, name)}


async def _service_account(runtime, name):
    entry, env = runtime.entry, runtime.env
    name = name.strip().lower()
    name_bi, rec = await entry._account_row(env, name)
    if not rec:
        return {"ok": False, "error": "no_such_account", "account": name}
    if rec.get("service_account") is True:
        return {"ok": True, "account": name, "alreadyServiceAccount": True}
    rec["service_account"] = True
    await entry._save_account(env, name_bi, rec)
    return {"ok": True, "account": name, "serviceAccount": True}


async def _early_ios_testers(runtime, _scope):
    entry, env = runtime.entry, runtime.env
    rows = await entry.d1_all(
        env, "SELECT username, data FROM users WHERE early_ios_tester=1 "
        "ORDER BY username")
    testers, skipped = [], []
    for row in rows:
        name = str(row.get("username") or "")
        rec = await entry.decrypt_row(env, row.get("data")) if row.get(
            "data") else None
        rec = rec if isinstance(rec, dict) else {}
        email = str(rec.get("email") or "").strip().lower()
        if rec.get("status") != "active":
            skipped.append({"account": name, "reason": "not_active"})
        elif not email:
            skipped.append({"account": name, "reason": "no_email"})
        elif rec.get("email_verified") is not True:
            skipped.append({"account": name, "reason": "email_not_verified"})
        else:
            testers.append({"account": name, "email": email})
    return {"ok": True, "testers": testers, "skipped": skipped}


IOS_INSTALL_PAGE = "https://forkmesh.com/ios-install.html"
IOS_BUILD_SPEC = re.compile(
    r"([0-9a-f]{64}):([0-9A-Za-z.\-]{1,32}):([0-9A-Za-z.\-]{1,32}):([0-9a-f]{0,40})")


def ios_build_ping(version, build, commit, page):
    title = "iPhone build %s (%s) is ready" % (version, build)
    body = "Tap to install it on a registered iPhone: " + page
    if commit:
        body += "\nBuilt from " + commit[:12] + "."
    return title, body


async def _ios_build_ping(runtime, spec):
    entry, env = runtime.entry, runtime.env
    match = IOS_BUILD_SPEC.fullmatch(spec.strip())
    if not match:
        return {"ok": False, "error": "bad_spec",
                "usage": "<manifest-sha256>:<version>:<build>:<commit>"}
    digest, version, build, commit = match.groups()
    page = (IOS_INSTALL_PAGE + "?build=" + build + "&v=" + digest[:12]
            + "#" + digest)
    title, body = ios_build_ping(version, build, commit, page)
    rows = await entry.d1_all(
        env, "SELECT username, data FROM users WHERE early_ios_tester=1 "
        "ORDER BY username")
    pinged, skipped = [], []
    for row in rows:
        name = str(row.get("username") or "")
        rec = await entry.decrypt_row(env, row.get("data")) if row.get(
            "data") else None
        rec = rec if isinstance(rec, dict) else {}
        if rec.get("status") != "active":
            skipped.append({"account": name, "reason": "not_active"})
            continue
        filed = await entry.enqueue_notification(
            env, name, "release_published", title, body=body, href=page,
            source="ios_build", dedupe="ios-build:" + digest,
            meta={"version": version, "build": build, "commit": commit})
        if filed:
            pinged.append(name)
        else:
            skipped.append({"account": name, "reason": "not_filed"})
    return {"ok": True, "page": page, "pinged": pinged, "skipped": skipped}


COMMANDS = {"verify-email": _verify_email, "show-account": _show_account,
            "service-account": _service_account,
            "early-ios-testers": _early_ios_testers,
            "ios-build-ping": _ios_build_ping}


def cmd_ops(args):
    from . import vault
    if vault.enabled(os.environ):
        # Vault on: this process has no key and must not get one. Run the
        # action inside the running (unsealed) app over its socket.
        try:
            result = vault.socket_request(
                ["ops %s %s" % (args.action, args.account)])
        except OSError as error:
            result = {"ok": False, "error": "app_socket_unavailable",
                      "detail": type(error).__name__}
        print(json.dumps(result, sort_keys=True))
        return 0 if result.get("ok") else 1
    runtime = _runtime(args)
    result = asyncio.run(COMMANDS[args.action](runtime, args.account))
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("ok") else 1


def add_parser(sub, common):
    ops = sub.add_parser("ops", help="one-off operator actions")
    common(ops)
    ops.add_argument("action", choices=sorted(COMMANDS))
    ops.add_argument("account")
    ops.set_defaults(func=cmd_ops)
