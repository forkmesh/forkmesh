import re
import subprocess
from pathlib import Path


JS = Path(__file__).resolve().parents[1] / "public" / "dashboard" / "js"
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
ISSUES_IO = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")


def function_source(source, name):
    start = re.search(r"\n  (?:async )?function %s\(" % name, source).start() + 1
    end = source.index("\n  }\n", start) + 4
    return source[start:end]


def test_fork_of_read_only_mirror_opens_pulls_on_itself():
    functions = "\n".join([
        function_source(HELPERS, "repoForkParent"),
        function_source(HELPERS, "repoMirrorOf"),
        function_source(HELPERS, "repoReadOnlyMirror"),
        function_source(ISSUES_IO, "pullRequestParent"),
        function_source(ISSUES_IO, "pullRequestTargetRepo"),
    ])
    script = """
const assert = require("assert");
const normalizeRepoSegment = (value) => String(value || "").trim().toLowerCase();
const records = {
  "org/mirror": {owner: "org", name: "mirror", mirrorOf: "https://upstream.example/a/b"},
  "org/app": {owner: "org", name: "app"},
};
const findRepository = (key) => records[key] || null;
%s
const forkOfMirror = {owner: "org", name: "platform", forkedFrom: "org/mirror"};
assert.strictEqual(pullRequestParent(forkOfMirror), null);
assert.strictEqual(pullRequestTargetRepo(forkOfMirror), forkOfMirror);
const forkOfApp = {owner: "alice", name: "app", forkedFrom: "org/app"};
assert.deepStrictEqual(pullRequestParent(forkOfApp), {owner: "org", name: "app", key: "org/app"});
assert.strictEqual(pullRequestTargetRepo(forkOfApp), records["org/app"]);
const plain = {owner: "org", name: "solo"};
assert.strictEqual(pullRequestTargetRepo(plain), plain);
""" % functions
    subprocess.run(["node", "-e", script], check=True)


def test_compose_and_submit_use_the_pull_parent():
    assert "const parent = pullRequestParent(repo);" in function_source(
        ISSUES_IO, "submitWebPullOpen")
    assert "const parent = pullRequestParent(repo);" in function_source(
        COMPOSE, "openPullCompose")
    assert "const crossFork = targetRepo !== repo;" in COMPOSE


def test_a_read_only_refusal_from_the_parent_reopens_the_pull_on_the_fork():
    submit = function_source(ISSUES_IO, "submitWebPullOpen")
    assert '!== "read_only_mirror") throw error;' in submit
    assert "signWebPull(repo, repo, title, body, base, head, head)" in submit
    assert "targetRepo: repo };" in submit
