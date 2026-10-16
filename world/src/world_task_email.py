import re
from urllib.parse import quote

import world_task_pings as pings


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


KIND = "task"
MAX_RECIPIENTS = 10
HOURLY_CAP = 30
HOUR_MS = 60 * 60 * 1000
SUMMARY_LIMIT = 200
TASKS_PATH = "/dashboard/tasks/"
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
FOLLOWED = {"open": "email_opened", "click": "email_clicked",
            "view": "email_viewed"}


def _name(value):
    name = str(value or "").strip().lower()
    return name if NAME_RE.match(name) else ""


def _short(value, limit):
    text = " ".join(str(value or "").split())
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "…"


def recipients(names, actor, action):
    found = []
    actor = _name(actor)
    if actor and str(action or "").strip().lower() == "created":
        found.append(actor)
    for value in names or []:
        name = _name(value)
        if name and name not in found:
            found.append(name)
    return found[:MAX_RECIPIENTS]


def task_ref(org_bi, task_id):
    return "task:%s:%s" % (org_bi or "", str(task_id or "").lower())


def parse_ref(ref):
    head, _, task_id = str(ref or "").rpartition(":")
    prefix, _, org_bi = head.partition(":")
    return (org_bi, task_id) if prefix == "task" and org_bi else ("", "")


def task_url(base, task_id, token="", deleted=False):
    root = str(base or "").rstrip("/") + TASKS_PATH
    if deleted or not task_id:
        return root
    url = root + "?task=" + quote(str(task_id))
    return url + ("&mail=" + quote(token) if token else "")


def entry_line(entry):
    line = entry.get("line") or "Updated"
    if entry.get("actor"):
        line += " by @" + entry["actor"]
    if entry.get("detail"):
        line += ": " + entry["detail"]
    return line


def message(meta, entries, details, url, org):
    meta = dict(meta or {})
    meta["entries"] = list(entries)
    meta["count"] = len(entries)
    subject, _body, _href = pings.task_text(meta)
    lines = [entry_line(entry) for entry in entries]
    where = ("the %s organization" % org) if org else "your organization"
    footer = (
        "You get this email because you are on this task in %s. Turn off "
        "email notifications in your ForkMesh settings, or unsubscribe, to "
        "stop these emails." % where)
    text = "\n".join(lines)
    if details:
        text += "\n\n" + details
    text += "\n\nOpen the task: " + url + "\n\n" + footer
    return {"subject": subject, "lines": lines, "details": details,
            "url": url, "footer": footer, "text": text}


def emailed_summary(subject, names):
    return _short("%s → %s" % (
        subject, ", ".join("@" + name for name in names)), SUMMARY_LIMIT)


def _html(view):
    items = "".join(
        "<li style=\"margin:0 0 6px\">" + _html_escape(line) + "</li>"
        for line in view["lines"])
    body = (
        "<ul class=\"fm-text\" style=\"margin:0 0 16px;padding-left:20px;"
        "color:#3f3f46;font-size:14px\">" + items + "</ul>")
    footer = (
        "<p class=\"fm-muted\" style=\"margin:22px 0 0;color:#71717a;"
        "font-size:12px\">" + _html_escape(view["footer"]) + "</p>")
    return _forkmesh_email_card_html(
        _html_escape(view["subject"]), _html_escape(view["details"]), body,
        footer, action_label="Open the task", action_url=view["url"])


class _Events:
    def __init__(self, env):
        self.env = env

    def new_id(self):
        return _ap_uuid()

    async def seal(self, value):
        return await encrypt_row(self.env, value)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)


async def record(env, org_bi, task_id, actor, kind, summary, now):
    actor = _name(actor)
    actor_bi = await blind_index(env, actor) if actor else ""
    return await world_office_tasks._record_event(
        _Events(env), org_bi, task_id, actor_bi, actor, kind, now,
        {"summary": summary})


async def _address(env, name):
    _, rec = await _account_row(env, name)
    email = str((rec or {}).get("email") or "").strip()
    if (
        not rec
        or _account_kind(rec) != "user"
        or rec.get("status") != "active"
        or not email
        or not rec.get("email_verified")
        or rec.get("email_notifications") is False
    ):
        return ""
    return email


async def _deliver(env, name, email, view, send_id, ref, now):
    if await mail_activity.sent_since(
            env, KIND, email, now - HOUR_MS) >= HOURLY_CAP:
        await mail_log.enqueue(env, mail_log.note(
            KIND, email, view["subject"], "suppressed", at=now,
            body=view["text"],
            error="task email limit, %d an hour per person" % HOURLY_CAP))
        return False
    ok = await _send_email(env, email, view["subject"], view["text"],
                           _html(view), email_kind=KIND, send_id=send_id,
                           ref=ref, recipient=name)
    await _record_account_email(env, name, KIND, ok)
    return bool(ok)


