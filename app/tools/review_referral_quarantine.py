#!/usr/bin/env python3
"""Review and safely mutate the referral quarantine in remote D1.

Examples:
  python tools/review_referral_quarantine.py DB --referrer alice
  python tools/review_referral_quarantine.py DB --referrer alice \
      --action quarantine --apply
  python tools/review_referral_quarantine.py DB --username bob \
      --action restore --reviewer operator@example.com --apply

All mutations are dry-run unless --apply is present. Raw encrypted account and
signup evidence is never printed.
"""

import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path


APP_ROOT = Path(__file__).resolve().parents[1]
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")


def _load_env(path):
    env = dict(os.environ)
    if not path.exists():
        return env
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        env.setdefault(key.strip(), value.strip().strip('"').strip("'"))
    return env


def _sql(value):
    return "'" + str(value or "").replace("'", "''") + "'"


def _wrangler(database, env, sql):
    process = subprocess.run(
        ["npx", "wrangler", "d1", "execute", database, "--remote",
         "--json", "--command", sql],
        cwd=APP_ROOT, env=env, check=False, capture_output=True, text=True,
    )
    if process.returncode:
        raise RuntimeError(process.stderr.strip() or "wrangler failed")
    payload = json.loads(process.stdout or "[]")
    return [row for result in payload for row in result.get("results", [])]


def _referrer(database, env, name):
    rows = _wrangler(
        database, env,
        "SELECT referrer_bi,referrer_name,COUNT(*) AS accounts "
        "FROM referral_attributions WHERE referrer_name=%s "
        "GROUP BY referrer_bi,referrer_name" % _sql(name),
    )
    if len(rows) != 1:
        raise RuntimeError("referrer was not found or is ambiguous")
    return rows[0]


def _inspect_referrer(database, env, name):
    row = _referrer(database, env, name)
    referrer_bi = _sql(row["referrer_bi"])
    control = _wrangler(
        database, env,
        "SELECT state,reason_codes,updated_at,review_after,reviewed_at,"
        "reviewed_by FROM referral_referrer_controls WHERE referrer_bi=%s"
        % referrer_bi,
    )
    decisions = _wrangler(
        database, env,
        "SELECT decision,COUNT(*) AS accounts,MAX(risk_score) AS max_score "
        "FROM referral_attributions WHERE referrer_bi=%s GROUP BY decision"
        % referrer_bi,
    )
    quarantine = _wrangler(
        database, env,
        "SELECT status,COUNT(*) AS accounts FROM quarantined_users "
        "WHERE referrer_bi=%s GROUP BY status" % referrer_bi,
    )
    return {"referrer": name, "attributions": decisions,
            "control": control, "quarantine": quarantine}


def _inspect_user(database, env, username):
    return _wrangler(
        database, env,
        "SELECT q.username,q.status,q.reason_codes,q.quarantined_at,"
        "q.review_after,q.reviewed_at,q.reviewed_by,a.referrer_name,"
        "a.decision,a.risk_score,a.risk_flags "
        "FROM quarantined_users q LEFT JOIN referral_attributions a "
        "ON a.referred_bi=q.user_bi WHERE q.username=%s" % _sql(username),
    )


