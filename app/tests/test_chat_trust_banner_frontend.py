#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT_TEXT = (PUBLIC / "chat.js").read_text(encoding="utf-8")


def _function_source(name):
    match = re.search(
        r"^(?:async )?function " + re.escape(name) + r"\(.*?^}\n", CHAT_TEXT,
        re.S | re.M)
    assert match, name
    return match.group(0)


def _decide(button, fail_on_kid=""):
    script = f"""
const document = {{ body: {{ id: "body" }}, activeElement: null }};
document.activeElement = document.body;
function element(id, parent = null) {{
  let hidden = false;
  let disabled = false;
  const node = {{
    id, parent, textContent: "",
    get hidden() {{ return hidden; }},
    set hidden(value) {{
      hidden = Boolean(value);
      if (hidden && this.contains(document.activeElement)) document.activeElement = document.body;
    }},
    get disabled() {{ return disabled; }},
    set disabled(value) {{
      disabled = Boolean(value);
      if (disabled && document.activeElement === this) document.activeElement = document.body;
    }},
    dataset: {{}},
    contains(other) {{
      for (let at = other; at; at = at.parent) if (at === this) return true;
      return false;
    }},
    focus() {{
      for (let at = this; at; at = at.parent) if (at.hidden || at.disabled) return;
      document.activeElement = this;
    }},
  }};
  return node;
}}
const input = element("chat-input");
const trustBanner = element("chat-trust-banner");
const trustBannerCopy = element("chat-trust-banner-copy", trustBanner);
const trustApproveBtn = element("chat-trust-approve", trustBanner);
const trustBlockBtn = element("chat-trust-block", trustBanner);
const sealPanel = null;
const activeChannel = "dm:alex";
let pending = [{{ kid: "k1", acct: "alex" }}, {{ kid: "k2", acct: "alex" }}];
const decided = [];
function notePendingDevice() {{}}
function renderSafetyNumber() {{}}
function trustDeviceOwner() {{ return "Alex"; }}
function activeTrustTransport() {{
  const decide = async (kid) => {{
    await Promise.resolve();
    if (kid === {json.dumps(fail_on_kid)}) throw new Error("storage full");
    decided.push(kid);
    pending = pending.filter((record) => record.kid !== kid);
    return true;
  }};
  return {{ pendingDevices: () => pending.slice(), approveDevice: decide, blockDevice: decide }};
}}
{_function_source("renderTrustBanner")}
{_function_source("decideTrustBanner")}
renderTrustBanner();
const button = {json.dumps(button)} === "approve" ? trustApproveBtn : trustBlockBtn;
button.focus();
let threw = false;
try {{
  await decideTrustBanner({json.dumps(button)} === "approve");
}} catch (_) {{
  threw = true;
}}
console.log(JSON.stringify({{
  threw,
  decided,
  hidden: trustBanner.hidden,
  copy: trustBannerCopy.textContent,
  approveLabel: trustApproveBtn.textContent,
  approveDisabled: trustApproveBtn.disabled,
  focus: document.activeElement.id,
}}));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True, cwd=PUBLIC)
    return json.loads(result.stdout)


def test_approve_all_moves_keyboard_focus_to_the_composer():
    state = _decide("approve")
    assert state["decided"] == ["k1", "k2"]
    assert state["hidden"] is True
    assert state["focus"] == "chat-input"


def test_block_all_moves_keyboard_focus_to_the_composer():
    state = _decide("block")
    assert state["hidden"] is True
    assert state["focus"] == "chat-input"


def test_failed_approve_says_so_and_keeps_focus_on_the_button():
    state = _decide("approve", fail_on_kid="k2")
    assert state["threw"] is False
    assert state["decided"] == ["k1"]
    assert state["hidden"] is False
    assert state["copy"].startswith("Approving did not finish. Try again")
    assert "A new device for Alex wants to join this chat." in state["copy"]
    assert state["approveLabel"] == "Approve"
    assert state["approveDisabled"] is False
    assert state["focus"] == "chat-trust-approve"


def test_failed_block_says_so_and_keeps_focus_on_the_button():
    state = _decide("block", fail_on_kid="k1")
    assert state["hidden"] is False
    assert state["copy"].startswith("Blocking did not finish.")
    assert "2 new devices for Alex want to join this chat." in state["copy"]
    assert state["focus"] == "chat-trust-block"
