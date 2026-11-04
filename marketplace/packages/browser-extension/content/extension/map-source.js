(() => {
  const COMMIT = /^[0-9a-f]{40}(?:[0-9a-f]{24})?$/;
  const TEXT_BYTES = 512 * 1024;
  const SNIFF_BYTES = 8000;
  const GITEA_PAGE = 1000;
  const GITEA_PAGES = 60;
  const PARALLEL = 6;

  class MapError extends Error {
    constructor(code, status = 0) {
      super(String(code).replaceAll("_", " "));
      this.code = code; this.status = status;
    }
  }

  const encodePath = (value) => String(value || "").split("/").map(encodeURIComponent).join("/");

  async function get(url) {
    let response;
    try {
      response = await fetch(url, { headers: { accept: "application/json" }, credentials: "omit", cache: "no-store" });
    } catch {
      throw new MapError("provider_unreachable");
    }
    if (response.status === 404) throw new MapError("repository_not_found_or_authorization_required", 404);
    if (response.status === 429 || (response.status === 403 && response.headers.get("x-ratelimit-remaining") === "0")) {
      throw new MapError("provider_rate_limited", 429);
    }
    if (!response.ok) throw new MapError("provider_unavailable", response.status);
    return response.json().catch(() => { throw new MapError("provider_unavailable", response.status); });
  }

  function site(page) {
    if (!page || page.provider === "gitlab") return null;
    const slug = `${encodeURIComponent(page.owner)}/${encodeURIComponent(page.name)}`;
    if (page.provider === "github") {
      const api = `https://api.github.com/repos/${slug}`;
      return {
        async defaultBranch() { return (await get(api))?.default_branch; },
        async commit(branch) { return (await get(`${api}/branches/${encodeURIComponent(branch)}`))?.commit?.sha; },
        async entries(commit) {
          const tree = await get(`${api}/git/trees/${commit}?recursive=1`);
          return { entries: Array.isArray(tree?.tree) ? tree.tree : [], truncated: Boolean(tree?.truncated) };
        },
        raw: (commit, path) => `https://raw.githubusercontent.com/${slug}/${commit}/${encodePath(path)}`,
        web: (commit, path, folder) => `https://${page.host}/${slug}/${folder ? "tree" : "blob"}/${commit}/${encodePath(path)}`,
      };
    }
    const api = `https://${page.host}/api/v1/repos/${slug}`;
    return {
      async defaultBranch() { return (await get(api))?.default_branch; },
      async commit(branch) { return (await get(`${api}/branches/${encodeURIComponent(branch)}`))?.commit?.id; },
      async entries(commit) {
        const entries = [];
        let total = Infinity;
        let truncated = false;
        for (let number = 1; number <= GITEA_PAGES && entries.length < total; number += 1) {
          const tree = await get(`${api}/git/trees/${commit}?recursive=true&per_page=${GITEA_PAGE}&page=${number}`);
          const rows = Array.isArray(tree?.tree) ? tree.tree : [];
          if (Number.isFinite(tree?.total_count)) total = tree.total_count;
          truncated = truncated || Boolean(tree?.truncated);
          if (!rows.length) break;
          entries.push(...rows);
        }
        return { entries, truncated: truncated || (Number.isFinite(total) && entries.length < total) };
      },
      raw: (commit, path) => `${api}/raw/${encodePath(path)}?ref=${commit}`,
      web: (commit, path) => `https://${page.host}/${slug}/src/commit/${commit}/${encodePath(path)}`,
    };
  }

  function buildTree(entries) {
    const root = { type: "dir", name: "", path: "", size: 0, children: [], fileCount: 0 };
    const folders = new Map([["", root]]);
    const folder = (path) => {
      if (folders.has(path)) return folders.get(path);
      const cut = path.lastIndexOf("/");
      const parent = folder(cut < 0 ? "" : path.slice(0, cut));
      const node = { type: "dir", name: path.slice(cut + 1), path, size: 0, children: [] };
      parent.children.push(node);
      folders.set(path, node);
      return node;
    };
    for (const entry of entries || []) {
      const path = typeof entry?.path === "string" ? entry.path.replace(/^\/+|\/+$/g, "") : "";
      if (!path || path.split("/").some((part) => !part || part === "." || part === "..")) continue;
      if (entry.type === "tree") { folder(path); continue; }
      if (entry.type !== "blob") continue;
      const cut = path.lastIndexOf("/");
      const size = Number.isFinite(entry.size) && entry.size > 0 ? Math.round(entry.size) : 0;
      folder(cut < 0 ? "" : path.slice(0, cut)).children.push({ type: "file", name: path.slice(cut + 1), path, size });
      root.fileCount += 1;
    }
    const total = (node) => {
      if (node.type === "file") return node.size;
      node.size = node.children.reduce((sum, child) => sum + total(child), 0);
      node.children.sort((a, b) => b.size - a.size || a.name.localeCompare(b.name));
      return node.size;
    };
    total(root);
    return root;
  }

  async function load(page, branch) {
    const provider = site(page);
    if (!provider) throw new MapError("map_unsupported_provider", 400);
    const ref = String(branch || "") || String(await provider.defaultBranch() || "");
    if (!ref) throw new MapError("provider_unavailable");
    const commit = String(await provider.commit(ref) || "").toLowerCase();
    if (!COMMIT.test(commit)) throw new MapError("provider_unavailable");
    const { entries, truncated } = await provider.entries(commit);
    return {
      branch: ref, commit, truncated, root: buildTree(entries),
      rawUrl: (path) => provider.raw(commit, path),
      webUrl: (path, folder = false) => provider.web(commit, path, folder),
    };
  }

  async function readText(url, limit = TEXT_BYTES) {
    let response;
    try {
      response = await fetch(url, { credentials: "omit" });
    } catch {
      return { missing: true };
    }
    if (!response.ok) return { missing: true };
    const chunks = [];
    let bytes = 0;
    let truncated = false;
    const reader = response.body && response.body.getReader ? response.body.getReader() : null;
    if (reader) {
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        bytes += value.length;
        if (bytes >= limit) {
          truncated = true;
          await reader.cancel().catch(() => null);
          break;
        }
      }
    } else {
      const whole = new Uint8Array(await response.arrayBuffer());
      chunks.push(whole);
      bytes = whole.length;
      truncated = bytes > limit;
    }
    const all = new Uint8Array(Math.min(bytes, limit));
    let at = 0;
    for (const chunk of chunks) {
      const room = all.length - at;
      if (room <= 0) break;
      all.set(chunk.subarray(0, room), at);
      at += Math.min(room, chunk.length);
    }
    if (all.subarray(0, SNIFF_BYTES).includes(0)) return { binary: true };
    return { text: new TextDecoder().decode(all), truncated };
  }

  async function readTexts(paths, rawUrl) {
    const out = {};
    const queue = [...paths];
    const worker = async () => {
      while (queue.length) {
        const path = queue.shift();
        out[path] = await readText(rawUrl(path));
      }
    };
    await Promise.all(Array.from({ length: Math.min(PARALLEL, queue.length) }, worker));
    return out;
  }

  async function readBlob(url) {
    try {
      const response = await fetch(url, { credentials: "omit" });
      return response.ok ? response.blob() : null;
    } catch {
      return null;
    }
  }

  self.ForkMeshRepoMap = { MapError, site, buildTree, load, readText, readTexts, readBlob };
})();
