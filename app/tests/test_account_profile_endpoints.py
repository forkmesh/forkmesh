#!/usr/bin/env python3
"""Account signup/profile endpoint contracts.

These are source-level contract checks because src/entry.py depends on the
Workers Python JS runtime. They keep the product decision explicit: signup is a
simple email/password/name flow, and Solana payout address management lives in
the dashboard profile flow after account creation.
"""
import ast
import re
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ACCOUNT_USERS_TEXT = (ENTRY.parent / "account_users.py").read_text(
    encoding="utf-8")
PASSWORD_RESET_TEXT = (ENTRY.parent / "password_reset.py").read_text(
    encoding="utf-8")
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
)


def _module_function_source(text, name):
    """Exactly one top-level def's source, so a slice can't run past it."""
    for node in ast.parse(text).body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name == name):
            return ast.get_source_segment(text, node)
    raise AssertionError(name)


URLS = ENTRY.parent / "urls.py"
URLS_TEXT = URLS.read_text(encoding="utf-8")
SCHEMA = ENTRY.parent / "schema.py"
SCHEMA_TEXT = SCHEMA.read_text(encoding="utf-8")
ROUTE_HANDLERS_TEXT = (ENTRY.parent / "route_handlers.py").read_text(
    encoding="utf-8")
PROFILE_FOLLOW_MIGRATION = (
    Path(__file__).resolve().parents[1] / "migrations" / "0027_profile_follows.sql"
)
# MainWindow.cpp is split into feature TUs (MainWindow*.cpp); scan them all.
QT_SRC = Path(__file__).resolve().parents[2] / "desktop" / "src"
QT_TEXT = "\n".join(
    p.read_text(encoding="utf-8") for p in sorted(QT_SRC.glob("MainWindow*.cpp"))
)


def test_worker_routes_public_profile_contributions_before_account_lookup():
    assert "ACCOUNT_CONTRIBUTIONS_RE" in URLS_TEXT
    assert '"ACCOUNT_CONTRIBUTIONS_RE"' in ENTRY_TEXT
    routes = ENTRY_TEXT[ENTRY_TEXT.index("async def accounts_handler"):]
    assert routes.index("ACCOUNT_CONTRIBUTIONS_RE.match(url.path)") < routes.index(
        "ACCOUNTS_RE.match(url.path)"
    )
    assert "return await _contribution_profile_api(" in routes


def test_public_profile_contribution_gate_requires_active_nonprivate_profile():
    start = ENTRY_TEXT.index("async def _contribution_profile_api")
    body = ENTRY_TEXT[start:ENTRY_TEXT.index("async def ", start + 10)]
    assert 'rec.get("status") != "active"' in body
    assert 'rec.get("profile_private")' in body
    assert '{"error": "not_found"}' in body


def test_worker_exposes_simple_signup_endpoint():
    assert 'url.path == "/api/accounts/signup" and method == "POST"' in ENTRY_TEXT
    assert "async def _account_signup" in ENTRY_TEXT
    assert 'data.get("nodeName", "")' in ENTRY_TEXT
    assert 'data.get(email_field_name, "")' in ENTRY_TEXT
    assert 'data.get("email", ""), 254).strip()' in ENTRY_TEXT
    assert 'data.get("password", "")' in ENTRY_TEXT


def test_signup_endpoint_is_not_the_solana_payment_flow():
    signup_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup"):
        ENTRY_TEXT.index("async def _account_reserve")
    ]
    assert "donation_address" not in signup_body
    assert "donation_required_lamports" not in signup_body
    assert "SOLANA_RE" not in signup_body
    assert 'rec["status"] = "active"' in signup_body
    assert 'rec["email_verified"] = bool(rec.get("email_verified", False))' in signup_body


def _valid_account_email():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    wanted = {"ACCOUNT_EMAIL_LOCAL_RE", "DOMAIN_NAME_RE"}
    selected = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in wanted
                for target in node.targets):
            selected.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name == "valid_account_email":
            selected.append(node)
    namespace = {"re": re}
    exec(compile(ast.Module(selected, type_ignores=[]), str(ENTRY), "exec"), namespace)
    return namespace["valid_account_email"]


def test_signup_email_validation_requires_well_formed_domain_and_tld():
    valid_email = _valid_account_email()

    for email in (
        "person@example.com",
        "first.last+tag@sub-domain.example.org",
        "USER@EXAMPLE.CO",
    ):
        assert valid_email(email), email

    for email in (
        "user@example.com9089",
        "person@example",
        "person@example.c",
        "person@example.123",
        ".person@example.com",
        "person.@example.com",
        "first..last@example.com",
        "person@-example.com",
        "person@example-.com",
        "person@example..com",
    ):
        assert not valid_email(email), email

    signup_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_signup"):
        ENTRY_TEXT.index("async def _account_reserve")
    ]
    assert "if not valid_account_email(email):" in signup_body
    assert "valid_email_required" in signup_body


