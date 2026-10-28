"""End-to-end contracts for non-custodial organization succession."""

from __future__ import annotations

import asyncio
import importlib.util
import sqlite3
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MIGRATIONS = ROOT / "migrations"
ENTRY = SRC / "entry.py"
URLS = SRC / "urls.py"
SCHEMA = SRC / "schema.py"
DOC = ROOT.parent / "www" / "docs" / "operations" / "organization-succession.md"

sys.path.insert(0, str(SRC))
import organization_succession as policy  # noqa: E402
import organization_succession_api as api  # noqa: E402


def _module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


urls = _module("urls")


def _run(coro):
    return asyncio.run(coro)


class Runtime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (MIGRATIONS / "0038_orgs_teams.sql").read_text(encoding="utf-8")
        )
        self.db.executescript(
            (MIGRATIONS / "0064_organization_succession.sql").read_text(
                encoding="utf-8"
            )
        )
        self.db.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        for name in ("0183_organization_rbac_roles.sql",
                     "0295_organization_succession_approver_snapshot.sql"):
            self.db.executescript(
                (MIGRATIONS / name).read_text(encoding="utf-8"))
        # Sentinels from stores succession must never mutate.
        self.db.executescript(
            """
            CREATE TABLE repositories (
                key_bi TEXT PRIMARY KEY, owner_bi TEXT, data TEXT);
            CREATE TABLE repo_shares (
                repo_bi TEXT, grantee_bi TEXT, data TEXT, ts INTEGER);
            CREATE TABLE users (
                user_bi TEXT PRIMARY KEY, data TEXT);
            CREATE TABLE account_devices (
                device_bi TEXT PRIMARY KEY, account_bi TEXT, secret TEXT);
            INSERT INTO repositories VALUES
                ('repo:private', 'bi:owner', 'encrypted-private-repo');
            INSERT INTO repo_shares VALUES
                ('repo:private', 'bi:reader', 'encrypted-private-acl', 1);
            INSERT INTO users VALUES
                ('bi:owner', 'encrypted-owner-account'),
                ('bi:successor', 'encrypted-successor-account');
            INSERT INTO account_devices VALUES
                ('device:owner', 'bi:owner', 'owner-device-secret');
            """
        )
        self.now_ms = 1_700_000_000_000
        self.actor = "owner"
        self.method_value = "GET"
        self.body_value = {}
        self.ids = 0
        self.notifications = []
        self.accounts = {
            name: {"name": name, "status": "active", "kind": "user"}
            for name in (
                "owner", "successor", "alice", "bob", "charlie",
                "platform-admin",
            )
        }
        self.activity = {"bi:owner": self.now_ms}
        self.db.execute(
            "INSERT INTO orgs VALUES (?,?,?,?)",
            ("bi:org:acme", "acme", "{}", self.now_ms),
        )
        for index, (name, role) in enumerate((
            ("owner", "owner"),
            ("successor", "admin"),
            ("alice", "member"),
            ("bob", "member"),
            ("charlie", "member"),
        )):
            self.db.execute(
                "INSERT INTO org_members VALUES (?,?,?,?,?)",
                ("bi:org:acme", "bi:" + name, role, name, self.now_ms + index),
            )
        self.db.execute(
            "INSERT INTO org_teams VALUES (?,?,?,?)",
            ("bi:org:acme", "core", "maintain", self.now_ms),
        )
        self.db.execute(
            "INSERT INTO org_team_members VALUES (?,?,?,?,?)",
            ("bi:org:acme", "core", "bi:alice", "alice", self.now_ms),
        )
        self.db.execute(
            "INSERT INTO org_repos VALUES (?,?,?,?)",
            ("bi:org:acme", "private-project", "source-node", self.now_ms),
        )
        self.db.commit()

    def method(self):
        return self.method_value

    def now(self):
        return self.now_ms

    def new_id(self):
        self.ids += 1
        return f"{self.ids:032x}"

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {
            "status": status,
            "data": data,
            "cache": cache_control,
            "headers": dict(extra_headers or {}),
        }

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        return dict(self.body_value), ""

    async def session(self, _data=None):
        record = self.accounts.get(self.actor)
        return (
            ("bi:" + self.actor, dict(record))
            if record else ("", None)
        )

    async def organization(self, org):
        row = self.db.execute(
            "SELECT org_bi,name FROM orgs WHERE name=?", (org,)
        ).fetchone()
        return (
            (str(row["org_bi"]), dict(row))
            if row else ("bi:org:" + org, None)
        )

    async def org_role(self, org_bi, account):
        row = self.db.execute(
            "SELECT role FROM org_members WHERE org_bi=? AND name=?",
            (org_bi, account),
        ).fetchone()
        return str(row["role"]) if row else ""

    async def org_members(self, org_bi):
        return [
            dict(row) for row in self.db.execute(
                "SELECT member_bi,name,role,created_at FROM org_members "
                "WHERE org_bi=? ORDER BY created_at",
                (org_bi,),
            ).fetchall()
        ]

    async def account(self, name):
        record = self.accounts.get(name)
        if not record or record["status"] != "active" or record["kind"] != "user":
            return "", ""
        return "bi:" + name, name

    async def owner_activity(self, owner_bi, stored_at):
        return max(int(stored_at or 0), int(self.activity.get(owner_bi, 0)))

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()

    async def batch(self, statements):
        with self.db:
            for sql, args in statements:
                self.db.execute(sql, args)

    async def notify(self, recipient, title, body, dedupe):
        self.notifications.append({
            "recipient": recipient,
            "title": title,
            "body": body,
            "dedupe": dedupe,
        })
        return True

    def request(self, actor, method, action="", body=None):
        self.actor = actor
        self.method_value = method
        self.body_value = dict(body or {})
        return _run(api.handle(self, "acme", action))

    def roles(self):
        return {
            row["name"]: row["role"]
            for row in self.db.execute(
                "SELECT name,role FROM org_members WHERE org_bi=?",
                ("bi:org:acme",),
            )
        }

    def protected_state(self):
        return {
            "repositories": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM repositories ORDER BY key_bi")
            ],
            "shares": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM repo_shares ORDER BY repo_bi")
            ],
            "users": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM users ORDER BY user_bi")
            ],
            "devices": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM account_devices ORDER BY device_bi")
            ],
            "repos": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM org_repos ORDER BY repo")
            ],
            "teams": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM org_teams ORDER BY team")
            ],
            "teamMembers": [
                tuple(row) for row in self.db.execute(
                    "SELECT * FROM org_team_members ORDER BY team,member_bi")
            ],
        }


