(() => {
  const CACHE_KEY = "repoInfo";
  const CACHE_MS = 10 * 60 * 1000;
  const CACHE_ENTRIES = 40;
  const SEGMENT = /^[A-Za-z0-9_.-]{1,100}$/;
  const PROVIDERS = {
    "github.com": { provider: "github", label: "GitHub", api: "https://api.github.com/repos/" },
    "gitlab.com": { provider: "gitlab", label: "GitLab", api: "https://gitlab.com/api/v4/projects/" },
    "codeberg.org": { provider: "codeberg", label: "Codeberg", api: "https://codeberg.org/api/v1/repos/" },
    "gitea.com": { provider: "gitea", label: "Gitea", api: "https://gitea.com/api/v1/repos/" },
  };
  const RESERVED = {
    github: new Set(["about", "account", "apps", "codespaces", "collections", "contact", "copilot", "customer-stories",
      "dashboard", "enterprise", "events", "explore", "features", "github-copilot", "issues", "join", "login", "logout",
      "marketplace", "new", "notifications", "organizations", "orgs", "pricing", "pulls", "readme", "search",
      "security", "settings", "site", "sponsors", "stars", "team", "topics", "trending", "users", "watching"]),
    gitlab: new Set(["-", "admin", "api", "assets", "dashboard", "explore", "groups", "help", "profile", "projects",
      "search", "users"]),
    gitea: new Set(["-", "admin", "api", "assets", "attachments", "avatars", "explore", "issues", "milestones",
      "notifications", "org", "pulls", "repo", "swagger", "user"]),
  };

  class RepoError extends Error {
    constructor(code, status = 0) {
      super(String(code).replaceAll("_", " "));
      this.code = code; this.status = status;
    }
  }

  function repoPage(value) {
    let url;
    try { url = new URL(String(value || "")); } catch { return null; }
    const host = url.hostname.toLowerCase().replace(/^www\./, "");
    const site = PROVIDERS[host];
    if (url.protocol !== "https:" || !site || (url.port && url.port !== "443")) return null;
    let parts = url.pathname.split("/").filter(Boolean).map((part) => {
      try { return decodeURIComponent(part); } catch { return ""; }
    });
    if (site.provider === "gitlab") {
      const split = parts.indexOf("-");
      if (split >= 0) parts = parts.slice(0, split);
      if (parts.length < 2 || parts.length > 20) return null;
    } else parts = parts.slice(0, 2);
    if (parts.length < 2) return null;
    parts[parts.length - 1] = parts[parts.length - 1].replace(/\.git$/, "");
    if (!parts.every((part) => SEGMENT.test(part) && part !== "." && part !== "..")) return null;
    const reserved = RESERVED[site.provider] || RESERVED.gitea;
    if (reserved.has(parts[0].toLowerCase())) return null;
    const fullName = parts.join("/");
    return {
      provider: site.provider, providerLabel: site.label, host,
      owner: parts.slice(0, -1).join("/"), name: parts[parts.length - 1], fullName,
      canonicalUrl: `https://${host}/${parts.map(encodeURIComponent).join("/")}`,
      apiUrl: site.provider === "gitlab"
        ? `${site.api}${encodeURIComponent(fullName)}?license=true`
        : `${site.api}${parts.map(encodeURIComponent).join("/")}`,
    };
  }

  const count = (value) => (Number.isFinite(value) && value >= 0 ? Math.round(value) : null);
  const text = (value, limit = 300) => (typeof value === "string" ? value.trim().slice(0, limit) : "");

  function normalize(page, root, languages) {
    const base = { ...page, id: String(root.id ?? ""), description: text(root.description, 500) };
    if (page.provider === "gitlab") {
      const top = Object.entries(languages || {}).sort((a, b) => b[1] - a[1])[0];
      const bytes = root.statistics?.repository_size;
      return {
        ...base,
        fullName: text(root.path_with_namespace) || page.fullName,
        url: text(root.web_url, 500) || page.canonicalUrl,
        sizeKb: Number.isFinite(bytes) ? Math.round(bytes / 1024) : null,
        stars: count(root.star_count), forks: count(root.forks_count), issues: count(root.open_issues_count),
        language: top ? text(top[0], 60) : "", license: text(root.license?.name, 80),
        defaultBranch: text(root.default_branch, 200), pushedAt: text(root.last_activity_at, 40),
        archived: Boolean(root.archived), fork: Boolean(root.forked_from_project),
        isPrivate: text(root.visibility, 20) !== "public" && Boolean(root.visibility),
      };
    }
    const license = root.license?.spdx_id && root.license.spdx_id !== "NOASSERTION"
      ? root.license.spdx_id : root.license?.name === "Other" ? "" : text(root.license?.name, 80)
        || (Array.isArray(root.licenses) ? text(root.licenses[0], 80) : "");
    return {
      ...base,
      fullName: text(root.full_name) || page.fullName,
      url: text(root.html_url, 500) || page.canonicalUrl,
      sizeKb: count(root.size),
      stars: count(root.stargazers_count ?? root.stars_count), forks: count(root.forks_count),
      issues: count(root.open_issues_count), issuesIncludePulls: page.provider === "github",
      language: text(root.language, 60), license,
      defaultBranch: text(root.default_branch, 200), pushedAt: text(root.pushed_at || root.updated_at, 40),
      archived: Boolean(root.archived), fork: Boolean(root.fork), isPrivate: Boolean(root.private),
    };
  }

  async function providerJson(url) {
    let response;
    try {
      response = await fetch(url, { headers: { accept: "application/json" }, credentials: "omit", cache: "no-store" });
    } catch {
      throw new RepoError("provider_unreachable");
    }
    if (response.status === 404) return null;
    if (response.status === 429 || (response.status === 403 && response.headers.get("x-ratelimit-remaining") === "0")) {
      throw new RepoError("provider_rate_limited", 429);
    }
    if (!response.ok) throw new RepoError("provider_unavailable", response.status);
    return response.json().catch(() => { throw new RepoError("provider_unavailable", response.status); });
  }

  async function lookupProvider(page) {
    const [root, languages] = await Promise.all([
      providerJson(page.apiUrl),
      page.provider === "gitlab"
        ? providerJson(page.apiUrl.replace("?license=true", "/languages")).catch(() => null) : null,
    ]);
    return root && typeof root === "object" && root.id != null ? normalize(page, root, languages) : null;
  }

  async function importId(provider, externalId) {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`${provider}\n${externalId}\n`));
    return "ext_" + [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("").slice(0, 24);
  }

  function listing(repository, baseUrl) {
    if (!repository || typeof repository !== "object") return null;
    const owner = text(repository.targetOwner, 100);
    const name = text(repository.name, 100);
    const mirrored = Boolean(repository.mirrored);
    return {
      id: text(repository.id, 40), status: text(repository.status, 40),
      statusLabel: text(repository.statusLabel, 60) || "External repository",
      mirrored, targetOwner: owner, listedBy: text(repository.listedBy, 100),
      url: mirrored && owner && name
        ? `${baseUrl}/${encodeURIComponent(owner)}/${encodeURIComponent(name)}` : `${baseUrl}/dashboard/repos`,
    };
  }

  function install({ api, settings, handlers }) {
    async function cached(key) {
      const store = (await chrome.storage.session.get([CACHE_KEY]))[CACHE_KEY] || {};
      const hit = store[key];
      return hit && Date.now() - hit.at < CACHE_MS ? hit : null;
    }
    async function remember(key, repo) {
      const store = (await chrome.storage.session.get([CACHE_KEY]))[CACHE_KEY] || {};
      store[key] = { at: Date.now(), repo };
      const keep = Object.entries(store).sort((a, b) => b[1].at - a[1].at).slice(0, CACHE_ENTRIES);
      await chrome.storage.session.set({ [CACHE_KEY]: Object.fromEntries(keep) });
    }

    async function forkmeshListing(repo) {
      if (!repo?.id) return { listing: null, checked: false };
      const { baseUrl } = await settings();
      try {
        const payload = await api(`/api/repository-imports/${await importId(repo.provider, repo.id)}`);
        return { listing: listing(payload.repository, baseUrl), checked: true };
      } catch (error) {
        if (error?.code === "not_found" || error?.status === 404) return { listing: null, checked: true };
        return { listing: null, checked: false, error: error?.code || "forkmesh_unreachable" };
      }
    }

    async function repoInfo({ url }) {
      const page = repoPage(url);
      if (!page) return { ok: true, page: null };
      const hit = await cached(page.canonicalUrl);
      let repo = hit ? hit.repo : undefined;
      let unavailable = "";
      if (repo === undefined) {
        try {
          repo = await lookupProvider(page);
          await remember(page.canonicalUrl, repo);
        } catch (error) {
          unavailable = error?.code || "provider_unavailable";
          repo = { ...page, url: page.canonicalUrl, limited: true };
        }
      }
      if (!repo) return { ok: true, page, repo: null };
      return { ok: true, page, repo, unavailable, forkmesh: await forkmeshListing(repo) };
    }

    async function repoMirror({ url }) {
      const page = repoPage(url);
      if (!page) throw new RepoError("not_a_repository_page", 400);
      const { baseUrl } = await settings();
      const payload = await api("/api/repository-imports", "POST", { sourceUrl: page.canonicalUrl, mode: "import" });
      return {
        ok: true, created: Boolean(payload.created), notice: text(payload.notice, 300),
        listing: listing(payload.repository, baseUrl),
      };
    }

    async function repoOpen({ url }) {
      const { baseUrl } = await settings();
      const target = String(url || "");
      if (!target.startsWith(`${baseUrl}/`)) throw new RepoError("invalid_url", 400);
      await chrome.tabs.create({ url: target });
      return { ok: true };
    }

    Object.assign(handlers, {
      "fm-repo-info": repoInfo,
      "fm-repo-mirror": repoMirror,
      "fm-repo-open": repoOpen,
    });
  }

  self.ForkMeshRepoBackground = { install, repoPage, importId };
})();