def _quarantine(database, env, name, reviewer):
    row = _referrer(database, env, name)
    referrer_bi = _sql(row["referrer_bi"])
    now = int(time.time() * 1000)
    review_after = now + 72 * 60 * 60 * 1000
    reasons = _sql(json.dumps(
        ["manual_referral_review", "referrer_rewards_frozen"],
        separators=(",", ":")))
    reviewer_sql = _sql(reviewer)
    statements = [
        "INSERT INTO referral_referrer_controls "
        "(referrer_bi,state,reason_codes,updated_at,review_after,reviewed_by) "
        "VALUES (%s,'frozen',%s,%d,%d,%s) "
        "ON CONFLICT(referrer_bi) DO UPDATE SET state='frozen',"
        "reason_codes=excluded.reason_codes,updated_at=excluded.updated_at,"
        "review_after=excluded.review_after,reviewed_at=0,"
        "reviewed_by=excluded.reviewed_by" % (
            referrer_bi, reasons, now, review_after, reviewer_sql),
        "UPDATE referral_attributions SET decision='pending',credited_at=0,"
        "risk_score=MAX(risk_score,100),risk_flags=CASE "
        "WHEN json_valid(risk_flags) THEN json_insert(risk_flags,'$[#]',"
        "'manual_referral_review','$[#]','referrer_rewards_frozen') "
        "ELSE %s END WHERE referrer_bi=%s "
        "AND decision IN ('pending','credited')" % (reasons, referrer_bi),
        "INSERT INTO quarantined_users "
        "(user_bi,data,email_bi,username,is_admin,ip_bi,enable_outreach,"
        "discord_bi,referrer_bi,reason_codes,status,quarantined_at,"
        "review_after) "
        "SELECT u.user_bi,u.data,u.email_bi,u.username,u.is_admin,u.ip_bi,"
        "u.enable_outreach,u.discord_bi,%s,%s,'pending_verification',%d,%d "
        "FROM users u "
        "JOIN referral_attributions a ON a.referred_bi=u.user_bi "
        "WHERE a.referrer_bi=%s ON CONFLICT(user_bi) DO UPDATE SET "
        "data=excluded.data,email_bi=excluded.email_bi,"
        "username=excluded.username,is_admin=excluded.is_admin,"
        "ip_bi=excluded.ip_bi,"
        "enable_outreach=excluded.enable_outreach,"
        "discord_bi=excluded.discord_bi,"
        "referrer_bi=excluded.referrer_bi,"
        "reason_codes=excluded.reason_codes,status='pending_verification',"
        "quarantined_at=excluded.quarantined_at,"
        "review_after=excluded.review_after,reviewed_at=0,reviewed_by=''" % (
            referrer_bi, reasons, now, review_after, referrer_bi),
        "INSERT INTO quarantined_nodes "
        "(node_bi,user_bi,pubkey,data,name,last_seen,quarantined_user_bi,"
        "status,quarantined_at) SELECT n.node_bi,n.user_bi,n.pubkey,n.data,"
        "n.name,n.last_seen,CASE WHEN n.user_bi IN (SELECT user_bi FROM "
        "quarantined_users WHERE referrer_bi=%s AND "
        "status='pending_verification') THEN n.user_bi ELSE n.node_bi END,"
        "'pending_verification',%d FROM nodes n WHERE n.user_bi IN "
        "(SELECT user_bi FROM quarantined_users WHERE referrer_bi=%s AND "
        "status='pending_verification') OR n.node_bi IN (SELECT user_bi FROM "
        "quarantined_users WHERE referrer_bi=%s AND "
        "status='pending_verification') ON CONFLICT(node_bi) DO UPDATE SET "
        "status='pending_verification',quarantined_at=excluded.quarantined_at,"
        "reviewed_at=0,reviewed_by=''" % (
            referrer_bi, now, referrer_bi, referrer_bi),
        "DELETE FROM nodes WHERE user_bi IN (SELECT user_bi FROM "
        "quarantined_users WHERE referrer_bi=%s AND "
        "status='pending_verification') OR node_bi IN (SELECT user_bi FROM "
        "quarantined_users WHERE referrer_bi=%s AND "
        "status='pending_verification')" % (referrer_bi, referrer_bi),
        "UPDATE account_devices SET enabled=0,revoked_at=%d WHERE account_bi "
        "IN (SELECT user_bi FROM quarantined_users WHERE referrer_bi=%s "
        "AND status='pending_verification')" % (now, referrer_bi),
        "UPDATE account_sessions SET revoked_at=%d WHERE account_bi IN "
        "(SELECT user_bi FROM quarantined_users WHERE referrer_bi=%s "
        "AND status='pending_verification')" % (now, referrer_bi),
        "DELETE FROM account_presence WHERE name_bi IN "
        "(SELECT user_bi FROM quarantined_users WHERE referrer_bi=%s "
        "AND status='pending_verification')" % referrer_bi,
        "DELETE FROM users WHERE user_bi IN (SELECT user_bi FROM "
        "quarantined_users WHERE referrer_bi=%s "
        "AND status='pending_verification')" % referrer_bi,
        "UPDATE referral_stats SET signups=(SELECT COUNT(*) FROM "
        "referral_attributions WHERE referrer_bi=%s AND decision='credited') "
        "WHERE referrer_bi=%s" % (referrer_bi, referrer_bi),
        "DELETE FROM badge_awards WHERE badge_slug='first_referral' "
        "AND granted_by='system' AND account_bi=%s" % referrer_bi,
    ]
    _wrangler(database, env, ";".join(statements))


