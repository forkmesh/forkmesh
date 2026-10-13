"""Privacy-safe policy helpers for community-governed contextual placements."""

from __future__ import annotations

import ipaddress
import re
from urllib.parse import urlparse


BODY_MAX_BYTES = 32 * 1024
MIN_VOTING_MS = 7 * 24 * 60 * 60 * 1000
MAX_VOTING_MS = 30 * 24 * 60 * 60 * 1000
MAX_REVIEW_MS = 180 * 24 * 60 * 60 * 1000
MIN_ACCOUNT_AGE_MS = 7 * 24 * 60 * 60 * 1000
DEFAULT_QUORUM = 5
MAX_QUORUM = 500

CONTEXTS = frozenset({
    "town-square",
    "repository",
    "organization",
    "workshop",
    "events",
})
CONTEXT_TAGS = frozenset({
    "accessibility",
    "developer-tools",
    "education",
    "events",
    "infrastructure",
    "open-source",
    "security",
})
DESTINATION_TYPES = frozenset({
    "project-operations",
    "instance-operations",
    "community-grants",
    "nonprofit",
})
CHOICES = frozenset({"approve", "reject", "abstain"})
MODERATION_ACTIONS = frozenset({
    "note",
    "suspend",
    "reinstate",
    "reject",
    "approve",
    "appeal-filed",
    "appeal-upheld",
    "appeal-denied",
})
PROHIBITED_CATEGORIES = frozenset({
    "behavioral-surveillance",
    "credential-sales",
    "exploit-sales",
    "financial-returns",
    "illegal-goods",
    "malware",
    "weapons",
})

ELIGIBILITY_CRITERIA = {
    "label": "Community-reviewed placement",
    "claim": (
        "Approval means the disclosed evidence met this instance's current "
        "criteria; it is not a universal claim that a company is ethical."
    ),
    "requiredEvidence": [
        "Sponsor identity and beneficial ownership disclosure",
        "Public product or service description",
        "At least two dated, independently accessible HTTPS evidence sources",
        "Data-use and tracking disclosure",
        "Labor, accessibility, environmental, and community-impact disclosures",
        "Conflicts of interest declared by proposers, voters, and moderators",
    ],
    "prohibited": sorted(PROHIBITED_CATEGORIES),
    "decision": {
        "oneVerifiedAccountOneVote": True,
        "minimumAccountAgeDays": 7,
        "walletBalanceWeight": False,
        "defaultQuorum": DEFAULT_QUORUM,
        "approvalThreshold": "at least two-thirds of non-abstaining votes",
        "maximumApprovalDays": 180,
    },
    "targeting": {
        "contextualOnly": True,
        "behavioralTracking": False,
        "sensitiveTargeting": False,
        "personalProfilesUsed": False,
    },
}

_ID_RE = re.compile(r"^[a-f0-9]{32}$")
_CURRENCY_RE = re.compile(r"^[A-Z]{3,8}$")
_NONPUBLIC_SUFFIXES = (
    ".internal",
    ".invalid",
    ".local",
    ".localhost",
    ".test",
    ".example",
    ".onion",
)


def valid_id(value):
    return bool(_ID_RE.fullmatch(str(value or "").strip().lower()))


def clean_text(value, limit):
    raw = str(value or "")
    clean = "".join(
        char if char.isprintable() and char not in "<>" else " "
        for char in raw
    )
    return " ".join(clean.split())[:limit].strip()


def public_https_url(value, max_length=1200):
    raw = str(value or "").strip()
    if not raw or len(raw) > max_length or "\\" in raw:
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (
        parsed.scheme.lower() != "https"
        or not host
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
        or port not in (None, 443)
        or "." not in host
        or any(host.endswith(suffix) for suffix in _NONPUBLIC_SUFFIXES)
    ):
        return ""
    try:
        ipaddress.ip_address(host)
        return ""
    except ValueError:
        pass
    return parsed._replace(
        scheme="https",
        netloc=host,
        fragment="",
    ).geturl()


def _string_list(value, allowed, maximum):
    if not isinstance(value, list) or not value or len(value) > maximum:
        return None
    normalized = []
    for item in value:
        clean = clean_text(item, 40).lower()
        if clean not in allowed or clean in normalized:
            return None
        normalized.append(clean)
    return sorted(normalized)


