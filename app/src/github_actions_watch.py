import json
import re
from urllib.parse import quote


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


GITHUB_API = "https://api.github.com"
GITHUB_WEB = "https://github.com"
DEFAULT_REPO = "forkmesh/forkmesh"
DEFAULT_BRANCH = "main"
MONITOR_PREFIX = "github-actions:"
RUN_PAGE = 100
WORKFLOW_RUN_PAGE = 50
MAX_SWEEP_WORKFLOWS = 12
SWEEP_EVERY_MS = 3_600_000
SWEEP_WINDOW_MS = 300_000
REQUEST_TIMEOUT_SECONDS = 10
MAX_RESPONSE_CHARS = 4_000_000
MAX_ALERTS_PER_RUN = 8
MAX_FAILED_STEPS = 3
MAX_ANNOTATION_JOBS = 2
DETAIL_MAX = 220
FAILURE_MARKERS = ("FAILED ", "--- FAIL", "FAIL:", "Error:", "AssertionError", "E   ")
RUNNER_EXIT_PREFIX = "Process completed with exit code"
ADMIN_LIMIT = 50
PING_KIND = "operational_alert"
PING_SOURCE = "github-actions"
TITLE_MAX = 120
BODY_MAX = 480
FAILED_CONCLUSIONS = frozenset(("failure", "timed_out", "startup_failure"))
PASSED_CONCLUSIONS = frozenset(("success",))
WATCHED_EVENTS = frozenset(("push", "schedule", "workflow_dispatch"))
REPO_RE = re.compile(r"^[A-Za-z0-9_.-]{1,100}/[A-Za-z0-9_.-]{1,100}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9_./-]{1,200}$")
DISABLED_VALUES = frozenset(("off", "none", "0", "false", "disabled"))


def _text(value, maximum):
    text = " ".join(str(value if value is not None else "").split())
    return text[:maximum]


def watch_config(env):
    repo = _text(getattr(env, "GITHUB_ACTIONS_WATCH_REPO", "") or "", 201)
    if repo.lower() in DISABLED_VALUES:
        return None
    repo = repo or DEFAULT_REPO
    branch = _text(
        getattr(env, "GITHUB_ACTIONS_WATCH_BRANCH", "") or "", 200) \
        or DEFAULT_BRANCH
    if not REPO_RE.match(repo) or not BRANCH_RE.match(branch):
        return None
    return {"repo": repo, "branch": branch}


def _token(env):
    return _text(getattr(env, "GITHUB_ACTIONS_WATCH_TOKEN", "") or "", 400)


def runs_path(repo, branch):
    return "/repos/%s/actions/runs?branch=%s&per_page=%d&exclude_pull_requests=true" % (
        repo, quote(branch, safe=""), RUN_PAGE)


def workflows_path(repo):
    return "/repos/%s/actions/workflows?per_page=100" % repo


def workflow_runs_path(repo, workflow_id, branch):
    return ("/repos/%s/actions/workflows/%d/runs?branch=%s&per_page=%d"
            "&exclude_pull_requests=true") % (
        repo, int(workflow_id), quote(branch, safe=""), WORKFLOW_RUN_PAGE)


def sweep_due(now):
    return int(now) % SWEEP_EVERY_MS < SWEEP_WINDOW_MS


def unseen_workflows(workflows_payload, verdicts):
    workflows = (workflows_payload or {}).get("workflows") if isinstance(
        workflows_payload, dict) else None
    unseen = []
    for workflow in workflows if isinstance(workflows, list) else []:
        if not isinstance(workflow, dict):
            continue
        if str(workflow.get("state") or "") != "active":
            continue
        path = _text(workflow.get("path") or "", 200)
        workflow_id = int(workflow.get("id") or 0)
        if path and workflow_id > 0 and path not in verdicts:
            unseen.append(workflow_id)
    return unseen[:MAX_SWEEP_WORKFLOWS]


def jobs_path(repo, run_id):
    return "/repos/%s/actions/runs/%d/jobs?filter=latest&per_page=100" % (
        repo, int(run_id))


def annotations_path(repo, job_id):
    return "/repos/%s/check-runs/%d/annotations?per_page=50" % (
        repo, int(job_id))


