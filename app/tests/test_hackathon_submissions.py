#!/usr/bin/env python3
"""Teams, submissions, the gallery, Community Choice, judging and results.

The real hackathon_runtime against the in-memory D1 from test_hackathon. The
claims here are the ones that cost somebody an entry or a prize when wrong.
"""

import json

from test_hackathon import (  # noqa: F401  (pytest fixture import)
    NOW, body_of, harness, policy, run,
)

DAY = policy.DAY_MS
MONTH = policy.MONTH_MS


def _live(harness, **patch):
    slug = harness.campaign(status="live")
    for key, value in patch.items():
        harness.db.connection.execute(
            "UPDATE hackathons SET %s=? WHERE slug=?" % key, (value, slug))
    harness.db.connection.commit()
    return slug


def _person(harness, name, verified=True):
    harness.account(name, verified=verified)
    run(harness.module._create_participant(
        harness.env, "mesh-2026", "bi:" + name, name))
    if verified:
        harness.db.connection.execute(
            "UPDATE hackathon_participants SET verified_at=? WHERE name=?",
            (NOW, name))
        harness.db.connection.commit()
    return "bi:" + name


def _post(harness, who, handler, path, body, *args):
    harness.login(who)
    response = run(handler(
        harness.env, harness.request(path, "POST", body), *args))
    return response, body_of(response)


def _team(harness, captain, name="Keepers", *members):
    module = harness.module
    _response, body = _post(harness, captain, module.teams_handler,
                            "/api/hackathon/teams", {"name": name})
    code = body["team"]["code"]
    for member in members:
        _post(harness, member, module.teams_handler,
              "/api/hackathon/teams/join", {"code": code}, "join")
    return body["team"]


def _publish_repo(harness, owner, repo, first_hosted=NOW + DAY):
    connection = harness.db.connection
    key = "bi:%s/%s" % (owner, repo)
    connection.execute(
        "INSERT INTO repositories (key_bi,owner_bi,data) VALUES (?,?,?)",
        (key, "bi:" + owner, "{}"))
    connection.execute(
        "INSERT INTO repo_first_hosted (repo_bi,ts) VALUES (?,?)",
        (key, first_hosted))
    connection.commit()


def _submit(harness, who, track="agents", repo="alice/keepalive",
            title="Keepalive"):
    return _post(harness, who, harness.module.submissions_handler,
                 "/api/hackathon/submissions",
                 {"track": track, "repo": repo, "title": title,
                  "pitch": "Keeps mirrors honest."})


# --- teams -----------------------------------------------------------------------

def test_teams_hold_five_and_a_person_is_on_one(harness):
    _live(harness)
    names = ["alice", "bob", "cy", "dee", "eli", "fay"]
    for name in names:
        _person(harness, name)
    team = _team(harness, "alice", "Keepers", "bob", "cy", "dee", "eli")
    _response, full = _post(harness, "fay", harness.module.teams_handler,
                            "/api/hackathon/teams/join",
                            {"code": team["code"]}, "join")
    assert full["error"] == "team_full"
    _response, second = _post(harness, "bob", harness.module.teams_handler,
                              "/api/hackathon/teams", {"name": "Other"})
    assert second["error"] == "already_on_a_team"
    assert harness.db.scalar(
        "SELECT member_count FROM hackathon_teams") == 5


def test_the_captaincy_passes_on_and_the_last_member_cannot_strand_a_project(
        harness):
    _live(harness)
    for name in ("alice", "bob"):
        _person(harness, name)
    _publish_repo(harness, "alice", "keepalive")
    _team(harness, "alice", "Keepers", "bob")
    _submit(harness, "alice")
    _post(harness, "alice", harness.module.teams_handler,
          "/api/hackathon/teams/leave", {}, "leave")
    assert harness.db.scalar("SELECT captain_name FROM hackathon_teams") == "bob"
    _response, body = _post(harness, "bob", harness.module.teams_handler,
                            "/api/hackathon/teams/leave", {}, "leave")
    assert body["error"] == "withdraw_submissions_first"


# --- submissions -----------------------------------------------------------------

def test_a_public_repository_hosted_in_the_window_is_valid(harness):
    _live(harness)
    for name in ("alice", "bob"):
        _person(harness, name)
    _publish_repo(harness, "alice", "keepalive")
    _team(harness, "alice", "Keepers", "bob")
    response, body = _submit(harness, "alice")
    assert response.status == 201
    assert body["status"] == "valid" and body["reviewFlags"] == []
    # Every member earns the submission's points once.
    for who in ("bi:alice", "bi:bob"):
        assert harness.db.count(
            "hackathon_points", "account_bi=? AND kind='submission'", who) == 1
    assert harness.db.scalar(
        "SELECT submission_count FROM hackathons") == 1
    # Editing is an update, not a second submission.
    response, _body = _submit(harness, "bob", title="Keepalive 2")
    assert response.status == 200
    assert harness.db.count("hackathon_submissions") == 1
    assert harness.db.scalar(
        "SELECT submission_count FROM hackathons") == 1


