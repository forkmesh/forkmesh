import json
import time


MIN_INTERVAL_MINUTES = 5
MAX_SCHEDULES = 8
MAX_EXPRESSION = 100
SCAN_LIMIT = 500
ALIASES = {
    "@hourly": "0 * * * *",
    "@daily": "0 0 * * *",
    "@midnight": "0 0 * * *",
    "@weekly": "0 0 * * 0",
    "@monthly": "0 0 1 * *",
}
FIELDS = ((0, 59), (0, 23), (1, 31), (1, 12), (0, 6))


def _number(text, low, high):
    if not text.isdigit():
        raise ValueError(text)
    value = int(text)
    if not low <= value <= high:
        raise ValueError(text)
    return value


def _field(text, low, high, weekday=False):
    top = 7 if weekday else high
    values = set()
    for part in text.split(","):
        step = 1
        base = part
        if "/" in part:
            base, step_text = part.split("/", 1)
            step = _number(step_text, 1, top + 1)
        if base == "*":
            start, end = low, high
        elif "-" in base:
            first, last = base.split("-", 1)
            start, end = _number(first, low, top), _number(last, low, top)
            if start > end:
                raise ValueError(part)
        else:
            start = _number(base, low, top)
            end = top if "/" in part else start
        for value in range(start, end + 1, step):
            values.add(0 if weekday and value == 7 else value)
    if not values:
        raise ValueError(text)
    return frozenset(values)


def parse(expression):
    text = " ".join(str(expression or "").split())
    if not text or len(text) > MAX_EXPRESSION:
        return None
    text = ALIASES.get(text.lower(), text)
    parts = text.split(" ")
    if len(parts) != 5:
        return None
    try:
        fields = tuple(
            _field(part, low, high, index == 4)
            for index, (part, (low, high)) in enumerate(zip(parts, FIELDS)))
    except ValueError:
        return None
    minutes = sorted(fields[0])
    gaps = [b - a for a, b in zip(minutes, minutes[1:])]
    if len(fields[1]) > 1 or len(minutes) > 1:
        gaps.append(minutes[0] + 60 - minutes[-1])
    if gaps and min(gaps) < MIN_INTERVAL_MINUTES:
        return None
    return {
        "fields": fields,
        "anyDay": parts[2] == "*",
        "anyWeekday": parts[4] == "*",
    }


def valid(expression):
    return parse(expression) is not None


def matches(spec, slot):
    moment = time.gmtime(int(slot) * 60)
    minute, hour, day, month, weekday = spec["fields"]
    if (moment.tm_min not in minute or moment.tm_hour not in hour
            or moment.tm_mon not in month):
        return False
    day_ok = moment.tm_mday in day
    weekday_ok = (moment.tm_wday + 1) % 7 in weekday
    if spec["anyDay"] or spec["anyWeekday"]:
        return day_ok and weekday_ok
    return day_ok or weekday_ok


def _schedules(value):
    if isinstance(value, str):
        try:
            value = json.loads(value or "[]")
        except ValueError:
            return []
    if not isinstance(value, list):
        return []
    return [str(item) for item in value[:MAX_SCHEDULES]
            if isinstance(item, str)]


def due(rows, slot):
    slot = int(slot)
    found = []
    for row in rows or []:
        try:
            last = int(row.get("last_slot") or 0)
        except (TypeError, ValueError):
            last = 0
        if slot - last < MIN_INTERVAL_MINUTES:
            continue
        for expression in _schedules(row.get("schedules")):
            spec = parse(expression)
            if spec and matches(spec, slot):
                found.append((row, expression))
                break
    return found


async def run(runtime, slot):
    import runner_actions
    rows = await runtime.d1_all(
        "SELECT owner,repo,path,schedules,last_slot FROM repo_action_workflows "
        "WHERE schedules!='[]' ORDER BY owner,repo,path LIMIT %d" % SCAN_LIMIT)
    queued = 0
    for row, expression in due(rows, slot):
        result = await runtime.d1_run(
            "UPDATE repo_action_workflows SET last_slot=? WHERE owner=? "
            "AND repo=? AND path=? AND last_slot=?", int(slot), row["owner"],
            row["repo"], row["path"], int(row.get("last_slot") or 0))
        if runtime.changes(result) != 1:
            continue
        branch = await runtime.default_branch(row["owner"], row["repo"])
        if await runner_actions.enqueue(
                runtime, row["owner"], row["repo"], branch, "", "", "",
                event="schedule", workflow=row["path"],
                payload={"schedule": expression}):
            queued += 1
    return queued
