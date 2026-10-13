"""Public, normalized projection of every relay-owned financial record.

Active records are projected from the tables that already own each state
transition, so a payout cannot say ``paid`` without exposing the same signature
here and a linked issue or pull request cannot drift away from its claim. A
separate append-only backfill stores only finalized transfers discovered by a
read-only scan of known public system addresses; canonical signatures suppress
their discovered copy at read time.

Only public receipt data leaves this module. Queries never select the temporary
encrypted key used by an optional World deposit checkout, and encrypted
microjob envelopes are reduced to their public claimant and subject fields
before they are returned.
"""

import calendar
import json
import re
from datetime import datetime, timezone
from typing import Any
from urllib.parse import parse_qs, quote, urlparse


# These primitives are injected by entry._LazyModule when this route is first
# loaded. Explicit placeholders keep the module independently lintable.
Date: Any = None
FORKMESH_SITE_URL: Any = None
_account_row: Any = None
_audit_sensitive_action: Any = None
_display_balance_lamports: Any = None
_forkbot_blob_text: Any = None
_forkbot_repo_gateway_json: Any = None
_forkmesh_email_card_html: Any = None
_html_escape: Any = None
_record_account_email: Any = None
_reward_balance_lamports: Any = None
_send_email: Any = None
_solana_rpc: Any = None
_store_wallet_balance: Any = None
blind_index: Any = None
bounded_json_request: Any = None
d1_all: Any = None
d1_first: Any = None
d1_run: Any = None
decrypt_row: Any = None
enqueue_notification: Any = None
ensure_schema: Any = None
js_fetch_with_timeout: Any = None
json_response: Any = None
method_name: Any = None
notification_email_enabled: Any = None


PAGE_SIZE_DEFAULT = 40
PAGE_SIZE_MAX = 100
BACKFILL_SIGNATURE_BATCH = 3
BACKFILL_BOUNTY_SEED_BATCH = 1
BACKFILL_TRANSFER_INSERT_BATCH = 8
BACKFILL_BOUNTY_CURSOR_KEY = "ledger_backfill:legacy_bounties:v1"
SOL_USD_QUOTE_TTL_MS = 5 * 60 * 1000
SOL_USD_ENDPOINT = (
    "https://api.coingecko.com/api/v3/simple/price"
    "?ids=solana&vs_currencies=usd"
)
LAMPORTS_PER_SOL = 1_000_000_000
USD_MICROS = 1_000_000
EXPECTED_EXPENSE_LOOKBACK_MONTHS = 6
EXPECTED_EXPENSE_LAPSE_MONTHS = 3
DAY_MS = 86_400_000
SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
SOLANA_SIGNATURE_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{64,120}$")
REPOSITORY_NAME_RE = re.compile(
    r"^[a-z0-9][a-z0-9_.-]{0,99}/[a-z0-9][a-z0-9_.-]{0,99}$")
REWARD_ID_RE = re.compile(r"^reward-[a-z0-9-]{8,88}$")
# These are the reward states in which a repository owner is still waiting on
# something before the payment is fully recorded. The payment queue can name
# the contributor's linked Discord identity in these rows without carrying it
# forward into the permanent paid history.
UNSETTLED_CONTRIBUTOR_REWARD_STATUSES = frozenset((
    "pending_merge", "pending_wallet", "ready", "submitted",
))
# One nudge per contributor per three days, whatever they are owed. The thing
# being asked for (publish a payout address) takes minutes and then never has
# to be done again, so a shorter window would only be pressure.
REWARD_WALLET_REMINDER_INTERVAL_MS = 3 * 24 * 60 * 60 * 1000
# A merge-conditional pull estimate leaves pending_merge only once the pull's
# own record says it merged (see _settle_merged_pull_rewards). Every check is
# one gateway subrequest on a Workers Free budget, so one read verifies at most
# this many rows, and an isolate does not ask again about a pull it just found
# unmerged until the recheck window has passed.
MERGED_PULL_REWARD_CHECKS_PER_REQUEST = 2
MERGED_PULL_REWARD_RECHECK_MS = 10 * 60 * 1000
MERGED_PULL_REWARD_MEMO_MAX = 256
_merged_pull_reward_misses = {}
# The subject's own headline, carried alongside the link label.
RESOURCE_TITLE_MAX = 160
# A compact, plain-text snapshot of what the subject was about. The source
# body, comments, attachments, and patches stay in the repository rather than
# being copied into a financial record.
RESOURCE_SUMMARY_MAX = 360
OUTPUT_URL_MAX = 1000
_SOL_USD_QUOTE = {"priceMicros": 0, "observedAt": 0, "expiresAt": 0}
SOURCE_TABLE_BY_CATEGORY = {
    "member-tip": "tip_notice",
    "pool-contribution": "reward_contributions",
    "pending-reward": "pending_rewards",
    "mirror-reward": "chain_intents",
    "microjob-payout": "microjob_claims",
    "contributor-reward": "contributor_reward_estimates",
    "world-element-purchase": "world_element_purchases",
    "world-element-sweep": "world_element_purchases",
    "world-mirror-share": "chain_intents",
    "advertising-revenue": "community_ad_revenue",
    "operating-expense": "operating_expenses",
    "reimbursement-debt": "operating_expenses",
    "expected-expense": "operating_expenses",
    "historical-chain-transfer": "ledger_backfill_transactions",
}


def _bind_runtime(runtime):
    """Receive initialized Worker helpers without importing its JS runtime."""
    namespace = globals()
    for name, value in runtime.items():
        if (
            not name.startswith("__")
            and (name not in namespace or namespace[name] is None)
        ):
            namespace[name] = value


def _integer(value, default=0, minimum=None, maximum=None):
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError):
        number = int(default)
    if minimum is not None:
        number = max(int(minimum), number)
    if maximum is not None:
        number = min(int(maximum), number)
    return number


def _sol_text(lamports):
    lamports = max(0, _integer(lamports))
    whole, fraction = divmod(lamports, LAMPORTS_PER_SOL)
    if not fraction:
        return str(whole)
    return "%d.%s" % (whole, ("%09d" % fraction).rstrip("0"))


async def _contributor_payment_valuation(reward, lamports, now):
    """Return the immutable USD snapshot owned by a reward payment row."""
    valued_at = _integer((reward or {}).get("payment_valued_at"), minimum=0)
    if valued_at:
        return (
            _integer((reward or {}).get("payment_usd_micros"), minimum=0),
            _integer(
                (reward or {}).get("payment_sol_usd_micros"), minimum=0),
            valued_at,
        )
    try:
        quote_record = await _sol_usd_quote()
    except Exception:
        quote_record = {}
    price_micros = _integer(
        (quote_record or {}).get("priceMicros"), minimum=0)
    _, value_micros = _usd_from_lamports(lamports, price_micros)
    # Even an unavailable quote is frozen as unavailable. Repricing a payment
    # on some later retry would be a historical rewrite, not a recovery.
    observed_at = _integer(
        (quote_record or {}).get("observedAt"), minimum=0)
    return (
        value_micros,
        price_micros,
        observed_at if price_micros and observed_at else max(0, _integer(now)),
    )


async def _contributor_payment_source_balance(env, source_address):
    """Return one live post-finality balance for the paying wallet.

    Display-balance endpoints are deliberately cached and rate-limited. A
    finalized payment receipt is the one place where that older value would be
    actively misleading, so read the public wallet once here and carry the
    result back in the receipt response. Failure is non-fatal: the desktop also
    schedules its ordinary forced display refresh.
    """
    try:
        # Contributor receipts are verified through the configured reward
        # cluster, so their post-payment reading must use that same RPC rail.
        lamports = await _reward_balance_lamports(env, source_address)
    except Exception:
        lamports = None
    if lamports is None:
        return {}
    checked_at = int(Date.now())
    try:
        await _store_wallet_balance(
            env, source_address, int(lamports), checked_at)
    except Exception:
        pass
    return {
        "sourceBalanceLamports": max(0, _integer(lamports)),
        "sourceBalanceCheckedAt": checked_at,
    }


def _pull_record_fields(text):
    """The ``key: value`` header of one ``.forkmesh/pulls/<n>/pull.md``."""
    raw = str(text or "").lstrip()
    if not raw.startswith("---"):
        return {}
    body = raw[3:]
    end = body.find("\n---")
    if end < 0:
        return {}
    fields = {}
    for line in body[:end].splitlines():
        key, separator, value = line.partition(":")
        key = key.strip()
        if separator and key and key not in fields:
            fields[key] = value.strip()
    return fields


def _awaits_pull_merge(row):
    """A pull estimate still held at pending_merge with nothing paid on it."""
    row = row or {}
    return (
        str(row.get("source_kind") or "") == "pull"
        and str(row.get("condition") or "") == "merged"
        and str(row.get("status") or "") == "pending_merge"
        and not str(row.get("tx_signature") or "").strip()
    )


PULL_RECORD_REFS = ("forkmesh/pull/{}", "refs/pr/{}/metadata", "forkmesh/pulls")


async def _pull_reward_merge_verified(env, row):
    """True only when the pull's own record says it merged under this estimate.

    The record is read from the first of the pull's own ``forkmesh/pull/<n>``
    branch, its ``refs/pr/<n>/metadata`` ref and the ``forkmesh/pulls`` index
    that holds it. A record that cannot be read, is not merged, names another
    pull, or no longer carries this reward id verifies nothing.
    """
    row = row or {}
    owner = str(row.get("repo_owner") or "").strip()
    repo = str(row.get("repo_name") or "").strip()
    number = _integer(row.get("subject_number"), minimum=0)
    reward_id = str(row.get("reward_id") or "").strip().lower()
    if (not owner or not repo or number < 1 or not reward_id
            or _forkbot_repo_gateway_json is None
            or _forkbot_blob_text is None):
        return False
    fields = {}
    for ref in PULL_RECORD_REFS:
        try:
            data = await _forkbot_repo_gateway_json(
                env, owner, repo,
                "blob?path=%s&ref=%s" % (
                    quote(".forkmesh/pulls/%d/pull.md" % number, safe=""),
                    quote(ref.format(number), safe="")))
            fields = _pull_record_fields(
                _forkbot_blob_text(data) if data else "")
        except Exception:
            return False
        if _integer(fields.get("number"), minimum=0) == number:
            break
    if str(fields.get("status") or "").strip().lower() not in (
            "merged", "closed-merged"):
        return False
    if _integer(fields.get("number"), minimum=0) != number:
        return False
    return str(fields.get("rewardId") or "").strip().lower() == reward_id


async def _promote_merged_pull_reward(env, reward_id, now):
    """Compare-and-set one verified estimate out of pending_merge."""
    await d1_run(
        env,
        "UPDATE contributor_reward_estimates SET status='pending_wallet',"
        "updated_at=? WHERE reward_id=? AND status='pending_merge' "
        "AND condition='merged' AND source_kind='pull' AND tx_signature=''",
        now, reward_id)


async def _settle_merged_pull_rewards(env, rows):
    """Heal merge-conditional pull estimates whose pull has already landed.

    Merging records the transition in pull.md, but D1 used to learn it only
    from a hand-run reward sync, so landed work kept reading "pays on merge"
    and offered no way to pay it. Each read of the estimates list re-checks a
    bounded number of those rows against the pull's own record and moves the
    merged ones to pending_wallet, which projects as ready once the recipient
    has a payout address. Nothing is promoted without that record, and the
    list is still served when the check cannot run. Returns the checks spent.
    """
    checks = 0
    now = 0
    for row in rows or []:
        if not isinstance(row, dict) or not _awaits_pull_merge(row):
            continue
        now = now or int(Date.now())
        reward_id = str(row.get("reward_id") or "").strip().lower()
        missed_at = _merged_pull_reward_misses.get(reward_id, 0)
        if missed_at and 0 <= now - missed_at < MERGED_PULL_REWARD_RECHECK_MS:
            continue
        if checks >= MERGED_PULL_REWARD_CHECKS_PER_REQUEST:
            break
        checks += 1
        merged = False
        try:
            if await _pull_reward_merge_verified(env, row):
                await _promote_merged_pull_reward(env, reward_id, now)
                merged = True
        except Exception:
            merged = False
        if not merged:
            if len(_merged_pull_reward_misses) >= MERGED_PULL_REWARD_MEMO_MAX:
                _merged_pull_reward_misses.clear()
            _merged_pull_reward_misses[reward_id] = now
            continue
        _merged_pull_reward_misses.pop(reward_id, None)
        row["status"] = "pending_wallet"
        row["updated_at"] = now
    return checks


