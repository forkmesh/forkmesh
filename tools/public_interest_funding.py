#!/usr/bin/env python3
"""Public-interest funding ledger (issue #515).

The program (areas, offers, eligibility, selection criteria, conflict rules,
concentration limits, payment rails, and independence rules) and one record per
funding round live in ``conformance/funding/v1``. ``validate`` checks that each
round follows the program: funders and administrative cost are published before
applications open, conflicted reviewers recuse, awards meet the review quorum
and minimum score, no recipient exceeds its share of a round, awards grant no
access, identity right, vote, or network obligation, payment rails need no
ForkMesh account or plan, disbursements follow accepted milestones, and outcome
reports match the ledger. Reviewers and their disclosures come from the
governance ledger (``conformance/governance/v1``). ``render`` regenerates the
public funding register (Markdown and searchable JSON) and ``--check`` fails
when either is stale. ``gate`` runs both and never depends on today's date.
``status`` and ``closure`` evaluate overdue reports, concentration warnings,
measures, and the epic's closure criteria as of a date.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
FUNDING_DIRECTORY = Path("conformance/funding/v1")
PROGRAM_PATH = FUNDING_DIRECTORY / "program.json"
ROUNDS_DIRECTORY = FUNDING_DIRECTORY / "rounds"
REGISTER_MARKDOWN_PATH = Path("www/docs/funding/funding-register.md")
REGISTER_JSON_PATH = Path("www/public/docs/funding/register.json")
GOVERNANCE_CHARTER_PATH = Path("conformance/governance/v1/charter.json")
GOVERNANCE_PARTICIPANTS_PATH = Path("conformance/governance/v1/participants.json")
BUSINESS_MODEL_PATH = Path("www/public/business-model.json")

PROGRAM_SCHEMA = "forkmesh-public-interest-funding-program-v1"
ROUND_SCHEMA = "forkmesh-public-interest-funding-round-v1"
REGISTER_SCHEMA = "forkmesh-public-interest-funding-register-v1"

STAGES = ["draft", "open", "reviewing", "selected", "disbursing", "reported", "cancelled"]
STAGE_ORDER = {stage: index for index, stage in enumerate(STAGES[:-1])}
TIMELINE_KEYS = ["announced", "applicationsOpen", "applicationsClose", "selectionsPublished", "finalReportDue", "reported"]
OFFER_KINDS = {"donation", "recurring-sponsorship", "targeted-grant", "matching"}
OFFER_STATUSES = {"available", "planned", "retired"}
MILESTONE_STATUSES = {"pending", "accepted", "missed", "waived"}
DISBURSEMENT_STATUSES = {"sent", "returned"}
FINDING_STATUSES = {"open", "corrected", "accepted-risk"}
RAIL_KINDS = {"conventional", "on-chain"}
REQUIRED_RISKS = {"concentration", "accessibility", "conflict"}
REQUIRED_NEVER_GRANT = {"repository-access", "identity-rights", "protocol-votes", "required-network-behavior"}
EVIDENCE_KINDS = ("supported-platforms", "failure-paths", "rollback-recovery", "documentation", "telemetry")
MAINTAINER = "forkmesh-maintainer"
OPERATOR_MARKER = "forkmesh.com"
REPORTED_MEASURES = (
    "fundsRaised", "fundsAwarded", "fundsDisbursed", "administrativeCost",
    "applications", "recipients", "milestonesAccepted", "milestonesTotal", "operatorsFunded",
)


class FundingError(ValueError):
    pass


def _json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise FundingError(f"{path}: {exc}") from exc


def _day(value: Any, where: str, errors: list[str]) -> date | None:
    if value is None:
        return None
    if not isinstance(value, str):
        errors.append(f"{where}: date must be YYYY-MM-DD")
        return None
    try:
        return date.fromisoformat(value)
    except ValueError:
        errors.append(f"{where}: date must be YYYY-MM-DD")
        return None


def _amount(value: Any, *, zero_ok: bool = False) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and (value >= 0 if zero_ok else value > 0)


def _text(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _link_ok(href: Any, root: Path) -> bool:
    return isinstance(href, str) and (href.startswith("https://") or (bool(href) and (root / href.split("#", 1)[0]).exists()))


def load_ledger(root: Path = ROOT) -> dict:
    rounds = []
    for path in sorted((root / ROUNDS_DIRECTORY).glob("*.json")):
        record = _json(path)
        if isinstance(record, dict):
            record["_file"] = path.stem
        rounds.append(record)
    return {
        "program": _json(root / PROGRAM_PATH),
        "rounds": rounds,
        "governanceCharter": _json(root / GOVERNANCE_CHARTER_PATH),
        "governanceParticipants": _json(root / GOVERNANCE_PARTICIPANTS_PATH),
        "businessModel": _json(root / BUSINESS_MODEL_PATH),
    }


def _governance(ledger: dict) -> tuple[dict[str, dict], dict[str, dict]]:
    document = ledger["governanceParticipants"]
    participants = {item.get("id"): item for item in document.get("participants", []) if isinstance(item, dict)}
    disclosures = {item.get("id"): item for item in document.get("disclosures", []) if isinstance(item, dict)}
    return participants, disclosures


def _reached(round_: dict, stage: str) -> bool:
    current = round_.get("cancelledFrom") if round_.get("stage") == "cancelled" else round_.get("stage")
    return STAGE_ORDER.get(current, -1) >= STAGE_ORDER[stage]


def validate_program(ledger: dict, root: Path, errors: list[str]) -> None:
    program = ledger["program"]
    if program.get("schema") != PROGRAM_SCHEMA:
        errors.append(f"program: schema must be {PROGRAM_SCHEMA}")
    if program.get("programVersion") not in {item.get("version") for item in program.get("versions", [])}:
        errors.append("program: programVersion must be listed in versions")

    area_ids = [area.get("id") for area in program.get("areas", [])]
    if len(set(area_ids)) != len(area_ids) or not area_ids:
        errors.append("program: area ids must be unique and non-empty")
    for area in program.get("areas", []):
        if not _text(area.get("name")) or not area.get("eligibility"):
            errors.append(f"program area {area.get('id')}: name and eligibility are required")
    if not program.get("generalEligibility"):
        errors.append("program: generalEligibility is required")

    offers = program.get("offers", [])
    if {offer.get("kind") for offer in offers} != OFFER_KINDS:
        errors.append("program: offers must cover " + ", ".join(sorted(OFFER_KINDS)))
    for offer in offers:
        where = f"program offer {offer.get('id')}"
        if offer.get("status") not in OFFER_STATUSES:
            errors.append(f"{where}: status must be one of {', '.join(sorted(OFFER_STATUSES))}")
        for key in ("description", "eligibility", "selection"):
            if not _text(offer.get(key)):
                errors.append(f"{where}: {key} must be published")
        if offer.get("kind") == "matching" and not (isinstance(offer.get("maximumMatchRatio"), (int, float)) and offer["maximumMatchRatio"] > 0):
            errors.append(f"{where}: maximumMatchRatio must be positive")

    criteria = program.get("selectionCriteria", [])
    if len({item.get("id") for item in criteria}) != len(criteria) or not criteria:
        errors.append("program: selection criterion ids must be unique and non-empty")
    if sum(item.get("weight", 0) for item in criteria if _amount(item.get("weight"))) != 100 or not all(_amount(item.get("weight")) for item in criteria):
        errors.append("program: selection criterion weights must be positive integers summing to 100")
    scale = program.get("scoreScale", {})
    if not (_amount(scale.get("minimum"), zero_ok=True) and _amount(scale.get("maximum")) and scale["minimum"] < scale["maximum"]):
        errors.append("program: scoreScale needs integer minimum below maximum")
    if not (isinstance(program.get("minimumScore"), (int, float)) and 0 < program["minimumScore"] <= 100):
        errors.append("program: minimumScore must be between 1 and 100")
    if not _amount(program.get("minimumApplicationDays")):
        errors.append("program: minimumApplicationDays must be positive")
    review = program.get("review", {})
    if not (_amount(review.get("minimumScoringReviewers")) and review["minimumScoringReviewers"] >= 2):
        errors.append("program: at least two reviewers must score each application")
    if not (_amount(review.get("minimumIndependentReviewers")) and review["minimumIndependentReviewers"] <= review.get("minimumScoringReviewers", 0)):
        errors.append("program: at least one independent reviewer must score each application")
    for key in ("recusal", "funders"):
        if not _text(program.get("conflicts", {}).get(key)):
            errors.append(f"program: conflicts.{key} must be published")
    for key in ("maximumRecipientShareOfRound", "maximumAreaShareOfRound", "maximumFunderShareOfRound"):
        share = program.get("concentration", {}).get(key)
        if not (isinstance(share, (int, float)) and 0 < share <= 1):
            errors.append(f"program: concentration.{key} must be a share above 0 and at most 1")
    admin = program.get("administrativeCost", {}).get("maximumShareOfPool")
    if not (isinstance(admin, (int, float)) and 0 <= admin < 1):
        errors.append("program: administrativeCost.maximumShareOfPool must be below 1")

    rails = program.get("paymentRails", [])
    if len({rail.get("id") for rail in rails}) != len(rails):
        errors.append("program: payment rail ids must be unique")
    for rail in rails:
        where = f"program rail {rail.get('id')}"
        if rail.get("kind") not in RAIL_KINDS:
            errors.append(f"{where}: kind must be conventional or on-chain")
        if rail.get("forkmeshAccountRequired") is not False or rail.get("forkmeshPlanRequired") is not False:
            errors.append(f"{where}: a payment rail must not require a ForkMesh account or plan")
        if rail.get("recipientControlled") is not True:
            errors.append(f"{where}: the recipient must control the receiving account")
    if not any(rail.get("kind") == "conventional" for rail in rails):
        errors.append("program: at least one conventional payment rail is required")

    independence = program.get("independence", {})
    missing = REQUIRED_NEVER_GRANT - set(independence.get("awardsNeverGrant", []))
    if missing:
        errors.append("program: awardsNeverGrant must include " + ", ".join(sorted(missing)))
    if not independence.get("allowedObligations"):
        errors.append("program: allowedObligations must be published")
    if set(independence.get("allowedObligations", [])) & set(independence.get("awardsNeverGrant", [])):
        errors.append("program: an allowed obligation cannot be something awards never grant")
    commitments = ledger["businessModel"].get("permanentCommitments", {})
    if not independence.get("commitments"):
        errors.append("program: independence must cite business-model commitments")
    for commitment in independence.get("commitments", []):
        if commitment not in commitments:
            errors.append(f"program: unknown business-model commitment {commitment}")
        elif commitments[commitment] is not False:
            errors.append(f"program: business-model commitment {commitment} no longer holds")
    rights = {right.get("id") for right in ledger["governanceCharter"].get("protectedRights", [])}
    if independence.get("governanceRight") not in rights:
        errors.append(f"program: governance protected right {independence.get('governanceRight')} does not exist")

    risks = set(program.get("communityReview", {}).get("requiredRisks", []))
    if not REQUIRED_RISKS <= risks:
        errors.append("program: community review must cover " + ", ".join(sorted(REQUIRED_RISKS)))
    if not _amount(program.get("communityReview", {}).get("minimumCommentDays")):
        errors.append("program: communityReview.minimumCommentDays must be positive")
    if not _amount(program.get("reporting", {}).get("milestoneReportGraceDays"), zero_ok=True):
        errors.append("program: reporting.milestoneReportGraceDays is required")
    if not program.get("measures"):
        errors.append("program: measures must be published")

    evidence = program.get("closureEvidence", {})
    for kind in EVIDENCE_KINDS:
        if not evidence.get(kind):
            errors.append(f"program: closureEvidence.{kind} needs at least one link")
        for item in evidence.get(kind, []):
            path, _, test_name = str(item).partition("::")
            target = root / path
            if not target.exists():
                errors.append(f"program: closure evidence {item} does not exist")
            elif test_name and f"def {test_name}(" not in target.read_text(encoding="utf-8"):
                errors.append(f"program: closure evidence test {item} does not exist")


def _auto_conflict(participant: dict, application: dict) -> str | None:
    applicant = application.get("applicant") or {}
    if applicant.get("participant") and applicant.get("participant") == participant.get("id"):
        return "reviewer is the applicant"
    funder = str(participant.get("employerOrFunder", "")).casefold()
    if funder and funder != "none" and funder in {
        str(applicant.get("employerOrFunder", "")).casefold(), str(applicant.get("name", "")).casefold()
    }:
        return "reviewer shares an employer or funder with the applicant"
    return None


def application_score(round_: dict, program: dict, application_id: str) -> float | None:
    """Weighted score out of 100: each criterion's mean reviewer score, scaled by its weight."""
    reviews = [review for review in round_.get("reviews", []) if review.get("application") == application_id]
    maximum = program.get("scoreScale", {}).get("maximum") or 1
    if not reviews:
        return None
    total = 0.0
    for criterion in program.get("selectionCriteria", []):
        values = [review.get("scores", {}).get(criterion["id"]) for review in reviews]
        values = [value for value in values if isinstance(value, (int, float))]
        if values:
            total += criterion["weight"] * (sum(values) / len(values)) / maximum
    return round(total, 1)