def normalize_proposal(data, now):
    if not isinstance(data, dict):
        return None, "invalid_json"
    allowed = {
        "title",
        "sponsor",
        "sponsorDisclosure",
        "copy",
        "destinationUrl",
        "contextTags",
        "evidence",
        "prohibitedCategories",
        "conflictDisclosure",
        "votingDays",
        "quorum",
    }
    if set(data) - allowed:
        return None, "unsupported_field"
    title = clean_text(data.get("title"), 120)
    sponsor = clean_text(data.get("sponsor"), 120)
    sponsor_disclosure = clean_text(data.get("sponsorDisclosure"), 1200)
    copy = clean_text(data.get("copy"), 240)
    destination = public_https_url(data.get("destinationUrl"))
    conflict_disclosure = clean_text(data.get("conflictDisclosure"), 800)
    tags = _string_list(data.get("contextTags"), CONTEXT_TAGS, 5)
    prohibited = data.get("prohibitedCategories", [])
    if not isinstance(prohibited, list) or any(
        clean_text(item, 60).lower() in PROHIBITED_CATEGORIES
        for item in prohibited
    ):
        return None, "prohibited_category"
    evidence = data.get("evidence")
    if not isinstance(evidence, list) or not 2 <= len(evidence) <= 12:
        return None, "evidence_required"
    normalized_evidence = []
    evidence_urls = set()
    for item in evidence:
        if not isinstance(item, dict) or set(item) - {
            "url", "title", "publisher", "observedAt", "supports",
        }:
            return None, "invalid_evidence"
        url = public_https_url(item.get("url"))
        title_value = clean_text(item.get("title"), 160)
        publisher = clean_text(item.get("publisher"), 120)
        supports = clean_text(item.get("supports"), 400)
        try:
            observed_at = int(item.get("observedAt") or 0)
        except (TypeError, ValueError):
            return None, "invalid_evidence"
        if (
            not url
            or url in evidence_urls
            or not title_value
            or not publisher
            or not supports
            or observed_at <= 0
            or observed_at > int(now)
        ):
            return None, "invalid_evidence"
        evidence_urls.add(url)
        normalized_evidence.append({
            "url": url,
            "title": title_value,
            "publisher": publisher,
            "observedAt": observed_at,
            "supports": supports,
        })
    try:
        voting_days = int(data.get("votingDays") or 14)
        quorum = int(data.get("quorum") or DEFAULT_QUORUM)
    except (TypeError, ValueError):
        return None, "invalid_vote_window"
    duration = voting_days * 24 * 60 * 60 * 1000
    if (
        not title
        or not sponsor
        or not sponsor_disclosure
        or not copy
        or not destination
        or tags is None
        or duration < MIN_VOTING_MS
        or duration > MAX_VOTING_MS
        or quorum < DEFAULT_QUORUM
        or quorum > MAX_QUORUM
    ):
        return None, "invalid_proposal"
    return {
        "title": title,
        "sponsor": sponsor,
        "sponsorDisclosure": sponsor_disclosure,
        "copy": copy,
        "destinationUrl": destination,
        "contextTags": tags,
        "evidence": normalized_evidence,
        "conflictDisclosure": conflict_disclosure,
        "quorum": quorum,
        "tracking": "none",
        "targeting": "context-only",
    }, ""


def normalize_instance_policy(data):
    if not isinstance(data, dict):
        return None, "invalid_json"
    if set(data) - {
        "enabled",
        "contexts",
        "revenueDestination",
        "revenueDestinationType",
    }:
        return None, "unsupported_field"
    enabled = data.get("enabled")
    contexts = _string_list(data.get("contexts"), CONTEXTS, len(CONTEXTS))
    destination = clean_text(data.get("revenueDestination"), 240)
    destination_type = clean_text(
        data.get("revenueDestinationType"), 40).lower()
    if (
        not isinstance(enabled, bool)
        or contexts is None
        or destination_type not in DESTINATION_TYPES
        or not destination
    ):
        return None, "invalid_instance_policy"
    return {
        "enabled": enabled,
        "contexts": contexts,
        "revenueDestination": destination,
        "revenueDestinationType": destination_type,
        "ledgerClass": "advertising-revenue",
    }, ""


def voter_eligible(account, now):
    if not isinstance(account, dict):
        return False, "registered_account_required"
    if account.get("status") != "active":
        return False, "active_account_required"
    if not bool(account.get("email_verified")):
        return False, "verified_email_required"
    try:
        created = int(account.get("created_at") or 0)
    except (TypeError, ValueError):
        created = 0
    if created <= 0 or int(now) - created < MIN_ACCOUNT_AGE_MS:
        return False, "minimum_account_age"
    return True, ""


def normalize_vote(data):
    if not isinstance(data, dict) or set(data) - {
        "choice", "conflictOfInterest", "conflictDisclosure",
    }:
        return None, "invalid_vote"
    choice = clean_text(data.get("choice"), 20).lower()
    conflict = data.get("conflictOfInterest")
    disclosure = clean_text(data.get("conflictDisclosure"), 800)
    if choice not in CHOICES or not isinstance(conflict, bool):
        return None, "invalid_vote"
    if conflict and (choice != "abstain" or not disclosure):
        return None, "conflicted_voter_must_abstain"
    return {
        "choice": choice,
        "conflictOfInterest": conflict,
        "conflictDisclosure": disclosure,
    }, ""


def tally(vote_rows, quorum):
    counts = {"approve": 0, "reject": 0, "abstain": 0}
    for row in vote_rows or []:
        choice = str(row.get("choice") or "")
        if choice in counts:
            counts[choice] += 1
    decisive = counts["approve"] + counts["reject"]
    approved = (
        decisive >= int(quorum)
        and counts["approve"] > counts["reject"]
        and counts["approve"] * 3 >= decisive * 2
    )
    return {
        **counts,
        "decisive": decisive,
        "quorum": int(quorum),
        "quorumMet": decisive >= int(quorum),
        "approved": approved,
    }


def normalize_moderation(data):
    if not isinstance(data, dict) or set(data) - {
        "action", "reason", "evidenceUrl",
    }:
        return None, "invalid_moderation"
    action = clean_text(data.get("action"), 40).lower()
    reason = clean_text(data.get("reason"), 1200)
    evidence_url = (
        public_https_url(data.get("evidenceUrl"))
        if data.get("evidenceUrl") else ""
    )
    if (
        action not in MODERATION_ACTIONS
        or not reason
        or (data.get("evidenceUrl") and not evidence_url)
    ):
        return None, "invalid_moderation"
    return {
        "action": action,
        "reason": reason,
        "evidenceUrl": evidence_url,
    }, ""


def currency(value):
    clean = clean_text(value, 8).upper()
    return clean if _CURRENCY_RE.fullmatch(clean) else ""
