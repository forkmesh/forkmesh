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
    "ownWorld", "listOnHomepage",
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
  $("[data-start-config-copy]").addEventListener("click", async (event) => {
    const button = event.currentTarget;
    try {
      const response = await fetch($("[data-start-config-example]").getAttribute("href"), { cache: "no-store" });
      if (!response.ok) throw new Error(String(response.status));
      await navigator.clipboard.writeText(await response.text());
      button.textContent = "Copied";
    } catch {
      button.textContent = "Could not copy: use Download example";
    }
    setTimeout(() => {
      button.textContent = "Copy example";
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
      theme: value("theme"),
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
    $("[data-start-done-text]").textContent = `Sign in as @${launch.admin || "your admin"}. Taking you to ${url} in 5 seconds…`;
    clearTimeout(redirectTimer);
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