def test_worker_exposes_password_authenticated_profile_endpoint_for_wallet_and_verification():
    assert 'url.path == "/api/accounts/profile" and method == "POST"' in ENTRY_TEXT
    assert "async def _account_profile" in ENTRY_TEXT
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _login_locked_until")
    ]
    assert 'data.get("password", "")' in profile_body
    assert 'verify_password(password' in profile_body
    assert 'data.get("solana", "")' in profile_body
    assert 'resendVerification' in profile_body
    assert '_send_verification_email' in profile_body


def test_worker_profile_contract_includes_avatar_updates():
    public_payload_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_public_payload"):
        ENTRY_TEXT.index("def _donation_expiry_fields")
    ]
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _login_locked_until")
    ]
    heartbeat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_heartbeat"):
        ENTRY_TEXT.index("async def _account_treasury_address")
    ]
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]

    assert '"avatarPng": rec.get("avatar_png", "")' in public_payload_body
    assert '"avatarUpdatedAt": rec.get("avatar_updated_at", 0)' in public_payload_body
    assert '"createdAt": rec.get("created_at", 0)' in public_payload_body
    assert 'data.get("avatarPng", "")' in profile_body
    assert 'rec["avatar_png"] = avatar_png' in profile_body
    assert 'data.get("avatarPng", "")' in heartbeat_body
    assert 'rec["avatar_png"] = avatar_png' in heartbeat_body
    assert '"avatarPng": rec.get("avatar_png", "")' in public_lookup_body
    assert '"solana": ((rec.get("solana") or "").strip()' in public_lookup_body
    assert '"hasPayoutAddress": bool(' in public_lookup_body


def test_public_lookup_reads_only_users_nodes_tables():
    # Users and nodes are the authoritative tables (the legacy accounts table
    # was dropped by migration 0042), so the public /api/accounts/<name>
    # profile lookup resolves solely from them. Signed desktop reservations
    # live in nodes, so availability checks still see a reserved name as taken.
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]
    assert "_account_identity_rec_by_bi(env, name_bi)" in public_lookup_body
    assert "FROM accounts" not in public_lookup_body


def test_worker_profile_contract_includes_bio_links_mastodon_and_privacy():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]

    assert "MAX_PROFILE_BIO = 500" in ENTRY_TEXT
    assert "MAX_PROFILE_LINKS = 4" in ENTRY_TEXT
    assert 'PROFILE_TXT_PREFIX = "forkmesh-profile="' in ENTRY_TEXT
    assert '"profileBio": bio' in ENTRY_TEXT
    assert '"profilePrivate": bool(rec.get("profile_private"))' in ENTRY_TEXT
    assert '"mastodon": mastodon' in ENTRY_TEXT
    assert '"profileLinks": _profile_links_public(rec)' in ENTRY_TEXT
    assert '"profileBio" in data' in profile_body
    assert '"profilePrivate" in data' in profile_body
    assert '"mastodon" in data' in profile_body
    assert '"profileLinks" in data' in profile_body
    assert "cloudflare-dns.com/dns-query" in ENTRY_TEXT
    assert "**_account_profile_fields(rec)" in public_lookup_body


def test_worker_profile_contract_includes_editable_profile_readme():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]

    assert "MAX_PROFILE_README = 32000" in ENTRY_TEXT
    assert '"profileReadme": readme' in ENTRY_TEXT
    assert '"profileReadme" in data' in profile_body
    assert 'rec["profile_readme"] = readme' in profile_body
    assert "**_account_profile_fields(rec)" in public_lookup_body


def test_worker_profile_contract_includes_about_location_timezone_and_counts():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    public_payload_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_public_payload"):
        ENTRY_TEXT.index("def _account_chat_user_payload")
    ]
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]

    assert "MAX_PROFILE_ABOUT = 32000" in ENTRY_TEXT
    assert "MAX_PROFILE_LOCATION = 120" in ENTRY_TEXT
    assert "MAX_PROFILE_TIMEZONE = 80" in ENTRY_TEXT
    assert '"profileAbout": about' in ENTRY_TEXT
    assert '"profileLocation": location' in ENTRY_TEXT
    assert '"profileTimezone": timezone' in ENTRY_TEXT
    assert '"profileAbout" in data' in profile_body
    assert '"profileLocation" in data' in profile_body
    assert '"profileTimezone" in data' in profile_body
    assert 'rec["profile_about"] = about' in profile_body
    assert 'rec["profile_location"] = location' in profile_body
    assert 'rec["profile_timezone"] = timezone' in profile_body
    assert 'await _account_social_counts(env, name)' in public_payload_body
    # The public lookup passes the optional ?viewer= through so the /@name
    # page can show the caller's own Follow/Following state (display-only).
    assert '_account_social_counts(' in public_lookup_body
    assert 'env, rec.get("name", name), viewer, rec)' in public_lookup_body
    assert '"viewer"' in public_lookup_body


