from urllib.parse import parse_qs, urlparse

from apps_fakes import (APP, VERIFIER, World, authorize, call, code_from,
                        create_app, exchange, install)
import apps_core as core  # noqa: E402


def _dump(world):
    lines = []
    for table in ("apps", "app_tokens", "app_auth_codes", "app_installs"):
        for row in world.db.execute("SELECT * FROM %s" % table):
            lines.append(repr(tuple(row)))
    return "\n".join(lines)


def test_registration_shows_secrets_once_and_stores_them_hashed():
    world = World()
    created = create_app(world)
    assert created["clientSecret"].startswith("fmcs_")
    assert created["signingSecret"].startswith("fmss_")
    assert created["app"]["clientId"].startswith("fmapp_")
    assert created["app"]["botName"] == "Hello App (app)"
    dump = _dump(world)
    assert created["clientSecret"] not in dump
    assert created["signingSecret"] not in dump
    again = call(world, "GET", "/api/apps/" + created["app"]["id"])
    assert "clientSecret" not in str(again["data"])
    assert "fmss_" not in str(again["data"])
    rotated = call(world, "POST", "/api/apps/%s/rotate" % created["app"]["id"],
                   {"secret": "client"})
    assert rotated["data"]["clientSecret"] != created["clientSecret"]
    assert call(world, "GET", "/api/apps/" + created["app"]["id"],
                user="mallory")["status"] == 404


def test_registration_validates_urls_and_scopes():
    world = World()
    for field, value, error in (
        ("redirectUris", ["http://hello.example.org/cb"],
         "invalid_redirect_uri"),
        ("redirectUris", ["https://hello.example.org/cb#frag"],
         "invalid_redirect_uri"),
        ("redirectUris", ["https://user:pw@hello.example.org/cb"],
         "invalid_redirect_uri"),
        ("scopes", ["repos:read", "admin:everything"], "invalid_scopes"),
        ("scopes", [], "invalid_scopes"),
        ("icon", {"data": "https://tracker.example/pixel.png"},
         "invalid_icon"),
    ):
        body = dict(APP, **{field: value})
        result = call(world, "POST", "/api/apps", body)
        assert result["status"] == 400, (field, value)
        assert result["data"]["error"] == error
    no_scope = dict(APP, events=["issue.opened"],
                    eventUrl="https://hello.example.org/events",
                    scopes=["repos:read"])
    assert call(world, "POST", "/api/apps", no_scope)["data"]["error"] == \
        "event_scope_missing"
    reserved = dict(APP, commands=[{
        "command": "issue", "url": "https://hello.example.org/cmd"}])
    assert call(world, "POST", "/api/apps", reserved)["data"]["error"] == \
        "invalid_commands"


def test_authorization_code_with_pkce_issues_scoped_tokens():
    world = World()
    created = create_app(world)
    preview = call(world, "GET", "/api/apps/oauth/authorize", query={
        "response_type": "code", "client_id": created["app"]["clientId"],
        "redirect_uri": APP["redirectUris"][0], "scope": "repos:read",
        "state": "s1", "code_challenge": core.pkce_s256(VERIFIER),
        "code_challenge_method": "S256"}, user="bob")
    assert preview["status"] == 200
    assert [s["scope"] for s in preview["data"]["scopes"]] == ["repos:read"]
    assert preview["data"]["visible"] is False

    result = authorize(world, created, scope="repos:read issues:read")
    assert result["status"] == 200
    redirect = urlparse(result["data"]["redirect"])
    assert redirect.netloc == "hello.example.org"
    assert parse_qs(redirect.query)["state"] == ["xyz"]
    code = code_from(result)
    assert code not in _dump(world)

    wrong = exchange(world, created, code, verifier="w" * 50)
    assert wrong["status"] == 400
    assert wrong["data"]["error"] == "invalid_grant"

    result = authorize(world, created, scope="repos:read issues:read")
    code = code_from(result)
    tokens = exchange(world, created, code)
    assert tokens["status"] == 200, tokens["data"]
    body = tokens["data"]
    assert body["token_type"] == "Bearer"
    assert body["expires_in"] == 3600
    assert body["scope"] == "repos:read issues:read"
    assert body["install"]["target"] == "user"
    assert body["access_token"] not in _dump(world)
    assert body["refresh_token"] not in _dump(world)
    assert tokens["headers"]["pragma"] == "no-cache"

    context = __import__("asyncio").run(__import__("apps").authenticate(
        __import__("apps_fakes").Runtime(world), body["access_token"]))
    assert context["scopes"] == ("repos:read", "issues:read")
    assert context["actor"] == "app:hello-app"


