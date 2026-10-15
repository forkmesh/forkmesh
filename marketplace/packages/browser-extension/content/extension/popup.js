// Toolbar popup: sign in, the Password Vault (popup-vault.js), pick the
// organization, add the Mastodon instances the extension may read, file the page in the current tab under a contact,
// and the CRM panel for the message open in that tab (the same panel the
// page's FM button opens, from panel.js). The X widget shows what x.js read
// off the organization's X profile, and any error from reading or syncing it.
const $ = (selector) => document.querySelector(selector);
const MAIL_URL = /^https:\/\/mail\.(google\.com|zoho\.[a-z.]+|zohocloud\.ca)\//;
let panel = null;
let mastodonHosts = [];
let notePage = null;
let unreadToot = null;

// A Mastodon post's address: /@grace/113489, /deck/@grace/113489, or
// /users/grace/statuses/113489.
const MASTODON_POST = /^\/(?:deck\/)?(?:@[^/]+|users\/[^/]+\/statuses)\/\d{5,25}(?:\/|$)/;

const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (char) => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
}[char]));

function send(message) {
  return new Promise((resolve) => {
    chrome.runtime.sendMessage(message, (response) => {
      if (chrome.runtime.lastError) {
        resolve({ ok: false, error: "extension_unavailable", message: chrome.runtime.lastError.message });
      } else resolve(response || { ok: false, error: "no_response", message: "No response" });
    });
  });
}

function show(view) {
  for (const section of document.querySelectorAll("[data-view]")) {
    section.hidden = section.dataset.view !== view;
  }
}

function note(selector, message) {
  const node = $(selector);
  if (!node) return;
  node.textContent = message || "";
  node.hidden = !message;
}

function humanError(result) {
  const code = result?.error || "";
  if (code === "invalid_credentials") return "Wrong username, email, or password.";
  if (code === "totp_required" || code === "invalid_totp") return "Enter the two-factor code from your authenticator.";
  if (code === "account_pending_verification") return "Verify your email address on ForkMesh first.";
  if (code === "forkmesh_unreachable") return "Could not reach the ForkMesh server.";
  if (code === "plugin_not_installed") return "This organization has not installed the CRM package.";
  if (code === "organization_required") return "Pick an organization first.";
  if (code === "invalid_instance") return "Enter an instance host, like mastodon.social.";
  if (code === "invalid_url") return "This tab has no web address to save.";
  if (code === "notes_full") return "That contact's notes are full; trim them in the CRM first.";
  if (code === "contact_not_found") return "That contact is no longer in the CRM.";
  if (code === "permission_denied") return "Chrome did not grant access to that instance.";
  if (code === "invalid_x_handle") return "Enter an X handle, like forkmesh.";
  if (code === "x_profile_not_read") return "Open the X profile first so there is something to record.";
  if (code === "marketing_not_installed") return "Install the Marketing package for one of your organizations first.";
  if (code === "not_signed_in") return "Sign in to ForkMesh first.";
  if (code === "invalid_reddit_thread_url") return "Paste a Reddit thread or comment link, like https://www.reddit.com/r/devops/comments/….";
  if (code === "reddit_check_running") return "A check is already running.";
  if (code === "reddit_walk_running") return "A walk is already running.";
  if (code === "reddit_profile_not_read") return "This Reddit page could not be read: reload it, then try again.";
  if (code === "reddit_watch_empty") return "The watch list is empty. Add subreddits and searches on the Reddit desk, then press Refresh watch list.";
  if (code === "capture_found_no_numbers") return "No follower, post, view or review counts were found on that page. Open the channel's profile page and try again.";
  if (code === "capture_sign_in_needed") return "The site asked you to sign in. Sign in there, then try again.";
  if (code === "capture_host_mismatch") return "That page is not on this channel's site. Pick the right channel or set its link in Marketing.";
  if (code === "channel_not_configured") return "Add that channel in Marketing first.";
  if (code === "channel_url_required") return "Add the channel's link in Marketing first.";
  if (code === "capture_page_timeout") return "The page took too long to load.";
  if (code === "capture_tab_closed") return "The channel's tab was closed before it was read.";
  return result?.message || "Something went wrong.";
}

async function renderOrganizations() {
  const select = $("[data-organization]");
  const hint = $("[data-org-hint]");
  select.innerHTML = `<option value="">Loading…</option>`;
  const result = await send({ type: "fm-organizations" });
  if (!result.ok) {
    select.innerHTML = `<option value="">Unavailable</option>`;
    hint.textContent = humanError(result);
    return;
  }
  const orgs = result.organizations || [];
  select.innerHTML = orgs.length
    ? orgs.map((org) => `<option value="${org.name}"${org.installed ? "" : " disabled"}>${org.name} · ${org.role}${org.installed ? "" : " · CRM not installed"}</option>`).join("")
    : `<option value="">No organizations</option>`;
  select.value = result.organization || "";
  hint.textContent = orgs.some((org) => org.installed)
    ? "Linked emails land in this organization's CRM."
    : "Install the CRM package for one of your organizations from the ForkMesh Market first.";
}

async function renderSent() {
  const result = await send({ type: "fm-sent-settings" });
  if (!result.ok) return note("[data-sent-status]", humanError(result));
  $("[data-track-sent]").checked = Boolean(result.track);
  const last = result.last;
  const intro = result.track
    ? "Mail you send from Gmail or Zoho Mail to someone in the CRM is linked to their record."
    : "Sent mail is not linked on its own; use the panel on the message instead.";
  const recent = !last ? "" : last.error
    ? ` Last send not tracked (${ago(last.at)}): ${last.error}.`
    : ` Last tracked ${ago(last.at)}: ${last.subject || "(no subject)"} → ${last.contact || "contact"}.`;
  note("[data-sent-status]", intro + recent);
}

async function renderToots() {
  const result = await send({ type: "fm-toot-settings" });
  if (!result.ok) return note("[data-toot-status]", humanError(result));
  $("[data-track-toots]").checked = Boolean(result.track);
  const last = result.last;
  const intro = result.track
    ? "Open one of your own Mastodon posts and everyone it mentions is logged in the CRM, new people as contacts, with a ping for each."
    : "Your Mastodon posts are not logged on their own; use the panel on the post instead.";
  const people = last?.contacts?.length ? last.contacts.join(", ") : "";
  const recent = !last ? "" : last.error && !people
    ? ` Last post not logged (${ago(last.at)}): ${last.error}.`
    : people ? ` Last logged ${ago(last.at)}: ${people}${last.created ? ` (${last.created} new)` : ""}.` : "";
  note("[data-toot-status]", intro + recent);
}

