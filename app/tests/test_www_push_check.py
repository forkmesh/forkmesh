import io
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import www_push_check

GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
       "-c", "core.hooksPath=hooks-unused"]


def git(repo, *arguments):
    return subprocess.run([*GIT, "-C", str(repo), *arguments], check=True, capture_output=True,
                          text=True).stdout.strip()


@pytest.fixture
def repo(tmp_path):
    repo = tmp_path / "repo"
    for path in www_push_check.ARCHIVED:
        source = ROOT / path
        if source.is_dir():
            shutil.copytree(source, repo / path, ignore=shutil.ignore_patterns("__pycache__"))
        else:
            (repo / path).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, repo / path)
    git(tmp_path, "init", "-q", "-b", "main", str(repo))
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", "good")
    return repo


def hand_edit_public_page(repo):
    page = repo / "www" / "public" / "market.html"
    page.write_text(page.read_text(encoding="utf-8").replace("</main>", "<p>hand edit</p></main>", 1),
                    encoding="utf-8")
    git(repo, "commit", "-q", "-am", "bad")
    return git(repo, "rev-parse", "HEAD")


def push_line(commit, ref="refs/heads/main"):
    return "%s %s %s %s\n" % (ref, commit, ref, "0" * 40)


def test_rendered_pages_pass(repo):
    assert www_push_check.check_commit(git(repo, "rev-parse", "HEAD"), repo) == []


def test_hand_edited_public_page_is_refused_on_main(repo):
    bad = hand_edit_public_page(repo)
    stderr = io.StringIO()
    assert www_push_check.run_hook(push_line(bad), repo, stderr) == 1
    assert "www/public/market.html is out of date" in stderr.getvalue()
    assert "Nothing was pushed" in stderr.getvalue()


def test_other_branches_and_deletes_are_not_checked(repo):
    bad = hand_edit_public_page(repo)
    assert www_push_check.pushed_commits(push_line(bad, "refs/heads/feature")) == []
    assert www_push_check.pushed_commits(push_line("0" * 40)) == []
    assert www_push_check.run_hook(push_line(bad, "refs/heads/feature"), repo, io.StringIO()) == 0


def test_template_out_of_date_with_catalog_is_refused(repo):
    manifest = repo / "marketplace" / "packages" / "marketing" / "manifest.json"
    manifest.write_text(manifest.read_text(encoding="utf-8").replace(
        '"version": "', '"version": "9', 1), encoding="utf-8")
    git(repo, "commit", "-q", "-am", "manifest only")
    problems = www_push_check.check_commit(git(repo, "rev-parse", "HEAD"), repo)
    assert any("out of date with marketplace/catalog.json" in line for line in problems)


def test_install_writes_the_hook_and_leaves_foreign_hooks_alone(tmp_path):
    repo = tmp_path / "plain"
    git(tmp_path, "init", "-q", str(repo))
    out = io.StringIO()
    assert www_push_check.install(repo, out) == 0
    hook = repo / ".git" / "hooks" / "pre-push"
    assert www_push_check.HOOK_MARKER in hook.read_text(encoding="utf-8")
    assert www_push_check.install(repo, out) == 0
    hook.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    assert www_push_check.install(repo, out) == 1
    assert hook.read_text(encoding="utf-8") == "#!/bin/sh\nexit 0\n"
