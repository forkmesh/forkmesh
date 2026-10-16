#!/usr/bin/env python3
"""In-app bug reports: what is accepted, what is stored, and what is not.

The module is loaded into a fresh namespace and handed stub runtime primitives
through its ``_bind_runtime`` hook, exactly as entry.py's ``_LazyModule`` does
in production, so these tests exercise the real source with no Worker runtime.

Four contracts are worth pinning here, because each one is a way this endpoint
could quietly become the wrong thing:

* an anonymous report is accepted (an authenticated-only report endpoint
  cannot receive the crash that happens in the login flow),
* the reporter's prose and contact never appear in a plaintext column,
* the per-reporter hourly quota actually refuses the eleventh report, and
* the route is reachable and is dispatched before the account router.
"""

import asyncio
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MODULE = SRC / "bug_reports.py"


def run_async_test(function):
    def wrapped(*args, **kwargs):
        return asyncio.run(function(*args, **kwargs))
    return wrapped


class _Response:
    """Stands in for the Workers Response the handler returns."""

    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = headers or {}

    @property
    def json(self):
        return json.loads(self.body)


class _Request:
    def __init__(self, method="POST", body=None, headers=None):
        self.method = method
        self._body = "" if body is None else json.dumps(body)
        self.headers = headers or {}

    async def text(self):
        return self._body


class _Runtime:
    """Stub Worker primitives plus a recorder for every row written."""

    def __init__(self, account="", now=1_783_463_371_000, enqueue=None):
        self.now = now
        self.account = account
        self.rows = []
        self.updates = []
        self.issues = []
        self.errors = []
        self.enqueue = enqueue
        self.random = bytes(range(16))

    def namespace(self):
        source = MODULE.read_text(encoding="utf-8")
        namespace = {"__name__": "bug_reports"}
        exec(compile(source, str(MODULE), "exec"), namespace)
        namespace["_bind_runtime"](self.primitives())
        return namespace

    def primitives(self):
        runtime = self

        class _Date:
            @staticmethod
            def now():
                return runtime.now

        class _Uint8Array:
            @staticmethod
            def new(size):
                return list(runtime.random[:size]) if isinstance(
                    size, int) else list(size)

        class _Crypto:
            @staticmethod
            def getRandomValues(buffer):
                return buffer

        def clean_string(value, maximum=240):
            text = "".join(
                character if character.isprintable() or character in "\n\t"
                else " "
                for character in str(value if value is not None else ""))
            return text[:maximum] if maximum else text

        def safe_segment(value, maximum=200):
            candidate = clean_string(value, maximum).strip().lower()
            return candidate if re.fullmatch(
                r"[a-z0-9._-]{1,%d}" % maximum, candidate or "") else ""

        def json_response(data, status=200, cache_seconds=None,
                          cache_control=None, extra_headers=None):
            headers = {"content-type": "application/json; charset=utf-8"}
            if cache_control is not None:
                headers["cache-control"] = cache_control
            headers.update(extra_headers or {})
            return _Response(
                json.dumps(data, separators=(",", ":")), status, headers)

        def method_name(request):
            return request.method

        class RequestBodyTooLarge(BaseException):
            pass

        async def bounded_json_request(request, max_bytes=None):
            raw = await request.text()
            if max_bytes and len(raw.encode("utf-8")) > int(max_bytes):
                raise RequestBodyTooLarge()
            return json.loads(raw or "{}")

        async def blind_index(env, value):
            return "bi:" + hashlib.sha256(
                str(value).encode()).hexdigest()[:16]

        async def encrypt_row(env, item):
            # Stands in for AES-GCM: the point under test is *which* fields
            # end up inside the opaque blob, not the cipher itself.
            return "sealed:" + json.dumps(item, sort_keys=True)

        async def ensure_schema(env):
            return None

        async def d1_first(env, sql, *args):
            if "COUNT(*) AS n FROM bug_reports" in sql:
                reporter_bi, since = args
                return {"n": sum(
                    1 for row in runtime.rows
                    if row["reporter_bi"] == reporter_bi
                    and row["created_at"] >= since)}
            return None

        async def d1_run(env, sql, *args):
            if "INTO bug_reports" in sql:
                keys = ("report_id", "reporter_bi", "account", "source",
                        "platform", "app_version", "severity", "data",
                        "created_at")
                runtime.rows.append(dict(zip(keys, args)))
            elif "UPDATE bug_reports" in sql:
                runtime.updates.append(args)
            return None

        async def _forkbot_enqueue_issue(env, owner, repo, title, body,
                                         requester, **kwargs):
            call = dict(kwargs, owner=owner, repo=repo, title=title,
                        body=body, requester=requester)
            runtime.issues.append(call)
            if runtime.enqueue is not None:
                return runtime.enqueue(call)
            return True, {"number": 700 + len(runtime.issues)}

        async def log_error(env, status, method, path, message, **kwargs):
            runtime.errors.append(message)

        async def _authed_account_name(env, request, data=None):
            return runtime.account

        def _transient_client_address(request):
            return request.headers.get("cf-connecting-ip", "")

        return {
            "Date": _Date,
            "Uint8Array": _Uint8Array,
            "js_crypto": _Crypto,
            "clean_string": clean_string,
            "safe_segment": safe_segment,
            "json_response": json_response,
            "method_name": method_name,
            "RequestBodyTooLarge": RequestBodyTooLarge,
            "bounded_json_request": bounded_json_request,
            "blind_index": blind_index,
            "encrypt_row": encrypt_row,
            "ensure_schema": ensure_schema,
            "d1_first": d1_first,
            "d1_run": d1_run,
            "_authed_account_name": _authed_account_name,
            "_transient_client_address": _transient_client_address,
            "_forkbot_enqueue_issue": _forkbot_enqueue_issue,
            "log_error": log_error,
        }


