import asyncio
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import sqlite3
import sys


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
APP = REPO / "app"
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import surveys


HOUR = 60 * 60 * 1000
QUESTIONS = [
    {"type": "single", "title": "How was the offsite?", "required": True,
     "options": ["Great", "Fine", "Poor", "Fine", " "]},
    {"type": "multiple", "title": "Which sessions helped?", "options": ["Roadmap", "Hiring", "Demo"]},
    {"type": "rating", "title": "Rate the venue", "scale": 5},
    {"type": "text", "title": "One word for it"},
    {"type": "paragraph", "title": "Anything else?", "help": "Optional"},
    {"type": "text", "title": ""},
]


class Runtime:
    def __init__(self, installed=True):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.execute("CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL)")
        self.db.executescript(
            sorted((APP / "migrations").glob("0*_org_plugin_installs.sql"))[-1]
            .read_text(encoding="utf-8")
        )
        self.db.executescript("""
          CREATE TABLE orgs(org_bi TEXT PRIMARY KEY,name TEXT,data TEXT,created_at INTEGER);
          CREATE TABLE org_members(org_bi TEXT,member_bi TEXT,role TEXT,name TEXT,
            created_at INTEGER,PRIMARY KEY(org_bi,member_bi));
          INSERT INTO orgs VALUES('org:acme','acme','sealed:{}',1);
          INSERT INTO org_members VALUES('org:acme','bi:alice','owner','alice',1);
          INSERT INTO org_members VALUES('org:acme','bi:charlie','member','charlie',1);
          INSERT INTO org_members VALUES('org:acme','bi:dana','member','dana',1);
        """)
        if installed:
            self.install("surveys")
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.clock = 2_000_000_000_000
        self.ids = 0
        self.audits = []
        self.pings = []
        self.ip = "203.0.113.7"
        self.origin_ok = True

    def install(self, plugin):
        self.db.execute(
            "INSERT INTO org_plugin_installs VALUES(?,?,?,?,?,?)",
            ("org:acme", plugin, "acme", "bi:alice", "alice", 1),
        )

    def use(self, method, actor="alice", body=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        return self

    def method(self): return self.method_value
    def same_origin(self): return self.origin_ok
    def now(self): self.clock += 1; return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def request_origin(self): return "https://app.forkmesh.com"
    def request_visitor(self): return {"ip": self.ip, "agent": "Mozilla"}
    def package_secret(self, _name): return ""
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        bi = "bi:" + self.actor if self.actor in {"alice", "charlie", "dana"} else ""
        return (bi, {"name": self.actor}) if bi else ("", None)
    async def organization(self, name, account_bi):
        row = self.db.execute(
            "SELECT o.org_bi,m.role FROM orgs o JOIN org_members m ON "
            "m.org_bi=o.org_bi WHERE o.name=? AND m.member_bi=?",
            (name, account_bi),
        ).fetchone()
        return (row["org_bi"], row["role"]) if row else ("", "")
    async def plugin_installed(self, org_bi, plugin):
        return bool(self.db.execute(
            "SELECT 1 FROM org_plugin_installs WHERE org_bi=? AND plugin=?",
            (org_bi, plugin),
        ).fetchone())
    async def blind(self, value): return hashlib.sha256(value.encode()).hexdigest()
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value): return json.loads(str(value).removeprefix("sealed:"))
    async def notify(self, recipient, kind, title, body="", href="", dedupe="", push=True, **_):
        self.pings.append({"to": recipient, "kind": kind, "title": title, "body": body,
                           "href": href, "dedupe": dedupe, "push": push})
        return True
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append((args, kwargs))


def call(runtime, path):
    return asyncio.run(surveys.handle(runtime, path))


def create(runtime, actor="alice", **extra):
    body = {"title": "Offsite feedback", "description": "Two minutes, promise.",
            "questions": QUESTIONS}
    body.update(extra)
    response = call(runtime.use("POST", actor=actor, body=body), "/api/surveys/acme/items")
    assert response["status"] == 201, response
    return response["data"]["survey"]


def publish(runtime, survey):
    response = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{survey['id']}/open")
    assert response["status"] == 200, response
    return response["data"]["survey"]


def token_of(survey):
    return survey["publicUrl"].split("?t=", 1)[1]


