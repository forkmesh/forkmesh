import base64
import json
import re
from urllib.parse import parse_qs, quote, urlencode, urlparse


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


INTERNAL_ORIGIN = "https://forkmesh.internal"
PULL_RULES_SCHEMA = "forkmesh-pull-rules-v1"
ORG_PULL_RULES_MAX_BYTES = 6 * 1024
MAX_RULES = 32
MAX_PATTERNS = 32
MAX_SECRET_PATTERNS = 16
MAX_BRANCHES = 16
MAX_MODELS = 16
MAX_ORGS_PER_REPO = 4
RULE_KINDS = ("tests", "docs", "size", "paths", "secrets", "commits", "model")
RULE_FIELDS = frozenset({
    "id", "title", "kind", "branches", "enforcement", "suggestion", "paths",
    "testPaths", "docsPaths", "maxFiles", "maxLines", "patterns",
    "subjectPattern", "maxSubjectLength", "models",
})
RULE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,47}$")
BRANCH_PATTERN_RE = re.compile(r"^[A-Za-z0-9*?][A-Za-z0-9._/*?-]{0,199}$")
MODEL_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$")
REF_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$")
NON_RE2_RE = re.compile(r"\(\?[=!<]|\(\?P=|\\[1-9]|\(\?>|[*+?}][+]")
LIST_FIELDS = (
    ("branches", MAX_BRANCHES), ("paths", MAX_PATTERNS),
    ("testPaths", MAX_PATTERNS), ("docsPaths", MAX_PATTERNS),
    ("patterns", MAX_SECRET_PATTERNS), ("models", MAX_MODELS),
)
INT_FIELDS = (
    ("maxFiles", 100000), ("maxLines", 10000000), ("maxSubjectLength", 1000),
)
SUPPORTED_MODELS = (
    "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5", "claude-fable-5",
    "claude-haiku-4-5", "claude-sonnet-4-6", "claude-opus-4-8",
    "gpt-5.6-sol", "gpt-5.6-luna", "gpt-5.6-terra", "gpt-5.3-codex-spark",
)


class RulesError(Exception):
    pass


def _text(value, field, limit, multiline=False):
    if not isinstance(value, str) or len(value) > limit:
        raise RulesError("%s must be text of at most %d characters" % (
            field, limit))
    for char in value:
        if ord(char) < 32 and not (multiline and char == "\n"):
            raise RulesError("%s holds a control character" % field)
    return value


def _regex(value, field):
    _text(value, field, 200)
    if not value:
        raise RulesError("%s is empty" % field)
    if NON_RE2_RE.search(value):
        raise RulesError(
            "%s uses lookaround, backreferences or possessive repeats, "
            "which the rule checker does not support" % field)
    try:
        re.compile(value)
    except re.error:
        raise RulesError("%s is not a valid regular expression" % field)
    return value


def _normalize_rule(value, seen):
    if not isinstance(value, dict):
        raise RulesError("each rule must be an object")
    unknown = sorted(set(value) - RULE_FIELDS)
    if unknown:
        raise RulesError("unknown rule field %s" % unknown[0])
    rule_id = value.get("id")
    if not isinstance(rule_id, str) or not RULE_ID_RE.fullmatch(rule_id):
        raise RulesError(
            "rule ids use lowercase letters, digits and dashes (up to 48)")
    if rule_id in seen:
        raise RulesError("rule id %s is used twice" % rule_id)
    seen.add(rule_id)
    kind = value.get("kind")
    if kind not in RULE_KINDS:
        raise RulesError("rule %s has an unknown kind" % rule_id)
    rule = {"id": rule_id, "kind": kind}
    title = _text(value.get("title", ""), "title", 120)
    if title:
        rule["title"] = title
    suggestion = _text(value.get("suggestion", ""), "suggestion", 600, True)
    if suggestion:
        rule["suggestion"] = suggestion
    enforcement = value.get("enforcement", "")
    if enforcement not in ("", "block", "warn"):
        raise RulesError("rule %s enforcement must be block or warn" % rule_id)
    if enforcement:
        rule["enforcement"] = enforcement
    for field, limit in LIST_FIELDS:
        items = value.get(field, [])
        if not isinstance(items, list) or len(items) > limit:
            raise RulesError("%s holds at most %d entries" % (field, limit))
        cleaned = []
        for item in items:
            if field == "branches":
                if not isinstance(item, str) or not BRANCH_PATTERN_RE.fullmatch(
                        item):
                    raise RulesError("branch pattern %r is invalid" % (item,))
            elif field == "models":
                if not isinstance(item, str) or not MODEL_RE.fullmatch(item):
                    raise RulesError("model %r is invalid" % (item,))
                if item in cleaned:
                    raise RulesError("model %s is listed twice" % item)
            elif field == "patterns":
                _regex(item, "secret pattern")
            else:
                _text(item, field, 200)
                if not item:
                    raise RulesError("%s holds an empty pattern" % field)
            cleaned.append(item)
        if cleaned:
            rule[field] = cleaned
    for field, maximum in INT_FIELDS:
        number = value.get(field, 0)
        if isinstance(number, bool) or not isinstance(number, int) or not (
                0 <= number <= maximum):
            raise RulesError("%s must be a whole number up to %d" % (
                field, maximum))
        if number:
            rule[field] = number
    subject = value.get("subjectPattern", "")
    if subject:
        rule["subjectPattern"] = _regex(subject, "subjectPattern")
    if kind == "paths" and not rule.get("paths"):
        raise RulesError("rule %s names no protected paths" % rule_id)
    if kind == "size" and not (rule.get("maxFiles") or rule.get("maxLines")):
        raise RulesError("rule %s needs a file or line limit" % rule_id)
    if kind == "commits" and not (
            rule.get("subjectPattern") or rule.get("maxSubjectLength")):
        raise RulesError(
            "rule %s needs a subject pattern or length limit" % rule_id)
    if kind == "model" and not rule.get("models"):
        raise RulesError("rule %s names no allowed model" % rule_id)
    return rule


