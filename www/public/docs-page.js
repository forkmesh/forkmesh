// Behaviour for rendered documentation pages (/docs/<path>/): search over the
// sidebar library, outline highlighting, copy buttons and the helpfulness
// feedback that the hand-written /docs/ landing page also posts.
(() => {
  const docsSearch = document.querySelector("#docs-search");
  const docsSearchPanel = document.querySelector("#docs-search-panel");
  const docsSearchResults = document.querySelector("#docs-search-results");
  const docsSearchEmpty = document.querySelector("#docs-search-empty");
  const docsSearchIndex = Array.from(
    document.querySelectorAll("[data-search-title]"),
  ).map((element) => ({
    title: element.getAttribute("data-search-title") || "",
    body: element.getAttribute("data-search-body") || "",
    target:
      element.getAttribute("data-search-href") ||
      (element.id ? `#${element.id}` : "#"),
  }));

  function setDocsSearchPanel(open) {
    if (!docsSearch || !docsSearchPanel) return;
    docsSearchPanel.hidden = !open;
    docsSearch.setAttribute("aria-expanded", open ? "true" : "false");
  }

  function renderDocsSearchResults(query) {
    if (!docsSearchResults || !docsSearchEmpty) return;
    const normalized = query.trim().toLowerCase();
    const matches = (
      normalized
        ? docsSearchIndex
            .map((item) => {
              const title = item.title.toLowerCase();
              const body = item.body.toLowerCase();
              const matched =
                title.includes(normalized) || body.includes(normalized);
              const score = title.includes(normalized) ? 0 : 1;
              return { ...item, matched, score };
            })
            .filter((item) => item.matched)
            .sort((a, b) => a.score - b.score || a.title.localeCompare(b.title))
        : docsSearchIndex
    ).slice(0, 10);

    docsSearchResults.textContent = "";
    docsSearchEmpty.hidden = matches.length > 0;

    matches.forEach((item) => {
      const link = document.createElement("a");
      link.href = item.target;
      link.setAttribute("role", "option");
      const title = document.createElement("strong");
      title.textContent = item.title;
      const body = document.createElement("span");
      body.textContent = item.body;
      link.append(title, body);
      link.addEventListener("click", () => {
        setDocsSearchPanel(false);
        docsSearch.value = "";
      });
      docsSearchResults.append(link);
    });
  }

  if (docsSearch && docsSearchPanel) {
    docsSearch.addEventListener("focus", () => {
      renderDocsSearchResults(docsSearch.value);
      setDocsSearchPanel(true);
    });
    docsSearch.addEventListener("input", () => {
      renderDocsSearchResults(docsSearch.value);
      setDocsSearchPanel(true);
    });
    docsSearch.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        setDocsSearchPanel(false);
        docsSearch.blur();
        return;
      }
      if (event.key === "Enter") {
        const firstResult = docsSearchPanel.querySelector("a");
        if (firstResult) {
          event.preventDefault();
          firstResult.click();
        }
      }
    });
    document.addEventListener("keydown", (event) => {
      if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "k") {
        event.preventDefault();
        docsSearch.focus();
        docsSearch.select();
      }
    });
    document.addEventListener("click", (event) => {
      if (!docsSearchPanel.contains(event.target) && event.target !== docsSearch) {
        setDocsSearchPanel(false);
      }
    });
  }

  // The sidebar starts open so the library is browsable without JavaScript;
  // on narrow screens collapse it behind its summary.
  const navDetails = document.querySelector("details.dp-nav");
  if (navDetails) {
    const narrow = window.matchMedia("(max-width: 1023px)");
    const sync = () => {
      navDetails.open = !narrow.matches;
    };
    sync();
    narrow.addEventListener("change", sync);
    // Bring the current page into view inside the sidebar only; scrolling
    // the element into view would also scroll the document.
    const nav = navDetails.querySelector("nav");
    const active = navDetails.querySelector(".dp-nav-link.active");
    if (nav && active && !narrow.matches) {
      nav.scrollTop = Math.max(
        0,
        active.offsetTop - nav.clientHeight / 2 + active.offsetHeight / 2,
      );
    }
  }

  async function copyText(text) {
    try {
      await navigator.clipboard.writeText(text);
    } catch (error) {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.left = "-9999px";
      document.body.append(area);
      area.select();
      document.execCommand("copy");
      area.remove();
    }
  }

  function flash(button, label) {
    const original = button.textContent;
    button.textContent = label;
    setTimeout(() => {
      button.textContent = original;
    }, 1400);
  }

  const copyButton = document.querySelector("#copy-page");
  if (copyButton) {
    copyButton.addEventListener("click", async () => {
      await copyText(window.location.href.split("#")[0]);
      const label = copyButton.querySelector("span");
      if (label) flash(label, "Copied");
    });
  }

  document.querySelectorAll(".dp-prose pre").forEach((block) => {
    const code = block.querySelector("code");
    if (!code) return;
    const language = code.getAttribute("data-language");
    if (language) {
      const chip = document.createElement("span");
      chip.className = "dp-code-lang";
      chip.setAttribute("aria-hidden", "true");
      chip.textContent = language;
      block.append(chip);
    }
    const button = document.createElement("button");
    button.type = "button";
    button.className = "dp-copy-code";
    button.textContent = "Copy";
    button.addEventListener("click", async () => {
      await copyText(code.textContent.replace(/\n$/, ""));
      flash(button, "Copied");
    });
    block.append(button);
  });

  // Tables marked data-sortable (the /docs/documents/ index) sort by any
  // column header. Cells carry data-sort-value so dates sort by timestamp
  // rather than by their display text.
  const collator = new Intl.Collator(undefined, { numeric: true, sensitivity: "base" });
  document.querySelectorAll("table[data-sortable]").forEach((table) => {
    const body = table.tBodies[0];
    const headers = Array.from(table.querySelectorAll("thead th[data-sort]"));
    if (!body) return;
    const valueOf = (row, index) => {
      const cell = row.cells[index];
      if (!cell) return "";
      return cell.getAttribute("data-sort-value") ?? cell.textContent.trim();
    };
    headers.forEach((header, index) => {
      const button = header.querySelector("button");
      if (!button) return;
      button.addEventListener("click", () => {
        const numeric = header.getAttribute("data-sort") === "number";
        const current = header.getAttribute("aria-sort");
        // Dates and counts start newest/largest first; text starts A to Z.
        const direction =
          current === "none" || !current
            ? numeric
              ? "descending"
              : "ascending"
            : current === "ascending"
              ? "descending"
              : "ascending";
        const sign = direction === "ascending" ? 1 : -1;
        const rows = Array.from(body.rows);
        rows.sort((a, b) => {
          const left = valueOf(a, index);
          const right = valueOf(b, index);
          const order = numeric
            ? (Number(left) || 0) - (Number(right) || 0)
            : collator.compare(left, right);
          // Ties fall back to the title so the order is stable either way.
          return sign * order || collator.compare(valueOf(a, 0), valueOf(b, 0));
        });
        body.append(...rows);
        headers.forEach((other) => other.setAttribute("aria-sort", "none"));
        header.setAttribute("aria-sort", direction);
      });
    });
  });

  document.querySelectorAll("input[data-table-filter]").forEach((input) => {
    const table = document.getElementById(input.getAttribute("data-table-filter"));
    const body = table && table.tBodies[0];
    if (!body) return;
    const count = document.querySelector(
      `[data-table-count="${input.getAttribute("data-table-filter")}"]`,
    );
    const total = body.rows.length;
    input.addEventListener("input", () => {
      const terms = input.value.trim().toLowerCase().split(/\s+/).filter(Boolean);
      let shown = 0;
      Array.from(body.rows).forEach((row) => {
        const text = row.getAttribute("data-filter-text") || row.textContent.toLowerCase();
        const match = terms.every((term) => text.includes(term));
        row.hidden = !match;
        if (match) shown += 1;
      });
      if (count) count.textContent = `Showing ${shown} of ${total}`;
    });
  });

  const outlineLinks = Array.from(document.querySelectorAll(".dp-outline-link"));
  if (outlineLinks.length && "IntersectionObserver" in window) {
    const byId = new Map(
      outlineLinks.map((link) => [link.getAttribute("href").slice(1), link]),
    );
    let current = null;
    const observer = new IntersectionObserver(
      (entries) => {
        entries.forEach((entry) => {
          if (!entry.isIntersecting) return;
          const link = byId.get(entry.target.id);
          if (!link || link === current) return;
          if (current) current.classList.remove("active");
          link.classList.add("active");
          current = link;
        });
      },
      { rootMargin: "-20% 0px -70% 0px", threshold: 0 },
    );
    byId.forEach((_, id) => {
      const heading = document.getElementById(id);
      if (heading) observer.observe(heading);
    });
  }

  const feedbackLike = document.querySelector("[data-feedback-like]");
  const feedbackDislike = document.querySelector("[data-feedback-dislike]");
  const feedbackStatus = document.querySelector("[data-feedback-status]");
  const feedbackModal = document.querySelector("[data-feedback-modal]");
  const feedbackForm = document.querySelector("[data-feedback-form]");
  const feedbackMessage = document.querySelector("[data-feedback-message]");
  const feedbackCloseButtons = document.querySelectorAll("[data-feedback-close]");

  function setFeedbackStatus(message) {
    if (feedbackStatus) feedbackStatus.textContent = message;
  }

  const docsMeta = document.querySelector("[data-docs-meta]");

  async function showPageViews() {
    if (!docsMeta) return;
    try {
      const response = await fetch(
        `/api/analytics/_/pages?path=${encodeURIComponent(window.location.pathname)}`,
        { headers: { accept: "application/json" } },
      );
      if (!response.ok) return;
      const data = await response.json();
      const visitors = Number(data && data.visitors) || 0;
      if (visitors < 1) return;
      if (docsMeta.children.length) {
        const separator = document.createElement("span");
        separator.setAttribute("aria-hidden", "true");
        separator.textContent = "·";
        docsMeta.append(separator);
      }
      const views = document.createElement("span");
      views.setAttribute("data-docs-views", "");
      views.title = "Unique visitors, counted by ForkMesh Analytics";
      views.textContent = `${visitors.toLocaleString()} view${visitors === 1 ? "" : "s"}`;
      docsMeta.append(views);
    } catch (error) {}
  }

  showPageViews();

  function setFeedbackDisabled(disabled) {
    [feedbackLike, feedbackDislike].forEach((button) => {
      if (button) button.disabled = disabled;
    });
  }

  function setFeedbackModal(open) {
    if (!feedbackModal) return;
    feedbackModal.hidden = !open;
    if (open && feedbackMessage) feedbackMessage.focus();
  }

  async function submitFeedback({ vote, message = "" }) {
    setFeedbackDisabled(true);
    try {
      const response = await fetch("/api/feedback", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({
          source: "docs",
          vote,
          path: window.location.pathname + window.location.hash,
          message,
        }),
        keepalive: true,
      });
      if (!response.ok) throw new Error("feedback failed");
      setFeedbackStatus("Thanks for the feedback.");
      setFeedbackModal(false);
    } catch (error) {
      setFeedbackStatus("Was this page helpful?");
      setFeedbackDisabled(false);
    }
  }

  if (feedbackLike) {
    feedbackLike.addEventListener("click", () => submitFeedback({ vote: "like" }));
  }
  if (feedbackDislike) {
    feedbackDislike.addEventListener("click", () => setFeedbackModal(true));
  }
  if (feedbackForm) {
    feedbackForm.addEventListener("submit", (event) => {
      event.preventDefault();
      submitFeedback({
        vote: "dislike",
        message: feedbackMessage ? feedbackMessage.value : "",
      });
    });
  }
  feedbackCloseButtons.forEach((button) => {
    button.addEventListener("click", () => setFeedbackModal(false));
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") setFeedbackModal(false);
  });
})();