def submit(runtime, survey, answers, ip=None):
    if ip is not None:
        runtime.ip = ip
    return call(runtime.use("POST", actor="", body={"answers": answers}),
                f"/api/surveys/-/public/{token_of(survey)}")


def track(runtime, survey, kind, **extra):
    body = {"kind": kind, "session": "ab" * 16}
    body.update(extra)
    return call(runtime.use("POST", actor="", body=body),
                f"/api/surveys/-/public/{token_of(survey)}/track")


def detail_of(runtime, survey):
    return call(runtime.use("GET"), f"/api/surveys/acme/items/{survey['id']}")["data"]["survey"]


def answers_for(survey, single="Great", multiple=None, rating=4, text="Fun", paragraph=""):
    ids = [question["id"] for question in survey["questions"]]
    return {ids[0]: single, ids[1]: multiple or [], ids[2]: rating, ids[3]: text, ids[4]: paragraph}


def test_create_normalises_questions_and_seals_the_survey():
    runtime = Runtime()
    root = call(runtime.use("GET"), "/api/surveys")
    assert root["data"]["organizations"] == [{"name": "acme", "role": "owner", "installed": True}]
    survey = create(runtime)
    assert survey["status"] == "draft" and survey["displayStatus"] == "draft"
    assert survey["publicUrl"] == ""
    assert survey["thankYou"] == surveys.DEFAULT_THANK_YOU
    assert survey["questionCount"] == 5
    questions = survey["questions"]
    assert [question["type"] for question in questions] == [
        "single", "multiple", "rating", "text", "paragraph"]
    assert questions[0]["options"] == ["Great", "Fine", "Poor"]
    assert questions[0]["required"] is True
    assert questions[2]["scale"] == 5 and questions[3]["options"] == []
    assert len({question["id"] for question in questions}) == 5
    assert all(re.fullmatch(r"[0-9a-f]{8}", question["id"]) for question in questions)
    assert survey["settings"] == surveys.DEFAULT_SETTINGS
    stored = runtime.db.execute("SELECT data,question_count FROM survey_forms").fetchone()
    assert stored["data"].startswith("sealed:") and stored["question_count"] == 5
    untitled = create(runtime, title="", questions=[])
    assert untitled["title"] == "Untitled survey"


def test_invalid_questions_and_schedules_are_refused():
    runtime = Runtime()
    path = "/api/surveys/acme/items"
    few = call(runtime.use("POST", body={"questions": [
        {"type": "single", "title": "Pick", "options": ["Only", "Only"]}]}), path)
    assert few["status"] == 400 and few["data"] == {"error": "invalid_question_options", "question": 0}
    kind = call(runtime.use("POST", body={"questions": [
        {"type": "text", "title": "A"}, {"type": "dropdown", "title": "B"}]}), path)
    assert kind["data"] == {"error": "invalid_question_type", "question": 1}
    many = call(runtime.use("POST", body={"questions": [
        {"type": "text", "title": "Q%d" % index} for index in range(51)]}), path)
    assert many["data"]["error"] == "too_many_questions"
    schedule = call(runtime.use("POST", body={"settings": {"opensAt": 10, "closesAt": 5}}), path)
    assert schedule["data"]["error"] == "invalid_schedule"
    rating = create(runtime, questions=[{"type": "rating", "title": "Score", "scale": 40}])
    assert rating["questions"][0]["scale"] == 10


def test_editing_keeps_question_ids_and_open_surveys_keep_a_question():
    runtime = Runtime()
    survey = create(runtime)
    questions = survey["questions"]
    edited = [dict(questions[1], title="Which sessions helped most?"), questions[0]]
    path = f"/api/surveys/acme/items/{survey['id']}"
    updated = call(runtime.use("PATCH", actor="charlie", body={
        "questions": edited, "settings": {"maxResponses": 99999, "shuffleOptions": True}}), path)
    assert updated["status"] == 200, updated
    survey = updated["data"]["survey"]
    assert [question["id"] for question in survey["questions"]] == [questions[1]["id"], questions[0]["id"]]
    assert survey["settings"]["maxResponses"] == 10000
    assert survey["settings"]["shuffleOptions"] is True
    assert survey["updatedBy"] == "charlie"
    publish(runtime, survey)
    emptied = call(runtime.use("PATCH", body={"questions": []}), path)
    assert emptied["data"]["error"] == "survey_has_no_questions"


