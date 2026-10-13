(() => {
  if (globalThis.__forkmeshMarketingRead) return;

  const DAY = 86400000;
  const MAX_ELEMENTS = 25000;
  const MAX_TEXT = 70;
  const LABELS = [
    ["following", /^following$/],
    ["reach", /^weekly (?:visitors|active users)$/],
    ["engagements", /^weekly contributions$/],
    ["followers", /^(?:followers?|subscribers?|members?|fans|(?:monthly )?listeners|patrons|paid members|backers|supporters|connections|people follow this|followers on \w+)$/],
    ["posts", /^(?:posts?|videos?|tweets|toots|pins|episodes|articles|stories|uploads|repositories|shots|projects|photos|tracks|answers|questions|listings|items|products|threads)$/],
    ["likes", /^(?:likes?|upvotes?|points|hearts|reactions|stars|claps|appreciations|kudos|karma|people like this)$/],
    ["views", /^(?:views|plays|streams|reads|downloads|installs|impressions|monthly views|visits|listens)$/],
    ["replies", /^(?:comments?|replies|reply)$/],
    ["reposts", /^(?:shares|reposts|retweets|boosts|forks)$/],
    ["reviews", /^(?:reviews?|ratings?|reviews? total|customer reviews|global ratings)$/],
  ];
  const NUMBER = String.raw`(\d{1,3}(?:[,.   ]\d{3})+|\d+(?:[.,]\d+)?)\s*([KkMmBb](?![a-z])|mil|tsd\.?|thousand|million)?\+?`;
  const WORDS = String.raw`([a-z]+(?: [a-z]+){0,3})`;
  const END = String.raw`(?=$|[\s.,:;·•|)!(\]])`;
  const AFTER = new RegExp(`(?:^|[^\\w.,])${NUMBER}\\s*${WORDS}${END}`, "gi");
  const BEFORE = new RegExp(`(?:^|[^\\w])${WORDS}\\s*[:·•]?\\s*${NUMBER}${END}`, "gi");
  const RATING = [
    /(\d(?:[.,]\d{1,2})?)\s*(?:out of|\/|of)\s*5(?!\d)/i,
    /\brated\s+(\d(?:[.,]\d{1,2})?)/i,
    /\btrustscore\s+(\d(?:[.,]\d)?)/i,
    /(\d[.,]\d{1,2})\s*(?:★|stars?\b)/i,
  ];

  const clean = (value, limit = 500) => String(value ?? "").replace(/\s+/g, " ").trim().slice(0, limit);

  function count(digits, suffix) {
    const raw = String(digits || "").replace(/[   ]/g, "");
    const scale = { k: 1e3, m: 1e6, b: 1e9, mil: 1e3, tsd: 1e3, "tsd.": 1e3, thousand: 1e3, million: 1e6 }[String(suffix || "").toLowerCase()];
    if (scale) {
      const number = Number.parseFloat(raw.replace(/,(?=\d{1,2}$)/, ".").replace(/,/g, ""));
      return Number.isFinite(number) ? Math.round(number * scale) : null;
    }
    if (/^\d{1,3}(?:[,.]\d{3})+$/.test(raw)) return Number(raw.replace(/[,.]/g, ""));
    const number = Number.parseFloat(raw.replace(",", "."));
    return Number.isFinite(number) ? Math.round(number) : null;
  }

  function labelOf(words, fromEnd) {
    const parts = clean(words, 60).toLowerCase().split(" ").filter(Boolean);
    for (let size = parts.length; size > 0; size -= 1) {
      const value = (fromEnd ? parts.slice(-size) : parts.slice(0, size)).join(" ");
      for (const [key, pattern] of LABELS) if (pattern.test(value)) return key;
    }
    return "";
  }

  function scan(text, found, signals, origin) {
    const value = clean(text, 400);
    if (!value || !/\d/.test(value)) return;
    for (const pattern of [AFTER, BEFORE]) {
      pattern.lastIndex = 0;
      for (const match of value.matchAll(pattern)) {
        const after = pattern === AFTER;
        const key = labelOf(after ? match[3] : match[1], !after);
        if (!key || key in found) continue;
        const number = after ? count(match[1], match[2]) : count(match[2], match[3]);
        if (number === null || number > 1e12) continue;
        found[key] = number;
        signals.push(`${key} ← "${clean(match[0], 50)}" (${origin})`);
      }
    }
  }

  function rating(text) {
    const value = clean(text, 400);
    for (const pattern of RATING) {
      const match = value.match(pattern);
      const number = match ? Number.parseFloat(match[1].replace(",", ".")) : NaN;
      if (number > 0 && number <= 10) return Math.round(number * 100) / 100;
    }
    return 0;
  }

  const meta = (...names) => {
    for (const name of names) {
      const node = document.querySelector(`meta[property="${name}"], meta[name="${name}"]`);
      const value = clean(node?.getAttribute("content"));
      if (value) return value;
    }
    return "";
  };

  function https(value) {
    if (!String(value || "").trim()) return "";
    try {
      const url = new URL(String(value || ""), location.href);
      return url.protocol === "https:" ? url.href.slice(0, 990) : "";
    } catch {
      return "";
    }
  }

  function jsonLd() {
    const nodes = [];
    const walk = (value, depth = 0) => {
      if (!value || typeof value !== "object" || depth > 6 || nodes.length > 400) return;
      if (Array.isArray(value)) { value.forEach((item) => walk(item, depth + 1)); return; }
      nodes.push(value);
      for (const child of Object.values(value)) if (child && typeof child === "object") walk(child, depth + 1);
    };
    for (const script of document.querySelectorAll('script[type="application/ld+json"]')) {
      try { walk(JSON.parse(script.textContent || "")); } catch { }
    }
    return nodes;
  }

  const INTERACTIONS = [["follow", "followers"], ["subscribe", "followers"], ["like", "likes"],
    ["watch", "views"], ["listen", "views"], ["comment", "replies"], ["share", "reposts"], ["write", "posts"]];

  function fromJsonLd(nodes, found, signals, recent) {
    let best = 0;
    for (const node of nodes) {
      const stats = [].concat(node.interactionStatistic || []);
      for (const stat of stats) {
        const type = String(stat?.interactionType?.["@type"] || stat?.interactionType || "").toLowerCase();
        const pair = INTERACTIONS.find(([word]) => type.includes(word));
        const value = Number(stat?.userInteractionCount);
        if (pair && Number.isFinite(value) && value >= 0 && !(pair[1] in found)) {
          found[pair[1]] = Math.round(value);
          signals.push(`${pair[1]} ← structured data`);
        }
      }
      const aggregate = node.aggregateRating;
      if (aggregate && typeof aggregate === "object") {
        const value = Number.parseFloat(String(aggregate.ratingValue || "").replace(",", "."));
        const scale = Number.parseFloat(aggregate.bestRating) || 5;
        if (value > 0 && !best) best = Math.round((scale === 5 ? value : value * 5 / scale) * 100) / 100;
        const reviews = Number(aggregate.reviewCount || aggregate.ratingCount);
        if (Number.isFinite(reviews) && reviews >= 0 && !("reviews" in found)) {
          found.reviews = Math.round(reviews);
          signals.push("reviews ← structured data");
        }
      }
      const published = Date.parse(node.datePublished || node.uploadDate || node.dateCreated || "");
      if (Number.isFinite(published) && (node.url || node.headline || node.name)) {
        recent.push({ url: https(node.url), title: clean(node.headline || node.name, 140), at: published });
      }
    }
    if (best) signals.push("rating ← structured data");
    return best;
  }

  function visibleElements() {
    const list = [];
    const all = document.body ? document.body.getElementsByTagName("*") : [];
    for (let index = 0; index < all.length && list.length < MAX_ELEMENTS; index += 1) {
      const node = all[index];
      const tag = node.tagName;
      if (tag === "SCRIPT" || tag === "STYLE" || tag === "NOSCRIPT" || tag === "TEMPLATE" || tag === "svg") continue;
      list.push(node);
    }
    return list;
  }

  function fromPage(found, signals) {
    let stars = 0;
    for (const node of visibleElements()) {
      for (const attribute of ["aria-label", "title"]) {
        const value = node.getAttribute(attribute);
        if (value && value.length <= 120) {
          scan(value, found, signals, attribute);
          if (!stars) stars = rating(value);
        }
      }
      if (node.childElementCount > 6) continue;
      const text = node.textContent || "";
      if (text.length > MAX_TEXT || !/\d/.test(text)) continue;
      scan(text, found, signals, "page");
      if (!stars) stars = rating(text);
    }
    if (stars) signals.push("rating ← page");
    return stars;
  }

  function timeline(recent) {
    const seen = new Set();
    for (const node of document.querySelectorAll("time[datetime]")) {
      const at = Date.parse(node.getAttribute("datetime") || "");
      if (!Number.isFinite(at) || at > Date.now() + DAY) continue;
      const holder = node.closest("article, li, [role='article'], [data-testid='tweet'], .post, .entry") || node.parentElement;
      const link = node.closest("a[href]") || holder?.querySelector("a[href]");
      const url = https(link?.getAttribute("href"));
      const key = url || `${at}`;
      if (seen.has(key)) continue;
      seen.add(key);
      const heading = holder?.querySelector("h1, h2, h3, [data-testid='tweetText'], p");
      recent.push({ url, title: clean(heading?.textContent || link?.textContent || "", 140), at });
    }
  }

  function handleOf(title) {
    const fromTitle = String(title || "").match(/@([A-Za-z0-9_.-]{2,40})/);
    if (fromTitle) return `@${fromTitle[1]}`;
    const segment = location.pathname.split("/").filter(Boolean)[0] || "";
    return /^@?[A-Za-z0-9_.-]{2,40}$/.test(segment) ? segment : "";
  }

  function feedUrl() {
    const node = document.querySelector('link[rel="alternate"][type*="rss"], link[rel="alternate"][type*="atom"]');
    return https(node?.getAttribute("href"));
  }

  const googleSearch = () => /(^|\.)google(\.[a-z]{2,3}){1,2}$/.test(location.hostname) && location.pathname === "/search";

  function googleResults() {
    const query = clean(new URLSearchParams(location.search).get("q"), 200);
    const stats = clean(document.getElementById("result-stats")?.textContent, 200);
    const match = stats.match(new RegExp(NUMBER));
    const found = {};
    const signals = [];
    if (match) {
      found.indexedPages = count(match[1], "");
      signals.push(`indexedPages ← "${clean(stats, 50)}" (result count)`);
    } else if (document.getElementById("search") && !document.getElementById("pnnext")) {
      const links = new Set();
      for (const heading of document.querySelectorAll("#search a[href] h3")) links.add(heading.closest("a").href);
      found.indexedPages = links.size;
      signals.push(`indexedPages ← ${links.size} results on the only page`);
    }
    return {
      url: https(location.href), canonical: "", title: query, description: stats, image: "",
      handle: "", feedUrl: "", metrics: found, rating: 0, lastPostAt: 0, postsLast30d: 0,
      recent: [], signals, signInNeeded: false, readyState: document.readyState,
    };
  }

  const REDDIT_HOST = /^(?:www\.|old\.|new\.)?reddit\.com$/;
  const REDDIT_ATTRIBUTES = [["followers", /^subscribers$/], ["reach", /^weekly-(?:active-users|visitors|unique-users)$/],
    ["engagements", /^weekly-contributions$/]];

  function subredditOf() {
    if (!REDDIT_HOST.test(location.hostname)) return "";
    const match = location.pathname.match(/^\/r\/([A-Za-z0-9_]{2,21})(?:\/|$)/);
    return match ? match[1] : "";
  }

  function subredditCounts(found, signals) {
    const header = document.querySelector("shreddit-subreddit-header");
    for (const name of header?.getAttributeNames?.() || []) {
      const pair = REDDIT_ATTRIBUTES.find(([, pattern]) => pattern.test(name));
      const number = pair ? count(header.getAttribute(name), "") : null;
      if (!pair || pair[0] in found || number === null) continue;
      found[pair[0]] = number;
      signals.push(`${pair[0]} ← ${name} (subreddit header)`);
    }
    if (header) {
      for (const node of [header, ...(header.querySelectorAll?.("*") || [])]) {
        const text = node.textContent || "";
        if (node !== header && text.length <= MAX_TEXT) scan(text, found, signals, "subreddit header");
      }
    }
    const old = document.querySelector(".side .subscribers .number");
    const members = old ? count(clean(old.textContent, 40), "") : null;
    if (members !== null && !("followers" in found)) {
      found.followers = members;
      signals.push("followers ← readers (old Reddit sidebar)");
    }
  }

  function subredditPage(name) {
    const found = {};
    const signals = [];
    const recent = [];
    subredditCounts(found, signals);
    timeline(recent);
    const now = Date.now();
    const dated = recent.filter((item) => item.at > 0 && item.at <= now + DAY).sort((a, b) => b.at - a.at);
    return {
      url: https(location.href),
      canonical: https(document.querySelector('link[rel="canonical"]')?.getAttribute("href")),
      title: `r/${name}`,
      description: clean(meta("og:description", "description"), 500),
      image: https(meta("og:image")),
      handle: `r/${name}`,
      feedUrl: `https://www.reddit.com/r/${name}/.rss`,
      metrics: found, rating: 0,
      lastPostAt: dated[0]?.at || 0,
      postsLast30d: dated.filter((item) => item.at >= now - 30 * DAY).length,
      recent: dated.slice(0, 5),
      signals: signals.slice(0, 12),
      signInNeeded: false,
      readyState: document.readyState,
    };
  }

  globalThis.__forkmeshMarketingRead = () => {
    if (googleSearch()) return googleResults();
    const subreddit = subredditOf();
    if (subreddit) return subredditPage(subreddit);
    const found = {};
    const signals = [];
    const recent = [];
    let stars = fromJsonLd(jsonLd(), found, signals, recent);
    for (const [origin, value] of [["description", meta("og:description", "twitter:description", "description")],
      ["title", meta("og:title", "twitter:title") || document.title]]) {
      scan(value, found, signals, origin);
      if (!stars) stars = rating(value);
    }
    const pageStars = fromPage(found, signals);
    if (!stars) stars = pageStars;
    timeline(recent);
    const now = Date.now();
    const dated = recent.filter((item) => item.at > 0 && item.at <= now + DAY).sort((a, b) => b.at - a.at);
    const unique = [];
    const keys = new Set();
    for (const item of dated) {
      const key = item.url || `${item.at}:${item.title}`;
      if (keys.has(key)) continue;
      keys.add(key);
      unique.push(item);
    }
    const title = meta("og:title", "twitter:title") || clean(document.title, 200);
    const canonical = https(document.querySelector('link[rel="canonical"]')?.getAttribute("href"));
    const password = [...document.querySelectorAll('input[type="password"]')].some((input) => input.offsetParent !== null);
    return {
      url: https(location.href),
      canonical,
      title: clean(title, 200),
      description: clean(meta("og:description", "twitter:description", "description"), 500),
      image: https(meta("og:image", "twitter:image")),
      handle: handleOf(title),
      feedUrl: feedUrl(),
      metrics: found,
      rating: stars,
      lastPostAt: unique[0]?.at || 0,
      postsLast30d: unique.filter((item) => item.at >= now - 30 * DAY).length,
      recent: unique.slice(0, 5),
      signals: signals.slice(0, 12),
      signInNeeded: password && !Object.keys(found).length,
      readyState: document.readyState,
    };
  };
})();