async function renderStatus(status) {
  const current = status || await send({ type: "fm-status" });
  $("[data-account]").textContent = current.connected ? current.account || "" : "";
  if (!current.ok && current.error) {
    show("sign-in");
    note("[data-error]", humanError(current));
    return;
  }
  if (!current.connected) {
    show("sign-in");
    $("[data-base-url]").value = current.baseUrl && current.baseUrl !== "https://app.forkmesh.com" ? current.baseUrl : "";
    return;
  }
  show("connected");
  // The vault comes first: it needs no organization and no CRM install.
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  void ForkMeshVaultPopup.render(tab?.url || "");
  await renderReddit();
  await pollRedditWalk();
  await renderHere();
  await renderOrganizations();
  await renderSent();
  await renderToots();
  await renderMastodon();
  await renderNote();
  await renderX();
  await renderPanel();
}

async function signIn(event) {
  event.preventDefault();
  const button = $("[data-sign-in]");
  button.disabled = true;
  note("[data-error]", "");
  try {
    const baseUrl = $("[data-base-url]").value.trim();
    if (baseUrl) {
      // A self-hosted server is outside the manifest's hosts: ask Chrome once.
      const pattern = await send({ type: "fm-host-pattern", baseUrl });
      if (!pattern.ok) throw new Error(humanError(pattern));
      const granted = await chrome.permissions.request({ origins: [pattern.pattern] });
      if (!granted) throw new Error("Chrome did not grant access to that server.");
    }
    const result = await send({
      type: "fm-sign-in", baseUrl,
      identifier: $("[data-identifier]").value, password: $("[data-password]").value,
      totp: $("[data-totp]").value,
    });
    $("[data-password]").value = "";
    $("[data-totp]").value = "";
    if (!result.ok) throw new Error(humanError(result));
    void refreshMailTab();
    await renderStatus(result);
  } catch (error) {
    note("[data-error]", error.message || String(error));
  } finally {
    button.disabled = false;
  }
}

// A tab the extension has a reader on: Gmail, Zoho Mail, or one of the
// Mastodon instances that were added.
async function activeReadableTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  if (!tab?.id) return null;
  if (MAIL_URL.test(tab.url || "")) return tab;
  try {
    const url = new URL(tab.url || "");
    return url.protocol === "https:" && mastodonHosts.includes(url.hostname) ? tab : null;
  } catch {
    return null;
  }
}

// The same hosts the service worker accepts: a bare host, a URL, or the
// instance half of a handle. Chrome grants a permission only inside the click
// that asked for it, so the popup works the host out without a round trip.
function instanceHost(value) {
  const raw = String(value || "").trim().toLowerCase().replace(/\/+$/, "");
  const host = raw.includes("@") ? raw.split("@").pop() : raw;
  try {
    const url = new URL(host.includes("://") ? host : `https://${host}`);
    if (url.protocol !== "https:" || url.pathname !== "/") return "";
    return /^[a-z0-9-]+(\.[a-z0-9-]+)+$/.test(url.hostname) ? url.hostname : "";
  } catch {
    return "";
  }
}

async function renderMastodon() {
  const result = await send({ type: "fm-mastodon-hosts" });
  mastodonHosts = result.hosts || [];
  $("[data-mastodon-list]").innerHTML = mastodonHosts.length
    ? mastodonHosts.map((host) =>
      `<li><span>${host}</span><button data-mastodon-remove="${host}" class="link" type="button">Remove</button></li>`).join("")
    : `<li><span class="muted">No instances yet.</span></li>`;
}

async function addMastodon() {
  note("[data-mastodon-error]", "");
  const input = $("[data-mastodon-host]");
  const host = instanceHost(input.value);
  if (!host) { note("[data-mastodon-error]", "Enter an instance host, like mastodon.social."); return; }
  const granted = await chrome.permissions.request({ origins: [`https://${host}/*`] });
  if (!granted) { note("[data-mastodon-error]", `Chrome did not grant access to ${host}.`); return; }
  const added = await send({ type: "fm-mastodon-add", host });
  if (!added.ok) { note("[data-mastodon-error]", humanError(added)); return; }
  input.value = "";
  await renderMastodon();
}

// A Mastodon post open on an instance that was not added yet. The popup offers
// to record it: that adds the instance and starts the reader on this tab.
async function unreadTootTab() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  try {
    const url = new URL(tab?.url || "");
    const host = instanceHost(url.hostname);
    if (!tab?.id || !host || url.protocol !== "https:" || mastodonHosts.includes(host)) return null;
    return MASTODON_POST.test(url.pathname) ? { tab, host } : null;
  } catch {
    return null;
  }
}

// "Record this toot in the CRM": Chrome asks for the instance in this click
// (before any other wait), the service worker adds it and starts the reader
// on the open tab, and the panel then links the toot like any conversation.
async function recordToot() {
  if (!unreadToot) return;
  note("[data-toot-error]", "");
  const { tab, host } = unreadToot;
  const granted = await chrome.permissions.request({ origins: [`https://${host}/*`] });
  if (!granted) { note("[data-toot-error]", `Chrome did not grant access to ${host}.`); return; }
  const button = $("[data-toot-record]");
  button.disabled = true;
  const added = await send({ type: "fm-mastodon-add", host, tabId: tab.id });
  button.disabled = false;
  if (!added.ok) { note("[data-toot-error]", humanError(added)); return; }
  await renderMastodon();
  await renderPanel();
}

function readTab(tabId) {
  return new Promise((resolve) => {
    chrome.tabs.sendMessage(tabId, { type: "fm-read-message" }, (response) => {
      if (chrome.runtime.lastError) resolve({ ok: false, message: "Reload the tab so the extension can read it." });
      else resolve(response || { ok: false, message: "No message on screen." });
    });
  });
}

// The CRM header on the page reads everything again after a change here.
async function refreshMailTab() {
  const tab = await activeReadableTab();
  if (tab) chrome.tabs.sendMessage(tab.id, { type: "fm-refresh-page" }, () => void chrome.runtime.lastError);
}

// --- the open page, filed under a contact --------------------------------
// Any site at all: the address and title of the tab the icon was clicked on
// go onto a contact's notes. Only what the address bar shows is read.

async function renderNote() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  notePage = /^https?:\/\//i.test(tab?.url || "")
    ? { url: tab.url, title: tab.title || "" } : null;
  $("[data-note-section]").hidden = !notePage;
  if (!notePage) return;
  $("[data-note-page]").textContent = notePage.title
    ? `${notePage.title} — ${notePage.url}` : notePage.url;
}

