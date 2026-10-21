"""Tips on comments, issues and pull requests, wallet to wallet over Solana Pay.

A comment whose author has a Solana address on their profile (the existing
public ``solana`` payout field) gets a Tip button. Tipping asks this module for
a transfer request: it checks the reader, the recipient and the amount, mints a
fresh random ``reference`` public key and answers with a Solana Pay link
(``_solana_pay_uri``) the dashboard turns into a QR code. The tipper's own
wallet signs and sends the transfer straight to the author's address.

The forge never holds a key, never holds funds and never signs or submits a
transaction: verification is read-only. The tip sheet asks
``POST /api/tips/verify`` while it is open, and the ``tip-verify`` cron phase
looks again for a couple of hours, through ``_solana_rpc`` (keyless public
endpoints with fail-over, plus the operator's SOLANA_RPC_URL). A transaction
that carries the reference, succeeded, and moved at least the asked amount of
the asked token to the recipient marks the tip confirmed (its signature can
confirm one tip only), adds a "tipped" count to the comment and tells the
author with the existing ``tip_sent`` ping. A SOL tip is also written to
``tip_notice``, the receipt table desktop tips use.

Stored per tip: the repository and comment, blind indexes of tipper and
recipient, token, amount, reference, status and signature, and an encrypted
``{tipper, recipient, address, owner, repo}``. Pending requests expire after a
day; each account has a rate and a daily cap, and a record has a cap.
"""

import json
import re
import secrets
from decimal import Decimal, InvalidOperation
from urllib.parse import parse_qs, urlparse

SETTING_KEY = "tips"
USDC_MINT = "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v"
SOL_DECIMALS = 9
MAX_SOL_UNITS = 1000 * 10 ** 9
MAX_TOKEN_UNITS = 10 ** 18
REFERENCE_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
SIGNATURE_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{64,88}$")
SYMBOL_RE = re.compile(r"^[A-Za-z0-9$]{1,10}$")
AMOUNT_RE = re.compile(r"^\d{1,12}(?:\.\d{1,18})?$")
NAMES_MAX = 30
PENDING_TTL_MS = 24 * 60 * 60 * 1000
CRON_WINDOW_MS = 2 * 60 * 60 * 1000
CRON_RECHECK_MS = 2 * 60 * 1000
CRON_BATCH = 3
MAX_PENDING_PER_ACCOUNT = 10
MAX_INTENTS_PER_DAY = 50
MAX_TIPS_PER_RECORD = 5000
DAY_MS = 24 * 60 * 60 * 1000
RATE_WINDOW_MS = 60 * 1000
INTENT_RATE = 20
VERIFY_RATE = 12
SIGNATURES_LIMIT = 3
TRANSACTIONS_CHECKED = 2
PUBLIC_CACHE = "public, max-age=60, stale-while-revalidate=300"
PRIVATE_CACHE = "private, no-store"
DISCLAIMER = ("Tips go directly wallet to wallet; the forge never holds "
              "funds, keys or transactions.")
KINDS = {"issue": "issues", "pull": "pull", "discussion": "discussions"}
COMMENT_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@+-]{0,159}$")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def valid_address(value):
    """A base58 Solana public key: 32 to 44 characters that decode to 32
    bytes. The decode catches typos the alphabet check alone lets through."""
    text = str(value or "").strip() if isinstance(value, str) else ""
    if not REFERENCE_RE.fullmatch(text):
        return False
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    number = 0
    for char in text:
        number = number * 58 + alphabet.index(char)
    leading = len(text) - len(text.lstrip("1"))
    size = leading + (number.bit_length() + 7) // 8
    return size == 32


def new_reference():
    """A fresh random 32-byte public key in base58: never a real account, so
    the only transactions naming it are the ones paying this request."""
    data = secrets.token_bytes(32)
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    number = int.from_bytes(data, "big")
    out = ""
    while number:
        number, rem = divmod(number, 58)
        out = alphabet[rem] + out
    pad = len(data) - len(data.lstrip(b"\0"))
    return "1" * pad + (out or "1")


