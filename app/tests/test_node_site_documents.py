"""Node-origin Docs/Blog delivery contracts (issue #625)."""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlparse


APP = Path(__file__).resolve().parents[1]
ENTRY = APP / "src" / "app_runtime.py"
SOURCE = ENTRY.read_text(encoding="utf-8")


class Headers:
    def __init__(self, values=None):
        if isinstance(values, Headers):
            values = values.items()
        elif isinstance(values, dict):
            values = values.items()
        self._values = {
            str(name).lower(): str(value)
            for name, value in (values or ())
        }

    def get(self, name, default=None):
        return self._values.get(str(name).lower(), default)

    def has(self, name):
        return str(name).lower() in self._values

    def items(self):
        return self._values.items()


class Response:
    def __init__(self, body=None, *, status=200, status_text="", headers=None):
        self.body = body
        self.status = status
        self.status_text = status_text
        self.headers = Headers(headers)


class JsResponse:
    @staticmethod
    def new(body, init):
        return Response(
            body,
            status=int(init.get("status", 200)),
            headers=init.get("headers"),
        )


class Request:
    def __init__(self, method="GET", headers=None):
        self.method = method
        self.headers = Headers(headers)


class UpstreamResponse:
    def __init__(self, body, *, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = Headers(headers)

    async def text(self):
        return self.body


def _document_default(fetch):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    constants = {
        "SITE_DOCUMENT_PROXY_TIMEOUT_SECONDS",
        "SITE_DOCUMENT_PROXY_MAX_BYTES",
        "SITE_DOCUMENT_ORIGIN_HEADER",
        "SITE_DOCUMENT_SECURITY_HEADERS",
        "SITE_DOCUMENT_FALLBACK_SECURITY_HEADERS",
    }
    assignments = [
        node for node in tree.body
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in constants
            for target in node.targets
        )
    ]
    assert {
        target.id
        for node in assignments
        for target in node.targets
        if isinstance(target, ast.Name)
    } == constants
    default = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "Default"
    )
    names = (
        "_without_head_body",
        "_request_has_header",
        "_site_document_headers",
        "_serve_site_document",
        "_proxy_site_document",
    )
    methods = [
        node for node in default.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    assert {node.name for node in methods} == set(names)
    module = ast.fix_missing_locations(ast.Module(
        body=[*assignments, ast.ClassDef(
            name="Default",
            bases=[],
            keywords=[],
            body=methods,
            decorator_list=[],
        )],
        type_ignores=[],
    ))
    namespace = {
        "JsResponse": JsResponse,
        "Response": Response,
        "js_fetch_with_timeout": fetch,
        "method_name": lambda request: request.method,
        "to_js": lambda value: value,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["Default"](), namespace


def test_node_site_document_proxy_keeps_credentials_local_and_preserves_policy():
    calls = []

    async def fetch(url, options, timeout):
        calls.append((url, options, timeout))
        return UpstreamResponse(
            '<!doctype html><img src="/assets/blog/features/card.webp">',
            headers={
                "content-type": "text/html; charset=utf-8",
                "content-security-policy": "default-src 'self'",
                "x-frame-options": "DENY",
            },
        )

    default, namespace = _document_default(fetch)

    local_asset_reads = []

    class Assets:
        async def fetch(self, _url):
            local_asset_reads.append(_url)
            # The normal App bundle deliberately has no full docs/blog tree.
            return UpstreamResponse("not found", status=404)

    default.env = SimpleNamespace(ASSETS=Assets())
    request = Request(headers={
        "accept": "image/webp",
        "accept-language": "private-language",
        "authorization": "Bearer must-stay-on-node",
        "cookie": "forkmesh_session=node-only",
    })
    response = asyncio.run(default._serve_site_document(
        request,
        urlparse("https://pp.forkmesh.com/blog/parallel-agents/"),
        "https://forkmesh.com",
    ))

    assert response.status == 200
    assert response.headers.get("x-robots-tag") == "noindex"
    assert response.headers.get("content-security-policy") == "default-src 'self'"
    assert response.headers.get("x-frame-options") == "DENY"
    assert response.headers.get("referrer-policy") == (
        "strict-origin-when-cross-origin"
    )
    assert response.body == (
        '<!doctype html><img src="https://forkmesh.com/'
        'assets/blog/features/card.webp">'
    )

    assert len(calls) == 1
    # Even though App ships a compatibility blog shell, node origins must use
    # the canonical document (and its current artwork), never that partial
    # local copy.
    assert local_asset_reads == []
    destination, options, timeout = calls[0]
    assert destination == "https://forkmesh.com/blog/parallel-agents/"
    assert timeout == namespace["SITE_DOCUMENT_PROXY_TIMEOUT_SECONDS"]
    assert options["method"] == "GET"
    assert options["redirect"] == "follow"
    assert options["headers"] == {
        "accept": "text/html,application/json;q=0.9,*/*;q=0.1",
        "x-forkmesh-site-document-proxy": "pp.forkmesh.com",
    }
    assert "cookie" not in options["headers"]
    assert "authorization" not in options["headers"]
    assert "accept-language" not in options["headers"]


def test_node_site_document_proxy_returns_a_bodyless_head_response():
    async def fetch(_url, _options, _timeout):
        return UpstreamResponse(
            "<!doctype html><title>Docs</title>",
            headers={"content-type": "text/html; charset=utf-8"},
        )

    default, _ = _document_default(fetch)

    class Assets:
        async def fetch(self, _url):
            return UpstreamResponse("not found", status=404)

    default.env = SimpleNamespace(ASSETS=Assets())
    response = asyncio.run(default._serve_site_document(
        Request(method="HEAD"),
        urlparse("https://pp.forkmesh.com/docs/protocol/"),
        "https://forkmesh.com",
    ))

    assert response.status == 200
    assert response.body is None
    assert response.headers.get("content-type") == "text/html; charset=utf-8"


def test_node_site_document_proxy_keeps_linked_docs_json_available():
    async def fetch(_url, _options, _timeout):
        return UpstreamResponse(
            '{"$schema":"https://json-schema.org/draft/2020-12/schema"}',
            headers={"content-type": "application/schema+json"},
        )

    default, _ = _document_default(fetch)
    default.env = SimpleNamespace(ASSETS=SimpleNamespace())
    response = asyncio.run(default._serve_site_document(
        Request(),
        urlparse(
            "https://pp.forkmesh.com/"
            "docs/protocol/collaboration-event-v1.schema.json"
        ),
        "https://forkmesh.com",
    ))

    assert response.status == 200
    assert response.body == (
        '{"$schema":"https://json-schema.org/draft/2020-12/schema"}'
    )
    assert response.headers.get("content-type") == "application/schema+json"