// The contact list is only fetched when the section is opened: it is the
// whole organization, and most clicks on the icon never need it.
async function loadNoteContacts() {
  const select = $("[data-note-contact]");
  if (select.dataset.loaded) return;
  select.innerHTML = `<option value="">Loading…</option>`;
  const result = await send({ type: "fm-contacts" });
  if (!result.ok) {
    select.innerHTML = `<option value="">Unavailable</option>`;
    note("[data-note-error]", humanError(result));
    return;
  }
  const label = (contact) => [contact.name || contact.email || contact.mastodon || "Unnamed",
    contact.company].filter(Boolean).join(" · ");
  const contacts = (result.contacts || [])
    .sort((one, two) => label(one).localeCompare(label(two)));
  select.innerHTML = contacts.length
    ? contacts.map((contact) =>
      `<option value="${escapeHtml(contact.id)}">${escapeHtml(label(contact))}</option>`).join("")
    : `<option value="">No contacts yet</option>`;
  select.dataset.loaded = "1";
}

async function addNoteLink() {
  note("[data-note-error]", "");
  note("[data-note-status]", "");
  const contactId = $("[data-note-contact]").value;
  if (!notePage) { note("[data-note-error]", "This tab has no web address to save."); return; }
  if (!contactId) { note("[data-note-error]", "Pick a contact first."); return; }
  const button = $("[data-note-add]");
  button.disabled = true;
  const result = await send({
    type: "fm-note-link", contactId, url: notePage.url, title: notePage.title,
  });
  button.disabled = false;
  if (!result.ok) { note("[data-note-error]", humanError(result)); return; }
  note("[data-note-status]", `Added to ${result.contact?.name || "the contact"}'s notes.`);
}

const X_TILES = [
  ["followers", "Followers"], ["following", "Following"], ["posts", "Posts"], ["media", "Media"],
  ["replies", "Replies"], ["reposts", "Reposts"], ["likes", "Likes"], ["mentions", "Mentions"],
];

const { ago } = ForkMeshRefreshView;

async function renderX(result) {
  const stats = result?.ok ? result : await send({ type: "fm-x-stats" });
  if (!stats.ok) {
    note("[data-x-status]", humanError(stats));
    return;
  }
  const totals = stats.totals || {};
  const errors = stats.errors || [];
  $("[data-x-summary]").textContent = `X profile stats · @${stats.handle}${errors.length ? ` · ${errors.length} error${errors.length === 1 ? "" : "s"}` : ""}`;
  $("[data-x-handle]").placeholder = stats.handle;
  $("[data-x-grid]").innerHTML = X_TILES.map(([key, label]) => `<div><dt>${label}</dt><dd>${
    stats.readAt ? Number(totals[key] || 0).toLocaleString() : "—"}</dd></div>`).join("");
  const recent = stats.recent || [];
  $("[data-x-recent]").innerHTML = recent.map((post) => {
    const counts = [post.at ? new Date(post.at).toLocaleDateString() : "",
      `${Number(post.views || 0).toLocaleString()} views`, `${Number(post.likes || 0).toLocaleString()} likes`,
      post.images?.length ? plural(post.images.length, "image") : ""].filter(Boolean).join(" · ");
    const thumbs = (post.images || []).slice(0, 2).map((src) => `<img src="${escapeHtml(src)}" alt="" loading="lazy" referrerpolicy="no-referrer" />`).join("");
    return `<li>${thumbs ? `<span class="x-thumbs">${thumbs}</span>` : ""}<span><a href="${escapeHtml(post.url)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(post.text)}">${escapeHtml(post.text || "Post without text")}</a><small>${escapeHtml(counts)}</small></span></li>`;
  }).join("");
  $("[data-x-recent]").hidden = !recent.length;
  $("[data-x-recent-title]").hidden = !recent.length;
  const sync = stats.lastSync || {};
  const lines = [stats.readAt
    ? `Read ${ago(stats.readAt)} · ${Number(totals.views || 0).toLocaleString()} views on ${Number(totals.postsSeen || 0).toLocaleString()} posts from the last 30 days.`
    : `Open x.com/${stats.handle} to read its numbers.`];
  if (sync.at) lines.push(`Recorded in ${sync.organization} Marketing ${ago(sync.at)}${sync.linksAdded || sync.linksUpdated
    ? `: ${plural(sync.linksAdded || 0, "new post link")}, ${sync.linksUpdated || 0} updated` : ""}.`);
  if (!stats.connected) lines.push("Sign in to record them in Marketing.");
  note("[data-x-status]", lines.join(" "));
  $("[data-x-errors]").innerHTML = errors.map((item) => `<li>${escapeHtml(ago(item.at))}: ${escapeHtml(item.message)}</li>`).join("");
  $("[data-x-errors]").hidden = !errors.length;
  $("[data-x-clear]").hidden = !errors.length;
  $("[data-x-sync]").disabled = !stats.readAt || !stats.connected;
}

async function syncX() {
  $("[data-x-sync]").disabled = true;
  const result = await send({ type: "fm-x-sync" });
  if (!result.ok) note("[data-x-status]", humanError(result));
  else await renderX(result);
}

async function trackX() {
  const handle = $("[data-x-handle]").value.trim();
  if (!handle) return;
  const result = await send({ type: "fm-x-set-handle", handle });
  if (!result.ok) return note("[data-x-status]", humanError(result));
  $("[data-x-handle]").value = "";
  await renderX(result);
}

const REDDIT_TILES = [
  ["subreddits", "Subreddits"], ["queries", "Searches"], ["waiting", "To send"], ["ruleSets", "Rule sets"],
  ["mine", "Your posts"],
];