def test_worker_public_email_is_opt_in_and_verification_gated():
    """Issue #605: the Public email dropdown needs a field to read and write.

    Publishing is a stored opt-in that only ever resolves to the account's own
    confirmed address, so the settings dropdown can offer it and the /@name
    payload can carry it without any other address ever leaking.
    """
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    public_email_body = ENTRY_TEXT[
        ENTRY_TEXT.index("def _account_public_email"):
        ENTRY_TEXT.index("def _account_profile_fields")
    ]
    public_lookup_start = ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)")
    public_lookup_body = ENTRY_TEXT[
        public_lookup_start:
        ENTRY_TEXT.index('return json_response({"error": "not_found"}', public_lookup_start)
    ]

    # Both halves of the gate: an account that never opted in, and one whose
    # address is not (or no longer) confirmed, publish nothing.
    assert ('if not rec.get("profile_email_public") or not '
            'rec.get("email_verified"):' in public_email_body)
    assert 'return ""' in public_email_body
    assert '"publicEmail": _account_public_email(rec)' in ENTRY_TEXT
    # The /@name lookup ships the profile fields, so an opted-in address is
    # what a visitor sees.
    assert "**_account_profile_fields(rec)" in public_lookup_body

    # Writes: the session-authed save must stay on the session path, and only
    # this account's own confirmed address may be selected.
    assert '"followersPublic", "publicEmail", "mastodon",' in profile_body
    assert '"publicEmail" in data' in profile_body
    assert ('if wanted and (\n'
            '                wanted != account_email or not '
            'rec.get("email_verified")):' in profile_body)
    assert '{"error": "bad_public_email"}' in profile_body
    assert 'rec["profile_email_public"] = True' in profile_body
    assert 'rec.pop("profile_email_public", None)' in profile_body


def test_account_finalize_email_swap_drops_the_public_email_opt_in():
    # Consent was given for one address; finalizing with a different one must
    # not silently publish the new mailbox.
    finalize = ROUTE_HANDLERS_TEXT[
        ROUTE_HANDLERS_TEXT.index("async def _account_finalize(env, request):"):
    ]
    body = finalize[
        finalize.index("        salt, phash = await hash_password(password)"):
        finalize.index('    rec["kind"] = "node"')
    ]
    assert 'rec.pop("profile_email_public", None)' in body
    assert body.index('rec.pop("profile_email_public", None)') < body.index(
        'rec["email"] = email')


def test_worker_profile_follow_schema_routes_and_cleanup_exist():
    assert "CREATE TABLE IF NOT EXISTS profile_follows" in SCHEMA_TEXT
    assert "follower_bi TEXT NOT NULL" in SCHEMA_TEXT
    assert "target_bi TEXT NOT NULL" in SCHEMA_TEXT
    assert "PRIMARY KEY (follower_bi, target_bi)" in SCHEMA_TEXT
    assert "CREATE INDEX IF NOT EXISTS idx_profile_follows_target" in SCHEMA_TEXT
    assert PROFILE_FOLLOW_MIGRATION.is_file()
    migration = PROFILE_FOLLOW_MIGRATION.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS profile_follows" in migration
    assert "CREATE INDEX IF NOT EXISTS idx_profile_follows_target" in migration

    assert "ACCOUNT_FOLLOW_RE" in ENTRY_TEXT
    assert "async def _account_follow" in ENTRY_TEXT
    assert 'ACCOUNT_FOLLOW_RE.match(url.path)' in ENTRY_TEXT
    assert 'method in ("POST", "DELETE")' in ENTRY_TEXT
    assert 'return await _account_follow(env, request, follow_match.group(1), method)' in ENTRY_TEXT
    assert 'DELETE FROM profile_follows WHERE follower_bi=? OR target_bi=?' in ENTRY_TEXT


