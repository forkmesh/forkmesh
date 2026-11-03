from datetime import datetime, timezone
import csv
import hmac
import io
import ipaddress
import re


PREFIX = "/api/surveys"
PLUGIN_ID = "surveys"
REQUEST_PROOF = "forkmesh-surveys-request-v1"
PAGE_PATH = "/dashboard/surveys"
MAX_BODY_BYTES = 256 * 1024
MAX_SURVEYS = 500
MAX_LISTED = 500
MAX_QUESTIONS = 50
MAX_OPTIONS = 30
MAX_RESPONSES = 10000
MAX_RESPONSES_LISTED = 2000
MAX_RESULT_ROWS = 2000
MAX_TEXT_ANSWERS = 20
HOURLY_PER_RESPONDENT = 10
SOURCE_HOURLY_RESPONSES = 60
SOURCE_HOURLY_CHARS = 4 * 1024 * 1024
MAX_ORG_RESPONSE_CHARS = 32 * 1024 * 1024
MAX_VISITS = 1000
MAX_RECENT_VISITS = 50
VISIT_RETAIN_MS = 90 * 24 * 60 * 60 * 1000
VISIT_SUMMARY_DAYS = 30
VISITOR_HOURLY_EVENTS = 120
SURVEY_HOURLY_EVENTS = 1500
SURVEY_HOURLY_VISIT_PINGS = 10
TRACK_KINDS = ("view", "start", "answer", "submit", "link", "leave")
TRACK_START_KINDS = frozenset({"start", "answer"})
TRACK_LINKS = frozenset({"forkmesh"})
MANAGER_ROLES = frozenset({"owner", "admin"})
ORG_READ_ONLY_ROLES = frozenset({"auditor"})
STATUSES = ("draft", "open", "closed")
QUESTION_TYPES = ("single", "multiple", "text", "paragraph", "rating")
CHOICE_TYPES = frozenset({"single", "multiple"})
TEXT_LIMITS = {"text": 500, "paragraph": 5000}
LINK_ORIGIN = "https://app.forkmesh.com"
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS
ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
QID_RE = re.compile(r"^[0-9a-f]{8}$")
TOKEN_RE = re.compile(r"^([0-9a-f]{32})\.([0-9a-f]{24})$")
SESSION_RE = re.compile(r"^[0-9a-f]{32}$")
COUNT_RE = re.compile(r"^(?:0|[1-9]\d?)$")
ORIGIN_RE = re.compile(r"^https://[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?(?::[0-9]{1,5})?$")
DEFAULT_SETTINGS = {
    "oneResponsePerPerson": False,
    "opensAt": 0,
    "closesAt": 0,
    "maxResponses": 0,
    "notifyOnResponse": True,
    "notifyOnVisit": True,
    "shuffleOptions": False,
}
DEFAULT_THANK_YOU = "Thank you for answering."


DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS survey_forms (
        survey_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('draft','open','closed')),
        question_count INTEGER NOT NULL DEFAULT 0 CHECK(question_count >= 0),
        response_count INTEGER NOT NULL DEFAULT 0 CHECK(response_count >= 0),
        last_response_at INTEGER NOT NULL DEFAULT 0 CHECK(last_response_at >= 0),
        published_at INTEGER NOT NULL DEFAULT 0 CHECK(published_at >= 0),
        closed_at INTEGER NOT NULL DEFAULT 0 CHECK(closed_at >= 0),
        notified_at INTEGER NOT NULL DEFAULT 0 CHECK(notified_at >= 0),
        created_by_bi TEXT NOT NULL,
        created_by_name TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_survey_forms_org_created "
    "ON survey_forms(org_bi,created_at DESC,survey_id)",
    """CREATE TRIGGER IF NOT EXISTS trg_survey_forms_limit
        BEFORE INSERT ON survey_forms
        WHEN (SELECT COUNT(*) FROM survey_forms WHERE org_bi=NEW.org_bi)>=500
        BEGIN SELECT RAISE(ABORT,'survey_catalog_full'); END""",
    """CREATE TABLE IF NOT EXISTS survey_responses (
        response_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        survey_id TEXT NOT NULL REFERENCES survey_forms(survey_id) ON DELETE CASCADE,
        respondent_key TEXT NOT NULL DEFAULT '',
        submitted_at INTEGER NOT NULL CHECK(submitted_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_survey_responses_survey "
    "ON survey_responses(survey_id,submitted_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_survey_responses_respondent "
    "ON survey_responses(survey_id,respondent_key,submitted_at) WHERE respondent_key<>''",
    "CREATE INDEX IF NOT EXISTS idx_survey_responses_org "
    "ON survey_responses(org_bi,submitted_at DESC)",
    """CREATE TRIGGER IF NOT EXISTS trg_survey_responses_limit
        BEFORE INSERT ON survey_responses
        WHEN (SELECT COUNT(*) FROM survey_responses WHERE survey_id=NEW.survey_id)>=10000
        BEGIN SELECT RAISE(ABORT,'survey_responses_full'); END""",
    """CREATE TABLE IF NOT EXISTS survey_visits (
        visit_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        survey_id TEXT NOT NULL REFERENCES survey_forms(survey_id) ON DELETE CASCADE,
        visitor_key TEXT NOT NULL DEFAULT '',
        kind TEXT NOT NULL,
        detail TEXT NOT NULL DEFAULT '',
        pinged INTEGER NOT NULL DEFAULT 0 CHECK(pinged IN (0,1,2)),
        created_at INTEGER NOT NULL CHECK(created_at >= 0))""",
    "CREATE INDEX IF NOT EXISTS idx_survey_visits_survey "
    "ON survey_visits(survey_id,created_at DESC)",
    "CREATE INDEX IF NOT EXISTS idx_survey_visits_visitor "
    "ON survey_visits(survey_id,visitor_key,created_at)",
    "CREATE INDEX IF NOT EXISTS idx_survey_visits_org ON survey_visits(org_bi,created_at)",
    """CREATE TABLE IF NOT EXISTS survey_usage (
        org_bi TEXT PRIMARY KEY,
        chars INTEGER NOT NULL DEFAULT 0 CHECK(chars >= 0))""",
    """CREATE TABLE IF NOT EXISTS survey_sources (
        bucket TEXT PRIMARY KEY,
        responses INTEGER NOT NULL DEFAULT 0 CHECK(responses >= 0),
        chars INTEGER NOT NULL DEFAULT 0 CHECK(chars >= 0),
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_survey_sources_expires ON survey_sources(expires_at)",
]
DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS survey_visits",
    "DROP INDEX IF EXISTS idx_survey_sources_expires",
    "DROP TABLE IF EXISTS survey_sources",
    "DROP TABLE IF EXISTS survey_usage",
    "DROP TABLE IF EXISTS survey_responses",
    "DROP TABLE IF EXISTS survey_forms",
]