def normalize_policy(value):
    if not isinstance(value, dict):
        raise RulesError("the rules must be an object")
    unknown = sorted(set(value) - {"schema", "rules"})
    if unknown:
        raise RulesError("unknown field %s" % unknown[0])
    if value.get("schema", PULL_RULES_SCHEMA) != PULL_RULES_SCHEMA:
        raise RulesError("unsupported rules schema")
    rules = value.get("rules", [])
    if not isinstance(rules, list) or len(rules) > MAX_RULES:
        raise RulesError("a rule set holds at most %d rules" % MAX_RULES)
    seen = set()
    policy = {
        "schema": PULL_RULES_SCHEMA,
        "rules": [_normalize_rule(rule, seen) for rule in rules],
    }
    if len(_canonical(policy).encode("utf-8")) > ORG_PULL_RULES_MAX_BYTES:
        raise RulesError(
            "the rules are larger than %d bytes" % ORG_PULL_RULES_MAX_BYTES)
    return policy


def _canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _b64url(text):
    return base64.urlsafe_b64encode(
        text.encode("utf-8")).decode("ascii").rstrip("=")


def combine_policies(policies):
    rules = []
    used = set()
    for _name, policy in policies:
        for rule in policy.get("rules") or []:
            rule = dict(rule)
            base = rule["id"]
            suffix = 2
            while rule["id"] in used:
                tail = "-%d" % suffix
                rule["id"] = base[:48 - len(tail)] + tail
                suffix += 1
            used.add(rule["id"])
            rules.append(rule)
    if not rules:
        return None
    if len(rules) > MAX_RULES:
        raise RulesError(
            "the organizations linked to this repository set more than %d "
            "rules together" % MAX_RULES)
    combined = {"schema": PULL_RULES_SCHEMA, "rules": rules}
    if len(_canonical(combined).encode("utf-8")) > ORG_PULL_RULES_MAX_BYTES:
        raise RulesError(
            "the organizations linked to this repository set more than %d "
            "bytes of rules together" % ORG_PULL_RULES_MAX_BYTES)
    return combined


async def _org_policies(env, owner, repo):
    rows = await d1_all(
        env,
        "SELECT o.name AS name, p.data AS data FROM org_repos r "
        "JOIN orgs o ON o.org_bi=r.org_bi "
        "JOIN org_pull_rules p ON p.org_bi=r.org_bi "
        "WHERE r.node_owner=? AND r.repo=? ORDER BY o.name LIMIT ?",
        str(owner or "").lower(), str(repo or "").lower(), MAX_ORGS_PER_REPO)
    policies = []
    for row in rows or []:
        try:
            policy = json.loads(str(row.get("data") or ""))
        except ValueError:
            continue
        if isinstance(policy, dict) and policy.get("rules"):
            policies.append((str(row.get("name") or ""), policy))
    return policies


async def _evaluate(env, owner, repo, pull_number, base, head, policy):
    query = {
        "base": base, "head": head, "rules": "1", "pull": str(int(pull_number)),
    }
    if policy:
        query["orgRules"] = _b64url(_canonical(policy))
    url = "%s/api/repo/%s/%s/merge-preview?%s" % (
        INTERNAL_ORIGIN, quote(owner, safe=""), quote(repo, safe=""),
        urlencode(query))
    try:
        response = await _routed_repository_read(
            env, url, "merge-preview", bypass_cache=True)
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        value = json.loads(str(await response.text()))
    except Exception:
        return None
    rules = value.get("rules") if isinstance(value, dict) else None
    return rules if isinstance(rules, dict) else None


def _policy_error_report(message):
    return {
        "available": True, "passed": False, "blocked": True, "rules": [{
            "id": "policy", "title": "Rules can be read", "kind": "policy",
            "source": "organization", "enforcement": "block",
            "status": "failed", "problems": [message],
            "suggestion": "An organization owner or admin must fix the "
                          "rules before anything can merge.",
        }],
    }


