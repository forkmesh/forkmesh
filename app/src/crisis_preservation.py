"""Preservation-first crisis onboarding, reference implementation.

Turns a maintainer's situation (planned shutdown, urgent takedown risk, lost
account, censorship, provider end-of-life) plus the time and access they still
have into an ordered preservation plan, then judges what was actually
preserved from evidence ForkMesh already produces:

- a ``forkmesh-repository-migration-report-v1`` report (the owner's
  ``GET /api/repository-imports/migrations/{id}/report``), and
- a ``forkmesh-availability-proof-v1`` report (``availability`` on
  ``GET /api/repo/<owner>/<repo>/mirrors``).

A category is ``preserved`` only when that evidence proves it. A category the
plan deferred, or could not reach, and for which no evidence exists is reported
as ``skipped`` or ``unavailable``, never as preserved.

The contract lives in ``conformance/crisis-preservation/v1/model.json`` with
the shared fixture beside it, and is documented in
``www/docs/crisis-preservation.md``. The module is pure: the same input always
produces the same output.
"""

import math

PLAN_SCHEMA = "forkmesh-crisis-preservation-plan-v1"
ASSESSMENT_SCHEMA = "forkmesh-crisis-preservation-assessment-v1"
DRILL_SCHEMA = "forkmesh-crisis-preservation-drill-v1"
CATALOG_SCHEMA = "forkmesh-crisis-preservation-catalog-v1"

MIGRATION_REPORT_SCHEMA = "forkmesh-repository-migration-report-v1"
AVAILABILITY_SCHEMA = "forkmesh-availability-proof-v1"

DOCS_URL = "https://forkmesh.com/docs/crisis-preservation/"

# At or below this many minutes every plan runs in urgent mode.
URGENT_MINUTES = 240
MAX_MINUTES = 365 * 24 * 60

# Planning rates. Conservative on purpose: a plan that finishes early is
# better than one that promises data the window cannot hold.
TRANSFER_MB_PER_MINUTE = 50
PROVIDER_RECORDS_PER_PAGE = 100
# Pages per minute a provider serves: a token-authenticated client, and an
# anonymous one (GitHub allows 60 unauthenticated requests an hour).
PAGES_PER_MINUTE = {"owner": 30, "read": 30, "public": 1}
# One import job copies provider data in repository_migration.PHASES order:
# issues, pull requests, and reviews; then releases and documentation; then
# labels, users, and discussion history. It cannot skip ahead, so a plan
# finishes these categories in IMPORT_ORDER or not at all. Pages per 100
# issues before releases, and after documentation:
ISSUE_EARLY_PAGE_FACTOR = 2
ISSUE_LATE_PAGE_FACTOR = 1
IMPORT_ORDER = ("releases", "documentation", "issues")
# A new mirror is verified at its first signed endpoint receipt, which the
# availability proof trusts for this long.
MIRROR_RECEIPT_MINUTES = 10

ACCESS_LEVELS = ("owner", "read", "public", "none")
ACCESS_LABELS = {
    "owner": "owner access",
    "read": "a read-scoped token",
    "public": "public access only",
    "none": "no access to the source",
}
_ACCESS_RANK = {"none": 0, "public": 1, "read": 2, "owner": 3}

ASSUMED_REPOSITORY = {
    "sizeMb": 100,
    "issues": 500,
    "releases": 20,
    "assetsMb": 200,
}
_REPOSITORY_LIMITS = {
    "sizeMb": 10_000_000,
    "issues": 10_000_000,
    "releases": 1_000_000,
    "assetsMb": 10_000_000,
}

CATEGORIES = (
    {
        "id": "refs",
        "title": "Branches, tags, and Git history",
        "migrationTypes": ("git_refs",),
        "source": "git",
        "required": True,
    },
    {
        "id": "identity_proofs",
        "title": "Maintainer identity proofs",
        "migrationTypes": (),
        "source": "local",
        "required": False,
    },
    {
        "id": "trusted_mirrors",
        "title": "Independent trusted mirrors",
        "migrationTypes": (),
        "source": "network",
        "required": False,
    },
    {
        "id": "issues",
        "title": "Issues, pull requests, reviews, and discussion",
        "migrationTypes": (
            "issues", "pull_requests", "reviews", "labels", "users",
            "collaboration_history",
        ),
        "source": "provider",
        "required": False,
    },
    {
        "id": "releases",
        "title": "Release notes and tags",
        "migrationTypes": ("releases",),
        "source": "provider",
        "required": False,
    },
    {
        "id": "documentation",
        "title": "Documentation",
        "migrationTypes": ("documentation",),
        "source": "provider",
        "required": False,
    },
    {
        "id": "artifacts",
        "title": "Release artifact files",
        "migrationTypes": ("release_assets",),
        "source": "provider",
        "required": False,
    },
)
CATEGORY_IDS = tuple(category["id"] for category in CATEGORIES)
_CATEGORY = {category["id"]: category for category in CATEGORIES}

