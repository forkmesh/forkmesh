#!/usr/bin/env python3
"""Codemagic builds of the native iOS app, cloned from the Next core nodes.

    next_codemagic.py build <workflow> [--branch main] [--no-push] [--mirror] [--wait] [--execute]
    next_codemagic.py status <build-id> [--wait] [--logs]
    next_codemagic.py create-app [--team <id>] [--execute]   # once; see below

The Codemagic application clones
``https://fmnext-ci:<GIT_CI_PASSWORD>@app.next.forkmesh.com/git/forkmesh-next.git``
(tools/next_git_host.py). Its id is kept in ``<config>/codemagic-next.json``
(0600; ``appId`` is not secret). Workflows are in ``codemagic.yaml``.

* ``build`` pushes ``main`` + tags to every core node first
  (``next_git_host.py push``; skip with ``--no-push``), so Codemagic builds
  exactly what is here, then starts ``<workflow>`` on ``--branch``.
* ``status`` prints the build's steps; ``--logs`` adds the tail of every
  failed step's log, with URL credentials masked (Codemagic's own
  "Fetching app sources" log shows the clone URL with the password).
* ``create-app`` adds the application (``POST /apps``) with the password
  embedded in the repository URL, and records its id. It refuses when an
  ``appId`` is already recorded.

The API token is ``CODEMAGIC_API_TOKEN`` from the environment, else
``<config>/codemagic.env``, else the ForkMesh desktop's variables
(``~/projects/forkmesh/config/ForkMesh/ForkMesh.conf``, read only). No token
or password is ever printed or put on a command line.
**Dry-run is the default** for everything that changes something.
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import next_common as common  # noqa: E402
import next_git_host as git_host  # noqa: E402
from next_common import ToolError  # noqa: E402

API = "https://api.codemagic.io"
WORKFLOWS = ("ios-native-build", "ios-native-adhoc", "ios-native-release")
DONE = ("finished", "failed", "canceled", "timeout", "skipped")
MIRROR_URL = "https://forkmesh.com/forkmesh/forkmesh"
MIRROR_APP_ID = "6aac6e1e7b35578e30ad5a76"
# Codemagic clones with --depth 50 by default, which Git's dumb-HTTP
# transport refuses ("does not support shallow capabilities").
CLONE_VARIABLES = {"CM_CLONE_UNSHALLOW": "true"}
# Codemagic's "Fetching app sources" log shows the clone URL, password too.
CREDENTIALS_RE = re.compile(r"(https?://[^/\s:@]+):[^@\s/]+@")
MARKUP_RE = re.compile(r"<[^>]+>")


def redact(text: str) -> str:
    return CREDENTIALS_RE.sub(r"\1:***@", text)
DESKTOP_CONF = Path.home() / "projects" / "forkmesh" / "config" / "ForkMesh" / "ForkMesh.conf"


def state_path() -> Path:
    return common.config_dir() / "codemagic-next.json"


def load_state() -> dict:
    return common.load_json(state_path(), {}) or {}


def desktop_token(path=None) -> str:
    """CODEMAGIC_API_TOKEN from the desktop's QSettings ``variables`` line
    (``variables="@ByteArray({...})"``, JSON with escaped quotes)."""
    try:
        text = Path(path or DESKTOP_CONF).read_text("utf-8")
    except FileNotFoundError:
        return ""
    for line in text.splitlines():
        if not line.startswith("variables="):
            continue
        raw = line[len("variables="):].strip()
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1].replace('\\"', '"').replace("\\\\", "\\")
        if raw.startswith("@ByteArray(") and raw.endswith(")"):
            raw = raw[len("@ByteArray("):-1]
        try:
            return str(json.loads(raw).get("CODEMAGIC_API_TOKEN") or "")
        except ValueError:
            return ""
    return ""


def api_token() -> str:
    token = os.environ.get("CODEMAGIC_API_TOKEN", "")
    env_file = common.config_dir() / "codemagic.env"
    if not token and env_file.is_file():
        token = common.read_env_file(env_file).get("CODEMAGIC_API_TOKEN", "")
    token = token or desktop_token()
    if not token:
        raise ToolError("no CODEMAGIC_API_TOKEN (environment, %s or the desktop "
                        "variables)" % env_file)
    return token


class Codemagic(common.JsonHttp):
    """Codemagic's REST API authenticates with ``x-auth-token``."""

    def __init__(self, token, opener=None):
        super().__init__(API, token, opener=opener)

    def request(self, method, path, body=None, expect=(200, 201, 202, 204)):
        import urllib.error
        import urllib.request
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(
            self.base + path, data=data, method=method,
            headers={"x-auth-token": self._token, "Content-Type": "application/json",
                     "Accept": "application/json", "User-Agent": self.user_agent})
        try:
            with self.opener(request, timeout=60) as response:
                status, raw = response.status, response.read()
        except urllib.error.HTTPError as error:
            status, raw = error.code, error.read() or b""
        if status not in expect:
            raise ToolError("%s %s -> HTTP %d: %s" % (
                method, path.split("?")[0], status, raw[:300].decode("utf-8", "replace")))
        try:
            return json.loads(raw) if raw.strip() else {}
        except ValueError:
            return {"text": raw.decode("utf-8", "replace")}


