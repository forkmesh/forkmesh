import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEMPLATE = (REPO / "www" / "templates" / "pages" / "index.html").read_text(
    encoding="utf-8")
PAGE = (REPO / "www" / "public" / "index.html").read_text(encoding="utf-8")
NODE = shutil.which("node")


def section(text):
    start = text.index('<section id="instances"')
    return text[start:text.index("</section>", start)]


def script(text):
    marker = 'document.querySelector("[data-instances-section]")'
    start = text.rindex("<script>", 0, text.index(marker)) + len("<script>")
    return text[start:text.index("</script>", start)]


def test_the_section_lists_owasp_blt_and_an_add_your_instance_box():
    html = section(TEMPLATE)
    assert '<h2 id="instances-title">Federated instances</h2>' in html
    assert 'aria-label="Federated instances" data-instances-list>' in html
    blt = html.split('<li data-instances-pinned="git.owaspblt.org">', 1)[1].split("</li>", 1)[0]
    assert '<a class="inst-card" href="https://git.owaspblt.org/BLT" rel="noopener">' in blt
    assert '<img src="/assets/owasp-blt-logo.svg" alt=""' in blt
    assert "OWASP BLT" in blt
    add = html.split("<li data-instances-add>", 1)[1].split("</li>", 1)[0]
    assert re.search(r'<a class="inst-card inst-add" href="/start"[^>]*>', add)
    assert 'data-forkmesh-product-action="home-start-instance"' in add
    assert "Add your instance" in add
    assert "data-instances-empty" not in html and "inst-start" not in html
    assert "data-instances-section hidden" in html
    logo = (REPO / "www" / "public" / "assets" / "owasp-blt-logo.svg").read_text(encoding="utf-8")
    assert logo.startswith("<svg") and not re.search(r"<script|\son\w+=|href=", logo, re.I)


def test_the_rendered_homepage_matches_the_template():
    assert section(PAGE) == section(TEMPLATE)
    assert script(PAGE) == script(TEMPLATE)


def test_the_add_box_is_a_dashed_card_not_a_second_green_cta():
    css = TEMPLATE.split("#instances .inst-add {", 1)[1].split("}", 1)[0]
    assert "border-style: dashed" in css
    assert "#3fb950" not in css and "cta" not in css
    online = TEMPLATE.split(
        '#instances .inst-status[data-state="online"] .inst-dot {', 1)[1]
    assert "#3fb950" in online.split("}", 1)[0]


def test_the_script_reads_the_public_directory_without_credentials():
    code = script(TEMPLATE)
    assert 'fetch("/api/start/instances", { headers: { accept: ' \
        '"application/json" }, credentials: "omit" })' in code
    assert "instance.listed === true" in code
    assert 'url.protocol === "https:"' in code
    assert 'instance.status === "online"' in code
    assert "innerHTML" not in code


DOM = r"""
const vm = await import("node:vm");
const payload = (__PAYLOAD__);
class Node {
  constructor(tag, attrs = {}) {
    this.tagName = tag; this.children = []; this.attrs = { ...attrs };
    this.dataset = {}; this.hidden = "hidden" in attrs; this.className = "";
    this.text = "";
  }
  getAttribute(name) { return name in this.attrs ? this.attrs[name] : null; }
  set textContent(value) { this.text = String(value); this.children = []; }
  get textContent() {
    return this.text + this.children.map((c) => c.textContent).join("");
  }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this.children = nodes; }
  setAttribute(name, value) { this.attrs[name] = value; }
  querySelector(selector) {
    const attr = selector.slice(1, -1);
    const stack = [...this.children];
    while (stack.length) {
      const node = stack.shift();
      if (attr in node.attrs) return node;
      stack.push(...node.children);
    }
    return null;
  }
}
const section = new Node("section", { "data-instances-section": "", hidden: "" });
const list = new Node("ul", { "data-instances-list": "" });
const pinned = new Node("li", { "data-instances-pinned": "git.owaspblt.org" });
pinned.append(new Node("a"));
const add = new Node("li", { "data-instances-add": "" });
add.append(new Node("a"));
list.append(pinned, add);
section.append(list);
const brand = __INSTANCE__ ? new Node("script", { "data-instance-brand": "" }) : null;
const document = {
  querySelector: (selector) => selector === "[data-instances-section]" ? section
    : selector === "script[data-instance-brand]" ? brand : null,
  createElement: (tag) => new Node(tag),
  createTextNode: (text) => { const node = new Node("#text"); node.text = text; return node; },
};
const fetch = async () => payload === null
  ? Promise.reject(new Error("offline"))
  : { ok: true, json: async () => payload };
vm.runInNewContext(__SCRIPT__, { document, fetch, URL, String, Array, Boolean });
await new Promise((resolve) => setTimeout(resolve, 20));
console.log(JSON.stringify({
  hidden: section.hidden, addHidden: add.hidden,
  order: list.children.map((item) => item === pinned ? "blt" : item === add ? "add" : "card"),
  cards: list.children.filter((item) => item !== pinned && item !== add).map((item) => {
    const link = item.children[0];
    const status = link.children[1].children[2];
    return { href: link.href, text: link.textContent, state: status.dataset.state };
  }),
}));
"""


def run(payload, instance=False):
    if not NODE:
        pytest.skip("node is not installed")
    code = (DOM.replace("__PAYLOAD__", json.dumps(payload))
            .replace("__INSTANCE__", "true" if instance else "false")
            .replace("__SCRIPT__", json.dumps(script(TEMPLATE))))
    result = subprocess.run([NODE, "--input-type=module", "-e", code],
                            capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


ENTRIES = {"ok": True, "instances": [
    {"id": "a" * 32, "name": "Acme Code", "url": "https://code.example.org/",
     "logo": "", "listed": True, "world": True, "status": "online"},
    {"id": "b" * 32, "name": "World only", "url": "https://w.example.org/",
     "listed": False, "world": True, "status": "online"},
    {"id": "c" * 32, "name": "New one", "url": "https://new.example.org/",
     "listed": True, "status": "checking"},
    {"id": "d" * 32, "name": "Bad", "url": "javascript:alert(1)",
     "listed": True},
    {"id": "e" * 32, "name": "OWASP BLT", "url": "https://git.owaspblt.org/",
     "listed": True, "status": "online"},
]}


def test_listed_instances_show_their_name_address_and_status():
    shown = run(ENTRIES)
    assert shown["hidden"] is False and shown["addHidden"] is False
    assert shown["order"] == ["blt", "card", "card", "add"]
    assert [(card["href"], card["state"]) for card in shown["cards"]] == [
        ("https://code.example.org/", "online"),
        ("https://new.example.org/", "checking")]
    assert "Acme Codecode.example.orgOnline" in shown["cards"][0]["text"]
    assert shown["cards"][1]["text"].endswith("Checking")


def test_forkmesh_com_shows_blt_and_the_add_box_even_when_empty():
    shown = run({"ok": True, "instances": []})
    assert shown["hidden"] is False and shown["addHidden"] is False
    assert shown["order"] == ["blt", "add"]
    failed = run(None)
    assert failed["hidden"] is False and failed["order"] == ["blt", "add"]


def test_an_instance_homepage_hides_the_empty_section_and_the_add_box():
    shown = run({"ok": True, "instances": []}, instance=True)
    assert shown["hidden"] is True and shown["addHidden"] is True
    assert run(None, instance=True)["hidden"] is True
