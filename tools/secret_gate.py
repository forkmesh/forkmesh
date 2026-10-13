#!/usr/bin/env python3
"""One secret-scanning contract for every ForkMesh replication boundary.

A credential that reaches a ForkMesh node is copied to every mirror, clone,
fork, and backup that replicates the repository. Nothing in the network can
recall it. This module is the single scanner those boundaries share:

* ``hook pre-commit`` and ``hook pre-push`` run on the contributor's machine,
  before any remote receives an object.
* ``node_operator_policy.py pre-receive`` calls :func:`scan_git` on the objects
  Git has staged in its receive quarantine, so a blocked push is discarded
  before the node stores it.
* ``node_operator_policy.py enforce`` calls :func:`scan_git` on repositories
  that arrived by upstream sync or mirror intake, where detection is already
  after replication and :data:`RESPONSE_PLAYBOOK` applies.
* ``security_scan.py`` (the public report) uses :func:`scan_text`.
* The desktop client's ``RepoSecurity::secretPatterns()`` carries the same
  rule patterns; ``app/tests/test_secret_gate.py`` pins them together.

The rules, placeholder filter, and inline ignore marker are the contract.
Organization and repository policy decide, per rule, whether a finding is off,
a warning, or a block; add custom patterns; and allowlist test data by path.
A block can only be lifted by an SSH-signed override naming the exact finding
fingerprints. Findings never carry matched values, excerpts, or digests of
values: a fingerprint is derived from the rule, the path, and the Git blob id.

The module uses only the standard library so it can run as ``python3 -I``
beside the gateway tools.
"""

from __future__ import annotations

import argparse
import copy
import fnmatch
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import time
from typing import Any, Iterable, Mapping


CONTRACT_ID = "forkmesh-secret-scan-v1"
SCHEMA_VERSION = 1
IGNORE_MARKER = "forkmesh-secret-scan:ignore-line"
OVERRIDE_TYPE = "forkmesh-secret-override"
OVERRIDE_NAMESPACE = "forkmesh-secret-override"
REPOSITORY_POLICY_PATH = ".forkmesh/secret-scan.json"
REPOSITORY_OVERRIDE_DIR = ".forkmesh/secret-overrides"
HOOK_MARKER = "# Managed-By: ForkMesh secret gate"

MAX_SCAN_BYTES = 1024 * 1024
MAX_POLICY_BYTES = 128 * 1024
MAX_OVERRIDE_BYTES = 32 * 1024
MAX_OVERRIDE_FILES = 200
MAX_CUSTOM_RULES = 100
MAX_ALLOWLIST_ENTRIES = 200
MAX_PATTERN_CHARS = 500
MAX_OVERRIDE_FINGERPRINTS = 500
DEFAULT_OVERRIDE_LIFETIME_DAYS = 90
GIT_TIMEOUT_SECONDS = 600

ACTIONS = ("off", "warn", "block")
ACTION_RANK = {name: index for index, name in enumerate(ACTIONS)}
SEVERITIES = ("critical", "high", "medium", "low")
OUTCOMES = ("block", "warn", "overridden", "allowlisted")

CUSTOM_RULE_ID_RE = re.compile(r"^custom-[a-z0-9][a-z0-9-]{0,59}$")
FINGERPRINT_RE = re.compile(r"^[0-9a-f]{32}$")
OBJECT_ID_RE = re.compile(r"^(?:[0-9a-f]{40}|[0-9a-f]{64})$")
ZERO_OID_RE = re.compile(r"^0+$")
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9._-]{1,100}/[A-Za-z0-9._-]{1,100}$")
SIGNER_RE = re.compile(r"^[A-Za-z0-9._@+-]{1,200}$")

PERMANENCE_WARNING = (
    "A pushed object is copied to every node, mirror, fork, clone, and backup "
    "that replicates this repository. ForkMesh cannot recall or erase it "
    "afterwards; treat any real credential that leaves this machine as "
    "compromised and rotate it."
)


class GateError(ValueError):
    """A policy, override, or command argument is invalid."""


# --------------------------------------------------------------------------
# The contract


def _rule(rule_id: str, title: str, severity: str, pattern: str,
          rotation: str, *, value_group: int = 0,
          placeholder_filter: bool = False) -> dict[str, Any]:
    return {
        "id": rule_id,
        "title": title,
        "severity": severity,
        "pattern": pattern,
        "valueGroup": value_group,
        "placeholderFilter": placeholder_filter,
        "summary": "A value shaped like a %s may be present." % title,
        "recommendation": (
            "Rotate the credential at its issuer, remove it from the "
            "repository, and load it from a secret store instead."),
        "rotation": rotation,
    }


# Patterns are byte-for-byte the ones in desktop/src/RepoSecurity.cpp
# secretPatterns(); "title" is the desktop finding title.
RULES: tuple[dict[str, Any], ...] = (
    _rule("github-token", "GitHub token", "critical",
          r"\bgh[pousr]_[A-Za-z0-9_]{36,}\b",
          "Revoke it under GitHub Settings > Developer settings (or the "
          "installation/app that issued it) and review the security log."),
    _rule("github-fine-grained-token", "GitHub fine-grained PAT", "critical",
          r"\bgithub_pat_[A-Za-z0-9_]{22,}\b",
          "Delete the fine-grained token in GitHub Settings > Developer "
          "settings and review the repositories it could reach."),
    _rule("aws-access-key", "AWS access key", "critical",
          r"\bAKIA[0-9A-Z]{16,}\b",
          "Deactivate and delete the access key in IAM, then check "
          "CloudTrail for calls made with it."),
    _rule("aws-temporary-access-key", "AWS temporary access key", "high",
          r"\bASIA[0-9A-Z]{16,}\b",
          "Revoke the role session (IAM > Roles > Revoke active sessions) "
          "and check CloudTrail."),
    _rule("slack-token", "Slack token", "critical",
          r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b",
          "Revoke the token from the Slack app's OAuth settings and rotate "
          "the app's signing secret if it was nearby."),
    _rule("openai-api-key", "OpenAI API key", "critical",
          r"\bsk-proj-[A-Za-z0-9_-]{20,}\b|\bsk-[A-Za-z0-9]{48,}\b",
          "Delete the key in the OpenAI dashboard and review usage."),
    _rule("anthropic-api-key", "Anthropic API key", "critical",
          r"\bsk-ant-[A-Za-z0-9_-]{20,}\b",
          "Delete the key in the Anthropic Console and review usage."),
    _rule("stripe-secret-key", "Stripe secret key", "critical",
          r"\bsk_(live|test)_[A-Za-z0-9]{24,}\b",
          "Roll the key in the Stripe Dashboard (Developers > API keys)."),
    _rule("stripe-restricted-key", "Stripe restricted key", "critical",
          r"\brk_(live|test)_[A-Za-z0-9]{24,}\b",
          "Roll the restricted key in the Stripe Dashboard."),
    _rule("google-api-key", "Google API key", "high",
          r"\bAIza[A-Za-z0-9_-]{35,}\b",
          "Delete or regenerate the key in Google Cloud Console > "
          "Credentials and add API restrictions to its replacement."),
    _rule("google-oauth-token", "Google OAuth token", "high",
          r"\bya29\.[A-Za-z0-9_-]{20,}\b",
          "Revoke the grant (https://myaccount.google.com/permissions or the "
          "token revocation endpoint)."),
    _rule("google-oauth-client-secret", "Google OAuth client secret",
          "critical", r"\bGOCSPX-[A-Za-z0-9_-]{28,}\b",
          "Reset the client secret in Google Cloud Console > Credentials."),
    _rule("sendgrid-api-key", "SendGrid API key", "critical",
          r"\bSG\.[A-Za-z0-9_-]{22,}\.[A-Za-z0-9_-]{43,}\b",
          "Delete the key in SendGrid Settings > API Keys."),
    _rule("twilio-api-key", "Twilio auth token", "critical",
          r"\bSK[a-f0-9]{32,}\b",
          "Delete the API key in the Twilio Console and rotate the auth "
          "token if it was exposed with it."),
    _rule("npm-access-token", "npm access token", "critical",
          r"\bnpm_[A-Za-z0-9]{36,}\b",
          "Revoke the token with `npm token revoke` and check published "
          "package versions."),
    _rule("vault-token", "HashiCorp Vault token", "critical",
          r"\bhv[sbr]\.[A-Za-z0-9]{24,}\b",
          "Revoke it with `vault token revoke` and audit its lease tree."),
    _rule("cloudflare-api-token", "Cloudflare API token", "critical",
          r"(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)"
          r'''\s*=\s*['"]?([A-Za-z0-9_-]{40,})['"]?''',
          "Roll the token in Cloudflare My Profile > API Tokens and review "
          "the account audit log.", value_group=1),
    _rule("private-key-material", "PEM private key", "critical",
          r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY"
          r"(?:-----| BLOCK-----)",
          "Replace the key pair, remove the public half from every "
          "authorized_keys file, certificate, or keyring that trusts it."),
    _rule("generic-secret-assignment", "Secret/token assignment", "high",
          r"(?i)(?:password|passwd|api[_\-]?(?:key|secret|token)|"
          r"auth[_\-]?token|secret[_\-]?key|access[_\-]?token|"
          r'''private[_\-]?key)\s*[:=]\s*['"]([^'"\s]{20,})['"]''',
          "Change the password or token wherever it is accepted.",
          value_group=1, placeholder_filter=True),
)
RULE_IDS = tuple(rule["id"] for rule in RULES)

