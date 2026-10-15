(() => {
  const API = "/api/plugin-race";
  const RATINGS = 10;
  const CHECKS = [
    ["account", "ForkMesh account"],
    ["member", "Member check"],
    ["x", "X post"],
    ["reddit", "Reddit post"],
    ["mastodon", "Mastodon post"],
    ["discord", "Discord"],
  ];
  const STATES = {
    confirmed: "Done",
    pending: "Waiting",
    missing: "Missing",
    rejected: "Not accepted",
  };
  const ERRORS = {
    sign_in_required: "Sign in first.",
    email_not_verified: "Verify your email address first. There is a link in your account settings.",
    rules_required: "Tick the box to say you have read the rules.",
    invalid_post: "That does not look like a link to a single public post.",
    post_already_used: "Someone else has already entered that post. Use your own.",
    invalid_discord: "That does not look like a Discord username.",
    disqualified: "This account has been taken out of the race.",
    qualify_first: "Pass all four checks before entering a plugin.",
    race_not_open: "Plugin entries open on 19 October 2026.",
    race_closed: "The race has closed.",
    invalid_package: "Use the package name exactly as it appears in the marketplace.",
    package_not_yours: "We could not find a release of that package you published in the community marketplace since the race opened.",
    summary_required: "Tell us what the plugin does.",
    platforms_required: "Tell us how to try it on desktop, iOS and the web.",
    plugin_limit: "You have already entered three plugins.",
    already_entered: "That plugin is already in the race.",
    origin_not_allowed: "Open this page on forkmesh.com and try again.",
  };
  const NETWORK_NAMES = { x: "X", reddit: "Reddit", mastodon: "Mastodon" };

  const $ = (selector) => document.querySelector(selector);

  function session() {
    try {
      const value = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!value || !value.nodeName || value.kind === "node") return null;
      return value;
    } catch (_) {
      return null;
    }
  }

  function headers(json) {
    const result = { accept: "application/json" };
    if (json) result["content-type"] = "application/json";
    const token = String(session()?.sessionToken || "");
    if (token && token !== "cookie") result.authorization = `Bearer ${token}`;
    return result;
  }

  async function call(path, body) {
    const response = await fetch(path, {
      method: body ? "POST" : "GET",
      headers: headers(Boolean(body)),
      credentials: "same-origin",
      cache: "no-store",
      body: body ? JSON.stringify(body) : undefined,
    });
    const data = await response.json().catch(() => ({}));
    return { ok: response.ok, data };
  }

  function explain(data) {
    const error = String(data?.error || "");
    let text = ERRORS[error] || "Something went wrong. Try again in a minute.";
    if (data?.network && NETWORK_NAMES[data.network]) {
      text = `${NETWORK_NAMES[data.network]}: ${text}`;
    }
    return text;
  }

  function say(node, text, bad) {
    if (!node) return;
    node.textContent = text || "";
    if (bad) node.dataset.tone = "bad";
    else delete node.dataset.tone;
  }

  function element(tag, props, children) {
    const node = document.createElement(tag);
    Object.entries(props || {}).forEach(([key, value]) => {
      if (key === "text") node.textContent = value;
      else if (key === "data") Object.entries(value).forEach(([k, v]) => { node.dataset[k] = v; });
      else node.setAttribute(key, value);
    });
    (children || []).forEach((child) => child && node.append(child));
    return node;
  }

  function renderBoard(payload) {
    const list = $("[data-race-board]");
    const empty = $("[data-race-empty]");
    const left = $("[data-race-left]");
    const plugins = Array.isArray(payload?.plugins) ? payload.plugins : [];
    if (left) left.textContent = `${Number(payload?.prizesLeft ?? 10)} left`;
    const summary = $("[data-race-summary]");
    if (summary && payload) {
      const people = Number(payload.qualifiedEntrants || 0);
      summary.textContent = `${plugins.length} ${plugins.length === 1 ? "plugin" : "plugins"} on the board, ${people} ${people === 1 ? "person has" : "people have"} passed the checks, ${Number(payload.prizesLeft ?? 10)} of 10 prizes left.`;
    }
    if (!list || !empty) return;
    list.replaceChildren();
    if (!plugins.length) {
      list.hidden = true;
      empty.hidden = false;
      return;
    }
    plugins.forEach((plugin, index) => {
      const ratings = Math.min(Number(plugin.ratings || 0), RATINGS);
      const bar = element("span", { class: "bar" }, [element("i", { style: `width:${(ratings / RATINGS) * 100}%` })]);
      const title = element("a", {
        href: `https://app.forkmesh.com/market-reviews?market=${encodeURIComponent(plugin.market)}&package=${encodeURIComponent(plugin.package)}`,
        text: plugin.package,
      });
      const row = element("li", plugin.rank ? { data: { won: "1" } } : {}, [
        element("span", { class: "pos", text: plugin.rank ? `#${plugin.rank}` : String(index + 1).padStart(2, "0") }),
        element("span", { class: "name" }, [
          title,
          element("span", { text: `by @${plugin.builder}${plugin.summary ? ` · ${plugin.summary}` : ""}` }),
        ]),
        element("span", { class: "meter" }, [
          element("span", { text: plugin.rank ? (plugin.paid ? "Won · paid" : "Won") : `${Number(plugin.ratings || 0)} / ${RATINGS}` }),
          bar,
        ]),
      ]);
      list.append(row);
    });
    list.hidden = false;
    empty.hidden = true;
  }

  function renderMe(me) {
    const signedOut = $("[data-race-signed-out]");
    const form = $("[data-race-entrant]");
    const status = $("[data-race-status]");
    const pluginForm = $("[data-race-plugin]");
    const locked = $("[data-race-plugin-locked]");
    const mine = $("[data-race-mine]");
    if (!form) return;
    if (!me) {
      signedOut.hidden = false;
      form.hidden = true;
      if (pluginForm) pluginForm.hidden = true;
      if (status) status.textContent = "";
      return;
    }
    signedOut.hidden = true;
    form.hidden = false;
    const entrant = me.entrant;
    const checks = entrant?.checks || { account: me.emailVerified ? "confirmed" : "missing" };
    const list = $("[data-race-checks]");
    list.replaceChildren(...CHECKS.map(([key, label]) => {
      const state = checks[key] || "missing";
      return element("li", { data: { state } }, [
        document.createTextNode(label),
        element("span", { text: STATES[state] || state }),
      ]);
    }));
    if (entrant) {
      ["x", "reddit", "mastodon"].forEach((network) => {
        const input = form.elements[network];
        if (input && !input.value) input.value = entrant.posts?.[network]?.url || "";
      });
      if (form.elements.discord && !form.elements.discord.value) form.elements.discord.value = entrant.discord || "";
      $("[data-race-accept]").hidden = true;
    }
    const qualified = entrant?.status === "qualified";
    if (status) {
      status.textContent = entrant
        ? qualified ? "Qualified" : entrant.status === "disqualified" ? "Out of the race" : "In progress"
        : "Not entered";
    }
    const notes = [];
    if (entrant) {
      ["x", "reddit", "mastodon"].forEach((network) => {
        const detail = entrant.posts?.[network]?.detail;
        if (detail) notes.push(`${NETWORK_NAMES[network]}: ${detail}`);
      });
      if ((entrant.memberFlags || []).includes("account_maturity_pending")) {
        notes.push("Member check: your account needs to be at least 7 days old.");
      }
      if ((entrant.memberFlags || []).includes("desktop_activity_unproven")) {
        notes.push("Member check: run the ForkMesh desktop app, signed in, on two days a day apart.");
      }
    }
    const message = $("[data-race-msg]");
    if (message && !message.dataset.tone) say(message, notes.join(" "));
    if (pluginForm && locked) {
      pluginForm.hidden = !qualified;
      locked.hidden = qualified;
    }
    if (mine) {
      const plugins = Array.isArray(me.plugins) ? me.plugins : [];
      const labels = { review: "Being reviewed", accepted: "On the board", rejected: "Not accepted" };
      mine.replaceChildren(...plugins.map((plugin) => element("li", {}, [
        element("b", { text: plugin.package }),
        element("span", { text: plugin.rank ? `Won #${plugin.rank}` : `${labels[plugin.status] || plugin.status}${plugin.note ? ` · ${plugin.note}` : ""}` }),
      ])));
      mine.hidden = !plugins.length;
      if (plugins.length && !me.payoutWallet) {
        mine.append(element("li", {}, [element("span", { text: "Add a Solana address in your account settings so we can pay you." })]));
      }
    }
  }

  async function load() {
    try {
      const { ok, data } = await call(API);
      if (!ok) return;
      renderBoard(data);
      renderMe(data.me);
    } catch (_) {}
  }

  function wire() {
    const form = $("[data-race-entrant]");
    form?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const message = $("[data-race-msg]");
      const body = {};
      ["x", "reddit", "mastodon", "discord"].forEach((name) => {
        const value = String(form.elements[name]?.value || "").trim();
        if (value) body[name] = value;
      });
      if (form.elements.acceptRules?.checked) body.acceptRules = true;
      say(message, "Checking your posts…");
      const { ok, data } = await call(`${API}/entrant`, body);
      if (!ok) {
        say(message, explain(data), true);
        return;
      }
      say(message, "");
      renderMe(data.me);
      if (data.me?.entrant?.status === "qualified") say(message, "You have passed all four checks.");
    });
    const pluginForm = $("[data-race-plugin]");
    pluginForm?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const message = $("[data-race-plugin-msg]");
      const value = (name) => String(pluginForm.elements[name]?.value || "").trim();
      say(message, "Sending…");
      const { ok, data } = await call(`${API}/plugins`, {
        package: value("package"),
        summary: value("summary"),
        platforms: { desktop: value("desktop"), ios: value("ios"), web: value("web") },
      });
      if (!ok) {
        say(message, explain(data), true);
        return;
      }
      pluginForm.reset();
      say(message, "Entered. We will review it and put it on the board once it is accepted.");
      renderMe(data.me);
    });
  }

  wire();
  load();
})();