def test_an_unconfirmable_repository_is_flagged_not_rejected(harness):
    _live(harness)
    _person(harness, "alice")
    _team(harness, "alice")
    response, body = _submit(harness, "alice", repo="alice/missing")
    assert response.status == 201
    assert body["status"] == "needs_review"
    assert body["reviewFlags"] == ["repository_not_public"]
    _publish_repo(harness, "alice", "old", first_hosted=NOW - 30 * DAY)
    _response, early = _submit(harness, "alice", track="preserve",
                               repo="alice/old")
    assert early["reviewFlags"] == ["hosted_before_campaign"]


def test_an_administrators_rejection_survives_an_edit(harness):
    _live(harness)
    _person(harness, "alice")
    harness.account("admin")
    harness.admins.add("admin")
    _publish_repo(harness, "alice", "keepalive")
    _team(harness, "alice")
    _response, body = _submit(harness, "alice")
    _post(harness, "admin", harness.module.admin_handler,
          "/api/admin/hackathon",
          {"slug": "mesh-2026", "action": "review_submission",
           "submissionId": body["submissionId"], "status": "invalid"})
    _response, edited = _submit(harness, "alice", title="Try again")
    assert edited["status"] == "invalid"


def test_submissions_close_at_the_deadline_and_need_a_team(harness):
    _live(harness, ends_at=NOW + DAY)
    _person(harness, "alice")
    _response, body = _submit(harness, "alice")
    assert body["error"] == "no_team"
    _team(harness, "alice")
    harness.clock["now"] = NOW + 2 * DAY
    _response, late = _submit(harness, "alice")
    assert late["error"] == "submissions_closed"


def test_joining_a_team_that_already_submitted_earns_its_points(harness):
    _live(harness)
    for name in ("alice", "bob"):
        _person(harness, name)
    _publish_repo(harness, "alice", "keepalive")
    team = _team(harness, "alice")
    _submit(harness, "alice")
    _post(harness, "bob", harness.module.teams_handler,
          "/api/hackathon/teams/join", {"code": team["code"]}, "join")
    assert harness.db.count(
        "hackathon_points", "account_bi='bi:bob' AND kind='submission'") == 1


# --- gallery and votes -------------------------------------------------------------

def _two_projects(harness):
    _live(harness)
    for name in ("alice", "bob", "cy"):
        _person(harness, name)
    _publish_repo(harness, "alice", "keepalive")
    _publish_repo(harness, "bob", "archiver")
    _team(harness, "alice", "Keepers")
    _team(harness, "bob", "Archivists")
    _response, first = _submit(harness, "alice")
    _response, second = _submit(harness, "bob", repo="bob/archiver",
                                title="Archiver")
    return first["submissionId"], second["submissionId"]


def test_the_gallery_lists_only_valid_projects(harness):
    first, _second = _two_projects(harness)
    harness.db.connection.execute(
        "UPDATE hackathon_submissions SET status='needs_review' "
        "WHERE submission_id=?", (first,))
    harness.db.connection.commit()
    harness.cache.clear()
    gallery = body_of(run(harness.module.projects_handler(
        harness.env, harness.request("/api/hackathon/projects"))))
    assert [p["title"] for p in gallery["projects"]] == ["Archiver"]
    assert "status" not in gallery["projects"][0]
    assert "reviewFlags" not in gallery["projects"][0]
    assert run(harness.module.project_page_handler(
        harness.env, harness.request("/hackathon/projects/" + first),
        first)) is None


def test_the_project_page_escapes_what_a_submitter_wrote(harness):
    _live(harness)
    _person(harness, "alice")
    _publish_repo(harness, "alice", "keepalive")
    _team(harness, "alice", "Keepers")
    _response, body = _submit(harness, "alice",
                              title="<script>alert(1)</script>")
    page = run(harness.module.project_page_handler(
        harness.env, harness.request("/hackathon/projects/x"),
        body["submissionId"]))
    assert "<script>alert" not in page.body
    assert "&lt;script&gt;" in page.body
    assert 'href="/alice/keepalive"' in page.body


def test_community_choice_opens_with_its_milestone_and_counts_one_vote(harness):
    first, second = _two_projects(harness)
    vote = harness.module.vote_handler
    _response, closed = _post(harness, "cy", vote,
                              "/api/hackathon/projects/%s/vote" % first, {},
                              first)
    assert closed["error"] == "voting_closed"
    harness.db.connection.execute(
        "UPDATE hackathon_milestones SET unlocked_at=? "
        "WHERE key='community_choice'", (NOW,))
    harness.db.connection.commit()
    _response, own = _post(harness, "alice", vote,
                           "/api/hackathon/projects/%s/vote" % first, {},
                           first)
    assert own["error"] == "own_project"
    _person(harness, "unverified", verified=False)
    _response, unverified = _post(harness, "unverified", vote,
                                  "/api/hackathon/projects/%s/vote" % first,
                                  {}, first)
    assert unverified["error"] == "verified_participants_only"
    _post(harness, "cy", vote, "/api/hackathon/projects/%s/vote" % first, {},
          first)
    counts = dict(harness.db.connection.execute(
        "SELECT submission_id,vote_count FROM hackathon_submissions"
    ).fetchall())
    assert counts == {first: 1, second: 0}
    # Moving the vote within the same track moves the count.
    _response, moved = _post(harness, "cy", vote,
                             "/api/hackathon/projects/%s/vote" % second, {},
                             second)
    assert moved["moved"] is True
    counts = dict(harness.db.connection.execute(
        "SELECT submission_id,vote_count FROM hackathon_submissions"
    ).fetchall())
    assert counts == {first: 0, second: 1}


