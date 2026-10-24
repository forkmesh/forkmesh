#!/usr/bin/env python3
from __future__ import annotations

import base64
import hashlib
import json
import re
import time
from pathlib import Path
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


SCHEMA = "forkmesh-attachments-v1"
ISSUE_SCHEMA = "forkmesh-attachments-issue-v1"
DIR_REL = ".forkmesh/attachments"
INDEX_REL = DIR_REL + "/index.json"
ISSUES_REL = DIR_REL + "/issues"
MAX_BYTES = 25 * 1024 * 1024
PASS_BYTES = 256 * 1024 * 1024
PASS_FILES = 300
TOTAL_BYTES = 1024 * 1024 * 1024
PASS_SECONDS = 300
TIMEOUT_SECONDS = 30
PACE_SECONDS = 0.5
RETRY_MS = 24 * 3600 * 1000
RETRY_MAX_MS = 30 * RETRY_MS
RATE_WAIT_MAX_SECONDS = 60
MAX_URL = 4096
USER_AGENT = "forkmesh-forge-sync/1 (attachment mirror)"
HANDLERS: tuple[Any, ...] = ()

SOURCE_RE = re.compile(
    r"https://(?:"
    r"github\.com/(?:"
    r"user-attachments/assets/[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"
    r"|user-attachments/files/\d+/[^\s\"'<>()\[\]`?#]+"
    r"|[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/(?:"
    r"assets/\d+/[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}"
    r"|files/\d+/[^\s\"'<>()\[\]`?#]+))"
    r"|user-images\.githubusercontent\.com/\d+/[A-Za-z0-9._-]+"
    r"|private-user-images\.githubusercontent\.com/\d+/[A-Za-z0-9._-]+(?:\?jwt=[A-Za-z0-9._-]+)?"
    r"|objects\.githubusercontent\.com/github-production-repository-file-5c1aeb/\d+/\d+"
    r"(?:\?[^\s\"'<>()\[\]`]*)?)")
TRAILING = ".,:;!?"
PRIVATE_PATH_RE = re.compile(
    r"^/(\d+)/(\d+-([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"
    r"\.[A-Za-z0-9]{1,8})$", re.I)
OBJECT_PATH_RE = re.compile(r"^/github-production-repository-file-5c1aeb/\d+/(\d+)$")
FILE_NAME_RE = re.compile(r"^[^/\\\x00-\x1f\"'<>?#]{1,255}$")
GITHUB_PATH_RE = re.compile(
    r"^/(?:user-attachments/(?:assets|files)/|[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/(?:assets|files)/)")
CONTENT_HOSTS = frozenset({
    "user-images.githubusercontent.com",
    "private-user-images.githubusercontent.com",
    "objects.githubusercontent.com",
    "github-production-user-asset-6210df.s3.amazonaws.com",
    "github-production-repository-file-5c1aeb.s3.amazonaws.com",
})
STORED_PATH_RE = re.compile(r"^\.forkmesh/attachments/[0-9a-f]{64}\.[a-z0-9]{1,8}$")
TEXT_NAMES = frozenset({"txt", "log", "csv", "tsv", "json", "md", "c", "h", "py", "go", "sh",
                        "conf", "cfg", "ini", "yaml", "yml", "toml", "xml", "html", "diff",
                        "patch", "pac", "list", "rules"})
SVG_RE = re.compile(
    rb"^(?:\xef\xbb\xbf)?\s*(?:<\?xml[^>]*>\s*)?(?:<!--.*?-->\s*)*(?:<!DOCTYPE[^>]*>\s*)?<svg[\s>]",
    re.S | re.I)


class Fetched:
    def __init__(self, data: bytes | None = None, *, status: int = 200, reason: str = "",
                 retry_after: float | None = None, final_url: str = "") -> None:
        self.data = data
        self.status = status
        self.reason = reason
        self.retry_after = retry_after
        self.final_url = final_url

    @property
    def rate_limited(self) -> bool:
        return self.status == 429 or (self.status == 403 and self.retry_after is not None)


def trim(url: str) -> str:
    while url and url[-1] in TRAILING:
        url = url[:-1]
    return url


def canonical(url: str) -> str:
    return str(url or "").strip().replace("&amp;", "&")


