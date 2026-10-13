"""Ping settings (issue #347), loaded on demand.

Per-kind on/off switches, daily quiet hours, the kind catalog every settings
page renders, the one wire shape the Pings settings page reads and writes, and
the administrators' /status monitor alert matrix that used to live on the admin
console. Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for it on every isolate; entry.py exports these names
through a _LazyModule proxy.
"""

import marketing_email


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# Ping settings (issue #347): every kind in NOTIFICATION_KINDS can be switched
# off, and a daily "quiet hours" window mutes delivery without touching what is
# filed. A switched-off kind is never filed at all — no inbox row, no bell
# count, no card, no mail, no phone banner — which is what "turn it off" means
# to the person who asked. Quiet hours behave like the pause: the ping is still
# filed, so the inbox holds it for the morning, but nothing buzzes.
#
# The catalog is what every settings page renders, grouped the way a person
# thinks about their pings rather than the way the Worker raises them. Web and
# mobile read it from the account payload, so a kind added here reaches their
# settings pages without a client release; the desktop keeps its own copy
# (tested against this one). "admin" rows are shown to administrators only.
PING_KIND_GROUPS = (
    ("conversations", "Conversations"),
    ("repositories", "Repositories"),
    ("organizations", "Organizations"),
    ("rewards", "Money & rewards"),
    ("devices", "Devices & apps"),
    ("admin", "Administration"),
)
PING_KIND_CATALOG = (
    ("mention", "conversations", "Mentions"),
    ("subscribed", "conversations", "Activity on things you follow"),
    ("chat_webhook", "conversations", "Chat webhook posts"),
    ("channel_unread", "conversations", "Unread channel messages"),
    ("calendar_invitation", "conversations", "Calendar invitations"),
    ("calendar_reminder", "conversations", "Calendar reminders"),
    ("calendar_rsvp", "conversations", "Calendar replies"),
    ("issue_opened", "repositories", "New issues"),
    ("pull_submitted", "repositories", "New pull requests"),
    ("issue_assigned", "repositories", "Issue assignments"),
    ("pending_inbox", "repositories", "Inbox updates"),
    ("code_push", "repositories", "Code pushes"),
    ("repo_activity", "repositories", "Merged pull requests"),
    ("release_published", "repositories", "Releases"),
    ("repo_shared", "repositories", "Repositories shared with you"),
    ("mirror_request", "repositories", "Mirror requests"),
    ("repository_hosted", "repositories", "Imports finished hosting"),
    ("package_review", "repositories", "Reviews of your Market packages"),
    ("org_succession", "organizations", "Organization succession"),
    ("organization_task_update", "organizations", "Tasks"),
    ("bounty_funded", "rewards", "Bounties funded"),
    ("bounty_paid", "rewards", "Bounties paid"),
    ("pending_reward", "rewards", "Pending rewards"),
    ("credits_refilled", "rewards", "Credits refilled"),
    ("tip_sent", "rewards", "Tips"),
    ("hackathon_squad", "rewards", "Hackathon squad progress"),
    ("hackathon_reward", "rewards", "Hackathon rewards"),
    ("host_online", "devices", "Host came online"),
    ("host_offline", "devices", "Host went offline"),
    ("device_added", "devices", "New device signed in"),
    ("timer_finished", "devices", "Timers and alarms"),
    ("desktop_ping", "devices", "Pings from your desktop"),
    ("plugin_activity", "devices", "App package activity"),
    ("operational_alert", "admin", "Operational alerts"),
    ("error_group", "admin", "New error groups"),
    ("account_email_sent", "admin", "Account emails sent"),
    ("security_alert", "admin", "Security alerts (intrusion detection)"),
)
PING_QUIET_DAYS = (0, 1, 2, 3, 4, 5, 6)  # Monday = 0, like datetime.weekday()
PING_QUIET_DEFAULT_START = 22 * 60
PING_QUIET_DEFAULT_END = 7 * 60
# Per-isolate memo of the recipient's delivery settings, read on every
# enqueue_notification. A fan-out to one thread's subscribers would otherwise
# read each account once per ping; a switch flipped on another isolate reaches
# this one within the TTL, and a save on this isolate forgets its entry.
_PING_SETTINGS_MEMO = {}
PING_SETTINGS_MEMO_TTL_MS = 60 * 1000
PING_SETTINGS_MEMO_MAX = 512


