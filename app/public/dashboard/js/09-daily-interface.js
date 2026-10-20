  // Shared daily-work interaction layer (issue #517). It is deliberately
  // page-agnostic: every dashboard document gets the same keyboard entry point,
  // focus restoration, live feedback, and privacy-preserving local timings.
  const DAILY_INTERFACE_BUDGETS_MS = Object.freeze({
    startup: Object.freeze({ local: 1500, remote: 2500 }),
    browse: Object.freeze({ local: 100, remote: 1000 }),
    search: Object.freeze({ local: 100, remote: 100 }),
    diff: Object.freeze({ local: 500, remote: 500 }),
    navigation: Object.freeze({ local: 100, remote: 250 }),
    feedback: Object.freeze({ local: 100, remote: 100 }),
  });

  const commandPaletteState = {
    previousFocus: null,
    selectedIndex: 0,
    commands: [],
  };

  function interfaceNow() {
    return window.performance?.now?.() ?? Date.now();
  }

  function recordInterfaceMetric(name, startedAt, source = "local") {
    const duration = Math.max(0, interfaceNow() - Number(startedAt || 0));
    const normalizedSource = source === "remote" ? "remote" : "local";
    const maximum = DAILY_INTERFACE_BUDGETS_MS[name]?.[normalizedSource];
    if (!Number.isFinite(maximum)) return null;
    const entry = Object.freeze({
      name,
      duration: Math.round(duration * 100) / 100,
      maximum,
      source: normalizedSource,
      passed: duration <= maximum,
    });
    const metrics = window.ForkMeshInterfaceMetrics || [];
    metrics.push(entry);
    // Keep only a short page-local diagnostic trail. Metrics contain no route,
    // repository, query, account, or user content and are never persisted.
    if (metrics.length > 50) metrics.splice(0, metrics.length - 50);
    window.ForkMeshInterfaceMetrics = metrics;
    window.dispatchEvent(new CustomEvent("forkmesh:interface-metric", {
      detail: entry,
    }));
    return entry;
  }

  function announceInterfaceStatus(message) {
    const status = document.querySelector("[data-interface-status]");
    if (!status) return;
    status.textContent = "";
    window.requestAnimationFrame(() => { status.textContent = String(message || ""); });
  }

  function commandPaletteCommands() {
    return [
      { group: "Go to", icon: "book-marked", label: "Browse repositories", hint: "Repositories", url: "/dashboard/repos", keywords: "browse code files" },
      { group: "Go to", icon: "search", label: "Search repositories", hint: "/", key: true, action: "search", keywords: "find filter code" },
      { group: "Go to", icon: "circle-dot", label: "Open issues", hint: "Issues", url: state.selectedRepo ? `${repoLinkUrl(state.selectedRepo)}/issues` : "/dashboard/repos", keywords: "issue bug task" },
      { group: "Go to", icon: "git-pull-request", label: "Review pull requests", hint: "Pulls", url: state.selectedRepo ? `${repoLinkUrl(state.selectedRepo)}/pulls` : "/dashboard/repos", keywords: "review diff changes" },
      { group: "Go to", icon: "share-2", label: "Open network status", hint: "Network", url: "/dashboard/network", keywords: "offline mirror recovery" },
      { group: "Actions", icon: "plus", label: "Create a repository", hint: "New", url: "/dashboard/repos/new", keywords: "clone import mirror" },
      { group: "Actions", icon: "sun-moon", label: "Switch color theme", hint: "Theme", action: "theme", keywords: "light dark contrast appearance" },
      { group: "Actions", icon: "keyboard", label: "Open keyboard help", hint: "Shortcuts", action: "help", keywords: "commands accessibility" },
    ];
  }

  function ensureCommandPalette() {
    if (document.querySelector("[data-command-palette]")) return;
    const palette = document.createElement("div");
    palette.hidden = true;
    palette.dataset.commandPalette = "";
    palette.className = "fm-spotlight-overlay";
    palette.setAttribute("role", "dialog");
    palette.setAttribute("aria-modal", "true");
    palette.setAttribute("aria-labelledby", "commandPaletteTitle");
    palette.setAttribute("aria-describedby", "commandPaletteHelp");
    palette.innerHTML = `
      <button data-command-palette-backdrop type="button" tabindex="-1" class="fm-spotlight-backdrop" aria-label="Close command palette"></button>
      <section class="fm-spotlight">
        <h2 id="commandPaletteTitle" class="sr-only">Command palette</h2>
        <p id="commandPaletteHelp" class="sr-only">Type to filter. Use Up and Down to choose, then Enter.</p>
        <div class="fm-spotlight-search">
          <i data-lucide="search" aria-hidden="true"></i>
          <label for="commandPaletteInput" class="sr-only">Filter commands</label>
          <input id="commandPaletteInput" data-command-palette-input type="search" autocomplete="off" spellcheck="false" role="combobox" aria-autocomplete="list" aria-controls="commandPaletteResults" aria-expanded="true" placeholder="Search ${escapeHtml(platformName())}" />
          <kbd>esc</kbd>
        </div>
        <div id="commandPaletteResults" data-command-palette-results role="listbox" aria-label="Commands" class="fm-spotlight-results"></div>
        <div class="fm-spotlight-footer" aria-hidden="true">
          <span class="fm-spotlight-brand"><i data-lucide="command"></i>${escapeHtml(platformName())}</span>
          <span class="fm-spotlight-keys"><span><kbd>↵</kbd>Open</span><span><kbd>↑</kbd><kbd>↓</kbd>Move</span><span><kbd>esc</kbd>Close</span></span>
        </div>
      </section>`;
    document.body.appendChild(palette);
    window.lucide?.createIcons();

    const status = document.createElement("div");
    status.dataset.interfaceStatus = "";
    status.className = "sr-only";
    status.setAttribute("role", "status");
    status.setAttribute("aria-live", "polite");
    status.setAttribute("aria-atomic", "true");
    document.body.appendChild(status);
  }

  function commandPaletteOpen() {
    return document.querySelector("[data-command-palette]")?.hidden === false;
  }

  function renderCommandPalette(query = "") {
    const normalized = String(query).trim().toLowerCase();
    commandPaletteState.commands = commandPaletteCommands().filter((command) =>
      `${command.label} ${command.keywords}`.toLowerCase().includes(normalized));
    commandPaletteState.selectedIndex = Math.min(
      commandPaletteState.selectedIndex,
      Math.max(0, commandPaletteState.commands.length - 1),
    );
    const results = document.querySelector("[data-command-palette-results]");
    const input = document.querySelector("[data-command-palette-input]");
    if (!results || !input) return;
    if (!commandPaletteState.commands.length) {
      results.innerHTML = '<div role="status" class="fm-spotlight-empty"><p class="fm-spotlight-empty-title">No results</p><p>Try repositories, issues, review, or theme.</p></div>';
      window.lucide?.createIcons();
      input.removeAttribute("aria-activedescendant");
      return;
    }
    let group = "";
    results.innerHTML = commandPaletteState.commands.map((command, index) => {
      const selected = index === commandPaletteState.selectedIndex;
      const heading = command.group !== group
        ? `<div class="fm-spotlight-group" role="presentation">${escapeHtml(command.group)}</div>`
        : "";
      group = command.group;
      const hint = command.key
        ? `<kbd>${escapeHtml(command.hint)}</kbd>`
        : `<span>${escapeHtml(command.hint)}</span>`;
      return `${heading}<button id="commandPaletteResult${index}" data-command-palette-result="${index}" type="button" role="option" aria-selected="${selected ? "true" : "false"}" class="fm-spotlight-item"><span class="fm-spotlight-icon"><i data-lucide="${escapeHtml(command.icon)}" aria-hidden="true"></i></span><span class="fm-spotlight-label">${escapeHtml(command.label)}</span><span class="fm-spotlight-hint">${hint}</span></button>`;
    }).join("");
    window.lucide?.createIcons();
    input.setAttribute("aria-activedescendant", `commandPaletteResult${commandPaletteState.selectedIndex}`);
  }

  function moveCommandPaletteSelection(delta) {
    if (!commandPaletteState.commands.length) return;
    const count = commandPaletteState.commands.length;
    commandPaletteState.selectedIndex = (commandPaletteState.selectedIndex + delta + count) % count;
    renderCommandPalette(document.querySelector("[data-command-palette-input]")?.value || "");
    document.getElementById(`commandPaletteResult${commandPaletteState.selectedIndex}`)?.scrollIntoView({ block: "nearest" });
  }

  function setCommandPaletteOpen(open) {
    ensureCommandPalette();
    const palette = document.querySelector("[data-command-palette]");
    if (!palette || palette.hidden === !open) return;
    if (open) {
      const started = interfaceNow();
      commandPaletteState.previousFocus = document.activeElement;
      commandPaletteState.selectedIndex = 0;
      palette.hidden = false;
      renderCommandPalette("");
      const input = document.querySelector("[data-command-palette-input]");
      input.value = "";
      input.focus();
      recordInterfaceMetric("feedback", started);
      announceInterfaceStatus("Command palette opened.");
      return;
    }
    palette.hidden = true;
    const previous = commandPaletteState.previousFocus;
    if (previous?.isConnected && typeof previous.focus === "function") previous.focus();
    announceInterfaceStatus("Command palette closed.");
  }

  function runCommandPaletteCommand(index = commandPaletteState.selectedIndex) {
    const command = commandPaletteState.commands[Number(index)];
    if (!command) return;
    const started = interfaceNow();
    if (command.url) {
      recordInterfaceMetric("navigation", started);
      location.assign(command.url);
      return;
    }
    setCommandPaletteOpen(false);
    if (command.action === "search") {
      const search = document.querySelector("#repoSearch, [data-global-search]");
      if (search && getComputedStyle(search).display !== "none") search.focus();
      else location.assign("/dashboard/repos");
    } else if (command.action === "theme") {
      saveDashboardTheme(readDashboardTheme() === "dark" ? "light" : "dark");
      announceInterfaceStatus(`Color theme switched to ${readDashboardTheme()}.`);
    } else if (command.action === "help") {
      announceInterfaceStatus("Shortcuts: Control or Command K opens commands, slash searches, T finds a file, and Escape closes overlays.");
    }
    recordInterfaceMetric("feedback", started);
  }

  function initDailyInterface() {
    ensureCommandPalette();
    document.addEventListener("click", (event) => {
      if (event.target.closest("[data-command-palette-open]")) {
        setCommandPaletteOpen(true);
        return;
      }
      if (event.target.closest("[data-command-palette-backdrop]")) {
        setCommandPaletteOpen(false);
        return;
      }
      const result = event.target.closest("[data-command-palette-result]");
      if (result) runCommandPaletteCommand(result.dataset.commandPaletteResult);
    });
    document.addEventListener("mousemove", (event) => {
      const result = event.target.closest?.("[data-command-palette-result]");
      if (!result) return;
      const index = Number(result.dataset.commandPaletteResult);
      if (index === commandPaletteState.selectedIndex) return;
      commandPaletteState.selectedIndex = index;
      for (const option of document.querySelectorAll("[data-command-palette-result]")) {
        option.setAttribute("aria-selected", option === result ? "true" : "false");
      }
      document.querySelector("[data-command-palette-input]")
        ?.setAttribute("aria-activedescendant", result.id);
    });
    document.addEventListener("input", (event) => {
      if (!event.target.matches?.("[data-command-palette-input]")) return;
      commandPaletteState.selectedIndex = 0;
      const started = interfaceNow();
      renderCommandPalette(event.target.value);
      recordInterfaceMetric("search", started);
    });
    document.addEventListener("keydown", (event) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
        event.preventDefault();
        setCommandPaletteOpen(!commandPaletteOpen());
        return;
      }
      if (!commandPaletteOpen()) return;
      if (event.key === "Escape") {
        event.preventDefault();
        setCommandPaletteOpen(false);
      } else if (event.key === "ArrowDown") {
        event.preventDefault();
        moveCommandPaletteSelection(1);
      } else if (event.key === "ArrowUp") {
        event.preventDefault();
        moveCommandPaletteSelection(-1);
      } else if (event.key === "Enter" && event.target.matches?.("[data-command-palette-input]")) {
        event.preventDefault();
        runCommandPaletteCommand();
      } else if (event.key === "Tab") {
        // The filter is the palette's sole tab stop; options use the standard
        // combobox active-descendant pattern and Arrow keys.
        event.preventDefault();
        document.querySelector("[data-command-palette-input]")?.focus();
      }
    });
  }