def _configure(runtime):
    return runtime.request(
        "owner",
        "PUT",
        body={
            "successor": "successor",
            "inactivityDays": 30,
            "graceDays": 7,
            "approvalThreshold": 2,
            "sessionToken": "transport-only-token",
        },
    )


def test_policy_bounds_and_non_transferable_fields_fail_closed():
    config, error = policy.normalize_config({
        "successor": "successor",
        "sessionToken": "not-persisted",
    })
    assert not error
    assert config == {
        "successor": "successor",
        "inactivityDays": 180,
        "graceDays": 30,
        "approvalThreshold": 2,
    }
    for field in policy.PROHIBITED_TRANSFER_FIELDS:
        value, error = policy.normalize_config({
            "successor": "successor",
            field: "must-not-transfer",
        })
        assert value is None and error == "non_transferable_field", field
    for field, value, error in (
        ("inactivityDays", 29, "invalid_inactivity_threshold"),
        ("inactivityDays", 731, "invalid_inactivity_threshold"),
        ("graceDays", 6, "invalid_grace_period"),
        ("graceDays", 91, "invalid_grace_period"),
        ("approvalThreshold", 1, "invalid_approval_threshold"),
        ("approvalThreshold", 21, "invalid_approval_threshold"),
    ):
        payload = {"successor": "successor", field: value}
        assert policy.normalize_config(payload) == (None, error)


def test_timeline_warns_before_inactivity_and_never_opens_grace_itself():
    last = 1_000_000_000
    warning = policy.inactivity_timeline(
        last, 30, last + 24 * policy.DAY_MS)
    assert warning["state"] == "warning"
    assert warning["inactivityAt"] == last + 30 * policy.DAY_MS
    eligible = policy.inactivity_timeline(
        last, 30, last + 30 * policy.DAY_MS)
    assert eligible["state"] == "eligible"


