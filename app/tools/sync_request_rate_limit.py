#!/usr/bin/env python3
"""Keep ForkMesh's zone-wide edge rate limit present and up to date."""

from __future__ import annotations

import argparse
from pathlib import Path

from sync_request_block_rules import Cloudflare, load_env, zone_id


MANAGED_REF = "forkmesh_site_request_rate_limit"
MANAGED_DESCRIPTION = "ForkMesh site request rate limit"
PHASE = "http_ratelimit"
EXPRESSION = (
    '(not cf.client.bot and not starts_with(http.request.uri.path, "/cdn-cgi/"))'
)
SUPPORTED_PERIODS = {
    10, 15, 20, 30, 40, 45, 60, 90, 120, 180, 240, 300, 480, 600,
    900, 1200, 1800, 2400, 3600, 65535, 86400,
}
SUPPORTED_TIMEOUTS = {0, 10, 60, 120, 300, 600, 3600, 86400}


def _integer(environment: dict[str, str], name: str, default: int) -> int:
    raw = str(environment.get(name, "") or default).strip()
    try:
        value = int(raw)
    except ValueError as error:
        raise ValueError("%s must be an integer" % name) from error
    if value < 1:
        raise ValueError("%s must be positive" % name)
    return value


def enabled(environment: dict[str, str]) -> bool:
    return str(environment.get(
        "FORKMESH_EDGE_RATE_LIMIT_ENABLED", "1"
    )).strip().lower() not in {"0", "false", "no", "off"}


def rule_payload(environment: dict[str, str]) -> dict:
    period = _integer(
        environment, "FORKMESH_EDGE_RATE_LIMIT_PERIOD_SECONDS", 10)
    timeout = _integer(
        environment, "FORKMESH_EDGE_RATE_LIMIT_TIMEOUT_SECONDS", 10)
    requests = _integer(
        environment, "FORKMESH_EDGE_RATE_LIMIT_REQUESTS", 120)
    if period not in SUPPORTED_PERIODS:
        raise ValueError("unsupported edge rate-limit period")
    if timeout not in SUPPORTED_TIMEOUTS:
        raise ValueError("unsupported edge rate-limit timeout")
    return {
        "action": "block",
        "description": MANAGED_DESCRIPTION,
        "enabled": True,
        "expression": EXPRESSION,
        "ref": MANAGED_REF,
        "ratelimit": {
            "characteristics": ["cf.colo.id", "ip.src"],
            "period": period,
            "requests_per_period": requests,
            "mitigation_timeout": timeout,
        },
    }


def sync(environment: dict[str, str]) -> str:
    client = Cloudflare(environment.get("CLOUDFLARE_API_TOKEN", ""))
    zone = zone_id(client, environment.get("CLOUDFLARE_ZONE_ID", ""))
    entry_path = "/zones/%s/rulesets/phases/%s/entrypoint" % (zone, PHASE)
    status, payload = client.request("GET", entry_path, allow_404=True)
    entry = payload.get("result") or {}
    rules = entry.get("rules") or []
    existing = next(
        (rule for rule in rules
         if rule.get("ref") == MANAGED_REF
         or rule.get("description") == MANAGED_DESCRIPTION),
        None,
    )

    if not enabled(environment):
        if existing is None:
            return "edge rate limit already absent"
        client.request(
            "DELETE",
            "/zones/%s/rulesets/%s/rules/%s"
            % (zone, entry["id"], existing["id"]),
        )
        return "removed edge rate limit"

    desired = rule_payload(environment)
    if status == 404:
        client.request("POST", "/zones/%s/rulesets" % zone, {
            "name": "zone",
            "description": "Zone-level phase entry point",
            "kind": "zone",
            "phase": PHASE,
            "rules": [desired],
        })
        return "created edge rate limit"

    ruleset_id = str(entry.get("id") or "")
    if not ruleset_id:
        raise RuntimeError("Cloudflare returned an entrypoint without an id")
    if existing is None:
        client.request(
            "POST", "/zones/%s/rulesets/%s/rules" % (zone, ruleset_id),
            desired,
        )
        return "created edge rate limit"
    client.request(
        "PATCH",
        "/zones/%s/rulesets/%s/rules/%s"
        % (zone, ruleset_id, existing["id"]),
        desired,
    )
    return "updated edge rate limit"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--env-file", type=Path,
        default=Path(__file__).resolve().parents[1] / ".env.production")
    args = parser.parse_args()
    print(sync(load_env(args.env_file)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