def round_measures(round_: dict) -> dict:
    applications = {item.get("id"): item for item in round_.get("applications", []) if isinstance(item, dict)}
    awards = [award for award in round_.get("awards", []) if isinstance(award, dict)]
    funders = [funder for funder in round_.get("funders", []) if isinstance(funder, dict)]

    def amount(item: dict) -> int:
        return item.get("amount") if _amount(item.get("amount")) else 0

    awarded = sum(amount(award) for award in awards)
    raised = sum(amount(funder) for funder in funders)
    sent = [item for item in round_.get("disbursements", []) if isinstance(item, dict) and item.get("status") == "sent"]
    by_recipient: dict[str, int] = {}
    by_area: dict[str, int] = {}
    operators = set()
    accessibility = 0
    for award in awards:
        application = applications.get(award.get("application"), {})
        name = str((application.get("applicant") or {}).get("name", award.get("application"))).casefold()
        by_recipient[name] = by_recipient.get(name, 0) + amount(award)
        area = application.get("area")
        by_area[area] = by_area.get(area, 0) + amount(award)
        if area == "volunteer-mirrors":
            operators.add(name)
        if area == "accessibility":
            accessibility += 1
    milestones = [milestone for award in awards for milestone in award.get("milestones", []) if isinstance(milestone, dict)]

    def share(values: dict[str, int] | list[int], total: int) -> float | None:
        numbers = list(values.values()) if isinstance(values, dict) else values
        return round(max(numbers) / total, 4) if numbers and total else None

    admin = (round_.get("budget") or {}).get("administrativeCost")
    accepted = sum(1 for milestone in milestones if milestone.get("status") == "accepted")
    return {
        "fundsCommitted": (round_.get("budget") or {}).get("committed"),
        "fundsRaised": raised,
        "fundsAwarded": awarded,
        "fundsDisbursed": sum(amount(item) for item in sent),
        "administrativeCost": admin if _amount(admin, zero_ok=True) else None,
        "applications": len(applications),
        "recipients": len(by_recipient),
        "milestonesAccepted": accepted,
        "milestonesTotal": len(milestones),
        "milestoneCompletion": round(accepted / len(milestones), 4) if milestones else None,
        "operatorsFunded": len(operators),
        "operatorsRetained": (round_.get("outcomeReport") or {}).get("operatorsRetained"),
        "accessibilityAwards": accessibility,
        "largestRecipientShare": share(by_recipient, awarded),
        "largestAreaShare": share(by_area, awarded),
        "largestFunderShare": share([amount(funder) for funder in funders], raised),
    }


