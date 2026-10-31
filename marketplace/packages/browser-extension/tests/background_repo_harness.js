"use strict";
const fs = require("fs"), path = require("path"), vm = require("vm");

const EXTENSION = path.join(__dirname, "..", "content", "extension");
let checks = 0;
function check(name, ok, detail = "") {
  checks += 1;
  if (!ok) { console.error("FAIL " + name + (detail ? ": " + detail : "")); process.exit(1); }
  console.log("ok   " + name);
}

const browser = { session: {}, opened: [], fetched: [], calls: [], listings: {} };
globalThis.self = globalThis;
globalThis.chrome = {
  storage: { session: {
    async get(keys) { return Object.fromEntries(keys.filter((k) => k in browser.session).map((k) => [k, structuredClone(browser.session[k])])); },
    async set(values) { Object.assign(browser.session, structuredClone(values)); },
  } },
  tabs: { async create({ url }) { browser.opened.push(url); } },
};

const GITHUB = { id: 36502, full_name: "git/git", html_url: "https://github.com/git/git", description: "Git Source Code Mirror",
  size: 324154, stargazers_count: 64096, forks_count: 28709, open_issues_count: 419, language: "C",
  license: { spdx_id: "NOASSERTION", name: "Other" }, default_branch: "master", pushed_at: "2026-10-30T22:09:57Z",
  archived: false, fork: false, private: false };
const GITLAB = { id: 250833, path_with_namespace: "gitlab-org/gitlab-runner", web_url: "https://gitlab.com/gitlab-org/gitlab-runner",
  description: "Runner", star_count: 2583, forks_count: 2732, default_branch: "main", visibility: "public",
  last_activity_at: "2026-10-31T17:02:28.213Z", license: { name: "MIT License" } };
const CODEBERG = { id: 73144, full_name: "forgejo/forgejo", html_url: "https://codeberg.org/forgejo/forgejo", size: 338026,
  stars_count: 5751, forks_count: 1017, open_issues_count: 1594, language: "Go", default_branch: "forgejo",
  updated_at: "2026-10-31T10:34:06+01:00", archived: false, fork: false, private: false, licenses: ["GPL-3.0-or-later"] };
const responses = {
  "https://api.github.com/repos/git/git": [200, GITHUB],
  "https://gitlab.com/api/v4/projects/gitlab-org%2Fgitlab-runner?license=true": [200, GITLAB],
  "https://gitlab.com/api/v4/projects/gitlab-org%2Fgitlab-runner/languages": [200, { Go: 98.5, Shell: 0.8 }],
  "https://codeberg.org/api/v1/repos/forgejo/forgejo": [200, CODEBERG],
  "https://api.github.com/repos/someone/secret": [404, { message: "Not Found" }],
  "https://api.github.com/repos/busy/repo": [403, { message: "rate limit" }, { "x-ratelimit-remaining": "0" }],
};
globalThis.fetch = async (url, options) => {
  browser.fetched.push({ url, options });
  const [status, body, headers = {}] = responses[url] || [404, {}];
  return { status, ok: status < 400, headers: { get: (name) => headers[name.toLowerCase()] ?? null }, json: async () => body };
};

vm.runInThisContext(fs.readFileSync(path.join(EXTENSION, "repo.js"), "utf8"), { filename: "repo.js" });
const { install, repoPage, importId } = globalThis.ForkMeshRepoBackground;
const handlers = {};
const api = async (apiPath, method = "GET", body = null) => {
  browser.calls.push({ apiPath, method, body });
  if (method === "POST") {
    const repository = { id: "ext_" + "a".repeat(24), status: "external_repository", statusLabel: "External repository",
      mirrored: false, targetOwner: "alice", name: "git" };
    browser.listings[repository.id] = repository;
    return { ok: true, created: true, repository };
  }
  const id = apiPath.split("/").pop();
  if (browser.listings[id]) return { ok: true, repository: browser.listings[id] };
  throw Object.assign(new Error("not found"), { code: "not_found", status: 404 });
};
install({ api, settings: async () => ({ baseUrl: "https://app.forkmesh.com" }), handlers });

