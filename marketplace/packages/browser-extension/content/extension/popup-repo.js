(() => {
  const $ = (selector) => document.querySelector(selector);
  let send = null;
  let tabUrl = "";
  let current = null;
  let busy = false;

  const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[char]));
  const WORDS = {
    not_signed_in: "Sign in to ForkMesh first.",
    invalid_session: "Your ForkMesh session expired. Sign in again.",
    forkmesh_unreachable: "Could not reach the ForkMesh server.",
    provider_rate_limited: "The provider is rate-limiting requests. Try again in a few minutes.",
    provider_unavailable: "The provider did not answer. Try again in a moment.",
    provider_unreachable: "The provider could not be reached.",
    private_authorization_required: "This repository is private. Import it from Repositories on ForkMesh with a read token.",
    repository_not_found_or_authorization_required: "The provider says this repository does not exist or is private.",
    provider_authorization_failed: "The provider refused to share this repository's details.",
    target_owner_forbidden: "Your account cannot receive this repository.",
    unsupported_provider: "ForkMesh mirrors GitHub, GitLab, Codeberg and Gitea repositories.",
    not_a_repository_page: "This tab is not a repository page.",
  };
  const explain = (result) => WORDS[result?.error] || result?.message || "Something went wrong.";

  function size(kb) {
    if (!Number.isFinite(kb)) return "";
    if (kb < 1024) return `${kb} KB`;
    if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(kb < 10 * 1024 ? 1 : 0)} MB`;
    return `${(kb / 1024 / 1024).toFixed(1)} GB`;
  }
  const number = (value) => (Number.isFinite(value) ? Number(value).toLocaleString() : "–");

  function ago(value) {
    const at = Date.parse(value || "");
    if (!Number.isFinite(at)) return "";
    const days = Math.floor((Date.now() - at) / 86400000);
    if (days < 1) return "today";
    if (days === 1) return "yesterday";
    if (days < 60) return `${days} days ago`;
    if (days < 730) return `${Math.round(days / 30)} months ago`;
    return `${Math.round(days / 365)} years ago`;
  }

  function say(kind, text) {
    const ok = $("[data-repo-ok]"), error = $("[data-repo-error]");
    ok.hidden = kind !== "ok" || !text; error.hidden = kind !== "error" || !text;
    (kind === "ok" ? ok : error).textContent = text || "";
  }

  function draw() {
    const section = $("[data-repo]");
    const repo = current?.repo;
    section.hidden = !repo;
    if (!repo) return;
    $("[data-repo-name]").textContent = repo.fullName;
    $("[data-repo-provider]").textContent = repo.providerLabel;
    note("[data-repo-description]", repo.description);
    const tiles = repo.limited ? [] : [
      ["Size", size(repo.sizeKb) || "–", repo.sizeKb == null && repo.provider === "gitlab"
        ? "GitLab shows a project's size to its members only" : ""],
      ["Stars", number(repo.stars), ""],
      ["Forks", number(repo.forks), ""],
      [repo.issuesIncludePulls ? "Issues+PRs" : "Issues", number(repo.issues), ""],
    ];
    const grid = $("[data-repo-grid]");
    grid.hidden = !tiles.length;
    grid.innerHTML = tiles.map(([label, value, title]) =>
      `<div${title ? ` title="${escapeHtml(title)}"` : ""}><dt>${escapeHtml(label)}</dt><dd>${escapeHtml(value)}</dd></div>`).join("");
    const facts = [repo.language, repo.license, repo.defaultBranch && `branch ${repo.defaultBranch}`,
      repo.pushedAt && `updated ${ago(repo.pushedAt)}`, repo.fork && "fork", repo.archived && "archived"].filter(Boolean);
    note("[data-repo-facts]", repo.limited
      ? `${repo.providerLabel} did not share this repository's details: ${explain({ error: current.unavailable })}`
      : facts.join(" · "));
    const listing = current.forkmesh?.listing;
    let status = "";
    if (listing) {
      status = listing.mirrored
        ? `On ForkMesh: ${listing.statusLabel}${listing.targetOwner ? ` under ${listing.targetOwner}` : ""}.`
        : `Listed on ForkMesh (${listing.statusLabel}). Mirror nodes copy a public repository within about 30 minutes of its listing.`;
      if (repo.archived) status += " Archived repositories are listed but not copied by mirror nodes.";
    } else if (current.forkmesh?.checked) status = "Not on ForkMesh yet.";
    else if (current.forkmesh?.error) status = `ForkMesh status unknown: ${explain({ error: current.forkmesh.error })}`;
    note("[data-repo-status]", status);
    const mirror = $("[data-repo-mirror]");
    mirror.hidden = Boolean(listing) || repo.isPrivate;
    mirror.disabled = busy;
    mirror.textContent = busy ? "Mirroring…" : "Mirror to my ForkMesh account";
    $("[data-repo-open]").hidden = !listing;
    $("[data-repo-private]").hidden = !repo.isPrivate || Boolean(listing);
  }

  function note(selector, text) {
    const node = $(selector);
    node.textContent = text || "";
    node.hidden = !text;
  }

  async function render(url) {
    tabUrl = url || tabUrl;
    const result = tabUrl ? await send({ type: "fm-repo-info", url: tabUrl }) : null;
    current = result?.ok ? result : null;
    draw();
  }

  async function mirror() {
    if (busy || !current?.repo) return;
    busy = true;
    say("", "");
    draw();
    const result = await send({ type: "fm-repo-mirror", url: tabUrl });
    busy = false;
    if (!result.ok) {
      say("error", explain(result));
      draw();
      return;
    }
    const owner = result.listing?.targetOwner;
    say("ok", result.notice || (result.listing?.mirrored
      ? `Already mirrored on ForkMesh${owner ? ` under ${owner}` : ""}.`
      : `Added to ForkMesh${owner ? ` under ${owner}` : ""}. Until its Git history is copied it shows under External repositories.`));
    if (current.forkmesh) current.forkmesh.listing = result.listing;
    else current.forkmesh = { listing: result.listing, checked: true };
    draw();
  }

  function bind(sender) {
    send = sender;
    $("[data-repo-mirror]").addEventListener("click", () => void mirror());
    $("[data-repo-open]").addEventListener("click", () => {
      const url = current?.forkmesh?.listing?.url;
      if (url) void send({ type: "fm-repo-open", url }).then(() => window.close());
    });
  }

  self.ForkMeshRepoPopup = { bind, render, size };
})();