def concentration_warnings(round_: dict, program: dict) -> list[dict]:
    """Area and funder concentration above the program limits. Community review must address each one."""
    limits = program.get("concentration", {})
    measures = round_measures(round_)
    warnings = []
    for measure, limit_key, subject in (
        ("largestAreaShare", "maximumAreaShareOfRound", "area"),
        ("largestFunderShare", "maximumFunderShareOfRound", "funder"),
    ):
        value = measures[measure]
        if value is not None and value > limits.get(limit_key, 1) + 1e-9:
            warnings.append({"round": round_.get("id"), "subject": subject, "share": value, "limit": limits.get(limit_key)})
    return warnings


def validate_round(round_: Any, ledger: dict, root: Path, errors: list[str]) -> None:
    if not isinstance(round_, dict):
        errors.append("round: must be an object")
        return
    program = ledger["program"]
    round_id = round_.get("id")
    where = f"round {round_id}"
    if round_.get("schema") != ROUND_SCHEMA:
        errors.append(f"{where}: schema must be {ROUND_SCHEMA}")
    if round_.get("_file") != round_id:
        errors.append(f"{where}: file name must match the round id")
    if not str(round_id).startswith("PIF-"):
        errors.append(f"{where}: round ids start with PIF-")
    if round_.get("programVersion") not in {item.get("version") for item in program.get("versions", [])}:
        errors.append(f"{where}: programVersion must be a published program version")
    for key in ("title", "summary"):
        if not _text(round_.get(key)):
            errors.append(f"{where}: {key} is required")
    if not isinstance(round_.get("pilot"), bool):
        errors.append(f"{where}: pilot must be true or false")
    stage = round_.get("stage")
    if stage not in STAGES:
        errors.append(f"{where}: stage must be one of {', '.join(STAGES)}")
        return
    if stage == "cancelled" and round_.get("cancelledFrom") not in set(STAGE_ORDER) - {"reported"}:
        errors.append(f"{where}: a cancelled round names the stage it was cancelled from")
    terminal = stage in {"reported", "cancelled"}

    area_ids = {area.get("id") for area in program.get("areas", [])}
    offers = {offer.get("id"): offer for offer in program.get("offers", [])}
    rails = {rail.get("id"): rail for rail in program.get("paymentRails", [])}
    areas = set(round_.get("areas", []))
    if not areas or not areas <= area_ids:
        errors.append(f"{where}: areas must be non-empty program areas")
    if not round_.get("offers") or not set(round_.get("offers", [])) <= set(offers):
        errors.append(f"{where}: offers must be non-empty program offers")
    round_rails = set(round_.get("paymentRails", []))
    if not round_rails or not round_rails <= set(rails):
        errors.append(f"{where}: paymentRails must be non-empty program rails")
    elif not any(rails[rail].get("kind") == "conventional" for rail in round_rails):
        errors.append(f"{where}: must offer a conventional payment rail")
    channels = round_.get("applicationChannels", [])
    if not any(isinstance(channel, dict) and channel.get("forkmeshAccountRequired") is False for channel in channels):
        errors.append(f"{where}: needs an application channel that requires no ForkMesh account")

    timeline = round_.get("timeline", {})
    if set(timeline) != set(TIMELINE_KEYS):
        errors.append(f"{where}: timeline keys must be {', '.join(TIMELINE_KEYS)}")
    days = {key: _day(timeline.get(key), f"{where} timeline.{key}", errors) for key in TIMELINE_KEYS}
    ordered = [days[key] for key in TIMELINE_KEYS if days.get(key) and key != "finalReportDue"]
    if ordered != sorted(ordered):
        errors.append(f"{where}: timeline dates must not go backwards")

    budget = round_.get("budget") or {}
    if budget.get("currency") != program.get("currency"):
        errors.append(f"{where}: budget currency must be {program.get('currency')}")
    participants, disclosures = _governance(ledger)
    kinds = set(ledger["governanceCharter"].get("disclosures", {}).get("fundingKinds", [])) - {"none"}

    funder_ids: set[str] = set()
    funder_participants = set()
    for funder in round_.get("funders", []):
        fwhere = f"{where} funder {funder.get('id')}"
        if funder.get("id") in funder_ids:
            errors.append(f"{fwhere}: id is not unique")
        funder_ids.add(funder.get("id"))
        if not _text(funder.get("name")):
            errors.append(f"{fwhere}: name is required")
        if funder.get("kind") not in kinds:
            errors.append(f"{fwhere}: kind must be a governance funding kind")
        if not _amount(funder.get("amount")):
            errors.append(f"{fwhere}: the amount must be published")
        if not isinstance(funder.get("conflicts"), list):
            errors.append(f"{fwhere}: conflicts must be a list (empty when there are none)")
        restrictions = funder.get("restrictions", {})
        if not isinstance(restrictions, dict) or set(restrictions) - {"areas"}:
            errors.append(f"{fwhere}: a funder may restrict a gift only to areas")
        elif not set(restrictions.get("areas", [])) <= areas:
            errors.append(f"{fwhere}: restricted areas must be round areas")
        if funder.get("participant"):
            if funder["participant"] not in participants:
                errors.append(f"{fwhere}: unknown governance participant {funder['participant']}")
            funder_participants.add(funder["participant"])

    reviewers: dict[str, dict] = {}
    for reviewer in round_.get("reviewers", []):
        pid = reviewer.get("participant")
        rwhere = f"{where} reviewer {pid}"
        if pid in reviewers:
            errors.append(f"{rwhere}: listed twice")
        participant = participants.get(pid)
        if participant is None:
            errors.append(f"{rwhere}: not a governance participant")
            continue
        if participant.get("kind") == "group":
            errors.append(f"{rwhere}: a reviewer is a person, not a group")
        disclosure = disclosures.get(reviewer.get("disclosure"))
        if disclosure is None or disclosure.get("participant") != pid:
            errors.append(f"{rwhere}: needs a governance disclosure filed by the reviewer")
        if pid in funder_participants:
            errors.append(f"{rwhere}: a funder never reviews")
        reviewers[pid] = participant

    review_rules = program.get("review", {})
    if _reached(round_, "open"):
        for key in ("announced", "applicationsOpen", "applicationsClose"):
            if days.get(key) is None:
                errors.append(f"{where}: an open round publishes timeline.{key}")
        if days.get("applicationsOpen") and days.get("applicationsClose") and \
                (days["applicationsClose"] - days["applicationsOpen"]).days < program.get("minimumApplicationDays", 0):
            errors.append(f"{where}: application window is shorter than {program.get('minimumApplicationDays')} days")
        committed = budget.get("committed")
        admin = budget.get("administrativeCost")
        if not _amount(committed):
            errors.append(f"{where}: an open round publishes its committed pool")
        if not _amount(admin, zero_ok=True):
            errors.append(f"{where}: an open round publishes its administrative cost")
        elif _amount(committed) and admin > program["administrativeCost"]["maximumShareOfPool"] * committed + 1e-9:
            errors.append(f"{where}: administrative cost exceeds {program['administrativeCost']['maximumShareOfPool']:.0%} of the pool")
        if not round_.get("funders"):
            errors.append(f"{where}: an open round names its funders")
        elif _amount(committed) and round_measures(round_)["fundsRaised"] < committed:
            errors.append(f"{where}: funders cover less than the committed pool")
        if len(reviewers) < review_rules.get("minimumScoringReviewers", 0):
            errors.append(f"{where}: an open round seats at least {review_rules.get('minimumScoringReviewers')} reviewers")
        if sum(1 for item in reviewers.values() if item.get("constituency") != MAINTAINER) < review_rules.get("minimumIndependentReviewers", 0):
            errors.append(f"{where}: an open round seats an independent reviewer")
    if _reached(round_, "selected") and days.get("selectionsPublished") is None:
        errors.append(f"{where}: a selected round publishes timeline.selectionsPublished")
    if stage == "reported" and days.get("reported") is None:
        errors.append(f"{where}: a reported round publishes timeline.reported")

    applications: dict[str, dict] = {}
    if round_.get("applications") and not _reached(round_, "open"):
        errors.append(f"{where}: applications arrive only after the round opens")
    for application in round_.get("applications", []):
        awhere = f"{where} application {application.get('id')}"
        if application.get("id") in applications:
            errors.append(f"{awhere}: id is not unique")
        applications[application.get("id")] = application
        applicant = application.get("applicant") or {}
        if not _text(applicant.get("name")):
            errors.append(f"{awhere}: applicant name is required")
        if not str(applicant.get("source", "")).startswith("https://"):
            errors.append(f"{awhere}: applicant needs a public https source")
        if not _text(applicant.get("employerOrFunder")):
            errors.append(f"{awhere}: applicant employerOrFunder is required (use \"none\")")
        if application.get("area") not in areas:
            errors.append(f"{awhere}: area must be a round area")
        if application.get("offer") not in set(round_.get("offers", [])):
            errors.append(f"{awhere}: offer must be a round offer")
        if not _amount(application.get("requested")):
            errors.append(f"{awhere}: requested amount is required")
        received = _day(application.get("received"), awhere, errors)
        if received is None or (days.get("applicationsOpen") and received < days["applicationsOpen"]) or \
                (days.get("applicationsClose") and received > days["applicationsClose"]):
            errors.append(f"{awhere}: received date must fall inside the application window")
        if not isinstance(application.get("eligible"), bool):
            errors.append(f"{awhere}: eligible must be true or false")
        elif not application["eligible"] and not _text(application.get("eligibilityNote")):
            errors.append(f"{awhere}: an ineligible application records why in eligibilityNote")
        if application.get("eligible") and OPERATOR_MARKER in str(applicant.get("employerOrFunder", "")).casefold():
            errors.append(f"{awhere}: applicants paid by the forkmesh.com operator are not eligible")

    recusals = set()
    for conflict in round_.get("conflicts", []):
        key = (conflict.get("application"), conflict.get("reviewer"))
        cwhere = f"{where} conflict {key[0]}/{key[1]}"
        if key[0] not in applications or key[1] not in reviewers:
            errors.append(f"{cwhere}: names an unknown application or reviewer")
        if not _text(conflict.get("reason")) or conflict.get("action") != "recused":
            errors.append(f"{cwhere}: records a reason and action \"recused\"")
        recusals.add(key)

    criteria = {item.get("id") for item in program.get("selectionCriteria", [])}
    scale = program.get("scoreScale", {})
    scored: dict[str, set[str]] = {}
    if round_.get("reviews") and not _reached(round_, "reviewing"):
        errors.append(f"{where}: reviews are recorded only once the round is reviewing")
    for review in round_.get("reviews", []):
        key = (review.get("application"), review.get("reviewer"))
        vwhere = f"{where} review {key[0]}/{key[1]}"
        if key[0] not in applications or key[1] not in reviewers:
            errors.append(f"{vwhere}: names an unknown application or reviewer")
            continue
        if key[1] in scored.get(key[0], set()):
            errors.append(f"{vwhere}: reviewer scored the application twice")
        if key in recusals:
            errors.append(f"{vwhere}: reviewer scored an application they recused from")
        scores = review.get("scores", {})
        if set(scores) != criteria or not all(
            _amount(value, zero_ok=True) and scale.get("minimum", 0) <= value <= scale.get("maximum", 0) for value in scores.values()
        ):
            errors.append(f"{vwhere}: scores every selection criterion within the score scale")
        if _day(review.get("recorded"), vwhere, errors) is None:
            errors.append(f"{vwhere}: recorded date is required")
        scored.setdefault(key[0], set()).add(key[1])
    if _reached(round_, "reviewing"):
        for application_id, application in applications.items():
            for pid, participant in reviewers.items():
                reason = _auto_conflict(participant, application)
                if reason and (application_id, pid) not in recusals:
                    errors.append(f"{where}: {pid} must record a recusal from {application_id} ({reason})")

    if _reached(round_, "selected"):
        for application_id, application in applications.items():
            if not application.get("eligible"):
                continue
            voters = scored.get(application_id, set())
            independent = sum(1 for pid in voters if reviewers[pid].get("constituency") != MAINTAINER)
            if len(voters) < review_rules.get("minimumScoringReviewers", 0) or independent < review_rules.get("minimumIndependentReviewers", 0):
                errors.append(f"{where}: {application_id} was scored by {len(voters)} reviewers ({independent} independent); "
                              f"selection needs {review_rules.get('minimumScoringReviewers')} ({review_rules.get('minimumIndependentReviewers')} independent)")

    independence = program.get("independence", {})
    never = set(independence.get("awardsNeverGrant", []))
    allowed = set(independence.get("allowedObligations", []))
    awards: dict[str, dict] = {}
    awarded_applications = set()
    if round_.get("awards") and not _reached(round_, "selected"):
        errors.append(f"{where}: awards are published only once the round is selected")
    for award in round_.get("awards", []):
        awhere = f"{where} award {award.get('id')}"
        if award.get("id") in awards:
            errors.append(f"{awhere}: id is not unique")
        awards[award.get("id")] = award
        grants = award.get("grants")
        if grants != []:
            granted = ", ".join(str(item) for item in grants) if isinstance(grants, list) else "unspecified rights"
            errors.append(f"{awhere}: grants must be empty; awards never grant {granted}"
                          + (" (" + ", ".join(sorted(never & set(grants))) + " is explicitly prohibited)" if isinstance(grants, list) and never & set(grants) else ""))
        extra = set(award.get("obligations", [])) - allowed
        if extra:
            errors.append(f"{awhere}: obligations {', '.join(sorted(extra))} are not allowed")
        if award.get("rail") not in round_rails:
            errors.append(f"{awhere}: rail must be one the round offers, chosen by the recipient")
        application = applications.get(award.get("application"))
        if application is None:
            errors.append(f"{awhere}: names an unknown application")
            continue
        if award["application"] in awarded_applications:
            errors.append(f"{awhere}: an application receives at most one award")
        awarded_applications.add(award["application"])
        if not application.get("eligible"):
            errors.append(f"{awhere}: awarded an ineligible application")
        score = application_score(round_, program, award["application"])
        if score is None or score < program.get("minimumScore", 0):
            errors.append(f"{awhere}: score {score} is below the minimum {program.get('minimumScore')}")
        if not _amount(award.get("amount")):
            errors.append(f"{awhere}: amount is required")
            continue
        if _amount(application.get("requested")) and award["amount"] > application["requested"]:
            errors.append(f"{awhere}: amount exceeds the requested amount")
        if application.get("offer") == "matching":
            matching = award.get("matching") or {}
            ratio = matching.get("ratio")
            if not _amount(matching.get("contributions")) or not isinstance(ratio, (int, float)) or ratio <= 0 or not _link_ok(matching.get("receipt"), root):
                errors.append(f"{awhere}: matching needs contributions, a ratio, and a public receipt")
            else:
                if ratio > offers.get("matching", {}).get("maximumMatchRatio", 0):
                    errors.append(f"{awhere}: match ratio exceeds the program maximum")
                if award["amount"] > matching["contributions"] * ratio:
                    errors.append(f"{awhere}: amount exceeds contributions times the match ratio")
        milestones = award.get("milestones", [])
        if not milestones:
            errors.append(f"{awhere}: at least one milestone is required")
        milestone_ids = set()
        for milestone in milestones:
            mwhere = f"{awhere} milestone {milestone.get('id')}"
            if milestone.get("id") in milestone_ids:
                errors.append(f"{mwhere}: id is not unique")
            milestone_ids.add(milestone.get("id"))
            if not _text(milestone.get("deliverable")):
                errors.append(f"{mwhere}: deliverable is required")
            if _day(milestone.get("due"), mwhere, errors) is None:
                errors.append(f"{mwhere}: due date is required")
            if not _amount(milestone.get("amount"), zero_ok=True):
                errors.append(f"{mwhere}: amount is required")
            status = milestone.get("status")
            if status not in MILESTONE_STATUSES:
                errors.append(f"{mwhere}: status must be one of {', '.join(sorted(MILESTONE_STATUSES))}")
            if status in {"accepted", "missed"} and (not _link_ok(milestone.get("report"), root) or _day(milestone.get("reported"), mwhere, errors) is None):
                errors.append(f"{mwhere}: an {status} milestone links its public report and reported date")
            if status == "waived" and not _text(milestone.get("note")):
                errors.append(f"{mwhere}: a waived milestone explains why in note")
            if terminal and status == "pending":
                errors.append(f"{mwhere}: a {stage} round has no pending milestones")
        if sum(item.get("amount", 0) for item in milestones if _amount(item.get("amount"), zero_ok=True)) != award["amount"]:
            errors.append(f"{awhere}: milestone amounts must add up to the award")

    if awards:
        measures = round_measures(round_)
        cap = program.get("concentration", {}).get("maximumRecipientShareOfRound", 1)
        if measures["largestRecipientShare"] is not None and measures["largestRecipientShare"] > cap + 1e-9:
            errors.append(f"{where}: one recipient receives {measures['largestRecipientShare']:.0%} of awards; the limit is {cap:.0%}")
        committed, admin = budget.get("committed"), budget.get("administrativeCost")
        if _amount(committed) and _amount(admin, zero_ok=True) and measures["fundsAwarded"] + admin > committed:
            errors.append(f"{where}: awards plus administrative cost exceed the committed pool")

    disbursement_ids = set()
    per_milestone: dict[tuple[str, str], int] = {}
    if round_.get("disbursements") and not _reached(round_, "disbursing"):
        errors.append(f"{where}: disbursements are recorded only once the round is disbursing")
    for disbursement in round_.get("disbursements", []):
        dwhere = f"{where} disbursement {disbursement.get('id')}"
        if disbursement.get("id") in disbursement_ids:
            errors.append(f"{dwhere}: id is not unique")
        disbursement_ids.add(disbursement.get("id"))
        award = awards.get(disbursement.get("award"))
        milestone = next((item for item in (award or {}).get("milestones", []) if item.get("id") == disbursement.get("milestone")), None)
        if award is None or milestone is None:
            errors.append(f"{dwhere}: names an unknown award milestone")
            continue
        if not _amount(disbursement.get("amount")):
            errors.append(f"{dwhere}: amount is required")
            continue
        if disbursement.get("rail") not in round_rails:
            errors.append(f"{dwhere}: rail must be one the round offers")
        if disbursement.get("status") not in DISBURSEMENT_STATUSES:
            errors.append(f"{dwhere}: status must be sent or returned")
        if disbursement.get("status") == "returned" and not _text(disbursement.get("note")):
            errors.append(f"{dwhere}: a returned payment explains what happened in note")
        if not _link_ok(disbursement.get("receipt"), root):
            errors.append(f"{dwhere}: needs a public receipt")
        paid = _day(disbursement.get("date"), dwhere, errors)
        if milestone.get("status") != "accepted":
            errors.append(f"{dwhere}: pays milestone {milestone.get('id')} before it is accepted")
        else:
            reported = _day(milestone.get("reported"), dwhere, errors)
            if paid is None or (reported and paid < reported):
                errors.append(f"{dwhere}: is dated before the milestone report")
        if disbursement.get("status") == "sent":
            key = (award["id"], milestone["id"])
            per_milestone[key] = per_milestone.get(key, 0) + disbursement["amount"]
            if per_milestone[key] > milestone.get("amount", 0):
                errors.append(f"{dwhere}: pays more than milestone {milestone['id']} is worth")

    community = round_.get("communityReview") or {}
    risks = set(program.get("communityReview", {}).get("requiredRisks", []))
    opened = _day(community.get("opened"), f"{where} communityReview", errors)
    closes = _day(community.get("closes"), f"{where} communityReview", errors)
    if opened and closes and (closes - opened).days < program.get("communityReview", {}).get("minimumCommentDays", 0):
        errors.append(f"{where}: community review is shorter than {program['communityReview']['minimumCommentDays']} days")
    if stage == "reported" and (opened is None or closes is None):
        errors.append(f"{where}: a reported round publishes its community review window")
    finding_ids = set()
    for finding in community.get("findings", []):
        fwhere = f"{where} finding {finding.get('id')}"
        if finding.get("id") in finding_ids:
            errors.append(f"{fwhere}: id is not unique")
        finding_ids.add(finding.get("id"))
        if finding.get("risk") not in risks:
            errors.append(f"{fwhere}: risk must be one of {', '.join(sorted(risks))}")
        for key in ("raisedBy", "statement"):
            if not _text(finding.get(key)):
                errors.append(f"{fwhere}: {key} is required")
        if finding.get("status") not in FINDING_STATUSES:
            errors.append(f"{fwhere}: status must be one of {', '.join(sorted(FINDING_STATUSES))}")
        elif finding["status"] != "open" and not _text(finding.get("correction")):
            errors.append(f"{fwhere}: a {finding['status']} finding records the correction or rationale")
        if _day(finding.get("recorded"), fwhere, errors) is None:
            errors.append(f"{fwhere}: recorded date is required")

    report = round_.get("outcomeReport")
    if terminal and not isinstance(report, dict):
        errors.append(f"{where}: a {stage} round publishes an outcome report")
    elif report is not None and not terminal:
        errors.append(f"{where}: only a reported or cancelled round has an outcome report")
    if isinstance(report, dict):
        if _day(report.get("published"), f"{where} outcomeReport", errors) is None or not _link_ok(report.get("href"), root):
            errors.append(f"{where}: outcome report needs a published date and public link")
        lessons = report.get("lessons")
        if not isinstance(lessons, list) or not lessons or not all(_text(item) for item in lessons):
            errors.append(f"{where}: outcome report lists lessons, including what did not work")
        if not _text(report.get("accessibilityImpact")):
            errors.append(f"{where}: outcome report describes accessibility impact")
        measures = round_measures(round_)
        for key in REPORTED_MEASURES:
            if report.get(key) != measures[key]:
                errors.append(f"{where}: outcome report {key} is {report.get(key)} but the ledger says {measures[key]}")
        retained = report.get("operatorsRetained")
        if not _amount(retained, zero_ok=True) or retained > measures["operatorsFunded"]:
            errors.append(f"{where}: operatorsRetained must be between 0 and operatorsFunded")


