import asyncio
import importlib.util
import re
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (SRC / "app_runtime.py").read_text(encoding="utf-8")
PING_SETTINGS = (SRC / "ping_settings.py").read_text(encoding="utf-8")
FOOTPRINT = (
    ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
DESKTOP = ROOT.parent / "desktop"
NOW = 1_760_000_000_000


class _Date:
    @staticmethod
    def now():
        return NOW


class _Request:
    def __init__(self, url, method="POST"):
        self.url = url
        self.method = method


def _load(session_account="", signature_ok=True, account=None):
    spec = importlib.util.spec_from_file_location(
        "desktop_pings_under_test", SRC / "desktop_pings.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    calls = {"enqueued": [], "verified": [], "sql": [], "events": []}

    async def ensure_schema(env):
        return None

    async def bounded_json_request(request):
        return request.body

    def json_response(payload, status=200, **kwargs):
        return {"status": status, "payload": payload}

    async def _authed_account_name(env, request, data=None):
        return session_account

    async def _verify_owner_signature(env, user, sig, canonical):
        calls["verified"].append((user, sig, canonical))
        return signature_ok

    async def _account_row(env, name):
        record = account if account is not None else {"status": "active"}
        return ("bi", record)

    async def enqueue_notification(env, recipient, kind, title, **kwargs):
        calls["enqueued"].append((recipient, kind, title, kwargs))
        return True

    async def blind_index(env, value):
        return "bi(" + value + ")"

    async def d1_run(env, sql, *params):
        calls["sql"].append((sql, params))

    async def notify_account_event(env, owner, topic, push_alert=None):
        calls["events"].append((owner, topic, push_alert))

    module._bind_runtime({
        "Date": _Date,
        "blind_index": blind_index,
        "d1_run": d1_run,
        "notify_account_event": notify_account_event,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "ensure_schema": ensure_schema,
        "bounded_json_request": bounded_json_request,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "MAX_NODE_NAME": 64,
        "_ts_ok": lambda ts: str(ts).isdigit(),
        "_authed_account_name": _authed_account_name,
        "_verify_owner_signature": _verify_owner_signature,
        "_account_row": _account_row,
        "enqueue_notification": enqueue_notification,
    })
    return module, calls


def _post(module, body, query=None, method="POST"):
    url = "https://app.forkmesh.com/api/notifications/desktop"
    if query:
        url += "?" + urlencode(query)
    request = _Request(url, method)
    request.body = body
    return asyncio.run(module.desktop_ping_handler(None, request))


def _body(**overrides):
    body = {"pingId": "1760000000000-7", "node": "jett-laptop",
            "kind": "agent-done", "category": "agent-finished",
            "sessionId": "12", "repo": "jett/forkmesh", "warning": False,
            "title": "Agent #12 is done", "body": "Fixed the flaky test."}
    body.update(overrides)
    return body


SIGNED = {"user": "jett", "ts": "123", "sig": "proof"}


def test_route_kind_catalog_and_lazy_module_are_wired():
    assert '"desktop_ping",' in ENTRY
    assert '_desktop_pings = _LazyModule("desktop_pings")' in ENTRY
    assert ('desktop_ping_handler = _desktop_pings.export('
            '"desktop_ping_handler")') in ENTRY
    assert '"/api/notifications/desktop"' in APP_RUNTIME
    assert "return await desktop_ping_handler(self.env, request)" in (
        APP_RUNTIME)
    assert '("desktop_ping", "devices", "Pings from your desktop")' in (
        PING_SETTINGS)
    assert '"desktop_pings.py",' in FOOTPRINT


def test_desktop_signs_the_same_proof_route_and_canonical():
    module, _ = _load()
    source = (DESKTOP / "src" / "DesktopPingForward.cpp").read_text(
        encoding="utf-8")
    assert f'QStringLiteral("{module.DESKTOP_PING_PROOF}")' in source
    assert 'QStringLiteral("/api/notifications/desktop")' in source
    test = (DESKTOP / "tests" / "test_desktop_ping_forward.cpp").read_text(
        encoding="utf-8")
    fields = module.desktop_ping_fields(_body())
    expected = module.desktop_ping_canonical("jett", "123", fields).decode()
    literal = "".join(re.findall(
        r'"((?:[^"\\]|\\.)*)"',
        test.split("check(canonical(")[1].split("the canonical matches")[0]))
    assert literal.endswith(expected.replace("\n", "\\n")), literal


def test_signed_ping_is_filed_for_the_signer_with_agent_routing():
    module, calls = _load()
    response = _post(module, _body(), SIGNED)
    assert response["status"] == 200
    fields = module.desktop_ping_fields(_body())
    assert calls["verified"] == [
        ("jett", "proof", module.desktop_ping_canonical("jett", "123", fields))]
    [(recipient, kind, title, kwargs)] = calls["enqueued"]
    assert (recipient, kind, title) == ("jett", "desktop_ping",
                                        "Agent #12 is done")
    assert kwargs["body"] == "Fixed the flaky test."
    assert kwargs["repo"] == "jett/forkmesh"
    assert kwargs["href"] == "/jett/forkmesh/agents/12"
    assert kwargs["source"] == "desktop"
    assert kwargs["dedupe"] == "desktop:jett-laptop:1760000000000-7"
    assert kwargs["ts"] == NOW
    assert kwargs["meta"] == {
        "node": "jett-laptop", "desktopKind": "agent-done",
        "warning": False, "pingId": "1760000000000-7",
        "category": "agent-finished", "sessionId": "12"}


def test_a_plain_ping_carries_no_agent_fields_and_a_body_only_ping_is_titled():
    module, calls = _load(session_account="jett")
    response = _post(module, _body(category="", sessionId="", repo="",
                                   kind="action", title="",
                                   body="Workflow build failed"))
    assert response["status"] == 200
    assert calls["verified"] == []
    [(_, _, title, kwargs)] = calls["enqueued"]
    assert title == "Workflow build failed"
    assert kwargs["body"] == ""
    assert kwargs["href"] == ""
    assert "category" not in kwargs["meta"]
    assert "sessionId" not in kwargs["meta"]


def _run_body(**overrides):
    body = _body(kind="action", category="ci-failed", sessionId="",
                 runId="41", warning=True, title="Workflow build failed",
                 body="Step test exited 1")
    body.update(overrides)
    return body


def test_a_failed_action_ping_opens_its_run():
    module, calls = _load()
    response = _post(module, _run_body(), SIGNED)
    assert response["status"] == 200
    fields = module.desktop_ping_fields(_run_body())
    assert calls["verified"] == [
        ("jett", "proof", module.desktop_ping_canonical("jett", "123", fields))]
    [(_, _, _, kwargs)] = calls["enqueued"]
    assert kwargs["href"] == "/jett/forkmesh/actions/runs/41"
    assert kwargs["meta"]["runId"] == "41"
    assert kwargs["meta"]["category"] == "ci-failed"


def test_desktop_signs_the_same_run_canonical():
    module, _ = _load()
    test = (DESKTOP / "tests" / "test_desktop_ping_forward.cpp").read_text(
        encoding="utf-8")
    fields = module.desktop_ping_fields(_run_body())
    expected = module.desktop_ping_canonical("jett", "123", fields).decode()
    literal = "".join(re.findall(
        r'"((?:[^"\\]|\\.)*)"',
        test.split("check(canonical(")[2].split("the run canonical matches")[0]))
    assert literal.endswith(expected.replace("\n", "\\n")), literal


def test_a_ping_without_a_run_signs_the_old_canonical():
    module, _ = _load()
    with_run = module.desktop_ping_fields(_body())
    assert "\n12\njett/forkmesh\n" in module.desktop_ping_resource(with_run)


def test_a_power_loss_ping_is_an_alarm_that_rings_through_quiet_hours():
    module, calls = _load()
    response = _post(module, _body(kind="power-battery", category="",
                                   sessionId="", repo="", warning=True,
                                   title="tower lost power",
                                   body="Running on battery."), SIGNED)
    assert response["status"] == 200
    [(_, kind, title, kwargs)] = calls["enqueued"]
    assert (kind, title) == ("desktop_ping", "tower lost power")
    assert kwargs["meta"]["alarm"] == "power"
    assert kwargs["force"] is True
    source = (DESKTOP / "src" / "DesktopPowerAlert.cpp").read_text(
        encoding="utf-8")
    for desktop_kind in module.DESKTOP_PING_ALARMS:
        assert f'QStringLiteral("{desktop_kind}")' in source

    module, calls = _load()
    _post(module, _body(kind="power-restored", category="", sessionId="",
                        repo="", title="tower is back on mains power"),
          SIGNED)
    [(_, _, _, kwargs)] = calls["enqueued"]
    assert "alarm" not in kwargs["meta"]
    assert kwargs["force"] is False


def test_the_apns_banner_carries_the_alarm():
    source = ENTRY.split("async def _apns_ping_alert(")[1].split(
        "\nasync def ")[0]
    assert 'alert["alarm"]' in source


def test_refusals():
    module, calls = _load(signature_ok=False)
    assert _post(module, _body(), SIGNED)["status"] == 401
    assert _post(module, _body())["status"] == 401
    assert _post(module, _body(), SIGNED, method="GET")["status"] == 405

    module, calls = _load()
    for bad in (dict(pingId="../x"), dict(node="a b"), dict(kind="x" * 41),
                dict(category="pr-review"), dict(sessionId="s1"),
                dict(runId="0"), dict(runId="r1"), dict(runId=41),
                dict(repo="not a repo"), dict(warning="yes"),
                dict(title="x" * 161), dict(body="x" * 501),
                dict(title=" ", body="")):
        assert _post(module, _body(**bad), SIGNED)["status"] == 400, bad
    assert calls["enqueued"] == []
    assert _post(module, ["not", "an", "object"], SIGNED)["status"] == 400

    module, calls = _load(account={"status": "suspended"})
    assert _post(module, _body(), SIGNED)["status"] == 401
    assert calls["enqueued"] == []


def test_the_apns_banner_names_the_category_and_session():
    source = ENTRY.split("async def _apns_ping_alert(")[1].split(
        "\nasync def ")[0]
    assert 'record.get("kind") == "desktop_ping"' in source
    assert 'alert["category"]' in source
    assert 'alert["sessionId"]' in source
    assert 'alert["runId"]' in source


def _delete_body(**overrides):
    body = {"node": "jett-laptop",
            "pingIds": ["1760000000000-7", "1760000000500-8"]}
    body.update(overrides)
    return body


def test_desktop_signs_the_same_delete_canonical():
    module, _ = _load()
    source = (DESKTOP / "src" / "DesktopPingForward.cpp").read_text(
        encoding="utf-8")
    assert f'QStringLiteral("{module.DESKTOP_PING_DELETE_PROOF}")' in source
    test = (DESKTOP / "tests" / "test_desktop_ping_forward.cpp").read_text(
        encoding="utf-8")
    fields = module.desktop_ping_delete_fields(_delete_body())
    expected = module.desktop_ping_delete_canonical(
        "jett", "123", fields).decode()
    literal = "".join(re.findall(
        r'"((?:[^"\\]|\\.)*)"',
        test.split("check(deleteCanonical(")[1].split("the delete canonical")[0]))
    assert literal.endswith(expected.replace("\n", "\\n")), literal
    assert f"kDeleteBatch == {module.DESKTOP_PING_DELETE_MAX}" in test


def test_a_signed_delete_removes_the_forwarded_rows_and_wakes_clients():
    module, calls = _load()
    response = _post(module, _delete_body(), SIGNED, method="DELETE")
    assert response["status"] == 200
    fields = module.desktop_ping_delete_fields(_delete_body())
    assert calls["verified"] == [
        ("jett", "proof",
         module.desktop_ping_delete_canonical("jett", "123", fields))]
    [(sql, params)] = calls["sql"]
    assert sql.startswith("DELETE FROM notifications WHERE recipient_bi=?")
    assert "dedupe_bi IN (?,?)" in sql
    assert params == (
        "bi(jett)",
        "bi(notification:jett:desktop:jett-laptop:1760000000000-7)",
        "bi(notification:jett:desktop:jett-laptop:1760000000500-8)")
    assert calls["events"] == [("jett", "pings", None)]
    assert calls["enqueued"] == []


def test_the_delete_dedupe_matches_the_filed_row():
    module, calls = _load(session_account="jett")
    _post(module, _body())
    [(_, _, _, kwargs)] = calls["enqueued"]
    _post(module, _delete_body(pingIds=["1760000000000-7"]), method="DELETE")
    [(_, params)] = calls["sql"]
    assert params[1] == "bi(notification:jett:" + kwargs["dedupe"] + ")"


def test_delete_refusals():
    module, calls = _load(signature_ok=False)
    assert _post(module, _delete_body(), SIGNED, method="DELETE")["status"] == 401
    assert _post(module, _delete_body(), method="DELETE")["status"] == 401

    module, calls = _load()
    for bad in (dict(node="a b"), dict(pingIds=[]), dict(pingIds="1-2"),
                dict(pingIds=["../x"]), dict(pingIds=[7]),
                dict(pingIds=["1-%d" % n for n in range(51)])):
        response = _post(module, _delete_body(**bad), SIGNED, method="DELETE")
        assert response["status"] == 400, bad
    assert calls["sql"] == [] and calls["events"] == []


APPROVAL_ID = "0123456789abcdef0123456789abcdef"
PUSH_ICONS = (SRC / "push_icons.py").read_text(encoding="utf-8")


def _post_approval(module, body, query=None, method="POST"):
    url = "https://app.forkmesh.com/api/notifications/agent-approval"
    if query:
        url += "?" + urlencode(query)
    request = _Request(url, method)
    request.body = body
    return asyncio.run(module.agent_approval_ping_handler(None, request))


def _approval_body(**overrides):
    body = {"sessionId": "12", "approvalId": APPROVAL_ID,
            "agent": "Claude Code",
            "summary": "Wants to run a command in alice/app"}
    body.update(overrides)
    return body


def test_agent_approval_route_kind_and_catalog_are_wired():
    assert '    "agent_approval",\n' in ENTRY
    assert ('agent_approval_ping_handler = _desktop_pings.export(\n'
            '    "agent_approval_ping_handler")') in ENTRY
    assert '"/api/notifications/agent-approval"' in APP_RUNTIME
    assert "return await agent_approval_ping_handler(self.env, request)" in (
        APP_RUNTIME)
    assert ('("agent_approval", "devices", '
            '"Agent approvals from your desktop")') in PING_SETTINGS
    assert '"agent_approval": ' in PUSH_ICONS


def test_desktop_signs_the_same_approval_proof_route_and_canonical():
    module, _ = _load()
    source = (DESKTOP / "src" / "LocalControlAgentOps.cpp").read_text(
        encoding="utf-8")
    assert f'QStringLiteral("{module.AGENT_APPROVAL_PROOF}")' in source
    assert 'QStringLiteral("/api/notifications/agent-approval")' in source
    test = (DESKTOP / "tests" / "test_local_control_agentops.cpp").read_text(
        encoding="utf-8")
    assert ('"forkmesh-account-agent-approval-ping-v1\\nalice\\n12\\n") +\n'
            '                           facts.id +') in test
    assert ('"\\nClaude Code\\nWants to run a command in alice/app\\n"\n'
            '                                          "1700000000000"') in test
    fields = module.agent_approval_fields(_approval_body())
    assert module.agent_approval_canonical(
        "alice", "1700000000000", fields).decode() == (
        "forkmesh-account-agent-approval-ping-v1\nalice\n12\n" + APPROVAL_ID
        + "\nClaude Code\nWants to run a command in alice/app\n1700000000000")
    assert 'QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}$"))' in (
        source)
    assert module.AGENT_APPROVAL_AGENT_MAX == 80
    assert module.AGENT_APPROVAL_SUMMARY_MAX == 200
    header = (DESKTOP / "src" / "LocalControlAgentOps.h").read_text(
        encoding="utf-8")
    assert "kMaxPingSummaryChars = 200;" in header
    assert "kMaxPingAgentChars = 80;" in header


def test_a_signed_approval_is_filed_with_approve_and_deny_routing():
    module, calls = _load()
    response = _post_approval(module, _approval_body(), SIGNED)
    assert response["status"] == 200
    fields = module.agent_approval_fields(_approval_body())
    assert calls["verified"] == [
        ("jett", "proof", module.agent_approval_canonical("jett", "123", fields))]
    [(recipient, kind, title, kwargs)] = calls["enqueued"]
    assert (recipient, kind, title) == (
        "jett", "agent_approval", "Claude Code needs your approval")
    assert kwargs["body"] == "Wants to run a command in alice/app"
    assert kwargs["source"] == "desktop"
    assert kwargs["dedupe"] == "agent-approval:" + APPROVAL_ID
    assert kwargs["ts"] == NOW
    assert kwargs["meta"] == {
        "category": "agent-approval", "sessionId": "12",
        "approvalId": APPROVAL_ID, "agent": "Claude Code"}


def test_a_session_approval_needs_no_signature_and_blank_text_is_filled():
    module, calls = _load(session_account="jett")
    response = _post_approval(module, _approval_body(agent="", summary=" "))
    assert response["status"] == 200
    assert calls["verified"] == []
    [(_, _, title, kwargs)] = calls["enqueued"]
    assert title == "An agent needs your approval"
    assert kwargs["body"] == "Wants your approval"


def test_an_unusual_agent_name_on_one_line_is_accepted():
    module, calls = _load(session_account="jett")
    response = _post_approval(
        module, _approval_body(agent="Agent \ue000 caf\u00e9 \U0001F916"))
    assert response["status"] == 200
    [(_, _, title, _)] = calls["enqueued"]
    assert title.startswith("Agent \ue000 caf\u00e9")


def test_approval_refusals():
    module, calls = _load(signature_ok=False)
    assert _post_approval(module, _approval_body(), SIGNED)["status"] == 401
    assert _post_approval(module, _approval_body())["status"] == 401
    assert _post_approval(
        module, _approval_body(), SIGNED, method="GET")["status"] == 405

    module, calls = _load()
    for bad in (dict(approvalId="A" * 32), dict(approvalId="0" * 31),
                dict(approvalId=7), dict(sessionId=""), dict(sessionId="a b"),
                dict(sessionId="x" * 65), dict(agent="x" * 81),
                dict(summary="x" * 201), dict(summary="two\nlines"),
                dict(agent="tab\there"), dict(summary="a\u2028b"),
                dict(agent="zero\u200bwidth"), dict(agent=None)):
        response = _post_approval(module, _approval_body(**bad), SIGNED)
        assert response["status"] == 400, bad
    assert _post_approval(module, ["x"], SIGNED)["status"] == 400
    assert calls["enqueued"] == []

    module, calls = _load()
    url_signed = _Request(
        "https://app.forkmesh.com/api/notifications/desktop?"
        + urlencode(SIGNED), "POST")
    url_signed.body = _approval_body()
    response = asyncio.run(module.agent_approval_ping_handler(None, url_signed))
    assert response["status"] == 401

    module, calls = _load(account={"status": "suspended"})
    assert _post_approval(module, _approval_body(), SIGNED)["status"] == 401
    assert calls["enqueued"] == []


def test_the_apns_banner_carries_the_approval():
    source = ENTRY.split("async def _apns_ping_alert(")[1].split(
        "\nasync def ")[0]
    assert 'record.get("kind") == "agent_approval"' in source
    assert 'alert["category"] = "agent-approval"' in source
    assert 'alert["approvalId"]' in source
