"""Federated reward reports require original, fresh mirror attestations."""

import ast
import asyncio
from pathlib import Path
import re
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)

import sys

sys.path.insert(0, str(ROOT / "src"))
import edge_routing  # noqa: E402
import reward_policy  # noqa: E402


NOW = 2_000_000_000_000
FRESH_MS = 120_000
WALLET = "11111111111111111111111111111111"
PUBLIC_KEY = "A" * 43
REGISTRATION_SIGNATURE = "B" * 86
HEALTH_SIGNATURE = "C" * 86
REFS = "d" * 64
OPERATIONS = "e" * 64
BASE_URL = "https://mirror.example.com"


def _load(*names, extra_globals=None):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra_globals or {})
    exec(compile(ast.fix_missing_locations(ast.Module(
        body=selected, type_ignores=[])), str(ENTRY), "exec"), namespace)
    return namespace


def _clean(value, maximum):
    return str(value or "")[:maximum]


def attestation(*, checked_at=NOW, wallet=WALLET):
    message = edge_routing.repository_health_challenge(
        "mirror-one",
        "fresh-random-health-nonce",
        checked_at,
        "forkmesh",
        "forkmesh",
        True,
        "ok",
        REFS,
        OPERATIONS,
    )
    return {
        "name": "mirror-one",
        "operator": "alice",
        "wallet": wallet,
        "publicKey": PUBLIC_KEY,
        "baseUrl": BASE_URL,
        "registrationSignature": REGISTRATION_SIGNATURE,
        "registrationIssuedAt": NOW - 1_000_000,
        "healthMessage": message,
        "healthSignature": HEALTH_SIGNATURE,
        "checkedAt": checked_at,
        "healthy": True,
        "integrity": "ok",
        "mirrorsForkMesh": True,
        "forkmeshVerifiedAt": checked_at,
        "refsSha256": REFS,
        "operationsSha256": OPERATIONS,
        "requiredOperationsVerified": True,
        "abuseFlagged": False,
    }


