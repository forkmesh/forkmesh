#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import html
import io
import json
import re
import sys
import time
import zipfile
from pathlib import Path
from typing import Any, Callable, Iterable
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_safe_delete  # noqa: E402

SCHEMA = "forkmesh-machine-translations-issue-v1"
DIR_REL = ".forkmesh/translations/issues"
ISSUES_REL = ".forkmesh/issues"
TARGET = "en"
PASS_SECONDS = 240
MAX_POST_CHARS = 20000
MAX_SENTENCE_CHARS = 600
ARGOS_INDEX = "https://raw.githubusercontent.com/argosopentech/argospm-index/main/index.json"
USER_AGENT = "forkmesh-forge-translate/1"
DETECT_CHARS = 2000
MAX_MODEL_MB = 120
MAX_DOWNLOAD_BYTES = 512 * 1024 * 1024
MAX_UNPACKED_BYTES = 1024 * 1024 * 1024
VERSION_RE = re.compile(r"-(\d+_\d+)\.argosmodel$")


WORDS = {
    "en": "the and is are was of to in that it for this with not on be you have but can from they will "
          "what about there a an as at by or if has we so do which would their been more some all just "
          "also only than how who",
    "es": "el la los las de que y en por para con una es no se del como pero más este esta también",
    "fr": "le la les de des et est une que pour dans pas sur avec il ce qui du ne sont mais aussi",
    "de": "der die das und ist nicht ein eine zu mit von den auf für ich es sich auch dass wird",
    "pt": "o os de que e do da em um uma para com não por mais se é dos das mas também",
    "it": "il la di che e è un una per non con del della sono si anche gli più come",
    "tr": "ve bir bu için ile de da çok ne olarak değil gibi ama daha var ben",
    "id": "yang dan di ini itu dengan untuk tidak ada dari saya akan juga bisa atau",
    "nl": "de het een en van is dat niet op te met voor zijn ook maar",
    "pl": "i w nie na się z że do to jest jak co ale czy przez",
    "tk": "we bilen üçin bu hem däl bolsa ýa-da ýaly bar",
}
WORD_SETS = {code: frozenset(words.split()) for code, words in WORDS.items()}
LETTERS = {"de": "[ß]", "es": "[ñ¿¡]", "pt": "[ãõ]", "tr": "[ğı]", "pl": "[ąęłśźżń]", "tk": "[ňýž]"}
NOISE_RE = re.compile(r"(?:https?://|www\.)\S+|[@#][\w.-]+|\S*[/_.]\S*[/_.]\S*|\S*\d\S*", re.I)
WORD_RE = re.compile(r"[^\W\d_][^\W\d_'’-]*")


def _prose(text: str) -> str:
    return NOISE_RE.sub(" ", str(text or "")[:DETECT_CHARS])


def _count(pattern: str, text: str, flags: int = 0) -> int:
    return len(re.findall(pattern, text, flags))


def script_guess(text: str) -> str:
    sample = _prose(text)
    letters = sum(1 for ch in sample if ch.isalpha())
    if not letters:
        return ""
    share = lambda value: value / letters  # noqa: E731
    kana = _count(r"[\u3040-\u30ff]", sample)
    han = _count(r"[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff]", sample)
    hangul = _count(r"[\uac00-\ud7af\u1100-\u11ff]", sample)
    arabic = _count(r"[\u0600-\u06ff\u0750-\u077f\ufb50-\ufdff\ufe70-\ufeff]", sample)
    cyrillic = _count(r"[\u0400-\u04ff]", sample)
    if share(kana) > 0.1:
        return "ja"
    if share(hangul) > 0.3:
        return "ko"
    if share(han) > 0.3 or (han >= 12 and share(han) > 0.12):
        return "zh"
    if share(arabic) > 0.5:
        persian = _count("[پچژگکی]", sample)
        arabic_only = _count("[ةيىأإ]", sample)
        if persian >= 2 and persian > arabic_only:
            return "fa"
        if arabic_only >= 2 and persian == 0:
            return "ar"
        return ""
    if share(cyrillic) > 0.5:
        if _count("[әғқңөұүһ]", sample, re.I) >= 2:
            return "kk"
        ukrainian = _count("[іїєґ]", sample, re.I)
        russian = _count("[ыэъё]", sample, re.I)
        if ukrainian >= 2 and ukrainian > russian:
            return "uk"
        return "" if ukrainian and not russian else "ru"
    if _count("[ăđơư]", sample, re.I) >= 2 and _count(
            "[ạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ]", sample, re.I) >= 2:
        return "vi"
    for code, pattern in (("th", r"[\u0e00-\u0e7f]"), ("he", r"[\u0590-\u05ff]"),
                          ("el", r"[\u0370-\u03ff]"), ("my", r"[\u1000-\u109f]"),
                          ("hy", r"[\u0530-\u058f]"), ("ka", r"[\u10a0-\u10ff]")):
        if share(_count(pattern, sample)) > 0.5:
            return code
    return ""


