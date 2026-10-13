import asyncio
import base64
import hashlib
import json
import os
import sqlite3
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "src") not in sys.path:
    sys.path.insert(0, str(ROOT / "src"))

import apps  # noqa: E402
import apps_core as core  # noqa: E402

NOW = 1_800_000_000_000


def make_db():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript((ROOT / "migrations" / "0203_apps.sql").read_text("utf-8"))
    db.execute("CREATE TABLE marketplace_package_rating_stats (market TEXT, "
               "package_id TEXT, rating_count INTEGER, rating_total INTEGER)")
    return db


class World:
    def __init__(self):
        self.db = make_db()
        self.now = NOW
        self.admins = {"root"}
        self.orgs = {}
        self.notices = []
        self.audits = []
        self.http = []
        self.http_handler = None
        self.chat = []
        self.channels = {}
        self.issues = []
        self.pulls = []
        self.repos = {}
        self.config = {}

    def org(self, name, **members):
        self.orgs[name] = dict(members)


class Runtime:
    def __init__(self, world, method="GET", body=None, user="alice",
                 query=None, headers=None, raw_form=None):
        self.world = world
        self._method = method
        self._body = body
        self._user = user
        self._query = query or {}
        self._headers = {k.lower(): v for k, v in (headers or {}).items()}
        self._raw_form = raw_form

    def method(self):
        return self._method

    def now(self):
        return self.world.now

    def new_hex(self, nbytes):
        return os.urandom(int(nbytes)).hex()

    def query_params(self):
        return dict(self._query)

    def header(self, name):
        return str(self._headers.get(name.lower(), ""))

    def public_origin(self):
        return "https://app.forkmesh.com"

    def config(self, name):
        return self.world.config.get(name, "")

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {"data": data, "status": status, "cache": cache_control,
                "headers": extra_headers or {}}

    async def ensure_schema(self):
        return None

    async def json_body(self, limit):
        if self._body is None:
            return {}, ""
        if not isinstance(self._body, dict):
            return None, "invalid_json"
        return dict(self._body), ""

    async def form_body(self, limit):
        if self._raw_form is not None:
            return {k: v[0] for k, v in parse_qs(self._raw_form).items()}, ""
        return await self.json_body(limit)

    async def session(self, data=None):
        if not self._user:
            return "", None
        return "acct:" + self._user, {"name": self._user}

    async def is_admin(self, name):
        return name in self.world.admins

    async def blind(self, value):
        return "bi:" + hashlib.sha256(
            ("key|" + str(value).strip().lower()).encode()).hexdigest()

    async def seal(self, value):
        return "sealed:" + base64.b64encode(json.dumps(value).encode()).decode()

    async def open(self, value):
        text = str(value or "")
        if not text.startswith("sealed:"):
            return None
        return json.loads(base64.b64decode(text[7:]))

    async def org_lookup(self, name):
        return ("org:" + name) if name in self.world.orgs else ""

    async def org_role(self, org_bi, account):
        name = str(org_bi or "")[4:]
        return self.world.orgs.get(name, {}).get(account, "")

    async def orgs_for(self, account):
        return sorted((org, members[account])
                      for org, members in self.world.orgs.items()
                      if account in members)

    async def org_admins(self, org_bi):
        name = str(org_bi or "")[4:]
        return sorted(member for member, role in
                      self.world.orgs.get(name, {}).items()
                      if role in ("owner", "admin"))

    async def notify(self, recipient, title, body="", href="", dedupe=""):
        self.world.notices.append((recipient, title, href))

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        self.world.audits.append((actor, action, target, outcome))

    async def http_post(self, url, headers, body, timeout):
        self.world.http.append({"url": url, "headers": dict(headers),
                                "body": body})
        if self.world.http_handler is None:
            return 200, ""
        return self.world.http_handler(url, headers, body)

    async def account_owns(self, actor, owner):
        return actor == owner or owner in self.world.repos.get(
            "nodes:" + actor, ())

    async def repo_accounts(self, owner):
        accounts = [owner] if owner not in self.world.orgs else []
        for key, nodes in self.world.repos.items():
            if isinstance(key, str) and key.startswith("nodes:") and \
                    owner in nodes:
                accounts.append(key[6:])
        return accounts

    async def chat_channel(self, channel_id):
        return self.world.channels.get(channel_id)

    async def chat_post(self, channel_id, key_version, plain):
        self.world.chat.append((channel_id, plain))

    async def issue_open(self, owner, repo, title, body, author,
                         author_name, slug):
        if (owner, repo) not in self.world.repos:
            return False, "repository_not_found"
        number = len(self.world.issues) + 1
        self.world.issues.append({"owner": owner, "repo": repo,
                                  "number": number, "title": title,
                                  "body": body, "author": author,
                                  "authorName": author_name, "type": "open"})
        return True, number

    async def issue_comment(self, owner, repo, number, body, author,
                            author_name, wants_agent=False):
        if (owner, repo) not in self.world.repos:
            return False, "repository_not_found"
        self.world.issues.append({"owner": owner, "repo": repo,
                                  "number": number, "body": body,
                                  "author": author, "authorName": author_name,
                                  "type": "comment", "wantsAgent": wants_agent})
        return True, number

    async def pull_comment(self, owner, repo, number, body, author,
                           author_name):
        if (owner, repo) not in self.world.repos:
            return False, "repository_not_found"
        self.world.pulls.append({"owner": owner, "repo": repo,
                                 "number": number, "body": body,
                                 "author": author, "authorName": author_name})
        return True, number

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.world.db.execute(sql, args)]

    async def d1_first(self, sql, *args):
        cursor = self.world.db.execute(sql, args)
        row = cursor.fetchone()
        cursor.fetchall()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.world.db.execute(sql, args)

    async def batch(self, statements):
        with self.world.db:
            for sql, args in statements:
                self.world.db.execute(sql, args)


