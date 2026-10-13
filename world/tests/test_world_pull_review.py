#!/usr/bin/env python3
"""Bounded, immutable in-world pull-request review contracts."""

import json
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
MODULE = WORLD / "world-pull-review.js"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")


def run_module(source):
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            source,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_exact_immutable_oids_and_safe_pull_paths():
    script = f"""
      import {{
        immutableGitOid, parsePullFrontMatter, safeDiffPath, safePullNumber
      }}
        from {json.dumps(MODULE.as_uri())};
      process.stdout.write(JSON.stringify({{
        oid40: immutableGitOid("A".repeat(40)),
        oid64: immutableGitOid("b".repeat(64)),
        bad39: immutableGitOid("c".repeat(39)),
        bad41: immutableGitOid("c".repeat(41)),
        bad65: immutableGitOid("c".repeat(65)),
        path: safeDiffPath("src/review file.js"),
        traversal: safeDiffPath("../private/key"),
        absolute: safeDiffPath("/etc/passwd"),
        drive: safeDiffPath("C:/secret"),
        quoted: safeDiffPath('"private path"'),
        pull: safePullNumber("44"),
        badPull: safePullNumber("0"),
        missingStatus: parsePullFrontMatter("", 44).status
      }}));
    """
    value = run_module(script)
    assert value["oid40"] == "a" * 40
    assert value["oid64"] == "b" * 64
    assert value["bad39"] == ""
    assert value["bad41"] == ""
    assert value["bad65"] == ""
    assert value["path"] == "src/review file.js"
    assert value["traversal"] == ""
    assert value["absolute"] == ""
    assert value["drive"] == ""
    assert value["quoted"] == ""
    assert value["pull"] == 44
    assert value["badPull"] == 0
    assert value["missingStatus"] == "unknown"


def test_front_matter_and_unified_diff_are_bounded_and_line_numbered():
    script = f"""
      import {{ parsePullFrontMatter, parseUnifiedDiff }}
        from {json.dumps(MODULE.as_uri())};
      const metadata = parsePullFrontMatter(`---
schema: forkmesh-pull-v1
number: 44
title: "Review exact metadata"
status: merged
authorName: Alice
base: main
head: feature/review
derive: branch
creationBaseOid: ${{"a".repeat(40)}}
creationHeadOid: ${{"b".repeat(40)}}
---
An owner-authored description.`);
      const patch = `diff --git a/src/a.js b/src/a.js
index 1111111..2222222 100644
--- a/src/a.js
+++ b/src/a.js
@@ -3,2 +3,3 @@
 context
-old value
+new value
+tail
diff --git a/assets/logo.png b/assets/logo.png
new file mode 100644
Binary files /dev/null and b/assets/logo.png differ`;
      const diff = parseUnifiedDiff(patch);
      process.stdout.write(JSON.stringify({{ metadata, diff }}));
    """
    value = run_module(script)
    metadata = value["metadata"]
    assert metadata["number"] == 44
    assert metadata["schema"] == "forkmesh-pull-v1"
    assert metadata["title"] == "Review exact metadata"
    assert metadata["status"] == "merged"
    assert metadata["derive"] == "branch"
    assert metadata["creationBaseOid"] == "a" * 40
    assert metadata["creationHeadOid"] == "b" * 40
    assert metadata["body"] == "An owner-authored description."
    diff = value["diff"]
    assert len(diff["files"]) == 2
    assert diff["additions"] == 2
    assert diff["deletions"] == 1
    rows = diff["files"][0]["rows"]
    assert rows[1] == {
        "type": "context", "oldLine": 3, "newLine": 3, "text": "context"}
    assert rows[2] == {
        "type": "delete", "oldLine": 4, "newLine": None, "text": "old value"}
    assert rows[3] == {
        "type": "add", "oldLine": None, "newLine": 4, "text": "new value"}
    assert diff["files"][1]["binary"] is True
    assert diff["files"][1]["status"] == "added"