SCENARIOS = (
    {
        "id": "planned_shutdown",
        "title": "Planned shutdown",
        "situation": "You are closing the project or its hosting on a known date and still control the account.",
        "urgent": False,
        "defaultMinutes": 30 * 24 * 60,
        "defaultAccess": "owner",
        "maxAccess": "owner",
        "drillBudgetMinutes": 120,
        "priority": ("refs", "releases", "documentation", "issues",
                     "artifacts", "identity_proofs", "trusted_mirrors"),
        "topics": ("ownership", "secrets", "external_dependencies",
                   "verification", "legal", "emergency_contacts"),
        "guidance": "There is time for everything. Import all of it, publish the artifacts, then let two independent operators prove the copy before the shutdown date.",
    },
    {
        "id": "urgent_takedown",
        "title": "Urgent takedown risk",
        "situation": "A takedown, suspension, or removal could happen at any moment.",
        "urgent": True,
        "defaultMinutes": 60,
        "defaultAccess": "owner",
        "maxAccess": "owner",
        "drillBudgetMinutes": 30,
        "priority": ("refs", "identity_proofs", "trusted_mirrors",
                     "releases", "documentation", "issues", "artifacts"),
        "topics": ("legal", "secrets", "verification", "ownership",
                   "emergency_contacts", "external_dependencies"),
        "guidance": "Get the Git history and your identity proofs out first, then a second operator. Everything the window cannot hold is listed as skipped, not preserved.",
    },
    {
        "id": "lost_account",
        "title": "Lost account",
        "situation": "You no longer control the provider account that owns the repository.",
        "urgent": False,
        "defaultMinutes": 24 * 60,
        "defaultAccess": "public",
        "maxAccess": "public",
        "drillBudgetMinutes": 60,
        "priority": ("refs", "identity_proofs", "trusted_mirrors",
                     "releases", "documentation", "issues", "artifacts"),
        "topics": ("ownership", "verification", "emergency_contacts",
                   "secrets", "legal", "external_dependencies"),
        "guidance": "Only public data is reachable without the account. Preserve it, and prove you are the maintainer with keys and proofs you still hold rather than with the lost account.",
    },
    {
        "id": "censorship",
        "title": "Censorship",
        "situation": "The repository is blocked, or likely to be blocked, in a region or by the provider.",
        "urgent": False,
        "defaultMinutes": 4 * 60,
        "defaultAccess": "public",
        "maxAccess": "owner",
        "drillBudgetMinutes": 60,
        "priority": ("refs", "trusted_mirrors", "identity_proofs",
                     "releases", "documentation", "artifacts", "issues"),
        "topics": ("legal", "verification", "ownership",
                   "emergency_contacts", "secrets", "external_dependencies"),
        "guidance": "Independence matters more than completeness: copies held by different operators in different regions keep the project readable where one path is blocked.",
    },
    {
        "id": "provider_eol",
        "title": "Provider end-of-life",
        "situation": "The hosting provider or product is being discontinued.",
        "urgent": False,
        "defaultMinutes": 7 * 24 * 60,
        "defaultAccess": "owner",
        "maxAccess": "owner",
        "drillBudgetMinutes": 120,
        "priority": ("refs", "releases", "documentation", "issues",
                     "artifacts", "identity_proofs", "trusted_mirrors"),
        "topics": ("external_dependencies", "ownership", "secrets",
                   "verification", "legal", "emergency_contacts"),
        "guidance": "Everything the provider holds disappears with it, including artifacts and integrations. Import all of it and replace every provider-only dependency before the end date.",
    },
)
SCENARIO_IDS = tuple(scenario["id"] for scenario in SCENARIOS)
_SCENARIO = {scenario["id"]: scenario for scenario in SCENARIOS}

DECISIONS = ("included", "deferred", "unavailable")

# Closed reasons a category is not planned, or is planned over budget.
REASONS = {
    "time_budget": "The time available is used up by higher-priority data.",
    "requires_token": "The source is private, so this needs a read-scoped token for the provider account.",
    "source_unreachable": "The source can no longer be reached, and no local copy holds this data.",
    "import_order": "The import copies provider data in a fixed order, and an earlier part did not fit.",
    "needs_refs": "Mirrors can only be requested once the Git history is being preserved.",
    "over_budget": "Required even though its estimate is longer than the time available.",
}

COVERAGE_STATES = (
    "preserved", "empty", "partial", "failed", "missing", "skipped",
    "unavailable",
)
_COVERED_STATES = ("preserved", "empty")

COVERAGE_LABELS = {
    "preserved": "Preserved",
    "empty": "Nothing to preserve",
    "partial": "Partly preserved",
    "failed": "Failed",
    "missing": "Planned, no evidence yet",
    "skipped": "Skipped",
    "unavailable": "Unavailable",
}

