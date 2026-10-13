#!/usr/bin/env python3
"""Publish enabled request_block_rules rows to Cloudflare's earliest WAF phase."""

from __future__ import annotations

import argparse
import ipaddress
import json
import os
import shlex
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


API_ROOT = "https://api.cloudflare.com/client/v4"
MANAGED_REF = "forkmesh_database_request_policy"
MANAGED_DESCRIPTION = "ForkMesh database request policy"
MAX_EXPRESSION_BYTES = 3_500
SUPPORTED_TYPES = {"user_agent_keyword", "ip", "cidr", "ip_range"}
VERIFIED_GOOGLE_CRAWLER = (
    '(cf.client.bot and lower(http.user_agent) contains "google")')
WELCOME_AGENTS = (
    '(lower(http.user_agent) contains "chatgpt-user" or '
    'lower(http.user_agent) contains "oai-searchbot")')
ENABLED_RULES_SQL = (
    "SELECT rule_id,match_type,pattern,priority FROM request_block_rules "
    "WHERE enabled=1 ORDER BY priority DESC,created_at,rule_id LIMIT 1000")
REPOSITORY = Path(__file__).resolve().parents[2]


def load_env(path: Path) -> dict[str, str]:
    values = dict(os.environ)
    if not path.exists():
        return values
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        name, raw_value = line.split("=", 1)
        name = name.strip()
        if not name:
            continue
        try:
            parsed = shlex.split(raw_value.strip(), comments=False, posix=True)
        except ValueError:
            parsed = [raw_value.strip()]
        values.setdefault(name, parsed[0] if parsed else "")
    return values


def wrangler_rows(raw: str) -> list[dict]:
    decoder = json.JSONDecoder()
    payload = None
    for offset, character in enumerate(raw):
        if character not in "[{":
            continue
        try:
            candidate, _end = decoder.raw_decode(raw[offset:])
        except json.JSONDecodeError:
            continue
        if isinstance(candidate, (dict, list)):
            payload = candidate
            break
    if payload is None:
        raise ValueError("Wrangler did not return a JSON result")
    blocks = payload if isinstance(payload, list) else [payload]
    rows = []
    for block in blocks:
        if not isinstance(block, dict):
            continue
        if block.get("error"):
            raise ValueError("database query failed: %s" % block["error"])
        for item in block.get("results") or []:
            if not isinstance(item, dict):
                continue
            if item.get("error"):
                raise ValueError("database query failed: %s" % item["error"])
            if isinstance(item.get("rows"), list):
                rows.extend(row for row in item["rows"] if isinstance(row, dict))
            elif "types" not in item:
                rows.append(item)
    return rows