def to_units(amount, decimals):
    """Decimal text -> integer base units, or 0 when it does not fit."""
    text = str(amount or "").strip() if isinstance(amount, (str, int, float)) else ""
    if not AMOUNT_RE.fullmatch(text):
        return 0
    try:
        value = Decimal(text) * (Decimal(10) ** int(decimals))
    except InvalidOperation:
        return 0
    if value != value.to_integral_value():
        return 0
    return int(value)


def clean_settings(doc):
    doc = doc if isinstance(doc, dict) else {}
    token = doc.get("token") if isinstance(doc.get("token"), dict) else None
    clean_token = None
    if token and valid_address(token.get("mint")):
        try:
            decimals = int(token.get("decimals"))
        except (TypeError, ValueError):
            decimals = -1
        symbol = str(token.get("symbol") or "").strip()
        if 0 <= decimals <= 18 and SYMBOL_RE.fullmatch(symbol) \
                and token["mint"] != USDC_MINT:
            clean_token = {"mint": token["mint"], "symbol": symbol,
                           "decimals": decimals}
    return {"enabled": doc.get("enabled", True) is not False,
            "usdc": doc.get("usdc", True) is not False,
            "token": clean_token}


def tokens(config):
    out = [{"id": "sol", "symbol": "SOL", "mint": "", "decimals": SOL_DECIMALS}]
    if config["usdc"]:
        out.append({"id": "usdc", "symbol": "USDC", "mint": USDC_MINT,
                    "decimals": 6})
    if config["token"]:
        out.append(dict(config["token"], id="token"))
    return out


async def settings(env):
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", SETTING_KEY)
        doc = json.loads((row or {}).get("doc") or "{}")
    except Exception:
        doc = {}
    return clean_settings(doc)


def _account_keys(tx):
    message = (((tx or {}).get("transaction") or {}).get("message") or {})
    keys = []
    for key in message.get("accountKeys") or []:
        keys.append(str(key.get("pubkey") if isinstance(key, dict) else key))
    loaded = ((tx or {}).get("meta") or {}).get("loadedAddresses") or {}
    for group in ("writable", "readonly"):
        keys.extend(str(key) for key in loaded.get(group) or [])
    return keys


def transfer_matches(tx, recipient, reference, mint, units):
    """True when a jsonParsed transaction succeeded, names ``reference`` and
    moved at least ``units`` of SOL (no mint) or of ``mint`` to
    ``recipient``. Balances, not instructions, are compared, so a transfer
    wrapped in any program still counts and a failed one never does."""
    if not isinstance(tx, dict):
        return False
    meta = tx.get("meta") if isinstance(tx.get("meta"), dict) else None
    if meta is None or meta.get("err") is not None:
        return False
    keys = _account_keys(tx)
    if reference not in keys:
        return False
    if not mint:
        if recipient not in keys:
            return False
        index = keys.index(recipient)
        try:
            delta = int(meta["postBalances"][index]) - int(meta["preBalances"][index])
        except (KeyError, IndexError, TypeError, ValueError):
            return False
        return delta >= units

    def balances(name):
        out = {}
        for row in meta.get(name) or []:
            if not isinstance(row, dict) or row.get("mint") != mint \
                    or row.get("owner") != recipient:
                continue
            try:
                out[int(row.get("accountIndex"))] = int(
                    (row.get("uiTokenAmount") or {}).get("amount") or 0)
            except (TypeError, ValueError):
                continue
        return out

    before, after = balances("preTokenBalances"), balances("postTokenBalances")
    return any(amount - before.get(index, 0) >= units
               for index, amount in after.items())


def _error(code, status=400, headers=None):
    return json_response({"error": code}, status=status,  # noqa: F821
                         cache_control=PRIVATE_CACHE, extra_headers=headers)