RISK_TOPICS = (
    {
        "id": "ownership",
        "title": "Ownership",
        "detail": "A copy does not transfer ownership. Keep the license, copyright notices, and contributor attribution with every copy, and record who may act for the project.",
    },
    {
        "id": "legal",
        "title": "Legal risk",
        "detail": "Each node operator answers legal requests for their own node. A lawful takedown of one copy does not remove the others, and ForkMesh cannot make an operator keep or remove a copy. Get legal advice before republishing content that is subject to a notice.",
    },
    {
        "id": "secrets",
        "title": "Secrets",
        "detail": "A mirror copies every secret ever committed. Rotate exposed credentials before mirroring, and never put provider tokens into a copy; ForkMesh never stores them.",
    },
    {
        "id": "verification",
        "title": "Verification",
        "detail": "Only evidence counts: a reconciled, signed migration report and an availability proof with verified copies. Check the report signature yourself with tools/verify_migration_report.py.",
    },
    {
        "id": "external_dependencies",
        "title": "External dependencies",
        "detail": "Release artifact files, wikis, CI logs, package registries, and links to the old provider are not copied by a metadata import. The migration report lists each one.",
    },
    {
        "id": "emergency_contacts",
        "title": "Emergency contacts",
        "detail": "Report a compromised account or leaked key to security@forkmesh.com. Name recovery contacts for your portable identity before you need them.",
    },
)
TOPIC_IDS = tuple(topic["id"] for topic in RISK_TOPICS)
_TOPIC = {topic["id"]: topic for topic in RISK_TOPICS}

# Where a maintainer can get capacity. Every route keeps the same exit:
# standard Git, the signed report export, and the portable identity kit.
EXIT_PATH = "git clone --mirror, the signed migration report export, and the portable identity kit all work without ForkMesh."
ROUTES = (
    {
        "id": "self_serve",
        "title": "Import and mirror it yourself",
        "detail": "Start a resumable import and run your own mirror node. The protocol and node software are MIT-licensed and free.",
        "provider": "you",
        "requiresPayment": False,
        "available": True,
        "href": "https://forkmesh.com/docs/repository-imports/",
    },
    {
        "id": "community_mirror",
        "title": "Ask community mirror operators",
        "detail": "Ask nodes run by other operators to mirror the repository from its Mirrors tab. Operators are independent volunteers and choose what they carry.",
        "provider": "community",
        "requiresPayment": False,
        "available": True,
        "href": "https://forkmesh.com/docs/availability-proof/#quorum_not_met",
    },
    {
        "id": "managed_recovery",
        "title": "Paid verified recovery",
        "detail": "Managed recovery is a research preview and not for sale yet. When it opens it operates a replaceable role: it gains no authority over your repository, and export stays available.",
        "provider": "forkmesh",
        "requiresPayment": True,
        "available": False,
        "href": "https://forkmesh.com/docs/business-model/",
    },
)
ROUTE_IDS = tuple(route["id"] for route in ROUTES)

DRILL_FAILURES = (
    "invalid_timing",
    "over_time_budget",
    "no_migration_report",
    "migration_report_unreconciled",
    "no_verified_mirror",
)

_COMPLETE_OUTCOMES = (
    "verified", "completed_with_failures", "completed_with_unmeasured_sources",
)
_REPORT_OUTCOMES = _COMPLETE_OUTCOMES + ("unreconciled", "abandoned", "in_progress")
_REPORT_CLASSES = ("imported", "skipped", "failed", "external")


class PlanRequestError(ValueError):
    """A plan request names an unknown scenario, access level, or value."""

    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code
        self.detail = detail


