#!/usr/bin/env python3
"""Link Discord reporters to their ForkMesh accounts in D1.

Reporter credit on an issue is its unsigned ``authorName``. Which ForkMesh
account a Discord reporter owns used to live nowhere, so every triage pass
re-derived it from chat history and one pass guessed wrong. It is now a column
on ``users`` (``discord_bi``, migration 0148) plus the readable handle inside
the encrypted record, so the answer is queryable instead of remembered.

This reads #bugs, works out who is who, and writes the links into D1. Like
sync_contributor_rewards.py it is a dry run unless ``--apply`` is supplied.

Confidence, strongest first:

``self-declared``    the reporter stated their username in the channel
``connector-linked`` the connector announced a link it had made
``connector-guess``  the connector said it was guessing

A Discord id claimed by two different accounts is never silently resolved: the
higher-confidence claim wins and the conflict is reported, so a human can
settle it. Only ``self-declared`` and ``connector-linked`` are written by
default -- a guess has to be confirmed with ``--include-guesses``.

Usage:
    DISCORD_BOT_TOKEN=... python3 app/tools/sync_discord_accounts.py
    DISCORD_BOT_TOKEN=... python3 app/tools/sync_discord_accounts.py --apply
    python3 app/tools/sync_discord_accounts.py --from-file messages.json
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

APP_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = APP_ROOT.parent
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from tools.legacy_solana_custody import (  # noqa: E402
    decrypt_d1_record,
    encrypt_d1_record,
)

SCHEMA = "forkmesh-discord-accounts-v1"
# ForkMesh Next starts with Discord off: the guild and bug channel come from
# the environment (the production ids are never baked in here).
GUILD_ID = os.environ.get("FORKMESH_DISCORD_GUILD_ID", "")
BUGS_CHANNEL_ID = os.environ.get("FORKMESH_DISCORD_BUGS_CHANNEL_ID", "")
DISCORD_API = "https://discord.com/api/v10"

# Strongest first; index doubles as the precedence rank.
CONFIDENCE_ORDER = ("self-declared", "connector-linked", "connector-guess")

# A ForkMesh username: lowercase letters, digits and hyphens, starting with a
# letter (the rule the settings page enforces).
_USER = r"[a-z][a-z0-9-]{1,38}"

# "My ForkMesh username is: spidertest" and near-miss phrasings.
SELF_DECLARED_RE = re.compile(
    r"my\s+forkmesh\s+(?:user\s*name|username|handle|account)\s*(?:on\s+the\s+site\s*)?"
    r"(?:is|:)\s*[:\-]?\s*`?(" + _USER + r")`?",
    re.I,
)
# "I linked Discord `@yagyashen` to ForkMesh `yagya-sen`"
PROSE_LINK_RE = re.compile(
    r"linked\s+Discord\s*`?@?([^`\s]+)`?\s+to\s+ForkMesh\s*`(" + _USER + r")`",
    re.I,
)
# "<@123> `md-mehboob`: ..." / "<@123> ... credited to `sanjay`"
MENTION_RE = re.compile(r"<@!?(\d+)>")
BACKTICK_USER_RE = re.compile(r"`(" + _USER + r")`")
GUESS_MARKERS = ("best available guess", "could not match", "couldn't match")


def confidence_rank(confidence: str) -> int:
    try:
        return CONFIDENCE_ORDER.index(confidence)
    except ValueError:
        return len(CONFIDENCE_ORDER)


def message_link(message_id: str) -> str:
    return (f"https://discord.com/channels/{GUILD_ID}"
            f"/{BUGS_CHANNEL_ID}/{message_id}")


def _bullet_for(line: str) -> str:
    return line.strip().lstrip("*-• ").strip()


def extract_claims(messages):
    """Every (discord_id, forkmesh_user, confidence, evidence) claim found.

    Pure: takes already-fetched message dicts so it is testable offline.
    """
    claims = []
    by_id = {}
    for message in messages:
        author = message.get("author") or {}
        if author.get("id"):
            by_id.setdefault(author["id"], author)

    # Reporters naming themselves. Strongest evidence there is.
    for message in messages:
        author = message.get("author") or {}
        if author.get("bot"):
            continue
        found = SELF_DECLARED_RE.search(message.get("content") or "")
        if found and author.get("id"):
            claims.append({
                "discordId": author["id"],
                "forkmeshUser": found.group(1).lower(),
                "confidence": "self-declared",
                "evidenceMessageId": message.get("id", ""),
                "evidence": "reporter stated their username in #bugs",
            })

    # Connector announcements. Parsed per line so one multi-user summary
    # message does not smear one person's username across everybody it mentions.
    for message in messages:
        author = message.get("author") or {}
        if not author.get("bot"):
            continue
        content = message.get("content") or ""
        for handle, user in PROSE_LINK_RE.findall(content):
            resolved = next(
                (a["id"] for a in by_id.values()
                 if a.get("username", "").lower() == handle.lower().lstrip("@")),
                "",
            )
            if resolved:
                claims.append({
                    "discordId": resolved,
                    "forkmeshUser": user.lower(),
                    "confidence": "connector-linked",
                    "evidenceMessageId": message.get("id", ""),
                    "evidence": f"connector: linked Discord @{handle} to {user}",
                })
        for line in content.splitlines():
            mentions = MENTION_RE.findall(line)
            if len(mentions) != 1:
                continue  # ambiguous or none: do not attribute
            users = BACKTICK_USER_RE.findall(line)
            if not users:
                continue
            guess = any(marker in line.lower() for marker in GUESS_MARKERS)
            claims.append({
                "discordId": mentions[0],
                "forkmeshUser": users[0].lower(),
                "confidence": "connector-guess" if guess else "connector-linked",
                "evidenceMessageId": message.get("id", ""),
                "evidence": "connector: " + _bullet_for(line)[:180],
            })
    return claims


def merge_claims(claims, messages):
    """Fold claims into one entry per Discord id, strongest confidence winning."""
    identities = {}
    for message in messages:
        author = message.get("author") or {}
        if author.get("id") and not author.get("bot"):
            identities[author["id"]] = author

    entries = {}
    for claim in claims:
        discord_id = claim["discordId"]
        current = entries.get(discord_id)
        if current is None:
            entries[discord_id] = dict(claim)
            continue
        if (confidence_rank(claim["confidence"])
                < confidence_rank(current["confidence"])):
            claim = dict(claim)
            claim["conflicts"] = current.get("conflicts", [])
            if current["forkmeshUser"] != claim["forkmeshUser"]:
                claim["conflicts"] = claim["conflicts"] + [{
                    "forkmeshUser": current["forkmeshUser"],
                    "confidence": current["confidence"],
                    "evidenceMessageId": current["evidenceMessageId"],
                }]
            entries[discord_id] = claim
        elif current["forkmeshUser"] != claim["forkmeshUser"]:
            current.setdefault("conflicts", []).append({
                "forkmeshUser": claim["forkmeshUser"],
                "confidence": claim["confidence"],
                "evidenceMessageId": claim["evidenceMessageId"],
            })

    out = []
    for discord_id, entry in entries.items():
        author = identities.get(discord_id, {})
        record = {
            "discordId": discord_id,
            "discordUsername": author.get("username", ""),
            "discordDisplayName": author.get("global_name") or "",
            "forkmeshUser": entry["forkmeshUser"],
            "confidence": entry["confidence"],
            "evidence": entry["evidence"],
            "evidenceMessageId": entry["evidenceMessageId"],
            "evidenceUrl": message_link(entry["evidenceMessageId"]),
        }
        if entry.get("conflicts"):
            record["conflicts"] = entry["conflicts"]
        out.append(record)
    return sorted(out, key=lambda r: (r["discordUsername"], r["discordId"]))


def reporters_without_account(messages, entries):
    """Discord authors who posted but have no mapped account."""
    mapped = {e["discordId"] for e in entries}
    unknown = {}
    for message in messages:
        author = message.get("author") or {}
        if author.get("bot") or not author.get("id"):
            continue
        if author["id"] in mapped:
            continue
        unknown[author["id"]] = {
            "discordId": author["id"],
            "discordUsername": author.get("username", ""),
            "discordDisplayName": author.get("global_name") or "",
        }
    return sorted(unknown.values(), key=lambda r: r["discordUsername"])


ACCOUNTS_URL = "https://app.forkmesh.com/api/accounts/users?limit=1000"


def fetch_accounts(url=ACCOUNTS_URL):
    """Live account directory, keyed by lowercase name."""
    request = urllib.request.Request(
        url, headers={"User-Agent": "ForkMesh-account-map/1.0"})
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = json.loads(response.read().decode())
    users = payload.get("users") if isinstance(payload, dict) else payload
    return {str(u.get("name", "")).lower(): u for u in (users or [])}


def verify_against_accounts(entries, accounts):
    """Flag mapped names that do not exist, and note who can be paid.

    A mapping that points at a nonexistent account is worse than no mapping:
    credit lands nowhere and nobody notices.
    """
    problems = []
    for entry in entries:
        account = accounts.get(entry["forkmeshUser"])
        if account is None:
            problems.append(
                f"{entry['discordUsername']} -> {entry['forkmeshUser']} "
                "(no such ForkMesh account)")
            entry["accountExists"] = False
            continue
        entry["accountExists"] = True
        entry["hasPayoutAddress"] = bool(account.get("solana"))
    return problems


def fetch_messages(token, limit_pages=10):
    messages, before = [], None
    for _ in range(limit_pages):
        url = f"{DISCORD_API}/channels/{BUGS_CHANNEL_ID}/messages?limit=100"
        if before:
            url += f"&before={before}"
        request = urllib.request.Request(
            url, headers={"Authorization": f"Bot {token}",
                          "User-Agent": "ForkMesh-account-map/1.0"})
        with urllib.request.urlopen(request, timeout=30) as response:
            page = json.loads(response.read().decode())
        if not page:
            break
        messages.extend(page)
        before = page[-1]["id"]
    return messages


def _load_env(path):
    environment = dict(os.environ)
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        environment.setdefault(key.strip(), value.strip().strip('"').strip("'"))
    return environment


def _sql_quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def blind_index_key(data_key):
    """The Worker's blind-index HMAC key.

    Mirrors entry.py _hmac_key: the key is SHA-256 of the DATA_KEY secret with
    a ":blind-index" context suffix, NOT the DATA_KEY itself -- the blind-index
    key is deliberately distinct from the AES key.
    """
    return hashlib.sha256((data_key + ":blind-index").encode()).digest()


def blind_index(data_key, value):
    """entry.py blind_index(): HMAC-SHA256 over the stripped, lowercased value."""
    return hmac.new(blind_index_key(data_key),
                    (value or "").strip().lower().encode(),
                    hashlib.sha256).hexdigest()


def render_sql(links, data_key, encrypted_records=None):
    """UPDATE statements linking each account to its Discord id.

    The column stores a blind index, never the raw id. Matching on ``username``
    means an account that does not exist updates zero rows instead of creating
    one: this tool links existing accounts, it never invents them. During a
    real apply, ``encrypted_records`` also persists the readable Discord names
    and raw id inside the account's existing AES-GCM record. The old ciphertext
    is included in the WHERE clause so a concurrent account save wins rather
    than being overwritten by this offline synchronization.
    """
    encrypted_records = encrypted_records or {}
    statements = [
        "-- Generated by app/tools/sync_discord_accounts.py; do not hand-edit.",
    ]
    for link in links:
        digest = blind_index(data_key, link["discordId"])
        account = encrypted_records.get(link["forkmeshUser"], {})
        data_assignment = (
            ",data=" + _sql_quote(account["updated"])
            if account.get("updated") else "")
        data_guard = (
            " AND data=" + _sql_quote(account["original"])
            if account.get("original") else "")
        statements.append(
            "UPDATE users SET discord_bi=" + _sql_quote(digest) +
            data_assignment +
            " WHERE username=" + _sql_quote(link["forkmeshUser"]) +
            " AND (discord_bi IS NULL OR discord_bi='' OR discord_bi=" +
            _sql_quote(digest) + ")" + data_guard + ";"
        )
    return "\n".join(statements) + "\n"


def _wrangler_rows(database, environment, sql):
    """Run one bounded remote D1 read without printing encrypted records."""
    process = subprocess.run(
        [
            "bash", "-c",
            '. ./pywrangler.sh; pywrangler d1 execute "$1" --remote '
            '--json --command "$2"',
            "sync-discord-accounts", database, sql,
        ],
        cwd=APP_ROOT, env=environment, check=False,
        capture_output=True, text=True,
    )
    if process.returncode:
        raise RuntimeError(process.stderr.strip() or "D1 read failed")
    output = process.stdout
    start = output.find("[")
    if start < 0:
        raise RuntimeError("D1 returned no JSON result")
    payload = json.loads(output[start:])
    return [
        row
        for result in (payload if isinstance(payload, list) else [])
        for row in (result.get("results") or [])
    ]


def load_encrypted_account_records(database, environment, links):
    """Read only the linked users' ciphertexts for an optimistic update."""
    names = sorted({
        str(link.get("forkmeshUser") or "").strip().lower()
        for link in links
        if str(link.get("forkmeshUser") or "").strip()
    })
    if not names:
        return []
    sql = (
        "SELECT username,data FROM users WHERE username IN ("
        + ",".join(_sql_quote(name) for name in names)
        + ") LIMIT " + str(len(names))
    )
    return _wrangler_rows(database, environment, sql)