FORM_COLUMNS = (
    "survey_id,org_name,status,question_count,response_count,last_response_at,"
    "published_at,closed_at,notified_at,created_by_bi,created_by_name,updated_by_name,"
    "created_at,updated_at,data"
)

_SCHEMA_READY = False


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _text(value, limit):
    return str(value or "").replace("\x00", "").replace("\r\n", "\n")[:limit].strip()


def _integer(value, default=0):
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _stamp(value, default=0):
    stamp = _integer(value, default)
    return stamp if 0 <= stamp < 10 ** 14 else default


def _iso(stamp):
    if not stamp:
        return ""
    return datetime.fromtimestamp(stamp / 1000, timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("organizations",)
    match = re.fullmatch(r"/api/surveys/-/public/([0-9a-f]{32}\.[0-9a-f]{24})(/track)?", clean)
    if match:
        return ("track" if match.group(2) else "public", match.group(1))
    match = re.fullmatch(
        r"/api/surveys/([a-z][a-z0-9-]{0,62})"
        r"(?:/(items)(?:/([0-9a-f]{32})"
        r"(?:/(open|close|duplicate|export|responses)(?:/([0-9a-f]{32}))?)?)?)?",
        clean,
    )
    if not match:
        return None
    action, response_id = match.group(4) or "", match.group(5) or ""
    if response_id and action != "responses":
        return None
    return ("organization", match.group(1), match.group(2) or "", match.group(3) or "",
            action, response_id)


def _settings(value, existing=None):
    result = dict(DEFAULT_SETTINGS)
    for key in DEFAULT_SETTINGS:
        if isinstance(existing, dict) and key in existing:
            result[key] = existing[key]
    value = value if isinstance(value, dict) else {}
    for key in ("oneResponsePerPerson", "notifyOnResponse", "notifyOnVisit", "shuffleOptions"):
        if key in value:
            result[key] = value.get(key) is True
    for key in ("opensAt", "closesAt"):
        if key in value:
            result[key] = _stamp(value.get(key))
    if "maxResponses" in value:
        result["maxResponses"] = min(MAX_RESPONSES, max(0, _integer(value.get("maxResponses"))))
    for key in ("oneResponsePerPerson", "notifyOnResponse", "notifyOnVisit", "shuffleOptions"):
        result[key] = result[key] is True
    for key in ("opensAt", "closesAt", "maxResponses"):
        result[key] = _integer(result[key])
    return result


def _new_question_id(runtime, taken):
    for _ in range(20):
        candidate = str(runtime.new_id())[-8:]
        if QID_RE.fullmatch(candidate) and candidate not in taken:
            return candidate
    return next("%08x" % number for number in range(MAX_QUESTIONS + 1)
                if "%08x" % number not in taken)


def _options(value):
    options = []
    for item in value if isinstance(value, list) else []:
        label = _clean(item, 200)
        if label and label not in options:
            options.append(label)
    return options[:MAX_OPTIONS]


def _question(value, existing_type=""):
    value = value if isinstance(value, dict) else {}
    kind = _clean(value.get("type") or existing_type or "single", 20).lower()
    title = _clean(value.get("title"), 300)
    question = {
        "id": _clean(value.get("id"), 8).lower(),
        "type": kind,
        "title": title,
        "help": _text(value.get("help"), 1000),
        "required": value.get("required") is True,
        "options": _options(value.get("options")) if kind in CHOICE_TYPES else [],
        "scale": 0,
    }
    if kind == "rating":
        question["scale"] = min(10, max(3, _integer(value.get("scale"), 5)))
    return question


def _questions(runtime, value):
    if not isinstance(value, list):
        return None, "invalid_questions", -1
    questions, taken = [], set()
    for index, item in enumerate(value):
        question = _question(item)
        if not question["title"]:
            continue
        if question["type"] not in QUESTION_TYPES:
            return None, "invalid_question_type", index
        if question["type"] in CHOICE_TYPES and len(question["options"]) < 2:
            return None, "invalid_question_options", index
        if not QID_RE.fullmatch(question["id"]) or question["id"] in taken:
            question["id"] = _new_question_id(runtime, taken)
        taken.add(question["id"])
        questions.append(question)
        if len(questions) > MAX_QUESTIONS:
            return None, "too_many_questions", index
    return questions, "", -1


def _stored_questions(value):
    questions = []
    for item in value if isinstance(value, list) else []:
        question = _question(item)
        if question["title"] and question["type"] in QUESTION_TYPES and QID_RE.fullmatch(question["id"]):
            questions.append(question)
    return questions[:MAX_QUESTIONS]


def _envelope(runtime, data, existing=None):
    envelope = dict(existing or {})
    if "title" in data or "title" not in envelope:
        envelope["title"] = _clean(data.get("title"), 200) or "Untitled survey"
    for key, limit in (("description", 4000), ("thankYou", 1000)):
        if key in data or key not in envelope:
            envelope[key] = _text(data.get(key), limit)
    if "questions" in data or "questions" not in envelope:
        questions, error, index = _questions(runtime, data.get("questions", []))
        if error:
            return None, (error, index)
        envelope["questions"] = questions
    envelope["settings"] = _settings(data.get("settings"), envelope.get("settings"))
    settings = envelope["settings"]
    if settings["opensAt"] and settings["closesAt"] and settings["closesAt"] <= settings["opensAt"]:
        return None, ("invalid_schedule", -1)
    return envelope, None


def _envelope_error(runtime, error):
    code, index = error
    payload = {"error": code}
    if index >= 0:
        payload["question"] = index
    return _response(runtime, payload, 400)


async def _open_data(runtime, row):
    data = await runtime.open(row.get("data"))
    return data if isinstance(data, dict) else {}


def _display_status(row, settings, now):
    status = str(row.get("status") or "draft")
    if status != "open":
        return status
    if settings["opensAt"] and now < settings["opensAt"]:
        return "scheduled"
    if settings["closesAt"] and now >= settings["closesAt"]:
        return "closed"
    if settings["maxResponses"] and _integer(row.get("response_count")) >= settings["maxResponses"]:
        return "closed"
    return "open"


async def _link_token(runtime, survey_id):
    return "%s.%s" % (survey_id, (await runtime.blind("survey-link:" + survey_id))[:24])


async def _token_survey(runtime, token):
    match = TOKEN_RE.fullmatch(str(token or ""))
    if not match:
        return ""
    survey_id = match.group(1)
    if not hmac.compare_digest(await _link_token(runtime, survey_id), token):
        return ""
    return survey_id


def _link_origin(runtime):
    secret = getattr(runtime, "package_secret", None)
    configured = str(secret("LINK_ORIGIN") if secret else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(configured):
        return configured
    origin = str(getattr(runtime, "public_origin", lambda: "")() or "").rstrip("/").lower()
    return origin if ORIGIN_RE.fullmatch(origin) else LINK_ORIGIN


async def _row(runtime, org_bi, survey_id):
    return await runtime.d1_first(
        "SELECT " + FORM_COLUMNS + " FROM survey_forms WHERE org_bi=? AND survey_id=?",
        org_bi, survey_id)


async def _project(runtime, row, now, viewer, origin=""):
    data = await _open_data(runtime, row)
    settings = _settings(data.get("settings"))
    status = str(row.get("status") or "draft")
    survey_id = str(row.get("survey_id") or "")
    public_url = ""
    if origin and status != "draft":
        public_url = "%s/survey?t=%s" % (origin, await _link_token(runtime, survey_id))
    account_bi, role = viewer
    return {
        "id": survey_id,
        "organization": str(row.get("org_name") or ""),
        "title": str(data.get("title") or "Untitled survey"),
        "description": str(data.get("description") or ""),
        "thankYou": str(data.get("thankYou") or ""),
        "status": status,
        "displayStatus": _display_status(row, settings, now),
        "settings": settings,
        "questionCount": _integer(row.get("question_count")),
        "responseCount": _integer(row.get("response_count")),
        "lastResponseAt": _integer(row.get("last_response_at")),
        "publishedAt": _integer(row.get("published_at")),
        "closedAt": _integer(row.get("closed_at")),
        "createdBy": str(row.get("created_by_name") or ""),
        "updatedBy": str(row.get("updated_by_name") or ""),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
        "canDelete": role in MANAGER_ROLES or str(row.get("created_by_bi") or "") == account_bi,
        "publicUrl": public_url,
    }, data


async def _surveys(runtime, org_bi, now, viewer, origin=""):
    rows = await runtime.d1_all(
        "SELECT " + FORM_COLUMNS + " FROM survey_forms WHERE org_bi=? "
        "ORDER BY created_at DESC,survey_id LIMIT ?", org_bi, MAX_LISTED)
    surveys = []
    for row in rows or []:
        survey, _ = await _project(runtime, row, now, viewer, origin)
        surveys.append(survey)
    return surveys


def _choice_answer(question, value):
    if value in (None, ""):
        return None, ""
    if not isinstance(value, str) or _clean(value, 200) not in question["options"]:
        return None, "invalid_answer"
    return _clean(value, 200), ""


def _multiple_answer(question, value):
    if value in (None, ""):
        return None, ""
    if not isinstance(value, list):
        return None, "invalid_answer"
    chosen = set()
    for item in value:
        label = _clean(item, 200) if isinstance(item, str) else None
        if label not in question["options"]:
            return None, "invalid_answer"
        chosen.add(label)
    picked = [label for label in question["options"] if label in chosen]
    return (picked or None), ""


def _rating_answer(question, value):
    if value in (None, ""):
        return None, ""
    if isinstance(value, bool):
        return None, "invalid_answer"
    if isinstance(value, float) and value != int(value):
        return None, "invalid_answer"
    number = _integer(value, -1)
    if not 1 <= number <= question["scale"]:
        return None, "invalid_answer"
    return number, ""


def _text_answer(question, value):
    if value in (None, ""):
        return None, ""
    if not isinstance(value, str):
        return None, "invalid_answer"
    text = _text(value, TEXT_LIMITS[question["type"]])
    if question["type"] == "text":
        text = _clean(text, TEXT_LIMITS["text"])
    return (text or None), ""


def _answer(question, value):
    kind = question["type"]
    if kind == "single":
        return _choice_answer(question, value)
    if kind == "multiple":
        return _multiple_answer(question, value)
    if kind == "rating":
        return _rating_answer(question, value)
    return _text_answer(question, value)


def _answers(questions, value):
    if not isinstance(value, dict):
        return None, "invalid_answers", ""
    answers = {}
    for question in questions:
        answer, error = _answer(question, value.get(question["id"]))
        if error:
            return None, error, question["id"]
        if answer is None:
            if question["required"]:
                return None, "answer_required", question["id"]
            continue
        answers[question["id"]] = answer
    if not answers:
        return None, "empty_response", ""
    return answers, "", ""


def _fits(question, value):
    if question["type"] == "multiple" and isinstance(value, list):
        chosen = {item for item in value if isinstance(item, str)}
        return [label for label in question["options"] if label in chosen] or None
    answer, error = _answer(question, value)
    return None if error else answer


def _results(questions, answer_sets):
    total = len(answer_sets)
    results = []
    for question in questions:
        kind = question["type"]
        entry = {
            "id": question["id"], "type": kind, "title": question["title"],
            "answered": 0, "skipped": 0, "options": [], "average": 0,
            "distribution": [], "answers": [],
        }
        counts = {label: 0 for label in question["options"]}
        distribution = [0] * question["scale"]
        ratings = []
        for answers in answer_sets:
            value = _fits(question, answers.get(question["id"]))
            if value is None:
                continue
            entry["answered"] += 1
            if kind == "single":
                counts[value] += 1
            elif kind == "multiple":
                for label in value:
                    counts[label] += 1
            elif kind == "rating":
                distribution[value - 1] += 1
                ratings.append(value)
            elif len(entry["answers"]) < MAX_TEXT_ANSWERS:
                entry["answers"].append(value)
        entry["skipped"] = total - entry["answered"]
        if kind in CHOICE_TYPES:
            entry["options"] = [{"label": label, "count": counts[label]}
                                for label in question["options"]]
        if kind == "rating":
            entry["distribution"] = distribution
            entry["average"] = round(sum(ratings) / len(ratings), 1) if ratings else 0
        results.append(entry)
    return results


async def _response_rows(runtime, survey_id, limit):
    return await runtime.d1_all(
        "SELECT response_id,submitted_at,data FROM survey_responses WHERE survey_id=? "
        "ORDER BY submitted_at DESC,response_id DESC LIMIT ?", survey_id, limit)


async def _responses(runtime, survey_id, limit=MAX_RESPONSES_LISTED):
    responses = []
    for row in await _response_rows(runtime, survey_id, limit) or []:
        data = await runtime.open(row.get("data"))
        answers = (data if isinstance(data, dict) else {}).get("answers")
        responses.append({
            "id": str(row.get("response_id") or ""),
            "submittedAt": _integer(row.get("submitted_at")),
            "answers": answers if isinstance(answers, dict) else {},
        })
    return responses


async def _full_survey(runtime, org_bi, survey_id, viewer):
    row = await _row(runtime, org_bi, survey_id)
    survey, data = await _project(runtime, row, runtime.now(), viewer, _link_origin(runtime))
    questions = _stored_questions(data.get("questions"))
    responses = await _responses(runtime, survey_id, MAX_RESULT_ROWS)
    survey["questions"] = questions
    survey["results"] = _results(questions, [item["answers"] for item in responses])
    survey["resultsCounted"] = len(responses)
    survey["activity"] = await _activity(runtime, survey_id, questions, runtime.now())
    return survey


async def _detail(runtime, org_bi, survey_id, viewer, status=200):
    return _response(runtime, {"ok": True, "survey": await _full_survey(
        runtime, org_bi, survey_id, viewer)}, status)


async def _summary(runtime, org_bi, surveys, now):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM survey_responses WHERE org_bi=? AND submitted_at>=?",
        org_bi, now - 7 * DAY_MS)
    statuses = [item["displayStatus"] for item in surveys]
    return {
        "surveys": len(surveys),
        "open": statuses.count("open"),
        "drafts": statuses.count("draft"),
        "closed": statuses.count("closed"),
        "responses": sum(item["responseCount"] for item in surveys),
        "responsesLast7Days": _integer((row or {}).get("n")),
    }


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _organizations(runtime, account_bi):
    rows = await runtime.d1_all(
        "SELECT o.name,m.role,p.plugin AS installed FROM org_members m "
        "JOIN orgs o ON o.org_bi=m.org_bi LEFT JOIN org_plugin_installs p "
        "ON p.org_bi=o.org_bi AND p.plugin=? WHERE m.member_bi=? "
        "ORDER BY o.name COLLATE NOCASE LIMIT 200",
        PLUGIN_ID,
        account_bi,
    )
    return [
        {
            "name": str(row.get("name") or ""),
            "role": str(row.get("role") or "member"),
            "installed": bool(row.get("installed")),
        }
        for row in rows or []
    ]


async def _overview(runtime, org_bi, org_name, viewer):
    now = runtime.now()
    surveys = await _surveys(runtime, org_bi, now, viewer, _link_origin(runtime))
    role = viewer[1]
    return _response(runtime, {
        "ok": True,
        "organization": org_name,
        "role": role,
        "canManagePlugin": role in MANAGER_ROLES,
        "surveys": surveys,
        "summary": await _summary(runtime, org_bi, surveys, now),
    })


def _full_error(error, code):
    return code in str(error)


async def _insert(runtime, org_bi, org_name, account_bi, actor, envelope):
    now = runtime.now()
    survey_id = runtime.new_id()
    try:
        await runtime.d1_run(
            "INSERT INTO survey_forms(survey_id,org_bi,org_name,status,question_count,"
            "created_by_bi,created_by_name,updated_by_name,created_at,updated_at,data) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?)",
            survey_id, org_bi, org_name, "draft", len(envelope["questions"]),
            account_bi, actor, actor, now, now, await runtime.seal(envelope))
    except Exception as error:
        if _full_error(error, "survey_catalog_full"):
            return "", _response(runtime, {"error": "survey_catalog_full"}, 409)
        raise
    return survey_id, None


async def _create(runtime, org_bi, org_name, viewer, actor, data):
    envelope, error = _envelope(runtime, data)
    if error:
        return _envelope_error(runtime, error)
    if "thankYou" not in data:
        envelope["thankYou"] = DEFAULT_THANK_YOU
    survey_id, failure = await _insert(runtime, org_bi, org_name, viewer[0], actor, envelope)
    if failure:
        return failure
    await runtime.audit(actor, "survey.create", "survey", survey_id)
    return await _detail(runtime, org_bi, survey_id, viewer, 201)


async def _update(runtime, row, org_bi, viewer, actor, data):
    envelope, error = _envelope(runtime, data, await _open_data(runtime, row))
    if error:
        return _envelope_error(runtime, error)
    if row.get("status") != "draft" and not envelope["questions"]:
        return _response(runtime, {"error": "survey_has_no_questions"}, 400)
    await runtime.d1_run(
        "UPDATE survey_forms SET question_count=?,updated_by_name=?,updated_at=?,data=? "
        "WHERE org_bi=? AND survey_id=?",
        len(envelope["questions"]), actor, runtime.now(), await runtime.seal(envelope),
        org_bi, row["survey_id"])
    await runtime.audit(actor, "survey.update", "survey", row["survey_id"])
    return await _detail(runtime, org_bi, row["survey_id"], viewer)


async def _open(runtime, row, org_bi, viewer, actor):
    data = await _open_data(runtime, row)
    if not _stored_questions(data.get("questions")):
        return _response(runtime, {"error": "survey_has_no_questions"}, 400)
    settings = _settings(data.get("settings"))
    now = runtime.now()
    if settings["closesAt"] and settings["closesAt"] <= now:
        return _response(runtime, {"error": "survey_close_date_passed"}, 409)
    if settings["maxResponses"] and _integer(row.get("response_count")) >= settings["maxResponses"]:
        return _response(runtime, {"error": "survey_response_limit_reached"}, 409)
    if row.get("status") != "open":
        await runtime.d1_run(
            "UPDATE survey_forms SET status='open',published_at=CASE WHEN published_at=0 "
            "THEN ? ELSE published_at END,closed_at=0,updated_by_name=?,updated_at=? "
            "WHERE org_bi=? AND survey_id=?",
            now, actor, now, org_bi, row["survey_id"])
        await runtime.audit(actor, "survey.open", "survey", row["survey_id"])
    return await _detail(runtime, org_bi, row["survey_id"], viewer)


async def _close(runtime, row, org_bi, viewer, actor):
    if row.get("status") == "draft":
        return _response(runtime, {"error": "survey_not_open"}, 409)
    if row.get("status") == "open":
        now = runtime.now()
        await runtime.d1_run(
            "UPDATE survey_forms SET status='closed',closed_at=?,updated_by_name=?,updated_at=? "
            "WHERE org_bi=? AND survey_id=?",
            now, actor, now, org_bi, row["survey_id"])
        await runtime.audit(actor, "survey.close", "survey", row["survey_id"])
    return await _detail(runtime, org_bi, row["survey_id"], viewer)


async def _duplicate(runtime, row, org_bi, org_name, viewer, actor):
    data = await _open_data(runtime, row)
    envelope = {
        "title": _clean("Copy of " + str(data.get("title") or "Untitled survey"), 200),
        "description": _text(data.get("description"), 4000),
        "thankYou": _text(data.get("thankYou"), 1000),
        "questions": _stored_questions(data.get("questions")),
        "settings": _settings(data.get("settings")),
    }
    survey_id, failure = await _insert(runtime, org_bi, org_name, viewer[0], actor, envelope)
    if failure:
        return failure
    await runtime.audit(actor, "survey.duplicate", "survey", survey_id)
    return await _detail(runtime, org_bi, survey_id, viewer, 201)


async def _delete(runtime, row, org_bi, viewer, actor):
    account_bi, role = viewer
    if role not in MANAGER_ROLES and str(row.get("created_by_bi") or "") != account_bi:
        return _response(runtime, {"error": "survey_delete_forbidden"}, 403)
    await runtime.d1_run("DELETE FROM survey_visits WHERE survey_id=?", row["survey_id"])
    await runtime.d1_run("DELETE FROM survey_responses WHERE survey_id=?", row["survey_id"])
    await runtime.d1_run(
        "DELETE FROM survey_forms WHERE org_bi=? AND survey_id=?", org_bi, row["survey_id"])
    await _recount_usage(runtime, org_bi)
    await runtime.audit(actor, "survey.delete", "survey", row["survey_id"])
    return _response(runtime, {"ok": True, "deleted": row["survey_id"]})


async def _recount(runtime, survey_id):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n,COALESCE(MAX(submitted_at),0) AS last "
        "FROM survey_responses WHERE survey_id=?", survey_id)
    await runtime.d1_run(
        "UPDATE survey_forms SET response_count=?,last_response_at=? WHERE survey_id=?",
        _integer((row or {}).get("n")), _integer((row or {}).get("last")), survey_id)


