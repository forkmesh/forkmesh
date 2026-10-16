#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import io
import json
import re
import sys
from pathlib import Path
from urllib.parse import quote

REPO = Path(__file__).resolve().parents[1]
GRANTS_DIR = REPO / "docs" / "finance" / "grants"
SOURCE = GRANTS_DIR / "grants.json"
CSV_OUT = GRANTS_DIR / "forkmesh-grants.csv"
RANKED_OUT = GRANTS_DIR / "ranked.md"
CHATGPT_URL = "https://chatgpt.com/"
URL_LIMIT = 8000
NOTES_LIMIT = 2500

REQUIRED = (
    "id", "funder", "program", "category", "url", "applyUrl", "amountMinUsd",
    "amountMaxUsd", "amountText", "deadline", "status", "eligibility",
    "geography", "entityTypes", "process", "requiredMaterials", "decisionTime",
    "obligations", "forkmeshAngle", "fitScore", "effort", "verified",
    "sources", "risks",
)
CATEGORY_LABELS = {
    "open-source-infrastructure": "Open-source infrastructure",
    "government-research": "Government and research",
    "web3-ecosystem": "Web3 ecosystem",
    "cloud-credits": "Cloud and in-kind credits",
    "security-privacy": "Security, privacy and internet freedom",
    "preservation-commons": "Preservation and digital commons",
    "ai-devtools": "AI and developer tools",
}
CSV_HEADER = (
    "Name", "Company", "Website", "Pipeline", "Stage", "Deal value",
    "Next step", "Source", "Tags", "Notes", "Rank", "Fit score", "Award",
    "Deadline", "Round", "Geography", "Who can apply", "Effort",
    "Decision time", "Apply at",
)

FORKMESH_BRIEF = (
    "ForkMesh (https://forkmesh.com) is MIT-licensed open-source software that lets "
    "developers collaborate on, mirror and preserve Git repositories across independent, "
    "developer-operated hosts, so no single hosting provider is a point of failure. Git stays "
    "the source of truth; clones are served from live, signed, integrity-checked mirrors, so a "
    "repository stays reachable when its source host goes offline. Issues, pull requests, "
    "reviews and releases are signed records stored in the repository. The desktop app "
    "(Linux, macOS, Windows) is a full node that hosts and mirrors repositories and runs AI "
    "coding agents in isolated worktrees; iPhone and Android apps are access clients. The "
    "protocol, self-hosting and public clone are free; only bounded hosted services are paid. "
    "Source: https://forkmesh.com/forkmesh/forkmesh. Docs: https://forkmesh.com/docs/. Blog: "
    "https://forkmesh.com/blog. Changelog: https://forkmesh.com/changelog."
)


def load(path: Path = SOURCE) -> list[dict]:
    grants = json.loads(path.read_text(encoding="utf-8"))
    seen = set()
    for grant in grants:
        missing = [key for key in REQUIRED if key not in grant]
        if missing:
            raise ValueError(f"{grant.get('id', '?')}: missing {', '.join(missing)}")
        if grant["id"] in seen:
            raise ValueError(f"duplicate id {grant['id']}")
        if grant["category"] not in CATEGORY_LABELS:
            raise ValueError(f"{grant['id']}: unknown category {grant['category']}")
        for key in ("url", "applyUrl"):
            if not str(grant[key]).startswith("https://"):
                raise ValueError(f"{grant['id']}: {key} is not https")
        seen.add(grant["id"])
    return ranked(grants)


def ranked(grants: list[dict]) -> list[dict]:
    order = sorted(grants, key=lambda grant: (
        -int(grant["fitScore"]), not grant["verified"], -int(grant["amountMaxUsd"]),
        grant["id"]))
    return [dict(grant, rank=index) for index, grant in enumerate(order, start=1)]


def usd(value: int) -> str:
    return f"${int(value):,}" if int(value) > 0 else ""


def usd_range(grant: dict) -> str:
    low, high = int(grant["amountMinUsd"]), int(grant["amountMaxUsd"])
    if low and high and low != high:
        return f"{usd(low)} - {usd(high)}"
    return usd(high or low) or "varies"


def short(name: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"\s*\([^()]*\)", "", name)).strip()


def title(grant: dict) -> str:
    program, funder = short(grant["program"]), short(grant["funder"])
    return program if funder.lower() in program.lower() else f"{funder}: {program}"


def next_step(grant: dict) -> str:
    deadline = str(grant["deadline"]).strip()
    if grant["status"] == "rolling" or deadline.lower() == "rolling":
        return "Rolling: draft and submit now"
    if grant["status"] == "invite-only":
        return f"Invite only: ask for an invitation ({deadline})"[:200]
    return f"Apply by {deadline}"[:200]


def details(grant: dict) -> list[str]:
    return [
        f"Award: {grant['amountText']}",
        f"Deadline: {grant['deadline']} ({grant['status']})",
        f"Who can apply: {grant['eligibility']} Geography: {grant['geography']}. "
        f"Entity types: {grant['entityTypes']}.",
        f"Program page: {grant['url']}",
        f"Apply at: {grant['applyUrl']}",
        f"How to apply: {grant['process']}",
        "What the application asks for:",
        *[f"- {item}" for item in grant["requiredMaterials"]],
        f"Decision time: {grant['decisionTime']}",
        f"Obligations: {grant['obligations']}",
        f"Why ForkMesh fits: {grant['forkmeshAngle']}",
        f"Risks: {grant['risks']}",
    ]


def notes(grant: dict) -> str:
    text = "\n".join(details(grant))
    return text if len(text) <= NOTES_LIMIT else text[:NOTES_LIMIT - 1] + "…"


