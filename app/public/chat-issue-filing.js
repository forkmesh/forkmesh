// Turning a chat message into a repository issue.
//
// A bug report typed into chat should not have to be retyped on the issues
// page, so every message in every web chat surface carries a control that files
// it as an issue: the full chat page (chat.js), the dashboard chat and the
// World's in-world chat (both dashboard-chat.js). The desktop client does the
// same from its message "⋯" menu (MessageRow -> MainWindow::promptIssueFromChatMessage).
//
// The issue is filed as the same signed "open" event the dashboard's new-issue
// form posts (submitWebIssue in dashboard/js/03-issues-profile-io.js): it lands
// in the maintainer's inbox and appears once their node drains it. Three things
// have to stay in lockstep with that form and with verify_issue_event in the
// worker, or the relay rejects the event:
//
//   * the localStorage key below, so a visitor keeps one author identity no
//     matter which surface they file from;
//   * the "open" content preimage - title NUL body NUL attachments;
//   * the "forkmesh-issue-event-v1" canonical string, signed with number 0
//     (the maintainer assigns the durable number on drain).

export const WEB_ISSUE_KEY_STORAGE = "forkmesh.webIssueKey";

const encoder = new TextEncoder();

function bytesToB64url(bytes) {
  const array = new Uint8Array(bytes);
  let binary = "";
  for (let index = 0; index < array.length; index += 1) {
    binary += String.fromCharCode(array[index]);
  }
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

async function sha256HexLower(text) {
  const digest = await crypto.subtle.digest("SHA-256", encoder.encode(text));
  return [...new Uint8Array(digest)]
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

// The author key is minted on first use and kept in localStorage, so every
// issue and comment a visitor files from this browser carries one identity.
async function webIssueKey() {
  let stored = null;
  try {
    stored = JSON.parse(localStorage.getItem(WEB_ISSUE_KEY_STORAGE) || "null");
  } catch (_) {}
  if (stored && stored.jwk && stored.pub) {
    try {
      return {
        privateKey: await crypto.subtle.importKey(
          "jwk", stored.jwk, { name: "Ed25519" }, false, ["sign"]),
        pub: stored.pub,
      };
    } catch (_) { /* fall through and mint a fresh key */ }
  }
  const pair = await crypto.subtle.generateKey(
    { name: "Ed25519" }, true, ["sign", "verify"]);
  const jwk = await crypto.subtle.exportKey("jwk", pair.privateKey);
  const pub = bytesToB64url(await crypto.subtle.exportKey("raw", pair.publicKey));
  try {
    localStorage.setItem(WEB_ISSUE_KEY_STORAGE, JSON.stringify({ jwk, pub }));
  } catch (_) {}
  return {
    privateKey: await crypto.subtle.importKey(
      "jwk", jwk, { name: "Ed25519" }, false, ["sign"]),
    pub,
  };
}

// The first line of the message seeds the title (the author still edits it
// before filing); the whole message becomes the body.
export function issueTitleFromMessage(text) {
  const first = String(text || "").split(/\r?\n/)[0].trim().replace(/\s+/g, " ");
  return first.length > 200 ? `${first.slice(0, 199)}…` : first;
}

// Attribution matters: the issue is signed by whoever files it, so the body
// records who actually said it, where, and when.
export function issueBodyFromMessage({ text, who, tsMs, channelLabel } = {}) {
  const when = new Date(Number(tsMs) || Date.now()).toISOString();
  const channel = String(channelLabel || "").trim();
  return `${String(text || "").trim()}\n\n---\nFiled from a ${
    channel ? `${channel} ` : ""
  }chat message by ${String(who || "someone")} at ${when}.`;
}

let repositoriesPromise = null;

// Public catalog read (no session needed) shared by every filing surface that
// does not already have a repository picker of its own.
export function loadIssueRepositories() {
  if (repositoriesPromise) return repositoriesPromise;
  repositoriesPromise = (async () => {
    const response = await fetch("/api/repositories", {
      cache: "no-store",
      credentials: "same-origin",
      headers: { accept: "application/json" },
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error("repository_catalog_unavailable");
    const seen = new Set();
    const entries = [];
    for (const repository of Array.isArray(payload?.repositories)
      ? payload.repositories
      : []) {
      const routeOwner = String(repository?.owner || "");
      const name = String(repository?.name || repository?.repo || "");
      // A repository is listed once per logical owner it answers to (its node
      // name and any organization alias); the alias is what people recognize,
      // but the API route always wants the hosting owner.
      const logicalOwners = Array.isArray(repository?.logicalOwners)
        ? repository.logicalOwners
        : ["user", "organization"].includes(String(repository?.ownerKind || "")) &&
            repository?.logicalOwner
          ? [{ kind: repository.ownerKind, owner: repository.logicalOwner }]
          : [];
      for (const identity of logicalOwners) {
        const logicalOwner = String(identity?.owner || "").toLowerCase();
        const value = `${logicalOwner}/${name}`;
        if (!logicalOwner || !name || !routeOwner || seen.has(value)) continue;
        seen.add(value);
        entries.push({
          value,
          label: `${value}${
            String(identity?.kind || "") === "organization" ? " · Organization" : ""
          }`,
          owner: routeOwner,
          name,
          repo: name,
        });
      }
    }
    entries.sort((a, b) => a.value.localeCompare(b.value));
    return entries;
  })().catch((error) => {
    repositoriesPromise = null;
    throw error;
  });
  return repositoriesPromise;
}

export async function fileWebIssue(repository, title, body, authorName = "") {
  const { privateKey, pub } = await webIssueKey();
  // Match IssueStore's epoch-millisecond timestamps so the issue renders
  // correctly in both the browser and desktop timelines.
  const ts = Date.now();
  const cleanBody = String(body || "").replace(/[\r\n]+$/, "");
  // open event content = title NUL body NUL attachments (none from chat)
  const NUL = String.fromCharCode(0);
  const contentHash = await sha256HexLower(`${title}${NUL}${cleanBody}${NUL}`);
  const repoKey = `${String(repository.owner || "").trim().toLowerCase()}/${
    String(repository.name || "").trim().toLowerCase()}`;
  const canonical =
    `forkmesh-issue-event-v2\n${repoKey}\nopen\n0\n${pub}\n${ts}\n${contentHash}`;
  const sig = bytesToB64url(await crypto.subtle.sign(
    { name: "Ed25519" }, privateKey, encoder.encode(canonical)));
  const payload = {
    owner: repository.owner,
    repo: repository.name,
    number: 0,
    titleIfNew: title,
    event: {
      type: "open",
      id: "open-web-" + ts,
      title,
      body: cleanBody,
      attachments: [],
      author: pub,
      authorName: String(authorName || ""),
      ts,
      sig,
    },
    meta: {
      labels: [],
      milestone: "",
      project: "",
      priority: 0,
      assignees: [],
      wantsAgent: false,
      model: "",
      provider: "",
    },
  };
  const response = await fetch(
    `/api/repo/${encodeURIComponent(repository.owner)}/${
      encodeURIComponent(repository.name)}/issues`,
    {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify(payload),
    },
  );
  const data = await response.json().catch(() => ({}));
  if (!response.ok || data.ok === false) {
    throw new Error(data.error || `HTTP ${response.status}`);
  }
  return data;
}

// What a surface says once the relay accepts the submission. The response
// carries the issue's durable number, reserved before any mirror has committed
// the git record, so the confirmation names the issue that now exists instead
// of describing it as queued work waiting on the maintainer (issue #609).
// Shared so the dashboard, the World and the full chat page cannot drift.
export function issueFiledSummary(repositoryLabel, title, result) {
  const number = Number(result?.number || 0);
  return Number.isInteger(number) && number > 0
    ? `Issue #${number} “${title}” was created in ${repositoryLabel}.`
    : `Issue “${title}” was signed and sent to ${repositoryLabel}.`;
}