async def evaluate_pull(env, owner, repo, pull_number, base, head):
    policies = await _org_policies(env, owner, repo)
    try:
        combined = combine_policies(policies)
    except RulesError as exc:
        return [name for name, _ in policies], _policy_error_report(
            "The organization rules could not be combined: %s." % exc)
    report = await _evaluate(env, owner, repo, pull_number, base, head,
                             combined)
    return [name for name, _ in policies], report


async def merge_gate(env, context, pull_number, base_oid, head_oid):
    policies = await _org_policies(env, context["owner"], context["repo"])
    if not policies:
        return None
    _orgs, report = await evaluate_pull(
        env, context["owner"], context["repo"], pull_number, base_oid,
        head_oid)
    if not report or report.get("available") is not True:
        return {
            "error": "rules_required",
            "message": "The organization's pull request rules could not be "
                       "checked on the repository host, so the merge stays "
                       "locked. Try again shortly.",
            "rules": report or {"available": False, "rules": []},
        }
    if report.get("blocked"):
        return {
            "error": "rules_required",
            "message": "This pull request does not meet the required rules.",
            "rules": report,
        }
    return None


async def repo_pull_rules_handler(env, request, owner, repo, pull_number):
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        context = await _https_mirror_public_context(env, owner, repo)
    except _HttpsMirrorContextUnavailable:
        return json_response(
            {"error": "mirror_unavailable"}, status=503,
            cache_control="no-store")
    if context is None:
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    params = parse_qs(urlparse(request.url).query)
    base = (params.get("base") or [""])[0]
    head = (params.get("head") or [""])[0]
    if not REF_RE.fullmatch(base) or not REF_RE.fullmatch(head) or (
            ".." in base or ".." in head):
        return json_response(
            {"error": "invalid_request"}, status=400, cache_control="no-store")
    await ensure_schema(env)
    orgs, report = await evaluate_pull(
        env, context["owner"], context["repo"], int(pull_number), base, head)
    return json_response({
        "ok": True,
        "organizations": orgs,
        "rules": report or {"available": False, "rules": []},
    }, cache_control="no-store")


async def org_pull_rules_handler(env, request, org):
    await ensure_schema(env)
    method = method_name(request)
    if method not in ("GET", "POST"):
        return json_response({"error": "method_not_allowed"}, status=405)
    org_bi, row = await _org_row(env, org)
    if not row:
        return json_response({"error": "not_found"}, status=404)
    data = {}
    if method == "POST":
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
    _, account_rec = await _account_session_record(env, request, data)
    account = str((account_rec or {}).get("name") or "").strip().lower()
    if not account:
        return json_response({"error": "invalid_session"}, status=401)
    org_name = str(row.get("name") or org)
    role = await _org_role(env, org_bi, account)
    can_manage = _org_rbac.org_role_allows(
        role, "organization.pull_rules.manage")
    if method == "GET":
        if not _org_rbac.org_role_allows(role, "organization.read"):
            return json_response({"error": "forbidden"}, status=403)
        stored = await d1_first(
            env,
            "SELECT data,updated_by,updated_at FROM org_pull_rules "
            "WHERE org_bi=?", org_bi)
        policy = None
        if stored:
            try:
                policy = json.loads(str(stored.get("data") or ""))
            except ValueError:
                policy = None
        return json_response({
            "ok": True,
            "organization": org_name,
            "policy": policy or {"schema": PULL_RULES_SCHEMA, "rules": []},
            "canManage": can_manage,
            "updatedBy": str((stored or {}).get("updated_by") or ""),
            "updatedAt": int((stored or {}).get("updated_at") or 0),
            "models": list(SUPPORTED_MODELS),
        }, cache_control="no-store")
    if not can_manage:
        await _audit_sensitive_action(
            env, account, "organization.pull_rules_change", "organization",
            org_name, "denied", {"reason": "insufficient_role"})
        return json_response({"error": "forbidden"}, status=403)
    try:
        policy = normalize_policy(data.get("policy") or {})
    except RulesError as exc:
        return json_response(
            {"error": "invalid_rules", "message": str(exc)}, status=400,
            cache_control="no-store")
    now = int(Date.now())
    if policy["rules"]:
        await d1_run(
            env,
            "INSERT INTO org_pull_rules (org_bi,data,updated_by,updated_at) "
            "VALUES (?,?,?,?) ON CONFLICT(org_bi) DO UPDATE SET "
            "data=excluded.data,updated_by=excluded.updated_by,"
            "updated_at=excluded.updated_at",
            org_bi, _canonical(policy), account, now)
    else:
        await d1_run(env, "DELETE FROM org_pull_rules WHERE org_bi=?", org_bi)
    await _audit_sensitive_action(
        env, account, "organization.pull_rules_change", "organization",
        org_name, "success", {
            "rules": len(policy["rules"]),
            "kinds": sorted({rule["kind"] for rule in policy["rules"]}),
        })
    return json_response({
        "ok": True, "organization": org_name, "policy": policy,
        "canManage": True, "updatedBy": account, "updatedAt": now,
        "models": list(SUPPORTED_MODELS),
    }, cache_control="no-store")
