import asyncio
import importlib.util
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlencode, urlparse

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
FOOTPRINT = (
    ROOT.parent / "world" / "tools" / "build_worker_footprint.py"
).read_text(encoding="utf-8")
NOW = 1_790_000_000_000


class _Date:
    @staticmethod
    def now():
        return NOW


class _Request:
    def __init__(self, url, method="POST"):
        self.url = url
        self.method = method


def _load(admin_ok=True, admins=("jett", "ops"), emails=("ops@forkmesh.com",)):
    spec = importlib.util.spec_from_file_location(
        "operator_alerts_under_test", SRC / "operator_alerts.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    calls = {"enqueued": [], "authorized": [], "emails": []}

    async def ensure_schema(env):
        return None

    async def bounded_json_request(request):
        return request.body

    def json_response(payload, status=200, **kwargs):
        return {"status": status, "payload": payload}

    async def _admin_authorized(env, node, ts, sig, canonical):
        calls["authorized"].append((node, ts, sig, canonical))
        return {"name": node} if admin_ok else None

    async def d1_all(env, sql, *args):
        return [{"data": name} for name in admins] + [{"data": "robot"}]

    async def decrypt_row(env, data):
        if data == "robot":
            return {"name": "robot", "service_account": True}
        return {"name": data}

    async def enqueue_notification(env, recipient, kind, title, **kwargs):
        calls["enqueued"].append((recipient, kind, title, kwargs))
        return True

    async def _repository_monitor_admin_emails(env):
        return list(emails)

    async def _send_email(env, address, subject, text, email_kind="account"):
        calls["emails"].append((address, subject, text, email_kind))
        return True

    module._bind_runtime({
        "Date": _Date,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
        "ensure_schema": ensure_schema,
        "bounded_json_request": bounded_json_request,
        "json_response": json_response,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "MAX_NODE_NAME": 64,
        "_admin_authorized": _admin_authorized,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "enqueue_notification": enqueue_notification,
        "_repository_monitor_admin_emails": _repository_monitor_admin_emails,
        "_send_email": _send_email,
    })
    return module, calls


def _post(module, body, query=None, method="POST"):
    url = "https://app.forkmesh.com/api/accounts/admin-operator-alert"
    if query:
        url += "?" + urlencode(query)
    request = _Request(url, method)
    request.body = body
    return asyncio.run(module.operator_alert_handler(None, request))


def _body(**overrides):
    body = {"severity": "ping", "key": "deploy-failed:0123456789ab",
            "title": "Deploy of 0123456789ab failed", "body": "tests broke"}
    body.update(overrides)
    return body


def test_route_and_lazy_module_are_wired():
    assert '_operator_alerts = _LazyModule("operator_alerts")' in ENTRY
    assert ('operator_alert_handler = _operator_alerts.export('
            '"operator_alert_handler")') in ENTRY
    assert ('if url.path == "/api/accounts/admin-operator-alert" and method == '
            '"POST":\n        return await operator_alert_handler(env, request)'
            ) in ENTRY
    assert '"operator_alerts.py",' in FOOTPRINT


def test_the_deploy_agent_signs_the_same_canonical():
    module, _ = _load()
    sys.path.insert(0, str(ROOT.parent / "tools"))
    import next_autodeploy
    assert next_autodeploy.ALERT_PROOF == module.OPERATOR_ALERT_PROOF
    assert next_autodeploy.ALERT_PATH == module.OPERATOR_ALERT_PATH


def test_signed_ping_reaches_every_admin_but_not_service_accounts():
    module, calls = _load()
    response = _post(module, _body(), {"node": "jett", "ts": "123", "sig": "s"})
    assert response == {"status": 200, "payload": {"ok": True, "pings": 2, "emails": 0}}
    [(node, ts, sig, canonical)] = calls["authorized"]
    assert (node, ts, sig) == ("jett", "123", "s")
    assert canonical == module.operator_alert_canonical(
        "jett", "123", "ping", "deploy-failed:0123456789ab",
        "Deploy of 0123456789ab failed", "tests broke")
    assert [c[0] for c in calls["enqueued"]] == ["jett", "ops"]
    recipient, kind, title, kwargs = calls["enqueued"][0]
    assert kind == "operational_alert"
    assert kwargs["dedupe"] == "operator:deploy-failed:0123456789ab"
    assert kwargs["source"] == "operator"
    assert calls["emails"] == []


def test_email_severity_also_mails_the_admin_addresses():
    module, calls = _load()
    response = _post(module, _body(severity="email"),
                     {"node": "jett", "ts": "123", "sig": "s"})
    assert response["payload"] == {"ok": True, "pings": 2, "emails": 1}
    [(address, subject, text, kind)] = calls["emails"]
    assert address == "ops@forkmesh.com"
    assert subject == "ForkMesh: Deploy of 0123456789ab failed"
    assert kind == "operational"


def test_canonical_binds_every_field():
    module, _ = _load()
    base = module.operator_alert_canonical("jett", "1", "ping", "k", "t", "b")
    for changed in (("ops", "1", "ping", "k", "t", "b"),
                    ("jett", "2", "ping", "k", "t", "b"),
                    ("jett", "1", "email", "k", "t", "b"),
                    ("jett", "1", "ping", "k2", "t", "b"),
                    ("jett", "1", "ping", "k", "t2", "b"),
                    ("jett", "1", "ping", "k", "t", "b2")):
        assert module.operator_alert_canonical(*changed) != base


def test_refusals():
    module, calls = _load(admin_ok=False)
    query = {"node": "jett", "ts": "123", "sig": "s"}
    assert _post(module, _body(), query)["status"] == 401
    assert _post(module, _body(), query, method="GET")["status"] == 405
    for bad in (_body(severity="page"), _body(key="has space"), _body(key=""),
                _body(title=""), _body(title="x" * 121), _body(body="x" * 2001),
                ["not", "a", "dict"]):
        assert _post(module, bad, query)["status"] == 400
    assert calls["enqueued"] == []