def _verification_globals():
    return {
        "clean_string": _clean,
        "MAX_NODE_NAME": 63,
        "FEDERATED_HEALTH_MESSAGE_MAX_BYTES": 2048,
        "https_routing": edge_routing,
        "valid_node_name": lambda value: bool(re.fullmatch(
            r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", value)),
        "valid_node_pubkey": lambda value: bool(re.fullmatch(
            r"[A-Za-z0-9_-]{43}", value)),
        "SOLANA_RE": re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$"),
        "re": re,
    }


def test_wallet_only_report_is_not_an_attestation():
    namespace = _load(
        "_federated_reward_attestation",
        extra_globals=_verification_globals(),
    )
    normalize = namespace["_federated_reward_attestation"]
    assert normalize({"name": "mirror-one", "wallet": WALLET}) is None
    normalized = normalize(attestation())
    assert normalized["healthMessage"].startswith(
        "forkmesh-https-health-repository-v1\n")
    assert normalized["refsSha256"] == REFS


def test_original_registration_and_health_signatures_are_reverified():
    calls = []

    async def verify(public_key, signature, message):
        calls.append((public_key, signature, message.decode()))
        return (
            public_key == PUBLIC_KEY
            and signature in (REGISTRATION_SIGNATURE, HEALTH_SIGNATURE)
        )

    namespace = _load(
        "_federated_reward_attestation",
        "_verified_federated_reward_attestation",
        extra_globals={
            **_verification_globals(),
            "ed25519_verify": verify,
            "hmac": __import__("hmac"),
            "hashlib": __import__("hashlib"),
        },
    )
    verify_attestation = namespace[
        "_verified_federated_reward_attestation"]
    accepted = asyncio.run(verify_attestation(
        object(), attestation(), NOW, REFS, FRESH_MS))
    assert accepted["deviceId"]
    assert [call[1] for call in calls] == [
        REGISTRATION_SIGNATURE, HEALTH_SIGNATURE]
    assert asyncio.run(verify_attestation(
        object(), attestation(checked_at=NOW - FRESH_MS - 1),
        NOW, REFS, FRESH_MS)) is None
    assert asyncio.run(verify_attestation(
        object(), attestation(), NOW, "f" * 64, FRESH_MS)) is None


def test_main_relay_ingest_rejects_wallet_only_and_persists_verified_evidence():
    writes = []

    async def authorized(_env, _request, _body):
        return "relay-blind-index"

    async def verify(_env, value, *_args):
        if "healthMessage" not in value:
            return None
        return {
            **value,
            "deviceId": "device-digest",
            "registrationMessage": "registration",
        }

    async def run(_env, sql, *args):
        writes.append((sql, args))

    async def first(_env, _sql, *_args):
        return None

    namespace = _load("_federation_nodes", extra_globals={
        "_is_main_relay": lambda _env: True,
        "_relay_authorized": authorized,
        "_verified_federated_reward_attestation": verify,
        "_reward_config": lambda _env: {"healthFreshMs": FRESH_MS},
        "_https_mirror_expected_forkmesh_refs": lambda _env: asyncio.sleep(
            0, result=REFS),
        "blind_index": lambda _env, value: asyncio.sleep(
            0, result="bi:" + value),
        "d1_first": first,
        "d1_run": run,
        "Date": SimpleNamespace(now=lambda: NOW),
        "FEDERATION_NODES_MAX_BYTES": 2 * 1024 * 1024,
        "REWARD_OBSERVATION_GAP_MS": 600_000,
        "hashlib": __import__("hashlib"),
        "json": __import__("json"),
        "json_response": lambda payload, **_kwargs: payload,
    })
    request = SimpleNamespace(
        headers={"content-length": "0"},
        text=lambda: asyncio.sleep(0, result=__import__("json").dumps({
            "nodes": [
                {"name": "wallet-only", "wallet": WALLET},
                attestation(),
            ],
        }))
    )
    response = asyncio.run(namespace["_federation_nodes"](
        object(), request))
    assert response["count"] == 1
    assert response["rejected"] == 1
    insert = next(item for item in writes
                  if "INSERT INTO federated_presence" in item[0])
    assert "health_message" in insert[0]
    assert "registration_sig" in insert[0]
    assert attestation()["healthMessage"] in insert[1]
    assert WALLET in insert[1]


def test_federated_candidate_enters_the_shared_random_and_instant_snapshot():
    row = {
        "relay_bi": "relayblindindex0123456789",
        "relay_label": "Community relay",
        "name": "mirror-one",
        "operator_id": "operator-bi",
        "device_id": "device-digest",
        "wallet": WALLET,
        "public_key": PUBLIC_KEY,
        "base_url": BASE_URL,
        "registration_sig": REGISTRATION_SIGNATURE,
        "registration_issued_at": NOW - 1_000_000,
        "health_message": attestation()["healthMessage"],
        "health_sig": HEALTH_SIGNATURE,
        "checked_at": NOW,
        "healthy": 1,
        "integrity": "ok",
        "forkmesh_active": 1,
        "forkmesh_verified_at": NOW,
        "forkmesh_refs_sha256": REFS,
        "forkmesh_operations_sha256": OPERATIONS,
        "abuse_blocked": 0,
        "attestation_id": "a" * 64,
        "first_verified_at": NOW - 60_000,
        "ts": NOW,
    }

    async def all_rows(_env, sql, *_args):
        if "FROM mirror_https_endpoints e" in sql:
            return []
        if "FROM federated_presence fp" in sql:
            return [row]
        raise AssertionError(sql)

    async def verify(_env, _value, *_args):
        return {
            "name": "mirror-one",
            "wallet": WALLET,
            "deviceId": "device-digest",
            "checkedAt": NOW,
        }

    namespace = _load(
        "_eligible_reward_snapshot", "_write_reward_observations",
        extra_globals={
        "Date": SimpleNamespace(now=lambda: NOW),
        "_reward_config": lambda _env: {
            "healthFreshMs": FRESH_MS,
            "minimumUptimeMs": 0,
            "minimumContributionUnits": 0,
        },
        "_https_mirror_expected_forkmesh_refs": lambda _env: asyncio.sleep(
            0, result=REFS),
        "d1_all": all_rows,
        "d1_run": lambda *_args: asyncio.sleep(0),
        "_is_main_relay": lambda _env: True,
        "_verified_federated_reward_attestation": verify,
        "ACCOUNT_PRESENCE_STALE_MS": 90_000,
        "MAX_NODE_NAME": 63,
        "clean_string": _clean,
        "reward_policy": reward_policy,
        "re": re,
        "hmac": __import__("hmac"),
        "hashlib": __import__("hashlib"),
        })
    snapshot = asyncio.run(namespace["_eligible_reward_snapshot"](
        object(), NOW))
    assert len(snapshot["eligible"]) == 1
    assert snapshot["eligible"][0]["walletAddress"] == WALLET
    assert snapshot["eligible"][0]["nodeId"].startswith("relay-")
    assert "original node-signed" in snapshot["source"]
    assert any(
        "revocable trust boundary" in item
        for item in snapshot["limitations"]
    )

    random_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _distribute_central_fund"):
        ENTRY_TEXT.index("\n\nasync def _solana_contribution_details")]
    instant_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _create_instant_distribution_intent"):
        ENTRY_TEXT.index("\n\nasync def _finalize_reward_contribution")]
    assert "_eligible_reward_snapshot(env, now)" in random_body
    assert "_eligible_reward_snapshot(env, now)" in instant_body


