"""Admin-only view of what the maintenance cron is doing, and on what terms.

The scheduler itself must stay cheap, so nothing here runs on a tick: this
module is imported the first time an administrator asks for the report and
answers from the bounded ``cron_tick`` rows the dispatcher already writes plus
the declared schedule below. Keeping it out of entry.py also keeps its strings
out of every Pyodide isolate's startup memory (see status_monitoring for the
same pattern).
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the report implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# Every phase the scheduler can owe, with the cadence rule that decides when.
# ``period`` and ``offsets`` are the exact arithmetic _cron_due_phases applies
# (the phase is due when ``minute % period`` is one of ``offsets``), which is
# what makes "next due" a subtraction rather than a scan, and what lets a test
# prove the published schedule is the schedule that actually runs.
#
# The order here is the order the report presents: per-minute work first, then
# progressively slower cadences.
CRON_PHASE_CATALOG = (
    ("status-sample", 1, (0,), "status",
     "Sample every monitored system and write the minute row /status draws."),
    ("status-alert-pings", 1, (0,), "status",
     "Deliver ping alerts for monitors that changed state, plus due calendar "
     "reminders."),
    ("status-alert-email", 5, (0,), "status",
     "Deliver the email half of the alert ladder every five minutes."),
    ("mirror-health", 1, (0,), "mirrors",
     "Probe the oldest direct HTTPS mirror lease every minute so a multi-"
     "mirror fleet does not outrun the signed-proof freshness window."),
    ("mail-log", 1, (0,), "notifications",
     "Post queued outgoing-mail notes to the mail-log owner's private "
     "#email-log channel."),
    ("self-deploy-pings", 1, (0,), "notifications",
     "Ping the admins and the approver when the cores finish deploying a "
     "merge approved from a phone."),
    ("actions-schedule", 1, (0,), "platform",
     "Queue instance Actions workflows whose cron schedule is due."),
    ("snapshot-status", 3, (0,), "snapshots",
     "Republish the anonymous /api/status and /api/status/world bodies."),
    ("snapshot-mirrors", 3, (1,), "snapshots",
     "Republish the flagship mirror roster."),
    ("ap-outbox", 5, (0,), "federation",
     "Drain queued ActivityPub deliveries."),
    ("reward-chain", 5, (1,), "rewards",
     "Verify submitted on-chain reward intents."),
    ("reward-contributions", 5, (1,), "rewards",
     "Verify submitted contribution rewards."),
    ("reward-sweeps", 5, (1,), "rewards",
     "Sweep pending custodial element deposits."),
    ("direct-unread-email", 5, (1,), "notifications",
     "Send one reminder email to people whose direct messages have waited "
     "on the server for more than a day."),
    ("federation", 5, (2,), "federation",
     "Exchange catalog and node state with federated relays."),
    ("notification-digest", 5, (3,), "notifications",
     "Send account notification digests."),
    ("chat-digest", 5, (3,), "notifications",
     "Send the general chat digest."),
    ("channel-unread-email", 5, (3,), "notifications",
     "Send one reminder email to members of private channels with unread "
     "messages."),
    ("webhook-outbox", 5, (4,), "integrations",
     "Deliver queued repository webhooks to their owners' endpoints."),
    ("app-events", 5, (4,), "integrations",
     "Deliver queued ForkMesh Apps events to the apps' endpoints."),
    ("provider-mirror", 5, (4,), "integrations",
     "Sync repositories mirrored to GitHub or GitLab: send queued issues, "
     "comments and releases, and import new ones."),
    ("snapshot-catalog", 5, (2,), "snapshots",
     "Republish the anonymous repository catalog."),
    ("snapshot-network", 5, (4,), "snapshots",
     "Republish the network overview."),
    ("ledger-backfill", 5, (4,), "ledger",
     "Advance the ledger chain backfill cursor by one page."),
    ("probe-flagship", 1, (0,), "monitors",
     "Deep-probe the flagship repository render path."),
    ("probe-release-platforms", 1, (0,), "monitors",
     "Check the newest signed release carries a reachable build for each "
     "platform: Linux, Android, Windows and macOS, one status row each."),
    ("discord-bugs", 10, (4,), "intake",
     "Read the Discord bug channel and carry at most one report to an issue."),
    ("usage-budget", 10, (6,), "platform",
     "Sample the Cloudflare free-plan allowances and ping the thresholds."),
    ("online-sample", 10, (0,), "status",
     "Record the online-node history point."),
    ("ap-digest", 10, (8,), "federation",
     "Process queued ActivityPub digest batches."),
    ("marketing-email", 10, (9,), "notifications",
     "Send the next few emails of a marketing campaign to opted-in "
     "accounts."),
    ("stale-nodes", 15, (4,), "nodes",
     "Purge registered nodes that stopped reporting."),
    ("blocked-catalog", 15, (9,), "nodes",
     "Purge catalog entries for blocked repositories."),
    ("central-fund", 15, (14,), "rewards",
     "Distribute the central reward fund."),
    ("instance-health", 15, (11,), "nodes",
     "Ask listed instances for /readyz; hide offline ones and ping about "
     "deleting those offline for a day."),
    ("ap-profiles", 20, (12,), "federation",
     "Refresh one window of follower profiles."),
    ("status-compact", 60, (0, 1, 2, 3, 4, 30), "status",
     "Aggregate completed status minutes into hour and day buckets."),
    ("microjob-payouts", 60, (7,), "rewards",
     "Batch approved microjob claims into one unsigned payout plan."),
    ("referrer-prune", 60, (17,), "retention",
     "Age out the referrer board tail and blog read-dedupe rows."),
    ("feedback-email", 360, (22,), "notifications",
     "Send queued feedback emails."),
    ("chat-prune", 60, (37,), "retention",
     "Drop chat history past its retention window."),
    ("org-succession", 60, (42,), "nodes",
     "Warn organizations whose succession deadline is approaching."),
    ("account-device-prune", 60, (47,), "retention",
     "Purge stale account devices."),
    ("sensitive-cleanup", 60, (52,), "retention",
     "Delete expired sensitive audit records and purge organization tasks "
     "trashed more than 30 days ago."),
    ("expire-rewards", 60, (52,), "rewards",
     "Expire rewards nobody claimed."),
    ("world-media-cleanup", 60, (52,), "retention",
     "Delete orphaned world media records."),
    ("attribution-maintenance", 60, (27,), "retention",
     "Flag bounce sessions, purge 24h attribution IP digests and expired "
     "attribution rows."),
    ("instance-announce", 60, (32,), "nodes",
     "On a self-hosted instance that joined the forkmesh.com directory, "
     "announce it again when due so its name, logo and choices stay "
     "current."),
    ("badge-sweep", 60, (57,), "rewards",
     "Read pull and issue records of changed repositories and award "
     "achievement badges."),
    ("workflow-schedules", 5, (3,), "platform",
     "Run up to three workflows whose schedule trigger is due."),
    ("ai-review", 5, (0,), "platform",
     "Review at most one queued pull request with Cloudflare Workers AI "
     "when an administrator turned AI review on."),
    ("world-satellites", 180, (11,), "platform",
     "Refresh the world satellite snapshot."),
    ("attribution-clone-snapshot", 1440, (5,), "platform",
     "Snapshot per-repository clone totals for the acquisition dashboard."),
)
CRON_PHASE_DEDUPED_NOTE = "deduplicated in D1"
CRON_PHASE_REPLAY_NOTE = "safe to replay"


def _cadence_label(period, offsets):
    """Human cadence for one (period, offsets) rule."""
    count = len(offsets)
    if period == 1:
        return "every minute"
    if count == 1:
        if period == 60:
            return "hourly at :%02d" % offsets[0]
        if period % 60 == 0:
            return "every %d hours" % (period // 60)
        return "every %d minutes" % period
    return "%d minutes of every %d" % (count, period)


def _next_due_slot(minute_slot, period, offsets):
    """First slot after ``minute_slot`` at which this rule fires."""
    minute_slot = int(minute_slot)
    best = None
    for offset in offsets:
        ahead = (int(offset) - (minute_slot + 1)) % int(period)
        candidate = minute_slot + 1 + ahead
        best = candidate if best is None else min(best, candidate)
    return best


def _phase_rows(minute_slot, observed, chain, deduped, replay_safe):
    """The declared schedule, joined with what the retained ticks observed."""
    rows = []
    for phase, period, offsets, group, description in CRON_PHASE_CATALOG:
        seen = observed.get(phase) or {}
        notes = []
        if phase in chain:
            notes.append("status chain")
        if phase in deduped:
            notes.append(CRON_PHASE_DEDUPED_NOTE)
        if phase in replay_safe:
            notes.append(CRON_PHASE_REPLAY_NOTE)
        rows.append({
            "phase": phase,
            "group": group,
            "description": description,
            "cadence": _cadence_label(period, offsets),
            "periodMinutes": int(period),
            "runsPerDay": len(offsets) * (1440 // int(period)),
            "nextDueSlot": _next_due_slot(minute_slot, period, offsets),
            "notes": notes,
            "runs": int(seen.get("runs") or 0),
            "failures": int(seen.get("failures") or 0),
            "skips": int(seen.get("skips") or 0),
            "lastRunAt": int(seen.get("lastRunAt") or 0),
            "lastMs": int(seen.get("lastMs") or 0),
            "lastSubrequests": int(seen.get("lastSubrequests") or 0),
            "maxSubrequests": int(seen.get("maxSubrequests") or 0),
            "lastFailure": str(seen.get("lastFailure") or ""),
            "lastBatch": int(seen.get("lastBatch") or 0),
        })
    return rows


def _observe_tick(observed, detail, ts):
    """Fold one tick's detail into the per-phase running totals."""
    ran = detail.get("ran")
    if isinstance(ran, dict):
        for phase, measurement in ran.items():
            row = observed.setdefault(str(phase), {})
            row["runs"] = int(row.get("runs") or 0) + 1
            if not isinstance(measurement, dict):
                continue
            subrequests = int(measurement.get("sub") or 0)
            row["maxSubrequests"] = max(
                int(row.get("maxSubrequests") or 0), subrequests)
            if ts >= int(row.get("lastRunAt") or 0):
                row["lastRunAt"] = ts
                row["lastMs"] = int(measurement.get("ms") or 0)
                row["lastSubrequests"] = subrequests
                row["lastBatch"] = int(measurement.get("batch") or 0)
    elif isinstance(ran, list):
        for phase in ran:
            row = observed.setdefault(str(phase), {})
            row["runs"] = int(row.get("runs") or 0) + 1
    failed = detail.get("failed")
    names = (
        failed if isinstance(failed, (dict, list)) else ())
    for phase in names:
        row = observed.setdefault(str(phase), {})
        row["failures"] = int(row.get("failures") or 0) + 1
        message = failed.get(phase) if isinstance(failed, dict) else ""
        if ts >= int(row.get("lastFailureAt") or 0):
            row["lastFailureAt"] = ts
            row["lastFailure"] = str(message or "failed")
    for phase in detail.get("skipped") or ():
        row = observed.setdefault(str(phase), {})
        row["skips"] = int(row.get("skips") or 0) + 1