async function renderReddit(result) {
  const stats = result?.ok ? result : await send({ type: "fm-reddit-stats" });
  if (!stats.ok) {
    note("[data-reddit-status]", humanError(stats));
    return;
  }
  const errors = stats.errors || [];
  $("[data-reddit-summary]").textContent = `Reddit desk${stats.waiting ? ` · ${stats.waiting} to send` : ""}${errors.length ? ` · ${errors.length} error${errors.length === 1 ? "" : "s"}` : ""}`;
  $("[data-reddit-grid]").innerHTML = REDDIT_TILES.map(([key, label]) => `<div><dt>${label}</dt><dd>${
    Number(stats[key] || 0).toLocaleString()}</dd></div>`).join("");
  const sync = stats.lastSync || {};
  const lines = [stats.fetchedAt
    ? `Watch list from ${stats.organization} Marketing, fetched ${ago(stats.fetchedAt)}.`
    : "Press Refresh watch list to load the subreddits and searches Marketing watches."];
  if (stats.rulesWaiting) lines.push(`${stats.rulesWaiting} rule set${stats.rulesWaiting === 1 ? "" : "s"} to send.`);
  if (sync.at) {
    lines.push(sync.error
      ? `Last send ${ago(sync.at)} stopped: ${sync.error.replaceAll("_", " ")}.`
      : `Last send ${ago(sync.at)}: ${sync.threads} new thread${sync.threads === 1 ? "" : "s"}, ${sync.already} already logged, ${sync.rules} rule set${sync.rules === 1 ? "" : "s"}${sync.failed ? `, ${sync.failed} refused` : ""}.`);
  }
  if (stats.readingsWaiting) lines.push(`${stats.readingsWaiting} monitored reading${stats.readingsWaiting === 1 ? "" : "s"} to send.`);
  if ((stats.accounts || []).length) {
    lines.push(`Your accounts: ${stats.accounts.map((name) => `u/${name}`).join(", ")}. Press Update on one's profile page to record its posts and their views in Marketing.`);
  }
  if (stats.mineWaiting) lines.push(`${stats.mineWaiting} of your post${stats.mineWaiting === 1 ? "" : "s"} to send.`);
  const mine = stats.mineCheck || {};
  if (mine.at) {
    const who = mine.account ? `u/${mine.account}'s` : "your";
    lines.push(!mine.done && stats.mineChecking
      ? `Reading ${who} views: ${mine.read + mine.missed} of ${mine.total} done.`
      : `Views of ${who} posts read ${ago(mine.finishedAt || mine.at)}: ${mine.read} of ${mine.total}${mine.missed ? `, ${mine.missed} showed no views (sign in to Reddit as that account to see them)` : ""}${mine.error ? `, sending stopped: ${mine.error.replaceAll("_", " ")}` : ""}.`);
  }
  const check = stats.lastCheck || {};
  if (stats.checking) lines.push("Checking monitored threads…");
  else if (check.at) {
    lines.push(check.error
      ? `Last check ${ago(check.at)} stopped: ${check.error.replaceAll("_", " ")}.`
      : `Last check ${ago(check.at)}: read ${check.read} of ${check.checked}${check.missed ? `, ${check.missed} could not be read` : ""}.`);
  }
  if (!stats.connected) lines.push("Sign in to send them to Marketing.");
  note("[data-reddit-status]", lines.join(" "));
  renderRedditMonitored(stats);
  $("[data-reddit-errors]").innerHTML = errors.map((item) => `<li>${escapeHtml(ago(item.at))}: ${escapeHtml(item.message)}</li>`).join("");
  $("[data-reddit-errors]").hidden = !errors.length;
  $("[data-reddit-clear]").hidden = !errors.length;
  const missing = stats.fetchedAt ? stats.rulesMissing || [] : [];
  $("[data-reddit-rules]").innerHTML = missing.map((item) =>
    `<li><a href="${escapeHtml(item.url)}" data-reddit-rules-open target="_blank" rel="noopener">r/${escapeHtml(item.name)} rules ↗</a></li>`).join("");
  $("[data-reddit-rules-section]").hidden = !missing.length;
  $("[data-reddit-refresh]").disabled = !stats.connected;
  $("[data-reddit-sync]").disabled = !stats.connected || (!stats.waiting && !stats.rulesWaiting && !stats.readingsWaiting && !stats.mineWaiting);
}

const count = (value, one, many) => `${Number(value).toLocaleString()} ${value === 1 ? one : many}`;

function renderRedditMonitored(stats) {
  const monitored = stats.monitored || [];
  $("[data-reddit-monitored]").innerHTML = monitored.map((item) => {
    const parts = [];
    if (item.views !== null) parts.push(count(item.views, "view", "views"));
    if (item.score !== null) parts.push(count(item.score, "point", "points"));
    if (item.comments !== null) parts.push(count(item.comments, "comment", "comments"));
    if (item.commentUrl && (item.commentScore !== null || item.commentViews !== null)) {
      parts.push(`comment: ${[item.commentScore !== null ? count(item.commentScore, "point", "points") : "",
        item.commentViews !== null ? count(item.commentViews, "view", "views") : ""].filter(Boolean).join(", ")}`);
    }
    const when = item.checkedAt ? `read ${ago(item.checkedAt)}${item.waiting ? ", not sent yet" : ""}` : "not read yet";
    return `<li><span><a href="${escapeHtml(item.commentUrl || item.url)}" data-reddit-monitor-open target="_blank" rel="noopener">${escapeHtml(item.title || `r/${item.subreddit} thread`)}</a><small>${escapeHtml([...parts, when].join(" · "))}</small></span><button data-reddit-monitor-remove="${escapeHtml(item.deskId)}" class="link" type="button">Stop</button></li>`;
  }).join("");
  $("[data-reddit-auto-check]").checked = Boolean(stats.autoCheck);
  $("[data-reddit-check]").disabled = !stats.connected || (!monitored.length && !stats.mine) || Boolean(stats.checking);
  $("[data-reddit-monitor-add]").disabled = !stats.connected;
}

async function redditMonitorAdd() {
  const url = $("[data-reddit-monitor-url]").value.trim();
  if (!url) return;
  $("[data-reddit-monitor-add]").disabled = true;
  const result = await send({ type: "fm-reddit-monitor-add", url });
  $("[data-reddit-monitor-add]").disabled = false;
  if (!result.ok) return note("[data-reddit-status]", humanError(result));
  $("[data-reddit-monitor-url]").value = "";
  await renderReddit(result);
}

