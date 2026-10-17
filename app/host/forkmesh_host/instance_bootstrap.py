from __future__ import annotations

import asyncio
import json
import re
import sys

NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,37}[a-z0-9])?$")
PUBKEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
NODE = "forkmesh"
REPO = "forkmesh"
MAX_OWNERS = 20
MIRROR_FEATURES = {"issues": False, "pulls": False, "discussions": False}
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]{1,64}@"
    r"(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$")


class BootstrapError(Exception):
    pass


def read_owners(value, admin, email, owner):
    if value in (None, ""):
        return []
    if not isinstance(value, list) or len(value) > MAX_OWNERS:
        raise BootstrapError("invalid owners")
    owners, names, emails = [], {admin, owner, NODE}, {email}
    for item in value:
        if not isinstance(item, dict):
            raise BootstrapError("invalid owners")
        name = str(item.get("username") or "").strip().lower()
        mail = str(item.get("email") or "").strip().lower()
        password = item.get("password")
        if not NAME_RE.fullmatch(name) or name in names:
            raise BootstrapError("invalid owner username")
        if not EMAIL_RE.fullmatch(mail) or mail in emails:
            raise BootstrapError("invalid owner email for %s" % name)
        if not isinstance(password, str) or not 8 <= len(password) <= 256:
            raise BootstrapError("invalid owner password for %s" % name)
        names.add(name)
        emails.add(mail)
        owners.append({"name": name, "email": mail, "password": password})
    return owners


def read_spec(raw):
    try:
        spec = json.loads(raw)
    except ValueError as error:
        raise BootstrapError("the settings are not JSON") from error
    if not isinstance(spec, dict):
        raise BootstrapError("the settings are not an object")
    admin = str(spec.get("adminUsername") or "").strip().lower()
    email = str(spec.get("adminEmail") or "").strip().lower()
    password = spec.get("adminPassword")
    owner_type = str(spec.get("ownerType") or "org").strip().lower()
    owner = str(spec.get("ownerName") or "").strip().lower()
    if not NAME_RE.fullmatch(admin):
        raise BootstrapError("invalid admin username")
    if not EMAIL_RE.fullmatch(email):
        raise BootstrapError("invalid admin email")
    if not isinstance(password, str) or not 8 <= len(password) <= 256:
        raise BootstrapError("invalid admin password")
    if owner_type not in ("org", "user"):
        raise BootstrapError("invalid owner type")
    if owner_type == "user":
        owner = admin
    if not NAME_RE.fullmatch(owner):
        raise BootstrapError("invalid owner name")
    if owner_type == "org" and owner == admin:
        raise BootstrapError("the organization name equals the admin name")
    if NODE in (admin, owner):
        raise BootstrapError("the name forkmesh is kept for the instance's "
                             "git node")
    node_key = str(spec.get("nodePublicKey") or "").strip()
    if node_key and not PUBKEY_RE.fullmatch(node_key):
        raise BootstrapError("invalid node public key")
    owners = read_owners(spec.get("owners"), admin, email, owner)
    if owners and owner_type != "org":
        raise BootstrapError("other owners need an organization")
    theme = spec.get("theme")
    if theme not in (None, "") and not isinstance(theme, (str, dict)):
        raise BootstrapError("invalid theme")
    return {
        "admin": admin,
        "email": email,
        "password": password,
        "ownerType": owner_type,
        "owner": owner,
        "owners": owners,
        "displayName": " ".join(str(spec.get("projectName") or "").split())[:80],
        "nodePublicKey": node_key,
        "theme": theme or "",
    }


async def ensure_user(entry, env, name, email, password, is_admin):
    who = "admin" if is_admin else "owner %s" % name
    name_bi, rec = await entry._account_row(env, name)
    _, org = await entry._org_row(env, name)
    if org:
        raise BootstrapError("the %s username is taken by an organization"
                             % who)
    email_bi = await entry.blind_index(env, email)
    holder = await entry.d1_first(
        env, "SELECT user_bi AS name_bi FROM users WHERE email_bi=?", email_bi)
    if holder and holder.get("name_bi") != name_bi:
        raise BootstrapError("the %s email belongs to another account" % who)
    salt, phash = await entry.hash_password(password)
    now = int(entry.Date.now())
    created = not rec
    rec = rec or {}
    rec.update({
        "name": name,
        "email": email,
        "kind": "user",
        "pass_salt": salt,
        "pass_hash": phash,
        "status": "active",
        "email_verified": True,
        "email_verification_method": "instance_setup",
        "email_verified_by": "instance-setup",
    })
    rec.setdefault("email_verified_at", now)
    rec.setdefault("created_at", now)
    await entry._save_account_full(env, name_bi, rec, email_bi=email_bi,
                                   is_admin=1 if is_admin else 0)
    if is_admin:
        await entry.d1_run(
            env, "UPDATE users SET is_admin=1 WHERE user_bi=?", name_bi)
    return name_bi, created


