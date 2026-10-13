"""Repository availability contracts, reference implementation.

A preservation contract promises a repository a number of verified copies, a
trusted quorum of independent operators, region diversity, transport and
ref/artifact coverage, a verification frequency, an uptime objective, a
recovery time objective, restore-drill freshness, and an incident response
time. This module turns one evidence bundle into the contract's period report:
every verification slot, every SLO miss, the remedy each miss earns, the
capacity that was actually demonstrated, whether that capacity is admissible
for the promised terms, and what a simulated operator or regional failure
leaves behind.

The contract lives in ``conformance/availability-contract/v1/model.json``, the
shared fixture in ``conformance/availability-contract/v1/fixture.json``, and
the model is documented in ``www/docs/availability-contract.md``.

Observations are ``GET /api/repo/<owner>/<repo>/mirrors`` payloads, so every
sample is scored by ``availability_proof.evaluate_mirrors_payload`` from the
signed endpoint receipts and direct clone probes it already carries. Restore
drills are the ``forkmesh-mirror-node --restore-drill`` records. Nothing else
feeds the report, so the same bundle always reproduces the same report, and a
signed report can be checked against the evidence it names.

Operator accounts never appear in the output. Copies that share an operator
share an opaque operator number for the whole bundle.
"""

import hashlib
import itertools
import json
import re
from datetime import datetime, timedelta, timezone

import availability_proof


SCHEMA = "forkmesh-availability-contract-report-v1"
BUNDLE_SCHEMA = "forkmesh-availability-contract-evidence-v1"
SIGNED_SCHEMA = "forkmesh-availability-contract-signed-report-v1"
SIGNATURE_DOMAIN = SCHEMA

DOCS_URL = "https://forkmesh.com/docs/availability-contract/"

# A contract cannot sample faster than once a minute or split one period into
# more slots than a month of five-minute samples.
MIN_VERIFICATION_INTERVAL_MS = 60 * 1000
MAX_SLOTS = 10000
MAX_FAILURE_TOLERANCE = 2
MAX_DOMAIN_COUNT = 64

TRANSPORTS = ("router", "directHttps", "smartHttp")
CAPACITY_CLASSES = ("community", "contracted")
EXCLUSION_KINDS = ("scheduled-maintenance", "customer-requested", "legal-hold")
EXCLUSION_REASONS = ("unknown_kind", "announced_late", "over_budget")
# Nodes that declared no region form one shared failure domain in simulations,
# so a missing declaration can never make a copy look regionally independent.
UNDECLARED_REGION = "undeclared"

# Per-slot checks, in reporting order.
CHECKS = (
    ("unverified", "No observation was recorded in the verification slot."),
    ("quorum_below_contract", "Fewer independent operators than contracted held the same verified state."),
    ("copies_below_contract", "Fewer verified copies than contracted held the counted state."),
    ("regions_below_contract", "Verified copies spanned fewer regions than contracted."),
    ("transport_missing", "A contracted transport had no verified copy."),
    ("ref_coverage_incomplete", "Fewer copies than the contracted quorum carried every branch."),
    ("artifact_coverage_incomplete", "Fewer copies than the contracted quorum carried every release artifact."),
)
CHECK_CODES = tuple(code for code, _ in CHECKS)

SLOS = ("availability", "recovery", "incident-response", "restore-drills")

MISSES = (
    ("availability_window", "availability"),
    ("recovery_objective_exceeded", "recovery"),
    ("response_late", "incident-response"),
    ("response_missing", "incident-response"),
    ("drill_missing", "restore-drills"),
    ("drill_stale", "restore-drills"),
    ("drill_beyond_objective", "restore-drills"),
)
MISS_SLO = dict(MISSES)

DRILL_STATES = ("ok", "drill_missing", "drill_stale", "drill_beyond_objective")
RESPONSE_STATES = ("ok", "late", "missing", "pending")

ADMISSION_REASONS = (
    "no_evidence",
    "copies_not_demonstrated",
    "operators_not_demonstrated",
    "regions_not_demonstrated",
    "failure_simulation_failed",
)

