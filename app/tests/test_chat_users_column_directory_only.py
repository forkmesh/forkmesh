#!/usr/bin/env python3
"""The desktop users column lists registered accounts only (adhoc #8).

The column merges two sources: live roster peers (nodes, but also anonymous
"Guest 3923" browser tabs and World visitors) and the account directory that
/api/accounts/users returns — the users table. Only the second is a list of
people, so the merged groups are filtered back down to it before rendering;
otherwise the column (and its "USERS — 51" count) fills with guests nobody can
look up.

Contracts pinned here:

  * refreshChatMembers() drops every group whose account key isn't in the
    directory, keeping our own row unconditionally.
  * The filter only runs once the directory has actually loaded, so a pending or
    failed fetch can't blank the column.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
QT_SRC = ROOT.parent / "desktop" / "src"
MESSAGES = (QT_SRC / "MainWindowMessages.cpp").read_text(encoding="utf-8")


def _members_body() -> str:
    start = MESSAGES.index("void MainWindow::refreshChatMembers()")
    end = MESSAGES.index("void MainWindow::showChatUserProfile(", start)
    return MESSAGES[start:end]


def test_directory_is_indexed_by_account_name():
    # directoryUserKey() collapses our own entry to "\x01self", which never
    # matches a group key, so the filter needs the plain account-name key.
    assert "QString directoryAccountKey(const MemberInfo &u)" in MESSAGES


def test_users_column_keeps_only_directory_accounts():
    body = _members_body()
    assert "directoryAccountKey(u)" in body
    assert "accountKeys.contains(g.key)" in body
    assert "groups.erase(std::remove_if(" in body
    # Our own row survives even when the directory hasn't listed it yet.
    assert "!g.primary.self &&" in body


def test_filter_waits_for_a_loaded_directory():
    body = _members_body()
    guard = body.index("if (m_chatDirectoryLoaded) {")
    assert guard < body.index("accountKeys.contains(g.key)")


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