def validate(ledger: dict, root: Path = ROOT) -> None:
    errors: list[str] = []
    validate_program(ledger, root, errors)
    seen = set()
    for round_ in ledger["rounds"]:
        if isinstance(round_, dict):
            if round_.get("id") in seen:
                errors.append(f"round {round_.get('id')}: id is not unique")
            seen.add(round_.get("id"))
        validate_round(round_, ledger, root, errors)
    if errors:
        raise FundingError("\n".join(errors))


def _independent_voice(ledger: dict, name: str) -> bool:
    participant = _governance(ledger)[0].get(name)
    return participant is None or participant.get("constituency") != MAINTAINER


def evaluate(ledger: dict, as_of: date) -> dict:
    """Overdue reports, concentration warnings, measures, and closure blockers as of a date."""
    program = ledger["program"]
    grace = program["reporting"]["milestoneReportGraceDays"]
    overdue = []
    warnings = []
    rounds = []
    for round_ in sorted(ledger["rounds"], key=lambda item: item["id"]):
        rounds.append({"round": round_["id"], "stage": round_["stage"], "pilot": round_["pilot"], "measures": round_measures(round_)})
        warnings += concentration_warnings(round_, program)
        for award in round_.get("awards", []):
            for milestone in award.get("milestones", []):
                due = date.fromisoformat(milestone["due"])
                if milestone["status"] == "pending" and (as_of - due).days > grace:
                    overdue.append({"round": round_["id"], "award": award["id"], "milestone": milestone["id"], "due": milestone["due"], "awaiting": "milestone report"})
        report_due = round_["timeline"].get("finalReportDue")
        if report_due and round_["stage"] not in {"reported", "cancelled"} and as_of > date.fromisoformat(report_due):
            overdue.append({"round": round_["id"], "due": report_due, "awaiting": "outcome report"})

    blockers = []
    pilots = [
        round_ for round_ in ledger["rounds"]
        if round_["pilot"] and round_["stage"] == "reported"
        and round_.get("applications") and round_.get("awards") and round_.get("disbursements")
    ]
    if not pilots:
        blockers.append({"criterion": "pilot-round-reported", "detail": "no pilot round has published applications, conflict handling, selections, disbursements, milestones, and an outcome report"})
    paid = [
        round_ for round_ in pilots
        if all(any(item["award"] == award["id"] and item["status"] == "sent" for item in round_["disbursements"]) for award in round_["awards"])
    ]
    if not paid:
        blockers.append({"criterion": "recipients-paid-on-portable-rails", "detail": "no reported pilot round has paid every recipient on a rail they chose"})
    reviewed = []
    for round_ in paid:
        findings = round_["communityReview"].get("findings", [])
        independent = [item for item in findings if _independent_voice(ledger, item["raisedBy"])]
        covered = {item["risk"] for item in independent}
        open_findings = [item for item in findings if item["status"] == "open"]
        concentration_addressed = not concentration_warnings(round_, program) or any(
            item["risk"] == "concentration" and item["status"] != "open" for item in independent)
        if set(program["communityReview"]["requiredRisks"]) <= covered and not open_findings and concentration_addressed:
            reviewed.append(round_)
    if not reviewed:
        blockers.append({"criterion": "community-review-corrected", "detail": "no paid pilot round has independent community findings on concentration, accessibility, and conflict with every finding corrected or accepted with a rationale"})
    if overdue:
        blockers.append({"criterion": "reports-on-time", "detail": ", ".join(sorted({item["round"] for item in overdue})) + " has overdue reports"})
    return {
        "asOf": as_of.isoformat(),
        "programVersion": program["programVersion"],
        "programStatus": program["status"],
        "rounds": rounds,
        "overdue": overdue,
        "concentrationWarnings": warnings,
        "closureBlockers": blockers,
    }