# A value that advertises itself as a placeholder is not worth blocking over.
# Only rules with placeholderFilter consult this list: provider-prefixed
# tokens are specific enough that "example" inside one is still a credential.
PLACEHOLDER_MARKERS: tuple[str, ...] = (
    "correct-horse-battery-staple", "changeme", "change-me", "change_me",
    "placeholder", "example", "sample", "dummy", "redacted", "notreal",
    "not-a-real", "not_a_real", "fake", "your-", "your_", "test-password",
    "test_password", "testpassword", "${", "{{", "<",
)
_INDIRECTION_RE = re.compile(r"^(?:\$\w+|\$\{[^}]*\}|\$\([^)]*\)|%\w+%)$")


def contract() -> dict[str, Any]:
    """The public, value-free description every boundary implements."""
    return {
        "id": CONTRACT_ID,
        "schemaVersion": SCHEMA_VERSION,
        "ignoreMarker": IGNORE_MARKER,
        "maxScanBytes": MAX_SCAN_BYTES,
        "repositoryPolicyPath": REPOSITORY_POLICY_PATH,
        "repositoryOverrideDirectory": REPOSITORY_OVERRIDE_DIR,
        "overrideNamespace": OVERRIDE_NAMESPACE,
        "placeholderMarkers": list(PLACEHOLDER_MARKERS),
        "permanenceWarning": PERMANENCE_WARNING,
        "rules": [copy.deepcopy(rule) for rule in RULES],
    }


def looks_like_placeholder(value: str) -> bool:
    lower = value.lower()
    if any(marker in lower for marker in PLACEHOLDER_MARKERS):
        return True
    if _INDIRECTION_RE.match(value):
        return True
    return bool(lower) and all(character == lower[0] for character in lower)


# --------------------------------------------------------------------------
# Matching


def _continues(line: str) -> bool:
    trailing = len(line) - len(line.rstrip("\\"))
    return trailing % 2 == 1


def _logical_line_has_marker(lines: list[str], index: int) -> bool:
    """A trailing backslash joins lines; a marker anywhere in the run counts."""
    first = index
    while first > 0 and _continues(lines[first - 1]):
        first -= 1
    last = index
    while last + 1 < len(lines) and _continues(lines[last]):
        last += 1
    return any(IGNORE_MARKER in lines[i] for i in range(first, last + 1))