def repository_url(fleet, password) -> str:
    return "https://%s:%s@%s%s" % (git_host.GIT_USER, urllib.parse.quote(password, safe=""),
                                   fleet["app_host"], git_host.URL_PATH)


def create_app(fleet, api, runner, team=None) -> int:
    state = load_state()
    if state.get("appId"):
        raise ToolError("an app is already recorded (%s) in %s" % (state["appId"], state_path()))
    password = git_host.read_password()
    public_url = git_host.clone_url(fleet)

    def create():
        body = {"repositoryUrl": repository_url(fleet, password)}
        if team:
            body["teamId"] = team
        answer = api.request("POST", "/apps", body)
        app = answer.get("application") or answer
        app_id = app.get("_id")
        if not app_id:
            raise ToolError("Codemagic answered without an app id")
        common.save_json(state_path(), {
            "appId": app_id, "appName": app.get("appName"), "teamId": team,
            "repository": public_url, "createdAt": time.strftime(
                "%Y-%m-%dT%H:%M:%SZ", time.gmtime())})
        runner.say("  appId %s recorded in %s" % (app_id, state_path()))
        return app_id
    runner.step("POST /apps repositoryUrl=%s (password embedded, not shown)%s"
                % (public_url, " teamId=%s" % team if team else ""), create)
    return 0


def app_id() -> str:
    value = load_state().get("appId")
    if not value:
        raise ToolError("no appId in %s (create-app first)" % state_path())
    return value


def local_main(repo, run=subprocess.run) -> str:
    head = run(["git", "-C", str(repo), "rev-parse", "refs/heads/main"],
               capture_output=True, text=True, check=False)
    if head.returncode != 0:
        raise ToolError("%s has no main branch" % repo)
    return head.stdout.strip()


def served_main(url=MIRROR_URL, run=subprocess.run) -> str:
    try:
        done = run(["git", "ls-remote", url, "refs/heads/main"], capture_output=True,
                   text=True, check=False, timeout=60,
                   env=dict(os.environ, GIT_TERMINAL_PROMPT="0"))
    except subprocess.SubprocessError:
        return ""
    if done.returncode != 0:
        return ""
    return (done.stdout.split() or [""])[0]


def contains(repo, ancestor, commit, run=subprocess.run) -> bool:
    if not commit:
        return False
    if commit == ancestor:
        return True
    done = run(["git", "-C", str(repo), "merge-base", "--is-ancestor", ancestor, commit],
               capture_output=True, text=True, check=False)
    return done.returncode == 0


