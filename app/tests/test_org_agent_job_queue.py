"""Behavioral regressions for durable FIFO organization-agent job claims."""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"


def _load_handler(store):
    tree = ast.parse((
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "repo_org_agent_jobs_handler"
    ]
    assert len(selected) == 1
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    class Clock:
        @staticmethod
        def now():
            return 2_100_000_000_000

    ids = iter(f"lease-{number}" for number in range(1, 100))

    async def noop(*_args, **_kwargs):
        return None

    def response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    namespace = {
        "Date": Clock,
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "ORG_AGENT_JOB_LEASE_MS": 120_000,
        "ensure_schema": noop,
        "method_name": lambda request: request.method,
        "_authorized_agent_node_signing_key": lambda *_args: asyncio.sleep(
            0, result=store.claimant_key),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_run": store.run,
        "d1_first": store.first,
        "decrypt_row": lambda _env, value: asyncio.sleep(
            0, result=None if value == "corrupt" else value),
        "_ap_uuid": lambda: next(ids),
        "json_response": response,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["repo_org_agent_jobs_handler"]


class QueueStore:
    claimant_key = "claimant-key"

    def __init__(self, count=0):
        self.rows = [
            {
                "id": number,
                "session_id": f"session-{number}",
                "target_node": "desktop1",
                "repo": "forkmesh",
                "status": "queued",
                "lease_id": "",
                "updated_at": 2_100_000_000_000,
                "data": {
                    "sessionId": f"session-{number}",
                    "prompt": f"task {number}",
                    "securityCheck": {"required": False},
                },
            }
            for number in range(1, count + 1)
        ]

    async def run(self, _env, sql, *args):
        if sql.startswith(
                "UPDATE org_agent_jobs SET status='queued',lease_id=''"):
            now, owner, repo, threshold = args
            for row in self.rows:
                if (
                    row["target_node"] == owner
                    and row["repo"] == repo
                    and row["status"] == "leased"
                    and row["updated_at"] < threshold
                ):
                    row.update(
                        status="queued", lease_id="", updated_at=now)
            return
        if "SET status='failed'" in sql:
            now, job_id, owner = args
            for row in self.rows:
                if (
                    row["id"] == job_id
                    and row["target_node"] == owner
                    and row["status"] == "queued"
                ):
                    row.update(status="failed", updated_at=now)
            return
        if "SET status='leased'" in sql:
            lease_id, now, job_id, owner = args
            for row in self.rows:
                if (
                    row["id"] == job_id
                    and row["target_node"] == owner
                    and row["status"] == "queued"
                ):
                    row.update(
                        status="leased",
                        lease_id=lease_id,
                        updated_at=now,
                    )
            return
        raise AssertionError("unexpected mutation: " + sql)

    async def first(self, _env, sql, *args):
        if "FROM org_agent_jobs queued" in sql:
            owner, repo, *rest = args
            skipped = set(rest[:-2])
            leased_owner, leased_repo = rest[-2:]
            assert (owner, repo) == (leased_owner, leased_repo)
            if any(
                row["target_node"] == owner
                and row["repo"] == repo
                and row["status"] == "leased"
                for row in self.rows
            ):
                return None
            candidate = next(
                (
                    dict(row)
                    for row in self.rows
                    if row["target_node"] == owner
                    and row["repo"] == repo
                    and row["status"] == "queued"
                    and row["id"] not in skipped
                ),
                None,
            )
            # Capture before yielding so concurrent callers exercise the
            # conditional-update verification rather than serializing here.
            await asyncio.sleep(0)
            return candidate
        if "AND status='leased' AND lease_id=?" in sql:
            job_id, owner, repo, lease_id = args
            return next(
                (
                    {"id": row["id"]}
                    for row in self.rows
                    if row["id"] == job_id
                    and row["target_node"] == owner
                    and row["repo"] == repo
                    and row["status"] == "leased"
                    and row["lease_id"] == lease_id
                ),
                None,
            )
        raise AssertionError("unexpected query: " + sql)


def _request():
    return SimpleNamespace(method="GET")


def test_claims_one_job_at_a_time_in_fifo_order_without_admission_cap():
    async def scenario():
        store = QueueStore(17)
        handler = _load_handler(store)
        claimed = []
        for _index in range(17):
            response = await handler(
                object(), _request(), "desktop1", "forkmesh")
            assert response["status"] == 200
            jobs = response["data"]["jobs"]
            assert len(jobs) == 1
            claimed.append(jobs[0]["jobId"])
            row = next(
                row for row in store.rows if row["id"] == jobs[0]["jobId"])
            # A successful running report releases the preflight lease and lets
            # the next oldest durable row start.
            row["status"] = "running"
        assert claimed == list(range(1, 18))

    asyncio.run(scenario())


def test_concurrent_claimers_never_receive_the_same_job():
    async def scenario():
        store = QueueStore(1)
        handler = _load_handler(store)
        responses = await asyncio.gather(
            handler(object(), _request(), "desktop1", "forkmesh"),
            handler(object(), _request(), "desktop1", "forkmesh"),
        )
        jobs = [
            job
            for response in responses
            for job in response["data"]["jobs"]
        ]
        assert [job["jobId"] for job in jobs] == [1]

    asyncio.run(scenario())


def test_corrupt_and_expired_rows_do_not_starve_the_fifo():
    async def scenario():
        store = QueueStore(3)
        store.rows[0]["data"] = "corrupt"
        store.rows[1].update(
            status="leased",
            lease_id="expired",
            updated_at=2_100_000_000_000 - 120_001,
        )
        handler = _load_handler(store)
        response = await handler(
            object(), _request(), "desktop1", "forkmesh")
        assert store.rows[0]["status"] == "failed"
        assert response["data"]["jobs"][0]["jobId"] == 2

    asyncio.run(scenario())


def test_a_devices_own_cloud_handoff_is_left_for_another_node():
    """A "Move to cloud" job must not be claimed back by the desk it left.

    Every node this account runs drains one queue, so the origin polls for the
    very job it just queued.  It is passed over and left queued -- not failed,
    not leased -- so the next node online still gets it, and the FIFO behind it
    keeps moving in the meantime.
    """

    async def scenario():
        store = QueueStore(2)
        store.rows[0]["data"]["fromKey"] = store.claimant_key
        handler = _load_handler(store)
        response = await handler(
            object(), _request(), "desktop1", "forkmesh")
        assert response["data"]["jobs"][0]["jobId"] == 2
        assert store.rows[0]["status"] == "queued"
        assert store.rows[0]["lease_id"] == ""

    asyncio.run(scenario())


def test_another_nodes_cloud_handoff_is_claimed_normally():
    async def scenario():
        store = QueueStore(1)
        store.rows[0]["data"]["fromKey"] = "some-other-desk"
        handler = _load_handler(store)
        response = await handler(
            object(), _request(), "desktop1", "forkmesh")
        assert response["data"]["jobs"][0]["jobId"] == 1
        assert store.rows[0]["status"] == "leased"

    asyncio.run(scenario())


def _load_claim_handler(store):
    """The named-session claim behind the desktop's "Continue on this desktop"."""

    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name in (
            "repo_owner_agent_session_claim_handler",
            "_agent_job_queued_by_signer",
        )
    ]
    assert len(selected) == 2
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    from urllib.parse import parse_qs, urlparse

    async def blind(_env, name):
        return "bi:" + name

    async def owns(_env, actor, node):
        return (actor, node) in store.owned

    async def org_owner(_env, node, repo, actor):
        return (node, repo, actor) in store.org_owners

    async def account_key(_env, _request, signer):
        return store.account_keys.get(signer, "")

    class Clock:
        @staticmethod
        def now():
            return 2_100_000_000_000

    ids = iter(f"lease-{number}" for number in range(1, 100))

    async def noop(*_args, **_kwargs):
        return None

    def response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    def error(code, message, status, **kwargs):
        return response(
            {"ok": False, "error": code, "message": message, **kwargs},
            status=status,
        )

    namespace = {
        "Date": Clock,
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "ORG_AGENT_JOB_LEASE_MS": 120_000,
        "ensure_schema": noop,
        "method_name": lambda request: request.method,
        "_authorized_agent_claimant_key": lambda *_args: asyncio.sleep(
            0, result=store.claimant_key),
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "d1_run": store.run,
        "d1_first": store.first,
        "decrypt_row": lambda _env, value: asyncio.sleep(
            0, result=None if value == "corrupt" else value),
        "_ap_uuid": lambda: next(ids),
        "json_response": response,
        "_org_agent_error": error,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "valid_node_name": lambda name: bool(name) and all(
            ch.isalnum() or ch == "-" for ch in name),
        "blind_index": blind,
        "_account_owns_node": owns,
        "_org_owner_of_linked_repo": org_owner,
        "_authorized_owner_signing_key": account_key,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["repo_owner_agent_session_claim_handler"]


class ClaimStore(QueueStore):
    """One job aimed at a mirror, plus the session row the website shows."""

    def __init__(self, target_node="mirror-ams", organization="mirror35",
                 org_bi="org-bi"):
        super().__init__(1)
        self.owned = set()
        self.org_owners = set()
        self.account_keys = {}
        self.rows[0].update(target_node=target_node, org_bi=org_bi)
        self.rows[0]["data"]["organization"] = organization
        self.sessions = [{
            "session_id": "session-1",
            "org_bi": org_bi,
            "target_node": target_node,
            "updated_at": 0,
        }]

    async def run(self, _env, sql, *args):
        if sql.startswith("UPDATE org_agent_sessions SET target_node=?"):
            target_node, now, session_id, org_bi = args
            for row in self.sessions:
                if (
                    row["session_id"] == session_id
                    and row["org_bi"] == org_bi
                ):
                    row.update(target_node=target_node, updated_at=now)
            return
        if sql.startswith(
                "UPDATE org_agent_jobs SET status='queued',lease_id=''"):
            now, session_id, repo, threshold = args
            for row in self.rows:
                if (
                    row["session_id"] == session_id
                    and row["repo"] == repo
                    and row["status"] == "leased"
                    and row["updated_at"] < threshold
                ):
                    row.update(status="queued", lease_id="", updated_at=now)
            return
        if "SET status='leased'" in sql:
            lease_id, target_node, now, job_id = args
            for row in self.rows:
                if row["id"] == job_id and row["status"] == "queued":
                    row.update(
                        status="leased",
                        lease_id=lease_id,
                        target_node=target_node,
                        updated_at=now,
                    )
            return
        raise AssertionError("unexpected mutation: " + sql)

    async def first(self, _env, sql, *args):
        if "FROM org_agent_jobs WHERE session_id=?" in sql:
            session_id, repo = args
            return next(
                (
                    dict(row)
                    for row in self.rows
                    if row["session_id"] == session_id
                    and row["repo"] == repo
                    and row["status"] == "queued"
                ),
                None,
            )
        return await super().first(_env, sql, *args)


def _claim_request(signer=""):
    query = ("?owner=" + signer + "&ts=2100000000000&sig=sig") if signer else ""
    return SimpleNamespace(
        method="POST",
        url="https://app.forkmesh.com/api/repo/forkmesh/forkmesh/"
        "agent-sessions/session-1/claim" + query)


def test_a_desktop_claims_a_run_queued_for_an_absent_mirror():
    """The point of the endpoint: the chosen mirror never picked the job up.

    The claim re-targets job and session at the account namespace the
    claiming desktop reports results in -- the result route matches a lease
    by target node, so a run claimed away from a mirror could otherwise never
    report what it did.
    """

    async def scenario():
        store = ClaimStore()
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request(), "mirror35", "forkmesh", "session-1")
        assert response["status"] == 200
        job = response["data"]["job"]
        assert job["jobId"] == 1
        assert job["sessionId"] == "session-1"
        assert store.rows[0]["status"] == "leased"
        assert store.rows[0]["lease_id"] == job["leaseId"]
        assert store.rows[0]["target_node"] == "mirror35"
        assert store.sessions[0]["target_node"] == "mirror35"

    asyncio.run(scenario())


def test_a_claim_never_reaches_another_accounts_queued_job():
    async def scenario():
        store = ClaimStore(organization="someone-else")
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request(), "mirror35", "forkmesh", "session-1")
        assert response["status"] == 403
        assert store.rows[0]["status"] == "queued"
        assert store.sessions[0]["target_node"] == "mirror-ams"

    asyncio.run(scenario())