def compile_rules(rules: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    compiled = []
    for rule in rules:
        item = dict(rule)
        item["regex"] = re.compile(str(rule["pattern"]))
        compiled.append(item)
    return compiled


_CONTRACT_COMPILED: list[dict[str, Any]] | None = None


def contract_rules() -> list[dict[str, Any]]:
    global _CONTRACT_COMPILED
    if _CONTRACT_COMPILED is None:
        _CONTRACT_COMPILED = compile_rules(RULES)
    return _CONTRACT_COMPILED


def scan_text(
    text: str,
    *,
    rules: list[dict[str, Any]] | None = None,
    honor_markers: bool = True,
    lines_of_interest: set[int] | None = None,
) -> list[tuple[dict[str, Any], int]]:
    """Return ``(rule, 1-based line)`` for every match, one per rule and line.

    ``lines_of_interest`` limits results to matches that start on those lines
    (the added lines of a diff). Matched values are inspected only to apply
    the placeholder filter and are never returned.
    """
    rules = contract_rules() if rules is None else rules
    lines = text.split("\n")
    starts: list[int] = []
    offset = 0
    for line in lines:
        starts.append(offset)
        offset += len(line) + 1

    def line_index(position: int) -> int:
        low, high = 0, len(starts)
        while low < high:
            middle = (low + high) // 2
            if starts[middle] <= position:
                low = middle + 1
            else:
                high = middle
        return low - 1

    results: list[tuple[dict[str, Any], int]] = []
    seen: set[tuple[str, int]] = set()
    for rule in rules:
        for match in rule["regex"].finditer(text):
            index = line_index(match.start())
            if lines_of_interest is not None and index + 1 not in lines_of_interest:
                continue
            if honor_markers and _logical_line_has_marker(lines, index):
                continue
            if rule.get("placeholderFilter"):
                group = int(rule.get("valueGroup") or 0)
                value = match.group(group) if group else match.group(0)
                if looks_like_placeholder(value or ""):
                    continue
            key = (rule["id"], index + 1)
            if key in seen:
                continue
            seen.add(key)
            results.append((rule, index + 1))
    return results


def blob_oid(data: bytes, object_format: str = "sha1") -> str:
    header = b"blob %d\0" % len(data)
    digest = hashlib.sha256 if object_format == "sha256" else hashlib.sha1
    return digest(header + data).hexdigest()


def fingerprint(rule_id: str, path: str, blob: str) -> str:
    """Stable finding id; never derived from the matched value."""
    material = "forkmesh-secret-finding-v1\0%s\0%s\0%s" % (rule_id, path, blob)
    return hashlib.sha256(material.encode("utf-8", "surrogateescape")).hexdigest()[:32]


def _looks_binary(data: bytes) -> bool:
    return b"\0" in data[:8192]


# --------------------------------------------------------------------------
# Policy


def _expect_keys(value: Any, allowed: Iterable[str], label: str) -> dict:
    if not isinstance(value, dict):
        raise GateError(label + " must be an object")
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise GateError("%s has unknown fields: %s" % (label, ", ".join(unknown)))
    return value


def _action(value: Any, label: str) -> str:
    if value not in ACTIONS:
        raise GateError(label + " must be one of: " + ", ".join(ACTIONS))
    return value


def _string(value: Any, label: str, minimum: int = 1, maximum: int = 500) -> str:
    if not isinstance(value, str) or not minimum <= len(value.strip()) <= maximum:
        raise GateError("%s must be a string of %d-%d characters"
                        % (label, minimum, maximum))
    if any(character < " " for character in value):
        raise GateError(label + " must not contain control characters")
    return value.strip()


def _bool(value: Any, label: str) -> bool:
    if not isinstance(value, bool):
        raise GateError(label + " must be a boolean")
    return value


ORGANIZATION_FIELDS = (
    "schemaVersion", "mode", "rules", "customRules", "allowlist",
    "honorIgnoreMarkers", "allowRepositoryAllowlist",
    "allowRepositoryCustomRules", "overrides",
)
REPOSITORY_FIELDS = ("schemaVersion", "mode", "rules", "customRules", "allowlist")


def normalize_policy(raw: Mapping[str, Any] | None, *,
                     repository: bool = False) -> dict[str, Any]:
    """Validate an organization (or, with ``repository``, a repository) policy.

    An organization policy defaults to blocking every contract rule. A
    repository policy defaults to ``off`` because it can only tighten what the
    organization decided; see :func:`effective_policy`.
    """
    raw = {} if raw is None else raw
    label = "repository secret policy" if repository else "secret policy"
    _expect_keys(raw, REPOSITORY_FIELDS if repository else ORGANIZATION_FIELDS,
                 label)
    if raw.get("schemaVersion", SCHEMA_VERSION) != SCHEMA_VERSION:
        raise GateError(label + " schemaVersion must be 1")
    policy: dict[str, Any] = {
        "schemaVersion": SCHEMA_VERSION,
        "mode": _action(raw.get("mode", "off" if repository else "block"),
                        label + ".mode"),
    }
    rules = _expect_keys(raw.get("rules", {}), RULE_IDS, label + ".rules")
    policy["rules"] = {
        rule_id: _action(action, "%s.rules.%s" % (label, rule_id))
        for rule_id, action in sorted(rules.items())
    }

    custom = raw.get("customRules", [])
    if not isinstance(custom, list) or len(custom) > MAX_CUSTOM_RULES:
        raise GateError("%s.customRules must be a list of at most %d rules"
                        % (label, MAX_CUSTOM_RULES))
    seen_ids: set[str] = set()
    policy["customRules"] = []
    for index, item in enumerate(custom):
        item_label = "%s.customRules[%d]" % (label, index)
        _expect_keys(item, ("id", "title", "severity", "pattern", "action",
                            "rotation"), item_label)
        rule_id = item.get("id")
        if not isinstance(rule_id, str) or not CUSTOM_RULE_ID_RE.fullmatch(rule_id):
            raise GateError(item_label + ".id must look like custom-<name>")
        if rule_id in seen_ids:
            raise GateError(item_label + ".id is duplicated")
        seen_ids.add(rule_id)
        pattern = _string(item.get("pattern"), item_label + ".pattern",
                          maximum=MAX_PATTERN_CHARS)
        try:
            compiled = re.compile(pattern)
        except re.error as exc:
            raise GateError("%s.pattern does not compile: %s" % (item_label, exc))
        if compiled.match(""):
            raise GateError(item_label + ".pattern must not match empty text")
        severity = item.get("severity", "high")
        if severity not in SEVERITIES:
            raise GateError(item_label + ".severity must be one of: "
                            + ", ".join(SEVERITIES))
        policy["customRules"].append({
            "id": rule_id,
            "title": _string(item.get("title", rule_id), item_label + ".title",
                             maximum=120),
            "severity": severity,
            "pattern": pattern,
            "valueGroup": 0,
            "placeholderFilter": False,
            "summary": "A value matching the %s rule may be present." % rule_id,
            "recommendation": "Follow your organization's rotation procedure.",
            "rotation": _string(
                item.get("rotation", "Follow your organization's rotation "
                         "procedure for this credential."),
                item_label + ".rotation"),
            "action": _action(item.get("action", "block"), item_label + ".action"),
        })

    allowlist = raw.get("allowlist", [])
    if not isinstance(allowlist, list) or len(allowlist) > MAX_ALLOWLIST_ENTRIES:
        raise GateError("%s.allowlist must be a list of at most %d entries"
                        % (label, MAX_ALLOWLIST_ENTRIES))
    known = set(RULE_IDS) | seen_ids
    policy["allowlist"] = []
    for index, item in enumerate(allowlist):
        item_label = "%s.allowlist[%d]" % (label, index)
        _expect_keys(item, ("paths", "rules", "reason"), item_label)
        paths = item.get("paths")
        if (not isinstance(paths, list) or not paths or len(paths) > 50
                or not all(isinstance(p, str) and p.strip() for p in paths)):
            raise GateError(item_label + ".paths must be 1-50 path globs")
        if any(p.startswith("/") or ".." in p.split("/") for p in paths):
            raise GateError(item_label + ".paths must be repository-relative")
        if any(p.strip() in ("*", "**", "**/*") for p in paths):
            raise GateError(item_label + ".paths must not allowlist every path")
        rule_ids = item.get("rules", ["*"])
        if (not isinstance(rule_ids, list) or not rule_ids
                or not all(isinstance(r, str) for r in rule_ids)):
            raise GateError(item_label + ".rules must be a list of rule ids")
        unknown = [r for r in rule_ids if r != "*" and r not in known
                   and not CUSTOM_RULE_ID_RE.fullmatch(r)]
        if unknown:
            raise GateError(item_label + ".rules names unknown rules: "
                            + ", ".join(unknown))
        policy["allowlist"].append({
            "paths": sorted({p.strip() for p in paths}),
            "rules": sorted(set(rule_ids)),
            "reason": _string(item.get("reason"), item_label + ".reason",
                              minimum=8),
        })

    if not repository:
        policy["honorIgnoreMarkers"] = _bool(
            raw.get("honorIgnoreMarkers", True), label + ".honorIgnoreMarkers")
        policy["allowRepositoryAllowlist"] = _bool(
            raw.get("allowRepositoryAllowlist", False),
            label + ".allowRepositoryAllowlist")
        policy["allowRepositoryCustomRules"] = _bool(
            raw.get("allowRepositoryCustomRules", False),
            label + ".allowRepositoryCustomRules")
        overrides = _expect_keys(raw.get("overrides", {}),
                                 ("allowedSignersPath", "maxLifetimeDays"),
                                 label + ".overrides")
        signers = overrides.get("allowedSignersPath", "")
        if (not isinstance(signers, str) or "\0" in signers
                or (signers and not Path(signers).is_absolute())):
            raise GateError(label + ".overrides.allowedSignersPath must be an "
                            "absolute path")
        lifetime = overrides.get("maxLifetimeDays", DEFAULT_OVERRIDE_LIFETIME_DAYS)
        if (not isinstance(lifetime, int) or isinstance(lifetime, bool)
                or not 1 <= lifetime <= 366):
            raise GateError(label + ".overrides.maxLifetimeDays must be 1-366")
        policy["overrides"] = {"allowedSignersPath": signers,
                               "maxLifetimeDays": lifetime}
    return policy


def load_policy_file(path: Path | None, *, repository: bool = False) -> dict[str, Any]:
    if path is None:
        return normalize_policy(None, repository=repository)
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise GateError("cannot read secret policy %s: %s" % (path, exc))
    return parse_policy_bytes(raw, repository=repository)


def parse_policy_bytes(raw: bytes, *, repository: bool = False) -> dict[str, Any]:
    if len(raw) > MAX_POLICY_BYTES:
        raise GateError("secret policy is larger than %d bytes" % MAX_POLICY_BYTES)
    try:
        document = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise GateError("secret policy is not valid JSON: %s" % exc)
    return normalize_policy(document, repository=repository)


def effective_policy(
    organization: Mapping[str, Any],
    repositories: Iterable[Mapping[str, Any]] = (),
) -> dict[str, Any]:
    """Merge repository policies under an organization policy.

    A repository policy can raise a rule from off to warn or warn to block,
    and never lower one. Its allowlist and custom patterns count only when
    the organization opted in, because whoever pushes controls those files.
    """
    effective = {
        "mode": organization["mode"],
        "ruleActions": {},
        "customRules": [dict(rule) for rule in organization["customRules"]],
        "allowlist": [dict(item, source="organization")
                      for item in organization["allowlist"]],
        "honorIgnoreMarkers": organization["honorIgnoreMarkers"],
        "overrides": dict(organization["overrides"]),
        "ignoredRepositoryAllowlist": 0,
        "ignoredRepositoryCustomRules": 0,
    }
    repositories = list(repositories)
    for rule_id in RULE_IDS:
        action = organization["rules"].get(rule_id, organization["mode"])
        for repo in repositories:
            candidate = repo["rules"].get(rule_id, repo["mode"])
            if ACTION_RANK[candidate] > ACTION_RANK[action]:
                action = candidate
        effective["ruleActions"][rule_id] = action
    custom_ids = {rule["id"] for rule in effective["customRules"]}
    for repo in repositories:
        if ACTION_RANK[repo["mode"]] > ACTION_RANK[effective["mode"]]:
            effective["mode"] = repo["mode"]
        if organization["allowRepositoryAllowlist"]:
            effective["allowlist"].extend(
                dict(item, source="repository") for item in repo["allowlist"])
        else:
            effective["ignoredRepositoryAllowlist"] += len(repo["allowlist"])
        for rule in repo["customRules"]:
            if not organization["allowRepositoryCustomRules"]:
                effective["ignoredRepositoryCustomRules"] += 1
            elif rule["id"] not in custom_ids:
                custom_ids.add(rule["id"])
                effective["customRules"].append(dict(rule))
    return effective


def _allowlisted(effective: Mapping[str, Any], rule_id: str, path: str) -> str:
    for item in effective["allowlist"]:
        if "*" not in item["rules"] and rule_id not in item["rules"]:
            continue
        if any(fnmatch.fnmatchcase(path, pattern) for pattern in item["paths"]):
            return item["source"]
    return ""


def _rules_for(effective: Mapping[str, Any]) -> list[dict[str, Any]]:
    rules = [rule for rule in contract_rules()
             if effective["ruleActions"][rule["id"]] != "off"]
    custom = [rule for rule in effective["customRules"] if rule["action"] != "off"]
    return rules + compile_rules(custom)


def _rule_action(effective: Mapping[str, Any], rule: Mapping[str, Any]) -> str:
    if rule["id"] in effective["ruleActions"]:
        return effective["ruleActions"][rule["id"]]
    return rule.get("action", "block")


# --------------------------------------------------------------------------
# Overrides


def _canonical(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


def _parse_time(value: Any, label: str) -> float:
    if not isinstance(value, str) or not re.fullmatch(
            r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", value):
        raise GateError(label + " must be an ISO-8601 UTC time like "
                        "2026-01-31T12:00:00Z")
    import calendar

    return float(calendar.timegm(time.strptime(value, "%Y-%m-%dT%H:%M:%SZ")))


def _iso(epoch: float) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(epoch))


OVERRIDE_FIELDS = ("schemaVersion", "type", "repository", "fingerprints",
                   "reason", "signer", "issuedAt", "expiresAt", "signature")


def override_payload(document: Mapping[str, Any]) -> bytes:
    """The exact bytes an override signature covers."""
    return _canonical({key: value for key, value in document.items()
                       if key != "signature"})


def _override_fields(document: Any) -> dict[str, Any]:
    _expect_keys(document, OVERRIDE_FIELDS, "override")
    if document.get("schemaVersion") != SCHEMA_VERSION:
        raise GateError("override schemaVersion must be 1")
    if document.get("type") != OVERRIDE_TYPE:
        raise GateError("override type must be " + OVERRIDE_TYPE)
    repository = document.get("repository")
    if not isinstance(repository, str) or not REPOSITORY_RE.fullmatch(repository):
        raise GateError("override repository must be owner/name")
    fingerprints = document.get("fingerprints")
    if (not isinstance(fingerprints, list) or not fingerprints
            or len(fingerprints) > MAX_OVERRIDE_FINGERPRINTS
            or not all(isinstance(f, str) and FINGERPRINT_RE.fullmatch(f)
                       for f in fingerprints)):
        raise GateError("override fingerprints must be 1-%d finding "
                        "fingerprints" % MAX_OVERRIDE_FINGERPRINTS)
    signer = document.get("signer")
    if not isinstance(signer, str) or not SIGNER_RE.fullmatch(signer):
        raise GateError("override signer must be an allowed-signers principal")
    _string(document.get("reason"), "override reason", minimum=10)
    signature = document.get("signature", "")
    if not isinstance(signature, str):
        raise GateError("override signature must be a string")
    return dict(document)


def sign_override(
    *, key: Path, signer: str, repository: str, fingerprints: list[str],
    reason: str, days: int, now: float | None = None,
) -> dict[str, Any]:
    now = time.time() if now is None else now
    document = {
        "schemaVersion": SCHEMA_VERSION,
        "type": OVERRIDE_TYPE,
        "repository": repository,
        "fingerprints": sorted(set(fingerprints)),
        "reason": reason,
        "signer": signer,
        "issuedAt": _iso(now),
        "expiresAt": _iso(now + days * 86400),
    }
    _override_fields(dict(document, signature=""))
    completed = subprocess.run(
        ["ssh-keygen", "-q", "-Y", "sign", "-f", str(key), "-n",
         OVERRIDE_NAMESPACE],
        input=override_payload(document), capture_output=True,
        timeout=60, check=False)
    if completed.returncode != 0 or b"BEGIN SSH SIGNATURE" not in completed.stdout:
        raise GateError("ssh-keygen could not sign the override: "
                        + completed.stderr.decode("utf-8", "replace").strip())
    document["signature"] = completed.stdout.decode("ascii")
    return document


def verify_override(
    document: Any, *, repository: str, allowed_signers: str,
    max_lifetime_days: int = DEFAULT_OVERRIDE_LIFETIME_DAYS,
    now: float | None = None,
) -> dict[str, Any]:
    """Return the override if its signature and scope are valid; else raise."""
    now = time.time() if now is None else now
    item = _override_fields(document)
    if not allowed_signers:
        raise GateError("this policy accepts no overrides "
                        "(overrides.allowedSignersPath is not set)")
    if not repository or item["repository"].lower() != repository.lower():
        raise GateError("override names a different repository")
    issued = _parse_time(item.get("issuedAt"), "override issuedAt")
    expires = _parse_time(item.get("expiresAt"), "override expiresAt")
    if expires <= issued or expires - issued > max_lifetime_days * 86400:
        raise GateError("override lifetime must be positive and at most %d days"
                        % max_lifetime_days)
    if issued > now + 300:
        raise GateError("override is issued in the future")
    if now >= expires:
        raise GateError("override expired at " + item["expiresAt"])
    if "BEGIN SSH SIGNATURE" not in item["signature"]:
        raise GateError("override is not signed")
    with tempfile.TemporaryDirectory(prefix="forkmesh-override-") as scratch:
        signature_path = Path(scratch) / "override.sig"
        signature_path.write_text(item["signature"], encoding="ascii")
        completed = subprocess.run(
            ["ssh-keygen", "-Y", "verify", "-f", allowed_signers,
             "-I", item["signer"], "-n", OVERRIDE_NAMESPACE,
             "-s", str(signature_path)],
            input=override_payload(item), capture_output=True, timeout=60,
            check=False)
    if completed.returncode != 0:
        raise GateError("override signature does not verify for %s"
                        % item["signer"])
    return item


def resolve_overrides(
    documents: Iterable[tuple[str, bytes]],
    effective: Mapping[str, Any],
    repository: str,
    now: float | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, str]]]:
    valid: list[dict[str, Any]] = []
    invalid: list[dict[str, str]] = []
    for source, raw in documents:
        try:
            if len(raw) > MAX_OVERRIDE_BYTES:
                raise GateError("override is larger than %d bytes"
                                % MAX_OVERRIDE_BYTES)
            try:
                document = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise GateError("override is not valid JSON")
            valid.append(verify_override(
                document, repository=repository,
                allowed_signers=effective["overrides"]["allowedSignersPath"],
                max_lifetime_days=effective["overrides"]["maxLifetimeDays"],
                now=now))
        except (GateError, OSError, subprocess.SubprocessError) as exc:
            invalid.append({"source": source, "reason": str(exc)})
    return valid, invalid


