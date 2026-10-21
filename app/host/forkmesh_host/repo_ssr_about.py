"""The About rail of the server-rendered repository first view (repo_ssr).

The dashboard fills the rail from four reads after it boots (stats, the
.forkmesh listing, the contributor roster and the size map), so a first view
that drew only placeholders changed under the visitor twice: once when the
page arrived and again when those reads came back. The node reads them from
its own Git gateway while it renders the page, once per commit:

* ``extras()`` fetches them (remembered per commit, so a busy page costs no
  gateway reads after the first view of a push);
* ``rail_values()`` turns them into what the rail shows - file count, size,
  languages and contributors - the way the dashboard computes them
  (06-repo-content.js: loadRepoAboutStats, parseRepoContributorRoster,
  renderRepoAboutLanguages, renderRepoAboutRoster);
* ``seed_reads()`` hands the same answers to the dashboard (trimmed to what
  the rail uses), so its loaders render the rail in the frame it takes the
  page over, with no request and no placeholder in between.

Everything that reaches HTML is escaped by the caller's templates; this module
returns plain values.
"""

from __future__ import annotations

import asyncio
import json
import re
import urllib.parse

# 06-repo-content.js REPO_LANGUAGE_EXTENSIONS / REPO_LANGUAGE_COLORS
# (tests/host/test_repo_ssr.py checks the two stay identical).
LANGUAGE_EXTENSIONS = {
    "c": "C", "h": "C++", "cc": "C++", "cpp": "C++", "cxx": "C++", "hpp": "C++",
    "py": "Python", "js": "JavaScript", "mjs": "JavaScript", "jsx": "JavaScript",
    "ts": "TypeScript", "tsx": "TypeScript", "html": "HTML", "htm": "HTML",
    "css": "CSS", "json": "JSON", "md": "Markdown", "yml": "YAML", "yaml": "YAML",
    "sh": "Shell", "bash": "Shell", "dart": "Dart", "java": "Java", "kt": "Kotlin",
    "rs": "Rust", "go": "Go", "rb": "Ruby", "php": "PHP", "swift": "Swift",
    "m": "Objective-C", "mm": "Objective-C", "qml": "QML", "cmake": "CMake",
    "toml": "TOML", "sql": "SQL", "proto": "Protobuf", "svg": "SVG",
}
LANGUAGE_COLORS = {
    "C": "#555555", "C++": "#f34b7d", "Python": "#3572A5",
    "JavaScript": "#f1e05a", "TypeScript": "#3178c6", "HTML": "#e34c26",
    "CSS": "#563d7c", "JSON": "#8a8a8a", "Markdown": "#083fa1", "YAML": "#cb171e",
    "Shell": "#89e051", "Dart": "#00B4AB", "Java": "#b07219", "Kotlin": "#A97BFF",
    "Rust": "#dea584", "Go": "#00ADD8", "Ruby": "#701516", "PHP": "#4F5D95",
    "Swift": "#F05138", "Objective-C": "#438eff", "QML": "#44a51c",
    "CMake": "#DA3434", "TOML": "#9c4221", "SQL": "#e38c00", "Protobuf": "#4a76c6",
    "SVG": "#ff9900",
}
LANGUAGES_SHOWN = 6
CONTRIBUTORS_SHOWN = 8
ROSTER_DIR = ".forkmesh/contributors"
ROSTER_PATH = ROSTER_DIR + "/contributors.json"
ROSTER_MAX_CHARS = 2 * 1024 * 1024
ROSTER_MAX_ROWS = 10000
SLUG_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,99}$", re.I)
# Author rows handed to the dashboard (without their weekly activity, the bulk
# of a /stats answer); its bot-adjusted contributor count reads every row.
STATS_CONTRIBUTORS_KEPT = 2000
EXTRAS_MAX = 64

_extras = {}   # (owner, repo, commit) -> extras


async def _json(fastpath, url):
    try:
        response = await fastpath.client().get(url)
        if response.status_code != 200:
            return None
        value = response.json()
        return value if isinstance(value, dict) else None
    except Exception:
        return None


