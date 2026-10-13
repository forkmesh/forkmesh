"""End to end: Market ratings and improvement reviews on the CPython host.

The unchanged Worker app under uvicorn with the sqlite3 D1 fallback (every
app/migrations file applied, including 0198). Real accounts sign up, have their
address confirmed with ``ops verify-email``, and drive /api/market/reviews:
the server-side review requirement, the one-rating-per-account upsert, the
maintainer ping, owner replies, and the retired anonymous rating write.
"""

from __future__ import annotations

import re
import subprocess
import sys

import pytest

from forkmesh_host import paths

from test_host_e2e import client, server  # noqa: F401  (module fixtures)


PROBLEM = ("The contact import stopped halfway through my CSV file and "
           "silently dropped every row after the first empty line.")
IMPROVEMENT = ("Skip blank lines instead of stopping, and show a summary of "
               "which rows were imported and which ones failed.")


def _ops(server, *args):
    return subprocess.run(
        [sys.executable, "-m", "forkmesh_host", "ops", *args],
        cwd=str(paths.HOST_DIR), env=server["env"], capture_output=True,
        text=True, timeout=120)


def _account(client, server, name, verify=True):
    page = client.get("/signup")
    field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
    signup = client.post("/api/accounts/signup", json={
        "nodeName": name, field: name + "@example.com",
        "password": "correct-horse-battery",
        "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
    assert signup.status_code == 201, signup.text[:300]
    if verify:
        done = _ops(server, "verify-email", name)
        assert done.returncode == 0, done.stderr[-2000:]
    login = client.post("/api/accounts/login", json={
        "nodeName": name, "password": "correct-horse-battery"})
    assert login.status_code == 200, login.text[:300]
    return {"authorization": "Bearer " + login.json()["sessionToken"]}


@pytest.fixture(scope="module")
def accounts(client, server):  # noqa: F811
    owner = _account(client, server, "revowner")
    # The first-party packages are maintained by the forkmesh organization.
    org = client.post("/api/orgs", headers=owner,
                      json={"name": "forkmesh", "displayName": "ForkMesh"})
    assert org.status_code in (200, 201), org.text[:300]
    return {
        "owner": owner,
        "alice": _account(client, server, "revalice"),
        "bob": _account(client, server, "revbob"),
        "pending": _account(client, server, "revpending", verify=False),
    }


def test_review_page_is_served(client):  # noqa: F811
    page = client.get("/market-reviews?market=forkmesh&package=crm")
    assert page.status_code == 200
    assert "data-review-form" in page.text
    assert client.get("/market-review-form.js").status_code == 200


def test_ratings_under_five_need_a_review_and_ping_the_maintainers(
        client, accounts):  # noqa: F811
    url = "/api/market/reviews/forkmesh/crm"
    anonymous = client.post(url, json={"stars": 5})
    assert anonymous.status_code == 401
    pending = client.post(url, headers=accounts["pending"], json={"stars": 5})
    assert pending.status_code == 403
    assert pending.json()["error"] == "email_not_verified"
    own = client.post(url, headers=accounts["owner"], json={"stars": 5})
    assert own.status_code == 403
    assert own.json()["error"] == "maintainer_cannot_rate"

    bare = client.post(url, headers=accounts["alice"], json={"stars": 2})
    assert bare.status_code == 400, bare.text[:300]
    assert bare.json()["error"] == "review_required"
    lazy = client.post(url, headers=accounts["alice"], json={
        "stars": 2, "problem": "bad", "improvement": "fix it"})
    assert lazy.status_code == 400
    assert lazy.json()["fields"] == {"problem": "too_short",
                                     "improvement": "too_short"}

    rated = client.post(url, headers=accounts["alice"], json={
        "stars": 2, "problem": PROBLEM, "improvement": IMPROVEMENT,
        "category": "bug"})
    assert rated.status_code == 201, rated.text[:500]
    review = rated.json()["review"]
    assert rated.json()["notified"] == 1

    pings = client.get("/api/notifications?node=revowner",
                       headers=accounts["owner"])
    assert pings.status_code == 200, pings.text[:300]
    assert "package_review" in pings.text
    assert "2-star review of CRM" in pings.text

    five = client.put(url, headers=accounts["bob"], json={"stars": 5})
    assert five.status_code == 201, five.text[:300]
    again = client.post(url, headers=accounts["alice"], json={"stars": 4,
                        "problem": PROBLEM, "improvement": IMPROVEMENT})
    assert again.status_code == 200
    assert again.json()["review"]["id"] == review["id"]
    assert again.json()["review"]["revision"] == 2

    summary = client.get("/api/market/reviews/summary").json()
    assert summary["packages"]["crm"] == {
        "average": 4.5, "count": 2,
        "histogram": {"1": 0, "2": 0, "3": 0, "4": 1, "5": 1}}

    reply = client.post(
        "/api/market/reviews/items/%s/reply" % review["id"],
        headers=accounts["owner"],
        json={"text": "Blank lines are skipped in 1.14.", "addressedIn": "1.14"})
    assert reply.status_code == 200, reply.text[:300]
    listing = client.get(url + "?sort=newest").json()
    assert listing["total"] == 2
    alice = next(r for r in listing["reviews"]
                 if r["reviewer"]["name"] == "revalice")
    assert alice["reply"]["by"] == "revowner"
    assert alice["addressedIn"] == "v1.14"
    history = client.get("/api/market/reviews/items/%s/history" % review["id"],
                         headers=accounts["alice"]).json()
    assert [r["stars"] for r in history["revisions"]] == [2, 4]
    feedback = client.get(url + "/feedback", headers=accounts["owner"])
    assert feedback.status_code == 200
    assert feedback.json()["statuses"] == {"open": 0, "addressed": 1}
    assert client.get(url + "/feedback",
                      headers=accounts["bob"]).status_code == 403


def test_anonymous_rating_writes_are_retired(client):  # noqa: F811
    legacy = client.post("/api/market/ratings", json={
        "run": "browser-1", "id": "crm", "rating": 1})
    assert legacy.status_code == 410
    assert legacy.json()["error"] == "account_rating_required"
    assert client.get("/api/market/ratings").status_code == 200


def test_server_log_has_no_tracebacks(client, server):  # noqa: F811
    server["log"].flush()
    server["log"].seek(0)
    text = server["log"].read()
    assert "Traceback" not in text, text[-4000:]