async def _recount_usage(runtime, org_bi):
    row = await runtime.d1_first(
        "SELECT COALESCE(SUM(LENGTH(data)),0) AS chars FROM survey_responses WHERE org_bi=?", org_bi)
    chars = _integer((row or {}).get("chars"))
    await runtime.d1_run(
        "INSERT INTO survey_usage(org_bi,chars) VALUES(?,?) "
        "ON CONFLICT(org_bi) DO UPDATE SET chars=excluded.chars", org_bi, chars)
    return chars


async def _org_chars(runtime, org_bi):
    row = await runtime.d1_first("SELECT chars FROM survey_usage WHERE org_bi=?", org_bi)
    return _integer(row.get("chars")) if row else await _recount_usage(runtime, org_bi)


async def _delete_response(runtime, row, org_bi, viewer, actor, response_id):
    found = await runtime.d1_first(
        "SELECT response_id FROM survey_responses WHERE survey_id=? AND response_id=?",
        row["survey_id"], response_id)
    if not found:
        return _response(runtime, {"error": "response_not_found"}, 404)
    await runtime.d1_run(
        "DELETE FROM survey_responses WHERE survey_id=? AND response_id=?",
        row["survey_id"], response_id)
    await _recount(runtime, row["survey_id"])
    await _recount_usage(runtime, org_bi)
    await runtime.audit(actor, "survey.response.delete", "survey", row["survey_id"])
    return _response(runtime, {"ok": True, "deleted": response_id, "survey": await _full_survey(
        runtime, org_bi, row["survey_id"], viewer)})