(async () => {
  const pages = {
    "https://github.com/git/git/tree/master/Documentation": "https://github.com/git/git",
    "https://www.github.com/git/git.git": "https://github.com/git/git",
    "https://gitlab.com/gitlab-org/gitlab-runner/-/merge_requests/12": "https://gitlab.com/gitlab-org/gitlab-runner",
    "https://gitlab.com/group/sub/project": "https://gitlab.com/group/sub/project",
    "https://codeberg.org/forgejo/forgejo/issues/4": "https://codeberg.org/forgejo/forgejo",
    "https://gitea.com/gitea/tea/src/branch/main": "https://gitea.com/gitea/tea",
  };
  for (const [url, canonical] of Object.entries(pages)) {
    check(`${url} is the repository ${canonical}`, repoPage(url)?.canonicalUrl === canonical, JSON.stringify(repoPage(url)));
  }
  for (const url of ["https://github.com/settings/profile", "https://github.com/git", "https://github.com/orgs/git/people",
    "http://github.com/git/git", "https://bitbucket.org/a/b", "https://gitlab.com/explore/projects",
    "https://gitlab.com/gitlab-org", "https://codeberg.org/explore/repos", "https://github.com:8443/git/git", "not a url"]) {
    check(`${url} is not a repository page`, repoPage(url) === null);
  }

  const github = await handlers["fm-repo-info"]({ url: "https://github.com/git/git/pulls" });
  check("GitHub details are read from the public API", github.ok && github.repo.sizeKb === 324154
    && github.repo.stars === 64096 && github.repo.language === "C" && github.repo.license === ""
    && github.repo.issuesIncludePulls === true, JSON.stringify(github.repo));
  check("provider requests carry no cookies", browser.fetched.every((call) => call.options.credentials === "omit"));
  const expectedId = await importId("github", "36502");
  check("the ForkMesh listing is looked up by the import id", browser.calls.at(-1).apiPath === `/api/repository-imports/${expectedId}`);
  check("a repository with no listing says so", github.forkmesh.checked && github.forkmesh.listing === null);

  const before = browser.fetched.length;
  await handlers["fm-repo-info"]({ url: "https://github.com/git/git" });
  check("a second look within ten minutes uses the cache", browser.fetched.length === before);

  const gitlab = await handlers["fm-repo-info"]({ url: "https://gitlab.com/gitlab-org/gitlab-runner/-/tree/main" });
  check("GitLab details include the top language and an unknown size", gitlab.repo.language === "Go"
    && gitlab.repo.sizeKb === null && gitlab.repo.stars === 2583 && gitlab.repo.isPrivate === false, JSON.stringify(gitlab.repo));
  const codeberg = await handlers["fm-repo-info"]({ url: "https://codeberg.org/forgejo/forgejo" });
  check("Codeberg details are read from the Gitea API", codeberg.repo.sizeKb === 338026 && codeberg.repo.stars === 5751
    && codeberg.repo.license === "GPL-3.0-or-later", JSON.stringify(codeberg.repo));

  const missing = await handlers["fm-repo-info"]({ url: "https://github.com/someone/secret" });
  check("a private or missing repository shows no card", missing.ok && missing.repo === null);
  const limited = await handlers["fm-repo-info"]({ url: "https://github.com/busy/repo" });
  check("a rate-limited provider still offers the mirror", limited.repo.limited && limited.unavailable === "provider_rate_limited"
    && limited.repo.canonicalUrl === "https://github.com/busy/repo");
  const none = await handlers["fm-repo-info"]({ url: "https://example.com/a/b" });
  check("other sites are left alone", none.ok && none.page === null);

  const mirrored = await handlers["fm-repo-mirror"]({ url: "https://github.com/git/git/blob/master/README.md" });
  const post = browser.calls.at(-1);
  check("Mirror posts the canonical URL as an import", post.method === "POST" && post.apiPath === "/api/repository-imports"
    && post.body.sourceUrl === "https://github.com/git/git" && post.body.mode === "import"
    && !("providerToken" in post.body) && !("targetOwner" in post.body), JSON.stringify(post));
  check("an unmirrored listing links to the dashboard's repositories", mirrored.created
    && mirrored.listing.url === "https://app.forkmesh.com/dashboard/repos" && mirrored.listing.targetOwner === "alice");

  browser.listings[expectedId] = { id: expectedId, status: "actively_mirrored", statusLabel: "Actively mirrored",
    mirrored: true, targetOwner: "alice", name: "git" };
  const live = await handlers["fm-repo-info"]({ url: "https://github.com/git/git" });
  check("a mirrored listing links to its ForkMesh page", live.forkmesh.listing.mirrored
    && live.forkmesh.listing.url === "https://app.forkmesh.com/alice/git");

  await handlers["fm-repo-open"]({ url: "https://app.forkmesh.com/alice/git" });
  check("Open on ForkMesh opens the ForkMesh page", browser.opened.at(-1) === "https://app.forkmesh.com/alice/git");
  let refused = false;
  try { await handlers["fm-repo-open"]({ url: "https://evil.example/" }); } catch (error) { refused = error.code === "invalid_url"; }
  check("Open refuses an address off the ForkMesh server", refused);

  const ids = {};
  for (const [provider, external] of [["github", "36502"], ["gitlab", "250833"], ["codeberg", "73144"]]) {
    ids[`${provider}:${external}`] = await importId(provider, external);
  }
  console.log("RESULT " + JSON.stringify({ ids, canonical: Object.values(pages) }));
  console.log(`${checks} checks passed`);
})().catch((error) => { console.error(error); process.exit(1); });