# Correlated-risk disclosures, in reporting order.
DISCLOSURES = (
    {
        "code": "community_capacity_relied_on",
        "title": "The quorum relies on community mirrors",
        "detail": "Contracted capacity alone does not reach the promised operator quorum, so the promise depends on independent community mirrors that are not under contract.",
    },
    {
        "code": "contracted_single_operator",
        "title": "One operator runs all contracted capacity",
        "detail": "Every demonstrated contracted copy is run by one operator, so contracted capacity fails together.",
    },
    {
        "code": "operator_concentration",
        "title": "One operator holds most copies",
        "detail": "A single operator runs more than half of the demonstrated copies.",
    },
    {
        "code": "region_concentration",
        "title": "One region holds most copies",
        "detail": "More than half of the demonstrated copies share one region, or declared no region.",
    },
    {
        "code": "no_failure_headroom",
        "title": "No operator headroom",
        "detail": "Demonstrated operators do not exceed the promised quorum, so losing any one operator breaks it.",
    },
    {
        "code": "capacity_not_demonstrated",
        "title": "Contracted capacity was not demonstrated",
        "detail": "A node declared as contracted capacity lacks continuous verified probes or a qualifying restore drill, so it does not count.",
    },
)

_RFC3339 = re.compile(
    r"^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?"
    r"(Z|[+-]\d{2}:\d{2})$")


