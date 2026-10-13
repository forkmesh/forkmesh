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
