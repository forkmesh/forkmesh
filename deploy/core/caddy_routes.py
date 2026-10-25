#!/usr/bin/env python3
"""Generate the Caddy route snippets for app.next / world.next.

Cloudflare Workers Static Assets decide per request whether the Worker or the
asset store answers; on the core nodes Caddy makes the same decision so that
Python never serves static bytes (PLAN.md Performance). The decision is
derived from the manifests rather than hand-copied:

* ``run_worker_first`` of app/wrangler.toml and world/wrangler.toml: a path
  matching any positive glob and no ``!`` glob always goes to the App;
* otherwise an existing file in the site's ``dist`` is served by Caddy
  (precompressed, with the ``_headers`` rules of that dist), and everything
  else goes to the App, which applies Workers' html_handling and 404 rules
  itself (forkmesh_host.bindings.StaticAssets).

Globs follow Workers semantics: ``*`` matches any run of characters,
including ``/``; ``%40`` style escapes are compared decoded. Each glob list
becomes one anchored RE2 regular expression for Caddy's ``path_regexp``.

The output is imported by the rendered Caddyfile from
``/etc/forkmesh-next/caddy/{app,world}-routes.caddy``; tools/next_deploy.py
regenerates both from each release (its manifests and its dist/_headers)
and reloads Caddy when it switches releases.

Usage::

    caddy_routes.py app   --repo . [--dist DIR] [--root /opt/.../app/dist]
    caddy_routes.py world --repo . [--dist DIR] [--root ...] [--app-root ...]
"""

from __future__ import annotations

import argparse
import re
import sys
import tomllib
import urllib.parse
from pathlib import Path

DEFAULT_CURRENT = "/opt/forkmesh-next/app/current"
APP_UPSTREAM_SNIPPET = "app_proxy"
APP_HOST_UPSTREAM_SNIPPET = "app_proxy_app_host"

# Mirrors world/worker.js APP_ASSET_PATHS / isAppAsset(): paths the world
# origin serves from the App (its dist, else the App itself).
WORLD_APP_ASSET_PATHS = (
    "/account-events.js",
    "/api-client.js",
    "/chat-apps.js",
    "/chat-attachments.js",
    "/chat-crypto.js",
    "/chat-frame-auth.js",
    "/chat-issue-filing.js",
    "/chat-mlkem.js",
    "/chat-moderation.js",
    "/chat-retention.js",
    "/chat-room-transport.js",
    "/chat-seal.js",
    "/chat-call-facestream.js",
    "/fmm/cc.js",
    "/fmm/client.js",
    "/fmm/fec.js",
    "/fmm/fmm-playout-worklet.js",
    "/fmm/fmm-seal.js",
    "/fmm/fmm-worker.js",
    "/fmm/frag.js",
    "/fmm/hop.js",
    "/fmm/jitter.js",
    "/fmm/seqnum.js",
    "/fmm/wire.js",
    "/call-avatar-puppet.js",
    "/call-avatar-rig-wire.js",
    "/call-avatar-rig.js",
    "/call-quality-meter.js",
    "/call-speech-meter.js",
    "/chat-call-backgrounds.js",
    "/chat-call-e2ee.js",
    "/chat-call-face-custom.js",
    "/chat-call-face-expression.js",
    "/chat-call-face-geometry.js",
    "/chat-call-face-mesh.js",
    "/chat-call-face-render.js",
    "/chat-call-face-template.js",
    "/chat-call-face-tracker.js",
    "/chat-call-faces.js",
    "/chat-call-hand-tracker.js",
    "/chat-call-outfits.js",
    "/chat-call-segment.js",
    "/chat-call-voice-worklet.js",
    "/chat-call-voice.js",
    "/chat-group-call-crypto.js",
    "/chat-group-calls.js",
    "/fmm/fmm-call.js",
    "/fmm/fmm-e2ee.js",
    "/fmm/fmm-local.js",
    "/dashboard-chat.js",
    "/member-welcome.js",
    "/membership-prompt.js",
    "/analytics.js",
    "/safe-input.js",
    "/analytics-sdk.js",
    "/start",
)
WORLD_APP_ASSET_PREFIXES = ("/assets/", "/favicon/")
# world/worker.js mobileWorldHealth(): answered by the app host
# (app/host/forkmesh_host/world_health.py) from the files in world/dist, with
# the World's own Host header, so it carries x-forkmesh-worker: world.
WORLD_HEALTH_PATH = "/health/mobile"

