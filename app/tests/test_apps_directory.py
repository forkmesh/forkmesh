from apps_fakes import World, call, create_app, install

HOMEPAGE = "https://hello.example.org/"


def _submitted(world):
    created = create_app(world, homepageUrl=HOMEPAGE)
    app_id = created["app"]["id"]
    submitted = call(world, "POST", "/api/apps/%s/submit" % app_id)
    assert submitted["data"]["listing"] == "pending"
    return created, app_id


def _queue(world, user="root"):
    return call(world, "GET", "/api/apps/review", user=user)


def test_only_admins_review_and_approval_pins_the_content():
    world = World()
    created, app_id = _submitted(world)
    assert call(world, "GET", "/api/apps/directory", user="")["data"][
        "apps"] == []
    assert _queue(world, user="alice")["status"] == 403
    [item] = _queue(world)["data"]["apps"]
    assert item["listing"] == "pending"
    assert item["redirectUris"] == ["https://hello.example.org/oauth/callback"]
    denied = call(world, "POST", "/api/apps/%s/review" % app_id,
                  {"action": "approve", "contentHash": item["contentHash"]})
    assert denied["status"] == 403
    stale = call(world, "POST", "/api/apps/%s/review" % app_id,
                 {"action": "approve", "contentHash": "0" * 64}, user="root")
    assert stale["data"]["error"] == "content_changed"
    approved = call(world, "POST", "/api/apps/%s/review" % app_id,
                    {"action": "approve", "contentHash": item["contentHash"]},
                    user="root")
    assert approved["data"] == {"ok": True, "listing": "listed",
                                "selfReview": False}
    listing = call(world, "GET", "/api/apps/directory", user="")["data"]
    [entry] = listing["apps"]
    assert entry["slug"] == "hello-app"
    assert entry["homepageUrl"] == HOMEPAGE
    assert entry["rating"] == {"average": 0.0, "count": 0}
    assert "redirectUris" not in entry and "eventUrl" not in entry
    assert any(n[0] == "alice" and "listed" in n[1] for n in world.notices)

    edited = call(world, "PATCH", "/api/apps/" + app_id,
                  {"description": "Now with more hello."})
    assert edited["data"]["app"]["listing"] == "pending"
    assert call(world, "GET", "/api/apps/directory", user="")["data"][
        "apps"] == []


def test_reject_and_suspend_need_reasons_and_suspend_cuts_access():
    import asyncio
    import apps
    from apps_fakes import Runtime
    world = World()
    created, app_id = _submitted(world)
    assert call(world, "POST", "/api/apps/%s/review" % app_id,
                {"action": "reject"}, user="root")["data"]["error"] == \
        "reason_required"
    rejected = call(world, "POST", "/api/apps/%s/review" % app_id,
                    {"action": "reject", "reason": "Scopes are too broad."},
                    user="root")
    assert rejected["data"]["listing"] == "rejected"
    assert call(world, "POST", "/api/apps/%s/submit" % app_id)["data"][
        "listing"] == "pending"
    item = _queue(world)["data"]["apps"][0]
    assert item["reviews"][0]["action"] == "reject"
    call(world, "POST", "/api/apps/%s/review" % app_id,
         {"action": "approve", "contentHash": item["contentHash"]},
         user="root")
    tokens = install(world, created, user="bob")
    suspended = call(world, "POST", "/api/apps/%s/review" % app_id,
                     {"action": "suspend", "reason": "Spam reports."},
                     user="root")
    assert suspended["data"]["listing"] == "suspended"
    assert asyncio.run(apps.authenticate(
        Runtime(world), tokens["access_token"])) is None
    bad = call(world, "POST", "/api/apps/%s/review" % app_id,
               {"action": "approve", "contentHash": item["contentHash"]},
               user="root")
    assert bad["data"]["error"] == "invalid_transition"
    back = call(world, "POST", "/api/apps/%s/review" % app_id,
                {"action": "reinstate"}, user="root")
    assert back["data"]["listing"] == "pending"


def test_self_review_is_recorded():
    world = World()
    world.admins.add("alice")
    created, app_id = _submitted(world)
    item = _queue(world, user="alice")["data"]["apps"][0]
    result = call(world, "POST", "/api/apps/%s/review" % app_id,
                  {"action": "approve", "contentHash": item["contentHash"]})
    assert result["data"]["selfReview"] is True
    assert world.db.execute(
        "SELECT self_review FROM app_reviews").fetchone()[0] == 1
    assert ("alice", "apps.listing.approve", app_id, "success") in \
        world.audits


def test_listed_apps_are_installable_by_anyone_private_ones_are_not():
    world = World()
    created, app_id = _submitted(world)
    from apps_fakes import authorize
    assert authorize(world, created, user="bob")["data"]["error"] == \
        "app_not_available"
    item = _queue(world)["data"]["apps"][0]
    call(world, "POST", "/api/apps/%s/review" % app_id,
         {"action": "approve", "contentHash": item["contentHash"]},
         user="root")
    assert "code=" in authorize(world, created, user="bob")["data"]["redirect"]


def test_homepage_must_be_https():
    world = World()
    result = call(world, "POST", "/api/apps", {
        "name": "X App", "redirectUris": ["https://x.example.org/cb"],
        "scopes": ["repos:read"], "homepageUrl": "javascript:alert(1)"})
    assert result["data"]["error"] == "invalid_homepage_url"