def call(world, method, path, body=None, user="alice", query=None,
         headers=None, raw_form=None, runtime_class=Runtime):
    runtime = runtime_class(world, method, body, user, query, headers,
                            raw_form)
    return asyncio.run(apps.handle(runtime, path))


def run(coro):
    return asyncio.run(coro)


APP = {
    "name": "Hello App",
    "description": "Says hello.",
    "icon": {"glyph": "H", "color": "#22aa66"},
    "redirectUris": ["https://hello.example.org/oauth/callback"],
    "scopes": ["repos:read", "issues:read", "issues:write", "chat:write",
               "pings:write", "commands"],
}

VERIFIER = "v" * 20 + "erifier-0123456789_abcdefghijklmnop"


def create_app(world, user="alice", **overrides):
    body = dict(APP)
    body.update(overrides)
    result = call(world, "POST", "/api/apps", body, user=user)
    assert result["status"] == 201, result["data"]
    return result["data"]


def authorize(world, created, user="alice", target="user", scope=None,
              verifier=VERIFIER, state="xyz", decision="allow",
              redirect_uri=None):
    body = {
        "response_type": "code",
        "client_id": created["app"]["clientId"],
        "redirect_uri": redirect_uri or created["app"]["redirectUris"][0],
        "scope": scope or " ".join(created["app"]["scopes"]),
        "state": state,
        "code_challenge": core.pkce_s256(verifier),
        "code_challenge_method": "S256",
        "target": target,
        "decision": decision,
    }
    return call(world, "POST", "/api/apps/oauth/authorize", body, user=user)


def code_from(result):
    redirect = result["data"]["redirect"]
    return parse_qs(urlparse(redirect).query)["code"][0]


def exchange(world, created, code, verifier=VERIFIER, redirect_uri=None,
             secret=None):
    return call(world, "POST", "/api/apps/oauth/token", {
        "grant_type": "authorization_code",
        "code": code,
        "redirect_uri": redirect_uri or created["app"]["redirectUris"][0],
        "client_id": created["app"]["clientId"],
        "client_secret": secret or created["clientSecret"],
        "code_verifier": verifier,
    }, user="")


def install(world, created, user="alice", **kw):
    result = authorize(world, created, user=user, **kw)
    assert result["status"] == 200, result["data"]
    tokens = exchange(world, created, code_from(result))
    assert tokens["status"] == 200, tokens["data"]
    return tokens["data"]