# --------------------------------------------------------------------------
# Reports


def new_report(boundary: str) -> dict[str, Any]:
    return {
        "contract": CONTRACT_ID,
        "boundary": boundary,
        "scannedFiles": 0,
        "skippedFiles": 0,
        "findings": [],
        "counts": {outcome: 0 for outcome in OUTCOMES},
        "blocked": False,
        "overrides": [],
        "invalidOverrides": [],
        "ignoredRepositoryAllowlist": 0,
        "ignoredRepositoryCustomRules": 0,
    }


def _add_findings(
    report: dict[str, Any],
    effective: Mapping[str, Any],
    overridden: Mapping[str, Mapping[str, Any]],
    *,
    path: str,
    blob: str,
    text: str,
    rules: list[dict[str, Any]],
    lines_of_interest: set[int] | None = None,
) -> None:
    report["scannedFiles"] += 1
    for rule, line in scan_text(
            text, rules=rules, honor_markers=effective["honorIgnoreMarkers"],
            lines_of_interest=lines_of_interest):
        action = _rule_action(effective, rule)
        if action == "off":
            continue
        finding_id = fingerprint(rule["id"], path, blob)
        allow_source = _allowlisted(effective, rule["id"], path)
        if allow_source:
            outcome = "allowlisted"
        elif action == "block" and finding_id in overridden:
            outcome = "overridden"
        else:
            outcome = action
        finding = {
            "rule": rule["id"],
            "title": rule["title"],
            "severity": rule["severity"],
            "path": path,
            "line": line,
            "blob": blob,
            "fingerprint": finding_id,
            "outcome": outcome,
        }
        if allow_source:
            finding["allowlist"] = allow_source
        if outcome == "overridden":
            finding["overrideSigner"] = overridden[finding_id]["signer"]
        report["findings"].append(finding)
        report["counts"][outcome] += 1
    report["blocked"] = report["counts"]["block"] > 0


def _prepare(
    boundary: str,
    organization: Mapping[str, Any],
    repository_policies: Iterable[Mapping[str, Any]],
    override_documents: Iterable[tuple[str, bytes]],
    repository: str,
    now: float | None,
) -> tuple[dict[str, Any], dict[str, Any], dict[str, dict[str, Any]], list]:
    effective = effective_policy(organization, repository_policies)
    report = new_report(boundary)
    report["ignoredRepositoryAllowlist"] = effective["ignoredRepositoryAllowlist"]
    report["ignoredRepositoryCustomRules"] = effective["ignoredRepositoryCustomRules"]
    documents = list(override_documents)
    overridden: dict[str, dict[str, Any]] = {}
    if documents:
        valid, invalid = resolve_overrides(documents, effective, repository, now)
        report["invalidOverrides"] = invalid
        for item in valid:
            report["overrides"].append({
                "signer": item["signer"], "reason": item["reason"],
                "expiresAt": item["expiresAt"],
                "fingerprints": len(item["fingerprints"]),
            })
            for finding_id in item["fingerprints"]:
                overridden.setdefault(finding_id, item)
    return report, effective, overridden, _rules_for(effective)