def ping_kind_catalog(is_admin=False):
    """The settings-page rows, grouped; admin rows only for administrators."""
    groups = []
    for group_key, group_label in PING_KIND_GROUPS:
        if group_key == "admin" and not is_admin:
            continue
        kinds = [
            {"kind": kind, "label": label,
             "email": kind in NOTIFICATION_EMAIL_DEFAULTS}
            for kind, group, label in PING_KIND_CATALOG
            if group == group_key and kind in NOTIFICATION_KINDS
        ]
        if kinds:
            groups.append({"key": group_key, "label": group_label,
                           "kinds": kinds})
    return groups


def ping_muted_kinds(rec):
    """Sorted list of the ping kinds this account switched off."""
    raw = (rec or {}).get("ping_muted_kinds") if isinstance(rec, dict) else None
    if not isinstance(raw, (list, tuple)):
        return []
    return sorted({clean_string(kind, 40) for kind in raw
                   if clean_string(kind, 40) in NOTIFICATION_KINDS})


def _apply_ping_muted_kinds(rec, raw):
    """Store the switched-off kinds; True when the record actually changed.

    Accepts either a list of muted kinds or a {kind: enabled} map, so a client
    can send just the switch it flipped. Unknown kinds are dropped, and an
    empty set is stored as absence like every other untouched default.
    """
    muted = set(ping_muted_kinds(rec))
    if isinstance(raw, dict):
        for kind, enabled in raw.items():
            kind = clean_string(kind, 40)
            if kind not in NOTIFICATION_KINDS:
                continue
            if enabled:
                muted.discard(kind)
            else:
                muted.add(kind)
    elif isinstance(raw, (list, tuple)):
        muted = {clean_string(kind, 40) for kind in raw
                 if clean_string(kind, 40) in NOTIFICATION_KINDS}
    else:
        return False
    wanted = sorted(muted)
    if ping_muted_kinds(rec) == wanted and (
            bool(wanted) == ("ping_muted_kinds" in rec)):
        return False
    if wanted:
        rec["ping_muted_kinds"] = wanted
    else:
        rec.pop("ping_muted_kinds", None)
    _forget_ping_settings(rec)
    return True


def _quiet_minute(value, default):
    try:
        minute = int(value)
    except (TypeError, ValueError):
        return default
    return minute if 0 <= minute < 24 * 60 else default


def ping_quiet_hours(rec):
    """This account's daily quiet-hours window, always fully populated.

    start/end are minutes after local midnight; a window whose end is at or
    before its start runs across midnight (22:00 -> 07:00). days names the
    weekdays a window may START on, Monday = 0. timezone is the IANA zone the
    minutes are in, falling back to the profile timezone, then UTC.
    """
    raw = (rec or {}).get("notification_quiet_hours") if isinstance(
        rec, dict) else None
    raw = raw if isinstance(raw, dict) else {}
    days = raw.get("days")
    if isinstance(days, (list, tuple)):
        clean_days = []
        for day in days:
            try:
                day = int(day)
            except (TypeError, ValueError):
                continue
            if day in PING_QUIET_DAYS and day not in clean_days:
                clean_days.append(day)
        days = sorted(clean_days)
    else:
        days = list(PING_QUIET_DAYS)
    timezone = _clean_profile_timezone(raw.get("timezone", "")) or \
        _clean_profile_timezone((rec or {}).get("profile_timezone", "")) or \
        "UTC"
    return {
        "enabled": bool(raw.get("enabled")) and bool(days),
        "start": _quiet_minute(raw.get("start"), PING_QUIET_DEFAULT_START),
        "end": _quiet_minute(raw.get("end"), PING_QUIET_DEFAULT_END),
        "days": days,
        "timezone": timezone,
    }


def _apply_ping_quiet_hours(rec, raw):
    """Store a requested quiet-hours window; True when the record changed."""
    if not isinstance(raw, dict):
        return False
    merged = dict(ping_quiet_hours(rec))
    for key in ("enabled", "start", "end", "days", "timezone"):
        if key in raw:
            merged[key] = raw.get(key)
    clean = ping_quiet_hours({"notification_quiet_hours": merged,
                              "profile_timezone": (rec or {}).get(
                                  "profile_timezone", "")})
    if clean["start"] == clean["end"]:
        clean["enabled"] = False  # an empty window is no window
    stored = rec.get("notification_quiet_hours")
    untouched = (not clean["enabled"]
                 and clean["start"] == PING_QUIET_DEFAULT_START
                 and clean["end"] == PING_QUIET_DEFAULT_END
                 and clean["days"] == list(PING_QUIET_DAYS))
    if untouched:
        if stored is None:
            return False
        rec.pop("notification_quiet_hours", None)
    else:
        if stored == clean:
            return False
        rec["notification_quiet_hours"] = clean
    _forget_ping_settings(rec)
    return True