async def contributor_reward_payment(
        env, sender, recipient, signature, network, reward_id,
        source_address, lamports, destination_address):
    """Authorize and attach one exact finalized owner-wallet transfer."""
    if (not re.fullmatch(r"reward-[a-z0-9-]{8,88}", reward_id)
            or not SOLANA_ADDRESS_RE.fullmatch(source_address)):
        return None, json_response(
            {"error": "invalid_reward_payment"}, status=400,
            cache_control="no-store")
    reward = await d1_first(
        env, "SELECT * FROM contributor_reward_estimates WHERE reward_id=?",
        reward_id)
    if not reward:
        return None, json_response(
            {"error": "reward_not_found"}, status=404,
            cache_control="no-store")
    if (str(reward.get("recipient_name") or "").lower() != recipient
            or int(reward.get("amount_lamports") or 0) != lamports):
        return None, json_response(
            {"error": "reward_payment_mismatch"}, status=409,
            cache_control="no-store")
    repo_owner = str(reward.get("repo_owner") or "").strip().lower()
    org_bi, org = await _org_row(env, repo_owner)
    authorized = (
        await _org_role(env, org_bi, sender) == "owner"
        if org else await _account_owns_node(env, sender, repo_owner)
    )
    if not authorized:
        return None, json_response(
            {"error": "reward_owner_required"}, status=403,
            cache_control="no-store")
    status = str(reward.get("status") or "")
    recorded_signature = str(reward.get("tx_signature") or "")
    if status == "paid":
        if recorded_signature != signature:
            return None, json_response(
                {"error": "reward_already_paid"}, status=409,
                cache_control="no-store")
        body = {"ok": True, "duplicate": True, "rewardStatus": "paid"}
        body.update(await _contributor_payment_source_balance(
            env, source_address))
        return None, json_response(body, cache_control="no-store")
    if _awaits_pull_merge(reward) and await _pull_reward_merge_verified(
            env, reward):
        # The queue may still hold a landed pull's estimate at pending_merge
        # (see _settle_merged_pull_rewards). Verify it here as well, so the
        # click that pays it is never refused over a transition nobody synced.
        await _promote_merged_pull_reward(env, reward_id, int(Date.now()))
        status = "pending_wallet"
    if status not in ("ready", "pending_wallet", "submitted"):
        return None, json_response(
            {"error": "reward_not_payable", "rewardStatus": status},
            status=409, cache_control="no-store")
    if (status == "submitted" and recorded_signature
            and recorded_signature != signature):
        return None, json_response(
            {"error": "reward_signature_mismatch"}, status=409,
            cache_control="no-store")

    matches = await _solana_transaction_matches_plan(env, signature, {
        "network": "mainnet-beta" if network == "mainnet" else network,
        "sourceAddress": source_address,
        "transfers": [{
            "address": destination_address,
            "lamports": lamports,
        }],
    })
    if matches is False:
        return None, json_response(
            {"error": "reward_transaction_mismatch"}, status=409,
            cache_control="no-store")
    now = int(Date.now())
    payment_usd_micros, payment_sol_usd_micros, payment_valued_at = (
        await _contributor_payment_valuation(reward, lamports, now)
    )
    if matches is None:
        await d1_run(
            env,
            "UPDATE contributor_reward_estimates "
            "SET status='submitted',submitted_at=?,updated_at=?,"
            "source_address=?,destination_address=?,tx_signature=?,"
            "payment_usd_micros=CASE WHEN payment_valued_at=0 THEN ? "
            "ELSE payment_usd_micros END,"
            "payment_sol_usd_micros=CASE WHEN payment_valued_at=0 THEN ? "
            "ELSE payment_sol_usd_micros END,"
            "payment_valued_at=CASE WHEN payment_valued_at=0 THEN ? "
            "ELSE payment_valued_at END "
            "WHERE reward_id=? AND status IN "
            "('ready','pending_wallet','submitted')",
            now, now, source_address, destination_address, signature,
            payment_usd_micros, payment_sol_usd_micros, payment_valued_at,
            reward_id)
        return None, json_response({
            "ok": True,
            "status": "submitted",
            "rewardStatus": "submitted",
            "verification": "pending_finality",
        }, status=202, cache_control="no-store")

    # Compare-and-set prevents two concurrent clicks from attaching different
    # finalized transfers to one estimate.
    result = await d1_run(
        env,
        "UPDATE contributor_reward_estimates SET status='paid',"
        "paid_at=?,updated_at=?,source_address=?,destination_address=?,"
        "tx_signature=?,"
        "payment_usd_micros=CASE WHEN payment_valued_at=0 THEN ? "
        "ELSE payment_usd_micros END,"
        "payment_sol_usd_micros=CASE WHEN payment_valued_at=0 THEN ? "
        "ELSE payment_sol_usd_micros END,"
        "payment_valued_at=CASE WHEN payment_valued_at=0 THEN ? "
        "ELSE payment_valued_at END WHERE reward_id=? AND status IN "
        "('ready','pending_wallet','submitted') AND "
        "(tx_signature='' OR tx_signature=?)",
        now, now, source_address, destination_address, signature,
        payment_usd_micros, payment_sol_usd_micros, payment_valued_at,
        reward_id, signature)
    try:
        changes = int(result.meta.changes)
    except Exception:
        try:
            converted = result.to_py() if hasattr(result, "to_py") else result
            changes = int((converted.get("meta") or {}).get("changes") or 0)
        except Exception:
            changes = 0
    if changes == 1:
        reward.update(await _contributor_payment_source_balance(
            env, source_address))
        return reward, None
    latest = await d1_first(
        env,
        "SELECT status,tx_signature FROM contributor_reward_estimates "
        "WHERE reward_id=?",
        reward_id)
    if (str((latest or {}).get("status") or "") == "paid"
            and str((latest or {}).get("tx_signature") or "") == signature):
        body = {"ok": True, "duplicate": True, "rewardStatus": "paid"}
        body.update(await _contributor_payment_source_balance(
            env, source_address))
        return None, json_response(body, cache_control="no-store")
    return None, json_response(
        {"error": "reward_already_paid"}, status=409,
        cache_control="no-store")


def _discord_identity_text(value, maximum):
    """Return one bounded, plain-text Discord identity field."""
    return re.sub(r"\s+", " ", str(value or "")).strip()[:maximum]


async def _contributor_reward_recipient_profiles(env, rows):
    """Return the payment-queue identity for each unsettled recipient.

    The account record remains the source of truth for its currently published
    wallet. The queue also needs the linked Discord name that identifies a
    reporter to the owner; no Discord ID or other account fields leave this
    narrow projection.
    """
    profiles_by_recipient = {}
    recipient_indexes = sorted({
        str(row.get("recipient_bi") or "")
        for row in rows or []
        if str(row.get("status") or "")
        in UNSETTLED_CONTRIBUTOR_REWARD_STATUSES
        and str(row.get("recipient_bi") or "")
    })
    for offset in range(0, len(recipient_indexes), 80):
        batch = recipient_indexes[offset:offset + 80]
        user_rows = await d1_all(
            env,
            "SELECT user_bi,data FROM users WHERE user_bi IN (%s)"
            % ",".join("?" for _ in batch),
            *batch,
        )
        for user in user_rows or []:
            try:
                record = await decrypt_row(env, user.get("data"))
            except Exception:
                record = {}
            record = record if isinstance(record, dict) else {}
            wallet = str(record.get("solana") or "").strip()
            username = _discord_identity_text(
                record.get("discordUsername"), 80).lstrip("@")
            display_name = _discord_identity_text(
                record.get("discordDisplayName"), 100)
            profiles_by_recipient[str(user.get("user_bi") or "")] = {
                "wallet": (
                    wallet if SOLANA_ADDRESS_RE.fullmatch(wallet) else ""),
                "discordUsername": username,
                "discordDisplayName": display_name,
            }
    return profiles_by_recipient


async def contributor_reward_estimates_handler(env, request):
    """Public owner-authored issue/PR estimates and their receipt state."""
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET"})
    query = parse_qs(urlparse(request.url).query)
    owner_scoped = (query.get("ownerScope") or [""])[0] == "1"
    repository_filter = str(
        (query.get("repository") or [""])[0]).strip().lower()
    if owner_scoped:
        if not REPOSITORY_NAME_RE.fullmatch(repository_filter):
            return json_response(
                {"error": "invalid_repository_scope"}, status=400,
                cache_control="no-store")
        _, account = await _account_session_record(env, request)
        actor = str((account or {}).get("name") or "").strip().lower()
        if not actor:
            return json_response(
                {"error": "unauthorized"}, status=401,
                cache_control="no-store")
        repo_owner, _ = repository_filter.split("/", 1)
        org_bi, org = await _org_row(env, repo_owner)
        authorized = (
            await _org_role(env, org_bi, actor) == "owner"
            if org else await _account_owns_node(env, actor, repo_owner)
        )
        if not authorized:
            return json_response(
                {"error": "reward_owner_required"}, status=403,
                cache_control="no-store")
    await ensure_schema(env)
    if owner_scoped:
        repo_owner, repo_name = repository_filter.split("/", 1)
        rows = await d1_all(
            env,
            "SELECT reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
            "source_kind,subject_number,subject_title,subject_summary,"
            "amount_lamports,condition,status,created_at,updated_at,"
            "destination_address,tx_signature,payment_usd_micros,"
            "payment_sol_usd_micros,payment_valued_at,"
            # When this contributor was last asked for a payout address. The
            # limit is per person, so every row owed to them shares the answer.
            "(SELECT reminders.sent_at FROM reward_wallet_reminders AS reminders "
            "WHERE reminders.recipient_bi=contributor_reward_estimates.recipient_bi)"
            " AS wallet_reminder_sent_at "
            "FROM contributor_reward_estimates WHERE status<>'cancelled' "
            "AND lower(repo_owner)=? AND lower(repo_name)=? "
            "ORDER BY created_at DESC LIMIT 250",
            repo_owner, repo_name)
    else:
        rows = await d1_all(
            env,
            "SELECT reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
            "source_kind,subject_number,subject_title,subject_summary,"
            "amount_lamports,condition,status,created_at,updated_at,"
            "destination_address,tx_signature,payment_usd_micros,"
            "payment_sol_usd_micros,payment_valued_at "
            "FROM contributor_reward_estimates WHERE status<>'cancelled' "
            "ORDER BY created_at DESC LIMIT 250")
    # Landed pulls stop reading "pays on merge" where the payment queue is
    # read, instead of waiting for someone to re-run the reward sync.
    await _settle_merged_pull_rewards(env, rows)
    profiles_by_recipient = await _contributor_reward_recipient_profiles(
        env, rows)
    output = []
    for row in rows or []:
        status = str(row.get("status") or "")
        destination = str(row.get("destination_address") or "")
        recipient_profile = profiles_by_recipient.get(
            str(row.get("recipient_bi") or ""), {})
        if status in ("pending_wallet", "ready"):
            destination = recipient_profile.get("wallet", "")
            status = "ready" if destination else "pending_wallet"
        owner = str(row.get("repo_owner") or "")
        repo = str(row.get("repo_name") or "")
        kind = str(row.get("source_kind") or "")
        number = int(row.get("subject_number") or 0)
        payment_valued_at = _integer(
            row.get("payment_valued_at"), minimum=0)
        payment_price_micros = _integer(
            row.get("payment_sol_usd_micros"), minimum=0)
        payment_usd_micros = _integer(
            row.get("payment_usd_micros"), minimum=0)
        payment_usd = (
            _usd_text(payment_usd_micros)
            if payment_valued_at and payment_price_micros else None
        )
        reward = {
            "rewardId": str(row.get("reward_id") or ""),
            "recipient": str(row.get("recipient_name") or ""),
            "recipientAddress": destination,
            "repository": owner + "/" + repo,
            "sourceKind": kind,
            "subjectNumber": number,
            "subjectTitle": str(row.get("subject_title") or ""),
            "subjectSummary": str(row.get("subject_summary") or "")[:
                RESOURCE_SUMMARY_MAX],
            "subjectUrl": "/%s/%s/%s/%d" % (
                quote(owner, safe=""), quote(repo, safe=""),
                "pulls" if kind == "pull" else "issues", number),
            "amountLamports": int(row.get("amount_lamports") or 0),
            "amountSol": _sol_text(int(row.get("amount_lamports") or 0)),
            "condition": str(row.get("condition") or ""),
            "status": status,
            "createdAt": int(row.get("created_at") or 0),
            "updatedAt": int(row.get("updated_at") or 0),
            "transactionSignature": str(row.get("tx_signature") or ""),
            "paymentUsd": payment_usd,
            "paymentUsdMicros": payment_usd_micros,
            "paymentSolUsdPriceMicros": payment_price_micros,
            "paymentValuedAt": payment_valued_at,
            "valuationBasis": (
                "recorded-at-payment-sol-usd" if payment_usd is not None
                else "payment-sol-usd-unavailable" if payment_valued_at
                else "live-sol-usd-spot-estimate"),
        }
        # This label is only for a reward still awaiting payment. The public
        # receipt remains keyed to the ForkMesh account after it is settled.
        # Only the owner who can send the next reminder learns when the last
        # one went out; the public list never says who was nudged or when.
        reminder_sent_at = _integer(
            row.get("wallet_reminder_sent_at"), minimum=0)
        if owner_scoped and status == "pending_wallet" and reminder_sent_at:
            reward["walletReminderSentAt"] = reminder_sent_at
            reward["walletReminderAvailableAt"] = (
                reminder_sent_at + REWARD_WALLET_REMINDER_INTERVAL_MS)
        if status in UNSETTLED_CONTRIBUTOR_REWARD_STATUSES:
            username = recipient_profile.get("discordUsername", "")
            display_name = recipient_profile.get("discordDisplayName", "")
            if username:
                reward["recipientDiscordUsername"] = username
            if display_name:
                reward["recipientDiscordDisplayName"] = display_name
        output.append(reward)
    return json_response({
        "ok": True,
        "rewards": output,
        "custody": "direct-owner-wallet",
        "authorization": (
            "owner-session" if owner_scoped else "public-read"),
        "repository": repository_filter if owner_scoped else "",
        "notice": (
            "Estimates are authored by repository owners. A paid record is "
            "backed by an exact finalized direct Solana transfer."),
    }, cache_control="no-store")