async function redditPrefillMonitor() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const url = String(tab?.url || "");
  if (/^https:\/\/(?:www\.|old\.)?reddit\.com\/r\/[^/]+\/comments\//i.test(url) && !$("[data-reddit-monitor-url]").value) {
    $("[data-reddit-monitor-url]").value = url.split(/[?#]/)[0];
  }
}

const WALK_STATES = { running: "Walking", stopping: "Stopping", done: "Walked", stopped: "Stopped", error: "Stopped" };
let redditWalkTimer = 0;

function redditWalkLine(walk) {
  if (!walk) return "";
  const stop = (walk.stops || [])[walk.index] || null;
  const read = (walk.stops || []).filter((item) => item.state === "read").length;
  const label = WALK_STATES[walk.state] || "Walking";
  const where = walk.state === "running" && stop ? ` ${stop.label}` : "";
  const sync = walk.synced;
  const sent = sync && !sync.error
    ? ` ${count(sync.threads, "new thread", "new threads")}, ${sync.already} already logged${sync.rules ? `, ${sync.rules} rule sets` : ""}.`
    : sync?.error ? ` The send stopped: ${String(sync.error).replaceAll("_", " ")}.` : "";
  const failed = walk.state === "error" ? ` ${humanError(walk)}` : "";
  return `${label}${where}: ${read} of ${(walk.stops || []).length} pages read, ${count(walk.posts || 0, "post", "posts")} seen.${sent}${failed}`;
}

function renderRedditWalk(walk) {
  const node = $("[data-reddit-walk-status]");
  const button = $("[data-reddit-walk]");
  node.hidden = !walk;
  node.textContent = redditWalkLine(walk);
  const running = Boolean(walk) && (walk.state === "running" || walk.state === "stopping");
  button.textContent = running ? "Stop the walk" : "Walk the watch list";
  clearTimeout(redditWalkTimer);
  if (running) redditWalkTimer = setTimeout(() => void pollRedditWalk(), 1200);
  else if (walk && walk.finishedAt) void renderReddit();
}

async function pollRedditWalk() {
  const result = await send({ type: "fm-reddit-walk-state" });
  if (result.ok) renderRedditWalk(result.walk);
}

async function toggleRedditWalk() {
  const current = await send({ type: "fm-reddit-walk-state" });
  const running = current.ok && current.walk && ["running", "stopping"].includes(current.walk.state);
  const result = await send({ type: running ? "fm-reddit-walk-stop" : "fm-reddit-walk" });
  if (!result.ok) { note("[data-reddit-status]", humanError(result)); return; }
  renderRedditWalk(result.walk);
}

async function redditAction(type, button) {
  $(button).disabled = true;
  const result = await send({ type });
  $(button).disabled = false;
  if (!result.ok) note("[data-reddit-status]", humanError(result));
  else await renderReddit(result);
}

const CAPTURE_LABELS = [["followers", "followers"], ["posts", "posts"], ["likes", "likes"],
  ["views", "views"], ["reviews", "reviews"]];
let marketingSetup = null;

function captureLine(item) {
  const name = escapeHtml(item.name || item.channel);
  if (item.state === "error") return `<li class="failed">${name} · ${escapeHtml(ago(item.finishedAt || item.startedAt))}: ${escapeHtml(humanError(item))}</li>`;
  const metrics = item.result?.metrics || {};
  const parts = CAPTURE_LABELS.filter(([key]) => metrics[key]).map(([key, label]) => `${Number(metrics[key]).toLocaleString()} ${label}`);
  if (metrics.rating) parts.push(`${metrics.rating}★`);
  return `<li>${name} · ${escapeHtml(ago(item.finishedAt || item.startedAt))}${parts.length ? `: ${escapeHtml(parts.join(" · "))}` : ""}</li>`;
}

function hostMatches(url, domains) {
  try {
    const host = new URL(url).hostname.toLowerCase();
    return domains.some((domain) => host === domain || host.endsWith(`.${domain}`));
  } catch {
    return false;
  }
}

function capturePatterns(setup) {
  const patterns = new Set();
  for (const channel of setup?.channels || []) {
    if (channel.server) continue;
    for (const domain of channel.hosts || []) patterns.add(`https://*.${domain}/*`);
    try { if (channel.url) patterns.add(`${new URL(channel.url).origin}/*`); } catch { }
  }
  return [...patterns];
}

async function renderMarketing() {
  const result = await send({ type: "fm-marketing-setup" });
  if (!result.ok) {
    note("[data-marketing-status]", humanError(result));
    $("[data-marketing-send]").disabled = true;
    $("[data-marketing-allow]").disabled = true;
    return;
  }
  marketingSetup = result;
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const url = tab?.url || "";
  const configured = result.channels || [];
  const detected = channelFor(result, url);
  const ids = new Set(configured.map((item) => item.id));
  const others = (result.catalog || []).filter((item) => !ids.has(item.id));
  $("[data-marketing-channel]").innerHTML = configured.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}</option>`).join("")
    + (others.length ? `<optgroup label="Not added yet">${others.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}</option>`).join("")}</optgroup>` : "");
  if (detected) $("[data-marketing-channel]").value = detected.id;
  const history = result.history || [];
  $("[data-marketing-history]").innerHTML = history.slice(0, 6).map(captureLine).join("");
  const failed = history.filter((item) => item.state === "error").length;
  $("[data-marketing-summary]").textContent = `Marketing channels · ${result.organization}${failed ? ` · ${failed} failed` : ""}`;
  const patterns = capturePatterns(result);
  const granted = patterns.length ? await chrome.permissions.contains({ origins: patterns }) : true;
  $("[data-marketing-allow]").hidden = granted;
  $("[data-marketing-send]").disabled = !/^https:/.test(url);
  note("[data-marketing-status]", detected ? `This page looks like ${detected.name}.` : "");
}

async function sendPageToMarketing() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const channel = $("[data-marketing-channel]").value;
  if (!tab?.id || !channel) return;
  $("[data-marketing-send]").disabled = true;
  note("[data-marketing-status]", "Reading this page…");
  const result = await send({ type: "fm-marketing-capture-tab", tabId: tab.id, channel, organization: marketingSetup?.organization || "" });
  $("[data-marketing-send]").disabled = false;
  if (!result.ok) return note("[data-marketing-status]", humanError(result));
  if (result.job.state === "error") return note("[data-marketing-status]", humanError(result.job));
  note("[data-marketing-status]", `Recorded in ${result.job.organization} Marketing${postsAdded(result.job)}.`);
  await renderMarketing();
}

function allowChannelSites() {
  const patterns = capturePatterns(marketingSetup);
  if (!patterns.length) return;
  chrome.permissions.request({ origins: patterns }, (granted) => {
    note("[data-marketing-status]", granted ? "Channel sites allowed: each Update button now works in one click." : humanError({ error: "permission_denied" }));
    void renderMarketing();
  });
}

const REDDIT_HOST = /^(?:www\.|old\.|new\.)?reddit\.com$/;
const REDDIT_READ_HOST = /^(?:www|old)\.reddit\.com$/;
const REDDIT_LISTINGS = new Set(["new", "hot", "top", "rising", "controversial", "best"]);
const FORKMESH_HOST = /(^|\.)forkmesh\.com$/;
let hereBusy = false;
let liveTimer = 0;

function channelFor(setup, url) {
  const configured = setup?.channels || [];
  return configured.find((item) => { try { return item.url && hostMatches(url, [new URL(item.url).hostname.toLowerCase()]); } catch { return false; } })
    || configured.find((item) => hostMatches(url, item.hosts || []))
    || (setup?.catalog || []).find((item) => hostMatches(url, item.hosts || []));
}