# --- judging and results ------------------------------------------------------------

def _judge(harness, name):
    harness.account(name)
    harness.account("admin")
    harness.admins.add("admin")
    _post(harness, "admin", harness.module.admin_handler,
          "/api/admin/hackathon",
          {"slug": "mesh-2026", "action": "add_judge", "name": name})


def _score(harness, judge, submission_id, value):
    return _post(harness, judge, harness.module.judging_handler,
                 "/api/hackathon/judging/" + submission_id,
                 {"fit": value, "working": value, "portability": value,
                  "craft": value}, submission_id)


def test_only_judges_score_and_never_their_own_team(harness):
    first, _second = _two_projects(harness)
    harness.db.connection.execute("UPDATE hackathons SET status='judging'")
    harness.db.connection.commit()
    _response, denied = _score(harness, "cy", first, 5)
    assert denied["error"] == "judges_only"
    _judge(harness, "alice")
    _response, conflict = _score(harness, "alice", first, 5)
    assert conflict["error"] == "conflict_of_interest"


def test_a_revised_score_replaces_the_old_one(harness):
    first, _second = _two_projects(harness)
    harness.db.connection.execute("UPDATE hackathons SET status='judging'")
    harness.db.connection.commit()
    _judge(harness, "judy")
    _score(harness, "judy", first, 5)
    _score(harness, "judy", first, 3)
    row = harness.db.connection.execute(
        "SELECT score_total,score_count FROM hackathon_submissions "
        "WHERE submission_id=?", (first,)).fetchone()
    assert (row["score_total"], row["score_count"]) == (12, 1)


def test_judging_waits_for_submissions_to_close(harness):
    first, _second = _two_projects(harness)
    _judge(harness, "judy")
    _response, early = _score(harness, "judy", first, 4)
    assert early["error"] == "judging_closed"


def test_finalizing_places_the_tracks_and_grants_prizes_once(harness):
    first, second = _two_projects(harness)
    connection = harness.db.connection
    connection.execute("UPDATE hackathons SET status='judging'")
    connection.commit()
    _judge(harness, "judy")
    _score(harness, "judy", first, 5)
    _score(harness, "judy", second, 2)
    finalize = {"slug": "mesh-2026", "action": "finalize"}
    _response, result = _post(harness, "admin", harness.module.admin_handler,
                              "/api/admin/hackathon", finalize)
    assert result["placements"] == {first: "first", second: "second"}
    assert harness.db.scalar("SELECT status FROM hackathons") == "closed"
    pro = connection.execute(
        "SELECT expires_at FROM role_grants WHERE account_bi='bi:alice' "
        "AND role='pro_member'").fetchone()
    # A valid submission (3 months) plus first place (12 months).
    assert pro["expires_at"] == NOW + 15 * MONTH
    assert ("alice", "bi:alice", "hackathon_winner", "hackathon") in (
        harness.badges)
    bob = connection.execute(
        "SELECT expires_at FROM role_grants WHERE account_bi='bi:bob' "
        "AND role='pro_member'").fetchone()
    assert bob["expires_at"] == NOW + 9 * MONTH
    before = harness.db.count("hackathon_reward_grants")
    _post(harness, "admin", harness.module.admin_handler,
          "/api/admin/hackathon", finalize)
    assert harness.db.count("hackathon_reward_grants") == before
    assert connection.execute(
        "SELECT expires_at FROM role_grants WHERE account_bi='bi:alice' "
        "AND role='pro_member'").fetchone()["expires_at"] == NOW + 15 * MONTH
    kinds = {row[0] for row in connection.execute(
        "SELECT DISTINCT kind FROM hackathon_reward_grants")}
    assert kinds <= {"pro_months", "badge"}


def test_finalizing_needs_the_judging_phase(harness):
    _two_projects(harness)
    harness.account("admin")
    harness.admins.add("admin")
    _response, body = _post(harness, "admin", harness.module.admin_handler,
                            "/api/admin/hackathon",
                            {"slug": "mesh-2026", "action": "finalize"})
    assert body["error"] == "not_judging"


def test_the_hq_carries_the_team_and_the_windows(harness):
    _live(harness)
    _person(harness, "alice")
    _team(harness, "alice", "Keepers")
    harness.login("alice")
    payload = body_of(run(harness.module.me_handler(
        harness.env, harness.request("/api/hackathon/me"))))
    assert payload["team"]["name"] == "Keepers"
    assert payload["team"]["members"] == ["alice"]
    assert payload["submissionsOpen"] is True
    assert payload["judge"] is False
    assert payload["votingOpen"] is False
    json.dumps(payload)
