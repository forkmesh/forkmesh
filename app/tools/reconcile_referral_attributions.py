#!/usr/bin/env python3
"""Recover provable legacy referrals and retire unsupported counter credit.

The old referral implementation retained only aggregate signup counts. A small
newer overlap window also retained encrypted signup IP/User-Agent evidence and
blind-indexed referral clicks. This tool joins only those cryptographic records,
re-runs current chronology/network checks, preserves the unresolved aggregate
as an informational legacy remainder, and makes credited attribution rows the
authoritative signup total.

Raw signup evidence and DATA_KEY are never printed or sent back in plaintext.
Run without --apply for the required dry-run summary.
"""

import argparse
import hashlib
import hmac
import json
import os
import re
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path


APP_ROOT = Path(__file__).resolve().parents[1]
WORKSPACE_ROOT = APP_ROOT.parent
if str(WORKSPACE_ROOT) not in sys.path:
    sys.path.insert(0, str(WORKSPACE_ROOT))

from tools.legacy_solana_custody import (  # noqa: E402
    decrypt_d1_record,
    encrypt_d1_record,
)


DAY_MS = 24 * 60 * 60 * 1000
NETWORK_WINDOW_MS = 30 * DAY_MS
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def _load_env(path):
    env = dict(os.environ)
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        env.setdefault(key.strip(), value.strip().strip('"').strip("'"))
    return env


def _wrangler(database, env, sql):
    process = subprocess.run(
        ["npx", "wrangler", "d1", "execute", database, "--remote",
         "--json", "--command", sql],
        cwd=APP_ROOT,
        env=env,
        check=False,
        capture_output=True,
        text=True,
    )
    if process.returncode:
        raise RuntimeError(process.stderr.strip() or "wrangler failed")
    payload = json.loads(process.stdout)
    return [row for result in payload for row in result.get("results", [])]


def _sql(value):
    return "'" + str(value or "").replace("'", "''") + "'"


def _blind_key(data_key):
    return hashlib.sha256(
        (data_key.strip() + ":blind-index").encode("utf-8")).digest()


def _blind(key, value):
    return hmac.new(
        key, str(value or "").encode("utf-8"), hashlib.sha256).hexdigest()


def _chronology_flags(referred, referrer):
    try:
        referred_at = int((referred or {}).get("created_at") or 0)
        referrer_at = int((referrer or {}).get("created_at") or 0)
    except (TypeError, ValueError):
        referred_at = referrer_at = 0
    if not referred_at or not referrer_at:
        return ["join_chronology_unavailable"]
    if referred_at <= referrer_at:
        return ["referrer_joined_after_referred_user"]
    return []


def _dedupe(values):
    return list(dict.fromkeys(str(value) for value in values if value))


