import json
import re

import repo_short_urls
import server_update


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


SETTING_KEY = "self_update"
LAST_KEY = "self_update_last"
OWNER_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,79}$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,80}$")
BRANCH_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,120}$")
OUTCOMES = ("requested", "queued", "write_failed", "unprotected")


def parse(doc):
    try:
        value = json.loads(str(doc or "")[:4096] or "{}")
    except ValueError:
        value = {}
    if not isinstance(value, dict):
        value = {}
    return {"auto": value.get("auto") is True}


def parse_last(doc):
    try:
        value = json.loads(str(doc or "")[:4096] or "{}")
    except ValueError:
        value = {}
    if not isinstance(value, dict):
        return {}
    outcome = str(value.get("outcome") or "")
    if outcome not in OUTCOMES:
        return {}
    try:
        pull = max(0, int(value.get("pull") or 0))
        at = max(0, int(value.get("at") or 0))
    except (TypeError, ValueError):
        return {}
    return {
        "outcome": outcome, "pull": pull, "at": at,
        "actor": str(value.get("actor") or "")[:64],
        "repo": str(value.get("repo") or "")[:170],
        "branch": str(value.get("branch") or "")[:120],
        "commit": server_update.commit(value.get("commit"))["commit"],
    }


def target(source, origin, short_owner=""):
    source = str(source or "").strip()
    origin = str(origin or "").strip().rstrip("/")
    if not origin.startswith(("https://", "http://")) or not source.startswith(
            origin + "/"):
        return None
    path = source[len(origin) + 1:]
    if path.endswith(".git"):
        path = path[:-4]
    parts = path.split("/")
    if len(parts) == 2:
        owner, repo = parts
    elif len(parts) == 1 and short_owner:
        owner, repo = short_owner, parts[0]
    else:
        return None
    owner = owner.lower()
    if not OWNER_RE.fullmatch(owner) or not REPO_RE.fullmatch(repo) or \
            repo.startswith("."):
        return None
    return owner, repo


async def _doc(env, key):
    row = await d1_first(  # noqa: F821
        env, "SELECT doc FROM instance_settings WHERE k=?", key)
    return (row or {}).get("doc") or ""


async def _write(env, key, value):
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at",
        key, json.dumps(value, sort_keys=True, separators=(",", ":")),
        int(Date.now()))  # noqa: F821


async def setting(env):
    return parse(await _doc(env, SETTING_KEY))


async def last(env):
    return parse_last(await _doc(env, LAST_KEY))


async def save(env, auto):
    await _write(env, SETTING_KEY, {"auto": bool(auto)})


async def current_target(env):
    info = server_update.build_source(env)
    if not BRANCH_RE.fullmatch(info["ref"] or ""):
        return None
    short = repo_short_urls.parse(await _doc(env, repo_short_urls.SETTING_KEY))
    found = target(info["source"], getattr(env, "PUBLIC_BASE_URL", ""),
                   short["owner"] if short["short"] else "")
    if not found:
        return None
    return found[0], found[1], info["ref"]


async def same_repository(env, owner, repo, wanted_owner, wanted_repo):
    if str(repo or "").lower() != str(wanted_repo or "").lower():
        return False
    protection = _branch_protection  # noqa: F821
    return await protection.storage_owner(env, owner, repo) == (
        await protection.storage_owner(env, wanted_owner, wanted_repo))


async def source_protected(env, owner, repo, branch):
    protection = _branch_protection  # noqa: F821
    try:
        refs = await protection.protected_refs(env, owner, repo)
    except Exception:
        return False
    return protection.branch_is_protected(branch, refs)


async def on_merge(env, owner, repo, pull_number, actor, result):
    if server_update.mode(env) != "instance":
        return ""
    if not (await setting(env))["auto"]:
        return ""
    wanted = await current_target(env)
    if not wanted:
        return ""
    wanted_owner, wanted_repo, branch = wanted
    if not await same_repository(env, owner, repo, wanted_owner,
                                 wanted_repo):
        return ""
    record = await _pull_lifecycle_record(  # noqa: F821
        env, owner, repo, pull_number)
    if str((record or {}).get("base") or "").strip() != branch:
        return ""
    status = server_update.instance_status(env)
    if not await source_protected(env, wanted_owner, wanted_repo, branch):
        outcome = "unprotected"
    elif status["requested"] in ("update", "auto"):
        outcome = "queued"
    elif server_update.request_instance(env, "auto"):
        outcome = "requested"
    else:
        outcome = "write_failed"
    merged = str((result or {}).get("baseAfter") or "")
    entry = {
        "outcome": outcome, "pull": int(pull_number), "at": int(Date.now()),  # noqa: F821
        "actor": str(actor or ""), "repo": wanted_owner + "/" + wanted_repo,
        "branch": branch, "commit": merged,
    }
    await _write(env, LAST_KEY, entry)
    await _audit_sensitive_action(  # noqa: F821
        env, actor, "admin.auto_update", "server",
        wanted_owner + "/" + wanted_repo,
        "refused" if outcome in ("write_failed", "unprotected") else "success",
        {"pullNumber": int(pull_number), "branch": branch,
         "commit": merged, "outcome": outcome})
    return outcome