def find_urls(text: Any) -> list[str]:
    if not isinstance(text, str) or "https://" not in text:
        return []
    found = []
    for match in SOURCE_RE.finditer(text):
        url = trim(canonical(match.group(0)))
        if len(url) <= MAX_URL and url not in found:
            found.append(url)
    return found


def record_urls(record: Any) -> list[str]:
    found: list[str] = []
    events = record.get("events") if isinstance(record, dict) else None
    for event in events if isinstance(events, list) else []:
        if not isinstance(event, dict):
            continue
        for name in ("title", "body"):
            for url in find_urls(event.get(name)):
                if url not in found:
                    found.append(url)
    return found


def allowed(url: str) -> bool:
    try:
        parts = urlsplit(str(url or ""))
        port = parts.port
    except ValueError:
        return False
    if parts.scheme != "https" or port is not None or parts.username or parts.password \
            or len(url) > MAX_URL:
        return False
    if parts.hostname == "github.com":
        return bool(GITHUB_PATH_RE.match(parts.path))
    return parts.hostname in CONTENT_HOSTS


def _jwt_live(token: str, now_ms: int) -> bool:
    parts = token.split(".")
    if len(parts) != 3:
        return False
    try:
        payload = json.loads(base64.urlsafe_b64decode(parts[1] + "=" * (-len(parts[1]) % 4)))
        expires = int(payload.get("exp"))
    except (ValueError, TypeError, AttributeError):
        return False
    return expires * 1000 > now_ms + 60_000


def candidates(url: str, now_ms: int) -> list[str]:
    parts = urlsplit(url)
    if parts.hostname == "private-user-images.githubusercontent.com":
        match = PRIVATE_PATH_RE.match(parts.path)
        token = (parse_qs(parts.query).get("jwt") or [""])[0]
        found = [url] if token and _jwt_live(token, now_ms) else []
        if match:
            found.append("https://user-images.githubusercontent.com/%s/%s" % (
                match.group(1), match.group(2)))
            found.append("https://github.com/user-attachments/assets/" + match.group(3).lower())
        return found
    if parts.hostname == "objects.githubusercontent.com":
        match = OBJECT_PATH_RE.match(parts.path)
        disposition = (parse_qs(parts.query).get("response-content-disposition") or [""])[0]
        name = unquote(disposition.split("filename=", 1)[1]).strip('"') \
            if "filename=" in disposition else ""
        if match and FILE_NAME_RE.match(name):
            return ["https://github.com/user-attachments/files/%s/%s" % (
                match.group(1), name.replace(" ", "%20"))]
        return []
    return [url]


def _name_extension(url: str) -> str:
    path = urlsplit(url).path
    name = unquote(path.rsplit("/", 1)[-1])
    return name.rsplit(".", 1)[1].lower() if "." in name else ""


def sniff(data: bytes, url: str = "") -> tuple[str, str] | None:
    if not data:
        return None
    head = data[:16]
    if head.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png", "png"
    if head.startswith(b"\xff\xd8\xff"):
        return "image/jpeg", "jpg"
    if head.startswith((b"GIF87a", b"GIF89a")):
        return "image/gif", "gif"
    if head.startswith(b"RIFF") and data[8:12] == b"WEBP":
        return "image/webp", "webp"
    if head.startswith(b"%PDF-"):
        return "application/pdf", "pdf"
    if head.startswith((b"PK\x03\x04", b"PK\x05\x06")):
        return "application/zip", "zip"
    if head.startswith(b"\x1f\x8b"):
        return "application/gzip", "gz"
    if head.startswith(b"7z\xbc\xaf\x27\x1c"):
        return "application/x-7z-compressed", "7z"
    if head.startswith(b"\xfd7zXZ\x00"):
        return "application/x-xz", "xz"
    if head.startswith(b"BZh"):
        return "application/x-bzip2", "bz2"
    if data[4:8] == b"ftyp":
        return ("video/quicktime", "mov") if data[8:12] == b"qt  " else ("video/mp4", "mp4")
    if head.startswith(b"\x1a\x45\xdf\xa3"):
        return "video/webm", "webm"
    if head[:4] in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1",
                    b"\xa1\xb2\x3c\x4d"):
        return "application/vnd.tcpdump.pcap", "pcap"
    if head.startswith(b"\x0a\x0d\x0d\x0a"):
        return "application/x-pcapng", "pcapng"
    if SVG_RE.match(data[:4096]):
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            return None
        return "image/svg+xml", "svg"
    if _name_extension(url) in TEXT_NAMES and b"\x00" not in data:
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            return None
        return "text/plain", "txt"
    return None


