#!/usr/bin/env python3
"""Lint user-facing copy for discouraged terminology (issue #508).

The approved glossary, discouraged terms, allowances, and compatibility aliases
live in ``www/docs/terminology.json``; ``www/docs/terminology.md`` is the human
guide. Only user-visible text is checked: HTML text and accessible attribute
values, Markdown prose outside code and link targets, and sentence-like string
literals in JavaScript, Dart, and C++ sources. Comments and code are ignored.

Exit status is 1 when a primary surface has a finding, or when an allowance is
stale or its required explanation is missing. ``--extended`` also reports the
wider audit surfaces without failing on them.
"""

from __future__ import annotations

import argparse
import bisect
import html
import json
import re
import sys
from dataclasses import asdict, dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_PATH = ROOT / "www" / "docs" / "terminology.json"
REQUIRED_APPROVED_TERMS = (
    "repository", "pull request", "review", "branch", "fork", "mirror", "seed",
    "trusted maintainer",
)
CATEGORIES = ("replace", "explain")
ALLOW_MARKER = re.compile(r"terminology:\s*allow=([a-z0-9,-]+)")
TERM_ID = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")
SENTENCE_LIKE = re.compile(r"[A-Za-z]{2,}\s+[A-Za-z]")

_HTML_HIDDEN = re.compile(
    r"<!--.*?-->|<(script|style|pre|code)\b[^>]*>.*?</\1\s*>", re.S | re.I)
_HTML_TAG = re.compile(r"<[^>]*>", re.S)
_HTML_VISIBLE_ATTRIBUTE = re.compile(
    r"""\b(?:aria-label|title|placeholder|alt)\s*=\s*(?:"([^"]*)"|'([^']*)')""", re.I)
_MARKDOWN_HIDDEN = re.compile(
    r"^(```|~~~).*?^\1[^\n]*$|`[^`\n]*`|<!--.*?-->|\]\([^)\n]*\)|<https?://[^>\s]*>",
    re.S | re.M)
_CODE_TOKEN = (
    r"//[^\n]*|/\*.*?\*/|"
    r'"(?:\\.|[^"\\\n])*"|' r"'(?:\\.|[^'\\\n])*'|`|\{|\}"
)
_JS_TOKEN = re.compile(_CODE_TOKEN, re.S)
_TEMPLATE_TOKEN = re.compile(r"\\.|`|\$\{", re.S)


class ContractError(RuntimeError):
    """The terminology contract is malformed."""


@dataclass(frozen=True)
class Term:
    id: str
    pattern: re.Pattern
    preferred: str
    category: str


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    term: str
    match: str
    preferred: str