def _csv_cell(value):
    if isinstance(value, list):
        value = "; ".join(str(item) for item in value)
    text = "" if value is None else str(value)
    if text[:1] in ("=", "+", "-", "@", "\t", "\r"):
        text = "'" + text
    return text


def _slug(title):
    slug = re.sub(r"[^a-z0-9]+", "-", str(title or "").lower()).strip("-")[:60]
    return slug or "survey"


async def _export(runtime, row, actor):
    data = await _open_data(runtime, row)
    questions = _stored_questions(data.get("questions"))
    responses = await _responses(runtime, row["survey_id"], MAX_RESPONSES)
    buffer = io.StringIO()
    writer = csv.writer(buffer, lineterminator="\r\n")
    writer.writerow(["Submitted (UTC)"] + [_csv_cell(question["title"]) for question in questions])
    for response in reversed(responses):
        writer.writerow([_iso(response["submittedAt"])] + [
            _csv_cell(response["answers"].get(question["id"])) for question in questions])
    await runtime.audit(actor, "survey.export", "survey", row["survey_id"])
    return _response(runtime, {
        "ok": True,
        "filename": "survey-%s-responses.csv" % _slug(data.get("title")),
        "csv": buffer.getvalue(),
    })


async def _public_row(runtime, token):
    survey_id = await _token_survey(runtime, token)
    if not survey_id:
        return None
    row = await runtime.d1_first(
        "SELECT org_bi," + FORM_COLUMNS + " FROM survey_forms WHERE survey_id=?", survey_id)
    if not row or row.get("status") == "draft":
        return None
    if not await runtime.plugin_installed(row.get("org_bi"), PLUGIN_ID):
        return None
    return row