def _submit(module, body, headers=None, env=None):
    return asyncio.run(module["bug_reports_handler"](
        object() if env is None else env, _Request("POST", body, headers)))


class _Env:
    def __init__(self, **values):
        self.__dict__.update(values)


# --- What the endpoint accepts ----------------------------------------------

def test_anonymous_report_is_accepted_and_stored():
    # The crash worth hearing about most is the one in the login flow, and a
    # session-gated report endpoint is exactly the one that cannot receive it.
    runtime = _Runtime(account="")
    module = runtime.namespace()

    response = _submit(module, {
        "message": "Repos tab spins forever after I rotate the phone.",
        "source": "mobile",
        "platform": "ios",
        "appVersion": "1.0.0+2",
    }, {"cf-connecting-ip": "203.0.113.7"})

    assert response.status == 201
    assert response.json["ok"] is True
    assert response.json["credited"] is False
    assert re.fullmatch(r"[0-9a-f]{32}", response.json["id"])
    assert len(runtime.rows) == 1
    assert runtime.rows[0]["source"] == "mobile"
    assert runtime.rows[0]["account"] == ""


def test_anonymous_report_survives_safe_segment_returning_none():
    # Production safe_segment() answers None, not "", for an empty account.
    # The insert bound that None to the NOT NULL account column and every
    # anonymous report failed with a 500 (/status api row, 2026-09-14 15:10).
    runtime = _Runtime(account="")
    module = runtime.namespace()
    module["safe_segment"] = lambda value, maximum=200: value or None

    response = _submit(module, {
        "message": "The Windows installer link answers mirror_unavailable.",
        "source": "web",
    }, {"cf-connecting-ip": "203.0.113.9"})

    assert response.status == 201
    assert response.json["credited"] is False
    assert runtime.rows[0]["account"] == ""


def test_signed_in_report_is_credited_to_the_session_account():
    runtime = _Runtime(account="alice")
    module = runtime.namespace()

    response = _submit(module, {"message": "Chat badge never clears."})

    assert response.status == 201
    assert response.json["credited"] is True
    assert runtime.rows[0]["account"] == "alice"


def test_get_describes_the_endpoint_without_touching_the_database():
    runtime = _Runtime()
    module = runtime.namespace()

    response = asyncio.run(module["bug_reports_handler"](
        object(), _Request("GET")))

    assert response.status == 200
    assert response.json["endpoint"] == "/api/bug-reports"
    assert response.json["method"] == "POST"
    assert runtime.rows == []


def test_unsupported_method_is_refused_with_an_allow_header():
    runtime = _Runtime()
    module = runtime.namespace()

    response = asyncio.run(module["bug_reports_handler"](
        object(), _Request("DELETE")))

    assert response.status == 405
    assert response.headers["allow"] == "GET, POST"


