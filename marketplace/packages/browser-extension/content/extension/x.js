// ForkMesh CRM on X: the organization's own profile, read for Marketing.
//
// This script only looks. It adds nothing to the page, sends no request to X,
// writes nothing to the page's console, and stays idle everywhere except the
// tracked profile (x.com/forkmesh unless the popup names another), a search
// for that handle, and the notifications' Mentions tab. There it reads what the
// page already shows: the header's following, followers, posts and media
// counts, the text, images, replies, reposts, likes and views of the profile's
// own posts, and the posts that mention the handle. background.js keeps the numbers and
// records a Marketing snapshot for the X channel. Anything that goes wrong is
// handed to the extension's popup, which is the only place it is shown; no
// error is ever thrown or logged here.
(() => {
  if (window.__forkmeshXReader) return;
  window.__forkmeshXReader = true;

  const TICK_MS = 3000;
  let handle = "";
  let lastSent = "";
  let reportedHref = "";
  let lastError = "";
  let stopped = false;

  const text = (node) => String(node?.textContent || "").replace(/\s+/g, " ").trim();

  // Messages to the service worker. After the extension is reloaded this
  // copy of the script is orphaned: sending then throws, so it stops quietly.
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
    void send({ type: "fm-x-error", message, page: location.pathname });
  }

  // "1,234", "1.2K", "3,4 M", "12M": X abbreviates past ten thousand.
  function count(value) {
    const match = String(value || "").replace(/[  ]/g, " ")
      .match(/(\d[\d.,]*(?: \d{3})*)\s*([KMB])?(?!\p{L})/iu);
    if (!match) return null;
    const scale = { k: 1e3, m: 1e6, b: 1e9 }[String(match[2] || "").toLowerCase()];
    if (scale) {
      const number = Number.parseFloat(match[1].replace(/ /g, "").replace(",", "."));
      return Number.isFinite(number) ? Math.round(number * scale) : null;
    }
    const digits = match[1].replace(/\D/g, "");
    return digits ? Number(digits) : null;
  }

  // The first path segment names the account: /forkmesh, /forkmesh/media.
  const pathHandle = (path) => String(path || "").split(/[/?#]/)[1]?.toLowerCase() || "";

  function page() {
    const path = location.pathname.toLowerCase();
    if (path === "/notifications/mentions") return "mentions";
    if (path === "/search") {
      const query = new URLSearchParams(location.search).get("q") || "";
      return query.toLowerCase().includes(`@${handle}`) ? "search" : "";
    }
    if (pathHandle(path) !== handle) return "";
    const tab = path.split("/")[2] || "";
    if (!tab || tab === "with_replies") return "profile";
    if (tab === "media") return "media";
    if (tab === "following" || tab === "followers" || tab === "verified_followers") return "profile";
    return "";
  }

  // The header links: "123 Following", "4,567 Followers".
  function profileLink(suffixes) {
    for (const link of document.querySelectorAll(`a[href^="/"]`)) {
      const href = String(link.getAttribute("href") || "").toLowerCase();
      if (pathHandle(href) !== handle) continue;
      if (suffixes.some((suffix) => href === `/${handle}/${suffix}`)) return count(text(link));
    }
    return null;
  }

  // The bar over the timeline: "1,234 posts" on the profile, "56 photos &
  // videos" on its Media tab.
  function headerCount(pattern) {
    const heading = document.querySelector(`[data-testid="primaryColumn"] h2[role="heading"]`);
    const bar = heading?.parentElement;
    for (const node of bar ? bar.querySelectorAll("div, span") : []) {
      const value = text(node);
      if (value.length < 40 && pattern.test(value)) return count(value);
    }
    for (const node of document.querySelectorAll("main div, main span")) {
      if (node.children.length || node.closest("article")) continue;
      const value = text(node);
      if (value.length < 40 && pattern.test(value)) return count(value);
    }
    return null;
  }

  const quoted = (article, node) => {
    const box = node.closest(`div[role="link"]`);
    return node.closest("article") !== article || Boolean(box && article.contains(box));
  };

  const own = (article, selector) =>
    [...article.querySelectorAll(selector)].find((node) => !quoted(article, node)) || null;

  // "12 Replies. Reply": the accessible label carries the exact number.
  const labelled = (node) => count(node?.getAttribute("aria-label")) || count(text(node)) || 0;

  const SNOWFLAKE_EPOCH = 1288834974657;

  function snowflakeTime(id) {
    const at = Number(BigInt(id) >> 22n) + SNOWFLAKE_EPOCH;
    return at > SNOWFLAKE_EPOCH && at < Date.now() + 86400000 ? at : 0;
  }

  function statusOf(article) {
    const link = own(article, `a[href*="/status/"] time`)?.closest("a") || own(article, `a[href*="/status/"]`);
    const match = String(link?.getAttribute("href") || "").match(/^\/([^/]+)\/status\/(\d{5,25})/);
    if (!match) return null;
    const stamp = Date.parse(own(article, "time[datetime]")?.getAttribute("datetime") || "");
    return { author: match[1].toLowerCase(), id: match[2], at: Number.isFinite(stamp) ? stamp : snowflakeTime(match[2]) };
  }

  function postText(article) {
    const node = own(article, article.matches(`[data-testid="tweet"]`) ? `[data-testid="tweetText"]` : `div[dir="auto"]`);
    if (!node) return "";
    const copy = node.cloneNode(true);
    for (const button of copy.querySelectorAll("button")) button.remove();
    return text(copy);
  }

  const IMAGE_PATHS = /^\/(media|ext_tw_video_thumb|amplify_video_thumb|tweet_video_thumb|card_img)\//;

  function imageUrl(value) {
    try {
      const url = new URL(String(value || ""), location.href);
      if (url.protocol !== "https:" || url.hostname !== "pbs.twimg.com" || !IMAGE_PATHS.test(url.pathname)) return "";
      if (url.pathname.startsWith("/media/")) url.searchParams.set("name", "small");
      return url.href;
    } catch {
      return "";
    }
  }

  function imagesOf(article) {
    const found = [];
    for (const node of article.querySelectorAll("img[src], video[poster]")) {
      if (quoted(article, node)) continue;
      const url = imageUrl(node.tagName === "VIDEO" ? node.getAttribute("poster") : node.getAttribute("src"));
      if (url && !found.includes(url)) found.push(url);
      if (found.length >= 4) break;
    }
    return found;
  }

  function timeline(kind) {
    const posts = [];
    const mentions = [];
    for (const article of document.querySelectorAll(`article[data-testid="tweet"], article:not([data-testid])`)) {
      if (article.parentElement?.closest("article")) continue;
      const status = statusOf(article);
      if (!status) continue;
      if (status.author === handle) {
        if (kind !== "profile" && kind !== "media") continue;
        posts.push({
          id: status.id, at: status.at,
          text: postText(article).slice(0, 280),
          images: imagesOf(article),
          replies: labelled(own(article, `[data-testid="reply"], [data-engagement-action="reply"]`)),
          reposts: labelled(own(article, `[data-testid="retweet"], [data-testid="unretweet"], [data-engagement-action="retweet"]`)),
          likes: labelled(own(article, `[data-testid="like"], [data-testid="unlike"], [data-engagement-action="like"]`)),
          views: labelled(own(article, `a[href$="/analytics"], [data-engagement-action="generic"]`)),
        });
      } else if (kind === "mentions"
        || (kind === "search" && postText(article).toLowerCase().includes(`@${handle}`))) {
        mentions.push({ id: status.id, at: status.at });
      }
    }
    return { posts, mentions };
  }

  function observe() {
    const kind = page();
    if (!kind) return null;
    const observation = { handle, page: kind, profile: {}, ...timeline(kind) };
    if (kind === "profile") {
      observation.profile.following = profileLink(["following"]);
      observation.profile.followers = profileLink(["verified_followers", "followers"]);
      observation.profile.posts = headerCount(/^\d[\d.,\s]*[KMB]?\s+posts?$/i);
    } else if (kind === "media") {
      observation.profile.media = headerCount(/^\d[\d.,\s]*[KMB]?\s+photos?\s*(&|and)\s*videos?$/i);
    }
    for (const [key, value] of Object.entries(observation.profile)) {
      if (value === null) delete observation.profile[key];
    }
    return observation;
  }

  async function tick() {
    if (stopped) return;
    try {
      if (!handle) {
        const config = await send({ type: "fm-x-handle" });
        handle = String(config?.handle || "").toLowerCase();
      }
      if (handle && document.visibilityState === "visible") {
        const observation = observe();
        const empty = !observation || (!Object.keys(observation.profile).length
          && !observation.posts.length && !observation.mentions.length);
        const key = empty ? "" : JSON.stringify(observation);
        if (key && key !== lastSent) {
          lastSent = key;
          const fresh = location.href !== reportedHref;
          reportedHref = location.href;
          const result = await send({ type: "fm-x-observe", observation, fresh });
          if (result?.handle) handle = String(result.handle).toLowerCase();
        }
      }
    } catch (error) {
      report(error);
    }
    if (!stopped) setTimeout(() => void tick(), TICK_MS);
  }

  void tick();
})();
