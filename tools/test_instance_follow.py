import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parent / "instance_follow.py"
ENTRY = '''SCHEMA_PRE_CREATE_ALTER_STATEMENTS = []
SCHEMA_ALTER_STATEMENTS = []
_SCHEMA_FINGERPRINT = (
    "%s"
)
'''
TEST = '''def test_latest():
    assert migration_name == "%s"
'''


def fingerprint(statements):
    return hashlib.sha256("\n".join(statements).encode("utf-8")).hexdigest()


def migration(number, name, statements):
    return ("CREATE TABLE IF NOT EXISTS %s (k TEXT);\n\n"
            "INSERT INTO schema_meta (k, v) VALUES (\n"
            "  'schema_fingerprint:%s',\n  'applied'\n"
            ") ON CONFLICT(k) DO UPDATE SET v='applied';\n" % (
                name, fingerprint(statements)))


class Repo:
    def __init__(self, path, env):
        self.path, self.env = path, env

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.path), *args],
                              env=self.env, check=True, capture_output=True,
                              text=True).stdout.strip()

    def write(self, rel, text):
        file = self.path / rel
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text, encoding="utf-8")

    def schema(self, tables, newest):
        statements = ["CREATE TABLE %s (k TEXT)" % t for t in tables]
        self.write("app/src/schema.py", "SCHEMA_STATEMENTS = [\n%s]\n" % "".join(
            "    %r,\n" % statement for statement in statements))
        self.write("app/src/entry.py", ENTRY % fingerprint(statements))
        self.write("app/tests/test_local_migrations.py", TEST % newest)
        return statements

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")