_SAFE_HEADER_NAME = re.compile(r"^[A-Za-z0-9-]+$")


class RoutesError(ValueError):
    pass


# --------------------------------------------------------------------------
# globs


def glob_to_regex(pattern: str) -> str:
    """One Workers glob -> an unanchored RE2 fragment."""
    if not pattern.startswith("/"):
        raise RoutesError("route pattern must start with '/': %r" % pattern)
    decoded = urllib.parse.unquote(pattern)
    if any(ch.isspace() for ch in decoded) or "{" in decoded or "}" in decoded:
        raise RoutesError("unsupported characters in route pattern %r" % pattern)
    def literal(text):
        return "".join(ch if ch.isalnum() or ch in "/_-~@" else "\\" + ch
                       for ch in text)

    segments = []
    for segment in decoded.split("/"):
        # _headers/_redirects placeholders (":owner") match one segment.
        if re.fullmatch(r":[A-Za-z_][A-Za-z0-9_]*", segment):
            segments.append("[^/]+")
        else:
            segments.append(".*".join(literal(part) for part in segment.split("*")))
    return "/".join(segments)


def alternation(patterns) -> str:
    fragments = []
    for pattern in patterns:
        fragment = glob_to_regex(pattern)
        if fragment not in fragments:
            fragments.append(fragment)
    if not fragments:
        raise RoutesError("empty pattern list")
    return "^(?:" + "|".join(fragments) + ")$"


def split_worker_first(patterns):
    positive, negative = [], []
    for raw in patterns:
        if not isinstance(raw, str):
            raise RoutesError("run_worker_first entries must be strings")
        if raw.startswith("!"):
            negative.append(raw[1:])
        else:
            positive.append(raw)
    return positive, negative


def worker_first(patterns, path: str) -> bool:
    """Reference implementation used by the tests."""
    positive, negative = split_worker_first(patterns)
    path = urllib.parse.unquote(path)
    if not positive or not re.fullmatch(alternation(positive), path):
        return False
    return not (negative and re.fullmatch(alternation(negative), path))


def load_worker_first(manifest: Path):
    with open(manifest, "rb") as handle:
        data = tomllib.load(handle)
    patterns = ((data.get("assets") or {}).get("run_worker_first")) or []
    if isinstance(patterns, bool):
        raise RoutesError("run_worker_first = true/false is not supported")
    return list(patterns)


# --------------------------------------------------------------------------
# _headers (Cloudflare Pages/Workers format)


def parse_headers_file(text: str):
    """Return [(pattern, [(op, name, value)])] in file order.

    op is "set" or "unset" (a ``! Name`` line)."""
    rules = []
    current = None
    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not line[:1].isspace():
            current = (stripped, [])
            rules.append(current)
            continue
        if current is None:
            raise RoutesError("header line before any path: %r" % raw)
        if stripped.startswith("!"):
            name = stripped[1:].strip()
            op = ("unset", name, "")
        else:
            name, sep, value = stripped.partition(":")
            if not sep:
                raise RoutesError("malformed header line: %r" % raw)
            op = ("set", name.strip(), value.strip())
        if not _SAFE_HEADER_NAME.match(op[1]):
            raise RoutesError("unsafe header name %r" % op[1])
        current[1].append(op)
    return rules


def _caddy_quote(value: str) -> str:
    if "\n" in value or "\r" in value:
        raise RoutesError("header value spans lines")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def header_block(rules, prefix: str, indent: str = "\t"):
    """Named matchers (site level) and header directives (inside a handle).

    Named ``path_regexp`` matchers keep Caddy from re-sorting the header
    directives, so later rules override earlier ones as on Cloudflare."""
    matchers, directives = [], []
    for index, (pattern, ops) in enumerate(rules):
        if pattern.startswith("https://") or pattern.startswith("http://"):
            continue  # host-specific rules do not apply to these origins
        name = "@%s_h%d" % (prefix, index)
        matchers.append("%s path_regexp %s_h%d %s" % (
            name, prefix, index, alternation([pattern])))
        replaced = {h.lower() for op, h, _ in ops if op == "set"}
        for op, header, value in ops:
            if op == "unset" and header.lower() in replaced:
                continue
            if op == "unset":
                directives.append("%sheader %s -%s" % (indent, name, header))
            else:
                directives.append("%sheader %s %s %s" % (
                    indent, name, header, _caddy_quote(value)))
    return matchers, directives