def word_guess(text: str) -> str:
    sample = _prose(text)
    words = WORD_RE.findall(sample.lower())
    if len(words) < 4:
        return ""
    scores = []
    for code, known in WORD_SETS.items():
        hits = sum(1 for word in words if word in known)
        if code in LETTERS:
            hits += min(3, _count(LETTERS[code], sample))
        scores.append((hits / len(words), hits, code))
    scores.sort(reverse=True)
    (best, hits, code), (second, _h, _c) = scores[0], scores[1]
    if best < 0.06 or hits < 2:
        return ""
    return code if best >= second * 1.6 else ""


def guess(text: str) -> str:
    return script_guess(text) or word_guess(text)


FENCE_RE = re.compile(r"^\s{0,3}(`{3,}|~{3,})")
PREFIX_RE = re.compile(r"^(\s*(?:>\s*)*(?:#{1,6}\s+|[-*+]\s+(?:\[[ xX]\]\s+)?|\d{1,9}[.)]\s+)?)")
KEEP_RE = re.compile(
    r"`+[^`]*`+"
    r"|!\[[^\]]*\]\([^)]*\)"
    r"|\]\([^)]*\)"
    r"|<[^>]+>"
    r"|(?:https?://|www\.)[^\s<>()]+"
    r"|[@#][\w.-]+|\b[\w.-]+#\d+"
    r"|\*\*|__|~~|[\[\]*_|]"
    r"|&\w+;|&#\d+;")
SENTENCE_RE = re.compile(r"(?<=[.!?。！？؟])\s+|(?<=[。！？])")


def _has_letters(text: str) -> bool:
    return any(ch.isalpha() for ch in text)


def split_sentences(text: str) -> list[str]:
    parts = [part for part in SENTENCE_RE.split(text) if part and part.strip()]
    out: list[str] = []
    for part in parts:
        while len(part) > MAX_SENTENCE_CHARS:
            cut = part.rfind(" ", 0, MAX_SENTENCE_CHARS)
            cut = cut if cut > MAX_SENTENCE_CHARS // 2 else MAX_SENTENCE_CHARS
            out.append(part[:cut])
            part = part[cut:]
        out.append(part)
    return out