def _int(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(value):
        return int(value)
    if isinstance(value, str) and value.strip().isdigit():
        return int(value.strip())
    return None


def _flag(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on")
    return value == 1


def _ceil(numerator, denominator):
    return -(-int(numerator) // int(denominator))


def _plural(count, singular, plural):
    return "%d %s" % (count, singular if count == 1 else plural)


def _duration(minutes):
    if minutes % (24 * 60) == 0 and minutes >= 24 * 60:
        return _plural(minutes // (24 * 60), "day", "days")
    if minutes % 60 == 0 and minutes >= 60:
        return _plural(minutes // 60, "hour", "hours")
    return _plural(minutes, "minute", "minutes")


def _lower_first(text):
    return text[:1].lower() + text[1:]


def _docs(anchor):
    return DOCS_URL + "#" + anchor


def normalize_request(request):
    """Validate a plan request; raise PlanRequestError on a bad value."""
    request = request if isinstance(request, dict) else {}
    scenario_id = str(request.get("scenario") or "").strip()
    if scenario_id not in _SCENARIO:
        raise PlanRequestError(
            "unknown_scenario",
            "scenario must be one of: " + ", ".join(SCENARIO_IDS))
    scenario = _SCENARIO[scenario_id]

    minutes = request.get("minutes")
    if minutes is None or minutes == "":
        minutes = scenario["defaultMinutes"]
    else:
        minutes = _int(minutes)
        if minutes is None or not 1 <= minutes <= MAX_MINUTES:
            raise PlanRequestError(
                "invalid_minutes",
                "minutes must be a whole number from 1 to %d" % MAX_MINUTES)

    access = request.get("access")
    access = scenario["defaultAccess"] if access in (None, "") else str(access)
    if access not in _ACCESS_RANK:
        raise PlanRequestError(
            "unknown_access",
            "access must be one of: " + ", ".join(ACCESS_LEVELS))
    notes = []
    if _ACCESS_RANK[access] > _ACCESS_RANK[scenario["maxAccess"]]:
        notes.append(
            "Access was lowered from %s to %s: without the account only "
            "public data is reachable." % (access, scenario["maxAccess"]))
        access = scenario["maxAccess"]

    repository = request.get("repository")
    repository = repository if isinstance(repository, dict) else {}
    sizes = {}
    assumed = []
    for field, default in ASSUMED_REPOSITORY.items():
        raw = repository.get(field, request.get(field))
        if raw is None or raw == "":
            sizes[field] = default
            assumed.append(field)
            continue
        value = _int(raw)
        if value is None or not 0 <= value <= _REPOSITORY_LIMITS[field]:
            raise PlanRequestError(
                "invalid_" + field,
                "%s must be a whole number from 0 to %d" % (
                    field, _REPOSITORY_LIMITS[field]))
        sizes[field] = value
    private = _flag(repository.get("private", request.get("private", False)))
    local_clone = _flag(
        repository.get("localClone", request.get("localClone", False)))
    return {
        "scenario": scenario_id,
        "minutes": minutes,
        "access": access,
        "repository": dict(sizes, private=private, localClone=local_clone),
        "assumed": assumed,
        "notes": notes,
    }


def _provider_reachable(access, private):
    if access == "none":
        return False, "source_unreachable"
    if private and _ACCESS_RANK[access] < _ACCESS_RANK["read"]:
        return False, "requires_token"
    return True, ""


def _import_minutes(access, repository, category_id):
    """Minutes from the start of the import until it finishes a category."""
    rate = PAGES_PER_MINUTE.get(access, 1)
    issue_pages = _ceil(max(1, repository["issues"]), PROVIDER_RECORDS_PER_PAGE)
    pages = issue_pages * ISSUE_EARLY_PAGE_FACTOR + _ceil(
        max(1, repository["releases"]), PROVIDER_RECORDS_PER_PAGE)
    if category_id in ("documentation", "issues"):
        pages += 1
    if category_id == "issues":
        pages += issue_pages * ISSUE_LATE_PAGE_FACTOR
    return 1 + _ceil(pages, rate)


def _estimate(category_id, access, repository, via_local):
    size = repository["sizeMb"]
    transfer = _ceil(size, TRANSFER_MB_PER_MINUTE)
    if category_id == "refs":
        return 2 + transfer
    if category_id == "identity_proofs":
        return 5
    if category_id == "trusted_mirrors":
        return MIRROR_RECEIPT_MINUTES + transfer
    if category_id == "artifacts":
        return 2 + _ceil(repository["assetsMb"], TRANSFER_MB_PER_MINUTE)
    if via_local:
        # Documentation inside the repository travels with the pushed clone.
        return 2
    index = IMPORT_ORDER.index(category_id)
    before = (_import_minutes(access, repository, IMPORT_ORDER[index - 1])
              if index else 0)
    return _import_minutes(access, repository, category_id) - before


_ACTIONS = {
    "refs": {
        "kind": "start-import", "method": "POST",
        "path": "/api/repository-imports/migrations",
        "label": "Start a resumable import, and keep a git clone --mirror of your own",
    },
    "refs_local": {
        "kind": "push-local-clone", "method": "POST",
        "path": "/api/hosted-repositories",
        "label": "Create a hosted repository and git push --mirror from your local clone",
    },
    "identity_proofs": {
        "kind": "export-identity",
        "label": "Export your portable identity kit and link an external-account proof while you still can",
        "href": "https://forkmesh.com/docs/portable-identities/",
    },
    "trusted_mirrors": {
        "kind": "request-mirror",
        "label": "Ask a node run by a different operator to mirror the repository",
        "href": "https://forkmesh.com/docs/availability-proof/#quorum_not_met",
    },
    "issues": {
        "kind": "import-collaboration", "method": "POST",
        "path": "/api/repository-imports/migrations",
        "label": "Let the same import finish its issue, pull request, and review phases",
    },
    "releases": {
        "kind": "import-collaboration", "method": "POST",
        "path": "/api/repository-imports/migrations",
        "label": "Let the same import finish its release phase",
    },
    "documentation": {
        "kind": "import-collaboration", "method": "POST",
        "path": "/api/repository-imports/migrations",
        "label": "Let the same import copy the README, and mirror the wiki separately",
    },
    "documentation_local": {
        "kind": "push-local-clone", "method": "POST",
        "path": "/api/hosted-repositories",
        "label": "Documentation inside the repository travels with the local clone; the wiki does not",
    },
    "artifacts": {
        "kind": "publish-artifacts",
        "label": "Download the release artifact files and publish them as ForkMesh release assets",
        "href": "https://forkmesh.com/docs/repository-imports/#migration-reports",
    },
}


def _consequence(category, reason):
    title = category["title"]
    if category["id"] == "trusted_mirrors" and reason == "time_budget":
        return "No independent operator is asked for a verified copy in this window, so the repository still depends on one place."
    if reason == "import_order":
        return "%s come later in the import than data that did not fit, so they will not be copied in this window." % title
    if reason == "time_budget":
        return "%s will not be copied in this window. If the source disappears first, they are lost." % title
    if reason == "requires_token":
        return "%s of a private source cannot be read without a token, so they stay only at the provider." % title
    if reason == "source_unreachable":
        return "%s exist only where the source was. Ask anyone who holds a copy to mirror it." % title
    if reason == "needs_refs":
        return "Without the Git history no mirror can be requested, so no independent copy will exist."
    return "%s is required, but its estimate is longer than the time available. Start it now; the plan cannot promise it finishes." % title


def _follow_up(reason):
    if reason in ("time_budget", "import_order"):
        return "Run a follow-up import when time allows. Until evidence arrives this stays skipped, never preserved."
    if reason == "requires_token":
        return "Resume the import with a read-scoped token if you regain access."
    if reason == "source_unreachable":
        return "Check the Mirrors tab of the repository for copies other operators already hold."
    if reason == "needs_refs":
        return "Preserve the Git history first, then request mirrors."
    return "Keep the import running and check its report when it ends."


def build_plan(request):
    """Return the ordered preservation plan for a crisis request."""
    normalized = normalize_request(request)
    scenario = _SCENARIO[normalized["scenario"]]
    access = normalized["access"]
    repository = normalized["repository"]
    minutes = normalized["minutes"]
    urgent = bool(scenario["urgent"] or minutes <= URGENT_MINUTES)
    reachable, blocked = _provider_reachable(access, repository["private"])
    local_clone = repository["localClone"]

    remaining = minutes
    elapsed = 0
    import_blocked = False
    steps = []
    tradeoffs = []
    decided = {}
    for order, category_id in enumerate(scenario["priority"], start=1):
        category = _CATEGORY[category_id]
        via_local = False
        reason = ""
        if category["source"] == "provider" or category_id == "refs":
            if not reachable:
                if local_clone and category_id in ("refs", "documentation"):
                    via_local = True
                else:
                    reason = blocked
        if category_id == "trusted_mirrors" and decided.get("refs") != "included":
            reason = "needs_refs"
        in_import = category_id in IMPORT_ORDER and not via_local
        estimate = _estimate(category_id, access, repository, via_local)
        if reason:
            decision = "unavailable"
        elif in_import and import_blocked:
            decision = "deferred"
            reason = "import_order"
        elif estimate <= remaining:
            decision = "included"
        elif category["required"]:
            decision = "included"
            reason = "over_budget"
        else:
            decision = "deferred"
            reason = "time_budget"
        decided[category_id] = decision
        if in_import and decision != "included":
            import_blocked = True

        step = {
            "order": order,
            "category": category_id,
            "title": category["title"],
            "decision": decision,
            "estimateMinutes": estimate,
            "startsAtMinute": None,
            "reason": reason or None,
            "action": None,
        }
        if decision == "included":
            step["startsAtMinute"] = elapsed
            elapsed += estimate
            remaining = max(0, remaining - estimate)
            key = category_id + "_local" if via_local else category_id
            step["action"] = dict(_ACTIONS[key])
            step["viaLocalClone"] = via_local
        steps.append(step)
        if reason:
            tradeoffs.append({
                "category": category_id,
                "decision": decision,
                "reason": reason,
                "reasonDetail": REASONS[reason],
                "consequence": _consequence(category, reason),
                "followUp": _follow_up(reason),
                "href": _docs(reason),
            })

    included = [s for s in steps if s["decision"] == "included"]
    planned_minutes = sum(s["estimateMinutes"] for s in included)
    summary = [
        "%s: %d of %d categories fit in %s with %s." % (
            scenario["title"], len(included), len(steps), _duration(minutes),
            ACCESS_LABELS[access]),
    ]
    skipped = [t for t in tradeoffs if t["decision"] != "included"]
    if skipped:
        summary.append("Not planned: %s. None of it will be reported as preserved." % (
            "; ".join(_lower_first(_CATEGORY[t["category"]]["title"])
                      for t in skipped)))
    if any(t["reason"] == "over_budget" for t in tradeoffs):
        summary.append("The Git history alone needs longer than the time available.")
    if normalized["assumed"]:
        summary.append("Estimates assume default sizes for: %s." % ", ".join(
            normalized["assumed"]))

    routes = []
    for route in ROUTES:
        routes.append({
            "id": route["id"],
            "title": route["title"],
            "detail": route["detail"],
            "provider": route["provider"],
            "requiresPayment": route["requiresPayment"],
            "available": route["available"],
            "exit": EXIT_PATH,
            "href": route["href"],
        })

    return {
        "schema": PLAN_SCHEMA,
        "scenario": scenario["id"],
        "title": scenario["title"],
        "situation": scenario["situation"],
        "guidance": scenario["guidance"],
        "mode": "urgent" if urgent else "standard",
        "acknowledgementRequired": bool(urgent and tradeoffs),
        "minutesAvailable": minutes,
        "plannedMinutes": planned_minutes,
        "access": access,
        "repository": repository,
        "assumed": normalized["assumed"],
        "notes": normalized["notes"],
        "summary": summary,
        "drillBudgetMinutes": scenario["drillBudgetMinutes"],
        "steps": steps,
        "tradeoffs": tradeoffs,
        "risks": [
            {
                "id": topic_id,
                "title": _TOPIC[topic_id]["title"],
                "detail": _TOPIC[topic_id]["detail"],
                "href": _docs(topic_id),
            }
            for topic_id in scenario["topics"]
        ],
        "routes": routes,
        "href": _docs(scenario["id"]),
    }


def _report_rows(report, types):
    rows = report["reconciliation"]["rows"]
    found = [rows.get(kind) for kind in types if isinstance(rows.get(kind), dict)]
    if not found:
        return None
    totals = {key: 0 for key in ("source",) + _REPORT_CLASSES}
    for row in found:
        for key in totals:
            totals[key] += max(0, _int(row.get(key)) or 0)
    totals["unclassified"] = totals["source"] - sum(
        totals[key] for key in _REPORT_CLASSES)
    unmeasured = report["reconciliation"].get("unmeasuredSources")
    unmeasured = unmeasured if isinstance(unmeasured, list) else []
    totals["unmeasured"] = sum(
        1 for entry in unmeasured
        if isinstance(entry, dict) and entry.get("phase") in types)
    return totals


def _metadata_state(report, types):
    """(state, counts) for provider metadata judged from a migration report."""
    if report is None or not types:
        return None, None
    counts = _report_rows(report, types)
    if counts is None:
        return None, None
    outcome = report["outcome"]
    complete = (
        outcome in _COMPLETE_OUTCOMES and counts["unmeasured"] == 0
        and counts["unclassified"] == 0)
    if complete and counts["failed"] == 0 and counts["external"] == 0:
        return ("empty" if counts["source"] == 0 else "preserved"), counts
    if counts["imported"] > 0:
        return "partial", counts
    if outcome != "in_progress" and (
            counts["failed"] or counts["unclassified"] or counts["unmeasured"]):
        return "failed", counts
    return None, counts


def _valid_report(value):
    if not isinstance(value, dict) or value.get("schema") != MIGRATION_REPORT_SCHEMA:
        return None
    reconciliation = value.get("reconciliation")
    if not isinstance(reconciliation, dict) or not isinstance(
            reconciliation.get("rows"), dict):
        return None
    if value.get("outcome") not in _REPORT_OUTCOMES:
        return None
    return value


def _valid_availability(value):
    if not isinstance(value, dict) or value.get("schema") != AVAILABILITY_SCHEMA:
        return None
    if value.get("proof") not in ("quorum", "partial", "none"):
        return None
    return value


def report_reconciled(report):
    """Recompute the report's arithmetic instead of trusting its flag."""
    report = _valid_report(report)
    if report is None or report["outcome"] not in _COMPLETE_OUTCOMES:
        return False
    rows = report["reconciliation"]["rows"]
    for row in rows.values():
        if not isinstance(row, dict):
            return False
        source = _int(row.get("source"))
        parts = [_int(row.get(key)) for key in _REPORT_CLASSES]
        if source is None or source < 0 or any(p is None or p < 0 for p in parts):
            return False
        if source != sum(parts):
            return False
    return bool(rows)


def _coverage(availability, field):
    coverage = availability.get("coverage")
    coverage = coverage if isinstance(coverage, dict) else {}
    entry = coverage.get(field)
    entry = entry if isinstance(entry, dict) else {}
    return _int(entry.get("reference")), _int(entry.get("complete")) or 0


def _verified_copies(availability):
    counts = availability.get("counts")
    counts = counts if isinstance(counts, dict) else {}
    return max(0, _int(counts.get("verified")) or 0), max(
        0, _int(counts.get("failing")) or 0)


def _judge(category_id, report, availability, identity):
    """Return (state, evidence pointers, detail, counts) or (None, ...)."""
    category = _CATEGORY[category_id]
    meta_state, counts = _metadata_state(report, category["migrationTypes"])
    verified, failing = (0, 0)
    if availability is not None:
        verified, failing = _verified_copies(availability)

    if category_id == "refs":
        if availability is not None and availability["proof"] != "none" and verified:
            reference, complete = _coverage(availability, "branches")
            if reference == 0 or (reference is not None and complete >= 1):
                return ("preserved", ["availability:/coverage/branches"],
                        "A verified independent copy carries every branch.", counts)
            return ("partial", ["availability:/coverage/branches"],
                    "A verified copy exists, but no verified copy is proven to carry every branch.",
                    counts)
        if meta_state in ("preserved", "partial", "empty"):
            return ("partial", ["report:/reconciliation/rows/git_refs"],
                    "Branch and tag names were captured, but no independent copy of the Git history is verified.",
                    counts)
        if failing:
            return ("failed", ["availability:/counts/failing"],
                    "Every copy that was observed is failing its checks.", counts)
        return None, [], "", counts

    if category_id == "trusted_mirrors":
        if availability is None:
            return None, [], "", counts
        if availability["proof"] == "quorum":
            return ("preserved", ["availability:/quorum"],
                    "Independent operators hold the same verified copy.", counts)
        if availability["proof"] == "partial":
            return ("partial", ["availability:/quorum"],
                    "A copy is verified, but not yet by enough independent operators.",
                    counts)
        if failing:
            return ("failed", ["availability:/counts/failing"],
                    "Mirrors were requested, but every observed copy is failing.", counts)
        return None, [], "", counts

    if category_id == "identity_proofs":
        identity = identity if isinstance(identity, dict) else {}
        kit = identity.get("recoveryKitVerified") is True
        proof = identity.get("externalProofVerified") is True
        if kit and proof:
            return ("preserved", ["identity:/recoveryKitVerified",
                                  "identity:/externalProofVerified"],
                    "A restored identity kit and a verified external-account proof exist.",
                    counts)
        if kit or proof:
            return ("partial", ["identity:/recoveryKitVerified" if kit
                                else "identity:/externalProofVerified"],
                    "Only one of the identity kit restore and the external-account proof is verified.",
                    counts)
        return None, [], "", counts

    if category_id == "artifacts":
        if availability is not None and verified:
            reference, complete = _coverage(availability, "artifacts")
            if reference and complete >= 1:
                return ("preserved", ["availability:/coverage/artifacts"],
                        "A verified copy carries every release artifact file.", counts)
        if counts is not None and counts["source"] == 0 and meta_state == "empty":
            return ("empty", ["report:/reconciliation/rows/release_assets"],
                    "The source declares no release artifact files.", counts)
        if counts is not None and counts["external"]:
            return (None, [],
                    "%s declared by the source; the files were not copied."
                    % _plural(counts["external"], "artifact is", "artifacts are"),
                    counts)
        return None, [], "", counts

    pointer = ["report:/reconciliation/rows/" + kind
               for kind in category["migrationTypes"]]
    if meta_state == "preserved":
        return ("preserved", pointer,
                "%s imported and every source record is accounted for." % _plural(
                    counts["imported"], "record", "records"), counts)
    if meta_state == "empty":
        return ("empty", pointer, "The source has no records of this kind.", counts)
    if meta_state == "partial":
        return ("partial", pointer,
                "%d of %s imported; %d failed, %d external, %d unaccounted, %s unread." % (
                    counts["imported"], _plural(counts["source"], "record", "records"),
                    counts["failed"], counts["external"], counts["unclassified"],
                    _plural(counts["unmeasured"], "stream", "streams")), counts)
    if meta_state == "failed":
        return ("failed", pointer, "Nothing of this kind was imported.", counts)
    return None, [], "", counts


def assess(request, evidence):
    """Judge what a crisis plan actually preserved, from evidence alone."""
    plan = build_plan(request)
    evidence = evidence if isinstance(evidence, dict) else {}
    errors = []
    report = _valid_report(evidence.get("migrationReport"))
    if evidence.get("migrationReport") is not None and report is None:
        errors.append("migration_report_unreadable")
    availability = _valid_availability(evidence.get("availability"))
    if evidence.get("availability") is not None and availability is None:
        errors.append("availability_unreadable")
    identity = evidence.get("identity")

    decisions = {step["category"]: step for step in plan["steps"]}
    categories = []
    for category_id in CATEGORY_IDS:
        step = decisions[category_id]
        state, pointers, detail, counts = _judge(
            category_id, report, availability, identity)
        if state is None:
            # No evidence: the plan's decision decides the label, and none of
            # these labels is ever "preserved".
            state = {"included": "missing", "deferred": "skipped",
                     "unavailable": "unavailable"}[step["decision"]]
            if not detail:
                detail = (REASONS[step["reason"]] if step["reason"]
                          and step["decision"] != "included"
                          else "No evidence has arrived for this category yet.")
        categories.append({
            "id": category_id,
            "title": _CATEGORY[category_id]["title"],
            "decision": step["decision"],
            "state": state,
            "label": COVERAGE_LABELS[state],
            "evidence": pointers,
            "detail": detail,
            "counts": counts,
            "href": _docs("coverage-states"),
        })

    counts_by_state = {name: 0 for name in COVERAGE_STATES}
    for category in categories:
        counts_by_state[category["state"]] += 1
    refs_state = categories[CATEGORY_IDS.index("refs")]["state"]
    verified_mirror = bool(
        availability is not None and availability["proof"] != "none"
        and refs_state == "preserved")
    reconciled = report_reconciled(report)

    covered = sum(counts_by_state[name] for name in _COVERED_STATES)
    if covered == len(categories):
        status = "preserved"
    elif verified_mirror:
        status = "partial"
    else:
        status = "at_risk"
    residual = [
        {
            "category": category["id"],
            "state": category["state"],
            "detail": category["detail"],
        }
        for category in categories if category["state"] not in _COVERED_STATES
    ]
    summary = [
        "%d of %d categories preserved." % (covered, len(categories)),
        "A verified usable mirror exists." if verified_mirror
        else "No verified usable mirror yet.",
    ]
    if report is None:
        summary.append("No migration report was supplied.")
    elif reconciled:
        summary.append("The migration report reconciles (%s)." % report["outcome"])
    else:
        summary.append("The migration report does not reconcile (%s)." % report["outcome"])
    if residual:
        summary.append("Still at risk: %s." % ", ".join(
            "%s (%s)" % (item["category"].replace("_", " "), item["state"])
            for item in residual))

    return {
        "schema": ASSESSMENT_SCHEMA,
        "scenario": plan["scenario"],
        "mode": plan["mode"],
        "status": status,
        "summary": summary,
        "verifiedUsableMirror": verified_mirror,
        "migrationReport": {
            "present": report is not None,
            "jobId": str(report.get("jobId") or "")[:80] if report else "",
            "outcome": report["outcome"] if report else None,
            "reconciled": reconciled,
            "signatureCheck": "python3 tools/verify_migration_report.py report.json --objects objects.json",
        },
        "availabilityProof": availability["proof"] if availability else None,
        "coverage": dict(counts_by_state, covered=covered, total=len(categories)),
        "categories": categories,
        "residualRisks": residual,
        "evidenceErrors": errors,
        "plan": plan,
    }


def run_drill(drill):
    """Score one scenario drill against its documented time budget."""
    drill = drill if isinstance(drill, dict) else {}
    assessment = assess(drill.get("request"), drill.get("evidence"))
    budget = _SCENARIO[assessment["scenario"]]["drillBudgetMinutes"]
    started = _int(drill.get("startedAt"))
    verified_at = _int(drill.get("verifiedAt"))
    failures = []
    elapsed_ms = None
    if started is None or verified_at is None or started <= 0 or verified_at < started:
        failures.append("invalid_timing")
    else:
        elapsed_ms = verified_at - started
        if elapsed_ms > budget * 60 * 1000:
            failures.append("over_time_budget")
    if not assessment["migrationReport"]["present"]:
        failures.append("no_migration_report")
    elif not assessment["migrationReport"]["reconciled"]:
        failures.append("migration_report_unreconciled")
    if not assessment["verifiedUsableMirror"]:
        failures.append("no_verified_mirror")
    failures = [code for code in DRILL_FAILURES if code in failures]
    passed = not failures
    return {
        "schema": DRILL_SCHEMA,
        "id": str(drill.get("id") or "")[:120],
        "scenario": assessment["scenario"],
        "result": "passed" if passed else "failed",
        "budgetMinutes": budget,
        "elapsedMs": elapsed_ms,
        "withinBudget": elapsed_ms is not None and elapsed_ms <= budget * 60 * 1000,
        "failures": failures,
        "status": assessment["status"],
        "coverage": assessment["coverage"],
        "residualRisks": assessment["residualRisks"],
        "href": _docs("drills"),
    }


def catalog():
    """The model vocabulary a client needs to render the onboarding journey."""
    return {
        "schema": CATALOG_SCHEMA,
        "scenarios": [
            {
                "id": scenario["id"],
                "title": scenario["title"],
                "situation": scenario["situation"],
                "urgent": scenario["urgent"],
                "defaultMinutes": scenario["defaultMinutes"],
                "defaultAccess": scenario["defaultAccess"],
                "drillBudgetMinutes": scenario["drillBudgetMinutes"],
                "priority": list(scenario["priority"]),
                "href": _docs(scenario["id"]),
            }
            for scenario in SCENARIOS
        ],
        "accessLevels": list(ACCESS_LEVELS),
        "categories": [
            {"id": category["id"], "title": category["title"],
             "required": category["required"]}
            for category in CATEGORIES
        ],
        "coverageStates": [
            {"id": state, "label": COVERAGE_LABELS[state]}
            for state in COVERAGE_STATES
        ],
        "routes": [dict(route, exit=EXIT_PATH) for route in ROUTES],
        "urgentMinutes": URGENT_MINUTES,
        "href": DOCS_URL,
    }
