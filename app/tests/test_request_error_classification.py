import ast
import asyncio
import urllib.parse
from pathlib import Path
from types import SimpleNamespace

SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = SRC / "entry.py"
APP_RUNTIME = SRC / "app_runtime.py"
HELPERS = {"_error_chain", "_is_client_disconnect", "_is_too_many_fields"}


def _helpers():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, ast.FunctionDef) and n.name in HELPERS]
    assert {n.name for n in selected} == HELPERS
    namespace = {}
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(ENTRY), "exec"), namespace)
    return namespace


def _fetch_inner():
    tree = ast.parse(APP_RUNTIME.read_text(encoding="utf-8"),
                     filename=str(APP_RUNTIME))
    default = next(n for n in tree.body
                   if isinstance(n, ast.ClassDef) and n.name == "Default")
    method = next(n for n in default.body
                  if isinstance(n, ast.AsyncFunctionDef)
                  and n.name == "_fetch_inner")
    logged = []

    async def record(*args, **kwargs):
        logged.append(args)

    async def passthrough_page_view(env, request, url, response, status):
        return response

    async def nothing(*args, **kwargs):
        return None

    namespace = dict(_helpers())
    namespace.update({
        "urlparse": urllib.parse.urlparse,
        "RequestBodyTooLarge": type("RequestBodyTooLarge", (Exception,), {}),
        "UNVERIFIED_EMAIL_ERROR": "unverified",
        "EXPECTED_DEGRADED_HEADERS": {},
        "_api_cors_preflight_response": lambda env, request, path: None,
        "_admin_path": lambda env: "",
        "_is_d1_platform_error": lambda error: False,
        "capture_worker_exception": record,
        "log_error": record,
        "log_d1_unavailable": record,
        "json_response": lambda body, status=200, **_: SimpleNamespace(
            status=status, body=body),
        "_api_cors_response": lambda env, request, response, path: response,
        "_apply_browser_security_headers": lambda response, path: response,
        "_attribution": SimpleNamespace(
            capture_page_view=passthrough_page_view),
        "_api_metrics": SimpleNamespace(record_api_request=record),
        "record_site_referral": nothing,
        "record_blog_visit": nothing,
        "_is_tunnel_content_path": lambda path: False,
        "_response_is_expected_degraded": lambda response: False,
        "method_name": lambda request: request.method,
    })
    exec(compile(ast.Module(body=[method], type_ignores=[]),
                 str(APP_RUNTIME), "exec"), namespace)
    return namespace["_fetch_inner"], logged


def _run(error):
    fetch_inner, logged = _fetch_inner()

    async def route(request, url):
        raise error

    runtime = SimpleNamespace(
        env=None, _public_snapshot_path=lambda request, url: None,
        _route=route)
    request = SimpleNamespace(
        url="https://app.next.forkmesh.com/api/runners/claim",
        method="POST", headers={})
    response = asyncio.run(fetch_inner(runtime, request))
    return response, logged


class ClientDisconnected(ConnectionResetError):
    pass


def test_disconnect_is_recognised_through_wrapping():
    is_disconnect = _helpers()["_is_client_disconnect"]
    assert is_disconnect(ClientDisconnected("client disconnected"))
    assert is_disconnect(ConnectionResetError("client disconnected"))
    try:
        try:
            raise ClientDisconnected("client disconnected")
        except ConnectionResetError as inner:
            raise RuntimeError("read failed") from inner
    except RuntimeError as outer:
        assert is_disconnect(outer)
    assert not is_disconnect(ConnectionResetError("upstream reset"))
    assert not is_disconnect(ValueError("client disconnected"))


def test_too_many_fields_is_recognised():
    too_many = _helpers()["_is_too_many_fields"]
    try:
        urllib.parse.parse_qs("&".join("f%d=1" % i for i in range(50)),
                              max_num_fields=40)
    except ValueError as error:
        assert too_many(error)
    else:
        raise AssertionError("parse_qs did not refuse")
    assert not too_many(ValueError("bad number"))


def test_a_disconnected_client_is_not_logged_as_a_500():
    response, logged = _run(ClientDisconnected("client disconnected"))
    assert response.status == 499
    assert logged == []


def test_too_many_fields_answers_400_without_logging():
    response, logged = _run(ValueError("Max number of fields exceeded"))
    assert response.status == 400
    assert response.body == {"error": "too_many_fields"}
    assert logged == []


def test_other_errors_are_still_captured_and_raised():
    try:
        _run(ConnectionResetError("upstream reset"))
    except ConnectionResetError:
        pass
    else:
        raise AssertionError("the error was swallowed")