def test_publishing_needs_questions_and_drafts_stay_private():
    runtime = Runtime()
    empty = create(runtime, questions=[])
    refused = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{empty['id']}/open")
    assert refused["data"]["error"] == "survey_has_no_questions"
    survey = create(runtime)
    token = "%s.%s" % (survey["id"], hashlib.sha256(
        ("survey-link:" + survey["id"]).encode()).hexdigest()[:24])
    assert call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token}")["status"] == 404
    opened = publish(runtime, survey)
    assert opened["status"] == "open" and opened["publishedAt"]
    assert opened["publicUrl"] == f"https://app.forkmesh.com/survey?t={token}"
    public = call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token}")
    assert public["status"] == 200
    assert public["data"]["survey"]["status"] == "open"
    assert len(public["data"]["survey"]["questions"]) == 5
    assert "responseCount" not in public["data"]["survey"]
    forged = call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{survey['id']}.{'0' * 24}")
    assert forged["status"] == 404
    closed = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{survey['id']}/close")
    assert closed["data"]["survey"]["status"] == "closed"
    shut = call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token}")
    assert shut["data"]["survey"]["status"] == "closed"
    assert shut["data"]["survey"]["questions"] == []


def test_submissions_are_validated_counted_and_summarised():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    ids = [question["id"] for question in survey["questions"]]
    missing = submit(runtime, survey, {ids[3]: "Hi"})
    assert missing["status"] == 400 and missing["data"] == {"error": "answer_required", "question": ids[0]}
    wrong = submit(runtime, survey, answers_for(survey, single="Amazing"))
    assert wrong["data"] == {"error": "invalid_answer", "question": ids[0]}
    high = submit(runtime, survey, answers_for(survey, rating=6))
    assert high["data"] == {"error": "invalid_answer", "question": ids[2]}
    odd = submit(runtime, survey, answers_for(survey, multiple=["Lunch"]))
    assert odd["data"]["question"] == ids[1]
    flag = submit(runtime, survey, answers_for(survey, rating=True))
    assert flag["data"]["question"] == ids[2]
    first = submit(runtime, survey, answers_for(survey, multiple=["Demo", "Roadmap", "Demo"],
                                                 text="=SUM(A1)", paragraph="Loved\nit"))
    assert first["status"] == 201 and first["data"]["thankYou"] == surveys.DEFAULT_THANK_YOU
    submit(runtime, survey, answers_for(survey, single="Fine", rating="2", text="  ok   then "))
    submit(runtime, survey, {ids[0]: "Great"})
    detail = call(runtime.use("GET"), f"/api/surveys/acme/items/{survey['id']}")["data"]["survey"]
    assert detail["responseCount"] == 3 and detail["resultsCounted"] == 3
    single, multiple, rating, text, paragraph = detail["results"]
    assert single["options"] == [{"label": "Great", "count": 2}, {"label": "Fine", "count": 1},
                                 {"label": "Poor", "count": 0}]
    assert multiple["answered"] == 1 and multiple["skipped"] == 2
    assert multiple["options"][0] == {"label": "Roadmap", "count": 1}
    assert rating["distribution"] == [0, 1, 0, 1, 0] and rating["average"] == 3.0
    assert text["answers"] == ["ok then", "=SUM(A1)"]
    assert paragraph["answers"] == ["Loved\nit"]
    stored = runtime.db.execute("SELECT data,respondent_key FROM survey_responses").fetchall()
    assert all(row["data"].startswith("sealed:") for row in stored)
    assert all(re.fullmatch(r"[0-9a-f]{32}", row["respondent_key"]) for row in stored)
    assert runtime.ip not in json.dumps([dict(row) for row in stored])
    overview = call(runtime.use("GET"), "/api/surveys/acme")["data"]
    assert overview["summary"] == {"surveys": 1, "open": 1, "drafts": 0, "closed": 0,
                                   "responses": 3, "responsesLast7Days": 3}
    assert "questions" not in overview["surveys"][0]