# --------------------------------------------------------------------------
# snippets


def _static_handle(matcher: str, root: str, header_directives,
                   rewrite: str | None = None,
                   versioned_wins: bool = False) -> list[str]:
    lines = ["handle %s {" % matcher]
    lines.append("\troot * %s" % root)
    site = matcher.lstrip("@").split("_")[0]
    lines.append('\theader @%s_unversioned Cache-Control '
                 '"public, max-age=0, must-revalidate"' % site)
    lines.append('\theader @%s_versioned Cache-Control '
                 '"public, max-age=31536000, immutable"' % site)
    lines.extend(header_directives)
    if versioned_wins:
        lines.append('\theader @%s_versioned Cache-Control '
                     '"public, max-age=31536000, immutable"' % site)
    if rewrite:
        lines.append("\trewrite * %s" % rewrite)
    lines.append("\tfile_server {")
    lines.append("\t\tprecompressed br gzip")
    lines.append("\t}")
    lines.append("}")
    return lines


def app_routes(worker_first_patterns, headers_text: str, root: str) -> str:
    positive, negative = split_worker_first(worker_first_patterns)
    header_matchers, header_directives = header_block(
        parse_headers_file(headers_text) if headers_text else [], "app")
    out = [
        "# Generated by deploy/core/caddy_routes.py from app/wrangler.toml",
        "# run_worker_first and app/dist/_headers. Do not edit on the node.",
        "@app_wf {",
        "\tpath_regexp app_wf_pos %s" % alternation(positive),
    ]
    if negative:
        out.append("\tnot path_regexp app_wf_neg %s" % alternation(negative))
    out += [
        "}",
        "@app_file {",
        "\tmethod GET HEAD",
        "\tnot path *.html */",
        "\tfile {",
        "\t\troot %s" % root,
        "\t\ttry_files {path}",
        "\t}",
        "}",
        "@app_page {",
        "\tmethod GET HEAD",
        "\tnot path *.html */",
        "\tfile {",
        "\t\troot %s" % root,
        "\t\ttry_files {path}.html {path}/index.html",
        "\t}",
        "}",
        "@app_versioned query v=*",
        "@app_unversioned not query v=*",
    ]
    out += header_matchers
    out += ["handle @app_wf {", "\timport %s" % APP_UPSTREAM_SNIPPET, "}"]
    out += _static_handle("@app_file", root, header_directives)
    out += _static_handle("@app_page", root, header_directives,
                          rewrite="{http.matchers.file.relative}")
    out += ["handle {", "\timport %s" % APP_UPSTREAM_SNIPPET, "}"]
    return "\n".join(out) + "\n"


