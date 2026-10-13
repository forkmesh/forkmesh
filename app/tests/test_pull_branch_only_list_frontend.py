#!/usr/bin/env python3
"""Issue #683: the pull list must show every pull its Open badge counts.

The served openPulls badge unions the aggregate forkmesh/pulls index with the
authoritative per-pull forkmesh/pull/<n> branches. The list used to read only
the index, so a pull the index lagged was counted ("3 Open") but never listed.
"""

import json
import subprocess
from pathlib import Path

CONTENT = (
    Path(__file__).resolve().parents[1]
    / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")

INDEX_COMMIT = "a" * 40
BRANCH_COMMIT = "b" * 40


def test_pull_list_reads_branch_only_pulls_from_their_own_commit():
    loader = CONTENT[
        CONTENT.index("async function loadRepoRecordsFromMirror")
        : CONTENT.index("function renderRepoCollectionPagination")
    ]
    script = """
const assert = require("assert");
const INDEX_COMMIT = %s;
const BRANCH_COMMIT = %s;
const blobReads = [];
function immutableGitCommit(value) {
  const commit = String(value || "").trim();
  return /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/i.test(commit) ? commit : "";
}
async function resolveRepoPullMetadataCommit() { return INDEX_COMMIT; }
function isMissingMirrorFolder() { return false; }
function repoLiveUrl(repo, action, params) { return { action, params }; }
async function fetchRepoJson(url) {
  assert.equal(url.action, "tree");
  assert.equal(url.params.ref, INDEX_COMMIT);
  return {
    commit: INDEX_COMMIT,
    entries: [{ name: "250", type: "tree" }, { name: "251", type: "tree" }],
    // #250 is also indexed; the ref listing must not duplicate it.
    pullRefs: { "249": BRANCH_COMMIT, "250": BRANCH_COMMIT, "bogus": BRANCH_COMMIT },
  };
}
const record = (number) => ({ content: `status: open\\nnumber: ${number}` });
async function fetchRepoBlobs(repo, paths, options) {
  blobReads.push([options.ref, paths]);
  return Object.fromEntries(paths.map((path) => [path, record(path.split("/")[2])]));
}
async function fetchMissingRepoBlobs(repo, paths, options) {
  blobReads.push([options.ref, paths]);
  return Object.fromEntries(paths.map((path) => [path, record(path.split("/")[2])]));
}
function blobText(blob) { return blob.content; }
function parseFrontMatter(text) {
  const values = {};
  text.split("\\n").forEach((line) => {
    const [key, value] = line.split(": ");
    values[key] = value;
  });
  return { values, body: "" };
}
function formatRecordDate() { return ""; }
""" % (json.dumps(INDEX_COMMIT), json.dumps(BRANCH_COMMIT)) + loader + """
(async () => {
  const config = {
    dir: ".forkmesh/pulls", file: "pull.md", itemLabel: "pull request",
    meta: (values) => values.status,
  };
  const items = await loadRepoRecordsFromMirror({}, config);
  assert.deepEqual(items.map((item) => item.number), [251, 250, 249]);
  assert.ok(items.every((item) => item.state === "open"));
  assert.deepEqual(blobReads, [
    [INDEX_COMMIT, [".forkmesh/pulls/251/pull.md", ".forkmesh/pulls/250/pull.md"]],
    [BRANCH_COMMIT, [".forkmesh/pulls/249/pull.md"]],
  ]);
})().catch((error) => {
  process.stderr.write(String(error && error.stack || error));
  process.exit(1);
});
"""
    result = subprocess.run(
        ["node"], input=script, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 0, result.stderr


def test_pull_detail_falls_back_to_the_pull_branch():
    resolver = CONTENT[
        CONTENT.index("async function resolveRepoPullBranchCommit")
        : CONTENT.index("function isMissingMirrorFolder")
    ]
    assert "ref: `${PULL_BRANCH_PREFIX}${pull}`," in resolver
    assert 'const PULL_BRANCH_PREFIX = "forkmesh/pull/";' in CONTENT
    detail = CONTENT[
        CONTENT.index("async function loadRepoRecordDetail")
        : CONTENT.index("function setRepoTabCount")
    ]
    assert "await resolveRepoPullBranchCommit(repo, number)" in detail
    # The ledger refresh tick must not re-pin a branch-only pull to an index
    # commit that does not hold its record.
    refresh = CONTENT[
        CONTENT.index("if (servedCommit && servedCommit !== pinnedCommit) {")
        : CONTENT.index("const [conversation, commits, agents] = await Promise.all([\n      loadRepoPullConversation(repo, number, parsed.pullMetadataCommit")
    ]
    assert refresh.index("if (record) {") < refresh.index(
        "parsed.pullMetadataCommit = servedCommit;")
