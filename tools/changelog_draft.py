#!/usr/bin/env python3
from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import changelog  # noqa: E402

MAX_ITEM_CHARS = 140
AREAS = changelog.AREAS

PLATFORM_WORDS = {"ios": "iPhone", "iphone": "iPhone", "ipad": "iPhone", "android": "Android",
                  "desktop": "Desktop", "qt": "Desktop", "mobile": "", "native": "",
                  "app": "", "server": "Server", "api": "Server", "web": "Server",
                  "core": "", "phone": "", "phones": ""}

TOPIC_WORDS = (
    ("Market", ("market", "marketplace", "package", "packages")),
    ("Calls", ("call", "calls", "calling", "ringtone", "disguise", "disguises")),
    ("Projects", ("project", "projects", "instance", "instances")),
    ("Chat", ("chat", "message", "messages", "channel", "channels", "dm", "dms")),
    ("Agents", ("agent", "agents", "runner", "runners")),
    ("World", ("world",)),
    ("Settings", ("settings", "changelog")),
)

PREFIX_ONLY = {"Settings"}

TOPIC_PHRASES = (
    ("Calls", re.compile(r"\bface ?swap|\bvideo call|\bvoice chang|\bcall background", re.I)),
)

PATH_TOPICS = (
    ("Calls", re.compile(r"(^|/)(calls?|chat-call-[^/]*)(/|$)|/Calls/|call", re.I)),
    ("Market", re.compile(r"market", re.I)),
)

PATH_PLATFORMS = (
    ("iPhone", "native/ios/"),
    ("Android", "native/android/"),
    ("Desktop", "desktop/"),
    ("Server", "app/"),
)

NOISE_SUBJECT = re.compile(
    r"^(\d+ files? changed|merge\b|revert\b|wip\b|fixup!|squash!|"
    r"(docs?|documentation|document|tests?|testing|ci|lint|chore|refactor|format|"
    r"formatting|typo|bump|build|tooling|cleanup)\b)", re.I)
NOISE_WORDS = re.compile(r"\b(tests?|typo|lint|flake8|pyflakes|clang-tidy|docs?)\b", re.I)
NOISE_BODY = re.compile(r"^(add|fix|update|adjust|move)\s+(a\s+|the\s+)?tests?\b|\b(pngs?|svgs?|woff2?|assets|"
                        r"lockfile|ci|lint|refactor|typo)\b", re.I)
CONVENTIONAL = re.compile(r"^(feat|fix|perf|chore|docs|test|tests|refactor|style|build|ci)(\(([^)]*)\))?!?:\s*(.+)$",
                          re.I)
CODE_TOKEN = re.compile(r"`[^`]*`|\b\w+\.\w+\(|\b[a-z]{2,}[A-Z]\w*\b|\b[A-Z][a-z0-9]+[A-Z]\w*\b|"
                        r"\b\w+_\w+\b|\b[0-9a-f]{7,40}\b|\b\w+\.\w+\b|\w+/\w+")
PRODUCT_WORDS = re.compile(r"\b(ForkMesh|GitHub|GitLab|YouTube|FaceTime|AirPods|AirDrop|iOS|iPhone|iPad|"
                           r"macOS|watchOS|WhatsApp|LinkedIn|PayPal|TestFlight|JavaScript|TypeScript|"
                           r"app\.forkmesh\.com|forkmesh\.com|world\.forkmesh\.com)\b")
DOC_PATH = re.compile(r"(^docs/|\.md$|(^|/)(tests?|Tests|__tests__)/|(^|/)test_[^/]*\.py$|"
                      r"Tests?\.(kt|swift)$|_test\.(py|go)$|\.test\.[jt]s$|(^|/)\.forkmesh/|"
                      r"(^|/)CHANGELOG)")