function redditPage(url) {
  let parsed;
  try { parsed = new URL(url); } catch { return null; }
  const host = parsed.hostname.toLowerCase();
  if (parsed.protocol !== "https:" || !REDDIT_HOST.test(host)) return null;
  const parts = parsed.pathname.split("/").filter(Boolean).map((part) => {
    try { return decodeURIComponent(part); } catch { return part; }
  });
  const read = REDDIT_READ_HOST.test(host);
  const query = String(parsed.searchParams.get("q") || "").replace(/\s+/g, " ").trim().toLowerCase().slice(0, 120);
  if (parts[0]?.toLowerCase() === "search") return { kind: "search", query, read };
  if (["user", "u"].includes(parts[0]?.toLowerCase()) && /^[A-Za-z0-9_-]{1,20}$/.test(parts[1] || "")) {
    return { kind: "profile", account: parts[1], read };
  }
  if (parts[0]?.toLowerCase() !== "r" || !/^[a-z0-9_]{2,21}$/i.test(parts[1] || "")) return { kind: "other", read };
  const name = parts[1];
  const section = String(parts[2] || "").toLowerCase();
  if (section === "search") return { kind: "search", query, name, read };
  if (section === "comments") return { kind: "thread", name, read };
  if (section === "about" && String(parts[3] || "").toLowerCase() === "rules") return { kind: "rules", name, read };
  return { kind: !section || REDDIT_LISTINGS.has(section) ? "listing" : "other", name, read };
}

function samePage(url, saved) {
  try {
    const one = new URL(url);
    const two = new URL(saved);
    const host = (item) => item.hostname.toLowerCase().replace(/^(?:www|old|new|m|mobile)\./, "");
    const path = (item) => item.pathname.toLowerCase().replace(/\/+$/, "");
    return host(one) === host(two) && (path(one) === path(two) || path(one).startsWith(`${path(two)}/`));
  } catch {
    return false;
  }
}

const plural = (count, word) => `${count} ${word}${count === 1 ? "" : "s"}`;

function redditLines(page, stats) {
  if (!stats.ok) return [humanError(stats)];
  if (!page.read) return ["The Reddit desk reads www.reddit.com and old.reddit.com. Open this page there to collect it."];
  if (page.kind === "profile") {
    const mine = (stats.accounts || []).some((name) => name.toLowerCase() === page.account.toLowerCase());
    return [mine
      ? `u/${page.account} is one of your accounts: Update records each post and comment on this page, with its link, score and views, in Marketing posts.`
      : `Update adds u/${page.account} to the Reddit desk's accounts (owners and admins), then records each post and comment on this page, with its link, score and views, in Marketing posts.`,
    "Views are read by opening each link in a background tab; Reddit shows them only while you are signed in as that account."];
  }
  if (!stats.fetchedAt) return ["Press Refresh watch list to load the subreddits and searches Marketing watches; then the pages you open here are collected for the Reddit desk."];
  const sub = `r/${page.name}`;
  const watched = page.name && (stats.watched || []).includes(page.name.toLowerCase());
  const lines = [];
  if (page.kind === "search") {
    lines.push(!page.query ? "Search Reddit for one of the desk's watched searches to collect its posts."
      : (stats.watchedQueries || []).includes(page.query)
        ? `“${page.query}” is a watched search: every post listed here is collected for the desk.`
        : `“${page.query}” is not one of the desk's watched searches, so this page is not collected.`);
  } else if (!page.name) {
    lines.push("Open a watched subreddit, a thread in one, its rules page or a watched search to collect threads for the desk.");
  } else if (!watched) {
    lines.push(`${sub} is not on the desk's watch list, so this page is not collected. Owners and admins add it in Marketing; then press Refresh watch list.`);
  } else if (page.kind === "thread") {
    lines.push(`${sub} is watched: this thread is collected for the desk.`);
  } else if (page.kind === "rules") {
    lines.push(`${sub} is watched: the rules on this page go to the desk with the next send.`);
  } else if (page.kind === "listing") {
    lines.push(`${sub} is watched: posts here whose title holds every word of a watched search are collected for the desk.`);
  } else {
    lines.push(`${sub} is watched. Open its listing, a thread or its rules page to collect them.`);
  }
  lines.push(stats.waiting || stats.rulesWaiting
    ? `Waiting to send: ${plural(stats.waiting || 0, "thread")}, ${plural(stats.rulesWaiting || 0, "rule set")}.`
    : "Nothing waiting to send.");
  return lines;
}

function redditActions(page, stats, tab) {
  if (page.kind === "profile") {
    return [
      { label: `Update u/${page.account} posts and views`, primary: true, disabled: !stats.connected,
        run: () => hereProfile(tab) },
      { label: "Open Reddit desk", run: () => send({ type: "fm-open-reddit-desk" }) },
    ];
  }
  const waiting = (stats.waiting || 0) + (stats.rulesWaiting || 0);
  const actions = [
    { label: `Send to Reddit desk${waiting ? ` (${waiting})` : ""}`, primary: true, disabled: !stats.connected || !waiting,
      run: () => hereReddit("fm-reddit-sync") },
    { label: "Refresh watch list", disabled: !stats.connected, run: () => hereReddit("fm-reddit-refresh") },
  ];
  if (page.name && page.kind !== "rules") {
    actions.push({ label: `Read r/${page.name} rules`,
      run: () => hereGo(tab, `https://old.reddit.com/r/${encodeURIComponent(page.name)}/about/rules`) });
  }
  actions.push({ label: "Open Reddit desk", run: () => send({ type: "fm-open-reddit-desk" }) });
  return actions;
}

function channelPlan(setup, channel, tab, covered) {
  const configured = (setup.channels || []).find((item) => item.id === channel.id);
  const name = configured?.name || channel.name;
  const update = covered ? [] : [{ label: `Update ${name} stats from this page`, primary: true,
    run: () => hereCapture({ type: "fm-marketing-capture-tab", tabId: tab.id, channel: channel.id }) }];
  if (configured?.server) {
    return { lines: covered ? [] : [`${name} counts are read by Marketing from Discord's public invite, not from this page.`], actions: update };
  }
  if (configured?.url && !samePage(tab.url, configured.url)) {
    return { lines: [`This is not the page saved for ${name} in Marketing: Update from this page records its numbers as ${name}'s.`],
      actions: [
        ...update,
        { label: "Update from saved page", run: () => hereCapture({ type: "fm-marketing-capture", channel: channel.id }) },
        { label: `Go to ${name} page`, run: () => hereGo(tab, configured.url) },
      ] };
  }
  const lines = configured?.url ? [] : [configured
    ? `${name} has no saved link in Marketing. Owners and admins who update from this page save it as the link.`
    : `${name} is not one of ${setup.organization}'s Marketing channels yet. Updating from this page adds it, with this page as its link (owners and admins).`];
  return { lines, actions: update };
}

const here = { tab: null, plan: null, title: "", kind: "", lines: [], actions: [] };
let hereStatus = null;

function drawHere() {
  ForkMeshRefreshView.render($("[data-here]"), here.plan, {
    busy: hereBusy, status: hereStatus, title: here.title, kind: here.kind, lines: here.lines, actions: here.actions,
    go: (url) => void hereGo(here.tab, url),
    run: () => void hereWork({ type: "fm-refresh-run" }),
    update: (next) => void hereWork({ type: "fm-refresh-channel", channel: next.channel }),
    open: here.tab ? () => void chrome.sidePanel.open({ windowId: here.tab.windowId }).then(() => window.close()) : null,
  });
}