async def _public(runtime, token):
    row = await _public_row(runtime, token)
    if not row:
        return _response(runtime, {"error": "survey_not_found"}, 404)
    data = await _open_data(runtime, row)
    settings = _settings(data.get("settings"))
    status = _display_status(row, settings, runtime.now())
    return _response(runtime, {"ok": True, "survey": {
        "title": str(data.get("title") or "Untitled survey"),
        "description": str(data.get("description") or ""),
        "organization": str(row.get("org_name") or ""),
        "status": status,
        "questions": _stored_questions(data.get("questions")) if status == "open" else [],
        "thankYou": str(data.get("thankYou") or DEFAULT_THANK_YOU),
        "oneResponsePerPerson": settings["oneResponsePerPerson"],
        "shuffleOptions": settings["shuffleOptions"],
        "opensAt": settings["opensAt"],
    }})


def _visitor(runtime):
    visitor = getattr(runtime, "request_visitor", None)
    return visitor() if visitor else {}


def _source(runtime):
    address = str((_visitor(runtime) or {}).get("ip") or "").strip()
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError:
        return address
    if parsed.version == 4:
        return address
    return str(parsed.ipv4_mapped or ipaddress.ip_network(address + "/64", strict=False))


async def _respondent_key(runtime, survey_id):
    source = _source(runtime)
    if not source:
        return ""
    return (await runtime.blind("survey-respondent:%s:%s" % (survey_id, source)))[:32]