def test_renamed_options_stop_counting_without_touching_responses():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    submit(runtime, survey, answers_for(survey, multiple=["Hiring", "Demo"]))
    questions = survey["questions"]
    questions[1] = dict(questions[1], options=["Roadmap", "Recruiting", "Demo"])
    call(runtime.use("PATCH", body={"questions": questions}), f"/api/surveys/acme/items/{survey['id']}")
    detail = call(runtime.use("GET"), f"/api/surveys/acme/items/{survey['id']}")["data"]["survey"]
    assert detail["results"][1]["options"] == [
        {"label": "Roadmap", "count": 0}, {"label": "Recruiting", "count": 0}, {"label": "Demo", "count": 1}]
    responses = call(runtime.use("GET"), f"/api/surveys/acme/items/{survey['id']}/responses")
    assert responses["data"]["responses"][0]["answers"][questions[1]["id"]] == ["Hiring", "Demo"]


def test_one_response_per_person_hourly_limit_and_privacy_mode():
    runtime = Runtime()
    survey = publish(runtime, create(runtime, settings={"oneResponsePerPerson": True}))
    assert submit(runtime, survey, answers_for(survey))["status"] == 201
    again = submit(runtime, survey, answers_for(survey))
    assert again["status"] == 409 and again["data"]["error"] == "already_responded"
    assert submit(runtime, survey, answers_for(survey), ip="198.51.100.2")["status"] == 201
    runtime.ip = ""
    assert submit(runtime, survey, answers_for(survey))["status"] == 201
    assert submit(runtime, survey, answers_for(survey))["status"] == 201
    open_survey = publish(runtime, create(runtime))
    runtime.ip = "192.0.2.9"
    for _ in range(surveys.HOURLY_PER_RESPONDENT):
        assert submit(runtime, open_survey, answers_for(open_survey))["status"] == 201
    limited = submit(runtime, open_survey, answers_for(open_survey))
    assert limited["status"] == 429 and limited["data"]["error"] == "too_many_responses"
    runtime.clock += HOUR
    assert submit(runtime, open_survey, answers_for(open_survey))["status"] == 201


def test_schedule_and_response_limit_close_the_survey():
    runtime = Runtime()
    start = runtime.clock
    later = publish(runtime, create(runtime, settings={"opensAt": start + HOUR}))
    assert later["displayStatus"] == "scheduled"
    early = submit(runtime, later, answers_for(later))
    assert early["status"] == 409 and early["data"] == {"error": "survey_closed", "status": "scheduled"}
    capped = publish(runtime, create(runtime, settings={"maxResponses": 1, "closesAt": start + 2 * HOUR}))
    assert submit(runtime, capped, answers_for(capped))["status"] == 201
    full = submit(runtime, capped, answers_for(capped), ip="198.51.100.3")
    assert full["data"]["status"] == "closed"
    detail = call(runtime.use("GET"), f"/api/surveys/acme/items/{capped['id']}")["data"]["survey"]
    assert detail["status"] == "open" and detail["displayStatus"] == "closed"
    reopen = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{capped['id']}/open")
    assert reopen["data"]["error"] == "survey_response_limit_reached"
    runtime.clock += 3 * HOUR
    expired = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{later['id']}/open")
    assert expired["status"] == 200
    past = call(runtime.use("PATCH", body={"settings": {"maxResponses": 0}}),
                f"/api/surveys/acme/items/{capped['id']}")
    assert past["data"]["survey"]["displayStatus"] == "closed"
    reopen = call(runtime.use("POST", body={}), f"/api/surveys/acme/items/{capped['id']}/open")
    assert reopen["data"]["error"] == "survey_close_date_passed"


def test_responses_list_delete_and_csv_export():
    runtime = Runtime()
    survey = publish(runtime, create(runtime, title="Offsite: 2026!"))
    submit(runtime, survey, answers_for(survey, multiple=["Roadmap", "Demo"], text="@here"))
    submit(runtime, survey, answers_for(survey, single="Poor", text="Late"))
    path = f"/api/surveys/acme/items/{survey['id']}"
    listed = call(runtime.use("GET"), path + "/responses")["data"]["responses"]
    assert len(listed) == 2 and listed[0]["submittedAt"] > listed[1]["submittedAt"]
    exported = call(runtime.use("GET"), path + "/export")["data"]
    assert exported["filename"] == "survey-offsite-2026-responses.csv"
    rows = list(csv.reader(io.StringIO(exported["csv"])))
    assert rows[0] == ["Submitted (UTC)", "How was the offsite?", "Which sessions helped?",
                       "Rate the venue", "One word for it", "Anything else?"]
    assert rows[1][1:] == ["Great", "Roadmap; Demo", "4", "'@here", ""]
    assert rows[2][1] == "Poor"
    deleted = call(runtime.use("DELETE"), path + f"/responses/{listed[0]['id']}")
    assert deleted["data"]["deleted"] == listed[0]["id"]
    assert deleted["data"]["survey"]["responseCount"] == 1
    assert deleted["data"]["survey"]["results"][0]["options"][0]["count"] == 1
    again = call(runtime.use("DELETE"), path + f"/responses/{listed[0]['id']}")
    assert again["status"] == 404