VERBS = {"fix": "Fixed", "fixes": "Fixed", "add": "Added", "adds": "Added",
         "remove": "Removed", "removes": "Removed", "drop": "Removed", "drops": "Removed",
         "show": "Shows", "make": "Made", "move": "Moved", "rename": "Renamed",
         "update": "Updated", "improve": "Improved", "speed": "Sped", "support": "Added support for",
         "allow": "Allows", "let": "Lets", "stop": "Stopped", "hide": "Hides", "keep": "Keeps",
         "use": "Uses", "open": "Opens", "redesign": "Redesigned", "restore": "Restored",
         "handle": "Handles", "prevent": "Prevented", "shrink": "Shrank", "mark": "Marked",
         "serve": "Serves", "reload": "Reloads", "refresh": "Refreshes", "render": "Renders",
         "name": "Names", "put": "Put", "animate": "Animated", "subset": "Trimmed",
         "acknowledge": "Acknowledges", "roll": "Rolled", "cut": "Cut", "treat": "Treats",
         "classify": "Classifies", "introduce": "Introduced", "seed": "Seeds", "invite": "Invite",
         "suggest": "Suggests", "sync": "Syncs", "change": "Changed", "refine": "Refined",
         "explain": "Explains", "reach": "Reach", "tweak": "Tweaked", "polish": "Polished",
         "simplify": "Simplified", "speedup": "Sped up", "unify": "Unified", "replace": "Replaced",
         "rework": "Reworked", "rebuild": "Rebuilt", "enable": "Enabled", "disable": "Disabled",
         "start": "Starts", "send": "Sends", "save": "Saves", "match": "Matches", "cover": "Covers",
         "blur": "Blurs", "swap": "Swaps", "list": "Lists", "scrub": "Cleaned up", "give": "Gives",
         "avoid": "Avoids", "clear": "Clears", "skip": "Skips", "retry": "Retries", "wait": "Waits",
         "warn": "Warns", "ask": "Asks", "count": "Counts", "load": "Loads", "fetch": "Fetches",
         "track": "Tracks", "remember": "Remembers", "group": "Groups", "sort": "Sorts",
         "filter": "Filters", "link": "Links", "pin": "Pins", "turn": "Turned", "set": "Sets",
         "bring": "Brought", "return": "Returns", "write": "Writes", "read": "Reads",
         "accept": "Accepts", "reject": "Rejects"}


@dataclass
class Commit:
    sha: str
    subject: str
    paths: list[str] = field(default_factory=list)


def git(repo: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True,
                          check=True).stdout


def has_commit(repo: Path, rev: str) -> bool:
    if not rev:
        return False
    return subprocess.run(["git", "-C", str(repo), "cat-file", "-e", f"{rev}^{{commit}}"],
                          capture_output=True).returncode == 0


def is_ancestor(repo: Path, older: str, newer: str) -> bool:
    return subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", older, newer],
                          capture_output=True).returncode == 0


def parse_log(text: str) -> list[Commit]:
    commits: list[Commit] = []
    for block in text.split("\x1e"):
        lines = [line for line in block.strip("\n").split("\n")]
        if not lines or not lines[0].strip():
            continue
        head = lines[0].split("\x1f")
        if len(head) < 3:
            continue
        sha, parents, subject = head[0], head[1], head[2]
        if len(parents.split()) > 1:
            continue
        commits.append(Commit(sha, subject.strip(), [line.strip() for line in lines[1:] if line.strip()]))
    return commits


def commits_between(repo: Path, target: str, after_commit: str = "", after_date: str = "") -> list[Commit]:
    fmt = "--format=%x1e%H%x1f%P%x1f%s"
    if after_commit and has_commit(repo, after_commit) and is_ancestor(repo, after_commit, target):
        out = git(repo, "log", "--no-merges", "--name-only", fmt, f"{after_commit}..{target}")
    elif after_date:
        day = after_date if len(after_date) == 10 else f"{after_date}-01"
        out = git(repo, "log", "--no-merges", "--name-only", fmt, f"--since={day} 00:00", target)
    else:
        return []
    return parse_log(out)


def is_noise(commit: Commit) -> bool:
    subject = commit.subject
    if not subject or NOISE_SUBJECT.match(subject):
        return True
    conventional = CONVENTIONAL.match(subject)
    if conventional and conventional.group(1).lower() in {"chore", "docs", "test", "tests", "refactor",
                                                          "style", "build", "ci"}:
        return True
    topic, body = subject_parts(subject)
    if topic and NOISE_WORDS.search(topic):
        return True
    if NOISE_BODY.search(body):
        return True
    if commit.paths and all(DOC_PATH.search(path) for path in commit.paths):
        return True
    return False


def words(text: str) -> list[str]:
    return re.findall(r"[a-z0-9]+", text.lower())


