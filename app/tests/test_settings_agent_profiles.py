#!/usr/bin/env python3

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js
from _dashboard_shell import assembled_dashboard_page, deferred_dialogs

ROOT = Path(__file__).resolve().parents[1]
SETTINGS_VIEW = (
    ROOT / "public" / "dashboard" / "partials" / "views" / "settings.html"
).read_text(encoding="utf-8")
FRAGMENT = (
    ROOT / "public" / "dashboard" / "js" / "09-agent-profiles.js"
).read_text(encoding="utf-8")
RUNNERS_JS = (
    ROOT / "public" / "dashboard" / "js" / "09-runners.js"
).read_text(encoding="utf-8")
TERMINAL = ROOT / "public" / "agent-terminal.js"
DASHBOARD_JS = assembled_dashboard_js()


def function_source(source, name):
    match = re.search(r"\n  (?:async )?function %s\(" % re.escape(name), source)
    assert match, name
    end = source.find("\n  }\n", match.end())
    return source[match.start():end + 4]


def agents_panel():
    start = SETTINGS_VIEW.index('data-settings-section="agents"')
    return SETTINGS_VIEW[start:SETTINGS_VIEW.index("</section>", start)]


def test_the_settings_page_has_an_agents_and_runners_tab():
    assert 'data-settings-section-link="agents"' in SETTINGS_VIEW
    panel = agents_panel()
    for hook in (
        "data-agent-profiles-desktops",
        "data-agent-profiles-refresh",
        "data-agent-profiles-notice",
        "data-runners-list",
        "data-runners-notice",
        "data-runners-auth-banner",
        "data-runner-login-form",
        "data-runner-login-cancel",
        "data-runner-login-close",
        "data-runner-login-output",
        "data-settings-runners-refresh",
    ):
        assert hook in panel, hook
    assert "data-runners-panel" not in panel
    page = assembled_dashboard_page("settings")
    assert 'data-settings-section="agents"' in page
    assert "data-agent-login-modal" in deferred_dialogs()


def test_the_tab_is_a_known_section_that_loads_desktops_and_runners():
    assert '"danger", "apps", "agents", "reactions", "ai-review"];' in DASHBOARD_JS
    assert 'if (activeSection === "agents") initSettingsAgents();' in DASHBOARD_JS
    init = function_source(FRAGMENT, "initSettingsAgents")
    assert "loadAgentProfiles()" in init
    assert "initSettingsRunners()" in init
    runners = function_source(RUNNERS_JS, "initSettingsRunners")
    assert "wireRunnerLoginPanel();" in runners
    assert "loadRunners()" in runners
    page = function_source(RUNNERS_JS, "initRunnersPage")
    assert "wireRunnerLoginPanel();" in page
    wire = function_source(RUNNERS_JS, "wireRunnerLoginPanel")
    assert 'panel.dataset.wired === "1"' in wire


def test_desktop_accounts_use_the_control_channel_methods():
    for method in (
        '"status"',
        '"agentops.usage"',
        '"repos.list"',
        '"terminal.list"',
        '"terminal.close"',
        '"agentops.accounts.select"',
        '"agentops.accounts.rename"',
        '"agentops.accounts.add"',
    ):
        assert method in FRAGMENT, method
    for capability in ('"agentops.accounts"', '"terminal.accounts"', '"terminal.agents"'):
        assert capability in FRAGMENT, capability
    for mode in ('mode: "login"', 'mode: "logout"', 'mode: "agent"'):
        assert mode in FRAGMENT, mode
    add = function_source(FRAGMENT, "addAgentProfileAccount")
    assert 'mode: "login", account' in add
    opener = function_source(FRAGMENT, "openAgentProfileTerminal")
    assert 'import("/agent-terminal.js")' in opener
    assert "createDesktopControlTerminal" in opener
    assert "detachOnClose: !signIn" in opener
    assert "showAgentLoginModal(" in opener
    assert "desktopControlHub()" in function_source(FRAGMENT, "connectAgentProfileDesktop")


def test_the_fragment_is_in_the_core_bundle():
    assert "function initSettingsAgents()" in DASHBOARD_JS
    assert "function showAgentLoginModal(" in DASHBOARD_JS
    assert "function runnerUsageRow(" in DASHBOARD_JS