def test_worker_profile_public_edits_and_follows_accept_signed_session_token():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _account_claim_node")
    ]
    follow_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_follow"):
        ENTRY_TEXT.index("async def _account_reserve")
    ]
    login_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_login"):
        ENTRY_TEXT.index("async def _account_logout")
    ]

    assert "async def _account_session_token(env, name" in ENTRY_TEXT
    assert ("async def _account_session_record(env, request, data=None,\n"
            "                                  allow_unverified=False)") in ENTRY_TEXT
    assert "else await _account_session_token(" in ENTRY_TEXT
    assert ("env, name, session_device_label, session_client_ip,\n"
            "                session_device_bi)") in ENTRY_TEXT
    assert "_account_session_response(env, request, payload)" in login_body
    assert "_account_session_cookie(payload[\"sessionToken\"])" in ENTRY_TEXT
    assert "account_sessions" in ENTRY_TEXT
    # The profile POST owns the two moves an unverified account keeps (delete
    # itself, ask for the link again) and gates its own fields, so it is the
    # one profile path that opts out of the session-level gate (#592).
    assert ('await _account_session_record(\n'
            '        env, request, data, allow_unverified=True)') in profile_body
    assert 'await _account_session_record(env, request, data)' in follow_body
    public_edit_block = profile_body[
        profile_body.index('if "profileBio" in data:'):
        profile_body.index('if "emailNotifications" in data:')
    ]
    assert "verify_password" not in public_edit_block
    assert 'data.get("password", "")' not in public_edit_block


def test_public_profile_fields_whitelist_allows_the_always_sent_password_key():
    # The dashboard's profilePayload() always includes a `password` field, even
    # on pages (like the public-profile editor) that have no password input and
    # send it empty. If `password` is missing from public_profile_fields the
    # session-authed save falls through to the credentials branch and 401s with
    # invalid_credentials no matter how recently the user signed in (adhoc #49).
    whitelist_block = ENTRY_TEXT[
        ENTRY_TEXT.index("public_profile_fields = {"):
        ENTRY_TEXT.index("session_bi, session_rec = await _account_session_record")
    ]
    assert '"password"' in whitelist_block


def test_worker_public_profile_page_wires_follow_and_public_mode():
    # /@name now serves the FULL dashboard profile document; the follow
    # control and foreign-profile rendering live in the dashboard bundle.
    dashboard_js = (
        Path(__file__).resolve().parents[1] / "public" / "dashboard.js"
    ).read_text(encoding="utf-8")
    assert "function publicProfileNameFromPath()" in dashboard_js
    assert "function profileSubject()" in dashboard_js
    assert "state.publicProfile" in dashboard_js
    assert "data-profile-follow" in dashboard_js
    assert '"/api/accounts/" + encodeURIComponent(name) + "/follow"' in dashboard_js
    # Public mode never shows a mailbox for a foreign account.
    assert 'profile.email = "@" + userDisplayName(profile.nodeName)' in dashboard_js


def test_public_profile_page_injects_verified_link_tags():
    # _serve_profile_page injects the per-user head tags into the shared
    # prebuilt profile document: canonical + the reciprocal rel="me" (the
    # verified half of the fediverse actor's profile link) + the ActivityPub
    # alternate for discovery, and swaps the <title>.
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _serve_profile_page"):
        ENTRY_TEXT.index("async def _serve_repo_page")
    ]
    assert '"/dashboard/profile/repositories" if repositories' in body
    assert 'DASHBOARD_PAGE_ASSETS' in body
    assert '<link rel=\\"canonical\\" href=\\"%s\\">' in body
    assert '<link rel=\\"me\\" href=\\"%s\\">' in body
    assert 'application/activity+json' in body
    assert '/ap/users/%s' in body
    assert '"<title>@%s · %s</title>" % (' in body
    assert "instance_features.display_name(self.env)" in body
    assert "origin = _ap_origin(self.env)" in body
    # Eligibility mirrors the fediverse actor: active + not private (node
    # accounts included, matching _ap_user_federates).
    assert 'rec.get("status") != "active"' in body
    assert 'rec.get("profile_private")' in body
    assert '_serve_not_found_page(url)' in body


def test_worker_serves_public_at_profiles_and_private_profiles_404():
    assert "async def _serve_profile_page" in ENTRY_TEXT
    route_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _route"):
        ENTRY_TEXT.index("issues_match = REPO_ISSUES_RE.match", ENTRY_TEXT.index("async def _route"))
    ]

    assert 'r"^/@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)(/repositories)?/?$"' in route_body
    # The route matches the percent-decoded, case-folded path: pasted links
    # often arrive as /%40name (an encoded @) or /@Name, and both used to fall
    # through to the 404 page instead of the profile.
    assert "unquote(url.path).lower()" in route_body
    assert "_serve_profile_page(" in route_body
    assert "_wants_activity_json(" in route_body
    assert "ap.wants_activity_json(" not in route_body
    assert 'str(url.hostname or "").lower() == "app.forkmesh.com"' in route_body
    assert '"https://forkmesh.com/@" + quote(' in route_body
    assert '"x-forkmesh-public-profile-proxy"' in route_body
    assert 'bool(rec.get("profile_private"))' in ENTRY_TEXT
    assert 'return json_response({"error": "not_found"}, status=404)' in ENTRY_TEXT
    assert "cache-control" in ENTRY_TEXT