def ping_quiet_until(rec, now=0):
    """Millisecond end of the quiet window `now` falls in, else 0."""
    quiet = ping_quiet_hours(rec)
    if not quiet["enabled"]:
        return 0
    import datetime
    try:
        from zoneinfo import ZoneInfo
        zone = ZoneInfo(quiet["timezone"])
    except Exception:
        zone = datetime.timezone.utc
    now = int(now or Date.now())
    local = datetime.datetime.fromtimestamp(now / 1000, zone)
    minute = local.hour * 60 + local.minute
    start, end = quiet["start"], quiet["end"]
    midnight = local.replace(hour=0, minute=0, second=0, microsecond=0)
    # The window that could contain now started either today or, for one that
    # crosses midnight, yesterday. Wall-clock arithmetic in the zone, so the
    # deadline stays 07:00 across a DST change.
    if start < end:
        if local.weekday() in quiet["days"] and start <= minute < end:
            finish = midnight + datetime.timedelta(minutes=end)
        else:
            return 0
    elif minute >= start and local.weekday() in quiet["days"]:
        finish = midnight + datetime.timedelta(days=1, minutes=end)
    elif minute < end and (local.weekday() - 1) % 7 in quiet["days"]:
        finish = midnight + datetime.timedelta(minutes=end)
    else:
        return 0
    try:
        deadline = int(finish.timestamp() * 1000)
    except Exception:
        return 0
    return deadline if deadline > now else 0


def ping_settings_payload(rec, is_admin=False):
    """Everything the Pings settings page shows, in one wire shape."""
    return {
        "emailNotifications": (rec or {}).get("email_notifications") is not False,
        "notificationPreferences": notification_email_preferences(rec or {}),
        "notificationDelivery": notification_delivery_mode(rec or {}),
        "notificationPauseUntil": ping_pause_until(rec),
        "pingMutedKinds": ping_muted_kinds(rec),
        "notificationQuietHours": ping_quiet_hours(rec),
        "notificationQuietUntil": ping_quiet_until(rec),
        "pingKindCatalog": ping_kind_catalog(is_admin),
        "marketingEmail": marketing_email.since(rec) > 0,
        "marketingEmailSince": marketing_email.since(rec),
    }


def _apply_ping_settings(rec, raw):
    """Apply any subset of the Pings settings page; True when rec changed."""
    changed = False
    if "emailNotifications" in raw:
        wanted = bool(raw.get("emailNotifications"))
        if bool(rec.get("email_notifications", True)) != wanted:
            if wanted:
                rec.pop("email_notifications", None)
            else:
                rec["email_notifications"] = False
            changed = True
    if "notificationPreferences" in raw:
        prefs = _clean_notification_preferences(
            raw.get("notificationPreferences"), rec)
        if rec.get("notification_preferences", {}) != prefs:
            rec["notification_preferences"] = prefs
            changed = True
    if "notificationDelivery" in raw:
        changed = _apply_notification_delivery(
            rec, raw.get("notificationDelivery")) or changed
    if "notificationPauseUntil" in raw:
        changed = _apply_ping_pause(
            rec, raw.get("notificationPauseUntil")) or changed
    if "pingMutedKinds" in raw:
        changed = _apply_ping_muted_kinds(
            rec, raw.get("pingMutedKinds")) or changed
    if "notificationQuietHours" in raw:
        changed = _apply_ping_quiet_hours(
            rec, raw.get("notificationQuietHours")) or changed
    if "marketingEmail" in raw:
        changed = marketing_email.apply_consent(
            rec, raw.get("marketingEmail"), "settings",
            int(Date.now())) or changed
    return changed


def _forget_ping_settings(rec):
    memo = globals().get("_PING_SETTINGS_MEMO")
    name = clean_string((rec or {}).get("name", ""), MAX_NODE_NAME).lower()
    if isinstance(memo, dict) and name:
        memo.pop(name, None)


