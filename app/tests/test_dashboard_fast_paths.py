import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HELPERS = (ROOT / "public/dashboard/js/02-helpers.js").read_text()
CONTENT = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()


def _function(source, name):
    start = source.index(f"function {name}(")
    cursor = source.index("{", start)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _run(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


ICON_HARNESS = """
const SVG = "http://www.w3.org/2000/svg";
class Element {
  constructor(tag, attrs, ns) {
    this.tag = tag;
    this.namespaceURI = ns || "http://www.w3.org/1999/xhtml";
    this.attrs = new Map(Object.entries(attrs || {}));
    this.parentNode = null;
  }
  getAttribute(name) { return this.attrs.has(name) ? this.attrs.get(name) : null; }
  get attributes() { return [...this.attrs].map(([name, value]) => ({ name, value })); }
  get classList() {
    const list = String(this.getAttribute("class") || "").split(" ").filter(Boolean);
    return { contains: (name) => list.includes(name) };
  }
}
const root = { children: [], replaceChild(next, old) {
  this.children[this.children.indexOf(old)] = next;
  next.parentNode = this;
  old.parentNode = null;
} };
const add = (element) => { element.parentNode = root; root.children.push(element); return element; };
const document = { querySelectorAll: () => root.children.filter((el) => el.getAttribute("data-lucide") !== null) };
let built = 0;
let fullCalls = 0;
const window = { lucide: {
  icons: { GitBranch: ["svg", { width: 24 }, []], Star: ["svg", { width: 24 }, []] },
  createElement: ([tag, attrs]) => { built += 1; return new Element(tag, attrs, SVG); },
  createIcons: () => { fullCalls += 1; },
} };
"""


def test_icons_already_drawn_are_not_drawn_again():
    script = ICON_HARNESS + _function(HELPERS, "installIncrementalIcons") + """
installIncrementalIcons();
installIncrementalIcons();
add(new Element("i", { "data-lucide": "git-branch", class: "h-4 w-4" }));
window.lucide.createIcons();
const first = built;
window.lucide.createIcons();
const second = built;
add(new Element("i", { "data-lucide": "star" }));
window.lucide.createIcons();
const third = built;
root.children[0].attrs.set("data-lucide", "star");
window.lucide.createIcons();
window.lucide.createIcons({ icons: {} });
console.log(JSON.stringify({
  first, second, third, swapped: built, fullCalls,
  svg: root.children[0].namespaceURI === SVG,
  classes: root.children[1].getAttribute("class"),
  kept: root.children[0].getAttribute("data-lucide"),
}));
"""
    result = _run(script)
    assert result["first"] == 1
    assert result["second"] == 1
    assert result["third"] == 2
    assert result["swapped"] == 3
    assert result["fullCalls"] == 1
    assert result["svg"] is True
    assert result["classes"] == "lucide lucide-star"
    assert result["kept"] == "star"


def test_pull_reads_skip_files_the_pull_folder_does_not_list():
    functions = "\n".join((
        _function(CONTENT, "readRepoPullFolder"),
        "async " + _function(CONTENT, "repoPullFolderLacks"),
    ))
    script = """
const reads = [];
const repoKey = (repo) => repo.name;
const repoLiveUrl = (repo, op, params) => `${op}:${params.path}@${params.ref}`;
const immutableGitCommit = (value) => (/^[0-9a-f]{40}$/.test(value) ? value : "");
const fetchRepoJson = async (url) => {
  reads.push(url);
  if (url.includes("/404")) throw new Error("not_found");
  return { entries: [{ name: "pull.md" }, { name: "0001-comment.md" }] };
};
const repoPullFolderReads = new Map();
""" + functions + """
const repo = { name: "r" };
const commit = "a".repeat(40);
const result = {
  mbox: await repoPullFolderLacks(repo, 320, commit, "commits.mbox"),
  pull: await repoPullFolderLacks(repo, 320, commit, "pull.md"),
  named: await repoPullFolderLacks(repo, 320, "main", "commits.mbox"),
  missing: await repoPullFolderLacks(repo, 404, commit, "commits.mbox"),
};
result.missingAgain = await repoPullFolderLacks(repo, 404, commit, "commits.mbox");
result.reads = reads;
console.log(JSON.stringify(result));
"""
    result = _run(script)
    assert result["mbox"] is True
    assert result["pull"] is False
    assert result["named"] is False
    assert result["missing"] is False
    assert result["missingAgain"] is False
    commit = "a" * 40
    assert result["reads"] == [
        f"tree:.forkmesh/pulls/320@{commit}",
        f"tree:.forkmesh/pulls/404@{commit}",
        f"tree:.forkmesh/pulls/404@{commit}",
    ]