HARNESS = r'''
import { readFileSync } from "node:fs";
const source = readFileSync(process.argv[2], "utf8");
const url = "data:text/javascript;base64," + Buffer.from(source).toString("base64");

function element() {
  const node = {
    children: [], listeners: {}, style: {}, attrs: {}, className: "", textContent: "", innerHTML: "",
    scrollTop: 0, scrollHeight: 0,
    setAttribute(name, value) { this.attrs[name] = value; },
    appendChild(child) { this.children.push(child); child.parent = this; return child; },
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    getBoundingClientRect() { return { width: 808, height: 408 }; },
    remove() {},
    focus() { node.focused = true; },
  };
  return node;
}
globalThis.document = { createElement: () => element() };

const { createDesktopControlTerminal } = await import(url);

function fakeConnection() {
  const listeners = new Map();
  const calls = [];
  let answer = null;
  return {
    calls,
    isOpen: true,
    respond(fn) { answer = fn; },
    emit(event, data) { for (const fn of listeners.get(event) || []) fn(data); },
    on(event, fn) {
      if (!listeners.has(event)) listeners.set(event, new Set());
      listeners.get(event).add(fn);
      return () => listeners.get(event).delete(fn);
    },
    onClose() { return () => {}; },
    request(method, params) {
      calls.push([method, params]);
      return Promise.resolve(answer ? answer(method, params) : {});
    },
  };
}

const b64 = (text) => Buffer.from(text).toString("base64");
const timers = { setTimeoutImpl: (fn) => { fn(); return 1; }, clearTimeoutImpl: () => {} };
const result = {};

{
  const connection = fakeConnection();
  const surface = element();
  const exits = [];
  connection.respond((method) => {
    if (method === "terminal.start") {
      connection.emit("terminal.output", { terminal: "t1", data: b64("\x1b[?25learly ") });
      return { terminal: "t1", replay: "" };
    }
    return {};
  });
  const terminal = createDesktopControlTerminal({
    surface, connection, label: "Log in", start: { provider: "claude-code", mode: "login", account: "a1" },
    onExit: (code) => exits.push(code), requestAnimationFrameImpl: null, ...timers,
  });
  await terminal.start();
  connection.emit("terminal.output", { terminal: "other", data: b64("ignored") });
  connection.emit("terminal.output", { terminal: "t1", data: b64("paste the code") });
  const view = surface.children[0];
  result.start = connection.calls[0];
  result.text = view.innerHTML;
  view.listeners.keydown[0]({ key: "x", ctrlKey: false, altKey: false, metaKey: false, preventDefault() {} });
  result.input = connection.calls.find(([method]) => method === "terminal.input");
  connection.emit("terminal.exit", { terminal: "t1", code: 0 });
  result.exits = exits;
  terminal.close();
  result.closedAfterExit = connection.calls.some(([method]) => method === "terminal.close");
}

{
  const connection = fakeConnection();
  connection.respond((method) => (method === "terminal.start" ? { terminal: "t2" } : {}));
  const terminal = createDesktopControlTerminal({
    surface: element(), connection, start: { provider: "codex", mode: "agent", owner: "o", name: "r" },
    detachOnClose: true, requestAnimationFrameImpl: null, ...timers,
  });
  await terminal.start();
  terminal.close();
  result.agentClose = connection.calls.at(-1);
}

{
  const connection = fakeConnection();
  connection.respond((method) => (method === "terminal.start" ? { terminal: "t3" } : {}));
  const terminal = createDesktopControlTerminal({
    surface: element(), connection, start: { provider: "claude-code", mode: "logout" },
    requestAnimationFrameImpl: null, ...timers,
  });
  await terminal.start();
  terminal.close();
  result.signInClose = connection.calls.at(-1);
}

{
  const connection = fakeConnection();
  const surface = element();
  connection.respond((method) => (method === "terminal.attach" ? { terminal: "t4", replay: b64("before") } : {}));
  const terminal = createDesktopControlTerminal({
    surface, connection, attach: "t4", detachOnClose: true, requestAnimationFrameImpl: null, ...timers,
  });
  await terminal.start();
  result.attach = connection.calls[0];
  result.replay = surface.children[0].innerHTML;
}

{
  const connection = fakeConnection();
  connection.request = (method) => {
    connection.calls.push([method]);
    const error = new Error("missing");
    error.code = "cli_missing";
    return Promise.reject(error);
  };
  const terminal = createDesktopControlTerminal({
    surface: element(), connection, start: { provider: "codex", mode: "login" },
    requestAnimationFrameImpl: null, ...timers,
  });
  try {
    await terminal.start();
    result.refused = "";
  } catch (error) {
    result.refused = error.code;
  }
}

process.stdout.write(JSON.stringify(result));
'''


@pytest.mark.skipif(shutil.which("node") is None, reason="node is required for this test")
def test_the_desktop_terminal_streams_a_control_channel_terminal():
    completed = subprocess.run(
        ["node", "--input-type=module", "-", str(TERMINAL)],
        input=HARNESS, text=True, capture_output=True, check=True,
    )
    result = json.loads(completed.stdout)
    method, params = result["start"]
    assert method == "terminal.start"
    assert params["provider"] == "claude-code"
    assert params["mode"] == "login"
    assert params["account"] == "a1"
    assert params["cols"] >= 20 and params["rows"] >= 5
    assert "early paste the code" in result["text"]
    assert "ignored" not in result["text"]
    assert result["input"][1] == {"terminal": "t1", "data": "eA=="}
    assert result["exits"] == [0]
    assert result["closedAfterExit"] is False
    assert result["agentClose"] == ["terminal.detach", {"terminal": "t2"}]
    assert result["signInClose"] == ["terminal.close", {"terminal": "t3"}]
    assert result["attach"][0] == "terminal.attach"
    assert result["attach"][1]["terminal"] == "t4"
    assert "before" in result["replay"]
    assert result["refused"] == "cli_missing"