def scan_files(
    files: Iterable[tuple[str, bytes]],
    *,
    boundary: str = "publication",
    organization: Mapping[str, Any] | None = None,
    repository_policies: Iterable[Mapping[str, Any]] = (),
    override_documents: Iterable[tuple[str, bytes]] = (),
    repository: str = "",
    now: float | None = None,
) -> dict[str, Any]:
    """Scan whole files, e.g. a tree about to be published or uploaded."""
    organization = organization or normalize_policy(None)
    report, effective, overridden, rules = _prepare(
        boundary, organization, repository_policies, override_documents,
        repository, now)
    for path, data in files:
        if len(data) > MAX_SCAN_BYTES or _looks_binary(data):
            report["skippedFiles"] += 1
            continue
        _add_findings(report, effective, overridden, path=path,
                      blob=blob_oid(data),
                      text=data.decode("utf-8", "replace"), rules=rules)
    return report


def parse_unified_diff(diff: str) -> dict[str, dict[int, str]]:
    """Added lines per new path, keyed by their line number in the new file."""
    files: dict[str, dict[int, str]] = {}
    current: dict[int, str] | None = None
    new_line = 0
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
    for raw in diff.split("\n"):
        if raw.startswith("+++ "):
            target = raw[4:].strip()
            if target == "/dev/null":
                current = None
                continue
            if target.startswith("b/"):
                target = target[2:]
            current = files.setdefault(target, {})
            continue
        if raw.startswith("--- ") or raw.startswith("diff --git "):
            continue
        match = hunk.match(raw)
        if match:
            new_line = int(match.group(1))
            continue
        if current is None or not new_line:
            continue
        if raw.startswith("+"):
            current[new_line] = raw[1:]
            new_line += 1
        elif raw.startswith(" "):
            new_line += 1
        elif raw.startswith("\\"):
            continue
    return files


def scan_diff(
    diff: str,
    *,
    organization: Mapping[str, Any] | None = None,
    repository_policies: Iterable[Mapping[str, Any]] = (),
    now: float | None = None,
) -> dict[str, Any]:
    """Scan only the added lines of a unified diff (review surfaces)."""
    organization = organization or normalize_policy(None)
    report, effective, overridden, rules = _prepare(
        "review-diff", organization, repository_policies, (), "", now)
    for path, added in parse_unified_diff(diff).items():
        if not added:
            continue
        lines = [""] * (max(added) + 1)
        for number, text in added.items():
            lines[number - 1] = text
        _add_findings(report, effective, overridden, path=path, blob="",
                      text="\n".join(lines), rules=rules,
                      lines_of_interest=set(added))
    return report


# --------------------------------------------------------------------------
# Git boundaries


def _git_environment() -> dict[str, str]:
    environment = {
        "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
        "LANG": "C",
        "LC_ALL": "C",
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_OPTIONAL_LOCKS": "0",
    }
    # Inside pre-receive Git points these at its object quarantine; the new
    # objects are only visible through them.
    for key in ("GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
                "GIT_QUARANTINE_PATH"):
        if os.environ.get(key):
            environment[key] = os.environ[key]
    return environment


def _git(git_dir: Path, arguments: list[str], input_bytes: bytes = b"",
         check: bool = True) -> bytes:
    completed = subprocess.run(
        ["git", "-c", "core.fsmonitor=", "-c", "safe.directory=*",
         "--git-dir", str(git_dir), *arguments],
        input=input_bytes, capture_output=True, timeout=GIT_TIMEOUT_SECONDS,
        env=_git_environment(), check=False)
    if check and completed.returncode != 0:
        raise GateError("git %s failed: %s" % (
            arguments[0], completed.stderr.decode("utf-8", "replace").strip()))
    return completed.stdout if completed.returncode == 0 else b""


def _object_format(git_dir: Path) -> str:
    value = _git(git_dir, ["rev-parse", "--show-object-format"], check=False)
    return value.decode().strip() or "sha1"


def _read_blobs(git_dir: Path, oids: list[str]) -> dict[str, bytes]:
    """Contents of the given blobs no larger than MAX_SCAN_BYTES."""
    if not oids:
        return {}
    checked = _git(git_dir, [
        "cat-file", "--batch-check=%(objectname) %(objecttype) %(objectsize)"],
        ("\n".join(oids) + "\n").encode())
    wanted = []
    for line in checked.decode().splitlines():
        parts = line.split()
        if (len(parts) == 3 and parts[1] == "blob" and parts[2].isdigit()
                and int(parts[2]) <= MAX_SCAN_BYTES):
            wanted.append(parts[0])
    if not wanted:
        return {}
    output = _git(git_dir, ["cat-file", "--batch"],
                  ("\n".join(wanted) + "\n").encode())
    contents: dict[str, bytes] = {}
    position = 0
    while position < len(output):
        end = output.index(b"\n", position)
        header = output[position:end].split()
        position = end + 1
        if len(header) != 3:
            break
        size = int(header[2])
        contents[header[0].decode()] = output[position:position + size]
        position += size + 1
    return contents


def _tree_file(git_dir: Path, treeish: str, path: str) -> bytes | None:
    listing = _git(git_dir, ["ls-tree", "-z", treeish, "--", path], check=False)
    for entry in listing.split(b"\0"):
        meta, _, _name = entry.partition(b"\t")
        parts = meta.split()
        if len(parts) == 3 and parts[1] == b"blob":
            return _read_blobs(git_dir, [parts[2].decode()]).get(parts[2].decode())
    return None


def _tree_overrides(git_dir: Path, treeish: str) -> list[tuple[str, bytes]]:
    listing = _git(git_dir, ["ls-tree", "-z", treeish, "--",
                             REPOSITORY_OVERRIDE_DIR + "/"], check=False)
    oids: list[tuple[str, str]] = []
    for entry in listing.split(b"\0"):
        meta, _, name = entry.partition(b"\t")
        parts = meta.split()
        if (len(parts) == 3 and parts[1] == b"blob"
                and name.endswith(b".json")):
            oids.append((name.decode("utf-8", "replace"), parts[2].decode()))
    oids = oids[:MAX_OVERRIDE_FILES]
    blobs = _read_blobs(git_dir, [oid for _name, oid in oids])
    return [("%s:%s" % (treeish[:12], name), blobs[oid])
            for name, oid in oids if oid in blobs]


def repository_context(
    git_dir: Path, treeishes: Iterable[str]
) -> tuple[list[dict[str, Any]], list[tuple[str, bytes]], list[dict[str, str]]]:
    """Repository policies and committed overrides at the given commits.

    Policies from every commit are merged (they can only tighten), so a push
    that deletes the repository policy does not loosen it while the stored
    default branch still carries it.
    """
    policies: list[dict[str, Any]] = []
    overrides: list[tuple[str, bytes]] = []
    problems: list[dict[str, str]] = []
    seen: set[str] = set()
    for treeish in treeishes:
        if not treeish or treeish in seen:
            continue
        seen.add(treeish)
        raw = _tree_file(git_dir, treeish, REPOSITORY_POLICY_PATH)
        if raw is not None:
            try:
                policies.append(parse_policy_bytes(raw, repository=True))
            except GateError as exc:
                # An unreadable repository policy tightens nothing; report it.
                problems.append({"source": treeish[:12] + ":"
                                 + REPOSITORY_POLICY_PATH, "reason": str(exc)})
        overrides.extend(_tree_overrides(git_dir, treeish))
    return policies, overrides, problems


def scan_git(
    git_dir: Path,
    revision_arguments: list[str],
    *,
    input_bytes: bytes = b"",
    tips: Iterable[str] = (),
    boundary: str = "git-objects",
    organization: Mapping[str, Any] | None = None,
    repository: str = "",
    extra_overrides: Iterable[tuple[str, bytes]] = (),
    now: float | None = None,
) -> dict[str, Any]:
    """Scan the blobs a rev walk reaches (pushes, fetches, stored history)."""
    organization = organization or normalize_policy(None)
    tips = [tip for tip in tips if tip and not ZERO_OID_RE.fullmatch(tip)]
    stored_head = _git(git_dir, ["rev-parse", "--verify", "-q", "HEAD^{commit}"],
                       check=False).decode().strip()
    policies, committed, problems = repository_context(
        git_dir, [*tips, stored_head])
    report, effective, overridden, rules = _prepare(
        boundary, organization, policies, [*committed, *extra_overrides],
        repository, now)
    report["repositoryPolicyErrors"] = problems
    listing = _git(git_dir, ["rev-list", "--objects", *revision_arguments],
                   input_bytes)
    paths: dict[str, str] = {}
    for line in listing.decode("utf-8", "surrogateescape").split("\n"):
        oid, _, path = line.partition(" ")
        if OBJECT_ID_RE.fullmatch(oid) and path and oid not in paths:
            paths[oid] = path
    blobs = _read_blobs(git_dir, list(paths))
    for oid, path in sorted(paths.items(), key=lambda item: item[1]):
        data = blobs.get(oid)
        if data is None:
            # Trees, and blobs over the size limit, are not scanned.
            continue
        if _looks_binary(data):
            report["skippedFiles"] += 1
            continue
        _add_findings(report, effective, overridden, path=path, blob=oid,
                      text=data.decode("utf-8", "replace"), rules=rules)
    return report


