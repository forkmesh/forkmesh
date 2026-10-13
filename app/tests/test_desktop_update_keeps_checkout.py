import re
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2] / "desktop" / "src" / "MainWindowSetup.cpp"


def _body(name: str) -> str:
    text = SOURCE.read_text()
    start = text.index("void MainWindow::" + name + "(")
    end = text.find("\n}\n", start)
    return text[start:end]


def test_update_rebuild_restart_never_removes_the_checkout():
    body = _body("updateRebuildRestart")
    assert not re.search(r'"rm"', body)
    assert "rm -rf" not in body
    assert "recloneFromMirror" not in body


def test_failed_pull_stops_instead_of_recloning():
    body = _body("updateRebuildRestart")
    pull = body.index('"pull", "--ff-only"')
    early_return = body.index("return;\n    }\n\n    const QDir existing(repoDir);")
    assert body.index('"clone"') > early_return > pull
    assert "stopUntouched(QStringLiteral(\"Could not fast-forward" in body


def test_clone_only_into_a_missing_or_empty_folder():
    body = _body("updateRebuildRestart")
    guard = body.index("QDir::NoDotAndDotDot")
    assert guard < body.index('"clone"')


def test_other_update_paths_never_remove_the_checkout():
    text = SOURCE.read_text()
    assert not re.search(r'"rm",\s*\{"-rf",\s*repoDir\}', text)