def test_direct_reward_snapshot_bulk_loads_devices_and_observations():
    operations = sorted({
        "git-info-refs", "git-upload-pack", "tree", "blob", "raw",
    })
    operations_hash = edge_routing.operations_sha256(operations)
    direct_rows = []
    records = {}
    public_keys = {}
    for index in range(18):
        node_bi = f"node-bi-{index:02d}"
        public_key = chr(ord("A") + index) * 43
        public_keys[node_bi] = public_key
        records[node_bi] = {
            "status": "active",
            "name": f"mirror-{index:02d}",
            "owner": f"operator-{index:02d}",
            "solana": WALLET,
        }
        direct_rows.append({
            "node_bi": node_bi,
            "node_name": f"mirror-{index:02d}",
            "base_url": f"https://mirror-{index:02d}.example.com",
            "public_key": public_key,
            "checked_at": NOW,
            "healthy": 1,
            "integrity": "ok",
            "abuse_blocked": 0,
            "forkmesh_verified_at": NOW,
            "forkmesh_refs_sha256": REFS,
            "forkmesh_operations_sha256": operations_hash,
            "forkmesh_operations_json": __import__("json").dumps(operations),
            "forkmesh_active": 1,
            "registration_sig": REGISTRATION_SIGNATURE,
            "issued_at": NOW - 1_000,
            "health_sig": HEALTH_SIGNATURE,
            "health_message": "signed-health",
            "presence_ts": NOW,
            "data": node_bi,
            "node_pubkey": "",
            "first_verified_at": NOW - 60_000,
            "last_verified_at": NOW - 60_000,
            "consecutive_checks": 2,
            "contribution_units": 0,
        })

    reads = []
    writes = []

    async def all_rows(_env, sql, *args):
        reads.append((sql, args))
        if "FROM mirror_https_endpoints e" in sql:
            return direct_rows
        if "FROM account_devices" in sql:
            assert __import__("json").loads(args[0]) == sorted(public_keys)
            return [{
                "account_bi": node_bi,
                "pubkey": public_key,
                "capabilities": "owner_sign",
            } for node_bi, public_key in public_keys.items()]
        raise AssertionError(sql)

    async def decrypt(_env, value):
        return records[value]

    async def blind(_env, value):
        return "blind:" + value

    async def write(_env, sql, *args):
        writes.append((sql, args))

    async def verify(_env, value, *_args):
        return {"deviceId": "verified", **value}

    namespace = _load(
        "_eligible_reward_snapshot", "_write_reward_observations",
        extra_globals={
            "Date": SimpleNamespace(now=lambda: NOW),
            "_reward_config": lambda _env: {
                "healthFreshMs": FRESH_MS,
                "minimumUptimeMs": 0,
                "minimumContributionUnits": 0,
            },
            "_https_mirror_expected_forkmesh_refs": (
                lambda _env: asyncio.sleep(0, result=REFS)),
            "d1_all": all_rows,
            "d1_run": write,
            "decrypt_row": decrypt,
            "blind_index": blind,
            "_is_main_relay": lambda _env: False,
            "_verified_federated_reward_attestation": verify,
            "ACCOUNT_PRESENCE_STALE_MS": 90_000,
            "REWARD_OBSERVATION_GAP_MS": 20 * 60_000,
            "MAX_NODE_NAME": 63,
            "HTTPS_MIRROR_REQUIRED_FORKMESH_OPERATIONS": frozenset(operations),
            "clean_string": _clean,
            "reward_policy": reward_policy,
            "https_routing": edge_routing,
            "json": __import__("json"),
            "re": re,
            "hmac": __import__("hmac"),
            "hashlib": __import__("hashlib"),
        },
    )
    snapshot = asyncio.run(namespace["_eligible_reward_snapshot"](
        object(), NOW))

    assert len(reads) == 2
    assert sum("FROM account_devices" in sql for sql, _args in reads) == 1
    assert len(writes) == 2
    assert [len(args) for _sql, args in writes] == [80, 10]
    assert all(
        "INSERT INTO reward_node_observations" in sql
        for sql, _args in writes
    )
    assert len(snapshot["eligible"]) == 1


def test_upgrade_migration_deletes_wallet_only_rows_and_invalidates_readiness():
    import sqlite3

    migration = (
        ROOT / "migrations"
        / "0057_noncustodial_readiness_federated_rewards.sql"
    ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    connection.executescript(
        """
        CREATE TABLE federated_presence (
          relay_bi TEXT NOT NULL, wallet TEXT NOT NULL, name TEXT,
          ts INTEGER NOT NULL, PRIMARY KEY(relay_bi,wallet));
        INSERT INTO federated_presence VALUES
          ('relay','11111111111111111111111111111111','wallet-only',1);
        CREATE TABLE mirror_https_endpoints (
          node_bi TEXT PRIMARY KEY);
        CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
        INSERT INTO schema_meta VALUES
          ('legacy_wallet_custody_v2','clean-v2:old-build');
        """
    )
    connection.executescript(migration)
    assert connection.execute(
        "SELECT COUNT(*) FROM federated_presence").fetchone()[0] == 0
    columns = {
        row[1]
        for row in connection.execute(
            "PRAGMA table_info(federated_presence)")
    }
    assert {
        "health_message", "health_sig", "forkmesh_refs_sha256",
        "attestation_id", "first_verified_at",
    }.issubset(columns)
    assert connection.execute(
        "SELECT v FROM schema_meta "
        "WHERE k='legacy_wallet_custody_v2'").fetchone() is None
    connection.close()