def _worktree_git_dir(worktree: Path) -> Path:
    completed = subprocess.run(
        ["git", "-C", str(worktree), "rev-parse", "--absolute-git-dir"],
        capture_output=True, text=True, timeout=60, check=False)
    if completed.returncode != 0:
        raise GateError("%s is not a Git repository" % worktree)
    return Path(completed.stdout.strip())


def scan_index(
    worktree: Path,
    *,
    organization: Mapping[str, Any] | None = None,
    repository: str = "",
    extra_overrides: Iterable[tuple[str, bytes]] = (),
    now: float | None = None,
) -> dict[str, Any]:
    """Scan the lines a commit would add (the pre-commit boundary)."""
    organization = organization or normalize_policy(None)
    run = lambda arguments, check=True: subprocess.run(  # noqa: E731
        ["git", "-C", str(worktree), *arguments], capture_output=True,
        timeout=GIT_TIMEOUT_SECONDS, check=check)
    git_dir = _worktree_git_dir(worktree)
    has_head = run(["rev-parse", "--verify", "-q", "HEAD"], check=False).returncode == 0
    policies: list[dict[str, Any]] = []
    overrides: list[tuple[str, bytes]] = []
    problems: list[dict[str, str]] = []
    if has_head:
        policies, overrides, problems = repository_context(git_dir, ["HEAD"])
    staged_policy = run(["show", ":" + REPOSITORY_POLICY_PATH], check=False)
    if staged_policy.returncode == 0:
        try:
            policies.append(parse_policy_bytes(staged_policy.stdout,
                                               repository=True))
        except GateError as exc:
            problems.append({"source": ":" + REPOSITORY_POLICY_PATH,
                             "reason": str(exc)})
    staged = run(["ls-files", "-s", "-z", "--", REPOSITORY_OVERRIDE_DIR + "/"])
    for entry in staged.stdout.split(b"\0"):
        meta, _, name = entry.partition(b"\t")
        parts = meta.split()
        if len(parts) == 3 and name.endswith(b".json"):
            blob = _read_blobs(git_dir, [parts[1].decode()]).get(parts[1].decode())
            if blob is not None:
                overrides.append((":" + name.decode("utf-8", "replace"), blob))
    report, effective, overridden, rules = _prepare(
        "pre-commit", organization, policies, [*overrides, *extra_overrides],
        repository, now)
    report["repositoryPolicyErrors"] = problems
    # On an unborn branch `diff --cached` compares with the empty tree, so
    # every staged line counts as added.
    diff = run(["diff", "--cached", "--no-color", "--no-ext-diff", "--text",
                "-U0", "--diff-filter=ACMR", "--no-renames"]
               ).stdout.decode("utf-8", "replace")
    added = parse_unified_diff(diff)
    if not added:
        return report
    listing = run(["ls-files", "-s", "-z", "--", *added]).stdout
    oids: dict[str, str] = {}
    for entry in listing.split(b"\0"):
        meta, _, name = entry.partition(b"\t")
        parts = meta.split()
        if len(parts) == 3:
            oids[name.decode("utf-8", "surrogateescape")] = parts[1].decode()
    blobs = _read_blobs(git_dir, list(oids.values()))
    for path in sorted(added):
        oid = oids.get(path, "")
        data = blobs.get(oid)
        if data is None or _looks_binary(data):
            report["skippedFiles"] += 1
            continue
        _add_findings(report, effective, overridden, path=path, blob=oid,
                      text=data.decode("utf-8", "replace"), rules=rules,
                      lines_of_interest=set(added[path]))
    return report


def pre_push_revisions(
    git_dir: Path, remote: str, stdin_text: str
) -> tuple[list[str], list[str]]:
    """(tips, exclusions) for the objects a push would send."""
    tips: list[str] = []
    exclusions: list[str] = []
    for line in stdin_text.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        _local_ref, local_oid, _remote_ref, remote_oid = parts
        if ZERO_OID_RE.fullmatch(local_oid):
            continue
        tips.append(local_oid)
        # The remote's old tip is only a usable exclusion if we have it.
        if not ZERO_OID_RE.fullmatch(remote_oid) and subprocess.run(
                ["git", "--git-dir", str(git_dir), "cat-file", "-e",
                 remote_oid + "^{commit}"], capture_output=True, timeout=60,
                env=_git_environment(), check=False).returncode == 0:
            exclusions.append(remote_oid)
    if remote and re.fullmatch(r"[A-Za-z0-9._/-]{1,200}", remote):
        exclusions.append("--remotes=" + remote)
    return tips, exclusions


# --------------------------------------------------------------------------
# Guidance


PRE_REPLICATION_STEPS: dict[str, list[str]] = {
    "pre-commit": [
        "Nothing was committed. Remove the value from the file and load it "
        "from the environment or a secret store.",
        "git restore --staged <path>   # unstage while you fix it",
        "If the value is real, rotate it anyway: it has been written to disk "
        "and possibly to editor backups.",
    ],
    "pre-push": [
        "Nothing was sent. Drop the value from every unpushed commit, not "
        "just the newest one:",
        "git rebase -i @{upstream}   # edit the commits that add it",
        "If the value is real, rotate it anyway: it sat in local history.",
    ],
    "push": [
        "This node discarded the push; nothing was stored here.",
        "Other remotes you pushed the same commits to may already hold them. "
        "If the value is real, rotate it now.",
        "Rewrite the unpushed commits so they no longer add the value, then "
        "push again.",
    ],
}
OVERRIDE_STEPS = [
    "Intentional fake test data: move it under a path your organization "
    "allowlists, add a `%s` comment on the line, or ask an allowed signer "
    "for a signed override:" % IGNORE_MARKER,
    "python3 tools/secret_gate.py sign-override --key <ssh key> --signer <you> "
    "--repository <owner/name> --reason '<why this is not a credential>' "
    "--fingerprint <fingerprint> ... > %s/<name>.json" % REPOSITORY_OVERRIDE_DIR,
]

# Detection after another party may already hold the object.
RESPONSE_PLAYBOOK: dict[str, list[str]] = {
    "containment": [
        "Quarantine the repository on every node you operate "
        "(node_operator_policy.py quarantine) so it stops being served there; "
        "its bytes stay for review.",
        "Remove the value from the default branch in a new commit so new "
        "checkouts stop carrying it in their working tree.",
        "Disable any automation (Actions, deploy hooks) that could reuse the "
        "credential until it is rotated.",
    ],
    "rotation": [
        "Revoke or roll the credential at its issuer first; removal from Git "
        "does not make a replicated secret safe.",
        "Deploy the replacement from a secret store, then confirm the old "
        "credential is rejected.",
        "Review the issuer's audit log for use since the object's first "
        "publication time.",
    ],
    "notification": [
        "Tell the repository owner and their security contact. Share the "
        "evidence record (rule, path, blob id, fingerprint), never the value.",
        "If the credential belonged to a third party or reaches personal "
        "data, follow that party's disclosure process and your breach "
        "notification obligations.",
        "Record who was told and when in the node's case log "
        "(node_operator_policy.py report --category privacy).",
    ],
    "residualRisk": [
        "Every clone, fork, mirror, backup, and cache that already received "
        "these objects keeps a copy. Quarantine and history rewrites stop "
        "future serving only on nodes you control; they cannot erase copies "
        "elsewhere.",
        "Treat the secret as compromised until it is rotated, even if no "
        "misuse is visible.",
    ],
}


def response_plan(report: Mapping[str, Any]) -> dict[str, Any]:
    """Containment, rotation, notification, and residual-risk steps."""
    rules = {rule["id"]: rule for rule in RULES}
    rotation_hints = []
    for rule_id in sorted({f["rule"] for f in report.get("findings", [])
                           if f["outcome"] in ("block", "warn")}):
        rule = rules.get(rule_id)
        hint = rule["rotation"] if rule else (
            "Follow your organization's rotation procedure.")
        rotation_hints.append({"rule": rule_id, "step": hint})
    return {
        "contract": CONTRACT_ID,
        "permanenceWarning": PERMANENCE_WARNING,
        "containment": list(RESPONSE_PLAYBOOK["containment"]),
        "rotation": list(RESPONSE_PLAYBOOK["rotation"]),
        "rotationByRule": rotation_hints,
        "notification": list(RESPONSE_PLAYBOOK["notification"]),
        "residualRisk": list(RESPONSE_PLAYBOOK["residualRisk"]),
    }