def test_public_profile_html_route_does_not_cold_load_federation_handlers():
    """Ordinary profile HTML must fit within a cold Worker's CPU allowance."""
    worker = (Path(__file__).resolve().parents[1] / "src" / "entry.py").read_text(
        encoding="utf-8"
    )
    assert "def _ap_origin(env, request=None):" in worker
    assert "def _wants_activity_json(accept_header):" in worker
    assert '_fediverse_routes.export("_ap_origin")' not in worker
    assert '_fediverse_routes.export("_ap_prune_actor_inventory")' not in worker

    schema_fast_path = worker[
        worker.index("async def _apply_schema"):
        worker.index("def js_nullish")
    ]
    assert "_ap_prune_actor_inventory" not in schema_fast_path


def test_worker_exposes_public_user_directory_for_chat_without_private_fields():
    assert 'url.path == "/api/accounts/users" and method == "GET"' in ENTRY_TEXT
    assert '_account_users = _LazyModule("account_users")' in ENTRY_TEXT
    assert "async def _account_users_directory" in ACCOUNT_USERS_TEXT
    public_helpers = ENTRY_TEXT[
        ENTRY_TEXT.index("def _account_chat_user_payload"):
        ENTRY_TEXT.index("_credited_referrers_by_user =")
    ]
    body = public_helpers + ACCOUNT_USERS_TEXT

    assert '"LEFT JOIN world_user_activity a ON a.account_bi=u.user_bi "' in body
    assert '"ORDER BY u.username COLLATE NOCASE LIMIT ?"' in body
    # The campfire seats members in this array's order, one bench per
    # account, so the response is resorted by join date (oldest first)
    # rather than left in the query's alphabetical fetch order.
    assert 'out.sort(key=lambda user: user.get("createdAt", 0))' in body
    assert "last_touch_at" not in body
    assert "FROM accounts" not in body
    # The user/active/non-private roster predicate is shared with the chat
    # badge's member count (adhoc #1617), so it lives in one place rather
    # than being inlined here.
    assert "_is_public_roster_member(rec)" in body
    assert '_account_kind(rec) == "user"' in ENTRY_TEXT
    assert 'rec.get("status") == "active"' in ENTRY_TEXT
    assert '"avatarPng": rec.get("avatar_png", "")' in body
    assert '"nodes": _owned_nodes(rec)' in body
    assert 'solana = (rec.get("solana") or "").strip()' in body
    assert '"solana": solana if SOLANA_RE.match(solana) else ""' in body
    assert 'user_payload["referredBy"]' in body
    assert "_credited_referrers_by_user(env)" in body
    public_payload = public_helpers + ACCOUNT_USERS_TEXT[
        ACCOUNT_USERS_TEXT.index("async def _credited_referrers_by_user"):
        ACCOUNT_USERS_TEXT.index("async def _account_users_admin_actor")
    ]
    assert "signup_ip_bi" not in public_payload
    assert "user_agent_bi" not in public_payload
    assert "risk_flags" not in public_payload
    assert '"email"' not in public_payload
    # Sensitive fields live only behind the explicit is_admin gate and are
    # excluded from the shared public cache.
    assert "_account_users_admin_actor" in body
    assert "if admin_actor:" in body
    assert '"email": clean_string(rec.get("email", ""), 254)' in body
    assert '"pubkey"' not in body
    public_response = ACCOUNT_USERS_TEXT[
        ACCOUNT_USERS_TEXT.index("if not admin_actor:"):
        ACCOUNT_USERS_TEXT.index("if admin_actor:")
    ]
    assert '"isAdmin"' not in public_response
    assert '"isAdmin"' in ACCOUNT_USERS_TEXT
    assert '"pass_hash"' not in body