def test_duplicate_delete_permissions_and_notifications():
    runtime = Runtime()
    survey = publish(runtime, create(runtime, actor="charlie"))
    submit(runtime, survey, answers_for(survey))
    submit(runtime, survey, answers_for(survey), ip="198.51.100.4")
    assert len(runtime.pings) == 1
    ping = runtime.pings[0]
    assert ping["to"] == "charlie" and ping["kind"] == "plugin_activity"
    assert ping["href"] == f"/dashboard/surveys?org=acme&survey={survey['id']}"
    assert ping["body"] == "1 response so far"
    runtime.clock += HOUR
    submit(runtime, survey, answers_for(survey), ip="198.51.100.5")
    assert len(runtime.pings) == 2 and runtime.pings[1]["body"] == "3 responses so far"
    copy = call(runtime.use("POST", actor="dana", body={}),
                f"/api/surveys/acme/items/{survey['id']}/duplicate")
    assert copy["status"] == 201
    duplicate = copy["data"]["survey"]
    assert duplicate["title"] == "Copy of Offsite feedback"
    assert duplicate["status"] == "draft" and duplicate["responseCount"] == 0
    assert [item["id"] for item in duplicate["questions"]] == [item["id"] for item in survey["questions"]]
    assert duplicate["canDelete"] is True
    listing = call(runtime.use("GET", actor="dana"), "/api/surveys/acme")["data"]["surveys"]
    assert {item["id"]: item["canDelete"] for item in listing} == {
        duplicate["id"]: True, survey["id"]: False}
    forbidden = call(runtime.use("DELETE", actor="dana"), f"/api/surveys/acme/items/{survey['id']}")
    assert forbidden["status"] == 403 and forbidden["data"]["error"] == "survey_delete_forbidden"
    removed = call(runtime.use("DELETE", actor="charlie"), f"/api/surveys/acme/items/{survey['id']}")
    assert removed["data"] == {"ok": True, "deleted": survey["id"]}
    assert not runtime.db.execute("SELECT * FROM survey_responses").fetchall()
    gone = call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token_of(survey)}")
    assert gone["status"] == 404
    assert ("charlie", "survey.delete", "survey", survey["id"]) in [entry[0] for entry in runtime.audits]


def test_access_needs_membership_install_session_and_origin():
    runtime = Runtime(installed=False)
    blocked = call(runtime.use("GET"), "/api/surveys/acme")
    assert blocked["status"] == 409 and blocked["data"]["canInstall"] is True
    member = call(runtime.use("GET", actor="charlie"), "/api/surveys/acme")
    assert member["data"]["canInstall"] is False
    assert call(runtime.use("GET", actor="mallory"), "/api/surveys")["status"] == 401
    runtime = Runtime()
    assert call(runtime.use("GET"), "/api/surveys/globex")["status"] == 404
    assert call(runtime.use("PUT"), "/api/surveys/acme/items")["status"] == 405
    assert call(runtime.use("GET"), "/api/surveys/acme/items/" + "a" * 32 + "/open")["status"] == 405
    assert call(runtime.use("GET"), "/api/surveys/acme/items/" + "a" * 32)["status"] == 404
    assert call(runtime.use("GET"), "/api/surveys/acme/nope")["status"] == 404
    survey = publish(runtime, create(runtime))
    runtime.origin_ok = False
    assert call(runtime.use("POST", body={}), "/api/surveys/acme/items")["status"] == 403
    cross = submit(runtime, survey, answers_for(survey))
    assert cross["status"] == 403
    assert call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token_of(survey)}")["status"] == 200
    runtime.origin_ok = True
    runtime.db.execute("DELETE FROM org_plugin_installs")
    assert call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token_of(survey)}")["status"] == 404


