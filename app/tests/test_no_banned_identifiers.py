import hashlib
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

BANNED_SHA256 = frozenset({
    "230c8bf08fe52694cc370a2a128009933a7c3327dc1ed56578001b334788500a",
    "4bf55a9ce713ee0c1ca0a4d517cc5a5c3b83f7dd4dc5f9009b19b3346191e8cd",
})
BANNED_LENGTHS = (6, 9)
WORD = re.compile(r"[a-z0-9]+")
MAX_BYTES = 4 * 1024 * 1024


def _banned(token: str) -> bool:
    for length in BANNED_LENGTHS:
        for start in range(len(token) - length + 1):
            if hashlib.sha256(token[start:start + length].encode()).hexdigest() in BANNED_SHA256:
                return True
    return False


def _hits(text: str, seen: dict) -> bool:
    for token in set(WORD.findall(text.lower())):
        if token not in seen:
            seen[token] = _banned(token)
        if seen[token]:
            return True
    return False


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args], capture_output=True,
                          text=True, errors="replace", check=True).stdout


def test_no_tracked_file_names_a_banned_identifier():
    seen: dict = {}
    offenders = []
    for name in _git("ls-files", "-z").split("\0"):
        if not name:
            continue
        path = REPO / name
        if _hits(name, seen):
            offenders.append(name)
            continue
        try:
            if not path.is_file() or path.is_symlink() or path.stat().st_size > MAX_BYTES:
                continue
            data = path.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        if _hits(data.decode("utf-8", "replace"), seen):
            offenders.append(name)
    assert not offenders, "banned identifier in: " + ", ".join(offenders[:20])


def test_no_commit_identity_names_a_banned_identifier():
    seen: dict = {}
    offenders = [line.split("\x1f", 1)[0]
                 for line in _git("log", "--format=%h\x1f%an %ae %cn %ce", "HEAD").splitlines()
                 if _hits(line.split("\x1f", 1)[-1], seen)]
    assert not offenders, "banned identifier in commit identities: " + ", ".join(offenders[:20])