def world_routes(worker_first_patterns, headers_text: str, root: str,
                 app_root: str, app_headers_text: str = "") -> str:
    positive, negative = split_worker_first(worker_first_patterns)
    header_matchers, header_directives = header_block(
        parse_headers_file(headers_text) if headers_text else [], "world")
    app_matchers, app_directives = header_block(
        parse_headers_file(app_headers_text) if app_headers_text else [],
        "wapp", indent="\t\t")
    app_asset_regex = alternation(
        list(WORLD_APP_ASSET_PATHS) + [p + "*" for p in WORLD_APP_ASSET_PREFIXES])
    out = [
        "# Generated by deploy/core/caddy_routes.py from world/wrangler.toml",
        "# run_worker_first, world/dist/_headers and world/worker.js. Do not",
        "# edit on the node.",
        "@world_index path / /world /world/",
        "@world_health path %s" % WORLD_HEALTH_PATH,
        "@world_wf {",
        "\tpath_regexp world_wf_pos %s" % alternation(positive),
    ]
    if negative:
        out.append("\tnot path_regexp world_wf_neg %s" % alternation(negative))
    out += [
        "}",
        "@world_file {",
        "\tmethod GET HEAD",
        "\tfile {",
        "\t\troot %s" % root,
        "\t\ttry_files {path}",
        "\t}",
        "}",
        "@world_app_file {",
        "\tmethod GET HEAD",
        "\tpath_regexp world_app_file %s" % app_asset_regex,
        "\tfile {",
        "\t\troot %s" % app_root,
        "\t\ttry_files {path}",
        "\t}",
        "}",
        "@world_app_asset path_regexp world_app_asset %s" % app_asset_regex,
        "@world_versioned expression `query({'v': '*'}) || path('/world/vendor/*')`",
        "@world_unversioned expression `!(query({'v': '*'}) || path('/world/vendor/*'))`",
        "@wapp_versioned query v=*",
        "@wapp_unversioned not query v=*",
    ]
    out += header_matchers + app_matchers

    def app_asset_handles(indent: str) -> list[str]:
        inner = indent + "\t"
        lines = [indent + "handle @world_app_file {",
                 inner + "root * %s" % app_root,
                 inner + 'header @wapp_unversioned Cache-Control '
                 '"public, max-age=0, must-revalidate"',
                 inner + 'header @wapp_versioned Cache-Control '
                 '"public, max-age=31536000, immutable"']
        lines += [inner + d.strip() for d in app_directives]
        lines += [inner + "file_server {", inner + "\tprecompressed br gzip",
                  inner + "}", indent + "}",
                  indent + "handle @world_app_asset {",
                  inner + "import %s" % APP_HOST_UPSTREAM_SNIPPET,
                  indent + "}"]
        return lines

    world_index = [
        "handle @world_index {",
        "\troot * %s" % root,
    ] + header_directives + [
        "\theader Cache-Control \"no-store, max-age=0, must-revalidate\"",
        "\trewrite * /world/index.html",
        "\tfile_server {",
        "\t\tprecompressed br gzip",
        "\t}",
        "}",
    ]
    out += world_index
    out += [
        "handle @world_health {",
        "\timport %s" % APP_UPSTREAM_SNIPPET,
        "}",
        "handle @world_wf {",
    ]
    out += app_asset_handles("\t")
    out += ["\thandle {", "\t\timport %s" % APP_UPSTREAM_SNIPPET, "\t}", "}"]
    out += _static_handle("@world_file", root, header_directives,
                          versioned_wins=True)
    out += app_asset_handles("")
    out += ["handle {", "\timport %s" % APP_UPSTREAM_SNIPPET, "}"]
    return "\n".join(out) + "\n"


def _read(path: Path | None) -> str:
    if path and path.is_file():
        return path.read_text(encoding="utf-8")
    return ""


def generate(site: str, repo: Path, dist: Path | None = None,
             root: str | None = None, app_root: str | None = None,
             app_dist: Path | None = None) -> str:
    if site == "app":
        dist = dist or repo / "app" / "dist"
        return app_routes(
            load_worker_first(repo / "app" / "wrangler.toml"),
            _read(dist / "_headers"),
            root or DEFAULT_CURRENT + "/app/dist")
    if site == "world":
        dist = dist or repo / "world" / "dist"
        app_dist = app_dist or repo / "app" / "dist"
        return world_routes(
            load_worker_first(repo / "world" / "wrangler.toml"),
            _read(dist / "_headers"),
            root or DEFAULT_CURRENT + "/world/dist",
            app_root or DEFAULT_CURRENT + "/app/dist",
            _read(app_dist / "_headers"))
    raise RoutesError("site must be app or world")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("site", choices=("app", "world"))
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument("--dist")
    parser.add_argument("--app-dist")
    parser.add_argument("--root")
    parser.add_argument("--app-root")
    args = parser.parse_args(argv)
    try:
        sys.stdout.write(generate(
            args.site, Path(args.repo),
            Path(args.dist) if args.dist else None, args.root, args.app_root,
            Path(args.app_dist) if args.app_dist else None))
    except RoutesError as error:
        print("caddy_routes: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
