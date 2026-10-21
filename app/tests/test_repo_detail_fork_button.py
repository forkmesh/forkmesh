"""The repository page's Fork split button is built inside renderRepoDetail,
so the flag it reads must be declared there too (a reference to a const from
another function throws and blanks the whole repository page)."""

from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
          / "08-repo-detail-network.js").read_text(encoding="utf-8")


def _function_body(name):
    start = SOURCE.index(f"function {name}(")
    end = SOURCE.index("\n  function ", start + 1)
    return SOURCE[start:end]


def test_fork_split_button_flag_is_declared_in_render_repo_detail():
    body = _function_body("renderRepoDetail")
    declared = body.index("const websiteForkAvailable = !repo.isPrivate;")
    used = body.index("const forkSplitButton = (compact) => websiteForkAvailable")
    assert declared < used
