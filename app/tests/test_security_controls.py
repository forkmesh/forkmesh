"""Security-boundary tests that run without the Workers runtime."""

import base64
import ast
import hashlib
import importlib.util
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
WORLD_SRC = ROOT.parent / "world" / "src"
SPEC = importlib.util.spec_from_file_location(
    "security_controls", SRC / "security_controls.py"
)
security = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(security)
WORKER_SOURCE = (
    (SRC / "entry.py").read_text(encoding="utf-8") + "\n\n\n"
    + (SRC / "app_runtime.py").read_text(encoding="utf-8")
)


def _b64url(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def _envelope(**changes):
    value = {
        "kind": "forkmesh.owner-sealed",
        "v": 1,
        "alg": "x25519+mlkem768/aes256gcm",
        "nonce": _b64url(b"n" * 12),
        "tag": _b64url(b"t" * 16),
        "body": _b64url(b"opaque ciphertext"),
        "recipients": [{
            "kid": _b64url(hashlib.sha256(b"owner-key").digest()),
            "x25519": _b64url(b"x" * 32),
            "mlkem768": _b64url(b"m" * 1088),
            "nonce": _b64url(b"w" * 12),
            "tag": _b64url(b"g" * 16),
            "key": _b64url(b"k" * 32),
        }],
    }
    value.update(changes)
    return value


def test_roles_are_explicit_and_scope_limited():
    assert security.normalize_role("security-reviewer") == "security_reviewer"
    assert security.normalize_role("root") == ""
    assert security.role_scope_allowed("repository_administrator", "repository")
    assert not security.role_scope_allowed(
        "repository_administrator", "platform"
    )
    assert security.role_scope_allowed("moderator", "organization")
    assert not security.role_scope_allowed(
        "platform_administrator", "repository"
    )
    # Privileged moderation/reviewer/admin grants never become profile badges.
    assert security.public_roles(
        ["mirror_operator", "moderator", "platform_administrator"]
    ) == ["mirror_operator"]


def test_worker_has_no_abuse_quarantine_request_or_response_runtime():
    worker = WORKER_SOURCE
    retired_symbols = (
        "_security_enforce_request",
        "_security_observe_response",
        "_record_security_signal",
        "security_signal_for_path",
        "security_signal_for_response",
        "BoundedTrafficDetector",
        "public_restriction",
        "bounded_restriction_duration",
        "normalize_subject_token",
        "normalize_reason",
        "normalize_confidence",
        "normalize_appeal_status",
        "temporarily_quarantined",
        "/api/security/quarantine",
        "/api/security/appeals",
        "security.auto_quarantine",
        "security.account_restrict",
    )
    for removed in retired_symbols:
        assert removed not in worker
        assert not hasattr(security, removed)

    for removed in (
        "RESTRICTION_REASONS",
        "RESTRICTION_CONFIDENCE",
        "RESTRICTION_STATUSES",
        "APPEAL_STATUSES",
        "AUTO_RESTRICTION_MAX_MS",
        "MANUAL_RESTRICTION_MAX_MS",
        "DEFAULT_QUARANTINE_MS",
        "MAX_APPEAL_TEXT",
    ):
        assert not hasattr(security, removed)

    # Removing the cross-request quarantine does not weaken ordinary endpoint
    # authorization, input validation, or narrowly scoped traffic limits.
    for retained in (
        "signup_rate_check",
        "ROOM_MSG_WINDOW_MS",
        "HOST_RATE_WINDOW_MS",
        "security_roles_handler",
        "security_report_handler",
        "security_audit_handler",
    ):
        assert retained in worker


def test_audit_detail_sanitizer_is_scalar_allowlist_and_secret_denylist():
    cleaned = security.sanitize_audit_details({
        "status": "approved",
        "retryCount": 2,
        "automatic": True,
        "privateKey": "secret",
        "walletAddress": "public-but-sensitive-target",
        "authorization": "Bearer token",
        "source_ip": "192.0.2.4",
        "requestBody": "form contents",
        "nested": {"must": "not survive"},
        "items": ["must", "not", "survive"],
    })
    assert cleaned == {
        "status": "approved",
        "retryCount": 2,
        "automatic": True,
    }


def test_owner_public_bundle_contains_no_private_material_and_binds_key_id():
    x25519 = b"x" * 32
    mlkem = b"m" * 1184
    validated = security.validate_owner_public_bundle({
        "v": 1,
        "x25519": _b64url(x25519),
        "mlkem768": _b64url(mlkem),
        # Caller-provided aliases and private fields are ignored.
        "kid": "attacker-alias",
        "privateKey": "never-retained",
    })
    assert validated == {
        "v": 1,
        "x25519": _b64url(x25519),
        "mlkem768": _b64url(mlkem),
        "kid": _b64url(hashlib.sha256(x25519 + mlkem).digest()),
    }
    assert security.validate_owner_public_bundle({
        "v": 1, "x25519": _b64url(b"x" * 31),
        "mlkem768": _b64url(mlkem),
    }) is None


def test_owner_envelope_round_trip_is_opaque_and_routing_is_allowlisted():
    envelope = _envelope()
    assert security.validate_owner_envelope(envelope) == envelope
    assert security.owner_envelope_key_id(envelope) == (
        envelope["recipients"][0]["kid"])
    encoded = security.encode_owner_envelope(
        envelope,
        {
            "agentId": 42,
            "queuedAt": 1234,
            "repo": "must-not-be-stored-in-routing",
            "plaintext": "must-not-survive",
        },
    )
    assert encoded.startswith("owner-sealed-v1:")
    decoded = security.decode_owner_envelope(encoded)
    assert decoded == {
        "envelope": envelope,
        "routing": {"agentId": 42, "queuedAt": 1234},
    }
    assert "plaintext" not in encoded
    assert security.decode_owner_envelope("owner-sealed-v1:not-base64!") is None
    assert security.validate_owner_envelope(
        _envelope(alg="server-readable/aes")
    ) is None
    assert security.validate_owner_envelope(
        _envelope(body=_b64url(b"x" * 17)), max_body=16
    ) is None
    assert security.validate_owner_envelope(
        _envelope(recipients=[])
    ) is None
    duplicated = _envelope()
    duplicated["recipients"].append(dict(duplicated["recipients"][0]))
    assert security.validate_owner_envelope(duplicated) is None
    incomplete = _envelope()
    del incomplete["recipients"][0]["mlkem768"]
    assert security.validate_owner_envelope(incomplete) is None


def test_owner_envelope_accepts_bounded_distinct_owner_device_recipients():
    # One wrap entry per enrolled owner device: the sealing device first,
    # companion devices (e.g. the mobile node) after it.
    envelope = _envelope()
    phone = dict(envelope["recipients"][0])
    phone["kid"] = _b64url(hashlib.sha256(b"phone-owner-key").digest())
    envelope["recipients"].append(phone)
    assert security.validate_owner_envelope(envelope) == envelope
    assert security.owner_envelope_key_id(envelope) == (
        envelope["recipients"][0]["kid"])
    assert security.owner_envelope_key_ids(envelope) == [
        envelope["recipients"][0]["kid"],
        phone["kid"],
    ]

    crowded = _envelope()
    for index in range(security.MAX_OWNER_ENVELOPE_RECIPIENTS):
        extra = dict(crowded["recipients"][0])
        extra["kid"] = _b64url(
            hashlib.sha256(b"device-%d" % index).digest())
        crowded["recipients"].append(extra)
    assert len(crowded["recipients"]) == (
        security.MAX_OWNER_ENVELOPE_RECIPIENTS + 1)
    assert security.validate_owner_envelope(crowded) is None
    assert security.owner_envelope_key_ids(crowded) == []

    tampered = _envelope()
    second = dict(tampered["recipients"][0])
    second["kid"] = _b64url(hashlib.sha256(b"phone-owner-key").digest())
    second["mlkem768"] = _b64url(b"m" * 1087)
    tampered["recipients"].append(second)
    assert security.validate_owner_envelope(tampered) is None


def test_schema_and_worker_keep_sensitive_planes_out_of_generic_admin():
    schema = (SRC / "schema.py").read_text(encoding="utf-8")
    # The generic admin table plane lives in the on-demand admin console
    # module, outside Worker startup.
    worker = (
        WORKER_SOURCE + "\n"
        + (SRC / "admin_console.py").read_text(encoding="utf-8")
    )
    migration = (
        ROOT / "migrations" / "0043_security_control_plane.sql"
    ).read_text(encoding="utf-8")
    for table in (
        "role_grants", "sensitive_audit_log", "owner_encryption_keys",
        "repo_privacy_policy", "chain_intents", "pending_rewards",
    ):
        assert f"CREATE TABLE IF NOT EXISTS {table}" in schema
        assert f"CREATE TABLE IF NOT EXISTS {table}" in migration
        assert f'"{table}"' in worker
    for removed in (
        "security_signals", "security_restrictions", "security_appeals",
    ):
        assert f'"{removed}"' not in worker
    assert "allow_admin=False" in worker


def test_wallet_signing_is_isolated_from_generic_worker_flows():
    # The element store route moved out of entry.py to keep the Worker's
    # Python startup source under its budget; the isolation contract still
    # covers it, so every source it spans is read together.
    worker = (
        WORKER_SOURCE
        + (WORLD_SRC / "world_routes.py").read_text(encoding="utf-8")
    )
    solana = (SRC / "solana.py").read_text(encoding="utf-8")
    import_block = worker[
        worker.index('_solana = _LazyModule("solana")'):
        worker.index("MAX_ROOM_NAME =", worker.index('_solana ='))
    ]
    for forbidden in (
        "_solana_sign_message", "_solana_transfer_message",
        "_solana_send_transaction", "_solana_latest_blockhash", "_shortvec",
    ):
        assert forbidden not in import_block
    for helper in ("_solana_sign_transfers", "_solana_broadcast_raw"):
        assert f"def {helper}" not in worker
    for helper in (
        "_solana_send_transaction", "_solana_sign_message",
        "_solana_transfer_message",
    ):
        assert f"def {helper}" not in solana

    tree = ast.parse(worker, filename=str(SRC / "entry.py"))
    callers = {}
    for node in tree.body:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        for call in ast.walk(node):
            if not isinstance(call, ast.Call) or not isinstance(
                call.func, ast.Name
            ):
                continue
            if call.func.id in {
                "_solana_send_transfers", "_new_solana_keypair"
            }:
                callers.setdefault(call.func.id, set()).add(node.name)
    assert callers == {
        "_solana_send_transfers": {"_sweep_element_deposit"},
        "_new_solana_keypair": {"world_element_store_handler"},
    }
    purchase = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "world_element_store_handler"
    )
    purchase_source = ast.get_source_segment(worker, purchase)
    assert "deposits_enabled = _custodial_deposits_enabled(env)" in purchase_source
    assert 'if method == "deposit" and not deposits_enabled:' in purchase_source