function hereWork(message) {
  return hereRun(async () => {
    const result = await send({ ...message, url: here.tab.url, tabId: here.tab.id });
    if (!result.ok) throw new Error(humanError(result));
    if (result.failed) throw new Error(result.message);
    return result.message;
  }, "Working…");
}

async function renderHere() {
  const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const url = tab?.url || "";
  let host = "";
  try { host = new URL(url).hostname.toLowerCase(); } catch { }
  const result = url ? await send({ type: "fm-refresh-plan", url }) : null;
  const plan = result?.ok ? result : null;
  if (!tab?.id || !/^https:/.test(url) || MAIL_URL.test(url) || FORKMESH_HOST.test(host)) {
    Object.assign(here, { tab: tab || null, plan, title: "", kind: "", lines: [], actions: [] });
    return drawHere();
  }
  const reddit = redditPage(url);
  const [setup, stats, x] = await Promise.all([
    send({ type: "fm-marketing-setup" }),
    reddit ? send({ type: "fm-reddit-stats" }) : null,
    send({ type: "fm-x-stats" }),
  ]);
  if (setup.ok) marketingSetup = setup;
  const channel = setup.ok ? channelFor(setup, url) : null;
  const saved = (setup.channels || []).find((item) => item.id === channel?.id);
  const covered = Boolean(channel && plan?.refreshes?.includes(channel.id));
  const sending = plan?.here && plan.site === "reddit";
  const syncing = plan?.here && plan.site === "x";
  const lines = [];
  const actions = [];
  if (reddit) {
    lines.push(...redditLines(reddit, stats));
    actions.push(...redditActions(reddit, stats, tab).slice(sending ? 1 : 0));
  }
  if (channel && reddit?.kind !== "profile") {
    const part = channelPlan(setup, channel, tab, covered);
    lines.push(...part.lines);
    actions.splice(reddit && !sending ? 1 : 0, 0, ...part.actions);
  }
  if (channel?.id === "x" && x.ok && x.live?.tabId === tab.id) {
    lines.push(`Live: @${x.handle}'s posts and numbers read in this tab go to ${x.live.organization} Marketing as they load${x.live.at
      ? ` (${plural(x.live.linksAdded || 0, "new post link")}, ${x.live.linksUpdated || 0} updated, last ${ago(x.live.at)})` : ""}.`);
    actions.push({ label: "Stop live updates", run: () => hereRun(async () => {
      const result = await send({ type: "fm-x-live-stop" });
      if (!result.ok) throw new Error(humanError(result));
      await renderX(result);
      return "Live updates stopped.";
    }) });
  }
  if (channel?.id === "x" && x.ok && x.connected && x.readAt && !syncing) {
    lines.push(`X profile stats for @${x.handle} were read ${ago(x.readAt)}.`);
    actions.push({ label: "Sync X stats", run: () => hereRun(async () => {
      const result = await send({ type: "fm-x-sync" });
      if (!result.ok) throw new Error(humanError(result));
      await renderX(result);
      return `X stats and ${plural(result.lastSync?.linksAdded || 0, "new post link")} recorded in ${result.lastSync?.organization || "Marketing"}.`;
    }) });
  }
  if (channel || reddit || plan?.pages?.length) actions.push({ label: "Open Marketing", run: () => openMarketing() });
  const suggest = plan?.suggest;
  const mine = Boolean(channel && !covered && suggest?.current && suggest.channel === channel.id);
  Object.assign(here, {
    tab, plan: mine ? { ...plan, suggest: { ...suggest, covered: true } } : plan, lines, actions,
    title: reddit ? `Reddit${reddit.name ? ` · r/${reddit.name}` : reddit.account ? ` · u/${reddit.account}` : ""}` : saved?.name || channel?.name || "",
    kind: reddit ? { listing: "subreddit", thread: "thread", rules: "rules page", search: "search", profile: "profile" }[reddit.kind] || "this page"
      : channel ? "Marketing channel" : "",
  });
  drawHere();
}

async function hereRun(action, busy = "") {
  hereBusy = true;
  hereStatus = busy ? { text: busy, busy: true } : null;
  drawHere();
  try {
    hereStatus = { text: await action() || "" };
  } catch (error) {
    hereStatus = { text: error.message || String(error), error: true };
  }
  hereBusy = false;
  await renderHere();
}

const postsAdded = (job) => {
  const posts = job.result?.posts;
  return posts ? ` with ${plural(posts.linksAdded || 0, "new post link")} and ${posts.linksUpdated || 0} updated` : "";
};

function liveRefresh() {
  if (liveTimer) return;
  liveTimer = setTimeout(async () => {
    liveTimer = 0;
    await renderX();
    if (!hereBusy && here.actions.length) await renderHere();
  }, 500);
}

function hereReddit(type) {
  return hereRun(async () => {
    const result = await send({ type });
    if (!result.ok) throw new Error(humanError(result));
    await renderReddit(result);
    if (type === "fm-reddit-refresh") return "Watch list refreshed.";
    const sync = result.lastSync || {};
    if (sync.error) throw new Error(`Send stopped: ${sync.error.replaceAll("_", " ")}.`);
    return `Sent: ${plural(sync.threads || 0, "new thread")}, ${sync.already || 0} already logged, ${plural(sync.rules || 0, "rule set")}.`;
  });
}

function hereProfile(tab) {
  return hereRun(async () => {
    const result = await send({ type: "fm-reddit-profile-update", tabId: tab.id });
    if (!result.ok) throw new Error(humanError(result));
    await renderReddit(result);
    return result.text;
  }, "Reading this profile…");
}

function hereCapture(message) {
  return hereRun(async () => {
    const result = await send({ ...message, organization: marketingSetup?.organization || "" });
    if (!result.ok) throw new Error(humanError(result));
    if (result.job.state === "error") throw new Error(humanError(result.job));
    if (result.job.state !== "done") return `${result.job.name}: update started.`;
    if ($("[data-marketing-section]").open) await renderMarketing();
    const live = result.job.result?.posts?.tabId === message.tabId
      ? " Live: new posts and numbers on this page keep going there while the tab is open." : "";
    return `Recorded ${result.job.name} in ${result.job.organization} Marketing${postsAdded(result.job)}.${live}`;
  }, "Reading…");
}

function hereGo(tab, url) {
  return chrome.tabs.update(tab.id, { url }).then(() => window.close());
}