def wait_for_mirror(runner, repo=None, url=MIRROR_URL, run=subprocess.run, interval=20,
                    timeout=1800, sleep=time.sleep, clock=time.monotonic):
    repo = Path(repo or common.REPO)
    main = local_main(repo, run)

    def poll():
        deadline = clock() + timeout
        last = None
        while True:
            served = served_main(url, run)
            if contains(repo, main, served, run):
                runner.say("  %s serves main %s" % (url, served[:12]))
                return served
            if served != last:
                runner.say("  %s serves %s; waiting for %s" % (url, served[:12] or "nothing",
                                                              main[:12]))
                last = served
            if clock() > deadline:
                raise ToolError("%s still serves %s, not main %s, after %d minutes"
                                % (url, served[:12] or "nothing", main[:12], timeout // 60))
            sleep(interval)
    return runner.step("wait until %s serves main %s" % (url, main[:12]), poll)


def start_build(fleet, api, runner, workflow, branch="main", push=True, ssh=None,
                run=subprocess.run, mirror=False, sleep=time.sleep):
    if workflow not in WORKFLOWS:
        raise ToolError("unknown workflow %s (one of %s)" % (workflow, ", ".join(WORKFLOWS)))
    if mirror:
        if branch == "main":
            wait_for_mirror(runner, run=run, sleep=sleep)
        identifier = MIRROR_APP_ID
    else:
        if push:
            if branch != "main":
                raise ToolError("next_git_host pushes main only; use --no-push for %s" % branch)
            git_host.push(fleet, ssh or git_host.make_ssh(), runner, run=run)
        identifier = app_id()

    def trigger():
        answer = api.request("POST", "/builds", {
            "appId": identifier, "workflowId": workflow, "branch": branch,
            "environment": {"variables": CLONE_VARIABLES}})
        build_id = answer.get("buildId") or answer.get("_id")
        runner.say("  build %s: https://codemagic.io/app/%s/build/%s"
                   % (build_id, identifier, build_id))
        return build_id
    return runner.step("start %s on %s for app %s" % (workflow, branch, identifier), trigger)


def show(api, build_id, logs=False, out=print):
    answer = api.request("GET", "/builds/%s" % build_id)
    build = answer.get("build") or answer
    out("build %s: %s (%s, %s)" % (build_id, build.get("status"), build.get("fileWorkflowId")
                                   or build.get("workflowId"), build.get("branch")))
    failed = []
    for action in build.get("buildActions") or []:
        out("  %-10s %s" % (action.get("status"), action.get("name")))
        if action.get("status") == "failed":
            failed.append(action)
    for artefact in build.get("artefacts") or []:
        out("  artefact   %s" % artefact.get("name"))
    if logs:
        for action in failed:
            text = step_log(api, build_id, action.get("_id"))
            out("---- %s (last 80 lines)" % action.get("name"))
            out(redact("\n".join(text.splitlines()[-80:])))
    return build


def step_log(api, build_id, step_id) -> str:
    answer = api.request("GET", "/builds/%s/step/%s" % (build_id, step_id))
    if "text" in answer:
        return answer["text"]
    return str(answer.get("log") or answer.get("logs") or json.dumps(answer)[:4000])


def log_lines(api, build_id, step_id) -> list:
    try:
        answer = api.request("GET", "/builds/%s/step/%s" % (build_id, step_id))
    except ToolError:
        return []
    text = str(answer.get("text") or answer.get("log") or answer.get("logs") or "")
    text = redact(html.unescape(MARKUP_RE.sub("", text)))
    return [line.rstrip() for line in text.splitlines() if line.strip()]


def seconds(action) -> str:
    try:
        start = datetime.datetime.fromisoformat(action["startedAt"])
        end = datetime.datetime.fromisoformat(action["finishedAt"])
    except (KeyError, TypeError, ValueError):
        return ""
    return " (%ds)" % max(0, round((end - start).total_seconds()))


def wait(api, build_id, interval=15, timeout=7200, sleep=time.sleep, out=print, tail=20,
         clock=time.monotonic):
    deadline = clock() + timeout
    last = None
    started, ended, printed = set(), set(), {}

    def follow(action, final=False):
        lines = log_lines(api, build_id, action.get("_id"))
        fresh = lines[printed.get(action.get("_id"), 0):]
        printed[action.get("_id")] = len(lines)
        if final and action.get("status") == "failed":
            return
        if len(fresh) > tail:
            out("    ... %d more lines" % (len(fresh) - tail))
            fresh = fresh[-tail:]
        for line in fresh:
            out("    " + line)

    while True:
        build = (api.request("GET", "/builds/%s" % build_id).get("build") or {})
        status = build.get("status")
        if status != last:
            out("  %s build %s" % (time.strftime("%H:%M:%S"), status))
            last = status
        for action in build.get("buildActions") or []:
            step = action.get("_id")
            if not action.get("startedAt") or step in ended:
                continue
            if step not in started:
                started.add(step)
                out("  %s > %s" % (time.strftime("%H:%M:%S"), action.get("name")))
            if action.get("finishedAt"):
                ended.add(step)
                follow(action, final=True)
                out("  %s %s %s%s" % (time.strftime("%H:%M:%S"), action.get("status"),
                                      action.get("name"), seconds(action)))
            else:
                follow(action)
        if status in DONE or clock() > deadline:
            return status
        sleep(interval)


def main(argv=None, api=None, ssh=None, run=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("build")
    p.add_argument("workflow", choices=WORKFLOWS)
    p.add_argument("--branch", default="main")
    p.add_argument("--no-push", action="store_true")
    p.add_argument("--mirror", action="store_true",
                   help="build on the forkmesh app from the public mirror; push nothing")
    p.add_argument("--wait", action="store_true")
    common.add_execute_flags(p)
    p = sub.add_parser("status")
    p.add_argument("build_id")
    p.add_argument("--wait", action="store_true")
    p.add_argument("--logs", action="store_true")
    p = sub.add_parser("create-app")
    p.add_argument("--team", help="Codemagic team id")
    common.add_execute_flags(p)
    args = parser.parse_args(argv)
    try:
        fleet = common.load_fleet(args.fleet)
        runner = common.runner_from_args(args)
        client = api or Codemagic(api_token())
        if args.command == "create-app":
            code = create_app(fleet, client, runner, args.team)
        elif args.command == "status":
            if args.wait:
                wait(client, args.build_id)
            build = show(client, args.build_id, logs=args.logs)
            return 1 if build.get("status") in ("failed", "timeout", "canceled") else 0
        else:
            build_id = start_build(fleet, client, runner, args.workflow, args.branch,
                                   push=not args.no_push, ssh=ssh, run=run or subprocess.run,
                                   mirror=args.mirror)
            code = 0
            if build_id and args.wait:
                wait(client, build_id)
                code = 0 if show(client, build_id, logs=True).get("status") == "finished" else 1
        if not runner.execute:
            runner.say("(dry run: nothing was changed; pass --execute to apply)")
        return code
    except (ToolError, OSError, KeyError, subprocess.SubprocessError) as error:
        print("next_codemagic: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