async def _ping_delivery_settings(env, recipient, now):
    """(muted kinds, paused) for one recipient, memoized per isolate.

    A machine node's pings are governed by its owning user, the same account
    whose record the email rail and the heartbeat read settings from. A failed
    read is never remembered, and answers "deliver everything" — a lost
    setting must not cost somebody a ping they did not turn off.
    """
    memo = _PING_SETTINGS_MEMO
    hit = memo.get(recipient)
    if hit and 0 <= now - hit[0] < PING_SETTINGS_MEMO_TTL_MS:
        rec = hit[1]
    else:
        try:
            _, rec = await _account_row(env, recipient)
            owner = clean_string((rec or {}).get("owner", ""),
                                 MAX_NODE_NAME).lower()
            if owner:
                _, owner_rec = await _account_row(env, owner)
                if owner_rec and owner_rec.get("status") == "active":
                    rec = owner_rec
        except Exception:
            return set(), False
        rec = rec if isinstance(rec, dict) else {}
        if len(memo) >= PING_SETTINGS_MEMO_MAX:
            memo.clear()
        memo[recipient] = (now, {
            "ping_muted_kinds": rec.get("ping_muted_kinds"),
            "notification_pause_until": rec.get("notification_pause_until"),
            "notification_quiet_hours": rec.get("notification_quiet_hours"),
            "profile_timezone": rec.get("profile_timezone", ""),
        })
        rec = memo[recipient][1]
    return set(ping_muted_kinds(rec)), pings_paused(rec, now)


STATUS_MONITOR_ALERT_CHANNELS = ("pings", "emails", "continual")


async def status_monitor_alert_rows(env):
    """The /status monitor alert matrix as the Pings settings page shows it.

    Platform-wide, not per account: every administrator is paged by the same
    switches. It used to be the admin console's "Alert delivery" form; since
    #347 it lives on the Pings settings page, shown to administrators only.
    """
    settings = await _repo_alert_settings_get(
        env, *FLAGSHIP_MONITOR_ID.split("/", 1))
    return [
        dict({"id": system_id, "label": label}, **{
            channel: _status_monitor_alert_setting(settings, system_id, channel)
            for channel in STATUS_MONITOR_ALERT_CHANNELS})
        for system_id, label in STATUS_ALERT_ADMIN_SYSTEMS
    ]


async def _save_status_monitor_alerts(env, raw, actor=""):
    """Merge {monitor id: {pings, emails, continual}}; True when stored.

    Callers must already have established that `actor` is an administrator.
    Monitors and channels absent from `raw` keep their current value, so a
    client can send just the switch it flipped.
    """
    if not isinstance(raw, dict):
        return False
    current = await _repo_alert_settings_get(
        env, *FLAGSHIP_MONITOR_ID.split("/", 1))
    monitors = {}
    for system_id, _label in STATUS_ALERT_ADMIN_SYSTEMS:
        monitor = {channel: _status_monitor_alert_setting(
            current, system_id, channel)
            for channel in STATUS_MONITOR_ALERT_CHANNELS}
        wanted = raw.get(system_id)
        if isinstance(wanted, dict):
            for channel in STATUS_MONITOR_ALERT_CHANNELS:
                if channel in wanted:
                    monitor[channel] = bool(wanted.get(channel))
        monitors[system_id] = monitor
    if current.get("statusMonitors") == monitors:
        return False
    updated = dict(current)
    updated["statusMonitors"] = monitors
    # Keep the legacy aggregate values meaningful for older Worker versions
    # during a rolling deploy.
    updated["statusEmails"] = any(
        item["emails"] for item in monitors.values())
    updated["statusPings"] = any(
        item["pings"] for item in monitors.values())
    await d1_run(
        env,
        "INSERT INTO repo_alert_settings "
        "(repo_bi, data, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(repo_bi) DO UPDATE SET "
        "data=excluded.data, updated_at=excluded.updated_at",
        await _repo_alert_settings_bi(
            env, *FLAGSHIP_MONITOR_ID.split("/", 1)),
        json.dumps(updated), int(Date.now()))
    await _audit_sensitive_action(
        env, actor, "admin.status_monitor_alerts", "platform_alerts",
        "forkmesh-system-checks", "success", {
            "monitorCount": len(monitors),
            "continual": sorted(
                key for key, value in monitors.items() if value["continual"]),
        })
    return True
