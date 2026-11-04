#!/usr/bin/env python3

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCES = sorted(
    [*REPO.glob("desktop/src/*.cpp"),
     *REPO.glob("marketplace/packages/*/content/desktop/**/*.cpp")])
TOKEN_SOURCE = re.compile(
    r"\b(\w+)\s*=\s*[^;]*\b(?:accountSessionToken|accountSessionTokenForUrl|"
    r"m_accountSessionToken)\s*\(")
SIGN_IN = re.compile(r"[Ss]ign in")
FALLBACK = re.compile(
    r"signProof|signOrgTaskProof|signData|requestAccountSession|"
    r"ensureAccountSession|waitForAccountSession|accountSignedIn|"
    r"hasActiveAccountSession|canSign")


def _offenders(path):
    lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    out = []
    for index, line in enumerate(lines):
        match = TOKEN_SOURCE.search(
            line + " " + (lines[index + 1] if index + 1 < len(lines) else ""))
        if not match:
            continue
        start = next((back for back in range(index, -1, -1)
                      if lines[back] == "{"), 0)
        name = re.escape(match.group(1))
        empty = re.compile(r"\bif\s*\(.*\b" + name + r"(?:\.trimmed\(\))?\.isEmpty\(\)")
        for offset in range(index, min(index + 30, len(lines))):
            if not empty.search(lines[offset]):
                continue
            before = "\n".join(lines[start:offset])
            block = "\n".join(lines[offset:offset + 14])
            hit = "%s:%d" % (path.relative_to(REPO), offset + 1)
            if (SIGN_IN.search(block) and not FALLBACK.search(before + block)
                    and hit not in out):
                out.append(hit)
            break
    return out


def test_an_empty_session_token_never_means_signed_out_on_its_own():
    offenders = [hit for path in SOURCES if "/tests/" not in str(path)
                 for hit in _offenders(path)]
    assert offenders == [], (
        "A desktop signed in by its key has no session token at first. Check "
        "accountSignedIn()/hasActiveAccountSession() for the Sign in state and "
        "get the session with requestAccountSession/waitForAccountSession, or "
        "sign the request (docs/desktop/package-modules.md#checking-sign-in): "
        + ", ".join(offenders))


def test_the_guard_catches_the_old_spreadsheets_check(tmp_path):
    sample = tmp_path / "Page.cpp"
    sample.write_text(
        "const QString token =\n"
        "    m_host.accountSessionToken ? m_host.accountSessionToken(page) : QString();\n"
        "if (account.isEmpty() || token.isEmpty()) {\n"
        "    showMessage(QStringLiteral(\"Sign in to use Spreadsheets\"));\n"
        "}\n")
    global REPO
    saved, REPO = REPO, tmp_path
    try:
        assert _offenders(sample) == ["Page.cpp:3"]
    finally:
        REPO = saved