def test_desktop_publishes_user_chat_avatar_to_peers():
    # The desktop's avatar reaches peers through the chat backend broadcast
    # (the signed heartbeat carries no avatar; the worker-side avatarPng comes
    # from the dashboard profile flow, pinned above). Chat uses the user avatar
    # so messages do not show up as the node profile.
    assert "updateChatIdentity();" in QT_TEXT
    assert "m_backend->setAvatar(avatar);" in QT_TEXT
    assert "effectiveUserAvatar();" in QT_TEXT

    heartbeat_body = QT_TEXT[
        QT_TEXT.index("void MainWindow::sendNodeHeartbeat()"):
        QT_TEXT.index("void MainWindow::showNodeClaimCode")
    ]
    # The per-minute heartbeat stays lean: name + solana + ts + sig only, no
    # avatar re-upload every beat.
    assert '{"nodeName", name}, {"solana", solana}, {"ts", ts}' in heartbeat_body
    assert "avatarPng" not in heartbeat_body


def test_profile_endpoint_supports_verified_node_rename_and_hard_delete():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("async def _login_locked_until")
    ]
    login_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_login"):
        ENTRY_TEXT.index("def _random_bytes")
    ]

    assert 'data.get("newNodeName", "")' in profile_body
    assert 'data.get("deleteAccount")' in profile_body
    assert 'email_not_verified' in profile_body
    assert 'node_name_taken' in profile_body
    assert '_rename_account_namespace(env, name_bi, rec, new_name)' in profile_body
    assert '_delete_account_namespace(env, name_bi, rec)' in profile_body
    assert '"accountDeleted": True' in profile_body
    assert '"account_disabled"' in login_body


def test_public_account_lookup_reports_email_verified_status():
    # issue #320: the dashboard's profile refresh path reuses this same public
    # GET lookup, so it must reflect a verification that happened in another tab
    # or "verify your email" never clears.
    lookup_body = ENTRY_TEXT[
        ENTRY_TEXT.index("match = ACCOUNTS_RE.match(url.path)"):
        ENTRY_TEXT.index("# --- Repository submission inboxes")
    ]
    assert '"emailVerified": bool(rec.get("email_verified"))' in lookup_body
    assert '"email":' not in lookup_body


def test_hard_delete_removes_account_identity_and_owned_namespace_state():
    assert "async def _delete_account_namespace" in ENTRY_TEXT
    assert "async def _delete_repo_namespace" in ENTRY_TEXT
    delete_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _delete_bounties_namespace"):
        ENTRY_TEXT.index("async def _account_public_payload")
    ]

    for required in (
        "SELECT key_bi, data FROM repositories WHERE owner_bi=?",
        "DELETE FROM repo_shares WHERE repo_bi=?",
        "DELETE FROM repo_shares WHERE grantee_bi=?",
        "DELETE FROM issue_inbox WHERE repo_bi=?",
        "DELETE FROM pull_inbox WHERE repo_bi=?",
        "DELETE FROM discussion_inbox WHERE repo_bi=?",
        "DELETE FROM host_presence WHERE repo_bi=?",
        "DELETE FROM clone_rr WHERE repo_bi=?",
        "DELETE FROM repo_first_hosted WHERE repo_bi=?",
        "DELETE FROM mirror_repository_integrity WHERE repo_bi=?",
        "DELETE FROM issue_bounty WHERE bounty_bi=?",
        "DELETE FROM chat_history WHERE room_key LIKE ?",
        "DELETE FROM funds_received WHERE scope='project' AND key=?",
        "DELETE FROM catalog_rate WHERE owner_bi=?",
        "DELETE FROM account_presence WHERE name_bi=?",
        "DELETE FROM pending_verifications WHERE name_bi=?",
        "DELETE FROM notifications WHERE recipient_bi=?",
        "DELETE FROM login_attempts WHERE id_bi=?",
        "_delete_chat_channel_memberships(env, name_bi)",
        "DELETE FROM users WHERE user_bi=?",
        "DELETE FROM nodes WHERE node_bi=?",
        "purge_catalog_related_caches()",
    ):
        assert required in delete_body


