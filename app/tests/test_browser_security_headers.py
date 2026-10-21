"""Static and Worker-rendered HTML carry the same enforced browser policy."""

import ast
from pathlib import Path
import re


HEADERS = (
    Path(__file__).resolve().parents[2] / "www" / "public" / "_headers"
).read_text(encoding="utf-8")
ENTRY_PATH = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY = ENTRY_PATH.read_text(encoding="utf-8")
RUNTIME_PATH = ENTRY_PATH.with_name("app_runtime.py")
RUNTIME = RUNTIME_PATH.read_text(encoding="utf-8")


def _default_runtime_source():
    for node in ast.parse(RUNTIME, filename=str(RUNTIME_PATH)).body:
        if isinstance(node, ast.ClassDef) and node.name == "Default":
            return ast.get_source_segment(RUNTIME, node)
    raise AssertionError("Default runtime class is missing")


def _rule(path):
    marker = f"{path}\n"
    start = HEADERS.index(marker)
    lines = []
    for line in HEADERS[start + len(marker):].splitlines():
        if line and not line[0].isspace() and not line.startswith("#"):
            break
        if line.startswith((" ", "\t")):
            lines.append(line.strip())
    return "\n".join(lines)


def test_global_static_security_headers_are_enforced():
    assert HEADERS.startswith("/*\n")
    for header in (
        "Content-Security-Policy:",
        "Strict-Transport-Security:",
        "Referrer-Policy:",
        "Permissions-Policy:",
        "X-Content-Type-Options: nosniff",
        "X-Frame-Options: DENY",
        "Cross-Origin-Opener-Policy: same-origin",
    ):
        assert header in HEADERS
    assert "Content-Security-Policy-Report-Only" not in HEADERS
    assert "frame-ancestors 'none'" in HEADERS
    assert "object-src 'none'" in HEADERS
    assert "base-uri 'self'" in HEADERS


def test_only_explicit_embedded_documents_are_frameable():
    global_rule = _rule("/*")
    assert "frame-ancestors 'none'" in global_rule
    assert "X-Frame-Options: DENY" in global_rule
    assert "https://static.cloudflareinsights.com" in global_rule

    # Chat renders inside World frames; issue and pull details render inside
    # the World workbench. These documents stay same-origin only.
    for path in (
        "/dashboard/chat*",
        "/chat",
        "/chat.html",
        "/:owner/:repo/issues/:number",
        "/:owner/:repo/pull/:number",
    ):
        rule = _rule(path)
        assert "! Content-Security-Policy" in rule
        assert "frame-ancestors 'self'" in rule
        assert "frame-ancestors 'none'" not in rule
        assert "! X-Frame-Options" in rule
        assert "X-Frame-Options: SAMEORIGIN" in rule
        assert "X-Frame-Options: DENY" not in rule
        assert "https://cdn.jsdelivr.net" in rule
        assert "https://static.cloudflareinsights.com" in rule

    world_ancestors = (
        "frame-ancestors 'self' https://forkmesh.com "
        "https://forkmesh.com"
    )
    for path in ("/world", "/world/*"):
        rule = _rule(path)
        assert "Cache-Control: no-store, max-age=0, must-revalidate" in rule
        assert "! Content-Security-Policy" in rule
        csp = next(
            line.removeprefix("Content-Security-Policy: ")
            for line in rule.splitlines()
            if line.startswith("Content-Security-Policy: ")
        )
        ancestor_directive = next(
            directive.strip() for directive in csp.split(";")
            if directive.strip().startswith("frame-ancestors ")
        )
        assert ancestor_directive == world_ancestors
        assert "frame-ancestors 'none'" not in rule
        assert "! X-Frame-Options" in rule
        assert "X-Frame-Options:" not in rule
        assert "https://cdn.jsdelivr.net" in rule
        assert "https://static.cloudflareinsights.com" in rule

    assert HEADERS.count("frame-ancestors 'self'") == 7
    assert HEADERS.count("X-Frame-Options: SAMEORIGIN") == 5
    assert HEADERS.count(world_ancestors) == 2
    assert "https://cdn.tailwindcss.com" not in HEADERS


def test_policy_does_not_grant_sensitive_device_capabilities():
    permissions = next(
        line for line in HEADERS.splitlines()
        if "Permissions-Policy:" in line
    )
    for blocked in (
        "camera=()",
        "geolocation=()",
        "payment=()",
        "usb=()",
        "serial=()",
        "browsing-topics=()",
    ):
        assert blocked in permissions
    # Speech is an explicit, same-origin opt-in feature.
    assert "microphone=(self)" in permissions