async def extras(fastpath, base, owner, repo, commit, tree):
    """The rail's reads at ``commit`` (remembered per commit), or {}.

    ``base`` is the gateway's /v1/public/repositories/<owner>/<repo>/ path and
    ``tree`` the root listing already read for the first view."""
    key = (owner, repo, commit)
    if key in _extras:
        return _extras[key]
    entries = tree.get("entries") or []
    has_forkmesh = any(entry.get("type") == "tree"
                       and entry.get("name") == ".forkmesh" for entry in entries)

    def at(op, *pairs):
        return base + op + "?" + urllib.parse.urlencode(
            list(pairs) + [("ref", commit)])

    reads = [_json(fastpath, at("stats")), _json(fastpath, at("sizes"))]
    if has_forkmesh:
        reads.append(_json(fastpath, at("tree", ("path", ".forkmesh"))))
    stats, sizes, *rest = await asyncio.gather(*reads)
    listing = rest[0] if rest else None
    roster = None
    if listing and any(entry.get("type") == "tree" and entry.get("name") == "contributors"
                       for entry in listing.get("entries") or []):
        blobs = await _json(fastpath, at("blobs", ("path", ROSTER_PATH)))
        blob = ((blobs or {}).get("blobs") or {}).get(ROSTER_PATH)
        if isinstance(blob, dict) and not blob.get("truncated") \
                and blob.get("encoding") != "base64":
            roster = parse_roster(str(blob.get("content") or ""))
    if roster:
        # Only what the rail shows is kept in memory (a roster can be MBs).
        roster = {"count": roster["count"], "rows": roster["rows"][:CONTRIBUTORS_SHOWN]}
    out = {
        "stats": _trim_stats(stats) if stats and stats.get("ok") is not False else None,
        "sizeBytes": _int((sizes or {}).get("size")),
        "listing": listing,
        "roster": roster,
    }
    if len(_extras) >= EXTRAS_MAX:
        _extras.clear()
    _extras[key] = out
    return out


def _int(value):
    try:
        number = int(value)
    except (TypeError, ValueError):
        return 0
    return number if number > 0 else 0


def parse_roster(text):
    """06-repo-content.js parseRepoContributorRoster: {count, rows} or None."""
    if not isinstance(text, str) or not text or len(text) > ROSTER_MAX_CHARS:
        return None
    try:
        data = json.loads(text)
    except ValueError:
        return None
    rows_in = data.get("contributors") if isinstance(data, dict) else None
    if not isinstance(rows_in, list) or not rows_in or len(rows_in) > ROSTER_MAX_ROWS:
        return None
    rows = []
    for index, row in enumerate(rows_in):
        if not isinstance(row, dict):
            continue
        slug = str(row.get("slug") or "").strip()
        safe = slug if SLUG_RE.match(slug) and ".." not in slug else ""
        name = " ".join(str(row.get("name") or row.get("login") or slug or "").split())[:120]
        if not name:
            continue
        commits = row.get("commits")
        commits = int(commits) if isinstance(commits, (int, float)) and commits > 0 else 0
        profile = str(row.get("profile") or "").strip()
        rows.append({"slug": safe, "name": name, "commits": commits,
                     "profile": profile if _https_url(profile) else "", "index": index})
    if not rows:
        return None
    rows.sort(key=lambda row: (-row["commits"], row["index"]))
    declared = data.get("count")
    count = declared if isinstance(declared, int) and not isinstance(declared, bool) \
        and 0 < declared <= 10_000_000 else len(rows)
    return {"count": count, "rows": rows}


def _https_url(value):
    if not value or len(value) > 2048:
        return False
    try:
        parts = urllib.parse.urlsplit(value)
    except ValueError:
        return False
    return parts.scheme == "https" and bool(parts.hostname) and "@" not in parts.netloc


BOT_PATTERNS = (
    re.compile(r"\[bot\]$", re.I), re.compile(r"^github[\s_-]?actions?$", re.I),
    re.compile(r"^(?:dependabot|renovate)$", re.I), re.compile(r"(?:^|[\s_-])bot$", re.I),
    re.compile(r"^[a-z0-9][a-z0-9_-]*-actions$", re.I),
)


def is_bot(row, platform=""):
    """06-repo-content.js repoContributorIsBot."""
    name = str(row.get("name") or "").strip()
    email = str(row.get("email") or "").strip().lower()
    if not name and not email:
        return False
    if any(pattern.search(name) for pattern in BOT_PATTERNS) or "[bot]@" in email:
        return True
    lowered = name.lower()
    platform = str(platform or "").strip().lower()
    if lowered in ("forkmesh actions", "forkmesh deploy") or (
            platform and lowered == platform + " actions"):
        return True
    return email == "actions@forkmesh.local"