async def _rate_limited(env, key, limit, now):
    row = await d1_first(  # noqa: F821
        env,
        "INSERT INTO comment_extras_rate (k, window_start, count) "
        "VALUES (?,?,1) ON CONFLICT(k) DO UPDATE SET "
        "count=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN 1 ELSE comment_extras_rate.count+1 END, "
        "window_start=CASE WHEN ?-comment_extras_rate.window_start>=? "
        "THEN ? ELSE comment_extras_rate.window_start END "
        "RETURNING count", key, now, now, RATE_WINDOW_MS, now,
        RATE_WINDOW_MS, now)
    return int((row or {}).get("count") or 1) > limit


async def _count(env, sql, *args):
    row = await d1_first(env, sql, *args)  # noqa: F821
    return int((row or {}).get("n") or 0)


async def wallet_of(env, name):
    """The public tip address on ``name``'s profile, or ""."""
    name = clean_string(name, MAX_NODE_NAME).strip().lower()  # noqa: F821
    if not valid_node_name(name):  # noqa: F821
        return ""
    try:
        _bi, rec = await _account_row(env, name)  # noqa: F821
    except Exception:
        return ""
    address = str((rec or {}).get("solana") or "").strip()
    return address if SOLANA_RE.match(address) and valid_address(address) \
        else ""  # noqa: F821


async def _recipients(env, request):
    query = parse_qs(urlparse(str(request.url)).query)
    raw = ",".join(query.get("names") or [])
    names = []
    for part in raw.split(","):
        name = part.strip().lower()
        if name and name not in names and valid_node_name(name):  # noqa: F821
            names.append(name)
        if len(names) >= NAMES_MAX:
            break
    wallets = {}
    for name in names:
        address = await wallet_of(env, name)
        if address:
            wallets[name] = address
    return json_response({"ok": True, "wallets": wallets},  # noqa: F821
                         cache_control=PUBLIC_CACHE)


async def _session(env, request, data):
    account_bi, rec = await _account_session_record(env, request, data)  # noqa: F821
    account = clean_string((rec or {}).get("name"), MAX_NODE_NAME).lower()  # noqa: F821
    if not rec or not account_bi or not account:
        return "", "", _error("unauthorized", 401)
    return account_bi, account, None


