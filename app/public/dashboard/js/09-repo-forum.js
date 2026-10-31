  const FORUM_ROOT = ".forkmesh/forum";
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
      const blobs = await fetchRepoBlobs(repo, wanted.map((path) => `${FORUM_ROOT}/${path}`), { ref: repoDefaultBranch(repo) });
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

  function forumBarHtml(repo, base, route) {
    return `<div class="fm-forum-bar">${forumPickersHtml(repo, base, route)}${forumNavHtml(repo, base, route)}</div>`;
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

  function forumPostsHtml(repo, base, topic, posts) {
    const host = base.origin ? base.origin.host : "";
    const ctx = {
      origin: base.origin,
      uploads: topic.uploads && typeof topic.uploads === "object" ? topic.uploads : {},
      rawUrl: (file) => repoRawUrl(repo, `${FORUM_ROOT}/uploads/${file}`, repoDefaultBranch(repo)),
      topicHref: (id, post) => forumHref(repo, { view: "topic", id: String(id), slug: String(id) === String(topic.id) ? topic.slug : "topic", post }),
    };
    return posts.map((post) => {
      const number = Number(post.post_number) || 0;
      if (Number(post.post_type) === 3) {
        const action = String(post.action_code || "updated the topic").replace(/[._]/g, " ");
        return `<div class="fm-forum-small" id="post_${number}">${forumAvatar(post.username, 20)} <strong>${escapeHtml(post.username || "")}</strong> ${escapeHtml(action)} ${forumWhenHtml(post.created_at)}</div>`;
      }
      const display = post.name && post.name !== post.username ? ` <span class="fm-forum-ago">${escapeHtml(post.name)}</span>` : "";
      const reply = post.reply_to_post_number ? ` <a class="fm-forum-icon-btn" href="#post_${Number(post.reply_to_post_number)}" title="In reply to post ${Number(post.reply_to_post_number)}"><i data-lucide="corner-up-left"></i>${Number(post.reply_to_post_number)}</a>` : "";
      const body = post.format === "markdown" ? escapeHtml(post.raw || "") : forumCookedHtml(post.cooked, ctx);
      const likes = Number(post.like_count) || 0;
      const source = topic.source && host ? `${topic.source}/${number}` : "";
      return `<article class="fm-forum-post" id="post_${number}" data-forum-post="${number}"><div>${forumAvatar(post.username, 45)}</div><div class="min-w-0"><div class="fm-forum-post-head"><span><strong>${escapeHtml(post.username || "")}</strong>${display}${reply}</span>${forumWhenHtml(post.created_at)}</div><div class="fm-forum-cooked">${body}</div><div class="fm-forum-actions"><span class="fm-forum-icon-btn" title="${likes === 1 ? "1 like" : `${likes} likes`}"><i data-lucide="heart"></i>${likes ? escapeHtml(forumCount(likes)) : ""}</span><button type="button" class="fm-forum-icon-btn" data-forum-copy="${number}" title="Copy a link to this post"><i data-lucide="link"></i></button>${source ? `<a class="fm-forum-icon-btn" href="${escapeHtml(source)}" rel="nofollow noopener noreferrer" target="_blank" title="Open on ${escapeHtml(host)}"><i data-lucide="external-link"></i></a>` : ""}</div></div></article>`;
    }).join("");
  }

  function forumTopicPageHtml(repo, base, topic, posts, suggested, more) {
    const tags = (topic.tags || []).map((tag) => {
      const known = base.tags.find((item) => item.name === tag);
      return known ? `<a class="fm-forum-tag" href="${escapeHtml(forumHref(repo, { view: "tag", key: known.key }))}" data-forum-link>${escapeHtml(tag)}</a>` : `<span class="fm-forum-tag">${escapeHtml(tag)}</span>`;
    }).join("");
    const host = base.origin ? base.origin.host : "";
    const stats = [
      ["views", topic.views],
      ["replies", topic.reply_count ?? Math.max(0, Number(topic.posts_count || 1) - 1)],
      ["likes", topic.like_count],
      ["users", topic.participant_count],
      ["links", topic.link_count],
    ].filter(([, value]) => Number(value) > 0)
      .map(([label, value]) => `<div><strong>${escapeHtml(forumCount(value))}</strong>${label}</div>`).join("");
    const state = topic.archived ? "This topic is archived." : topic.closed ? "This topic is closed for new replies." : "";
    return `<header class="fm-forum-header"><h1>${forumStateIcons(topic)}${escapeHtml(topic.title || "")}</h1><div class="fm-forum-row-meta">${forumChip(repo, base, topic.category_id)}${tags}</div></header><div class="mt-4" data-forum-posts>${forumPostsHtml(repo, base, topic, posts)}</div>${more ? `<div class="fm-forum-more"><button type="button" class="fm-btn fm-btn-sm" data-forum-more-posts="1">Load more posts</button></div>` : ""}${stats ? `<div class="fm-forum-stats">${stats}</div>` : ""}<div class="fm-forum-reply">${state ? `<span class="fm-forum-ago mr-auto">${state}</span>` : ""}${topic.source && host && !topic.closed && !topic.archived ? `<a class="fm-btn fm-btn-sm" href="${escapeHtml(topic.source)}" rel="nofollow noopener noreferrer" target="_blank" data-forum-reply><i data-lucide="reply" class="h-3.5 w-3.5"></i>Reply on ${escapeHtml(host)}</a>` : ""}</div>${suggested.length ? `<h2>New &amp; Unread Topics</h2>${forumTopicTableHtml(repo, base, suggested)}` : ""}`;
  }

  function forumEmptyHtml(repo) {
    return `<div class="fm-blankslate" data-forum-empty><h2>No forum in this repository yet</h2><p>A forum lives in <code>${FORUM_ROOT}</code>. Copy a Discourse forum into it with the Discourse Mirror plugin, then publish the repository:</p><pre class="mt-2 overflow-auto rounded-md bg-secondary p-3 text-left text-xs">python3 discourse_mirror.py add https://forum.example.org --name example --forum --repository &lt;this repository's checkout&gt;
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
    const read = await forumRead(repo, [file, "latest/1.json"]);
    if (seq !== forumView.seq) return;
    const topic = read[file];
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
    const wanted = Number(route.post) || 0;
    const lastLoaded = Number(posts[posts.length - 1]?.post_number) || 0;
    if (wanted > lastLoaded && chunks > 1) {
      const files = Array.from({ length: chunks - 1 }, (_, index) => `topics/${route.id}/posts/${index + 1}.json`);
      const read = await forumRead(repo, files);
      if (seq !== forumView.seq) return;
      files.forEach((path) => { posts = posts.concat(Array.isArray(read[path]?.posts) ? read[path].posts : []); });
      next = chunks;
    }
    forumView.topic = { id: String(topic.id), next, chunks, topic };
    main.innerHTML = forumTopicPageHtml(repo, base, topic, posts, suggested, next < chunks);
    forumMarkSeen(repo, topic);
    window.lucide?.createIcons();
    if (wanted > 1) document.getElementById(`post_${wanted}`)?.scrollIntoView?.({ block: "start" });
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
    if (!base.forum) {
      container.innerHTML = forumEmptyHtml(repo);
      return;
    }
    const title = base.forum.title || "Forum";
    container.innerHTML = `<div class="fm-forum" data-forum-view="${escapeHtml(route.view)}"><div class="fm-forum-bar-wrap">${route.view === "topic" ? "" : forumBarHtml(repo, base, route)}</div><div data-forum-main aria-busy="true"><div class="fm-forum-ago">Loading ${escapeHtml(title)}…</div></div></div>`;
    window.lucide?.createIcons();
    const main = container.querySelector("[data-forum-main]");
    try {
      if (route.view === "topic") await forumRenderTopic(repo, base, route, main, seq);
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
    list.insertAdjacentHTML("beforeend", forumPostsHtml(repo, base, current.topic, Array.isArray(data.posts) ? data.posts : []));
    window.lucide?.createIcons();
    current.next += 1;
    if (current.next >= current.chunks) button.closest(".fm-forum-more")?.remove();
    else button.disabled = false;
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
      if (event.target.closest("[data-forum-retry]")) {
        forumView.base = null;
        forumView.cache.clear();
        void forumRender(repo);
      }
    });
    document.addEventListener("click", (event) => {
      container.querySelectorAll("details[data-forum-pick][open]").forEach((item) => {
        if (!item.contains(event.target)) item.removeAttribute("open");
      });
    });
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
    }
    forumBind(container);
    void forumRender(repo);
  }