def test_redirect_uri_mismatch_never_redirects():
    world = World()
    created = create_app(world)
    for uri in ("https://evil.example.org/oauth/callback",
                "https://hello.example.org/oauth/callback/",
                "https://hello.example.org/oauth/callback?x=1",
                "https://hello.example.org/oauth/CALLBACK"):
        result = authorize(world, created, redirect_uri=uri)
        assert result["status"] == 400
        assert result["data"]["error"] == "redirect_uri_mismatch"
        assert "redirect" not in result["data"]
    result = authorize(world, created)
    code = code_from(result)
    other = exchange(world, created, code,
                     redirect_uri="https://evil.example.org/oauth/callback")
    assert other["data"]["error"] == "invalid_grant"
    assert other["data"]["error_description"] == "redirect_uri_mismatch"


def test_pkce_is_required_and_plain_is_refused():
    world = World()
    created = create_app(world)
    base = {
        "response_type": "code", "client_id": created["app"]["clientId"],
        "redirect_uri": APP["redirectUris"][0], "scope": "repos:read",
        "state": "abc", "target": "user", "decision": "allow"}
    for extra in ({}, {"code_challenge": core.pkce_s256(VERIFIER),
                       "code_challenge_method": "plain"},
                  {"code_challenge": "short", "code_challenge_method": "S256"}):
        result = call(world, "POST", "/api/apps/oauth/authorize",
                      dict(base, **extra))
        assert result["data"]["error"] == "invalid_request"
        assert "error=invalid_request" in result["data"]["redirect"]
        assert "state=abc" in result["data"]["redirect"]
    too_wide = call(world, "POST", "/api/apps/oauth/authorize", dict(
        base, scope="repos:read agents:run",
        code_challenge=core.pkce_s256(VERIFIER),
        code_challenge_method="S256"))
    assert too_wide["data"]["error"] == "invalid_scope"


def test_code_is_single_use_and_replay_revokes_the_tokens():
    world = World()
    created = create_app(world)
    code = code_from(authorize(world, created))
    first = exchange(world, created, code)
    assert first["status"] == 200
    replay = exchange(world, created, code)
    assert replay["data"]["error"] == "invalid_grant"
    runtime = __import__("apps_fakes").Runtime(world)
    assert __import__("asyncio").run(__import__("apps").authenticate(
        runtime, first["data"]["access_token"])) is None


def test_client_authentication_is_required():
    world = World()
    created = create_app(world)
    code = code_from(authorize(world, created))
    bad = exchange(world, created, code, secret="fmcs_" + "0" * 64)
    assert bad["status"] == 401
    assert bad["data"]["error"] == "invalid_client"
    other = create_app(world, name="Other App",
                       redirectUris=["https://other.example.org/cb"])
    stolen = call(world, "POST", "/api/apps/oauth/token", {
        "grant_type": "authorization_code", "code": code,
        "redirect_uri": APP["redirectUris"][0],
        "client_id": other["app"]["clientId"],
        "client_secret": other["clientSecret"],
        "code_verifier": VERIFIER}, user="")
    assert stolen["data"]["error"] == "invalid_grant"


def test_refresh_tokens_rotate_and_reuse_revokes_the_family():
    world = World()
    created = create_app(world)
    tokens = install(world, created)
    refreshed = call(world, "POST", "/oauth/token".replace(
        "/oauth", "/api/apps/oauth"), {
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": created["app"]["clientId"],
        "client_secret": created["clientSecret"]}, user="")
    assert refreshed["status"] == 200, refreshed["data"]
    assert refreshed["data"]["refresh_token"] != tokens["refresh_token"]
    reuse = call(world, "POST", "/api/apps/oauth/token", {
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": created["app"]["clientId"],
        "client_secret": created["clientSecret"]}, user="")
    assert reuse["data"]["error"] == "invalid_grant"
    runtime = __import__("apps_fakes").Runtime(world)
    assert __import__("asyncio").run(__import__("apps").authenticate(
        runtime, refreshed["data"]["access_token"])) is None
    assert any(a[1] == "apps.oauth.refresh_reuse" for a in world.audits)


