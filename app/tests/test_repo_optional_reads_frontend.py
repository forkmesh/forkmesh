import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
EXPLORER = (JS / "05-repo-list-explorer.js").read_text(encoding="utf-8")
COMPOSE = (JS / "07-repo-compose-branch.js").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")


def _function(text, name):
    start = text.index("  function %s(" % name) if (
        "  function %s(" % name) in text else text.index(
        "  async function %s(" % name)
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _optional_block():
    start = CONTENT.index("  const REPO_OPTIONAL_TREE_MISS_MS")
    end = CONTENT.index("  // An authoritative \"the repository does not hold this\"")
    return CONTENT[start:end] + _function(CONTENT, "isMissingMirrorFolder")


STUBS = """
const storage = new Map();
globalThis.sessionStorage = {
  getItem: (key) => (storage.has(key) ? storage.get(key) : null),
  setItem: (key, value) => storage.set(key, String(value)),
};
globalThis.document = { body: null, addEventListener() {} };
const state = { repoTreeView: null };
const requests = [];
let trees = {};
function repoKey(repo) { return `${repo.owner}/${repo.name}`; }
function repoSelectedBranch() { return "main"; }
function repoDataVersion() { return "7"; }
function repoLiveUrl(repo, action, params) { return `${action}:${params.path}`; }
async function fetchRepoJson(url) {
  requests.push(url);
  const path = url.slice("tree:".length);
  if (path in trees) {
    if (trees[path] === 503) {
      throw Object.assign(new Error("mirror_unavailable"), { status: 503, code: "mirror_unavailable" });
    }
    return { entries: trees[path].map((name) => ({ name, type: "tree" })) };
  }
  throw Object.assign(new Error("not_found"), { status: 404, code: "not_found" });
}
const repo = { owner: "acme", name: "widgets" };
"""


def _run(body):
    script = STUBS + _optional_block() + "\n(async () => {\n" + body + "\n})();\n"
    result = subprocess.run(["node", "-e", script], check=True, text=True,
                            capture_output=True)
    return json.loads(result.stdout)


def test_a_missing_release_folder_is_read_from_its_parent_without_a_404():
    out = _run("""
      trees = { ".forkmesh": ["issues"] };
      const first = await fetchRepoOptionalTree(repo, ".forkmesh/releases");
      const again = await fetchRepoOptionalTree(repo, ".forkmesh/releases");
      console.log(JSON.stringify({ first, again, requests }));
    """)
    assert out == {"first": None, "again": None, "requests": ["tree:.forkmesh"]}


def test_an_absent_metadata_folder_in_the_root_listing_sends_nothing():
    out = _run("""
      state.repoTreeView = { repo, path: "", entries: [{ name: "src", type: "tree" }] };
      const releases = await fetchRepoOptionalTree(repo, ".forkmesh/releases");
      const issues = await fetchRepoOptionalTree(repo, ".forkmesh/issues", { fresh: true });
      console.log(JSON.stringify({ releases, issues, requests }));
    """)
    assert out == {"releases": None, "issues": None, "requests": []}


def test_a_present_folder_is_read_and_a_404_is_remembered_for_the_session():
    out = _run("""
      trees = { ".forkmesh": ["releases"], ".forkmesh/releases": ["latest"] };
      const found = await fetchRepoOptionalTree(repo, ".forkmesh/releases");
      delete trees[".forkmesh/releases"];
      const fresh = await fetchRepoOptionalTree(repo, ".forkmesh/releases", { fresh: true });
      const cached = await fetchRepoOptionalTree(repo, ".forkmesh/releases");
      console.log(JSON.stringify({
        found: found.entries.map((entry) => entry.name), fresh, cached, requests,
        remembered: [...storage.keys()],
      }));
    """)
    assert out["found"] == ["latest"]
    assert out["fresh"] is None and out["cached"] is None
    assert out["requests"] == [
        "tree:.forkmesh", "tree:.forkmesh/releases", "tree:.forkmesh/releases"]
    assert out["remembered"] == [
        "forkmesh.repoTreeMiss:acme/widgets@main#7:.forkmesh/releases"]


def test_an_unavailable_mirror_still_surfaces_as_an_error():
    out = _run("""
      trees = { ".forkmesh": 503 };
      let error = "";
      try { await fetchRepoOptionalTree(repo, ".forkmesh/releases"); }
      catch (caught) { error = caught.code; }
      console.log(JSON.stringify({ error, remembered: [...storage.keys()] }));
    """)
    assert out == {"error": "mirror_unavailable", "remembered": []}


