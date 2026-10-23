import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
JS = ROOT / "app/public/dashboard/js"
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
NETWORK = (JS / "08-repo-detail-network.js").read_text(encoding="utf-8")


def _function(source, name):
    start = source.index(f"  function {name}(")
    end = source.index("\n  }\n", start) + 4
    return source[start:end]


_HARNESS = """
const store = {};
globalThis.localStorage = {
  getItem: (key) => (key in store ? store[key] : null),
  setItem: (key, value) => { store[key] = String(value); },
};
const state = { repositories: [
  { owner: "drew", name: "Widgets", logicalOwners: [{ kind: "organization", owner: "acme" }] },
  { owner: "drew", name: "Other", logicalOwners: [{ kind: "organization", owner: "acme" }] },
] };
%s
const member = { owner: "drew", name: "Widgets", logicalOwners: [{ kind: "organization", owner: "acme" }] };
const organization = { owner: "acme", name: "Widgets", logicalOwners: [{ kind: "organization", owner: "acme" }] };
const other = { owner: "someone", name: "Widgets" };
const before = [contributionTipDismissed("pull", member), contributionTipDismissed("pull", organization)];
dismissContributionTip("pull", member);
const afterMember = [contributionTipDismissed("pull", member), contributionTipDismissed("pull", organization)];
for (const key of Object.keys(store)) delete store[key];
dismissContributionTip("issue", organization);
const afterOrganization = [issueContributionTipDismissed(member), issueContributionTipDismissed(organization),
  contributionTipDismissed("pull", member), issueContributionTipDismissed(other)];
for (const key of Object.keys(store)) delete store[key];
store["forkmesh.pull-contribution-tip.v1:drew/Widgets"] = "1";
const legacy = [contributionTipDismissed("pull", organization), contributionTipDismissed("pull", { owner: "acme", name: "Other" })];
console.log(JSON.stringify({ before, afterMember, afterOrganization, legacy }));
"""


def test_dismissal_holds_under_every_owner_name_the_repository_loads_with():
    node = shutil.which("node")
    if node is None:
        pytest.skip("node is not installed")
    functions = "\n".join([
        _function(HELPERS, "repoLogicalOwners"),
        _function(COMPOSE, "contributionTipKeys"),
        _function(COMPOSE, "contributionTipDismissed"),
        _function(COMPOSE, "dismissContributionTip"),
        _function(COMPOSE, "issueContributionTipDismissed"),
    ])
    result = subprocess.run([node, "-e", _HARNESS % functions], capture_output=True, text=True, check=True)
    assert json.loads(result.stdout) == {
        "before": [False, False],
        "afterMember": [True, True],
        "afterOrganization": [True, True, False, False],
        "legacy": [True, False],
    }


def test_both_dismiss_buttons_store_through_the_shared_helper():
    assert 'dismissContributionTip("issue", state.selectedRepo)' in NETWORK
    assert 'dismissContributionTip("pull", state.selectedRepo)' in NETWORK
    assert not re.search(r"contribution-tip\.v1:\$\{repoKey", COMPOSE + NETWORK)
