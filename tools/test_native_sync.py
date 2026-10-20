import subprocess
from pathlib import Path

import pytest

import native_sync

LEDGER_TEXT = """# Sync

<!-- triaged-through: {since} -->

<!-- ledger:start -->
| ID | Date | Area | Kind | Change on iOS | iOS commit | Android | Android commit or reason |
|---|---|---|---|---|---|---|---|
<!-- ledger:end -->
"""


@pytest.fixture
def repo(tmp_path, monkeypatch):
    for key in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setenv("GIT_CONFIG_GLOBAL", str(tmp_path / "gitconfig"))
    monkeypatch.setenv("GIT_CONFIG_NOSYSTEM", "1")
    for role in ("AUTHOR", "COMMITTER"):
        monkeypatch.setenv(f"GIT_{role}_NAME", "Test")
        monkeypatch.setenv(f"GIT_{role}_EMAIL", "test@example.com")
    path = tmp_path / "repo"
    subprocess.run(["git", "init", "-q", "-b", "main", str(path)], check=True)
    monkeypatch.setattr(native_sync, "ROOT", path)
    monkeypatch.setattr(native_sync, "LEDGER", path / "SYNC.md")
    return path


def commit(repo: Path, subject: str, name: str) -> str:
    target = repo / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(target.read_text() + "x" if target.exists() else "x")
    subprocess.run(["git", "-C", str(repo), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-q", "-m", subject], check=True)
    return subprocess.run(["git", "-C", str(repo), "rev-parse", "--short=10", "HEAD"], capture_output=True,
                          text=True, check=True).stdout.strip()


def seed(repo: Path) -> str:
    base = commit(repo, "base", "native/ios/a.swift")
    (repo / "SYNC.md").write_text(LEDGER_TEXT.format(since=base))
    return base


def test_add_lists_and_marks_rows(repo, capsys):
    seed(repo)
    first = commit(repo, "iOS: swipe for message info", "native/ios/b.swift")
    commit(repo, "Android: unrelated", "native/android/a.kt")
    assert [sha for sha, _, _ in native_sync.untracked(native_sync.read_ledger()[1], native_sync.read_ledger()[3])] == [first]
    assert native_sync.main(["add", first, "Swipe left on a message | info", "--area", "Chat", "--kind", "ui"]) == 0
    _, rows, _, _ = native_sync.read_ledger()
    assert [row.id for row in rows] == ["S001"]
    assert rows[0].change == "Swipe left on a message | info"
    assert native_sync.untracked(rows, native_sync.read_ledger()[3]) == []
    android = commit(repo, "Android: swipe for message info", "native/android/b.kt")
    assert native_sync.main(["mark", "S001", "--synced", android]) == 0
    _, rows, _, _ = native_sync.read_ledger()
    assert rows[0].android == "synced" and rows[0].ref == android
    assert native_sync.main(["check", "--strict"]) == 0


def test_check_rejects_bad_rows(repo, capsys):
    seed(repo)
    text = (repo / "SYNC.md").read_text().replace(
        "|---|---|---|---|---|---|---|---|",
        "|---|---|---|---|---|---|---|---|\n| S001 | 2026-10-20 | Chat | ui | x | abc1234 | synced |  |\n| S001 | 2026-10-20 | Chat | perf | x | baseline | maybe |  |")
    (repo / "SYNC.md").write_text(text)
    assert native_sync.main(["check"]) == 1
    out = capsys.readouterr().out
    assert "names the Android commit" in out
    assert "duplicate ID" in out
    assert "kind must be" in out
    assert "Android must be" in out


def test_triage_needs_every_ios_commit_covered(repo, capsys):
    seed(repo)
    sha = commit(repo, "iOS: new screen", "native/ios/c.swift")
    assert native_sync.main(["triage", sha]) == 1
    assert sha in capsys.readouterr().out
    assert native_sync.main(["check", "--strict"]) == 1
    assert native_sync.main(["triage", sha, "--force"]) == 0
    assert native_sync.read_ledger()[3] == sha
    assert native_sync.main(["check", "--strict"]) == 0


def test_repository_ledger_is_valid():
    _, rows, _, since = native_sync.read_ledger(Path(__file__).resolve().parents[1] / "docs" / "native" / "SYNC.md")
    assert since
    assert native_sync.problems(rows) == []


def test_shared_icon_font_and_docs_are_not_ios_changes(repo):
    seed(repo)
    commit(repo, "Android: new icons", "native/ios/ForkMesh/Resources/Fonts/material_icons_regular.otf")
    commit(repo, "Docs: iOS readme", "native/ios/README.md")
    assert native_sync.untracked([], native_sync.read_ledger()[3]) == []
    sha = commit(repo, "iOS: new screen", "native/ios/c.swift")
    assert [item[0] for item in native_sync.untracked([], native_sync.read_ledger()[3])] == [sha]
