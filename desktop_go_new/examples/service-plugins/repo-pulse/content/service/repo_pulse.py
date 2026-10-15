import json
import os
import sys
import threading
import time

STATE_FILE = "watched.json"
POLL_SECONDS = 30

lock = threading.Lock()
out_lock = threading.Lock()
watched = {}
stopping = threading.Event()


def send(message):
    with out_lock:
        sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
        sys.stdout.flush()


def head_of(path):
    git = os.path.join(path, ".git")
    if os.path.isfile(git):
        with open(git, encoding="utf-8") as handle:
            text = handle.read().strip()
        if text.startswith("gitdir:"):
            git = os.path.normpath(os.path.join(path, text[7:].strip()))
    try:
        with open(os.path.join(git, "HEAD"), encoding="utf-8") as handle:
            head = handle.read().strip()
    except OSError:
        return None, None
    if not head.startswith("ref: "):
        return "(detached)", head[:12]
    ref = head[5:]
    branch = ref.rsplit("/", 1)[-1] if ref.startswith("refs/heads/") else ref
    try:
        with open(os.path.join(git, ref), encoding="utf-8") as handle:
            return branch, handle.read().strip()[:12]
    except OSError:
        pass
    try:
        with open(os.path.join(git, "packed-refs"), encoding="utf-8") as handle:
            for line in handle:
                parts = line.strip().split(" ")
                if len(parts) == 2 and parts[1] == ref:
                    return branch, parts[0][:12]
    except OSError:
        pass
    return branch, ""


def load():
    try:
        with open(STATE_FILE, encoding="utf-8") as handle:
            for path in json.load(handle):
                if isinstance(path, str):
                    watched[path] = {"branch": None, "commit": None, "changedMs": 0}
    except (OSError, ValueError):
        pass


def save():
    with open(STATE_FILE + ".tmp", "w", encoding="utf-8") as handle:
        json.dump(sorted(watched), handle)
    os.replace(STATE_FILE + ".tmp", STATE_FILE)


def poll():
    changed = []
    with lock:
        for path, state in watched.items():
            branch, commit = head_of(path)
            if (branch, commit) != (state["branch"], state["commit"]):
                first = state["commit"] is None
                state.update(branch=branch, commit=commit, changedMs=int(time.time() * 1000))
                if not first:
                    changed.append({"path": path, "branch": branch, "commit": commit})
    for item in changed:
        send({"event": "changed", "data": item})


def ticker():
    while not stopping.wait(POLL_SECONDS):
        poll()


def card():
    rows = []
    with lock:
        for path, state in sorted(watched.items()):
            missing = state["commit"] is None
            rows.append({
                "id": path,
                "label": os.path.basename(path) or path,
                "value": "not a repository" if missing else f"{state['branch']} @ {state['commit']}",
                "tone": "warning" if missing else "neutral",
            })
    return {
        "title": "Repository pulse",
        "subtitle": f"{len(rows)} watched on this desktop" if rows else "Nothing watched yet",
        "rows": rows,
        "actions": [{"id": "poll", "title": "Check now", "method": "poll"}],
        "refreshSeconds": 300,
    }


def handle(message):
    method = message.get("method")
    params = message.get("params") or {}
    if method == "plugin.init":
        return {"ready": True, "watched": len(watched)}
    if method == "card":
        return card()
    if method == "status":
        with lock:
            return {"watched": {path: dict(state) for path, state in watched.items()}}
    if method == "poll":
        poll()
        return card()
    if method in ("watch", "unwatch"):
        path = params.get("path")
        if not isinstance(path, str) or not os.path.isabs(path):
            raise ValueError("bad_params")
        with lock:
            if method == "watch":
                watched.setdefault(os.path.normpath(path), {"branch": None, "commit": None, "changedMs": 0})
            else:
                watched.pop(os.path.normpath(path), None)
            save()
        poll()
        return card()
    raise ValueError("unknown_method")


def main():
    load()
    poll()
    threading.Thread(target=ticker, daemon=True).start()
    for line in sys.stdin:
        try:
            message = json.loads(line)
        except ValueError:
            continue
        method = message.get("method")
        if method == "plugin.shutdown":
            break
        if method == "$/cancel" or "id" not in message:
            continue
        try:
            send({"id": message["id"], "ok": True, "result": handle(message)})
        except ValueError as error:
            send({"id": message["id"], "ok": False, "error": str(error)})
        except Exception:
            send({"id": message["id"], "ok": False, "error": "plugin_error"})
    stopping.set()


if __name__ == "__main__":
    main()