def translate_markdown(text: str, translate: Callable[[list[str]], list[str]],
                       keep: Callable[[str], bool] | None = None) -> str:
    lines = str(text or "").replace("\r\n", "\n").split("\n")
    plan: list[Any] = []
    batch: list[str] = []
    fence = ""
    for line in lines:
        opener = FENCE_RE.match(line)
        if fence:
            plan.append(line)
            if opener and opener.group(1)[0] == fence[0] and len(opener.group(1)) >= len(fence):
                fence = ""
            continue
        if opener:
            fence = opener.group(1)
            plan.append(line)
            continue
        if not _has_letters(line) or line.startswith(("    ", "\t")) or re.match(r"^\s*\|.*\|\s*$", line) \
                or (keep is not None and keep(line)):
            plan.append(line)
            continue
        prefix = PREFIX_RE.match(line).group(1)
        rest = line[len(prefix):]
        pieces: list[Any] = []
        cursor = 0
        for match in KEEP_RE.finditer(rest):
            if match.start() > cursor:
                pieces.append(rest[cursor:match.start()])
            pieces.append((match.group(0),))
            cursor = match.end()
        if cursor < len(rest):
            pieces.append(rest[cursor:])
        planned: list[Any] = []
        for piece in pieces:
            if isinstance(piece, tuple) or not _has_letters(piece):
                planned.append(piece if isinstance(piece, tuple) else (piece,))
                continue
            lead = piece[:len(piece) - len(piece.lstrip())]
            trail = piece[len(piece.rstrip()):]
            sentences = split_sentences(piece.strip())
            planned.append((lead, len(batch), len(sentences), trail))
            batch.extend(sentences)
        plan.append((prefix, planned))
    done = translate(batch) if batch else []
    if len(done) != len(batch):
        raise RuntimeError("translator returned %d of %d sentences" % (len(done), len(batch)))
    out = []
    for entry in plan:
        if isinstance(entry, str):
            out.append(entry)
            continue
        prefix, planned = entry
        text_out = prefix
        for piece in planned:
            if len(piece) == 1:
                text_out += piece[0]
            else:
                lead, start, count, trail = piece
                joiner = " " if TARGET not in ("zh", "ja") else ""
                text_out += lead + joiner.join(s.strip() for s in done[start:start + count]) + trail
        out.append(text_out)
    return "\n".join(out)


def _numeric_dirs(root: Path) -> dict[int, Path]:
    if not root.is_dir():
        return {}
    return {int(entry.name): entry for entry in root.iterdir()
            if entry.is_dir() and entry.name.isdigit()}


def load_records(checkout: Path) -> dict[int, dict[str, Any]]:
    root = Path(checkout) / ISSUES_REL
    records: dict[int, dict[str, Any]] = {}
    for side in (root, root / "open", root / "closed"):
        for number, directory in _numeric_dirs(side).items():
            try:
                record = json.loads((directory / ("issue-%d.json" % number)).read_text("utf-8"))
            except (FileNotFoundError, ValueError):
                continue
            if isinstance(record, dict):
                records[number] = record
    return records


def posts(record: dict[str, Any]) -> list[tuple[str, str]]:
    events = [event for event in record.get("events") or [] if isinstance(event, dict)]
    text: dict[str, str] = {}
    order: list[str] = []
    title = str(record.get("title") or "")
    opened = ""
    for event in events:
        kind = event.get("type")
        if kind == "open":
            text["body"] = str(event.get("body") or "")
            order.append("body")
            title = str(event.get("title") or title)
            opened = str(event.get("id") or "")
        elif kind == "comment" and event.get("id"):
            key = str(event["id"])
            text[key] = str(event.get("body") or "")
            order.append(key)
        elif kind == "edit":
            target = str(event.get("target") or "")
            key = "body" if target in ("self", opened) else target
            if key in text and "body" in event:
                text[key] = str(event.get("body") or "")
            if key == "body" and event.get("title"):
                title = str(event["title"])
        elif kind == "delete":
            target = str(event.get("target") or "")
            if target and target != "self":
                text.pop(target, None)
    found = [("title", title)] if title else []
    found += [(key, text[key]) for key in order if key in text]
    return found


def digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


LATIN_SOURCES = frozenset(WORDS)
MIN_FOREIGN_CHARS = 20


def prose_lines(text: str) -> list[str]:
    out, fence = [], ""
    for line in str(text or "").replace("\r\n", "\n").split("\n"):
        opener = FENCE_RE.match(line)
        if fence:
            if opener and opener.group(1)[0] == fence[0] and len(opener.group(1)) >= len(fence):
                fence = ""
            continue
        if opener:
            fence = opener.group(1)
            continue
        if _has_letters(line) and not line.startswith(("    ", "\t")):
            out.append(line)
    return out


def line_language(line: str) -> str:
    return script_guess(line) or word_guess(line)


def foreign_line(line: str, source: str, target: str = TARGET) -> bool:
    found = script_guess(line)
    if found:
        return found != target
    if source in LATIN_SOURCES:
        return word_guess(line) != target
    return False


