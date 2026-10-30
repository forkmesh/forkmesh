(() => {
  const page = document.querySelector("[data-market-package-page]");
  if (!page) return;
  const id = page.dataset.marketPackagePage;

  const writeText = (text) => {
    if (navigator.clipboard) return navigator.clipboard.writeText(text);
    const area = document.createElement("textarea");
    area.value = text;
    area.style.position = "fixed";
    area.style.left = "-9999px";
    document.body.append(area);
    area.select();
    document.execCommand("copy");
    area.remove();
    return Promise.resolve();
  };
  const flash = (button, label) => {
    const original = button.dataset.marketLabel || button.innerHTML;
    button.dataset.marketLabel = original;
    button.textContent = label;
    setTimeout(() => { button.innerHTML = original; }, 1500);
  };
  const copy = (button, url) => writeText(url)
    .then(() => flash(button, "Copied"), () => flash(button, "Copy failed"));
  page.querySelectorAll("[data-market-copy-link]").forEach((button) => {
    button.addEventListener("click", () => copy(button, button.dataset.marketCopyLink));
  });
  page.querySelectorAll("[data-market-share]").forEach((button) => {
    button.addEventListener("click", async () => {
      const url = button.dataset.marketShare;
      const name = button.dataset.marketShareName || "ForkMesh plugin";
      if (navigator.share) {
        try {
          await navigator.share({ title: name, text: `${name} on the ForkMesh Market`, url });
          return;
        } catch (error) {
          if (error && error.name === "AbortError") return;
        }
      }
      copy(button, url);
    });
  });

  const json = (path) => fetch(path, { headers: { accept: "application/json" } })
    .then((response) => (response.ok ? response.json() : null))
    .catch(() => null);
  const format = (value) => Number(value).toLocaleString();
  const stats = page.querySelector(`[data-market-stats="${CSS.escape(id)}"]`);
  const show = (selector, value) => {
    const target = stats?.querySelector(selector);
    if (!target || !(value > 0)) return;
    target.textContent = format(value);
    target.closest(".market-stat").hidden = false;
    stats.hidden = false;
  };

  json("/api/market/installs").then((payload) => {
    const count = payload?.packages?.[id];
    if (!count) return;
    show("[data-market-downloads]", Number(count.downloads) || 0);
    show("[data-market-installs]", Number(count.installs) || 0);
  });

  const rating = page.querySelector(`[data-market-rating="${CSS.escape(id)}"]`);
  const link = rating?.querySelector("[data-market-rating-link]");
  const origin = (() => {
    try { return window.ForkMeshAPI?.origin || ""; } catch (_) { return ""; }
  })();
  if (link && origin && /^https?:/.test(origin)) {
    const target = new URL(link.getAttribute("href"));
    link.href = origin.replace(/\/$/, "") + target.pathname + target.search;
  }
  json("/api/market/reviews/summary").then((payload) => {
    const used = payload?.usage?.[id];
    if (used) {
      show("[data-market-people]", Number(used.people) || 0);
      show("[data-market-opens]", Number(used.opens) || 0);
    }
    const score = payload?.packages?.[id];
    const count = Number(score?.count) || 0;
    if (!rating || !count) return;
    const average = Math.max(0, Math.min(5, Number(score.average) || 0));
    const stars = rating.querySelector("[data-market-rating-stars]");
    stars.style.setProperty("--rating", String(average));
    stars.setAttribute(
      "aria-label",
      `${average.toFixed(1)} out of 5 stars from ${count.toLocaleString()} rating${count === 1 ? "" : "s"}`,
    );
    rating.querySelector("[data-market-rating-summary]").textContent =
      `${average.toFixed(1)} (${count.toLocaleString()})`;
  });
})();