def _wallet_reminder_email(owner, recipient, waiting, lamports):
    """One nudge asking a contributor to publish a payout address."""
    amount = _sol_text(lamports)
    plural = "rewards" if waiting != 1 else "reward"
    settings_url = FORKMESH_SITE_URL.rstrip("/") + "/dashboard/settings"
    subject = "%s is waiting to pay you on ForkMesh" % owner
    text = (
        "Hi " + recipient + ",\n\n"
        + owner + " has approved " + str(waiting) + " contributor " + plural
        + " for you on ForkMesh, worth " + amount + " in total. They cannot "
        "send it: paying needs a public Solana payout address on your "
        "profile, and yours is empty.\n\n"
        "Add one here:\n" + settings_url + "\n\n"
        "ForkMesh does not create a wallet for you and never holds the funds. "
        "You publish an address you already control, and " + owner + " signs "
        "the transfer directly to it from their own wallet.\n\n"
        "If you would rather not be paid, ignore this — nothing expires and "
        "no one is charged."
    )
    safe_owner = _html_escape(owner)
    intro = (
        "<strong class=\"fm-strong\" style=\"color:#f5f5f5\">" + safe_owner
        + "</strong> has approved <strong class=\"fm-strong\" "
        "style=\"color:#f5f5f5\">" + str(waiting) + " contributor " + plural
        + "</strong> for you, worth <strong class=\"fm-strong\" "
        "style=\"color:#f5f5f5\">" + _html_escape(amount)
        + "</strong> in total."
    )
    body_html = (
        "<p class=\"fm-text\" style=\"margin:0 0 18px;color:#d4d4d8;font-size:14px\">"
        "They cannot send it yet. Paying needs a public Solana payout address "
        "on your ForkMesh profile, and yours is empty.</p>"
        "<div class=\"fm-border\" style=\"border-top:1px solid #313134;"
        "padding-top:18px;margin-top:22px\">"
        "<p class=\"fm-muted\" style=\"margin:0;color:#a3a3a3;font-size:14px\">"
        "ForkMesh does not create a wallet for you and never holds the funds. "
        "You publish an address you already control, and " + safe_owner
        + " signs the transfer directly to it from their own wallet.</p></div>"
    )
    footer_html = (
        "<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#8a8a93;font-size:12px\">"
        "You are getting this because a repository owner tried to pay you. "
        "Turn off reward email in your ForkMesh notification settings to stop "
        "these.</p>"
    )
    html = _forkmesh_email_card_html(
        "A reward is waiting for your payout address", intro, body_html,
        footer_html, action_label="Add a payout address",
        action_url=settings_url)
    return subject, text, html