def rqlite_rows() -> list[dict]:
    tools = str(REPOSITORY / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import next_common
    import next_deploy
    fleet = next_common.load_fleet(None)
    ssh = next_common.Ssh()
    node = next_deploy.find_rqlite_node(fleet, ssh)
    completed = ssh.run(node["public_ipv4"], next_common.rqlite_curl(
        "/db/query?level=strong&associative&q="
        + urllib.parse.quote(ENABLED_RULES_SQL)), check=True, timeout=60)
    raw = completed.stdout
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8")
    payload = json.loads(raw or "{}")
    results = payload.get("results") if isinstance(payload, dict) else None
    if not isinstance(results, list) or not results:
        raise RuntimeError("rqlite returned no result for the rule query")
    return wrangler_rows(raw)


def _quoted(value: str) -> str:
    # Rules-language string escaping matches JSON for the bounded ASCII policy
    # values accepted here.
    if any(ord(character) < 32 for character in value):
        raise ValueError("control characters are not valid in request rules")
    return json.dumps(value, ensure_ascii=True)


def build_expression(rows: list[dict]) -> str:
    user_agent_clauses = []
    networks = []
    for row in rows:
        kind = str(row.get("match_type") or "").strip().lower()
        pattern = str(row.get("pattern") or "").strip()
        if kind not in SUPPORTED_TYPES or not pattern:
            raise ValueError("invalid enabled request-policy row")
        if kind == "user_agent_keyword":
            if len(pattern) > 256:
                raise ValueError("user-agent keyword exceeds 256 characters")
            user_agent_clauses.append(
                "(lower(http.user_agent) contains %s)" % _quoted(pattern.lower()))
            continue
        if kind == "ip":
            networks.append(str(ipaddress.ip_address(pattern)))
        elif kind == "cidr":
            networks.append(str(ipaddress.ip_network(pattern, strict=False)))
        else:
            start_text, end_text = (
                part.strip() for part in pattern.split("-", 1))
            start = ipaddress.ip_address(start_text)
            end = ipaddress.ip_address(end_text)
            if start.version != end.version or int(start) > int(end):
                raise ValueError("invalid request-policy address range")
            networks.append("%s..%s" % (start, end))
    clauses = []
    if user_agent_clauses:
        # A Google-looking User-Agent is not enough to bypass the rule:
        # cf.client.bot is Cloudflare's verified-crawler signal. This permits
        # genuine Google indexing while fake Googlebot strings remain blocked.
        clauses.append(
            "((%s) and not %s and not %s)"
            % (" or ".join(user_agent_clauses), VERIFIED_GOOGLE_CRAWLER,
               WELCOME_AGENTS))
    if networks:
        clauses.append("(ip.src in {%s})" % " ".join(networks))
    expression = " or ".join(clauses)
    if len(expression.encode("utf-8")) > MAX_EXPRESSION_BYTES:
        raise ValueError("enabled request rules exceed the WAF expression budget")
    return expression


class Cloudflare:
    def __init__(self, token: str):
        if not token:
            raise ValueError("CLOUDFLARE_API_TOKEN is required")
        self.headers = {
            "authorization": "Bearer " + token,
            "content-type": "application/json",
        }

    def request(self, method: str, path: str, body=None, allow_404=False):
        payload = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(
            API_ROOT + path, data=payload, headers=self.headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                status = response.status
                raw = response.read()
                result = json.loads(raw) if raw else {"success": True}
        except urllib.error.HTTPError as error:
            status = error.code
            try:
                result = json.load(error)
            except Exception:
                result = {}
        if allow_404 and status == 404:
            return status, result
        if status < 200 or status >= 300 or not result.get("success"):
            messages = "; ".join(
                str(item.get("message") or item.get("code") or "API error")
                for item in (result.get("errors") or []))
            raise RuntimeError(
                "Cloudflare API %s %s failed (%s): %s"
                % (method, path, status, messages or "unknown error"))
        return status, result


def zone_id(client: Cloudflare, configured: str) -> str:
    if configured:
        return configured
    query = urllib.parse.urlencode({"name": "forkmesh.com", "status": "active"})
    _status, payload = client.request("GET", "/zones?" + query)
    zones = payload.get("result") or []
    if len(zones) != 1 or not zones[0].get("id"):
        raise RuntimeError("could not resolve the active forkmesh.com zone")
    return str(zones[0]["id"])


def rule_payload(expression: str) -> dict:
    return {
        "action": "block",
        "description": MANAGED_DESCRIPTION,
        "enabled": True,
        "expression": expression,
        "ref": MANAGED_REF,
        "position": {"index": 1},
    }


def sync(rows: list[dict], environment: dict[str, str]) -> str:
    client = Cloudflare(environment.get("CLOUDFLARE_API_TOKEN", ""))
    zone = zone_id(client, environment.get("CLOUDFLARE_ZONE_ID", ""))
    entry_path = (
        "/zones/%s/rulesets/phases/http_request_firewall_custom/entrypoint"
        % zone)
    status, payload = client.request("GET", entry_path, allow_404=True)
    expression = build_expression(rows)
    entry = payload.get("result") or {}
    entry_rules = entry.get("rules") or []
    existing = next(
        (rule for rule in entry_rules
         if rule.get("ref") == MANAGED_REF
         or rule.get("description") == MANAGED_DESCRIPTION),
        None,
    )

    if not expression:
        if existing is None:
            return "request policy already absent from zone WAF"
        ruleset_id = str(entry.get("id") or "")
        client.request(
            "DELETE",
            "/zones/%s/rulesets/%s/rules/%s"
            % (zone, ruleset_id, existing["id"]),
        )
        return "removed empty request policy from zone WAF"

    desired = rule_payload(expression)
    if status == 404:
        client.request("POST", "/zones/%s/rulesets" % zone, {
            "name": "zone",
            "description": "Zone-level phase entry point",
            "kind": "zone",
            "phase": "http_request_firewall_custom",
            "rules": [desired],
        })
        return "created zone WAF request policy"

    ruleset_id = str(entry.get("id") or "")
    if not ruleset_id:
        raise RuntimeError("Cloudflare returned an entrypoint without an id")
    if existing is None:
        client.request(
            "POST", "/zones/%s/rulesets/%s/rules" % (zone, ruleset_id), desired)
        return "created zone WAF request policy"
    # Cloudflare rejects a no-op position update, so only request a move when
    # another rule has actually been placed ahead of the managed block.
    if entry_rules and entry_rules[0].get("id") == existing.get("id"):
        desired.pop("position", None)
    client.request(
        "PATCH",
        "/zones/%s/rulesets/%s/rules/%s"
        % (zone, ruleset_id, existing["id"]),
        desired,
    )
    return "updated zone WAF request policy"


def default_env_file() -> Path:
    legacy = Path(__file__).resolve().parents[1] / ".env.production"
    if legacy.exists():
        return legacy
    tools = str(REPOSITORY / "tools")
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import next_common
    return next_common.config_dir() / "cloudflare.env"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--env-file", type=Path)
    parser.add_argument("--rqlite", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    rows = rqlite_rows() if args.rqlite else wrangler_rows(sys.stdin.read())
    if args.dry_run:
        print(build_expression(rows))
        print("would publish %d enabled database rule(s)" % len(rows))
        return 0
    print(sync(rows, load_env(args.env_file or default_env_file())))
    print("published %d enabled database rule(s)" % len(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
