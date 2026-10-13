(() => {
  function h(tag, props = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props)) {
      if (key === "on") for (const [event, handler] of Object.entries(value)) node.addEventListener(event, handler);
      else if (key === "class") node.className = value;
      else node[key] = value;
    }
    for (const child of children.flat()) {
      if (child !== null && child !== undefined && child !== false) node.append(child);
    }
    return node;
  }

  function ago(stamp) {
    if (!stamp) return "never";
    const minutes = Math.round((Date.now() - stamp) / 60000);
    if (minutes < 1) return "just now";
    if (minutes < 60) return `${minutes} min ago`;
    const hours = Math.round(minutes / 60);
    return hours < 48 ? `${hours} h ago` : `${Math.round(hours / 24)} d ago`;
  }

  const button = (label, primary, disabled, click, title = "") =>
    h("button", { class: primary ? "primary" : "secondary", type: "button", disabled, title, textContent: label, on: { click } });

  function render(root, plan, { busy = false, status = null, go, run, update, open, title = "", kind = "", lines = [], actions = [] } = {}) {
    const suggest = plan?.suggest || null;
    if (!plan?.show && !actions.length) {
      root.replaceChildren();
      root.hidden = true;
      return;
    }
    root.hidden = false;
    const pages = plan?.pages || [];
    const here = Boolean(plan?.here);
    const site = plan?.next && plan.next.url !== suggest?.url ? plan.next : null;
    const moving = Boolean(suggest && !suggest.covered && !suggest.current);
    const updated = suggest && (suggest.at ? `last updated ${ago(suggest.at)}` : "never updated");
    const buttons = [];
    if (here) {
      buttons.push(button(busy ? "Working…" : plan.action, true, busy, () => run(), `Record in ${plan.organization} Marketing`));
    }
    if (suggest && !suggest.covered) {
      buttons.push(suggest.current
        ? button(`Update ${suggest.label}`, !here, busy, () => update(suggest), `Record in ${plan.organization} Marketing`)
        : button(`Go to ${suggest.label}`, !here && !actions.some((item) => item.primary), busy, () => go(suggest.url), suggest.url));
    }
    if (site) buttons.push(button(`Go to ${site.label}`, !here && !moving && !actions.some((item) => item.primary), busy, () => go(site.url), site.url));
    for (const item of actions) buttons.push(button(item.label, item.primary && !here, busy || item.disabled, () => item.run()));
    if (open) buttons.push(button("Open refresh panel", false, false, open));
    const note = typeof status?.text === "string" && status.text ? status.text : lines.length || actions.length ? "" : plan?.hint || "";
    const heading = title || plan?.title || "";
    const count = pages.length > 1 ? `${plan.read}/${plan.total} read` : kind;
    root.replaceChildren(
      h("h2", { class: "refresh-title" }, h("b", { textContent: heading || "Next to update" }), count ? h("span", { textContent: count }) : null),
      suggest ? h("p", { class: "refresh-next", textContent: suggest.current ? `${suggest.label} is next to update: ${updated}.`
        : heading ? `Next to update: ${suggest.label}, ${updated}.` : `${suggest.label}, ${updated}.` }) : null,
      lines.map((line) => h("p", { class: "muted", textContent: line })),
      buttons.length ? h("div", { class: "actions" }, buttons) : null,
      note ? h("p", { class: status?.error ? "error" : status?.busy ? "muted" : status?.text ? "ok" : "muted", textContent: note }) : null,
      pages.length > 1 ? h("ul", { class: "refresh-pages" }, pages.map((page) => h("li", { class: `refresh-page${page.current ? " current" : ""}${page.done ? " done" : ""}` },
        h("button", {
          class: "secondary", type: "button", disabled: busy || (page.current && !here),
          title: page.current ? "The page in this tab" : page.url,
          textContent: `${page.done ? "✓" : page.current ? "•" : "○"} ${page.label}`,
          on: { click: () => (page.current ? here && run() : go(page.url)) },
        })))) : null);
  }

  globalThis.ForkMeshRefreshView = { render, ago };
})();
