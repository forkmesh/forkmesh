(() => {
  // Where a post was announced on the social networks we run. Each blog post
  // carries its own permalinks as data attributes on a placeholder element:
  //
  //   <section data-blog-social
  //            data-reddit=""
  //            data-mastodon=""
  //            data-twitter=""></section>
  //
  // Those attributes are the checked-in default. A platform administrator can
  // fill the same three slots straight from the rendered page — the values are
  // stored per post by the Worker and win over the attributes — so announcing
  // a post no longer needs a deploy. The ones still blank render as an
  // explicit empty slot rather than a silent gap, so an unannounced post reads
  // as unannounced.
  const NETWORKS = [
    { key: "reddit", label: "Reddit" },
    { key: "mastodon", label: "Mastodon" },
    { key: "twitter", label: "X (Twitter)" },
  ];

  const EMPTY_LABEL = "Not posted yet";
  const MASTODON_LIMIT = 500;
  const TWITTER_LIMIT = 250;
  // Mastodon indexes posts by hashtag, so its draft carries the wider tag set
  // that the people who browse those tags actually follow. X charges the same
  // tags against a 250-character draft, so it keeps the two that matter.
  const MASTODON_TAGS =
    "#ForkMesh #OpenSource #FOSS #DevTools #Git #SelfHosted " +
    "#Decentralized #P2P #DevOps #Programming";
  const TWITTER_TAGS = "#ForkMesh #OpenSource";
  const SHORT_LABELS = { reddit: "Reddit", mastodon: "Mastodon", twitter: "X" };
  const ICONS = {
    reddit: '<circle cx="12" cy="14.5" r="6.5"></circle><circle cx="5.4" cy="10.9" r="1.5"></circle><circle cx="18.6" cy="10.9" r="1.5"></circle><path d="m12 8 1.2-4.1 3.5.9"></path><circle cx="18.2" cy="5.3" r="1.3"></circle><circle cx="9.5" cy="14" r="0.6"></circle><circle cx="14.5" cy="14" r="0.6"></circle><path d="M9.4 17c1.6 1 3.6 1 5.2 0"></path>',
    mastodon: '<path d="M20 9.3C20 5.5 17.3 4 12 4S4 5.5 4 9.3v3C4 17 6.8 18.7 11 18.7c1.7 0 3.1-.3 4.3-.8"></path><path d="M8.2 13.6v-3.5a1.9 1.9 0 0 1 3.8 0v2m0-2a1.9 1.9 0 0 1 3.8 0v3.5"></path>',
    twitter: '<path d="M4.5 4h4.3l10.7 16h-4.3Z"></path><path d="m19.5 4-6.4 7.1M4.5 20l6.4-7.1"></path>',
    link: '<path d="M10 14a3.5 3.5 0 0 0 5 0l3-3a3.5 3.5 0 0 0-5-5l-1 1"></path><path d="M14 10a3.5 3.5 0 0 0-5 0l-3 3a3.5 3.5 0 0 0 5 5l1-1"></path>',
    copy: '<rect x="9" y="9" width="11" height="11" rx="2"></rect><path d="M5 15V6a2 2 0 0 1 2-2h8"></path>',
    check: '<path d="m5 12.5 4.5 4.5L19 7.5"></path>',
    external: '<path d="M7 17 17 7M8.5 7H17v8.5"></path>',
    chevron: '<path d="m6 9 6 6 6-6"></path>',
  };

  function svgIcon(name, className = "blog-social-glyph") {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("class", className);
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.8");
    svg.setAttribute("stroke-linecap", "round");
    svg.setAttribute("stroke-linejoin", "round");
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    svg.innerHTML = ICONS[name] || "";
    return svg;
  }

  function iconTile(name) {
    const tile = document.createElement("span");
    tile.className = "blog-social-icon";
    tile.setAttribute("aria-hidden", "true");
    tile.append(svgIcon(name, "blog-social-icon-svg"));
    return tile;
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch (error) {
      const field = document.createElement("textarea");
      field.value = text;
      field.setAttribute("readonly", "");
      field.style.position = "fixed";
      field.style.opacity = "0";
      document.body.append(field);
      field.select();
      let copied = false;
      try {
        copied = document.execCommand("copy");
      } catch (_) {
        copied = false;
      }
      field.remove();
      return copied;
    }
  }

  function copyButton(label, text, className) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = className;
    const glyph = svgIcon("copy");
    const caption = document.createElement("span");
    caption.textContent = label;
    button.append(glyph, caption);
    let timer = 0;
    button.addEventListener("click", async () => {
      const copied = await copyText(text);
      glyph.innerHTML = copied ? ICONS.check : ICONS.copy;
      caption.textContent = copied ? "Copied" : "Copy failed";
      button.dataset.state = copied ? "copied" : "error";
      clearTimeout(timer);
      timer = setTimeout(() => {
        glyph.innerHTML = ICONS.copy;
        caption.textContent = label;
        delete button.dataset.state;
      }, 1800);
    });
    return button;
  }

  // Only real web permalinks become links: a blank value and anything that is
  // not http(s) (a stray note, a javascript: URL) both read as empty.
  function permalink(value) {
    const raw = String(value || "").trim();
    return /^https?:\/\//i.test(raw) ? raw : "";
  }

  function attributeLinks(host) {
    const links = {};
    NETWORKS.forEach((network) => {
      const url = permalink(host.dataset[network.key]);
      if (url) links[network.key] = url;
    });
    return links;
  }

  // Show the permalink itself rather than a generic "View post", trimmed to
  // the part that identifies it: forkmesh.com's own network handles are the
  // point of the row.
  function linkText(url) {
    try {
      const parsed = new URL(url);
      const path = parsed.pathname.replace(/\/+$/, "");
      return parsed.host.replace(/^www\./, "") + path;
    } catch (error) {
      return url;
    }
  }

  function storedSession() {
    try {
      return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (error) {
      return null;
    }
  }

  // The editable fields are a convenience for the person already signed in as
  // an administrator; the Worker re-checks is_admin on every write, so this
  // only decides what to draw.
  function isAdminSession() {
    const session = storedSession();
    return Boolean(session && session.isAdmin && session.nodeName);
  }

  function authHeaders() {
    const token = String(storedSession()?.sessionToken || "");
    return token && token !== "cookie"
      ? { authorization: `Bearer ${token}` }
      : {};
  }

  function plural(count, one, many) {
    return `${count.toLocaleString()} ${count === 1 ? one : many}`;
  }

  // A network nobody pasted a permalink for, but which the referrer board
  // shows is sending readers here: the post was announced there even though
  // the slot is blank, so the slot says so instead of "Not posted yet".
  function detectedFor(state, key) {
    const entry = state.detected[key];
    const url = permalink(entry && entry.url);
    const host = String((entry && entry.host) || "");
    if (!url || !host) return null;
    return { url, host, visits: Number((entry && entry.visits) || 0) };
  }

  function detectedNote(detected) {
    const note = document.createElement("span");
    note.className = "blog-social-detected-note";
    note.textContent =
      `Detected from ${plural(detected.visits, "visit", "visits")}`;
    return note;
  }

  function renderItem(state, network) {
    const item = document.createElement("li");
    item.className = "blog-social-item";
    item.dataset.network = network.key;

    const text = document.createElement("span");
    text.className = "blog-social-item-text";
    const label = document.createElement("span");
    label.className = "blog-social-label";
    label.textContent = network.label;
    text.append(label);
    item.append(iconTile(network.key), text);

    const url = permalink(state.links[network.key]);
    const detected = url ? null : detectedFor(state, network.key);
    if (state.admin) {
      const input = document.createElement("input");
      input.type = "url";
      input.className = "blog-social-input";
      input.name = network.key;
      input.value = url;
      // The referrer already names where the post landed, so an admin filling
      // the slot in is confirming a source we found rather than recalling it.
      input.placeholder = detected
        ? `Seen on ${detected.host} — paste the permalink`
        : EMPTY_LABEL;
      input.autocomplete = "off";
      input.spellcheck = false;
      input.setAttribute("aria-label", `${network.label} permalink`);
      text.append(input);
      if (detected) {
        item.classList.add("is-detected");
        text.append(detectedNote(detected));
      } else if (!url) {
        item.classList.add("is-empty");
      }
      return item;
    }

    if (detected) {
      const link = document.createElement("a");
      link.className = "blog-social-link";
      link.href = detected.url;
      link.rel = "noopener noreferrer";
      link.target = "_blank";
      link.textContent = detected.host;
      item.classList.add("is-detected");
      text.append(link, detectedNote(detected));
      return item;
    }

    if (!url) {
      const empty = document.createElement("span");
      empty.className = "blog-social-empty";
      empty.textContent = EMPTY_LABEL;
      text.append(empty);
      item.classList.add("is-empty");
      return item;
    }

    const link = document.createElement("a");
    link.className = "blog-social-link";
    link.href = url;
    link.rel = "noopener noreferrer me";
    link.target = "_blank";
    link.textContent = linkText(url);
    text.append(link);
    return item;
  }

  function cleanText(value) {
    return String(value || "").replace(/\s+/g, " ").trim();
  }

  function clipped(value, limit) {
    const text = String(value || "");
    if (text.length <= limit) return text;
    return text.slice(0, Math.max(0, limit - 1)).trimEnd() + "…";
  }

  // Reddit accepts Markdown in text posts. Preserve the useful structure from
  // the article instead of flattening every heading and paragraph into one
  // hard-to-scan block of prose.
  function markdownInline(node) {
    if (node.nodeType === Node.TEXT_NODE) {
      return String(node.nodeValue || "").replace(/\s+/g, " ");
    }
    if (node.nodeType !== Node.ELEMENT_NODE) return "";

    const tag = node.tagName.toLowerCase();
    if (tag === "br") return "\n";
    const content = markdownInlineNodes(node.childNodes);
    if (!content) return "";
    if (tag === "code") return "`" + content.replace(/`/g, "\\`") + "`";
    if (tag === "strong" || tag === "b") return `**${content}**`;
    if (tag === "em" || tag === "i") return `*${content}*`;
    if (tag === "del" || tag === "s" || tag === "strike") {
      return `~~${content}~~`;
    }
    if (tag === "a") {
      const href = permalink(node.href);
      return href ? `[${content}](${href})` : content;
    }
    return content;
  }

  function markdownInlineNodes(nodes) {
    return Array.from(nodes).map(markdownInline).join("")
      .replace(/[ \t]+\n/g, "\n")
      .replace(/\n[ \t]+/g, "\n")
      .replace(/[ \t]{2,}/g, " ")
      .trim();
  }

  function markdownList(list, depth = 0) {
    const ordered = list.tagName.toLowerCase() === "ol";
    const items = Array.from(list.children).filter(
      (child) => child.tagName && child.tagName.toLowerCase() === "li",
    );
    return items.map((item, index) => {
      const inlineNodes = Array.from(item.childNodes).filter((child) =>
        child.nodeType !== Node.ELEMENT_NODE ||
        !["ul", "ol"].includes(child.tagName.toLowerCase()),
      );
      const marker = ordered ? `${index + 1}.` : "-";
      const prefix = "  ".repeat(depth);
      const body = markdownInlineNodes(inlineNodes);
      const nested = Array.from(item.children).filter((child) =>
        ["ul", "ol"].includes(child.tagName.toLowerCase()),
      ).map((child) => markdownList(child, depth + 1)).filter(Boolean);
      return [body ? `${prefix}${marker} ${body}` : "", ...nested]
        .filter(Boolean).join("\n");
    }).filter(Boolean).join("\n");
  }

  function markdownBlocks(node) {
    return Array.from(node.childNodes).flatMap((child) => {
      if (child.nodeType === Node.TEXT_NODE) {
        const text = cleanText(child.nodeValue);
        return text ? [text] : [];
      }
      if (child.nodeType !== Node.ELEMENT_NODE) return [];

      const tag = child.tagName.toLowerCase();
      const inline = markdownInlineNodes(child.childNodes);
      if (/^h[1-6]$/.test(tag)) {
        const level = Math.max(2, Number(tag.slice(1)) || 2);
        return inline ? [`${"#".repeat(level)} ${inline}`] : [];
      }
      if (tag === "p") return inline ? [inline] : [];
      if (tag === "pre") {
        const code = String(child.textContent || "").trim();
        return code ? [`\`\`\`\n${code}\n\`\`\``] : [];
      }
      if (tag === "blockquote") {
        const quote = markdownBlocks(child).join("\n\n") || inline;
        return quote ? [quote.split("\n").map((line) => `> ${line}`).join("\n")] : [];
      }
      if (tag === "ul" || tag === "ol") {
        const list = markdownList(child);
        return list ? [list] : [];
      }
      if (tag === "hr") return ["---"];
      if (child.classList.contains("note")) {
        return inline ? [`> **Note:** ${inline}`] : [];
      }

      const nested = markdownBlocks(child);
      return nested.length ? nested : (inline ? [inline] : []);
    });
  }

  function redditMarkdown(article, fallback) {
    const blocks = article ? markdownBlocks(article) : [];
    return blocks.join("\n\n").replace(/\n{3,}/g, "\n\n") || fallback;
  }

  function postContext() {
    const title = cleanText(
      document.querySelector("article h1, main h1")?.textContent ||
      document.querySelector('meta[property="og:title"]')?.content ||
      document.title,
    );
    const description = cleanText(
      document.querySelector(".lede")?.textContent ||
      document.querySelector('meta[name="description"]')?.content ||
      document.querySelector("article p")?.textContent,
    );
    const image = document.querySelector(
      'main img[alt], article img[alt], meta[property="og:image"]',
    );
    const alt = cleanText(
      image?.getAttribute?.("alt") ||
      document.querySelector('meta[property="og:image:alt"]')?.content ||
      `ForkMesh artwork for “${title}”`,
    );
    const canonical =
      document.querySelector('link[rel="canonical"]')?.href ||
      location.href.split("#", 1)[0];
    const article = document.querySelector("article");
    return { title, description, alt, url: canonical, article };
  }

  function socialDrafts() {
    const context = postContext();
    // The image description belongs in Mastodon's own alt-text field, not in
    // the post body: pasting it inline spent the character budget and still
    // left the attached image undescribed for screen readers. It is offered
    // as its own copyable field beside the draft.
    const mastodonTail = `\n\n${MASTODON_TAGS}\n${context.url}`;
    const mastodonPrefix = `${context.title}\n\n`;
    const mastodon = mastodonPrefix +
      clipped(
        context.description,
        MASTODON_LIMIT - mastodonPrefix.length - mastodonTail.length,
      ) +
      mastodonTail;
    const twitterTail = `\n\n${TWITTER_TAGS}`;
    const twitterPrefix = `${context.title} — `;
    const twitter = twitterPrefix +
      clipped(
        context.description,
        TWITTER_LIMIT - twitterPrefix.length - twitterTail.length,
      ) +
      twitterTail;
    const reddit = `${redditMarkdown(context.article, context.description)}\n\n` +
      `---\n\n[Read the full post on ForkMesh](${context.url})`;
    return {
      context,
      mastodon: clipped(mastodon, MASTODON_LIMIT),
      twitter: clipped(twitter, TWITTER_LIMIT),
      reddit,
    };
  }

  function shareHref(network, drafts) {
    if (network === "mastodon") {
      return "https://mastodon.social/share?" +
        new URLSearchParams({ text: drafts.mastodon }).toString();
    }
    if (network === "twitter") {
      return "https://x.com/intent/post?" +
        new URLSearchParams({
          text: drafts.twitter,
          url: drafts.context.url,
        }).toString();
    }
    return "https://www.reddit.com/r/forkmesh/submit?" +
      new URLSearchParams({
        title: drafts.context.title,
        text: drafts.reddit,
      }).toString();
  }

  function renderField(label, text, rows, limit = 0, hint = "") {
    const field = document.createElement("div");
    field.className = "blog-social-field";
    const head = document.createElement("div");
    head.className = "blog-social-field-head";
    const heading = document.createElement("h4");
    heading.className = "blog-social-field-label";
    heading.textContent = label;
    const count = document.createElement("span");
    count.className = "blog-social-count";
    count.textContent = limit
      ? `${text.length}/${limit} characters`
      : `${text.length.toLocaleString()} characters · full length`;
    head.append(heading, count);
    const textarea = document.createElement("textarea");
    textarea.readOnly = true;
    textarea.rows = rows;
    textarea.value = text;
    textarea.setAttribute("aria-label", label);
    field.append(head, textarea);
    if (hint) {
      const note = document.createElement("p");
      note.className = "blog-social-hint";
      note.textContent = hint;
      field.append(note);
    }
    return field;
  }

  function renderDraft(draft) {
    const section = document.createElement("section");
    section.className = "blog-social-draft";
    section.dataset.network = draft.network;
    const head = document.createElement("div");
    head.className = "blog-social-draft-head";
    const heading = document.createElement("h3");
    heading.textContent = draft.label;
    head.append(iconTile(draft.network), heading);
    section.append(
      head,
      renderField(
        `${draft.label} post`, draft.text, draft.rows, draft.limit || 0,
      ),
    );
    if (draft.alt) {
      section.append(renderField(
        "Image alt text",
        draft.alt,
        3,
        0,
        "Paste into the image description field when attaching the artwork.",
      ));
    }
    const actions = document.createElement("div");
    actions.className = "blog-social-draft-actions";
    const link = document.createElement("a");
    link.className = "blog-social-prefill blog-social-button is-small";
    link.href = draft.href;
    link.target = "_blank";
    link.rel = "noopener noreferrer";
    link.title = `Open ${draft.label} with this post prefilled`;
    const caption = document.createElement("span");
    caption.textContent = `Open in ${SHORT_LABELS[draft.network]}`;
    link.append(caption, svgIcon("external"));
    actions.append(
      copyButton("Copy", draft.text, "blog-social-button is-small"),
      link,
    );
    section.append(actions);
    return section;
  }

  function renderShare(drafts) {
    const card = document.createElement("div");
    card.className = "blog-social-share";
    const intro = document.createElement("div");
    intro.className = "blog-social-intro";
    const title = document.createElement("h2");
    title.className = "blog-social-title";
    title.textContent = "Share this post";
    const lede = document.createElement("p");
    lede.className = "blog-social-lede";
    lede.textContent = "Know someone who should read it? Send it their way.";
    intro.append(title, lede);
    const actions = document.createElement("div");
    actions.className = "blog-social-actions";
    actions.append(copyButton(
      "Copy link", drafts.context.url, "blog-social-button is-primary",
    ));
    NETWORKS.forEach((network) => {
      const link = document.createElement("a");
      link.className = "blog-social-button";
      link.dataset.network = network.key;
      link.href = shareHref(network.key, drafts);
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      link.title = `Share on ${network.label}`;
      const caption = document.createElement("span");
      caption.textContent = SHORT_LABELS[network.key];
      link.append(svgIcon(network.key), caption);
      actions.append(link);
    });
    card.append(intro, actions);
    return card;
  }

  function blogSlug() {
    const match = location.pathname.match(/^\/blog\/([a-z0-9-]+)\/?$/);
    return match ? match[1] : "";
  }

  function renderMetrics(state, payload) {
    const section = document.createElement("section");
    section.className = "blog-social-metrics";
    const head = document.createElement("div");
    head.className = "blog-social-metrics-head";
    const title = document.createElement("h2");
    title.className = "blog-social-title";
    title.textContent = "Post reach";
    const totals = document.createElement("p");
    totals.className = "blog-social-metric-counts";
    // Views are deduplicated per reader per day by the Worker, so this is a
    // reach number: reloading the post does not walk it up.
    totals.textContent =
      `${plural(Number(payload.views || 0), "view", "views")} · ` +
      `${plural(
        Number(payload.uniqueViews || 0),
        "approximate unique reader",
        "approximate unique readers",
      )}`;
    const hint = document.createElement("p");
    hint.className = "blog-social-metric-hint";
    hint.textContent = "A reader counts once a day, so a reload is not a view.";
    head.append(title, totals);
    section.append(head, hint);
    const board = Array.isArray(payload.referrers) ? payload.referrers : [];
    if (board.length) {
      const subtitle = document.createElement("h3");
      subtitle.textContent = "HTTP referrer leaderboard";
      const list = document.createElement("ol");
      list.className = "blog-social-referrers";
      board.forEach((row) => {
        const item = document.createElement("li");
        const link = document.createElement("a");
        link.href = String(row.url || "");
        link.target = "_blank";
        link.rel = "noopener noreferrer";
        link.textContent = String(row.host || "");
        const visits = document.createElement("span");
        visits.textContent = plural(Number(row.visits || 0), "visit", "visits");
        item.append(link, visits);
        list.append(item);
      });
      section.append(subtitle, list);
    }
    state.metrics.textContent = "";
    state.metrics.append(section);
  }

  // The blog is served as static assets by the www Worker, which never reaches
  // the app Worker that counts a post's reach — so every post read 0 views.
  // The rendered page reports its own view instead, carrying the referrer the
  // document actually arrived with, and reads the fresh aggregates back from
  // the same response. Sent as text/plain so it stays a simple cross-origin
  // request: no preflight round trip on a page load.
  async function reportView(slug) {
    try {
      const response = await fetch(
        `/api/blog/metrics/${encodeURIComponent(slug)}`,
        {
          method: "POST",
          headers: {
            accept: "application/json",
            "content-type": "text/plain;charset=UTF-8",
          },
          body: JSON.stringify({ referrer: document.referrer || "" }),
          cache: "no-store",
        },
      );
      return response.ok ? await response.json() : null;
    } catch (error) {
      return null;
    }
  }

  async function readMetrics(slug) {
    try {
      const response = await fetch(
        `/api/blog/metrics/${encodeURIComponent(slug)}`,
        { headers: { accept: "application/json" }, cache: "no-store" },
      );
      return response.ok ? await response.json() : null;
    } catch (error) {
      return null;
    }
  }

  // The reach response carries both halves of a slot: the permalink an
  // administrator saved, and the network the referrer board says is already
  // sending readers here.
  function applyPostedSlots(state, payload) {
    const links = payload?.links;
    let changed = false;
    if (links && typeof links === "object") {
      NETWORKS.forEach((network) => {
        const url = permalink(links[network.key]);
        if (url && url !== state.links[network.key]) {
          state.links[network.key] = url;
          changed = true;
        }
      });
    }
    const detected = payload?.detected;
    if (detected && typeof detected === "object") {
      NETWORKS.forEach((network) => {
        const entry = detected[network.key];
        if (entry && typeof entry === "object" && permalink(entry.url)) {
          state.detected[network.key] = entry;
          changed = true;
        }
      });
    }
    if (changed) renderPosted(state);
  }

  async function hydrate(state) {
    const slug = blogSlug();
    if (!slug || location.protocol === "file:") return;
    const payload = await reportView(slug) || await readMetrics(slug);
    if (!payload?.ok) return;
    applyPostedSlots(state, payload);
    renderMetrics(state, payload);
  }

  async function saveLinks(state, button, status) {
    const slug = blogSlug();
    if (!slug) return;
    const links = {};
    state.posted.querySelectorAll("input.blog-social-input").forEach((input) => {
      links[input.name] = input.value.trim();
    });
    button.disabled = true;
    status.textContent = "Saving…";
    status.dataset.state = "pending";
    try {
      const response = await fetch(
        `/api/blog/links/${encodeURIComponent(slug)}`,
        {
          method: "PUT",
          headers: {
            accept: "application/json",
            "content-type": "application/json",
            ...authHeaders(),
          },
          body: JSON.stringify({ links }),
          cache: "no-store",
        },
      );
      const body = await response.json().catch(() => null);
      if (!response.ok) {
        status.dataset.state = "error";
        status.textContent = body?.error === "not_admin"
          ? "Sign in as an administrator to edit these permalinks."
          : body?.error === "invalid_permalink"
            ? `That ${body.network || ""} value is not an https permalink.`.trim()
            : "Could not save. Try again.";
        return;
      }
      // Server-normalized values only: what is stored is what the page shows,
      // including a slot the save just cleared.
      state.links = {};
      NETWORKS.forEach((network) => {
        const url = permalink(body?.links?.[network.key]);
        if (url) state.links[network.key] = url;
      });
      renderPosted(state);
      const saved = state.posted.querySelector("[data-blog-social-status]");
      if (saved) {
        saved.dataset.state = "ok";
        saved.textContent = "Saved";
      }
    } catch (error) {
      status.dataset.state = "error";
      status.textContent = "Could not save. Try again.";
    } finally {
      button.disabled = false;
    }
  }

  function renderPosted(state) {
    state.posted.textContent = "";
    const list = document.createElement("ul");
    list.className = "blog-social-list";
    NETWORKS.forEach((network) => list.append(renderItem(state, network)));
    state.posted.append(list);
    if (!state.admin) return;

    const controls = document.createElement("div");
    controls.className = "blog-social-admin";
    const button = document.createElement("button");
    button.type = "button";
    button.className = "blog-social-save";
    button.textContent = "Save permalinks";
    const status = document.createElement("span");
    status.className = "blog-social-status";
    status.setAttribute("role", "status");
    status.dataset.blogSocialStatus = "";
    status.textContent = "Paste each announcement permalink, then save.";
    button.addEventListener("click", () => saveLinks(state, button, status));
    controls.append(button, status);
    state.posted.append(controls);
  }

  function render(host) {
    host.classList.add("blog-social");
    host.textContent = "";

    const state = {
      host,
      links: attributeLinks(host),
      // Filled from the reach response: network -> {host, url, visits}.
      detected: {},
      admin: isAdminSession() && Boolean(blogSlug()),
      posted: document.createElement("div"),
      metrics: document.createElement("div"),
    };
    state.posted.className = "blog-social-posted";
    state.metrics.className = "blog-social-metrics-slot";

    const drafts = socialDrafts();
    const posted = document.createElement("div");
    posted.className = "blog-social-section";
    const intro = document.createElement("div");
    intro.className = "blog-social-intro";
    const title = document.createElement("h2");
    title.className = "blog-social-title";
    title.textContent = "Posted on";
    const lede = document.createElement("p");
    lede.className = "blog-social-lede";
    lede.textContent = "Where this post was announced. Join the conversation there.";
    intro.append(title, lede);
    posted.append(intro, state.posted);
    host.append(renderShare(drafts), posted);
    renderPosted(state);

    const panel = document.createElement("details");
    panel.className = "blog-social-drafts-panel";
    const summary = document.createElement("summary");
    const summaryText = document.createElement("span");
    summaryText.className = "blog-social-intro";
    const draftTitle = document.createElement("span");
    draftTitle.className = "blog-social-title";
    draftTitle.textContent = "Prefilled drafts";
    const draftHint = document.createElement("span");
    draftHint.className = "blog-social-lede";
    draftHint.textContent =
      "Ready-to-post copy for Reddit, Mastodon, and X, sized to each network.";
    summaryText.append(draftTitle, draftHint);
    summary.append(summaryText, svgIcon("chevron", "blog-social-summary-chevron"));
    const grid = document.createElement("div");
    grid.className = "blog-social-drafts";
    // Same networks in the same order as the "Posted on" slots above, sharing
    // one column template, so each draft sits under the slot it fills in.
    grid.append(
      renderDraft({
        network: "reddit",
        label: "Reddit",
        text: drafts.reddit,
        href: shareHref("reddit", drafts),
        rows: 10,
      }),
      renderDraft({
        network: "mastodon",
        label: "Mastodon",
        text: drafts.mastodon,
        href: shareHref("mastodon", drafts),
        rows: 7,
        limit: MASTODON_LIMIT,
        alt: drafts.context.alt,
      }),
      renderDraft({
        network: "twitter",
        label: "X (Twitter)",
        text: drafts.twitter,
        href: shareHref("twitter", drafts),
        rows: 7,
        limit: TWITTER_LIMIT,
      }),
    );
    panel.append(summary, grid);
    host.append(panel, state.metrics);
    void hydrate(state);
  }

  function renderAll() {
    document.querySelectorAll("[data-blog-social]").forEach(render);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", renderAll);
  } else {
    renderAll();
  }
})();