def _run_fields(run):
    head_commit = run.get("head_commit") or {}
    message = str(head_commit.get("message") or run.get("display_title") or "")
    return {
        "runId": int(run.get("id") or 0),
        "runNumber": int(run.get("run_number") or 0),
        "name": _text(run.get("name") or run.get("path") or "workflow", 80),
        "path": _text(run.get("path") or "", 200),
        "conclusion": str(run.get("conclusion") or ""),
        "event": str(run.get("event") or ""),
        "sha": _text(run.get("head_sha") or "", 40),
        "subject": _text(message.split("\n", 1)[0], 120),
        "url": _text(run.get("html_url") or "", 300),
        "createdAt": str(run.get("created_at") or ""),
    }


def workflow_verdicts(runs, branch):
    verdicts = {}
    for run in runs if isinstance(runs, list) else []:
        if not isinstance(run, dict):
            continue
        if str(run.get("head_branch") or "") != branch:
            continue
        if str(run.get("event") or "") not in WATCHED_EVENTS:
            continue
        if str(run.get("status") or "") != "completed":
            continue
        conclusion = str(run.get("conclusion") or "")
        if conclusion not in FAILED_CONCLUSIONS | PASSED_CONCLUSIONS:
            continue
        fields = _run_fields(run)
        key = fields["path"] or str(run.get("workflow_id") or "")
        if not key or not fields["runId"]:
            continue
        current = verdicts.get(key)
        if current is None:
            fields["failing"] = conclusion in FAILED_CONCLUSIONS
            fields["failingSince"] = fields if fields["failing"] else None
            fields["streak"] = 1 if fields["failing"] else 0
            verdicts[key] = fields
        elif current["failing"] and current.get("streakOpen", True):
            if conclusion in FAILED_CONCLUSIONS:
                current["failingSince"] = fields
                current["streak"] += 1
            else:
                current["streakOpen"] = False
    for verdict in verdicts.values():
        open_streak = verdict.pop("streakOpen", True)
        verdict["streakComplete"] = not (verdict["failing"] and open_streak)
        since = verdict.get("failingSince")
        verdict["failingSince"] = (
            {"runId": since["runId"], "sha": since["sha"],
             "createdAt": since["createdAt"]} if since else None)
    return verdicts


def _failed_jobs(jobs_payload):
    jobs = (jobs_payload or {}).get("jobs") if isinstance(
        jobs_payload, dict) else None
    return [
        job for job in (jobs if isinstance(jobs, list) else [])
        if isinstance(job, dict)
        and str(job.get("conclusion") or "") in FAILED_CONCLUSIONS]


def failed_job_ids(jobs_payload):
    return [int(job.get("id") or 0) for job in _failed_jobs(jobs_payload)
            if int(job.get("id") or 0) > 0][:MAX_ANNOTATION_JOBS]


def annotation_detail(annotations):
    for annotation in annotations if isinstance(annotations, list) else []:
        if not isinstance(annotation, dict):
            continue
        if str(annotation.get("annotation_level") or "") != "failure":
            continue
        message = str(annotation.get("message") or "")
        if message.startswith(RUNNER_EXIT_PREFIX):
            continue
        lines = [line.strip() for line in message.splitlines() if line.strip()]
        picked = [line for line in lines
                  if line.startswith(FAILURE_MARKERS)][:2] or lines[-1:]
        title = _text(annotation.get("title") or "", 120)
        detail = "; ".join(picked)
        if title and detail:
            detail = title + ": " + detail
        detail = _text(detail or title, DETAIL_MAX)
        if detail:
            return detail
    return ""


def failed_steps(jobs_payload):
    found = []
    for job in _failed_jobs(jobs_payload):
        job_name = _text(job.get("name") or "job", 60)
        steps = [
            _text(step.get("name") or "", 90)
            for step in job.get("steps") or []
            if isinstance(step, dict)
            and str(step.get("conclusion") or "") in FAILED_CONCLUSIONS]
        if steps:
            found.extend(job_name + " › " + step for step in steps if step)
        else:
            found.append(job_name)
    return found


