import json


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


FEATURE = "protectDefaultBranch"
ACTIONS = "actions"
RESERVED_PREFIX = "refs/heads/forkmesh/"
APPROVER_ROLES = frozenset({"owner", "admin"})
REVIEW_STATES = frozenset({"approved", "changes_requested", "dismissed"})
LEDGER_LIMIT = 200
CONFIG_KEYS = (FEATURE, ACTIONS)


class Unavailable(Exception):
    pass


def _lower(value):
    return str(value or "").strip().lower()


async def storage_owner(env, owner, repo):
    owner, repo = _lower(owner), _lower(repo)
    node = await _org_repo_node_strict(env, owner, repo)
    return node or owner


async def stored_settings(env, owner, repo):
    node = await storage_owner(env, owner, repo)
    key = await blind_index(
        env, "repo-feature-settings:%s/%s" % (node, _lower(repo)))
    row = await d1_first(
        env, "SELECT data FROM repo_feature_settings WHERE repo_bi=?", key)
    try:
        stored = json.loads((row or {}).get("data") or "{}")
    except ValueError:
        stored = {}
    return (stored if isinstance(stored, dict) else {}), node


async def is_protected(env, owner, repo):
    try:
        stored, _node = await stored_settings(env, owner, repo)
    except Exception:
        return True
    return bool(stored.get(FEATURE))


async def actions_off(env, owner, repo):
    try:
        stored, _node = await stored_settings(env, owner, repo)
    except Exception:
        return True
    return stored.get(ACTIONS) is False


async def default_branch(env, node, repo):
    for name in (str(repo or ""), _lower(repo)):
        record = await _repo_catalog_record(env, node, name)
        if isinstance(record, dict):
            branch = str(record.get("branch") or "").strip()
            if branch and valid_branch(branch):
                return branch
    return ""


def valid_branch(branch):
    branch = str(branch or "")
    return (0 < len(branch) <= 200 and not branch.startswith(("-", "/"))
            and not branch.endswith(("/", ".lock")) and ".." not in branch
            and "//" not in branch and "@{" not in branch
            and not any(ch in branch for ch in "\x00\n\r\t ~^:?*[\\\x7f"))


async def protected_refs(env, owner, repo):
    stored, node = await stored_settings(env, owner, repo)
    if not stored.get(FEATURE):
        return []
    branch = await default_branch(env, node, repo)
    if not branch:
        raise Unavailable("default branch unknown")
    return ["refs/heads/" + branch, RESERVED_PREFIX]


def ref_is_protected(ref, refs):
    ref = str(ref or "")
    for item in refs or ():
        if item.endswith("/") and ref.startswith(item):
            return True
        if ref == item:
            return True
    return False


def branch_is_protected(branch, refs):
    return ref_is_protected("refs/heads/" + str(branch or ""), refs)


async def push_protection(env, owner, repo):
    try:
        return await protected_refs(env, owner, repo), None
    except Exception:
        return None, "protection_unavailable"


async def branch_refusal(env, owner, repo, branches):
    refs, problem = await push_protection(env, owner, repo)
    if problem:
        return json_response(
            {"error": problem, "message": "Branch protection could not be "
             "read, so the change is refused. Try again shortly."},
            status=503, cache_control="no-store")
    for branch in branches:
        if branch and branch_is_protected(branch, refs):
            return json_response(
                {"error": "branch_protected", "branch": branch,
                 "message": "%s is protected: it changes only by merging an "
                 "approved pull request." % branch},
                status=403, cache_control="no-store")
    return None


async def ledger_key(env, owner, repo):
    node = await storage_owner(env, owner, repo)
    return await blind_index(
        env, "pull-review-ledger:%s/%s" % (node, _lower(repo)))


async def record_review(env, owner, repo, number, account, state):
    account = _lower(account)
    if (state not in REVIEW_STATES or not account
            or not valid_node_name(account)):
        return False
    try:
        number = int(number)
    except (TypeError, ValueError):
        return False
    if number <= 0:
        return False
    await d1_run(
        env,
        "INSERT INTO pull_review_ledger (repo_bi, pull_number, account, "
        "state, updated_at) VALUES (?,?,?,?,?) ON CONFLICT(repo_bi, "
        "pull_number, account) DO UPDATE SET state=excluded.state, "
        "updated_at=excluded.updated_at",
        await ledger_key(env, owner, repo), number, account, state,
        int(Date.now()))
    return True