def languages(stats):
    """[(language, weight)] ranked as renderRepoAboutLanguages draws them."""
    tally = {}
    for ext, info in ((stats or {}).get("extensions") or {}).items():
        language = LANGUAGE_EXTENSIONS.get(str(ext).lower())
        if not language or not isinstance(info, dict):
            continue
        weight = _int(info.get("bytes")) or _int(info.get("files"))
        if weight:
            tally[language] = tally.get(language, 0) + weight
    return sorted(tally.items(), key=lambda item: -item[1])[:LANGUAGES_SHOWN]


def rail_values(extra, platform=""):
    """What the About rail shows, or {} when nothing was read."""
    if not extra:
        return {}
    stats = extra.get("stats") or {}
    values = {"files": _int(stats.get("fileCount")), "size": extra.get("sizeBytes") or 0,
              "languages": languages(stats)}
    roster = extra.get("roster")
    if roster:
        values["contributors"] = {"total": roster["count"], "roster": True,
                                  "rows": roster["rows"][:CONTRIBUTORS_SHOWN]}
    else:
        rows = [row for row in stats.get("contributors") or []
                if isinstance(row, dict) and (row.get("name") or row.get("email"))]
        people = [row for row in rows if not is_bot(row, platform)]
        known = _int(stats.get("contributorCount")) or len(rows)
        if people:
            values["contributors"] = {
                "total": max(len(people), known - (len(rows) - len(people))),
                "roster": False, "rows": people[:CONTRIBUTORS_SHOWN]}
    return values


def _trim_stats(stats):
    """A /stats answer without what the rail never reads (per-author weekly
    activity, extensions no language claims)."""
    trimmed = {key: value for key, value in stats.items()
               if key not in ("contributors", "extensions")}
    trimmed["extensions"] = {
        ext: {"bytes": _int((info or {}).get("bytes")),
              "files": _int((info or {}).get("files"))}
        for ext, info in (stats.get("extensions") or {}).items()
        if str(ext).lower() in LANGUAGE_EXTENSIONS and isinstance(info, dict)}
    trimmed["contributors"] = [
        {key: row.get(key) for key in ("name", "email", "commits", "avatarUrl")
         if row.get(key) is not None}
        for row in (stats.get("contributors") or [])[:STATS_CONTRIBUTORS_KEPT]
        if isinstance(row, dict)]
    return trimmed


def seed_reads(extra, ref):
    """The dashboard's reads of the rail, keyed as repoReadSeedKey builds them
    for ``ref`` (the branch the page shows), trimmed to what the rail uses."""
    if not extra or not ref:
        return {}
    reads = {}
    suffix = "ref=" + ref
    stats = extra.get("stats")
    if stats:
        trimmed = _trim_stats(stats)
        if extra.get("roster"):
            # A repository with a roster shows the roster, not the authors.
            trimmed["contributors"] = []
        reads["stats?" + suffix] = trimmed
    listing = extra.get("listing")
    if listing:
        reads["tree?path=.forkmesh&" + suffix] = listing
    roster = extra.get("roster")
    if roster:
        shown = roster["rows"][:CONTRIBUTORS_SHOWN]
        content = json.dumps({"count": roster["count"], "contributors": [
            {"slug": row["slug"], "name": row["name"], "commits": row["commits"],
             "profile": row["profile"]} for row in shown]},
            separators=(",", ":"), ensure_ascii=False)
        reads["blobs?path=%s&%s" % (ROSTER_PATH, suffix)] = {
            "ok": True, "blobs": {ROSTER_PATH: {
                "ok": True, "encoding": "utf8", "content": content,
                "size": len(content.encode("utf-8")), "truncated": False}}}
    return reads


def format_size(size):
    """02-helpers.js formatSize."""
    value = float(size or 0)
    units = ("B", "KB", "MB", "GB", "TB")
    unit = 0
    while value >= 1024 and unit < len(units) - 1:
        value /= 1024
        unit += 1
    return ("%d %s" % (value, units[unit])) if unit == 0 else "%.1f %s" % (value, units[unit])


def initials(name):
    words = [word for word in re.split(r"[\s._@-]+", str(name or "?")) if word]
    return ((words[0][:1] if words else "?") + (words[1][:1] if len(words) > 1 else "")).upper()