def register_document(ledger: dict) -> dict:
    program = ledger["program"]
    participants, _ = _governance(ledger)
    rounds = []
    for round_ in sorted(ledger["rounds"], key=lambda item: item["id"]):
        public = {key: value for key, value in round_.items() if not key.startswith("_") and key != "schema"}
        public["measures"] = round_measures(round_)
        public["concentrationWarnings"] = concentration_warnings(round_, program)
        public["applicationScores"] = {
            application["id"]: application_score(round_, program, application["id"]) for application in round_.get("applications", [])
        }
        public["reviewerConstituencies"] = {
            reviewer["participant"]: participants.get(reviewer["participant"], {}).get("constituency") for reviewer in round_.get("reviewers", [])
        }
        rounds.append(public)
    keys = ("programVersion", "status", "currency", "versions", "changePolicy", "areas", "generalEligibility", "offers",
            "selectionCriteria", "scoreScale", "minimumScore", "minimumApplicationDays", "review", "conflicts",
            "concentration", "administrativeCost", "paymentRails", "independence", "reporting", "communityReview", "measures")
    return {"schema": REGISTER_SCHEMA, "guide": "/docs/library/funding/", **{key: program[key] for key in keys}, "rounds": rounds}


def _cell(value: Any) -> str:
    text = "—" if value in (None, "", []) else str(value)
    return text.replace("|", "\\|").replace("\n", " ")


