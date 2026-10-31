(() => {
  const ID_RE = /^[0-9a-f]{32}$/;
  const TEXT_RE = /(https:\/\/[^\s<>"']+[^\s<>"'.,;:!?)\]])|(^|[^A-Za-z0-9_@/])@([A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)\b/g;

  function el(tag, attrs, children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
      if (value === null || value === undefined || value === false) continue;
      if (key === "text") node.textContent = value;
      else node.setAttribute(key, value === true ? "" : String(value));
    }
    for (const child of [].concat(children || [])) {
      if (child === null || child === undefined || child === false) continue;
      node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
  }

  function postHref(id) {
    return "/feed?post=" + encodeURIComponent(id);
  }

  function profileHref(name) {
    return "/@" + encodeURIComponent(name);
  }

  function richText(text) {
    const value = String(text || "");
    const nodes = [];
    let last = 0;
    for (const match of value.matchAll(TEXT_RE)) {
      nodes.push(value.slice(last, match.index));
      if (match[1]) {
        nodes.push(el("a", { href: match[1], rel: "noopener noreferrer nofollow ugc", target: "_blank", text: match[1] }));
      } else {
        nodes.push(match[2]);
        nodes.push(el("a", { href: profileHref(match[3].toLowerCase()), text: "@" + match[3] }));
      }
      last = match.index + match[0].length;
    }
    nodes.push(value.slice(last));
    return nodes;
  }

  function plural(count, one, many) {
    return count + " " + (count === 1 ? one : many);
  }

  function card(post, focused) {
    const name = post.authorName || post.author;
    const time = post.createdAt ? new Date(post.createdAt) : null;
    const when = time
      ? el("time", { datetime: time.toISOString(), text: time.toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" }) })
      : null;
    const body = el("div", { class: "fp-text" }, richText(post.text));
    const content = [];
    if (post.spoiler) {
      const toggle = el("button", { type: "button", class: "secondary-button fp-reveal", "aria-expanded": "false", text: "Show more" });
      body.hidden = true;
      toggle.addEventListener("click", () => {
        body.hidden = !body.hidden;
        toggle.textContent = body.hidden ? "Show more" : "Show less";
        toggle.setAttribute("aria-expanded", body.hidden ? "false" : "true");
      });
      content.push(el("p", { class: "fp-spoiler" }, [post.spoiler, " ", toggle]));
    }
    content.push(body);
    const counts = [
      post.replies ? plural(post.replies, "reply", "replies") : "",
      post.boosts ? plural(post.boosts, "boost", "boosts") : "",
      post.favourites ? plural(post.favourites, "favourite", "favourites") : "",
    ].filter(Boolean).join(" · ");
    return el("article", { class: "fp-post" + (focused ? " is-focused" : "") }, [
      el("a", { class: "fp-avatar", href: profileHref(post.author), "aria-hidden": "true", tabindex: "-1", text: String(name || "?").slice(0, 1) }),
      el("div", { class: "fp-main" }, [
        el("p", { class: "fp-byline" }, [
          el("a", { class: "fp-name", href: profileHref(post.author), text: name }),
          " ",
          el("span", { class: "fp-handle", text: "@" + post.author }),
          when ? (focused ? el("span", { class: "fp-time" }, [when]) : el("a", { class: "fp-time", href: postHref(post.id) }, [when])) : null,
        ]),
        post.replyTo ? el("p", { class: "fp-meta" }, ["Replying to ", post.replyToAuthor ? el("a", { href: profileHref(post.replyToAuthor), text: "@" + post.replyToAuthor }) : "a removed post"]) : null,
        ...content,
        counts ? el("p", { class: "fp-meta", text: counts }) : null,
      ]),
    ]);
  }

  function status(text) {
    const node = document.querySelector("[data-fp-status]");
    node.textContent = text;
    node.hidden = !text;
  }

  async function copyLink(button) {
    const url = new URL(location.href);
    url.hash = "";
    try {
      await navigator.clipboard.writeText(url.href);
      button.textContent = "Copied";
    } catch (_) {
      button.textContent = "Copy failed";
    }
    setTimeout(() => { button.textContent = "Copy link"; }, 1500);
  }

  async function load() {
    const id = String(new URLSearchParams(location.search).get("post") || "");
    if (!ID_RE.test(id)) {
      status("Open a link to a Feed post to read it here. Posts are written by people on ForkMesh in the Feed package.");
      return;
    }
    let payload = null;
    try {
      const response = await fetch("/api/instance-feed/public/" + id, { headers: { accept: "application/json" }, credentials: "omit" });
      if (response.status === 404) return status("This post was deleted, or the link is wrong.");
      payload = await response.json();
      if (!response.ok || !payload?.post) throw new Error("HTTP " + response.status);
    } catch (_) {
      return status("Could not load the post. Try again in a moment.");
    }
    const post = payload.post;
    const thread = document.querySelector("[data-fp-thread]");
    thread.replaceChildren(
      ...(payload.ancestors || []).map((item) => card(item, false)),
      card(post, true),
      ...(payload.replies || []).map((item) => card(item, false)),
    );
    thread.hidden = false;
    status("");
    const excerpt = post.spoiler || String(post.text || "").replace(/\s+/g, " ").slice(0, 60);
    document.title = (post.authorName || post.author) + ": “" + excerpt + "” · ForkMesh";
    const actions = document.querySelector("[data-fp-actions]");
    actions.querySelector("[data-fp-open]").href = "/dashboard/instance-feed?post=" + encodeURIComponent(post.id);
    const copy = actions.querySelector("[data-fp-copy]");
    copy.addEventListener("click", () => void copyLink(copy));
    actions.hidden = false;
  }

  void load();
})();
