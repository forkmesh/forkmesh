(() => {
  const ORIGIN_MAX = 200;

  function cleanOrigin(value) {
    try {
      const url = new URL(String(value || ""));
      if (url.protocol !== "https:" || url.username || url.password || !url.hostname.includes(".")) return "";
      const path = url.pathname.replace(/\/+$/, "");
      const origin = `${url.origin}${/^(\/[A-Za-z0-9._~-]+){0,4}$/.test(path) ? path : ""}`;
      return origin.length <= ORIGIN_MAX ? origin : "";
    } catch (_) {
      return "";
    }
  }

  function readForum() {
    const generator = document.querySelector('meta[name="generator"]')?.content || "";
    if (!/^Discourse\b/.test(generator)) return null;
    const title = document.querySelector('meta[property="og:site_name"]')?.content || document.title || "";
    return { origin: location.origin, title: String(title).slice(0, 200) };
  }

  function install({ settings, handlers }) {
    async function forumDetect({ tabId }) {
      if (!Number.isInteger(tabId)) return { ok: true, forum: null };
      let results = [];
      try {
        results = await chrome.scripting.executeScript({ target: { tabId }, func: readForum });
      } catch (_) {
        return { ok: true, forum: null };
      }
      const found = results?.[0]?.result;
      const origin = cleanOrigin(found?.origin);
      if (!origin) return { ok: true, forum: null };
      return { ok: true, forum: { origin, title: String(found.title || "").trim().slice(0, 200) } };
    }

    async function forumMirror({ origin }) {
      const forum = cleanOrigin(origin);
      if (!forum) return { ok: false, error: "invalid_forum" };
      const { baseUrl } = await settings();
      await chrome.tabs.create({ url: `${baseUrl}/start-discourse-mirror?forum=${encodeURIComponent(forum)}` });
      return { ok: true };
    }

    Object.assign(handlers, {
      "fm-forum-detect": forumDetect,
      "fm-forum-mirror": forumMirror,
    });
  }

  self.ForkMeshForumBackground = { install, cleanOrigin, readForum };
})();