def _task_title(task):
    return _short((task or {}).get("title") or "Organization task", 160)


def deleted_message(tasks, actor, reason, url, org):
    titles = [_task_title(task) for task in tasks]
    who = ("@" + actor) if _name(actor) else "a team member"
    if len(titles) == 1:
        subject = _short("Task deleted: " + titles[0], 160)
    else:
        subject = "%d tasks deleted" % len(titles)
    lines = ["“%s” deleted by %s" % (title, who) for title in titles]
    reason = _short(reason, 500)
    if reason:
        lines.append("Reason: " + reason)
    where = ("the %s organization" % org) if org else "your organization"
    details = (
        "%s deleted %s in %s. A deleted task counts as done. It stays in the "
        "trash for 30 days, where its creator, assignee or a manager can "
        "restore it." % (
            who, "this task" if len(titles) == 1 else "these tasks", where))
    footer = (
        "You get this email because you created or were assigned this task "
        "in %s. Turn off email notifications in your ForkMesh settings, or "
        "unsubscribe, to stop these emails." % where)
    text = "\n".join(lines) + "\n\n" + details
    text += "\n\nOpen the tasks: " + url + "\n\n" + footer
    return {"subject": subject, "lines": lines, "details": details,
            "url": url, "footer": footer, "text": text}


async def send_deleted(env, org_bi, org, actor, plan, reason=""):
    now = int(Date.now())
    actor = _name(actor)
    base = (_public_base_url(env) or FORKMESH_SITE_URL).rstrip("/")
    url = task_url(base, "", deleted=True)
    reached = {}
    subjects = {}
    count = 0
    for name, tasks in plan or []:
        name = _name(name)
        tasks = [task for task in tasks or []
                 if isinstance(task, dict)
                 and str(task.get("id") or "")]
        if not name or name == actor or not tasks:
            continue
        try:
            email = await _address(env, name)
            if not email:
                continue
            view = deleted_message(tasks, actor, reason, url, org)
            ref = task_ref(org_bi, tasks[0]["id"])
            if not await _deliver(env, name, email, view, _ap_uuid(), ref,
                                  now):
                continue
            count += 1
            for task in tasks:
                task_id = str(task["id"]).lower()
                reached.setdefault(task_id, []).append(name)
                subjects[task_id] = _short(
                    "Task deleted: " + _task_title(task), 160)
        except Exception as error:
            print("task delete email to @%s failed: %s" % (
                name, type(error).__name__))
    for task_id, names in reached.items():
        try:
            await record(env, org_bi, task_id, actor, "emailed",
                         emailed_summary(subjects[task_id], names), now)
        except Exception:
            pass
    return count


async def send_activity(env, org_bi, org, actor, task_id, action, changes,
                        names, meta, details):
    now = int(Date.now())
    task_id = str(task_id or "").lower()
    meta = meta if isinstance(meta, dict) else {}
    entries = pings.entries_for(
        action, changes, actor, now, task_id, meta.get("taskTitle") or "")
    if not entries or not task_id or not names:
        return 0
    deleted = any(entry.get("field") == "deleted" for entry in entries)
    base = (_public_base_url(env) or FORKMESH_SITE_URL).rstrip("/")
    ref = task_ref(org_bi, task_id)
    sent = []
    subject = ""
    for name in names:
        try:
            email = await _address(env, name)
            if not email:
                continue
            send_id = _ap_uuid()
            token = "" if deleted else await mail_activity.view_token(
                env, send_id)
            view = message(meta, entries, details,
                           task_url(base, task_id, token, deleted), org)
            subject = view["subject"]
            if await _deliver(env, name, email, view, send_id, ref, now):
                sent.append(name)
        except Exception as error:
            print("task email to @%s failed: %s" % (
                name, type(error).__name__))
    if sent:
        await record(env, org_bi, task_id, actor, "emailed",
                     emailed_summary(subject, sent), now)
    return len(sent)


async def mail_event(env, ref, event, who, subject, first, now):
    org_bi, task_id = parse_ref(ref)
    if not first or event not in FOLLOWED or not task_id:
        return False
    await record(env, org_bi, task_id, who, FOLLOWED[event], subject, now)
    return True


async def viewed(env, org_bi, task_id, token, viewer, now):
    return await mail_activity.viewed(
        env, token, task_ref(org_bi, task_id), viewer, now)