def test_closed_and_merged_pulls_do_not_load_a_patch():
    assert 'String(metadata.status || "").toLowerCase() !== "open"' in APP
    assert 'patchSource: "not-open"' in APP


def test_unsafe_files_are_omitted_and_render_budget_is_enforced():
    script = f"""
      import {{ parseUnifiedDiff }} from {json.dumps(MODULE.as_uri())};
      const unsafe = `diff --git a/../../secret b/../../secret
@@ -1 +1 @@
-secret
+leak
diff --git a/src/good.js b/src/good.js
@@ -1 +1 @@
-old
+new`;
      const many = Array.from({{length: 8}}, (_, index) =>
        `diff --git a/f${{index}}.js b/f${{index}}.js\\n@@ -1 +1 @@\\n-old\\n+new`
      ).join("\\n");
      process.stdout.write(JSON.stringify({{
        unsafe: parseUnifiedDiff(unsafe),
        bounded: parseUnifiedDiff(many, {{maxFiles: 2, maxRows: 5}})
      }}));
    """
    value = run_module(script)
    assert [item["path"] for item in value["unsafe"]["files"]] == ["src/good.js"]
    assert len(value["bounded"]["files"]) == 2
    assert value["bounded"]["rowCount"] == 5
    assert value["bounded"]["truncated"] is True
    assert value["bounded"]["omittedFiles"] == 6


def test_file_tree_and_viewed_key_store_only_identity_not_code():
    script = f"""
      import {{ buildPullFileTree, pullViewedStateKey }}
        from {json.dumps(MODULE.as_uri())};
      const files = [
        {{path:"src/api/a.js", status:"modified", additions:2, deletions:1}},
        {{path:"README.md", status:"modified", additions:1, deletions:0}}
      ];
      process.stdout.write(JSON.stringify({{
        tree: buildPullFileTree(files),
        key: pullViewedStateKey("ForkMesh", "ForkMesh", "d".repeat(40), 44)
      }}));
    """
    value = run_module(script)
    assert [node["name"] for node in value["tree"]] == ["src", "README.md"]
    assert value["tree"][0]["children"][0]["name"] == "api"
    assert value["tree"][0]["children"][0]["children"][0]["path"] == "src/api/a.js"
    assert value["key"] == f"forkmesh/forkmesh@{'d' * 40}#44"
    assert "README" not in value["key"]


def test_merge_request_requires_matching_exact_oids_and_a_stable_request_id():
    script = f"""
      import {{ buildPullMergeRequest, exactPullMergeContext }}
        from {json.dumps(MODULE.as_uri())};
      const active = {{
        commit: "a".repeat(40),
        entityRecords: {{
          repositoryCommit: "a".repeat(40),
          pullMetadataCommit: "c".repeat(40)
        }}
      }};
      const review = {{
        state: "ready",
        number: 44,
        metadataCommit: "c".repeat(40),
        metadata: {{
          number: 44,
          schema: "forkmesh-pull-v1",
          derive: "branch",
          status: "open",
          creationBaseOid: "a".repeat(40),
          creationHeadOid: "b".repeat(40)
        }}
      }};
      const context = exactPullMergeContext(active, review);
      const request = buildPullMergeRequest(
        context, "world_merge_1234567890abcdef"
      );
      process.stdout.write(JSON.stringify({{
        context,
        request,
        staleBase: exactPullMergeContext(
          {{...active, commit: "d".repeat(40)}}, review
        ),
        mixedHashKinds: exactPullMergeContext(active, {{
          ...review,
          metadata: {{
            ...review.metadata,
            creationHeadOid: "b".repeat(64)
          }}
        }}),
        badRequestId: buildPullMergeRequest(context, "predictable"),
        waived: buildPullMergeRequest(
          context, "world_merge_1234567890abcdef", {{ overrideReview: true }}
        )
      }}));
    """
    value = run_module(script)
    assert value["context"] == {
        "number": 44,
        "expectedBaseOid": "a" * 40,
        "expectedHeadOid": "b" * 40,
        "expectedPullsOid": "c" * 40,
    }
    assert value["request"] == {
        "schemaVersion": 1,
        "type": "forkmesh.pull-merge-v1",
        "pullNumber": 44,
        "requestId": "world_merge_1234567890abcdef",
        "expectedBaseOid": "a" * 40,
        "expectedHeadOid": "b" * 40,
        "expectedPullsOid": "c" * 40,
    }
    assert value["staleBase"] is None
    assert value["mixedHashKinds"] is None
    assert value["badRequestId"] is None
    # A repository owner may merge without the independent-review gate; every
    # other request keeps the exact shape the relay and the mirrors already
    # accept, so the field is absent rather than false.
    assert "overrideReview" not in value["request"]
    assert value["waived"] == {**value["request"], "overrideReview": True}


