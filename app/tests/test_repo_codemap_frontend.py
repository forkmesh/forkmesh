#!/usr/bin/env python3
"""The repository code map (adhoc #187).

public/assets/codemap/codemap.js draws a whole repository as a zoomable
treemap (and a 3D city built from the same layout). Everything a reader trusts
in that picture comes from three pure functions, so they are run here in Node
rather than only matched as source text:

  * the squarified layout gives every file an area proportional to its bytes,
    keeps each child inside its parent, and never overlaps siblings,
  * 3D elevation makes a folder's thickness follow its size and stacks every
    child on top of its parent,
  * the tokenizer keeps each language's comment and string rules apart.

The wiring (button, lazy script, served paths) is asserted as source text.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[1]
CODEMAP_DIR = ROOT / "public" / "assets" / "codemap"
CODEMAP = CODEMAP_DIR / "codemap.js"
DASHBOARD_JS = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
TAILWIND_CSS = (ROOT / "public" / "dashboard" / "tailwind.css").read_text(
    encoding="utf-8")

HARNESS = r'''
const map = require(process.argv[process.argv.length - 1]);

function file(dir, name, size) {
  return { name, path: dir ? `${dir}/${name}` : name, type: "file", size };
}
const raw = {
  size: 0,
  children: [
    { name: "src", type: "directory", children: [
      file("src", "big.py", 40000), file("src", "mid.py", 10000),
      { name: "deep", type: "directory", children: [
        file("src/deep", "a.js", 5000), file("src/deep", "b.js", 5000),
        file("src/deep", "empty.txt", 0),
      ] },
      { name: "…", type: "summary", size: 3000 },
    ] },
    { name: "docs", type: "directory", children: [file("docs", "guide.md", 20000)] },
    file("", "README.md", 2000),
  ],
};
(function total(node) {
  if (!node.children) return node.size;
  node.size = node.children.reduce((sum, child) => sum + total(child), 0);
  return node.size;
})(raw);

const root = map.buildTree(raw, "fixture");
const ranked = map.measureTree(root);
map.layoutTree(root, 1600, 1000);
map.elevateTree(root, 20);

const nodes = [];
(function walk(node) { nodes.push(node); (node.children || []).forEach(walk); })(root);
const view = (node) => ({
  path: node.path, type: node.type, size: node.size, weight: node.weight, lang: node.lang,
  x: node.x, y: node.y, w: node.w, h: node.h, band: node.band, base: node.base, top: node.top,
  parent: node.parent ? nodes.indexOf(node.parent) : -1,
});

const tokens = (text, lang) => {
  const code = map.prepareCode(text, lang);
  return code.lines.map((_, index) => map.tokenizeLine(code, index).map((t) => [t[0], t[1], t[2]]));
};
const big = map.prepareCode(Array.from({ length: 30000 }, (_, i) => `line ${i}`).join("\n"), "Text");

console.log(JSON.stringify({
  nodes: nodes.map(view),
  ranked,
  js: tokens('const n = call(1); // note\n/* open\nstill */ let s = "a//b";', "JavaScript"),
  py: tokens('x = 7 // 2  # floor\n"""Doc\nbody"""\nreturn x', "Python"),
  md: tokens("# Title don't", "Markdown"),
  bigLines: big.lines.length,
  bigTotal: big.totalLines,
  languages: ["a.cpp", "CMakeLists.txt", "Makefile", "x.weird", "noext"].map(map.languageOf),
  lods: [0.01, 0.9, 3, 14, 40].map(map.pickLod),
  threeUrl: map.THREE_URL,
  previewKinds: ["logo.PNG", "icon.svg", "Inter.woff2", "pkg.zip", "clip.mp4", "main.py", "notes.md"]
    .map(map.previewKindOf),
  categories: ["pkg.tar", "clip.mp4", "doc.pdf", "model.glb", "blob.xyz"].map(map.fileCategoryOf),
  wide: map.previewArea({ w: 200, h: 100, band: 5 }, 4),
  tall: map.previewArea({ w: 200, h: 100, band: 5 }, 0.5),
}));
'''

pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable")


@pytest.fixture(scope="module")
def result():
    completed = subprocess.run(
        ["node", "-e", HARNESS, str(CODEMAP)],
        check=True, capture_output=True, text=True, timeout=60,
    )
    return json.loads(completed.stdout)


def _children(result, index):
    return [n for n in result["nodes"] if n["parent"] == index]


def test_file_area_is_proportional_to_its_bytes(result):
    files = [n for n in result["nodes"] if n["type"] == "file" and n["size"] > 0]
    by_parent = {}
    for node in files:
        by_parent.setdefault(node["parent"], []).append(node)
    checked = 0
    for siblings in by_parent.values():
        for a in siblings:
            for b in siblings:
                if a is b:
                    continue
                assert (a["w"] * a["h"]) / (b["w"] * b["h"]) == pytest.approx(
                    a["weight"] / b["weight"], rel=1e-6)
                checked += 1
    assert checked >= 4


def test_every_child_sits_inside_its_parent_below_the_label_band(result):
    nodes = result["nodes"]
    eps = 1e-6
    for node in nodes:
        if node["parent"] < 0:
            assert (node["x"], node["y"], node["w"], node["h"]) == (0, 0, 1600, 1000)
            continue
        parent = nodes[node["parent"]]
        assert node["x"] >= parent["x"] - eps
        assert node["y"] >= parent["y"] + parent["band"] - eps
        assert node["x"] + node["w"] <= parent["x"] + parent["w"] + eps
        assert node["y"] + node["h"] <= parent["y"] + parent["h"] + eps


def test_siblings_never_overlap(result):
    nodes = result["nodes"]
    eps = 1e-6
    for index in range(len(nodes)):
        siblings = _children(result, index)
        for i, a in enumerate(siblings):
            for b in siblings[i + 1:]:
                apart = (
                    a["x"] + a["w"] <= b["x"] + eps or b["x"] + b["w"] <= a["x"] + eps
                    or a["y"] + a["h"] <= b["y"] + eps or b["y"] + b["h"] <= a["y"] + eps
                )
                assert apart, (a["path"], b["path"])


def test_empty_files_and_folded_entries_stay_visible(result):
    empty = next(n for n in result["nodes"] if n["path"] == "src/deep/empty.txt")
    assert empty["w"] > 0 and empty["h"] > 0
    more = next(n for n in result["nodes"] if n["type"] == "more")
    # The host's "…" overflow keeps its bytes, under the directory it folds.
    assert more["path"] == "src" and more["size"] == 3000
    assert more["w"] * more["h"] > 0


def test_folder_height_follows_folder_size_and_children_stack_on_parents(result):
    nodes = result["nodes"]
    dirs = {n["path"]: n for n in nodes if n["type"] == "dir"}
    thickness = {path: n["top"] - n["base"] for path, n in dirs.items()}
    # src (63 KB) > docs (20 KB) > src/deep (10 KB)
    assert thickness["src"] > thickness["docs"] > thickness["src/deep"] > 0
    # Thickness is the square root of the folder's share of the repository.
    assert thickness["src"] / thickness["docs"] == pytest.approx(
        (dirs["src"]["size"] / dirs["docs"]["size"]) ** 0.5, rel=1e-6)
    for node in nodes:
        if node["parent"] >= 0:
            assert node["base"] == pytest.approx(nodes[node["parent"]]["top"])
            assert node["top"] > node["base"]


def test_languages_are_ranked_by_bytes_for_fixed_order_colors(result):
    assert [lang for lang, _ in result["ranked"]][:3] == ["Python", "Markdown", "JavaScript"]
    assert result["languages"] == ["C++", "CMake", "Shell", ".weird", "Other"]
    src = next(n for n in result["nodes"] if n["path"] == "src")
    assert src["lang"] == "Python"  # a folder takes its dominant language


def test_javascript_comments_strings_calls_and_blocks(result):
    first, second, third = result["js"]
    assert [0, "const", 1] in first
    assert [10, "call", 5] in first
    assert [15, "1", 4] in first
    assert first[-1] == [19, "// note", 3]
    assert second == [[0, "/* open", 3]]
    # The block closes mid-line; a // inside a string is not a comment.
    assert third[0] == [0, "still */", 3]
    assert [9, "let", 1] in third
    assert third[-2] == [17, '"a//b"', 2]


def test_python_floor_division_is_not_a_comment_and_docstrings_are(result):
    first, second, third, fourth = result["py"]
    assert [6, "/", 6] in first and [7, "/", 6] in first
    assert first[-1] == [12, "# floor", 3]
    assert second == [[0, '"""Doc', 3]]
    assert third == [[0, 'body"""', 3]]
    assert [0, "return", 1] in fourth


