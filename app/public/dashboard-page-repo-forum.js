// 09-repo-forum.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["repo-forum"] = (core) => {
  const { $, ISSUE_TEXT_ENCODER, applyRepoFeatures, blobText, bytesToB64url, escapeHtml, fetchRepoBlobs, fetchRepoJson, formatDate, getWebIssueKey, navigateHistory, renderMarkdown, repoApiBase, repoDefaultBranch, repoKey, repoLinkUrl, repoRawUrl, repoRouteParts, safeDecodeURIComponent, sha256HexLower, state } = core;
  const FORUM_ROOT = ".forkmesh/forum";
  const FORUM_EVENT_SCHEMA = "forkmesh-forum-event-v1";
  const FORUM_STYLE_ID = "fm-forum-style";
  const FORUM_SEEN_LIMIT = 2000;
  const FORUM_SUGGESTED = 5;
  const FORUM_TOP_PERIODS = [
    { id: "all", label: "All time" },
    { id: "yearly", label: "Year" },
    { id: "quarterly", label: "Quarter" },
    { id: "monthly", label: "Month" },
    { id: "weekly", label: "Week" },
    { id: "daily", label: "Today" },
  ];
  const FORUM_MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
  const FORUM_ALLOWED_TAGS = {
    a: ["href", "title"],
    img: ["src", "alt", "title", "width", "height"],
    ol: ["start"],
    th: ["colspan", "rowspan"],
    td: ["colspan", "rowspan"],
    abbr: ["title"],
    details: ["open"],
    p: [], br: [], hr: [], b: [], strong: [], i: [], em: [], u: [], s: [], strike: [], del: [], ins: [],
    code: [], pre: [], blockquote: [], ul: [], li: [], h1: [], h2: [], h3: [], h4: [], h5: [], h6: [],
    table: [], thead: [], tbody: [], tfoot: [], tr: [], sup: [], sub: [], summary: [], span: [], div: [],
    aside: [], kbd: [], mark: [], small: [], cite: [], dl: [], dt: [], dd: [],
  };
  const FORUM_DROPPED_TAGS = new Set([
    "script", "style", "iframe", "object", "embed", "svg", "math", "template", "noscript", "form", "textarea",
    "select", "button", "input", "video", "audio", "source", "track", "canvas", "frame", "frameset", "head",
    "title", "link", "meta", "base", "picture",
  ]);
  const FORUM_VOID_TAGS = new Set(["br", "hr", "img"]);
  const FORUM_CLASSES = new Set(["quote", "title", "onebox", "lightbox-wrapper", "spoiler", "spoiled"]);
  const FORUM_STYLE = `
.fm-forum{max-width:1110px;color:rgb(var(--dashboard-foreground-rgb))}
.fm-forum a{color:var(--dashboard-link)}
.fm-forum-bar{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin:4px 0 12px}
.fm-forum-pick{position:relative}
.fm-forum-pick>summary{display:inline-flex;align-items:center;gap:6px;height:32px;max-width:18rem;padding:0 10px;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;font-size:13px;cursor:pointer;list-style:none;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.fm-forum-pick>summary::-webkit-details-marker{display:none}
.fm-forum-pick[open]>summary,.fm-forum-pick>summary:hover{background:rgb(var(--dashboard-secondary-rgb))}
.fm-forum-menu{position:absolute;z-index:30;top:36px;left:0;min-width:16rem;max-height:22rem;overflow:auto;padding:4px;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;background:rgb(var(--dashboard-background-rgb));box-shadow:0 8px 24px rgba(0,0,0,.18)}
.fm-forum-menu a{display:flex;align-items:center;gap:8px;min-height:30px;padding:0 8px;border-radius:6px;font-size:13px;color:rgb(var(--dashboard-foreground-rgb));text-decoration:none}
.fm-forum-menu a:hover,.fm-forum-menu a[aria-current="page"]{background:rgb(var(--dashboard-secondary-rgb))}
.fm-forum-menu .fm-forum-count{margin-left:auto;color:rgb(var(--dashboard-muted-foreground-rgb));font-size:12px}
.fm-forum-nav{display:flex;gap:4px;margin-left:8px}
.fm-forum-nav a{display:inline-flex;align-items:center;height:32px;padding:0 10px;font-size:14px;color:rgb(var(--dashboard-foreground-rgb));text-decoration:none;border-bottom:2px solid transparent}
.fm-forum-nav a[aria-current="page"]{color:var(--dashboard-link);border-bottom-color:var(--dashboard-link)}
.fm-forum-nav a:hover{background:rgb(var(--dashboard-secondary-rgb));border-radius:6px 6px 0 0}
.fm-forum-table{width:100%;border-collapse:collapse}
.fm-forum-table th{padding:8px 10px;border-bottom:1px solid rgb(var(--dashboard-border-rgb));font-size:12px;font-weight:400;color:rgb(var(--dashboard-muted-foreground-rgb));text-align:left}
.fm-forum-table th.fm-forum-num,.fm-forum-table td.fm-forum-num{text-align:center;white-space:nowrap}
.fm-forum-table td{padding:10px;border-bottom:1px solid rgb(var(--dashboard-border-rgb));vertical-align:top}
.fm-forum-cat td:first-child{border-left:4px solid var(--fm-forum-color,rgb(var(--dashboard-border-rgb)))}
.fm-forum-cat-name{display:inline-flex;align-items:center;gap:6px;font-size:16px;font-weight:600;color:rgb(var(--dashboard-foreground-rgb))!important;text-decoration:none}
.fm-forum-dot{display:inline-block;width:10px;height:10px;flex:none;background:var(--fm-forum-color,rgb(var(--dashboard-muted-foreground-rgb)))}
.fm-forum-desc{margin-top:4px;font-size:14px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-subs{display:flex;flex-wrap:wrap;gap:4px 14px;margin-top:8px}
.fm-forum-chip{display:inline-flex;align-items:center;gap:5px;font-size:12.5px;color:rgb(var(--dashboard-foreground-rgb))!important;text-decoration:none}
.fm-forum-chip:hover{text-decoration:underline}
.fm-forum-topics{font-size:18px;font-weight:600;text-align:center}
.fm-forum-latest{display:grid;gap:3px;font-size:14px}
.fm-forum-latest div{display:flex;gap:8px;min-width:0}
.fm-forum-latest a{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-decoration:none}
.fm-forum-ago{flex:none;font-size:12px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-title{font-size:16px;color:rgb(var(--dashboard-foreground-rgb))!important;text-decoration:none;word-break:break-word}
.fm-forum-title:hover{color:var(--dashboard-link)!important}
.fm-forum-row-meta{display:flex;flex-wrap:wrap;align-items:center;gap:8px;margin-top:4px}
.fm-forum-tag{font-size:12px;color:rgb(var(--dashboard-muted-foreground-rgb))!important;text-decoration:none}
.fm-forum-excerpt{margin-top:4px;font-size:13px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-state{display:inline-block;width:13px;height:13px;margin-right:4px;vertical-align:-1px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-new{display:inline-block;width:8px;height:8px;margin-left:6px;border-radius:50%;background:var(--dashboard-link);vertical-align:middle}
.fm-forum-posters{display:flex;gap:2px}
.fm-forum-avatar{display:inline-flex;align-items:center;justify-content:center;flex:none;border-radius:50%;background:rgb(var(--dashboard-secondary-rgb));color:rgb(var(--dashboard-foreground-rgb));font-weight:600;text-transform:uppercase}
.fm-forum-header h1{margin:0;font-size:24px;font-weight:600;line-height:1.3;word-break:break-word}
.fm-forum-header .fm-forum-row-meta{margin-top:6px}
.fm-forum-post{display:grid;grid-template-columns:52px minmax(0,1fr);gap:12px;max-width:820px;padding:14px 0;border-top:1px solid rgb(var(--dashboard-border-rgb))}
.fm-forum-post-head{display:flex;align-items:baseline;justify-content:space-between;gap:12px;font-size:14px}
.fm-forum-post-head strong{font-weight:600}
.fm-forum-small{max-width:820px;padding:8px 0 8px 64px;border-top:1px solid rgb(var(--dashboard-border-rgb));font-size:13px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-cooked{margin-top:8px;font-size:15px;line-height:1.55;word-break:break-word}
.fm-forum-cooked p{margin:0 0 12px}
.fm-forum-cooked ul,.fm-forum-cooked ol{margin:0 0 12px;padding-left:28px;list-style:revert}
.fm-forum-cooked img{max-width:100%;height:auto}
.fm-forum-cooked pre{margin:0 0 12px;padding:10px;overflow:auto;border-radius:6px;background:rgb(var(--dashboard-secondary-rgb));font-size:13px}
.fm-forum-cooked code{font-size:13px}
.fm-forum-cooked blockquote,.fm-forum-cooked aside{margin:0 0 12px;padding:8px 12px;border-left:4px solid rgb(var(--dashboard-border-rgb));background:rgb(var(--dashboard-subtle-rgb))}
.fm-forum-cooked aside .title{font-size:13px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-cooked table{display:block;max-width:100%;overflow-x:auto;border-collapse:collapse;margin:0 0 12px}
.fm-forum-cooked td,.fm-forum-cooked th{padding:4px 8px;border-bottom:1px solid rgb(var(--dashboard-border-rgb));word-break:normal;overflow-wrap:normal;white-space:nowrap}
.fm-forum-cooked h1,.fm-forum-cooked h2,.fm-forum-cooked h3{margin:16px 0 8px;font-weight:600}
.fm-forum-cooked .spoiler{filter:blur(4px)}
.fm-forum-cooked .spoiler:hover,.fm-forum-cooked .spoiler:focus{filter:none}
.fm-forum-mention{font-weight:600}
.fm-forum-actions{display:flex;justify-content:flex-end;gap:4px;margin-top:6px}
.fm-forum-icon-btn{display:inline-flex;align-items:center;gap:4px;height:28px;padding:0 8px;border-radius:6px;font-size:13px;color:rgb(var(--dashboard-muted-foreground-rgb))!important;text-decoration:none;background:transparent}
.fm-forum-icon-btn:hover{background:rgb(var(--dashboard-secondary-rgb))}
.fm-forum-icon-btn svg,.fm-forum-icon-btn i{width:15px;height:15px}
.fm-forum-stats{display:flex;gap:24px;max-width:820px;padding:12px 0 12px 64px;border-top:1px solid rgb(var(--dashboard-border-rgb));border-bottom:1px solid rgb(var(--dashboard-border-rgb))}
.fm-forum-stats div{display:grid;justify-items:center;font-size:12px;color:rgb(var(--dashboard-muted-foreground-rgb))}
.fm-forum-stats strong{font-size:16px;font-weight:500;color:var(--dashboard-link)}
.fm-forum-reply{display:flex;max-width:820px;justify-content:flex-end;padding:10px 0}
.fm-forum-more{display:flex;justify-content:center;padding:12px 0}
.fm-forum h2{margin:24px 0 8px;font-size:17px;font-weight:600}
.fm-forum-bar-end{display:flex;gap:8px;margin-left:auto}
.fm-forum-title-row{display:flex;align-items:flex-start;justify-content:space-between;gap:12px}
.fm-forum-composer{display:grid;gap:8px;max-width:820px;padding:12px;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px}
.fm-forum-composer h2{margin:0}
.fm-forum-input{width:100%;box-sizing:border-box;padding:6px 10px;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;background:rgb(var(--dashboard-background-rgb));color:rgb(var(--dashboard-foreground-rgb));font:inherit;font-size:14px}
.fm-forum-input:focus{outline:none;border-color:var(--dashboard-link);box-shadow:0 0 0 2px color-mix(in srgb,var(--dashboard-link) 30%,transparent)}
textarea.fm-forum-input{min-height:90px;resize:vertical}
.fm-forum-short{width:6rem}
.fm-forum-composer select{height:32px;padding:0 8px;border:1px solid rgb(var(--dashboard-border-rgb));border-radius:6px;background:rgb(var(--dashboard-background-rgb));color:rgb(var(--dashboard-foreground-rgb))}
.fm-forum-composer-bar{display:flex;align-items:center;justify-content:flex-end;gap:8px}
.fm-forum-composer-bar [data-forum-status]{margin-right:auto}
.fm-forum-reply-box{max-width:820px;margin-top:8px}
.fm-forum-move{display:flex;gap:6px;padding:6px 8px}
.fm-forum-icon-btn[aria-pressed="true"]{color:var(--dashboard-link)!important}
.fm-forum-icon-btn[aria-pressed="true"] svg{fill:currentColor}
@media (max-width:720px){.fm-forum-hide-sm{display:none}.fm-forum-post{grid-template-columns:36px minmax(0,1fr)}.fm-forum-small,.fm-forum-stats{padding-left:0}}
`;
  const forumView = {
    repoKey: "",
    cache: new Map(),
    seq: 0,
    bound: null,
    base: null,
    topic: null,
    features: null,
    viewer: null,
    ref: "",
  };

  function forumEnsureStyle() {
    if (document.getElementById(FORUM_STYLE_ID)) return;
    const style = document.createElement("style");
    style.id = FORUM_STYLE_ID;
    style.textContent = FORUM_STYLE;
    document.head.append(style);
  }

  function forumRoute(parts, search) {
    const rest = (parts || []).map((part) => safeDecodeURIComponent(String(part || "")));
    const params = new URLSearchParams(search || "");
    const id = (value) => (/^(\d{1,10}|n\d{1,9})$/.test(value || "") ? value : "");
    if (rest[0] === "latest") return { view: "latest" };
    if (rest[0] === "top") {
      const period = params.get("period") || "";
      return { view: "top", period: FORUM_TOP_PERIODS.some((item) => item.id === period) ? period : "all" };
    }
    if (rest[0] === "c") {
      const list = rest.indexOf("l");
      const head = list > 0 ? rest.slice(1, list) : rest.slice(1);
      const categoryId = /^\d{1,10}$/.test(head[head.length - 1] || "") ? Number(head[head.length - 1]) : 0;
      if (categoryId) return { view: "category", id: categoryId, list: list > 0 ? "latest" : "" };
    }
    if (rest[0] === "new") return { view: "new", category: /^\d{1,10}$/.test(params.get("category") || "") ? Number(params.get("category")) : 0 };
    if (rest[0] === "admin") return { view: "admin" };
    if (rest[0] === "tag" && rest[1] && /^[^/]{1,100}$/.test(rest[1])) return { view: "tag", key: rest[1] };
    if (rest[0] === "t") {
      const tail = rest.slice(1);
      const numberAt = tail.length >= 2 && id(tail[1]) ? 1 : 0;
      const topicId = id(tail[numberAt]);
      const post = /^\d{1,7}$/.test(tail[numberAt + 1] || "") ? Number(tail[numberAt + 1]) : 0;
      if (topicId) return { view: "topic", id: topicId, post };
    }
    return { view: "categories" };
  }

  function forumSlug(value) {
    const slug = String(value || "").toLowerCase().replace(/[^\p{L}\p{N}]+/gu, "-").replace(/^-+|-+$/g, "").slice(0, 80);
    return slug || "topic";
  }

  function forumHref(repo, route) {
    const base = `${repoLinkUrl(repo)}/forum`;
    if (!route || route.view === "categories") return base;
    if (route.view === "latest") return `${base}/latest`;
    if (route.view === "top") return `${base}/top${route.period && route.period !== "all" ? `?period=${encodeURIComponent(route.period)}` : ""}`;
    if (route.view === "category") return `${base}/c/${encodeURIComponent(forumSlug(route.slug))}/${route.id}${route.list ? "/l/latest" : ""}`;
    if (route.view === "tag") return `${base}/tag/${encodeURIComponent(route.key)}`;
    if (route.view === "new") return `${base}/new${route.category ? `?category=${route.category}` : ""}`;
    if (route.view === "admin") return `${base}/admin`;
    if (route.view === "topic") return `${base}/t/${encodeURIComponent(forumSlug(route.slug))}/${route.id}${route.post > 1 ? `/${route.post}` : ""}`;
    return base;
  }

  function forumColor(value) {
    const text = String(value || "").trim().replace(/^#/, "").toLowerCase();
    return /^([0-9a-f]{3}|[0-9a-f]{6})$/.test(text) ? `#${text}` : "";
  }

  function forumColorStyle(category) {
    const color = forumColor(category?.color);
    return color ? ` style="--fm-forum-color:${color}"` : "";
  }

  function forumTime(value) {
    const ms = typeof value === "number" ? value : Date.parse(String(value || ""));
    return Number.isFinite(ms) ? ms : 0;
  }

  function forumAgo(value, now = Date.now()) {
    const ms = forumTime(value);
    if (!ms) return "";
    const seconds = Math.max(0, (now - ms) / 1000);
    if (seconds < 3600) return `${Math.max(1, Math.floor(seconds / 60))}m`;
    if (seconds < 86400) return `${Math.floor(seconds / 3600)}h`;
    if (seconds < 30 * 86400) return `${Math.floor(seconds / 86400)}d`;
    const date = new Date(ms);
    const month = FORUM_MONTHS[date.getUTCMonth()];
    return date.getUTCFullYear() === new Date(now).getUTCFullYear()
      ? `${month} ${date.getUTCDate()}`
      : `${month} ${date.getUTCFullYear()}`;
  }

  function forumCount(value) {
    const number = Math.max(0, Math.floor(Number(value) || 0));
    if (number < 1000) return String(number);
    if (number < 1000000) return `${(number / 1000).toFixed(1)}k`;
    return `${(number / 1000000).toFixed(1)}m`;
  }

  function forumWhenHtml(value) {
    const ms = forumTime(value);
    if (!ms) return "";
    return `<time class="fm-forum-ago" datetime="${escapeHtml(new Date(ms).toISOString())}" title="${escapeHtml(formatDate(new Date(ms).toISOString()))}">${escapeHtml(forumAgo(ms))}</time>`;
  }

  function forumAvatar(username, size = 24, title = "") {
    const name = String(username || "").trim();
    const letter = (Array.from(name)[0] || "?").toUpperCase();
    const label = title || name || "unknown";
    return `<span class="fm-forum-avatar" role="img" aria-label="${escapeHtml(label)}" title="${escapeHtml(label)}" style="width:${size}px;height:${size}px;font-size:${Math.round(size * 0.45)}px">${escapeHtml(letter)}</span>`;
  }

  function forumOriginUrl(origin) {
    try {
      const url = new URL(String(origin || ""));
      return url.protocol === "https:" || url.protocol === "http:" ? url : null;
    } catch (_) {
      return null;
    }
  }

  function forumLink(value, ctx) {
    const text = String(value || "").trim();
    if (/^#[A-Za-z0-9_-]{1,64}$/.test(text)) return { href: text };
    if (/^mailto:[^\s"'<>]{3,200}$/i.test(text)) return { href: text, external: true };
    if (!text || /^(javascript|data|vbscript|file):/i.test(text.replace(/[\s\u0000-\u001f]+/g, ""))) return null;
    let url;
    try {
      url = new URL(text, ctx.origin ? `${ctx.origin.href.replace(/\/$/, "")}/` : undefined);
    } catch (_) {
      return null;
    }
    if (url.protocol !== "https:" && url.protocol !== "http:") return null;
    if (ctx.origin && url.host === ctx.origin.host) {
      const prefix = ctx.origin.pathname.replace(/\/$/, "");
      if (!prefix || url.pathname === prefix || url.pathname.startsWith(`${prefix}/`)) {
        const rest = url.pathname.slice(prefix.length);
        const topic = /^\/t\/(?:[^/]+\/)?(\d{1,10})(?:\/(\d{1,7}))?\/?$/.exec(rest);
        if (topic && ctx.topicHref) return { href: ctx.topicHref(topic[1], Number(topic[2] || 0)), internal: true };
        if (/^\/(u|users|c|tag|tags|badges|groups)\//.test(rest)) return { href: url.href, external: true, plain: true };
      }
    }
    return { href: url.href, external: true };
  }

  function forumImage(element, ctx) {
    const alt = element.getAttribute("alt") || "";
    if (element.classList.contains("emoji")) return escapeHtml(alt);
    if (element.classList.contains("avatar")) return "";
    let source = "";
    try {
      source = new URL(element.getAttribute("src") || "", ctx.origin ? `${ctx.origin.href.replace(/\/$/, "")}/` : undefined).href;
    } catch (_) {
      source = "";
    }
    const local = source && ctx.uploads ? ctx.uploads[source] : "";
    if (local && /^[0-9a-f]{8,64}\.(png|jpg|gif|webp|avif)$/.test(local) && ctx.rawUrl) {
      const size = ["width", "height"]
        .filter((name) => /^\d{1,5}$/.test(element.getAttribute(name) || ""))
        .map((name) => ` ${name}="${element.getAttribute(name)}"`)
        .join("");
      return `<img src="${escapeHtml(ctx.rawUrl(local))}" alt="${escapeHtml(alt)}" loading="lazy" decoding="async" referrerpolicy="no-referrer"${size}>`;
    }
    if (/^https?:/i.test(source)) {
      return `<a href="${escapeHtml(source)}" rel="nofollow noopener noreferrer" target="_blank">[image${alt ? `: ${escapeHtml(alt)}` : ""}]</a>`;
    }
    return alt ? escapeHtml(alt) : "";
  }

  function forumNodeHtml(node, ctx) {
    let html = "";
    node.childNodes.forEach((child) => {
      if (child.nodeType === 3) {
        html += escapeHtml(child.nodeValue || "");
        return;
      }
      if (child.nodeType !== 1) return;
      const tag = String(child.localName || "").toLowerCase();
      if (FORUM_DROPPED_TAGS.has(tag)) return;
      if (child.classList.contains("meta") && child.closest(".lightbox-wrapper")) return;
      const allowed = FORUM_ALLOWED_TAGS[tag];
      if (!allowed) {
        html += forumNodeHtml(child, ctx);
        return;
      }
      if (tag === "img") {
        html += forumImage(child, ctx);
        return;
      }
      const attrs = [];
      if (tag === "a") {
        const lightboxImage = child.classList.contains("lightbox") ? child.querySelector("img") : null;
        const link = lightboxImage ? null : forumLink(child.getAttribute("href"), ctx);
        if (child.classList.contains("mention") || child.classList.contains("hashtag") || link?.plain) {
          html += `<span class="fm-forum-mention">${forumNodeHtml(child, ctx)}</span>`;
          return;
        }
        if (lightboxImage || !link) {
          html += forumNodeHtml(child, ctx);
          return;
        }
        if (child.getAttribute("title")) attrs.push(`title="${escapeHtml(child.getAttribute("title"))}"`);
        attrs.push(`href="${escapeHtml(link.href)}"`);
        if (link.internal) attrs.push("data-forum-link");
        if (link.external) attrs.push('rel="nofollow noopener noreferrer" target="_blank"');
      } else {
        allowed.forEach((name) => {
          const value = child.getAttribute(name);
          if (value === null) return;
          if (["colspan", "rowspan", "start"].includes(name) && !/^\d{1,4}$/.test(value)) return;
          attrs.push(name === "open" ? "open" : `${name}="${escapeHtml(value)}"`);
        });
        const classes = Array.from(child.classList).filter((name) => FORUM_CLASSES.has(name));
        if (classes.length) attrs.push(`class="${escapeHtml(classes.join(" "))}"`);
        if (classes.includes("spoiler")) attrs.push('tabindex="0"');
      }
      html += `<${tag}${attrs.length ? ` ${attrs.join(" ")}` : ""}>`;
      if (!FORUM_VOID_TAGS.has(tag)) html += `${forumNodeHtml(child, ctx)}</${tag}>`;
    });
    return html;
  }

  function forumCookedHtml(cooked, ctx = {}) {
    const doc = new DOMParser().parseFromString(`<!doctype html><html><body>${String(cooked || "")}</body></html>`, "text/html");
    return forumNodeHtml(doc.body, ctx);
  }

  function forumSeenKey(repo) {
    return `forkmesh.forum.seen.${repoKey(repo)}`;
  }

  function forumSeen(repo) {
    try {
      const value = JSON.parse(localStorage.getItem(forumSeenKey(repo)) || "{}");
      return value && typeof value === "object" ? value : {};
    } catch (_) {
      return {};
    }
  }

  function forumMarkSeen(repo, topic) {
    const seen = forumSeen(repo);
    delete seen[topic.id];
    seen[topic.id] = Number(topic.highest_post_number || topic.posts_count) || 1;
    const keys = Object.keys(seen);
    keys.slice(0, Math.max(0, keys.length - FORUM_SEEN_LIMIT)).forEach((key) => delete seen[key]);
    try {
      localStorage.setItem(forumSeenKey(repo), JSON.stringify(seen));
    } catch (_) {
      return;
    }
  }

  function forumUnread(seen, row) {
    const read = Number(seen[row.id]) || 0;
    return read < (Number(row.posts_count) || 0);
  }

  async function forumRead(repo, paths) {
    const key = repoKey(repo);
    const wanted = paths.filter((path) => !forumView.cache.has(`${key}\n${path}`));
    if (wanted.length) {
      const fullPaths = wanted.map((path) => `${FORUM_ROOT}/${path}`);
      let blobs = await fetchRepoBlobs(repo, fullPaths, { ref: forumView.ref || repoDefaultBranch(repo) });
      if (forumView.ref && fullPaths.every((path) => !blobs[path])) {
        blobs = await fetchRepoBlobs(repo, fullPaths, { ref: repoDefaultBranch(repo) });
      }
      wanted.forEach((path) => {
        const blob = blobs[`${FORUM_ROOT}/${path}`];
        let value = null;
        if (blob) {
          try {
            value = JSON.parse(blobText(blob));
          } catch (_) {
            value = null;
          }
        }
        forumView.cache.set(`${key}\n${path}`, value);
      });
    }
    return Object.fromEntries(paths.map((path) => [path, forumView.cache.get(`${key}\n${path}`) ?? null]));
  }

  async function forumBase(repo) {
    if (forumView.base && forumView.base.key === repoKey(repo)) return forumView.base;
    const read = await forumRead(repo, ["forum.json", "categories.json", "tags.json"]);
    const forum = read["forum.json"];
    const categories = Array.isArray(read["categories.json"]) ? read["categories.json"].filter((item) => item && Number(item.id)) : [];
    const byId = new Map(categories.map((item) => [Number(item.id), item]));
    const origin = forumOriginUrl(forum?.source?.origin);
    forumView.base = {
      key: repoKey(repo),
      forum: forum && forum.kind === "forkmesh-forum" ? forum : null,
      categories,
      byId,
      tags: Array.isArray(read["tags.json"]) ? read["tags.json"].filter((item) => item && item.name) : [],
      origin,
    };
    return forumView.base;
  }

  function forumCategoryRoute(category, list = "") {
    return { view: "category", id: Number(category.id), slug: category.slug || category.name, list };
  }

  function forumTopicRoute(row, post = 0) {
    return { view: "topic", id: String(row.id), slug: row.slug || row.title, post };
  }

  function forumChip(repo, base, categoryId) {
    const category = base.byId.get(Number(categoryId));
    if (!category) return "";
    const parent = base.byId.get(Number(category.parent_category_id));
    const chip = (item) => `<a class="fm-forum-chip" href="${escapeHtml(forumHref(repo, forumCategoryRoute(item)))}" data-forum-link${forumColorStyle(item)}><span class="fm-forum-dot"></span>${escapeHtml(item.name)}</a>`;
    return parent ? `${chip(parent)}${chip(category)}` : chip(category);
  }

  function forumStateIcons(row) {
    return [
      row.closed || row.archived ? `<i data-lucide="lock" class="fm-forum-state" aria-label="${row.archived ? "Archived" : "Closed"}"></i>` : "",
      row.pinned || row.pinned_globally ? '<i data-lucide="pin" class="fm-forum-state" aria-label="Pinned"></i>' : "",
    ].join("");
  }

  function forumPickersHtml(repo, base, route) {
    const current = route.view === "category" ? base.byId.get(route.id) : null;
    const parent = current && current.parent_category_id ? base.byId.get(Number(current.parent_category_id)) : null;
    const top = parent || current;
    const categoryMenu = base.categories
      .filter((item) => !item.parent_category_id)
      .map((item) => `<a href="${escapeHtml(forumHref(repo, forumCategoryRoute(item)))}" data-forum-link${forumColorStyle(item)}${top && Number(top.id) === Number(item.id) ? ' aria-current="page"' : ""}><span class="fm-forum-dot"></span><span>${escapeHtml(item.name)}</span><span class="fm-forum-count">${escapeHtml(forumCount(item.total_topic_count ?? item.topic_count))}</span></a>`)
      .join("");
    const pickers = [
      `<details class="fm-forum-pick" data-forum-pick><summary${top ? forumColorStyle(top) : ""}>${top ? `<span class="fm-forum-dot"></span>${escapeHtml(top.name)}` : "categories"}<i data-lucide="chevron-right" class="h-3.5 w-3.5"></i></summary><div class="fm-forum-menu" role="menu"><a href="${escapeHtml(forumHref(repo, { view: "categories" }))}" data-forum-link>All categories</a>${categoryMenu}</div></details>`,
    ];
    if (top) {
      const subs = (top.subcategory_ids || []).map((id) => base.byId.get(Number(id))).filter(Boolean);
      if (subs.length) {
        const sub = parent ? current : null;
        pickers.push(`<details class="fm-forum-pick" data-forum-pick><summary${sub ? forumColorStyle(sub) : ""}>${sub ? `<span class="fm-forum-dot"></span>${escapeHtml(sub.name)}` : "subcategories"}<i data-lucide="chevron-right" class="h-3.5 w-3.5"></i></summary><div class="fm-forum-menu" role="menu"><a href="${escapeHtml(forumHref(repo, forumCategoryRoute(top, "latest")))}" data-forum-link>All of ${escapeHtml(top.name)}</a>${subs.map((item) => `<a href="${escapeHtml(forumHref(repo, forumCategoryRoute(item)))}" data-forum-link${forumColorStyle(item)}${sub && Number(sub.id) === Number(item.id) ? ' aria-current="page"' : ""}><span class="fm-forum-dot"></span><span>${escapeHtml(item.name)}</span><span class="fm-forum-count">${escapeHtml(forumCount(item.topic_count))}</span></a>`).join("")}</div></details>`);
      }
    }
    if (base.tags.length) {
      const currentTag = route.view === "tag" ? base.tags.find((tag) => tag.key === route.key) : null;
      pickers.push(`<details class="fm-forum-pick" data-forum-pick><summary>${currentTag ? escapeHtml(currentTag.name) : "tags"}<i data-lucide="chevron-right" class="h-3.5 w-3.5"></i></summary><div class="fm-forum-menu" role="menu">${base.tags.slice(0, 200).map((tag) => `<a href="${escapeHtml(forumHref(repo, { view: "tag", key: tag.key }))}" data-forum-link${currentTag && currentTag.key === tag.key ? ' aria-current="page"' : ""}><span>${escapeHtml(tag.name)}</span><span class="fm-forum-count">${escapeHtml(forumCount(tag.count))}</span></a>`).join("")}</div></details>`);
    }
    return pickers.join("");
  }

  function forumNavHtml(repo, base, route) {
    const category = route.view === "category" ? base.byId.get(route.id) : null;
    const hasSubs = Boolean(category && (category.subcategory_ids || []).length);
    const items = category
      ? [
        { label: "Categories", href: forumHref(repo, forumCategoryRoute(category)), shown: hasSubs, current: hasSubs && !route.list },
        { label: "Latest", href: forumHref(repo, forumCategoryRoute(category, "latest")), shown: true, current: route.list === "latest" || !hasSubs },
        { label: "Top", href: forumHref(repo, { view: "top" }), shown: true, current: false },
      ]
      : [
        { label: "Categories", href: forumHref(repo, { view: "categories" }), shown: true, current: route.view === "categories" },
        { label: "Latest", href: forumHref(repo, { view: "latest" }), shown: true, current: route.view === "latest" },
        { label: "Top", href: forumHref(repo, { view: "top" }), shown: true, current: route.view === "top" },
      ];
    return `<nav class="fm-forum-nav" aria-label="Forum lists">${items.filter((item) => item.shown).map((item) => `<a href="${escapeHtml(item.href)}" data-forum-link${item.current ? ' aria-current="page"' : ""}>${item.label}</a>`).join("")}</nav>`;
  }

  function forumBarHtml(repo, base, route, viewer = {}) {
    const category = route.view === "category" ? route.id : 0;
    const create = viewer.account ? `<a class="fm-btn fm-btn-cta fm-btn-sm" href="${escapeHtml(forumHref(repo, { view: "new", category }))}" data-forum-link data-forum-new><i data-lucide="plus" class="h-3.5 w-3.5"></i>New topic</a>` : "";
    const admin = viewer.moderator ? `<a class="fm-btn fm-btn-sm" href="${escapeHtml(forumHref(repo, { view: "admin" }))}" data-forum-link data-forum-admin-link><i data-lucide="settings" class="h-3.5 w-3.5"></i>Forum settings</a>` : "";
    return `<div class="fm-forum-bar">${forumPickersHtml(repo, base, route)}${forumNavHtml(repo, base, route)}<span class="fm-forum-bar-end">${admin}${create}</span></div>`;
  }

  function forumContentText(content) {
    const sorted = {};
    Object.keys(content || {}).sort().forEach((key) => { sorted[key] = content[key]; });
    return JSON.stringify(sorted);
  }

  function forumCanonical(repo, event, digest) {
    const key = `${String(repo?.owner || "").trim().toLowerCase()}/${String(repo?.name || "").trim().toLowerCase()}`;
    return [FORUM_EVENT_SCHEMA, key, event.type, event.topic, event.post, event.author, String(event.ts), digest].join("\n");
  }

  async function forumSignedEvent(repo, type, topic, post, content) {
    const { privateKey, pub } = await getWebIssueKey();
    const event = { type, topic: String(topic || ""), post: String(post || ""), content, author: pub, ts: Date.now() };
    const digest = await sha256HexLower(forumContentText(content));
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, ISSUE_TEXT_ENCODER.encode(forumCanonical(repo, event, digest))));
    const random = crypto.getRandomValues(new Uint32Array(2));
    return { ...event, sig, id: `${type}-${event.ts.toString(36)}-${random[0].toString(36)}${random[1].toString(36)}`, authorName: state.session?.nodeName || "" };
  }

  function forumAuthHeaders() {
    const token = state.session?.sessionToken || "";
    return token && token !== "cookie" ? { authorization: `Bearer ${token}` } : {};
  }

  async function forumViewer(repo) {
    if (forumView.viewer && forumView.viewer.key === repoKey(repo)) return forumView.viewer;
    let value = { account: "", moderator: false };
    if (state.session?.sessionToken || state.session?.nodeName) {
      try {
        const response = await fetch(`${repoApiBase(repo)}/forum`, { headers: { accept: "application/json", ...forumAuthHeaders() } });
        const body = await response.json().catch(() => ({}));
        if (body.ok) value = { account: String(body.account || ""), moderator: Boolean(body.moderator) };
      } catch (_) {
        value = { account: "", moderator: false };
      }
    }
    forumView.viewer = { key: repoKey(repo), ...value };
    return forumView.viewer;
  }

  async function forumSubmit(repo, type, topic, post, content) {
    const event = await forumSignedEvent(repo, type, topic, post, content);
    const response = await fetch(`${repoApiBase(repo)}/forum`, {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json", ...forumAuthHeaders() },
      body: JSON.stringify({ branch: repoDefaultBranch(repo), event, sessionToken: state.session?.sessionToken || "" }),
    });
    const body = await response.json().catch(() => ({}));
    if (!response.ok || !body.ok) throw new Error(body.message || body.error || "The forum did not accept that.");
    if (body.commit) forumView.ref = body.commit;
    forumView.cache.clear();
    forumView.base = null;
    return body;
  }

  function forumCategoryRowsHtml(repo, base, categories) {
    const rows = categories.map((category) => {
      const subs = (category.subcategory_ids || []).map((id) => base.byId.get(Number(id))).filter(Boolean);
      const latest = (category.latest || []).map((row) => `<div><span class="min-w-0 truncate">${forumStateIcons(row)}<a href="${escapeHtml(forumHref(repo, forumTopicRoute(row)))}" data-forum-link>${escapeHtml(row.title || "")}</a></span>${forumWhenHtml(row.last_posted_at || row.bumped_at)}</div>`).join("");
      return `<tr class="fm-forum-cat"${forumColorStyle(category)}><td><a class="fm-forum-cat-name" href="${escapeHtml(forumHref(repo, forumCategoryRoute(category)))}" data-forum-link><span class="fm-forum-dot"></span>${escapeHtml(category.name)}</a>${category.description ? `<div class="fm-forum-desc">${escapeHtml(category.description)}</div>` : ""}${subs.length ? `<div class="fm-forum-subs">${subs.map((sub) => `<a class="fm-forum-chip" href="${escapeHtml(forumHref(repo, forumCategoryRoute(sub)))}" data-forum-link${forumColorStyle(sub)}><span class="fm-forum-dot"></span>${escapeHtml(sub.name)}</a>`).join("")}</div>` : ""}</td><td class="fm-forum-topics">${escapeHtml(forumCount(category.total_topic_count ?? category.topic_count))}</td><td class="fm-forum-hide-sm"><div class="fm-forum-latest">${latest}</div></td></tr>`;
    }).join("");
    return `<table class="fm-forum-table" data-forum-categories><thead><tr><th>Category</th><th class="fm-forum-num">Topics</th><th class="fm-forum-hide-sm">Latest</th></tr></thead><tbody>${rows}</tbody></table>`;
  }

  function forumTopicRowsHtml(repo, base, rows, options = {}) {
    const seen = forumSeen(repo);
    return rows.map((row) => {
      const posters = (row.posters || []).slice(0, 5).map((poster) => forumAvatar(poster.username, 25, `${poster.username}${poster.description ? ` - ${poster.description}` : ""}`)).join("");
      const tags = (row.tags || []).map((tag) => {
        const known = base.tags.find((item) => item.name === tag);
        return known ? `<a class="fm-forum-tag" href="${escapeHtml(forumHref(repo, { view: "tag", key: known.key }))}" data-forum-link>${escapeHtml(tag)}</a>` : `<span class="fm-forum-tag">${escapeHtml(tag)}</span>`;
      }).join("");
      const chip = options.hideCategory === Number(row.category_id) ? "" : forumChip(repo, base, row.category_id);
      const replies = Number.isFinite(Number(row.reply_count)) ? Number(row.reply_count) : Math.max(0, Number(row.posts_count || 1) - 1);
      const unread = forumUnread(seen, row) ? '<span class="fm-forum-new" title="New posts"></span>' : "";
      const excerpt = row.pinned && row.excerpt ? `<div class="fm-forum-excerpt">${escapeHtml(row.excerpt)}</div>` : "";
      return `<tr data-forum-topic-row="${escapeHtml(String(row.id))}"><td>${forumStateIcons(row)}<a class="fm-forum-title" href="${escapeHtml(forumHref(repo, forumTopicRoute(row)))}" data-forum-link>${escapeHtml(row.title || "")}</a>${unread}${chip || tags ? `<div class="fm-forum-row-meta">${chip}${tags}</div>` : ""}${excerpt}</td><td class="fm-forum-hide-sm"><div class="fm-forum-posters">${posters}</div></td><td class="fm-forum-num">${escapeHtml(forumCount(replies))}</td><td class="fm-forum-num fm-forum-hide-sm">${escapeHtml(forumCount(row.views))}</td><td class="fm-forum-num">${forumWhenHtml(row.bumped_at || row.last_posted_at)}</td></tr>`;
    }).join("");
  }

  function forumTopicTableHtml(repo, base, rows, options = {}) {
    if (!rows.length) return '<div class="fm-blankslate"><p>No topics here yet.</p></div>';
    return `<table class="fm-forum-table" data-forum-topics><thead><tr><th>Topic</th><th class="fm-forum-hide-sm"></th><th class="fm-forum-num">Replies</th><th class="fm-forum-num fm-forum-hide-sm">Views</th><th class="fm-forum-num">Activity</th></tr></thead><tbody data-forum-topic-rows>${forumTopicRowsHtml(repo, base, rows, options)}</tbody></table>${options.more ? `<div class="fm-forum-more"><button type="button" class="fm-btn fm-btn-sm" data-forum-more="${escapeHtml(options.more)}">Load more topics</button></div>` : ""}`;
  }

  function forumPostKey(post) {
    return String(post.key || post.post_number || "");
  }

  function forumMergePosts(topic, imported, extra) {
    const state = extra.state || {};
    const likes = extra.likes || {};
    const account = extra.viewer?.account || "";
    const hidden = new Set(state.hidden || []);
    const deleted = new Set(state.deleted || []);
    const local = (extra.local?.posts || []).filter((post) => post && post.key);
    const merged = [...imported.map((post) => ({ ...post, key: forumPostKey(post) })), ...local];
    if (local.length) merged.sort((a, b) => forumTime(a.created_at) - forumTime(b.created_at));
    return merged.map((post) => {
      const likers = Array.isArray(likes[post.key]) ? likes[post.key] : [];
      return {
        ...post,
        hidden: hidden.has(post.key),
        removed: deleted.has(post.key) || Boolean(post.deleted),
        likeTotal: (Number(post.like_count) || 0) + likers.length,
        liked: Boolean(account) && likers.includes(account),
        mine: Boolean(account) && post.origin === "forkmesh" && post.username === account,
      };
    });
  }

  function forumPostsHtml(repo, base, topic, posts, extra = {}) {
    const host = base.origin ? base.origin.host : "";
    const viewer = extra.viewer || {};
    const moderator = Boolean(viewer.moderator);
    const canReply = Boolean(viewer.account) && (moderator || (!topic.closed && !topic.archived));
    const ctx = {
      origin: base.origin,
      uploads: topic.uploads && typeof topic.uploads === "object" ? topic.uploads : {},
      rawUrl: (file) => repoRawUrl(repo, `${FORUM_ROOT}/uploads/${file}`, repoDefaultBranch(repo)),
      topicHref: (id, post) => forumHref(repo, { view: "topic", id: String(id), slug: String(id) === String(topic.id) ? topic.slug : "topic", post }),
    };
    return posts.map((post) => {
      const key = forumPostKey(post);
      const anchor = `post_${escapeHtml(key)}`;
      if (post.removed && !moderator) return "";
      if (Number(post.post_type) === 3) {
        const action = String(post.action_code || "updated the topic").replace(/[._]/g, " ");
        return `<div class="fm-forum-small" id="${anchor}">${forumAvatar(post.username, 20)} <strong>${escapeHtml(post.username || "")}</strong> ${escapeHtml(action)} ${forumWhenHtml(post.created_at)}</div>`;
      }
      const display = post.name && post.name !== post.username ? ` <span class="fm-forum-ago">${escapeHtml(post.name)}</span>` : "";
      const replyTarget = String(post.reply_to || post.reply_to_post_number || "");
      const reply = replyTarget ? ` <a class="fm-forum-icon-btn" href="#post_${escapeHtml(replyTarget)}" title="In reply to post ${escapeHtml(replyTarget)}"><i data-lucide="corner-up-left"></i>${escapeHtml(replyTarget.replace(/^l/, ""))}</a>` : "";
      let body;
      if (post.removed) body = '<p class="fm-forum-ago">This post was deleted.</p>';
      else if (post.hidden && !moderator) body = '<p class="fm-forum-ago">An operator hid this post.</p>';
      else body = post.format === "markdown" ? renderMarkdown(post.raw || "", { repo }) : forumCookedHtml(post.cooked, ctx);
      const flag = post.hidden && moderator ? '<span class="fm-forum-tag">hidden</span>' : post.removed ? '<span class="fm-forum-tag">deleted</span>' : "";
      const sourceUrl = post.origin !== "forkmesh" && host ? forumOriginUrl(topic.source) : null;
      const source = sourceUrl ? `${escapeHtml(sourceUrl.href.replace(/\/$/, ""))}/${escapeHtml(key)}` : "";
      const likeLabel = post.likeTotal === 1 ? "1 like" : `${post.likeTotal} likes`;
      const like = viewer.account && !post.mine && !post.removed
        ? `<button type="button" class="fm-forum-icon-btn" data-forum-like="${escapeHtml(key)}" aria-pressed="${post.liked ? "true" : "false"}" title="${post.liked ? "Unlike" : "Like"} (${likeLabel})"><i data-lucide="heart"></i>${post.likeTotal ? escapeHtml(forumCount(post.likeTotal)) : ""}</button>`
        : `<span class="fm-forum-icon-btn" title="${likeLabel}"><i data-lucide="heart"></i>${post.likeTotal ? escapeHtml(forumCount(post.likeTotal)) : ""}</span>`;
      const own = post.mine && !post.removed
        ? `<button type="button" class="fm-forum-icon-btn" data-forum-edit="${escapeHtml(key)}" title="Edit"><i data-lucide="pencil"></i></button><button type="button" class="fm-forum-icon-btn" data-forum-delete="${escapeHtml(key)}" title="Delete"><i data-lucide="trash-2"></i></button>`
        : "";
      const operator = moderator && !post.mine
        ? `<button type="button" class="fm-forum-icon-btn" data-forum-hide="${escapeHtml(key)}" data-forum-hidden="${post.hidden ? "1" : ""}" title="${post.hidden ? "Show again" : "Hide"}"><i data-lucide="${post.hidden ? "eye" : "eye-off"}"></i></button>${post.removed ? "" : `<button type="button" class="fm-forum-icon-btn" data-forum-delete="${escapeHtml(key)}" title="Delete"><i data-lucide="trash-2"></i></button>`}${post.username ? `<button type="button" class="fm-forum-icon-btn" data-forum-silence="${escapeHtml(post.username)}" title="Silence @${escapeHtml(post.username)}"><i data-lucide="volume-x"></i></button>` : ""}`
        : "";
      const replyButton = canReply && !post.removed ? `<button type="button" class="fm-forum-icon-btn" data-forum-reply-to="${escapeHtml(key)}" title="Reply to this post"><i data-lucide="reply"></i></button>` : "";
      return `<article class="fm-forum-post" id="${anchor}" data-forum-post="${escapeHtml(key)}"><div>${forumAvatar(post.username, 45)}</div><div class="min-w-0"><div class="fm-forum-post-head"><span><strong>${escapeHtml(post.username || "")}</strong>${display}${reply} ${flag}</span>${forumWhenHtml(post.created_at)}</div><div class="fm-forum-cooked" data-forum-body>${body}</div><div class="fm-forum-actions">${operator}${own}${like}<button type="button" class="fm-forum-icon-btn" data-forum-copy="${escapeHtml(key)}" title="Copy a link to this post"><i data-lucide="link"></i></button>${source ? `<a class="fm-forum-icon-btn" href="${source}" rel="nofollow noopener noreferrer" target="_blank" title="Open on ${escapeHtml(host)}"><i data-lucide="external-link"></i></a>` : ""}${replyButton}</div></div></article>`;
    }).join("");
  }

  function forumOperatorMenuHtml(base, topic) {
    const toggle = (on, yes, no) => (on ? no : yes);
    const items = [
      topic.pinned ? ["unpin", "Unpin"] : ["pin", "Pin in its category"],
      topic.pinned ? null : ["pin-globally", "Pin everywhere"],
      toggle(topic.closed, ["close", "Close"], ["open", "Open again"]),
      toggle(topic.archived, ["archive", "Archive"], ["unarchive", "Unarchive"]),
      topic.visible === false ? ["list", "List again"] : ["unlist", "Unlist"],
    ].filter(Boolean);
    const moves = base.categories.filter((item) => Number(item.id) !== Number(topic.category_id))
      .map((item) => `<option value="${Number(item.id)}">${escapeHtml(item.name)}</option>`).join("");
    return `<details class="fm-forum-pick" data-forum-pick data-forum-operator><summary><i data-lucide="wrench" class="h-3.5 w-3.5"></i>Operator</summary><div class="fm-forum-menu" role="menu">${items.map(([action, label]) => `<a href="#" role="menuitem" data-forum-op="${action}">${label}</a>`).join("")}${moves ? `<form class="fm-forum-move" data-forum-move-form><select data-forum-move aria-label="Move to category">${moves}</select><button type="submit" class="fm-btn fm-btn-sm">Move</button></form>` : ""}</div></details>`;
  }

  function forumComposerHtml(kind, options = {}) {
    const title = kind === "topic"
      ? `<input type="text" class="fm-forum-input" data-forum-title maxlength="255" placeholder="Title" aria-label="Title" required>`
      : "";
    const categories = kind === "topic"
      ? `<div class="fm-forum-row-meta"><select data-forum-category aria-label="Category">${(options.categories || []).map((item) => `<option value="${Number(item.id)}"${Number(item.id) === Number(options.category) ? " selected" : ""}>${escapeHtml(item.name)}</option>`).join("")}</select><input type="text" class="fm-forum-input" data-forum-tags placeholder="Tags, separated by commas" aria-label="Tags"></div>`
      : "";
    const replying = options.replyTo ? `<p class="fm-forum-ago" data-forum-replying>Replying to post ${escapeHtml(String(options.replyTo).replace(/^l/, ""))} <button type="button" class="link-btn" data-forum-reply-clear>Cancel</button></p>` : "";
    const label = kind === "topic" ? "Create topic" : kind === "edit" ? "Save" : "Reply";
    const cta = kind === "topic" ? "fm-btn fm-btn-cta" : "fm-btn";
    return `<form class="fm-forum-composer" data-forum-composer="${kind}"${options.key ? ` data-forum-key="${escapeHtml(options.key)}"` : ""}${options.replyTo ? ` data-forum-reply="${escapeHtml(options.replyTo)}"` : ""}>${title}${categories}${replying}<textarea class="fm-forum-input" data-forum-body-input rows="${kind === "topic" ? 10 : 5}" maxlength="32000" placeholder="${kind === "topic" ? "What is this topic about? Markdown works." : "Write a reply. Markdown works."}" aria-label="Post" required>${escapeHtml(options.body || "")}</textarea><div class="fm-forum-composer-bar"><span class="fm-forum-ago" data-forum-status role="status" aria-live="polite"></span>${kind === "edit" ? '<button type="button" class="fm-btn fm-btn-sm" data-forum-edit-cancel>Cancel</button>' : ""}<button type="submit" class="${cta}">${label}</button></div></form>`;
  }

  function forumTopicPageHtml(repo, base, topic, posts, suggested, more, extra = {}) {
    const tags = (topic.tags || []).map((tag) => {
      const known = base.tags.find((item) => item.name === tag);
      return known ? `<a class="fm-forum-tag" href="${escapeHtml(forumHref(repo, { view: "tag", key: known.key }))}" data-forum-link>${escapeHtml(tag)}</a>` : `<span class="fm-forum-tag">${escapeHtml(tag)}</span>`;
    }).join("");
    const host = base.origin ? base.origin.host : "";
    const viewer = extra.viewer || {};
    const localCount = (extra.local?.posts || []).filter((post) => !post.deleted).length;
    const stats = [
      ["views", topic.views],
      ["replies", (topic.reply_count ?? Math.max(0, Number(topic.posts_count || 1) - 1)) + (String(topic.id).startsWith("n") ? Math.max(0, localCount - 1) : localCount)],
      ["likes", topic.like_count],
      ["users", topic.participant_count],
      ["links", topic.link_count],
    ].filter(([, value]) => Number(value) > 0)
      .map(([label, value]) => `<div><strong>${escapeHtml(forumCount(value))}</strong>${label}</div>`).join("");
    const state = topic.archived ? "This topic is archived." : topic.closed ? "This topic is closed for new replies." : topic.visible === false ? "This topic is unlisted." : "";
    const canReply = Boolean(viewer.account) && (viewer.moderator || (!topic.closed && !topic.archived));
    const originalUrl = topic.origin !== "forkmesh" && host ? forumOriginUrl(topic.source) : null;
    const original = originalUrl
      ? `<a class="fm-btn fm-btn-sm" href="${escapeHtml(originalUrl.href)}" rel="nofollow noopener noreferrer" target="_blank" data-forum-original><i data-lucide="external-link" class="h-3.5 w-3.5"></i>Open on ${escapeHtml(host)}</a>`
      : "";
    const signIn = viewer.account ? "" : `<a class="fm-btn fm-btn-sm" href="/login?next=${encodeURIComponent(location.pathname)}">Sign in to reply</a>`;
    return `<header class="fm-forum-header"><div class="fm-forum-title-row"><h1>${forumStateIcons(topic)}${escapeHtml(topic.title || "")}</h1>${viewer.moderator ? forumOperatorMenuHtml(base, topic) : ""}</div><div class="fm-forum-row-meta">${forumChip(repo, base, topic.category_id)}${tags}</div></header><div class="mt-4" data-forum-posts>${forumPostsHtml(repo, base, topic, posts, extra)}</div>${more ? `<div class="fm-forum-more"><button type="button" class="fm-btn fm-btn-sm" data-forum-more-posts="1">Load more posts</button></div>` : ""}${stats ? `<div class="fm-forum-stats">${stats}</div>` : ""}<div class="fm-forum-reply">${state ? `<span class="fm-forum-ago mr-auto">${state}</span>` : ""}${original}${signIn}</div>${canReply ? `<div class="fm-forum-reply-box" data-forum-reply-box>${forumComposerHtml("post")}</div>` : ""}${suggested.length ? `<h2>New &amp; Unread Topics</h2>${forumTopicTableHtml(repo, base, suggested)}` : ""}`;
  }

  function forumEmptyHtml(repo, viewer = {}) {
    const start = viewer.moderator ? `<p class="mt-3"><button type="button" class="fm-btn fm-btn-cta" data-forum-start>Start the forum</button></p><p class="mt-2">Or mirror a Discourse forum into it with <a href="/start-discourse-mirror">Start a Discourse mirror</a>.</p>` : "";
    return `<div class="fm-blankslate" data-forum-empty><h2>No forum in this repository yet</h2>${start}<p>A forum lives in <code>${FORUM_ROOT}</code>. Copy a Discourse forum into it with the Discourse Mirror plugin, then publish the repository:</p><pre class="mt-2 overflow-auto rounded-md bg-secondary p-3 text-left text-xs">python3 discourse_mirror.py add https://forum.example.org --name example --forum --repository &lt;this repository's checkout&gt;
python3 discourse_mirror.py sync example</pre></div>`;
  }

  function forumUnavailableHtml(message) {
    return `<div class="fm-blankslate" data-forum-error><p>${escapeHtml(message)}</p><button type="button" class="fm-btn fm-btn-sm mt-2" data-forum-retry>Try again</button></div>`;
  }

  function forumPageFile(route, base, page) {
    if (route.view === "category") return `c/${route.id}/${page}.json`;
    if (route.view === "tag") return `tag/${route.key}/${page}.json`;
    return `latest/${page}.json`;
  }

  async function forumListHtml(repo, base, route) {
    const file = forumPageFile(route, base, 1);
    const data = (await forumRead(repo, [file]))[file] || { rows: [] };
    const hide = route.view === "category" && !(base.byId.get(route.id)?.subcategory_ids || []).length ? route.id : 0;
    return forumTopicTableHtml(repo, base, Array.isArray(data.rows) ? data.rows : [], { more: data.more ? "2" : "", hideCategory: hide });
  }

  async function forumRenderView(repo, base, route) {
    if (route.view === "categories") {
      return forumCategoryRowsHtml(repo, base, base.categories.filter((item) => !item.parent_category_id));
    }
    if (route.view === "category") {
      const category = base.byId.get(route.id);
      if (!category) return '<div class="fm-blankslate"><p>This category is not in the forum.</p></div>';
      const subs = (category.subcategory_ids || []).map((id) => base.byId.get(Number(id))).filter(Boolean);
      const head = `${category.description ? `<p class="fm-forum-desc mb-3">${escapeHtml(category.description)}</p>` : ""}`;
      if (subs.length && !route.list) return `${head}${forumCategoryRowsHtml(repo, base, subs)}`;
      return `${head}${await forumListHtml(repo, base, route)}`;
    }
    if (route.view === "top") {
      const file = `top/${route.period}.json`;
      const data = (await forumRead(repo, [file]))[file] || { rows: [] };
      const periods = `<div class="fm-segmented mb-3" role="group" aria-label="Period">${FORUM_TOP_PERIODS.map((item) => `<a href="${escapeHtml(forumHref(repo, { view: "top", period: item.id }))}" data-forum-link class="fm-btn fm-btn-sm"${item.id === route.period ? ' aria-current="page"' : ""}>${item.label}</a>`).join("")}</div>`;
      return `${periods}${forumTopicTableHtml(repo, base, Array.isArray(data.rows) ? data.rows : [])}`;
    }
    if (route.view === "tag") {
      if (!base.tags.some((tag) => tag.key === route.key)) return '<div class="fm-blankslate"><p>No topics have this tag.</p></div>';
      return forumListHtml(repo, base, route);
    }
    return forumListHtml(repo, base, route);
  }

  async function forumRenderTopic(repo, base, route, main, seq) {
    const file = `topics/${route.id}/topic.json`;
    const folder = `topics/${route.id}/`;
    const [read, viewer] = await Promise.all([
      forumRead(repo, [file, "latest/1.json", `${folder}local.json`, `${folder}likes.json`, `${folder}state.json`]),
      forumViewer(repo),
    ]);
    if (seq !== forumView.seq) return;
    const stateFile = read[`${folder}state.json`] || {};
    const header = read[file];
    const topic = header ? { ...header, ...Object.fromEntries(["pinned", "pinned_globally", "closed", "archived", "visible", "category_id", "title"].filter((key) => key in stateFile).map((key) => [key, stateFile[key]])) } : null;
    const extra = { local: read[`${folder}local.json`] || { posts: [] }, likes: read[`${folder}likes.json`] || {}, state: stateFile, viewer };
    if (!topic || !Array.isArray(topic.posts)) {
      main.innerHTML = '<div class="fm-blankslate"><p>This topic is not in the forum. It may have been removed.</p></div>';
      return;
    }
    const canonical = forumHref(repo, forumTopicRoute(topic, route.post));
    if (`${location.pathname}${location.search}` !== canonical) window.history.replaceState(null, "", canonical);
    const seen = forumSeen(repo);
    const latest = Array.isArray(read["latest/1.json"]?.rows) ? read["latest/1.json"].rows : [];
    const others = latest.filter((row) => String(row.id) !== String(topic.id) && !row.pinned_globally);
    const suggested = [...others.filter((row) => forumUnread(seen, row)), ...others.filter((row) => !forumUnread(seen, row))].slice(0, FORUM_SUGGESTED);
    const chunks = Math.max(1, Number(topic.chunks) || 1);
    let posts = topic.posts;
    let next = 1;
    const localPosts = extra.local.posts || [];
    const wanted = Number(route.post) || 0;
    const lastLoaded = Number(posts[posts.length - 1]?.post_number) || 0;
    if ((wanted > lastLoaded || /^#post_l\d/.test(location.hash)) && chunks > 1) {
      const files = Array.from({ length: chunks - 1 }, (_, index) => `topics/${route.id}/posts/${index + 1}.json`);
      const read = await forumRead(repo, files);
      if (seq !== forumView.seq) return;
      files.forEach((path) => { posts = posts.concat(Array.isArray(read[path]?.posts) ? read[path].posts : []); });
      next = chunks;
    }
    forumView.topic = { id: String(topic.id), next, chunks, topic, extra };
    const merged = forumMergePosts(topic, posts, next < chunks ? { ...extra, local: { posts: [] } } : extra);
    main.innerHTML = forumTopicPageHtml(repo, base, topic, merged, suggested, next < chunks, extra);
    if (next < chunks && localPosts.length) forumView.topic.pendingLocal = true;
    forumMarkSeen(repo, topic);
    window.lucide?.createIcons();
    const anchor = /^#post_[A-Za-z0-9]{1,12}$/.test(location.hash) ? location.hash.slice(1) : wanted > 1 ? `post_${wanted}` : "";
    if (anchor) document.getElementById(anchor)?.scrollIntoView?.({ block: "start" });
  }

  async function forumFeatures(repo) {
    if (forumView.features && forumView.features.key === repoKey(repo)) return forumView.features.value;
    let value = null;
    try {
      const body = await fetchRepoJson(`${repoApiBase(repo)}/about`);
      value = body?.ok && body.features && typeof body.features === "object" ? body.features : null;
    } catch (_) {
      value = null;
    }
    forumView.features = { key: repoKey(repo), value };
    if (value) applyRepoFeatures(value);
    return value;
  }

  async function forumRender(repo) {
    const container = $("[data-repo-forum]");
    if (!container || !repo) return;
    const seq = ++forumView.seq;
    const route = forumRoute(repoRouteParts().slice(3), location.search);
    const features = await forumFeatures(repo);
    if (seq !== forumView.seq) return;
    if (features && features.forum === false) {
      container.innerHTML = '<div class="fm-blankslate" data-forum-off><p>The forum is turned off for this repository. Its owner can turn it on under Settings, Features.</p></div>';
      return;
    }
    let base;
    try {
      base = await forumBase(repo);
    } catch (error) {
      if (seq === forumView.seq) container.innerHTML = forumUnavailableHtml("The forum could not be read from any mirror right now.");
      return;
    }
    if (seq !== forumView.seq) return;
    const viewer = await forumViewer(repo);
    if (seq !== forumView.seq) return;
    if (!base.forum) {
      container.innerHTML = forumEmptyHtml(repo, viewer);
      return;
    }
    const title = base.forum.title || "Forum";
    const showBar = !["topic", "new", "admin"].includes(route.view);
    container.innerHTML = `<div class="fm-forum" data-forum-view="${escapeHtml(route.view)}"><div class="fm-forum-bar-wrap">${showBar ? forumBarHtml(repo, base, route, viewer) : ""}</div><div data-forum-main aria-busy="true"><div class="fm-forum-ago">Loading ${escapeHtml(title)}…</div></div></div>`;
    window.lucide?.createIcons();
    const main = container.querySelector("[data-forum-main]");
    try {
      if (route.view === "topic") await forumRenderTopic(repo, base, route, main, seq);
      else if (route.view === "new") main.innerHTML = forumNewTopicHtml(repo, base, route, viewer);
      else if (route.view === "admin") main.innerHTML = forumAdminHtml(repo, base, viewer);
      else {
        const html = await forumRenderView(repo, base, route);
        if (seq !== forumView.seq) return;
        main.innerHTML = html;
        window.lucide?.createIcons();
      }
    } catch (error) {
      if (seq === forumView.seq) main.innerHTML = forumUnavailableHtml("This part of the forum could not be read right now.");
    }
    main.removeAttribute("aria-busy");
  }

  async function forumLoadMoreTopics(repo, button) {
    const route = forumRoute(repoRouteParts().slice(3), location.search);
    const base = await forumBase(repo);
    const page = Number(button.dataset.forumMore) || 2;
    button.disabled = true;
    const file = forumPageFile(route, base, page);
    const data = (await forumRead(repo, [file]))[file];
    const body = button.closest("[data-repo-forum]")?.querySelector("[data-forum-topic-rows]");
    if (!data || !body) {
      button.disabled = false;
      return;
    }
    const hide = route.view === "category" && !(base.byId.get(route.id)?.subcategory_ids || []).length ? route.id : 0;
    body.insertAdjacentHTML("beforeend", forumTopicRowsHtml(repo, base, Array.isArray(data.rows) ? data.rows : [], { hideCategory: hide }));
    window.lucide?.createIcons();
    if (data.more) {
      button.dataset.forumMore = String(page + 1);
      button.disabled = false;
    } else {
      button.closest(".fm-forum-more")?.remove();
    }
  }

  async function forumLoadMorePosts(repo, button) {
    const current = forumView.topic;
    if (!current) return;
    const base = await forumBase(repo);
    button.disabled = true;
    const file = `topics/${current.id}/posts/${current.next}.json`;
    const data = (await forumRead(repo, [file]))[file];
    const list = button.closest("[data-repo-forum]")?.querySelector("[data-forum-posts]");
    if (!data || !list) {
      button.disabled = false;
      return;
    }
    current.next += 1;
    const last = current.next >= current.chunks;
    const chunk = forumMergePosts(current.topic, Array.isArray(data.posts) ? data.posts : [], last ? current.extra : { ...current.extra, local: { posts: [] } });
    list.insertAdjacentHTML("beforeend", forumPostsHtml(repo, base, current.topic, chunk, current.extra));
    window.lucide?.createIcons();
    if (current.next >= current.chunks) button.closest(".fm-forum-more")?.remove();
    else button.disabled = false;
  }

  function forumNewTopicHtml(repo, base, route, viewer) {
    if (!viewer.account) return `<div class="fm-blankslate"><p>Sign in to start a topic.</p><a class="fm-btn fm-btn-sm mt-2" href="/login?next=${encodeURIComponent(location.pathname + location.search)}">Sign in</a></div>`;
    return `<header class="fm-forum-header"><h1>New topic</h1></header><div class="mt-4">${forumComposerHtml("topic", { categories: base.categories, category: route.category })}</div>`;
  }

  function forumAdminHtml(repo, base, viewer) {
    if (!viewer.moderator) return '<div class="fm-blankslate"><p>Only the forum\'s operators can change its settings.</p></div>';
    const parents = base.categories.filter((item) => !item.parent_category_id);
    const parentOptions = (selected) => `<option value="0">No parent</option>${parents.map((item) => `<option value="${Number(item.id)}"${Number(item.id) === Number(selected) ? " selected" : ""}>${escapeHtml(item.name)}</option>`).join("")}`;
    const rows = base.categories.map((item) => `<tr${forumColorStyle(item)}><td><span class="fm-forum-dot"></span> ${escapeHtml(item.name)}${item.parent_category_id ? ` <span class="fm-forum-ago">in ${escapeHtml(base.byId.get(Number(item.parent_category_id))?.name || "")}</span>` : ""}</td><td class="fm-forum-num">${escapeHtml(forumCount(item.total_topic_count ?? item.topic_count))}</td><td class="fm-forum-num"><button type="button" class="fm-btn fm-btn-sm" data-forum-category-edit="${Number(item.id)}">Edit</button>${Number(item.id) >= 1000000 && !(item.total_topic_count || item.topic_count) && !(item.subcategory_ids || []).length ? ` <button type="button" class="fm-btn fm-btn-sm fm-btn-danger" data-forum-category-delete="${Number(item.id)}">Delete</button>` : ""}</td></tr>`).join("");
    const form = (item = {}) => `<form class="fm-forum-composer" data-forum-category-form data-forum-category-id="${Number(item.id) || 0}"><h2>${item.id ? `Edit ${escapeHtml(item.name)}` : "New category"}</h2><input type="text" class="fm-forum-input" data-forum-category-name maxlength="50" value="${escapeHtml(item.name || "")}" placeholder="Name" aria-label="Name" required><div class="fm-forum-row-meta"><label>Colour <input type="color" data-forum-category-color value="${escapeHtml(forumColor(item.color) || "#0088cc")}"></label><label>Parent <select data-forum-category-parent>${parentOptions(item.parent_category_id)}</select></label><label>Position <input type="number" class="fm-forum-input fm-forum-short" data-forum-category-position min="0" max="10000" value="${Number(item.position) || 0}"></label></div><textarea class="fm-forum-input" data-forum-category-description maxlength="500" rows="2" placeholder="Description" aria-label="Description">${escapeHtml(item.description || "")}</textarea><div class="fm-forum-composer-bar"><span class="fm-forum-ago" data-forum-status role="status" aria-live="polite"></span><button type="submit" class="fm-btn">${item.id ? "Save category" : "Add category"}</button></div></form>`;
    forumView.categoryForm = form;
    return `<header class="fm-forum-header"><h1>Forum settings</h1><p class="fm-forum-desc">Changes are signed by you and saved in the repository.</p></header><form class="fm-forum-composer mt-4" data-forum-site-form><h2>Title and description</h2><input type="text" class="fm-forum-input" data-forum-site-title maxlength="100" value="${escapeHtml(base.forum?.title || "")}" aria-label="Forum title" required><textarea class="fm-forum-input" data-forum-site-description maxlength="500" rows="2" aria-label="Forum description">${escapeHtml(base.forum?.description || "")}</textarea><div class="fm-forum-composer-bar"><span class="fm-forum-ago" data-forum-status role="status" aria-live="polite"></span><button type="submit" class="fm-btn">Save</button></div></form><h2>Categories</h2><table class="fm-forum-table" data-forum-admin-categories><tbody>${rows}</tbody></table><div data-forum-category-slot>${form()}</div>`;
  }

  function forumStatus(form, text) {
    const status = form?.querySelector("[data-forum-status]");
    if (status) status.textContent = text || "";
  }

  async function forumAct(repo, form, type, topic, post, content, after) {
    const submit = form?.querySelector('button[type="submit"]');
    if (submit) submit.disabled = true;
    forumStatus(form, "Saving…");
    try {
      const result = await forumSubmit(repo, type, topic, post, content);
      forumStatus(form, "");
      if (after) after(result);
      else void forumRender(repo);
      return result;
    } catch (error) {
      forumStatus(form, String(error?.message || error));
      if (!form) window.alert(String(error?.message || error));
      return null;
    } finally {
      if (submit) submit.disabled = false;
    }
  }

  async function forumHandleSubmit(repo, form) {
    const topic = forumView.topic ? String(forumView.topic.id) : "";
    if (form.matches("[data-forum-composer]")) {
      const kind = form.dataset.forumComposer;
      const body = form.querySelector("[data-forum-body-input]").value.replace(/\s+$/, "");
      if (!body) return;
      if (kind === "topic") {
        const tags = form.querySelector("[data-forum-tags]").value.split(",").map((tag) => tag.trim().toLowerCase().replace(/\s+/g, "-")).filter(Boolean).slice(0, 5);
        await forumAct(repo, form, "topic", "", "", {
          title: form.querySelector("[data-forum-title]").value.trim(),
          body,
          category: Number(form.querySelector("[data-forum-category]")?.value || 0),
          tags,
        }, (result) => {
          navigateHistory(forumHref(repo, { view: "topic", id: result.topic, slug: form.querySelector("[data-forum-title]").value }));
          void forumRender(repo);
        });
      } else if (kind === "post") {
        await forumAct(repo, form, "post", topic, "", { body, replyTo: form.dataset.forumReply || "" }, (result) => {
          navigateHistory(`${location.pathname}#post_${result.post}`);
          void forumRender(repo);
        });
      } else if (kind === "edit") {
        await forumAct(repo, form, "edit", topic, form.dataset.forumKey, { body });
      }
      return;
    }
    if (form.matches("[data-forum-site-form]")) {
      await forumAct(repo, form, "site", "", "", {
        title: form.querySelector("[data-forum-site-title]").value.trim(),
        description: form.querySelector("[data-forum-site-description]").value.trim(),
      });
      return;
    }
    if (form.matches("[data-forum-category-form]")) {
      await forumAct(repo, form, "category", "", "", {
        id: Number(form.dataset.forumCategoryId) || 0,
        name: form.querySelector("[data-forum-category-name]").value.trim(),
        color: form.querySelector("[data-forum-category-color]").value.replace(/^#/, "").toUpperCase(),
        description: form.querySelector("[data-forum-category-description]").value.trim(),
        parent: Number(form.querySelector("[data-forum-category-parent]").value) || 0,
        position: Number(form.querySelector("[data-forum-category-position]").value) || 0,
      });
      return;
    }
    if (form.matches("[data-forum-move-form]")) {
      await forumAct(repo, null, "move", topic, "", { category: Number(form.querySelector("[data-forum-move]").value) });
    }
  }

  function forumBind(container) {
    if (forumView.bound === container) return;
    forumView.bound = container;
    container.addEventListener("click", (event) => {
      const repo = state.selectedRepo;
      if (!repo) return;
      const plain = event.button === 0 && !event.metaKey && !event.ctrlKey && !event.shiftKey && !event.altKey;
      const link = event.target.closest("a[data-forum-link]");
      if (link && plain) {
        event.preventDefault();
        link.closest("details")?.removeAttribute("open");
        navigateHistory(link.getAttribute("href"));
        void forumRender(repo);
        container.scrollIntoView?.({ block: "start" });
        return;
      }
      const more = event.target.closest("[data-forum-more]");
      if (more) {
        void forumLoadMoreTopics(repo, more);
        return;
      }
      const morePosts = event.target.closest("[data-forum-more-posts]");
      if (morePosts) {
        void forumLoadMorePosts(repo, morePosts);
        return;
      }
      const copy = event.target.closest("[data-forum-copy]");
      if (copy && forumView.topic) {
        const url = new URL(forumHref(repo, forumTopicRoute(forumView.topic.topic, Number(copy.dataset.forumCopy) || 0)), location.origin).href;
        void navigator.clipboard?.writeText(url);
        copy.setAttribute("title", "Link copied");
        return;
      }
      const action = event.target.closest("[data-forum-like], [data-forum-reply-to], [data-forum-reply-clear], [data-forum-edit], [data-forum-edit-cancel], [data-forum-delete], [data-forum-hide], [data-forum-silence], [data-forum-op], [data-forum-start], [data-forum-category-edit], [data-forum-category-delete]");
      if (action) {
        event.preventDefault();
        void forumHandleAction(repo, container, action);
        return;
      }
      if (event.target.closest("[data-forum-retry]")) {
        forumView.base = null;
        forumView.cache.clear();
        void forumRender(repo);
      }
    });
    container.addEventListener("submit", (event) => {
      const form = event.target.closest("form");
      const repo = state.selectedRepo;
      if (!form || !repo || !container.contains(form)) return;
      event.preventDefault();
      void forumHandleSubmit(repo, form);
    });
    document.addEventListener("click", (event) => {
      container.querySelectorAll("details[data-forum-pick][open]").forEach((item) => {
        if (!item.contains(event.target)) item.removeAttribute("open");
      });
    });
  }

  async function forumHandleAction(repo, container, element) {
    const topic = forumView.topic ? String(forumView.topic.id) : "";
    const data = element.dataset;
    if ("forumLike" in data) {
      const liked = element.getAttribute("aria-pressed") === "true";
      element.disabled = true;
      await forumAct(repo, null, liked ? "unlike" : "like", topic, data.forumLike, {});
      return;
    }
    if ("forumReplyTo" in data) {
      const box = container.querySelector("[data-forum-reply-box]");
      if (!box) return;
      const draft = box.querySelector("[data-forum-body-input]")?.value || "";
      box.innerHTML = forumComposerHtml("post", { replyTo: data.forumReplyTo, body: draft });
      box.querySelector("[data-forum-body-input]")?.focus();
      return;
    }
    if ("forumReplyClear" in data) {
      const box = container.querySelector("[data-forum-reply-box]");
      const draft = box?.querySelector("[data-forum-body-input]")?.value || "";
      if (box) box.innerHTML = forumComposerHtml("post", { body: draft });
      return;
    }
    if ("forumEdit" in data) {
      const article = element.closest("[data-forum-post]");
      const post = (forumView.topic?.extra?.local?.posts || []).find((item) => item.key === data.forumEdit);
      const bodyNode = article?.querySelector("[data-forum-body]");
      if (!post || !bodyNode) return;
      bodyNode.innerHTML = forumComposerHtml("edit", { key: post.key, body: post.raw || "" });
      return;
    }
    if ("forumEditCancel" in data) {
      void forumRender(repo);
      return;
    }
    if ("forumDelete" in data) {
      if (!window.confirm("Delete this post?")) return;
      await forumAct(repo, null, "delete", topic, data.forumDelete, {});
      return;
    }
    if ("forumHide" in data) {
      await forumAct(repo, null, data.forumHidden ? "unhide" : "hide", topic, data.forumHide, {});
      return;
    }
    if ("forumSilence" in data) {
      const answer = window.prompt(`Silence @${data.forumSilence} in this forum for how many days? (0 lets them post again)`, "7");
      if (answer === null) return;
      const days = Math.floor(Number(answer));
      if (!Number.isFinite(days) || days < 0 || days > 3650) return;
      await forumAct(repo, null, days ? "silence" : "unsilence", "", "", days ? { account: data.forumSilence, days } : { account: data.forumSilence });
      return;
    }
    if ("forumOp" in data) {
      element.closest("details")?.removeAttribute("open");
      const op = data.forumOp;
      if (op === "pin-globally") await forumAct(repo, null, "pin", topic, "", { globally: true });
      else if (op === "pin") await forumAct(repo, null, "pin", topic, "", { globally: false });
      else await forumAct(repo, null, op, topic, "", {});
      return;
    }
    if ("forumStart" in data) {
      element.disabled = true;
      await forumAct(repo, null, "site", "", "", { title: repo.name, description: "" });
      return;
    }
    if ("forumCategoryEdit" in data) {
      const base = await forumBase(repo);
      const slot = container.querySelector("[data-forum-category-slot]");
      const item = base.byId.get(Number(data.forumCategoryEdit));
      if (slot && item && forumView.categoryForm) {
        slot.innerHTML = forumView.categoryForm(item);
        slot.querySelector("input")?.focus();
      }
      return;
    }
    if ("forumCategoryDelete" in data) {
      if (!window.confirm("Delete this empty category?")) return;
      await forumAct(repo, null, "category-delete", "", "", { id: Number(data.forumCategoryDelete) });
    }
  }

  function loadRepoForum(repo) {
    const container = $("[data-repo-forum]");
    if (!container || !repo) return;
    forumEnsureStyle();
    const key = repoKey(repo);
    if (forumView.repoKey !== key) {
      forumView.repoKey = key;
      forumView.cache.clear();
      forumView.base = null;
      forumView.topic = null;
      forumView.features = null;
      forumView.viewer = null;
      forumView.ref = "";
    }
    forumBind(container);
    void forumRender(repo);
  }
  return { loadRepoForum };
  };
})();
