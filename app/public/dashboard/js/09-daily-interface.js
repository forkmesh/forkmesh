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
      { label: "Browse repositories", hint: "Repositories", url: "/dashboard/repos", keywords: "browse code files" },
      { label: "Search repositories", hint: "/", action: "search", keywords: "find filter code" },
      { label: "Create a repository", hint: "New", url: "/dashboard/repos/new", keywords: "clone import mirror" },
      { label: "Open issues", hint: "Issues", url: state.selectedRepo ? `${repoLinkUrl(state.selectedRepo)}/issues` : "/dashboard/repos", keywords: "issue bug task" },
      { label: "Review pull requests", hint: "Pulls", url: state.selectedRepo ? `${repoLinkUrl(state.selectedRepo)}/pulls` : "/dashboard/repos", keywords: "review diff changes" },
      { label: "Open network status", hint: "Network", url: "/dashboard/network", keywords: "offline mirror recovery" },
      { label: "Switch color theme", hint: "Theme", action: "theme", keywords: "light dark contrast appearance" },
      { label: "Open keyboard help", hint: "Shortcuts", action: "help", keywords: "commands accessibility" },
    ];
  }

  function ensureCommandPalette() {
    if (document.querySelector("[data-command-palette]")) return;
    const palette = document.createElement("div");
    palette.hidden = true;
    palette.dataset.commandPalette = "";
    palette.className = "fixed inset-0 z-[90] items-start justify-center bg-background/80 p-4 pt-[10vh] backdrop-blur-sm";
    palette.setAttribute("role", "dialog");
    palette.setAttribute("aria-modal", "true");
    palette.setAttribute("aria-labelledby", "commandPaletteTitle");
    palette.setAttribute("aria-describedby", "commandPaletteHelp");
    palette.innerHTML = `
      <button data-command-palette-backdrop type="button" tabindex="-1" class="absolute inset-0 cursor-default" aria-label="Close command palette"></button>
      <section class="relative w-full max-w-xl overflow-hidden rounded-xl border border-border bg-popover text-popover-foreground shadow-2xl">
        <div class="border-b border-border px-4 py-3">
          <div class="flex items-center justify-between gap-3">
            <h2 id="commandPaletteTitle" class="text-sm font-semibold">Command palette</h2>
            <kbd class="rounded border border-border px-1.5 py-0.5 font-mono text-[10px] text-muted-foreground">Esc</kbd>
          </div>
          <p id="commandPaletteHelp" class="mt-1 text-xs text-muted-foreground">Type to filter. Use Up and Down to choose, then Enter.</p>
          <label for="commandPaletteInput" class="sr-only">Filter commands</label>
          <input id="commandPaletteInput" data-command-palette-input type="search" autocomplete="off" role="combobox" aria-autocomplete="list" aria-controls="commandPaletteResults" aria-expanded="true" class="mt-3 h-11 w-full rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none" placeholder="Search commands" />
        </div>
        <div id="commandPaletteResults" data-command-palette-results role="listbox" aria-label="Commands" class="max-h-[55vh] overflow-y-auto p-2"></div>
      </section>`;
    document.body.appendChild(palette);

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
      results.innerHTML = '<p role="status" class="px-3 py-6 text-center text-sm text-muted-foreground">No commands match. Try repositories, issues, review, or theme.</p>';
      input.removeAttribute("aria-activedescendant");
      return;
    }
    results.innerHTML = commandPaletteState.commands.map((command, index) => {
      const selected = index === commandPaletteState.selectedIndex;
      return `<button id="commandPaletteResult${index}" data-command-palette-result="${index}" type="button" role="option" aria-selected="${selected ? "true" : "false"}" class="flex min-h-11 w-full items-center justify-between gap-3 rounded-md px-3 py-2 text-left text-sm text-muted-foreground"><span>${escapeHtml(command.label)}</span><span class="text-xs text-muted-foreground">${escapeHtml(command.hint)}</span></button>`;
    }).join("");
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
