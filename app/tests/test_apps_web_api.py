import asyncio

from apps_fakes import Runtime, World, call, create_app, install
import apps_api  # noqa: E402
import apps_core as core  # noqa: E402

CHANNEL = "c" * 32
PRIVATE = "d" * 32


def _world():
    world = World()
    world.channels[CHANNEL] = {"public": True, "name": "general",
                               "key_version": 3}
    world.channels[PRIVATE] = {"public": False, "name": "secret",
                               "key_version": 1}
    world.repos[("alice", "demo")] = True
    world.repos[("bob", "other")] = True
    world.repos[("acme", "site")] = True
    return world


def _v1(world, token, method, body=None):
    return call(world, "POST", "/api/apps/v1/" + method, body or {},
                user="", headers={"authorization": "Bearer " + token})


def _gate(world, token, path, method="GET", query=None):
    runtime = Runtime(world, method)
    return asyncio.run(apps_api.gate(runtime, token, method, path,
                                     query or {}))


def test_existing_rest_reads_are_scope_gated_and_read_only():
    world = _world()
    created = create_app(world, bot={"enabled": True})
    tokens = install(world, created, scope="repos:read")
    token = tokens["access_token"]
    context, denied = _gate(world, token, "/api/repo/alice/demo/tree",
                            query={"path": "src"})
    assert denied is None
    assert context["identity"] == "acct:alice"
    _context, denied = _gate(world, token, "/api/repo/alice/demo/tree",
                             query={"path": ".forkmesh/issues"})
    assert denied["status"] == 403
    assert denied["data"] == {"ok": False, "error": "missing_scope",
                              "needed": "issues:read"}
    for method, path in (("POST", "/api/repo/alice/demo/tree"),
                         ("DELETE", "/api/accounts/sessions/all"),
                         ("GET", "/api/accounts/sessions"),
                         ("GET", "/api/admin/users")):
        _context, denied = _gate(world, token, path, method=method)
        assert denied["status"] == 403, (method, path)
        assert denied["data"]["error"] == "app_token_not_allowed"
    context, denied = _gate(world, token, "/api/repo/bob/other/tree")
    assert denied is None and context["identity"] == ""
    _context, denied = _gate(world, "fmat_" + "0" * 64, "/api/repositories")
    assert denied["status"] == 401


def test_pings_read_route_is_limited_to_the_installer():
    world = _world()
    created = create_app(world, scopes=["pings:read", "repos:read"])
    token = install(world, created, scope="pings:read")["access_token"]
    context, denied = _gate(world, token, "/api/notifications",
                            query={"node": "alice"})
    assert denied is None and context["identity"] == "acct:alice"
    _context, denied = _gate(world, token, "/api/notifications",
                             query={"node": "bob"})
    assert denied["status"] == 403


def test_v1_methods_enforce_scopes_and_the_bot_user():
    world = _world()
    created = create_app(world, bot={"enabled": True})
    token = install(world, created, scope="repos:read")["access_token"]
    test = _v1(world, token, "auth.test")
    assert test["data"]["bot"] == {"id": "app:hello-app",
                                   "name": "Hello App (app)"}
    denied = _v1(world, token, "chat.postMessage",
                 {"channel": CHANNEL, "text": "hi"})
    assert denied["status"] == 403
    assert denied["data"]["needed"] == "chat:write"
    assert _v1(world, token, "nope.method")["status"] == 404
    assert call(world, "POST", "/api/apps/v1/auth.test", {}, user="",
                headers={"authorization": "Bearer fmat_" + "1" * 64}
                )["status"] == 401

    no_bot = create_app(world, name="Quiet App",
                        redirectUris=["https://quiet.example.org/cb"])
    quiet = install(world, no_bot, scope="chat:write")["access_token"]
    refused = _v1(world, quiet, "chat.postMessage",
                  {"channel": CHANNEL, "text": "hi"})
    assert refused["data"]["error"] == "bot_user_required"