async def _spend_source(runtime, size, now):
    source = _source(runtime)
    if not source:
        return True
    hour = now // HOUR_MS
    bucket = await runtime.blind("survey-source:%s:%d" % (source, hour))
    row = await runtime.d1_first("SELECT responses,chars FROM survey_sources WHERE bucket=?", bucket)
    if row and (_integer(row.get("responses")) >= SOURCE_HOURLY_RESPONSES
                or _integer(row.get("chars")) + size > SOURCE_HOURLY_CHARS):
        return False
    await runtime.d1_run("DELETE FROM survey_sources WHERE expires_at<?", now)
    await runtime.d1_run(
        "INSERT INTO survey_sources(bucket,responses,chars,expires_at) VALUES(?,1,?,?) "
        "ON CONFLICT(bucket) DO UPDATE SET responses=responses+1,chars=chars+excluded.chars",
        bucket, size, (hour + 1) * HOUR_MS)
    return True


async def _notify_creator(runtime, row, data, now):
    settings = _settings(data.get("settings"))
    notify = getattr(runtime, "notify", None)
    creator = _clean(row.get("created_by_name"), 64).lower()
    if (notify is None or not creator or not settings["notifyOnResponse"]
            or now - _integer(row.get("notified_at")) < HOUR_MS):
        return
    await runtime.d1_run(
        "UPDATE survey_forms SET notified_at=? WHERE survey_id=?", now, row["survey_id"])
    count = _integer(row.get("response_count")) + 1
    await notify(
        creator, "plugin_activity",
        "📋 New response to %s" % str(data.get("title") or "your survey"),
        body="%d %s so far" % (count, "response" if count == 1 else "responses"),
        href="%s?org=%s&survey=%s" % (PAGE_PATH, row.get("org_name"), row["survey_id"]),
        dedupe="survey-response:%s:%d" % (row["survey_id"], now // HOUR_MS))


async def _submit(runtime, token, body):
    row = await _public_row(runtime, token)
    if not row:
        return _response(runtime, {"error": "survey_not_found"}, 404)
    data = await _open_data(runtime, row)
    settings = _settings(data.get("settings"))
    now = runtime.now()
    status = _display_status(row, settings, now)
    if status != "open":
        return _response(runtime, {"error": "survey_closed", "status": status}, 409)
    answers, error, question = _answers(_stored_questions(data.get("questions")),
                                        body.get("answers"))
    if error:
        payload = {"error": error}
        if question:
            payload["question"] = question
        return _response(runtime, payload, 400)
    survey_id = row["survey_id"]
    key = await _respondent_key(runtime, survey_id)
    if key:
        if settings["oneResponsePerPerson"] and await runtime.d1_first(
                "SELECT response_id FROM survey_responses WHERE survey_id=? AND respondent_key=? "
                "LIMIT 1", survey_id, key):
            return _response(runtime, {"error": "already_responded"}, 409)
        recent = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM survey_responses WHERE survey_id=? AND respondent_key=? "
            "AND submitted_at>=?", survey_id, key, now - HOUR_MS)
        if _integer((recent or {}).get("n")) >= HOURLY_PER_RESPONDENT:
            return _response(runtime, {"error": "too_many_responses"}, 429)
    sealed = await runtime.seal({"answers": answers})
    if await _org_chars(runtime, row["org_bi"]) + len(sealed) > MAX_ORG_RESPONSE_CHARS:
        return _response(runtime, {"error": "survey_storage_full"}, 409)
    if not await _spend_source(runtime, len(sealed), now):
        return _response(runtime, {"error": "too_many_responses"}, 429)
    try:
        await runtime.d1_run(
            "INSERT INTO survey_responses(response_id,org_bi,survey_id,respondent_key,"
            "submitted_at,data) VALUES(?,?,?,?,?,?)",
            runtime.new_id(), row["org_bi"], survey_id, key, now, sealed)
    except Exception as failure:
        if _full_error(failure, "survey_responses_full"):
            return _response(runtime, {"error": "survey_closed", "status": "closed"}, 409)
        raise
    await runtime.d1_run(
        "UPDATE survey_forms SET response_count=response_count+1,last_response_at=? "
        "WHERE survey_id=?", now, survey_id)
    await runtime.d1_run(
        "UPDATE survey_usage SET chars=chars+? WHERE org_bi=?", len(sealed), row["org_bi"])
    await _notify_creator(runtime, row, data, now)
    return _response(runtime, {
        "ok": True, "thankYou": str(data.get("thankYou") or DEFAULT_THANK_YOU)}, 201)


def _track_detail(kind, value, questions):
    detail = _clean(value, 16).lower()
    if kind == "answer":
        return detail if detail in {question["id"] for question in questions} else None
    if kind == "link":
        return detail if detail in TRACK_LINKS else None
    if kind == "leave":
        return detail if COUNT_RE.fullmatch(detail) else None
    return "" if not detail else None


async def _visit_key(runtime, survey_id, session):
    address = str((_visitor(runtime) or {}).get("ip") or "").strip()
    if address:
        return (await runtime.blind("survey-visitor:%s:%s" % (survey_id, address)))[:32]
    session = str(session or "").lower()
    if SESSION_RE.fullmatch(session):
        return (await runtime.blind("survey-visitor-session:%s:%s" % (survey_id, session)))[:32]
    return ""


async def _count(runtime, sql, *args):
    row = await runtime.d1_first(sql, *args)
    return _integer((row or {}).get("n"))


async def _visit_ping(runtime, row, data, key, kind, now):
    stage = 1 if kind == "view" else 2 if kind in TRACK_START_KINDS else 0
    notify = getattr(runtime, "notify", None)
    creator = _clean(row.get("created_by_name"), 64).lower()
    if (not stage or not key or notify is None or not creator
            or not _settings(data.get("settings"))["notifyOnVisit"]):
        return 0
    survey_id = row["survey_id"]
    hour = now - now % HOUR_MS
    prior = await _count(
        runtime, "SELECT COALESCE(MAX(pinged),0) AS n FROM survey_visits "
        "WHERE survey_id=? AND visitor_key=? AND created_at>=?", survey_id, key, hour)
    if prior >= stage:
        return 0
    if await _count(runtime, "SELECT COUNT(*) AS n FROM survey_visits WHERE survey_id=? "
                    "AND pinged>0 AND created_at>=?", survey_id, hour) >= SURVEY_HOURLY_VISIT_PINGS:
        return 0
    title = str(data.get("title") or "Untitled survey")
    count = _integer(row.get("response_count"))
    try:
        sent = await notify(
            creator, "plugin_activity",
            ("👀 Someone opened your survey ‘%s’" if stage == 1
             else "✍️ Someone started your survey ‘%s’") % title,
            body="%d %s so far" % (count, "response" if count == 1 else "responses"),
            href="%s?org=%s&survey=%s&tab=activity" % (PAGE_PATH, row.get("org_name"), survey_id),
            dedupe="survey-visit:%s:%s:%d" % (survey_id, key, hour // HOUR_MS),
            push=prior == 0)
    except Exception:
        return 0
    return stage if sent else 0


async def _track(runtime, token, body):
    row = await _public_row(runtime, token)
    if not row:
        return _response(runtime, {"error": "survey_not_found"}, 404)
    data = await _open_data(runtime, row)
    kind = _clean(body.get("kind"), 16).lower()
    detail = (_track_detail(kind, body.get("detail"), _stored_questions(data.get("questions")))
              if kind in TRACK_KINDS else None)
    if detail is None:
        return _response(runtime, {"error": "invalid_event"}, 400)
    now = runtime.now()
    survey_id = row["survey_id"]
    key = await _visit_key(runtime, survey_id, body.get("session"))
    if key and await _count(
            runtime, "SELECT COUNT(*) AS n FROM survey_visits WHERE survey_id=? AND visitor_key=? "
            "AND created_at>=?", survey_id, key, now - HOUR_MS) >= VISITOR_HOURLY_EVENTS:
        return _response(runtime, {"error": "rate_limited"}, 429)
    if await _count(runtime, "SELECT COUNT(*) AS n FROM survey_visits WHERE survey_id=? "
                    "AND created_at>=?", survey_id, now - HOUR_MS) >= SURVEY_HOURLY_EVENTS:
        return _response(runtime, {"error": "rate_limited"}, 429)
    pinged = await _visit_ping(runtime, row, data, key, kind, now)
    await runtime.d1_run(
        "INSERT INTO survey_visits(visit_id,org_bi,survey_id,visitor_key,kind,detail,pinged,"
        "created_at) VALUES(?,?,?,?,?,?,?,?)",
        runtime.new_id(), row["org_bi"], survey_id, key, kind, detail, pinged, now)
    if kind == "view":
        await runtime.d1_run(
            "DELETE FROM survey_visits WHERE survey_id=? AND (created_at<? OR visit_id NOT IN ("
            "SELECT visit_id FROM survey_visits WHERE survey_id=? "
            "ORDER BY created_at DESC,visit_id DESC LIMIT ?))",
            survey_id, now - VISIT_RETAIN_MS, survey_id, MAX_VISITS)
    return _response(runtime, {"ok": True}, 202)


def _visit_label(row, titles):
    kind, detail = str(row.get("kind") or ""), str(row.get("detail") or "")
    if kind == "view":
        return "Opened the survey"
    if kind == "start":
        return "Started answering"
    if kind == "answer":
        title = titles.get(detail)
        return ("Answered “%s”" % title) if title else "Answered a question"
    if kind == "submit":
        return "Sent their answers"
    if kind == "link":
        return "Clicked the ForkMesh link"
    if kind == "leave":
        count = _integer(detail)
        if not count:
            return "Left without answering"
        return "Left after answering %d %s" % (count, "question" if count == 1 else "questions")
    return kind


async def _activity(runtime, survey_id, questions, now, limit=MAX_RECENT_VISITS):
    row = await runtime.d1_first(
        "SELECT SUM(kind='view') AS views,"
        "SUM(kind NOT IN ('view','leave')) AS interactions,"
        "SUM(kind='start') AS starts,SUM(kind='submit') AS submits,SUM(kind='leave') AS leaves,"
        "COUNT(DISTINCT CASE WHEN visitor_key<>'' THEN visitor_key END) AS visitors,"
        "COALESCE(MAX(created_at),0) AS last FROM survey_visits "
        "WHERE survey_id=? AND created_at>=?", survey_id, now - VISIT_SUMMARY_DAYS * DAY_MS)
    row = row or {}
    rows = await runtime.d1_all(
        "SELECT kind,detail,visitor_key,created_at FROM survey_visits WHERE survey_id=? "
        "ORDER BY created_at DESC,visit_id DESC LIMIT ?", survey_id, limit)
    titles = {question["id"]: question["title"] for question in questions}
    return {
        "days": VISIT_SUMMARY_DAYS,
        "views": _integer(row.get("views")),
        "visitors": _integer(row.get("visitors")),
        "interactions": _integer(row.get("interactions")),
        "starts": _integer(row.get("starts")),
        "submits": _integer(row.get("submits")),
        "leaves": _integer(row.get("leaves")),
        "lastActivityAt": _integer(row.get("last")),
        "recent": [{
            "kind": str(item.get("kind") or ""),
            "detail": str(item.get("detail") or ""),
            "label": _visit_label(item, titles),
            "visitor": str(item.get("visitor_key") or "")[:6],
            "at": _integer(item.get("created_at")),
        } for item in rows or []],
    }


async def org_has_data(runtime, org_bi):
    return bool(await runtime.d1_first(
        "SELECT survey_id FROM survey_forms WHERE org_bi=? LIMIT 1", org_bi))


async def export_org_data(runtime, org_bi, org_name):
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT " + FORM_COLUMNS + " FROM survey_forms WHERE org_bi=? "
        "ORDER BY created_at DESC,survey_id", org_bi)
    surveys = []
    for row in rows or []:
        survey, data = await _project(runtime, row, now, ("", ""))
        survey.pop("canDelete", None)
        survey.pop("publicUrl", None)
        survey["questions"] = _stored_questions(data.get("questions"))
        survey["responses"] = await _responses(runtime, row["survey_id"], MAX_RESPONSES)
        survey["activity"] = await _activity(runtime, row["survey_id"], survey["questions"], now,
                                             MAX_VISITS)
        surveys.append(survey)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-surveys-export-v1",
        "organization": org_name,
        "exportedAt": now,
        "surveys": surveys,
    }


