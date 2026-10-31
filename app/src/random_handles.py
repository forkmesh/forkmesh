import json
import re
import secrets
from pathlib import Path

WORDS_FILE = Path(__file__).resolve().with_name("handle_words.json")
SUFFIX_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"
SUFFIX_LENGTH = 4
MAX_ATTEMPTS = 24
HANDLE_RE = re.compile(r"^([a-z]+)-([a-z]+)-([a-z0-9]{4})$")
_words = {}


def words():
    if not _words:
        data = json.loads(WORDS_FILE.read_text(encoding="utf-8"))
        _words["adjectives"] = tuple(sorted(set(data["adjectives"])))
        _words["animals"] = tuple(sorted(set(data["animals"])))
    return _words["adjectives"], _words["animals"]


def suffix(rng=secrets):
    return "".join(rng.choice(SUFFIX_ALPHABET) for _ in range(SUFFIX_LENGTH))


def generate(rng=secrets):
    adjectives, animals = words()
    return "%s-%s-%s" % (rng.choice(adjectives), rng.choice(animals), suffix(rng))


def suggest(reject=None, rng=secrets, attempts=MAX_ATTEMPTS):
    for _ in range(max(1, attempts)):
        handle = generate(rng)
        if reject is None or not reject(handle):
            return handle
    return ""


def is_random_handle(value):
    match = HANDLE_RE.fullmatch(str(value or "").strip().lower())
    if not match:
        return False
    adjectives, animals = words()
    if match.group(1) not in adjectives or match.group(2) not in animals:
        return False
    return all(ch in SUFFIX_ALPHABET for ch in match.group(3))


def combinations():
    adjectives, animals = words()
    return len(adjectives) * len(animals) * len(SUFFIX_ALPHABET) ** SUFFIX_LENGTH