def prompt(grant: dict) -> str:
    head = [
        f"You are helping the ForkMesh team write a complete draft of our application to "
        f"{title(grant)}.",
        "",
        "How to write it:",
        f"- First open the program page and the application form ({grant['url']} and "
        f"{grant['applyUrl']}) and use their current questions, limits, deadline and criteria; "
        "say so if anything below is out of date.",
        "- Answer each question under its own heading, in their order, with its word or "
        "character count after it.",
        "- Propose one concrete, fundable piece of ForkMesh work that fits what they fund, with "
        "milestones, dates, deliverables and a budget in their currency inside their range.",
        "- Plain, specific words, no hype, no emoji. Use only facts given here or found on the "
        "linked pages; never invent traction, users, partners, numbers or names.",
        "- Where the form needs something only we can supply (legal entity, registration "
        "numbers, bank details, CVs, letters of support), never invent it: list it at the end.",
        "- Reply with the finished application, ready to paste, then a checklist of every "
        "document and fact we still have to supply, then three ways to make it stronger.",
        "",
        "## About us: ForkMesh",
        FORKMESH_BRIEF,
        "",
        f"## About {title(grant)}",
    ]
    return "\n".join(head + details(grant))


def chatgpt_url(grant: dict) -> str:
    text = prompt(grant)
    url = f"{CHATGPT_URL}?q={quote(text, safe='')}"
    while len(url) > URL_LIMIT:
        text = text[:int(len(text) * URL_LIMIT / len(url)) - 2].rstrip() + "…"
        url = f"{CHATGPT_URL}?q={quote(text, safe='')}"
    return url


def csv_text(grants: list[dict]) -> str:
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(CSV_HEADER)
    for grant in grants:
        writer.writerow((
            title(grant), grant["funder"], grant["url"], "grants", "researching",
            int(grant["amountMaxUsd"]), next_step(grant), "Grant research 2026-10",
            grant["category"], notes(grant), grant["rank"], grant["fitScore"],
            grant["amountText"], grant["deadline"], grant["status"],
            grant["geography"], grant["entityTypes"], grant["effort"],
            grant["decisionTime"], grant["applyUrl"],
        ))
    return out.getvalue()


def cell(value) -> str:
    return " ".join(str(value).split()).replace("|", "\\|")


def ranked_text(grants: list[dict]) -> str:
    lines = [
        "# Ranked grants for ForkMesh",
        "",
        "Generated by `tools/grants_build.py` from `grants.json`; edit the JSON and "
        "re-run, never this file. See [README.md](README.md).",
        "",
        "| # | Grant | Amount (USD) | Deadline | Effort | Fit | Apply |",
        "|---|-------|--------------|----------|--------|-----|-------|",
    ]
    for grant in grants:
        lines.append(
            f"| {grant['rank']} | [{cell(title(grant))}](#{grant['id']}) | "
            f"{cell(usd_range(grant))} | {cell(grant['deadline'])} | {grant['effort']} | "
            f"{grant['fitScore']} | [Apply]({grant['applyUrl']}) |")
    for grant in grants:
        lines += [
            "",
            f"<a id=\"{grant['id']}\"></a>",
            f"## {grant['rank']}. {title(grant)}",
            "",
            f"**[Draft this application in ChatGPT]({chatgpt_url(grant)})** · "
            f"[Program page]({grant['url']}) · [Apply]({grant['applyUrl']})",
            "",
            f"- **Funder:** {grant['funder']}",
            f"- **Category:** {CATEGORY_LABELS[grant['category']]}",
            f"- **Amount:** {grant['amountText']} (about {usd_range(grant)})",
            f"- **Deadline:** {grant['deadline']} ({grant['status']})",
            f"- **Fit score:** {grant['fitScore']}/100 · **Effort:** {grant['effort']} · "
            f"**Checked live:** {'yes' if grant['verified'] else 'no, confirm first'}",
            f"- **Who can apply:** {grant['eligibility']}",
            f"- **Geography:** {grant['geography']} · **Entity types:** {grant['entityTypes']}",
            f"- **Decision time:** {grant['decisionTime']}",
            f"- **Obligations:** {grant['obligations']}",
            "",
            f"**Why ForkMesh fits:** {grant['forkmeshAngle']}",
            "",
            "**How to apply:**",
            "",
            *steps(grant["process"]),
            "",
            "**What the application asks for:**",
            "",
            *[f"- {item}" for item in grant["requiredMaterials"]],
            "",
            f"**Risks:** {grant['risks']}",
            "",
            "**Sources:** " + ", ".join(f"<{url}>" for url in grant["sources"]),
        ]
    return "\n".join(lines) + "\n"


def steps(process: str) -> list[str]:
    parts = [part.strip() for part in re.split(r"(?:^|\s)(?=\d{1,2}\.\s)", process) if part.strip()]
    return parts if len(parts) > 1 else [f"1. {process.strip()}"]


def build() -> dict[Path, str]:
    grants = load()
    return {CSV_OUT: csv_text(grants), RANKED_OUT: ranked_text(grants)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Build the grants CRM import and ranked page from docs/finance/grants/grants.json.")
    parser.add_argument("--check", action="store_true",
                        help="fail if the generated files are out of date")
    args = parser.parse_args(argv)
    stale = []
    for path, text in build().items():
        if args.check:
            if not path.exists() or path.read_bytes() != text.encode("utf-8"):
                stale.append(path)
        else:
            path.write_bytes(text.encode("utf-8"))
    if stale:
        print("out of date: " + ", ".join(str(path.relative_to(REPO)) for path in stale),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