def test_routes_schema_migration_and_admin_boundary_are_integrated():
    match = urls.ORG_SUCCESSION_RE.match(
        "/api/orgs/acme/succession/finalize")
    assert match and match.groups() == ("acme", "finalize")
    assert urls.ORG_SUCCESSION_RE.match("/api/orgs/acme/succession")
    assert not urls.ORG_RE.match("/api/orgs/acme/succession")

    migration = (
        MIGRATIONS / "0064_organization_succession.sql"
    ).read_text(encoding="utf-8")
    schema = SCHEMA.read_text(encoding="utf-8")
    entry = ENTRY.read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    admin_console = (SRC / "admin_console.py").read_text(encoding="utf-8")
    for table in (
        "org_succession_policies",
        "org_succession_cases",
        "org_succession_approvals",
        "org_succession_events",
    ):
        assert "CREATE TABLE IF NOT EXISTS " + table in migration
        assert "CREATE TABLE IF NOT EXISTS " + table in schema
        assert f'"{table}"' in admin_console.split(
            "ADMIN_HIDDEN_TABLES", 1)[1]
    assert "ORG_SUCCESSION_RE.match(url.path)" in runtime
    due_phases = entry[
        entry.index("def _cron_due_phases"):
        entry.index("async def _run_cron_phase")
    ]
    # The phase dispatch table only, cut at whatever top-level definition
    # follows it, so renaming that neighbour cannot silently widen the slice.
    runner_start = entry.index("async def _run_cron_phase")
    phase_runner = entry[runner_start:min(
        offset for offset in (
            entry.find("\n\n\ndef ", runner_start),
            entry.find("\n\n\nasync def ", runner_start),
        ) if offset > 0
    )]
    assert 'if hourly == 42:\n        phases.append("org-succession")' in due_phases
    assert (
        'if phase == "org-succession":\n'
        '        return await process_organization_succession_warnings(env)'
    ) in phase_runner
    api_text = (SRC / "organization_succession_api.py").read_text(
        encoding="utf-8")
    assert "is_admin" not in api_text
    assert "allow_admin" not in api_text


def test_migration_triggers_do_not_embed_case_end_terminators():
    """Wrangler's D1 migration splitter treats an inner ``END;`` as the end of
    a trigger body.  Express conditional aborts as ``RAISE ... WHERE`` so the
    production migration remains one complete statement per trigger.
    """

    migration = (
        MIGRATIONS / "0064_organization_succession.sql"
    ).read_text(encoding="utf-8")
    assert "SELECT CASE" not in migration


def test_schema_has_no_sensitive_transfer_columns():
    runtime = Runtime()
    forbidden = {
        "wallet", "address", "fund", "reward", "private_key", "credential",
        "account_data", "agent", "device", "personal", "repository_owner",
        "repo_permission",
    }
    for table in (
        "org_succession_policies",
        "org_succession_cases",
        "org_succession_approvals",
        "org_succession_events",
    ):
        columns = {
            str(row["name"]).lower()
            for row in runtime.db.execute(f"PRAGMA table_info({table})")
        }
        assert not {
            col for col in columns
            if any(term in col for term in forbidden)
        }, (table, columns)
    assert _configure(runtime)["status"] == 201
    persisted = "\n".join(runtime.db.iterdump())
    assert "transport-only-token" not in persisted


def test_only_sole_owner_can_configure_named_eligible_member():
    runtime = Runtime()
    denied = runtime.request(
        "alice", "PUT", body={"successor": "successor"})
    assert denied["status"] == 403
    platform = runtime.request(
        "platform-admin", "PUT", body={"successor": "successor"})
    assert platform["status"] == 403

    runtime.actor = "owner"
    unknown = runtime.request(
        "owner", "PUT", body={"successor": "outsider"})
    assert unknown["status"] == 400
    runtime.db.execute(
        "UPDATE org_members SET role='owner' WHERE name='alice'")
    runtime.db.commit()
    ambiguous = _configure(runtime)
    assert ambiguous["status"] == 409
    assert ambiguous["data"]["error"] == "single_owner_required"


def test_warning_job_is_deduplicated_and_does_not_open_case():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 24 * policy.DAY_MS
    assert _run(api.process_warnings(runtime)) == 1
    assert _run(api.process_warnings(runtime)) == 0
    assert len(runtime.notifications) == 1
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM org_succession_cases"
    ).fetchone()[0] == 0
    event = runtime.db.execute(
        "SELECT event_type,status FROM org_succession_events "
        "WHERE event_type='warning-issued'"
    ).fetchone()
    assert tuple(event) == ("warning-issued", "warning")