async def ledger(env, owner, repo, number):
    rows = await d1_all(
        env,
        "SELECT account, state FROM pull_review_ledger WHERE repo_bi=? AND "
        "pull_number=? ORDER BY updated_at LIMIT ?",
        await ledger_key(env, owner, repo), int(number), LEDGER_LIMIT)
    latest = {}
    for row in rows or []:
        account = _lower(row.get("account"))
        state = str(row.get("state") or "")
        if state == "dismissed":
            latest.pop(account, None)
        elif state in REVIEW_STATES:
            latest[account] = state
    return latest


async def dismiss_approvals(env, owner, repo, number):
    dismissed = []
    for account, state in (await ledger(env, owner, repo, number)).items():
        if state == "approved" and await record_review(
                env, owner, repo, number, account, "dismissed"):
            dismissed.append(account)
    return dismissed


async def linked_orgs(env, node, repo):
    rows = await d1_all(
        env,
        "SELECT org_bi FROM org_repos WHERE node_owner=? AND repo=? LIMIT 8",
        node, _lower(repo))
    return [row.get("org_bi") for row in rows or [] if row.get("org_bi")]


async def may_approve(env, node, repo, orgs, account):
    if not orgs:
        return account == node
    for org_bi in orgs:
        if await _org_role(env, org_bi, account) in APPROVER_ROLES:
            return True
    return False


async def pull_authors(env, record):
    names = set()
    named = _lower(record.get("authorName"))
    if named:
        names.add(named)
    key = str(record.get("author") or "").strip()
    if key:
        account = _lower(await _git_push_account_for_pubkey(env, key))
        if account:
            names.add(account)
    return names


def evaluate(latest, authors, approvers_ok):
    approved = sorted(
        account for account, state in latest.items()
        if state == "approved" and account not in authors
        and approvers_ok.get(account))
    blocking = sorted(
        account for account, state in latest.items()
        if state == "changes_requested" and approvers_ok.get(account))
    if blocking:
        return {"error": "changes_requested", "accounts": blocking,
                "message": "An organization owner or admin asked for "
                           "changes; their review must be approved or "
                           "dismissed first."}
    if not approved:
        return {"error": "approval_required",
                "message": "This branch is protected: an organization owner "
                           "or admin other than the author must approve the "
                           "pull request before it can be merged."}
    return None


async def merge_refusal(env, owner, repo, pull_number, override_review):
    try:
        refs = await protected_refs(env, owner, repo)
    except Exception:
        return 503, {"error": "protection_unavailable",
                     "message": "Branch protection could not be read, so "
                                "the merge stays locked. Try again shortly."}
    if not refs:
        return None
    record = await _pull_lifecycle_record(env, owner, repo, pull_number)
    if not record:
        return 503, {"error": "pull_unavailable",
                     "message": "The pull request could not be read, so the "
                                "merge stays locked. Try again shortly."}
    base = str(record.get("base") or "").strip()
    if base and not branch_is_protected(base, refs):
        return None
    if override_review:
        return 403, {"error": "review_waiver_not_allowed",
                     "message": "The review gate cannot be waived on a "
                                "protected branch."}
    node = await storage_owner(env, owner, repo)
    orgs = await linked_orgs(env, node, repo)
    latest = await ledger(env, owner, repo, pull_number)
    authors = await pull_authors(env, record)
    approvers_ok = {}
    for account in latest:
        approvers_ok[account] = await may_approve(
            env, node, repo, orgs, account)
    refusal = evaluate(latest, authors, approvers_ok)
    if refusal:
        return 409, refusal
    return None


async def may_configure(env, actor, owner, repo):
    actor = _lower(actor)
    if not actor:
        return False
    node = await storage_owner(env, owner, repo)
    orgs = await linked_orgs(env, node, repo)
    if not orgs:
        return actor == node
    for org_bi in orgs:
        if await _org_role(env, org_bi, actor) in APPROVER_ROLES:
            return True
    return False


SETTINGS_ONLY_FIELDS = frozenset({
    "features", "fediverse", "sessionToken", "ownerAccount", "owner",
    "repo"})


async def settings_only(env, data, owner, repo):
    if not isinstance(data, dict) or set(data) - SETTINGS_ONLY_FIELDS:
        return False
    node = await storage_owner(env, owner, repo)
    for name in (str(repo or ""), _lower(repo)):
        record = await _repo_catalog_record(env, node, name)
        if isinstance(record, dict):
            return record.get("source") == "hosted-web"
    return False