# --- What it refuses ---------------------------------------------------------

def test_a_report_with_no_usable_message_is_refused():
    runtime = _Runtime()
    module = runtime.namespace()

    for body in ({}, {"message": ""}, {"message": "   "}, {"message": "ab"}):
        response = _submit(module, body)
        assert response.status == 400
        assert response.json["error"] == "empty_report"
    assert runtime.rows == []


def test_an_oversized_body_is_refused_before_it_reaches_the_database():
    runtime = _Runtime()
    module = runtime.namespace()

    response = _submit(module, {"message": "x" * (module["MAX_BODY_BYTES"] + 1)})

    assert response.status == 413
    assert response.json["error"] == "report_too_large"
    assert runtime.rows == []


def test_the_hourly_quota_refuses_the_eleventh_report_from_one_reporter():
    # Anonymous POST plus D1 is a spam target; the quota is what keeps this
    # from being an open write.
    runtime = _Runtime()
    module = runtime.namespace()
    headers = {"cf-connecting-ip": "203.0.113.7"}

    for index in range(module["RATE_LIMIT_MAX"]):
        assert _submit(
            module, {"message": "report %d" % index}, headers).status == 201

    refused = _submit(module, {"message": "one too many"}, headers)

    assert refused.status == 429
    assert refused.json["error"] == "rate_limited"
    assert refused.headers["retry-after"] == "3600"
    assert len(runtime.rows) == module["RATE_LIMIT_MAX"]


def test_a_different_reporter_is_not_blocked_by_someone_elses_quota():
    runtime = _Runtime()
    module = runtime.namespace()

    for index in range(module["RATE_LIMIT_MAX"]):
        _submit(module, {"message": "report %d" % index},
                {"cf-connecting-ip": "203.0.113.7"})
    other = _submit(module, {"message": "unrelated problem"},
                    {"cf-connecting-ip": "198.51.100.4"})

    assert other.status == 201


def test_quota_only_counts_reports_inside_the_window():
    runtime = _Runtime()
    module = runtime.namespace()
    headers = {"cf-connecting-ip": "203.0.113.7"}
    for index in range(module["RATE_LIMIT_MAX"]):
        _submit(module, {"message": "report %d" % index}, headers)

    runtime.now += module["RATE_LIMIT_WINDOW_MS"] + 1
    later = _submit(module, {"message": "an hour later"}, headers)

    assert later.status == 201


# --- What is stored, and where ----------------------------------------------

def test_reporter_prose_and_contact_never_reach_a_plaintext_column():
    # `message` and `contact` are whatever the reporter typed, which routinely
    # includes an email address. Both belong inside the encrypted blob.
    runtime = _Runtime()
    module = runtime.namespace()

    _submit(module, {
        "message": "Crashes when I open my private repo.",
        "contact": "reporter@example.com",
        "platform": "android",
        "appVersion": "1.0.0+2",
    }, {"cf-connecting-ip": "203.0.113.7"})

    row = runtime.rows[0]
    plaintext = " ".join(
        str(value) for key, value in row.items() if key != "data")
    assert "reporter@example.com" not in plaintext
    assert "private repo" not in plaintext
    sealed = json.loads(row["data"].removeprefix("sealed:"))
    assert sealed["contact"] == "reporter@example.com"
    assert sealed["message"] == "Crashes when I open my private repo."


def test_the_stored_row_never_retains_the_reporters_address():
    runtime = _Runtime()
    module = runtime.namespace()

    _submit(module, {"message": "Something is broken."},
            {"cf-connecting-ip": "203.0.113.7"})

    row = runtime.rows[0]
    assert "203.0.113.7" not in json.dumps(row)
    # The quota key is a blind index, so counting one reporter's recent
    # submissions stays possible without keeping the address.
    assert row["reporter_bi"].startswith("bi:")


def test_operational_columns_are_normalized_for_the_console():
    runtime = _Runtime()
    module = runtime.namespace()

    _submit(module, {
        "message": "The agent transcript stops mid-run.",
        "severity": "CRASH",
        "source": "Mobile",
        "platform": "ios 26.1",
        "appVersion": "1.0.0+2",
    })

    row = runtime.rows[0]
    assert row["severity"] == "crash"
    assert row["source"] == "mobile"
    assert row["platform"] == "ios 26.1"
    assert row["app_version"] == "1.0.0+2"
    assert row["created_at"] == runtime.now


