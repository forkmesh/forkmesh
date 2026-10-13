KIND = "organization_task_update"
WINDOW_MS = 60 * 1000
SCAN_ROWS = 12
EXCERPT_CHARS = 80
MAX_ENTRIES = 20
MAX_DIGEST_TASKS = 30
MAX_ACTORS = 8
MAX_TITLE = 160
MAX_BODY = 900
TASKS_HREF = "/dashboard/tasks/"

STATUS_COPY = {
    "started": ("started", "Started"),
    "stopped": ("stopped", "Stopped"),
    "completed": ("done", "Marked done"),
    "undone": ("undone", "Marked undone"),
    "qa requested": ("sent to QA", "Sent to QA"),
    "returned": ("returned", "Returned to the task list"),
    "qa passed": ("QA passed", "QA passed"),
    "qa failed": ("QA failed", "QA failed"),
}
CHECKIN_ACTIONS = {
    "checked in going well": "going well",
    "checked in blocked": "blocked",
    "checked in needs help": "needs help",
}
COUNTED_FIELDS = {"comment": ("comment", "comments"),
                  "checkin": ("check-in", "check-ins")}
META_VERBS = {
    "created": "created",
    "deleted": "deleted",
    "comment": "commented on",
    "checkin": "checked in on",
}


def _clean(value, limit):
    text = " ".join(str(value if value is not None else "").split())
    return text[:limit]


def _name(value):
    return _clean(value, 64).lower()


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError, OverflowError):
        return default


def excerpt(value, limit=EXCERPT_CHARS):
    text = _clean(value, 4000)
    if len(text) <= limit:
        return text
    return text[:limit].rstrip() + "…"


def _short(value, limit):
    text = _clean(value, 4000)
    if len(text) <= limit:
        return text
    return text[:max(1, limit - 1)].rstrip() + "…"


def _entry(field, token, line, detail=""):
    return {"field": field, "token": token, "line": line, "detail": detail}


def change_entry(change):
    change = change if isinstance(change, dict) else {}
    field = _clean(change.get("field"), 32).lower()
    if field == "progress":
        after = max(0, min(100, _int(change.get("to"))))
        before = change.get("from")
        line = ("Progress %d%% → %d%%" % (
            max(0, min(100, _int(before))), after)
            if before is not None and before != "" else
            "Progress %d%%" % after)
        return _entry("progress", "progress %d%%" % after, line)
    if field == "priority":
        after = _int(change.get("to"))
        before = change.get("from")
        line = ("Priority %d → %d" % (_int(before), after)
                if before is not None and before != "" else
                "Priority %d" % after)
        return _entry("priority", "priority %d" % after, line)
    if field == "title":
        before = _short(change.get("from"), 60)
        return _entry("title", "title",
                      "Renamed from “%s”" % before
                      if before else "Title changed")
    if field == "details":
        return _entry("details", "description", "Description edited")
    if field == "assignee":
        kind = _clean(change.get("kind"), 16).lower()
        after = _name(change.get("to"))
        before = _name(change.get("from"))
        was = (" (was @%s)" % before) if before and before != after else ""
        if kind == "agent":
            return _entry("assignee", "assigned to the bot",
                          "Assigned to the bot" + was)
        if kind == "user" and after:
            return _entry("assignee", "assigned @" + after,
                          "Assigned to @" + after + was)
        return _entry("assignee", "unassigned", "Unassigned" + was)
    if field == "repository":
        after = _clean(change.get("to"), 201)
        return _entry("repository", "repository",
                      "Repository set to " + after
                      if after else "Repository cleared")
    if field == "routing":
        placement = " / ".join(
            part for part in (
                _clean(change.get("department"), 64).lower(),
                _clean(change.get("team"), 64).lower(),
            ) if part)
        return _entry("routing", "moved",
                      "Moved to " + placement if placement else "Moved")
    if field == "howtotest":
        return _entry("howToTest", "test steps", "Test steps edited")
    if field == "parent":
        return _entry("parent", "parent task",
                      "Parent task set" if _clean(change.get("to"), 80)
                      else "Parent task cleared")
    if field == "workitems":
        summary = _short(change.get("summary"), 120)
        return _entry("workItems", "work items",
                      "Work items: " + summary if summary
                      else "Work items changed")
    if field == "status":
        to = _clean(change.get("to"), 32).lower()
        token, line = STATUS_COPY.get(to, (to or "updated", (
            to or "updated").capitalize()))
        return _entry("status", token, line)
    if field == "comment":
        quote = excerpt(change.get("excerpt"))
        return _entry("comment", "comment", "New comment", quote)
    if field == "checkin":
        state = _clean(change.get("state"), 32).lower()
        quote = excerpt(change.get("excerpt"))
        return _entry("checkin", "check-in",
                      "Check-in (%s)" % state if state else "Check-in",
                      quote)
    if field == "created":
        return _entry("created", "created", "Created")
    if field == "deleted":
        return _entry("deleted", "deleted", "Deleted")
    return None