def test_a_run_a_node_already_took_is_not_claimed_by_hand():
    async def scenario():
        store = ClaimStore()
        store.rows[0].update(
            status="leased", lease_id="held", updated_at=2_100_000_000_000)
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request(), "mirror35", "forkmesh", "session-1")
        assert response["status"] == 409
        assert response["data"]["error"] == "job_not_queued"
        assert store.rows[0]["lease_id"] == "held"

    asyncio.run(scenario())


def test_an_expired_lease_does_not_keep_a_run_from_being_claimed():
    async def scenario():
        store = ClaimStore()
        store.rows[0].update(
            status="leased",
            lease_id="expired",
            updated_at=2_100_000_000_000 - 120_001,
        )
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request(), "mirror35", "forkmesh", "session-1")
        assert response["status"] == 200
        assert store.rows[0]["target_node"] == "mirror35"

    asyncio.run(scenario())


def test_a_claim_is_a_post():
    async def scenario():
        store = ClaimStore()
        handler = _load_claim_handler(store)
        response = await handler(
            object(), SimpleNamespace(method="GET"), "mirror35", "forkmesh",
            "session-1")
        assert response["status"] == 405
        assert store.rows[0]["status"] == "queued"

    asyncio.run(scenario())


def _load_agent_node_auth(node_keys, verified_key):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"_authorized_agent_node_signing_key"}
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef) and node.name in wanted
    ]
    assert {node.name for node in selected} == wanted
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    from urllib.parse import parse_qs, urlparse

    class Clock:
        @staticmethod
        def now():
            return 2_100_000_000_000

    async def claimed_node_signing_pubkeys(_env, _node):
        return list(node_keys)

    async def verify(public_key, _sig, canonical):
        assert canonical == (
            b"forkmesh-issues-pull-v1\nmirror35\n2100000000000")
        return public_key == verified_key

    namespace = {
        "Date": Clock,
        "LOGIN_MAX_SKEW_MS": 300_000,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "ed25519_verify": verify,
        "_claimed_node_signing_pubkeys": claimed_node_signing_pubkeys,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_authorized_agent_node_signing_key"]


def test_a_managed_mirror_claims_with_its_own_claimed_node_key():
    # A mirror-node service has no desktop account key: its node key is what
    # the owner claimed, and it must be able to claim and report agent jobs.
    request = SimpleNamespace(
        url="https://app.forkmesh.com/api/repo/mirror35/proj/org-agent-jobs"
        "?owner=mirror35&ts=2100000000000&sig=node-sig&mirror=1")
    authorize = _load_agent_node_auth(
        node_keys=["node-key"], verified_key="node-key")
    assert asyncio.run(authorize(object(), request, "mirror35")) == "node-key"

    stranger = _load_agent_node_auth(
        node_keys=["node-key"], verified_key="other-key")
    assert asyncio.run(stranger(object(), request, "mirror35")) == ""


def test_agent_job_endpoints_accept_the_node_key_not_only_account_keys():
    handlers = (ENTRY.parent / "route_handlers.py").read_text(encoding="utf-8")
    entry = ENTRY.read_text(encoding="utf-8")
    assert (
        "await _authorized_agent_node_signing_key(\n        env, request, owner)"
        in entry
    )
    assert (
        "if not await _authorized_agent_node_signing_key(env, request, owner):"
        in handlers
    )
    assert 'in ("start", "continue", "steer")' in handlers
    assert "Confirm its Qt" not in handlers


def _load_agent_claimant_auth(node_key="", account_key="", owned=(),
                              org_owners=()):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_authorized_agent_claimant_key"
    ]
    assert len(selected) == 1
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    from urllib.parse import parse_qs, urlparse

    seen = []

    async def node_key_for(_env, _request, node):
        seen.append(("node", node))
        return node_key

    async def account_key_for(_env, _request, signer):
        seen.append(("account", signer))
        return account_key

    async def owns(_env, actor, node):
        return (actor, node) in owned

    async def org_owner(_env, node, repo, actor):
        return (node, repo, actor) in org_owners

    namespace = {
        "MAX_NODE_NAME": 64,
        "MAX_REPO_SEGMENT": 100,
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "clean_string": lambda value, maximum: str(value or "")[:maximum],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "_authorized_agent_node_signing_key": node_key_for,
        "_authorized_owner_signing_key": account_key_for,
        "_account_owns_node": owns,
        "_org_owner_of_linked_repo": org_owner,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_authorized_agent_claimant_key"], seen


def _alias_claim_request(signer):
    return SimpleNamespace(
        url="https://app.forkmesh.com/api/repo/forkmesh/forkmesh/"
        "agent-sessions/59d02cea4f84599b07d64770599a398d/claim"
        "?owner=" + signer + "&ts=2100000000000&sig=account-sig")


def test_an_org_alias_claim_signed_by_the_node_owners_account_is_accepted():
    authorize, seen = _load_agent_claimant_auth(
        account_key="jett-key", owned={("jett", "mainnode")})
    key = asyncio.run(authorize(
        object(), _alias_claim_request("jett"), "mainnode", "forkmesh"))
    assert key == "jett-key"
    assert seen == [("node", "mainnode"), ("account", "jett")]


def test_an_org_owner_may_claim_on_the_node_serving_the_linked_repo():
    authorize, _seen = _load_agent_claimant_auth(
        account_key="jett-key",
        org_owners={("mainnode", "forkmesh", "jett")})
    assert asyncio.run(authorize(
        object(), _alias_claim_request("jett"), "mainnode", "forkmesh",
    )) == "jett-key"


def test_an_account_unrelated_to_the_node_cannot_claim_its_jobs():
    authorize, seen = _load_agent_claimant_auth(account_key="eve-key")
    assert asyncio.run(authorize(
        object(), _alias_claim_request("eve"), "mainnode", "forkmesh",
    )) == ""
    assert ("account", "eve") not in seen


def test_the_node_proof_still_needs_no_account_lookup():
    authorize, seen = _load_agent_claimant_auth(node_key="node-key")
    assert asyncio.run(authorize(
        object(), _alias_claim_request("mainnode"), "mainnode", "forkmesh",
    )) == "node-key"
    assert seen == [("node", "mainnode")]


def test_claim_and_result_accept_the_account_that_owns_the_node():
    handlers = (ENTRY.parent / "route_handlers.py").read_text(encoding="utf-8")
    entry = ENTRY.read_text(encoding="utf-8")
    call = "if not await _authorized_agent_claimant_key(env, request, owner, repo):"
    assert call in entry
    assert call in handlers


def _runner_pool_claim_store():
    store = ClaimStore(
        target_node="fmnext-n1", organization="jett", org_bi="bi:jett")
    store.owned.add(("jett", "fmnext-n1"))
    store.account_keys["jett"] = "jett-key"
    return store


def test_the_account_claims_its_runner_pool_job_on_an_org_repository():
    async def scenario():
        store = _runner_pool_claim_store()
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request("jett"), "fmnext-n1", "forkmesh",
            "session-1")
        assert response["status"] == 200
        assert response["data"]["job"]["sessionId"] == "session-1"
        assert store.rows[0]["status"] == "leased"
        assert store.rows[0]["target_node"] == "fmnext-n1"

    asyncio.run(scenario())


