from __future__ import annotations

import asyncio
import contextlib
import json
import time

POLL_SECONDS = 10.0
CLAIM_TIMEOUT_MS = 60_000
BATCH = 5
MAX_AGE_MS = 7 * 24 * 3600 * 1000
PAGED = ("alert", "lockdown", "test")
EMAILED = ("lockdown", "test")
EMAIL_INTERVAL_MS = 3600 * 1000
EMAIL_DAILY_CAP = 5
KIND = "security_alert"
BODY_MAX = 6000
DETAIL_VALUE_MAX = 4000
LOCKDOWN_SETTLE_MS = 15_000
SELECT_PENDING = (
    "SELECT id, node_id, created_ms, severity, detector, summary, detail "
    "FROM security_incidents WHERE delivered_ms IS NULL AND severity IN "
    "('alert', 'lockdown', 'test') AND created_ms >= ? "
    "AND (claimed_ms IS NULL OR claimed_ms < ?) "
    "ORDER BY created_ms LIMIT %d" % BATCH)


def now_ms():
    return time.time_ns() // 1_000_000


def _clean(value, limit):
    return "".join(c for c in str(value or "") if c.isprintable()
                   or c == "\n")[:limit]


def detail_of(row):
    try:
        detail = json.loads(row.get("detail") or "{}")
    except ValueError:
        detail = {"text": row.get("detail")}
    return detail if isinstance(detail, dict) else {"value": detail}


def settling(row, now):
    return (str(row.get("severity") or "") == "lockdown"
            and "lockdown" not in detail_of(row)
            and now - int(row.get("created_ms") or 0) < LOCKDOWN_SETTLE_MS)


def _value(value):
    return _clean(value if isinstance(value, str)
                  else json.dumps(value, sort_keys=True, default=str),
                  DETAIL_VALUE_MAX)


def _steps(steps):
    if not isinstance(steps, dict):
        return ["Lockdown steps: %s" % _value(steps)]
    lines = ["Lockdown steps:"]
    for step, outcome in steps.items():
        if isinstance(outcome, (list, tuple)) and len(outcome) == 2:
            lines.append("  %s: %s%s" % (
                _clean(step, 40), "ok" if outcome[0] else "FAILED",
                (" (%s)" % _clean(outcome[1], 300)) if outcome[1] else ""))
        else:
            lines.append("  %s: %s" % (_clean(step, 40), _value(outcome)))
    return lines


def message(row):
    severity = str(row.get("severity") or "alert")
    node = _clean(row.get("node_id"), 40)
    summary = _clean(row.get("summary"), 300)
    label = {"lockdown": "LOCKDOWN", "alert": "Security alert",
             "test": "IDS test (harmless)"}.get(severity, severity)
    if _clean(row.get("detector"), 60) == "autoscaler":
        label = "Fleet autoscaler"
    title = "%s on %s: %s" % (label, node, summary)
    detail = detail_of(row)
    lines = [summary, ""]
    if detail.get("verdict"):
        lines += ["Verdict: %s" % _value(detail["verdict"]), ""]
    lines += ["Node: %s" % node,
             "Severity: %s" % severity,
             "Detector: %s" % _clean(row.get("detector"), 60),
             "Incident: %s" % _clean(row.get("id"), 64),
             "At: %s UTC" % time.strftime(
                 "%Y-%m-%d %H:%M:%S", time.gmtime(
                     int(row.get("created_ms") or 0) / 1000))]
    for key in sorted(detail):
        if key in ("lockdown", "verdict"):
            continue
        value = detail[key]
        if isinstance(value, list) and value:
            lines.append("%s:" % _clean(key, 40))
            lines += ["  - %s" % _value(item) for item in value]
        elif isinstance(value, dict) and value:
            lines.append("%s:" % _clean(key, 40))
            lines += ["  %s: %s" % (_clean(k, 40), _value(value[k]))
                      for k in sorted(value)]
        else:
            lines.append("%s: %s" % (_clean(key, 40), _value(value)))
    if "lockdown" in detail:
        lines += [""] + _steps(detail["lockdown"])
    if severity == "lockdown":
        lines += ["", "The node sealed itself (DATA_KEY left memory), "
                  "blocked inbound traffic except SSH and stopped its "
                  "services. Investigate, then release it with "
                  "tools/next_ids.py release %s --execute (docs/IDS.md)."
                  % node]
    return title[:200], "\n".join(lines)[:BODY_MAX]