def action_changes(action):
    action = _clean(action, 40).lower().replace("_", " ")
    if action == "created":
        return [{"field": "created"}]
    if action == "deleted":
        return [{"field": "deleted"}]
    if action in STATUS_COPY:
        return [{"field": "status", "to": action}]
    if action in CHECKIN_ACTIONS:
        return [{"field": "checkin", "state": CHECKIN_ACTIONS[action]}]
    if action == "commented":
        return [{"field": "comment"}]
    return [{"field": "status", "to": action or "updated"}]


def entries_for(action, changes, actor, now, task_id, task_title):
    raw = changes if isinstance(changes, (list, tuple)) and changes else (
        action_changes(action))
    entries = []
    for change in raw:
        entry = change_entry(change)
        if entry is None:
            continue
        entry["actor"] = _name(actor)
        entry["at"] = _int(now)
        entry["taskId"] = _clean(task_id, 80)
        entry["taskTitle"] = _clean(task_title, MAX_TITLE)
        entries.append(entry)
    return entries


def _actors_text(actors):
    names = ["@" + name for name in actors if name]
    if not names:
        return ""
    if len(names) == 1:
        return names[0]
    if len(names) == 2:
        return names[0] + " and " + names[1]
    if len(names) == 3:
        return names[0] + ", " + names[1] + " and " + names[2]
    return "%s, %s and %d others" % (names[0], names[1], len(names) - 2)


def _merge_actors(existing, entries):
    actors = [name for name in (existing or []) if isinstance(name, str)]
    for entry in entries:
        name = entry.get("actor") or ""
        if name and name not in actors:
            actors.append(name)
    return actors[:MAX_ACTORS]


def summary_tokens(entries):
    order = []
    latest = {}
    counts = {}
    for entry in entries:
        field = entry.get("field") or ""
        if field not in order:
            order.append(field)
        counts[field] = counts.get(field, 0) + 1
        latest[field] = entry.get("token") or field
    tokens = []
    for field in order:
        if field in COUNTED_FIELDS and counts[field] > 1:
            tokens.append("%d %s" % (counts[field], COUNTED_FIELDS[field][1]))
        else:
            tokens.append(latest[field])
    return tokens


def _single_line(entry):
    by = (" by @" + entry["actor"]) if entry.get("actor") else ""
    line = entry.get("line") or "Updated"
    detail = entry.get("detail") or ""
    if detail:
        return "%s%s: %s" % (line, by, detail)
    return line + by


def task_text(meta):
    entries = [entry for entry in (meta.get("entries") or [])
               if isinstance(entry, dict)]
    count = max(len(entries), _int(meta.get("count"), len(entries)))
    title_text = _clean(meta.get("taskTitle"), MAX_TITLE) or "Organization task"
    fields = {entry.get("field") for entry in entries}
    if "deleted" in fields:
        lead = "Task deleted: "
    elif count == 1 and "created" in fields:
        lead = "New task: "
    else:
        lead = "Task updated: "
    title = _short(lead + title_text, MAX_TITLE)
    if count <= 1 and entries:
        body = _single_line(entries[-1])
        if entries[-1].get("field") == "created":
            extra = []
            assignee = _name(meta.get("assignee"))
            if meta.get("assigneeKind") == "agent":
                extra.append("assigned to the bot")
            elif assignee:
                extra.append("assigned to @" + assignee)
            priority = _int(meta.get("priority"))
            if priority > 0:
                extra.append("priority %d" % priority)
            if extra:
                body = body + " · " + ", ".join(extra)
    else:
        actors = _actors_text(meta.get("actors") or [])
        body = "%d changes: %s" % (count, ", ".join(summary_tokens(entries)))
        if actors:
            body += " by " + actors
        latest_comment = next(
            (entry for entry in reversed(entries)
             if entry.get("field") in COUNTED_FIELDS and entry.get("detail")),
            None)
        if latest_comment:
            body += " · %s: %s" % (
                COUNTED_FIELDS[latest_comment["field"]][0].capitalize(),
                latest_comment["detail"])
    task_id = _clean(meta.get("taskId"), 80)
    href = (TASKS_HREF if "deleted" in fields or not task_id
            else TASKS_HREF + "?task=" + task_id)
    return title, _short(body, MAX_BODY), href