def test_private_profiles_and_repo_pages_revoke_cached_public_metadata():
    worker = WORKER_SOURCE
    account_users = (SRC / "account_users.py").read_text(encoding="utf-8")
    lookup_start = worker.index("match = ACCOUNTS_RE.match(url.path)")
    lookup_end = worker.index(
        'return json_response({"error": "not_found"}', lookup_start)
    lookup = worker[lookup_start:lookup_end]
    assert lookup.index("profile_private = bool(rec.get") < lookup.index(
        "cached = await _account_lookup_cached(")
    lookup_cached = account_users[
        account_users.index("async def _account_lookup_cached")
        :account_users.index("async def _account_lookup_refresh")
    ]
    assert "await edge_cache_match(key)" in lookup_cached
    assert "await _account_session_record(env, request)" in lookup
    assert '"profilePrivate": True' in lookup
    assert '"nodes": _owned_nodes(rec)' in lookup

    directory = account_users[
        account_users.index("async def _account_users_directory"):
    ]
    assert "if not _is_public_roster_member(rec):" in directory
    roster_member = worker[
        worker.index("def _is_public_roster_member")
        :worker.index("def _account_has_legacy_wallet_key")
    ]
    assert 'and not rec.get("profile_private")' in roster_member

    profile_page = worker[
        worker.index("async def _serve_profile_page")
        :worker.index("async def _serve_repo_page")
    ]
    assert profile_page.index('rec.get("profile_private")') < (
        profile_page.index("cached = await edge_cache_match(cache_key)"))


def test_error_logs_redact_browser_repository_routes_too():
    worker = WORKER_SOURCE
    body = worker[
        worker.index("async def _privacy_safe_log_path")
        :worker.index("async def capture_worker_exception")
    ]
    assert "looks_like_repo_route(path)" in body
    # The redacted bucket keeps an identity-free route-family suffix
    # ([page] / [git-*] / [api:<fixed-route-name>]) so recurring failures can
    # be told apart by endpoint without re-exposing owner/repo.
    assert '"/private-or-unpublished-repository/"' in body
    assert "_privacy_redacted_route_kind(path)" in body
