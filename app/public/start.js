(() => {
  const $ = (selector, root = document) => root.querySelector(selector);
  const form = $("[data-start-form]");
  if (!form) return;
  const progress = $("[data-start-progress]");
  const errorBox = $("[data-start-error]");
  const launchButton = $("[data-start-launch]");
  const field = (name) => form.elements.namedItem(name);
  const SECRET_FIELDS = ["sshPassword", "sshPrivateKey", "sshPassphrase", "adminPassword", "mailApiKey"];
  const KEY_LABELS = { mailtrap: "Mailtrap API token", sendgrid: "SendGrid API key", mailgun: "Mailgun API key" };
  const STATE_MARKS = { done: "✓", failed: "!", skipped: "–" };
  const POLL_MS = 1000;
  let pollTimer = 0;
  let clockTimer = 0;
  let startedAt = 0;
  let redirectTimer = 0;
  let launchedAdminPath = "";
  const ADMIN_PATH_ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789";
  const ADMIN_PATH_RE = /^\/[A-Za-z0-9_-]{8,64}$/;
  const newAdminPath = () => {
    const chars = [];
    while (chars.length < 20) {
      for (const byte of crypto.getRandomValues(new Uint8Array(32))) {
        if (byte < 252 && chars.length < 20) chars.push(ADMIN_PATH_ALPHABET[byte % 36]);
      }
    }
    return `/admin-${chars.join("")}`;
  };
  const copyText = async (button, text, label) => {
    try {
      await navigator.clipboard.writeText(text);
      button.textContent = "Copied";
    } catch {
      button.textContent = "Select and copy it";
    }
    setTimeout(() => {
      button.textContent = label;
    }, 2000);
  };

  const params = new URLSearchParams(location.search);
  const plot = (params.get("plot") || "").replace(/[^0-9A-Za-z-]/g, "").slice(0, 12);
  if (plot) {
    const banner = $("[data-start-plot]");
    banner.textContent = `You came from plot ${plot} in the World.`;
    banner.hidden = false;
  }

  const slug = (value) =>
    value.toLowerCase().trim().replace(/[^a-z0-9-]+/g, "-").replace(/^-+|-+$/g, "").replace(/^[0-9-]+/, "").slice(0, 39);

  const updatePreview = () => {
    const name = field("projectName").value.trim() || "Acme Code";
    const domain = field("domain").value.trim().toLowerCase();
    const host = field("host").value.trim();
    $("[data-start-preview-name]").textContent = name;
    $("[data-start-preview-host]").textContent = domain
      ? `at ${domain}`
      : host
        ? `at ${host.replace(/[.:]/g, "-")}.sslip.io`
        : "";
    const ownerType = form.querySelector("input[name=ownerType]:checked").value;
    $("[data-start-owner-org]").hidden = ownerType !== "org";
    $("[data-start-owner-others]").hidden = ownerType !== "org";
    const owner = ownerType === "org" ? field("ownerName").value.trim() : field("adminUsername").value.trim();
    $("[data-start-theme-owner]").textContent = owner.toLowerCase() || "acme";
    const base = domain || (host ? `${host.replace(/[.:]/g, "-")}.sslip.io` : "your-domain");
    $("[data-start-repo-preview]").textContent = owner
      ? `Your repository: https://${base}/${owner.toLowerCase()}/forkmesh`
      : "";
    const help = $("[data-start-domain-help]");
    help.innerHTML = domain
      ? `At your DNS provider, add an <b>A</b> record: <code>${escapeHtml(domain)}</code> → <code>${escapeHtml(host || "your server IP")}</code>. Setup waits for it, then gets an HTTPS certificate. On Cloudflare the proxy can stay on; set SSL/TLS to <b>Full (strict)</b>.`
      : "Without a domain you start on a free <code>sslip.io</code> address that points at your server. You can move to your own domain later.";
  };

  const escapeHtml = (value) =>
    String(value).replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch]);

  let ownerTouched = false;
  field("projectName").addEventListener("input", () => {
    if (!ownerTouched) field("ownerName").value = slug(field("projectName").value);
    updatePreview();
  });
  field("ownerName").addEventListener("input", () => {
    ownerTouched = true;
    updatePreview();
  });
  for (const name of ["domain", "host", "adminUsername"]) field(name).addEventListener("input", updatePreview);
  for (const radio of form.querySelectorAll("input[name=ownerType]")) radio.addEventListener("change", updatePreview);

  const authKind = () => form.querySelector("input[name=authKind]:checked").value;
  const syncAuth = () => {
    for (const panel of form.querySelectorAll("[data-start-auth]")) panel.hidden = panel.dataset.startAuth !== authKind();
  };
  for (const radio of form.querySelectorAll("input[name=authKind]")) radio.addEventListener("change", syncAuth);

  const syncMail = () => {
    const provider = field("mailProvider").value;
    $("[data-start-mail]").hidden = provider === "none";
    $("[data-start-mailgun]").hidden = provider !== "mailgun";
    $("[data-start-mail-key-label]").textContent = KEY_LABELS[provider] || "API key";
  };
  field("mailProvider").addEventListener("change", syncMail);

  const keyFile = $("[data-start-key-file]");
  const drop = $("[data-start-key-drop]");
  const readKey = (file) => {
    if (!file) return;
    if (file.size > 16000) {
      showError("That file is too large to be an SSH private key.", "sshPrivateKey");
      return;
    }
    file.text().then((text) => {
      field("sshPrivateKey").value = text.trim() + "\n";
      $("[data-start-key-name]").textContent = `Loaded ${file.name}`;
      clearError();
    });
  };
  $("[data-start-key-pick]").addEventListener("click", () => keyFile.click());
  keyFile.addEventListener("change", () => readKey(keyFile.files[0]));
  drop.addEventListener("dragover", (event) => {
    event.preventDefault();
    drop.classList.add("dragging");
  });
  drop.addEventListener("dragleave", () => drop.classList.remove("dragging"));
  drop.addEventListener("drop", (event) => {
    event.preventDefault();
    drop.classList.remove("dragging");
    readKey(event.dataTransfer.files[0]);
  });

  const LOGO_TYPES = ["image/png", "image/jpeg", "image/webp", "image/svg+xml"];
  const MAX_LOGO_BYTES = 200 * 1024;
  const LOGO_HINT = "or drop it here";
  const logoFile = $("[data-start-logo-file]");
  const logoDrop = $("[data-start-logo-drop]");
  const showLogo = (label) => {
    const value = field("logo").value;
    for (const image of [$("[data-start-logo-preview]"), $("[data-start-preview-logo]")]) {
      image.hidden = !value;
      if (value) image.src = value;
      else image.removeAttribute("src");
    }
    $("[data-start-theme-default-logo]").hidden = Boolean(value);
    $("[data-start-logo-clear]").hidden = !value;
    $("[data-start-logo-name]").textContent = value ? label || "Logo loaded" : LOGO_HINT;
  };
  const readLogo = (file) => {
    if (!file) return;
    logoFile.value = "";
    if (!LOGO_TYPES.includes(file.type)) {
      showError("The logo must be a PNG, JPEG, WebP or SVG image.", "logo");
      return;
    }
    if (file.size > MAX_LOGO_BYTES) {
      showError("The logo is larger than 200 KB. Export a smaller version and try again.", "logo");
      return;
    }
    const reader = new FileReader();
    reader.addEventListener("load", () => {
      field("logo").value = String(reader.result || "");
      showLogo(`Loaded ${file.name}`);
      clearError();
    });
    reader.readAsDataURL(file);
  };
  $("[data-start-logo-pick]").addEventListener("click", () => logoFile.click());
  logoFile.addEventListener("change", () => readLogo(logoFile.files[0]));
  $("[data-start-logo-clear]").addEventListener("click", () => {
    field("logo").value = "";
    showLogo();
  });
  logoDrop.addEventListener("dragover", (event) => {
    event.preventDefault();
    logoDrop.classList.add("dragging");
  });
  logoDrop.addEventListener("dragleave", () => logoDrop.classList.remove("dragging"));
  logoDrop.addEventListener("drop", (event) => {
    event.preventDefault();
    logoDrop.classList.remove("dragging");
    readLogo(event.dataTransfer.files[0]);
  });

  const THEME_MODES = ["dark", "light"];
  const THEME_GROUPS = [
    ["Surfaces", ["background", "header", "sidebar", "card", "popover", "canvas", "subtle", "muted", "secondary"]],
    ["Text", ["foreground", "card-foreground", "popover-foreground", "secondary-foreground", "muted-foreground"]],
    ["Accent", ["primary", "primary-foreground", "accent", "accent-foreground", "ring", "link", "link-hover", "tab-underline"]],
    ["Status and lines", ["border", "destructive", "open", "scrollbar"]],
  ];
  const THEME_TOKENS = THEME_GROUPS.flatMap(([, names]) => names);
  const ACCENT_TOKENS = THEME_GROUPS[2][1];
  const NEUTRAL_TOKENS = THEME_TOKENS.filter((name) => !ACCENT_TOKENS.includes(name));
  const THEME_STATUS = {
    dark: { destructive: "#f85149", open: "#3fb950" },
    light: { destructive: "#dc2626", open: "#1a7f37" },
  };
  const NEUTRALS = {
    slate: {
      label: "Slate",
      dark: { header: "#010409", sidebar: "#0c1117", background: "#010409", foreground: "#f0f6fc", card: "#0d1117", "card-foreground": "#f0f6fc", popover: "#161b22", "popover-foreground": "#f0f6fc", secondary: "#21262d", "secondary-foreground": "#c9d1d9", muted: "#161b22", "muted-foreground": "#9198a1", border: "#3d444d", canvas: "#0d1117", subtle: "#151b23", scrollbar: "#8b949e" },
      light: { header: "#f6f8fa", sidebar: "#ffffff", background: "#ffffff", foreground: "#000000", card: "#ffffff", "card-foreground": "#000000", popover: "#ffffff", "popover-foreground": "#000000", secondary: "#ffffff", "secondary-foreground": "#1f2328", muted: "#ffffff", "muted-foreground": "#59636e", border: "#e2e8f0", canvas: "#ffffff", subtle: "#f6f8fa", scrollbar: "#475569" },
    },
    zinc: {
      label: "Zinc",
      dark: { header: "#09090b", sidebar: "#18181b", background: "#09090b", foreground: "#f4f4f5", card: "#18181b", "card-foreground": "#f4f4f5", popover: "#27272a", "popover-foreground": "#f4f4f5", secondary: "#27272a", "secondary-foreground": "#e4e4e7", muted: "#27272a", "muted-foreground": "#a1a1aa", border: "#27272a", canvas: "#18181b", subtle: "#18181b", scrollbar: "#a1a1aa" },
      light: { header: "#ffffff", sidebar: "#f9fafb", background: "#ffffff", foreground: "#111827", card: "#ffffff", "card-foreground": "#111827", popover: "#ffffff", "popover-foreground": "#111827", secondary: "#f9fafb", "secondary-foreground": "#111827", muted: "#f9fafb", "muted-foreground": "#4b5563", border: "#e5e7eb", canvas: "#ffffff", subtle: "#f9fafb", scrollbar: "#71717a" },
    },
    stone: {
      label: "Stone",
      dark: { header: "#0c0a09", sidebar: "#1c1917", background: "#0c0a09", foreground: "#f5f5f4", card: "#1c1917", "card-foreground": "#f5f5f4", popover: "#292524", "popover-foreground": "#f5f5f4", secondary: "#292524", "secondary-foreground": "#e7e5e4", muted: "#292524", "muted-foreground": "#a8a29e", border: "#44403c", canvas: "#1c1917", subtle: "#1c1917", scrollbar: "#a8a29e" },
      light: { header: "#fafaf9", sidebar: "#f5f5f4", background: "#ffffff", foreground: "#1c1917", card: "#ffffff", "card-foreground": "#1c1917", popover: "#ffffff", "popover-foreground": "#1c1917", secondary: "#f5f5f4", "secondary-foreground": "#292524", muted: "#f5f5f4", "muted-foreground": "#57534e", border: "#e7e5e4", canvas: "#ffffff", subtle: "#fafaf9", scrollbar: "#78716c" },
    },
  };
  const ACCENTS = [
    {
      id: "forkmesh", label: "ForkMesh blue", dark: "#4493f8", light: "#0969da",
      extra: {
        dark: { accent: "#58a6ff", ring: "#58a6ff", link: "#58a6ff", "link-hover": "#79c0ff", "tab-underline": "#f78166" },
        light: { link: "#0969da", "link-hover": "#0550ae", "tab-underline": "#fd8c73" },
      },
    },
    { id: "crimson", label: "Crimson", dark: "#ef4444", light: "#dc2626" },
    { id: "orange", label: "Orange", dark: "#f97316", light: "#ea580c" },
    { id: "amber", label: "Amber", dark: "#f59e0b", light: "#d97706" },
    { id: "green", label: "Green", dark: "#22c55e", light: "#16a34a" },
    { id: "teal", label: "Teal", dark: "#14b8a6", light: "#0d9488" },
    { id: "violet", label: "Violet", dark: "#8b5cf6", light: "#7c3aed" },
    { id: "pink", label: "Pink", dark: "#ec4899", light: "#db2777" },
  ];
  const THEME_PRESETS = { crimson: { neutral: "zinc", accent: "crimson" } };
  const HEX_RE = /^#[0-9a-f]{6}$/;

  const hexRgb = (hex) => [1, 3, 5].map((index) => parseInt(hex.slice(index, index + 2), 16));
  const rgbHex = (rgb) => "#" + rgb.map((part) => Math.round(Math.min(255, Math.max(0, part))).toString(16).padStart(2, "0")).join("");
  const mixHex = (a, b, amount) => {
    const [from, to] = [hexRgb(a), hexRgb(b)];
    return rgbHex(from.map((part, index) => part + (to[index] - part) * amount));
  };
  const luminance = (hex) => {
    const [r, g, b] = hexRgb(hex).map((part) => {
      const value = part / 255;
      return value <= 0.03928 ? value / 12.92 : ((value + 0.055) / 1.055) ** 2.4;
    });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };
  const readableOn = (hex) => (luminance(hex) > 0.4 ? "#000000" : "#ffffff");
  const hslHex = (hue, saturation, lightness) => {
    const s = saturation / 100;
    const l = lightness / 100;
    const k = (n) => (n + hue / 30) % 12;
    const a = s * Math.min(l, 1 - l);
    return rgbHex([0, 8, 4].map((n) => 255 * (l - a * Math.max(-1, Math.min(k(n) - 3, 9 - k(n), 1)))));
  };

  const themeState = { neutral: "slate", accent: ACCENTS[0], loaded: null, preset: "", named: "", edits: { dark: {}, light: {} }, mode: "dark" };
  const themeCatalogue = [];
  const namedTheme = () => themeCatalogue.find((theme) => theme.id === themeState.named && theme.status === "ready");
  const accentTokens = (accent, mode) => {
    const colour = accent[mode];
    return {
      primary: colour,
      "primary-foreground": readableOn(colour),
      accent: colour,
      "accent-foreground": readableOn(colour),
      ring: colour,
      link: mode === "dark" ? mixHex(colour, "#ffffff", 0.2) : colour,
      "link-hover": mode === "dark" ? mixHex(colour, "#ffffff", 0.4) : mixHex(colour, "#000000", 0.2),
      "tab-underline": colour,
      ...((accent.extra || {})[mode] || {}),
    };
  };
  const currentTheme = () => {
    const theme = {};
    const named = namedTheme();
    if (named) {
      for (const mode of THEME_MODES) {
        const colours = Object.entries((named.colors || {})[mode] || {})
          .filter(([name, colour]) => THEME_TOKENS.includes(name) && HEX_RE.test(String(colour).toLowerCase()));
        theme[mode] = {
          ...NEUTRALS.slate[mode],
          ...THEME_STATUS[mode],
          ...accentTokens(ACCENTS[0], mode),
          ...Object.fromEntries(colours.map(([name, colour]) => [name, String(colour).toLowerCase()])),
        };
      }
      return theme;
    }
    for (const mode of THEME_MODES) {
      theme[mode] = {
        ...NEUTRALS[themeState.neutral][mode],
        ...THEME_STATUS[mode],
        ...accentTokens(themeState.accent, mode),
        ...((themeState.loaded || {})[mode] || {}),
        ...themeState.edits[mode],
      };
    }
    return theme;
  };
  const themeIsDefault = () =>
    themeState.neutral === "slate" && themeState.accent.id === "forkmesh" && !themeState.loaded &&
    THEME_MODES.every((mode) => !Object.keys(themeState.edits[mode]).length);
  const themeValue = () => {
    if (namedTheme()) return themeState.named;
    if (themeState.preset) return themeState.preset;
    if (themeIsDefault()) return "";
    return JSON.stringify(currentTheme());
  };

  const themeField = $(".theme-field");
  const cardBox = $("[data-start-theme-cards]");
  const accentBox = $("[data-start-theme-accents]");
  const neutralBox = $("[data-start-theme-neutrals]");
  const tokenBox = $("[data-start-theme-tokens]");
  const themePreview = $("[data-start-theme-preview]");
  const chip = (className, label, inner) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `theme-chip ${className}`;
    button.setAttribute("role", "radio");
    button.title = label;
    button.setAttribute("aria-label", label);
    button.innerHTML = inner;
    accentBox.append(button);
    return button;
  };
  for (const accent of ACCENTS) {
    const button = chip("", accent.label, `<span class="swatch-dot" style="--swatch:${accent.dark}"></span>`);
    button.dataset.accent = accent.id;
    button.addEventListener("click", () => chooseAccent(accent));
  }
  const customChip = chip("theme-custom", "Your own colour", '<span class="swatch-dot rainbow"></span><input type="color" value="#4493f8" aria-label="Pick your own accent colour" />');
  customChip.dataset.accent = "custom";
  const customInput = customChip.querySelector("input");
  customInput.addEventListener("input", () => {
    const colour = customInput.value.toLowerCase();
    chooseAccent({ id: "custom", label: "Your own colour", dark: colour, light: luminance(colour) > 0.3 ? mixHex(colour, "#000000", 0.18) : colour });
  });
  for (const [id, neutral] of Object.entries(NEUTRALS)) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "theme-chip neutral";
    button.setAttribute("role", "radio");
    button.dataset.neutral = id;
    button.innerHTML = `<span class="swatch-dot split" style="--swatch-a:${neutral.dark.background};--swatch-b:${neutral.light.subtle}"></span>${neutral.label}`;
    button.addEventListener("click", () => chooseNeutral(id));
    neutralBox.append(button);
  }

  const tokenInputs = {};
  for (const [title, names] of THEME_GROUPS) {
    const heading = document.createElement("p");
    heading.className = "theme-group-title";
    heading.textContent = title;
    tokenBox.append(heading);
    for (const name of names) {
      const row = document.createElement("label");
      row.className = "theme-token";
      row.innerHTML = `<input type="color" aria-label="${name} colour" /><span>${name.replace(/-/g, " ")}</span><input type="text" maxlength="7" spellcheck="false" aria-label="${name} hex value" />`;
      const [picker, text] = row.querySelectorAll("input");
      const set = (value) => {
        const colour = value.trim().toLowerCase();
        if (!HEX_RE.test(colour)) return false;
        themeState.edits[themeState.mode][name] = colour;
        themeState.preset = "";
        renderTheme();
        return true;
      };
      picker.addEventListener("input", () => set(picker.value));
      text.addEventListener("input", () => {
        text.toggleAttribute("aria-invalid", !set(text.value.startsWith("#") ? text.value : "#" + text.value));
      });
      text.addEventListener("blur", () => {
        text.removeAttribute("aria-invalid");
        renderTheme();
      });
      tokenInputs[name] = { picker, text };
      tokenBox.append(row);
    }
  }

  const renderTheme = () => {
    const theme = currentTheme();
    const tokens = theme[themeState.mode];
    for (const name of THEME_TOKENS) themePreview.style.setProperty(`--t-${name}`, tokens[name]);
    themePreview.dataset.mode = themeState.mode;
    for (const [name, inputs] of Object.entries(tokenInputs)) {
      inputs.picker.value = tokens[name];
      if (document.activeElement !== inputs.text) inputs.text.value = tokens[name];
    }
    for (const button of accentBox.querySelectorAll("[data-accent]")) {
      button.setAttribute("aria-checked", String(button.dataset.accent === themeState.accent.id));
    }
    for (const button of neutralBox.querySelectorAll("[data-neutral]")) {
      button.setAttribute("aria-checked", String(button.dataset.neutral === themeState.neutral));
    }
    for (const button of document.querySelectorAll("[data-start-theme-mode]")) {
      button.setAttribute("aria-pressed", String(button.dataset.startThemeMode === themeState.mode));
    }
    $("[data-start-theme-mode-name]").textContent = themeState.mode;
    themeField.toggleAttribute("data-named", Boolean(namedTheme()));
    for (const card of cardBox.querySelectorAll("[data-theme-card]")) {
      card.setAttribute("aria-checked", String(card.dataset.themeCard === (namedTheme() ? themeState.named : "default")));
    }
    field("theme").value = themeValue();
  };
  const dropEdits = (names) => {
    for (const mode of THEME_MODES) for (const name of names) delete themeState.edits[mode][name];
  };
  const chooseAccent = (accent) => {
    themeState.named = "";
    themeState.accent = accent;
    themeState.loaded = null;
    themeState.preset = "";
    dropEdits(ACCENT_TOKENS);
    renderTheme();
  };
  const chooseNeutral = (id) => {
    themeState.named = "";
    themeState.neutral = id;
    themeState.loaded = null;
    themeState.preset = "";
    dropEdits(NEUTRAL_TOKENS);
    renderTheme();
  };
  const resetTheme = () => {
    Object.assign(themeState, { neutral: "slate", accent: ACCENTS[0], loaded: null, preset: "", named: "", edits: { dark: {}, light: {} } });
    renderTheme();
  };
  const setThemeFrom = (raw) => {
    if (raw === null || raw === undefined || raw === "") {
      resetTheme();
      return true;
    }
    if (typeof raw === "string") {
      const name = raw.trim().toLowerCase();
      if (name === "default") {
        resetTheme();
        return true;
      }
      if (themeCatalogue.some((theme) => theme.id === name && theme.status === "ready")) {
        chooseNamedTheme(name);
        return true;
      }
      const preset = THEME_PRESETS[raw.trim().toLowerCase()];
      if (!preset) return false;
      resetTheme();
      themeState.neutral = preset.neutral;
      themeState.accent = ACCENTS.find((accent) => accent.id === preset.accent);
      themeState.preset = raw.trim().toLowerCase();
      renderTheme();
      return true;
    }
    if (typeof raw !== "object" || Array.isArray(raw)) return false;
    const loaded = {};
    for (const [mode, tokens] of Object.entries(raw)) {
      if (!THEME_MODES.includes(mode) || !tokens || typeof tokens !== "object" || Array.isArray(tokens)) return false;
      loaded[mode] = {};
      for (const [name, colour] of Object.entries(tokens)) {
        const value = String(colour || "").trim().toLowerCase();
        if (!THEME_TOKENS.includes(name) || !HEX_RE.test(value)) return false;
        loaded[mode][name] = value;
      }
    }
    resetTheme();
    themeState.loaded = loaded;
    themeState.accent = { id: "loaded", label: "From your settings", dark: (loaded.dark || {}).primary || ACCENTS[0].dark, light: (loaded.light || {}).primary || ACCENTS[0].light };
    renderTheme();
    return true;
  };
  for (const button of document.querySelectorAll("[data-start-theme-mode]")) {
    button.addEventListener("click", () => {
      themeState.mode = button.dataset.startThemeMode;
      renderTheme();
    });
  }
  $("[data-start-theme-shuffle]").addEventListener("click", () => {
    const hue = Math.floor(Math.random() * 360);
    const names = Object.keys(NEUTRALS);
    themeState.neutral = names[Math.floor(Math.random() * names.length)];
    themeState.edits = { dark: {}, light: {} };
    const dark = hslHex(hue, 78, 60);
    customInput.value = dark;
    chooseAccent({ id: "custom", label: "Your own colour", dark, light: hslHex(hue, 72, 42) });
  });
  $("[data-start-theme-reset]").addEventListener("click", resetTheme);
  const chooseNamedTheme = (id) => {
    resetTheme();
    if (id !== "default") themeState.named = id;
    renderTheme();
  };
  const themeCard = (theme) => {
    const palette = (theme.palette || []).filter((colour) => HEX_RE.test(String(colour).toLowerCase()));
    const ready = theme.status === "ready";
    const card = document.createElement("button");
    card.type = "button";
    card.className = "theme-card";
    card.setAttribute("role", "radio");
    card.dataset.themeCard = theme.id;
    card.disabled = !ready;
    card.innerHTML = `
      <span class="theme-card-palette" aria-hidden="true">${palette.map((colour, index) => `<span style="background:${colour};flex:${index === 0 ? 3 : 1}"></span>`).join("")}</span>
      <span class="theme-card-name"><span></span>${ready ? "" : "<em>Coming soon</em>"}</span>
      <span class="theme-card-desc"></span>`;
    card.querySelector(".theme-card-name span").textContent = String(theme.name || theme.id);
    card.querySelector(".theme-card-desc").textContent = String(theme.description || "");
    card.addEventListener("click", () => chooseNamedTheme(theme.id));
    return card;
  };
  const themesLoaded = fetch("/instance-themes/themes.json", { headers: { accept: "application/json" } })
    .then((response) => (response.ok ? response.json() : null))
    .then((data) => {
      const themes = Array.isArray(data && data.themes) ? data.themes : [];
      themeCatalogue.push(...themes.filter((theme) => theme && /^[a-z][a-z0-9-]{0,40}$/.test(String(theme.id || ""))));
      cardBox.replaceChildren(...themeCatalogue.map(themeCard));
      renderTheme();
    })
    .catch(() => {});
  themeState.mode = document.documentElement.classList.contains("light") ? "light" : "dark";
  renderTheme();

  const MAX_OWNERS = 20;
  const ownerList = $("[data-start-owners]");
  const ownerTemplate = $("[data-start-owner-template]");
  const ownerRows = () => [...ownerList.querySelectorAll("[data-start-owner]")];
  const ownerInput = (row, name) => row.querySelector(`[data-owner="${name}"]`);
  const syncOwnerAdd = () => {
    $("[data-start-owner-add]").hidden = ownerRows().length >= MAX_OWNERS;
  };
  const addOwner = (owner = {}) => {
    if (ownerRows().length >= MAX_OWNERS) return null;
    const row = ownerTemplate.content.firstElementChild.cloneNode(true);
    for (const name of ["username", "email", "password"]) {
      const value = owner[name];
      if (value !== null && value !== undefined && typeof value !== "object") ownerInput(row, name).value = String(value);
    }
    row.querySelector("[data-owner-remove]").addEventListener("click", () => {
      row.remove();
      syncOwnerAdd();
    });
    ownerList.append(row);
    syncOwnerAdd();
    return row;
  };
  const setOwners = (owners) => {
    ownerList.replaceChildren();
    for (const owner of owners) if (owner && typeof owner === "object" && !Array.isArray(owner)) addOwner(owner);
    syncOwnerAdd();
  };
  $("[data-start-owner-add]").addEventListener("click", () => {
    const row = addOwner();
    if (row) ownerInput(row, "username").focus();
  });

  const CONFIG_FIELDS = [
    "projectName", "logo", "theme", "host", "sshUser", "port", "sshPassword", "sshPrivateKey", "sshPassphrase",
    "hostKeyFingerprint", "domain", "adminUsername", "adminEmail", "adminPassword", "ownerType",
    "ownerName", "owners", "mailProvider", "mailApiKey", "mailFrom", "mailgunDomain", "mailgunRegion",
    "ownWorld", "listOnHomepage", "adminPath",
  ];
  const configFile = $("[data-start-config-file]");
  const configDrop = $("[data-start-config-drop]");
  const configStatus = $("[data-start-config-status]");
  const configLaunch = $("[data-start-config-launch]");
  const configNote = (message) => {
    configStatus.textContent = message;
    configStatus.hidden = !message;
  };

  const setChoice = (name, value) => {
    const radio = [...form.querySelectorAll(`input[name=${name}]`)].find((input) => input.value === value);
    if (radio) radio.checked = true;
    return Boolean(radio);
  };

  const applyConfig = (settings) => {
    const skipped = [];
    for (const [key, raw] of Object.entries(settings)) {
      if (/^[_$]/.test(key)) continue;
      if (key === "theme") {
        if (!setThemeFrom(raw)) skipped.push(key);
        continue;
      }
      if (key === "owners" && Array.isArray(raw) && raw.length <= MAX_OWNERS) {
        setOwners(raw);
        continue;
      }
      if (!CONFIG_FIELDS.includes(key) || (raw !== null && typeof raw === "object")) {
        skipped.push(key);
        continue;
      }
      const value = raw === null || raw === undefined ? "" : String(raw);
      if (!value.trim()) continue;
      if (key === "ownerType") {
        if (!setChoice("ownerType", value.trim().toLowerCase())) skipped.push(key);
        continue;
      }
      const input = field(key);
      if (input.type === "checkbox") {
        input.checked = !/^(false|no|off|0)$/i.test(value.trim());
        continue;
      }
      if (input.tagName === "SELECT") {
        const choice = value.trim().toLowerCase();
        if ([...input.options].some((option) => option.value === choice)) input.value = choice;
        else skipped.push(key);
        continue;
      }
      input.value = key === "sshPrivateKey" ? value.trim() + "\n" : value;
      if (key === "ownerName") ownerTouched = true;
    }
    if (!ownerTouched) field("ownerName").value = slug(field("projectName").value);
    if (field("sshPrivateKey").value.trim()) setChoice("authKind", "key");
    else if (field("sshPassword").value) setChoice("authKind", "password");
    if (field("hostKeyFingerprint").value) field("hostKeyFingerprint").closest("details").open = true;
    if (field("logo").value && !/^data:image\/(png|jpeg|webp|svg\+xml);base64,/.test(field("logo").value)) {
      field("logo").value = "";
      skipped.push("logo");
    }
    if ("logo" in settings) showLogo("Logo loaded from the settings file");
    syncAuth();
    syncMail();
    updatePreview();
    return skipped;
  };

  const MAX_CONFIG_BYTES = 400000;
  const MAX_ZIP_BYTES = 1024 * 1024;
  const IMAGE_TYPES = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", webp: "image/webp", svg: "image/svg+xml" };

  const zipEntries = (bytes) => {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let end = -1;
    for (let at = bytes.length - 22; at >= Math.max(0, bytes.length - 22 - 65535); at -= 1) {
      if (view.getUint32(at, true) === 0x06054b50) {
        end = at;
        break;
      }
    }
    if (end < 0) return null;
    const count = view.getUint16(end + 10, true);
    let at = view.getUint32(end + 16, true);
    const entries = [];
    for (let index = 0; index < count; index += 1) {
      if (at + 46 > bytes.length || view.getUint32(at, true) !== 0x02014b50) return null;
      const nameLength = view.getUint16(at + 28, true);
      const skip = nameLength + view.getUint16(at + 30, true) + view.getUint16(at + 32, true);
      entries.push({
        path: new TextDecoder().decode(bytes.subarray(at + 46, at + 46 + nameLength)),
        flags: view.getUint16(at + 8, true),
        method: view.getUint16(at + 10, true),
        packed: view.getUint32(at + 20, true),
        size: view.getUint32(at + 24, true),
        offset: view.getUint32(at + 42, true),
      });
      at += 46 + skip;
    }
    return { bytes, view, entries };
  };

  const zipRead = async (zip, entry, limit) => {
    if (entry.flags & 1) throw new Error(`${entry.path} is encrypted. Make the zip without a password.`);
    if (entry.size > limit) throw new Error(`${entry.path} is too large.`);
    const head = entry.offset;
    if (head + 30 > zip.bytes.length || zip.view.getUint32(head, true) !== 0x04034b50) throw new Error("The zip file is damaged.");
    const start = head + 30 + zip.view.getUint16(head + 26, true) + zip.view.getUint16(head + 28, true);
    const packed = zip.bytes.subarray(start, start + entry.packed);
    if (entry.method === 0) return packed.slice(0, limit);
    if (entry.method !== 8 || typeof DecompressionStream !== "function") {
      throw new Error(`${entry.path} is packed in a way this browser cannot open. Make the zip again with standard compression.`);
    }
    const reader = new Blob([packed]).stream().pipeThrough(new DecompressionStream("deflate-raw")).getReader();
    const chunks = [];
    let total = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.length;
      if (total > limit) {
        reader.cancel();
        throw new Error(`${entry.path} is too large.`);
      }
      chunks.push(value);
    }
    const out = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      out.set(chunk, offset);
      offset += chunk.length;
    }
    return out;
  };

  const dataUrl = (bytes, type) =>
    new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.addEventListener("load", () => resolve(String(reader.result || "")));
      reader.addEventListener("error", () => reject(new Error("The logo could not be read.")));
      reader.readAsDataURL(new Blob([bytes], { type }));
    });

  const baseName = (path) => path.split("/").pop();
  const extension = (path) => (baseName(path).match(/\.([A-Za-z0-9]+)$/) || ["", ""])[1].toLowerCase();

  const parseSettings = (text, name) => {
    let settings = null;
    try {
      settings = JSON.parse(text);
    } catch {
      settings = null;
    }
    if (!settings || typeof settings !== "object" || Array.isArray(settings)) {
      throw new Error(`${name} is not a JSON settings file. Compare it with the example.`);
    }
    return settings;
  };

  const readZip = async (file) => {
    const zip = zipEntries(new Uint8Array(await file.arrayBuffer()));
    if (!zip) throw new Error(`${file.name} is not a zip file this page can open.`);
    const files = zip.entries.filter(
      (entry) => !entry.path.endsWith("/") && !entry.path.startsWith("__MACOSX/") && !baseName(entry.path).startsWith("."),
    );
    const jsons = files.filter((entry) => extension(entry.path) === "json");
    if (jsons.length !== 1) {
      throw new Error(`${file.name} needs exactly one settings .json file; it has ${jsons.length}.`);
    }
    const settings = parseSettings(new TextDecoder().decode(await zipRead(zip, jsons[0], MAX_CONFIG_BYTES)), baseName(jsons[0].path));
    const images = files.filter((entry) => IMAGE_TYPES[extension(entry.path)]);
    const named = typeof settings.logo === "string" && settings.logo.trim() && !settings.logo.trim().startsWith("data:")
      ? settings.logo.trim().replace(/^\.\//, "")
      : "";
    let logo = null;
    if (named) {
      logo = images.find((entry) => entry.path === named) || images.find((entry) => baseName(entry.path) === baseName(named));
      if (!logo) throw new Error(`The settings name the logo ${named}, but ${file.name} has no image by that name.`);
    } else if (!settings.logo && images.length === 1) {
      logo = images[0];
    } else if (!settings.logo && images.length > 1) {
      throw new Error(`${file.name} has ${images.length} images. Name the logo in the settings, for example "logo": "${baseName(images[0].path)}".`);
    }
    if (logo) {
      if (logo.size > MAX_LOGO_BYTES) throw new Error(`The logo ${baseName(logo.path)} is larger than 200 KB. Export a smaller version.`);
      settings.logo = await dataUrl(await zipRead(zip, logo, MAX_LOGO_BYTES), IMAGE_TYPES[extension(logo.path)]);
    }
    return { settings, logoName: logo ? baseName(logo.path) : "" };
  };

  const readConfig = async (file) => {
    if (!file) return;
    configFile.value = "";
    configLaunch.hidden = true;
    const zipped = extension(file.name) === "zip" || /zip/.test(file.type);
    if (file.size > (zipped ? MAX_ZIP_BYTES : MAX_CONFIG_BYTES)) {
      configNote("That file is too large to be a settings file.");
      return;
    }
    let loaded = null;
    try {
      loaded = zipped ? await readZip(file) : { settings: parseSettings(await file.text(), file.name), logoName: "" };
    } catch (error) {
      configNote(error.message || `${file.name} could not be read.`);
      return;
    }
    const { settings, logoName } = loaded;
    let logoMissing = "";
    if (typeof settings.logo === "string" && settings.logo.trim() && !settings.logo.trim().startsWith("data:")) {
      logoMissing = ` The logo ${settings.logo.trim()} was not loaded: upload the zip with the settings and the logo together.`;
      delete settings.logo;
    }
    clearError();
    if (typeof settings.theme === "string") await themesLoaded;
    const skipped = applyConfig(settings);
    if (logoName) showLogo(`Loaded ${logoName} from ${file.name}`);
    $("[data-start-config-name]").textContent = `Loaded ${file.name}`;
    const ignored = (skipped.length ? ` Skipped settings this page does not know: ${skipped.join(", ")}.` : "") + logoMissing;
    const problem = localCheck(collect());
    if (problem) {
      configNote(`Settings loaded from ${file.name}. One thing is still missing below.${ignored}`);
      showError(problem[0], problem[1]);
      return;
    }
    configNote(`Every setting is filled in from ${file.name}. Check them below, or start now.${ignored}`);
    configLaunch.hidden = false;
    configLaunch.focus();
  };
  $("[data-start-config-pick]").addEventListener("click", () => configFile.click());
  configFile.addEventListener("change", () => readConfig(configFile.files[0]));
  configDrop.addEventListener("dragover", (event) => {
    event.preventDefault();
    configDrop.classList.add("dragging");
  });
  configDrop.addEventListener("dragleave", () => configDrop.classList.remove("dragging"));
  configDrop.addEventListener("drop", (event) => {
    event.preventDefault();
    configDrop.classList.remove("dragging");
    readConfig(event.dataTransfer.files[0]);
  });
  configLaunch.addEventListener("click", () => form.requestSubmit());
  if (!field("adminPath").value) field("adminPath").value = newAdminPath();
  $("[data-start-admin-path-new]").addEventListener("click", () => {
    field("adminPath").value = newAdminPath();
  });
  $("[data-start-admin-path-copy]").addEventListener("click", (event) =>
    copyText(event.currentTarget, field("adminPath").value.trim(), "Copy"),
  );
  $("[data-start-admin-path-note-copy]").addEventListener("click", (event) =>
    copyText(event.currentTarget, $("[data-start-admin-path-url]").textContent, "Copy"),
  );

  const CRC_TABLE = Array.from({ length: 256 }, (_, n) => {
    let c = n;
    for (let k = 0; k < 8; k += 1) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    return c >>> 0;
  });
  const crc32 = (bytes) => {
    let c = 0xffffffff;
    for (const byte of bytes) c = CRC_TABLE[(c ^ byte) & 0xff] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  };
  const storedZip = (files) => {
    const parts = [];
    const central = [];
    let offset = 0;
    for (const file of files) {
      const name = new TextEncoder().encode(file.name);
      const crc = crc32(file.bytes);
      const local = new DataView(new ArrayBuffer(30));
      local.setUint32(0, 0x04034b50, true);
      local.setUint16(4, 20, true);
      local.setUint16(8, 0, true);
      local.setUint32(14, crc, true);
      local.setUint32(18, file.bytes.length, true);
      local.setUint32(22, file.bytes.length, true);
      local.setUint16(26, name.length, true);
      parts.push(new Uint8Array(local.buffer), name, file.bytes);
      const entry = new DataView(new ArrayBuffer(46));
      entry.setUint32(0, 0x02014b50, true);
      entry.setUint16(4, 20, true);
      entry.setUint16(6, 20, true);
      entry.setUint32(16, crc, true);
      entry.setUint32(20, file.bytes.length, true);
      entry.setUint32(24, file.bytes.length, true);
      entry.setUint16(28, name.length, true);
      entry.setUint32(42, offset, true);
      central.push(new Uint8Array(entry.buffer), name);
      offset += 30 + name.length + file.bytes.length;
    }
    const size = central.reduce((total, part) => total + part.length, 0);
    const end = new DataView(new ArrayBuffer(22));
    end.setUint32(0, 0x06054b50, true);
    end.setUint16(8, files.length, true);
    end.setUint16(10, files.length, true);
    end.setUint32(12, size, true);
    end.setUint32(16, offset, true);
    return new Blob([...parts, ...central, new Uint8Array(end.buffer)], { type: "application/zip" });
  };
  const saveBlob = (blob, name) => {
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
  };
  const withAdminPath = (text) => {
    const settings = JSON.parse(text);
    settings.adminPath = newAdminPath();
    return `${JSON.stringify(settings, null, 2)}\n`;
  };
  $("[data-start-config-zip]").addEventListener("click", async (event) => {
    const link = event.currentTarget;
    event.preventDefault();
    try {
      const response = await fetch(link.getAttribute("href"), { cache: "no-store" });
      if (!response.ok) throw new Error(String(response.status));
      const zip = zipEntries(new Uint8Array(await response.arrayBuffer()));
      if (!zip) throw new Error("zip");
      const files = [];
      for (const entry of zip.entries) {
        if (entry.path.endsWith("/")) continue;
        let bytes = await zipRead(zip, entry, MAX_ZIP_BYTES);
        if (extension(entry.path) === "json") bytes = new TextEncoder().encode(withAdminPath(new TextDecoder().decode(bytes)));
        files.push({ name: entry.path, bytes });
      }
      saveBlob(storedZip(files), link.getAttribute("download"));
    } catch {
      location.assign(link.getAttribute("href"));
    }
  });
  $("[data-start-config-example]").addEventListener("click", async (event) => {
    const link = event.currentTarget;
    event.preventDefault();
    try {
      const response = await fetch(link.getAttribute("href"), { cache: "no-store" });
      if (!response.ok) throw new Error(String(response.status));
      saveBlob(new Blob([withAdminPath(await response.text())], { type: "application/json" }), link.getAttribute("download"));
    } catch {
      location.assign(link.getAttribute("href"));
    }
  });
  $("[data-start-config-copy]").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    try {
      const response = await fetch($("[data-start-config-example]").getAttribute("href"), { cache: "no-store" });
      if (!response.ok) throw new Error(String(response.status));
      await navigator.clipboard.writeText(withAdminPath(await response.text()));
      button.textContent = "Copied";
    } catch {
      button.textContent = "Could not copy: use Download example";
    }
    setTimeout(() => {
      button.textContent = "Copy example";
    }, 2000);
  });
  $("[data-start-agent-copy]").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    const prompt = $("[data-start-agent-prompt]");
    try {
      await navigator.clipboard.writeText(prompt.value);
      button.textContent = "Copied";
    } catch {
      prompt.select();
      button.textContent = "Press Ctrl+C";
    }
    setTimeout(() => {
      button.textContent = "Copy";
    }, 2000);
  });

  const clearError = () => {
    errorBox.hidden = true;
    errorBox.textContent = "";
    for (const element of form.querySelectorAll("[aria-invalid]")) element.removeAttribute("aria-invalid");
  };

  const showError = (message, name) => {
    errorBox.textContent = message;
    errorBox.hidden = false;
    const input = typeof name === "string" ? (name ? field(name) : null) : name;
    if (input && input.focus) {
      input.setAttribute("aria-invalid", "true");
      const details = input.closest("details");
      if (details) details.open = true;
      input.focus();
    }
  };

  const localCheck = (body) => {
    if (!body.projectName) return ["Give your project a name.", "projectName"];
    if (!body.host) return ["Enter your server's IP address.", "host"];
    if (authKind() === "password" && !body.sshPassword) return ["Enter the server's SSH password.", "sshPassword"];
    if (authKind() === "key" && !body.sshPrivateKey) return ["Upload or paste the server's private key.", "sshPrivateKey"];
    if (!/^[a-z][a-z0-9-]*$/.test(body.adminUsername)) return ["Pick an admin username: lowercase letters, numbers and dashes.", "adminUsername"];
    if (!body.adminEmail.includes("@")) return ["Enter the admin's email address.", "adminEmail"];
    if (body.adminPassword.length < 10) return ["The admin password needs at least 10 characters.", "adminPassword"];
    if (!ADMIN_PATH_RE.test(body.adminPath)) {
      return ["The admin console address is a / and then 8 to 64 letters, numbers, - or _. Press New for a fresh one.", "adminPath"];
    }
    if (body.ownerType === "org" && !/^[a-z][a-z0-9-]*$/.test(body.ownerName)) return ["Name the organization: lowercase letters, numbers and dashes.", "ownerName"];
    const names = new Set([body.adminUsername, body.ownerName]);
    const emails = new Set([body.adminEmail.toLowerCase()]);
    const rows = ownerRows();
    for (const [index, owner] of body.owners.entries()) {
      const row = rows[index];
      if (!/^[a-z][a-z0-9-]*$/.test(owner.username)) return ["Give each other owner a username: lowercase letters, numbers and dashes.", ownerInput(row, "username")];
      if (names.has(owner.username)) return [`@${owner.username} is listed twice, or is the admin or the organization.`, ownerInput(row, "username")];
      if (!owner.email.includes("@") || emails.has(owner.email.toLowerCase())) return [`Give @${owner.username} an email address of their own.`, ownerInput(row, "email")];
      if (owner.password.length < 10) return [`The password for @${owner.username} needs at least 10 characters.`, ownerInput(row, "password")];
      names.add(owner.username);
      emails.add(owner.email.toLowerCase());
    }
    if (body.mailProvider !== "none" && !body.mailApiKey) return ["Paste the API key for your email service, or choose no email.", "mailApiKey"];
    if (body.mailProvider === "mailgun" && !body.mailgunDomain) return ["Mailgun needs your sending domain.", "mailgunDomain"];
    return null;
  };

  const collect = () => {
    const value = (name) => (field(name) ? String(field(name).value || "") : "");
    const kind = authKind();
    const ownerType = form.querySelector("input[name=ownerType]:checked").value;
    return {
      projectName: value("projectName").trim(),
      logo: value("logo"),
      theme: value("theme").startsWith("{") ? JSON.parse(value("theme")) : value("theme"),
      host: value("host").trim(),
      port: Number(value("port") || 22),
      sshUser: value("sshUser").trim() || "root",
      sshPassword: kind === "password" ? value("sshPassword") : "",
      sshPrivateKey: kind === "key" ? value("sshPrivateKey") : "",
      sshPassphrase: kind === "key" ? value("sshPassphrase") : "",
      hostKeyFingerprint: value("hostKeyFingerprint").trim(),
      domain: value("domain").trim().toLowerCase(),
      adminUsername: value("adminUsername").trim().toLowerCase(),
      adminEmail: value("adminEmail").trim(),
      adminPassword: value("adminPassword"),
      adminPath: value("adminPath").trim(),
      ownerType,
      ownerName: value("ownerName").trim().toLowerCase(),
      owners: ownerType === "org"
        ? ownerRows()
            .map((row) => ({
              username: ownerInput(row, "username").value.trim().toLowerCase(),
              email: ownerInput(row, "email").value.trim(),
              password: ownerInput(row, "password").value,
            }))
            .filter((owner) => owner.username || owner.email || owner.password)
        : [],
      mailProvider: value("mailProvider"),
      mailApiKey: value("mailApiKey").trim(),
      mailFrom: value("mailFrom").trim(),
      mailgunDomain: value("mailgunDomain").trim(),
      mailgunRegion: value("mailgunRegion"),
      ownWorld: field("ownWorld").checked,
      listOnHomepage: field("listOnHomepage").checked,
    };
  };

  let lastLaunch = null;
  const refillFrom = (launch) => {
    if (!launch || field("projectName").value.trim()) return;
    const domain = String(launch.domain || "");
    applyConfig({
      projectName: launch.projectName,
      host: launch.address,
      port: launch.port,
      domain: domain.endsWith(".sslip.io") ? "" : domain,
      adminUsername: launch.admin,
      ownerType: launch.ownerType,
      ownerName: launch.ownerType === "org" ? launch.owner : "",
    });
  };

  const clearSecrets = () => {
    for (const name of SECRET_FIELDS) if (field(name)) field(name).value = "";
    for (const row of ownerRows()) ownerInput(row, "password").value = "";
    keyFile.value = "";
    $("[data-start-key-name]").textContent = "or drop it here, or paste it below";
    configFile.value = "";
    configLaunch.hidden = true;
    configNote("");
    $("[data-start-config-name]").textContent = "or drop it here";
  };
  addEventListener("pagehide", clearSecrets);

  form.addEventListener("submit", async (event) => {
    event.preventDefault();
    clearError();
    const body = collect();
    const problem = localCheck(body);
    if (problem) {
      showError(problem[0], problem[1]);
      return;
    }
    launchButton.disabled = true;
    configLaunch.disabled = true;
    launchButton.textContent = "Connecting to your server…";
    let response;
    let data = {};
    try {
      response = await fetch("/api/start/launch", {
        method: "POST",
        credentials: "same-origin",
        headers: { "content-type": "application/json" },
        body: JSON.stringify(body),
      });
      data = await response.json().catch(() => ({}));
    } catch {
      data = { message: "ForkMesh could not be reached. Check your connection and try again." };
    } finally {
      for (const name of SECRET_FIELDS) body[name] = "";
      for (const owner of body.owners) owner.password = "";
      launchButton.disabled = false;
      configLaunch.disabled = false;
      launchButton.textContent = "Start my instance";
    }
    if (!response || response.status !== 202 || !data.id) {
      showError(data.message || "The setup could not start. Try again in a moment.", data.field || "");
      return;
    }
    clearSecrets();
    launchedAdminPath = typeof data.adminPath === "string" && ADMIN_PATH_RE.test(data.adminPath) ? data.adminPath : "";
    if (launchedAdminPath && data.domain) {
      $("[data-start-admin-path-url]").textContent = `https://${data.domain}${launchedAdminPath}`;
      $("[data-start-admin-path-note]").hidden = false;
    }
    history.replaceState(null, "", `${location.pathname}?launch=${data.id}${plot ? `&plot=${plot}` : ""}`);
    showProgress({ ...data, projectName: body.projectName, state: "running", log: "" });
    poll(data.id);
  });

  const renderSteps = (steps) => {
    const list = $("[data-start-steps]");
    list.replaceChildren(
      ...steps.map((step, index) => {
        const item = document.createElement("li");
        item.className = `step ${step.state}`;
        const dot = document.createElement("span");
        dot.className = "dot";
        dot.textContent = STATE_MARKS[step.state] || String(index + 1);
        dot.setAttribute("aria-hidden", "true");
        const label = document.createElement("span");
        label.textContent = step.title;
        item.append(dot, label);
        if (step.state === "running") item.setAttribute("aria-current", "step");
        return item;
      }),
    );
    const finished = steps.filter((step) => step.state === "done" || step.state === "skipped").length;
    const percent = Math.round((finished / Math.max(steps.length, 1)) * 100);
    const bar = $("[data-start-bar]");
    bar.firstElementChild.style.width = `${percent}%`;
    bar.setAttribute("aria-valuenow", String(percent));
  };

  const tick = () => {
    const seconds = Math.max(0, Math.floor((Date.now() - startedAt) / 1000));
    $("[data-start-elapsed]").textContent = `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, "0")}`;
  };

  const showProgress = (launch) => {
    form.hidden = true;
    progress.hidden = false;
    startedAt = launch.createdAt || Date.now();
    clearInterval(clockTimer);
    clockTimer = setInterval(tick, 1000);
    tick();
    render(launch);
    progress.scrollIntoView({ block: "start" });
  };

  const render = (launch) => {
    if (launch.id) lastLaunch = launch;
    if (launch.createdAt) startedAt = launch.createdAt;
    const name = launch.projectName || "your instance";
    $("[data-start-progress-title]").textContent =
      launch.state === "succeeded" ? `${name} is ready` : launch.state === "failed" ? `Setting up ${name} stopped` : `Setting up ${name}`;
    const parts = [];
    if (launch.domain) parts.push(`Address: <code>https://${escapeHtml(launch.domain)}</code>`);
    if (launch.hostKey && launch.hostKey.fingerprint) {
      parts.push(`Server key: <code>${escapeHtml(launch.hostKey.type)} ${escapeHtml(launch.hostKey.fingerprint)}</code>`);
    }
    $("[data-start-progress-sub]").innerHTML = parts.join(" · ");
    if (Array.isArray(launch.steps) && launch.steps.length) renderSteps(launch.steps);
    $("[data-start-status]").textContent = launch.message || "";
    const log = $("[data-start-log]");
    const text = launch.log || "Waiting for the first output from your server…";
    if (log.textContent !== text) {
      const atBottom = log.scrollHeight - log.scrollTop - log.clientHeight < 40;
      log.textContent = text;
      if (atBottom) log.scrollTop = log.scrollHeight;
    }
    if (launch.state === "succeeded") finishOk(launch);
    if (launch.state === "failed") finishBad(launch);
  };

  const stop = () => {
    clearTimeout(pollTimer);
    clearInterval(clockTimer);
  };

  const finishOk = (launch) => {
    stop();
    const url = launch.url || (launch.domain ? `https://${launch.domain}/` : "/");
    const done = $("[data-start-done]");
    done.hidden = false;
    $("[data-start-open]").href = url;
    clearTimeout(redirectTimer);
    if (launchedAdminPath) {
      $("[data-start-done-text]").textContent = `Sign in as @${launch.admin || "your admin"}. Save the admin console address above, then open ${url}.`;
      return;
    }
    $("[data-start-done-text]").textContent = `Sign in as @${launch.admin || "your admin"}. Taking you to ${url} in 5 seconds…`;
    redirectTimer = setTimeout(() => location.assign(url), 5000);
  };

  const finishBad = (launch) => {
    stop();
    $("[data-start-failed]").hidden = false;
    $("[data-start-failed-text]").textContent =
      launch.message || "The setup stopped. The log above shows the last thing it was doing.";
  };

  const poll = async (id) => {
    clearTimeout(pollTimer);
    let data = null;
    try {
      const response = await fetch(`/api/start/launches/${encodeURIComponent(id)}`, {
        credentials: "same-origin",
        cache: "no-store",
      });
      if (response.status === 404) {
        finishBad({ message: "This setup is no longer on record. Start again from the form." });
        return;
      }
      data = await response.json().catch(() => null);
    } catch {
      data = null;
    }
    if (data && data.launch) {
      render(data.launch);
      if (data.launch.state !== "running") return;
    }
    pollTimer = setTimeout(() => poll(id), POLL_MS);
  };

  $("[data-start-retry]").addEventListener("click", () => {
    stop();
    clearTimeout(redirectTimer);
    progress.hidden = true;
    $("[data-start-admin-path-note]").hidden = true;
    launchedAdminPath = "";
    $("[data-start-failed]").hidden = true;
    $("[data-start-done]").hidden = true;
    form.hidden = false;
    refillFrom(lastLaunch);
    history.replaceState(null, "", `${location.pathname}${plot ? `?plot=${plot}` : ""}`);
    field(authKind() === "key" ? "sshPrivateKey" : "sshPassword").focus();
  });

  syncAuth();
  syncMail();
  updatePreview();
  const resume = (params.get("launch") || "").toLowerCase();
  if (/^[0-9a-f]{32}$/.test(resume)) {
    showProgress({ state: "running", steps: [], log: "" });
    poll(resume);
  }
})();