async def purge_org_data(runtime, org_bi):
    await runtime.d1_run("DELETE FROM survey_visits WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM survey_responses WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM survey_forms WHERE org_bi=?", org_bi)
    await runtime.d1_run("DELETE FROM survey_usage WHERE org_bi=?", org_bi)


def _allowed(route):
    kind = route[0]
    if kind == "organizations":
        return ("GET",)
    if kind == "public":
        return ("GET", "POST")
    if kind == "track":
        return ("POST",)
    _, _, resource, item, action, response_id = route
    if not resource:
        return ("GET",)
    if not item:
        return ("POST",)
    if action == "responses":
        return ("DELETE",) if response_id else ("GET",)
    if action == "export":
        return ("GET",)
    if action:
        return ("POST",)
    return ("GET", "PATCH", "DELETE")


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def _public_schema_ready(runtime):
    if not await runtime.d1_first(
            "SELECT plugin FROM org_plugin_installs WHERE plugin=? LIMIT 1", PLUGIN_ID):
        return False
    await _ensure_tables(runtime)
    return True


async def _body(runtime):
    data, error = await runtime.json_body(MAX_BODY_BYTES)
    if error:
        return None, _response(runtime, {"error": error},
                               413 if error == "payload_too_large" else 400)
    return data, None


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed(route)
    if method == "HEAD" and "GET" in allowed:
        method = "GET"
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    await runtime.ensure_schema()
    if route[0] in ("public", "track"):
        if method != "GET" and not runtime.same_origin():
            return _response(runtime, {"error": "origin_not_allowed"}, 403)
        if (not await _token_survey(runtime, route[1])
                or not await _public_schema_ready(runtime)):
            return _response(runtime, {"error": "survey_not_found"}, 404)
        if method == "GET":
            return await _public(runtime, route[1])
        body, failure = await _body(runtime)
        if failure:
            return failure
        if route[0] == "track":
            return await _track(runtime, route[1], body)
        return await _submit(runtime, route[1], body)
    if method != "GET" and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method != "GET":
        data, failure = await _body(runtime)
        if failure:
            return failure
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if route[0] == "organizations":
        return _response(runtime, {"ok": True, "actor": actor,
                                   "organizations": await _organizations(runtime, account_bi)})
    _, org_name, resource, item, action, response_id = route
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, account_bi)
    if not org_bi or not role:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if role in ORG_READ_ONLY_ROLES and method not in ("GET", "HEAD"):
        return _response(runtime, {"error": "read_only_role"}, 403)
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return _response(runtime, {"error": "plugin_not_installed", "organization": org_name,
                                   "canInstall": role in MANAGER_ROLES}, 409)
    await _ensure_tables(runtime)
    viewer = (account_bi, role)
    if not resource:
        return await _overview(runtime, org_bi, org_name, viewer)
    if not item:
        return await _create(runtime, org_bi, org_name, viewer, actor, data)
    row = await _row(runtime, org_bi, item)
    if not row:
        return _response(runtime, {"error": "survey_not_found"}, 404)
    if action == "open":
        return await _open(runtime, row, org_bi, viewer, actor)
    if action == "close":
        return await _close(runtime, row, org_bi, viewer, actor)
    if action == "duplicate":
        return await _duplicate(runtime, row, org_bi, org_name, viewer, actor)
    if action == "export":
        return await _export(runtime, row, actor)
    if action == "responses":
        if response_id:
            return await _delete_response(runtime, row, org_bi, viewer, actor, response_id)
        return _response(runtime, {"ok": True, "responses": await _responses(runtime, item)})
    if method == "GET":
        return await _detail(runtime, org_bi, item, viewer)
    if method == "DELETE":
        return await _delete(runtime, row, org_bi, viewer, actor)
    return await _update(runtime, row, org_bi, viewer, actor, data)