def test_prose_has_no_keywords_strings_or_hash_comments(result):
    classes = {cls for _, _, cls in result["md"][0]}
    assert classes <= {0, 6}


def test_huge_files_are_capped_but_report_their_real_length(result):
    assert result["bigLines"] == 20000
    assert result["bigTotal"] == 30000


def test_level_of_detail_ladder(result):
    # Far cells use a fractional density level; past the ladder, 0 means the
    # 2D view draws live text instead of a bitmap.
    assert result["lods"] == [0.125, 1, 4, 16, 0]


def test_three_is_vendored_beside_the_viewer_under_assets():
    # /assets/* bypasses the Worker's /owner/repo router and is what a mirror
    # maps onto app/public/assets, so both files must live there.
    vendor = CODEMAP_DIR / "vendor" / "three-0.184.0"
    module = (vendor / "three.module.min.js").read_text(encoding="utf-8")
    assert 'from"./three.core.min.js"' in module
    assert (vendor / "three.core.min.js").is_file()
    assert "MIT" in (vendor / "LICENSE").read_text(encoding="utf-8")


def test_viewer_resolves_three_relative_to_itself(result):
    assert result["threeUrl"] == "/assets/codemap/vendor/three-0.184.0/three.module.min.js"


def test_repository_toolbar_offers_the_code_map():
    assert DASHBOARD_JS.count("data-repo-codemap-open") >= 3  # About + file view + handler
    assert 'const REPO_CODEMAP_SCRIPT = "/assets/codemap/codemap.js?v=' in DASHBOARD_JS
    assert 'fetchRepoJson(repoLiveUrl(repo, "sizes"))' in DASHBOARD_JS
    # The map's reads are pinned to the commit the size tree was built from.
    assert "const commit = immutableGitCommit(data.commit);" in DASHBOARD_JS
    # The toolbar grew a column; the committed stylesheet must carry the class.
    assert "md:grid-cols-[auto_minmax(0,1fr)_auto_auto_auto]" in DASHBOARD_JS
    assert r"auto_minmax\(0\2c 1fr\)_auto_auto_auto\]" in TAILWIND_CSS