def telemetry(report: Mapping[str, Any]) -> dict[str, Any]:
    """Value-free counters for dashboards and audit records."""
    return {
        "contract": CONTRACT_ID,
        "boundary": report["boundary"],
        "scannedFiles": report["scannedFiles"],
        "skippedFiles": report["skippedFiles"],
        "blocked": report["counts"]["block"],
        "warned": report["counts"]["warn"],
        "overridden": report["counts"]["overridden"],
        "allowlisted": report["counts"]["allowlisted"],
        "invalidOverrides": len(report["invalidOverrides"]),
        "rules": sorted({f["rule"] for f in report["findings"]}),
    }


def format_report(report: Mapping[str, Any], stage: str) -> list[str]:
    """Human-readable lines; never includes a matched value."""
    counts = report["counts"]
    lines: list[str] = []
    if counts["block"]:
        noun = "commit" if stage == "pre-commit" else "push"
        lines.append("ForkMesh secret scan blocked this %s: %d probable "
                     "secret%s." % (noun, counts["block"],
                                    "" if counts["block"] == 1 else "s"))
    elif counts["warn"]:
        lines.append("ForkMesh secret scan warning: %d probable secret%s "
                     "(not blocked by policy)." % (
                         counts["warn"], "" if counts["warn"] == 1 else "s"))
    shown = [f for f in report["findings"] if f["outcome"] in ("block", "warn")]
    for finding in shown[:20]:
        lines.append("  %s %s (%s) at %s:%d  fingerprint %s" % (
            finding["outcome"].upper(), finding["rule"], finding["title"],
            finding["path"], finding["line"], finding["fingerprint"]))
    if len(shown) > 20:
        lines.append("  ... %d more" % (len(shown) - 20))
    for finding in report["findings"]:
        if finding["outcome"] == "overridden":
            lines.append("  overridden %s at %s:%d by signed override from %s"
                         % (finding["rule"], finding["path"], finding["line"],
                            finding["overrideSigner"]))
    for item in report["invalidOverrides"]:
        lines.append("  ignored override %s: %s" % (item["source"], item["reason"]))
    if shown:
        lines.append("  " + PERMANENCE_WARNING)
    if counts["block"]:
        for step in PRE_REPLICATION_STEPS.get(stage, PRE_REPLICATION_STEPS["push"]):
            lines.append("  " + step)
        for step in OVERRIDE_STEPS:
            lines.append("  " + step)
    return lines


# --------------------------------------------------------------------------
# Client hooks


def hook_script(stage: str, script: Path, organization_policy: Path | None,
                repository: str) -> bytes:
    arguments = ["python3", "-I", str(script), "hook", stage]
    if organization_policy is not None:
        arguments += ["--org-policy", str(organization_policy)]
    if repository:
        arguments += ["--repository", repository]
    return ("#!/bin/sh\n%s\nexec %s \"$@\"\n" % (
        HOOK_MARKER, " ".join(shlex.quote(item) for item in arguments))).encode()


def install_hooks(worktree: Path, *, organization_policy: Path | None,
                  repository: str, force: bool = False) -> dict[str, Any]:
    """Install pre-commit and pre-push; never overwrite a hook we don't own."""
    if organization_policy is not None:
        organization_policy = organization_policy.resolve()
        load_policy_file(organization_policy)
    if repository and not REPOSITORY_RE.fullmatch(repository):
        raise GateError("--repository must be owner/name")
    completed = subprocess.run(
        ["git", "-C", str(worktree), "rev-parse", "--git-path", "hooks"],
        capture_output=True, text=True, timeout=60, check=False)
    if completed.returncode != 0:
        raise GateError("%s is not a Git repository" % worktree)
    hooks = Path(completed.stdout.strip())
    if not hooks.is_absolute():
        hooks = worktree / hooks
    hooks.mkdir(parents=True, exist_ok=True)
    installed, preserved = [], []
    for stage in ("pre-commit", "pre-push"):
        target = hooks / stage
        if target.exists() and not force:
            try:
                current = target.read_bytes()
            except OSError:
                current = b""
            if HOOK_MARKER.encode() not in current:
                preserved.append(str(target))
                continue
        target.write_bytes(hook_script(stage, Path(__file__).resolve(),
                                       organization_policy, repository))
        target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP
                     | stat.S_IXOTH)
        installed.append(str(target))
    return {"installed": installed, "preservedForeignHooks": preserved}


def _load_override_files(paths: Iterable[str]) -> list[tuple[str, bytes]]:
    documents = []
    for path in paths:
        try:
            documents.append((path, Path(path).read_bytes()[:MAX_OVERRIDE_BYTES + 1]))
        except OSError as exc:
            raise GateError("cannot read override %s: %s" % (path, exc))
    return documents


def run_hook(stage: str, arguments: list[str], stdin_text: str,
             *, organization: Mapping[str, Any], repository: str,
             overrides: list[tuple[str, bytes]], worktree: Path,
             stderr: Any = sys.stderr) -> int:
    if stage == "pre-commit":
        report = scan_index(worktree, organization=organization,
                            repository=repository, extra_overrides=overrides)
    elif stage == "pre-push":
        git_dir = _worktree_git_dir(worktree)
        remote = arguments[0] if arguments else ""
        tips, exclusions = pre_push_revisions(git_dir, remote, stdin_text)
        if not tips:
            return 0
        report = scan_git(
            git_dir, [*tips, "--not", *exclusions] if exclusions else tips,
            tips=tips, boundary="pre-push", organization=organization,
            repository=repository, extra_overrides=overrides)
    else:
        raise GateError("unknown hook stage " + stage)
    for line in format_report(report, stage):
        print(line, file=stderr)
    return 1 if report["blocked"] else 0


# --------------------------------------------------------------------------
# Corpus