def test_a_runner_pool_job_needs_the_queuing_accounts_own_proof():
    async def scenario():
        store = _runner_pool_claim_store()
        store.account_keys.clear()
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request("jett"), "fmnext-n1", "forkmesh",
            "session-1")
        assert response["status"] == 403
        assert store.rows[0]["status"] == "queued"

    asyncio.run(scenario())


def test_a_signer_named_on_another_accounts_job_cannot_claim_it():
    async def scenario():
        store = _runner_pool_claim_store()
        store.account_keys["eve"] = "eve-key"
        store.owned.add(("eve", "fmnext-n1"))
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request("eve"), "fmnext-n1", "forkmesh",
            "session-1")
        assert response["status"] == 403
        assert store.rows[0]["status"] == "queued"

    asyncio.run(scenario())


def test_a_runner_pool_job_filed_under_another_blind_index_is_refused():
    async def scenario():
        store = _runner_pool_claim_store()
        store.rows[0]["org_bi"] = "org:forkmesh"
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request("jett"), "fmnext-n1", "forkmesh",
            "session-1")
        assert response["status"] == 403
        assert store.rows[0]["status"] == "queued"

    asyncio.run(scenario())


def test_the_signer_must_still_own_the_node_serving_the_job():
    async def scenario():
        store = _runner_pool_claim_store()
        store.owned.clear()
        handler = _load_claim_handler(store)
        response = await handler(
            object(), _claim_request("jett"), "fmnext-n1", "forkmesh",
            "session-1")
        assert response["status"] == 403

    asyncio.run(scenario())