@pytest.fixture
def repos(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("GIT_")}
    env.update(HOME=str(tmp_path), GIT_CONFIG_GLOBAL="/dev/null",
               GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t",
               GIT_AUTHOR_EMAIL="t@example.com", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@example.com")
    upstream = Repo(tmp_path / "upstream", env)
    upstream.path.mkdir()
    upstream.git("init", "-q", "-b", "main")
    statements = upstream.schema(["a", "b", "c"], "0001_a.sql")
    upstream.write("app/migrations/0001_a.sql", migration(1, "a", statements))
    upstream.write("app/migrations/down/0001_a.sql", "DROP TABLE a;\n")
    upstream.write("changelog/changelog.json", '{"changes": [\n  "First"\n]}\n')
    upstream.commit("First")
    fork = Repo(tmp_path / "fork", env)
    subprocess.run(["git", "clone", "-q", str(upstream.path), str(fork.path)],
                   env=env, check=True)
    statements = fork.schema(["mine", "a", "b", "c"], "0002_mine.sql")
    fork.write("app/migrations/0002_mine.sql", migration(2, "mine", statements))
    fork.write("changelog/changelog.json",
               '{"changes": [\n  "Mine",\n  "First"\n]}\n')
    fork.commit("Instance change")
    publish = tmp_path / "publish.git"
    subprocess.run(["git", "init", "-q", "--bare", str(publish)], env=env,
                   check=True)
    return env, upstream, fork, publish, tmp_path


def follow(env, fork, upstream, work, publish=""):
    result = subprocess.run(
        [sys.executable, str(TOOL), "--work", str(work),
         "--source-url", str(fork.path), "--follow-url", str(upstream.path),
         "--publish", str(publish)],
        env=env, capture_output=True, text=True, timeout=120)
    return result.returncode, json.loads(result.stdout.strip().splitlines()[-1])


def test_a_new_upstream_migration_is_numbered_after_the_instance(repos):
    env, upstream, fork, publish, tmp = repos
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 0 and result["state"] == "current"
    statements = upstream.schema(["a", "b", "c", "theirs"], "0002_theirs.sql")
    upstream.write("app/migrations/0002_theirs.sql",
                   migration(2, "theirs", statements))
    upstream.write("app/migrations/down/0002_theirs.sql",
                   "DROP TABLE theirs;\n")
    upstream.write("changelog/changelog.json",
                   '{"changes": [\n  "Theirs",\n  "First"\n]}\n')
    upstream.write("docs/theirs.md", "See 0002_theirs.\n")
    upstream.commit("Upstream change")
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 0 and result["state"] == "merged", result
    assert result["renumbered"] == {"0002_theirs": "0003_theirs"}
    work = tmp / "work"
    assert sorted(p.name for p in (work / "app/migrations").glob("*.sql")) == [
        "0001_a.sql", "0002_mine.sql", "0003_theirs.sql"]
    assert (work / "app/migrations/down/0003_theirs.sql").exists()
    assert (work / "docs/theirs.md").read_text() == "See 0003_theirs.\n"
    merged = ["CREATE TABLE %s (k TEXT)" % t for t in ("mine", "a", "b", "c", "theirs")]
    assert result["fingerprint"] == fingerprint(merged)
    assert result["fingerprint"] in (work / "app/migrations/0003_theirs.sql"
                                     ).read_text()
    assert '"0003_theirs.sql"' in (
        work / "app/tests/test_local_migrations.py").read_text()
    changes = json.loads((work / "changelog/changelog.json").read_text())
    assert changes["changes"][:2] == ["Mine", "Theirs"]
    published = subprocess.run(
        ["git", "-C", str(publish), "rev-parse", "main"], env=env,
        capture_output=True, text=True, check=True).stdout.strip()
    assert published == result["commit"]
    assert fingerprint(merged) in (work / "app/src/entry.py").read_text()


def test_a_conflict_a_person_must_resolve_stops_without_publishing(repos):
    env, upstream, fork, publish, tmp = repos
    fork.write("app/src/feature.py", "VALUE = 1\n")
    fork.commit("Ours")
    upstream.write("app/src/feature.py", "VALUE = 2\n")
    upstream.commit("Theirs")
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 1 and result["state"] == "conflict"
    assert result["files"] == ["app/src/feature.py"]
    assert result["behind"] == 1
    assert not subprocess.run(
        ["git", "-C", str(publish), "rev-parse", "--verify", "-q", "main"],
        env=env, capture_output=True).stdout
    assert not (tmp / "work" / ".git" / "MERGE_HEAD").exists()


def test_a_folder_the_tool_did_not_make_is_never_reset(repos):
    env, upstream, fork, publish, tmp = repos
    plain = tmp / "plain"
    plain.mkdir()
    (plain / "keep.txt").write_text("keep\n")
    code, result = follow(env, fork, upstream, plain, publish)
    assert code == 1 and result["state"] == "failed"
    assert sorted(p.name for p in plain.iterdir()) == ["keep.txt"]
    (fork.path / "untracked.txt").write_text("keep\n")
    head = fork.git("rev-parse", "HEAD")
    code, result = follow(env, fork, upstream, fork.path, publish)
    assert code == 1 and result["state"] == "failed"
    assert (fork.path / "untracked.txt").read_text() == "keep\n"
    assert fork.git("rev-parse", "HEAD") == head
    assert not fork.git("for-each-ref", "refs/follow/")


def test_a_patch_forkmesh_accepted_and_revised_takes_the_revision(repos):
    env, upstream, fork, publish, tmp = repos
    sent = "# The reason, before review.\nVALUE = 1\n"
    fork.write("app/src/feature.py", sent)
    fork.commit("Feature")
    upstream.write("app/src/feature.py", sent)
    upstream.commit("Feature")
    upstream.write("app/src/feature.py", "VALUE = 1\n")
    upstream.commit("Feature: the reason moves to docs")
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 0 and result["state"] == "merged", result
    assert (tmp / "work/app/src/feature.py").read_text() == "VALUE = 1\n"


def test_an_instance_change_survives_a_revised_patch_in_the_same_file(repos):
    env, upstream, fork, publish, tmp = repos
    filler = "".join("LINE_%d = %d\n" % (n, n) for n in range(10))
    upstream.write("app/src/feature.py", "ONE = 1\n" + filler + "TWO = 2\n")
    upstream.commit("Feature file")
    fork.git("pull", "-q", "--no-rebase", "--no-edit", "origin", "main")
    fork.write("app/src/feature.py", "ONE = 1\n" + filler + "TWO = 22\n")
    fork.commit("Instance only")
    fork.write("app/src/feature.py",
               "ONE = 1\n# Why three.\nTHREE = 3\n" + filler + "TWO = 22\n")
    fork.commit("Sent")
    upstream.write("app/src/feature.py",
                   "ONE = 1\n# Why three.\nTHREE = 3\n" + filler + "TWO = 2\n")
    upstream.commit("Sent")
    upstream.write("app/src/feature.py",
                   "ONE = 1\nTHREE = 3\n" + filler + "TWO = 2\n")
    upstream.commit("Sent: the reason moves to docs")
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 0 and result["state"] == "merged", result
    assert (tmp / "work/app/src/feature.py").read_text() == (
        "ONE = 1\nTHREE = 3\n" + filler + "TWO = 22\n")


def test_a_migration_number_forkmesh_gave_a_sent_migration_keeps_the_instances(
        repos):
    env, upstream, fork, publish, tmp = repos
    fork.write("docs/mine.md", "Run 0002_mine.sql once.\n")
    fork.commit("Mine docs")
    upstream.write("docs/mine.md", "Run 0005_mine.sql once.\n")
    upstream.commit("Mine docs")
    code, result = follow(env, fork, upstream, tmp / "work", publish)
    assert code == 0 and result["state"] == "merged", result
    assert (tmp / "work/docs/mine.md").read_text() == "Run 0002_mine.sql once.\n"