def _retry_after(headers: Any) -> float | None:
    value = headers.get("Retry-After") if headers is not None else None
    if value is None and headers is not None and headers.get("X-RateLimit-Remaining") == "0":
        value = "60"
    try:
        return max(0.0, float(value)) if value is not None else None
    except (TypeError, ValueError):
        return 60.0


def fetch(url: str) -> Fetched:
    if not allowed(url):
        return Fetched(status=0, reason="refused")

    class Redirects(HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            if not allowed(newurl):
                raise HTTPError(newurl, 310, "redirect off the allowlist", headers, fp)
            return super().redirect_request(req, fp, code, msg, headers, newurl)

    request = Request(url, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
    try:
        with build_opener(Redirects, *HANDLERS).open(request, timeout=TIMEOUT_SECONDS) as response:
            final = response.geturl()
            if not allowed(final):
                return Fetched(status=0, reason="redirect")
            length = response.headers.get("Content-Length")
            if length and length.isdigit() and int(length) > MAX_BYTES:
                return Fetched(status=200, reason="too-large", final_url=final)
            data = response.read(MAX_BYTES + 1)
    except HTTPError as error:
        if error.code == 310:
            return Fetched(status=0, reason="redirect")
        return Fetched(status=error.code, reason="http-%d" % error.code,
                       retry_after=_retry_after(error.headers))
    except (URLError, OSError, ValueError):
        return Fetched(status=0, reason="network")
    if len(data) > MAX_BYTES:
        return Fetched(status=200, reason="too-large", final_url=final)
    return Fetched(data, final_url=final)


def load_index(checkout: Path) -> dict[str, Any]:
    path = Path(checkout) / INDEX_REL
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        doc = {}
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        doc = {}
    files = doc.get("files") if isinstance(doc.get("files"), dict) else {}
    unavailable = doc.get("unavailable") if isinstance(doc.get("unavailable"), dict) else {}
    return {"schema": SCHEMA,
            "files": {url: entry for url, entry in files.items() if valid_entry(entry)},
            "unavailable": {url: entry for url, entry in unavailable.items()
                            if isinstance(entry, dict)}}


def valid_entry(entry: Any) -> bool:
    return (isinstance(entry, dict) and isinstance(entry.get("path"), str)
            and bool(STORED_PATH_RE.match(entry["path"]))
            and isinstance(entry.get("type"), str)
            and isinstance(entry.get("bytes"), int) and not isinstance(entry.get("bytes"), bool))


def _dump(doc: Any) -> str:
    return json.dumps(doc, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _write_if_changed(path: Path, data: str | bytes) -> bool:
    raw = data.encode("utf-8") if isinstance(data, str) else data
    if path.is_file() and path.read_bytes() == raw:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(raw)
    temp.replace(path)
    return True


def store(checkout: Path, data: bytes, kind: tuple[str, str]) -> dict[str, Any]:
    digest = hashlib.sha256(data).hexdigest()
    rel = "%s/%s.%s" % (DIR_REL, digest, kind[1])
    target = Path(checkout) / rel
    if not (target.is_file() and target.stat().st_size == len(data)):
        _write_if_changed(target, data)
    return {"path": rel, "type": kind[0], "bytes": len(data)}


def _due(entry: dict[str, Any], now: int) -> bool:
    try:
        return int(entry.get("nextTry") or 0) <= now
    except (TypeError, ValueError):
        return True


def _size(entry: Any) -> int:
    try:
        return max(0, int(entry.get("bytes") or 0)) if isinstance(entry, dict) else 0
    except (TypeError, ValueError):
        return 0


def _unavailable(previous: Any, reason: str, now: int) -> dict[str, Any]:
    try:
        tries = int(previous.get("tries") or 0) + 1 if isinstance(previous, dict) else 1
    except (TypeError, ValueError):
        tries = 1
    permanent = reason in ("too-large", "type", "no-source")
    wait = RETRY_MAX_MS if permanent else min(RETRY_MAX_MS, RETRY_MS * 2 ** min(tries - 1, 5))
    return {"reason": reason, "tries": tries, "nextTry": now + wait}


class Pass:
    def __init__(self, checkout: Path, *, fetcher: Callable[[str], Fetched] = fetch,
                 sleep: Callable[[float], None] = time.sleep,
                 monotonic: Callable[[], float] = time.monotonic,
                 max_files: int = PASS_FILES, max_bytes: int = PASS_BYTES,
                 max_seconds: float = PASS_SECONDS, pace: float = PACE_SECONDS,
                 max_total: int = TOTAL_BYTES) -> None:
        self.checkout = Path(checkout)
        self.max_total = max_total
        self.stored_total = 0
        self.fetcher = fetcher
        self.sleep = sleep
        self.monotonic = monotonic
        self.max_files = max_files
        self.max_bytes = max_bytes
        self.max_seconds = max_seconds
        self.pace = pace
        self.summary = {"fetched": 0, "stored": 0, "unavailable": 0, "bytes": 0,
                        "pending": 0, "stopped": ""}
        self.requests = 0
        self.spent = 0
        self.started = 0.0

    def out_of_budget(self) -> str:
        if self.requests >= self.max_files:
            return "files"
        if self.spent >= self.max_bytes:
            return "bytes"
        if self.stored_total >= self.max_total:
            return "total"
        if self.monotonic() - self.started >= self.max_seconds:
            return "time"
        return ""

    def get(self, url: str) -> Fetched | None:
        for attempt in range(2):
            if self.requests:
                self.sleep(self.pace)
            self.requests += 1
            answer = self.fetcher(url)
            if not answer.rate_limited:
                return answer
            wait = answer.retry_after if answer.retry_after is not None else 60.0
            if attempt or wait > RATE_WAIT_MAX_SECONDS:
                self.summary["stopped"] = "rate-limited"
                return None
            self.sleep(wait)
        return None

    def download(self, url: str, now: int) -> tuple[dict[str, Any] | None, str]:
        reason = "no-source"
        for source in candidates(url, now):
            if not allowed(source):
                reason = "refused"
                continue
            answer = self.get(source)
            if answer is None:
                return None, ""
            self.summary["fetched"] += 1
            if answer.data is None:
                reason = answer.reason or "unavailable"
                continue
            self.spent += len(answer.data)
            kind = sniff(answer.data, source)
            if kind is None:
                reason = "type"
                continue
            return store(self.checkout, answer.data, kind), ""
        return None, reason

    def run(self, records: dict[int, Any], now: int) -> dict[str, Any]:
        self.started = self.monotonic()
        index = load_index(self.checkout)
        files, unavailable = index["files"], index["unavailable"]
        self.stored_total = sum(_size(entry) for entry in files.values())
        by_issue = {int(number): record_urls(record) for number, record in records.items()}
        wanted: list[str] = []
        for number in sorted(by_issue, reverse=True):
            for url in by_issue[number]:
                if url not in files and url not in wanted:
                    wanted.append(url)
        for url in wanted:
            if url in unavailable and not _due(unavailable[url], now):
                continue
            if self.summary["stopped"] or self.out_of_budget():
                self.summary["stopped"] = self.summary["stopped"] or self.out_of_budget()
                self.summary["pending"] += 1
                continue
            entry, reason = self.download(url, now)
            if entry is not None:
                files[url] = entry
                self.stored_total += _size(entry)
                unavailable.pop(url, None)
                self.summary["stored"] += 1
                self.summary["bytes"] += entry["bytes"]
            elif reason:
                unavailable[url] = _unavailable(unavailable.get(url), reason, now)
                self.summary["unavailable"] += 1
            else:
                self.summary["pending"] += 1
        if files or unavailable or (self.checkout / INDEX_REL).is_file():
            _write_if_changed(self.checkout / INDEX_REL, _dump(
                {"schema": SCHEMA, "files": files, "unavailable": unavailable}))
        for number in sorted(by_issue):
            mapped = {url: files[url] for url in by_issue[number] if url in files}
            path = self.checkout / ISSUES_REL / ("%d.json" % number)
            if mapped or path.is_file():
                _write_if_changed(path, _dump({"schema": ISSUE_SCHEMA, "files": mapped}))
        return self.summary


def sync(checkout: Path, records: dict[int, Any], now: int, **options: Any) -> dict[str, Any]:
    return Pass(checkout, **options).run(records, now)


def issue_urls(records: Iterable[Any]) -> list[str]:
    found: list[str] = []
    for record in records:
        for url in record_urls(record):
            if url not in found:
                found.append(url)
    return found