def subject_parts(subject: str) -> tuple[str, str]:
    conventional = CONVENTIONAL.match(subject.strip())
    if conventional:
        return (conventional.group(3) or "").strip(), conventional.group(4).strip()
    match = re.match(r"^([A-Za-z][\w .&/+-]{0,40}):\s+(.+)$", subject)
    if match:
        return match.group(1).strip(), match.group(2).strip()
    return "", subject.strip()


def platform_of(paths: list[str]) -> str:
    found = {name for name, prefix in PATH_PLATFORMS for path in paths if path.startswith(prefix)}
    for name, _ in PATH_PLATFORMS:
        if found == {name}:
            return name
    return ""


def area_of(commit: Commit) -> str:
    prefix, body = subject_parts(commit.subject)
    prefix_words = words(prefix)
    topical = [word for word in prefix_words if word not in PLATFORM_WORDS]
    for area, keys in TOPIC_WORDS:
        if any(word in keys for word in topical):
            return area
    for area, pattern in TOPIC_PHRASES:
        if pattern.search(body):
            return area
    body_words = set(words(body))
    for area, keys in TOPIC_WORDS:
        if area not in PREFIX_ONLY and body_words.intersection(keys):
            return area
    for area, pattern in PATH_TOPICS:
        if commit.paths and all(pattern.search(path) or DOC_PATH.search(path) for path in commit.paths):
            return area
    for word in prefix_words:
        if PLATFORM_WORDS.get(word):
            return PLATFORM_WORDS[word]
    return platform_of(commit.paths) or "Other improvements"


def foreign(commit: Commit, platform: str) -> bool:
    other = {"ios": "native/android/", "android": "native/ios/"}.get(platform)
    if not other:
        return False
    prefix, _ = subject_parts(commit.subject)
    names = set(words(prefix))
    mine = {"ios": {"ios", "iphone", "ipad"}, "android": {"android"}}[platform]
    theirs = {"ios": {"android"}, "android": {"ios", "iphone", "ipad"}}[platform]
    if names & theirs and not names & mine:
        return True
    code = [path for path in commit.paths if not DOC_PATH.search(path)]
    return bool(code) and all(path.startswith(other) for path in code)


def sentence(subject: str) -> str:
    _, body = subject_parts(subject)
    text = re.sub(r"\s*\((#\d+|[0-9a-f]{7,40})\)\s*$", "", body).strip()
    text = re.sub(r"\s+", " ", text).rstrip(" .;:,")
    if not text:
        return ""
    first, _, rest = text.partition(" ")
    verb = VERBS.get(first.lower())
    if verb:
        text = f"{verb} {rest}".strip()
    text = text[0].upper() + text[1:]
    if len(text) > MAX_ITEM_CHARS:
        cut = text[:MAX_ITEM_CHARS - 1].rsplit(" ", 1)[0].rstrip(" ,;:")
        return cut + "…"
    return text + "."


def readable(commit: Commit) -> bool:
    _, body = subject_parts(commit.subject)
    plain = PRODUCT_WORDS.sub("", body)
    return len(words(body)) >= 3 and not CODE_TOKEN.search(plain)


def sections_for(commits: list[Commit], platform: str = "") -> list[dict]:
    grouped: dict[str, list[str]] = {}
    for commit in commits:
        if is_noise(commit) or foreign(commit, platform) or not readable(commit):
            continue
        text = sentence(commit.subject)
        if text:
            grouped.setdefault(area_of(commit), []).append(text)
    hidden = {"ios": "Android", "android": "iPhone"}.get(platform)
    ordered = [{"title": area, "items": grouped[area]} for area in AREAS
               if area in grouped and area != hidden]
    return changelog.clean_sections(ordered)


def resolve_target(repo: Path, commit: str) -> str:
    for rev in (commit, "main", "HEAD"):
        if rev and has_commit(repo, rev):
            return rev
    raise SystemExit(f"no commit {commit!r} in {repo}")


def sections_since(repo: Path, since: str, commit: str = "HEAD", platform: str = "") -> list[dict]:
    target = resolve_target(repo, commit)
    if changelog.DATE.match(since):
        return sections_for(commits_between(repo, target, after_date=since), platform)
    return sections_for(commits_between(repo, target, since), platform)