def test_cancel_during_grace_preserves_every_role_and_permission():
    runtime = Runtime()
    protected = runtime.protected_state()
    roles = runtime.roles()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    opened = runtime.request("alice", "POST", "activate")
    assert opened["status"] == 201
    assert opened["data"]["case"]["status"] == "grace"
    cancelled = runtime.request("owner", "POST", "cancel")
    assert cancelled["status"] == 200
    assert cancelled["data"]["case"] is None
    assert runtime.roles() == roles
    assert runtime.protected_state() == protected
    assert runtime.db.execute(
        "SELECT status FROM org_succession_cases"
    ).fetchone()[0] == "cancelled"


def test_threshold_approvals_and_grace_complete_scoped_roles_only():
    runtime = Runtime()
    protected = runtime.protected_state()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("alice", "POST", "activate")["status"] == 201

    assert runtime.request("owner", "POST", "approve")["status"] == 403
    assert runtime.request("successor", "POST", "approve")["status"] == 403
    assert runtime.request(
        "platform-admin", "POST", "approve")["status"] == 403
    assert runtime.request("alice", "POST", "approve")["status"] == 200
    # Duplicate approval is idempotent and does not inflate the threshold.
    assert runtime.request(
        "alice", "POST", "approve")["data"]["case"]["approvalCount"] == 1
    assert runtime.request("bob", "POST", "approve")["status"] == 200

    early = runtime.request("alice", "POST", "finalize")
    assert early["status"] == 409
    assert early["data"]["error"] == "grace_period_active"
    runtime.now_ms += 8 * policy.DAY_MS
    completed = runtime.request("charlie", "POST", "finalize")
    assert completed["status"] == 200
    assert runtime.roles() == {
        "owner": "admin",
        "successor": "owner",
        "alice": "member",
        "bob": "member",
        "charlie": "member",
    }
    assert runtime.protected_state() == protected
    assert completed["data"]["configured"] is False
    boundary = completed["data"]["transferBoundary"]
    assert boundary["nonCustodial"] is True
    assert boundary["platformAdministratorOverride"] is False
    assert "private keys" in boundary["neverTransferred"]
    assert "repository and private-repository permissions" in boundary["preserved"]


def test_removed_approver_stops_counting_and_database_guard_fails_closed():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("alice", "POST", "activate")["status"] == 201
    assert runtime.request("alice", "POST", "approve")["status"] == 200
    assert runtime.request("bob", "POST", "approve")["status"] == 200
    runtime.db.execute("DELETE FROM org_members WHERE name='bob'")
    runtime.db.commit()
    runtime.now_ms += 8 * policy.DAY_MS
    denied = runtime.request("charlie", "POST", "finalize")
    assert denied["status"] == 409
    assert denied["data"]["approvalCount"] == 1
    assert runtime.roles()["owner"] == "owner"
    assert runtime.roles()["successor"] == "admin"

    case_id = runtime.db.execute(
        "SELECT case_id FROM org_succession_cases WHERE status='grace'"
    ).fetchone()[0]
    with pytest.raises(sqlite3.IntegrityError) as error:
        runtime.db.execute(
            "UPDATE org_succession_cases SET status='completed',resolved_at=? "
            "WHERE case_id=?",
            (runtime.now_ms, case_id),
        )
    assert "succession_approval_threshold_not_met" in str(error.value)


