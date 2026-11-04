(() => {
  if (window.__forkmeshXSent) return;
  window.__forkmeshXSent = true;

  const POST = /^\/i\/api\/graphql\/[A-Za-z0-9_-]+\/(?:CreateTweet|CreateNoteTweet)\/?$/;
  const DM = /^\/i\/api\/1\.1\/dm\/new2?\.json$/;
  const PEOPLE = 20;

  function kindOf(method, url) {
    if (String(method || "GET").toUpperCase() !== "POST") return "";
    try {
      const target = new URL(String(url || ""), location.href);
      if (target.origin !== location.origin) return "";
      return POST.test(target.pathname) ? "post" : DM.test(target.pathname) ? "dm" : "";
    } catch {
      return "";
    }
  }

  const str = (value) => String(value ?? "");

  function screenName(user) {
    return str(user?.core?.screen_name || user?.legacy?.screen_name || user?.screen_name);
  }

  function postOf(data) {
    let result = data?.data?.create_tweet?.tweet_results?.result
      || data?.data?.notetweet_create?.tweet_results?.result;
    if (result?.tweet) result = result.tweet;
    const legacy = result?.legacy;
    if (!legacy) return null;
    const people = [];
    if (legacy.in_reply_to_screen_name) people.push({ handle: str(legacy.in_reply_to_screen_name), name: "" });
    for (const mention of Array.isArray(legacy.entities?.user_mentions) ? legacy.entities.user_mentions : []) {
      people.push({ handle: str(mention?.screen_name), name: str(mention?.name) });
    }
    return {
      kind: "post",
      id: str(result.rest_id || legacy.id_str),
      author: { handle: screenName(result.core?.user_results?.result) },
      people: people.slice(0, PEOPLE),
      text: str(result.note_tweet?.note_tweet_results?.result?.text || legacy.full_text),
      createdAt: str(legacy.created_at),
      conversationId: str(legacy.conversation_id_str),
      inReplyTo: str(legacy.in_reply_to_status_id_str),
    };
  }

  function dmOf(data) {
    const entry = (Array.isArray(data?.entries) ? data.entries : []).find((item) => item?.message?.message_data);
    const message = entry?.message;
    const body = message?.message_data;
    if (!body) return null;
    const users = data.users && typeof data.users === "object" ? data.users : {};
    const sender = str(body.sender_id);
    const ids = body.recipient_id ? [str(body.recipient_id)] : Object.keys(users).filter((id) => id !== sender);
    return {
      kind: "dm",
      id: str(body.id || message.id),
      author: { handle: screenName(users[sender]) },
      people: ids.slice(0, PEOPLE).map((id) => ({ handle: screenName(users[id]), name: str(users[id]?.name) })),
      text: str(body.text),
      createdAt: str(body.time || message.time),
      conversationId: str(message.conversation_id),
      inReplyTo: "",
    };
  }

  function report(kind, data) {
    let parsed = data;
    if (typeof parsed === "string") {
      try { parsed = JSON.parse(parsed); } catch { return; }
    }
    if (!parsed || typeof parsed !== "object") return;
    const sent = kind === "dm" ? dmOf(parsed) : postOf(parsed);
    if (sent?.id && sent.author.handle) window.postMessage({ source: "forkmesh-x-sent", sent }, location.origin);
  }

  const open = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (method, url, ...rest) {
    const kind = kindOf(method, url);
    if (kind) {
      this.addEventListener("load", () => {
        if (this.status < 200 || this.status >= 300) return;
        try {
          report(kind, this.responseType === "" || this.responseType === "text" ? this.responseText : this.response);
        } catch { }
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
      const kind = kindOf(method, url);
      if (kind) {
        result.then((response) => (response.ok ? response.clone().json() : null))
          .then((data) => { if (data) report(kind, data); }).catch(() => {});
      }
      return result;
    };
  }
})();