def test_namespace_rename_moves_account_repo_and_repo_scoped_state():
    assert "async def _rename_account_namespace" in ENTRY_TEXT
    assert "async def _move_repo_namespace" in ENTRY_TEXT
    assert "async def _save_account_full" in ENTRY_TEXT

    save_full_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _save_account_full"):
        ENTRY_TEXT.index("async def _move_repo_shares")
    ]
    assert "_mirror_account_identity_tables" in save_full_body
    shares_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _move_repo_shares"):
        ENTRY_TEXT.index("async def _move_bounties_namespace")
    ]
    assert "SELECT grantee_bi, data, ts FROM repo_shares WHERE repo_bi=?" in shares_body
    bounty_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _move_bounties_namespace"):
        ENTRY_TEXT.index("async def _move_chat_history_namespace")
    ]
    assert "SELECT bounty_bi, data FROM issue_bounty" in bounty_body
    chat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _move_chat_history_namespace"):
        ENTRY_TEXT.index("async def _move_repo_namespace")
    ]
    assert "SELECT room_key, msg_id, ts, body FROM chat_history WHERE room_key LIKE ?" in chat_body

    rename_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _rename_account_namespace"):
        ENTRY_TEXT.index("async def _account_profile")
    ]

    for required in (
        "SELECT data, email_bi, ip_bi, is_admin FROM users WHERE user_bi=?",
        '("DELETE FROM users WHERE user_bi=?", (new_name_bi,))',
        '"UPDATE users SET user_bi=?,data=?,username=? WHERE user_bi=?"',
        "await _contribution_run_batch(env, [",
        "DELETE FROM users WHERE user_bi=?",
        "UPDATE account_presence SET name_bi=? WHERE name_bi=?",
        "_move_chat_channel_memberships(",
        "UPDATE pending_verifications SET name_bi=? WHERE name_bi=?",
        "UPDATE notifications SET recipient_bi=? WHERE recipient_bi=?",
        "UPDATE catalog_rate SET owner_bi=? WHERE owner_bi=?",
        "purge_catalog_related_caches()",
    ):
        assert required in rename_body

    # The source email remains unique during a rename only when the existing
    # row is re-keyed in place; inserting a second user first collides with the
    # partial unique index before the old row can be removed.
    rekey = rename_body.index(
        '"UPDATE users SET user_bi=?,data=?,username=? WHERE user_bi=?"')
    mirror = rename_body.index("await _save_account_full(")
    assert rekey < mirror

    repo_move_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _move_repo_namespace"):
        ENTRY_TEXT.index("async def _rename_account_namespace")
    ]

    for required in (
        "SELECT key_bi, data, is_private FROM repositories WHERE owner_bi=?",
        'rec["owner"] = new_owner',
        "UPDATE issue_inbox SET repo_bi=? WHERE repo_bi=?",
        "UPDATE pull_inbox SET repo_bi=? WHERE repo_bi=?",
        "UPDATE discussion_inbox SET repo_bi=? WHERE repo_bi=?",
        "UPDATE host_presence SET repo_bi=? WHERE repo_bi=?",
        "UPDATE clone_rr SET repo_bi=? WHERE repo_bi=?",
        "UPDATE repo_first_hosted SET repo_bi=? WHERE repo_bi=?",
        "UPDATE funds_received SET key=?, name=? WHERE scope='project' AND key=?",
    ):
        assert required in repo_move_body


def test_worker_exposes_password_reset_flow():
    # Both halves of the emailed password-reset flow are routed and implemented.
    assert 'url.path == "/api/accounts/forgot-password" and method == "POST"' in ENTRY_TEXT
    assert 'url.path == "/api/accounts/reset-password" and method == "POST"' in ENTRY_TEXT
    assert "async def _account_forgot_password" in PASSWORD_RESET_TEXT
    assert "async def _account_reset_password" in ENTRY_TEXT
    assert "async def _password_reset_token" in ENTRY_TEXT
    assert "async def _send_password_reset_email" in ENTRY_TEXT


def test_forgot_password_does_not_leak_account_existence():
    # Sliced by AST, not to end-of-file: password_reset.py also holds the
    # authenticated change-password handler, which legitimately answers
    # invalid_credentials to a caller whose session already names them.
    forgot_body = _module_function_source(
        PASSWORD_RESET_TEXT, "_account_forgot_password")
    # Look up by email OR node name, but always answer {"ok": True} so the
    # endpoint can't enumerate which accounts exist.
    assert 'data.get("identifier", "")' in forgot_body
    assert "_send_password_reset_email" in forgot_body
    assert '{"ok": True}' in forgot_body
    assert "_password_reset_requester_allowed" in forgot_body
    assert "_password_reset_claim_account" in forgot_body
    assert "invalid_credentials" not in forgot_body
    assert "no_such_account" not in forgot_body


def test_reset_password_validates_token_expiry_and_hashes_new_password():
    reset_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_reset_password"):
        ENTRY_TEXT.index("def _verify_email_page")
    ]
    # Expiry is enforced, the token is recomputed from the stored pass_hash
    # (single-use), a short password is rejected, and the new one is PBKDF2-hashed.
    assert "reset_link_expired" in reset_body
    assert "password_too_short" in reset_body
    assert "invalid_reset_token" in reset_body
    assert "hmac.compare_digest(token, expected)" in reset_body
    assert "hash_password(password)" in reset_body
    assert 'rec["pass_hash"] = phash' in reset_body

    token_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _password_reset_token"):
        ENTRY_TEXT.index("async def _send_password_reset_email")
    ]
    # The token is domain-separated and binds the current pass_hash + expiry.
    assert "forkmesh-password-reset-v1" in token_body
    assert "pass_hash" in token_body


