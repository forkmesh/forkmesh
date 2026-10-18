import json
import shutil
import subprocess

import pytest

from _dashboard_bundle import assembled_dashboard_js
from _dashboard_shell import assembled_dashboard


DASHBOARD = assembled_dashboard()
DASHBOARD_JS = assembled_dashboard_js()


def _slice(start, end):
    return DASHBOARD_JS[DASHBOARD_JS.index(start):DASHBOARD_JS.index(end)]


RECONNECT = _slice("function nodeNeedsReconnect", "function setEmailVerificationHint")

_HARNESS = r"""
const vm = require("vm");
const source = process.argv[1];
const cases = JSON.parse(process.argv[2]);
const run = async () => {
  const out = [];
  for (const item of cases) {
    const label = { textContent: "Reconnect node" };
    const button = {
      hidden: true, title: "", attrs: {},
      classList: { toggle(name, on) { if (name === "hidden") button.hidden = on; } },
      querySelector: () => label,
      setAttribute(k, v) { this.attrs[k] = v; },
    };
    const fetched = [];
    const sandbox = {
      state: { session: item.session, linkedNodePresence: {} },
      $: (sel) => (sel === "[data-node-reconnect]" ? button : null),
      dashboardOnInstance: () => item.instance,
      fetchJson: async (path) => {
        fetched.push(path);
        const name = decodeURIComponent(path.split("/").pop());
        if (!(name in item.remote)) throw new Error("missing");
        return item.remote[name];
      },
      encodeURIComponent, Promise, Set, String, Array, Boolean, JSON,
    };
    vm.createContext(sandbox);
    vm.runInContext(source + "\nthis.api = { nodeNeedsReconnect, refreshLinkedNodePresence, renderNodeReconnect };", sandbox);
    const before = sandbox.api.nodeNeedsReconnect(item.session);
    sandbox.api.renderNodeReconnect(item.session);
    await sandbox.api.refreshLinkedNodePresence(item.session);
    out.push({
      before,
      after: sandbox.api.nodeNeedsReconnect(item.session),
      hidden: button.hidden,
      label: label.textContent,
      title: button.title,
      fetched,
    });
  }
  console.log(JSON.stringify(out));
};
run();
"""


def _run(cases):
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node.js is unavailable")
    result = subprocess.run(
        [node, "-e", _HARNESS, RECONNECT, json.dumps(cases)],
        capture_output=True, text=True, timeout=30, check=True)
    return json.loads(result.stdout)


WEB_ONLY = {"nodeName": "drew", "kind": "user", "nodes": []}
WITH_DESKTOP = {"nodeName": "drew", "kind": "user", "nodes": ["Laptop"]}


def test_forkmesh_com_still_flags_an_account_with_no_linked_node():
    [web_only, linked] = _run([
        {"session": WEB_ONLY, "instance": False, "remote": {}},
        {"session": WITH_DESKTOP, "instance": False, "remote": {}},
    ])
    assert web_only["before"] is True and web_only["hidden"] is False
    assert web_only["label"] == "Reconnect node"
    assert web_only["fetched"] == []
    assert linked["before"] is False and linked["hidden"] is True


def test_instance_never_tells_a_web_only_account_to_reconnect():
    [result] = _run([{"session": WEB_ONLY, "instance": True, "remote": {}}])
    assert result["before"] is False and result["after"] is False
    assert result["hidden"] is True
    assert result["fetched"] == []


def test_instance_flags_only_a_linked_desktop_that_is_offline():
    offline, online, unknown, guest = _run([
        {"session": WITH_DESKTOP, "instance": True,
         "remote": {"laptop": {"exists": True, "kind": "node", "online": False}}},
        {"session": WITH_DESKTOP, "instance": True,
         "remote": {"laptop": {"exists": True, "kind": "node", "online": True}}},
        {"session": WITH_DESKTOP, "instance": True, "remote": {}},
        {"session": {"nodeName": "forkmesh", "kind": "node", "nodes": []},
         "instance": True, "remote": {}},
    ])
    assert offline["before"] is False
    assert offline["after"] is True and offline["hidden"] is False
    assert offline["fetched"] == ["/api/accounts/laptop"]
    assert offline["label"] == "Desktop offline"
    assert "Your desktop node laptop is offline" in offline["title"]
    assert online["after"] is False and online["hidden"] is True
    assert unknown["after"] is False and unknown["hidden"] is True
    assert guest["after"] is False and guest["hidden"] is True


def test_instance_hides_the_no_node_linked_callout():
    panel = _slice("function renderClaimNodePanel", "const NODE_ID_FORMS")
    assert "!linked && !dashboardOnInstance()" in panel


def test_instance_detection_reads_the_brand_script_tag():
    helper = _slice("function dashboardOnInstance", "const DIRECT_MIRROR_ORIGINS_KEY")
    assert 'script[data-instance-brand]' in helper
    assert "data-node-reconnect-label" in DASHBOARD