def test_refresh_accepts_form_encoding_and_basic_auth():
    world = World()
    created = create_app(world)
    tokens = install(world, created)
    import base64
    basic = base64.b64encode(("%s:%s" % (
        created["app"]["clientId"], created["clientSecret"])).encode()).decode()
    result = call(world, "POST", "/api/apps/oauth/token",
                  raw_form="grant_type=refresh_token&refresh_token=%s&scope="
                  "repos:read" % tokens["refresh_token"],
                  headers={"authorization": "Basic " + basic}, user="")
    assert result["status"] == 200, result["data"]
    assert result["data"]["scope"] == "repos:read"


def test_access_tokens_expire():
    world = World()
    created = create_app(world)
    tokens = install(world, created)
    import apps
    from apps_fakes import Runtime
    world.now += core.ACCESS_TTL_MS + 1
    assert __import__("asyncio").run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None


def test_revocation_endpoint_and_uninstall_revoke_everything():
    import asyncio
    import apps
    from apps_fakes import Runtime
    world = World()
    created = create_app(world)
    tokens = install(world, created)
    revoked = call(world, "POST", "/api/apps/oauth/revoke", {
        "token": tokens["access_token"],
        "client_id": created["app"]["clientId"],
        "client_secret": created["clientSecret"]}, user="")
    assert revoked["status"] == 200
    assert asyncio.run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None

    tokens = install(world, created)
    listing = call(world, "GET", "/api/apps/installed")
    installs = listing["data"]["installs"]
    assert len(installs) == 1 and installs[0]["canRevoke"]
    install_id = installs[0]["installId"]
    assert call(world, "DELETE", "/api/apps/installs/" + install_id,
                user="mallory")["status"] == 403
    removed = call(world, "DELETE", "/api/apps/installs/" + install_id)
    assert removed["status"] == 200
    assert asyncio.run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None
    refresh = call(world, "POST", "/api/apps/oauth/token", {
        "grant_type": "refresh_token",
        "refresh_token": tokens["refresh_token"],
        "client_id": created["app"]["clientId"],
        "client_secret": created["clientSecret"]}, user="")
    assert refresh["data"]["error"] == "invalid_grant"
    assert call(world, "GET", "/api/apps/installed")["data"]["installs"] == []


def test_private_apps_are_limited_to_the_developer_and_their_org():
    world = World()
    world.org("acme", alice="owner", bob="member", carol="admin")
    created = create_app(world, organization="acme")
    ok = authorize(world, created, user="bob")
    assert ok["status"] == 200
    outsider = authorize(world, created, user="mallory")
    assert outsider["data"]["error"] == "app_not_available"
    world.org("elsewhere", carol="owner")
    elsewhere = authorize(world, created, user="carol",
                          target="org:elsewhere")
    assert elsewhere["data"]["error"] == "app_not_available"


def test_org_install_needs_admin_approval():
    import asyncio
    import apps
    from apps_fakes import Runtime
    world = World()
    world.org("acme", carol="owner", bob="member")
    created = create_app(world)
    world.db.execute("UPDATE apps SET listing='listed'")
    pending = authorize(world, created, user="bob", target="org:acme",
                        scope="repos:read issues:read")
    assert pending["data"]["pending"] is True
    assert "error=access_denied" in pending["data"]["redirect"]
    assert "code=" not in pending["data"]["redirect"]
    assert any(n[0] == "carol" for n in world.notices)

    requests = call(world, "GET", "/api/apps/orgs/acme/requests", user="bob")
    assert requests["status"] == 403
    requests = call(world, "GET", "/api/apps/orgs/acme/requests",
                    user="carol")
    [request] = requests["data"]["requests"]
    assert request["requester"] == "bob"
    decided = call(world, "POST",
                   "/api/apps/orgs/acme/requests/" + request["id"],
                   {"action": "approve"}, user="carol")
    assert decided["data"]["status"] == "approved"

    wider = authorize(world, created, user="bob", target="org:acme",
                      scope="repos:read issues:read issues:write")
    assert wider["data"].get("pending") is True
    done = authorize(world, created, user="bob", target="org:acme",
                     scope="repos:read issues:read")
    tokens = exchange(world, created, code_from(done))
    assert tokens["data"]["install"]["target"] == "org"
    assert tokens["data"]["install"]["name"] == "acme"
    context = asyncio.run(apps.authenticate(
        Runtime(world), tokens["data"]["access_token"]))
    assert context["targetKind"] == "org"

    admin_view = call(world, "GET", "/api/apps/installed", user="carol")
    [org] = admin_view["data"]["organizations"]
    assert org["installs"][0]["canRevoke"] is True
    member_view = call(world, "GET", "/api/apps/installed", user="bob")
    assert member_view["data"]["organizations"][0]["installs"][0][
        "canRevoke"] is False