def test_lifecycle_exports_and_purges_all_rows():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    submit(runtime, survey, answers_for(survey))
    exported = asyncio.run(surveys.export_org_data(runtime, "org:acme", "acme"))
    assert exported["format"] == "forkmesh-surveys-export-v1"
    assert exported["surveys"][0]["title"] == "Offsite feedback"
    assert len(exported["surveys"][0]["questions"]) == 5
    assert len(exported["surveys"][0]["responses"]) == 1
    assert "publicUrl" not in exported["surveys"][0]
    assert asyncio.run(surveys.org_has_data(runtime, "org:acme")) is True
    asyncio.run(surveys.purge_org_data(runtime, "org:acme"))
    assert asyncio.run(surveys.org_has_data(runtime, "org:acme")) is False
    for statement in surveys.DATA_SCHEMA_DOWN:
        runtime.db.execute(statement)
    assert not runtime.db.execute(
        "SELECT name FROM sqlite_master WHERE name LIKE 'survey_%'").fetchall()


def test_package_owns_dashboard_and_public_page_contracts():
    manifest = json.loads((PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    page = (PACKAGE / "content/web/page.js").read_text(encoding="utf-8")
    view = (PACKAGE / "content/web/view.html").read_text(encoding="utf-8")
    public = (PACKAGE / "content/www/survey.js").read_text(encoding="utf-8")
    public_page = (PACKAGE / "content/www/survey.html").read_text(encoding="utf-8")
    assert manifest["worker"] == {"module": "surveys", "api": "/api/surveys"}
    assert manifest["dataLifecycle"] == {"scope": "organization"}
    assert manifest["install"] == {"bundled": True, "default": False}
    routes = {item["socket"]: item["config"]["route"] for item in manifest["contributions"]}
    assert routes == {"dashboard.page": "/dashboard/surveys", "www.page": "/survey"}
    assert 'registerPage("surveys"' in page
    assert view.count("fm-btn-cta") == 1
    for marker in ("data-survey-install", "data-survey-uninstall", "data-survey-export"):
        assert marker in view
    for marker in set(re.findall(r"\[(data-survey-[a-z-]+)", page)):
        assert marker in view or re.search(re.escape(marker) + r'[= >]', page), marker
    for marker in set(re.findall(r'"\[(data-sv-[a-z-]+)', public)):
        assert marker in public_page, marker
    assert "/api/surveys/-/public/" in public
    assert "innerHTML" not in public
    assert "http://" not in page + public


def test_tracking_accepts_known_events_and_drops_the_rest():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    first = survey["questions"][0]["id"]
    for kind, extra in (("view", {}), ("start", {}), ("answer", {"detail": first}),
                        ("submit", {"junk": "x" * 5000}), ("link", {"detail": "forkmesh"}),
                        ("leave", {"detail": "3"})):
        assert track(runtime, survey, kind, **extra)["status"] == 202, kind
    for kind, extra in (("click", {}), ("answer", {"detail": "deadbeef"}), ("answer", {}),
                        ("link", {"detail": "https://evil.example"}), ("leave", {"detail": "999"}),
                        ("view", {"detail": "hello"}), ("", {})):
        refused = track(runtime, survey, kind, **extra)
        assert refused["status"] == 400 and refused["data"]["error"] == "invalid_event", (kind, extra)
    rows = [dict(row) for row in runtime.db.execute(
        "SELECT kind,detail,visitor_key FROM survey_visits ORDER BY created_at")]
    assert [row["kind"] for row in rows] == ["view", "start", "answer", "submit", "link", "leave"]
    assert rows[2]["detail"] == first and rows[5]["detail"] == "3"
    assert len({row["visitor_key"] for row in rows}) == 1 and len(rows[0]["visitor_key"]) == 32
    assert runtime.ip not in json.dumps(rows)
    respondent = hashlib.sha256(("survey-respondent:%s:%s" % (survey["id"], runtime.ip)).encode()).hexdigest()[:32]
    assert rows[0]["visitor_key"] != respondent
    draft = create(runtime)
    hidden = call(runtime.use("POST", actor="", body={"kind": "view"}),
                  f"/api/surveys/-/public/{draft['id']}.{'0' * 24}/track")
    assert hidden["status"] == 404
    assert call(runtime.use("GET", actor=""), f"/api/surveys/-/public/{token_of(survey)}/track")["status"] == 405
    runtime.origin_ok = False
    assert track(runtime, survey, "view")["status"] == 403


def test_tracking_rate_limits_per_connection_and_survey():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    for _ in range(surveys.VISITOR_HOURLY_EVENTS):
        assert track(runtime, survey, "start")["status"] == 202
    limited = track(runtime, survey, "start")
    assert limited["status"] == 429 and limited["data"]["error"] == "rate_limited"
    runtime.ip = "198.51.100.8"
    assert track(runtime, survey, "start")["status"] == 202
    runtime.ip = "203.0.113.7"
    runtime.clock += HOUR
    assert track(runtime, survey, "start")["status"] == 202
    original = surveys.SURVEY_HOURLY_EVENTS
    surveys.SURVEY_HOURLY_EVENTS = 2
    try:
        runtime.ip = "192.0.2.50"
        assert track(runtime, survey, "start")["status"] == 202
        assert track(runtime, survey, "start", session="cd" * 16)["data"]["error"] == "rate_limited"
    finally:
        surveys.SURVEY_HOURLY_EVENTS = original


def test_visit_pings_once_per_visitor_an_hour_and_upgrade_when_they_start():
    runtime = Runtime()
    survey = publish(runtime, create(runtime, actor="charlie"))
    track(runtime, survey, "view")
    track(runtime, survey, "view")
    assert len(runtime.pings) == 1
    ping = runtime.pings[0]
    assert ping["to"] == "charlie" and ping["kind"] == "plugin_activity" and ping["push"] is True
    assert ping["title"] == "👀 Someone opened your survey ‘Offsite feedback’"
    assert ping["body"] == "0 responses so far"
    assert ping["href"] == f"/dashboard/surveys?org=acme&survey={survey['id']}&tab=activity"
    track(runtime, survey, "start")
    track(runtime, survey, "answer", detail=survey["questions"][0]["id"])
    assert len(runtime.pings) == 2
    upgrade = runtime.pings[1]
    assert upgrade["title"] == "✍️ Someone started your survey ‘Offsite feedback’"
    assert upgrade["dedupe"] == ping["dedupe"] and upgrade["push"] is False
    track(runtime, survey, "view", session="cd" * 16)
    assert len(runtime.pings) == 2
    runtime.ip = "198.51.100.9"
    track(runtime, survey, "start")
    assert len(runtime.pings) == 3 and runtime.pings[2]["push"] is True
    assert runtime.pings[2]["title"].startswith("✍️ Someone started")
    runtime.clock += HOUR
    runtime.ip = "203.0.113.7"
    track(runtime, survey, "view")
    assert len(runtime.pings) == 4
    for index in range(surveys.SURVEY_HOURLY_VISIT_PINGS):
        runtime.ip = "192.0.2.%d" % (index + 1)
        track(runtime, survey, "view")
    assert len(runtime.pings) == 3 + surveys.SURVEY_HOURLY_VISIT_PINGS
    runtime.clock += HOUR
    quiet = call(runtime.use("PATCH", actor="charlie", body={"settings": {"notifyOnVisit": False}}),
                 f"/api/surveys/acme/items/{survey['id']}")
    assert quiet["data"]["survey"]["settings"]["notifyOnVisit"] is False
    assert quiet["data"]["survey"]["settings"]["notifyOnResponse"] is True
    runtime.ip = "198.51.100.30"
    track(runtime, survey, "view")
    assert len(runtime.pings) == 3 + surveys.SURVEY_HOURLY_VISIT_PINGS


def test_privacy_mode_uses_the_page_session_and_never_pings_without_one():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    runtime.ip = ""
    track(runtime, survey, "view", session="cd" * 16)
    track(runtime, survey, "view", session="ef" * 16)
    track(runtime, survey, "view", session="")
    keys = [row[0] for row in runtime.db.execute("SELECT visitor_key FROM survey_visits")]
    assert len(set(keys)) == 3 and "" in keys
    assert len(runtime.pings) == 2


def test_survey_detail_summarises_activity():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    assert detail_of(runtime, survey)["activity"]["views"] == 0
    ids = [question["id"] for question in survey["questions"]]
    track(runtime, survey, "view")
    track(runtime, survey, "start")
    track(runtime, survey, "answer", detail=ids[0])
    track(runtime, survey, "submit")
    submit(runtime, survey, answers_for(survey))
    runtime.ip = "198.51.100.40"
    track(runtime, survey, "view")
    track(runtime, survey, "leave", detail="0")
    runtime.ip = "198.51.100.41"
    track(runtime, survey, "view")
    track(runtime, survey, "leave", detail="2")
    activity = detail_of(runtime, survey)["activity"]
    assert {key: activity[key] for key in ("days", "views", "visitors", "interactions", "starts",
                                          "submits", "leaves")} == {
        "days": 30, "views": 3, "visitors": 3, "interactions": 3, "starts": 1, "submits": 1, "leaves": 2}
    assert activity["lastActivityAt"] > 0
    assert [item["label"] for item in activity["recent"]] == [
        "Left after answering 2 questions", "Opened the survey", "Left without answering",
        "Opened the survey", "Sent their answers", "Answered “How was the offsite?”",
        "Started answering", "Opened the survey"]
    overview = call(runtime.use("GET"), "/api/surveys/acme")["data"]
    assert "activity" not in overview["surveys"][0]
    runtime.clock += 31 * 24 * HOUR
    later = detail_of(runtime, survey)["activity"]
    assert later["views"] == 0 and len(later["recent"]) == 8


def test_visits_are_pruned_and_removed_with_the_survey():
    runtime = Runtime()
    survey = publish(runtime, create(runtime))
    for index in range(5):
        runtime.ip = "192.0.2.%d" % index
        track(runtime, survey, "view")
    original = surveys.MAX_VISITS
    surveys.MAX_VISITS = 3
    try:
        track(runtime, survey, "view")
    finally:
        surveys.MAX_VISITS = original
    assert runtime.db.execute("SELECT COUNT(*) FROM survey_visits").fetchone()[0] == 3
    runtime.clock += surveys.VISIT_RETAIN_MS + 1
    track(runtime, survey, "view")
    assert runtime.db.execute("SELECT COUNT(*) FROM survey_visits").fetchone()[0] == 1
    exported = asyncio.run(surveys.export_org_data(runtime, "org:acme", "acme"))
    assert exported["surveys"][0]["activity"]["views"] == 1
    call(runtime.use("DELETE"), f"/api/surveys/acme/items/{survey['id']}")
    assert runtime.db.execute("SELECT COUNT(*) FROM survey_visits").fetchone()[0] == 0
    other = publish(runtime, create(runtime))
    track(runtime, other, "view")
    asyncio.run(surveys.purge_org_data(runtime, "org:acme"))
    assert runtime.db.execute("SELECT COUNT(*) FROM survey_visits").fetchone()[0] == 0


def test_otf_feedback_survey_creates_unchanged_and_takes_answers():
    body = json.loads((REPO / "docs/finance/grants/otf-survey.json").read_text(encoding="utf-8"))
    runtime = Runtime()
    response = call(runtime.use("POST", body=body), "/api/surveys/acme/items")
    assert response["status"] == 201, response
    survey = response["data"]["survey"]
    assert survey["title"] == body["title"] and survey["description"] == body["description"]
    assert survey["thankYou"] == body["thankYou"]
    assert survey["settings"] == surveys.DEFAULT_SETTINGS
    expected = [{"help": "", "required": False, "options": [], "scale": 0, **question}
                for question in body["questions"]]
    assert survey["questions"] == expected
    assert [question["id"] for question in expected] == ["%08x" % (0x0f000000 + n) for n in range(1, 28)]
    assert [question["id"] for question in expected if question["required"]] == ["0f000001"]
    survey = publish(runtime, survey)
    refused = submit(runtime, survey, {"0f000007": 4})
    assert refused["data"]["error"] == "answer_required"
    answered = submit(runtime, survey, {
        "0f000001": expected[0]["options"][0], "0f000003": expected[2]["options"][:2],
        "0f000007": 5, "0f00001b": "Mirrors please."})
    assert answered["status"] == 201, answered
