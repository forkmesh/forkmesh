import json
import subprocess

from _dashboard_bundle import assembled_dashboard_js
from test_repo_logical_owner_labels import BOOT, BROWSER_STUBS

DASHBOARD_JS = assembled_dashboard_js()


def _run(exports, body):
    transformed = DASHBOARD_JS.replace(
        BOOT, "  globalThis.__dashboardExports = { " + exports + " };")
    assert transformed != DASHBOARD_JS
    script = ("const SOURCE = " + json.dumps(transformed) + ";\n"
              + BROWSER_STUBS + body)
    result = subprocess.run(
        ["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""
    return result.stdout


REPOS = """
const project = {
  owner: "alice", name: "Project", source: "hosted-web", rootCommit: "a".repeat(40),
  ownerKind: "organization", logicalOwner: "acme",
  logicalOwners: [{ kind: "organization", owner: "acme" }],
};
const fork = {
  owner: "bob", name: "Project", source: "hosted-web", rootCommit: "a".repeat(40),
  forkedFrom: "acme/project",
  logicalOwners: [{ kind: "user", owner: "bob" }],
};
"""


def test_a_fork_of_the_organization_address_finds_its_parent():
    output = _run(
        "state, repoMatchesKey, findRepository, repoProvenanceHtml",
        REPOS + """
const exports = global.__dashboardExports;
exports.state.repositories = [project, fork];
assert.ok(exports.repoMatchesKey(project, "acme/project"));
assert.ok(!exports.repoMatchesKey(fork, "acme/project"));
assert.strictEqual(exports.findRepository("acme/project").owner, "alice");
const line = exports.repoProvenanceHtml(fork);
assert.ok(line.includes("acme/Project"), line);
assert.ok(!line.includes("alice"), line);
console.log("ok");
""")
    assert output.strip() == "ok"