def test_unknown_severity_and_source_fall_back_rather_than_reject():
    # A newer client must not have its reports dropped by an older Worker that
    # has not heard of its source yet.
    runtime = _Runtime()
    module = runtime.namespace()

    response = _submit(module, {
        "message": "Filed from a client this Worker predates.",
        "severity": "apocalyptic",
        "source": "smart-fridge",
    })

    assert response.status == 201
    assert runtime.rows[0]["severity"] == "normal"
    assert runtime.rows[0]["source"] == "unknown"


def test_normalize_report_is_pure_and_clamps_every_field():
    runtime = _Runtime()
    module = runtime.namespace()

    record = module["normalize_report"]({
        "message": "m" * 9000,
        "contact": "c" * 900,
        "screen": "s" * 900,
        "device": "d" * 900,
    })

    assert len(record["message"]) == module["MAX_MESSAGE"]
    assert len(record["contact"]) == module["MAX_CONTACT"]
    assert len(record["screen"]) == module["MAX_SCREEN"]
    assert len(record["device"]) == module["MAX_DEVICE"]
    assert module["normalize_report"]("not a mapping") is None


def test_an_invalid_plugin_id_is_dropped_not_rejected():
    runtime = _Runtime()
    module = runtime.namespace()

    for bad in ("../etc", "has space", "-lead", 42, None, "x" * 81):
        record = module["normalize_report"](
            {"message": "Something broke.", "package": bad})
        assert record["package"] == ""
    assert "issue" not in module["capability_document"]()


def test_attachments_are_encrypted_and_never_queued_as_issues():
    runtime = _Runtime()
    module = runtime.namespace()

    response = _submit(module, {
        "message": "The camera preview is black.",
        "attachments": [{"name": "screen.png", "contentType": "image/png",
                         "data": "cGl4ZWxz"}],
    })

    assert response.status == 201
    assert response.json["attachments"] == 1
    assert runtime.issues == []
    stored = json.loads(runtime.rows[0]["data"][len("sealed:"):])
    assert stored["attachments"][0]["name"] == "screen.png"
    assert stored["attachments"][0]["contentType"] == "image/png"
    assert stored["attachments"][0]["size"] == 6
    assert re.fullmatch(r"[0-9a-f]{32}", stored["attachments"][0]["id"])


def test_invalid_or_oversized_attachments_are_refused():
    runtime = _Runtime()
    module = runtime.namespace()

    invalid = _submit(module, {"message": "The camera preview is black.", "attachments": [{}]})
    too_many = _submit(module, {"message": "The camera preview is black.", "attachments": [{}] * 6})

    assert invalid.status == 400
    assert too_many.status == 400
    assert runtime.rows == []


# --- Wiring ------------------------------------------------------------------

def test_the_route_is_dispatched_before_the_account_router():
    # /api/accounts/ has a catch-all prefix route. A bug-report path matched
    # after it would send every anonymous submission into the account handler.
    runtime_source = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    report_at = runtime_source.index('"/api/bug-reports"')
    accounts_at = runtime_source.index('url.path.startswith("/api/accounts/")')

    assert report_at < accounts_at
    assert "bug_reports_handler" in runtime_source


def test_entry_exports_the_handler_as_a_lazy_module():
    # A top-level import would put this domain in every isolate's startup
    # memory, which is the budget Cloudflare validates a deploy against.
    entry_source = (SRC / "entry.py").read_text(encoding="utf-8")

    assert '_bug_reports = _LazyModule("bug_reports")' in entry_source
    assert 'bug_reports_handler = _bug_reports.export(' in entry_source
    assert "import bug_reports" not in entry_source


def test_the_table_is_declared_in_both_the_schema_and_a_migration():
    schema_source = (SRC / "schema.py").read_text(encoding="utf-8")
    migration = (ROOT / "migrations" / "0173_bug_reports.sql").read_text(
        encoding="utf-8")

    assert "CREATE TABLE IF NOT EXISTS bug_reports" in schema_source
    assert "CREATE TABLE IF NOT EXISTS bug_reports" in migration
    for index in ("idx_bug_reports_created", "idx_bug_reports_reporter",
                  "idx_bug_reports_status"):
        assert index in schema_source
        assert index in migration