def digest_text(meta):
    tasks = [task for task in (meta.get("tasks") or [])
             if isinstance(task, dict)]
    task_count = max(len(tasks), _int(meta.get("taskCount"), len(tasks)))
    count = _int(meta.get("count"), task_count)
    title = "Tasks updated: %d task%s" % (
        task_count, "" if task_count == 1 else "s")
    actors = _actors_text(meta.get("actors") or [])
    lead = "%d change%s to %d task%s" % (
        count, "" if count == 1 else "s",
        task_count, "" if task_count == 1 else "s")
    if actors:
        lead += " by " + actors
    parts = []
    for task in tasks:
        tokens = ", ".join(task.get("tokens") or []) or "updated"
        parts.append("%s: %s" % (_short(task.get("taskTitle"), 48), tokens))
    body = lead
    shown = 0
    for part in parts:
        candidate = body + " · " + part
        if len(candidate) > MAX_BODY - 32:
            break
        body = candidate
        shown += 1
    remaining = task_count - shown
    if remaining > 0 and shown:
        body += " · and %d more" % remaining
    return _short(title, MAX_TITLE), _short(body, MAX_BODY), TASKS_HREF


def render(meta):
    if meta.get("digest"):
        return digest_text(meta)
    return task_text(meta)


def _context_meta(context):
    context = context if isinstance(context, dict) else {}
    keep = ("organization", "department", "team", "priority", "status",
            "assignee", "assigneeKind", "repository", "taskTitle", "taskId")
    return {key: context.get(key) for key in keep if key in context}


def new_task_meta(entries, context, now, dedupe):
    meta = _context_meta(context)
    meta["taskId"] = entries[-1]["taskId"]
    meta["taskTitle"] = entries[-1]["taskTitle"] or meta.get("taskTitle", "")
    meta["entries"] = [dict(entry) for entry in entries][-MAX_ENTRIES:]
    meta["count"] = len(entries)
    meta["actors"] = _merge_actors([], entries)
    meta["firstAt"] = _int(now)
    meta["dedupe"] = dedupe
    meta["digest"] = False
    _stamp_action(meta, entries)
    return meta


def fold_task_meta(meta, entries, context):
    folded = dict(meta if isinstance(meta, dict) else {})
    folded.update(_context_meta(context))
    folded["taskTitle"] = entries[-1]["taskTitle"] or folded.get(
        "taskTitle", "")
    previous = [entry for entry in (folded.get("entries") or [])
                if isinstance(entry, dict)]
    folded["entries"] = (previous + [dict(e) for e in entries])[-MAX_ENTRIES:]
    folded["count"] = max(len(previous), _int(
        folded.get("count"), len(previous))) + len(entries)
    folded["actors"] = _merge_actors(folded.get("actors"), entries)
    folded["digest"] = False
    _stamp_action(folded, entries)
    return folded


def _stamp_action(meta, entries):
    last = entries[-1]
    meta["actor"] = last.get("actor") or ""
    meta["action"] = META_VERBS.get(last.get("field"), "updated")


def _digest_add(tasks, task_id, title, entries, count=None):
    for task in tasks:
        if task.get("taskId") == task_id:
            prior = task.get("_entries") or []
            task["_entries"] = prior + list(entries)
            task["count"] = _int(task.get("count")) + (
                len(entries) if count is None else count)
            if title:
                task["taskTitle"] = title
            return
    tasks.append({
        "taskId": task_id,
        "taskTitle": title,
        "_entries": list(entries),
        "count": len(entries) if count is None else count,
    })


def _digest_finish(meta, tasks):
    finished = []
    for task in tasks:
        tokens = task.get("tokens") or []
        entries = task.pop("_entries", None)
        if entries:
            tokens = summary_tokens(entries)
        finished.append({
            "taskId": task.get("taskId") or "",
            "taskTitle": _clean(task.get("taskTitle"), MAX_TITLE),
            "tokens": tokens[:6],
            "count": _int(task.get("count"), 1),
        })
    meta["taskCount"] = max(
        len(finished), _int(meta.get("taskCount"), 0))
    meta["tasks"] = finished[-MAX_DIGEST_TASKS:]
    return meta


def start_digest_meta(rows, entries, now, dedupe):
    tasks = []
    actors = []
    first_at = _int(now)
    count = 0
    for row in rows:
        meta = row.get("meta") if isinstance(row.get("meta"), dict) else {}
        row_entries = [entry for entry in (meta.get("entries") or [])
                       if isinstance(entry, dict)]
        row_count = max(len(row_entries), _int(meta.get("count"), 0))
        _digest_add(tasks, meta.get("taskId") or "", meta.get("taskTitle"),
                    row_entries, row_count)
        count += row_count
        actors = _merge_actors(actors, row_entries)
        for name in meta.get("actors") or []:
            if isinstance(name, str) and name and name not in actors:
                actors.append(name)
        first = _int(meta.get("firstAt"), first_at)
        if first and first < first_at:
            first_at = first
    _digest_add(tasks, entries[-1]["taskId"], entries[-1]["taskTitle"],
                entries)
    count += len(entries)
    meta = {
        "digest": True,
        "count": count,
        "actors": _merge_actors(actors, entries)[:MAX_ACTORS],
        "firstAt": first_at,
        "dedupe": dedupe,
        "action": "updated",
        "actor": entries[-1].get("actor") or "",
    }
    return _digest_finish(meta, tasks)


