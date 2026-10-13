#!/usr/bin/env python3
"""Category comparison claim ledger: check, report, and render (issue #509).

``conformance/category-comparisons/v1/ledger.json`` holds every claim the
public comparison page makes about ForkMesh and about adjacent categories:
centralized hosting, self-hosted forges, decentralized Git-native tools,
replicated storage, and basic Git servers. ``check`` fails when:

- a matrix cell is missing, duplicated, or scoped to the wrong column;
- a ForkMesh claim cites no repository test, specification, or product
  evidence, or cites a test, file, or heading that no longer exists;
- a claim about another category cites no dated primary source, or names a
  vendor that none of its sources is published by;
- a claim, source, or the page review is older than the review cadence, or
  dated in the future;
- claim text or next-step copy uses an overclaiming phrase;
- a measured next step carries a UTM value off the attribution allowlist;
- the generated block in the comparison page is out of date.

``render`` refreshes that block; ``report`` prints the measures as JSON.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import sys
import tomllib
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


ROOT = Path(__file__).resolve().parents[1]
LEDGER_PATH = ROOT / "conformance" / "category-comparisons" / "v1" / "ledger.json"
SCHEMA = "forkmesh-category-comparisons-v1"
DOC_BEGIN = "<!-- category-comparisons:begin -->"
DOC_END = "<!-- category-comparisons:end -->"
SUBJECT_KINDS = {"forkmesh", "category"}
SCOPES = {"protocol", "hosted-service", "category"}
BASES = {"evidence", "definition", "no-claim"}
REPOSITORY_EVIDENCE = {"test", "specification", "product-evidence"}
EVIDENCE_KINDS = REPOSITORY_EVIDENCE | {"source"}
NEXT_STEP_KINDS = ("trial", "self-host", "community")
SECTIONS = (
    ("notForYou", "Not for you"),
    ("limitations", "Known limitations"),
    ("migrationCost", "Migration cost"),
    ("hostedDependencies", "Hosted dependencies"),
)
# Vendors a category claim may name only when one of its sources is theirs.
VENDORS = ("GitHub", "GitLab", "Gitea", "Forgejo", "Codeberg", "Bitbucket",
           "SourceHut", "Radicle", "git-bug")
OVERCLAIMS = re.compile(
    r"\b(?:unlimited|guaranteed?|never (?:goes? down|lose|fails?)|always (?:on|available|online)"
    r"|100\s?%|best[- ]in[- ]class|better than|fastest|most secure|only platform"
    r"|zero risk|risk[- ]free|bulletproof|unbreakable)\b",
    re.I,
)
UTM_ALLOWLISTS = {
    "utm_source": "ATTRIBUTION_ALLOWED_SOURCES",
    "utm_medium": "ATTRIBUTION_ALLOWED_MEDIUMS",
    "utm_campaign": "ATTRIBUTION_ALLOWED_CAMPAIGNS",
}


class LedgerError(Exception):
    pass


def load_ledger(path: Path = LEDGER_PATH) -> dict:
    try:
        ledger = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise LedgerError(f"cannot read {path}: {exc}") from exc
    if not isinstance(ledger, dict) or ledger.get("schema") != SCHEMA:
        raise LedgerError(f"{path} is not a {SCHEMA} ledger")
    return ledger


def _date(value: object, where: str, problems: list[str]) -> dt.date | None:
    try:
        return dt.date.fromisoformat(str(value))
    except ValueError:
        problems.append(f"{where}: date {value!r} is not YYYY-MM-DD")
        return None


# --------------------------------------------------------------------------
# Evidence resolution


def _test_exists(reference: str, root: Path) -> bool:
    path_text, _, name = reference.partition("::")
    path = root / path_text
    if not name or not path.is_file():
        return False
    source = path.read_text(encoding="utf-8")
    if path.suffix == ".py":
        return re.search(rf"^\s*(?:async\s+)?def {re.escape(name)}\(", source, re.M) is not None
    if path.suffix == ".go":
        return re.search(rf"^func {re.escape(name)}\(", source, re.M) is not None
    return f'"{name}"' in source or f"'{name}'" in source


def _anchors(path: Path, cache: dict[Path, set[str]]) -> set[str]:
    if path not in cache:
        tools = str(ROOT / "app" / "tools")
        if tools not in sys.path:
            sys.path.insert(0, tools)
        import docs_markdown

        rendered = docs_markdown.render(path.read_text(encoding="utf-8"))
        ids = {heading.id for heading in rendered.headings}
        ids.update(re.findall(r'<a id="([^"]+)"></a>', rendered.html))
        cache[path] = ids
    return cache[path]


def _evidence_problems(item: dict, where: str, ledger: dict, root: Path,
                       anchor_cache: dict[Path, set[str]]) -> list[str]:
    kind = item.get("kind")
    if kind not in EVIDENCE_KINDS:
        return [f"{where}: unknown evidence kind {kind!r}"]
    if kind == "source":
        if item.get("id") not in {source["id"] for source in ledger.get("sources", [])}:
            return [f"{where}: unknown source {item.get('id')!r}"]
        return []
    if kind == "test":
        reference = str(item.get("ref", ""))
        return [] if _test_exists(reference, root) else [f"{where}: test {reference!r} does not exist"]
    path = root / str(item.get("path", ""))
    if not item.get("path") or not path.is_file():
        return [f"{where}: evidence file {item.get('path')!r} does not exist"]
    anchor = item.get("anchor")
    if anchor:
        if path.suffix != ".md":
            return [f"{where}: anchor {anchor!r} on a non-Markdown file"]
        if anchor not in _anchors(path, anchor_cache):
            return [f"{where}: {item['path']} has no heading #{anchor}"]
    return []


# --------------------------------------------------------------------------
# Checking


def attribution_allowlists(root: Path = ROOT) -> dict[str, set[str]]:
    wrangler = tomllib.loads((root / "app" / "wrangler.toml").read_text(encoding="utf-8"))
    variables = wrangler.get("vars", {})
    return {
        parameter: {value.strip().lower() for value in str(variables.get(name, "")).split(",") if value.strip()}
        for parameter, name in UTM_ALLOWLISTS.items()
    }


def _claim_problems(claim: dict, where: str, subject: dict | None, ledger: dict, today: dt.date,
                    root: Path, anchor_cache: dict[Path, set[str]]) -> list[str]:
    problems: list[str] = []
    text = str(claim.get("text", "")).strip()
    if not text:
        problems.append(f"{where}: empty text")
    if OVERCLAIMS.search(text):
        problems.append(f"{where}: overclaiming phrase {OVERCLAIMS.search(text).group(0)!r}")
    basis = claim.get("basis", "evidence")
    if basis not in BASES:
        return problems + [f"{where}: unknown basis {basis!r}"]
    forkmesh = subject is None or subject["kind"] == "forkmesh"
    evidence = claim.get("evidence") or []
    if basis != "evidence":
        if forkmesh:
            problems.append(f"{where}: a ForkMesh claim must cite evidence, not {basis!r}")
        if evidence or claim.get("verifiedOn"):
            problems.append(f"{where}: a {basis!r} cell cites no evidence and has no verified date")
        named = [vendor for vendor in VENDORS if re.search(rf"\b{re.escape(vendor)}\b", text)]
        if named:
            problems.append(f"{where}: a {basis!r} cell may not name {', '.join(named)}")
        return problems
    if not evidence:
        return problems + [f"{where}: no evidence"]
    for index, item in enumerate(evidence):
        problems.extend(_evidence_problems(item, f"{where} evidence[{index}]", ledger, root, anchor_cache))
    kinds = {item.get("kind") for item in evidence}
    if forkmesh and not kinds & REPOSITORY_EVIDENCE:
        problems.append(f"{where}: a ForkMesh claim needs a test, specification, or product evidence")
    if not forkmesh:
        if kinds != {"source"}:
            problems.append(f"{where}: a category claim cites only dated primary sources")
        publishers = {
            source["publisher"] for source in ledger.get("sources", [])
            if source["id"] in {item.get("id") for item in evidence}
        }
        for vendor in VENDORS:
            if re.search(rf"\b{re.escape(vendor)}\b", text) and vendor not in publishers:
                problems.append(f"{where}: names {vendor} without a {vendor} source")
    verified = _date(claim.get("verifiedOn"), where, problems)
    if verified is not None:
        problems.extend(_freshness(verified, where, ledger, today))
    return problems


def _freshness(verified: dt.date, where: str, ledger: dict, today: dt.date) -> list[str]:
    if verified > today:
        return [f"{where}: verified {verified} is in the future"]
    age = (today - verified).days
    if age > int(ledger["reviewCadenceDays"]):
        return [f"{where}: stale, verified {verified} ({age} days ago)"]
    return []


def build_report(ledger: dict, root: Path = ROOT, today: dt.date | None = None) -> dict:
    today = today or dt.date.today()
    problems: list[str] = []
    anchor_cache: dict[Path, set[str]] = {}
    cadence = ledger.get("reviewCadenceDays")
    if not isinstance(cadence, int) or not 1 <= cadence <= 180:
        raise LedgerError("reviewCadenceDays must be an integer between 1 and 180")
    reviewed = _date(ledger.get("reviewedOn"), "reviewedOn", problems)
    if reviewed is not None:
        problems.extend(_freshness(reviewed, "page review", ledger, today))

    subjects = {subject["id"]: subject for subject in ledger.get("subjects", [])}
    for subject in subjects.values():
        if subject.get("kind") not in SUBJECT_KINDS or subject.get("scope") not in SCOPES:
            problems.append(f"subject {subject['id']}: unknown kind or scope")
        if (subject.get("kind") == "category") != (subject.get("scope") == "category"):
            problems.append(f"subject {subject['id']}: only a category uses the category scope")
    scopes = {subject.get("scope") for subject in subjects.values() if subject.get("kind") == "forkmesh"}
    if not {"protocol", "hosted-service"} <= scopes:
        problems.append("the matrix must separate protocol capability from hosted-service capability")
    dimensions = [dimension["id"] for dimension in ledger.get("dimensions", [])]

    for source in ledger.get("sources", []):
        where = f"source {source.get('id')}"
        if not str(source.get("url", "")).startswith("https://"):
            problems.append(f"{where}: url must be https")
        for key in ("publisher", "title", "quote"):
            if not str(source.get(key, "")).strip():
                problems.append(f"{where}: missing {key}")
        verified = _date(source.get("verifiedOn"), where, problems)
        if verified is not None:
            problems.extend(_freshness(verified, where, ledger, today))

    ids: set[str] = set()
    cells: dict[tuple[str, str], str] = {}
    counts = {basis: 0 for basis in BASES}
    oldest: dt.date | None = None

    def remember(item: dict, where: str) -> None:
        nonlocal oldest
        if item.get("id") in ids:
            problems.append(f"{where}: duplicate id")
        ids.add(item.get("id"))
        try:
            verified = dt.date.fromisoformat(str(item.get("verifiedOn")))
        except ValueError:
            return
        oldest = verified if oldest is None else min(oldest, verified)

    for claim in ledger.get("matrix", []):
        where = f"claim {claim.get('id')}"
        remember(claim, where)
        subject = subjects.get(claim.get("subject"))
        if subject is None or claim.get("dimension") not in dimensions:
            problems.append(f"{where}: unknown subject or dimension")
            continue
        key = (claim["subject"], claim["dimension"])
        if key in cells:
            problems.append(f"{where}: duplicates {cells[key]}")
        cells[key] = claim["id"]
        counts[claim.get("basis", "evidence")] = counts.get(claim.get("basis", "evidence"), 0) + 1
        problems.extend(_claim_problems(claim, where, subject, ledger, today, root, anchor_cache))
    for subject_id in subjects:
        for dimension in dimensions:
            if (subject_id, dimension) not in cells:
                problems.append(f"matrix: no cell for {subject_id} / {dimension}")

    for key, label in SECTIONS:
        entries = ledger.get(key) or []
        if not entries:
            problems.append(f"{label}: section is empty")
        for entry in entries:
            where = f"{label} {entry.get('id')}"
            remember(entry, where)
            problems.extend(_claim_problems(entry, where, None, ledger, today, root, anchor_cache))
            if key == "notForYou":
                instead = entry.get("instead") or []
                if not instead or any(subjects.get(item, {}).get("kind") != "category" for item in instead):
                    problems.append(f"{where}: instead must name at least one category")
    covered = {item for entry in ledger.get("notForYou", []) for item in entry.get("instead", [])}
    for subject in subjects.values():
        if subject["kind"] == "category" and subject["id"] not in covered:
            problems.append(f"category {subject['id']}: no not-for-you entry points to it")

    allowlists = attribution_allowlists(root)
    steps = {step.get("kind"): step for step in ledger.get("nextSteps", [])}
    for kind in NEXT_STEP_KINDS:
        if kind not in steps:
            problems.append(f"next steps: missing a {kind} path")
    measured = 0
    for step in ledger.get("nextSteps", []):
        where = f"next step {step.get('id')}"
        copy = f"{step.get('label', '')} {step.get('boundary', '')}"
        if not str(step.get("label", "")).strip() or not str(step.get("boundary", "")).strip():
            problems.append(f"{where}: needs a label and a boundary")
        if OVERCLAIMS.search(copy):
            problems.append(f"{where}: overclaiming phrase {OVERCLAIMS.search(copy).group(0)!r}")
        for index, item in enumerate(step.get("evidence") or []):
            problems.extend(_evidence_problems(item, f"{where} evidence[{index}]", ledger, root, anchor_cache))
        if not step.get("evidence"):
            problems.append(f"{where}: no evidence for its boundary")
        url = str(step.get("url", ""))
        if url.startswith("https://"):
            query = parse_qs(urlsplit(url).query)
            if step.get("measured"):
                measured += 1
                for parameter, allowed in allowlists.items():
                    values = query.get(parameter, [])
                    if len(values) != 1 or values[0] not in allowed:
                        problems.append(f"{where}: {parameter}={values!r} is not on {UTM_ALLOWLISTS[parameter]}")
        elif not (root / url).is_file():
            problems.append(f"{where}: {url!r} is neither https nor a repository file")
        if not step.get("measured") and not str(step.get("measurementGap", "")).strip():
            problems.append(f"{where}: an unmeasured path must say why")

    for correction in ledger.get("corrections", []):
        where = f"correction {correction.get('date')}"
        _date(correction.get("date"), where, problems)
        if not str(correction.get("summary", "")).strip() or not correction.get("claims"):
            problems.append(f"{where}: needs a summary and the corrected claim ids")

    documentation = root / str(ledger.get("documentation", ""))
    if not documentation.is_file():
        problems.append(f"documentation {ledger.get('documentation')!r} does not exist")
    elif extract_block(documentation.read_text(encoding="utf-8")) != render_block(ledger, root):
        problems.append(f"{ledger['documentation']}: generated block is out of date; run render")

    due = reviewed + dt.timedelta(days=cadence) if reviewed else None
    return {
        "ok": not problems,
        "problems": problems,
        "checkedOn": today.isoformat(),
        "reviewedOn": ledger.get("reviewedOn"),
        "nextReviewDue": due.isoformat() if due else None,
        "daysUntilReviewDue": (due - today).days if due else None,
        "oldestClaimVerifiedOn": oldest.isoformat() if oldest else None,
        "matrixCells": len(cells),
        "matrixCellsByBasis": counts,
        "primarySources": len(ledger.get("sources", [])),
        "notForYouEntries": len(ledger.get("notForYou", [])),
        "corrections": len(ledger.get("corrections", [])),
        "measuredNextSteps": measured,
        "unmeasuredNextSteps": sorted(
            step["id"] for step in ledger.get("nextSteps", []) if not step.get("measured")),
    }


# --------------------------------------------------------------------------
# Rendering


def _cell(text: str) -> str:
    return " ".join(str(text).split()).replace("|", "\\|")


def _link(item: dict, ledger: dict, root: Path) -> str:
    documentation_dir = (root / ledger["documentation"]).parent
    kind = item["kind"]
    if kind == "source":
        source = next(source for source in ledger["sources"] if source["id"] == item["id"])
        return f"[{source['publisher']}: {source['title']}]({source['url']})"
    if kind == "test":
        path_text, _, name = item["ref"].partition("::")
        target = Path(path_text)
        label = f"test `{name}`"
    else:
        target = Path(item["path"])
        label = f"`{target.name}`" + (f" §{item['anchor']}" if item.get("anchor") else "")
    relative = Path(*([".."] * len(documentation_dir.relative_to(root).parts)), target).as_posix()
    if target.parts[:2] == ("www", "docs"):
        relative = Path(*target.parts[len(documentation_dir.relative_to(root).parts):]).as_posix()
    anchor = f"#{item['anchor']}" if kind != "test" and item.get("anchor") else ""
    return f"[{label}]({relative}{anchor})"


def _evidence(entry: dict, ledger: dict, root: Path) -> str:
    basis = entry.get("basis", "evidence")
    if basis == "definition":
        return "By definition of the category"
    if basis == "no-claim":
        return "No claim made"
    links = "; ".join(_link(item, ledger, root) for item in entry["evidence"])
    return f"{links}. Verified {entry['verifiedOn']}"


def render_block(ledger: dict, root: Path = ROOT) -> str:
    subjects = {subject["id"]: subject for subject in ledger["subjects"]}
    reviewed = dt.date.fromisoformat(ledger["reviewedOn"])
    due = reviewed + dt.timedelta(days=ledger["reviewCadenceDays"])
    cells = {(claim["subject"], claim["dimension"]): claim for claim in ledger["matrix"]}
    lines = [
        DOC_BEGIN,
        "",
        "<!-- Generated by tools/category_comparisons.py render; edit the ledger, not this block. -->",
        "",
        "## Review status",
        "",
        f"Reviewed **{ledger['reviewedOn']}** by {ledger['reviewOwner']}. The next review is due "
        f"**{due.isoformat()}**, every {ledger['reviewCadenceDays']} days; after that date the check "
        "fails until every claim is verified again.",
        "",
        "## At a glance",
        "",
        "| Option | What it is | Consider it instead of ForkMesh when |",
        "| --- | --- | --- |",
    ]
    for subject in ledger["subjects"]:
        reasons = [entry for entry in ledger["notForYou"] if subject["id"] in entry.get("instead", [])]
        consider = "<br>".join(_cell(entry["text"]) for entry in reasons) or "—"
        lines.append(f"| {_cell(subject['label'])} | {_cell(subject['definition'])} | {consider} |")
    lines += ["", "## Not for you", ""]
    for entry in ledger["notForYou"]:
        instead = ", ".join(subjects[item]["label"] for item in entry["instead"])
        lines.append(f"- **{_cell(entry['text'])}** Consider: {instead}. "
                     f"Evidence: {_evidence(entry, ledger, root)}.")
    lines += ["", "## Comparison matrix", ""]
    for dimension in ledger["dimensions"]:
        lines += [
            f"### {dimension['label']}",
            "",
            f"{dimension['question']}",
            "",
            "| Option | Scope | What holds | Evidence |",
            "| --- | --- | --- | --- |",
        ]
        for subject in ledger["subjects"]:
            claim = cells[(subject["id"], dimension["id"])]
            lines.append(
                f"| {_cell(subject['label'])} | {subject['scope']} | {_cell(claim['text'])} "
                f"| {_evidence(claim, ledger, root)} |")
        lines.append("")
    for key, label in SECTIONS[1:]:
        lines += [f"## {label}", ""]
        for entry in ledger[key]:
            lines.append(f"- {_cell(entry['text'])} Evidence: {_evidence(entry, ledger, root)}.")
        lines.append("")
    lines += ["## Next steps", ""]
    for step in ledger["nextSteps"]:
        url = step["url"]
        if not url.startswith("https://"):
            url = Path(url).relative_to(Path(ledger["documentation"]).parent).as_posix()
        measured = "Measured with UTM attribution." if step["measured"] else f"Not measured: {step['measurementGap']}"
        lines.append(f"- **[{step['label']}]({url})** — {_cell(step['boundary'])} {measured}")
    lines += [
        "",
        "## Primary sources",
        "",
        "| Source | Verified | What it says |",
        "| --- | --- | --- |",
    ]
    for source in ledger["sources"]:
        lines.append(f"| [{_cell(source['publisher'])}: {_cell(source['title'])}]({source['url']}) "
                     f"| {source['verifiedOn']} | “{_cell(source['quote'])}” |")
    lines += ["", "## Corrections", ""]
    if ledger["corrections"]:
        for correction in ledger["corrections"]:
            lines.append(f"- {correction['date']}: {_cell(correction['summary'])} "
                         f"(claims: {', '.join(correction['claims'])})")
    else:
        lines.append("No published claim has been corrected yet.")
    lines += ["", DOC_END]
    return "\n".join(lines)


def extract_block(text: str) -> str:
    start = text.find(DOC_BEGIN)
    end = text.find(DOC_END)
    if start < 0 or end < start:
        return ""
    return text[start:end + len(DOC_END)]


def render_document(ledger: dict, root: Path = ROOT) -> bool:
    path = root / ledger["documentation"]
    text = path.read_text(encoding="utf-8")
    current = extract_block(text)
    if not current:
        raise LedgerError(f"{ledger['documentation']} has no generated block markers")
    updated = text.replace(current, render_block(ledger, root))
    if updated != text:
        path.write_text(updated, encoding="utf-8")
        return True
    return False


# --------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=("check", "report", "render"))
    parser.add_argument("--ledger", type=Path, default=LEDGER_PATH)
    parser.add_argument("--today", type=dt.date.fromisoformat,
                        help="check freshness as of this date instead of today")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        ledger = load_ledger(args.ledger)
        if args.command == "render":
            changed = render_document(ledger)
            print("category comparisons: " + ("documentation updated" if changed else "documentation current"))
            return 0
        report = build_report(ledger, today=args.today)
    except LedgerError as exc:
        print(f"category comparisons: {exc}", file=sys.stderr)
        return 2
    encoded = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    if args.command == "report" or not report["ok"]:
        sys.stdout.write(encoded)
    else:
        print(f"category comparisons: {report['matrixCells']} cells verified; "
              f"next review due {report['nextReviewDue']}")
    return 0 if report["ok"] or args.command == "report" else 1


if __name__ == "__main__":
    raise SystemExit(main())