def test_members_added_after_grace_opens_cannot_approve_or_complete():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.db.execute(
        "INSERT INTO org_members VALUES (?,?,?,?,?)",
        ("bi:org:acme", "bi:gwen", "guest", "gwen", runtime.now_ms))
    runtime.db.commit()
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("successor", "POST", "activate")["status"] == 201
    runtime.accounts.update({
        name: {"name": name, "status": "active", "kind": "user"}
        for name in ("puppet-one", "puppet-two")
    })
    for name in ("puppet-one", "puppet-two"):
        runtime.db.execute(
            "INSERT INTO org_members VALUES (?,?,?,?,?)",
            ("bi:org:acme", "bi:" + name, "member", name, runtime.now_ms))
    runtime.db.execute(
        "UPDATE org_members SET role='member' WHERE name='gwen'")
    runtime.db.commit()
    runtime.accounts["gwen"] = {
        "name": "gwen", "status": "active", "kind": "user"}
    for name in ("puppet-one", "puppet-two", "gwen"):
        refused = runtime.request(name, "POST", "approve")
        assert refused["status"] == 403
        assert refused["data"]["error"] == "approver_joined_after_grace_opened"
    case_id = runtime.db.execute(
        "SELECT case_id FROM org_succession_cases WHERE status='grace'"
    ).fetchone()[0]
    with pytest.raises(sqlite3.IntegrityError) as error:
        runtime.db.execute(
            "INSERT INTO org_succession_approvals VALUES (?,?,?)",
            (case_id, "bi:puppet-one", runtime.now_ms))
    assert "succession_approver_not_authorized" in str(error.value)
    with pytest.raises(sqlite3.IntegrityError) as error:
        runtime.db.execute(
            "DELETE FROM org_succession_case_approvers")
    assert "succession_approver_snapshot_immutable" in str(error.value)
    assert runtime.request("alice", "POST", "approve")["status"] == 200
    runtime.now_ms += 8 * policy.DAY_MS
    denied = runtime.request("successor", "POST", "finalize")
    assert denied["status"] == 409
    assert denied["data"]["approvalCount"] == 1
    assert runtime.roles()["owner"] == "owner"


def test_check_in_cancels_case_even_after_deadline_before_completion():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("alice", "POST", "activate")["status"] == 201
    runtime.now_ms += 8 * policy.DAY_MS
    checked = runtime.request("owner", "POST", "check-in")
    assert checked["status"] == 200
    assert checked["data"]["case"] is None
    assert runtime.roles()["owner"] == "owner"
    assert runtime.roles()["successor"] == "admin"


def test_append_only_history_cases_and_approvals_are_enforced_by_sql():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("alice", "POST", "activate")["status"] == 201
    assert runtime.request("alice", "POST", "approve")["status"] == 200
    for sql, message in (
        (
            "DELETE FROM org_succession_events",
            "succession_events_append_only",
        ),
        (
            "UPDATE org_succession_events SET status='disabled'",
            "succession_events_append_only",
        ),
        (
            "DELETE FROM org_succession_approvals",
            "succession_approvals_append_only",
        ),
        (
            "DELETE FROM org_succession_cases",
            "succession_cases_append_only",
        ),
    ):
        with pytest.raises(sqlite3.IntegrityError) as error:
            runtime.db.execute(sql)
        assert message in str(error.value)


def test_organization_delete_disables_and_cancels_but_preserves_history():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    runtime.now_ms += 31 * policy.DAY_MS
    assert runtime.request("alice", "POST", "activate")["status"] == 201
    runtime.db.execute(
        "DELETE FROM orgs WHERE org_bi=?", ("bi:org:acme",))
    runtime.db.commit()
    assert runtime.db.execute(
        "SELECT enabled FROM org_succession_policies"
    ).fetchone()[0] == 0
    assert runtime.db.execute(
        "SELECT status FROM org_succession_cases"
    ).fetchone()[0] == "cancelled"
    disabled = runtime.db.execute(
        "SELECT event_type,status FROM org_succession_events "
        "WHERE dedupe_key LIKE 'org-delete:%'"
    ).fetchone()
    assert tuple(disabled) == ("configuration-disabled", "disabled")
    assert runtime.db.execute(
        "SELECT COUNT(*) FROM org_succession_events"
    ).fetchone()[0] >= 3


def test_status_is_member_only_no_store_and_exposes_bounded_history():
    runtime = Runtime()
    assert _configure(runtime)["status"] == 201
    status = runtime.request("alice", "GET")
    assert status["status"] == 200
    assert status["cache"] == "no-store, max-age=0, must-revalidate"
    assert status["headers"]["x-content-type-options"] == "nosniff"
    assert status["data"]["policy"]["successor"] == "successor"
    assert len(status["data"]["history"]) <= policy.MAX_HISTORY
    outsider = runtime.request("platform-admin", "GET")
    assert outsider["status"] == 403


def test_operations_document_states_exact_non_custodial_boundary():
    text = DOC.read_text(encoding="utf-8").lower()
    for phrase in (
        "exactly two scoped changes",
        "wallets, wallet addresses, funds",
        "private keys",
        "user accounts",
        "agent credentials",
        "devices, or device ownership",
        "repository ownership",
        "private-repository",
        "platform-administrator authorization",
        "append-only",
        "30–730 days",
        "7–90 days",
        "2–20",
    ):
        assert phrase in text
