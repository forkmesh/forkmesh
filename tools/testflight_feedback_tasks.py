#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import io
import json
import os
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "native" / "ios" / "tools"))

import forkmesh_mcp_server as mcp  # noqa: E402
import mac_adhoc_deploy  # noqa: E402
from asc_bundle_ids import API  # noqa: E402
from asc_testflight import Session, app_id  # noqa: E402

KINDS = {"screenshot": "betaFeedbackScreenshotSubmissions", "crash": "betaFeedbackCrashSubmissions"}
MARKER = "TestFlight feedback id:"
REPOSITORY = "forkmesh/forkmesh"
BODY_BUDGET = 60 * 1024
MAX_IMAGES = 4
MAX_COMMENT = 2000
MAX_TITLE = 160
SIDES = (1280, 1024, 900, 800, 700, 600, 500, 400, 320)
QUALITIES = (70, 55, 40)


def say(message: str) -> None:
    print(message, flush=True)


def feedback(api, app: str) -> list[dict]:
    entries = []
    for kind, collection in KINDS.items():
        path = f"/v1/apps/{app}/{collection}?" + urllib.parse.urlencode(
            {"include": "build,tester", "sort": "createdDate", "limit": "200"})
        while path:
            found = api("GET", path)
            extra = {(row["type"], row["id"]): row["attributes"] for row in found.get("included", [])}
            for row in found.get("data", []):
                links = row.get("relationships", {})
                build = (links.get("build") or {}).get("data") or {}
                tester = (links.get("tester") or {}).get("data") or {}
                entries.append({
                    "kind": kind, "id": row["id"], "attributes": row["attributes"],
                    "build": extra.get((build.get("type"), build.get("id")), {}),
                    "tester": extra.get((tester.get("type"), tester.get("id")), {}),
                })
            following = (found.get("links") or {}).get("next") or ""
            path = following[len(API):] if following.startswith(API) else ""
    return sorted(entries, key=lambda entry: entry["attributes"].get("createdDate") or "")


def key(entry: dict) -> str:
    return f"{entry['kind']} {entry['id']}"


def marked(task: dict) -> str:
    for line in str(task.get("details") or "").splitlines():
        if line.startswith(MARKER):
            return line[len(MARKER):].strip()
    return ""


def known_tasks(tasks) -> dict[str, dict]:
    found = {}
    for status in ("all", "deleted"):
        for task in tasks(status)["tasks"]:
            feedback_key = marked(task)
            if feedback_key and feedback_key not in found:
                found[feedback_key] = {**task, "trashed": status == "deleted"}
    return found


def shrink(raw: bytes, budget: int) -> bytes | None:
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("ERROR: Pillow is missing; install python3-pil to attach screenshots.") from None
    try:
        image = Image.open(io.BytesIO(raw))
        image.load()
    except (OSError, ValueError):
        return None
    image = image.convert("RGB")
    for side in SIDES:
        sized = image.copy()
        sized.thumbnail((side, side))
        for quality in QUALITIES:
            out = io.BytesIO()
            sized.save(out, "JPEG", quality=quality, optimize=True)
            if out.tell() <= budget:
                return out.getvalue()
    return None


