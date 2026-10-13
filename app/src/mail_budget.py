import re
import time

DEFAULT_DAILY_LIMIT = 150
DEFAULT_ACCOUNT_RESERVE_PERCENT = 70
DEFAULT_MARKETING_DAILY_LIMIT = 100
MARKETING_KINDS = frozenset({"marketing"})
ACCOUNT_KINDS = frozenset({
    "account", "verification", "password_reset", "password_change"})
NO_RECEIVE_DOMAINS = ("next.forkmesh.com",)
RESERVED_DOMAINS = ("example.com", "example.net", "example.org")
RESERVED_TLDS = ("invalid", "test", "example", "localhost", "local")
KEEP_MS = 3 * 24 * 3600 * 1000
PROVIDER_HOLD_BUCKET = "mail-provider-hold"
DEFAULT_PROVIDER_HOLD_MS = 3600 * 1000
MAX_PROVIDER_HOLD_MS = 24 * 3600 * 1000
_RETRY_IN = re.compile(r"retry in\s+(?:(\d+)h)?\s*(?:(\d+)m)?\s*(?:(\d+)s)?")


def day_key(now_ms):
    return time.strftime("%Y-%m-%d", time.gmtime(int(now_ms) / 1000))


def _int_setting(env, name, default, low, high):
    try:
        value = int(str(getattr(env, name, "") or "").strip())
    except (TypeError, ValueError):
        return default
    return max(low, min(high, value))


def daily_limit(env):
    return _int_setting(env, "MAIL_DAILY_LIMIT", DEFAULT_DAILY_LIMIT, 1,
                        1_000_000)


def account_reserve_percent(env):
    return _int_setting(env, "MAIL_ACCOUNT_RESERVE_PERCENT",
                        DEFAULT_ACCOUNT_RESERVE_PERCENT, 0, 100)


def bulk_limit(env):
    return daily_limit(env) * (100 - account_reserve_percent(env)) // 100


def marketing_limit(env):
    return _int_setting(env, "MAIL_MARKETING_DAILY_LIMIT",
                        DEFAULT_MARKETING_DAILY_LIMIT, 0, 1_000_000)


def is_marketing_kind(kind):
    return str(kind or "").strip().lower() in MARKETING_KINDS


async def spend_marketing(execute, env, now_ms):
    return await reserve(execute, "mail-marketing:" + day_key(now_ms),
                         marketing_limit(env), now_ms)


def is_account_kind(kind):
    return str(kind or "account").strip().lower() in ACCOUNT_KINDS


def no_receive_domains(env=None):
    extra = str(getattr(env, "MAIL_NO_RECEIVE_DOMAINS", "") or "")
    domains = set(NO_RECEIVE_DOMAINS) | set(RESERVED_DOMAINS)
    domains |= {d.strip().lower().strip(".") for d in extra.split(",")
                if d.strip().strip(".")}
    return domains


def deliverable(address, env=None):
    address = str(address or "").strip().lower()
    local, at, domain = address.rpartition("@")
    domain = domain.strip(".")
    if not at or not local or "@" in local or "." not in domain:
        return False
    if domain.rsplit(".", 1)[-1] in RESERVED_TLDS:
        return False
    for blocked in no_receive_domains(env):
        if domain == blocked or domain.endswith("." + blocked):
            return False
    return True


async def reserve(execute, bucket, cap, now_ms):
    if cap <= 0:
        return False
    created = await execute(
        "INSERT OR IGNORE INTO mail_budget (bucket, used, stamp_ms) "
        "VALUES (?, 0, ?)", bucket, now_ms)
    if created:
        await execute("DELETE FROM mail_budget WHERE stamp_ms < ?",
                      now_ms - KEEP_MS)
    return await execute(
        "UPDATE mail_budget SET used = used + 1, stamp_ms = ? "
        "WHERE bucket = ? AND used < ?", now_ms, bucket, cap) == 1


async def reserve_interval(execute, bucket, interval_ms, now_ms):
    await execute(
        "INSERT OR IGNORE INTO mail_budget (bucket, used, stamp_ms) "
        "VALUES (?, 0, ?)", bucket, now_ms - interval_ms)
    return await execute(
        "UPDATE mail_budget SET used = used + 1, stamp_ms = ? "
        "WHERE bucket = ? AND stamp_ms <= ?", now_ms, bucket,
        now_ms - interval_ms) == 1


async def spend(execute, env, kind, now_ms):
    day = day_key(now_ms)
    if not is_account_kind(kind):
        if not await reserve(execute, "mail-bulk:" + day, bulk_limit(env),
                             now_ms):
            return False
    return await reserve(execute, "mail:" + day, daily_limit(env), now_ms)



def provider_limit_hold_ms(detail):
    text = str(detail or "").lower()
    if "daily sending limit" not in text:
        return 0
    match = _RETRY_IN.search(text)
    if match and any(match.groups()):
        hours, minutes, seconds = (int(part or 0) for part in match.groups())
        wait_ms = ((hours * 60 + minutes) * 60 + seconds) * 1000
        if wait_ms > 0:
            return min(wait_ms, MAX_PROVIDER_HOLD_MS)
    return DEFAULT_PROVIDER_HOLD_MS


async def hold_provider(execute, until_ms):
    await execute(
        "INSERT OR IGNORE INTO mail_budget (bucket, used, stamp_ms) "
        "VALUES (?, 0, ?)", PROVIDER_HOLD_BUCKET, until_ms)
    await execute(
        "UPDATE mail_budget SET stamp_ms = ? WHERE bucket = ? AND stamp_ms < ?",
        until_ms, PROVIDER_HOLD_BUCKET, until_ms)


async def provider_held(execute, now_ms):
    return await execute(
        "UPDATE mail_budget SET used = used WHERE bucket = ? AND stamp_ms > ?",
        PROVIDER_HOLD_BUCKET, now_ms) == 1


def describe(env, kind, used, bulk_used, hold_until_ms, now_ms):
    text = "%d of %d sent today (UTC)" % (int(used or 0), daily_limit(env))
    if not is_account_kind(kind):
        text += ", bulk %d of %d (%d%% kept for account mail)" % (
            int(bulk_used or 0), bulk_limit(env),
            account_reserve_percent(env))
    if int(hold_until_ms or 0) > int(now_ms):
        text += ", Mailtrap daily limit hold until " + time.strftime(
            "%Y-%m-%d %H:%M:%SZ", time.gmtime(int(hold_until_ms) / 1000))
    return text


async def usage(query, env, kind, now_ms):
    day = day_key(now_ms)
    rows = await query(
        "SELECT bucket, used, stamp_ms FROM mail_budget "
        "WHERE bucket IN (?, ?, ?)",
        "mail:" + day, "mail-bulk:" + day, PROVIDER_HOLD_BUCKET)
    found = {str(row.get("bucket")): row for row in rows or []}
    return describe(
        env, kind,
        (found.get("mail:" + day) or {}).get("used"),
        (found.get("mail-bulk:" + day) or {}).get("used"),
        (found.get(PROVIDER_HOLD_BUCKET) or {}).get("stamp_ms"), now_ms)