def encrypted_identity_updates(links, rows, data_key):
    """Return old/new ciphertexts with Discord identity stored inside each row."""
    by_name = {
        str(row.get("username") or "").strip().lower(): row
        for row in rows or []
    }
    updates = {}
    missing = []
    for link in links:
        name = str(link.get("forkmeshUser") or "").strip().lower()
        row = by_name.get(name)
        original = str((row or {}).get("data") or "")
        if not original:
            missing.append(name)
            continue
        record = decrypt_d1_record(original, data_key)
        record["discordId"] = str(link.get("discordId") or "").strip()
        username = str(link.get("discordUsername") or "").strip()[:80]
        display_name = str(link.get("discordDisplayName") or "").strip()[:100]
        if username:
            record["discordUsername"] = username
        else:
            record.pop("discordUsername", None)
        if display_name:
            record["discordDisplayName"] = display_name
        else:
            record.pop("discordDisplayName", None)
        updates[name] = {
            "original": original,
            "updated": encrypt_d1_record(record, data_key),
        }
    if missing:
        raise RuntimeError(
            "account record missing for: " + ", ".join(sorted(missing)))
    return updates


def _apply(database, sql, environment):
    path = ""
    try:
        with tempfile.NamedTemporaryFile(
                mode="w", encoding="utf-8", suffix=".sql",
                prefix="forkmesh-discord-accounts-", delete=False) as handle:
            handle.write(sql)
            path = handle.name
        subprocess.run(
            [
                "bash", "-c",
                '. ./pywrangler.sh; pywrangler d1 execute "$1" --remote '
                '--file "$2"',
                "sync-discord-accounts", database, path,
            ],
            cwd=APP_ROOT, env=environment, check=True,
        )
    finally:
        if path:
            Path(path).unlink(missing_ok=True)