def _review_user(database, env, username, action, reviewer):
    rows = _inspect_user(database, env, username)
    if len(rows) != 1:
        raise RuntimeError("quarantined username was not found or is ambiguous")
    now = int(time.time() * 1000)
    username_sql = _sql(username)
    reviewer_sql = _sql(reviewer)
    if action == "restore":
        statements = [
            "INSERT INTO users "
            "(user_bi,data,email_bi,username,is_admin,ip_bi,enable_outreach,"
            "discord_bi) "
            "SELECT user_bi,data,email_bi,username,is_admin,ip_bi,"
            "enable_outreach,discord_bi FROM quarantined_users WHERE username=%s "
            "AND status='pending_verification'" % username_sql,
            "INSERT INTO nodes (node_bi,user_bi,pubkey,data,name,last_seen) "
            "SELECT node_bi,user_bi,pubkey,data,name,last_seen FROM "
            "quarantined_nodes WHERE quarantined_user_bi=(SELECT user_bi "
            "FROM quarantined_users WHERE username=%s) AND "
            "status='pending_verification' ON CONFLICT(node_bi) DO UPDATE SET "
            "user_bi=excluded.user_bi,pubkey=excluded.pubkey,data=excluded.data,"
            "name=excluded.name,last_seen=excluded.last_seen" % username_sql,
            "UPDATE quarantined_users SET status='restored',reviewed_at=%d,"
            "reviewed_by=%s WHERE username=%s AND status='pending_verification'"
            % (now, reviewer_sql, username_sql),
            "UPDATE quarantined_nodes SET status='restored',reviewed_at=%d,"
            "reviewed_by=%s WHERE quarantined_user_bi=(SELECT user_bi FROM "
            "quarantined_users WHERE username=%s) AND "
            "status='pending_verification'" % (
                now, reviewer_sql, username_sql),
        ]
    else:
        statements = [
            "UPDATE referral_attributions SET decision='rejected',"
            "credited_at=0 WHERE referred_bi=(SELECT user_bi FROM "
            "quarantined_users WHERE username=%s)" % username_sql,
            "UPDATE quarantined_users SET status='rejected',reviewed_at=%d,"
            "reviewed_by=%s WHERE username=%s AND status='pending_verification'"
            % (now, reviewer_sql, username_sql),
            "UPDATE quarantined_nodes SET status='rejected',reviewed_at=%d,"
            "reviewed_by=%s WHERE quarantined_user_bi=(SELECT user_bi FROM "
            "quarantined_users WHERE username=%s) AND "
            "status='pending_verification'" % (
                now, reviewer_sql, username_sql),
        ]
    _wrangler(database, env, ";".join(statements))


def _unfreeze(database, env, name, reviewer):
    row = _referrer(database, env, name)
    now = int(time.time() * 1000)
    _wrangler(
        database, env,
        "UPDATE referral_referrer_controls SET state='active',updated_at=%d,"
        "review_after=0,reviewed_at=%d,reviewed_by=%s WHERE referrer_bi=%s"
        % (now, now, _sql(reviewer), _sql(row["referrer_bi"])),
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", help="Wrangler D1 database binding/name")
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--referrer", type=str.lower)
    target.add_argument("--username", type=str.lower)
    parser.add_argument(
        "--action", choices=("inspect", "quarantine", "restore", "reject",
                              "unfreeze"), default="inspect")
    parser.add_argument("--reviewer", default="operator")
    parser.add_argument("--env-file", type=Path, default=APP_ROOT / ".dev.vars")
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    name = args.referrer or args.username
    if not NAME_RE.fullmatch(name or ""):
        parser.error("target must be a valid lowercase ForkMesh username")
    if args.action in ("quarantine", "unfreeze") and not args.referrer:
        parser.error("this action requires --referrer")
    if args.action in ("restore", "reject") and not args.username:
        parser.error("this action requires --username")

    env = _load_env(args.env_file)
    before = (_inspect_referrer(args.database, env, args.referrer)
              if args.referrer else _inspect_user(
                  args.database, env, args.username))
    if args.action == "inspect" or not args.apply:
        print(json.dumps({"apply": False, "action": args.action,
                          "result": before}, indent=2, sort_keys=True))
        return
    if args.action == "quarantine":
        _quarantine(args.database, env, args.referrer, args.reviewer)
    elif args.action in ("restore", "reject"):
        _review_user(
            args.database, env, args.username, args.action, args.reviewer)
    else:
        _unfreeze(args.database, env, args.referrer, args.reviewer)
    after = (_inspect_referrer(args.database, env, args.referrer)
             if args.referrer else _inspect_user(
                 args.database, env, args.username))
    print(json.dumps({"apply": True, "action": args.action,
                      "before": before, "after": after},
                     indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