def test_non_code_files_preview_as_pictures_or_type_cards(result):
    # Images, fonts and videos load their bytes for a picture; other binaries show a
    # card naming their type and are never fetched; source stays text.
    assert result["previewKinds"] == ["image", "image", "font", "card", "video", "", ""]
    assert result["categories"] == ["Archive", "Video", "Document", "3D model", "Binary"]


def test_previews_keep_their_aspect_inside_the_cell_content_box(result):
    inset = 100 * 0.035
    box_w, box_h = 200 - inset * 2, 100 - 5 - inset * 2
    wide, tall = result["wide"], result["tall"]
    assert wide["cw"] == pytest.approx(box_w) and wide["cw"] / wide["ch"] == pytest.approx(4)
    assert wide["oy"] == pytest.approx(5 + inset + (box_h - wide["ch"]) / 2)
    assert tall["ch"] == pytest.approx(box_h) and tall["cw"] / tall["ch"] == pytest.approx(0.5)
    assert tall["ox"] == pytest.approx(inset + (box_w - tall["cw"]) / 2)


def test_code_map_reads_images_raw_at_the_pinned_commit():
    assert 'repoLiveUrl(repo, "raw", { path, ...refParams })' in DASHBOARD_JS


LOADER_HARNESS = r'''
const map = require(process.argv[process.argv.length - 1]);
const node = (path, size) => ({ path, name: path.split("/").pop(), size, cs: 0, pk: "", lang: "JSON" });
const files = [node("a.json", 10), node("b.json", 10), node("c.json", 10), node("bin.dat", 10),
               node("gone.json", 10), node("flaky.json", 10)];
const calls = [];
let flaky = 0;
const fetchBlobs = async (paths) => {
  calls.push(paths.slice());
  if (paths.length > 1 && calls.length === 1) throw new Error("timeout");
  const out = {};
  paths.forEach((path) => {
    if (path === "bin.dat") out[path] = { binary: true };
    else if (path === "gone.json") out[path] = { missing: true };
    else if (path === "flaky.json") out[path] = (flaky += 1) < 2 ? { missing: true } : { text: "{\"ok\":1}" };
    else out[path] = { text: "{\"k\":\"ایران\"}" };
  });
  return out;
};
const loader = map.createLoader(fetchBlobs, null, null, () => {}, () => {}, null);
loader.want(files);
const until = Date.now() + 8000;
(function wait() {
  if (files.every((f) => f.cs >= 2) || Date.now() > until) {
    console.log(JSON.stringify({ states: Object.fromEntries(files.map((f) => [f.path, f.cs])), calls: calls.length }));
    loader.close();
    return;
  }
  setTimeout(wait, 20);
})();
'''


def test_a_failed_or_partial_batch_is_retried_never_shown_as_binary(tmp_path):
    harness = tmp_path / "loader.js"
    harness.write_text(LOADER_HARNESS, encoding="utf-8")
    completed = subprocess.run(["node", str(harness), str(CODEMAP)], check=True,
                               capture_output=True, text=True, timeout=60)
    out = json.loads(completed.stdout)
    states = out["states"]
    assert states["a.json"] == states["b.json"] == states["c.json"] == states["flaky.json"] == 2
    assert states["bin.dat"] == 3
    assert states["gone.json"] == 4
    assert out["calls"] > 1