def needs(text: str, target: str = TARGET) -> str:
    if not text.strip() or len(text) > MAX_POST_CHARS:
        return ""
    whole = guess(text)
    mixed = any(word_guess(text[start:start + DETECT_CHARS]) == target
                for start in range(0, len(text), DETECT_CHARS))
    if whole and whole != target and not mixed:
        return whole
    counts: dict[str, int] = {}
    for line in prose_lines(text):
        found = line_language(line)
        if found and found != target:
            counts[found] = counts.get(found, 0) + len(line)
    if not counts or sum(counts.values()) < MIN_FOREIGN_CHARS:
        return ""
    return max(sorted(counts), key=counts.get)


class ArgosEngine:

    def __init__(self, models: Path, threads: int = 1) -> None:
        self.models = Path(models)
        self.threads = threads
        self.loaded: dict[str, Any] = {}

    def pairs(self) -> set[str]:
        return {entry.name for entry in self.models.iterdir()
                if entry.is_dir() and (entry / "model").is_dir()} if self.models.is_dir() else set()

    def _load(self, pair: str) -> Any:
        if pair not in self.loaded:
            import ctranslate2  # noqa: PLC0415 - only on the server
            import sentencepiece  # noqa: PLC0415
            root = self.models / pair
            spm = next(iter(sorted(root.glob("*.model"))), None) or root / "sentencepiece.model"
            self.loaded.clear()
            self.loaded[pair] = (
                ctranslate2.Translator(str(root / "model"), device="cpu", compute_type="int8",
                                       inter_threads=1, intra_threads=self.threads),
                sentencepiece.SentencePieceProcessor(model_file=str(spm)))
        return self.loaded[pair]

    def translate(self, source: str, target: str, sentences: list[str]) -> list[str]:
        translator, spm = self._load("%s-%s" % (source, target))
        tokens = [spm.encode(sentence, out_type=str) for sentence in sentences]
        results = translator.translate_batch(tokens, beam_size=2, max_batch_size=16,
                                             max_decoding_length=256)
        return [html.unescape(spm.decode(result.hypotheses[0]).replace("\u2581", " ")).strip() for result in results]


