"""An in-memory host runtime for the vault Worker module.

Shared by the Python API tests and by the stdio bridge the JavaScript
end-to-end test drives, so both exercise the same module the deploy stages.
"""

import hashlib
import json
from pathlib import Path
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import vault  # noqa: E402


ACCOUNTS = ("alice", "bob", "carol", "dave", "erin")
ORGANIZATIONS = {"acme": ("alice", "bob", "carol"), "globex": ("dave",)}


def bi(name):
    return "bi:" + name


class Runtime:
    def __init__(self):
        vault._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT NOT NULL,"
            "data TEXT NOT NULL,created_at INTEGER NOT NULL);"
            "CREATE TABLE org_members(org_bi TEXT NOT NULL,member_bi TEXT NOT NULL,"
            "role TEXT NOT NULL DEFAULT 'member',name TEXT NOT NULL,"
            "created_at INTEGER NOT NULL,PRIMARY KEY(org_bi,member_bi));")
        for org, members in ORGANIZATIONS.items():
            self.db.execute("INSERT INTO orgs VALUES(?,?,?,0)", ("org:" + org, org, "{}"))
            for index, member in enumerate(members):
                self.join(org, member, "owner" if index == 0 else "member")
        self.accounts = set(ACCOUNTS)
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.origin_ok = True
        self.clock = 1_789_561_860_000
        self.audits = []
        self.pings = []

    def join(self, org, member, role="member"):
        self.db.execute(
            "INSERT OR REPLACE INTO org_members VALUES(?,?,?,?,0)",
            ("org:" + org, bi(member), role, member))
        self.db.commit()

    def leave(self, org, member):
        self.db.execute(
            "DELETE FROM org_members WHERE org_bi=? AND member_bi=?",
            ("org:" + org, bi(member)))
        self.db.commit()

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {},
                "cache": cache_control}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        if self.actor not in self.accounts:
            return "", None
        return bi(self.actor), {"name": self.actor}
    async def account(self, name):
        return (bi(name), name) if name in self.accounts else ("", "")
    async def organization(self, name, account_bi):
        row = self.db.execute(
            "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
            ("org:" + name, account_bi)).fetchone()
        if not self.db.execute("SELECT 1 FROM orgs WHERE name=?", (name,)).fetchone():
            return "", ""
        return "org:" + name, (row["role"] if row else "")
    # "sealed:" stands in for the host row key: a test can tell a sealed
    # column from a plain one, and nothing opens without going through open().
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    async def blind(self, value):
        return "blind:" + hashlib.sha256(str(value).encode()).hexdigest()[:48]
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)
    async def notify(self, recipient, kind, title, **kwargs):
        self.pings.append(dict(kwargs, recipient=recipient, kind=kind, title=title))
        return True