async def ensure_admin(entry, env, spec):
    return await ensure_user(entry, env, spec["admin"], spec["email"],
                             spec["password"], True)


async def ensure_org(entry, env, spec, admin_bi, owners=()):
    owner = spec["owner"]
    _, account = await entry._account_row(env, owner)
    if account:
        raise BootstrapError("the organization name is taken by an account")
    org_bi, org = await entry._org_row(env, owner)
    now = int(entry.Date.now())
    created = not org
    if created:
        enc = await entry.encrypt_row(env, {
            "name": owner,
            "displayName": spec["displayName"],
            "description": "",
            "logoUrl": "",
            "worldAccess": entry._org_world_access(None),
            "creator": spec["admin"],
            "createdAt": now,
        })
        await entry.d1_run(
            env, "INSERT INTO orgs (org_bi, name, data, created_at) "
            "VALUES (?,?,?,?)", org_bi, owner, enc, now)
    for member_bi, name in [(admin_bi, spec["admin"])] + list(owners):
        await entry.d1_run(
            env, "INSERT OR REPLACE INTO org_members "
            "(org_bi, member_bi, role, name, created_at) VALUES (?,?,?,?,?)",
            org_bi, member_bi, "owner", name, now)
    return created


async def ensure_node(entry, env, public_key):
    name_bi, rec = await entry._account_row(env, NODE)
    if rec and entry._account_kind(rec) != "node":
        raise BootstrapError("the name forkmesh is taken by a user account")
    rec = rec or {}
    rec.update({"name": NODE, "kind": "node", "status": "active",
                "pubkey": public_key})
    rec.setdefault("created_at", int(entry.Date.now()))
    await entry._save_account(env, name_bi, rec)
    await entry.touch_registered_node(env, name_bi, rec)


async def ensure_alias(entry, env, owner):
    org_bi = await entry.blind_index(env, "org:" + owner)
    await entry.d1_run(
        env, "INSERT OR REPLACE INTO org_repos (org_bi, repo, node_owner, "
        "created_at) VALUES (?,?,?,?)", org_bi, REPO, NODE,
        int(entry.Date.now()))


async def ensure_mirror_features(entry, env):
    repo_bi = await entry.blind_index(
        env, "repo-feature-settings:%s/%s" % (NODE, REPO))
    await entry.d1_run(
        env, "INSERT OR IGNORE INTO repo_feature_settings (repo_bi, data, "
        "updated_at) VALUES (?,?,?)", repo_bi,
        json.dumps(MIRROR_FEATURES, sort_keys=True), int(entry.Date.now()))


async def ensure_theme(entry, env, value):
    try:
        doc = entry.instance_theme.dump(entry.instance_theme.clean(value))
    except entry.instance_theme.ThemeError as error:
        raise BootstrapError(str(error)) from error
    await entry.d1_run(
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) "
        "VALUES ('theme', ?, ?) ON CONFLICT(k) DO UPDATE SET "
        "doc=excluded.doc, updated_at=excluded.updated_at",
        doc, int(entry.Date.now()))


async def bootstrap(runtime, spec):
    entry, env = runtime.entry, runtime.env
    await entry.ensure_schema(env)
    admin_bi, admin_created = await ensure_admin(entry, env, spec)
    org_created = False
    owners = []
    for item in spec["owners"]:
        owner_bi, _ = await ensure_user(entry, env, item["name"],
                                        item["email"], item["password"],
                                        False)
        owners.append((owner_bi, item["name"]))
    if spec["ownerType"] == "org":
        org_created = await ensure_org(entry, env, spec, admin_bi, owners)
    if spec["nodePublicKey"]:
        await ensure_node(entry, env, spec["nodePublicKey"])
        await ensure_alias(entry, env, spec["owner"])
        await ensure_mirror_features(entry, env)
    if spec["theme"]:
        await ensure_theme(entry, env, spec["theme"])
    return {
        "ok": True,
        "admin": spec["admin"],
        "adminCreated": admin_created,
        "owner": spec["owner"],
        "ownerType": spec["ownerType"],
        "orgCreated": org_created,
        "owners": [name for _, name in owners],
        "node": NODE if spec["nodePublicKey"] else "",
        "repository": "%s/%s" % (spec["owner"], REPO),
    }


def cmd_instance_bootstrap(args):
    from .ops import _runtime
    try:
        spec = read_spec(sys.stdin.read())
    except BootstrapError as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2
    runtime = _runtime(args)
    try:
        result = asyncio.run(bootstrap(runtime, spec))
    except BootstrapError as error:
        result = {"ok": False, "error": str(error)}
    finally:
        for item in spec.get("owners") or []:
            item.clear()
        spec.clear()
    print(json.dumps(result, sort_keys=True))
    return 0 if result.get("ok") else 1


def add_parser(sub, common):
    command = sub.add_parser(
        "instance-bootstrap",
        help="create the first admin and owner of a new instance (JSON on stdin)")
    common(command)
    command.set_defaults(func=cmd_instance_bootstrap)
