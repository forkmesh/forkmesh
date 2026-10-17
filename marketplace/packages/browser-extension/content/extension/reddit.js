(() => {
  if (window.__forkmeshRedditReader) return;
  window.__forkmeshRedditReader = true;

  const TICK_MS = 3000;
  const WATCH_EVERY_MS = 30000;
  const MAX_POSTS = 100;
  const LISTINGS = new Set(["new", "hot", "top", "rising", "controversial", "best"]);
  const SUBREDDIT = /^[a-z0-9_]{2,21}$/i;
  const POST_ID = /^[a-z0-9]{1,12}$/;
  const AUTHOR = /^[A-Za-z0-9_-]{1,20}$/;
  const VIEW_ATTRIBUTES = ["view-count", "views", "viewcount", "view_count", "data-view-count", "data-views", "impression-count", "impressions"];
  const VIEW_ATTRIBUTE = /(?:^|[-_])(?:view|views|viewcount|impressions?)(?:[-_]?count)?$/i;
  const VIEW_TEXT = [
    /^([\d.,]+\s*[kmb]?)\s+(?:total\s+)?(?:views?|impressions?)(?:\s+(?:on|of)\s+.{1,30})?$/i,
    /^(?:total\s+)?(?:views?|impressions?)(?:\s+count)?\s*[:·]?\s*([\d.,]+\s*[kmb]?)$/i,
    /^viewed\s+([\d.,]+\s*[kmb]?)\s+times?$/i,
    /^([\d.,]+\s*[kmb]?)\s+(?:people|users|redditors)\s+(?:have\s+)?(?:viewed|seen)\b.{0,30}$/i,
  ];
  const VIEW_ICON = /(?:^|[-_\s])(?:views?|eye|insights?|impressions?|analytics)(?:[-_\s]|$)/i;
  const VIEW_LABEL_ATTRIBUTES = ["aria-label", "title", "alt", "data-tooltip", "tooltip", "label"];
  const VIEW_SCAN = 3000;
  let watch = { subreddits: new Set(), queries: new Set(), monitored: new Map(), accounts: new Set() };
  let watchAt = 0;
  let lastSent = "";
  let reportedHref = "";
  let lastError = "";
  let stopped = false;

  const text = (node) => String(node?.textContent || "").replace(/\s+/g, " ").trim();
  const query = (value) => String(value || "").replace(/\s+/g, " ").trim().toLowerCase();

  function send(message) {
    return new Promise((resolve) => {
      try {
        if (!chrome.runtime?.id) {
          stopped = true;
          resolve(null);
          return;
        }
        chrome.runtime.sendMessage(message, (response) => {
          resolve(chrome.runtime.lastError ? null : response || null);
        });
      } catch {
        stopped = true;
        resolve(null);
      }
    });
  }

  function report(error) {
    const message = String(error?.message || error || "unknown error").slice(0, 200);
    if (message === lastError) return;
    lastError = message;
    void send({ type: "fm-reddit-error", message, page: location.pathname });
  }

  async function loadWatch(force = false) {
    const now = Date.now();
    if (!force && watchAt && now - watchAt < WATCH_EVERY_MS) return;
    watchAt = now;
    const result = await send({ type: "fm-reddit-watch" });
    if (!result?.ok) return;
    watch = {
      subreddits: new Set((result.subreddits || []).map((name) => String(name).toLowerCase())),
      queries: new Set((result.queries || []).map(query)),
      monitored: new Map((result.monitored || []).map((item) => [String(item.postId), String(item.commentId || "")])),
      accounts: new Set((result.accounts || []).map((name) => String(name).toLowerCase())),
    };
  }

  function profileHere() {
    const parts = location.pathname.split("/").filter(Boolean).map((part) => decodeURIComponent(part));
    if (!["user", "u"].includes(parts[0]?.toLowerCase()) || !AUTHOR.test(parts[1] || "")) return null;
    return { kind: "profile", account: parts[1], tab: String(parts[2] || "").toLowerCase() };
  }

  function threadHere() {
    const parts = location.pathname.split("/").filter(Boolean);
    const postId = String(parts[3] || "").toLowerCase();
    return parts[0]?.toLowerCase() === "r" && parts[2]?.toLowerCase() === "comments" && POST_ID.test(postId) ? postId : "";
  }

  function page() {
    const parts = location.pathname.split("/").filter(Boolean).map((part) => decodeURIComponent(part));
    const searched = query(new URLSearchParams(location.search).get("q"));
    const profile = profileHere();
    if (profile) return watch.accounts.has(profile.account.toLowerCase()) ? profile : null;
    if (parts[0]?.toLowerCase() === "search") {
      return searched && watch.queries.has(searched) ? { kind: "search", query: searched } : null;
    }
    if (parts[0]?.toLowerCase() !== "r" || !SUBREDDIT.test(parts[1] || "")) return null;
    const subreddit = parts[1].toLowerCase();
    const section = String(parts[2] || "").toLowerCase();
    if (section === "search") {
      return searched && watch.queries.has(searched) ? { kind: "search", query: searched, subreddit } : null;
    }
    const postId = String(parts[3] || "").toLowerCase();
    if (section === "comments" && watch.monitored.has(postId)) return { kind: "thread", subreddit, postId, monitored: true };
    if (!watch.subreddits.has(subreddit)) return null;
    if (section === "comments" && POST_ID.test(String(parts[3] || "").toLowerCase())) {
      return { kind: "thread", subreddit, postId: parts[3].toLowerCase() };
    }
    if (section === "about" && String(parts[3] || "").toLowerCase() === "rules") {
      return { kind: "rules", subreddit, name: parts[1] };
    }
    if (!section || LISTINGS.has(section)) return { kind: "listing", subreddit };
    return null;
  }

  function permalinkParts(value) {
    try {
      const url = new URL(String(value || ""), location.origin);
      if (!/(^|\.)reddit\.com$/i.test(url.hostname)) return null;
      const parts = url.pathname.split("/").filter(Boolean);
      if (parts[0]?.toLowerCase() !== "r" || !SUBREDDIT.test(parts[1] || "")) return null;
      if (parts[2]?.toLowerCase() !== "comments") return null;
      const id = String(parts[3] || "").toLowerCase();
      if (!POST_ID.test(id)) return null;
      const slug = /^[A-Za-z0-9_-]{1,200}$/.test(parts[4] || "") ? `${parts[4]}/` : "";
      const commentId = parts.length > 5 && POST_ID.test(String(parts[5]).toLowerCase()) ? String(parts[5]).toLowerCase() : "";
      return {
        subreddit: parts[1], id, comment: parts.length > 5, commentId,
        url: `https://www.reddit.com/r/${parts[1]}/comments/${id}/${slug}`,
        commentUrl: commentId ? `https://www.reddit.com/r/${parts[1]}/comments/${id}/${slug || "comment/"}${commentId}/` : "",
      };
    } catch {
      return null;
    }
  }

  function integer(value) {
    const clean = String(value ?? "").replace(/[,\s]/g, "");
    if (!/^-?\d{1,12}$/.test(clean)) return null;
    return Number(clean);
  }

  function stamp(value) {
    const raw = String(value || "").trim();
    if (/^\d{10,14}$/.test(raw)) return Number(raw) < 1e11 ? Number(raw) * 1000 : Number(raw);
    const parsed = Date.parse(raw.replace(/([+-]\d{2})(\d{2})$/, "$1:$2"));
    return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
  }

  function author(value) {
    const name = String(value || "").trim().replace(/^\/?u\//i, "");
    return AUTHOR.test(name) ? name : "";
  }

  function post(link, fields) {
    if (!link) return null;
    return {
      id: link.id, subreddit: link.subreddit, url: link.url,
      title: String(fields.title || "").replace(/\s+/g, " ").trim().slice(0, 300),
      author: author(fields.author),
      comments: integer(fields.comments),
      score: integer(fields.score),
      postedAt: stamp(fields.postedAt),
    };
  }

  function posts() {
    const found = new Map();
    const keep = (item) => {
      if (item && !found.has(item.id) && found.size < MAX_POSTS) found.set(item.id, item);
    };
    for (const node of document.querySelectorAll("shreddit-post")) {
      const link = permalinkParts(node.getAttribute("permalink"));
      const id = String(node.getAttribute("id") || "").replace(/^t3_/, "").toLowerCase();
      if (link && id && id !== link.id) continue;
      keep(post(link, {
        title: node.getAttribute("post-title"), author: node.getAttribute("author"),
        comments: node.getAttribute("comment-count"), score: node.getAttribute("score"),
        postedAt: node.getAttribute("created-timestamp"),
      }));
    }
    for (const node of document.querySelectorAll(`div.thing[data-fullname^="t3_"]`)) {
      const link = permalinkParts(node.getAttribute("data-permalink"));
      keep(post(link, {
        title: text(node.querySelector("a.title")), author: node.getAttribute("data-author"),
        comments: node.getAttribute("data-comments-count"), score: node.getAttribute("data-score"),
        postedAt: node.getAttribute("data-timestamp"),
      }));
    }
    for (const anchor of document.querySelectorAll(`a[href*="/comments/"]`)) {
      const link = permalinkParts(anchor.getAttribute("href"));
      if (!link || link.comment || found.has(link.id)) continue;
      const title = anchor.getAttribute("aria-label") || text(anchor);
      if (title.length < 4 || /^\d+\s+comments?$/i.test(title)) continue;
      keep(post(link, { title }));
    }
    return [...found.values()];
  }

  function abbreviated(value) {
    const match = /^([\d.,]+)\s*([kmb]?)$/i.exec(String(value ?? "").trim());
    if (!match) return null;
    const scale = { "": 1, k: 1e3, m: 1e6, b: 1e9 }[match[2].toLowerCase()];
    const digits = scale === 1 ? match[1].replace(/[.,]/g, "") : match[1].replace(/,/g, ".");
    const number = Math.round(Number(digits) * scale);
    return Number.isFinite(number) && number >= 0 && number <= 1e9 && digits !== "" ? number : null;
  }

  function viewLabel(value) {
    const clean = String(value || "").replace(/\s+/g, " ").trim();
    if (!clean || clean.length > 40) return null;
    for (const pattern of VIEW_TEXT) {
      const match = pattern.exec(clean);
      if (match) return abbreviated(match[1]);
    }
    return null;
  }

  function viewAttribute(node) {
    for (const name of VIEW_ATTRIBUTES) {
      const value = abbreviated(node.getAttribute?.(name));
      if (value !== null) return value;
    }
    for (const attribute of node.attributes || []) {
      if (VIEW_ATTRIBUTE.test(attribute.name)) {
        const value = abbreviated(attribute.value);
        if (value !== null) return value;
      }
    }
    return null;
  }

  function viewIcon(node) {
    const names = [node.getAttribute?.("icon-name"), node.getAttribute?.("name"), node.getAttribute?.("data-icon"),
      node.getAttribute?.("aria-label"), typeof node.className === "string" ? node.className : node.className?.baseVal];
    return names.some((name) => name && VIEW_ICON.test(String(name)));
  }

  function viewBeside(node) {
    const children = [...(node.children || [])];
    if (!children.length || children.length > 4) return null;
    const own = text(node);
    const counted = children.map((child) => abbreviated(child.getAttribute?.("number"))).find((value) => value !== null) ?? null;
    const value = own ? abbreviated(own) : counted;
    if (value === null) return null;
    const icon = children.some((child) => viewIcon(child) || [...(child.querySelectorAll?.("[icon-name], svg") || [])].slice(0, 4).some(viewIcon));
    return icon ? value : null;
  }

  function viewsIn(root, nested = "") {
    if (!root) return null;
    const stack = [root];
    let seen = 0;
    while (stack.length && seen < VIEW_SCAN) {
      const node = stack.pop();
      seen += 1;
      const children = [...(node.children || [])];
      let found = node.getAttribute ? viewAttribute(node) : null;
      for (const name of VIEW_LABEL_ATTRIBUTES) found ??= viewLabel(node.getAttribute?.(name));
      found ??= viewLabel(node.textContent) ?? viewBeside(node);
      if (found !== null) return found;
      if (node.shadowRoot) stack.push(node.shadowRoot);
      for (const child of children.reverse()) if (!nested || !child.matches?.(nested)) stack.push(child);
    }
    return null;
  }

  function postViews(node) {
    const own = viewsIn(node);
    if (own !== null) return own;
    for (const panel of document.querySelectorAll("[id*=\"insights\" i], [class*=\"insights\" i], [data-testid*=\"insights\" i]")) {
      const found = viewsIn(panel);
      if (found !== null) return found;
    }
    return null;
  }

  function ownItem(link, fields) {
    if (!link || (fields.comment && !link.commentId)) return null;
    return {
      kind: fields.comment ? "comment" : "post", postId: link.id, commentId: fields.comment ? link.commentId : "",
      subreddit: link.subreddit, url: fields.comment ? link.commentUrl : link.url,
      title: String(fields.title || "").replace(/\s+/g, " ").trim().slice(0, 300),
      score: integer(fields.score), comments: fields.comment ? null : integer(fields.comments),
      views: fields.views ?? null, postedAt: stamp(fields.postedAt),
    };
  }

  function profileItems(account, tab) {
    const name = account.toLowerCase();
    const found = new Map();
    const keep = (item) => {
      const key = item && `${item.postId}_${item.commentId}`;
      if (item && !found.has(key) && found.size < MAX_POSTS) found.set(key, item);
    };
    const mine = (value) => author(value).toLowerCase() === name;
    for (const node of document.querySelectorAll("shreddit-post")) {
      if (!mine(node.getAttribute("author"))) continue;
      const link = permalinkParts(node.getAttribute("permalink"));
      if (!link || link.comment) continue;
      keep(ownItem(link, {
        title: node.getAttribute("post-title"), score: node.getAttribute("score"),
        comments: node.getAttribute("comment-count"), postedAt: node.getAttribute("created-timestamp"), views: viewsIn(node),
      }));
    }
    for (const node of document.querySelectorAll(`div.thing[data-fullname^="t3_"], div.thing[data-fullname^="t1_"]`)) {
      if (!mine(node.getAttribute("data-author"))) continue;
      const comment = String(node.getAttribute("data-fullname")).startsWith("t1_");
      const link = permalinkParts(node.getAttribute("data-permalink"));
      if (!link || link.comment !== comment) continue;
      keep(ownItem(link, {
        comment, title: text(node.querySelector("a.title")),
        score: comment ? node.querySelector(".tagline .score.unvoted")?.getAttribute("title") : node.getAttribute("data-score"),
        comments: node.getAttribute("data-comments-count"),
        postedAt: node.getAttribute("data-timestamp") || node.querySelector("time[datetime]")?.getAttribute("datetime"),
      }));
    }
    for (const node of document.querySelectorAll("shreddit-profile-comment, shreddit-comment")) {
      const who = node.getAttribute("author");
      if (who && !mine(who)) continue;
      const link = permalinkParts(node.getAttribute("permalink") || node.getAttribute("href"));
      if (!link?.commentId) continue;
      keep(ownItem(link, {
        comment: true, title: node.getAttribute("post-title"), score: node.getAttribute("score"),
        postedAt: node.getAttribute("created-timestamp"), views: viewsIn(node, "shreddit-comment"),
      }));
    }
    for (const anchor of document.querySelectorAll(`a[href*="/comments/"]`)) {
      const link = permalinkParts(anchor.getAttribute("href"));
      if (!link || (!link.commentId && (link.comment || tab !== "submitted"))) continue;
      const holder = anchor.closest?.("shreddit-profile-comment, article") || null;
      keep(ownItem(link, {
        comment: Boolean(link.commentId),
        title: holder?.getAttribute?.("post-title") || (link.commentId ? "" : anchor.getAttribute("aria-label") || text(anchor)),
        views: link.commentId ? viewsIn(holder) : null,
      }));
    }
    return [...found.values()];
  }

  function monitorReading(postId) {
    return watch.monitored.has(postId) ? threadReading(postId, watch.monitored.get(postId)) : null;
  }

  function signedIn() {
    const link = document.querySelector("#header-bottom-right span.user a[href*=\"/user/\"], #header-bottom-right .user a[href*=\"/user/\"]");
    return link ? author(String(link.getAttribute("href") || "").split("/user/")[1]?.split("/")[0]) : "";
  }

  function commentNode(postId, commentId) {
    const found = document.querySelector(`shreddit-comment[thingid="t1_${commentId}"]`)
      || document.querySelector(`[thingid="t1_${commentId}"]`)
      || [...document.querySelectorAll("shreddit-comment[permalink]")].find((node) =>
        permalinkParts(node.getAttribute("permalink"))?.commentId === commentId);
    if (found) return { node: found, old: false };
    const old = document.querySelector(`div.thing[data-fullname="t1_${commentId}"]`) || document.querySelector(`#thing_t1_${commentId}`);
    return old ? { node: old, old: true } : null;
  }

  function threadReading(postId, commentId) {
    const reading = {
      postId, title: "", views: null, score: null, comments: null, author: "", commentId, commentViews: null, commentScore: null,
      commentAuthor: "", signedIn: signedIn(), readAs: "",
    };
    const shreddit = document.querySelector(`shreddit-post[id="t3_${postId}"]`);
    const old = document.querySelector(`div.thing[data-fullname="t3_${postId}"]`);
    if (shreddit) {
      Object.assign(reading, {
        title: String(shreddit.getAttribute("post-title") || ""), score: integer(shreddit.getAttribute("score")),
        comments: integer(shreddit.getAttribute("comment-count")), views: postViews(shreddit), author: author(shreddit.getAttribute("author")),
      });
    } else if (old) {
      Object.assign(reading, {
        title: text(old.querySelector("a.title")), score: integer(old.getAttribute("data-score")),
        comments: integer(old.getAttribute("data-comments-count")), views: postViews(old), author: author(old.getAttribute("data-author")),
      });
    }
    const comment = commentId ? commentNode(postId, commentId) : null;
    if (comment?.old) {
      reading.commentScore = integer(comment.node.querySelector(".tagline .score.unvoted")?.getAttribute("title"));
      reading.commentViews = viewsIn(comment.node.querySelector(".entry"));
      reading.commentAuthor = author(comment.node.getAttribute("data-author"));
    } else if (comment) {
      reading.commentScore = integer(comment.node.getAttribute("score"));
      reading.commentViews = viewsIn(comment.node, "shreddit-comment");
      reading.commentAuthor = author(comment.node.getAttribute("author"));
    }
    reading.readAs = reading.signedIn
      || (reading.commentViews !== null && reading.commentAuthor) || (reading.views !== null && reading.author) || "";
    reading.title = reading.title.replace(/\s+/g, " ").trim().slice(0, 300);
    const found = [reading.views, reading.score, reading.comments, reading.commentViews, reading.commentScore]
      .some((value) => value !== null);
    return found ? reading : null;
  }

  function rulesText(node) {
    return String(node?.innerText || node?.textContent || "")
      .split(/\n/).map((line) => line.replace(/\s+/g, " ").trim()).join("\n")
      .replace(/\n{3,}/g, "\n\n").trim();
  }

  function rules() {
    const found = [];
    for (const node of document.querySelectorAll(".subreddit-rule-item")) {
      const title = text(node.querySelector(".rule-title"));
      const description = rulesText(node.querySelector(".rule-description"));
      if (title || description) found.push({ title: title.slice(0, 200), description: description.slice(0, 2000) });
    }
    if (!found.length) {
      for (const node of document.querySelectorAll("shreddit-subreddit-rules details, #subreddit-rules details, [data-testid=\"rule\"]")) {
        const summary = node.querySelector("summary") || node.firstElementChild;
        const title = text(summary).replace(/^\d+\s*[.)]?\s*/, "");
        const whole = rulesText(node);
        const description = whole.startsWith(text(summary)) ? whole.slice(text(summary).length).trim() : whole;
        if (title) found.push({ title: title.slice(0, 200), description: description.slice(0, 2000) });
      }
    }
    if (found.length) return found.slice(0, 30);
    const oldRules = location.hostname === "old.reddit.com" && document.readyState === "complete"
      && document.querySelector(".subreddit-rules-page");
    return oldRules ? [] : null;
  }

  function observe() {
    const where = page();
    if (!where) return null;
    if (where.kind === "rules") {
      const found = rules();
      return found ? { ...where, rules: found, posts: [] } : null;
    }
    if (where.kind === "profile") {
      const found = profileItems(where.account, where.tab);
      const readAs = signedIn() || (found.some((item) => item.views !== null) ? author(where.account) : "");
      return found.length ? { kind: "profile", account: where.account, posts: found, readAs } : null;
    }
    let found = posts();
    if (where.kind === "thread") found = found.filter((item) => item.id === where.postId);
    if (where.kind === "listing") found = found.filter((item) => item.subreddit.toLowerCase() === where.subreddit);
    const monitor = where.monitored ? monitorReading(where.postId) : null;
    if (monitor) return { ...where, posts: found, monitor };
    return found.length ? { ...where, posts: found } : null;
  }

  function observed(observation) {
    const fresh = location.href !== reportedHref;
    reportedHref = location.href;
    return send({ type: "fm-reddit-observe", observation, fresh });
  }

  async function scan() {
    await loadWatch(true);
    const observation = observe();
    const key = observation ? JSON.stringify(observation) : "";
    if (!key || key === lastSent) return { ok: Boolean(key), posts: (observation?.posts || []).length, page: observation?.kind || "" };
    lastSent = key;
    await observed(observation);
    return { ok: true, posts: (observation.posts || []).length, page: observation.kind || "" };
  }

  chrome.runtime.onMessage?.addListener((request, sender, sendResponse) => {
    if (request?.type === "fm-reddit-scan" && sender?.id === chrome.runtime.id && !sender.tab) {
      scan().then(sendResponse, (error) => {
        report(error);
        sendResponse({ ok: false, posts: 0, page: "" });
      });
      return true;
    }
    if (request?.type === "fm-reddit-profile-read" && sender?.id === chrome.runtime.id && !sender.tab) {
      const where = profileHere();
      sendResponse(where ? { ok: true, account: where.account, items: profileItems(where.account, where.tab) } : { ok: false });
      return false;
    }
    if (request?.type !== "fm-reddit-read" || sender?.id !== chrome.runtime.id || sender.tab) return false;
    loadWatch(true).then(() => {
      const postId = String(request.postId || "").toLowerCase();
      const commentId = String(request.commentId ?? watch.monitored.get(postId) ?? "").toLowerCase();
      const monitor = threadHere() === postId ? threadReading(postId, commentId) : null;
      sendResponse(monitor ? { ok: true, monitor } : { ok: false });
    }, () => sendResponse({ ok: false }));
    return true;
  });

  async function tick() {
    if (stopped) return;
    try {
      await loadWatch();
      if (!stopped && document.visibilityState === "visible"
        && (watch.subreddits.size || watch.queries.size || watch.monitored.size || watch.accounts.size)) {
        const observation = observe();
        const key = observation ? JSON.stringify(observation) : "";
        if (key && key !== lastSent) {
          lastSent = key;
          await observed(observation);
        }
      }
    } catch (error) {
      report(error);
    }
    if (!stopped) setTimeout(() => void tick(), TICK_MS);
  }

  void tick();
})();