def test_signup_verification_email_is_a_professional_welcome_email():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _send_verification_email"):
        ENTRY_TEXT.index("async def _password_reset_token")
    ]
    for marker in (
        'subject = "Welcome to ForkMesh, " + display_name',
        '"Welcome to ForkMesh, " + display_name',
        "Confirm your email",
        "https://forkmesh.com/docs",
        "https://forkmesh.com/blogs",
        "What was confusing",
        "founders@forkmesh.com",
        "color:#4ade80",
        "/api/accounts/verify-email?node=",
        # Renders through the shared branded card, rather than a
        # bespoke inline template.
        "_forkmesh_email_card_html(",
    ):
        assert marker in body
    card_body = ENTRY_TEXT[
        ENTRY_TEXT.index("def _light_email_fragment"):
        ENTRY_TEXT.index("def _format_email_ts")
    ]
    for marker in (
        "background:#fafafa",
        "background:#171717",
        'content=\\"light\\"',
        "data-forkmesh-site-action",
    ):
        assert marker in card_body
    # One explicit light palette avoids partial dark-mode overrides that can
    # combine a light card with light text.
    assert "@media" not in card_body
    assert "background:#090909" not in card_body
    assert "background:#141416" not in card_body


def test_login_page_links_to_password_reset():
    login_html = (Path(__file__).resolve().parents[1] /
                  "public" / "login.html").read_text(encoding="utf-8")
    assert "/forgot-password" in login_html
    assert "/forgot-password.html" not in login_html
    for asset in ("forgot-password.html", "forgot-password.js",
                  "reset-password.html", "reset-password.js"):
        assert (Path(__file__).resolve().parents[1] / "public" / asset).exists()


if __name__ == "__main__":
    for test in (
        test_worker_exposes_simple_signup_endpoint,
        test_signup_endpoint_is_not_the_solana_payment_flow,
        test_worker_exposes_password_authenticated_profile_endpoint_for_wallet_and_verification,
        test_worker_profile_contract_includes_avatar_updates,
        test_desktop_publishes_effective_avatar_to_peers,
        test_profile_endpoint_supports_verified_node_rename_and_hard_delete,
        test_hard_delete_removes_account_identity_and_owned_namespace_state,
        test_namespace_rename_moves_account_repo_and_repo_scoped_state,
        test_worker_exposes_password_reset_flow,
        test_forgot_password_does_not_leak_account_existence,
        test_reset_password_validates_token_expiry_and_hashes_new_password,
        test_signup_verification_email_is_a_professional_welcome_email,
        test_login_page_links_to_password_reset,
    ):
        test()
        print("PASS", test.__name__)


def test_worker_exposes_password_reset_endpoints():
    assert 'url.path == "/api/accounts/forgot-password" and method == "POST"' in ENTRY_TEXT
    assert 'url.path == "/api/accounts/reset-password" and method == "POST"' in ENTRY_TEXT
    assert "async def _account_forgot_password" in PASSWORD_RESET_TEXT
    assert "async def _account_reset_password" in ENTRY_TEXT


def test_forgot_password_never_leaks_account_existence():
    body = _module_function_source(
        PASSWORD_RESET_TEXT, "_account_forgot_password")
    assert '{"ok": True}' in body
    assert "_password_reset_requester_allowed" in body
    assert "_password_reset_claim_account" in body
    assert "_send_password_reset_email" in body


def test_reset_password_verifies_token_and_rehashes():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_reset_password"):
        ENTRY_TEXT.index("def _verify_email_page")
    ]
    assert "_password_reset_token" in body
    assert "hmac.compare_digest" in body
    assert "hash_password(password)" in body
    assert '"password_too_short"' in body
    assert '"reset_link_expired"' in body


def test_reset_token_bound_to_current_hash_and_expiry():
    body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _password_reset_token"):
        ENTRY_TEXT.index("async def _send_password_reset_email")
    ]
    assert "forkmesh-password-reset-v1" in body
    assert "pass_hash" in body
    assert "expires" in body


def test_login_page_links_to_password_reset():
    login_html = (Path(__file__).resolve().parents[1] /
                  "public" / "login.html").read_text(encoding="utf-8")
    assert "/forgot-password" in login_html
    assert "/forgot-password.html" not in login_html