async def cron_activity_report(env, now, limit, settings=None):
    """Build the admin cron report: recent ticks, per-phase state, settings."""
    rows = await d1_all(
        env,
        "SELECT slot, ts, batches, phases, failures, subrequests, ms, data "
        "FROM cron_tick ORDER BY slot DESC LIMIT ?",
        max(1, int(limit)),
    ) or []
    now = int(now)
    minute_slot = now // CRON_RUNNER_INTERVAL_MS
    ticks = []
    observed = {}
    window_start = now
    batches_total = 0
    phases_total = 0
    failed_ticks = 0
    subrequests_total = 0
    for row in rows:
        try:
            detail = json.loads(str(row.get("data") or "{}"))
        except Exception:
            detail = {}
        detail = detail if isinstance(detail, dict) else {}
        ts = int(row.get("ts") or 0)
        failures = int(row.get("failures") or 0)
        batches = int(row.get("batches") or 0)
        phases = int(row.get("phases") or 0)
        subrequests = int(row.get("subrequests") or 0)
        _observe_tick(observed, detail, ts)
        batches_total += batches
        phases_total += phases
        subrequests_total += subrequests
        failed_ticks += 1 if failures else 0
        window_start = min(window_start, ts) if ts else window_start
        ticks.append({
            "slot": int(row.get("slot") or 0),
            "ts": ts,
            "batches": batches,
            "phases": phases,
            "failures": failures,
            "subrequests": subrequests,
            "ms": int(row.get("ms") or 0),
            "ran": sorted(detail.get("ran") or ()),
            "failed": sorted(detail.get("failed") or ()),
            "skipped": sorted(detail.get("skipped") or ()),
            "detail": detail.get("ran") if isinstance(
                detail.get("ran"), dict) else {},
            "failureDetail": detail.get("failed") if isinstance(
                detail.get("failed"), dict) else {},
        })
    observed_ticks = len(ticks)
    last_tick_at = ticks[0]["ts"] if ticks else 0
    return {
        "ok": True,
        "sampledAt": now,
        "slot": minute_slot,
        "intervalMs": CRON_RUNNER_INTERVAL_MS,
        "trigger": {
            "cron": "* * * * *",
            "intervalMs": CRON_RUNNER_INTERVAL_MS,
            "batchObjects": CRON_BATCH_MAX_OBJECTS,
            "subrequestCap": CRON_BATCH_SUBREQUEST_CAP,
            "queryLimit": CRON_PHASE_D1_QUERY_LIMIT,
            "reserveHeadroom": CRON_BATCH_RESERVE_HEADROOM,
            "watchdogGraceMs": CRON_WATCHDOG_GRACE_MS,
            "watchdogRetryMs": CRON_WATCHDOG_RETRY_MS,
            "retainMs": CRON_TICK_RETAIN_MS,
            "coordinator": CRON_RUNNER_NAME,
            "phaseCount": len(CRON_PHASE_CATALOG),
        },
        "summary": {
            "ticks": observed_ticks,
            "failedTicks": failed_ticks,
            "batches": batches_total,
            "phases": phases_total,
            "subrequests": subrequests_total,
            "windowStart": window_start if observed_ticks else 0,
            "lastTickAt": last_tick_at,
            "gapMs": max(0, now - last_tick_at) if last_tick_at else 0,
            "objectsPerTick": (
                round(batches_total / observed_ticks, 2)
                if observed_ticks else 0.0),
            "phasesPerTick": (
                round(phases_total / observed_ticks, 2)
                if observed_ticks else 0.0),
            # What the same phases would have cost as one Durable Object
            # invocation each, which is what this loop did before batching.
            "unbatchedObjectsPerTick": (
                round((phases_total + observed_ticks) / observed_ticks, 2)
                if observed_ticks else 0.0),
        },
        "phases": _phase_rows(
            minute_slot, observed, frozenset(CRON_STATUS_CHAIN),
            CRON_PHASE_D1_DEDUPED, CRON_PHASE_REPLAY_SAFE),
        "ticks": ticks,
        "settings": cron_settings_rows(settings),
    }