def resolve(messages):
    claims = extract_claims(messages)
    entries = merge_claims(claims, messages)
    return entries, reporters_without_account(messages, entries)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--from-file", help="read messages from a JSON file")
    parser.add_argument("--apply", action="store_true",
                        help="write the links to D1 (otherwise a dry run)")
    parser.add_argument("--include-guesses", action="store_true",
                        help="also write links the connector called a guess")
    parser.add_argument("--verify-accounts", action="store_true",
                        help="check every mapped name against the live "
                             "account directory")
    parser.add_argument("--database", default="forkmesh")
    parser.add_argument("--env-file", type=Path,
                        default=APP_ROOT / ".env.production")
    args = parser.parse_args(argv)

    if args.from_file:
        raw = json.loads(Path(args.from_file).read_text())
        messages = raw if isinstance(raw, list) else raw.get("messages", [])
    else:
        token = os.environ.get("DISCORD_BOT_TOKEN", "").strip()
        if not token:
            print("DISCORD_BOT_TOKEN is not set", file=sys.stderr)
            return 2
        try:
            messages = fetch_messages(token)
        except urllib.error.URLError as error:
            print(f"could not reach Discord: {error}", file=sys.stderr)
            return 2

    entries, unmapped = resolve(messages)

    if args.verify_accounts or args.apply:
        try:
            accounts = fetch_accounts()
        except urllib.error.URLError as error:
            print(f"could not reach the account directory: {error}",
                  file=sys.stderr)
            return 2
        problems = verify_against_accounts(entries, accounts)
        for problem in problems:
            print(f"  no such account: {problem}", file=sys.stderr)
        if problems:
            return 1

    writable = [e for e in entries
                if args.include_guesses or e["confidence"] != "connector-guess"]
    skipped = [e for e in entries if e not in writable]
    conflicts = [e for e in entries if e.get("conflicts")]

    print(f"{len(entries)} link(s) resolved, {len(writable)} writable, "
          f"{len(unmapped)} reporter(s) unmapped, {len(conflicts)} conflicted")
    for entry in sorted(writable, key=lambda e: e["discordUsername"]):
        print(f"  {entry['discordUsername']:24s} -> {entry['forkmeshUser']:16s}"
              f" {entry['confidence']}")
    for entry in skipped:
        print(f"  SKIPPED (guess, use --include-guesses): "
              f"{entry['discordUsername']} -> {entry['forkmeshUser']}")
    for entry in conflicts:
        print(f"  CONFLICT: {entry['discordUsername']} -> "
              f"{entry['forkmeshUser']} ({entry['confidence']}) vs "
              + ", ".join(c["forkmeshUser"] for c in entry["conflicts"]))
    for reporter in unmapped:
        print(f"  UNMAPPED: {reporter['discordUsername']} "
              f"({reporter['discordDisplayName']}) -- ask them in-channel")

    if not args.apply:
        print("\ndry run; pass --apply to write these links to D1")
        return 0

    environment = _load_env(args.env_file)
    data_key = environment.get("DATA_KEY", "").strip()
    if not data_key:
        print("DATA_KEY is missing from the env file; cannot compute the "
              "blind index", file=sys.stderr)
        return 2
    try:
        account_rows = load_encrypted_account_records(
            args.database, environment, writable)
        encrypted_records = encrypted_identity_updates(
            writable, account_rows, data_key)
    except (RuntimeError, ValueError) as error:
        print(f"could not prepare encrypted account identities: {error}",
              file=sys.stderr)
        return 2
    sql = render_sql(writable, data_key, encrypted_records)
    _apply(args.database, sql, environment)
    print(f"applied {len(writable)} link(s) to {args.database}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