class ContractError(ValueError):
    """The evidence bundle or its contract terms cannot be evaluated."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


def canonical_json(value):
    return json.dumps(
        value, ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")


def digest(value):
    return "sha256:" + hashlib.sha256(canonical_json(value)).hexdigest()


def report_hash(report):
    return digest(report)


def signing_message(report_digest):
    return (SIGNATURE_DOMAIN + "\n" + str(report_digest or "")).encode("utf-8")


def _int(value):
    return value if isinstance(value, int) and not isinstance(value, bool) else None


def _text(value, limit):
    return value.strip()[:limit] if isinstance(value, str) else ""


def _timestamp_ms(value):
    number = _int(value)
    if number is not None:
        return number if number > 0 else None
    match = _RFC3339.match(value) if isinstance(value, str) else None
    if not match:
        return None
    year, month, day, hour, minute, second, fraction, zone = match.groups()
    try:
        moment = datetime(int(year), int(month), int(day), int(hour),
                          int(minute), int(second), tzinfo=timezone.utc)
    except ValueError:
        return None
    if zone != "Z":
        sign = 1 if zone[0] == "+" else -1
        moment -= sign * timedelta(hours=int(zone[1:3]), minutes=int(zone[4:6]))
    millis = int(((fraction or "") + "000")[:3])
    return int(moment.timestamp()) * 1000 + millis


def _percent(bps):
    return "%d.%02d%%" % (bps // 100, bps % 100)


def _require_int(source, field, minimum, maximum=None):
    value = _int(source.get(field))
    if value is None or value < minimum or (maximum is not None and value > maximum):
        raise ContractError("invalid_" + field)
    return value


def _require_bool(source, field):
    value = source.get(field)
    if not isinstance(value, bool):
        raise ContractError("invalid_" + field)
    return value


def _terms(contract):
    raw = contract.get("terms")
    if not isinstance(raw, dict):
        raise ContractError("invalid_terms")
    transports = raw.get("transports")
    if (not isinstance(transports, list)
            or any(item not in TRANSPORTS for item in transports)
            or len(set(transports)) != len(transports)):
        raise ContractError("invalid_transports")
    terms = {
        "copies": _require_int(raw, "copies", 1, MAX_DOMAIN_COUNT),
        # A contract can never promise less than the trusted quorum the
        # availability proof requires.
        "operators": _require_int(
            raw, "operators", availability_proof.QUORUM_TARGET, MAX_DOMAIN_COUNT),
        "regions": _require_int(raw, "regions", 1, MAX_DOMAIN_COUNT),
        "uptimeBps": _require_int(raw, "uptimeBps", 1, 10000),
        "verificationIntervalMs": _require_int(
            raw, "verificationIntervalMs", MIN_VERIFICATION_INTERVAL_MS),
        "transports": [name for name in TRANSPORTS if name in transports],
        "refCoverage": _require_bool(raw, "refCoverage"),
        "artifactCoverage": _require_bool(raw, "artifactCoverage"),
        "recoveryTimeObjectiveMs": _require_int(raw, "recoveryTimeObjectiveMs", 1),
        "drillIntervalMs": _require_int(raw, "drillIntervalMs", 1),
        "incidentResponseMs": _require_int(raw, "incidentResponseMs", 1),
        "failureTolerance": _require_int(
            raw, "failureTolerance", 0, MAX_FAILURE_TOLERANCE),
        "maxExcludedBps": _require_int(raw, "maxExcludedBps", 0, 10000),
    }
    if terms["operators"] > terms["copies"]:
        raise ContractError("invalid_operators")
    return terms


def _remedies(contract, terms):
    raw = contract.get("remedies")
    if not isinstance(raw, dict) or not isinstance(raw.get("availabilityTiers"), list):
        raise ContractError("invalid_remedies")
    tiers = []
    for tier in raw["availabilityTiers"]:
        if not isinstance(tier, dict):
            raise ContractError("invalid_availabilityTiers")
        below = _require_int(tier, "belowBps", 1, terms["uptimeBps"])
        tiers.append({"belowBps": below,
                      "creditBps": _require_int(tier, "creditBps", 0, 10000)})
    tiers.sort(key=lambda tier: (-tier["belowBps"], tier["creditBps"]))
    return {
        "availabilityTiers": tiers,
        "recoveryMissCreditBps": _require_int(raw, "recoveryMissCreditBps", 0, 10000),
        "responseMissCreditBps": _require_int(raw, "responseMissCreditBps", 0, 10000),
        "drillMissCreditBps": _require_int(raw, "drillMissCreditBps", 0, 10000),
        "maxCreditBps": _require_int(raw, "maxCreditBps", 0, 10000),
    }


def _exclusions(contract):
    raw = contract.get("exclusions")
    if not isinstance(raw, list):
        raise ContractError("invalid_exclusions")
    exclusions = []
    for item in raw:
        if not isinstance(item, dict):
            raise ContractError("invalid_exclusions")
        start = _require_int(item, "start", 1)
        exclusions.append({
            "kind": _text(item.get("kind"), 40),
            "start": start,
            "end": _require_int(item, "end", start + 1),
            "announcedAt": _require_int(item, "announcedAt", 1),
        })
    exclusions.sort(key=lambda item: (item["start"], item["end"], item["kind"]))
    return exclusions


def _drills(raw):
    drills = []
    for record in raw if isinstance(raw, list) else []:
        if not isinstance(record, dict) or record.get("drill") is not True:
            continue
        node = _text(record.get("node"), 80)
        completed_at = _timestamp_ms(record.get("completedAt"))
        duration = _int(record.get("durationMs"))
        repositories = _int(record.get("repositories"))
        if (not node or completed_at is None or duration is None or duration < 0
                or repositories is None or repositories < 1):
            continue
        drills.append({"node": node, "completedAt": completed_at,
                       "durationMs": duration})
    return drills


def _drill_status(drills, node, terms, period_end):
    # The latest drill decides, so an older passing drill cannot hide a newer
    # one that was slow. Equal timestamps take the slower record.
    latest = max(
        (record for record in drills
         if record["node"] == node and record["completedAt"] <= period_end),
        key=lambda record: (record["completedAt"], record["durationMs"]),
        default=None,
    )
    if latest is None:
        return "drill_missing", None
    if period_end - latest["completedAt"] > terms["drillIntervalMs"]:
        return "drill_stale", latest
    if latest["durationMs"] > terms["recoveryTimeObjectiveMs"]:
        return "drill_beyond_objective", latest
    return "ok", latest


def _rows(observation):
    mirrors = observation.get("mirrors")
    return [row for row in mirrors if isinstance(row, dict)] if isinstance(mirrors, list) else []


def _region(row):
    return _text(row.get("region"), 40).lower() or None


def _covered(coverage, needed):
    if coverage["reference"] == 0:
        return True
    return coverage["reference"] is not None and coverage["complete"] >= needed


def _observation_codes(proof, terms):
    codes = set()
    if proof["quorum"]["operators"] < terms["operators"]:
        codes.add("quorum_below_contract")
    if proof["quorum"]["copies"] < terms["copies"]:
        codes.add("copies_below_contract")
    if proof["diversity"]["regions"] < terms["regions"]:
        codes.add("regions_below_contract")
    if any(proof["transports"][name] < 1 for name in terms["transports"]):
        codes.add("transport_missing")
    if terms["refCoverage"] and not _covered(
            proof["coverage"]["branches"], terms["operators"]):
        codes.add("ref_coverage_incomplete")
    if terms["artifactCoverage"] and not _covered(
            proof["coverage"]["artifacts"], terms["operators"]):
        codes.add("artifact_coverage_incomplete")
    return codes


def _ordered_codes(codes):
    return [code for code in CHECK_CODES if code in codes]


def evaluate_contract(bundle):
    """Return the period report for one availability-contract evidence bundle.

    Raises ``ContractError`` when the bundle or its terms are malformed.
    """
    if not isinstance(bundle, dict) or bundle.get("schema") != BUNDLE_SCHEMA:
        raise ContractError("invalid_schema")
    contract = bundle.get("contract")
    if not isinstance(contract, dict):
        raise ContractError("invalid_contract")
    contract_id = _text(contract.get("id"), 80)
    if not contract_id:
        raise ContractError("invalid_id")
    repository = _text(contract.get("repository"), 200)
    if not repository:
        raise ContractError("invalid_repository")
    period_start = _require_int(contract, "periodStart", 1)
    period_end = _require_int(contract, "periodEnd", period_start + 1)
    fee = _require_int(contract, "feeMicrousd", 0)
    terms = _terms(contract)
    remedy_terms = _remedies(contract, terms)
    exclusions = _exclusions(contract)
    interval = terms["verificationIntervalMs"]
    rto = terms["recoveryTimeObjectiveMs"]
    total = -(-(period_end - period_start) // interval)
    if total > MAX_SLOTS:
        raise ContractError("too_many_slots")

    declared = {}
    capacity = bundle.get("capacity")
    for item in capacity if isinstance(capacity, list) else []:
        node = _text(item.get("node"), 80) if isinstance(item, dict) else ""
        if node:
            # Anything but an explicit contracted declaration is community
            # capacity; a typo can never claim paid, controlled capacity.
            declared[node] = (
                "contracted" if item.get("class") == "contracted" else "community")

    raw_observations = bundle.get("observations")
    raw_observations = raw_observations if isinstance(raw_observations, list) else []
    observations = sorted(
        (item for item in raw_observations
         if isinstance(item, dict) and _int(item.get("generatedAt")) is not None
         and period_start <= item["generatedAt"] < period_end),
        key=lambda item: item["generatedAt"],
    )
    ignored = len(raw_observations) - len(observations)
    drills = _drills(bundle.get("drills"))
    responses = bundle.get("responses")
    responses = responses if isinstance(responses, list) else []

    # Score every sample once and assign bundle-wide opaque operator numbers.
    operator_index = {}
    node_operator = {}
    node_region = {}
    slot_samples = [[] for _ in range(total)]
    for observation in observations:
        proof = availability_proof.evaluate_mirrors_payload(observation)
        verified = {}
        for row, copy in zip(_rows(observation), proof["copies"]):
            node = copy["node"]
            if not node:
                continue
            operator = availability_proof._operator_key(row)
            if operator not in operator_index:
                operator_index[operator] = len(operator_index) + 1
            node_operator[node] = operator
            node_region[node] = _region(row)
            verified[node] = verified.get(node, True) and copy["class"] == "verified"
        slot = (observation["generatedAt"] - period_start) // interval
        slot_samples[slot].append((
            _observation_codes(proof, terms),
            {node for node, ok in verified.items() if ok},
        ))

    # Exclusions honour only whole slots, announced before they start, within
    # the contract's exclusion budget, earliest first.
    excluded = [False] * total
    budget = terms["maxExcludedBps"] * total // 10000
    used = 0
    exclusion_rows = []
    for item in exclusions:
        row = {
            "kind": item["kind"], "start": item["start"], "end": item["end"],
            "announcedAt": item["announcedAt"], "honored": False, "reason": None,
            "slotsExcluded": 0, "slotsOverBudget": 0,
        }
        if item["kind"] not in EXCLUSION_KINDS:
            row["reason"] = "unknown_kind"
        elif item["announcedAt"] > item["start"]:
            row["reason"] = "announced_late"
        else:
            row["honored"] = True
            for slot in range(total):
                slot_start = period_start + slot * interval
                slot_end = min(slot_start + interval, period_end)
                if excluded[slot] or slot_start < item["start"] or slot_end > item["end"]:
                    continue
                if used < budget:
                    excluded[slot] = True
                    used += 1
                    row["slotsExcluded"] += 1
                else:
                    row["slotsOverBudget"] += 1
            if row["slotsOverBudget"]:
                row["reason"] = "over_budget"
        exclusion_rows.append(row)

    checks = {code: 0 for code in CHECK_CODES}
    slot_codes = []
    slot_verified = []
    for slot in range(total):
        samples = slot_samples[slot]
        if not samples:
            codes, nodes = {"unverified"}, set()
        else:
            # Every sample in a slot must pass, and a node counts as verified
            # for the slot only when every sample verified it.
            codes = set().union(*(sample[0] for sample in samples))
            nodes = set.intersection(*(sample[1] for sample in samples))
        slot_codes.append(codes)
        slot_verified.append(nodes)
        if not excluded[slot]:
            for code in codes:
                checks[code] += 1
    counted = total - sum(excluded)
    met_slots = sum(
        1 for slot in range(total) if not excluded[slot] and not slot_codes[slot])

    # Consecutive missed slots form one incident. Excluded slots neither end
    # nor extend an incident, but their time still counts toward recovery.
    incidents = []
    current = None
    for slot in range(total + 1):
        if slot < total and excluded[slot]:
            continue
        slot_start = period_start + slot * interval
        if slot < total and slot_codes[slot]:
            if current is None:
                current = {"start": slot_start, "slots": 0, "codes": set()}
            current["slots"] += 1
            current["codes"] |= slot_codes[slot]
            current["end"] = min(slot_start + interval, period_end)
        elif current is not None:
            current["recoveredAt"] = slot_start if slot < total else None
            incidents.append(current)
            current = None

    response_times = {}
    for response in responses:
        if not isinstance(response, dict):
            continue
        start = _int(response.get("incidentStart"))
        acknowledged = _int(response.get("acknowledgedAt"))
        if start is None or acknowledged is None or acknowledged < start:
            continue
        response_times[start] = min(response_times.get(start, acknowledged), acknowledged)

    incident_rows = []
    for number, incident in enumerate(incidents, 1):
        recovery_ms = (incident["recoveredAt"] or period_end) - incident["start"]
        acknowledged = response_times.get(incident["start"])
        response_ms = acknowledged - incident["start"] if acknowledged is not None else None
        if response_ms is not None:
            response = "ok" if response_ms <= terms["incidentResponseMs"] else "late"
        elif period_end - incident["start"] <= terms["incidentResponseMs"]:
            response = "pending"
        else:
            response = "missing"
        incident_rows.append({
            "id": "incident-%d" % number,
            "start": incident["start"],
            "end": incident["end"],
            "recoveredAt": incident["recoveredAt"],
            "open": incident["recoveredAt"] is None,
            "slots": incident["slots"],
            "codes": _ordered_codes(incident["codes"]),
            "recoveryMs": recovery_ms,
            "recoveryWithinObjective": recovery_ms <= rto,
            "acknowledgedAt": acknowledged,
            "responseMs": response_ms,
            "response": response,
        })

    misses = []

    def add_miss(code, **fields):
        entry = {
            "id": "miss-%d" % (len(misses) + 1), "slo": MISS_SLO[code], "code": code,
            "incident": None, "node": None, "start": None, "end": None,
            "slots": None, "codes": [], "actualMs": None, "objectiveMs": None,
            "remedy": None,
        }
        entry.update(fields)
        misses.append(entry)
        return entry

    for incident in incident_rows:
        add_miss("availability_window", incident=incident["id"],
                 start=incident["start"], end=incident["end"],
                 slots=incident["slots"], codes=incident["codes"])
        if not incident["recoveryWithinObjective"]:
            add_miss("recovery_objective_exceeded", incident=incident["id"],
                     start=incident["start"], end=incident["recoveredAt"],
                     actualMs=incident["recoveryMs"], objectiveMs=rto)
        if incident["response"] in ("late", "missing"):
            add_miss("response_" + incident["response"], incident=incident["id"],
                     start=incident["start"], end=incident["acknowledgedAt"],
                     actualMs=incident["responseMs"],
                     objectiveMs=terms["incidentResponseMs"])

    # Capacity counts only when it was verified in enough counted slots AND
    # proved a restore within the recovery objective.
    names = sorted(set(node_operator) | set(declared))
    nodes = []
    for node in names:
        verified_slots = sum(
            1 for slot in range(total)
            if not excluded[slot] and node in slot_verified[slot])
        verified_bps = verified_slots * 10000 // counted if counted else 0
        probes = counted > 0 and verified_bps >= terms["uptimeBps"]
        drill, record = _drill_status(drills, node, terms, period_end)
        operator = node_operator.get(node)
        nodes.append({
            "node": node,
            "class": declared.get(node, "community"),
            "operator": operator_index[operator] if node in node_operator else None,
            "region": node_region.get(node),
            "verifiedSlots": verified_slots,
            "verifiedBps": verified_bps,
            "probesDemonstrated": probes,
            "drill": drill,
            "drillCompletedAt": record["completedAt"] if record else None,
            "drillDurationMs": record["durationMs"] if record else None,
            "demonstrated": probes and drill == "ok",
        })

    for entry in nodes:
        if entry["class"] != "contracted" or entry["drill"] == "ok":
            continue
        if entry["drill"] == "drill_beyond_objective":
            actual, objective = entry["drillDurationMs"], rto
        else:
            # Missing or stale: how old the latest drill was at period end.
            actual = (period_end - entry["drillCompletedAt"]
                      if entry["drillCompletedAt"] is not None else None)
            objective = terms["drillIntervalMs"]
        add_miss(entry["drill"], node=entry["node"], start=entry["drillCompletedAt"],
                 actualMs=actual, objectiveMs=objective)

    demonstrated = [entry for entry in nodes if entry["demonstrated"]]
    demonstrated_names = {entry["node"] for entry in demonstrated}

    def summarize(capacity_class):
        return {
            "declared": sum(1 for entry in nodes if entry["class"] == capacity_class),
            "copies": sum(1 for entry in demonstrated if entry["class"] == capacity_class),
            "operators": len({entry["operator"] for entry in demonstrated
                              if entry["class"] == capacity_class}),
        }

    demonstrated_summary = {
        "copies": len(demonstrated),
        "operators": len({entry["operator"] for entry in demonstrated}),
        "regions": len({entry["region"] for entry in demonstrated if entry["region"]}),
    }

    # Simulated failures: take the latest sample, keep only demonstrated
    # capacity, remove every combination of failureTolerance operators (then
    # regions), and recompute the availability proof on what is left.
    simulations = []
    simulation_at = None
    if observations:
        latest = observations[-1]
        simulation_at = latest["generatedAt"]
        rows = [row for row in _rows(latest)
                if _text(row.get("node"), 80) in demonstrated_names]
        tolerance = terms["failureTolerance"]
        drill_ms = {entry["node"]: entry["drillDurationMs"] for entry in demonstrated}
        scopes = (
            ("operator", availability_proof._operator_key),
            ("region", lambda row: _region(row) or UNDECLARED_REGION),
        ) if tolerance else (("none", lambda row: ""),)
        for scope, domain in scopes:
            domains = sorted({domain(row) for row in rows})
            for failed in itertools.combinations(domains, min(tolerance, len(domains))):
                remaining = [row for row in rows if domain(row) not in failed]
                proof = availability_proof.evaluate_mirrors_payload(
                    dict(latest, mirrors=remaining))
                survivors = [
                    drill_ms[_text(row.get("node"), 80)] for row in remaining]
                recovery_ms = min(survivors) if survivors else None
                retained = proof["quorum"]["operators"] >= terms["operators"]
                within = recovery_ms is not None and recovery_ms <= rto
                simulations.append({
                    "scope": scope,
                    "failed": (sorted(operator_index[key] for key in failed)
                               if scope == "operator" else list(failed)),
                    "remainingCopies": proof["counts"]["verified"],
                    "quorumOperators": proof["quorum"]["operators"],
                    "retained": retained,
                    "recoveryMs": recovery_ms,
                    "recoveryWithinObjective": within,
                    "passed": retained and within,
                })

    admission = []
    if not observations:
        admission.append("no_evidence")
    if demonstrated_summary["copies"] < terms["copies"]:
        admission.append("copies_not_demonstrated")
    if demonstrated_summary["operators"] < terms["operators"]:
        admission.append("operators_not_demonstrated")
    if demonstrated_summary["regions"] < terms["regions"]:
        admission.append("regions_not_demonstrated")
    if not simulations or not all(item["passed"] for item in simulations):
        admission.append("failure_simulation_failed")

    contracted = [entry for entry in demonstrated if entry["class"] == "contracted"]
    per_operator = {}
    per_region = {}
    for entry in demonstrated:
        per_operator[entry["operator"]] = per_operator.get(entry["operator"], 0) + 1
        region = entry["region"] or UNDECLARED_REGION
        per_region[region] = per_region.get(region, 0) + 1
    copies = len(demonstrated)
    triggered = {
        "community_capacity_relied_on": (
            len({entry["operator"] for entry in contracted}) < terms["operators"]),
        "contracted_single_operator": (
            len(contracted) >= 2 and len({entry["operator"] for entry in contracted}) == 1),
        "operator_concentration": bool(copies) and max(per_operator.values()) * 2 > copies,
        "region_concentration": bool(copies) and max(per_region.values()) * 2 > copies,
        "no_failure_headroom": demonstrated_summary["operators"] <= terms["operators"],
        "capacity_not_demonstrated": any(
            entry["class"] == "contracted" and not entry["demonstrated"]
            for entry in nodes),
    }
    disclosures = [
        {"code": item["code"], "title": item["title"], "detail": item["detail"],
         "href": DOCS_URL + "#" + item["code"]}
        for item in DISCLOSURES if triggered[item["code"]]
    ]

    attained = met_slots * 10000 // counted if counted else 0
    recovery_misses = [m for m in misses if m["slo"] == "recovery"]
    response_misses = [m for m in misses if m["slo"] == "incident-response"]
    drill_misses = [m for m in misses if m["slo"] == "restore-drills"]
    contracted_nodes = [entry for entry in nodes if entry["class"] == "contracted"]
    slos = [
        {"id": "availability", "unit": "bps", "target": terms["uptimeBps"],
         "attained": attained, "met": counted > 0 and attained >= terms["uptimeBps"]},
        {"id": "recovery", "unit": "ms", "target": rto,
         "attained": max((i["recoveryMs"] for i in incident_rows), default=0),
         "met": not recovery_misses},
        {"id": "incident-response", "unit": "ms", "target": terms["incidentResponseMs"],
         "attained": max((i["responseMs"] for i in incident_rows
                          if i["responseMs"] is not None), default=0),
         "met": not response_misses},
        {"id": "restore-drills", "unit": "nodes", "target": len(contracted_nodes),
         "attained": sum(1 for entry in contracted_nodes if entry["drill"] == "ok"),
         "met": not drill_misses},
    ]

    lines = []

    def add_remedy(slo, code, remedied, credit_bps):
        line = {
            "id": "remedy-%d" % (len(lines) + 1), "slo": slo, "code": code,
            "misses": [m["id"] for m in remedied], "creditBps": credit_bps,
            "creditMicrousd": fee * credit_bps // 10000,
        }
        lines.append(line)
        for miss in remedied:
            miss["remedy"] = line["id"]

    if not slos[0]["met"]:
        tier = max((tier["creditBps"] for tier in remedy_terms["availabilityTiers"]
                    if attained < tier["belowBps"]), default=0)
        add_remedy("availability", "availability_tier",
                   [m for m in misses if m["slo"] == "availability"], tier)
    for miss in recovery_misses:
        add_remedy("recovery", miss["code"], [miss], remedy_terms["recoveryMissCreditBps"])
    for miss in response_misses:
        add_remedy("incident-response", miss["code"], [miss],
                   remedy_terms["responseMissCreditBps"])
    for miss in drill_misses:
        add_remedy("restore-drills", miss["code"], [miss],
                   remedy_terms["drillMissCreditBps"])
    uncapped = sum(line["creditBps"] for line in lines)
    total_bps = min(uncapped, remedy_terms["maxCreditBps"])

    outcome = "met" if all(slo["met"] for slo in slos) else "missed"
    parts = [
        "Contract %s for %s." % (outcome, repository),
        "Availability %s against a %s target over %d of %d slots." % (
            _percent(attained), _percent(terms["uptimeBps"]), counted, total),
        "%d %s; credit %s of the fee." % (
            len(misses), "miss" if len(misses) == 1 else "misses", _percent(total_bps)),
    ]
    if admission:
        parts.append("Capacity not admissible: %s." % ", ".join(admission))
    else:
        parts.append("Capacity admissible: %d copies, %d operators, %d regions demonstrated." % (
            demonstrated_summary["copies"], demonstrated_summary["operators"],
            demonstrated_summary["regions"]))

    return {
        "schema": SCHEMA,
        "contractId": contract_id,
        "repository": repository,
        "periodStart": period_start,
        "periodEnd": period_end,
        "outcome": outcome,
        "summary": " ".join(parts),
        "terms": terms,
        "slots": {
            "total": total, "counted": counted, "excluded": total - counted,
            "met": met_slots, "missed": counted - met_slots,
        },
        "checks": checks,
        "slos": slos,
        "incidents": incident_rows,
        "misses": misses,
        "remedies": {
            "terms": remedy_terms,
            "feeMicrousd": fee,
            "lines": lines,
            "uncappedCreditBps": uncapped,
            "capped": uncapped > total_bps,
            "totalCreditBps": total_bps,
            "totalCreditMicrousd": fee * total_bps // 10000,
        },
        "exclusions": exclusion_rows,
        "capacity": {
            "nodes": nodes,
            "demonstrated": demonstrated_summary,
            "community": summarize("community"),
            "contracted": summarize("contracted"),
        },
        "admission": {"admissible": not admission, "reasons": admission},
        "simulationObservedAt": simulation_at,
        "simulations": simulations,
        "disclosures": disclosures,
        "evidence": {
            "contractSha256": digest(contract),
            "capacitySha256": digest(capacity if isinstance(capacity, list) else []),
            "observations": len(observations),
            "ignoredObservations": ignored,
            "observationsSha256": digest(observations),
            "lastObservationAt": simulation_at,
            "drills": len(drills),
            "drillsSha256": digest(bundle.get("drills") if isinstance(bundle.get("drills"), list) else []),
            "responsesSha256": digest(responses),
        },
    }