async def _json(request):
    try:
        data = await bounded_json_request(request)  # noqa: F821
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def intent_handler(env, request, owner, repo):
    """POST /api/repo/<owner>/<repo>/tips: a transfer request for one tip."""
    await ensure_schema(env)  # noqa: F821
    if method_name(request) != "POST":  # noqa: F821
        return _error("method_not_allowed", 405)
    data = await _json(request)
    if data is None:
        return _error("invalid_json")
    account_bi, account, refused = await _session(env, request, data)
    if refused is not None:
        return refused
    config = await settings(env)
    if not config["enabled"]:
        return _error("tips_disabled", 404)
    kind = str(data.get("kind") or "")
    try:
        number = int(data.get("number"))
    except (TypeError, ValueError):
        number = 0
    comment = str(data.get("comment") or "") if isinstance(
        data.get("comment"), str) else ""
    if kind not in KINDS or not 0 < number <= 10 ** 9 \
            or not COMMENT_RE.fullmatch(comment):
        return _error("invalid_target")
    recipient = clean_string(data.get("recipient"), MAX_NODE_NAME).strip().lower()  # noqa: F821
    if not valid_node_name(recipient):  # noqa: F821
        return _error("invalid_recipient")
    if recipient == account:
        return _error("self_tip")
    token = next((t for t in tokens(config) if t["id"] == data.get("token")), None)
    if token is None:
        return _error("invalid_token")
    units = to_units(data.get("amount"), token["decimals"])
    if units <= 0 or units > (MAX_SOL_UNITS if token["id"] == "sol"
                              else MAX_TOKEN_UNITS):
        return _error("invalid_amount")
    access = await _repository_access_context(  # noqa: F821
        env, request, owner, repo, viewer=account)
    if access is None:
        return _error("not_found", 404)
    address = await wallet_of(env, recipient)
    if not address:
        return _error("no_wallet", 404)
    now = int(Date.now())  # noqa: F821
    if await _rate_limited(env, "tips:" + account_bi, INTENT_RATE, now):
        return _error("rate_limited", 429, {"Retry-After": "30"})
    await d1_run(  # noqa: F821
        env, "DELETE FROM comment_tips WHERE status='pending' AND created_at<?",
        now - PENDING_TTL_MS)
    if await _count(env, "SELECT COUNT(*) AS n FROM comment_tips WHERE "
                    "tipper_bi=? AND created_at>=?", account_bi,
                    now - DAY_MS) >= MAX_INTENTS_PER_DAY:
        return _error("daily_limit", 429)
    if await _count(env, "SELECT COUNT(*) AS n FROM comment_tips WHERE "
                    "tipper_bi=? AND status='pending'",
                    account_bi) >= MAX_PENDING_PER_ACCOUNT:
        await d1_run(  # noqa: F821
            env, "DELETE FROM comment_tips WHERE reference IN (SELECT "
            "reference FROM comment_tips WHERE tipper_bi=? AND "
            "status='pending' ORDER BY created_at LIMIT 1)", account_bi)
    subject = "%s:%d" % (kind, number)
    if await _count(env, "SELECT COUNT(*) AS n FROM comment_tips WHERE "
                    "repo_bi=? AND subject=?", access["repoBi"],
                    subject) >= MAX_TIPS_PER_RECORD:
        return _error("record_full", 409)
    reference = new_reference()
    recipient_bi = await blind_index(env, recipient)  # noqa: F821
    sealed = await encrypt_row(env, {  # noqa: F821
        "tipper": account, "recipient": recipient, "address": address,
        "owner": owner, "repo": repo})
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO comment_tips (reference, repo_bi, subject, comment, "
        "tipper_bi, recipient_bi, token, mint, amount, status, signature, "
        "created_at, checked_at, confirmed_at, checks, data) VALUES "
        "(?,?,?,?,?,?,?,?,?,'pending','',?,0,0,0,?)",
        reference, access["repoBi"], subject, comment, account_bi,
        recipient_bi, token["id"], token["mint"], str(units), now, sealed)
    label = "Tip for @%s" % recipient
    message = "Thanks for your comment on %s/%s#%d" % (owner, repo, number)
    uri = _solana_pay_uri(  # noqa: F821
        address, units, reference, message, spl_token=token["mint"],
        decimals=token["decimals"], label=label)
    return json_response({  # noqa: F821
        "ok": True, "reference": reference, "uri": uri, "address": address,
        "recipient": recipient, "token": token["id"],
        "symbol": token["symbol"], "amount": _amount_units(  # noqa: F821
            units, token["decimals"]),
        "disclaimer": DISCLAIMER}, cache_control=PRIVATE_CACHE)


async def _find_signature(env, row, data):
    reference = row["reference"]
    found = await _solana_rpc(env, "getSignaturesForAddress", [  # noqa: F821
        reference, {"limit": SIGNATURES_LIMIT, "commitment": "confirmed"}])
    listed = (found or {}).get("result") if isinstance(found, dict) else None
    if not isinstance(listed, list):
        return None, False
    checked = 0
    for entry in listed:
        if not isinstance(entry, dict) or entry.get("err") is not None:
            continue
        signature = str(entry.get("signature") or "")
        if not SIGNATURE_RE.fullmatch(signature):
            continue
        if checked >= TRANSACTIONS_CHECKED:
            break
        checked += 1
        reply = await _solana_rpc(env, "getTransaction", [  # noqa: F821
            signature, {"encoding": "jsonParsed", "commitment": "confirmed",
                        "maxSupportedTransactionVersion": 0}])
        tx = (reply or {}).get("result") if isinstance(reply, dict) else None
        if transfer_matches(tx, data.get("address"), reference,
                            row.get("mint") or "", int(row["amount"])):
            return signature, True
    return None, True