def _dump(doc: Any) -> str:
    return json.dumps(doc, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _write_if_changed(path: Path, data: str) -> bool:
    raw = data.encode("utf-8")
    if path.is_file() and path.read_bytes() == raw:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(raw)
    temp.replace(path)
    return True


def sync(checkout: Path, engine: Any, seconds: float = PASS_SECONDS, target: str = TARGET,
         clock: Callable[[], float] = time.monotonic) -> dict[str, Any]:
    checkout = Path(checkout)
    pairs = engine.pairs()
    deadline = clock() + seconds
    summary = {"translated": 0, "kept": 0, "removed": 0, "skipped": {}, "pending": 0, "files": 0}
    records = load_records(checkout)
    for number in sorted(records, reverse=True):
        path = checkout / DIR_REL / ("%d.json" % number)
        try:
            old = json.loads(path.read_text("utf-8")).get("posts") or {}
        except (FileNotFoundError, ValueError, AttributeError):
            old = {}
        fresh: dict[str, Any] = {}
        for key, text in posts(records[number]):
            source = needs(text, target)
            if not source:
                continue
            pair = "%s-%s" % (source, target)
            if pair not in pairs:
                summary["skipped"][source] = summary["skipped"].get(source, 0) + 1
                continue
            hashed = digest(text)
            kept = old.get(key)
            if isinstance(kept, dict) and kept.get("hash") == hashed and kept.get("source") == source:
                fresh[key] = kept
                summary["kept"] += 1
                continue
            if clock() >= deadline:
                summary["pending"] += 1
                if isinstance(kept, dict):
                    fresh[key] = kept
                continue
            try:
                done = translate_markdown(text, lambda batch: engine.translate(source, target, batch),
                                          keep=lambda line: not foreign_line(line, source, target))
            except Exception as error:
                print("translate #%d %s: %r" % (number, key, error), file=sys.stderr)
                continue
            fresh[key] = {"source": source, "hash": hashed, "model": pair, "text": done}
            summary["translated"] += 1
        summary["removed"] += len(set(old) - set(fresh))
        if fresh:
            if _write_if_changed(path, _dump({"schema": SCHEMA, "target": target, "posts": fresh})):
                summary["files"] += 1
        elif path.is_file():
            path.unlink()
            summary["files"] += 1
    return summary


def _fetch(url: str) -> bytes:
    if not str(url).startswith("https://"):
        raise ValueError("only https downloads: %r" % url)
    with urlopen(Request(url, headers={"User-Agent": USER_AGENT}), timeout=120) as response:
        if not str(response.geturl()).startswith("https://"):
            raise ValueError("redirected off https: %r" % url)
        data = response.read(MAX_DOWNLOAD_BYTES + 1)
    if len(data) > MAX_DOWNLOAD_BYTES:
        raise ValueError("download over %d bytes: %r" % (MAX_DOWNLOAD_BYTES, url))
    return data


def _unpack(data: bytes, staging: Path) -> bool:
    archive = zipfile.ZipFile(io.BytesIO(data))
    if sum(member.file_size for member in archive.infolist()) > MAX_UNPACKED_BYTES:
        return False
    for member in archive.infolist():
        parts = Path(member.filename).parts[1:]
        if not parts or ".." in parts or member.is_dir():
            continue
        if parts[0] not in ("model", "sentencepiece.model", "metadata.json", "README.md"):
            continue
        out = staging.joinpath(*parts)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(archive.read(member))
    return (staging / "model").is_dir()


def _model_mb(staging: Path) -> float:
    return sum(path.stat().st_size for path in (staging / "model").rglob("*") if path.is_file()) / 2 ** 20


def _discard(path: Path) -> None:
    if path.exists():
        forkmesh_safe_delete.remove_tree(path, path.parent)


def install_models(directory: Path, languages: Iterable[str], target: str = TARGET,
                   fetch: Callable[[str], bytes] = _fetch, max_mb: float = MAX_MODEL_MB) -> list[str]:
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    index = json.loads(fetch(ARGOS_INDEX).decode("utf-8"))
    installed = []
    for language in languages:
        pair = "%s-%s" % (language, target)
        if (directory / pair / "model").is_dir():
            installed.append(pair)
            continue
        package = next((entry for entry in index
                        if entry.get("from_code") == language and entry.get("to_code") == target), None)
        if not package or not package.get("links"):
            print("no Argos model for %s" % pair, file=sys.stderr)
            continue
        link = package["links"][0]
        staging = directory / (pair + ".tmp")
        _discard(staging)
        if not _unpack(fetch(link), staging):
            print("%s: the package has no CTranslate2 model" % pair, file=sys.stderr)
            continue
        older = VERSION_RE.sub("-1_0.argosmodel", link)
        if _model_mb(staging) > max_mb and older != link:
            small = directory / (pair + ".small")
            _discard(small)
            try:
                if _unpack(fetch(older), small) and _model_mb(small) < _model_mb(staging):
                    staging, small = small, staging
            except Exception as error:
                print("%s: no smaller model (%r)" % (pair, error), file=sys.stderr)
            _discard(small)
        staging.replace(directory / pair)
        installed.append(pair)
    return installed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Machine translations of a mirror's issue threads")
    sub = parser.add_subparsers(dest="command", required=True)
    models = sub.add_parser("models", help="download Argos models into a directory")
    models.add_argument("--dir", required=True, type=Path)
    models.add_argument("--languages", required=True)
    models.add_argument("--max-mb", type=float, default=MAX_MODEL_MB)
    run = sub.add_parser("sync", help="translate what is new or changed")
    run.add_argument("--checkout", required=True, type=Path)
    run.add_argument("--models", required=True, type=Path)
    run.add_argument("--seconds", type=float, default=PASS_SECONDS)
    run.add_argument("--threads", type=int, default=1)
    args = parser.parse_args(argv)
    if args.command == "models":
        done = install_models(args.dir, [code.strip() for code in args.languages.split(",") if code.strip()],
                              max_mb=args.max_mb)
        print(" ".join(done))
        return 0
    summary = sync(args.checkout, ArgosEngine(args.models, args.threads), args.seconds)
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
