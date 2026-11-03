(() => {
  if (window.__forkmeshTootSent) return;
  window.__forkmeshTootSent = true;

  const SENT = /^\/api\/v1\/statuses\/?$/;

  function sending(method, url) {
    if (String(method || "GET").toUpperCase() !== "POST") return false;
    try {
      const target = new URL(String(url || ""), location.href);
      return target.origin === location.origin && SENT.test(target.pathname);
    } catch {
      return false;
    }
  }

  function report(data) {
    let status = data;
    if (typeof status === "string") {
      try { status = JSON.parse(status); } catch { return; }
    }
    if (!status || typeof status !== "object" || !status.account) return;
    window.postMessage({
      source: "forkmesh-toot-sent",
      status: {
        id: String(status.id || ""),
        url: String(status.url || ""),
        created_at: String(status.created_at || ""),
        in_reply_to_id: String(status.in_reply_to_id || ""),
        spoiler_text: String(status.spoiler_text || ""),
        content: String(status.content || ""),
        account: { acct: String(status.account.acct || "") },
        mentions: (Array.isArray(status.mentions) ? status.mentions : [])
          .slice(0, 20).map((mention) => ({ acct: String(mention?.acct || "") })),
      },
    }, location.origin);
  }

  const open = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    if (sending(method, url)) {
      this.addEventListener("load", () => {
        if (this.status < 200 || this.status >= 300) return;
        report(this.responseType === "" || this.responseType === "text" ? this.responseText : this.response);
      });
    }
    return open.call(this, method, url, ...rest);
  };

  const originalFetch = window.fetch;
  if (typeof originalFetch === "function") {
    window.fetch = function (input, init) {
      const result = originalFetch.apply(this, arguments);
      const method = init?.method || (input instanceof Request ? input.method : "GET");
      const url = input instanceof Request ? input.url : input;
      if (sending(method, url)) {
        result.then((response) => (response.ok ? response.clone().json() : null))
          .then((status) => { if (status) report(status); }).catch(() => {});
      }
      return result;
    };
  }
})();
