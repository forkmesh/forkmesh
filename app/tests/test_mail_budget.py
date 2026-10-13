import asyncio
import importlib.util
import sqlite3
import types
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
T0 = 1_790_000_000_000
DAY = 24 * 3600 * 1000


def load():
    spec = importlib.util.spec_from_file_location(
        "mail_budget_under_test", ROOT / "src" / "mail_budget.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


mb = load()


class Db:
    def __init__(self):
        self.conn = sqlite3.connect(":memory:")
        self.conn.executescript(
            "CREATE TABLE security_incidents (id TEXT PRIMARY KEY);\n"
            + (ROOT / "migrations" / "0208_mail_budget.sql").read_text())

    async def execute(self, sql, *args):
        return self.conn.execute(sql, args).rowcount

    def used(self, bucket):
        row = self.conn.execute(
            "SELECT used FROM mail_budget WHERE bucket=?", (bucket,)).fetchone()
        return row[0] if row else 0


def spend(db, env, kind, now=T0):
    return asyncio.run(mb.spend(db.execute, env, kind, now))


def env(**values):
    return types.SimpleNamespace(**values)


def test_defaults_reserve_seventy_percent_for_account_mail():
    assert mb.daily_limit(env()) == 150
    assert mb.bulk_limit(env()) == 45
    assert mb.bulk_limit(env(MAIL_DAILY_LIMIT="200",
                             MAIL_ACCOUNT_RESERVE_PERCENT="50")) == 100
    assert mb.daily_limit(env(MAIL_DAILY_LIMIT="nope")) == 150


def test_bulk_mail_stops_at_its_share_and_account_mail_keeps_the_rest():
    db = Db()
    e = env(MAIL_DAILY_LIMIT="10")
    bulk = [spend(db, e, kind) for kind in
            ("security", "notifications", "status_alert", "package")]
    assert bulk == [True, True, True, False]
    assert all(spend(db, e, "verification") for _ in range(7))
    assert spend(db, e, "password_reset") is False
    day = mb.day_key(T0)
    assert db.used("mail:" + day) == 10
    assert db.used("mail-bulk:" + day) == 3


def test_account_mail_can_use_the_whole_day_but_not_more():
    db = Db()
    e = env(MAIL_DAILY_LIMIT="4")
    assert [spend(db, e, "account") for _ in range(5)] == [
        True, True, True, True, False]
    assert spend(db, e, "security") is False


def test_budget_resets_the_next_utc_day_and_prunes_old_buckets():
    db = Db()
    e = env(MAIL_DAILY_LIMIT="1")
    assert spend(db, e, "verification")
    assert not spend(db, e, "verification")
    assert spend(db, e, "verification", T0 + DAY)
    assert spend(db, e, "verification", T0 + 4 * DAY)
    buckets = {r[0] for r in db.conn.execute("SELECT bucket FROM mail_budget")}
    assert "mail:" + mb.day_key(T0) not in buckets


def test_reserve_interval_allows_one_per_window():
    db = Db()
    hour = 3600 * 1000

    def take(now):
        return asyncio.run(mb.reserve_interval(db.execute, "k", hour, now))
    assert take(T0)
    assert not take(T0 + hour - 1)
    assert take(T0 + hour)


def test_undeliverable_addresses():
    assert mb.deliverable("Ops@ForkMesh.com")
    assert not mb.deliverable("admin@next.forkmesh.com")
    assert not mb.deliverable("x@app.next.forkmesh.com")
    assert not mb.deliverable("a@example.com")
    assert not mb.deliverable("a@host.test")
    assert not mb.deliverable("a@localhost")
    assert not mb.deliverable("nobody")
    assert not mb.deliverable("a@b@c.com")
    assert not mb.deliverable("a@corp.example")
    assert not mb.deliverable("a@mail.acme.io", env(
        MAIL_NO_RECEIVE_DOMAINS="acme.io"))
    assert mb.deliverable("a@acme.io.uk", env(
        MAIL_NO_RECEIVE_DOMAINS="acme.io"))


def _between(start, end):
    return ENTRY[ENTRY.index(start):ENTRY.index(end)]


def test_every_platform_send_spends_the_shared_budget():
    sender = _between("async def _send_email", "# --- Account email activity")
    assert "mail_budget.deliverable(to_email, env)" in sender
    assert "await _mail_budget_spend(env, email_kind)" in sender
    assert sender.index("_mail_budget_spend(env") < sender.index(
        "js_fetch_with_timeout(")
    package = _between("async def send_mailtrap", "async def _send_package_smtp")
    assert '_mail_budget_spend(self.env, "package")' in package
    local = _between("async def _send_package_smtp", "async def _mail_log(")
    assert "_mail_budget_spend" not in local
    text_fn = _between("async def _mail_budget_text", "async def _mail_suppressed")
    assert text_fn.index("smtp_mail.enabled(env)") < text_fn.index(
        "mail_budget.usage(")
    spend_fn = _between("async def _mail_budget_spend", "async def _send_email")
    assert "return allowed" in spend_fn
    assert "mail_budget.is_account_kind(email_kind)" in spend_fn


def test_admin_alert_recipients_skip_service_and_undeliverable_accounts():
    admins = _between("async def _repository_monitor_admin_emails",
                      "def _sanitize_attention_log_text")
    assert 'record.get("service_account") is True' in admins
    assert "mail_budget.deliverable(address, env)" in admins


def test_provider_daily_limit_hold_reads_the_retry_time():
    detail = ('{"success":false,"errors":["Your account has reached its daily '
              'sending limit. Please upgrade your plan or retry in 2h31m5s."]}')
    assert mb.provider_limit_hold_ms(detail) == ((2 * 60 + 31) * 60 + 5) * 1000
    assert mb.provider_limit_hold_ms(
        "daily sending limit reached") == mb.DEFAULT_PROVIDER_HOLD_MS
    assert mb.provider_limit_hold_ms(
        "daily sending limit. retry in 90h") == mb.MAX_PROVIDER_HOLD_MS
    assert mb.provider_limit_hold_ms('{"errors":["Unauthorized"]}') == 0
    assert mb.provider_limit_hold_ms("") == 0


def test_provider_hold_stops_sends_until_it_ends():
    db = Db()
    held = lambda now: asyncio.run(mb.provider_held(db.execute, now))
    assert not held(T0)
    asyncio.run(mb.hold_provider(db.execute, T0 + 1000))
    assert held(T0)
    assert held(T0 + 999)
    assert not held(T0 + 1000)
    asyncio.run(mb.hold_provider(db.execute, T0 + 500))
    assert held(T0 + 999)
    asyncio.run(mb.hold_provider(db.execute, T0 + 5000))
    assert held(T0 + 4000)


def test_send_checks_the_provider_hold_and_holds_on_the_daily_limit():
    sender = _between("async def _send_email", "# --- Account email activity")
    assert sender.index("await _mail_provider_held(env)") < sender.index(
        "js_fetch_with_timeout(")
    assert "mail_budget.provider_limit_hold_ms(detail)" in sender
    rejected = sender[sender.index("hold_ms = mail_budget"):]
    assert rejected.index("_mail_provider_hold(env, hold_ms)") < rejected.index(
        "%s send rejected")


def test_usage_reports_the_day_budget_bulk_share_and_provider_hold():
    db = Db()
    e = env(MAIL_DAILY_LIMIT="10")

    async def query(sql, *args):
        cursor = db.conn.execute(sql, args)
        names = [column[0] for column in cursor.description]
        return [dict(zip(names, row)) for row in cursor.fetchall()]

    usage = lambda kind: asyncio.run(mb.usage(query, e, kind, T0))
    assert usage("verification") == "0 of 10 sent today (UTC)"
    for kind in ("security", "notifications", "status_alert", "account"):
        spend(db, e, kind)
    assert usage("verification") == "4 of 10 sent today (UTC)"
    assert usage("package") == (
        "4 of 10 sent today (UTC), bulk 3 of 3 (70% kept for account mail)")
    asyncio.run(mb.hold_provider(db.execute, T0 + 3600 * 1000))
    assert usage("account").endswith(
        ", Mailtrap daily limit hold until " + mb.time.strftime(
            "%Y-%m-%d %H:%M:%SZ", mb.time.gmtime(T0 / 1000 + 3600)))