async def contributor_reward_wallet_reminder_handler(env, request):
    """Ask a reward recipient to publish the payout address that unblocks it.

    An "awaiting the recipient's wallet" row on the Ledger is the one pending
    state the repository owner cannot resolve themselves: the estimate is
    approved and funded, and it stays unpayable until the contributor puts a
    self-custodial address on their profile. This turns that dead end into one
    rate-limited nudge, addressed to the recipient rather than to the owner.

    It sends nothing the recipient's own record does not still justify: the
    reward must still be waiting on a wallet, the profile must still be without
    one, and the recipient's notification preferences still decide whether mail
    goes out at all.
    """
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        data = {}
    if not isinstance(data, dict):
        data = {}
    reward_id = str(data.get("rewardId") or "").strip().lower()[:96]
    if not REWARD_ID_RE.fullmatch(reward_id):
        return json_response(
            {"error": "invalid_reward_id"}, status=400,
            cache_control="no-store")
    _, account = await _account_session_record(env, request, data)
    actor = str((account or {}).get("name") or "").strip().lower()
    if not actor:
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control="no-store")

    await ensure_schema(env)
    row = await d1_first(
        env,
        "SELECT reward_id,recipient_bi,recipient_name,repo_owner,repo_name,"
        "status,tx_signature FROM contributor_reward_estimates "
        "WHERE reward_id=?",
        reward_id)
    if not row:
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    repo_owner = str(row.get("repo_owner") or "").strip().lower()
    org_bi, org = await _org_row(env, repo_owner)
    authorized = (
        await _org_role(env, org_bi, actor) == "owner"
        if org else await _account_owns_node(env, actor, repo_owner)
    )
    if not authorized:
        return json_response(
            {"error": "reward_owner_required"}, status=403,
            cache_control="no-store")
    if (str(row.get("status") or "") != "pending_wallet"
            or str(row.get("tx_signature") or "").strip()):
        return json_response(
            {"error": "reward_not_awaiting_wallet"}, status=409,
            cache_control="no-store")

    recipient = str(row.get("recipient_name") or "").strip().lower()
    if not recipient or recipient == actor:
        return json_response(
            {"error": "invalid_reward_recipient"}, status=400,
            cache_control="no-store")
    _, rec = await _account_row(env, recipient)
    if not rec or (rec.get("status") or "active") != "active":
        return json_response(
            {"error": "no_such_account"}, status=404,
            cache_control="no-store")
    # They may have published one since the owner's page was drawn. Mailing a
    # request for something already on the profile is worse than sending
    # nothing: say so, and let the caller refresh into a payable row.
    if SOLANA_ADDRESS_RE.fullmatch(str(rec.get("solana") or "").strip()):
        return json_response(
            {"error": "wallet_already_published", "recipient": recipient},
            status=409, cache_control="no-store")

    now = int(Date.now())
    recipient_bi = (
        str(row.get("recipient_bi") or "").strip()
        or await blind_index(env, recipient))
    previous = await d1_first(
        env,
        "SELECT sent_at FROM reward_wallet_reminders WHERE recipient_bi=?",
        recipient_bi)
    # Keyed on the person, not the reward: one contributor can be owed a dozen
    # unpayable rewards at once, and a per-reward limit would let one Ledger
    # page fan out into a dozen emails about the same missing address.
    last_sent = _integer((previous or {}).get("sent_at"), minimum=0)
    if last_sent and now - last_sent < REWARD_WALLET_REMINDER_INTERVAL_MS:
        retry_ms = REWARD_WALLET_REMINDER_INTERVAL_MS - (now - last_sent)
        return json_response(
            {"error": "reminder_too_soon", "recipient": recipient,
             "lastSentAt": last_sent, "retryAfterMs": retry_ms},
            status=429,
            extra_headers={"retry-after": str(max(1, retry_ms // 1000))},
            cache_control="no-store")

    waiting = await d1_first(
        env,
        "SELECT COUNT(*) AS n, SUM(amount_lamports) AS lamports "
        "FROM contributor_reward_estimates "
        "WHERE recipient_bi=? AND status='pending_wallet' AND tx_signature=''",
        recipient_bi)
    waiting_count = max(1, _integer((waiting or {}).get("n"), minimum=0))
    waiting_lamports = _integer((waiting or {}).get("lamports"), minimum=0)

    # Claim the window before mailing, so a crash mid-send cannot double-send
    # and a mail provider that keeps failing cannot be hammered from the row.
    await d1_run(
        env,
        "INSERT INTO reward_wallet_reminders "
        "(recipient_bi,recipient_name,reward_id,requested_by,sent_at,"
        "send_count) VALUES (?,?,?,?,?,1) "
        "ON CONFLICT(recipient_bi) DO UPDATE SET "
        "recipient_name=excluded.recipient_name,"
        "reward_id=excluded.reward_id,"
        "requested_by=excluded.requested_by,"
        "sent_at=excluded.sent_at,"
        "send_count=reward_wallet_reminders.send_count+1",
        recipient_bi, recipient, reward_id, actor, now)

    email = str(rec.get("email") or "").strip()
    wants_email = (
        bool(email) and bool(rec.get("email_verified"))
        and rec.get("email_notifications") is not False
        and notification_email_enabled(rec, "pending_reward"))
    emailed = False
    if wants_email:
        subject, text, html = _wallet_reminder_email(
            actor, recipient, waiting_count, waiting_lamports)
        emailed = bool(await _send_email(
            env, email, subject, text, html,
            email_kind="reward_wallet_reminder"))
        if emailed:
            await _record_account_email(
                env, recipient, "reward_wallet_reminder", True)

    # The in-app ping goes out either way: it is the copy of this request the
    # recipient sees when they have no verified address to mail.
    await enqueue_notification(
        env, recipient, "pending_reward",
        "%s is waiting to pay you" % actor,
        body=("%s in contributor rewards needs a public payout address on "
              "your profile." % _sol_text(waiting_lamports)),
        href="/dashboard/settings",
        actor=actor, source="reward-wallet:" + reward_id,
        dedupe="reward-wallet:" + recipient, ts=now)
    await _audit_sensitive_action(
        env, actor, "reward.wallet_reminder", "contributor_reward", reward_id,
        "sent" if emailed else "notified",
        {"recipient": recipient, "emailed": bool(emailed),
         "pendingCount": waiting_count})

    return json_response({
        "ok": True,
        "emailed": emailed,
        "notified": True,
        # When this reminder was recorded, and when the one before it was
        # (0 for a first reminder), so the owner sees both after the click.
        "sentAt": now,
        "previousSentAt": last_sent,
        "recipient": recipient,
        "pendingCount": waiting_count,
        "pendingLamports": waiting_lamports,
        "nextReminderAt": now + REWARD_WALLET_REMINDER_INTERVAL_MS,
        "custody": "recipient-published-payout-address",
    }, cache_control="no-store")


def _usd_from_lamports(lamports, price_micros):
    lamports = max(0, _integer(lamports))
    price_micros = max(0, _integer(price_micros))
    if not lamports or not price_micros:
        return None, 0
    value_micros = (
        lamports * price_micros + LAMPORTS_PER_SOL // 2
    ) // LAMPORTS_PER_SOL
    whole, fraction = divmod(value_micros, USD_MICROS)
    return "%d.%06d" % (whole, fraction), value_micros


def _usd_text(value_micros):
    whole, fraction = divmod(max(0, _integer(value_micros)), USD_MICROS)
    return "%d.%06d" % (whole, fraction)


def _network(value):
    clean = str(value or "").strip().lower()
    if clean in ("mainnet", "mainnet-beta", ""):
        return "mainnet-beta"
    return clean if clean in ("devnet", "testnet") else "mainnet-beta"


def _explorer_url(signature, network):
    signature = str(signature or "").strip()
    if not signature:
        return ""
    suffix = ""
    network = _network(network)
    if network in ("devnet", "testnet"):
        suffix = "?cluster=" + network
    return "https://explorer.solana.com/tx/" + quote(signature, safe="") + suffix


def _wallet_link(address, role, network):
    address = str(address or "").strip()
    if not address:
        return None
    suffix = ""
    network = _network(network)
    if network in ("devnet", "testnet"):
        suffix = "?cluster=" + network
    return {
        "type": "wallet",
        "role": str(role or "wallet"),
        "key": address,
        "label": address,
        "url": (
            "https://explorer.solana.com/address/"
            + quote(address, safe="") + suffix
        ),
    }


def _user_link(name, role="participant", *, discord_username="",
               discord_display_name=""):
    name = str(name or "").strip().lower()
    if not name:
        return None
    link = {
        "type": "user",
        "role": role,
        "key": name,
        "label": "@" + name,
        "url": "/@" + quote(name, safe=""),
    }
    username = _discord_identity_text(discord_username, 80).lstrip("@")
    display_name = _discord_identity_text(discord_display_name, 100)
    if username:
        link["discordUsername"] = username
    if display_name:
        link["discordDisplayName"] = display_name
    return link


def _resource_link(
        kind, key, label, url, role="subject", title="", summary=""):
    key = str(key or "").strip()
    label = str(label or "").strip()
    if not key or not label:
        return None
    link = {
        "type": str(kind or "record"),
        "role": str(role or "subject"),
        "key": key,
        "label": label,
        "url": str(url or ""),
    }
    # The human name of the thing the money is about ("Crash on startup"),
    # kept beside the stable label rather than folded into it: readers sort
    # and scan by it, and the label is what every existing client renders.
    title = str(title or "").strip()[:RESOURCE_TITLE_MAX]
    if title:
        link["title"] = title
    summary = re.sub(r"\s+", " ", str(summary or "")).strip()
    if summary:
        link["summary"] = summary[:RESOURCE_SUMMARY_MAX]
    return link


def _links(*items):
    output = []
    seen = set()
    for item in items:
        if not isinstance(item, dict):
            continue
        marker = (item.get("type"), item.get("role"), item.get("key"))
        if marker in seen:
            continue
        seen.add(marker)
        output.append(item)
    return output


def _entry(
        entry_id, category, title, status, occurred_at, *,
        direction="record", network="mainnet-beta", signature="",
        lamports=0, links=None, source_id="", verification="recorded",
        currency="SOL", amount_minor=0, price_micros=0, note="",
        amount_known=True, recorded_usd_micros=0,
        recorded_price_micros=0, recorded_valued_at=0):
    lamports = max(0, _integer(lamports))
    amount_minor = max(0, _integer(amount_minor))
    currency = str(currency or "SOL").upper()
    amount_usd = None
    amount_usd_micros = 0
    valuation_basis = "not-applicable"
    amount_known = bool(amount_known)
    recorded_valued_at = max(0, _integer(recorded_valued_at))
    recorded_price_micros = max(0, _integer(recorded_price_micros))
    if currency == "SOL" and amount_known:
        if recorded_valued_at:
            amount_usd_micros = max(0, _integer(recorded_usd_micros))
            amount_usd = (
                _usd_text(amount_usd_micros)
                if recorded_price_micros else None
            )
            valuation_basis = (
                "recorded-at-payment-sol-usd" if amount_usd is not None
                else "payment-sol-usd-unavailable"
            )
        else:
            amount_usd, amount_usd_micros = _usd_from_lamports(
                lamports, price_micros)
            valuation_basis = (
                "live-sol-usd-spot-estimate" if amount_usd is not None
                else "sol-usd-estimate-unavailable"
            )
    elif currency == "USD":
        amount_usd_micros = amount_minor * 10_000
        amount_usd = "%d.%02d" % divmod(amount_minor, 100)
        valuation_basis = "recorded-fiat-amount"
    return {
        "id": str(entry_id or ""),
        "category": str(category or "record"),
        "title": str(title or "Financial record"),
        "status": str(status or "recorded"),
        "direction": str(direction or "record"),
        "verification": str(verification or "recorded"),
        "network": _network(network) if currency == "SOL" else "",
        "transactionId": str(signature or ""),
        "explorerUrl": _explorer_url(signature, network),
        "amountLamports": lamports,
        "amountSol": (
            _sol_text(lamports)
            if currency == "SOL" and amount_known else None
        ),
        "amountKnown": amount_known,
        "currency": currency,
        "amountMinor": amount_minor if currency != "SOL" else 0,
        "amountUsd": amount_usd,
        "amountUsdMicros": amount_usd_micros,
        "valuationBasis": valuation_basis,
        "valuationPriceMicros": (
            recorded_price_micros if recorded_valued_at else 0),
        "valuationObservedAt": recorded_valued_at,
        "occurredAt": max(0, _integer(occurred_at)),
        "sourceTable": SOURCE_TABLE_BY_CATEGORY.get(
            str(category or ""), ""),
        "sourceId": str(source_id or ""),
        "links": list(links or []),
        "note": str(note or ""),
    }


async def _sol_usd_quote():
    """Return a bounded, cached public spot quote, or an unavailable marker."""
    now = _integer(Date.now())
    if (
        _SOL_USD_QUOTE["priceMicros"] > 0
        and _SOL_USD_QUOTE["expiresAt"] > now
    ):
        return dict(_SOL_USD_QUOTE)
    price_micros = 0
    try:
        response = await js_fetch_with_timeout(
            SOL_USD_ENDPOINT,
            {
                "method": "GET",
                "headers": {"accept": "application/json"},
                "redirect": "error",
            },
            4,
        )
        if _integer(getattr(response, "status", 0)) == 200:
            body = json.loads(str(await response.text()))
            price = float(((body or {}).get("solana") or {}).get("usd") or 0)
            if 0 < price < 1_000_000:
                price_micros = int(round(price * USD_MICROS))
    except Exception:
        price_micros = 0
    if price_micros:
        _SOL_USD_QUOTE.update({
            "priceMicros": price_micros,
            "observedAt": now,
            "expiresAt": now + SOL_USD_QUOTE_TTL_MS,
        })
    return {
        "priceMicros": price_micros,
        "observedAt": now if price_micros else 0,
        "expiresAt": now + SOL_USD_QUOTE_TTL_MS if price_micros else 0,
    }


class _BackfillRpcEnvironment:
    """Narrow, network-specific view for the shared read-only RPC helper."""

    def __init__(self, env, network):
        self.SOLANA_NETWORK = _network(network)
        reward_network = _network(getattr(
            env, "COMMUNITY_REWARD_NETWORK", "mainnet-beta"))
        self.SOLANA_RPC_URL = (
            str(getattr(env, "COMMUNITY_REWARD_RPC_URL", "") or "")
            if self.SOLANA_NETWORK == reward_network else ""
        )


async def _backfill_rpc(env, network, method, params):
    return await _solana_rpc(
        _BackfillRpcEnvironment(env, network), method, params)


def _parsed_system_transfers(response, signature, observed_address):
    """Return successful System transfers involving one enrolled address."""
    if not isinstance(response, dict):
        return None
    result = response.get("result")
    if result is None:
        return []
    if not isinstance(result, dict):
        return []
    meta = result.get("meta")
    transaction = result.get("transaction")
    if (
        not isinstance(meta, dict)
        or meta.get("err") is not None
        or not isinstance(transaction, dict)
        or signature not in (transaction.get("signatures") or [])
    ):
        return []
    message = transaction.get("message")
    instructions = (
        message.get("instructions") if isinstance(message, dict) else None)
    if not isinstance(instructions, list):
        return []
    transfers = []
    transfer_by_pair = {}
    for index, instruction in enumerate(instructions):
        if not isinstance(instruction, dict):
            continue
        parsed = instruction.get("parsed")
        if (
            str(instruction.get("program") or "").lower() != "system"
            or not isinstance(parsed, dict)
            or parsed.get("type") != "transfer"
        ):
            continue
        info = parsed.get("info")
        if not isinstance(info, dict):
            continue
        source = str(info.get("source") or "").strip()
        destination = str(info.get("destination") or "").strip()
        lamports = _integer(info.get("lamports"), minimum=0)
        if (
            lamports <= 0
            or not SOLANA_ADDRESS_RE.fullmatch(source)
            or not SOLANA_ADDRESS_RE.fullmatch(destination)
            or observed_address not in (source, destination)
        ):
            continue
        pair = (source, destination)
        existing = transfer_by_pair.get(pair)
        if existing is not None:
            # Repeated transfers between the same pair in one transaction are
            # one financial movement for this ledger. Aggregating preserves
            # every lamport while bounding D1 rows by the transaction's public
            # account list rather than its instruction count.
            existing["lamports"] += lamports
            continue
        item = {
            "instructionIndex": index,
            "sourceAddress": source,
            "destinationAddress": destination,
            "lamports": lamports,
            "blockTime": _integer(result.get("blockTime"), minimum=0) * 1000,
        }
        transfer_by_pair[pair] = item
        transfers.append(item)
    return transfers


async def _seed_retained_bounty_sources(env, now):
    """Enroll retained bounty wallets without exposing repository names."""
    state = await d1_first(
        env, "SELECT v FROM schema_meta WHERE k=?", BACKFILL_BOUNTY_CURSOR_KEY)
    cursor = str((state or {}).get("v") or "")
    if cursor == "\uffff":
        return 0
    rows = await d1_all(
        env,
        "SELECT bounty_bi,data FROM issue_bounty WHERE bounty_bi>? "
        "ORDER BY bounty_bi ASC LIMIT ?",
        cursor, BACKFILL_BOUNTY_SEED_BATCH,
    )
    enrolled = 0
    for row in rows or []:
        bounty_bi = str(row.get("bounty_bi") or "")
        cursor = bounty_bi or cursor
        try:
            record = await decrypt_row(env, row.get("data"))
        except Exception:
            record = {}
        record = record if isinstance(record, dict) else {}
        address = str(record.get("address") or "").strip()
        owner = str(record.get("owner") or "").strip().lower()
        repo = str(record.get("repo") or "").strip().lower()
        if (
            not SOLANA_ADDRESS_RE.fullmatch(address)
            or not owner
            or not repo
        ):
            continue
        repo_bi = await blind_index(env, owner + "/" + repo)
        subject_kind = "pull" if record.get("kind") == "pr" else "issue"
        subject_number = _integer(record.get("number"), minimum=0)
        await d1_run(
            env,
            "INSERT INTO ledger_backfill_sources "
            "(source_id,source_kind,address,network,subject_repo_bi,"
            "subject_kind,subject_number,updated_at) "
            "VALUES (?,?,?,?,?,?,?,?) ON CONFLICT(network,address) DO UPDATE "
            "SET source_kind=excluded.source_kind,"
            "subject_repo_bi=excluded.subject_repo_bi,"
            "subject_kind=excluded.subject_kind,"
            "subject_number=excluded.subject_number",
            "legacy-bounty:" + bounty_bi, "legacy-bounty", address,
            "mainnet-beta", repo_bi, subject_kind, subject_number, now,
        )
        enrolled += 1
    next_cursor = "\uffff" if len(rows or []) < BACKFILL_BOUNTY_SEED_BATCH else cursor
    await d1_run(
        env,
        "INSERT INTO schema_meta(k,v) VALUES (?,?) "
        "ON CONFLICT(k) DO UPDATE SET v=excluded.v",
        BACKFILL_BOUNTY_CURSOR_KEY, next_cursor,
    )
    return enrolled


async def _seed_backfill_sources(env, now):
    await _seed_retained_bounty_sources(env, now)
    await d1_run(
        env,
        "INSERT OR IGNORE INTO ledger_backfill_sources "
        "(source_id,source_kind,address,network,updated_at) "
        "SELECT 'legacy-reconciliation:'||record_id,"
        "'legacy-'||source_table,public_address,"
        "CASE WHEN lower(network) IN ('devnet','testnet') "
        "THEN lower(network) ELSE 'mainnet-beta' END,? "
        "FROM legacy_custody_reconciliation WHERE public_address<>''",
        now,
    )
    world_network = _network(getattr(
        env, "COMMUNITY_REWARD_NETWORK",
        getattr(env, "SOLANA_NETWORK", "mainnet-beta"),
    ))
    await d1_run(
        env,
        "INSERT OR IGNORE INTO ledger_backfill_sources "
        "(source_id,source_kind,address,network,updated_at) "
        "SELECT 'world-deposit:'||purchase_id,'world-deposit',"
        "deposit_address,?,? FROM world_element_purchases "
        "WHERE deposit_address<>'' AND sweep_signature<>''",
        world_network, now,
    )
    pool_address = str(
        getattr(env, "COMMUNITY_REWARD_POOL_ADDRESS", "")
        or getattr(env, "TREASURY_SOLANA_ADDRESS", "")
        or ""
    ).strip()
    if SOLANA_ADDRESS_RE.fullmatch(pool_address):
        await d1_run(
            env,
            "INSERT OR IGNORE INTO ledger_backfill_sources "
            "(source_id,source_kind,address,network,updated_at) "
            "VALUES (?,?,?,?,?)",
            "community-pool:" + world_network + ":" + pool_address,
            "community-pool", pool_address, world_network, now,
        )


async def _scan_backfill_source(env, source, now):
    address = str(source.get("address") or "").strip()
    network = _network(source.get("network"))
    cursor = str(source.get("before_signature") or "").strip()
    if not SOLANA_ADDRESS_RE.fullmatch(address):
        await d1_run(
            env,
            "UPDATE ledger_backfill_sources SET completed=1,updated_at=? "
            "WHERE source_id=?",
            now, source.get("source_id"),
        )
        return 0
    options = {
        "limit": BACKFILL_SIGNATURE_BATCH,
        "commitment": "finalized",
    }
    if cursor:
        options["before"] = cursor
    response = await _backfill_rpc(
        env, network, "getSignaturesForAddress", [address, options])
    items = response.get("result") if isinstance(response, dict) else None
    if not isinstance(items, list):
        return 0
    if not items:
        await d1_run(
            env,
            "UPDATE ledger_backfill_sources SET completed=1,updated_at=? "
            "WHERE source_id=?",
            now, source.get("source_id"),
        )
        return 0
    last_processed = cursor
    processed = 0
    discovered = 0
    for item in items:
        signature = str(
            item.get("signature") if isinstance(item, dict) else ""
        ).strip()
        if not SOLANA_SIGNATURE_RE.fullmatch(signature):
            continue
        if isinstance(item, dict) and item.get("err") is not None:
            last_processed = signature
            processed += 1
            continue
        transaction = await _backfill_rpc(
            env, network, "getTransaction", [signature, {
                "encoding": "jsonParsed",
                "commitment": "finalized",
                "maxSupportedTransactionVersion": 0,
            }])
        transfers = _parsed_system_transfers(
            transaction, signature, address)
        if transfers is None:
            break
        transfer_rows = []
        for transfer in transfers:
            index = _integer(transfer.get("instructionIndex"), minimum=0)
            transfer_rows.append((
                "chain-backfill:%s:%s:%03d" % (network, signature, index),
                network,
                signature,
                index,
                transfer.get("blockTime"),
                transfer.get("sourceAddress"),
                transfer.get("destinationAddress"),
                transfer.get("lamports"),
                address,
                source.get("source_id"),
            ))
        for offset in range(0, len(transfer_rows), BACKFILL_TRANSFER_INSERT_BATCH):
            chunk = transfer_rows[
                offset:offset + BACKFILL_TRANSFER_INSERT_BATCH]
            await d1_run(
                env,
                "INSERT OR IGNORE INTO ledger_backfill_transactions "
                "(entry_id,network,signature,instruction_index,block_time,"
                "source_address,destination_address,amount_lamports,"
                "observed_address,discovered_from) "
                "VALUES " + ",".join(
                    "(?,?,?,?,?,?,?,?,?,?)" for _row in chunk),
                *(value for transfer_row in chunk for value in transfer_row),
            )
            discovered += len(chunk)
        outgoing = sum(
            _integer(transfer.get("lamports"), minimum=0)
            for transfer in transfers
            if transfer.get("sourceAddress") == address
        )
        if outgoing:
            await d1_run(
                env,
                "UPDATE world_element_purchases SET sweep_lamports=? "
                "WHERE sweep_lamports=0 AND sweep_signature=? "
                "AND deposit_address=?",
                outgoing, signature, address,
            )
        last_processed = signature
        processed += 1
    completed = int(
        processed == len(items) and len(items) < BACKFILL_SIGNATURE_BATCH)
    await d1_run(
        env,
        "UPDATE ledger_backfill_sources SET before_signature=?,completed=?,"
        "updated_at=? WHERE source_id=?",
        last_processed, completed, now, source.get("source_id"),
    )
    return discovered


async def ledger_backfill_cron(env):
    """Discover one bounded page of historical public SOL transfers."""
    await ensure_schema(env)
    now = _integer(Date.now(), minimum=0)
    await _seed_backfill_sources(env, now)
    source = await d1_first(
        env,
        "SELECT source_id,source_kind,address,network,before_signature "
        "FROM ledger_backfill_sources WHERE completed=0 "
        "ORDER BY updated_at ASC,source_id ASC LIMIT 1",
    )
    if not source:
        return 0
    return await _scan_backfill_source(env, source, now)


def _microjob_subject(envelope, job):
    envelope = envelope if isinstance(envelope, dict) else {}
    record = envelope.get("record")
    record = record if isinstance(record, dict) else {}
    owner = str(record.get("owner") or "").strip().lower()
    repo = str(record.get("repo") or "").strip().lower()
    repository = owner + "/" + repo if owner and repo else ""
    if job == "merged_pull":
        number = _integer(record.get("number"), minimum=0)
        title = str(
            record.get("title") or envelope.get("subject") or "").strip()
        if repository and number:
            return _resource_link(
                "pull",
                repository + "#" + str(number),
                repository + " pull #" + str(number),
                "/" + quote(owner, safe="") + "/" + quote(repo, safe="")
                + "/pulls/" + str(number),
                title=title,
            )
    if job == "issue_report":
        number = _integer(record.get("number"), minimum=0)
        title = str(record.get("title") or envelope.get("subject") or "").strip()
        if repository:
            suffix = " #" + str(number) if number else ""
            url = (
                "/" + quote(owner, safe="") + "/" + quote(repo, safe="")
                + "/issues" + ("/" + str(number) if number else "")
            )
            return _resource_link(
                "issue",
                repository + ("#" + str(number) if number else ":" + title),
                repository + " issue" + suffix,
                url,
                title=title,
            )
    if job == "referral_post":
        url = str(record.get("url") or envelope.get("subject") or "").strip()
        if url:
            return _resource_link("external-page", url, url, url)
    subject = str(envelope.get("subject") or "").strip()
    return _resource_link("work", subject, subject, "") if subject else None


async def _tip_entries(env, cutoff, before_id, limit, price_micros):
    # A contributor reward is paid by the same audited member-tip signer and
    # reported through the same notice endpoint, so one transfer lands in two
    # tables: the estimate that owns its state, and a tip notice whose whole
    # job is emailing the recipient. Publishing both listed one payment twice
    # — the second time as a "Member-to-member tip", which is wrong about why
    # the money moved and counts its SOL again in the page totals.
    #
    # The estimate is the canonical record of that transfer: it names the
    # issue or pull the money is for, and the relay verified the exact
    # finalized transfer before writing it. So its signature suppresses the
    # notice's own row, exactly as a canonical signature suppresses its
    # discovered copy in _backfill_entries. A cancelled estimate publishes
    # nothing, so it must not silence the notice either.
    suffix = (
        "sender.username AS sender_name,"
        "recipient.username AS recipient_name FROM tip_notice AS tips "
        "LEFT JOIN users AS sender ON sender.user_bi=tips.from_bi "
        "LEFT JOIN users AS recipient ON recipient.user_bi=tips.to_bi "
        "WHERE NOT EXISTS (SELECT 1 FROM contributor_reward_estimates "
        "WHERE tx_signature=tips.signature AND status<>'cancelled') "
        "AND (tips.ts<? OR (tips.ts=? AND "
        "('tip:'||tips.signature)<?)) "
        "ORDER BY tips.ts DESC,tips.signature DESC LIMIT ?"
    )
    try:
        rows = await d1_all(
            env,
            "SELECT tips.signature,tips.from_bi,tips.to_bi,tips.lamports,"
            "tips.network,tips.ts,tips.task_description,tips.output_url,"
            "tips.entered_currency,tips.entered_usd_micros,"
            "tips.sol_usd_price_micros,tips.price_observed_at," + suffix,
            cutoff, cutoff, before_id, limit,
        )
    except Exception:
        # A mixed-version relay can serve its pre-context tip rows while the
        # additive migration catches up. Those rows retain their old shape.
        rows = await d1_all(
            env,
            "SELECT tips.signature,tips.from_bi,tips.to_bi,tips.lamports,"
            "tips.network,tips.ts," + suffix,
            cutoff, cutoff, before_id, limit,
        )
    output = []
    for row in rows or []:
        signature = str(row.get("signature") or "")
        task = re.sub(
            r"\s+", " ", str(row.get("task_description") or "")
        ).strip()[:RESOURCE_SUMMARY_MAX]
        output_url = str(row.get("output_url") or "").strip()[:OUTPUT_URL_MAX]
        work_link = None
        if task:
            work_link = _resource_link(
                "work-product", "tip:" + signature,
                "Open work product" if output_url else "Task payment",
                output_url, title=task,
            )
        entered_currency = str(
            row.get("entered_currency") or "SOL").upper()
        recorded_at = (
            _integer(row.get("price_observed_at"), minimum=0)
            if entered_currency == "USD" else 0
        )
        output.append(_entry(
            "tip:" + signature,
            "member-tip",
            "Task payment" if task else "Member-to-member tip",
            "paid" if task else "reported-on-chain",
            row.get("ts"),
            direction="peer",
            network=row.get("network"),
            signature=signature,
            lamports=row.get("lamports"),
            price_micros=price_micros,
            recorded_usd_micros=(
                row.get("entered_usd_micros") if recorded_at else 0),
            recorded_price_micros=(
                row.get("sol_usd_price_micros") if recorded_at else 0),
            recorded_valued_at=recorded_at,
            source_id=signature,
            verification=(
                "finalized-exact-work-payment" if task else
                "sender-reported-public-receipt"),
            links=_links(
                _user_link(row.get("sender_name"), "sender"),
                _user_link(row.get("recipient_name"), "recipient"),
                work_link,
            ),
            note=(
                (task + " The exact self-custodial transfer was verified on "
                 "Solana.")
                if task else
                "The sender reported this self-custodial receipt. The linked "
                "Solana transaction is authoritative."
            ),
        ))
    return output


async def _contribution_entries(
        env, cutoff, before_id, limit, network, price_micros):
    rows = await d1_all(
        env,
        "SELECT contribution_id,mode,amount_lamports,status,created_at,"
        "tx_signature,source_address,confirmed_at,distribution_intent_id,"
        "CASE WHEN confirmed_at>0 THEN confirmed_at ELSE created_at END "
        "AS occurred_at FROM reward_contributions WHERE ("
        "CASE WHEN confirmed_at>0 THEN confirmed_at ELSE created_at END<? "
        "OR (CASE WHEN confirmed_at>0 THEN confirmed_at ELSE created_at END=? "
        "AND ('reward-contribution:'||contribution_id)<?)) "
        "ORDER BY occurred_at DESC,contribution_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in rows or []:
        contribution_id = str(row.get("contribution_id") or "")
        signature = str(row.get("tx_signature") or "")
        status = str(row.get("status") or "prepared")
        occurred = row.get("occurred_at") or row.get("created_at")
        output.append(_entry(
            "reward-contribution:" + contribution_id,
            "pool-contribution",
            (
                "All-node pool contribution"
                if row.get("mode") == "instant_all_nodes"
                else "Community reward-pool contribution"
            ),
            status,
            occurred,
            direction="inbound",
            network=network,
            signature=signature,
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            source_id=contribution_id,
            verification=(
                "finalized-exact-transfer" if row.get("confirmed_at")
                else "prepared-or-awaiting-finality"
            ),
            links=_links(
                _wallet_link(row.get("source_address"), "contributor", network),
                _resource_link(
                    "contribution", contribution_id,
                    "Contribution " + contribution_id[:12], "",
                ),
                _resource_link(
                    "signing-intent", row.get("distribution_intent_id"),
                    "Distribution intent "
                    + str(row.get("distribution_intent_id") or "")[:12], "",
                    role="distribution",
                ),
            ),
        ))
    return output


async def _reward_entries(
        env, cutoff, before_id, limit, network, price_micros):
    rows = await d1_all(
        env,
        "SELECT rewards.reward_id,rewards.recipient_name,"
        "rewards.source_address,rewards.amount_lamports,"
        "rewards.wallet_address,rewards.status,rewards.created_at,"
        "rewards.expires_at,rewards.intent_id,rewards.tx_signature,"
        "users.username AS recipient_username,"
        "intents.completed_at AS payout_completed_at "
        "FROM pending_rewards AS rewards "
        "LEFT JOIN users ON users.user_bi=rewards.recipient_bi "
        "LEFT JOIN chain_intents AS intents "
        "ON intents.intent_id=rewards.intent_id WHERE ("
        "CASE WHEN intents.completed_at>0 THEN intents.completed_at "
        "ELSE rewards.created_at END<? OR ("
        "CASE WHEN intents.completed_at>0 THEN intents.completed_at "
        "ELSE rewards.created_at END=? AND "
        "('pending-reward:'||rewards.reward_id)<?)) ORDER BY "
        "CASE WHEN intents.completed_at>0 THEN intents.completed_at "
        "ELSE rewards.created_at END DESC,rewards.reward_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in rows or []:
        reward_id = str(row.get("reward_id") or "")
        signature = str(row.get("tx_signature") or "")
        recipient = row.get("recipient_name") or row.get("recipient_username")
        status = str(row.get("status") or "pending_wallet")
        output.append(_entry(
            "pending-reward:" + reward_id,
            "pending-reward",
            "Account reward allocation",
            status,
            row.get("payout_completed_at") or row.get("created_at"),
            direction="outbound" if signature else "allocation",
            network=network,
            signature=signature,
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            source_id=reward_id,
            verification=(
                "finalized-exact-transfer" if status == "completed" and signature
                else "ledger-allocation-only"
            ),
            links=_links(
                _user_link(recipient, "recipient"),
                _wallet_link(row.get("source_address"), "source", network),
                _wallet_link(row.get("wallet_address"), "recipient", network),
                _resource_link(
                    "reward", reward_id, "Reward " + reward_id[:12], "",
                ),
                _resource_link(
                    "signing-intent", row.get("intent_id"),
                    "Signing intent " + str(row.get("intent_id") or "")[:12], "",
                    role="payout",
                ),
            ),
            note=(
                "Pending and expired rows are reservations only; funds remain "
                "in the source wallet unless a finalized signature is shown."
            ),
        ))
    return output


async def _chain_entries(env, cutoff, before_id, limit, price_micros):
    rows = await d1_all(
        env,
        "SELECT intent_id,kind,source_address,status,data,created_at,"
        "tx_signature,completed_at FROM chain_intents WHERE kind IN "
        "('community_reward_random','community_reward_instant_distribution',"
        "'world_element_store_mirror_split') AND ("
        "CASE WHEN completed_at>0 THEN completed_at ELSE created_at END<? "
        "OR (CASE WHEN completed_at>0 THEN completed_at ELSE created_at END=? "
        "AND ('chain-intent:'||intent_id||':')<?)) "
        "ORDER BY CASE WHEN completed_at>0 THEN completed_at ELSE created_at "
        "END DESC,intent_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in rows or []:
        try:
            plan = json.loads(str(row.get("data") or "{}"))
        except (TypeError, ValueError, json.JSONDecodeError):
            plan = {}
        plan = plan if isinstance(plan, dict) else {}
        intent_id = str(row.get("intent_id") or "")
        kind = str(row.get("kind") or "")
        signature = str(row.get("tx_signature") or "")
        network = _network(plan.get("network"))
        completed_at = row.get("completed_at") or row.get("created_at")
        transfers = plan.get("transfers")
        transfers = transfers if isinstance(transfers, list) else []
        for index, transfer in enumerate(transfers[:16]):
            if not isinstance(transfer, dict):
                continue
            node_id = str(transfer.get("nodeId") or "").strip()
            contribution_id = str(plan.get("contributionId") or "")
            purchase_id = str(plan.get("purchaseId") or "")
            element_id = str(plan.get("elementId") or "")
            if kind == "community_reward_random":
                title = "Randomized mirror reward"
                category = "mirror-reward"
            elif kind == "community_reward_instant_distribution":
                title = "All-node community distribution"
                category = "mirror-reward"
            else:
                title = "World purchase mirror share"
                category = "world-mirror-share"
            output.append(_entry(
                "chain-intent:%s:%03d" % (intent_id, index),
                category,
                title,
                row.get("status"),
                completed_at,
                direction="outbound",
                network=network,
                signature=signature,
                lamports=transfer.get("lamports"),
                price_micros=price_micros,
                source_id=intent_id,
                verification=(
                    "finalized-exact-transfer"
                    if row.get("status") == "completed" and signature
                    else "unsigned-public-plan"
                ),
                links=_links(
                    _wallet_link(row.get("source_address"), "source", network),
                    _wallet_link(transfer.get("address"), "recipient", network),
                    _resource_link(
                        "mirror-node", node_id, node_id, "/network",
                        role="recipient",
                    ),
                    _resource_link(
                        "contribution", contribution_id,
                        "Contribution " + contribution_id[:12], "",
                        role="funding-source",
                    ),
                    _resource_link(
                        "world-element-purchase", purchase_id,
                        "World purchase " + purchase_id[:12], "",
                    ),
                    _resource_link(
                        "world-element", element_id,
                        "World element " + element_id, "/world",
                    ),
                    _resource_link(
                        "signing-intent", intent_id,
                        "Signing intent " + intent_id[:12], "",
                        role="source-record",
                    ),
                ),
            ))
    return output


async def _microjob_entries(
        env, cutoff, before_id, limit, network, price_micros):
    rows = await d1_all(
        env,
        "SELECT claims.claim_id,claims.job,claims.status,"
        "claims.amount_lamports,claims.data,claims.created_at,"
        "claims.updated_at,claims.intent_id,claims.tx_signature,"
        "CASE WHEN claims.updated_at>0 THEN claims.updated_at "
        "ELSE claims.created_at END AS occurred_at,"
        "users.username AS account_name FROM microjob_claims AS claims "
        "LEFT JOIN users ON users.user_bi=claims.account_bi "
        "WHERE (CASE WHEN claims.updated_at>0 THEN claims.updated_at "
        "ELSE claims.created_at END<? OR ("
        "CASE WHEN claims.updated_at>0 THEN claims.updated_at "
        "ELSE claims.created_at END=? AND "
        "('microjob:'||claims.claim_id)<?)) "
        "ORDER BY occurred_at DESC,"
        "claims.claim_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in rows or []:
        try:
            envelope = await decrypt_row(env, row.get("data"))
        except Exception:
            envelope = {}
        envelope = envelope if isinstance(envelope, dict) else {}
        claim_id = str(row.get("claim_id") or "")
        job = str(row.get("job") or "")
        account = envelope.get("account") or row.get("account_name")
        subject = _microjob_subject(envelope, job)
        signature = str(row.get("tx_signature") or "")
        titles = {
            "issue_report": "Issue-report microjob",
            "merged_pull": "Merged-pull microjob",
            "referral_post": "Referral-post microjob",
        }
        output.append(_entry(
            "microjob:" + claim_id,
            "microjob-payout",
            titles.get(job, "Microjob payout"),
            row.get("status"),
            row.get("occurred_at") or row.get("created_at"),
            direction="outbound" if signature else "allocation",
            network=network,
            signature=signature,
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            source_id=claim_id,
            verification=(
                "finalized-exact-transfer"
                if row.get("status") == "paid" and signature
                else "verified-work-payout-state"
            ),
            links=_links(
                _user_link(account, "claimant"),
                subject,
                _resource_link(
                    "microjob-claim", claim_id,
                    "Claim " + claim_id[:12], "/microjobs",
                    role="source-record",
                ),
                _resource_link(
                    "signing-intent", row.get("intent_id"),
                    "Signing intent " + str(row.get("intent_id") or "")[:12], "",
                    role="payout",
                ),
            ),
            note=(
                "The claim and its issue, pull request, or public page share "
                "this exact transaction signature once paid."
            ),
        ))
    return output


async def _contributor_reward_entries(
        env, cutoff, before_id, limit, network, price_micros):
    # The payer's own account name comes from the tip notice its payout was
    # reported through (see _tip_entries): this row is the only one published
    # for that transfer, so it has to name both sides of it, not just the
    # recipient. An unpaid estimate has no notice and no payer yet.
    try:
        rows = await d1_all(
            env,
            "SELECT rewards.reward_id,rewards.recipient_bi,"
            "rewards.recipient_name,"
            "rewards.repo_owner,rewards.repo_name,rewards.source_kind,"
            "rewards.subject_number,rewards.subject_title,"
            "rewards.subject_summary,"
            "rewards.amount_lamports,rewards.condition,rewards.status,"
            "rewards.created_at,rewards.updated_at,rewards.source_address,"
            "rewards.destination_address,rewards.tx_signature,"
            "rewards.payment_usd_micros,rewards.payment_sol_usd_micros,"
            "rewards.payment_valued_at,"
            "payer.username AS payer_name,"
            "CASE WHEN rewards.paid_at>0 THEN rewards.paid_at "
            "WHEN rewards.submitted_at>0 THEN rewards.submitted_at "
            "WHEN rewards.updated_at>0 THEN rewards.updated_at "
            "ELSE rewards.created_at END AS occurred_at "
            "FROM contributor_reward_estimates AS rewards "
            "LEFT JOIN tip_notice AS notices "
            "ON notices.signature=rewards.tx_signature "
            "LEFT JOIN users AS payer ON payer.user_bi=notices.from_bi "
            "WHERE rewards.status<>'cancelled' AND ("
            "CASE WHEN rewards.paid_at>0 THEN rewards.paid_at "
            "WHEN rewards.submitted_at>0 THEN rewards.submitted_at "
            "WHEN rewards.updated_at>0 THEN rewards.updated_at "
            "ELSE rewards.created_at END<? OR ("
            "CASE WHEN rewards.paid_at>0 THEN rewards.paid_at "
            "WHEN rewards.submitted_at>0 THEN rewards.submitted_at "
            "WHEN rewards.updated_at>0 THEN rewards.updated_at "
            "ELSE rewards.created_at END=? AND "
            "('contributor-reward:'||rewards.reward_id)<?)) "
            "ORDER BY occurred_at DESC,rewards.reward_id DESC LIMIT ?",
            cutoff, cutoff, before_id, limit)
    except Exception:
        # Rolling deployments can briefly serve code before this additive
        # table exists in a colo. The rest of the public ledger remains useful
        # and the next request sees it after ensure_schema converges.
        rows = []
    profiles_by_recipient = await _contributor_reward_recipient_profiles(
        env, rows)
    output = []
    for row in rows or []:
        reward_id = str(row.get("reward_id") or "")
        owner = str(row.get("repo_owner") or "")
        repo = str(row.get("repo_name") or "")
        kind = "pull" if row.get("source_kind") == "pull" else "issue"
        number = _integer(row.get("subject_number"), minimum=1)
        plural = "pulls" if kind == "pull" else "issues"
        signature = str(row.get("tx_signature") or "")
        status = str(row.get("status") or "")
        destination = str(row.get("destination_address") or "")
        recipient_profile = profiles_by_recipient.get(
            str(row.get("recipient_bi") or ""), {})
        if status in ("pending_wallet", "ready"):
            destination = recipient_profile.get("wallet", "")
            status = "ready" if destination else "pending_wallet"
        output.append(_entry(
            "contributor-reward:" + reward_id,
            "contributor-reward",
            "Issue report reward" if kind == "issue" else "Pull request reward",
            status,
            row.get("occurred_at") or row.get("created_at"),
            direction="outbound" if signature else "allocation",
            network=network,
            signature=signature,
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            recorded_usd_micros=row.get("payment_usd_micros"),
            recorded_price_micros=row.get("payment_sol_usd_micros"),
            recorded_valued_at=row.get("payment_valued_at"),
            source_id=reward_id,
            verification=(
                "finalized-exact-transfer" if status == "paid"
                and signature else "owner-authored-estimate"),
            links=_links(
                _user_link(row.get("payer_name"), "sender"),
                _user_link(
                    row.get("recipient_name"), "recipient",
                    discord_username=(
                        recipient_profile.get("discordUsername", "")
                        if status in UNSETTLED_CONTRIBUTOR_REWARD_STATUSES
                        else ""),
                    discord_display_name=(
                        recipient_profile.get("discordDisplayName", "")
                        if status in UNSETTLED_CONTRIBUTOR_REWARD_STATUSES
                        else "")),
                _wallet_link(row.get("source_address"), "sender", network),
                _wallet_link(destination, "recipient", network),
                _resource_link(
                    kind, owner + "/" + repo + "#" + str(number),
                    owner + "/" + repo + " " + kind + " #" + str(number),
                    "/" + quote(owner, safe="") + "/"
                    + quote(repo, safe="") + "/" + plural + "/"
                    + str(number), role="subject",
                    title=row.get("subject_title"),
                    summary=row.get("subject_summary")),
                _resource_link(
                    "reward-estimate", reward_id,
                    "Reward estimate " + reward_id, "", role="source-record"),
            ),
            note=(
                "Payment is conditional on merge."
                if row.get("condition") == "merged"
                and status == "pending_merge"
                else "Paid directly from the repository owner's Qt wallet."
            ),
        ))
    return output


def _world_purchase_links(row, network):
    purchase_id = str(row.get("purchase_id") or "")
    element_id = str(row.get("element_id") or "")
    return _links(
        _user_link(row.get("account_name"), "buyer"),
        _wallet_link(row.get("source_address"), "buyer", network),
        _resource_link(
            "world-element", element_id,
            "World element " + element_id, "/world",
        ),
        _resource_link(
            "world-element-purchase", purchase_id,
            "Purchase " + purchase_id[:12], "",
            role="source-record",
        ),
        _resource_link(
            "signing-intent", row.get("distribution_intent_id"),
            "Distribution intent "
            + str(row.get("distribution_intent_id") or "")[:12], "",
            role="distribution",
        ),
    )


async def _world_purchase_entries(
        env, cutoff, before_id, limit, network, price_micros):
    purchase_rows = await d1_all(
        env,
        "SELECT purchases.purchase_id,purchases.element_id,"
        "purchases.amount_lamports,purchases.status,purchases.created_at,"
        "purchases.tx_signature,purchases.source_address,"
        "purchases.confirmed_at,purchases.distribution_intent_id,"
        "users.username AS account_name "
        "FROM world_element_purchases AS purchases "
        "LEFT JOIN users ON users.user_bi=purchases.account_bi "
        "WHERE (CASE WHEN purchases.confirmed_at>0 "
        "THEN purchases.confirmed_at ELSE purchases.created_at END<? OR ("
        "CASE WHEN purchases.confirmed_at>0 THEN purchases.confirmed_at "
        "ELSE purchases.created_at END=? AND "
        "('world-purchase:'||purchases.purchase_id)<?)) "
        "ORDER BY CASE WHEN purchases.confirmed_at>0 "
        "THEN purchases.confirmed_at ELSE purchases.created_at END DESC,"
        "purchases.purchase_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    sweep_rows = await d1_all(
        env,
        "SELECT purchases.purchase_id,purchases.element_id,"
        "purchases.source_address,purchases.distribution_intent_id,"
        "purchases.deposit_address,purchases.sweep_lamports,"
        "purchases.sweep_signature,purchases.sweep_at,"
        "users.username AS account_name "
        "FROM world_element_purchases AS purchases "
        "LEFT JOIN users ON users.user_bi=purchases.account_bi "
        "WHERE purchases.sweep_signature<>'' AND (purchases.sweep_at<? OR "
        "(purchases.sweep_at=? AND "
        "('world-purchase-sweep:'||purchases.purchase_id)<?)) "
        "ORDER BY purchases.sweep_at DESC,purchases.purchase_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in purchase_rows or []:
        purchase_id = str(row.get("purchase_id") or "")
        signature = str(row.get("tx_signature") or "")
        output.append(_entry(
            "world-purchase:" + purchase_id,
            "world-element-purchase",
            "World element purchase",
            row.get("status"),
            row.get("confirmed_at") or row.get("created_at"),
            direction="inbound",
            network=network,
            signature=signature,
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            source_id=purchase_id,
            verification=(
                "finalized-exact-transfer" if row.get("confirmed_at")
                else "prepared-or-awaiting-finality"
            ),
            links=_world_purchase_links(row, network),
        ))
    for row in sweep_rows or []:
        purchase_id = str(row.get("purchase_id") or "")
        sweep_signature = str(row.get("sweep_signature") or "")
        sweep_lamports = _integer(row.get("sweep_lamports"), minimum=0)
        output.append(_entry(
            "world-purchase-sweep:" + purchase_id,
            "world-element-sweep",
            "World purchase deposit sweep",
            "completed",
            row.get("sweep_at"),
            direction="outbound",
            network=network,
            signature=sweep_signature,
            lamports=sweep_lamports,
            amount_known=bool(sweep_lamports),
            price_micros=price_micros,
            source_id=purchase_id,
            verification="finalized-custodial-deposit-sweep",
            links=_links(
                *_world_purchase_links(row, network),
                _wallet_link(row.get("deposit_address"), "source", network),
            ),
            note=(
                "The exact swept amount is stored with new receipts. A "
                "legacy receipt is marked amount-unavailable rather than "
                "estimated from its required purchase amount."
            ),
        ))
    return output


async def _advertising_entries(env, cutoff, before_id, limit):
    rows = await d1_all(
        env,
        "SELECT revenue.entry_id,revenue.instance_id,revenue.proposal_id,"
        "revenue.ledger_class,revenue.amount_minor,revenue.currency,"
        "revenue.destination_snapshot,revenue.external_reference,"
        "revenue.recorded_at,users.username AS proposer_name "
        "FROM community_ad_revenue AS revenue "
        "LEFT JOIN community_ad_proposals AS proposals "
        "ON proposals.proposal_id=revenue.proposal_id "
        "LEFT JOIN users ON users.user_bi=proposals.proposer_bi "
        "WHERE (revenue.recorded_at<? OR (revenue.recorded_at=? AND "
        "('advertising-revenue:'||revenue.entry_id)<?)) "
        "ORDER BY revenue.recorded_at DESC,"
        "revenue.entry_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    for row in rows or []:
        entry_id = str(row.get("entry_id") or "")
        proposal_id = str(row.get("proposal_id") or "")
        external_reference = str(row.get("external_reference") or "")
        output.append(_entry(
            "advertising-revenue:" + entry_id,
            "advertising-revenue",
            "Community advertising receipt",
            "recorded",
            row.get("recorded_at"),
            direction="inbound",
            currency=row.get("currency"),
            amount_minor=row.get("amount_minor"),
            source_id=entry_id,
            verification="external-accounting-record-only",
            links=_links(
                _user_link(row.get("proposer_name"), "proposal-author"),
                _resource_link(
                    "ad-proposal", proposal_id,
                    "Advertising proposal " + proposal_id[:12],
                    "/world", role="subject",
                ),
                _resource_link(
                    "instance", row.get("instance_id"),
                    "Instance " + str(row.get("instance_id") or ""), "",
                    role="recipient",
                ),
                _resource_link(
                    "revenue-destination", row.get("destination_snapshot"),
                    str(row.get("destination_snapshot") or ""), "",
                    role="destination",
                ),
                _resource_link(
                    "external-accounting", external_reference,
                    external_reference, "", role="receipt",
                ),
            ),
            note=(
                "This is an external accounting receipt, separate from user "
                "funds and the Solana community reward pool."
            ),
        ))
    return output


async def _operating_expense_entries(env, cutoff, before_id, limit):
    """Project each external cost and its linked member reimbursement.

    Both rows come from one canonical source. The expense remains historical;
    the liability is outstanding only until the same source row records a
    reimbursement or waiver.
    """
    rows = await d1_all(
        env,
        "SELECT expense_id,vendor,description,paid_by_name,amount_minor,"
        "currency,original_amount_minor,original_currency,"
        "external_reference,incurred_at,recorded_at,"
        "reimbursement_status,reimbursed_at,reimbursement_reference "
        "FROM operating_expenses WHERE (incurred_at<? OR (incurred_at=? AND "
        "(('operating-expense:'||expense_id)<? OR "
        "('reimbursement-debt:'||expense_id)<?))) "
        "ORDER BY incurred_at DESC,expense_id DESC LIMIT ?",
        cutoff, cutoff, before_id, before_id, limit,
    )
    output = []
    for row in rows or []:
        expense_id = str(row.get("expense_id") or "")
        vendor = str(row.get("vendor") or "Vendor").strip()
        description = str(row.get("description") or "Operating expense").strip()
        paid_by = str(row.get("paid_by_name") or "").strip().lower()
        external_reference = str(row.get("external_reference") or "").strip()
        reimbursement_reference = str(
            row.get("reimbursement_reference") or "").strip()
        original_currency = str(row.get("original_currency") or "").upper()
        original_minor = _integer(
            row.get("original_amount_minor"), minimum=0)
        original_text = ""
        if original_currency and original_minor:
            original_text = "%s %d.%02d" % (
                original_currency, original_minor // 100, original_minor % 100)
        expense_links = _links(
            _user_link(paid_by, "paid-by"),
            _resource_link(
                "external-accounting", external_reference,
                "Invoice " + external_reference, "", role="receipt"),
        )
        note_parts = [
            description,
            ("Original charge " + original_text) if original_text else "",
        ]
        output.append(_entry(
            "operating-expense:" + expense_id,
            "operating-expense",
            vendor + " operating expense",
            "incurred",
            row.get("incurred_at"),
            direction="expense",
            currency=row.get("currency"),
            amount_minor=row.get("amount_minor"),
            source_id=expense_id,
            verification="external-invoice-record",
            links=expense_links,
            note=" · ".join(part for part in note_parts if part),
        ))

        reimbursement_status = str(
            row.get("reimbursement_status") or "outstanding")
        reimbursement_links = list(expense_links)
        reimbursement_links.extend(_links(
            _resource_link(
                "operating-expense", expense_id,
                "Expense " + expense_id, "", role="source-record"),
            _resource_link(
                "external-accounting", reimbursement_reference,
                "Reimbursement " + reimbursement_reference, "",
                role="settlement"),
        ))
        output.append(_entry(
            "reimbursement-debt:" + expense_id,
            "reimbursement-debt",
            "Reimbursement owed to @" + paid_by,
            reimbursement_status,
            (row.get("reimbursed_at")
             if reimbursement_status == "reimbursed"
             else row.get("incurred_at")),
            direction=(
                "settlement"
                if reimbursement_status in ("reimbursed", "waived")
                else "debt"
            ),
            currency=row.get("currency"),
            amount_minor=row.get("amount_minor"),
            source_id=expense_id,
            verification="linked-operating-expense-liability",
            links=_links(*reimbursement_links),
            note=(
                "ForkMesh owes this reimbursement for the linked "
                + vendor + " expense."
                if reimbursement_status == "outstanding"
                else "The linked expense reimbursement is "
                + reimbursement_status + "."
            ),
        ))
    return output


def _utc_month(timestamp_ms):
    moment = datetime.fromtimestamp(
        _integer(timestamp_ms, minimum=0) / 1000, timezone.utc)
    return moment.year * 12 + moment.month - 1, moment.day


def _month_start_ms(month):
    year, index = divmod(month, 12)
    return int(datetime(
        year, index + 1, 1, tzinfo=timezone.utc).timestamp() * 1000)


def _month_label(month):
    year, index = divmod(month, 12)
    return "%s %d" % (calendar.month_name[index + 1], year)


def _month_key(month):
    year, index = divmod(month, 12)
    return "%04d-%02d" % (year, index + 1)


def _month_day_ms(month, day):
    year, index = divmod(month, 12)
    last_day = calendar.monthrange(year, index + 1)[1]
    return (_month_start_ms(month)
            + (min(max(1, day), last_day) - 1) * DAY_MS + DAY_MS // 2)


async def _expected_expenses(env, now):
    current, _day = _utc_month(now)
    rows = await d1_all(
        env,
        "SELECT expense_id,vendor,paid_by_name,amount_minor,currency,"
        "incurred_at FROM operating_expenses WHERE incurred_at>=? "
        "ORDER BY incurred_at,expense_id",
        float(_month_start_ms(
            current - EXPECTED_EXPENSE_LOOKBACK_MONTHS + 1)),
    )
    vendors = {}
    for row in rows or []:
        vendor = str(row.get("vendor") or "").strip()
        month, day = _utc_month(row.get("incurred_at"))
        if not vendor or month > current:
            continue
        vendors.setdefault(vendor, {}).setdefault(month, []).append(
            (row, day))
    expected = []
    for vendor, months in vendors.items():
        recorded = sorted(months)
        if not any(month + 1 in months for month in recorded):
            continue
        if current - recorded[-1] >= EXPECTED_EXPENSE_LAPSE_MONTHS:
            continue
        for month in range(recorded[0] + 1, current + 1):
            if month in months:
                continue
            basis_month = max(seen for seen in recorded if seen < month)
            charges = months[basis_month]
            latest, day = charges[-1]
            currency = str(latest.get("currency") or "USD").upper()
            amount = sum(
                _integer(row.get("amount_minor"), minimum=0)
                for row, _charge_day in charges
                if str(row.get("currency") or "USD").upper() == currency
            )
            expected_at = _month_day_ms(month, day)
            expected.append({
                "id": "expected-expense:%s:%s" % (
                    re.sub(r"[^a-z0-9]+", "-", vendor.lower()).strip("-"),
                    _month_key(month)),
                "vendor": vendor,
                "month": _month_key(month),
                "monthLabel": _month_label(month),
                "basisMonthLabel": _month_label(basis_month),
                "basisExpenseId": str(latest.get("expense_id") or ""),
                "paidBy": str(
                    latest.get("paid_by_name") or "").strip().lower(),
                "status": (
                    "missing_receipt" if expected_at <= now else "expected"),
                "expectedAt": expected_at,
                "currency": currency,
                "amountMinor": amount,
            })
    expected.sort(key=lambda item: (item["expectedAt"], item["id"]))
    return expected


def _expected_expense_entry(item):
    missing = item["status"] == "missing_receipt"
    entry = _entry(
        item["id"],
        "expected-expense",
        (
            "%s receipt missing for %s" if missing
            else "%s charge expected in %s"
        ) % (item["vendor"], item["monthLabel"]),
        item["status"],
        item["expectedAt"],
        direction="forecast",
        currency=item["currency"],
        amount_minor=item["amountMinor"],
        source_id=item["basisExpenseId"],
        verification="estimated-from-monthly-history",
        links=_links(
            _user_link(item["paidBy"], "paid-by"),
            _resource_link(
                "operating-expense", item["basisExpenseId"],
                "Estimated from " + item["basisExpenseId"], "",
                role="estimate-basis"),
        ),
        note=(
            ("No receipt is recorded for this month yet; record it to "
             "replace this estimate. " if missing else "")
            + "Estimated from the " + item["basisMonthLabel"] + " "
            + item["vendor"] + " charges and not counted in expenses or "
            "debt until the receipt is recorded."
        ),
    )
    entry["valuationBasis"] = "monthly-history-estimate"
    return entry


async def _backfill_subject_link(env, row):
    blob = str(row.get("repo_data") or "")
    if not blob or not row.get("subject_repo_bi"):
        return None
    try:
        repository = await decrypt_row(env, blob)
    except Exception:
        return None
    repository = repository if isinstance(repository, dict) else {}
    if repository.get("visibility") != "public":
        return None
    owner = str(repository.get("owner") or "").strip().lower()
    repo = str(repository.get("name") or "").strip().lower()
    number = _integer(row.get("subject_number"), minimum=0)
    if not owner or not repo or not number:
        return None
    kind = "pull" if row.get("subject_kind") == "pull" else "issue"
    plural = "pulls" if kind == "pull" else "issues"
    return _resource_link(
        kind,
        owner + "/" + repo + "#" + str(number),
        owner + "/" + repo + " " + kind + " #" + str(number),
        "/" + quote(owner, safe="") + "/" + quote(repo, safe="")
        + "/" + plural + "/" + str(number),
    )


async def _backfill_entries(
        env, cutoff, before_id, limit, price_micros):
    rows = await d1_all(
        env,
        "SELECT history.entry_id,history.network,history.signature,"
        "history.instruction_index,history.block_time,"
        "history.source_address,history.destination_address,"
        "history.amount_lamports,history.observed_address,"
        "history.discovered_from,sources.source_kind,"
        "sources.subject_repo_bi,sources.subject_kind,"
        "sources.subject_number,repositories.data AS repo_data "
        "FROM ledger_backfill_transactions AS history "
        "JOIN ledger_backfill_sources AS sources "
        "ON sources.source_id=history.discovered_from "
        "LEFT JOIN repositories ON repositories.key_bi="
        "sources.subject_repo_bi AND repositories.is_private=0 "
        "WHERE (history.block_time<? OR (history.block_time=? "
        "AND history.entry_id<?)) "
        "AND NOT EXISTS (SELECT 1 FROM tip_notice "
        "WHERE signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM reward_contributions "
        "WHERE tx_signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM pending_rewards "
        "WHERE tx_signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM chain_intents "
        "WHERE tx_signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM microjob_claims "
        "WHERE tx_signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM contributor_reward_estimates "
        "WHERE tx_signature=history.signature) "
        "AND NOT EXISTS (SELECT 1 FROM world_element_purchases "
        "WHERE tx_signature=history.signature "
        "OR sweep_signature=history.signature) "
        "ORDER BY history.block_time DESC,history.entry_id DESC LIMIT ?",
        cutoff, cutoff, before_id, limit,
    )
    output = []
    titles = {
        "community-pool": "Historical community-pool transfer",
        "world-deposit": "Historical World deposit transfer",
        "legacy-bounty": "Historical bounty transfer",
        "legacy-issue_bounty": "Historical bounty transfer",
        "legacy-central_fund": "Historical community-fund transfer",
        "legacy-bounty_wallet": "Historical bounty-wallet transfer",
        "legacy-federated_signup": "Historical signup-wallet transfer",
        "legacy-users": "Historical account-wallet transfer",
    }
    for row in rows or []:
        source_kind = str(row.get("source_kind") or "historical-wallet")
        observed = str(row.get("observed_address") or "")
        source_address = str(row.get("source_address") or "")
        destination = str(row.get("destination_address") or "")
        direction = (
            "outbound" if source_address == observed
            else "inbound" if destination == observed else "record"
        )
        subject = await _backfill_subject_link(env, row)
        source_id = str(row.get("discovered_from") or "")
        source_label = source_kind.replace("_", " ").replace("-", " ")
        source_record = _resource_link(
            "historical-source", source_id,
            "Backfill source: " + source_label, "",
            role="discovered-from",
        )
        if source_id.startswith("world-deposit:"):
            purchase_id = source_id.split(":", 1)[1]
            source_record = _resource_link(
                "world-element-purchase", purchase_id,
                "World purchase " + purchase_id[:12], "/world",
                role="discovered-from",
            )
        output.append(_entry(
            row.get("entry_id"),
            "historical-chain-transfer",
            titles.get(source_kind, "Historical system-wallet transfer"),
            "finalized",
            row.get("block_time"),
            direction=direction,
            network=row.get("network"),
            signature=row.get("signature"),
            lamports=row.get("amount_lamports"),
            price_micros=price_micros,
            source_id=str(row.get("entry_id") or ""),
            verification="finalized-read-only-chain-backfill",
            links=_links(
                _wallet_link(source_address, "sender", row.get("network")),
                _wallet_link(destination, "recipient", row.get("network")),
                subject,
                source_record,
            ),
            note=(
                "Discovered through a bounded read-only history scan of a "
                "public system address. Canonical ForkMesh receipts with the "
                "same signature are shown by their owning record instead."
            ),
        ))
    return output


async def _backfill_status(env):
    row = await d1_first(
        env,
        "SELECT (SELECT COUNT(*) FROM ledger_backfill_sources) AS sources,"
        "(SELECT COUNT(*) FROM ledger_backfill_sources WHERE completed=1) "
        "AS completed_sources,"
        "(SELECT COUNT(*) FROM ledger_backfill_transactions) AS transfers",
    )
    sources = _integer((row or {}).get("sources"), minimum=0)
    completed = _integer((row or {}).get("completed_sources"), minimum=0)
    return {
        "sourceAddresses": sources,
        "completedSourceAddresses": completed,
        "discoveredTransfers": _integer(
            (row or {}).get("transfers"), minimum=0),
        "running": bool(sources and completed < sources),
        "scope": (
            "configured community pool, temporary World deposit addresses, "
            "and public legacy-custody reconciliation addresses"
        ),
        "memberWalletHistoriesScanned": False,
    }


# The two addresses this relay itself names in a money path: the pool every
# community reward round is distributed from, and the treasury a swept World
# deposit pays into. Both are public configuration, and reading them here goes
# through the same shared hourly display cache the rest of the site uses, so
# naming them at the top of the ledger costs the public RPC at most one read
# per address per hour. Personal wallets are never listed: the ones a reader
# owns are resolved by the client that already knows them.
SYSTEM_WALLETS = (
    ("community-reward-pool", "Global reward wallet",
     "Randomized community reward rounds are distributed from this address."),
    ("treasury", "Treasury wallet",
     "The treasury half of a swept World deposit is paid into this address."),
)


def _asked_for_refresh(params):
    """True when the reader pressed the page's own refresh control.

    The balance read behind it is still floored to one live check per address
    per minute by the shared display cache, so a held-down button cannot turn
    into a burst of public-RPC reads.
    """
    return str((params.get("refresh") or [""])[0] or "").strip().lower() in (
        "1", "true", "yes")


def _system_wallet_addresses(env):
    """Resolve both names exactly as the money paths that spend them do.

    A deployment that configured only one of the two keys resolves both names
    to it, so the ledger has to show the address that actually receives the
    money rather than an empty row.
    """
    pool = str(getattr(env, "COMMUNITY_REWARD_POOL_ADDRESS", "") or "").strip()
    treasury = str(getattr(env, "TREASURY_SOLANA_ADDRESS", "") or "").strip()
    return (pool or treasury, treasury or pool)


async def _system_wallets(env, refresh=False):
    """Public system wallet addresses with their last known public balance."""
    addresses = _system_wallet_addresses(env)
    if not any(SOLANA_ADDRESS_RE.fullmatch(one) for one in addresses):
        return []
    network = _network(getattr(
        env, "COMMUNITY_REWARD_NETWORK",
        getattr(env, "SOLANA_NETWORK", "mainnet-beta")))
    # Deliberately one targeted read per named address rather than the whole
    # stored-balance table: this module publishes two public system wallets and
    # has no business loading a page of member payout balances to do it. The
    # budget bounds a cold cache to one live read per address.
    budget = {"left": len(SYSTEM_WALLETS)}
    # Configuring one key for both is the common single-treasury setup. Saying
    # so keeps a reader from adding one balance to itself.
    shared = addresses[0] == addresses[1]
    wallets = []
    for (key, label, purpose), address in zip(SYSTEM_WALLETS, addresses):
        if not SOLANA_ADDRESS_RE.fullmatch(address):
            continue
        try:
            lamports, checked_at = await _display_balance_lamports(
                env, address, force=refresh, budget=budget,
                reader=_reward_balance_lamports)
        except Exception:
            lamports, checked_at = None, 0
        wallets.append({
            "key": key,
            "label": label,
            "purpose": purpose,
            "address": address,
            "network": network,
            "explorerUrl": (
                _wallet_link(address, key, network) or {}).get("url", ""),
            "balanceLamports": _integer(lamports, minimum=0),
            "balanceSol": _sol_text(lamports or 0),
            "balanceKnown": lamports is not None,
            "balanceCheckedAt": _integer(checked_at, minimum=0),
            "sharedAddress": shared,
            "custody": "external-owner-device-signer",
        })
    return wallets


def _after_cursor(entry, before_at, before_id):
    occurred = _integer(entry.get("occurredAt"))
    entry_id = str(entry.get("id") or "")
    return occurred < before_at or (
        occurred == before_at and entry_id < before_id
    )


async def ledger_handler(env, request):
    """Serve one public, stable page of normalized financial records."""
    if method_name(request) != "GET":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            extra_headers={"allow": "GET"},
        )
    await ensure_schema(env)
    params = parse_qs(urlparse(request.url).query)
    limit = _integer(
        (params.get("limit") or [PAGE_SIZE_DEFAULT])[0],
        default=PAGE_SIZE_DEFAULT,
        minimum=1,
        maximum=PAGE_SIZE_MAX,
    )
    # Python integers at the upper JavaScript-safe boundary cross Pyodide's
    # FFI as ``bigint`` values, which D1 prepared statements reject.  A float
    # remains an exact JavaScript Number for every millisecond timestamp this
    # cursor accepts, including the initial sentinel and subsequent pages.
    before_at = float(_integer(
        (params.get("beforeAt") or [9_007_199_254_740_991])[0],
        default=9_007_199_254_740_991,
        minimum=0,
        maximum=9_007_199_254_740_991,
    ))
    before_id = str((params.get("beforeId") or ["\uffff"])[0] or "\uffff")[:300]
    scan = limit + 1
    quote_data = await _sol_usd_quote()
    price_micros = _integer(quote_data.get("priceMicros"), minimum=0)
    network = _network(getattr(env, "SOLANA_NETWORK", "mainnet-beta"))

    entries = []
    # Keep these sequential: the Python Worker runtime cannot safely re-enter
    # one Pyodide request task through concurrent D1 awaits.
    entries.extend(await _tip_entries(
        env, before_at, before_id, scan, price_micros))
    entries.extend(await _contribution_entries(
        env, before_at, before_id, scan, network, price_micros))
    entries.extend(await _reward_entries(
        env, before_at, before_id, scan, network, price_micros))
    entries.extend(await _chain_entries(
        env, before_at, before_id, scan, price_micros))
    entries.extend(await _microjob_entries(
        env, before_at, before_id, scan, network, price_micros))
    entries.extend(await _contributor_reward_entries(
        env, before_at, before_id, scan, network, price_micros))
    entries.extend(await _world_purchase_entries(
        env, before_at, before_id, scan, network, price_micros))
    entries.extend(await _advertising_entries(
        env, before_at, before_id, scan))
    entries.extend(await _operating_expense_entries(
        env, before_at, before_id, scan))
    now = _integer(Date.now())
    expected_expenses = await _expected_expenses(env, now)
    entries.extend(
        _expected_expense_entry(item) for item in expected_expenses)
    entries.extend(await _backfill_entries(
        env, before_at, before_id, scan, price_micros))
    backfill_status = await _backfill_status(env)
    wallets = await _system_wallets(env, refresh=_asked_for_refresh(params))

    entries = [
        entry for entry in entries
        if _after_cursor(entry, before_at, before_id)
    ]
    entries.sort(
        key=lambda entry: (
            _integer(entry.get("occurredAt")), str(entry.get("id") or "")
        ),
        reverse=True,
    )
    has_more = len(entries) > limit
    page = entries[:limit]
    next_cursor = None
    if has_more and page:
        last = page[-1]
        next_cursor = {
            "beforeAt": _integer(last.get("occurredAt")),
            "beforeId": str(last.get("id") or ""),
        }
    settled = [entry for entry in page if entry.get("transactionId")]
    return json_response({
        "ok": True,
        "schemaVersion": 1,
        "observedAt": now,
        "network": network,
        "entries": page,
        "expectedExpenses": expected_expenses,
        "page": {
            "limit": limit,
            "count": len(page),
            "hasMore": has_more,
            "nextCursor": next_cursor,
        },
        "pageTotals": {
            "records": len(page),
            "withTransactionId": len(settled),
            "solLamports": sum(
                _integer(entry.get("amountLamports"))
                for entry in page if entry.get("currency") == "SOL"
            ),
        },
        "solUsd": {
            "price": (
                "%d.%06d" % divmod(price_micros, USD_MICROS)
                if price_micros else None
            ),
            "priceMicros": price_micros,
            "observedAt": _integer(quote_data.get("observedAt")),
            "source": "CoinGecko",
            "sourceUrl": SOL_USD_ENDPOINT,
            "basis": "live spot estimate, not transaction-time accounting",
            "available": bool(price_micros),
        },
        "wallets": wallets,
        "backfill": backfill_status,
        "scope": {
            "sources": [
                "member tips",
                "community-pool contributions and distributions",
                "pending account rewards",
                "microjob claims and payouts",
                "organization-owner contributor reward estimates and direct payouts",
                "World element purchases and distributions",
                "community advertising receipts",
                "historical transfers discovered from public system wallets",
            ],
            "projection": "canonical source-of-truth records",
            "privateRepositoryDetailsExposed": False,
            "payoutKeysStoredByWorker": False,
            "temporaryWorldDepositKeysExposed": False,
        },
    }, cache_control="public, max-age=30, stale-while-revalidate=120")