async def verify(env, row, now):
    """Look for the transaction paying one pending tip. Returns "confirmed",
    "pending" or "unavailable" (the RPC answered nothing usable)."""
    try:
        data = await decrypt_row(env, row.get("data"))  # noqa: F821
    except Exception:
        data = None
    if not isinstance(data, dict) or not data.get("address"):
        return "pending"
    await d1_run(  # noqa: F821
        env, "UPDATE comment_tips SET checked_at=?, checks=checks+1 "
        "WHERE reference=?", now, row["reference"])
    signature, answered = await _find_signature(env, row, data)
    if not answered:
        return "unavailable"
    if not signature:
        return "pending"
    taken = await d1_first(  # noqa: F821
        env, "SELECT reference FROM comment_tips WHERE signature=?", signature)
    if taken and taken.get("reference") != row["reference"]:
        return "pending"
    await d1_run(  # noqa: F821
        env, "UPDATE comment_tips SET status='confirmed', signature=?, "
        "confirmed_at=? WHERE reference=? AND status='pending'",
        signature, now, row["reference"])
    await _announce(env, row, data, signature, now)
    return "confirmed"


async def _announce(env, row, data, signature, now):
    tipper = str(data.get("tipper") or "")
    recipient = str(data.get("recipient") or "")
    kind, _, number = str(row.get("subject") or "").partition(":")
    token = row.get("token") or "sol"
    decimals = SOL_DECIMALS
    symbol = "SOL"
    if token != "sol":
        config = await settings(env)
        match = next((t for t in tokens(config) if t["mint"] == row.get("mint")),
                     None)
        decimals = match["decimals"] if match else 0
        symbol = match["symbol"] if match else "tokens"
    amount = "%s %s" % (_amount_units(int(row["amount"]), decimals), symbol)  # noqa: F821
    if token == "sol":
        try:
            network = str(getattr(env, "SOLANA_NETWORK", "") or "mainnet")
            network = network if network in ("mainnet", "devnet", "testnet") \
                else "mainnet"
            await d1_run(  # noqa: F821
                env, "INSERT INTO tip_notice (signature, from_bi, to_bi, "
                "lamports, network, ts, emailed) VALUES (?,?,?,?,?,?,0) "
                "ON CONFLICT(signature) DO NOTHING", signature,
                row["tipper_bi"], row["recipient_bi"], int(row["amount"]),
                network, now)
        except Exception:
            pass
    owner, repo = str(data.get("owner") or ""), str(data.get("repo") or "")
    try:
        href = repo_web_href(owner, repo) + "/%s/%s" % (  # noqa: F821
            KINDS.get(kind, "issues"), number)
        await enqueue_notification(  # noqa: F821
            env, recipient, "tip_sent",
            "%s tipped your comment on %s/%s#%s" % (tipper, owner, repo, number),
            body="%s, sent wallet to wallet." % amount,
            repo=owner + "/" + repo, href=href, actor=tipper,
            source="tip:" + signature, dedupe="tip:" + signature, ts=now)
    except Exception:
        pass


async def _verify(env, request):
    data = await _json(request)
    if data is None:
        return _error("invalid_json")
    account_bi, _account, refused = await _session(env, request, data)
    if refused is not None:
        return refused
    reference = str(data.get("reference") or "") if isinstance(
        data.get("reference"), str) else ""
    if not REFERENCE_RE.fullmatch(reference):
        return _error("invalid_reference")
    row = await d1_first(  # noqa: F821
        env, "SELECT reference, subject, comment, tipper_bi, recipient_bi, "
        "token, mint, amount, status, signature, created_at, data FROM "
        "comment_tips WHERE reference=? AND tipper_bi=?", reference, account_bi)
    if not row:
        return _error("not_found", 404)
    if row.get("status") == "confirmed":
        return json_response({"ok": True, "status": "confirmed",  # noqa: F821
                              "signature": row.get("signature")},
                             cache_control=PRIVATE_CACHE)
    now = int(Date.now())  # noqa: F821
    if await _rate_limited(env, "tipcheck:" + account_bi, VERIFY_RATE, now):
        return _error("rate_limited", 429, {"Retry-After": "10"})
    status = await verify(env, row, now)
    found = await d1_first(  # noqa: F821
        env, "SELECT signature FROM comment_tips WHERE reference=?", reference)
    return json_response(  # noqa: F821
        {"ok": True, "status": status,
         "signature": (found or {}).get("signature") or ""},
        cache_control=PRIVATE_CACHE)