def _console_href(entry, env):
    rules = getattr(entry, "_admin_path_rules", None)
    return rules.notification_href(env) if rules is not None else "/admin"


class IncidentDispatcher:
    def __init__(self, runtime, membership, node_id, clock=None):
        self.runtime = runtime
        self.membership = membership
        self.node_id = node_id
        self.clock = clock or now_ms
        self._task = None
        self._logged = -1e9
        self.stats = {"rounds": 0, "delivered": 0, "claims_lost": 0,
                      "errors": 0}

    @property
    def rqlite(self):
        return self.membership.rqlite

    def ready(self):
        return (not self.runtime.sealed
                and getattr(self.membership, "hosting", False))

    async def pending(self, now):
        return await self.rqlite.query(
            SELECT_PENDING, now - MAX_AGE_MS, now - CLAIM_TIMEOUT_MS,
            level="weak")

    async def claim(self, incident_id, now):
        results = await self.rqlite.execute([[
            "UPDATE security_incidents SET claimed_by=?, claimed_ms=? "
            "WHERE id=? AND delivered_ms IS NULL AND (claimed_ms IS NULL "
            "OR claimed_ms < ?)", self.node_id, now, incident_id,
            now - CLAIM_TIMEOUT_MS]], transaction=False)
        return int((results or [{}])[0].get("rows_affected") or 0) == 1

    async def _execute(self, sql, *args):
        results = await self.rqlite.execute([[sql, *args]], transaction=False)
        return int((results or [{}])[0].get("rows_affected") or 0)

    def _buckets(self, row, now, budget):
        return ("ids-mail:%s:%s" % (_clean(row.get("node_id"), 40),
                                     _clean(row.get("detector"), 60)),
                "ids-mail:" + budget.day_key(now))

    async def email_gate(self, row, now):
        if str(row.get("severity") or "") not in EMAILED:
            return "severity"
        budget = self.runtime.entry.mail_budget
        pair, day = self._buckets(row, now, budget)
        if not await budget.reserve_interval(self._execute, pair,
                                             EMAIL_INTERVAL_MS, now):
            return "dedupe"
        if not await budget.reserve(self._execute, day, EMAIL_DAILY_CAP, now):
            return "cap"
        return "send"

    async def refund_email(self, row, now):
        pair, day = self._buckets(row, now, self.runtime.entry.mail_budget)
        await self._execute(
            "UPDATE mail_budget SET stamp_ms = ? WHERE bucket = ?",
            now - EMAIL_INTERVAL_MS, pair)
        await self._execute(
            "UPDATE mail_budget SET used = used - 1 WHERE bucket = ? "
            "AND used > 0", day)

    async def take_suppressed(self):
        return await self._execute(
            "UPDATE security_incidents SET email_state = 'digested' "
            "WHERE email_state IN ('dedupe', 'cap')")

    async def _admins(self, env, entry):
        rows = await entry.d1_all(
            env, "SELECT data FROM users WHERE is_admin=1 "
            "ORDER BY user_bi LIMIT 50")
        names = []
        for row in rows or []:
            try:
                record = await entry.decrypt_row(env, row.get("data", "")) \
                    or {}
            except Exception:
                continue
            name = str(record.get("name") or "").strip().lower()
            if name and entry.valid_node_name(name):
                names.append(name)
        return names

    async def deliver(self, row, now=None):
        entry = self.runtime.entry
        env = self.runtime.env
        now = self.clock() if now is None else now
        title, body = message(row)
        summary = {"by": self.node_id, "pings": 0, "emails": 0, "errors": [],
                   "email": "severity"}
        scope = getattr(self.runtime, "_db_scope", None)
        with (scope() if scope is not None else contextlib.nullcontext()):
            try:
                for admin in await self._admins(env, entry):
                    if await entry.enqueue_notification(
                            env, admin, KIND, title, body=body,
                            href=_console_href(entry, env), source="ids",
                            dedupe="ids:%s" % row.get("id"),
                            meta={"incident": str(row.get("id")),
                                  "node": str(row.get("node_id")),
                                  "detector": str(row.get("detector")),
                                  "severity": str(row.get("severity"))}):
                        summary["pings"] += 1
            except Exception as error:
                summary["errors"].append("pings:%s" % type(error).__name__)
            try:
                summary["email"] = await self.email_gate(row, now)
                if summary["email"] == "send":
                    await self._email(env, entry, row, title, body, summary,
                                      now)
                elif summary["email"] == "cap":
                    await self._log_suppressed(env, entry, title, body)
            except Exception as error:
                summary["email"] = "failed"
                summary["errors"].append("email:%s" % type(error).__name__)
        return summary

    async def _log_suppressed(self, env, entry, title, body=""):
        note = getattr(entry, "_mail_log_note", None)
        if note is None:
            return
        try:
            await note(env, "security", "", title, "suppressed",
                       error="IDS alert email daily cap",
                       body=body,
                       budget="%d of %d IDS alert emails sent today (UTC)" % (
                           EMAIL_DAILY_CAP, EMAIL_DAILY_CAP))
        except Exception:
            pass

    async def _email(self, env, entry, row, title, body, summary, now):
        summary["email"] = "failed"
        suppressed = await self.take_suppressed()
        if suppressed:
            summary["digest"] = suppressed
            body += ("\n\n%d more IDS alert%s suppressed (same node and "
                     "detector within an hour, or over %d emails a day); "
                     "they went to Pings only. tools/next_ids.py status "
                     "lists them." % (suppressed,
                                      "" if suppressed == 1 else "s",
                                      EMAIL_DAILY_CAP))
        try:
            emails = await entry._repository_monitor_admin_emails(env)
            for address in emails or []:
                if await entry._send_email(env, address, title, body,
                                           email_kind="security"):
                    summary["emails"] += 1
        finally:
            if summary["emails"]:
                summary["email"] = "sent"
            else:
                await self.refund_email(row, now)

    async def mark(self, incident_id, summary, now):
        await self.rqlite.execute([[
            "UPDATE security_incidents SET delivered_ms=?, delivery=?, "
            "email_state=? WHERE id=?", now,
            json.dumps(summary, sort_keys=True), summary.get("email"),
            incident_id]])

    async def round(self):
        if not self.ready():
            return 0
        self.stats["rounds"] += 1
        now = self.clock()
        delivered = 0
        for row in await self.pending(now) or []:
            if settling(row, now):
                continue
            if not await self.claim(row["id"], now):
                self.stats["claims_lost"] += 1
                continue
            summary = await self.deliver(row, now)
            if summary["pings"] or summary["emails"] or not summary["errors"]:
                await self.mark(row["id"], summary, self.clock())
                delivered += 1
                self.stats["delivered"] += 1
                print("ids: incident %s (%s, %s) delivered: %d pings, %d "
                      "emails%s" % (row["id"], row.get("node_id"),
                                    row.get("severity"), summary["pings"],
                                    summary["emails"],
                                    (" (errors %s)" % summary["errors"])
                                    if summary["errors"] else ""),
                      flush=True)
            else:
                # Every channel failed: leave it claimed; another node (or
                # this one) retries once the claim times out.
                print("ids: incident %s not delivered (%s); will retry"
                      % (row["id"], ",".join(summary["errors"])), flush=True)
        return delivered

    async def loop(self):
        await asyncio.sleep(3.0)
        while True:
            try:
                await self.round()
            except Exception as error:
                self.stats["errors"] += 1
                # e.g. before migration 0197: say so every 5 minutes only.
                if time.monotonic() - self._logged > 300:
                    self._logged = time.monotonic()
                    print("ids: alert round failed: %s" % type(
                        error).__name__, flush=True)
            await asyncio.sleep(POLL_SECONDS)

    def start(self):
        self._task = asyncio.get_running_loop().create_task(self.loop())

    async def stop(self):
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(BaseException):
                await self._task


def from_runtime(runtime):
    membership = getattr(getattr(runtime, "durable", None), "membership",
                         None)
    if membership is None or getattr(membership, "rqlite", None) is None:
        return None
    return IncidentDispatcher(runtime, membership, membership.node_id)