def ping_text(repo, branch, verdict, steps=(), detail=""):
    name = verdict["name"]
    sha = verdict["sha"][:7]
    at = "%s at %s" % (branch, sha) if sha else branch
    subject = (" (%s)" % verdict["subject"]) if verdict["subject"] else ""
    url = verdict["url"] or "%s/%s/actions" % (GITHUB_WEB, repo)
    if verdict["failing"]:
        title = "GitHub Actions: %s is failing" % name
        what = "; ".join(list(steps)[:MAX_FAILED_STEPS]) or (
            "run #%d" % verdict["runNumber"])
        streak = int(verdict.get("streak") or 1)
        tail = (" It has failed %s%d runs in a row." % (
            "" if verdict.get("streakComplete", True) else "at least ",
            streak)) if streak > 1 else ""
        detail = (" " + detail.rstrip(".") + ".") if detail else ""
        body = "%s %s on %s%s.%s%s %s" % (
            what, "timed out" if verdict["conclusion"] == "timed_out"
            else "failed", at, subject, detail, tail, url)
    else:
        title = "GitHub Actions: %s passes again" % name
        body = "Run #%d passed on %s%s. %s" % (
            verdict["runNumber"], at, subject, url)
    return _text(title, TITLE_MAX), _text(body, BODY_MAX)


def monitor_id(repo, path):
    return MONITOR_PREFIX + repo.lower() + ":" + path


def plan_alerts(verdicts, prior_rows, repo):
    plans = []
    for path in sorted(verdicts):
        verdict = verdicts[path]
        key = monitor_id(repo, path)
        row = prior_rows.get(key)
        state = "down" if verdict["failing"] else "up"
        if row is None:
            pinged = "" if verdict["failing"] else "up"
            changed = True
        else:
            pinged = str(row.get("pinged_state") or "")
            changed = bool(int(row.get("is_up") or 0)) == verdict["failing"]
        plans.append({
            "monitorId": key, "verdict": verdict, "state": state,
            "changed": changed, "pinged": pinged,
            "alert": pinged != state,
            "changedAt": None if changed else int(
                row.get("changed_at") or 0)})
    return plans


async def _github_get(env, path):
    headers = {
        "accept": "application/vnd.github+json",
        "user-agent": "ForkMesh-GitHub-Actions-Watch",
        "x-github-api-version": "2022-11-28",
    }
    token = _token(env)
    if token:
        headers["authorization"] = "Bearer " + token
    try:
        response = await js_fetch_with_timeout(
            GITHUB_API + path, {"method": "GET", "headers": headers,
                                "redirect": "follow"},
            REQUEST_TIMEOUT_SECONDS)
    except Exception:
        return 0, None
    if response is None:
        return 0, None
    status = int(getattr(response, "status", 0) or 0)
    try:
        text = str(await response.text() or "")
    except Exception:
        text = ""
    if len(text) > MAX_RESPONSE_CHARS:
        return 502, None
    try:
        return status, json.loads(text) if text else None
    except ValueError:
        return status, None


async def _admins(env):
    rows = await d1_all(
        env, "SELECT data FROM users WHERE is_admin=1 ORDER BY user_bi LIMIT ?",
        ADMIN_LIMIT)
    names = []
    for row in rows or []:
        try:
            record = await decrypt_row(env, row.get("data", "")) or {}
        except Exception:
            continue
        if record.get("service_account") is True:
            continue
        name = clean_string(record.get("name", ""), MAX_NODE_NAME).strip().lower()
        if valid_node_name(name) and name not in names:
            names.append(name)
    return names


async def _prior_rows(env, monitor_ids):
    if not monitor_ids:
        return {}
    rows = await d1_all(
        env,
        "SELECT monitor_id,is_up,changed_at,pinged_state "
        "FROM repository_monitor_state WHERE monitor_id IN ("
        + ",".join("?" * len(monitor_ids)) + ")",
        *monitor_ids)
    return {str(row.get("monitor_id") or ""): row for row in rows or []}