def reconcile(database, env, apply=False):
    data_key = env.get("DATA_KEY", "").strip()
    if not data_key:
        raise RuntimeError("DATA_KEY is required")
    users = _wrangler(
        database, env,
        "SELECT username,user_bi,ip_bi,data FROM users ORDER BY username")
    clicks = _wrangler(
        database, env,
        "SELECT visit_bi,referrer_bi,seen_at FROM referral_click_seen "
        "ORDER BY seen_at")
    attributions = _wrangler(
        database, env, "SELECT * FROM referral_attributions ORDER BY created_at")
    stats = _wrangler(
        database, env, "SELECT * FROM referral_stats ORDER BY name")
    legacy_exists = bool(_wrangler(
        database, env,
        "SELECT 1 AS found FROM sqlite_master WHERE type='table' "
        "AND name='referral_legacy_totals'"))
    legacy_rows = _wrangler(
        database, env, "SELECT * FROM referral_legacy_totals") \
        if legacy_exists else []

    records_by_bi = {}
    rows_by_bi = {}
    for row in users:
        record = decrypt_d1_record(row.get("data", ""), data_key)
        account_bi = str(row.get("user_bi") or "")
        records_by_bi[account_bi] = record
        rows_by_bi[account_bi] = row

    referrer_names = {
        str(row.get("referrer_bi") or ""): str(row.get("name") or "").lower()
        for row in stats
    }
    click_referrers = set(referrer_names).intersection(
        str(row.get("referrer_bi") or "") for row in clicks)
    key = _blind_key(data_key)
    existing_by_referred = {
        str(row.get("referred_bi") or ""): row for row in attributions}

    matches = []
    for account_bi, record in records_by_bi.items():
        if account_bi in existing_by_referred:
            continue
        signup = record.get("signup") \
            if isinstance(record.get("signup"), dict) else {}
        raw_ip = str(signup.get("ip") or "")
        user_agent = str(
            signup.get("userAgent") or signup.get("ua") or "")
        signup_at = int(signup.get("at") or 0)
        if not raw_ip or not user_agent or not signup_at:
            continue
        ip_bi = _blind(key, raw_ip)
        user_agent_bi = _blind(
            key, "referral-user-agent:" + user_agent.lower())
        found = []
        for click in clicks:
            referrer_bi = str(click.get("referrer_bi") or "")
            seen_at = int(click.get("seen_at") or 0)
            if (referrer_bi not in click_referrers or seen_at > signup_at
                    or signup_at - seen_at > DAY_MS):
                continue
            bucket = seen_at // DAY_MS
            visit_bi = _blind(
                key, "referral-click:%s:%s:%s:%s" % (
                    referrer_bi, ip_bi, user_agent_bi, bucket))
            if hmac.compare_digest(
                    visit_bi, str(click.get("visit_bi") or "")):
                found.append(referrer_bi)
        found = _dedupe(found)
        if len(found) == 1 and found[0] != account_bi:
            matches.append({
                "account_bi": account_bi,
                "name": str(record.get("name") or "").lower(),
                "record": record,
                "signup": signup,
                "signup_at": signup_at,
                "signup_ip_bi": ip_bi,
                "user_agent_bi": user_agent_bi,
                "referrer_bi": found[0],
            })

    invalid_existing = []
    valid_existing = []
    for row in attributions:
        if row.get("decision") not in ("pending", "credited"):
            continue
        flags = _chronology_flags(
            records_by_bi.get(str(row.get("referred_bi") or "")),
            records_by_bi.get(str(row.get("referrer_bi") or "")),
        )
        if flags:
            invalid_existing.append((row, flags))
        elif row.get("decision") == "credited":
            valid_existing.append(row)

    agent_counts = Counter(match["user_agent_bi"] for match in matches)
    agent_counts.update(
        str(row.get("user_agent_bi") or "") for row in attributions
        if row.get("user_agent_bi"))
    claimed_windows = {
        str(row.get("network_window_bi") or "") for row in valid_existing
        if row.get("network_window_bi")
    }
    planned = []
    for match in sorted(matches, key=lambda item: item["signup_at"]):
        referrer_bi = match["referrer_bi"]
        referrer = records_by_bi.get(referrer_bi)
        flags = _chronology_flags(match["record"], referrer)
        referrer_ip_bi = str(rows_by_bi.get(referrer_bi, {}).get("ip_bi") or "")
        if referrer_ip_bi and referrer_ip_bi == match["signup_ip_bi"]:
            flags.append("same_network_as_referrer")
        if match["signup"].get("clientCategory") == "automated-client":
            flags.append("automated_client")
        if agent_counts[match["user_agent_bi"]] > 1:
            flags.append("shared_user_agent")
        network_window_bi = _blind(
            key, "referral-network-window:%s:%s:%s" % (
                referrer_bi, match["signup_ip_bi"],
                match["signup_at"] // NETWORK_WINDOW_MS))
        hard_reject = any(flag in flags for flag in (
            "join_chronology_unavailable",
            "referrer_joined_after_referred_user",
            "same_network_as_referrer",
            "automated_client",
        ))
        if not hard_reject and network_window_bi in claimed_windows:
            flags.append("repeat_referral_network")
            hard_reject = True
        verified = match["record"].get("email_verified") is True
        decision = "rejected" if hard_reject else (
            "credited" if verified else "pending")
        if decision == "credited":
            claimed_windows.add(network_window_bi)
        match.update({
            "decision": decision,
            "flags": _dedupe(["legacy_click_evidence", *flags]),
            "network_window_bi": network_window_bi,
        })
        planned.append(match)

    existing_credit_counts = Counter(
        str(row.get("referrer_bi") or "") for row in attributions
        if row.get("decision") == "credited")
    recovered_counts = Counter(item["referrer_bi"] for item in planned)
    prior_legacy = {
        str(row.get("referrer_bi") or ""):
        int(row.get("unattributed_signups") or 0)
        for row in legacy_rows
    }
    legacy = {}
    for row in stats:
        referrer_bi = str(row.get("referrer_bi") or "")
        if referrer_bi in prior_legacy:
            remaining = prior_legacy[referrer_bi]
        else:
            counter_only = max(
                0, int(row.get("signups") or 0)
                - existing_credit_counts[referrer_bi])
            remaining = max(0, counter_only - recovered_counts[referrer_bi])
        legacy[referrer_bi] = {
            "name": str(row.get("name") or ""),
            "remaining": remaining,
        }

    print("Existing chronology rejections:", len(invalid_existing))
    for row, flags in invalid_existing:
        print("  reject", row.get("referred_name"), "from",
              row.get("referrer_name"), "(" + ",".join(flags) + ")")
    print("Recoverable legacy identities:", len(planned))
    for item in planned:
        print(" ", item["decision"], item["name"], "from",
              referrer_names.get(item["referrer_bi"], "unknown"),
              "(" + ",".join(item["flags"]) + ")")
    print("Unattributed legacy remainder:", sum(
        item["remaining"] for item in legacy.values()))
    for item in legacy.values():
        if item["remaining"]:
            print(" ", item["name"], item["remaining"])
    if not apply:
        print("Dry run only; pass --apply after reviewing this summary.")
        return

    now = int(time.time() * 1000)
    statements = [
        "CREATE TABLE IF NOT EXISTS referral_legacy_totals ("
        "referrer_bi TEXT PRIMARY KEY,name TEXT NOT NULL,"
        "unattributed_signups INTEGER NOT NULL DEFAULT 0,"
        "migrated_at INTEGER NOT NULL DEFAULT 0)",
    ]
    for referrer_bi, item in legacy.items():
        statements.append(
            "INSERT INTO referral_legacy_totals "
            "(referrer_bi,name,unattributed_signups,migrated_at) VALUES ("
            + ",".join((
                _sql(referrer_bi), _sql(item["name"]),
                str(item["remaining"]), str(now)))
            + ") ON CONFLICT(referrer_bi) DO UPDATE SET "
            "name=excluded.name,unattributed_signups=excluded.unattributed_signups,"
            "migrated_at=excluded.migrated_at")
    for row, added_flags in invalid_existing:
        flags = _dedupe([
            *json.loads(str(row.get("risk_flags") or "[]")), *added_flags])
        statements.append(
            "UPDATE referral_attributions SET decision='rejected',credited_at=0,"
            "risk_flags=%s WHERE referred_bi=%s AND decision IN ('pending','credited')"
            % (_sql(json.dumps(flags, separators=(",", ":"))),
               _sql(row.get("referred_bi"))))
    for item in planned:
        protected = encrypt_d1_record({
            "ip": str(item["signup"].get("ip") or ""),
            "userAgent": str(
                item["signup"].get("userAgent")
                or item["signup"].get("ua") or ""),
            "country": str(item["signup"].get("country") or "")[:8],
            "clientCategory": str(
                item["signup"].get("clientCategory") or "")[:40],
            "migrationEvidence": "referral_click_seen",
        }, data_key)
        verified_at = item["signup_at"] \
            if item["record"].get("email_verified") is True else 0
        credited_at = item["signup_at"] \
            if item["decision"] == "credited" else 0
        statements.append(
            "INSERT INTO referral_attributions ("
            "referred_bi,referred_name,referrer_bi,referrer_name,signup_ip_bi,"
            "user_agent_bi,network_window_bi,decision,risk_flags,data,created_at,"
            "verified_at,credited_at) VALUES ("
            + ",".join((
                _sql(item["account_bi"]), _sql(item["name"]),
                _sql(item["referrer_bi"]),
                _sql(referrer_names[item["referrer_bi"]]),
                _sql(item["signup_ip_bi"]), _sql(item["user_agent_bi"]),
                _sql(item["network_window_bi"]), _sql(item["decision"]),
                _sql(json.dumps(item["flags"], separators=(",", ":"))),
                _sql(protected), str(item["signup_at"]), str(verified_at),
                str(credited_at)))
            + ") ON CONFLICT(referred_bi) DO NOTHING")
    statements.append(
        "UPDATE referral_stats SET signups=(SELECT COUNT(*) FROM "
        "referral_attributions r WHERE r.referrer_bi=referral_stats.referrer_bi "
        "AND r.decision='credited')")
    details = json.dumps({
        "existingRejected": len(invalid_existing),
        "identitiesRecovered": len(planned),
        "legacyUnattributed": sum(
            item["remaining"] for item in legacy.values()),
    }, separators=(",", ":"))
    statements.append(
        "INSERT INTO sensitive_audit_log "
        "(ts,actor_bi,actor_label,action,target_type,target_token,outcome,details) "
        "VALUES (%d,NULL,'reconcile_referral_attributions',"
        "'referral.legacy_reconcile','referral','all','success',%s)"
        % (now, _sql(details)))
    _wrangler(database, env, ";".join(statements))
    print("Applied referral reconciliation successfully.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", default="forkmesh")
    parser.add_argument(
        "--env-file", type=Path, default=APP_ROOT / ".env.production")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    reconcile(args.database, _load_env(args.env_file), apply=args.apply)


if __name__ == "__main__":
    main()