def test_bot_posts_are_attributed_to_the_app_and_audited():
    world = _world()
    created = create_app(world, bot={"enabled": True})
    token = install(world, created)["access_token"]
    posted = _v1(world, token, "chat.postMessage", {
        "channel": CHANNEL, "text": "Build passed",
        "blocks": [{"type": "section", "text": "Build passed"},
                   {"type": "actions", "elements": [
                       {"type": "button", "action_id": "rerun",
                        "text": "Re-run", "value": "rerun"}]}]})
    assert posted["status"] == 200, posted["data"]
    [(channel_id, plain)] = world.chat
    assert channel_id == CHANNEL
    assert plain["senderId"] == "app:hello-app"
    assert plain["sender"] == "Hello App (app)"
    assert plain["blocks"][1]["elements"][0]["action_id"] == "rerun"
    assert plain["appMessageId"] == posted["data"]["messageId"]
    assert world.db.execute("SELECT COUNT(*) FROM app_messages").fetchone()[0] == 1
    assert ("app:hello-app", "apps.chat.post", CHANNEL, "success") in world.audits

    private = _v1(world, token, "chat.postMessage",
                  {"channel": PRIVATE, "text": "hi"})
    assert private["data"]["error"] == "public_channel_required"
    bad_blocks = _v1(world, token, "chat.postMessage", {
        "channel": CHANNEL, "text": "x",
        "blocks": [{"type": "html", "text": "<script>"}]})
    assert bad_blocks["data"]["error"] == "invalid_block"


def test_issue_writes_need_coverage_and_never_use_a_person():
    world = _world()
    created = create_app(world, bot={"enabled": True},
                         scopes=["issues:write", "pulls:write", "agents:run",
                                 "repos:read"])
    token = install(world, created)["access_token"]
    opened = _v1(world, token, "issues.create", {
        "owner": "alice", "repo": "demo", "title": "Nightly failed",
        "body": "Details"})
    assert opened["status"] == 201, opened["data"]
    assert world.issues[-1]["author"] == "app:hello-app"
    assert world.issues[-1]["authorName"] == "Hello App (app)"
    other = _v1(world, token, "issues.create", {
        "owner": "bob", "repo": "other", "title": "x"})
    assert other["data"]["error"] == "repository_not_covered"
    commented = _v1(world, token, "pulls.comment", {
        "owner": "alice", "repo": "demo", "number": 4, "body": "LGTM"})
    assert commented["status"] == 201
    assert world.pulls[-1]["author"] == "app:hello-app"
    agent = _v1(world, token, "agents.run", {
        "owner": "alice", "repo": "demo", "number": 1})
    assert agent["status"] == 201
    assert world.issues[-1]["wantsAgent"] is True


def test_org_install_covers_org_repositories_and_pings_admins():
    world = _world()
    world.org("acme", carol="owner", dave="admin", bob="member")
    created = create_app(world, bot={"enabled": True},
                         scopes=["issues:write", "pings:write"])
    world.db.execute("UPDATE apps SET listing='listed'")
    token = install(world, created, user="carol",
                    target="org:acme")["access_token"]
    assert _v1(world, token, "issues.create", {
        "owner": "acme", "repo": "site", "title": "x"})["status"] == 201
    assert _v1(world, token, "issues.create", {
        "owner": "carol", "repo": "demo", "title": "x"}
    )["data"]["error"] == "repository_not_covered"
    sent = _v1(world, token, "pings.send",
               {"title": "Deploy done", "href": "https://evil.example"})
    assert sent["data"]["sent"] == 2
    assert sorted(n[0] for n in world.notices[-2:]) == ["carol", "dave"]
    assert world.notices[-1][2] == "/dashboard/apps"
    assert world.notices[-1][1] == "Hello App: Deploy done"


def test_rate_limits_per_install():
    world = _world()
    created = create_app(world)
    token = install(world, created)["access_token"]
    for _ in range(core.RATE_PER_INSTALL):
        assert _v1(world, token, "auth.test")["status"] == 200
    limited = _v1(world, token, "auth.test")
    assert limited["status"] == 429
    assert 1 <= limited["data"]["retryAfter"] <= 61
    world.now += core.RATE_WINDOW_MS + 1
    assert _v1(world, token, "auth.test")["status"] == 200


def test_revoked_install_tokens_stop_at_the_web_api():
    world = _world()
    created = create_app(world)
    token = install(world, created)["access_token"]
    install_id = call(world, "GET", "/api/apps/installed")["data"][
        "installs"][0]["installId"]
    call(world, "DELETE", "/api/apps/installs/" + install_id)
    assert _v1(world, token, "auth.test")["status"] == 401
