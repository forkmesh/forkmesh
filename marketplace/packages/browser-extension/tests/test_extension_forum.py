import json
import shutil
import subprocess
from pathlib import Path

import pytest

EXTENSION = Path(__file__).resolve().parents[1] / "content" / "extension"
NODE = shutil.which("node")

HARNESS = r"""
const fs = require("fs");
const vm = require("vm");
const opened = [];
const pages = {
  1: { generator: "Discourse 3.4.0", site: "Example Forum", origin: "https://forum.example.org" },
  2: { generator: "WordPress 6", site: "Blog", origin: "https://blog.example.org" },
  3: { generator: "Discourse", site: "Plain", origin: "http://insecure.example.org" },
};
const chrome = {
  scripting: {
    async executeScript({ target, func }) {
      const page = pages[target.tabId];
      if (!page) throw new Error("No access");
      const document = {
        title: page.site,
        querySelector(selector) {
          if (selector === 'meta[name="generator"]') return { content: page.generator };
          if (selector === 'meta[property="og:site_name"]') return { content: page.site };
          return null;
        },
      };
      const result = new Function("document", "location", `return (${func.toString()})();`)(document, new URL(page.origin));
      return [{ result }];
    },
  },
  tabs: { async create({ url }) { opened.push(url); } },
};
const context = { self: {}, chrome, URL, console };
context.self = context;
vm.createContext(context);
vm.runInContext(fs.readFileSync(process.argv[2], "utf8"), context);
const handlers = {};
context.ForkMeshForumBackground.install({ settings: async () => ({ baseUrl: "https://git.example.org" }), handlers });
(async () => {
  const out = {
    detect: [
      await handlers["fm-forum-detect"]({ tabId: 1 }),
      await handlers["fm-forum-detect"]({ tabId: 2 }),
      await handlers["fm-forum-detect"]({ tabId: 3 }),
      await handlers["fm-forum-detect"]({ tabId: 9 }),
      await handlers["fm-forum-detect"]({ tabId: "1" }),
    ],
    clean: ["https://forum.example.org/", "https://example.org/community/", "https://u:p@x.example.org",
            "javascript:alert(1)", "https://localhost", "https://example.org/a b"].map(context.ForkMeshForumBackground.cleanOrigin),
    mirror: [
      await handlers["fm-forum-mirror"]({ origin: "https://forum.example.org" }),
      await handlers["fm-forum-mirror"]({ origin: "file:///etc/passwd" }),
    ],
    opened,
  };
  process.stdout.write(JSON.stringify(out));
})();
"""


@pytest.mark.skipif(NODE is None, reason="node is not installed")
def test_the_popup_offers_a_mirror_only_on_discourse_pages(tmp_path):
    harness = tmp_path / "harness.js"
    harness.write_text(HARNESS, encoding="utf-8")
    result = subprocess.run([NODE, str(harness), str(EXTENSION / "forum.js")],
                            capture_output=True, text=True, check=True)
    out = json.loads(result.stdout)
    assert out["detect"] == [
        {"ok": True, "forum": {"origin": "https://forum.example.org", "title": "Example Forum"}},
        {"ok": True, "forum": None},
        {"ok": True, "forum": None},
        {"ok": True, "forum": None},
        {"ok": True, "forum": None},
    ]
    assert out["clean"] == ["https://forum.example.org", "https://example.org/community", "", "", "", "https://example.org"]
    assert out["mirror"] == [{"ok": True}, {"ok": False, "error": "invalid_forum"}]
    assert out["opened"] == [
        "https://git.example.org/start-discourse-mirror?forum=https%3A%2F%2Fforum.example.org"]


def test_the_forum_messages_answer_the_popup_only():
    background = (EXTENSION / "background.js").read_text(encoding="utf-8")
    assert 'importScripts("forum.js");' in background
    assert 'for (const type of ["fm-forum-detect", "fm-forum-mirror"]) POPUP_ONLY.add(type);' in background
    assert "self.ForkMeshForumBackground.install({ settings, handlers: HANDLERS });" in background
    popup = (EXTENSION / "popup.html").read_text(encoding="utf-8")
    assert '<script src="popup-forum.js"></script>' in popup and "data-forum-mirror" in popup
    forum = (EXTENSION / "forum.js").read_text(encoding="utf-8")
    assert "fetch(" not in forum and "host_permissions" not in forum
    assert "fetch(" not in (EXTENSION / "popup-forum.js").read_text(encoding="utf-8")
