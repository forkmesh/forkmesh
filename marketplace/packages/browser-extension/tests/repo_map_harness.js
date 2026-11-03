"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
let checks = 0;
function check(name, ok, detail = "") {
  checks += 1;
  if (!ok) { console.error("FAIL " + name + (detail ? ": " + detail : "")); process.exit(1); }
  console.log("ok   " + name);
}

const SHA = "3541888a1b2c3d4e5f60718293a4b5c6d7e8f901";
const fetched = [];
const responses = {
  "https://api.github.com/repos/acme/engine/branches/main": [200, { commit: { sha: SHA } }],
  [`https://api.github.com/repos/acme/engine/git/trees/${SHA}?recursive=1`]: [200, { truncated: false, tree: [
    { path: "README.md", type: "blob", size: 300 },
    { path: "src", type: "tree" },
    { path: "src/main.go", type: "blob", size: 5000 },
    { path: "src/util", type: "tree" },
    { path: "src/util/a.go", type: "blob", size: 700 },
    { path: "empty", type: "tree" },
    { path: "vendor/lib", type: "commit" },
    { path: "../escape", type: "blob", size: 9 },
  ] }],
  "https://api.github.com/repos/busy/repo/branches/main": [403, {}, { "x-ratelimit-remaining": "0" }],
  "https://codeberg.org/api/v1/repos/forgejo/forgejo": [200, { default_branch: "forgejo" }],
  "https://codeberg.org/api/v1/repos/forgejo/forgejo/branches/forgejo": [200, { commit: { id: SHA } }],
  [`https://codeberg.org/api/v1/repos/forgejo/forgejo/git/trees/${SHA}?recursive=true&per_page=1000&page=1`]:
    [200, { total_count: 3, tree: [{ path: "go.mod", type: "blob", size: 10 }, { path: "cmd", type: "tree" }] }],
  [`https://codeberg.org/api/v1/repos/forgejo/forgejo/git/trees/${SHA}?recursive=true&per_page=1000&page=2`]:
    [200, { total_count: 3, tree: [{ path: "cmd/main.go", type: "blob", size: 90 }] }],
};
const bodies = {
  "https://raw.githubusercontent.com/acme/engine/x/a.go": new TextEncoder().encode("package a\n"),
  "https://raw.githubusercontent.com/acme/engine/x/logo.bin": new Uint8Array([137, 80, 0, 71]),
  "https://raw.githubusercontent.com/acme/engine/x/big.txt": new TextEncoder().encode("x".repeat(3000)),
};
globalThis.self = globalThis;
globalThis.fetch = async (url, options = {}) => {
  fetched.push({ url, options });
  if (url in bodies) {
    const bytes = bodies[url];
    let sent = false;
    return { ok: true, status: 200, headers: { get: () => null }, body: { getReader: () => ({
      async read() {
        if (sent) return { done: true };
        sent = true;
        return { done: false, value: bytes };
      },
      async cancel() { return undefined; },
    }) } };
  }
  const [status, body, headers = {}] = responses[url] || [404, {}];
  return { status, ok: status < 400, headers: { get: (name) => headers[name.toLowerCase()] ?? null }, json: async () => body };
};

vm.runInThisContext(fs.readFileSync(path.join(EXTENSION, "repo.js"), "utf8"), { filename: "repo.js" });
vm.runInThisContext(fs.readFileSync(path.join(EXTENSION, "map-source.js"), "utf8"), { filename: "map-source.js" });
const { repoPage } = globalThis.ForkMeshRepoBackground;
const { load, readText, readTexts, buildTree } = globalThis.ForkMeshRepoMap;

(async () => {
  const github = await load(repoPage("https://github.com/acme/engine/tree/main/src"), "main");
  check("a GitHub map is pinned to the branch's commit", github.commit === SHA && github.branch === "main");
  const root = github.root;
  check("folder sizes add up their files", root.size === 6000 && root.fileCount === 3,
    JSON.stringify({ size: root.size, files: root.fileCount }));
  check("folders sort largest first", root.children[0].name === "src" && root.children[0].size === 5700);
  check("empty folders stay, submodules and escaping paths are dropped",
    root.children.some((node) => node.name === "empty" && node.size === 0)
    && !JSON.stringify(root).includes("vendor") && !JSON.stringify(root).includes("escape"));
  check("files keep their full path", root.children[0].children.find((node) => node.type === "dir").children[0].path === "src/util/a.go");
  check("raw reads go to the pinned commit", github.rawUrl("src/a b.go") === `https://raw.githubusercontent.com/acme/engine/${SHA}/src/a%20b.go`);
  check("files and folders open on GitHub at the commit", github.webUrl("src/main.go") === `https://github.com/acme/engine/blob/${SHA}/src/main.go`
    && github.webUrl("src", true) === `https://github.com/acme/engine/tree/${SHA}/src`);
  check("provider reads carry no cookies", fetched.every((call) => call.options.credentials === "omit"));

  const codeberg = await load(repoPage("https://codeberg.org/forgejo/forgejo"), "");
  check("a Gitea map asks for the default branch and reads every tree page",
    codeberg.branch === "forgejo" && codeberg.root.fileCount === 2 && codeberg.root.size === 100 && !codeberg.truncated);
  check("Gitea raw reads use the API at the commit",
    codeberg.rawUrl("cmd/main.go") === `https://codeberg.org/api/v1/repos/forgejo/forgejo/raw/cmd/main.go?ref=${SHA}`
    && codeberg.webUrl("cmd") === `https://codeberg.org/forgejo/forgejo/src/commit/${SHA}/cmd`);

  let unsupported = null;
  try { await load(repoPage("https://gitlab.com/gitlab-org/gitlab-runner"), "main"); } catch (error) { unsupported = error.code; }
  check("GitLab has no map: its API lists no file sizes", unsupported === "map_unsupported_provider");
  let limited = null;
  try { await load(repoPage("https://github.com/busy/repo"), "main"); } catch (error) { limited = error.code; }
  check("a rate-limited provider says so", limited === "provider_rate_limited");

  const texts = await readTexts(["a.go", "logo.bin", "gone.go"], (p) => `https://raw.githubusercontent.com/acme/engine/x/${p}`);
  check("source text is read for the code map", texts["a.go"].text === "package a\n" && !texts["a.go"].truncated);
  check("a binary file is not drawn as text", texts["logo.bin"].binary === true);
  check("a missing file is reported missing", texts["gone.go"].missing === true);
  const big = await readText("https://raw.githubusercontent.com/acme/engine/x/big.txt", 1000);
  check("a large file is cut short and marked truncated", big.text.length === 1000 && big.truncated);
  check("an empty listing is an empty tree", buildTree([]).fileCount === 0 && buildTree(null).size === 0);
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); process.exit(1); });
