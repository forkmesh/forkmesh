import asyncio
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import instance_bootstrap as boot  # noqa: E402

NODE_KEY = "A" * 43


def spec(**changes):
    raw = {"adminUsername": "Ada", "adminEmail": "Ada@Example.com",
           "adminPassword": "correct-horse-9", "ownerType": "org",
           "ownerName": "acme", "projectName": "  Acme   Code ",
           "nodePublicKey": NODE_KEY}
    raw.update(changes)
    return json.dumps(raw)


class Entry:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            CREATE TABLE users (user_bi TEXT PRIMARY KEY, email_bi TEXT,
                is_admin INTEGER NOT NULL DEFAULT 0, data TEXT);
            CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT, data TEXT,
                created_at INTEGER);
            CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT,
                name TEXT, created_at INTEGER, PRIMARY KEY (org_bi, member_bi));
            CREATE TABLE org_repos (org_bi TEXT NOT NULL, repo TEXT NOT NULL,
                node_owner TEXT NOT NULL, created_at INTEGER NOT NULL,
                PRIMARY KEY (org_bi, repo));
            CREATE TABLE repo_feature_settings (repo_bi TEXT PRIMARY KEY,
                data TEXT NOT NULL, updated_at INTEGER NOT NULL);
            CREATE TABLE instance_settings (k TEXT PRIMARY KEY,
                doc TEXT NOT NULL, updated_at INTEGER NOT NULL);
        """)
        self.Date = SimpleNamespace(now=lambda: 1_700_000_000_000)
        self.touched = []

    async def ensure_schema(self, _env):
        pass

    async def blind_index(self, _env, value):
        return "bi:" + value

    async def hash_password(self, password):
        return "salt", "hash:" + password

    async def encrypt_row(self, _env, value):
        return json.dumps(value)

    def _org_world_access(self, _value):
        return "members"

    def _account_kind(self, rec):
        return rec.get("kind", "user")

    async def _account_row(self, env, name):
        bi = await self.blind_index(env, name)
        row = self.db.execute("SELECT data FROM users WHERE user_bi=?", (bi,)).fetchone()
        return bi, (json.loads(row["data"]) if row else None)

    async def _org_row(self, env, name):
        bi = await self.blind_index(env, "org:" + name)
        row = self.db.execute("SELECT * FROM orgs WHERE org_bi=?", (bi,)).fetchone()
        return bi, (dict(row) if row else None)

    async def _save_account(self, env, name_bi, rec, email_bi=None):
        await self._save_account_full(env, name_bi, rec, email_bi=email_bi)

    async def _save_account_full(self, _env, name_bi, rec, email_bi=None,
                                 is_admin=0):
        self.db.execute(
            "INSERT INTO users (user_bi, email_bi, is_admin, data) VALUES (?,?,?,?) "
            "ON CONFLICT(user_bi) DO UPDATE SET data=excluded.data, "
            "email_bi=COALESCE(excluded.email_bi, users.email_bi)",
            (name_bi, email_bi, is_admin, json.dumps(rec)))

    async def touch_registered_node(self, _env, name_bi, rec):
        self.touched.append((name_bi, rec["pubkey"]))

    async def d1_first(self, _env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, _env, sql, *args):
        return self.db.execute(sql, args)

    async def d1_all(self, _env, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]


def run(entry, raw):
    runtime = SimpleNamespace(entry=entry, env=object())
    return asyncio.run(boot.bootstrap(runtime, boot.read_spec(raw)))


def user(entry, name):
    row = entry.db.execute("SELECT * FROM users WHERE user_bi=?",
                           ("bi:" + name,)).fetchone()
    return dict(row, data=json.loads(row["data"])) if row else None


def test_spec_is_normalized_and_refuses_bad_values():
    parsed = boot.read_spec(spec())
    assert parsed["admin"] == "ada" and parsed["email"] == "ada@example.com"
    assert parsed["displayName"] == "Acme Code"
    assert boot.read_spec(spec(ownerType="user", ownerName="x"))["owner"] == "ada"
    for bad in ({"adminUsername": "Bad Name"}, {"adminEmail": "x"},
                {"adminPassword": "short"}, {"ownerType": "team"},
                {"ownerName": "ada"}, {"ownerName": "forkmesh"},
                {"adminUsername": "forkmesh", "ownerType": "user"},
                {"nodePublicKey": "short"}):
        with pytest.raises(boot.BootstrapError):
            boot.read_spec(spec(**bad))
    with pytest.raises(boot.BootstrapError):
        boot.read_spec("not json")


def test_admin_org_node_and_alias_are_created_once():
    entry = Entry()
    result = run(entry, spec())
    assert result == {"ok": True, "admin": "ada", "adminCreated": True,
                      "owner": "acme", "ownerType": "org", "orgCreated": True,
                      "owners": [], "node": "forkmesh",
                      "repository": "acme/forkmesh",
                      "singleOrganization": False}
    admin = user(entry, "ada")
    assert admin["is_admin"] == 1 and admin["email_bi"] == "bi:ada@example.com"
    assert admin["data"]["email_verified"] is True
    assert admin["data"]["status"] == "active" and admin["data"]["kind"] == "user"
    assert admin["data"]["pass_hash"] == "hash:correct-horse-9"
    node = user(entry, "forkmesh")
    assert node["data"]["kind"] == "node" and node["data"]["pubkey"] == NODE_KEY
    assert entry.touched == [("bi:forkmesh", NODE_KEY)]
    member = entry.db.execute("SELECT * FROM org_members").fetchone()
    assert (member["org_bi"], member["member_bi"], member["role"]) == (
        "bi:org:acme", "bi:ada", "owner")
    alias = entry.db.execute("SELECT * FROM org_repos").fetchone()
    assert (alias["org_bi"], alias["repo"], alias["node_owner"]) == (
        "bi:org:acme", "forkmesh", "forkmesh")
    features = entry.db.execute("SELECT * FROM repo_feature_settings").fetchone()
    assert features["repo_bi"] == "bi:repo-feature-settings:forkmesh/forkmesh"
    assert json.loads(features["data"]) == {
        "discussions": False, "issues": False, "pulls": False}
    entry.db.execute("UPDATE repo_feature_settings SET data=?",
                     ('{"issues": true}',))
    again = run(entry, spec(adminPassword="another-pass-1"))
    assert again["adminCreated"] is False and again["orgCreated"] is False
    assert user(entry, "ada")["data"]["pass_hash"] == "hash:another-pass-1"
    assert entry.db.execute("SELECT COUNT(*) FROM orgs").fetchone()[0] == 1
    assert entry.db.execute(
        "SELECT data FROM repo_feature_settings").fetchone()[0] == '{"issues": true}'


def test_user_owner_gets_the_alias_without_an_org():
    entry = Entry()
    result = run(entry, spec(ownerType="user"))
    assert result["owner"] == "ada" and result["orgCreated"] is False
    assert entry.db.execute("SELECT COUNT(*) FROM orgs").fetchone()[0] == 0
    alias = entry.db.execute("SELECT org_bi FROM org_repos").fetchone()
    assert alias["org_bi"] == "bi:org:ada"


def test_conflicts_are_refused():
    entry = Entry()
    run(entry, spec())
    with pytest.raises(boot.BootstrapError):
        run(entry, spec(adminUsername="bob"))
    entry = Entry()
    asyncio.run(entry._save_account_full(None, "bi:acme", {"kind": "user"}))
    with pytest.raises(boot.BootstrapError):
        run(entry, spec())


def owner(**changes):
    item = {"username": "Grace", "email": "Grace@Example.com",
            "password": "grace-password-3"}
    item.update(changes)
    return item


def test_other_owners_are_normalized_and_refused_when_wrong():
    parsed = boot.read_spec(spec(owners=[owner()]))
    assert parsed["owners"] == [{"name": "grace", "email": "grace@example.com",
                                 "password": "grace-password-3"}]
    assert boot.read_spec(spec())["owners"] == []
    for bad in ({"owners": "grace"}, {"owners": ["grace"]},
                {"owners": [owner()] * 21},
                {"owners": [owner()], "ownerType": "user"},
                {"owners": [owner(username="ada")]},
                {"owners": [owner(username="acme")]},
                {"owners": [owner(username="forkmesh")]},
                {"owners": [owner(), owner(email="g2@example.com")]},
                {"owners": [owner(email="ada@example.com")]},
                {"owners": [owner(email="nope")]},
                {"owners": [owner(password="short")]}):
        with pytest.raises(boot.BootstrapError):
            boot.read_spec(spec(**bad))


def test_other_owners_get_accounts_and_own_the_org():
    entry = Entry()
    result = run(entry, spec(owners=[owner(), owner(
        username="linus", email="linus@example.com",
        password="linus-password-4")]))
    assert result["owners"] == ["grace", "linus"]
    grace = user(entry, "grace")
    assert grace["is_admin"] == 0 and grace["email_bi"] == "bi:grace@example.com"
    assert grace["data"]["email_verified"] is True
    assert grace["data"]["status"] == "active"
    assert grace["data"]["pass_hash"] == "hash:grace-password-3"
    assert user(entry, "linus")["data"]["pass_hash"] == "hash:linus-password-4"
    members = entry.db.execute(
        "SELECT member_bi, role, name FROM org_members ORDER BY name").fetchall()
    assert [tuple(row) for row in members] == [
        ("bi:ada", "owner", "ada"), ("bi:grace", "owner", "grace"),
        ("bi:linus", "owner", "linus")]
    again = run(entry, spec(owners=[owner(password="grace-password-5")]))
    assert again["owners"] == ["grace"]
    assert user(entry, "grace")["data"]["pass_hash"] == "hash:grace-password-5"
    assert entry.db.execute("SELECT COUNT(*) FROM org_members").fetchone()[0] == 3


def test_an_owner_email_held_by_another_account_is_refused():
    entry = Entry()
    run(entry, spec(owners=[owner()]))
    with pytest.raises(boot.BootstrapError, match="owner linus"):
        run(entry, spec(owners=[owner(username="linus")]))


SEED = "11" * 32


def test_the_actions_bot_is_derived_from_the_seed_and_joins_the_org():
    entry = Entry()
    runtime = SimpleNamespace(entry=entry, env=object())
    result = asyncio.run(boot.bootstrap(runtime, boot.read_spec(spec()), SEED))
    assert result["actionsBot"] == "acme-actions"
    bot = user(entry, "acme-actions")
    assert bot["data"]["kind"] == "node" and bot["data"]["actionsBot"] is True
    assert bot["data"]["pubkey"] == boot.bot_public_key(SEED)
    assert len(bot["data"]["pubkey"]) == 43
    role = entry.db.execute(
        "SELECT role FROM org_members WHERE member_bi='bi:acme-actions'"
    ).fetchone()[0]
    assert role == "maintainer"
    assert "actionsBot" not in run(Entry(), spec())
    with pytest.raises(boot.BootstrapError):
        asyncio.run(boot.bootstrap(runtime, boot.read_spec(spec()), "xyz"))


def test_instance_runner_registers_a_trusted_pool_runner():
    entry = Entry()
    entry.db.executescript(
        (Path(__file__).resolve().parents[2] / "migrations"
         / "0193_runner_nodes.sql").read_text()
        .split("-- The runtime schema text changed")[0])
    entry.db.execute(
        "ALTER TABLE runner_nodes ADD COLUMN actions_trusted INTEGER "
        "NOT NULL DEFAULT 0")
    runtime = SimpleNamespace(entry=entry, env=object())
    for key in ("B" * 43, "C" * 43):
        result = asyncio.run(boot.register_runner(
            runtime, "box-runner", key, "acme"))
    assert result["actionsTrusted"] is True
    row = dict(entry.db.execute("SELECT * FROM runner_nodes").fetchone())
    assert (row["public_key"], row["scope"], row["allowed_owners"],
            row["status"], row["actions_trusted"]) == (
        "C" * 43, "pool", '["acme"]', "active", 1)


def repo_paths(entry):
    row = entry.db.execute(
        "SELECT doc FROM instance_settings WHERE k='repo_paths'").fetchone()
    return json.loads(row["doc"]) if row else None


def test_addresses_stay_as_they_are_unless_the_install_opts_in():
    entry = Entry()
    assert boot.read_spec(spec())["singleOrganization"] is False
    assert run(entry, spec())["singleOrganization"] is False
    assert repo_paths(entry) is None
    with pytest.raises(boot.BootstrapError):
        boot.read_spec(spec(singleOrganization="yes"))
    with pytest.raises(boot.BootstrapError):
        boot.read_spec(spec(singleOrganization=True, ownerType="user"))


def test_a_single_organization_install_starts_with_canonical_addresses():
    entry = Entry()
    assert run(entry, spec(singleOrganization=True))["singleOrganization"] is True
    assert repo_paths(entry) == {"canonical": True, "owner": "acme",
                                 "short": True}
    entry.db.execute("UPDATE instance_settings SET doc=? WHERE k='repo_paths'",
                     ('{"short":false,"owner":"acme"}',))
    again = run(entry, spec(singleOrganization=True))
    assert again["singleOrganization"] is False
    assert repo_paths(entry) == {"short": False, "owner": "acme"}


def test_an_existing_address_setting_is_kept_at_first_install():
    entry = Entry()
    entry.db.execute(
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES "
        "('repo_paths', '{\"short\":false,\"owner\":\"\"}', 1)")
    run(entry, spec(singleOrganization=True))
    assert repo_paths(entry) == {"short": False, "owner": ""}


def convert(entry, org="acme", enable=True):
    runtime = SimpleNamespace(entry=entry, env=object())
    return asyncio.run(boot.single_org(runtime, org, enable))


def test_an_installed_instance_is_converted_once_and_can_switch_back():
    entry = Entry()
    run(entry, spec())
    entry.db.execute("DELETE FROM org_repos")
    entry.db.execute(
        "INSERT INTO org_repos VALUES ('bi:org:acme', 'bolt', 'drew', 1)")
    first = convert(entry)
    assert first["changed"] is True and first["linkedForkmesh"] is True
    assert first["repositories"] == ["/bolt", "/forkmesh"]
    assert repo_paths(entry) == {"canonical": True, "owner": "acme",
                                 "short": True}
    again = convert(entry)
    assert again["changed"] is False and again["linkedForkmesh"] is False
    assert entry.db.execute("SELECT COUNT(*) FROM org_repos").fetchone()[0] == 2
    off = convert(entry, enable=False)
    assert off["changed"] is True and off["singleOrganization"] is False
    assert repo_paths(entry) == {"canonical": False, "owner": "acme",
                                 "short": True}
    with pytest.raises(boot.BootstrapError):
        convert(entry, org="nobody")


def test_the_conversion_command_is_registered():
    import argparse
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers()
    boot.add_parser(sub, lambda command: None)
    args = parser.parse_args(["instance-single-org", "--org", "acme-bolt"])
    assert args.func is boot.cmd_instance_single_org and args.off is False
    args = parser.parse_args(["instance-single-org", "--org", "x", "--off"])
    assert args.off is True
