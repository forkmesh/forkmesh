#!/usr/bin/env python3
"""Protocol and ecosystem governance ledger (issue #513).

The charter, participants, conflict and funding disclosures, and one record per
proposal, security decision, or appeal live in ``conformance/governance/v1``.
``validate`` checks that records follow the charter: timelines meet the
published minimums, every author and voter links a disclosure, every "no" vote
carries recorded dissent, quorums include the required constituencies, and
protected rights cite real business-model commitments. ``render`` regenerates
the public decision register (Markdown and searchable JSON) and ``--check``
fails when either is stale. ``gate`` runs both and never depends on today's
date. ``status`` and ``closure`` evaluate overdue deadlines, stale disclosures,
and the epic's closure criteria as of a date.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import date
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
GOVERNANCE_DIRECTORY = Path("conformance/governance/v1")
CHARTER_PATH = GOVERNANCE_DIRECTORY / "charter.json"
PARTICIPANTS_PATH = GOVERNANCE_DIRECTORY / "participants.json"
RECORDS_DIRECTORY = GOVERNANCE_DIRECTORY / "records"
REGISTER_MARKDOWN_PATH = Path("www/docs/governance/decision-register.md")
REGISTER_JSON_PATH = Path("www/public/docs/governance/register.json")
THREATS_PATH = Path("conformance/protocol/v1/threats.json")
BUSINESS_MODEL_PATH = Path("www/public/business-model.json")

CHARTER_SCHEMA = "forkmesh-governance-charter-v1"
PARTICIPANTS_SCHEMA = "forkmesh-governance-participants-v1"
RECORD_SCHEMA = "forkmesh-governance-record-v1"
REGISTER_SCHEMA = "forkmesh-governance-register-v1"

PROCESS_BODY_SCOPE = {"proposal": None, "security": "security", "appeal": "appeal"}
DECIDED_STAGES = {"proposal": {"decided", "implemented"}, "security": {"decided"}, "appeal": {"decided"}}
STAGES = {
    "proposal": None,
    "security": ["reported", "embargoed", "decided", "withdrawn"],
    "appeal": ["filed", "hearing", "decided", "withdrawn"],
}
VOTES = {"yes", "no", "abstain"}
INDEPENDENT_CONSTITUENCIES = ("independent-implementer", "community-operator")
# The value each business-model commitment must hold for a protected right to be intact.
COMMITMENT_EXPECTED = {"independentImplementationsAllowed": True, "noDataSale": True}
TIMELINE_KEYS = {
    "proposal": ["opened", "reviewCloses", "lastCallCloses", "decided", "implemented"],
    "security": ["opened", "disclosed", "embargoEnds", "decided", "ratified"],
    "appeal": ["filed", "decided"],
}


class GovernanceError(ValueError):
    pass


def _json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise GovernanceError(f"{path}: {exc}") from exc


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


def load_ledger(root: Path = ROOT) -> dict:
    records = []
    directory = root / RECORDS_DIRECTORY
    for path in sorted(directory.glob("*.json")):
        record = _json(path)
        if isinstance(record, dict):
            record["_file"] = path.stem
        records.append(record)
    return {
        "charter": _json(root / CHARTER_PATH),
        "participants": _json(root / PARTICIPANTS_PATH),
        "records": records,
        "threats": _json(root / THREATS_PATH),
        "businessModel": _json(root / BUSINESS_MODEL_PATH),
    }


def _bodies(charter: dict) -> dict[str, dict]:
    return {body["id"]: body for body in charter.get("bodies", []) if isinstance(body, dict) and "id" in body}


def _seats(charter: dict) -> dict[str, tuple[str, dict]]:
    seats = {}
    for body in _bodies(charter).values():
        for seat in body.get("seats", []):
            seats[seat["id"]] = (body["id"], seat)
    return seats


def validate_charter(ledger: dict, errors: list[str]) -> None:
    charter = ledger["charter"]
    if charter.get("schema") != CHARTER_SCHEMA:
        errors.append(f"charter: schema must be {CHARTER_SCHEMA}")
    constituencies = charter.get("constituencies", {})
    seen_seats: set[str] = set()
    bodies = charter.get("bodies", [])
    if len(_bodies(charter)) != len(bodies):
        errors.append("charter: body ids must be unique")
    for body in bodies:
        where = f"charter body {body.get('id')}"
        for seat in body.get("seats", []):
            if seat.get("id") in seen_seats:
                errors.append(f"{where}: seat {seat.get('id')} is not unique")
            seen_seats.add(seat.get("id"))
            if seat.get("constituency") not in constituencies:
                errors.append(f"{where}: seat {seat.get('id')} has unknown constituency")
        quorum = body.get("quorum", {})
        represented = {seat.get("constituency") for seat in body.get("seats", [])}
        for required in quorum.get("requiredConstituencies", []):
            if required not in represented:
                errors.append(f"{where}: quorum requires {required} but no seat belongs to it")
        if not 0 < quorum.get("minimumVoters", 0) <= len(body.get("seats", [])):
            errors.append(f"{where}: minimumVoters must be between 1 and the seat count")
        share = quorum.get("maximumShareFromOneEmployerOrFunder")
        if not isinstance(share, (int, float)) or not 0 < share < 1:
            errors.append(f"{where}: maximumShareFromOneEmployerOrFunder must be below 1")
    decides = {scope for body in bodies for scope in body.get("decides", [])}
    for scope in ("protocol", "security", "appeal", "charter"):
        if scope not in decides:
            errors.append(f"charter: no body decides {scope}")
    if charter.get("adoptionRecord") and not any(
        isinstance(record, dict) and record.get("id") == charter["adoptionRecord"] for record in ledger["records"]
    ):
        errors.append(f"charter: adoption record {charter['adoptionRecord']} does not exist")
    commitments = ledger["businessModel"].get("permanentCommitments", {})
    right_ids = set()
    for right in charter.get("protectedRights", []):
        right_ids.add(right.get("id"))
        if not right.get("commitments"):
            errors.append(f"charter right {right.get('id')}: must cite business-model commitments")
        for commitment in right.get("commitments", []):
            if commitment not in commitments:
                errors.append(f"charter right {right.get('id')}: unknown commitment {commitment}")
            elif commitments[commitment] is not COMMITMENT_EXPECTED.get(commitment, False):
                errors.append(f"charter right {right.get('id')}: business-model commitment {commitment} no longer holds")
    if "token-free-governance" not in right_ids:
        errors.append("charter: token-free-governance must stay a protected right")
    if not charter.get("moderationBaseline"):
        errors.append("charter: moderation baseline must not be empty")
    for key in ("vacantSeatFilledWithinDays", "interimHolder", "stewardshipTransfer"):
        if not charter.get("succession", {}).get(key):
            errors.append(f"charter: succession.{key} is required")


def validate_participants(ledger: dict, errors: list[str]) -> None:
    charter = ledger["charter"]
    document = ledger["participants"]
    if document.get("schema") != PARTICIPANTS_SCHEMA:
        errors.append(f"participants: schema must be {PARTICIPANTS_SCHEMA}")
    constituencies = charter.get("constituencies", {})
    participants = {}
    for participant in document.get("participants", []):
        if participant.get("id") in participants:
            errors.append(f"participant {participant.get('id')}: id is not unique")
        participants[participant.get("id")] = participant
        if participant.get("constituency") not in constituencies:
            errors.append(f"participant {participant.get('id')}: unknown constituency")
        if not participant.get("employerOrFunder"):
            errors.append(f"participant {participant.get('id')}: employerOrFunder is required (use \"none\")")
        if participant.get("constituency") in INDEPENDENT_CONSTITUENCIES:
            source = participant.get("source", "")
            if not isinstance(source, str) or not source.startswith("https://"):
                errors.append(f"participant {participant.get('id')}: independent participants need a public https source")
            if "forkmesh" in str(participant.get("employerOrFunder", "")).casefold():
                errors.append(f"participant {participant.get('id')}: paid by the ForkMesh operator, so not independent")
    seats = _seats(charter)
    held: set[str] = set()
    bodies_by_participant: dict[tuple[str, str], str] = {}
    for holder in document.get("seatHolders", []):
        where = f"seat holder {holder.get('seat')}"
        if holder.get("seat") not in seats:
            errors.append(f"{where}: unknown seat")
            continue
        if holder.get("seat") in held:
            errors.append(f"{where}: seat has two holders")
        held.add(holder.get("seat"))
        body_id, seat = seats[holder["seat"]]
        participant = participants.get(holder.get("participant"))
        if participant is None:
            errors.append(f"{where}: unknown participant {holder.get('participant')}")
            continue
        if participant.get("kind") == "group":
            errors.append(f"{where}: a seat is held by a person, not a group")
        if participant.get("constituency") != seat.get("constituency"):
            errors.append(f"{where}: participant constituency does not match the seat")
        key = (body_id, participant["id"])
        if key in bodies_by_participant:
            errors.append(f"{where}: {participant['id']} already holds {bodies_by_participant[key]} in {body_id}")
        bodies_by_participant[key] = holder["seat"]
        since = _day(holder.get("since"), where, errors)
        until = _day(holder.get("termEnds"), where, errors)
        if since is None or until is None or until <= since:
            errors.append(f"{where}: since and termEnds are required and termEnds follows since")
    kinds = set(charter.get("disclosures", {}).get("fundingKinds", []))
    disclosure_ids = set()
    for disclosure in document.get("disclosures", []):
        where = f"disclosure {disclosure.get('id')}"
        if disclosure.get("id") in disclosure_ids:
            errors.append(f"{where}: id is not unique")
        disclosure_ids.add(disclosure.get("id"))
        if disclosure.get("participant") not in participants:
            errors.append(f"{where}: unknown participant")
        if _day(disclosure.get("filed"), where, errors) is None:
            errors.append(f"{where}: filed date is required")
        if not isinstance(disclosure.get("conflicts"), list):
            errors.append(f"{where}: conflicts must be a list (empty when there are none)")
        funding = disclosure.get("funding")
        if not isinstance(funding, list) or not funding:
            errors.append(f"{where}: funding must list at least one entry (kind \"none\" when unfunded)")
            continue
        for entry in funding:
            if entry.get("kind") not in kinds:
                errors.append(f"{where}: unknown funding kind {entry.get('kind')}")
            if not isinstance(entry.get("amountPublished"), bool):
                errors.append(f"{where}: amountPublished must be true or false")
            if entry.get("amountPublished") and not entry.get("amount"):
                errors.append(f"{where}: amountPublished requires an amount")


def _local_link_exists(href: str, root: Path) -> bool:
    return href.startswith("https://") or (root / href.split("#", 1)[0]).exists()


def _quorum_failures(record: dict, ledger: dict) -> list[str]:
    charter = ledger["charter"]
    body = _bodies(charter).get(record.get("decidingBody"))
    if body is None:
        return ["no deciding body"]
    participants = {item["id"]: item for item in ledger["participants"].get("participants", [])}
    holders = {item["seat"]: item["participant"] for item in ledger["participants"].get("seatHolders", [])}
    seats = {seat["id"]: seat for seat in body.get("seats", [])}
    failures = []
    voters = [vote for vote in record.get("votes", []) if vote.get("vote") in VOTES]
    quorum = body.get("quorum", {})
    if len(voters) < quorum.get("minimumVoters", 1):
        failures.append(f"{len(voters)} voters, quorum needs {quorum.get('minimumVoters')}")
    present = {seats[vote["seat"]]["constituency"] for vote in voters if vote.get("seat") in seats}
    for required in quorum.get("requiredConstituencies", []):
        if required not in present:
            failures.append(f"no {required} voted")
    funders: dict[str, int] = {}
    for vote in voters:
        funder = str(participants.get(vote.get("participant"), {}).get("employerOrFunder", "")).casefold()
        if funder and funder != "none":
            funders[funder] = funders.get(funder, 0) + 1
    limit = quorum.get("maximumShareFromOneEmployerOrFunder", 0.5)
    for funder, count in funders.items():
        if voters and count / len(voters) > limit:
            failures.append(f"{funder} holds {count} of {len(voters)} votes")
    for vote in voters:
        if holders.get(vote.get("seat")) != vote.get("participant"):
            failures.append(f"{vote.get('participant')} does not hold seat {vote.get('seat')}")
    if record.get("narrowsProtectedRights") and record.get("outcome") == "accepted":
        yes = sum(1 for vote in voters if vote["vote"] == "yes")
        if voters and yes * 3 < len(voters) * 2:
            failures.append("narrowing a protected right needs two-thirds")
        for seat_id, participant in holders.items():
            seat = seats.get(seat_id)
            if seat and seat["constituency"] in INDEPENDENT_CONSTITUENCIES and not any(
                vote.get("seat") == seat_id and vote["vote"] == "yes" for vote in voters
            ):
                failures.append(f"narrowing a protected right needs a yes from independent seat {seat_id}")
    return failures


def validate_record(record: Any, ledger: dict, root: Path, errors: list[str]) -> None:
    if not isinstance(record, dict):
        errors.append("record: must be an object")
        return
    charter = ledger["charter"]
    record_id = record.get("id")
    where = f"record {record_id}"
    if record.get("schema") != RECORD_SCHEMA:
        errors.append(f"{where}: schema must be {RECORD_SCHEMA}")
    if record.get("_file") != record_id:
        errors.append(f"{where}: file name must match the record id")
    process = record.get("process")
    rules = charter.get("processes", {}).get(process)
    if process not in PROCESS_BODY_SCOPE or rules is None:
        errors.append(f"{where}: unknown process {process}")
        return
    if not str(record_id).startswith(rules["recordPrefix"] + "-"):
        errors.append(f"{where}: {process} ids start with {rules['recordPrefix']}-")
    under_charter = record.get("adoptedUnder") is not None
    if under_charter and record["adoptedUnder"] != charter.get("charterVersion"):
        errors.append(f"{where}: adoptedUnder must be the current charter version or null for pre-charter records")
    if not under_charter and not record.get("note"):
        errors.append(f"{where}: a pre-charter record must explain itself in note")
    for key in ("title", "summary"):
        if not isinstance(record.get(key), str) or not record[key].strip():
            errors.append(f"{where}: {key} is required")

    participants = {item["id"]: item for item in ledger["participants"].get("participants", [])}
    disclosures = {item["id"]: item for item in ledger["participants"].get("disclosures", [])}
    linked = record.get("disclosures", [])
    for disclosure_id in linked:
        if disclosure_id not in disclosures:
            errors.append(f"{where}: unknown disclosure {disclosure_id}")
    disclosed = {disclosures[item]["participant"] for item in linked if item in disclosures}
    named = list(record.get("authors", [])) + [vote.get("participant") for vote in record.get("votes", [])]
    if not record.get("authors"):
        errors.append(f"{where}: at least one author is required")
    for participant in named:
        if participant not in participants:
            errors.append(f"{where}: unknown participant {participant}")
        elif participant not in disclosed:
            errors.append(f"{where}: {participant} takes part without a linked disclosure")

    stages = STAGES[process] or rules.get("stages", [])
    if record.get("stage") not in stages:
        errors.append(f"{where}: stage must be one of {', '.join(stages)}")
    decided = record.get("stage") in DECIDED_STAGES[process]
    if decided and record.get("outcome") not in rules.get("outcomes", []):
        errors.append(f"{where}: a decided record needs an outcome from {', '.join(rules.get('outcomes', []))}")
    if not decided and record.get("outcome") not in (None, "withdrawn"):
        errors.append(f"{where}: only a decided record has an outcome")

    timeline = record.get("timeline", {})
    if set(timeline) != set(TIMELINE_KEYS[process]):
        errors.append(f"{where}: timeline keys must be {', '.join(TIMELINE_KEYS[process])}")
    days = {key: _day(timeline.get(key), f"{where} timeline.{key}", errors) for key in TIMELINE_KEYS[process]}
    ordered = [days[key] for key in TIMELINE_KEYS[process] if days.get(key) and key != "embargoEnds"]
    if ordered != sorted(ordered):
        errors.append(f"{where}: timeline dates must not go backwards")
    start = days.get("opened") or days.get("filed")
    if start is None:
        errors.append(f"{where}: timeline needs its opening date")
    if decided and days.get("decided") is None:
        errors.append(f"{where}: a decided record needs timeline.decided")

    if process == "proposal" and under_charter:
        minimum = rules["breakingChangeMinimumReviewDays"] if record.get("breaking") else rules["minimumReviewDays"]
        if start and days["reviewCloses"] is None:
            errors.append(f"{where}: reviewCloses is required")
        elif start and (days["reviewCloses"] - start).days < minimum:
            errors.append(f"{where}: review window is shorter than {minimum} days")
        if decided:
            if days["lastCallCloses"] is None or days["reviewCloses"] is None:
                errors.append(f"{where}: a decided proposal needs a last call")
            elif (days["lastCallCloses"] - days["reviewCloses"]).days < rules["minimumLastCallDays"]:
                errors.append(f"{where}: last call is shorter than {rules['minimumLastCallDays']} days")
            if days["decided"] and days["lastCallCloses"] and days["decided"] < days["lastCallCloses"]:
                errors.append(f"{where}: decided before last call closed")
    if process == "security":
        for threat in record.get("threats", []):
            if threat not in {item["id"] for item in ledger["threats"].get("threats", [])}:
                errors.append(f"{where}: unknown threat {threat}")
        if under_charter and start and days["embargoEnds"]:
            if (days["embargoEnds"] - start).days > rules["maximumEmbargoDays"]:
                errors.append(f"{where}: embargo exceeds {rules['maximumEmbargoDays']} days")
    if process == "appeal":
        appealed = next((item for item in ledger["records"] if isinstance(item, dict) and item.get("id") == record.get("appeals")), None)
        if appealed is None:
            errors.append(f"{where}: appeals must name an existing record")
        else:
            appealed_decided = _day(appealed.get("timeline", {}).get("decided"), where, errors)
            if appealed_decided is None:
                errors.append(f"{where}: the appealed record has no decision")
            elif start and (start - appealed_decided).days > rules["fileWithinDaysOfDecision"]:
                errors.append(f"{where}: filed more than {rules['fileWithinDaysOfDecision']} days after the decision")
            original = set(appealed.get("authors", [])) | {vote.get("participant") for vote in appealed.get("votes", [])}
            for vote in record.get("votes", []):
                if vote.get("participant") in original:
                    errors.append(f"{where}: {vote.get('participant')} took part in the appealed decision")

    body = _bodies(charter).get(record.get("decidingBody"))
    if record.get("decidingBody") is not None and body is None:
        errors.append(f"{where}: unknown deciding body")
    if under_charter:
        scope = PROCESS_BODY_SCOPE[process]
        allowed = set(record.get("affects", [])) if scope is None else {scope}
        if body is None:
            errors.append(f"{where}: a charter record names its deciding body")
        elif not allowed & set(body.get("decides", [])):
            errors.append(f"{where}: {body['id']} does not decide {', '.join(sorted(allowed))}")
        if not record.get("dissentSolicited"):
            errors.append(f"{where}: a charter record must solicit dissent")
        if decided:
            for failure in _quorum_failures(record, ledger):
                errors.append(f"{where}: quorum not met: {failure}")
    if record.get("narrowsProtectedRights") and not record.get("breaking"):
        errors.append(f"{where}: narrowing a protected right is a breaking change")
    right_ids = {right.get("id") for right in charter.get("protectedRights", [])}
    for right in record.get("protectedRights", []):
        if right not in right_ids:
            errors.append(f"{where}: unknown protected right {right}")
    for vote in record.get("votes", []):
        if vote.get("vote") not in VOTES:
            errors.append(f"{where}: vote must be yes, no, or abstain")
        if body is not None and vote.get("seat") not in {seat["id"] for seat in body.get("seats", [])}:
            errors.append(f"{where}: seat {vote.get('seat')} is not in {body['id']}")
    dissenters = set()
    for entry in record.get("dissent", []):
        if entry.get("participant") not in participants:
            errors.append(f"{where}: dissent from unknown participant {entry.get('participant')}")
        if not isinstance(entry.get("statement"), str) or not entry["statement"].strip():
            errors.append(f"{where}: dissent needs a statement")
        _day(entry.get("recorded"), f"{where} dissent", errors)
        dissenters.add(entry.get("participant"))
    for vote in record.get("votes", []):
        if vote.get("vote") == "no" and vote.get("participant") not in dissenters:
            errors.append(f"{where}: {vote.get('participant')} voted no without recorded dissent")
    for link in record.get("links", []):
        if not _local_link_exists(str(link.get("href", "")), root):
            errors.append(f"{where}: link {link.get('href')} does not exist")


def validate(ledger: dict, root: Path = ROOT) -> None:
    errors: list[str] = []
    validate_charter(ledger, errors)
    validate_participants(ledger, errors)
    seen = set()
    for record in ledger["records"]:
        if isinstance(record, dict):
            if record.get("id") in seen:
                errors.append(f"record {record.get('id')}: id is not unique")
            seen.add(record.get("id"))
        validate_record(record, ledger, root, errors)
    if errors:
        raise GovernanceError("\n".join(errors))


def _deadline(start: date | None, days: int | None) -> date | None:
    if start is None or days is None:
        return None
    return date.fromordinal(start.toordinal() + days)


def _current_disclosure(ledger: dict, participant: str, as_of: date) -> dict | None:
    freshness = ledger["charter"]["disclosures"]["freshnessDays"]
    filed = [
        item for item in ledger["participants"].get("disclosures", [])
        if item["participant"] == participant and 0 <= (as_of - date.fromisoformat(item["filed"])).days <= freshness
    ]
    return max(filed, key=lambda item: item["filed"], default=None)


def evaluate(ledger: dict, as_of: date) -> dict:
    """Deadlines, disclosure freshness, and closure blockers as of a date."""
    charter = ledger["charter"]
    processes = charter["processes"]
    overdue = []
    for record in ledger["records"]:
        if record.get("adoptedUnder") is None or record.get("stage") in {"withdrawn", "implemented"}:
            continue
        timeline = record["timeline"]
        process = record["process"]
        due = None
        if process == "proposal" and record["stage"] in {"review", "last-call"}:
            if timeline.get("lastCallCloses"):
                due = _deadline(date.fromisoformat(timeline["lastCallCloses"]), processes["proposal"]["decisionDueDaysAfterLastCall"])
                label = "decision"
            elif timeline.get("reviewCloses"):
                due = date.fromisoformat(timeline["reviewCloses"])
                label = "last call or withdrawal"
        elif process == "security" and not timeline.get("ratified") and timeline.get("disclosed"):
            due = _deadline(date.fromisoformat(timeline["disclosed"]), processes["security"]["ratifyWithinDaysOfDisclosure"])
            label = "ratification"
        elif process == "appeal" and record["stage"] != "decided":
            due = _deadline(date.fromisoformat(timeline["filed"]), processes["appeal"]["decisionDueDaysAfterFiling"])
            label = "appeal decision"
        if due is not None and as_of > due:
            overdue.append({"record": record["id"], "due": due.isoformat(), "awaiting": label})

    holders = ledger["participants"].get("seatHolders", [])
    active = {holder["participant"] for holder in holders}
    for record in ledger["records"]:
        if record.get("adoptedUnder") is not None and record.get("stage") not in {"withdrawn", "implemented"}:
            active.update(record.get("authors", []))
    stale_disclosures = sorted(participant for participant in active if _current_disclosure(ledger, participant, as_of) is None)
    unpublished_amounts = sorted({
        item["id"] for item in ledger["participants"].get("disclosures", [])
        for entry in item.get("funding", []) if not entry.get("amountPublished") and entry.get("kind") != "none"
    })

    seats = _seats(charter)
    council = {holder["seat"]: holder["participant"] for holder in holders if seats.get(holder["seat"], ("",))[0] == "protocol-council"}
    filled = {seats[seat][1]["constituency"] for seat in council}
    blockers = []

    def completed(process: str) -> list[dict]:
        done = []
        for record in ledger["records"]:
            if record.get("process") != process or record.get("adoptedUnder") is None:
                continue
            if record.get("stage") not in DECIDED_STAGES[process] or not record.get("dissentSolicited"):
                continue
            if process == "security" and not record["timeline"].get("ratified"):
                continue
            if _quorum_failures(record, ledger):
                continue
            done.append(record)
        return done

    proposals = completed("proposal")
    security = completed("security")
    if not proposals:
        blockers.append({"criterion": "proposal-completed", "detail": "no proposal adopted under the charter has been decided with a quorum, timeline, and solicited dissent"})
    if not security:
        blockers.append({"criterion": "security-decision-completed", "detail": "no security decision adopted under the charter has been decided and ratified with a quorum"})
    missing = [constituency for constituency in INDEPENDENT_CONSTITUENCIES if constituency not in filled]
    if missing:
        blockers.append({"criterion": "independent-roles", "detail": "protocol council has no " + " or ".join(missing) + " seat holder"})
    else:
        voted = {
            seats[vote["seat"]][1]["constituency"]
            for record in proposals + security for vote in record.get("votes", []) if vote.get("seat") in seats
        }
        if not all(constituency in voted for constituency in INDEPENDENT_CONSTITUENCIES):
            blockers.append({"criterion": "independent-roles", "detail": "independent seat holders have not voted in a completed decision"})
    if stale_disclosures or unpublished_amounts:
        parts = []
        if stale_disclosures:
            parts.append("no current disclosure for " + ", ".join(stale_disclosures))
        if unpublished_amounts:
            parts.append("funding amounts unpublished in " + ", ".join(unpublished_amounts))
        blockers.append({"criterion": "disclosures-current", "detail": "; ".join(parts)})
    if overdue:
        blockers.append({"criterion": "timelines-met", "detail": ", ".join(item["record"] for item in overdue) + " overdue"})
    return {
        "asOf": as_of.isoformat(),
        "charterVersion": charter["charterVersion"],
        "charterStatus": charter["status"],
        "overdue": overdue,
        "staleDisclosures": stale_disclosures,
        "closureBlockers": blockers,
    }


def _public_record(record: dict, disclosures: dict[str, dict]) -> dict:
    public = {key: value for key, value in record.items() if not key.startswith("_") and key != "schema"}
    public["countsForCharter"] = record.get("adoptedUnder") is not None
    public["disclosedParticipants"] = sorted({disclosures[item]["participant"] for item in record.get("disclosures", []) if item in disclosures})
    return public


def register_document(ledger: dict) -> dict:
    charter = ledger["charter"]
    participants = ledger["participants"]
    disclosures = {item["id"]: item for item in participants.get("disclosures", [])}
    holders = {holder["seat"]: holder for holder in participants.get("seatHolders", [])}
    bodies = []
    for body in charter["bodies"]:
        bodies.append({
            "id": body["id"],
            "name": body["name"],
            "decides": body["decides"],
            "seats": [
                {
                    "id": seat["id"],
                    "constituency": seat["constituency"],
                    "holder": holders.get(seat["id"], {}).get("participant"),
                    "since": holders.get(seat["id"], {}).get("since"),
                    "termEnds": holders.get(seat["id"], {}).get("termEnds"),
                }
                for seat in body["seats"]
            ],
        })
    records = sorted(ledger["records"], key=lambda record: record["id"])
    return {
        "schema": REGISTER_SCHEMA,
        "charterVersion": charter["charterVersion"],
        "charterStatus": charter["status"],
        "guide": "/docs/library/governance/",
        "bodies": bodies,
        "participants": participants.get("participants", []),
        "disclosures": [
            dict(item, linkedRecords=[record["id"] for record in records if item["id"] in record.get("disclosures", [])])
            for item in participants.get("disclosures", [])
        ],
        "records": [_public_record(record, disclosures) for record in records],
    }


def _cell(value: Any) -> str:
    text = "—" if value in (None, "", []) else str(value)
    return text.replace("|", "\\|").replace("\n", " ")


def render_markdown(document: dict) -> str:
    lines = [
        "<!-- Generated by tools/protocol_governance.py render. Do not edit. -->",
        "",
        "# Governance decision register",
        "",
        f"Charter `{document['charterVersion']}` · Status: **{document['charterStatus']}**. "
        "Process rules are in the [governance charter](README.md). "
        "The same data is published as searchable JSON at `/docs/governance/register.json`.",
        "",
        "A record marked *pre-charter* was made before the charter existed. It is kept for the public",
        "history but does not count as a completed charter process.",
        "",
        "## Records",
        "",
        "| Record | Process | Title | Stage | Outcome | Opened | Decided | Counts |",
        "| --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for record in document["records"]:
        timeline = record["timeline"]
        lines.append(
            f"| [{record['id']}](#{record['id'].lower()}) | {record['process']} | {_cell(record['title'])} | {record['stage']} "
            f"| {_cell(record.get('outcome'))} | {_cell(timeline.get('opened') or timeline.get('filed'))} "
            f"| {_cell(timeline.get('decided'))} | {'charter' if record['countsForCharter'] else 'pre-charter'} |"
        )
    for record in document["records"]:
        lines += ["", f"### {record['id']}", "", f"**{record['title']}**", "", record["summary"], ""]
        lines.append(f"- Deciding body: {_cell(record.get('decidingBody'))}")
        lines.append(f"- Authors: {_cell(', '.join(record.get('authors', [])))}")
        lines.append(f"- Disclosures: {_cell(', '.join(f'[{item}](#{item.lower()})' for item in record.get('disclosures', [])))}")
        if record.get("protectedRights"):
            lines.append(f"- Protected rights touched: {', '.join(record['protectedRights'])}")
        if record.get("threats"):
            lines.append(f"- Threats: {', '.join(record['threats'])}")
        lines.append("- Timeline: " + "; ".join(f"{key} {_cell(value)}" for key, value in record["timeline"].items()))
        votes = record.get("votes", [])
        lines.append("- Votes: " + (", ".join(f"{vote['participant']} ({vote['seat']}) {vote['vote']}" for vote in votes) if votes else "none recorded"))
        if record.get("dissent"):
            lines.append("- Dissent:")
            for entry in record["dissent"]:
                lines.append(f"  - {entry['participant']} ({entry['recorded']}): {entry['statement']}")
        else:
            lines.append("- Dissent: " + ("solicited; none recorded" if record.get("dissentSolicited") else "not solicited"))
        for link in record.get("links", []):
            href = link["href"]
            if not href.startswith("https://"):
                href = "https://app.forkmesh.com/forkmesh/forkmesh/blob/main/" + href
            lines.append(f"- [{link['label']}]({href})")
        if record.get("note"):
            lines += ["", f"> {record['note']}"]
    lines += ["", "## Seats", "", "| Body | Seat | Constituency | Holder | Term |", "| --- | --- | --- | --- | --- |"]
    for body in document["bodies"]:
        for seat in body["seats"]:
            term = f"{seat['since']} to {seat['termEnds']}" if seat["holder"] else "vacant"
            lines.append(f"| {body['name']} | {seat['id']} | {seat['constituency']} | {_cell(seat['holder'])} | {term} |")
    lines += ["", "## Disclosures"]
    for disclosure in document["disclosures"]:
        lines += ["", f"### {disclosure['id']}", "", f"Participant `{disclosure['participant']}`, filed {disclosure['filed']}.", ""]
        lines.append("Conflicts:" if disclosure["conflicts"] else "Conflicts: none declared.")
        lines += [f"- {item}" for item in disclosure["conflicts"]]
        lines += ["", "| Funding kind | Source | Amount |", "| --- | --- | --- |"]
        for entry in disclosure["funding"]:
            amount = entry.get("amount") if entry.get("amountPublished") else "not published"
            lines.append(f"| {entry['kind']} | {_cell(entry.get('source'))} | {_cell(amount)} |")
        linked = ", ".join(f"[{item}](#{item.lower()})" for item in disclosure["linkedRecords"])
        lines += ["", f"Linked records: {linked or 'none'}."]
        if disclosure.get("note"):
            lines += ["", f"> {disclosure['note']}"]
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
    commands.add_parser("validate", help="validate the charter, participants, disclosures, and records")
    render_command = commands.add_parser("render", help="regenerate the public decision register")
    render_command.add_argument("--check", action="store_true", help="fail when generated outputs are stale")
    commands.add_parser("gate", help="validate and check generated outputs; independent of today's date")
    for name, help_text in (("status", "print deadlines, disclosure freshness, and closure blockers"),
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
            print("governance ledger is valid")
            return 0
        if args.command in {"render", "gate"}:
            stale = render(ledger, check=args.command == "gate" or args.check)
            if stale and (args.command == "gate" or args.check):
                print("stale governance outputs: " + ", ".join(stale) + "; run python3 tools/protocol_governance.py render", file=sys.stderr)
                return 1
            print("governance register is current" if not stale else "rendered " + ", ".join(stale))
            return 0
        status = evaluate(ledger, args.as_of or date.today())
        print(json.dumps(status, indent=2))
        return 1 if args.command == "closure" and status["closureBlockers"] else 0
    except GovernanceError as exc:
        print(f"protocol-governance: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