def evaluate_corpus(corpus_path: Path, scratch: Path | None = None) -> dict[str, Any]:
    """Run the maintained corpus through every scanner boundary.

    A malicious case is detected when its expected rule produces a block or
    warn finding; a benign case is a false positive when anything does. Cases
    flagged ``knownLimitation`` are expected to be misclassified; they still
    count toward the rates, and the evaluation fails if any other case is
    misclassified or a known limitation silently starts passing.
    """
    corpus = json.loads(corpus_path.read_text(encoding="utf-8"))
    organization = normalize_policy(corpus.get("policy"))
    cases = corpus["cases"]
    ids = [case["id"] for case in cases]
    if len(set(ids)) != len(ids):
        raise GateError("corpus case ids must be unique")
    materialized = []
    for case in cases:
        content = "".join(case["content"])
        materialized.append({
            **case,
            "text": content,
            "gitPath": "%s/%s" % (case["id"], case["path"]),
        })

    def detected(report: Mapping[str, Any], path: str) -> set[str]:
        return {f["rule"] for f in report["findings"]
                if f["path"] == path and f["outcome"] in ("block", "warn")}

    outcomes: dict[str, dict[str, set[str]]] = {}
    outcomes["publication"] = {}
    publication = scan_files(
        [(case["gitPath"], case["text"].encode()) for case in materialized],
        organization=organization)
    for case in materialized:
        outcomes["publication"][case["id"]] = detected(publication, case["gitPath"])

    diff_lines = []
    for case in materialized:
        body = case["text"].split("\n")
        if body and body[-1] == "":
            body = body[:-1]
        diff_lines += [
            "diff --git a/%s b/%s" % (case["gitPath"], case["gitPath"]),
            "--- /dev/null", "+++ b/%s" % case["gitPath"],
            "@@ -0,0 +1,%d @@" % len(body), *("+" + line for line in body)]
    review = scan_diff("\n".join(diff_lines) + "\n", organization=organization)
    outcomes["review-diff"] = {
        case["id"]: detected(review, case["gitPath"]) for case in materialized}

    owned = scratch is None
    root = Path(tempfile.mkdtemp(prefix="forkmesh-secret-corpus-")) if owned else scratch
    try:
        worktree = root / "work"
        worktree.mkdir(parents=True)
        env = dict(_git_environment(), GIT_AUTHOR_NAME="Corpus",
                   GIT_AUTHOR_EMAIL="corpus@example.test",
                   GIT_COMMITTER_NAME="Corpus",
                   GIT_COMMITTER_EMAIL="corpus@example.test")

        def git(*arguments: str) -> None:
            subprocess.run(["git", "-C", str(worktree), *arguments],
                           capture_output=True, check=True, env=env,
                           timeout=GIT_TIMEOUT_SECONDS)

        git("init", "-q", "-b", "main")
        for case in materialized:
            target = worktree / case["gitPath"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(case["text"].encode())
        git("add", "-A")
        staged = scan_index(worktree, organization=organization,
                            repository="corpus/secrets")
        outcomes["pre-commit"] = {
            case["id"]: detected(staged, case["gitPath"]) for case in materialized}
        git("commit", "-q", "--no-verify", "-m", "corpus")
        git_dir = _worktree_git_dir(worktree)
        objects = scan_git(git_dir, ["HEAD"], tips=["HEAD"],
                           organization=organization,
                           repository="corpus/secrets")
        outcomes["git-objects"] = {
            case["id"]: detected(objects, case["gitPath"]) for case in materialized}
    finally:
        if owned:
            import shutil

            shutil.rmtree(root, ignore_errors=True)

    bounds = corpus["bounds"]
    boundaries = {}
    ok = True
    for boundary, results in outcomes.items():
        malicious = [c for c in materialized if c["label"] == "malicious"]
        benign = [c for c in materialized if c["label"] == "benign"]
        false_negatives = sorted(
            c["id"] for c in malicious if c["rule"] not in results[c["id"]])
        false_positives = sorted(c["id"] for c in benign if results[c["id"]])
        known = {c["id"] for c in materialized
                 if boundary in c.get("knownLimitation", [])}
        unexpected = sorted((set(false_negatives) | set(false_positives)) - known)
        fixed = sorted(known - set(false_negatives) - set(false_positives))
        fn_rate = len(false_negatives) / len(malicious) if malicious else 0.0
        fp_rate = len(false_positives) / len(benign) if benign else 0.0
        within = (fn_rate <= bounds["maxFalseNegativeRate"]
                  and fp_rate <= bounds["maxFalsePositiveRate"])
        boundary_ok = within and not unexpected and not fixed
        ok = ok and boundary_ok
        boundaries[boundary] = {
            "malicious": len(malicious),
            "benign": len(benign),
            "falseNegatives": false_negatives,
            "falsePositives": false_positives,
            "falseNegativeRate": round(fn_rate, 4),
            "falsePositiveRate": round(fp_rate, 4),
            "unexpected": unexpected,
            "knownLimitationsNowPassing": fixed,
            "ok": boundary_ok,
        }
    covered = {c["rule"] for c in materialized if c["label"] == "malicious"}
    uncovered = sorted(set(RULE_IDS) - covered)
    ok = ok and not uncovered
    return {
        "contract": CONTRACT_ID,
        "corpus": corpus.get("id", ""),
        "bounds": bounds,
        "boundaries": boundaries,
        "rulesWithoutMaliciousCases": uncovered,
        "ok": ok,
    }


# --------------------------------------------------------------------------
# CLI


def _emit(value: Any) -> None:
    print(json.dumps(value, indent=2, sort_keys=True))


def _read_optional_policy(value: str | None) -> dict[str, Any]:
    if not value:
        value = os.environ.get("FORKMESH_SECRET_ORG_POLICY", "")
    return load_policy_file(Path(value) if value else None)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)

    def policy_options(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--org-policy", help="organization secret policy JSON")
        sub.add_argument("--repository", default="", help="owner/name")
        sub.add_argument("--override", action="append", default=[],
                         help="signed override JSON file (repeatable)")

    commands.add_parser("contract", help="print the scanner contract")
    sub = commands.add_parser("validate-policy", help="check a policy file")
    sub.add_argument("policy")
    sub.add_argument("--repository-policy", action="store_true")
    sub = commands.add_parser("scan-files", help="scan whole files")
    policy_options(sub)
    sub.add_argument("--root", default=".")
    sub.add_argument("paths", nargs="+")
    sub = commands.add_parser("scan-diff", help="scan added lines of a diff")
    policy_options(sub)
    sub.add_argument("--diff", help="diff file (default: stdin)")
    sub = commands.add_parser("scan-git", help="scan objects a rev walk reaches")
    policy_options(sub)
    sub.add_argument("--git-dir", required=True)
    sub.add_argument("revisions", nargs="+")
    sub = commands.add_parser("hook", help="run as a Git client hook")
    policy_options(sub)
    sub.add_argument("stage", choices=("pre-commit", "pre-push"))
    sub.add_argument("hook_arguments", nargs="*")
    sub = commands.add_parser("install-hooks", help="install client hooks")
    sub.add_argument("--repo", default=".")
    sub.add_argument("--org-policy")
    sub.add_argument("--repository", default="")
    sub.add_argument("--force", action="store_true")
    sub = commands.add_parser("sign-override", help="sign an override")
    sub.add_argument("--key", required=True)
    sub.add_argument("--signer", required=True)
    sub.add_argument("--repository", required=True)
    sub.add_argument("--fingerprint", action="append", required=True)
    sub.add_argument("--reason", required=True)
    sub.add_argument("--days", type=int, default=30)
    sub = commands.add_parser("verify-override", help="verify an override")
    sub.add_argument("override")
    sub.add_argument("--allowed-signers", required=True)
    sub.add_argument("--repository", required=True)
    sub = commands.add_parser("respond", help="post-replication response plan")
    sub.add_argument("--report", help="scan report JSON (default: stdin)")
    sub = commands.add_parser("corpus", help="evaluate the maintained corpus")
    sub.add_argument("--corpus", default=str(
        Path(__file__).resolve().parents[1]
        / "conformance" / "secrets" / "v1" / "corpus.json"))
    args = parser.parse_args(argv)

    try:
        if args.command == "contract":
            _emit(contract())
        elif args.command == "validate-policy":
            _emit(load_policy_file(Path(args.policy),
                                   repository=args.repository_policy))
        elif args.command == "scan-files":
            root = Path(args.root)
            files = []
            for item in args.paths:
                path = Path(item)
                full = path if path.is_absolute() else root / path
                relative = os.path.relpath(full, root).replace(os.sep, "/")
                files.append((relative, full.read_bytes()))
            report = scan_files(
                files, organization=_read_optional_policy(args.org_policy),
                override_documents=_load_override_files(args.override),
                repository=args.repository)
            _emit(report)
            return 1 if report["blocked"] else 0
        elif args.command == "scan-diff":
            text = (Path(args.diff).read_text(encoding="utf-8", errors="replace")
                    if args.diff else sys.stdin.read())
            report = scan_diff(text,
                               organization=_read_optional_policy(args.org_policy))
            _emit(report)
            return 1 if report["blocked"] else 0
        elif args.command == "scan-git":
            report = scan_git(
                Path(args.git_dir), args.revisions,
                tips=[r for r in args.revisions if not r.startswith("-")
                      and not r.startswith("^")],
                organization=_read_optional_policy(args.org_policy),
                repository=args.repository,
                extra_overrides=_load_override_files(args.override))
            _emit(report)
            return 1 if report["blocked"] else 0
        elif args.command == "hook":
            stdin_text = "" if args.stage == "pre-commit" else sys.stdin.read()
            return run_hook(
                args.stage, args.hook_arguments, stdin_text,
                organization=_read_optional_policy(args.org_policy),
                repository=args.repository,
                overrides=_load_override_files(args.override),
                worktree=Path.cwd())
        elif args.command == "install-hooks":
            _emit(install_hooks(
                Path(args.repo),
                organization_policy=Path(args.org_policy) if args.org_policy else None,
                repository=args.repository, force=args.force))
        elif args.command == "sign-override":
            _emit(sign_override(
                key=Path(args.key), signer=args.signer,
                repository=args.repository, fingerprints=args.fingerprint,
                reason=args.reason, days=args.days))
        elif args.command == "verify-override":
            raw = Path(args.override).read_bytes()
            try:
                document = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                raise GateError("override is not valid JSON")
            item = verify_override(document, repository=args.repository,
                                   allowed_signers=args.allowed_signers)
            _emit({"valid": True, "signer": item["signer"],
                   "fingerprints": item["fingerprints"],
                   "expiresAt": item["expiresAt"]})
        elif args.command == "respond":
            raw = (Path(args.report).read_text(encoding="utf-8")
                   if args.report else sys.stdin.read())
            _emit(response_plan(json.loads(raw)))
        elif args.command == "corpus":
            result = evaluate_corpus(Path(args.corpus))
            _emit(result)
            return 0 if result["ok"] else 1
    except (GateError, OSError, subprocess.SubprocessError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