def test_world_review_uses_exact_pull_ref_and_stays_internal_memory_only():
    assert 'candidate.name === "forkmesh/pulls" && candidate.commit' in APP
    assert "immutableGitOid(tree?.commit) !== pullMetadataCommit" in APP
    assert "immutableGitOid(result?.commit) !== pullMetadataCommit" in APP
    assert "responseCommit !== metadataCommit" in APP
    assert 'query.set("ref", pullMetadataCommit)' in APP
    assert 'query.set("ref", metadataCommit)' in APP
    assert "No main-branch, guessed-ref" in APP
    assert "private pull metadata is not publicly probed" in APP
    assert 'state: "unknown"' in APP
    assert "metadataAvailable: false" in APP
    assert "not labeled open" in APP
    assert "this.pullViewedFiles = new Map()" in APP
    assert "data-world-pull-file-tree" not in APP  # semantic nav, not a fake widget
    assert "data-world-pull-file-path" in APP
    assert "data-world-pull-diff" in APP
    assert "data-world-pull-viewed-summary" in APP
    review_slice = APP[
        APP.index("  repositoryPullRecords("):
        APP.index("  applyRepositoryFilters(")
    ]
    assert "localStorage" not in review_slice
    assert "sessionStorage" not in review_slice
    assert 'target="_blank"' not in review_slice
    assert "data-world-pull-merge" in review_slice
    assert "Merge pull request #" in review_slice
    assert "exactPullMergeContext(active, review)" in review_slice
    assert "/pulls/${context.number}/merge" in review_slice
    assert "authorization: `Bearer ${sessionToken}`" in review_slice
    assert "WORLD_PULL_MERGE_MAX_REQUESTS" in APP
    assert "result.state === \"merged\" && result.published === true" in review_slice
    assert "reloadRepositoryAfterPublishedPullMerge" in review_slice
    assert '.world-detail[data-repository-review="true"]' in CSS
    assert ".world-pull-review-layout" in CSS
    assert ".world-pull-file-tree" in CSS
    assert ".world-pull-diff-row.is-add" in CSS
    assert ".world-pull-merge-panel" in CSS
    assert "forbidden:" in review_slice


def test_a_refused_review_gate_offers_the_owner_a_fresh_waived_request():
    # The gate stops a merge; it does not stop the repository's owner. The
    # endpoint still answers "forbidden" to everyone else, exactly as it does
    # for the ordinary merge button.
    assert 'data-world-pull-merge="waive-review"' in APP
    assert 'state === "review-required"' in APP
    assert "Merge without review" in APP
    assert 'pullMerge.dataset.worldPullMerge === "waive-review"' in APP
    # A waiver is a different request, not a retry: the relay binds one request
    # id to one exact payload, so reusing the refused id would come back as a
    # reused id rather than as a merge.
    assert "const existingRequest = overrideReview\n      ? null\n" in APP
    assert "buildPullMergeRequest(context, requestId, {\n      overrideReview,\n    });" in APP