def _money(value: Any, currency: str) -> str:
    return f"{currency} {value:,}" if isinstance(value, int) and not isinstance(value, bool) else "not published"


def _share(value: Any) -> str:
    return f"{value:.0%}" if isinstance(value, (int, float)) else "—"


def _href(href: str) -> str:
    return href if href.startswith("https://") else "https://app.forkmesh.com/forkmesh/forkmesh/blob/main/" + href


def render_markdown(document: dict) -> str:
    currency = document["currency"]
    lines = [
        "<!-- Generated by tools/public_interest_funding.py render. Do not edit. -->",
        "",
        "# Public-interest funding register",
        "",
        f"Program `{document['programVersion']}` · Status: **{document['status']}**. "
        "How to apply, review, and report is in the [program guide](README.md). "
        "The same data is published as searchable JSON at `/docs/funding/register.json`.",
        "",
        "## Offers",
        "",
        "| Offer | Status | Eligibility | Selection |",
        "| --- | --- | --- | --- |",
    ]
    for offer in document["offers"]:
        lines.append(f"| {offer['name']} | {offer['status']} | {_cell(offer['eligibility'])} | {_cell(offer['selection'])} |")
    lines += ["", "## Areas and eligibility", ""]
    for area in document["areas"]:
        lines += [f"- **{area['name']}** (`{area['id']}`): " + " ".join(area["eligibility"])]
    lines += ["", "Every application must also meet these rules:", ""]
    lines += [f"- {item}" for item in document["generalEligibility"]]
    review = document["review"]
    lines += [
        "", "## Selection criteria", "",
        "| Criterion | Weight | What reviewers score |", "| --- | --- | --- |",
    ]
    lines += [f"| {item['id']} | {item['weight']} | {_cell(item['description'])} |" for item in document["selectionCriteria"]]
    concentration = document["concentration"]
    lines += [
        "",
        f"Reviewers score each criterion from {document['scoreScale']['minimum']} to {document['scoreScale']['maximum']}. "
        f"An award needs a weighted score of at least {document['minimumScore']} out of 100, from at least "
        f"{review['minimumScoringReviewers']} reviewers, of whom at least {review['minimumIndependentReviewers']} "
        "is not a ForkMesh maintainer.",
        "",
        f"- Conflicts: {document['conflicts']['recusal']} {document['conflicts']['funders']}",
        f"- Concentration: at most {_share(concentration['maximumRecipientShareOfRound'])} of a round's awards to one recipient. "
        f"More than {_share(concentration['maximumAreaShareOfRound'])} to one area, or more than "
        f"{_share(concentration['maximumFunderShareOfRound'])} of the pool from one funder, must be addressed in community review.",
        f"- Administrative cost: at most {_share(document['administrativeCost']['maximumShareOfPool'])} of the pool. "
        + document["administrativeCost"]["statement"],
        "",
        "## Payment rails",
        "",
        "| Rail | Kind | Recurring | ForkMesh account or plan needed |",
        "| --- | --- | --- | --- |",
    ]
    for rail in document["paymentRails"]:
        needed = "yes" if rail["forkmeshAccountRequired"] or rail["forkmeshPlanRequired"] else "no"
        lines.append(f"| {rail['name']} | {rail['kind']} | {'yes' if rail['recurringSupported'] else 'no'} | {needed} |")
    independence = document["independence"]
    lines += [
        "",
        "## Independence",
        "",
        independence["statement"],
        "",
        "Awards never grant: " + ", ".join(f"`{item}`" for item in independence["awardsNeverGrant"]) + ".",
        "",
        "The only obligations an award may carry: " + ", ".join(f"`{item}`" for item in independence["allowedObligations"]) + ".",
        "",
        "## Rounds",
        "",
        "| Round | Title | Pilot | Stage | Pool | Awarded | Disbursed | Recipients |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for round_ in document["rounds"]:
        measures = round_["measures"]
        lines.append(
            f"| [{round_['id']}](#{round_['id'].lower()}) | {_cell(round_['title'])} | {'yes' if round_['pilot'] else 'no'} "
            f"| {round_['stage']} | {_money(measures['fundsCommitted'], currency)} | {_money(measures['fundsAwarded'], currency)} "
            f"| {_money(measures['fundsDisbursed'], currency)} | {measures['recipients']} |"
        )
    for round_ in document["rounds"]:
        measures = round_["measures"]
        lines += ["", f"### {round_['id']}", "", f"**{round_['title']}**", "", round_["summary"], ""]
        lines.append(f"- Areas: {', '.join(round_['areas'])}. Offers: {', '.join(round_['offers'])}.")
        lines.append(f"- Payment rails: {', '.join(round_['paymentRails'])}.")
        lines.append("- Apply through: " + " ".join(channel["description"] for channel in round_["applicationChannels"]))
        lines.append("- Timeline: " + "; ".join(f"{key} {_cell(value)}" for key, value in round_["timeline"].items()))
        lines.append(f"- Pool: {_money(measures['fundsCommitted'], currency)}. Administrative cost: {_money(measures['administrativeCost'], currency)}.")
        if round_.get("opensWhen") and round_["stage"] == "draft":
            lines += ["", "The round opens when:", ""] + [f"- {item}" for item in round_["opensWhen"]]
        lines += ["", "#### Funders", ""]
        if round_["funders"]:
            lines += ["| Funder | Kind | Amount | Conflicts | Restricted to |", "| --- | --- | --- | --- | --- |"]
            for funder in round_["funders"]:
                lines.append(f"| {_cell(funder['name'])} | {funder['kind']} | {_money(funder['amount'], currency)} "
                             f"| {_cell('; '.join(funder['conflicts']))} | {_cell(', '.join(funder.get('restrictions', {}).get('areas', [])))} |")
        else:
            lines.append("None committed.")
        lines += ["", "#### Reviewers", ""]
        if round_["reviewers"]:
            lines += [f"- {item['participant']} ({round_['reviewerConstituencies'][item['participant']]}), "
                      f"disclosure [{item['disclosure']}](../governance/decision-register.md#{item['disclosure'].lower()})" for item in round_["reviewers"]]
        else:
            lines.append("None seated.")
        lines += ["", "#### Applications and selections", ""]
        awarded = {award["application"]: award for award in round_["awards"]}
        if round_["applications"]:
            lines += ["| Application | Applicant | Area | Offer | Requested | Eligible | Score | Award |",
                      "| --- | --- | --- | --- | --- | --- | --- | --- |"]
            for application in round_["applications"]:
                award = awarded.get(application["id"])
                eligible = "yes" if application["eligible"] else "no: " + application.get("eligibilityNote", "")
                lines.append(
                    f"| {application['id']} | [{_cell(application['applicant']['name'])}]({application['applicant']['source']}) "
                    f"| {application['area']} | {application['offer']} | {_money(application['requested'], currency)} | {_cell(eligible)} "
                    f"| {_cell(round_['applicationScores'][application['id']])} | {_money(award['amount'], currency) if award else 'not selected'} |"
                )
        else:
            lines.append("No applications.")
        lines += ["", "#### Conflict handling", ""]
        lines += [f"- {item['reviewer']} recused from {item['application']}: {item['reason']}" for item in round_["conflicts"]] or ["No conflicts recorded."]
        lines += ["", "#### Milestones and disbursements", ""]
        if round_["awards"]:
            lines += ["| Award | Milestone | Deliverable | Due | Amount | Status | Report |", "| --- | --- | --- | --- | --- | --- | --- |"]
            for award in round_["awards"]:
                for milestone in award["milestones"]:
                    report = f"[report]({_href(milestone['report'])})" if milestone.get("report") else "—"
                    lines.append(f"| {award['id']} | {milestone['id']} | {_cell(milestone['deliverable'])} | {milestone['due']} "
                                 f"| {_money(milestone['amount'], currency)} | {milestone['status']} | {report} |")
            lines += ["", "| Disbursement | Award | Milestone | Amount | Rail | Date | Status |", "| --- | --- | --- | --- | --- | --- | --- |"]
            for item in round_["disbursements"]:
                lines.append(f"| [{item['id']}]({_href(item['receipt'])}) | {item['award']} | {item['milestone']} "
                             f"| {_money(item['amount'], currency)} | {item['rail']} | {item['date']} | {item['status']} |")
        else:
            lines.append("No awards.")
        lines += ["", "#### Community review", ""]
        review_window = round_["communityReview"]
        lines.append(f"Window: {_cell(review_window.get('opened'))} to {_cell(review_window.get('closes'))}.")
        for finding in review_window.get("findings", []):
            lines.append(f"- **{finding['id']}** ({finding['risk']}, {finding['status']}), raised by {finding['raisedBy']}: "
                         f"{finding['statement']}" + (f" Correction: {finding['correction']}" if finding.get("correction") else ""))
        for warning in round_["concentrationWarnings"]:
            lines.append(f"- Concentration warning: one {warning['subject']} holds {_share(warning['share'])} (limit {_share(warning['limit'])}).")
        lines += ["", "#### Outcome report", ""]
        report = round_.get("outcomeReport")
        if report:
            lines.append(f"[Published {report['published']}]({_href(report['href'])}).")
            lines.append(f"- Raised {_money(measures['fundsRaised'], currency)}, awarded {_money(measures['fundsAwarded'], currency)}, "
                         f"disbursed {_money(measures['fundsDisbursed'], currency)}, administrative cost {_money(measures['administrativeCost'], currency)}.")
            lines.append(f"- {measures['applications']} applications, {measures['recipients']} recipients, "
                         f"{measures['milestonesAccepted']} of {measures['milestonesTotal']} milestones accepted.")
            lines.append(f"- Volunteer-mirror operators funded {measures['operatorsFunded']}, still operating {report['operatorsRetained']}.")
            lines.append(f"- Largest shares: recipient {_share(measures['largestRecipientShare'])}, area {_share(measures['largestAreaShare'])}, "
                         f"funder {_share(measures['largestFunderShare'])}.")
            lines.append(f"- Accessibility impact: {report['accessibilityImpact']}")
            lines += ["- Lessons:"] + [f"  - {item}" for item in report["lessons"]]
        else:
            lines.append("Not published.")
        if round_.get("note"):
            lines += ["", f"> {round_['note']}"]
    return "\n".join(lines) + "\n"


def generated_outputs(ledger: dict) -> dict[Path, str]:
    document = register_document(ledger)
    return {
        REGISTER_MARKDOWN_PATH: render_markdown(document),
        REGISTER_JSON_PATH: json.dumps(document, indent=2, ensure_ascii=False) + "\n",
    }


def render(ledger: dict, *, check: bool, root: Path = ROOT) -> list[str]:
    stale = []
    for relative, text in generated_outputs(ledger).items():
        path = root / relative
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        stale.append(str(relative))
        if not check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
    return stale


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate", help="validate the program and every round")
    render_command = commands.add_parser("render", help="regenerate the public funding register")
    render_command.add_argument("--check", action="store_true", help="fail when generated outputs are stale")
    commands.add_parser("gate", help="validate and check generated outputs; independent of today's date")
    for name, help_text in (("status", "print measures, overdue reports, concentration warnings, and closure blockers"),
                            ("closure", "exit non-zero while any closure criterion is unmet")):
        command = commands.add_parser(name, help=help_text)
        command.add_argument("--as-of", type=date.fromisoformat, default=None)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        ledger = load_ledger()
        validate(ledger)
        if args.command == "validate":
            print("funding ledger is valid")
            return 0
        if args.command in {"render", "gate"}:
            stale = render(ledger, check=args.command == "gate" or args.check)
            if stale and (args.command == "gate" or args.check):
                print("stale funding outputs: " + ", ".join(stale) + "; run python3 tools/public_interest_funding.py render", file=sys.stderr)
                return 1
            print("funding register is current" if not stale else "rendered " + ", ".join(stale))
            return 0
        status = evaluate(ledger, args.as_of or date.today())
        print(json.dumps(status, indent=2))
        return 1 if args.command == "closure" and status["closureBlockers"] else 0
    except FundingError as exc:
        print(f"public-interest-funding: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
