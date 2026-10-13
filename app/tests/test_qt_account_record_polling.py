"""The desktop reads GET /api/accounts/<name> once per account per session.

That route answers with the entire account record — status, key, admin and
email flags, and the base64 avatar PNG — and it changes only when its owner
changes it. The Qt client used to re-read it on a 60s timer *and* on every
profile repaint, which put 195 identical requests into a single session: 1% of
the whole network log for a value nobody had touched. The dashboard already
learned this lesson (see test_poll_digest.py); these pin it for the desktop.
"""
from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
)

ROOT = Path(__file__).resolve().parents[2]
QT_SRC = ROOT / "desktop" / "src"
CHAT = "".join(
    (QT_SRC / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
SETUP = (QT_SRC / "MainWindowSetup.cpp").read_text(encoding="utf-8")
HEADER = (QT_SRC / "MainWindow.h").read_text(encoding="utf-8")


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    return source[i:source.index(end, i)]


def test_payout_address_is_not_re_read_on_a_timer():
    # No timer of any period may reintroduce the poll.
    assert "m_webSolanaTimer" not in HEADER
    assert "m_webSolanaTimer" not in CHAT
    body = _body(
        CHAT,
        "void MainWindow::refreshWebUserSolanaAddress()",
        "void MainWindow::updateNavSolanaBalance()",
    )
    assert "QTimer" not in body
    # The address is a field of the account record, so a repaint reads the
    # copy already in hand rather than the relay.
    assert "cachedAccountRecord(account)" in body


def test_profile_repaint_reads_the_cached_account_record():
    body = _body(
        CHAT,
        "void MainWindow::refreshProfileAccountStatus(bool force)",
        "void MainWindow::fetchLinkedNodesFromOwner(",
    )
    # The cache is consulted before any request is built, and only the callers
    # that just changed the record ask for a fresh read.
    assert body.index("cachedAccountRecord(node)") < body.index(
        "m_networkAccess->get(")
    assert "if (force)" in body and "invalidateAccountRecord();" in body


def test_login_is_what_primes_the_account_record_cache():
    body = _body(
        SETUP,
        "bool MainWindow::authenticateSilently(",
        "bool MainWindow::testAccountLoginResponseAccepted(",
    )
    assert "cacheAccountRecord(accountName, lookup);" in body
