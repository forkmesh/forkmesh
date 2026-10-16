PUSH_DEFAULT_ICON = "\U0001F514"
PUSH_CHAT_ICON = "\U0001F4AC"
PUSH_DIRECT_MESSAGE_ICON = "\U0001F4E8"
PUSH_DIRECT_START_ICON = "\U0001F44B"
PUSH_KIND_ICONS = {
    "mention": "\U0001F4E3",
    "hackathon_squad": "\U0001F3C1",
    "hackathon_reward": "\U0001F3C6",
    "subscribed": "\U0001F440",
    "issue_opened": "\U0001F41B",
    "pull_submitted": "\U0001F500",
    "issue_assigned": "\U0001F4CC",
    "repo_shared": "\U0001F91D",
    "bounty_funded": "\U0001F4B0",
    "bounty_paid": "\U0001F4B8",
    "release_published": "\U0001F680",
    "host_online": "\U0001F7E2",
    "host_offline": "\U0001F534",
    "credits_refilled": "\U0001F50B",
    "pending_inbox": "\U0001F4E5",
    "mirror_request": "\U0001FA9E",
    "pending_reward": "\U0001F381",
    "org_succession": "\U0001F451",
    "repository_hosted": "\U0001F3E0",
    "organization_task_update": "\u2705",
    "calendar_invitation": "\U0001F4C5",
    "calendar_reminder": "\u23F0",
    "calendar_rsvp": "\U0001F64B",
    "chat_webhook": "\U0001FA9D",
    "channel_unread": PUSH_CHAT_ICON,
    "direct_unread": PUSH_CHAT_ICON,
    "general_chat": PUSH_CHAT_ICON,
    "account_email_sent": "\U0001F4E7",
    "operational_alert": "\U0001F4DF",
    "tip_sent": "\U0001FA99",
    "error_group": "\U0001F4A5",
    "device_added": "\U0001F4F1",
    "plugin_activity": "\U0001F9E9",
    "timer_finished": "\u231B",
    "desktop_ping": "\U0001F4BB",
    "repo_activity": "\U0001F4E6",
    "code_push": "\U0001F4E4",
    "security_alert": "\U0001F6A8",
    "package_review": "\u2B50",
    "badge": "\U0001F3C5",
}
PUSH_DESKTOP_CATEGORY_ICONS = {
    "agent-finished": "\U0001F916",
    "agent-failed": "\u26A0\uFE0F",
}
PUSH_DESKTOP_ALARM_ICONS = {
    "power": "\U0001FAAB",
}


def alert_icon(kind="", meta=None):
    meta = meta if isinstance(meta, dict) else {}
    kind = str(kind or "").strip()
    if kind == "desktop_ping":
        alarm = str(meta.get("alarm") or "").strip()
        if alarm in PUSH_DESKTOP_ALARM_ICONS:
            return PUSH_DESKTOP_ALARM_ICONS[alarm]
        category = str(meta.get("category") or "").strip()
        if category in PUSH_DESKTOP_CATEGORY_ICONS:
            return PUSH_DESKTOP_CATEGORY_ICONS[category]
    return PUSH_KIND_ICONS.get(kind, PUSH_DEFAULT_ICON)


def with_icon(title, icon):
    title = str(title or "").strip()
    icon = str(icon or "").strip()
    if not title or not icon or title.startswith(icon):
        return title
    return icon + " " + title