def download(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read(16 * 1024 * 1024)


def day(stamp: str | None) -> str:
    return (stamp or "")[:16].replace("T", " ") or "?"


def title(entry: dict) -> str:
    comment = " ".join((entry["attributes"].get("comment") or "").split())
    label = "TestFlight crash" if entry["kind"] == "crash" else "TestFlight"
    text = f"{label}: {comment or 'feedback with no comment'}"
    return text if len(text) <= MAX_TITLE else text[:MAX_TITLE - 1].rstrip() + "…"


def details(entry: dict, app: str, notes: list[str]) -> str:
    a = entry["attributes"]
    tester = " ".join(filter(None, [entry["tester"].get("firstName"), entry["tester"].get("lastName")]))
    who = ", ".join(filter(None, [tester, a.get("email")])) or "anonymous tester"
    device = " ".join(filter(None, [a.get("deviceModel"), a.get("osVersion")])) or "unknown device"
    comment = (a.get("comment") or "").strip()[:MAX_COMMENT] or "(no comment)"
    lines = [
        comment,
        "",
        f"- From: {who}",
        f"- Device: {device}, {a.get('locale') or '?'}, {a.get('connectionType') or '?'}",
        f"- Build: {entry['build'].get('version') or '?'} ({a.get('buildBundleId') or '?'})",
        f"- Sent: {day(a.get('createdDate'))} UTC",
        *[f"- {note}" for note in notes],
        "",
        f"App Store Connect: https://appstoreconnect.apple.com/apps/{app}/testflight/ios",
        "When this task is done, the next run of the \"Turn TestFlight feedback into tasks\" "
        "shortcut deletes this feedback from App Store Connect.",
        "",
        f"{MARKER} {key(entry)}",
    ]
    return "\n".join(lines)


def attachments(entry: dict, base_size: int, fetch=download) -> tuple[list[dict], list[str]]:
    shots = (entry["attributes"].get("screenshots") or [])[:MAX_IMAGES]
    if not shots:
        return [], []
    budget = (BODY_BUDGET - base_size) * 3 // 4 // len(shots) - 256
    found, notes = [], []
    for index, shot in enumerate(shots, 1):
        try:
            raw = fetch(shot["url"])
        except (OSError, KeyError, ValueError) as error:
            notes.append(f"Screenshot {index} could not be downloaded: {error}")
            continue
        small = shrink(raw, budget)
        if small is None:
            notes.append(f"Screenshot {index} could not be made small enough to attach")
            continue
        found.append({"name": f"testflight-{entry['id'][:12]}-{index}.jpg", "mime": "image/jpeg",
                      "size": len(small), "file": base64.b64encode(small).decode("ascii")})
    if found:
        sizes = ", ".join(f"{shot.get('width')}x{shot.get('height')}" for shot in shots)
        notes.insert(0, f"Screenshots: {len(found)} attached, shrunk from {sizes}")
    return found, notes


def task_fields(entry: dict, app: str, repository: str, fetch=download) -> dict:
    fields = {"title": title(entry), "details": details(entry, app, ["Screenshots: x attached"]),
              "repository": repository}
    base = len(json.dumps(fields, separators=(",", ":")))
    images, notes = attachments(entry, base, fetch)
    fields["details"] = details(entry, app, notes)
    if images:
        fields["attachments"] = images
    return fields


def create(fields: dict) -> dict:
    body = {**fields, "destination": "department", "assigneeKind": "unassigned"}
    answer = mcp.task_request("POST", "/api/tasks", ["forkmesh-org-task-open-v1"], body)
    return answer.get("task") if isinstance(answer.get("task"), dict) else answer


def delete_feedback(api, entry: dict) -> bool:
    try:
        api("DELETE", f"/v1/{KINDS[entry['kind']]}/{entry['id']}")
    except SystemExit as error:
        if "answered 404" in str(error):
            return True
        say(f"  could not delete it: {str(error)[:300]}")
        return False
    return True


def run(api, app: str, tasks, make_task, repository: str, dry_run: bool, fetch=download) -> int:
    entries = feedback(api, app)
    known = known_tasks(tasks)
    say(f"{len(entries)} TestFlight feedback item{'s' if len(entries) != 1 else ''} in App Store Connect, "
        f"{len(known)} already on the Tasks board")
    made = removed = failed = 0
    for entry in entries:
        task = known.get(key(entry))
        label = f"{day(entry['attributes'].get('createdDate'))} {entry['kind']} {entry['id']}"
        if task is None:
            fields = task_fields(entry, app, repository, fetch)
            if dry_run:
                say(f"would add task for {label}: {fields['title']} "
                    f"({len(fields.get('attachments', []))} image(s))")
                continue
            try:
                created = make_task(fields)
            except ValueError as error:
                say(f"could not add a task for {label}: {error}")
                failed += 1
                continue
            made += 1
            say(f"added task {created.get('id', '')} for {label}: {fields['title']} "
                f"({len(fields.get('attachments', []))} image(s))")
        elif task.get("completedAt") and not task.get("trashed"):
            if dry_run:
                say(f"would delete {label} from App Store Connect: task {task.get('id')} is done")
                continue
            say(f"task {task.get('id')} is done; deleting {label} from App Store Connect")
            if delete_feedback(api, entry):
                removed += 1
            else:
                failed += 1
    if not dry_run:
        say(f"{made} task{'s' if made != 1 else ''} added, {removed} feedback item"
            f"{'s' if removed != 1 else ''} marked done (deleted from App Store Connect)")
    return 1 if failed else 0


def credentials() -> None:
    os.environ.update(mac_adhoc_deploy.asc_ids())
    path = Path.home() / ".appstoreconnect" / "private_keys" / f"AuthKey_{os.environ['ASC_KEY_ID']}.p8"
    if not path.is_file():
        raise SystemExit(f"ERROR: no App Store Connect key at {path} (docs/native/IOS_RELEASE.md)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Turn TestFlight tester feedback into tasks with its screenshots, and delete "
                    "the feedback from App Store Connect once its task is done.")
    parser.add_argument("--dry-run", action="store_true", help="show what would change, change nothing")
    parser.add_argument("--repository", default=REPOSITORY, help="repository the tasks are filed under")
    args = parser.parse_args(argv)
    try:
        credentials()
    except (mac_adhoc_deploy.ToolError, mac_adhoc_deploy.RELEASE.CompletionError,
            OSError, subprocess.SubprocessError) as error:
        print(f"testflight_feedback_tasks: {error}", file=sys.stderr)
        return 1
    api = Session()
    return run(api, app_id(api), lambda status: mcp.list_tasks(status, limit=2000, full=True),
               create, args.repository, args.dry_run)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
