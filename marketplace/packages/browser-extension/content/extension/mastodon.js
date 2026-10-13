// ForkMesh CRM inside Mastodon.
//
// Reads the conversation on screen the way content.js reads an email: who is
// in it, the status and thread ids, the permalink, and when it was posted.
// Above the open post it places panel.js's compact CRM header, whose "Look
// up" button puts the contact's pipeline, stage, owner and history over the
// conversation, and whose FM button opens the panel that links the thread to
// the record. Opening a conversation asks ForkMesh for nothing by itself.
//
// Mastodon knows people by handle (@name@instance) rather than by address, so
// a linked conversation carries the handle and the CRM matches the contact on
// it. Everything that talks to ForkMesh goes through background.js; this
// script never sees the session token, and the post text is only read here to
// fill the editable subject line.
(() => {
  if (window.__forkmeshCrmPanel) return;
  window.__forkmeshCrmPanel = true;

  const crm = globalThis.ForkMeshCrm;
  if (!crm) return;
  const { handleOf, realName, applyContact } = crm;

  const text = (node) => String(node?.textContent || "").replace(/\s+/g, " ").trim();
  const attr = (node, name) => String(node?.getAttribute?.(name) || "").trim();

  // "@grace" on its own instance, "@grace@mastodon.social" from elsewhere.
  function fullHandle(acct, home = location.hostname) {
    const clean = String(acct || "").trim().replace(/^@/, "");
    if (!clean) return "";
    return handleOf(clean.includes("@") ? clean : `${clean}@${home}`);
  }

  // A permalink names the status: /@grace/113489, /deck/@grace/113489, or
  // /users/grace/statuses/113489 on the server-rendered page.
  function statusId(href) {
    const match = String(href || "").match(/\/(\d{5,25})(?:[/?#]|$)/);
    return match ? match[1] : "";
  }

  // The signed-in account. Mastodon embeds the page's starting state as JSON;
  // the navigation bar names the same account when that is gone.
  function myHandle() {
    try {
      const state = JSON.parse(document.getElementById("initial-state")?.textContent || "{}");
      const mine = state?.accounts?.[state?.meta?.me];
      const handle = fullHandle(mine?.acct, state?.meta?.domain || location.hostname);
      if (handle) return handle;
    } catch { /* an older or rewritten page has no initial state */ }
    return fullHandle(text(document.querySelector(
      ".navigation-bar .display-name__account, .account__header__tabs__name .display-name__account")));
  }

  // Who posted a status: the display name beside its avatar and the handle
  // under it, with the profile link as the fallback for both.
  function accountOf(node) {
    const link = node?.querySelector(
      "a.detailed-status__display-name, a.status__display-name, .display-name, a[href^='/@']");
    const acct = text(link?.querySelector(".display-name__account"));
    const href = attr(link, "href") || attr(node?.querySelector("a[href^='/@']"), "href");
    const handle = fullHandle(acct)
      || handleOf(href.startsWith("http") ? href : `https://${location.host}${href}`);
    const name = text(link?.querySelector(".display-name__html, bdi"));
    return { handle, name: realName(name, handle) };
  }

  // What the post is about, in one line: its content warning when it has one,
  // otherwise the opening of the text. Shown in the panel before anything is
  // linked, and the only part of a post the CRM keeps.
  function summary(node) {
    const warning = text(node?.querySelector(
      ".status__content__spoiler-text, .p-summary, .spoiler-text"));
    if (warning) return `CW: ${warning}`.slice(0, 300);
    const body = String(node?.querySelector(".status__content__text, .status__content")?.innerText || "")
      .replace(/\s+/g, " ").trim();
    return body.length > 180 ? `${body.slice(0, 177)}…` : body;
  }

  // A mention reads "@grace" even when grace lives elsewhere; its link says
  // where. Mastodon routes remote accounts through itself with a relative
  // href (/@grace@aus.social), so read the resolved link, not the attribute,
  // and a link to another server (https://aus.social/users/grace) still names
  // grace's instance when the text alone would guess this one.
  function mentionHandle(mention) {
    const href = String(mention?.href || "");
    const handle = handleOf(href) || handleOf(attr(mention, "title"));
    if (handle) return handle;
    let home = location.hostname;
    try { home = new URL(href).hostname || home; } catch { /* no usable link */ }
    return fullHandle(text(mention), home);
  }

  // Everyone the thread mentions, in page order: the accounts of the posts
  // above the open one and the handles linked inside it.
  function threadPeople(detailed) {
    const people = [];
    const add = (person) => {
      if (person?.handle && !people.some((item) => item.handle === person.handle)) people.push(person);
    };
    for (const status of document.querySelectorAll(".status__wrapper, .status")) {
      if (!detailed.contains(status)) add(accountOf(status));
    }
    for (const mention of detailed.querySelectorAll("a.mention")) {
      add({ handle: mentionHandle(mention), name: "" });
    }
    return people;
  }

  // The open conversation, and the node the CRM header goes above: the text of
  // the post being read (its content warning first when it has one), so the
  // header sits under who wrote it the way an email's sits over its body.
  // Timelines and profiles have no open conversation.
  function readPage() {
    const detailed = document.querySelector(".detailed-status");
    if (!detailed) return { message: { provider: "mastodon", messageId: "" }, anchor: null };
    const wrapper = detailed.closest(".detailed-status__wrapper") || detailed;
    const stamp = detailed.querySelector("time[datetime]");
    const permalink = [...detailed.querySelectorAll("a[href]")]
      .map((link) => link.href)
      .find((href) => statusId(href) && /\/@|\/statuses\//.test(href));
    const messageId = statusId(location.pathname) || statusId(permalink)
      || attr(wrapper, "data-id").replace(/^.*?(\d+)$/, "$1");
    // The posts above this one are its ancestors; the first is the thread's
    // root, which is what ties every reply to one conversation.
    const ancestors = [...document.querySelectorAll(".status__wrapper a[href]")]
      .map((link) => link.href).filter((href) => statusId(href));
    const author = accountOf(detailed);
    const body = detailed.querySelector(".content-warning, .status__content");
    const mine = myHandle();
    const outbound = Boolean(mine) && author.handle === mine;
    const participants = [];
    for (const person of outbound ? threadPeople(detailed) : [author, ...threadPeople(detailed)]) {
      if (person.handle && person.handle !== mine
          && !participants.some((item) => item.handle === person.handle)) {
        participants.push({ email: "", handle: person.handle, name: person.name });
      }
    }
    const message = {
      provider: "mastodon", accountIndex: 0, messageId,
      threadId: statusId(ancestors[0]) || messageId,
      subject: summary(detailed),
      from: outbound ? { handle: mine, name: "" } : author,
      to: outbound ? participants.slice(0, 1) : [],
      mailbox: "", account: mine, direction: outbound ? "outbound" : "inbound",
      sentAt: Date.parse(attr(stamp, "datetime")) || NaN,
      providerUrl: permalink && permalink.startsWith("https://") ? permalink
        : `https://${location.host}${location.pathname}`,
      greeting: "", signature: {}, participants,
    };
    return {
      message: applyContact(message, participants[0] || { handle: "", name: "" }),
      anchor: body?.parentNode ? body : detailed,
    };
  }

  // --- CRM header above the open conversation ------------------------------

  const { host, panel, place } = crm.header({ read: async () => readPage().message });

  // Mastodon redraws its columns as the timeline moves: put the header back
  // above the open post whenever it is redrawn, and show the new conversation
  // in the header when it or its contact changes. The CRM record behind it is
  // only read when the header's "Look up" button is pressed, so moving
  // through a timeline sends nothing to ForkMesh. `asked` carries the one
  // exception: a link or a sign-in change made in the toolbar popup.
  let shown = "";
  let asked = false;
  function sync() {
    let page;
    try { page = readPage(); } catch { return; }
    const { message, anchor } = page;
    if (!message.messageId || !anchor?.parentNode) { host.remove(); return; }
    place(anchor);
    const key = `${message.messageId}|${message.handle}`;
    if (key === shown) return;
    shown = key;
    if (asked) { asked = false; void panel.load(message); }
    else void panel.present(message);
  }

  let timer = 0;
  function schedule(delay = 400) {
    if (timer) return;
    timer = setTimeout(() => { timer = 0; sync(); }, delay);
  }
  new MutationObserver(() => schedule()).observe(document.body || document.documentElement, { childList: true, subtree: true });
  // The single-page app swaps conversations without reloading.
  window.addEventListener("popstate", () => schedule(150));
  schedule(0);

  chrome.runtime.onMessage.addListener((request, _sender, sendResponse) => {
    if (request?.type === "fm-refresh-page") { shown = ""; asked = true; schedule(0); return false; }
    if (request?.type !== "fm-read-message") return false;
    try { sendResponse({ ok: true, message: readPage().message }); }
    catch (error) { sendResponse({ ok: false, error: "read_failed", message: String(error?.message || error) }); }
    return false;
  });
})();
