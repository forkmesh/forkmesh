(() => {
  const $ = (selector) => document.querySelector(selector);
  const PARAMS = ["response_type", "client_id", "redirect_uri", "scope", "state", "code_challenge", "code_challenge_method"];
  const ERRORS = {
    invalid_client: "This app is not registered, or it has been suspended.",
    redirect_uri_mismatch: "The app sent you back to an address it did not register, so ForkMesh stopped here.",
    invalid_scope: "The app asked for permissions it did not register.",
    invalid_request: "The app's sign-in request is incomplete (PKCE is required).",
    unsupported_response_type: "The app asked for an unsupported response type.",
    app_not_available: "This app is private to its developer and their organization.",
    not_org_member: "You are not a member of that organization.",
    org_requests_disabled: "This organization does not accept app requests from members.",
    app_not_allowed_by_org: "This organization only allows apps on its allowlist.",
  };

  const query = new URLSearchParams(location.search);
  const request = {};
  for (const name of PARAMS) request[name] = query.get(name) || "";

  function showError(code) {
    $("[data-consent-status]").hidden = true;
    $("[data-consent-body]").hidden = true;
    const box = $("[data-consent-error]");
    box.hidden = false;
    box.textContent = ERRORS[code] || "Something went wrong (" + code + ").";
  }

  function safeRedirect(url) {
    try {
      const parsed = new URL(url);
      if (parsed.protocol === "https:" || parsed.protocol === "http:") return parsed.href;
    } catch (_) {}
    return "";
  }

  async function call(method, body) {
    const init = { method, credentials: "same-origin", cache: "no-store", headers: { accept: "application/json" } };
    let url = "/api/apps/oauth/authorize";
    if (method === "GET") {
      url += "?" + new URLSearchParams({ ...request, target: "user" }).toString();
    } else {
      init.headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(url, init);
    const payload = await response.json().catch(() => ({}));
    return { status: response.status, payload };
  }

  function renderIcon(icon, name) {
    const box = $("[data-consent-icon]");
    box.textContent = "";
    if (icon && icon.data && /^data:image\/(png|webp);base64,/.test(icon.data)) {
      const img = document.createElement("img");
      img.alt = "";
      img.src = icon.data;
      box.append(img);
      return;
    }
    box.style.background = (icon && icon.color) || "#6366f1";
    box.textContent = (icon && icon.glyph) || String(name || "?").slice(0, 1).toUpperCase();
  }

  function render(payload) {
    const app = payload.app || {};
    $("[data-consent-app]").textContent = app.name || "An app";
    $("[data-consent-developer]").textContent = app.organization || app.developer || "unknown";
    $("[data-consent-account]").textContent = "@" + (payload.account || "");
    $("[data-consent-host]").textContent = payload.redirectHost || "";
    $("[data-consent-unlisted]").hidden = app.listing === "listed";
    renderIcon(app.icon, app.name);
    const list = $("[data-consent-scopes]");
    list.textContent = "";
    for (const scope of payload.scopes || []) {
      const item = document.createElement("li");
      const title = document.createElement("strong");
      title.textContent = scope.label;
      const hint = document.createElement("span");
      hint.textContent = scope.hint + " ";
      const code = document.createElement("code");
      code.textContent = scope.scope;
      item.append(title, hint, code);
      list.append(item);
    }
    const select = $("[data-consent-target]");
    select.textContent = "";
    const mine = document.createElement("option");
    mine.value = "user";
    mine.textContent = "Just me (@" + (payload.account || "") + ")";
    select.append(mine);
    for (const org of payload.organizations || []) {
      const option = document.createElement("option");
      option.value = "org:" + org.name;
      option.textContent = org.name + (org.canApprove ? " (organization)" : " (needs an admin's approval)");
      select.append(option);
    }
    const note = $("[data-consent-target-note]");
    select.addEventListener("change", () => {
      const org = (payload.organizations || []).find((entry) => "org:" + entry.name === select.value);
      note.hidden = !org || org.canApprove;
      note.textContent = org && !org.canApprove ? "An owner or admin of " + org.name + " has to approve this app first. Allow sends them the request." : "";
    });
    $("[data-consent-status]").hidden = true;
    $("[data-consent-body]").hidden = false;
  }

  async function decide(decision) {
    for (const button of document.querySelectorAll(".consent-actions button")) button.disabled = true;
    const target = $("[data-consent-target]").value || "user";
    const { status, payload } = await call("POST", { ...request, target, decision });
    const redirect = safeRedirect(payload.redirect || "");
    if (payload.pending) {
      $("[data-consent-body]").hidden = true;
      $("[data-consent-status]").hidden = false;
      $("[data-consent-status]").textContent = "Request sent. You can finish installing once an organization admin approves it.";
      if (redirect) setTimeout(() => location.assign(redirect), 2500);
      return;
    }
    if (redirect) {
      location.assign(redirect);
      return;
    }
    if (status === 401) {
      location.assign("/login?next=" + encodeURIComponent(location.pathname + location.search));
      return;
    }
    showError(payload.error || "error");
  }

  async function start() {
    const { status, payload } = await call("GET");
    if (status === 401) {
      location.assign("/login?next=" + encodeURIComponent(location.pathname + location.search));
      return;
    }
    if (!payload.ok) {
      const redirect = safeRedirect(payload.redirect || "");
      if (redirect) {
        location.assign(redirect);
        return;
      }
      showError(payload.error || "error");
      return;
    }
    if (payload.visible === false && !(payload.organizations || []).length) {
      showError("app_not_available");
      return;
    }
    render(payload);
    $("[data-allow]").addEventListener("click", () => decide("allow"));
    $("[data-deny]").addEventListener("click", () => decide("deny"));
  }

  start().catch(() => showError("network"));
})();