@dataclass
class LintResult:
    scanned: list[str] = field(default_factory=list)
    findings: list[Finding] = field(default_factory=list)
    allowed: int = 0
    stale_allowances: list[dict] = field(default_factory=list)
    unexplained_allowances: list[dict] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not (self.findings or self.stale_allowances or self.unexplained_allowances)

    def counts(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for finding in self.findings:
            counts[finding.term] = counts.get(finding.term, 0) + 1
        return dict(sorted(counts.items()))


def load_contract(path: Path = CONTRACT_PATH) -> dict:
    try:
        contract = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ContractError(f"cannot read terminology contract: {exc}") from exc
    validate_contract(contract)
    return contract


def _require_text(item: dict, key: str, context: str) -> str:
    value = item.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ContractError(f"{context}: {key} must be non-empty text")
    return value


def validate_contract(contract: dict, root: Path = ROOT) -> None:
    if not isinstance(contract, dict) or contract.get("schemaVersion") != 1:
        raise ContractError("unsupported terminology schemaVersion")

    approved = contract.get("approvedTerms")
    if not isinstance(approved, list) or not approved:
        raise ContractError("approvedTerms must be a non-empty list")
    names = set()
    for item in approved:
        name = _require_text(item, "term", "approved term")
        _require_text(item, "definition", name)
        names.add(name)
    missing = [term for term in REQUIRED_APPROVED_TERMS if term not in names]
    if missing:
        raise ContractError(f"approvedTerms is missing: {', '.join(missing)}")

    terms = compile_terms(contract)
    term_ids = {term.id for term in terms}

    for key in ("primarySurfaces", "extendedAuditSurfaces"):
        surfaces = contract.get(key)
        if not isinstance(surfaces, list) or not surfaces:
            raise ContractError(f"{key} must be a non-empty list")
        for pattern in surfaces:
            if not isinstance(pattern, str) or not sorted(root.glob(pattern)):
                raise ContractError(f"{key}: {pattern!r} matches no file")

    allowances = contract.get("allowances")
    if not isinstance(allowances, list):
        raise ContractError("allowances must be a list")
    for allowance in allowances:
        path = _require_text(allowance, "path", "allowance")
        _require_text(allowance, "reason", path)
        if allowance.get("term") not in term_ids:
            raise ContractError(f"{path}: allowance names an unknown term")
        explanation = allowance.get("explanation")
        if explanation is not None and (not isinstance(explanation, str) or not explanation.strip()):
            raise ContractError(f"{path}: explanation must be non-empty text")

    aliases = contract.get("compatibilityAliases")
    if not isinstance(aliases, list) or not aliases:
        raise ContractError("compatibilityAliases must be a non-empty list")
    alias_ids = set()
    for alias in aliases:
        alias_id = _require_text(alias, "id", "compatibility alias")
        if alias_id in alias_ids:
            raise ContractError(f"duplicate compatibility alias: {alias_id}")
        alias_ids.add(alias_id)
        for key in ("kind", "alias", "canonical", "policy"):
            _require_text(alias, key, alias_id)


def compile_terms(contract: dict) -> list[Term]:
    items = contract.get("discouragedTerms")
    if not isinstance(items, list) or not items:
        raise ContractError("discouragedTerms must be a non-empty list")
    terms: list[Term] = []
    seen = set()
    for item in items:
        term_id = _require_text(item, "id", "discouraged term")
        if not TERM_ID.match(term_id) or term_id in seen:
            raise ContractError(f"invalid or duplicate discouraged term id: {term_id}")
        seen.add(term_id)
        if item.get("category") not in CATEGORIES:
            raise ContractError(f"{term_id}: category must be one of {CATEGORIES}")
        _require_text(item, "reason", term_id)
        # A space in a pattern matches any whitespace, so a phrase that wraps
        # across source lines is still caught.
        source = _require_text(item, "pattern", term_id).replace(" ", r"\s+")
        flags = 0 if item.get("caseSensitive") else re.I
        try:
            pattern = re.compile(source, flags)
        except re.error as exc:
            raise ContractError(f"{term_id}: invalid pattern: {exc}") from exc
        terms.append(Term(term_id, pattern, _require_text(item, "preferred", term_id),
                          item["category"]))
    return terms


def _blank(text: str) -> str:
    return re.sub(r"[^\n]", " ", text)


def html_segments(source: str) -> list[tuple[int, str]]:
    text = _HTML_HIDDEN.sub(lambda match: _blank(match.group(0)), source)
    pieces: list[str] = []
    last = 0
    for tag in _HTML_TAG.finditer(text):
        pieces.append(text[last:tag.start()])
        raw = tag.group(0)
        kept = list(_blank(raw))
        for attribute in _HTML_VISIBLE_ATTRIBUTE.finditer(raw):
            group = 1 if attribute.group(1) is not None else 2
            start, end = attribute.span(group)
            kept[start:end] = raw[start:end]
        pieces.append("".join(kept))
        last = tag.end()
    pieces.append(text[last:])
    return [(1, html.unescape("".join(pieces)))]


def markdown_segments(source: str) -> list[tuple[int, str]]:
    return [(1, _MARKDOWN_HIDDEN.sub(lambda match: _blank(match.group(0)), source))]


def code_segments(source: str) -> list[tuple[int, str]]:
    """Sentence-like string literals, with template-literal interpolation."""
    newlines = [index for index, character in enumerate(source) if character == "\n"]
    segments: list[tuple[int, str]] = []

    def emit(start: int, body: str) -> None:
        if SENTENCE_LIKE.search(body):
            segments.append((bisect.bisect_right(newlines, start - 1) + 1, body))

    token = _JS_TOKEN
    frames: list[list] = [["code", 0]]
    position = 0
    while position < len(source):
        frame = frames[-1]
        if frame[0] == "code":
            match = token.search(source, position)
            if match is None:
                break
            value = match.group(0)
            position = match.end()
            if value == "`":
                frames.append(["template", position])
            elif value == "{":
                frame[1] += 1
            elif value == "}":
                if frame[1] == 0 and len(frames) > 1:
                    frames.pop()
                    frames[-1][1] = position
                else:
                    frame[1] = max(0, frame[1] - 1)
            elif not value.startswith(("//", "/*")):
                emit(match.start() + 1, value[1:-1])
            continue
        match = _TEMPLATE_TOKEN.search(source, position)
        if match is None:
            emit(frame[1], source[frame[1]:])
            break
        position = match.end()
        if match.group(0).startswith("\\"):
            continue
        emit(frame[1], source[frame[1]:match.start()])
        if match.group(0) == "`":
            frames.pop()
        else:
            frames.append(["code", 0])
    return segments


def text_segments(path: Path, source: str) -> list[tuple[int, str]]:
    suffix = path.suffix.casefold()
    if suffix in {".html", ".htm"}:
        return html_segments(source)
    if suffix == ".md":
        return markdown_segments(source)
    if suffix in {".js", ".mjs", ".cjs", ".cpp", ".cc", ".h", ".hpp"}:
        return code_segments(source)
    return [(1, source)]


def resolve_surfaces(patterns: list[str], root: Path = ROOT) -> list[Path]:
    paths: dict[Path, None] = {}
    for pattern in patterns:
        for path in sorted(root.glob(pattern)):
            if path.is_file():
                paths.setdefault(path, None)
    return list(paths)


def lint(paths: list[Path], contract: dict, *, root: Path = ROOT,
         check_allowances: bool = True) -> LintResult:
    terms = compile_terms(contract)
    allowances = {
        (allowance["path"], allowance["term"]): allowance
        for allowance in contract.get("allowances", [])
    }
    used: set[tuple[str, str]] = set()
    result = LintResult()
    for path in paths:
        resolved = path.resolve()
        try:
            relative = resolved.relative_to(root.resolve()).as_posix()
        except ValueError:
            relative = resolved.as_posix()
        source = path.read_text(encoding="utf-8")
        lines = source.split("\n")
        flattened = " ".join(source.split())
        result.scanned.append(relative)
        for start_line, text in text_segments(path, source):
            for term in terms:
                for match in term.pattern.finditer(text):
                    line = start_line + text.count("\n", 0, match.start())
                    nearby = " ".join(lines[max(0, line - 2):line])
                    marker = ALLOW_MARKER.search(nearby)
                    if marker and term.id in marker.group(1).split(","):
                        result.allowed += 1
                        continue
                    allowance = allowances.get((relative, term.id))
                    if allowance is not None:
                        used.add((relative, term.id))
                        explanation = allowance.get("explanation")
                        if explanation and " ".join(explanation.split()) not in flattened:
                            if allowance not in result.unexplained_allowances:
                                result.unexplained_allowances.append(allowance)
                        else:
                            result.allowed += 1
                        continue
                    result.findings.append(Finding(
                        relative, line, term.id, " ".join(match.group(0).split()),
                        term.preferred))
    if check_allowances:
        scanned = set(result.scanned)
        result.stale_allowances = [
            allowance for key, allowance in allowances.items()
            if key[0] in scanned and key not in used
        ]
    return result


def _report(label: str, result: LintResult) -> dict:
    return {
        "scope": label,
        "scannedFiles": len(result.scanned),
        "discouragedTermCount": len(result.findings),
        "countsByTerm": result.counts(),
        "allowedOccurrences": result.allowed,
        "findings": [asdict(finding) for finding in result.findings],
        "staleAllowances": result.stale_allowances,
        "unexplainedAllowances": result.unexplained_allowances,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("paths", nargs="*", type=Path,
                        help="lint these files instead of the primary surfaces")
    parser.add_argument("--extended", action="store_true",
                        help="also report (without failing) the extended audit surfaces")
    parser.add_argument("--json", action="store_true", help="print a JSON report")
    args = parser.parse_args(argv)

    try:
        contract = load_contract()
    except ContractError as exc:
        print(f"terminology lint: {exc}", file=sys.stderr)
        return 2

    if args.paths:
        primary = lint([path.resolve() for path in args.paths], contract,
                       check_allowances=False)
    else:
        primary = lint(resolve_surfaces(contract["primarySurfaces"]), contract)
    reports = [_report("primary", primary)]
    if args.extended:
        extended = lint(resolve_surfaces(contract["extendedAuditSurfaces"]), contract,
                        check_allowances=False)
        reports.append(_report("extended", extended))

    if args.json:
        print(json.dumps(reports, indent=2))
    else:
        for report in reports:
            for finding in report["findings"]:
                print(f"{finding['path']}:{finding['line']}: [{finding['term']}] "
                      f"\"{finding['match']}\" -> use {finding['preferred']}")
            for allowance in report["staleAllowances"]:
                print(f"{allowance['path']}: stale allowance for {allowance['term']}")
            for allowance in report["unexplainedAllowances"]:
                print(f"{allowance['path']}: {allowance['term']} lacks its explanation "
                      f"{allowance['explanation']!r}")
            print(f"{report['scope']}: {report['discouragedTermCount']} discouraged "
                  f"term(s) in {report['scannedFiles']} file(s)")
    return 0 if primary.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