async def _save_state(env, plan, now):
    verdict = plan["verdict"]
    is_up = 0 if verdict["failing"] else 1
    changed_at = now if plan["changed"] else int(plan["changedAt"] or now)
    reason = _text("%s #%d %s %s" % (
        verdict["name"], verdict["runNumber"], verdict["conclusion"],
        verdict["sha"][:7]), 240)
    await d1_run(
        env,
        """INSERT INTO repository_monitor_state
             (monitor_id,is_up,changed_at,outage_started_at,checked_at,reason,
              notified_state,pinged_state)
           VALUES (?,?,?,?,?,?,?,?)
           ON CONFLICT(monitor_id) DO UPDATE SET
             is_up=excluded.is_up,
             changed_at=CASE
               WHEN repository_monitor_state.is_up!=excluded.is_up
               THEN excluded.changed_at
               ELSE repository_monitor_state.changed_at END,
             outage_started_at=CASE
               WHEN excluded.is_up=1 THEN 0
               WHEN repository_monitor_state.is_up!=excluded.is_up
               THEN excluded.outage_started_at
               ELSE repository_monitor_state.outage_started_at END,
             checked_at=excluded.checked_at,
             reason=excluded.reason""",
        plan["monitorId"], is_up, changed_at, 0 if is_up else changed_at,
        now, reason, "", plan["pinged"])
    return changed_at


async def _mark_pinged(env, plan):
    await d1_run(
        env,
        "UPDATE repository_monitor_state SET pinged_state=? "
        "WHERE monitor_id=? AND is_up=?",
        plan["state"], plan["monitorId"],
        0 if plan["verdict"]["failing"] else 1)


async def _deliver(env, config, plan, admins, changed_at, now):
    verdict = plan["verdict"]
    steps = []
    detail = ""
    if verdict["failing"]:
        status, jobs = await _github_get(
            env, jobs_path(config["repo"], verdict["runId"]))
        if status == 200:
            steps = failed_steps(jobs)
            for job_id in failed_job_ids(jobs):
                status, annotations = await _github_get(
                    env, annotations_path(config["repo"], job_id))
                detail = annotation_detail(annotations) if status == 200 else ""
                if detail:
                    break
    title, body = ping_text(
        config["repo"], config["branch"], verdict, steps, detail)
    delivered = False
    for admin in admins:
        if await enqueue_notification(
                env, admin, PING_KIND, title, body=body, href="/status",
                source=PING_SOURCE,
                dedupe="%s%s:%s:%d" % (
                    MONITOR_PREFIX, plan["monitorId"][len(MONITOR_PREFIX):],
                    plan["state"], changed_at),
                ts=now,
                meta={"system": plan["monitorId"], "state": plan["state"],
                      "repo": config["repo"], "workflow": verdict["path"],
                      "runId": verdict["runId"], "url": verdict["url"]}):
            delivered = True
    return delivered


async def _sweep_unseen(env, config, verdicts):
    status, payload = await _github_get(env, workflows_path(config["repo"]))
    if status != 200:
        return
    for workflow_id in unseen_workflows(payload, verdicts):
        status, runs = await _github_get(env, workflow_runs_path(
            config["repo"], workflow_id, config["branch"]))
        if status == 200 and isinstance(runs, dict):
            verdicts.update(workflow_verdicts(
                runs.get("workflow_runs"), config["branch"]))


async def watch_github_actions(env):
    config = watch_config(env)
    if not config:
        return {"ok": True, "checked": 0, "reason": "watch_not_configured"}
    await ensure_schema(env)
    status, payload = await _github_get(
        env, runs_path(config["repo"], config["branch"]))
    if status != 200 or not isinstance(payload, dict):
        return {"ok": False, "checked": 0, "status": status}
    verdicts = workflow_verdicts(
        payload.get("workflow_runs"), config["branch"])
    now = int(Date.now())
    if sweep_due(now):
        await _sweep_unseen(env, config, verdicts)
    prior = await _prior_rows(env, [
        monitor_id(config["repo"], path) for path in verdicts])
    plans = plan_alerts(verdicts, prior, config["repo"])
    admins = None
    alerted = 0
    failing = 0
    for plan in plans:
        changed_at = await _save_state(env, plan, now)
        if plan["verdict"]["failing"]:
            failing += 1
        if not plan["alert"] or alerted >= MAX_ALERTS_PER_RUN:
            continue
        if admins is None:
            admins = await _admins(env)
        if await _deliver(env, config, plan, admins, changed_at, now):
            await _mark_pinged(env, plan)
            alerted += 1
    return {"ok": True, "checked": len(plans), "failing": failing,
            "alerted": alerted}
