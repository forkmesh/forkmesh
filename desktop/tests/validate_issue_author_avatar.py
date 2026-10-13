#!/usr/bin/env python3
"""Guard the face the issue detail puts next to a reporter's name.

Issues that arrive through ForkBot intake, the Discord bug channel or an agent
are signed by this node but carry the reporter's name, so the signing key and
the printed name describe different people. The timeline used to pick the
picture from the key alone, which put the local user's own avatar beside every
one of those reports — 744 of the 1142 locally signed events in this repo.

The rule: the picture follows the name printed next to it, and the local user's
own avatar is only reachable when that name is one of ours.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ISSUES = (ROOT / "src/MainWindowIssues.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    resolver = between(
        ISSUES,
        "QPixmap MainWindow::issueTimelineAvatar(",
        "\nvoid MainWindow::reloadIssues()",
    )
    # The local user's own picture is gated on the printed name being ours.
    assert "isLocalAuthorName(name)" in resolver
    assert resolver.index("const bool self =") < resolver.index(
        "effectiveUserAvatar()"
    )
    # A picture cached against the signing key describes the reporter only when
    # that key is not this install signing on someone else's behalf.
    assert "(self || !signedHere)" in resolver
    # The generated portrait is seeded from the displayed name, so one person
    # keeps one face across the issue views.
    assert "const QString seed = name.isEmpty() ? authorId : name;" in resolver

    # Both timeline surfaces resolve through it instead of reading the avatar
    # cache with the signing key.
    card = between(
        ISSUES,
        "void MainWindow::addIssueThreadCard(",
        "\n}\n",
    )
    activity = between(
        ISSUES,
        "void MainWindow::addIssueActivityRow(",
        "avatarColumn->setFixedWidth(36);",
    )
    for name, region in (("card", card), ("activity", activity)):
        assert "issueTimelineAvatar(ev.author, ev.authorName," in region, name
        assert "m_avatars.value(" not in region, name
        assert "effectiveAvatar()" not in region, name


if __name__ == "__main__":
    main()
    print("issue author avatar contract ok")