def test_the_probes_use_the_optional_reader():
    about = _function(CONTENT, "loadRepoAboutRelease")
    assert 'fetchRepoOptionalTree(repo, ".forkmesh/releases")' in about
    assert 'fetchRepoOptionalTree(repo, ".forkmesh/releases")' in _function(
        COMPOSE, "loadRepoReleases")
    assert 'fetchRepoOptionalTree(repo, ".forkmesh/issues", { fresh: true })' in CONTENT
    assert 'repoLiveUrl(repo, "tree", { path: ".forkmesh/releases" })' not in CONTENT
    assert "function fetchRepoOptionalTree(" in BUNDLE


def _svg_script(body):
    block = (_function(CONTENT, "isRepoRawSvgUrl")
             + _function(CONTENT, "svgDataUrl")
             + _function(CONTENT, "hydrateRepoSvgImage"))
    return """
globalThis.location = { origin: "https://code.example.org" };
const REPO_SVG_IMAGE_MAX_BYTES = 1024 * 1024;
const calls = [];
let reply = { ok: true, body: '<svg xmlns="http://www.w3.org/2000/svg"></svg>' };
globalThis.fetch = async (url) => {
  calls.push(url);
  const bytes = new TextEncoder().encode(reply.body);
  return { ok: reply.ok, arrayBuffer: async () => bytes.buffer };
};
globalThis.FileReader = class {
  readAsDataURL(blob) {
    blob.arrayBuffer().then((buffer) => {
      this.result = `data:${blob.type};base64,` + Buffer.from(buffer).toString("base64");
      this.onload();
    });
  }
};
function image(src) {
  const attrs = { "data-repo-svg-src": src };
  return {
    isConnected: true, src: "",
    getAttribute: (name) => attrs[name] ?? null,
    removeAttribute: (name) => { delete attrs[name]; },
  };
}
""" + block + "\n(async () => {\n" + body + "\n})();\n"


def test_repository_svg_images_render_from_a_data_url():
    script = _svg_script("""
      const good = image("/api/repo/acme/widgets/raw?path=docs%2Flogo.svg&ref=main");
      await hydrateRepoSvgImage(good);
      const foreign = image("https://elsewhere.example.net/api/repo/acme/widgets/raw?path=logo.svg");
      await hydrateRepoSvgImage(foreign);
      const png = image("/api/repo/acme/widgets/raw?path=logo.png");
      await hydrateRepoSvgImage(png);
      reply = { ok: true, body: "<html><script>alert(1)</script></html>" };
      const html = image("/api/repo/acme/widgets/raw?path=page.svg");
      await hydrateRepoSvgImage(html);
      reply = { ok: false, body: "" };
      const missing = image("/api/repo/acme/widgets/raw?path=gone.svg");
      await hydrateRepoSvgImage(missing);
      console.log(JSON.stringify({
        good: good.src, foreign: foreign.src, png: png.src, html: html.src,
        missing: missing.src, calls,
        cleared: good.getAttribute("data-repo-svg-src"),
      }));
    """)
    result = subprocess.run(["node", "-e", script], check=True, text=True,
                            capture_output=True)
    out = json.loads(result.stdout)
    assert out["good"].startswith("data:image/svg+xml;base64,")
    assert out["foreign"] == out["png"] == out["html"] == out["missing"] == ""
    assert out["calls"] == [
        "/api/repo/acme/widgets/raw?path=docs%2Flogo.svg&ref=main",
        "/api/repo/acme/widgets/raw?path=page.svg",
        "/api/repo/acme/widgets/raw?path=gone.svg"]
    assert out["cleared"] is None


def test_readme_and_file_previews_defer_svg_images_to_the_hydrator():
    image = _function(CONTENT, "mdImageHtml")
    assert '${svg ? "data-repo-svg-src" : "src"}' in image
    preview = _function(EXPLORER, "renderRepoMediaPreview")
    assert 'meta.mime === "image/svg+xml" ? "data-repo-svg-src" : "src"' in preview
    assert "new MutationObserver(" in _function(CONTENT, "watchRepoSvgImages")
    assert "watchRepoSvgImages" in BUNDLE