def test_revoking_an_org_install_withdraws_the_approval_and_guests_cannot_install():
    world = World()
    world.org("acme", carol="owner", bob="member", gwen="guest",
              otto="auditor")
    created = create_app(world)
    world.db.execute("UPDATE apps SET listing='listed'")
    scope = "repos:read issues:read"
    pending = authorize(world, created, user="bob", target="org:acme",
                        scope=scope)
    [request] = call(world, "GET", "/api/apps/orgs/acme/requests",
                     user="carol")["data"]["requests"]
    call(world, "POST", "/api/apps/orgs/acme/requests/" + request["id"],
         {"action": "approve"}, user="carol")
    for user in ("gwen", "otto"):
        refused = authorize(world, created, user=user, target="org:acme",
                            scope=scope)
        assert refused["status"] == 403
        assert refused["data"]["error"] == "not_org_member"
    done = authorize(world, created, user="bob", target="org:acme",
                     scope=scope)
    exchange(world, created, code_from(done))
    [org] = call(world, "GET", "/api/apps/installed",
                 user="carol")["data"]["organizations"]
    install_id = org["installs"][0]["installId"]
    assert call(world, "DELETE", "/api/apps/installs/" + install_id,
                user="carol")["status"] == 200
    again = authorize(world, created, user="bob", target="org:acme",
                      scope=scope)
    assert pending["data"]["pending"] is True
    assert again["data"].get("pending") is True
    assert "code=" not in again["data"]["redirect"]


def test_org_guests_cannot_see_the_organization_private_apps():
    world = World()
    world.org("acme", alice="owner", gwen="guest")
    created = create_app(world, organization="acme")
    refused = authorize(world, created, user="gwen")
    assert refused["data"]["error"] == "app_not_available"


def test_org_policy_restricts_member_requests():
    world = World()
    world.org("acme", carol="owner", bob="member")
    created = create_app(world)
    world.db.execute("UPDATE apps SET listing='listed'")
    policy = call(world, "PUT", "/api/apps/orgs/acme/policy",
                  {"requestPolicy": "none"}, user="carol")
    assert policy["status"] == 200
    blocked = authorize(world, created, user="bob", target="org:acme")
    assert blocked["data"]["error"] == "org_requests_disabled"
    call(world, "PUT", "/api/apps/orgs/acme/policy",
         {"requestPolicy": "allowlist", "allowlist": []}, user="carol")
    blocked = authorize(world, created, user="bob", target="org:acme")
    assert blocked["data"]["error"] == "app_not_allowed_by_org"
    call(world, "PUT", "/api/apps/orgs/acme/policy",
         {"requestPolicy": "allowlist",
          "allowlist": [created["app"]["id"]]}, user="carol")
    allowed = authorize(world, created, user="bob", target="org:acme")
    assert allowed["data"]["pending"] is True
    admin = authorize(world, created, user="carol", target="org:acme")
    assert "code=" in admin["data"]["redirect"]
    assert call(world, "PUT", "/api/apps/orgs/acme/policy",
                {"requestPolicy": "any"}, user="bob")["status"] == 403


def test_narrowing_scopes_on_reconsent_revokes_old_tokens():
    import asyncio
    import apps
    from apps_fakes import Runtime
    world = World()
    created = create_app(world)
    tokens = install(world, created, scope="repos:read issues:read")
    authorize(world, created, scope="repos:read")
    assert asyncio.run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None


def test_suspended_apps_lose_access():
    import asyncio
    import apps
    from apps_fakes import Runtime
    world = World()
    created = create_app(world)
    tokens = install(world, created)
    world.db.execute("UPDATE apps SET listing='suspended'")
    assert asyncio.run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None
    assert authorize(world, created)["data"]["error"] == "invalid_client"


def test_pkce_reference_vector():
    assert core.pkce_s256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == \
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