def cron_settings_rows(settings):
    """The runtime switches that decide what a tick actually delivers."""
    settings = settings if isinstance(settings, dict) else {}
    rows = [{
        "key": "trigger",
        "label": "Cron Trigger",
        "value": "* * * * *",
        "detail": "Worker-owned, one invocation a minute.",
    }, {
        "key": "batching",
        "label": "Phase batching",
        "value": "%d phases per %d subrequests" % (
            CRON_BATCH_MAX_PHASES, CRON_BATCH_SUBREQUEST_CAP),
        "detail": (
            "Each batch object runs the longest prefix of the due phases its "
            "measured subrequest reserves fit, then hands the rest to the "
            "next object."),
    }, {
        "key": "watchdog",
        "label": "Completion watchdog",
        "value": "%d minute grace" % (CRON_WATCHDOG_GRACE_MS // 60000),
        "detail": (
            "An independent alarm object emails admins when no tick reports "
            "completion inside the grace window."),
    }, {
        "key": "retention",
        "label": "Tick history",
        "value": "%d hours" % (CRON_TICK_RETAIN_MS // 3600000),
        "detail": "One D1 row per tick, swept twice an hour.",
    }]
    for system_id, label in STATUS_ALERT_ADMIN_SYSTEMS:
        rows.append({
            "key": "alerts:" + system_id,
            "label": "Alerts · " + label,
            "value": ", ".join([
                channel for channel, enabled in (
                    ("ping", _status_monitor_alert_setting(
                        settings, system_id, "pings")),
                    ("email", _status_monitor_alert_setting(
                        settings, system_id, "emails")),
                    ("continual", _status_monitor_alert_setting(
                        settings, system_id, "continual")),
                ) if enabled
            ]) or "off",
            "detail": (
                "Delivery channels the status-alert phases use for this "
                "monitor."),
        })
    return rows