def fold_digest_meta(meta, entries):
    folded = dict(meta if isinstance(meta, dict) else {})
    tasks = [dict(task) for task in (folded.get("tasks") or [])
             if isinstance(task, dict)]
    task_id = entries[-1]["taskId"]
    fresh = summary_tokens(entries)
    added = True
    for task in tasks:
        if task.get("taskId") == task_id:
            task["tokens"] = _merge_tokens(task.get("tokens") or [], fresh)
            task["count"] = _int(task.get("count"), 1) + len(entries)
            task["taskTitle"] = entries[-1]["taskTitle"] or task.get(
                "taskTitle", "")
            added = False
            break
    if added:
        tasks.append({"taskId": task_id,
                      "taskTitle": entries[-1]["taskTitle"],
                      "tokens": fresh[:6], "count": len(entries)})
    folded["count"] = _int(folded.get("count"), 0) + len(entries)
    folded["actors"] = _merge_actors(folded.get("actors"), entries)
    folded["actor"] = entries[-1].get("actor") or ""
    folded["action"] = "updated"
    folded["digest"] = True
    folded["taskCount"] = max(len(tasks), _int(
        folded.get("taskCount"), 0) + (1 if added else 0))
    folded["tasks"] = tasks[-MAX_DIGEST_TASKS:]
    return folded


def _token_key(token):
    text = str(token or "")
    if text.startswith(("assigned", "unassigned")):
        return "assignee"
    for prefix in ("progress ", "priority "):
        if text.startswith(prefix):
            return prefix.strip()
    for key, (one, many) in COUNTED_FIELDS.items():
        if text.endswith(one) or text.endswith(many):
            return key
    return text


def _merge_tokens(existing, fresh):
    merged = [token for token in existing if token]
    for token in fresh:
        slot = _token_key(token)
        merged = [item for item in merged if _token_key(item) != slot]
        merged.append(token)
    return merged[-6:]


def open_rows(rows, now):
    found = []
    for row in rows or []:
        meta = row.get("meta") if isinstance(row, dict) else None
        if not isinstance(meta, dict):
            continue
        first_at = _int(meta.get("firstAt"), _int(row.get("ts")))
        if not first_at or _int(now) - first_at >= WINDOW_MS:
            continue
        if not meta.get("dedupe"):
            continue
        found.append(row)
    return found


async def deliver(store, recipients, entries, context, now):
    entries = [entry for entry in (entries or []) if isinstance(entry, dict)]
    if not entries:
        return 0
    task_id = entries[-1]["taskId"]
    filed = 0
    for recipient in recipients:
        try:
            if await store.muted(recipient):
                continue
        except Exception:
            pass
        try:
            rows = open_rows(await store.window(recipient, now), now)
        except Exception:
            rows = []
        digest = None
        same = None
        others = []
        seen_read = False
        for row in rows:
            meta = row["meta"]
            mine = meta.get("digest") or meta.get("taskId") == task_id
            if row.get("read"):
                seen_read = seen_read or bool(mine)
                continue
            if meta.get("digest"):
                digest = digest or row
            elif meta.get("taskId") == task_id:
                same = same or row
            else:
                others.append(row)
        absorbed = []
        if digest is not None:
            meta = fold_digest_meta(digest["meta"], entries)
            push = False
        elif same is not None:
            meta = fold_task_meta(same["meta"], entries, context)
            push = False
        elif others:
            meta = start_digest_meta(
                others, entries, now,
                "organization-task-digest:%d" % _int(now))
            absorbed = others
            push = True
        else:
            meta = new_task_meta(
                entries, context, now,
                "organization-task-update:%s:%d" % (task_id, _int(now)))
            push = not seen_read
        title, body, href = render(meta)
        try:
            ok = await store.file(
                recipient, title, body, href, entries[-1].get("actor") or "",
                task_id if not meta.get("digest") else "tasks",
                meta["dedupe"], meta, push)
        except Exception:
            ok = False
        if not ok:
            continue
        filed += 1
        for row in absorbed:
            try:
                await store.drop(recipient, row.get("dedupeBi") or "")
            except Exception:
                pass
    return filed
