(() => {
  if (window.__forkmeshXCrm) return;
  window.__forkmeshXCrm = true;

  const crm = globalThis.ForkMeshCrm;
  if (!crm) return;
  const { xHandleOf, applyContact } = crm;
  const realName = (name, handle) => crm.realName(name, /\s/.test(String(name || "").trim()) ? "" : handle);

  const text = (node) => String(node?.textContent || "").replace(/\s+/g, " ").trim();
  const attr = (node, name) => String(node?.getAttribute?.(name) || "").trim();
  const clip = (value) => {
    const body = String(value || "").replace(/\s+/g, " ").trim();
    return body.length > 180 ? `${body.slice(0, 177)}…` : body;
  };

  const POST_ID = /^\d{5,25}$/;
  const PERMALINK = /^\/([A-Za-z0-9_]{1,15})\/status\/(\d{5,25})(?:\/|$)/;
  const MENTION = /^@([A-Za-z0-9_]{1,15})$/;
  const SNOWFLAKE_EPOCH = 1288834974657;
  const OWN_SETTLE = 1500;

  function snowflakeTime(id) {
    if (!POST_ID.test(String(id || ""))) return NaN;
    const at = Number(BigInt(id) >> 22n) + SNOWFLAKE_EPOCH;
    return at > SNOWFLAKE_EPOCH && at < Date.now() + 86400000 ? at : NaN;
  }

  const quoted = (article, node) => {
    const box = node.closest(`div[role="link"]`);
    return node.closest("article") !== article || Boolean(box && article.contains(box));
  };
  const own = (article, selector) => [...article.querySelectorAll(selector)].filter((node) => !quoted(article, node));
  const articles = () => [...document.querySelectorAll(`article[data-testid="tweet"]`)]
    .filter((article) => !article.parentElement?.closest("article"));
  const before = (node, other) => Boolean(node.compareDocumentPosition(other) & Node.DOCUMENT_POSITION_FOLLOWING);

  function statusOf(article) {
    const links = own(article, `a[href*="/status/"]`);
    const link = links.find((item) => item.querySelector("time")) || links[0];
    const match = attr(link, "href").match(PERMALINK);
    return match ? { author: match[1].toLowerCase(), id: match[2] } : null;
  }

  function myHandle() {
    const profile = document.querySelector(`a[data-testid="AppTabBar_Profile_Link"]`);
    const handle = xHandleOf(attr(profile, "href").replace(/^\//, ""));
    if (handle) return handle;
    const switcher = text(document.querySelector(`[data-testid="SideNav_AccountSwitcher_Button"]`));
    return xHandleOf((switcher.match(/@[A-Za-z0-9_]{1,15}/) || [""])[0]);
  }

  function accountOf(article) {
    const byline = own(article, `[data-testid="User-Name"]`)[0];
    const links = byline ? [...byline.querySelectorAll(`a[href^="/"]`)] : [];
    const handle = xHandleOf(attr(links[0], "href").replace(/^\//, "")) || statusOf(article)?.author || "";
    return { handle, name: realName(text(links[0]), handle) };
  }

  function mentionsOf(article) {
    const people = [];
    for (const link of own(article, `a[href^="/"]`)) {
      if (link.closest(`[data-testid="User-Name"]`)) continue;
      const shown = text(link).match(MENTION);
      const handle = shown ? xHandleOf(attr(link, "href").replace(/^\//, "")) : "";
      if (handle && handle === shown[1].toLowerCase() && !people.some((item) => item.handle === handle)) {
        people.push({ handle, name: "" });
      }
    }
    return people;
  }

  function bodyOf(article) {
    return own(article, `[data-testid="tweetText"]`)[0] || null;
  }

  function mainPost() {
    const path = location.pathname.match(PERMALINK);
    if (!path) return null;
    return articles().find((article) => statusOf(article)?.id === path[2])
      || document.querySelector(`article[data-testid="tweet"][tabindex="-1"]`);
  }

  function readPage() {
    const path = location.pathname.match(PERMALINK);
    const focal = mainPost();
    if (!path || !focal) return { message: { provider: "x", messageId: "" }, anchor: null };
    const messageId = path[2];
    const above = articles().filter((article) => article !== focal && before(article, focal));
    const rootId = above.map((article) => statusOf(article)?.id).find(Boolean) || "";
    const author = accountOf(focal);
    const mine = myHandle();
    const outbound = Boolean(mine) && author.handle === mine;
    const people = [];
    const add = (person) => {
      if (person?.handle && person.handle !== mine && !people.some((item) => item.handle === person.handle)) {
        people.push({ email: "", handle: person.handle, name: person.name || "" });
      }
    };
    if (!outbound) add(author);
    for (const article of above) add(accountOf(article));
    const names = new Map(people.map((person) => [person.handle, person.name]));
    for (const person of mentionsOf(focal)) add({ ...person, name: names.get(person.handle) || "" });
    const stamp = Date.parse(attr(own(focal, "time[datetime]")[0], "datetime"));
    const body = bodyOf(focal);
    const handle = author.handle || path[1].toLowerCase();
    const message = {
      provider: "x", accountIndex: 0, messageId,
      threadId: rootId || messageId,
      subject: clip(body?.innerText || body?.textContent),
      from: outbound ? { handle: mine, name: "" } : author,
      to: outbound ? people.slice(0, 1) : [],
      mailbox: "", account: mine, direction: outbound ? "outbound" : "inbound",
      sentAt: Number.isFinite(stamp) ? stamp : snowflakeTime(messageId),
      providerUrl: `https://x.com/${handle}/status/${messageId}`,
      greeting: "", signature: {}, participants: people,
    };
    return {
      message: applyContact(message, people[0] || { handle: "", name: "" }),
      anchor: body?.parentNode ? body : focal,
      focal,
    };
  }

  function ownPost() {
    const { message, focal } = readPage();
    if (!focal || message.direction !== "outbound") return null;
    const names = new Map(message.participants.map((person) => [person.handle, person.name]));
    const people = mentionsOf(focal).filter((person) => person.handle !== message.account)
      .map((person) => ({ handle: person.handle, name: names.get(person.handle) || "" }));
    return {
      statusId: message.messageId, threadId: message.threadId, kind: "post",
      author: { handle: message.account, name: "" }, mine: message.account, people,
      subject: message.subject, sentAt: message.sentAt, providerUrl: message.providerUrl,
    };
  }

  const { host, panel, place } = crm.header({ read: async () => readPage().message });
  for (const type of ["keydown", "keypress", "keyup"]) host.addEventListener(type, (event) => event.stopPropagation());

  let shown = "";
  let asked = false;
  function sync() {
    let page;
    try { page = readPage(); } catch { return; }
    const { message, anchor } = page;
    if (!message.messageId || !anchor?.parentNode) { host.remove(); return; }
    place(anchor);
    scheduleOwn();
    const key = `${message.messageId}|${message.handle}`;
    if (key === shown) return;
    shown = key;
    if (asked) { asked = false; void panel.load(message); }
    else void panel.present(message);
  }

  const tried = new Set();
  let ownTimer = 0;
  function scheduleOwn() {
    if (ownTimer) return;
    ownTimer = setTimeout(() => { ownTimer = 0; logOwnPost(); }, OWN_SETTLE);
  }

  function logOwnPost() {
    let post;
    try { post = ownPost(); } catch { return; }
    track(post);
  }

  function track(post) {
    if (!post?.people.length || tried.has(post.statusId)) return;
    tried.add(post.statusId);
    try {
      chrome.runtime.sendMessage({ type: "fm-track-tweet", post }, (response) => {
        if (chrome.runtime.lastError || !response?.tracked) return;
        shown = "";
        asked = true;
        schedule(0);
      });
    } catch { }
  }

  function sentPost(sent) {
    const id = String(sent?.id || "");
    const author = xHandleOf(sent?.author?.handle);
    if (!POST_ID.test(id) || !author) return null;
    const people = [];
    for (const person of Array.isArray(sent.people) ? sent.people : []) {
      const handle = xHandleOf(person?.handle);
      if (handle && handle !== author && !people.some((item) => item.handle === handle)) {
        people.push({ handle, name: realName(person?.name, handle) });
      }
    }
    const stamp = String(sent.createdAt || "");
    const sentAt = (/^\d{12,14}$/.test(stamp) ? Number(stamp) : Date.parse(stamp)) || snowflakeTime(id);
    const mine = myHandle();
    if (sent.kind === "dm") {
      const conversation = String(sent.conversationId || "");
      if (!/^\d{1,25}(?:-\d{1,25})?$/.test(conversation)) return null;
      return {
        statusId: id, threadId: conversation, kind: "dm", author: { handle: author, name: "" }, mine, people,
        subject: clip(sent.text), sentAt, providerUrl: `https://x.com/messages/${conversation}`,
      };
    }
    const parent = String(sent.inReplyTo || "");
    let threadId = POST_ID.test(String(sent.conversationId || "")) ? String(sent.conversationId) : id;
    if (threadId === id && POST_ID.test(parent)) {
      threadId = parent;
      try {
        const open = readPage().message;
        if (open.messageId === parent && POST_ID.test(open.threadId)) threadId = open.threadId;
      } catch { }
    }
    return {
      statusId: id, threadId, kind: "post", author: { handle: author, name: "" }, mine, people,
      subject: clip(sent.text), sentAt, providerUrl: `https://x.com/${author}/status/${id}`,
    };
  }

  window.addEventListener("message", (event) => {
    if (event.source !== window || event.origin !== location.origin) return;
    if (event.data?.source !== "forkmesh-x-sent") return;
    let post;
    try { post = sentPost(event.data.sent); } catch { return; }
    track(post);
  });

  let timer = 0;
  function schedule(delay = 400) {
    if (timer) return;
    timer = setTimeout(() => { timer = 0; sync(); }, delay);
  }
  new MutationObserver(() => schedule()).observe(document.body || document.documentElement, { childList: true, subtree: true });
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