async function openMarketing() {
  const current = await send({ type: "fm-status" });
  const org = marketingSetup?.organization ? `?org=${encodeURIComponent(marketingSetup.organization)}&page=channels` : "?page=channels";
  void chrome.tabs.create({ url: `${current.baseUrl || "https://app.forkmesh.com"}/dashboard/marketing${org}` });
}

async function renderPanel() {
  const hostNode = $("[data-panel-host]");
  const tab = await activeReadableTab();
  unreadToot = tab ? null : await unreadTootTab();
  hostNode.hidden = !tab;
  $("[data-toot]").hidden = !unreadToot;
  $("[data-toot-host]").textContent = unreadToot?.host || "";
  $("[data-mail-hint]").hidden = Boolean(tab || unreadToot || here.actions.length);
  if (!tab) return;
  if (!panel) {
    hostNode.dataset.theme = matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    const element = document.createElement("div");
    ForkMeshCrm.mount(hostNode).append(element);
    panel = ForkMeshCrm.create({
      element, send, embedded: true,
      read: async () => {
        const read = await readTab(tab.id);
        if (!read.ok) throw new Error(read.message || "Could not read the message.");
        return read.message;
      },
      onLinked: () => void refreshMailTab(),
    });
  }
  await panel.refresh();
}

document.addEventListener("DOMContentLoaded", () => {
  ForkMeshVaultPopup.bind(send);
  $("[data-sign-in-form]").addEventListener("submit", signIn);
  $("[data-organization]").addEventListener("change", async (event) => {
    await send({ type: "fm-set-organization", organization: event.target.value });
    // The contacts on offer belong to the organization that was showing.
    delete $("[data-note-contact]").dataset.loaded;
    if ($("[data-note-section]").open) await loadNoteContacts();
    await renderPanel();
    void refreshMailTab();
  });
  $("[data-track-sent]").addEventListener("change", async (event) => {
    await send({ type: "fm-sent-set-track", track: event.target.checked });
    await renderSent();
  });
  $("[data-track-toots]").addEventListener("change", async (event) => {
    await send({ type: "fm-toot-set-track", track: event.target.checked });
    await renderToots();
  });
  $("[data-note-section]").addEventListener("toggle", (event) => {
    if (event.target.open) void loadNoteContacts();
  });
  $("[data-note-add]").addEventListener("click", () => void addNoteLink());
  $("[data-toot-record]").addEventListener("click", () => void recordToot());
  $("[data-mastodon-add]").addEventListener("click", () => void addMastodon());
  $("[data-mastodon-host]").addEventListener("keydown", (event) => {
    if (event.key === "Enter") void addMastodon();
  });
  $("[data-mastodon-list]").addEventListener("click", async (event) => {
    const host = event.target.closest("[data-mastodon-remove]")?.dataset.mastodonRemove;
    if (!host) return;
    await send({ type: "fm-mastodon-remove", host });
    await renderMastodon();
  });
  $("[data-x-section]").addEventListener("toggle", (event) => {
    if (event.target.open) void renderX();
  });
  $("[data-x-sync]").addEventListener("click", () => void syncX());
  $("[data-x-handle-save]").addEventListener("click", () => void trackX());
  $("[data-x-handle]").addEventListener("keydown", (event) => {
    if (event.key === "Enter") void trackX();
  });
  $("[data-x-open]").addEventListener("click", () => void send({ type: "fm-open-x" }));
  $("[data-x-clear]").addEventListener("click", async () => renderX(await send({ type: "fm-x-clear-errors" })));
  chrome.storage.onChanged.addListener((changes) => {
    if ("xStats" in changes || "xLive" in changes) liveRefresh();
  });
  $("[data-marketing-section]").addEventListener("toggle", (event) => {
    if (event.target.open) void renderMarketing();
  });
  $("[data-marketing-send]").addEventListener("click", () => void sendPageToMarketing());
  $("[data-marketing-allow]").addEventListener("click", allowChannelSites);
  $("[data-marketing-open]").addEventListener("click", () => void openMarketing());
  $("[data-reddit-section]").addEventListener("toggle", (event) => {
    if (!event.target.open) return;
    void renderReddit();
    void redditPrefillMonitor();
  });
  $("[data-reddit-monitor-add]").addEventListener("click", () => void redditMonitorAdd());
  $("[data-reddit-monitor-url]").addEventListener("keydown", (event) => {
    if (event.key === "Enter") void redditMonitorAdd();
  });
  $("[data-reddit-monitored]").addEventListener("click", async (event) => {
    const link = event.target.closest("[data-reddit-monitor-open]");
    if (link) {
      event.preventDefault();
      void chrome.tabs.create({ url: link.href });
      return;
    }
    const deskId = event.target.closest("[data-reddit-monitor-remove]")?.dataset.redditMonitorRemove;
    if (!deskId) return;
    const result = await send({ type: "fm-reddit-monitor-remove", deskId });
    if (!result.ok) note("[data-reddit-status]", humanError(result));
    else await renderReddit(result);
  });
  $("[data-reddit-auto-check]").addEventListener("change", async (event) => {
    const result = await send({ type: "fm-reddit-auto-check", on: event.target.checked });
    if (!result.ok) note("[data-reddit-status]", humanError(result));
    else await renderReddit(result);
  });
  $("[data-reddit-check]").addEventListener("click", () => {
    note("[data-reddit-status]", "Checking monitored threads: each opens in a background tab for a few seconds…");
    void redditAction("fm-reddit-check", "[data-reddit-check]");
  });
  $("[data-reddit-refresh]").addEventListener("click", () => void redditAction("fm-reddit-refresh", "[data-reddit-refresh]"));
  $("[data-reddit-sync]").addEventListener("click", () => void redditAction("fm-reddit-sync", "[data-reddit-sync]"));
  $("[data-reddit-walk]").addEventListener("click", () => void toggleRedditWalk());
  $("[data-reddit-open]").addEventListener("click", () => void send({ type: "fm-open-reddit-desk" }));
  $("[data-reddit-clear]").addEventListener("click", async () => renderReddit(await send({ type: "fm-reddit-clear-errors" })));
  $("[data-reddit-rules]").addEventListener("click", (event) => {
    const link = event.target.closest("[data-reddit-rules-open]");
    if (!link) return;
    event.preventDefault();
    void chrome.tabs.create({ url: link.href });
  });
  // Shown signed in or not: the popup's one path to a paid membership.
  $("[data-membership]").addEventListener("click", (event) => {
    event.preventDefault();
    void chrome.tabs.create({ url: event.currentTarget.href });
  });
  $("[data-open-crm]").addEventListener("click", () => void send({ type: "fm-open-crm" }));
  $("[data-sign-out]").addEventListener("click", async () => {
    await send({ type: "fm-sign-out" });
    void refreshMailTab();
    await renderStatus();
  });
  void renderStatus();
});