def test_dynamic_app_html_receives_the_same_security_boundary():
    wanted = {
        "_BROWSER_CSP",
        "_BROWSER_SECURITY_HEADERS",
        "_apply_browser_security_headers",
        "_TURNSTILE_CHALLENGE_ORIGIN",
        "_TURNSTILE_PAGE_PATHS",
        "_DEVICE_PAGE_PERMISSIONS",
        "_DEVICE_PAGE_MEDIA_SRC",
        "_plugin_device_page",
    }
    nodes = []
    for node in ast.parse(ENTRY, filename=str(ENTRY_PATH)).body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in wanted
            for target in node.targets
        ):
            nodes.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in wanted:
            nodes.append(node)
    namespace = {"re": re}
    exec(
        compile(
            ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[])),
            str(ENTRY_PATH),
            "exec",
        ),
        namespace,
    )

    class HeaderMap(dict):
        def get(self, name, default=None):
            return super().get(name.lower(), default)

        def set(self, name, value):
            self[name.lower()] = value

    class DynamicResponse:
        def __init__(self):
            self.headers = HeaderMap({"content-type": "text/html; charset=utf-8"})

    ordinary = namespace["_apply_browser_security_headers"](
        DynamicResponse(), "/dashboard"
    )
    assert ordinary.headers["x-frame-options"] == "DENY"
    assert "frame-ancestors 'none'" in ordinary.headers[
        "content-security-policy"
    ]
    for header in (
        "strict-transport-security",
        "referrer-policy",
        "permissions-policy",
        "x-content-type-options",
        "cross-origin-opener-policy",
    ):
        assert ordinary.headers[header]

    embedded = namespace["_apply_browser_security_headers"](
        DynamicResponse(), "/alice/repo/issues/7"
    )
    assert embedded.headers["x-frame-options"] == "SAMEORIGIN"
    assert "frame-ancestors 'self'" in embedded.headers[
        "content-security-policy"
    ]
    assert "_apply_browser_security_headers(" in _default_runtime_source()

    # Only the Turnstile pages may load challenges.cloudflare.com.
    challenge = "https://challenges.cloudflare.com"
    signup_csp = namespace["_apply_browser_security_headers"](
        DynamicResponse(), "/signup"
    ).headers["content-security-policy"]
    directives = {
        part.strip().split(" ", 1)[0]: part.strip()
        for part in signup_csp.split(";") if part.strip()
    }
    assert challenge in directives["script-src"]
    assert challenge in directives["frame-src"]
    assert "frame-ancestors 'none'" in signup_csp
    for path in ("/dashboard", "/login", "/alice/repo/issues/7"):
        other = namespace["_apply_browser_security_headers"](
            DynamicResponse(), path
        ).headers["content-security-policy"]
        assert challenge not in other

    assert "camera=()" in ordinary.headers["permissions-policy"]
    assert "mediastream:" not in ordinary.headers["content-security-policy"]
    devices = {"/dashboard/studio": ("camera", "microphone"),
               "/dashboard/listen": ("microphone",)}
    namespace["_plugin_device_page"] = lambda path: devices.get(path, ())
    studio = namespace["_apply_browser_security_headers"](
        DynamicResponse(), "/dashboard/studio")
    assert "camera=(self)" in studio.headers["permissions-policy"]
    assert "microphone=(self)" in studio.headers["permissions-policy"]
    assert "media-src 'self' blob: https: mediastream:" in studio.headers[
        "content-security-policy"]
    assert studio.headers["x-frame-options"] == "DENY"
    listen = namespace["_apply_browser_security_headers"](
        DynamicResponse(), "/dashboard/listen")
    assert "camera=()" in listen.headers["permissions-policy"]
    assert "mediastream:" in listen.headers["content-security-policy"]
    for path in ("/dashboard", "/dashboard/studio/x", "/studio"):
        other = namespace["_apply_browser_security_headers"](
            DynamicResponse(), path)
        assert "camera=()" in other.headers["permissions-policy"]
        assert "mediastream:" not in other.headers["content-security-policy"]

    strict = DynamicResponse()
    strict.headers["content-security-policy"] = (
        "default-src 'none'; script-src 'nonce-abc'; "
        "form-action https://mirror.example")
    kept = namespace["_apply_browser_security_headers"](strict, "/dashboard")
    assert kept.headers["content-security-policy"] == (
        "default-src 'none'; script-src 'nonce-abc'; "
        "form-action https://mirror.example")
    assert kept.headers["x-frame-options"] == "DENY"
    assert kept.headers["strict-transport-security"]


def test_static_turnstile_page_alone_allows_the_challenge_origin():
    challenge = "https://challenges.cloudflare.com"
    assert challenge not in _rule("/*")
    for path in ("/forgot-password", "/forgot-password.html"):
        rule = _rule(path)
        assert "! Content-Security-Policy" in rule
        csp = rule.split("Content-Security-Policy: ", 1)[1]
        script = [d for d in csp.split(";") if d.strip().startswith("script-src")]
        frame = [d for d in csp.split(";") if d.strip().startswith("frame-src")]
        assert challenge in script[0] and challenge in frame[0]
        assert "frame-ancestors 'none'" in csp