async def verify_cron(env):
    """The tip-verify phase: a few pending tips from the last two hours,
    least recently checked first, and pending ones older than a day
    dropped. At most CRON_BATCH x (1 + TRANSACTIONS_CHECKED) RPC calls, each
    charged to the phase's subrequest budget by _solana_rpc."""
    await ensure_schema(env)  # noqa: F821
    now = int(Date.now())  # noqa: F821
    await d1_run(  # noqa: F821
        env, "DELETE FROM comment_tips WHERE status='pending' AND created_at<?",
        now - PENDING_TTL_MS)
    rows = await d1_all(  # noqa: F821
        env, "SELECT reference, subject, comment, tipper_bi, recipient_bi, "
        "token, mint, amount, status, signature, created_at, data FROM "
        "comment_tips WHERE status='pending' AND created_at>=? AND "
        "checked_at<? ORDER BY checked_at LIMIT ?",
        now - CRON_WINDOW_MS, now - CRON_RECHECK_MS, CRON_BATCH)
    results = {"checked": 0, "confirmed": 0}
    for row in rows or []:
        try:
            status = await verify(env, row, now)
        except Exception:
            continue
        results["checked"] += 1
        if status == "confirmed":
            results["confirmed"] += 1
        if status == "unavailable":
            break
    return results


async def _settings(env, request):
    data = None
    if method_name(request) == "POST":  # noqa: F821
        data = await _json(request)
        if data is None:
            return _error("invalid_json")
    _account_bi, account, refused = await _session(env, request, data)
    if refused is not None:
        return refused
    if not await _is_admin(env, account):  # noqa: F821
        return _error("forbidden", 403)
    if data is None:
        config = await settings(env)
        return json_response(dict(config, ok=True, tokens=tokens(config)),  # noqa: F821
                             cache_control=PRIVATE_CACHE)
    token = data.get("token")
    if token not in (None, "") and not isinstance(token, dict):
        return _error("invalid_token")
    if isinstance(token, dict) and (token.get("mint") or "").strip():
        cleaned = clean_settings({"token": {
            "mint": str(token.get("mint") or "").strip(),
            "symbol": token.get("symbol"), "decimals": token.get("decimals")}})
        if cleaned["token"] is None:
            return _error("invalid_token")
        token = cleaned["token"]
    else:
        token = None
    doc = {"enabled": data.get("enabled", True) is not False,
           "usdc": data.get("usdc", True) is not False, "token": token}
    await d1_run(  # noqa: F821
        env, "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at", SETTING_KEY, json.dumps(doc),
        int(Date.now()))  # noqa: F821
    try:
        await _audit_sensitive_action(  # noqa: F821
            env, account, "admin.tips_settings", "instance_settings",
            SETTING_KEY, "success", doc)
    except Exception:
        pass
    config = clean_settings(doc)
    return json_response(dict(config, ok=True, tokens=tokens(config)),  # noqa: F821
                         cache_control=PRIVATE_CACHE)


async def handler(env, request, rest):
    await ensure_schema(env)  # noqa: F821
    method = method_name(request)  # noqa: F821
    if rest == "config" and method == "GET":
        config = await settings(env)
        return json_response({  # noqa: F821
            "ok": True, "enabled": config["enabled"],
            "tokens": tokens(config) if config["enabled"] else [],
            "disclaimer": DISCLAIMER}, cache_control=PUBLIC_CACHE)
    if rest == "recipients" and method == "GET":
        return await _recipients(env, request)
    if rest == "verify" and method == "POST":
        return await _verify(env, request)
    if rest == "settings" and method in ("GET", "POST"):
        return await _settings(env, request)
    if rest in ("config", "recipients", "verify", "settings"):
        return _error("method_not_allowed", 405)
    return _error("not_found", 404)
