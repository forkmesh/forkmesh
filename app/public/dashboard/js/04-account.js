  // Every top-level page is its own document now (/dashboard, /dashboard/repos,
  // /dashboard/network, ...) — navigation between them is a real page load via
  // plain <a href> links, so there is no client-side section router anymore.
  // The only client-routed state left is within-page: repo tabs/tree/blob on
  // the repo page, and the settings sub-tabs below.

  // Every password field sits in its own <form data-credential-scope>. Chrome
  // pools all form-less inputs of a page into one synthetic login form, and a
  // bare password field anywhere (even inside a hidden modal) made it treat the
  // visible search boxes as the username and stuff the saved email into them
  // (#671). The forms are scopes only; the buttons beside them do the saving.
  document.addEventListener("submit", (event) => {
    if (event.target instanceof Element && event.target.matches("[data-credential-scope]")) {
      event.preventDefault();
    }
  });

  const SETTINGS_SECTIONS = ["public-profile", "account", "ssh-keys", "appearance", "notifications", "payout", "nodes", "organizations", "danger", "apps", "agents"];

  function normalizeSettingsSection(section) {
    return SETTINGS_SECTIONS.includes(section) ? section : "public-profile";
  }

  // The settings sub-tab addressed by the URL (/dashboard/settings/<tab>), so a
  // refresh keeps the tab instead of snapping back to public-profile.
  function settingsSectionFromPath() {
    const parts = location.pathname.split("/").filter(Boolean);
    return normalizeSettingsSection(parts[0] === "dashboard" && parts[1] === "settings" ? parts[2] || "" : "");
  }

  function setSettingsSection(section, { scroll = true, push = false } = {}) {
    const activeSection = normalizeSettingsSection(section);
    if (!state.settingsView) state.settingsView = {};
    state.settingsView.section = activeSection;
    if (push) {
      // Reflect the tab in the URL so refresh/back keep it. public-profile is
      // the default, so it stays on the bare /dashboard/settings URL.
      navigateHistory(activeSection === "public-profile"
        ? "/dashboard/settings"
        : `/dashboard/settings/${activeSection}`);
    }

    $$("[data-settings-section]").forEach((panel) => {
      const active = panel.dataset.settingsSection === activeSection;
      panel.classList.toggle("hidden", !active);
    });

    $$("[data-settings-section-link]").forEach((button) => {
      const active = button.dataset.settingsSectionLink === activeSection;
      button.setAttribute("aria-current", active ? "page" : "false");
      button.className = active
        ? "relative flex h-10 items-center gap-3 rounded-md bg-secondary px-3 pl-4 text-left font-semibold text-foreground"
        : "relative flex h-10 items-center gap-3 rounded-md px-4 text-left text-muted-foreground hover:bg-secondary hover:text-foreground";
      button.querySelector("[data-settings-active-indicator]")?.classList.toggle("hidden", !active);
    });

    const settingsMain = $("[data-settings-main]");
    if (scroll && settingsMain) {
      settingsMain.scrollIntoView({ block: "start", behavior: "smooth" });
    }

    // The Organizations tab is data-driven and only fetched when first opened.
    if (activeSection === "organizations") initOrgsSection();
    if (activeSection === "ssh-keys") loadSshKeys();
    if (activeSection === "notifications") loadPingSettings();
    if (activeSection === "agents") initSettingsAgents();
    // Session state changes on other devices, so re-entering this tab always
    // performs a fresh no-store read instead of keeping a page-lifetime copy.
    if (activeSection === "account") {
      bindAccountPasswordControls();
      loadAccountSessions({ force: true });
      loadAccountDevices({ force: true });
      loadPatreonConnection();
      consumePatreonCallbackOutcome();
      loadMembership({ force: true });
      loadPlanUsage();
      loadEmailFooter();
    }
  }

  // ---- Patreon membership link ---------------------------------------------
  const PATREON_OUTCOME_TEXT = {
    connected: ["Patreon linked. Membership syncs from your current pledge.", "good"],
    denied: ["Patreon authorization was denied.", "bad"],
    duplicate_identity: ["That Patreon account is already linked to another ForkMesh user.", "bad"],
    expired: ["The Patreon link expired. Start Connect Patreon again.", "bad"],
    failed: ["Patreon linking failed. Try Connect Patreon again.", "bad"],
    invalid: ["Patreon linking could not be completed.", "bad"],
    invalid_membership: ["Patreon membership data was incomplete. Try again after your pledge is active.", "bad"],
    mismatch: ["Patreon linking did not match this signed-in session.", "bad"],
    not_configured: ["Patreon linking is not configured on this instance.", "bad"],
    replayed: ["That Patreon link was already used.", "bad"],
    wrong_campaign: ["That Patreon membership is not for the ForkMesh campaign.", "bad"],
  };

  function accountBlankslate(icon, title, desc = "") {
    return `<div class="fm-blankslate"><i data-lucide="${escapeHtml(icon)}" aria-hidden="true"></i><p class="fm-blankslate-title">${escapeHtml(title)}</p>${desc ? `<p class="fm-blankslate-desc">${escapeHtml(desc)}</p>` : ""}</div>`;
  }

  function accountListError(message) {
    return `<p class="flex items-center gap-2 p-4 text-sm text-destructive"><i data-lucide="circle-alert" class="h-4 w-4 shrink-0" aria-hidden="true"></i>${escapeHtml(message)}</p>`;
  }

  function confirmAccountAction({ title, body = "", confirmLabel = "Confirm", danger = true } = {}) {
    return new Promise((resolve) => {
      const previous = document.activeElement;
      const overlay = document.createElement("div");
      overlay.dataset.accountConfirm = "";
      overlay.className = "fixed inset-0 z-[90] flex items-start justify-center bg-background/70 p-4 pt-[15vh]";
      overlay.innerHTML = `
        <div role="alertdialog" aria-modal="true" aria-labelledby="accountConfirmTitle"${body ? ' aria-describedby="accountConfirmBody"' : ""} class="fm-dialog w-full max-w-md">
          <div class="fm-dialog-header">
            <h2 id="accountConfirmTitle" class="fm-dialog-title">${escapeHtml(title)}</h2>
            <button type="button" data-account-confirm-cancel aria-label="Close" class="fm-btn fm-btn-sm fm-btn-icon fm-btn-invisible"><i data-lucide="x"></i></button>
          </div>
          ${body ? `<div id="accountConfirmBody" class="fm-dialog-body text-muted-foreground">${escapeHtml(body)}</div>` : ""}
          <div class="fm-dialog-footer">
            <button type="button" data-account-confirm-cancel class="fm-btn fm-btn-sm">Cancel</button>
            <button type="button" data-account-confirm-ok class="fm-btn fm-btn-sm ${danger ? "fm-btn-danger" : "fm-btn-cta"}">${escapeHtml(confirmLabel)}</button>
          </div>
        </div>`;
      const onKey = (event) => {
        if (event.key !== "Escape") return;
        event.preventDefault();
        event.stopPropagation();
        finish(false);
      };
      const finish = (value) => {
        document.removeEventListener("keydown", onKey, true);
        overlay.remove();
        if (previous?.isConnected && typeof previous.focus === "function") previous.focus();
        resolve(value);
      };
      overlay.addEventListener("click", (event) => {
        if (event.target === overlay || event.target.closest("[data-account-confirm-cancel]")) finish(false);
        else if (event.target.closest("[data-account-confirm-ok]")) finish(true);
      });
      document.addEventListener("keydown", onKey, true);
      document.body.appendChild(overlay);
      window.lucide?.createIcons();
      overlay.querySelector(".fm-dialog-footer [data-account-confirm-cancel]")?.focus();
    });
  }

  function setPatreonStatus(message, kind = "") {
    const target = $("[data-patreon-connection-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  function consumePatreonCallbackOutcome() {
    const params = new URLSearchParams(location.search);
    const outcome = (params.get("patreon") || "").trim();
    if (!outcome) return;
    params.delete("patreon");
    const rest = params.toString();
    window.history.replaceState(
      null,
      "",
      location.pathname + (rest ? `?${rest}` : "") + location.hash,
    );
    const mapped = PATREON_OUTCOME_TEXT[outcome] || PATREON_OUTCOME_TEXT.invalid;
    setPatreonStatus(mapped[0], mapped[1]);
  }

  async function loadPatreonConnection() {
    const stateEl = $("[data-patreon-connection-state]");
    const button = $("[data-patreon-connect]");
    if (!stateEl || !button) return;
    if (!state.session?.sessionToken) {
      stateEl.textContent = "Sign in to link Patreon.";
      button.textContent = "Connect Patreon";
      button.disabled = true;
      return;
    }
    button.disabled = true;
    stateEl.textContent = "Checking connection…";
    try {
      const token = state.session.sessionToken || "";
      const response = await fetch("/api/accounts/patreon", {
        headers: {
          accept: "application/json",
          ...(token && token !== "cookie"
            ? { authorization: "Bearer " + token }
            : {}),
        },
        credentials: "same-origin",
        cache: "no-store",
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok) {
        stateEl.textContent = "Could not load Patreon connection.";
        button.textContent = "Connect Patreon";
        button.disabled = true;
        return;
      }
      button.dataset.patreonConfigured = data.configured ? "true" : "false";
      button.disabled = !data.configured;
      if (data.connected) {
        const tier = String(data.tier || "core");
        const tierLabel = {
          supporter: "Supporter",
          pro: "Pro",
          builder: "Builder",
          studio: "Studio",
        }[tier];
        stateEl.textContent = tierLabel
          ? `Connected · ${tierLabel} membership synced`
          : "Connected · waiting for an active pledge sync";
        button.textContent = "Reconnect Patreon";
      } else {
        stateEl.textContent = "Not connected";
        button.textContent = "Connect Patreon";
      }
      if (!data.configured) {
        button.textContent = data.connected
          ? "Patreon linking unavailable"
          : "Use Patreon memberships";
        button.disabled = true;
        setPatreonStatus(
          "Account linking is not configured here. Use the Patreon memberships link below.",
        );
      }
    } catch (_) {
      stateEl.textContent = "Could not load Patreon connection.";
      button.textContent = "Connect Patreon";
      button.disabled = true;
    }
  }

  async function startPatreonOAuth() {
    const button = $("[data-patreon-connect]");
    const token = state.session?.sessionToken || "";
    if (!token) {
      setPatreonStatus("Sign in to link Patreon.", "bad");
      return;
    }
    if (button?.dataset.patreonConfigured === "false") {
      setPatreonStatus(
        "Account linking is not configured here. Use the Patreon memberships link below.",
      );
      return;
    }
    if (button) {
      button.disabled = true;
      button.textContent = "Redirecting…";
    }
    setPatreonStatus("Starting Patreon connection…", "");
    try {
      const body = {};
      if (token !== "cookie") body.sessionToken = token;
      const response = await fetch("/api/accounts/patreon/oauth/start", {
        method: "POST",
        headers: {
          accept: "application/json",
          "content-type": "application/json",
          ...(token !== "cookie" ? { authorization: "Bearer " + token } : {}),
        },
        credentials: "same-origin",
        body: JSON.stringify(body),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || !data.authorizationUrl) {
        throw new Error(data.error || "oauth_start_failed");
      }
      location.href = String(data.authorizationUrl);
      return;
    } catch (error) {
      const code = String(error.message || "");
      setPatreonStatus(
        code === "patreon_oauth_not_configured"
          ? "Patreon linking is not configured on this instance yet."
          : "Could not start Patreon linking. Try again.",
        "bad",
      );
    } finally {
      if (button) {
        button.disabled = true;
        void loadPatreonConnection();
      }
    }
  }

  // ---- Membership ---------------------------------------------------------
  function sessionHeaders(json = false) {
    const token = String(state.session?.sessionToken || "");
    return {
      accept: "application/json",
      ...(json ? { "content-type": "application/json" } : {}),
      ...(token && token !== "cookie"
        ? { authorization: "Bearer " + token }
        : {}),
    };
  }

  // Whoever sold the membership, the entitlement is a role grant; the plan
  // surface reports it, so this card needs no payment provider of its own.
  let membershipLoaded = false;
  let membershipLoading = null;

  function renderMembership(plan) {
    const summary = $("[data-membership-summary]");
    const choose = $("[data-membership-choose]");
    const invite = $("[data-membership-invite]");
    if (!summary || !choose) return;
    const membership = plan?.membership || null;
    const promotion = plan?.promotion || null;
    const held = membership || promotion;
    const tier = String(held?.tier || "");
    const expires = Number(held?.expiresAt || 0);
    const name = tier === "pro" ? "Pro" : "Supporter";
    summary.textContent = !held
      ? "No active membership. ForkMesh plans remain free during launch."
      : membership
        ? `${name} membership active${expires ? ` through ${formatDate(expires)}` : ""}.`
        : `${name} membership through ${expires ? `${formatDate(expires)}` : "a ForkMesh award"}, on the house.`;
    choose.hidden = Boolean(held);
    if (invite) invite.hidden = !held;
    if (held && (tier === "pro" || tier === "supporter")) {
      document.documentElement.dataset.fmPlan = tier;
    } else {
      delete document.documentElement.dataset.fmPlan;
    }
  }

  async function loadMembership({ force = false } = {}) {
    if (!$("[data-membership-summary]")) return;
    if (membershipLoaded && !force) return;
    if (membershipLoading) return membershipLoading;
    membershipLoading = (async () => {
      try {
        const response = await fetch("/api/plans/usage", {
          headers: sessionHeaders(),
          credentials: "same-origin",
          cache: "no-store",
        });
        const data = await response.json().catch(() => ({}));
        if (!response.ok) throw new Error(String(data.error || response.status));
        renderMembership(data);
        membershipLoaded = true;
      } catch (_) {
        const summary = $("[data-membership-summary]");
        if (summary) summary.textContent = "Could not load membership.";
      } finally {
        membershipLoading = null;
      }
    })();
    return membershipLoading;
  }

  // ---- Plan usage + trial (issue #488) -------------------------------------
  function setPlanUsageStatus(message, kind = "") {
    const target = $("[data-plan-usage-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "mt-2 min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  async function planServicesApi(action, method = "GET") {
    const token = String(state.session?.sessionToken || "");
    if (!token) throw new Error("invalid_session");
    const response = await fetch("/api/plans/" + action, {
      method,
      headers: sessionHeaders(method !== "GET"),
      credentials: "same-origin",
      cache: "no-store",
      ...(method !== "GET"
        ? { body: JSON.stringify(token === "cookie" ? {} : { sessionToken: token }) }
        : {}),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `http_${response.status}`);
    }
    return data;
  }

  function renderPlanUsage(data) {
    const list = $("[data-plan-usage-meters]");
    if (!list) return;
    const meters = data?.meters && typeof data.meters === "object" ? data.meters : {};
    list.textContent = "";
    for (const meter of Object.values(meters)) {
      const row = document.createElement("div");
      row.className = "flex items-center justify-between gap-3 rounded-md border border-border px-3 py-2";
      const label = document.createElement("dt");
      label.className = "text-sm text-muted-foreground";
      label.textContent = String(meter?.label || "");
      const value = document.createElement("dd");
      value.className = "text-sm font-semibold text-foreground";
      value.textContent = `${Number(meter?.used || 0)} / ${Number(meter?.limit || 0)}`;
      row.append(label, value);
      list.append(row);
    }
    list.hidden = !list.childElementCount;
    const summary = $("[data-plan-usage-summary]");
    if (summary) {
      summary.textContent = "What this account uses today, against the limits every account shares.";
    }
  }

  function renderPlanTrial(trial) {
    const button = $("[data-plan-trial-start]");
    if (!button) return;
    button.hidden = !trial?.eligible;
    if (trial?.active) {
      setPlanUsageStatus(`Supporter trial active through ${formatDate(Number(trial.endsAt || 0))}.`);
    } else if (trial?.used) {
      setPlanUsageStatus("Your one Supporter trial has ended. Memberships continue through checkout above.");
    }
  }

  function bindPlanTrialControls() {
    const button = $("[data-plan-trial-start]");
    if (!button || button.dataset.bound === "true") return;
    button.dataset.bound = "true";
    button.addEventListener("click", async () => {
      button.disabled = true;
      setPlanUsageStatus("Starting your Supporter trial…");
      try {
        const data = await planServicesApi("trial", "POST");
        button.hidden = true;
        setPlanUsageStatus(`Supporter trial active through ${formatDate(Number(data.endsAt || 0))}.`);
        loadMembership({ force: true });
      } catch (error) {
        const code = String(error.message || "");
        setPlanUsageStatus(
          code === "trial_already_used" ? "This account already used its Supporter trial."
            : code === "already_subscribed" ? "You already have an active membership — no trial needed."
              : "Could not start the trial. Please try again.",
          "bad",
        );
        button.disabled = false;
      }
    });
  }

  async function loadPlanUsage() {
    if (!$("[data-plan-usage-summary]")) return;
    bindPlanTrialControls();
    try {
      renderPlanUsage(await planServicesApi("usage"));
      renderPlanTrial(await planServicesApi("trial"));
    } catch (_) {
      const summary = $("[data-plan-usage-summary]");
      if (summary) summary.textContent = "Could not load usage.";
      setPlanUsageStatus("Sign in again to see plan usage.", "bad");
    }
  }

  // ---- Active account sessions ---------------------------------------------
  let accountSessionsLoaded = false;
  let accountSessionsLoading = null;

  function setAccountSessionStatus(message, kind = "") {
    const target = $("[data-account-session-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  async function accountSessionApi(method, path = "") {
    const token = state.session?.sessionToken || "";
    if (!token) throw new Error("invalid_session");
    const response = await fetch("/api/accounts/sessions" + path, {
      method,
      headers: {
        accept: "application/json",
        authorization: "Bearer " + token,
      },
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `http_${response.status}`);
    }
    return data;
  }

  function renderAccountSessions(data) {
    const list = $("[data-account-session-list]");
    if (!list) return;
    const sessions = Array.isArray(data?.sessions) ? data.sessions : [];
    if (!sessions.length) {
      list.innerHTML = accountBlankslate("monitor", "No active sessions were returned.");
      window.lucide?.createIcons();
      return;
    }
    list.innerHTML = sessions.map((session, index) => `
      <article class="flex flex-col gap-3 p-4 sm:flex-row sm:items-center sm:justify-between ${index ? "border-t border-border" : ""}">
        <div class="min-w-0">
          <p class="text-sm font-semibold text-foreground">
            ${escapeHtml(session.deviceLabel || "Unknown device")}
            ${session.current ? '<span class="fm-label fm-label-accent ml-2">This device</span>' : ""}
          </p>
          <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(String(session.ipAddress || "address unavailable"))} · last active ${escapeHtml(formatTimeAgo(Number(session.lastSeenAt || 0)))} · signed in ${escapeHtml(formatDate(Number(session.createdAt || 0)))} · expires ${escapeHtml(formatDate(Number(session.expiresAt || 0)))}</p>
        </div>
        <button type="button" data-account-session-revoke="${escapeHtml(session.id || "")}" data-account-session-current="${session.current ? "true" : "false"}" class="fm-btn fm-btn-sm fm-btn-danger shrink-0">
          ${session.current ? "Sign out here" : "Sign out"}
        </button>
      </article>`).join("");
  }

  // Coarse kinds only — the Worker stores when an account was emailed and
  // whether the provider accepted it, never the subject or body.
  const ACCOUNT_EMAIL_KIND_LABELS = {
    verification: "Email verification",
    password_reset: "Password reset",
    password_change: "Password change",
    feedback: "Founder feedback",
    notifications: "Notification digest",
    general_chat: "#general digest",
    admin_test: "Test email",
  };

  function renderAccountActivity(data) {
    const account = data?.account || {};
    const lastSeen = $("[data-account-last-seen]");
    if (lastSeen) {
      const seenAt = Number(account.lastSeenAt || 0);
      lastSeen.textContent = seenAt ? formatTimeAgo(seenAt) : "Never";
      lastSeen.title = seenAt ? formatDate(seenAt) : "";
    }
    const emailedAt = Number(account.lastEmailAt || 0);
    const lastEmail = $("[data-account-last-email]");
    if (lastEmail) {
      lastEmail.textContent = emailedAt ? formatTimeAgo(emailedAt) : "Never emailed";
      lastEmail.title = emailedAt ? formatDate(emailedAt) : "";
    }
    const detail = $("[data-account-last-email-detail]");
    if (detail) {
      const status = String(account.lastEmailStatus || "");
      const kind = ACCOUNT_EMAIL_KIND_LABELS[String(account.lastEmailKind || "")] || "Email";
      const sent = Number(account.emailSendCount || 0);
      detail.textContent = !emailedAt ? "" : kind + " · " + (
        status === "delivered" ? "Delivered to the mail provider"
          : status === "failed" ? "The mail provider rejected it"
            : "Delivery status unknown") + (
        sent > 0 ? ` · ${sent} email${sent === 1 ? "" : "s"} sent to this account` : "");
      detail.className = "mt-1 text-xs " + (
        status === "delivered" && emailedAt ? "text-muted-foreground"
          : status === "failed" && emailedAt ? "text-destructive"
            : "text-muted-foreground");
    }
  }

  function bindAccountSessionControls() {
    const list = $("[data-account-session-list]");
    if (list && list.dataset.controlsBound !== "true") {
      list.dataset.controlsBound = "true";
      list.addEventListener("click", (event) => {
        const button = event.target.closest("[data-account-session-revoke]");
        if (!button) return;
        revokeAccountSession(
          button.dataset.accountSessionRevoke || "",
          button.dataset.accountSessionCurrent === "true");
      });
    }
    const others = $("[data-account-sessions-revoke-others]");
    if (others && others.dataset.controlsBound !== "true") {
      others.dataset.controlsBound = "true";
      others.addEventListener("click", () => revokeOtherAccountSessions());
    }
    const all = $("[data-account-sessions-revoke-all]");
    if (all && all.dataset.controlsBound !== "true") {
      all.dataset.controlsBound = "true";
      all.addEventListener("click", () => revokeAllAccountSessions());
    }
  }

  async function loadAccountSessions({ force = false } = {}) {
    if (!$("[data-account-session-list]")) return;
    bindAccountSessionControls();
    if (accountSessionsLoaded && !force) return;
    if (accountSessionsLoading) return accountSessionsLoading;
    accountSessionsLoading = (async () => {
      try {
        const data = await accountSessionApi("GET");
        renderAccountSessions(data);
        renderAccountActivity(data);
        accountSessionsLoaded = true;
        setAccountSessionStatus(data.privacyNotice || "");
      } catch (error) {
        const list = $("[data-account-session-list]");
        if (list) {
          list.innerHTML = accountListError(error.message === "invalid_session" ? "Sign in to manage active sessions." : "Could not load active sessions.");
          window.lucide?.createIcons();
        }
      } finally {
        accountSessionsLoading = null;
      }
    })();
    return accountSessionsLoading;
  }

  async function revokeAccountSession(sessionId, current) {
    if (!sessionId || !await confirmAccountAction({
      title: current ? "Sign out this device now?" : "Sign out that device?",
      confirmLabel: "Sign out",
    })) return;
    try {
      const result = await accountSessionApi(
        "DELETE", "/" + encodeURIComponent(sessionId));
      if (current || result.currentRevoked) {
        logout();
        return;
      }
      accountSessionsLoaded = false;
      await loadAccountSessions({ force: true });
      setAccountSessionStatus("That device was signed out.", "good");
    } catch (_) {
      setAccountSessionStatus("Could not sign out that device.", "bad");
    }
  }

  async function revokeOtherAccountSessions() {
    if (!await confirmAccountAction({
      title: "Sign out every other active device?",
      body: "This browser stays signed in.",
      confirmLabel: "Sign out others",
    })) return;
    const button = $("[data-account-sessions-revoke-others]");
    if (button) button.disabled = true;
    try {
      await accountSessionApi("DELETE", "/others");
      accountSessionsLoaded = false;
      await loadAccountSessions({ force: true });
      setAccountSessionStatus("All other devices were signed out.", "good");
    } catch (_) {
      setAccountSessionStatus("Could not sign out the other devices.", "bad");
    } finally {
      if (button) button.disabled = false;
    }
  }

  // A sign-in lasts years, so a lost laptop or a shared browser needs one
  // action that ends every session, this browser included. The Worker revokes
  // them all; logout() then clears this browser and tells its other tabs.
  async function revokeAllAccountSessions() {
    if (!await confirmAccountAction({
      title: `Sign out of ${platformName()} on every device, including this one?`,
      confirmLabel: "Sign out everywhere",
    })) return;
    const button = $("[data-account-sessions-revoke-all]");
    if (button) button.disabled = true;
    try {
      await accountSessionApi("DELETE", "/all");
      logout();
    } catch (_) {
      if ($("[data-account-session-status]")) {
        setAccountSessionStatus("Could not sign out all devices.", "bad");
      } else {
        showErrorNotice("Could not sign out all devices. Please try again.");
      }
      if (button) button.disabled = false;
    }
  }

  let emailFooter = null;

  function setEmailFooterStatus(message, kind = "") {
    const target = $("[data-email-footer-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (kind === "bad" ? "text-destructive" : kind === "good" ? "text-success" : "text-muted-foreground");
  }

  function renderEmailFooter(data) {
    const preview = $("[data-email-footer-preview]");
    if (preview) preview.innerHTML = data.previewHtml || "";
    const opens = $("[data-email-footer-opens]");
    if (opens) {
      const count = Number(data.opens || 0);
      opens.textContent = count
        ? `Opened ${count.toLocaleString()} time${count === 1 ? "" : "s"}, last ${formatTimeAgo(data.lastOpenedAt)}.`
        : "Not opened yet.";
    }
    $$("[data-email-footer-copy], [data-email-footer-copy-html]").forEach((button) => {
      button.disabled = !data.html;
    });
  }

  async function copyEmailFooter(asHtml) {
    if (!emailFooter?.html) return;
    try {
      if (!asHtml && window.ClipboardItem && navigator.clipboard?.write) {
        await navigator.clipboard.write([new ClipboardItem({
          "text/html": new Blob([emailFooter.html], { type: "text/html" }),
          "text/plain": new Blob([`Sent with ${platformName()}`], { type: "text/plain" }),
        })]);
      } else {
        await navigator.clipboard.writeText(emailFooter.html);
      }
      setEmailFooterStatus(asHtml
        ? "HTML copied. Paste it into your signature's HTML view."
        : "Footer copied. Paste it into your mail app's signature.");
    } catch (_) {
      setEmailFooterStatus("Could not copy. Select the footer above and copy it.", "bad");
    }
  }

  async function loadEmailFooter() {
    const panel = $("[data-email-footer]");
    if (!panel) return;
    if (!panel.dataset.bound) {
      panel.dataset.bound = "true";
      $("[data-email-footer-copy]")?.addEventListener("click", () => copyEmailFooter(false));
      $("[data-email-footer-copy-html]")?.addEventListener("click", () => copyEmailFooter(true));
    }
    const token = state.session?.sessionToken || "";
    if (!token) {
      setEmailFooterStatus("Sign in to get your tracked footer.", "bad");
      return;
    }
    try {
      const response = await fetch("/api/mail/footer", {
        headers: { accept: "application/json", authorization: "Bearer " + token },
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok === false) throw new Error(data.error || `http_${response.status}`);
      emailFooter = data;
      renderEmailFooter(data);
      setEmailFooterStatus("");
    } catch (_) {
      const preview = $("[data-email-footer-preview]");
      if (preview) preview.textContent = "";
      setEmailFooterStatus("Could not load your tracked footer.", "bad");
    }
  }

  // ---- Change password (issue #648) ----------------------------------------
  // Settings' other "Current password" boxes each CONFIRM some other action
  // (rename, payout, pings, claim, delete); this is the only one that sets a
  // new password, so a signed-in user no longer has to log out and wait for a
  // reset email. The Worker re-verifies the current password and signs every
  // other device out, so the sessions list above is refetched on success.
  const ACCOUNT_PASSWORD_MIN_LENGTH = 8;
  const ACCOUNT_PASSWORD_ERRORS = {
    email_not_verified: "Confirm your email address before changing your password.",
    invalid_credentials: "That current password is not right.",
    invalid_session: "Sign in again to change your password.",
    password_not_set: "This account signs in with a key, not a password.",
    password_too_short: `Use at least ${ACCOUNT_PASSWORD_MIN_LENGTH} characters.`,
    password_unchanged: "Choose a password different from your current one.",
    too_many_attempts: "Too many attempts. Wait a few minutes and try again.",
  };

  function setAccountPasswordStatus(message, kind = "") {
    const target = $("[data-account-password-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  function accountPasswordFields() {
    return [
      $("[data-account-password-current]"),
      $("[data-account-password-new]"),
      $("[data-account-password-confirm]"),
    ];
  }

  function bindAccountPasswordControls() {
    const button = $("[data-account-password-save]");
    if (!button || button.dataset.controlsBound === "true") return;
    button.dataset.controlsBound = "true";
    button.addEventListener("click", () => changeAccountPassword());
    // The card's <form data-credential-scope> exists only to keep browser
    // password managers away from the page's search boxes; it never submits,
    // so Enter is wired by hand rather than leaving the keyboard path dead.
    accountPasswordFields().forEach((input) => {
      input?.addEventListener("keydown", (event) => {
        if (event.key !== "Enter") return;
        event.preventDefault();
        changeAccountPassword();
      });
    });
  }

  async function changeAccountPassword() {
    const [currentField, newField, confirmField] = accountPasswordFields();
    const currentPassword = currentField?.value || "";
    const newPassword = newField?.value || "";
    if (!currentPassword) {
      setAccountPasswordStatus("Enter your current password.", "bad");
      currentField?.focus();
      return;
    }
    if (newPassword.length < ACCOUNT_PASSWORD_MIN_LENGTH) {
      setAccountPasswordStatus(
        ACCOUNT_PASSWORD_ERRORS.password_too_short, "bad");
      newField?.focus();
      return;
    }
    if (newPassword !== (confirmField?.value || "")) {
      setAccountPasswordStatus("The new passwords do not match.", "bad");
      confirmField?.focus();
      return;
    }
    const button = $("[data-account-password-save]");
    if (button) { button.disabled = true; button.textContent = "Updating…"; }
    setAccountPasswordStatus("Updating password…");
    try {
      const token = state.session?.sessionToken || "";
      const response = await fetch("/api/accounts/change-password", {
        method: "POST",
        credentials: "same-origin",
        cache: "no-store",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          ...(token ? { authorization: "Bearer " + token } : {}),
        },
        body: JSON.stringify({ currentPassword, newPassword }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok !== true) {
        throw new Error(body.error || `http_${response.status}`);
      }
      accountPasswordFields().forEach((input) => { if (input) input.value = ""; });
      // The Worker keeps this device signed in; the fallback only fires if it
      // could not tell which session made the change and cleared them all.
      if (body.currentRevoked) {
        logout();
        return;
      }
      setAccountPasswordStatus(
        "Password updated. Every other device was signed out.", "good");
      accountSessionsLoaded = false;
      await loadAccountSessions({ force: true });
    } catch (error) {
      setAccountPasswordStatus(
        ACCOUNT_PASSWORD_ERRORS[error.message]
          || "Could not update your password. Try again.", "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Update password"; }
    }
  }

  // ---- Nodes & devices (account_devices registry) ----------------------------
  // The web dashboard is the source of truth for every node key bound to the
  // account (desktop and mobile). Listing is session-gated like sessions; the
  // revoke routes are sudo-gated, so the browser re-collects the password (and
  // optional authenticator code) in an inline form before each call even while
  // a live session exists. Revoking a node signs out its sessions and makes its
  // owner-signed calls fail closed on the next request.
  let accountDevicesLoaded = false;
  let accountDevicesLoading = null;
  // The sudo action the inline re-auth form will run on submit:
  // { kind: "one", id, current } or { kind: "all" }.
  let accountDevicesPendingAction = null;

  const ACCOUNT_DEVICE_KIND_LABELS = {
    desktop_node: "Desktop node",
    mobile_node: "Mobile node",
    browser_control: "Browser desktop control",
    browser: "Browser",
    client: "Client",
  };
  // Short hints for the capability strings that change what a key may do on
  // the owner's behalf; the browse/comment/submit baseline is implied.
  const ACCOUNT_DEVICE_CAPABILITY_HINTS = [
    ["host_repo", "hosts"],
    ["mirror_repo", "mirrors"],
    ["publish_repo", "publishes"],
    ["owner_sign", "signs as owner"],
    ["desktop_control", "controls your desktop's agents"],
  ];

  function accountDeviceKindLabel(kind) {
    const key = String(kind || "").trim();
    if (ACCOUNT_DEVICE_KIND_LABELS[key]) return ACCOUNT_DEVICE_KIND_LABELS[key];
    return key ? key.replace(/[_-]+/g, " ").replace(/^\w/, (c) => c.toUpperCase()) : "Device";
  }

  function accountDeviceCapabilityHint(device) {
    const caps = Array.isArray(device?.capabilities) ? device.capabilities.map(String) : [];
    const hints = ACCOUNT_DEVICE_CAPABILITY_HINTS
      .filter(([cap]) => caps.includes(cap))
      .map(([, hint]) => hint);
    if (hints.length) return hints.join(" · ");
    const kind = String(device?.kind || "");
    if (kind === "mobile_node") return "publishes · signs as owner";
    if (kind === "desktop_node") return "hosts · publishes";
    return "";
  }

  function setAccountDevicesStatus(message, kind = "") {
    const target = $("[data-account-devices-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  async function accountDeviceApi(method, path = "", body) {
    const token = state.session?.sessionToken || "";
    if (!token) throw new Error("invalid_session");
    const headers = {
      accept: "application/json",
      authorization: "Bearer " + token,
    };
    if (body !== undefined) headers["content-type"] = "application/json";
    const response = await fetch("/api/accounts/devices" + path, {
      method,
      headers,
      cache: "no-store",
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `http_${response.status}`);
    }
    return data;
  }

  function renderAccountDevices(data) {
    const list = $("[data-account-devices-list]");
    if (!list) return;
    const devices = Array.isArray(data?.devices) ? data.devices : [];
    if (!devices.length) {
      list.innerHTML = accountBlankslate("laptop", "No nodes or devices are bound to this account yet.");
      window.lucide?.createIcons();
      return;
    }
    list.innerHTML = devices.map((device, index) => {
      const enabled = device.enabled !== false && !device.revokedAt;
      const lastSeen = Number(device.lastSeen || 0);
      const created = Number(device.createdAt || 0);
      const hint = accountDeviceCapabilityHint(device);
      const meta = [
        enabled
          ? "last seen " + (lastSeen ? formatTimeAgo(lastSeen) : "never")
          : "revoked " + formatDate(Number(device.revokedAt || 0)),
        created ? "added " + formatDate(created) : "",
        hint,
      ].filter(Boolean).join(" · ");
      const control = !enabled
        ? '<span class="fm-label shrink-0">Revoked</span>'
        : device.current
          ? '<span class="fm-label fm-label-accent shrink-0">This device</span>'
          : `<button type="button" data-account-device-revoke="${escapeHtml(device.id || "")}" class="fm-btn fm-btn-sm fm-btn-danger shrink-0">Revoke</button>`;
      return `
      <article data-account-device-row="${escapeHtml(device.id || "")}" class="flex flex-col gap-3 p-4 sm:flex-row sm:items-center sm:justify-between ${index ? "border-t border-border" : ""} ${enabled ? "" : "opacity-60"}">
        <div class="min-w-0">
          <p class="flex flex-wrap items-center gap-2 text-sm font-semibold text-foreground">
            <span class="fm-label">${escapeHtml(accountDeviceKindLabel(device.kind))}</span>
            <span class="truncate">${escapeHtml(device.label || "Unnamed device")}</span>
            ${device.current ? '<span class="fm-label fm-label-accent">This device</span>' : ""}
          </p>
          <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(meta)}</p>
          ${device.id ? `<p class="mt-1 text-[11px] text-muted-foreground">Device ID <span data-account-device-id class="break-all font-mono text-foreground select-all">${escapeHtml(device.id)}</span></p>` : ""}
        </div>
        ${control}
      </article>`;
    }).join("");
  }

  function accountDevicesReauthForm() {
    return $("[data-account-devices-reauth]");
  }

  function openAccountDevicesReauth(action) {
    const form = accountDevicesReauthForm();
    if (!form) return;
    accountDevicesPendingAction = action;
    const title = $("[data-account-devices-reauth-title]");
    if (title) {
      title.textContent = action.kind === "all"
        ? "Sign out all other nodes?"
        : "Revoke " + (action.label || "this node") + "?";
    }
    const confirm = $("[data-account-devices-reauth-confirm]");
    if (confirm) {
      confirm.textContent = action.kind === "all" ? "Sign out other nodes" : "Revoke node";
      confirm.disabled = false;
    }
    const password = $("[data-account-devices-reauth-password]");
    const totp = $("[data-account-devices-reauth-totp]");
    if (password) password.value = "";
    if (totp) totp.value = "";
    form.hidden = false;
    setAccountDevicesStatus("");
    password?.focus();
  }

  function closeAccountDevicesReauth() {
    const form = accountDevicesReauthForm();
    accountDevicesPendingAction = null;
    if (!form) return;
    form.hidden = true;
    const password = $("[data-account-devices-reauth-password]");
    const totp = $("[data-account-devices-reauth-totp]");
    if (password) password.value = "";
    if (totp) totp.value = "";
  }

  function accountDevicesReauthBody() {
    const password = String($("[data-account-devices-reauth-password]")?.value || "");
    const totp = String($("[data-account-devices-reauth-totp]")?.value || "").replace(/\s+/g, "");
    const body = { password };
    if (totp) body.totp = totp;
    return body;
  }

  function accountDevicesErrorText(error, fallback) {
    const code = String(error?.message || "");
    if (code === "reauth_required" || code === "invalid_password" || code === "invalid_totp") {
      return "Password or authenticator code was not accepted. Try again.";
    }
    if (code === "invalid_session") return "Sign in again to manage nodes and devices.";
    if (code === "not_found") return "That node was already removed.";
    return fallback;
  }

  async function submitAccountDevicesReauth() {
    const action = accountDevicesPendingAction;
    if (!action) return;
    const body = accountDevicesReauthBody();
    if (!body.password) {
      setAccountDevicesStatus("Enter your current password to continue.", "bad");
      $("[data-account-devices-reauth-password]")?.focus();
      return;
    }
    const confirm = $("[data-account-devices-reauth-confirm]");
    if (confirm) confirm.disabled = true;
    setAccountDevicesStatus(action.kind === "all" ? "Signing out other nodes…" : "Revoking node…");
    try {
      if (action.kind === "all") {
        const result = await accountDeviceApi("POST", "/revoke-all", body);
        closeAccountDevicesReauth();
        accountDevicesLoaded = false;
        accountSessionsLoaded = false;
        await Promise.all([
          loadAccountDevices({ force: true }),
          loadAccountSessions({ force: true }),
        ]);
        const count = Number(result?.revokedDevices || 0);
        setAccountDevicesStatus(
          count ? `${count} other node${count === 1 ? " was" : "s were"} signed out.` : "No other nodes needed signing out.",
          "good");
      } else {
        const result = await accountDeviceApi(
          "DELETE", "/" + encodeURIComponent(action.id), body);
        closeAccountDevicesReauth();
        if (action.current || result?.currentRevoked) {
          // Revoking the node bound to this session invalidates it too.
          logout();
          return;
        }
        accountDevicesLoaded = false;
        accountSessionsLoaded = false;
        await Promise.all([
          loadAccountDevices({ force: true }),
          loadAccountSessions({ force: true }),
        ]);
        setAccountDevicesStatus("That node was revoked and signed out.", "good");
      }
    } catch (error) {
      setAccountDevicesStatus(accountDevicesErrorText(
        error,
        action.kind === "all" ? "Could not sign out the other nodes." : "Could not revoke that node."), "bad");
      if (confirm) confirm.disabled = false;
    }
  }

  function bindAccountDevicesControls() {
    const list = $("[data-account-devices-list]");
    if (list && list.dataset.controlsBound !== "true") {
      list.dataset.controlsBound = "true";
      list.addEventListener("click", (event) => {
        const button = event.target.closest("[data-account-device-revoke]");
        if (!button) return;
        const row = button.closest("[data-account-device-row]");
        const label = row?.querySelector(".truncate")?.textContent?.trim() || "";
        openAccountDevicesReauth({
          kind: "one",
          id: button.dataset.accountDeviceRevoke || "",
          label,
          current: false,
        });
      });
    }
    const others = $("[data-account-devices-revoke-others]");
    if (others && others.dataset.controlsBound !== "true") {
      others.dataset.controlsBound = "true";
      others.addEventListener("click", () => openAccountDevicesReauth({ kind: "all" }));
    }
    const form = accountDevicesReauthForm();
    if (form && form.dataset.controlsBound !== "true") {
      form.dataset.controlsBound = "true";
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        submitAccountDevicesReauth();
      });
      $("[data-account-devices-reauth-cancel]")?.addEventListener("click", () => {
        closeAccountDevicesReauth();
        setAccountDevicesStatus("");
      });
    }
  }

  async function loadAccountDevices({ force = false } = {}) {
    if (!$("[data-account-devices-list]")) return;
    bindAccountDevicesControls();
    if (accountDevicesLoaded && !force) return;
    if (accountDevicesLoading) return accountDevicesLoading;
    accountDevicesLoading = (async () => {
      try {
        const data = await accountDeviceApi("GET");
        renderAccountDevices(data);
        accountDevicesLoaded = true;
      } catch (error) {
        const list = $("[data-account-devices-list]");
        if (list) {
          list.innerHTML = accountListError(error.message === "invalid_session" ? "Sign in to manage nodes and devices." : "Could not load nodes and devices.");
          window.lucide?.createIcons();
        }
      } finally {
        accountDevicesLoading = null;
      }
    })();
    return accountDevicesLoading;
  }

  // ---- SSH public keys (settings tab) ---------------------------------------
  // Public keys are account-scoped and encrypted at rest by the Worker. The
  // browser never handles a private key; every push is authorized again by the
  // Worker and executed by a separately operated node-side SSH gateway.
  let sshKeysLoaded = false;
  let sshKeysLoading = null;

  function setSshKeyStatus(message, kind = "") {
    const target = $("[data-ssh-key-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  async function sshKeyApi(method, path = "", body) {
    const token = state.session?.sessionToken || "";
    if (!token) throw new Error("invalid_session");
    const headers = {
      accept: "application/json",
      authorization: "Bearer " + token,
    };
    if (body !== undefined) headers["content-type"] = "application/json";
    const response = await fetch("/api/accounts/ssh-keys" + path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      throw new Error(data.error || `http_${response.status}`);
    }
    return data;
  }

  let gitPushWired = false;

  function setGitPushStatus(message, kind = "") {
    const target = $("[data-git-push-status]");
    if (!target) return;
    target.textContent = message || "";
    target.className = "min-h-4 text-xs " + (
      kind === "bad" ? "text-destructive"
        : kind === "good" ? "text-success"
        : "text-muted-foreground"
    );
  }

  async function loadGitPushSettings() {
    const note = $("[data-git-push-contributor]");
    const input = $("[data-git-push-repo]");
    try {
      const response = await fetch("/api/hosted-repositories", { headers: { accept: "application/json" } });
      const data = await response.json().catch(() => ({}));
      const listed = (Array.isArray(data?.contributorPush) ? data.contributorPush : [])
        .filter((item) => /^[^*/\s]+\/[^*/\s]+$/.test(String(item)));
      const account = String(state.session?.nodeName || state.session?.name || "").toLowerCase();
      if (note && listed.length) {
        note.textContent = `To contribute, Fork the repository from its page (Forks, then Fork this repository), push any branch to your fork with a credential for it, and open a pull request back. Without a fork, anyone with a verified email address can also push branches named pr/${account || "<your-name>"}/… to ${listed.join(", ")} directly. Other branches stay with the owners.`;
        note.classList.remove("hidden");
      }
      if (input && !input.value && listed.length) input.value = listed[0];
    } catch (_) {
    }
  }

  function renderGitPushGrant(push) {
    const target = $("[data-git-push-result]");
    if (!target) return;
    const contributor = push?.access === "contributor";
    const expires = Number(push?.expiresAt || 0);
    const field = (label, value, attr) => `
      <label class="grid gap-1 text-xs font-semibold text-foreground">${escapeHtml(label)}
        <input ${attr} type="text" readonly value="${escapeHtml(value || "")}" class="h-9 rounded-md border border-border bg-secondary px-2 font-mono text-xs font-normal text-foreground" />
      </label>`;
    const commands = (Array.isArray(push?.commands) ? push.commands : []).join("\n");
    target.innerHTML = `
      ${field("Remote URL", push?.remoteUrl, "data-git-push-remote")}
      ${field("Username", push?.username, "data-git-push-username")}
      ${field("Password (shown once; copy it now)", push?.password, "data-git-push-password")}
      <p class="text-xs text-muted-foreground">${contributor
        ? `You can push only branches named <span class="font-mono">${escapeHtml(push.branchPrefix || "")}…</span>. Pushes are fast-forward only: add commits instead of force-pushing, or push a new branch name. Then open a pull request from that branch on the repository's Pull requests tab.`
        : "You have write access to this repository; its branch protection still applies."}
        ${expires ? ` Expires ${escapeHtml(formatDate(expires))}.` : ""}</p>
      <pre class="overflow-x-auto rounded-md border border-border bg-secondary p-3 font-mono text-xs text-foreground">${escapeHtml(commands)}</pre>`;
    target.classList.remove("hidden");
  }

  async function mintGitPushCredential() {
    const raw = String($("[data-git-push-repo]")?.value || "").trim().replace(/^https?:\/\/[^/]+\//, "").replace(/\.git$/, "");
    const [owner, name, extra] = raw.split("/");
    if (!owner || !name || extra !== undefined) {
      setGitPushStatus("Enter the repository as owner/repository.", "bad");
      return;
    }
    const token = state.session?.sessionToken || "";
    if (!token) {
      setGitPushStatus("Sign in to create a push credential.", "bad");
      return;
    }
    const button = $("[data-git-push-mint]");
    if (button) button.disabled = true;
    setGitPushStatus("Creating a push credential…");
    try {
      const response = await fetch("/api/hosted-repositories/push-credential", {
        method: "POST",
        headers: { accept: "application/json", "content-type": "application/json" },
        body: JSON.stringify({ owner, name, sessionToken: token }),
      });
      const data = await response.json().catch(() => ({}));
      if (!response.ok || !data.push) {
        const messages = {
          unauthorized: "Sign in to create a push credential.",
          session_required: "Sign in again in this browser to create a push credential.",
          hosted_git_unavailable: "This server cannot accept pushes right now.",
        };
        setGitPushStatus(data.message || messages[data.error] || "A push credential could not be created.", "bad");
        return;
      }
      setGitPushStatus(data.push.access === "contributor" ? "Contributor push credential created." : "Push credential created.", "good");
      renderGitPushGrant(data.push);
    } catch (_) {
      setGitPushStatus("A push credential could not be created.", "bad");
    } finally {
      if (button) button.disabled = false;
    }
  }

  function wireGitPushPanel() {
    if (gitPushWired || !$("[data-git-push-panel]")) return;
    gitPushWired = true;
    $("[data-git-push-mint]")?.addEventListener("click", () => mintGitPushCredential());
    $("[data-git-push-repo]")?.addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        mintGitPushCredential();
      }
    });
    loadGitPushSettings();
  }

  // How to use a key, shown once the server has an SSH gateway. The HTTPS
  // push credential above stays the fallback for networks that block SSH.
  function renderSshUsePanel(data) {
    const panel = $("[data-ssh-use-panel]");
    const steps = $("[data-ssh-use-steps]");
    if (!panel || !steps) return;
    const host = String(data?.gatewayHost || "");
    if (!data?.gatewayConfigured || !host) {
      panel.classList.add("hidden");
      return;
    }
    const port = Number(data.gatewayPort) || 22;
    const example = String(data.gatewayExampleUrl || "");
    const login = port === 22 ? `ssh -T git@${host}` : `ssh -T -p ${port} git@${host}`;
    const account = String(data.account || "<you>");
    const code = (value) => `<code class="mt-1 block overflow-x-auto rounded-md border border-border bg-secondary px-2 py-1 font-mono text-xs text-foreground">${escapeHtml(value)}</code>`;
    const items = [
      ["Create a key if you do not have one, on your own computer:", code('ssh-keygen -t ed25519 -C "you@example.com"')],
      ["Paste the public half (the .pub file) below and select <strong>Add SSH key</strong>:", code("cat ~/.ssh/id_ed25519.pub")],
      ["Check the connection. It prints a greeting; there is no shell:", code(login)],
      ["Clone with the SSH address from a repository's <strong>Code</strong> button, or switch an existing clone:", code(`git clone ${example}`) + code(`git remote set-url origin ${example}`)],
    ];
    if (data.gatewayAllRepositories) {
      items.push(["Contributing without write access: fork the repository and push to your fork, or, where the repository accepts contributor branches, push one named pr/" + escapeHtml(account) + "/&lt;topic&gt; and open a pull request:", code(`git push origin HEAD:pr/${account}/my-fix`)]);
    }
    steps.innerHTML = items.map(([text, body], index) => `
      <li class="grid gap-1"><span><span class="font-semibold text-foreground">${index + 1}.</span> ${text}</span>${body}</li>`).join("");
    panel.classList.remove("hidden");
  }

  function renderSshKeys(data) {
    const list = $("[data-ssh-key-list]");
    if (!list) return;
    const keys = Array.isArray(data?.keys) ? data.keys : [];
    const gateway = $("[data-ssh-gateway-status]");
    if (gateway) {
      gateway.textContent = data?.gatewayConfigured
        ? `SSH gateway: ${data.gatewayHost}${Number(data.gatewayPort) === 22 ? "" : `:${data.gatewayPort}`}`
        : "SSH pushes are not available on this server. Push over HTTPS with a push credential instead.";
    }
    renderSshUsePanel(data);
    if (!keys.length) {
      list.innerHTML = accountBlankslate("key-round", "No SSH keys registered.", "Add a public key above to clone and push over SSH.");
      window.lucide?.createIcons();
      return;
    }
    list.innerHTML = keys.map((key, index) => {
      const used = Number(key.lastUsedAt || 0);
      return `
        <article class="flex flex-col gap-3 p-4 sm:flex-row sm:items-center sm:justify-between ${index ? "border-t border-border" : ""}">
          <div class="min-w-0">
            <p class="truncate text-sm font-semibold text-foreground">${escapeHtml(key.label || "SSH key")}</p>
            <p class="mt-1 break-all font-mono text-xs text-muted-foreground">${escapeHtml(key.fingerprint || "")}</p>
            <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(key.keyType || "SSH")} · added ${escapeHtml(formatDate(key.createdAt))}${used ? ` · last used ${escapeHtml(formatTimeAgo(used))}` : " · never used"}</p>
          </div>
          <button type="button" data-ssh-key-revoke="${escapeHtml(key.id || "")}" class="fm-btn fm-btn-sm fm-btn-danger shrink-0">
            Revoke
          </button>
        </article>`;
    }).join("");
  }

  async function loadSshKeys({ force = false } = {}) {
    wireGitPushPanel();
    if (!$("[data-ssh-key-list]")) return;
    if (sshKeysLoaded && !force) return;
    if (sshKeysLoading) return sshKeysLoading;
    sshKeysLoading = (async () => {
      try {
        const data = await sshKeyApi("GET");
        renderSshKeys(data);
        sshKeysLoaded = true;
      } catch (error) {
        const list = $("[data-ssh-key-list]");
        if (list) {
          list.innerHTML = accountListError(error.message === "invalid_session" ? "Sign in to manage SSH keys." : "Could not load SSH keys.");
          window.lucide?.createIcons();
        }
      } finally {
        sshKeysLoading = null;
      }
    })();
    return sshKeysLoading;
  }

  async function addSshKey() {
    const publicKey = ($("[data-ssh-public-key]")?.value || "").trim();
    const label = ($("[data-ssh-key-label]")?.value || "").trim();
    if (!publicKey) {
      setSshKeyStatus("Paste one OpenSSH public key.", "bad");
      return;
    }
    const button = $("[data-ssh-key-add]");
    if (button) {
      button.disabled = true;
      button.textContent = "Adding…";
    }
    try {
      await sshKeyApi("POST", "", { publicKey, label });
      if ($("[data-ssh-public-key]")) $("[data-ssh-public-key]").value = "";
      if ($("[data-ssh-key-label]")) $("[data-ssh-key-label]").value = "";
      setSshKeyStatus("SSH public key registered.", "good");
      sshKeysLoaded = false;
      await loadSshKeys({ force: true });
    } catch (error) {
      const messages = {
        unsupported_key_type: "Use an Ed25519, ECDSA, security-key, or RSA (3072-bit or stronger) public key.",
        rsa_key_too_small: "RSA keys must be at least 3072 bits.",
        ssh_key_already_registered: "That public key is already registered or was previously revoked. Use a fresh keypair.",
        ssh_key_limit_reached: "Revoke an unused key before adding another.",
        one_public_key_required: "Paste exactly one public key.",
        invalid_session: "Sign in to manage SSH keys.",
      };
      setSshKeyStatus(messages[error.message] || "That SSH public key could not be registered.", "bad");
    } finally {
      if (button) {
        button.disabled = false;
        button.textContent = "Add SSH key";
      }
    }
  }

  async function revokeSshKey(keyId) {
    if (!keyId || !await confirmAccountAction({
      title: "Revoke this SSH key?",
      body: "It will stop authenticating immediately.",
      confirmLabel: "Revoke key",
    })) return;
    try {
      await sshKeyApi("DELETE", "/" + encodeURIComponent(keyId));
      setSshKeyStatus("SSH key revoked.", "good");
      sshKeysLoaded = false;
      await loadSshKeys({ force: true });
    } catch (_) {
      setSshKeyStatus("Could not revoke that SSH key.", "bad");
    }
  }

  // ---- Organizations (settings tab) -----------------------------------------
  // Client for the /api/orgs endpoints:
  // list/create the orgs you belong to, then a master/detail panel to manage
  // one org's members, teams, and linked repos. All rendered lazily into
  // [data-orgs-root] the first time the Organizations settings tab is opened.
  // Mirrors ORG_ROLES in the Worker (organization_rbac.py, strongest first).
  // The server decides what each role may do; only owners can hand out
  // owner/admin, and the members API refuses anything the matrix denies.
  const ORG_ROLE_OPTIONS = ["owner", "admin", "maintainer", "member", "auditor", "guest"];
  const ORG_TEAM_PERMISSIONS = ["read", "write", "maintain", "admin"];
  const ORG_WORLD_ACCESS_OPTIONS = ["public", "restricted", "private"];
  // Keep these aliases aligned with world-office-tower.js and the Worker's
  // OFFICE_FLOOR_TEAM_ALIASES. The server remains authoritative for elevator
  // access; this organization-admin matrix explains every floor group a user
  // can belong to and highlights access granted by their current teams.
  const ORG_OFFICE_FLOOR_GROUPS = Object.freeze([
    { id: "marketing", label: "Marketing", aliases: ["marketing", "marketing-team", "growth", "brand", "comms", "communications"] },
    { id: "engineering", label: "Engineering", aliases: ["engineering", "engineers", "development", "developers", "platform", "frontend", "backend"] },
    { id: "infrastructure", label: "Infrastructure", aliases: ["infrastructure", "infra", "devops", "site-reliability", "sre"] },
  ]);

  // Worker org-endpoint error codes -> human text. Unknown codes fall through
  // to a generic message so the UI never shows a raw slug.
  const ORG_ERROR_TEXT = {
    invalid_org_name: "Invalid name. Use lowercase letters, numbers and dashes.",
    invalid_team_name: "Invalid team name. Use lowercase letters, numbers and dashes.",
    org_name_taken: "That name is already taken.",
    too_many_orgs: "You have reached the organization limit for this account.",
    too_many_members: "This organization has reached its member limit.",
    too_many_teams: "This organization has reached its team limit.",
    too_many_repos: "This organization has reached its linked-repo limit.",
    invalid_session: "Sign in to manage organizations.",
    forbidden: "You do not have permission to do that.",
    not_found: "Not found.",
    last_owner: "An organization must keep at least one owner.",
    unknown_account: "No account exists with that name.",
    not_a_member: "That account is not a member of this organization.",
    bad_role: "Invalid role.",
    bad_permission: "Invalid permission.",
    member_required: "Enter a member account name.",
    repo_required: "Enter a repository name.",
    not_your_node: "You can only link repositories from your own node.",
    unknown_repo: "That repository is not published on your node.",
    enabled_required: "Choose whether organization digests are enabled.",
    invalid_logo_url: "Logo URLs must use HTTPS and cannot contain credentials or fragments.",
    invalid_world_access: "Choose a valid access level for every organization space.",
    invalid_provider: "Choose Codex or Claude Code.",
    invalid_bot_permissions: "Choose at least one valid bot permission.",
    invalid_expiration: "Bot tokens may expire in 1–365 days.",
    too_many_bot_tokens: "This organization has reached its active bot-token limit.",
    invalid_token_id: "That bot token is invalid.",
  };

  function orgErrorText(code) {
    return ORG_ERROR_TEXT[code] || (code ? "Request failed (" + code + ")." : "Request failed.");
  }

  // fetchJson only does cached GETs; org writes need POST/DELETE with the
  // session bearer token and must surface the structured {error} body, so they
  // go through this dedicated helper instead.
  async function orgApiRequest(method, path, body) {
    const token = state.session?.sessionToken || "";
    const headers = { accept: "application/json" };
    if (body !== undefined) headers["content-type"] = "application/json";
    if (token) headers.authorization = "Bearer " + token;
    const response = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(orgErrorText(data.error || ""));
      error.code = data.error || "http_" + response.status;
      error.data = data;
      throw error;
    }
    return data;
  }

  function orgsRoot() {
    return $("[data-orgs-root]");
  }

  let orgsSectionLoaded = false;
  function initOrgsSection() {
    if (orgsSectionLoaded) return;
    orgsSectionLoaded = true;
    showOrgsList();
  }

  function orgSignedIn() {
    return Boolean(state.session?.nodeName) && state.session?.kind !== "preview";
  }

  function orgRoleBadge(role) {
    const clean = String(role || "member").toLowerCase();
    const tone = clean === "owner"
      ? " fm-label-accent"
      : clean === "admin"
        ? " fm-label-attention"
        : clean === "guest"
          ? " border-dashed"
          : "";
    return '<span class="fm-label' + tone + '">' + escapeHtml(clean) + "</span>";
  }

  function orgMemberFloorGroups(member, canManage) {
    const teams = new Set(
      (Array.isArray(member?.teams) ? member.teams : [])
        .map((team) => String(team || "").trim().toLowerCase())
        .filter(Boolean),
    );
    const groups = ORG_OFFICE_FLOOR_GROUPS.map((group) => {
      const matchingTeams = group.aliases.filter((team) => teams.has(team));
      const active = matchingTeams.length > 0;
      const state = active ? "In group" : "Available";
      const title = active
        ? "Remove from " + group.label + " (" + matchingTeams.join(", ") + ")"
        : "Add to " + group.label;
      const tag = canManage ? "button" : "span";
      const controls = canManage
        ? ' type="button" data-org-member-floor-group="' + escapeHtml(group.id) +
          '" data-org-member="' + escapeHtml(member?.name || "") +
          '" aria-pressed="' + (active ? "true" : "false") + '"'
        : "";
      return "<" + tag + controls + ' title="' + escapeHtml(title) + '" aria-label="' +
        escapeHtml(group.label + ": " + state) + '" class="inline-flex items-center gap-1 rounded-full border px-2 py-1 text-[11px] font-semibold transition-colors ' +
        (active
          ? "border-border bg-secondary text-foreground"
          : "border-border bg-background text-muted-foreground") +
        (canManage
          ? " cursor-pointer hover:border-primary/70 hover:bg-primary/10 hover:text-primary disabled:cursor-wait disabled:opacity-60"
          : "") + '">' +
        '<span aria-hidden="true" class="h-1.5 w-1.5 rounded-full ' +
        (active ? "bg-foreground" : "bg-muted-foreground/40") + '"></span>' +
        escapeHtml(group.label) + "</" + tag + ">";
    }).join("");
    const activeCount = ORG_OFFICE_FLOOR_GROUPS.filter(
      (group) => group.aliases.some((team) => teams.has(team)),
    ).length;
    return '<div data-org-member-floor-groups class="mt-2 border-t border-border/70 pt-2">' +
      '<div class="mb-1.5 flex items-center justify-between gap-2 text-[11px] text-muted-foreground">' +
        '<span class="font-semibold text-foreground">Office floor groups</span>' +
        '<span>' + activeCount + " of " + ORG_OFFICE_FLOOR_GROUPS.length + " active</span>" +
      '</div><div class="flex flex-wrap gap-1.5">' + groups + "</div></div>";
  }

  function orgOptionTags(options, selected) {
    return options.map((opt) =>
      '<option value="' + escapeHtml(opt) + '"' + (opt === selected ? " selected" : "") + ">" +
      escapeHtml(opt) + "</option>").join("");
  }

  function orgStatus(root, message, ok) {
    const status = root.querySelector("[data-org-status]");
    if (!status) return;
    status.textContent = message || "";
    status.className = "min-h-4 text-xs " + (ok ? "text-muted-foreground" : "text-destructive");
  }

  const ORG_INPUT_CLASS = "h-10 rounded-md border border-border bg-card px-3 text-sm font-normal " +
    "text-foreground placeholder:text-muted-foreground focus:outline-none focus:ring-1 focus:ring-primary";
  const ORG_BTN_PRIMARY = "fm-btn fm-btn-cta";
  const ORG_BTN_SECONDARY = "fm-btn";

  async function showOrgsList() {
    const root = orgsRoot();
    if (!root) return;
    if (!orgSignedIn()) {
      root.innerHTML = '<p class="text-sm text-muted-foreground">Sign in to create and manage organizations.</p>';
      return;
    }
    root.innerHTML = '<p class="text-sm text-muted-foreground">' + loadingHtml("Loading organizations…") + "</p>";
    let orgs = [];
    try {
      const data = await orgApiRequest("GET", "/api/orgs");
      orgs = Array.isArray(data.orgs) ? data.orgs : [];
    } catch (error) {
      root.innerHTML = '<p class="text-sm text-destructive">' + escapeHtml(error.message) + "</p>";
      return;
    }
    const rows = orgs.length
      ? orgs.map((org) => {
          const name = escapeHtml(org.name || "");
          return '<button type="button" data-org-open="' + name + '" ' +
            'class="flex w-full items-center gap-3 px-4 py-3 text-left hover:bg-secondary transition-colors">' +
            '<i data-lucide="building-2" class="h-4 w-4 text-muted-foreground"></i>' +
            '<span class="min-w-0 flex-1 truncate text-sm font-semibold text-foreground">' + name + "</span>" +
            orgRoleBadge(org.role) +
            '<i data-lucide="chevron-right" class="h-4 w-4 text-muted-foreground"></i>' +
            "</button>";
        }).join("")
      : accountBlankslate("building-2", "No organizations yet.", "Create one below.");
    root.innerHTML =
      '<div class="divide-y divide-border overflow-hidden rounded-md border border-border bg-card">' + rows + "</div>" +
      '<form data-org-create class="mt-6 grid max-w-md gap-3 border-t border-border pt-6">' +
        '<h3 class="text-sm font-semibold text-foreground">New organization</h3>' +
        '<input data-org-name required placeholder="name (lowercase, dashes)" autocomplete="off" class="' + ORG_INPUT_CLASS + '" />' +
        '<input data-org-display placeholder="Display name (optional)" autocomplete="off" class="' + ORG_INPUT_CLASS + '" />' +
        '<input data-org-description placeholder="Description (optional)" autocomplete="off" class="' + ORG_INPUT_CLASS + '" />' +
        '<p data-org-status class="min-h-4 text-xs text-muted-foreground"></p>' +
        '<button type="submit" class="' + ORG_BTN_PRIMARY + ' w-fit">Create organization</button>' +
      "</form>";
    window.lucide?.createIcons();
    root.querySelector("[data-org-create]")?.addEventListener("submit", onCreateOrgSubmit);
    root.querySelectorAll("[data-org-open]").forEach((button) => {
      button.addEventListener("click", () => showOrgDetail(button.dataset.orgOpen));
    });
  }

  async function onCreateOrgSubmit(event) {
    event.preventDefault();
    const root = orgsRoot();
    if (!root) return;
    const name = (root.querySelector("[data-org-name]")?.value || "").trim().toLowerCase();
    if (!name) {
      orgStatus(root, "Enter an organization name.", false);
      return;
    }
    const body = { name };
    const display = (root.querySelector("[data-org-display]")?.value || "").trim();
    const description = (root.querySelector("[data-org-description]")?.value || "").trim();
    if (display) body.displayName = display;
    if (description) body.description = description;
    orgStatus(root, "Creating…", true);
    try {
      const created = await orgApiRequest("POST", "/api/orgs", body);
      showOrgDetail(created.org || name);
    } catch (error) {
      orgStatus(root, error.message, false);
    }
  }

  async function showOrgDetail(name) {
    const root = orgsRoot();
    if (!root || !name) return;
    root.innerHTML = '<p class="text-sm text-muted-foreground">' + loadingHtml("Loading organization…") + "</p>";
    let profile;
    let members = [];
    let teams = [];
    let repos = [];
    let fediverse = { controls: { enabled: true }, repos: [] };
    let botTokens = { permissions: [], tokens: [], usagePreview: [] };
    try {
      const [profileData, membersData, teamsData, reposData] = await Promise.all([
        orgApiRequest("GET", "/api/orgs/" + encodeURIComponent(name)),
        orgApiRequest("GET", "/api/orgs/" + encodeURIComponent(name) + "/members"),
        orgApiRequest("GET", "/api/orgs/" + encodeURIComponent(name) + "/teams").catch(() => ({ teams: [] })),
        orgApiRequest("GET", "/api/orgs/" + encodeURIComponent(name) + "/repos"),
      ]);
      profile = profileData;
      members = membersData.members || [];
      teams = teamsData.teams || [];
      repos = reposData.repos || [];
      if (profile.viewerRole === "owner" || profile.viewerRole === "admin") {
        [fediverse, botTokens] = await Promise.all([
          orgApiRequest(
            "GET", "/api/orgs/" + encodeURIComponent(name) + "/fediverse"),
          orgApiRequest(
            "GET", "/api/orgs/" + encodeURIComponent(name) + "/bot-tokens"),
        ]);
      }
    } catch (error) {
      root.innerHTML =
        '<button type="button" data-org-back class="mb-4 inline-flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground">' +
        '<i data-lucide="arrow-left" class="h-4 w-4"></i> Back</button>' +
        '<p class="text-sm text-destructive">' + escapeHtml(error.message) + "</p>";
      window.lucide?.createIcons();
      root.querySelector("[data-org-back]")?.addEventListener("click", showOrgsList);
      return;
    }
    renderOrgDetail(
      root, name, profile, members, teams, repos, fediverse, botTokens);
    void loadOrgPullRules(root, name);
  }

  async function loadOrgPullRules(root, name) {
    const holder = root.querySelector("[data-org-pull-rules]");
    if (!holder) return;
    let data;
    try {
      data = await orgApiRequest(
        "GET", "/api/orgs/" + encodeURIComponent(name) + "/pull-rules");
    } catch (error) {
      holder.innerHTML = '<p class="text-xs text-muted-foreground">' +
        escapeHtml(error.message) + "</p>";
      return;
    }
    const updated = data.updatedAt
      ? '<p class="text-[11px] text-muted-foreground">Last changed by ' +
          escapeHtml(data.updatedBy || "an admin") + " on " +
          escapeHtml(new Date(data.updatedAt).toLocaleString()) + ".</p>"
      : "";
    holder.innerHTML = updated + pullRulesEditorHtml(data.policy, {
      editable: data.canManage === true,
      models: data.models,
      saveLabel: "Save organization rules",
    });
    window.lucide?.createIcons();
    bindPullRulesEditor(holder, {
      models: data.models,
      onSave: async (policy) => {
        try {
          await orgApiRequest(
            "POST", "/api/orgs/" + encodeURIComponent(name) + "/pull-rules",
            { policy });
        } catch (error) {
          throw new Error(error.data?.message || error.message);
        }
        return policy.rules.length
          ? "Saved. Every pull request on a linked repository is checked against these rules."
          : "Saved. This organization sets no pull request rules.";
      },
    });
  }

  function orgRemoteMcpPrompt(name, mode, token) {
    const deploy = mode === "deploy";
    const configuration = {
      mcpServers: {
        forkmesh: {
          type: "http",
          url: publicSiteOrigin() + "/mcp",
          headers: {
            Authorization: "Bearer " + token,
          },
        },
      },
    };
    const finish = deploy
      ? [
          "Fetch the latest main again and integrate it into the task branch without force-pushing.",
          "Push the branch, merge only when the merge and required checks are clean, then deploy using the repository's documented deploy command.",
          "Verify the live revision and production behavior. Only then call complete_org_task with the commit, deployed revision, tests, and exact QA steps.",
        ]
      : [
          `Push the task branch and open a focused pull request against main using the repository's normal ${platformName()} tooling.`,
          "Do not merge or deploy. After the pull request exists, call complete_org_task with its number, branch, tests, and exact QA steps.",
        ];
    return [
      "Configure the following remote Streamable HTTP MCP server named forkmesh, then immediately use it for this assignment:",
      "",
      JSON.stringify(configuration, null, 2),
      "",
      `Work the shared task list for the ${name} organization.`,
      "1. Call list_org_tasks. Select the highest-priority unfinished task routed to an agent or explicitly unassigned with a linked repository. Never take a task assigned to a person.",
      "2. Call get_org_task for the selected id. Read its repository instructions and inspect the latest main branch before planning.",
      "3. Keep the base checkout untouched. Fetch main and create a dedicated worktree and task branch. Never stash, reset, clean, commit, or edit the base checkout.",
      "4. Work only in that task worktree. Preserve unrelated changes, implement the smallest complete fix, and run focused tests plus repository-required checks.",
      "5. Commit only the task files with a clear message. Never force-push or bypass failing checks or conflicts.",
      ...finish.map((line, index) => `${index + 6}. ${line}`),
      "",
      "If credentials, authorization, infrastructure, tests, or conflict resolution block safe completion, report the blocker and do not mark the task complete. Treat the bearer credential above as a secret; never print it in logs, commits, pull requests, task notes, or chat. It can be revoked from Organization Admin.",
    ].join("\n");
  }

  function orgRemoteMcpSetup(name) {
    const workflows = [
      {
        id: "pr",
        icon: "git-pull-request-arrow",
        title: "Remote MCP → pull request",
        description: "Creates an isolated task branch and pull request, then sends the completed task to QA without deploying.",
      },
      {
        id: "deploy",
        icon: "rocket",
        title: "Remote MCP → verified deployment",
        description: "Creates isolated work, synchronizes it safely, deploys, verifies production, and then sends the task to QA.",
      },
    ];
    return '<section class="mt-6 rounded-md border border-border bg-card p-4" data-org-remote-mcp>' +
      '<div class="flex items-center gap-2"><i data-lucide="plug-zap" class="h-4 w-4 text-muted-foreground"></i>' +
        '<h4 class="text-sm font-semibold text-foreground">Remote ' + escapeHtml(platformName()) + ' MCP</h4></div>' +
      '<p class="mt-1 max-w-3xl text-xs leading-5 text-muted-foreground">Generate a revocable task-only credential and one complete paste block. It connects the agent directly to <span class="font-mono text-foreground">' +
        escapeHtml(publicSiteOrigin() + "/mcp") +
        '</span>; there is no local Python server, repository path, or Qt connector token to fill in.</p>' +
      '<div class="mt-4 grid gap-4 xl:grid-cols-2">' +
        workflows.map((workflow) =>
          '<article class="rounded-md border border-border bg-card p-3" data-org-remote-mcp-workflow="' + workflow.id + '">' +
            '<div class="flex items-start gap-2"><i data-lucide="' + workflow.icon + '" class="mt-0.5 h-4 w-4 shrink-0 text-muted-foreground"></i>' +
              '<div class="min-w-0 flex-1"><h5 class="text-sm font-semibold text-foreground">' + workflow.title + '</h5>' +
              '<p class="mt-1 text-[11px] leading-4 text-muted-foreground">' + workflow.description + "</p></div></div>" +
            '<button type="button" data-org-remote-mcp-generate="' + workflow.id + '" class="' + ORG_BTN_SECONDARY + ' fm-btn-sm mt-3">' +
              '<i data-lucide="key-round"></i> Generate complete setup</button>' +
            '<div data-org-remote-mcp-output="' + workflow.id + '" hidden class="mt-3">' +
              '<p class="flex items-center gap-1.5 text-xs font-semibold text-attention"><i data-lucide="triangle-alert" class="h-3.5 w-3.5 shrink-0"></i>Shown once. This block contains a live credential.</p>' +
              '<textarea readonly rows="20" spellcheck="false" class="mt-1 w-full resize-y rounded-md border border-border bg-background p-2 font-mono text-[10px] leading-4 text-foreground"></textarea>' +
              '<button type="button" data-org-remote-mcp-copy="' + workflow.id + '" class="' + ORG_BTN_SECONDARY + ' fm-btn-sm mt-2"><i data-lucide="copy"></i> Copy complete prompt</button>' +
            "</div>" +
          "</article>"
        ).join("") +
      "</div>" +
      '<p data-org-remote-mcp-status class="mt-3 min-h-4 text-xs text-muted-foreground" role="status"></p>' +
    "</section>";
  }

  function renderOrgDetail(
    root, name, profile, members, teams, repos, fediverse, botTokens,
  ) {
    const canManage = profile.viewerRole === "owner" || profile.viewerRole === "admin";
    const isOwner = profile.viewerRole === "owner";
    const title = escapeHtml(profile.displayName || profile.org || name);
    const worldAccess = {
      lobby: profile.worldAccess?.lobby || "public",
      floors: profile.worldAccess?.floors || "restricted",
      offices: profile.worldAccess?.offices || "restricted",
    };
    const worldSettings = canManage
      ? '<form data-org-world-settings class="mt-5 grid gap-3 rounded-md border border-border bg-card p-4">' +
          '<div><h4 class="text-sm font-semibold text-foreground">World building</h4>' +
          '<p class="mt-1 text-xs leading-5 text-muted-foreground">Set the public identity of this building and independently control its lobby, repository floors, and personal offices. Restricted spaces are visible to organization members; private spaces are limited to owners and administrators.</p></div>' +
          '<div class="grid gap-3 md:grid-cols-2">' +
            '<label class="grid gap-1 text-xs font-semibold text-foreground">Display name' +
              '<input data-org-world-display maxlength="80" value="' + escapeHtml(profile.displayName || "") + '" class="' + ORG_INPUT_CLASS + '" /></label>' +
            '<label class="grid gap-1 text-xs font-semibold text-foreground">HTTPS logo URL' +
              '<input data-org-world-logo type="url" inputmode="url" maxlength="500" value="' + escapeHtml(profile.logoUrl || "") + '" placeholder="https://…" class="' + ORG_INPUT_CLASS + '" /></label>' +
          '</div>' +
          '<label class="grid gap-1 text-xs font-semibold text-foreground">Description' +
            '<textarea data-org-world-description maxlength="500" rows="3" class="' + ORG_INPUT_CLASS + ' h-auto py-2">' + escapeHtml(profile.description || "") + "</textarea></label>" +
          '<div class="grid gap-3 sm:grid-cols-3">' +
            '<label class="grid gap-1 text-xs font-semibold text-foreground">Lobby access<select data-org-world-lobby class="' + ORG_INPUT_CLASS + '">' +
              orgOptionTags(ORG_WORLD_ACCESS_OPTIONS, worldAccess.lobby) + "</select></label>" +
            '<label class="grid gap-1 text-xs font-semibold text-foreground">Repository floors<select data-org-world-floors class="' + ORG_INPUT_CLASS + '">' +
              orgOptionTags(ORG_WORLD_ACCESS_OPTIONS, worldAccess.floors) + "</select></label>" +
            '<label class="grid gap-1 text-xs font-semibold text-foreground">Personal offices<select data-org-world-offices class="' + ORG_INPUT_CLASS + '">' +
              orgOptionTags(ORG_WORLD_ACCESS_OPTIONS, worldAccess.offices) + "</select></label>" +
          "</div>" +
          '<button type="submit" class="' + ORG_BTN_SECONDARY + ' w-fit">Save world settings</button>' +
        "</form>"
      : '<section class="mt-5 rounded-md border border-border bg-card p-4">' +
          '<h4 class="text-sm font-semibold text-foreground">World building access</h4>' +
          '<p class="mt-1 text-xs text-muted-foreground">Lobby: ' + escapeHtml(worldAccess.lobby) +
          " · floors: " + escapeHtml(worldAccess.floors) +
          " · offices: " + escapeHtml(worldAccess.offices) + "</p></section>";

    const memberRows = members.map((member) => {
      const memberName = escapeHtml(member.name || "");
      rememberUserNameCase(member.name, member.nameCase);
      const controls = canManage
        ? '<select data-org-member-role="' + memberName + '" class="' + ORG_INPUT_CLASS + ' h-8 py-0">' +
            orgOptionTags(ORG_ROLE_OPTIONS, member.role) + "</select>" +
          '<button type="button" data-org-member-remove="' + memberName + '" title="Remove from org" ' +
            'class="fm-btn fm-btn-sm fm-btn-icon fm-btn-danger">' +
            '<i data-lucide="user-minus" class="h-4 w-4"></i></button>'
        : orgRoleBadge(member.role);
      return '<div class="rounded-md border border-border bg-card px-3 py-2">' +
        '<div class="flex items-center gap-2">' +
          '<span class="min-w-0 flex-1 truncate text-sm font-medium text-foreground">' + userNameHtml(member.name || "") + "</span>" +
          controls +
        "</div>" +
        orgMemberFloorGroups(member, canManage) +
      "</div>";
    }).join("") || '<p class="text-sm text-muted-foreground">No members.</p>';

    const teamRows = teams.map((team) => {
      const teamName = escapeHtml(team.team || "");
      const controls = canManage
        ? '<select data-org-team-perm="' + teamName + '" class="' + ORG_INPUT_CLASS + ' h-8 py-0">' +
            orgOptionTags(ORG_TEAM_PERMISSIONS, team.permission) + "</select>" +
          '<button type="button" data-org-team-delete="' + teamName + '" title="Delete team" ' +
            'class="fm-btn fm-btn-sm fm-btn-icon fm-btn-danger">' +
            '<i data-lucide="trash-2" class="h-4 w-4"></i></button>'
        : '<span class="inline-flex items-center rounded-full border border-border bg-secondary px-2 py-0.5 text-xs font-semibold text-muted-foreground">' +
            escapeHtml(team.permission || "") + "</span>";
      return '<div class="flex items-center gap-2 rounded-md border border-border bg-card px-3 py-2">' +
        '<button type="button" data-org-team-open="' + teamName + '" class="min-w-0 flex-1 truncate text-left text-sm font-medium text-foreground hover:underline">' +
          teamName + ' <span class="text-xs text-muted-foreground">· ' + (team.members || 0) + ' member' + ((team.members === 1) ? "" : "s") + "</span></button>" +
        controls + "</div>";
    }).join("") || '<p class="text-sm text-muted-foreground">No teams.</p>';

    const repoRows = repos.map((repo) => {
      const repoName = escapeHtml(repo.repo || "");
      const serves = repo.node ? '<span class="text-xs text-muted-foreground">serves ' + escapeHtml(repo.node) + "/" + repoName + "</span>" : "";
      const control = canManage
        ? '<button type="button" data-org-repo-unlink="' + repoName + '" title="Unlink repo" ' +
            'class="fm-btn fm-btn-sm fm-btn-icon fm-btn-danger">' +
            '<i data-lucide="unlink" class="h-4 w-4"></i></button>'
        : "";
      return '<div class="flex items-center gap-2 rounded-md border border-border bg-card px-3 py-2">' +
        '<div class="min-w-0 flex-1"><div class="truncate text-sm font-medium text-foreground">' + escapeHtml(name) + "/" + repoName + "</div>" + serves + "</div>" +
        control + "</div>";
    }).join("") || '<p class="text-sm text-muted-foreground">No repos linked.</p>';

    const memberAdd = canManage
      ? '<form data-org-member-add class="mt-2 flex flex-wrap items-start gap-2">' +
          '<div class="min-w-[14rem] flex-1">' +
            '<input data-member-name list="org-member-suggestions" role="combobox" aria-autocomplete="list" aria-controls="org-member-suggestions" placeholder="Start typing an account name" autocomplete="off" class="' + ORG_INPUT_CLASS + ' w-full" />' +
            '<datalist id="org-member-suggestions" data-org-member-suggestions></datalist>' +
            '<p data-org-member-search-status class="mt-1 min-h-4 text-[11px] text-muted-foreground">Type a name to search active ' + escapeHtml(platformName()) + ' accounts.</p>' +
          "</div>" +
          '<select data-member-role class="' + ORG_INPUT_CLASS + '">' + orgOptionTags(ORG_ROLE_OPTIONS, "member") + "</select>" +
          '<button type="submit" class="' + ORG_BTN_SECONDARY + '">Add member</button></form>'
      : "";
    const teamAdd = canManage
      ? '<form data-org-team-add class="mt-2 flex flex-wrap items-center gap-2">' +
          '<input data-team-name placeholder="team name" autocomplete="off" class="' + ORG_INPUT_CLASS + ' flex-1 min-w-[10rem]" />' +
          '<select data-team-perm class="' + ORG_INPUT_CLASS + '">' + orgOptionTags(ORG_TEAM_PERMISSIONS, "read") + "</select>" +
          '<button type="submit" class="' + ORG_BTN_SECONDARY + '">Add team</button></form>'
      : "";
    const account = String(state.session?.nodeName || "").toLowerCase();
    const repoAdd = canManage
      ? '<form data-org-repo-add class="mt-2 flex flex-wrap items-center gap-2">' +
          '<input data-repo-name placeholder="repo name" autocomplete="off" class="' + ORG_INPUT_CLASS + ' flex-1 min-w-[10rem]" />' +
          '<input data-repo-node value="' + escapeHtml(account) + '" placeholder="your node" autocomplete="off" class="' + ORG_INPUT_CLASS + '" />' +
          '<button type="submit" class="' + ORG_BTN_SECONDARY + '">Link repo</button></form>'
      : "";
    const digestRows = (fediverse?.repos || []).map((item) => {
      const repoName = escapeHtml(item.repo || "");
      const preview = String(item.preview?.text || "").trim();
      const status = !item.ownerEnabled
        ? "Disabled by repository owner"
        : item.enabled ? "Eligible for daily digest" : "Disabled for organization";
      return '<details class="rounded-md border border-border bg-card p-3">' +
        '<summary class="cursor-pointer text-sm font-medium text-foreground">' +
          escapeHtml(name) + "/" + repoName +
          ' <span class="ml-1 text-xs font-normal text-muted-foreground">· ' +
          escapeHtml(status) + "</span></summary>" +
        '<pre class="mt-2 max-h-52 overflow-auto whitespace-pre-wrap break-words rounded bg-background p-2 font-mono text-[10px] leading-4 text-foreground">' +
          escapeHtml(preview || "No meaningful public updates are queued.") +
        "</pre></details>";
    }).join("") || '<p class="text-sm text-muted-foreground">No public linked repositories have queued updates.</p>';
    const digestControls = canManage
      ? '<section class="mt-6" data-org-fediverse-controls>' +
          '<div class="flex items-start justify-between gap-4">' +
            '<div><h4 class="text-sm font-semibold text-foreground">Fediverse daily digests</h4>' +
            '<p class="mt-1 max-w-2xl text-xs leading-5 text-muted-foreground">Combine meaningful public updates for each organization alias into at most one clearly automated post per 24 hours. Repository-owner federation settings remain authoritative; empty posts are never sent.</p></div>' +
            '<label class="inline-flex shrink-0 items-center gap-2 text-xs font-semibold text-foreground"><input data-org-fediverse-enabled type="checkbox" class="h-4 w-4"' +
              (fediverse?.controls?.enabled !== false ? " checked" : "") +
              " />Enabled</label>" +
          "</div>" +
          '<div class="mt-3 grid gap-2">' + digestRows + "</div>" +
        "</section>"
      : "";
    const botTokenControls = canManage
      ? (() => {
          const permissionRows = (botTokens?.permissions || []).map((permission) =>
            '<label class="flex items-start gap-2 rounded-md border border-border bg-background px-3 py-2 text-xs text-foreground">' +
              '<input type="checkbox" data-org-bot-scope value="' +
                escapeHtml(permission.scope || "") + '" class="mt-0.5 h-4 w-4"' +
                (permission.default === false ? "" : " checked") + " />" +
              '<span><strong class="font-mono">' +
                escapeHtml(permission.scope || "") + "</strong>" +
                '<span class="mt-0.5 block leading-4 text-muted-foreground">' +
                  escapeHtml(permission.description || "") +
                "</span></span></label>"
          ).join("");
          const tokenRows = (botTokens?.tokens || []).map((token) => {
            const revoked = Number(token.revokedAt || 0) > 0;
            const linked = token.deviceName
              ? "linked to " + escapeHtml(token.deviceName)
              : "not linked yet";
            return '<div class="rounded-md border border-border bg-card p-3">' +
              '<div class="flex items-start gap-3">' +
                '<div class="min-w-0 flex-1"><strong class="block truncate text-sm text-foreground">' +
                  escapeHtml(token.label || token.provider || "Bot") +
                "</strong>" +
                '<span class="mt-1 block text-xs text-muted-foreground">' +
                  escapeHtml(token.provider || "") + " · " + linked +
                  (revoked ? " · revoked" : "") + "</span></div>" +
                (!revoked
                  ? '<button type="button" data-org-bot-revoke="' +
                      escapeHtml(token.id || "") + '" class="' +
                      ORG_BTN_SECONDARY + ' fm-btn-sm fm-btn-danger">Revoke</button>'
                  : "") +
              "</div>" +
              '<p class="mt-2 break-all font-mono text-[10px] text-muted-foreground">' +
                escapeHtml((token.scopes || []).join(" · ")) + "</p></div>";
          }).join("") || '<p class="text-sm text-muted-foreground">No bot tokens yet.</p>';
          const usageRows = (botTokens?.usagePreview || []).slice(0, 5).map((usage) =>
            '<li class="flex items-start justify-between gap-3 rounded-md border border-border bg-background px-3 py-2">' +
              '<span class="min-w-0"><strong class="block truncate text-xs text-foreground">' +
                escapeHtml(usage.label || usage.provider || "Bot token") +
              '</strong><span class="mt-0.5 block truncate font-mono text-[10px] text-muted-foreground">' +
                escapeHtml((usage.method || "GET") + " " + (usage.action || "/api/bot/session")) +
              "</span></span>" +
              '<time class="shrink-0 text-[10px] text-muted-foreground" title="' +
                escapeHtml(formatDate(Number(usage.usedAt || 0))) + '">' +
                escapeHtml(formatTimeAgo(Number(usage.usedAt || 0))) +
              "</time></li>"
          ).join("") || '<li class="text-xs text-muted-foreground">No token use recorded yet.</li>';
          return '<section class="mt-6 rounded-md border border-border bg-card p-4" data-org-bot-tokens>' +
            '<div class="flex items-center gap-2"><i data-lucide="bot" class="h-4 w-4 text-muted-foreground"></i>' +
              '<h4 class="text-sm font-semibold text-foreground">Bot tokens</h4></div>' +
            '<p class="mt-1 text-xs leading-5 text-muted-foreground">Create a revocable Codex or Claude credential for an attended local computer. The secret is shown once. Destructive node, membership, organization-administration, and merge permissions are intentionally unavailable.</p>' +
            '<form data-org-bot-create class="mt-4 grid gap-3">' +
              '<div class="grid gap-3 sm:grid-cols-3">' +
                '<label class="grid gap-1 text-xs font-semibold text-foreground">Provider<select data-org-bot-provider class="' +
                  ORG_INPUT_CLASS + '"><option value="codex">Codex</option><option value="claude-code">Claude Code</option></select></label>' +
                '<label class="grid gap-1 text-xs font-semibold text-foreground">Label<input data-org-bot-label maxlength="80" value="Codex bot" class="' +
                  ORG_INPUT_CLASS + '" /></label>' +
                '<label class="grid gap-1 text-xs font-semibold text-foreground">Expires in days<input data-org-bot-expiry type="number" min="1" max="365" value="90" class="' +
                  ORG_INPUT_CLASS + '" /></label>' +
              "</div>" +
              '<div><span class="text-xs font-semibold text-foreground">Permissions</span>' +
                '<div class="mt-2 grid gap-2 md:grid-cols-2">' + permissionRows + "</div></div>" +
              '<button type="submit" class="' + ORG_BTN_PRIMARY + ' w-fit">Create bot token</button>' +
            "</form>" +
            '<div data-org-bot-secret hidden class="mt-4 rounded-md border border-attention/40 bg-attention/10 p-3"></div>' +
            '<div class="mt-4 grid gap-2">' + tokenRows + "</div>" +
            '<div class="mt-5 border-t border-border pt-4" data-org-bot-usage-preview>' +
              '<h5 class="text-xs font-semibold text-foreground">Recent token use</h5>' +
              '<p class="mt-1 text-[11px] leading-4 text-muted-foreground">Latest five successful authentications. The full metadata-only log is available to platform administrators; secrets, bodies, network addresses, and user agents are never logged.</p>' +
              '<ol class="mt-2 grid gap-2">' + usageRows + "</ol>" +
            "</div>" +
          "</section>";
        })()
      : "";
    const accessSummary =
      '<aside class="rounded-md border border-border bg-subtle p-4 lg:sticky lg:top-4 lg:self-start" data-org-access-summary>' +
        '<div class="flex items-center gap-2"><i data-lucide="shield-check" class="h-4 w-4 text-muted-foreground"></i>' +
          '<h4 class="text-sm font-semibold text-foreground">What organization access means</h4></div>' +
        '<div class="mt-3 grid gap-3 text-xs leading-5 text-muted-foreground">' +
          '<section><strong class="block text-foreground">Every organization member can</strong>' +
            '<ul class="mt-1 list-disc space-y-1 pl-5">' +
              '<li>View the organization, linked repositories, member directory, and teams.</li>' +
              '<li>Enter member-restricted World floors and see non-private organization activity.</li>' +
            "</ul></section>" +
          '<section><strong class="block text-foreground">Engineering team members can</strong>' +
            '<p class="mt-1">View organization agents and start, review, re-prompt, and continue Claude or Codex sessions on approved headless mirrors. This is checked server-side on every agent request; organization role alone does not grant access.</p></section>' +
          '<section><strong class="block text-foreground">Member role cannot</strong>' +
            '<p class="mt-1">Change organization settings, membership, teams, or repository links. Members outside Engineering also cannot view or control Claude/Codex sessions. Team permissions can grant repository work, but protected mirror approval and merge remain owner-only.</p></section>' +
          '<section><strong class="block text-foreground">Admin role adds</strong>' +
            '<p class="mt-1">Organization settings, members, teams, linked repositories, World access, and federation controls. Admins cannot remove the final owner.</p></section>' +
          '<section><strong class="block text-foreground">Owner role adds</strong>' +
            '<p class="mt-1">Owner-only protected mirror approval/merge controls and organization deletion. The organization must always retain one owner.</p></section>' +
          '<section><strong class="block text-foreground">Team permission</strong>' +
            '<p class="mt-1"><span class="font-mono text-foreground">read</span> views; <span class="font-mono text-foreground">write</span> changes content; <span class="font-mono text-foreground">maintain</span> manages repository work; <span class="font-mono text-foreground">admin</span> manages repository access. Team permission does not change the member’s organization role.</p></section>' +
        "</div>" +
      "</aside>";
    const memberAndTeamSettings =
      '<div class="mt-5 grid gap-6 lg:grid-cols-[minmax(0,1fr)_22rem]">' +
        '<div>' +
          '<section><h4 class="text-sm font-semibold text-foreground">Members</h4>' +
            '<div class="mt-2 grid gap-2">' + memberRows + "</div>" + memberAdd + "</section>" +
          '<section class="mt-6"><h4 class="text-sm font-semibold text-foreground">Teams</h4>' +
            '<div class="mt-2 grid gap-2">' + teamRows + "</div>" + teamAdd + "</section>" +
        "</div>" +
        accessSummary +
      "</div>";

    root.innerHTML =
      '<div class="flex items-center gap-3">' +
        '<button type="button" data-org-back class="inline-flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground">' +
          '<i data-lucide="arrow-left" class="h-4 w-4"></i> Organizations</button>' +
        '<span class="flex-1"></span>' +
        (isOwner ? '<button type="button" data-org-delete class="fm-btn fm-btn-sm fm-btn-danger"><i data-lucide="trash-2"></i> Delete org</button>' : "") +
      "</div>" +
      '<div class="mt-4 flex items-center gap-3">' +
        '<h3 class="text-xl font-semibold text-foreground">' + title + "</h3>" +
        (profile.viewerRole ? orgRoleBadge(profile.viewerRole) : "") +
      "</div>" +
      (profile.description ? '<p class="mt-1 text-sm text-muted-foreground">' + escapeHtml(profile.description) + "</p>" : "") +
      worldSettings +
      '<p data-org-status class="mt-2 min-h-4 text-xs text-muted-foreground"></p>' +
      memberAndTeamSettings +
      '<section class="mt-6"><h4 class="text-sm font-semibold text-foreground">Linked repos</h4>' +
        '<div class="mt-2 grid gap-2">' + repoRows + "</div>" + repoAdd + "</section>" +
      digestControls +
      '<section class="mt-6" data-org-pull-rules-section>' +
        '<h4 class="text-sm font-semibold text-foreground">Pull request rules</h4>' +
        '<p class="mt-1 max-w-2xl text-xs leading-5 text-muted-foreground">Requirements every pull request on this organization\'s linked repositories must meet: tests, documentation, size, commit format, secrets, protected paths, or a required AI model. Each repository can add its own rules in <span class="font-mono">.forkmesh/pull-rules.json</span>. A failing rule is flagged on the pull request with how to fix it; a blocking one also locks the merge.</p>' +
        '<div data-org-pull-rules class="mt-3 text-xs text-muted-foreground">Loading rules…</div>' +
      "</section>" +
      botTokenControls +
      (canManage
        ? '<section class="mt-6" data-org-actions-secrets-section>' +
            '<h4 class="text-sm font-semibold text-foreground">Actions secrets</h4>' +
            '<div data-actions-secrets data-actions-secrets-scope="org" data-actions-secrets-base="' +
              escapeHtml("/api/org/" + encodeURIComponent(name) + "/actions/secrets") +
              '" class="mt-2 text-xs text-muted-foreground"></div>' +
          "</section>"
        : "") +
      (canManage ? orgRemoteMcpSetup(name) : "");

    window.lucide?.createIcons();
    mountActionsSecrets(root.querySelector("[data-org-actions-secrets-section] [data-actions-secrets]"));
    wireOrgDetail(root, name, members, teams);
  }

  function wireOrgDetail(root, name, members, teams) {
    const reload = () => showOrgDetail(name);
    const guard = async (fn) => {
      try {
        await fn();
        reload();
      } catch (error) {
        orgStatus(root, error.message, false);
      }
    };

    root.querySelector("[data-org-back]")?.addEventListener("click", showOrgsList);

    root.querySelector("[data-org-delete]")?.addEventListener("click", async () => {
      if (!await confirmAccountAction({
        title: "Delete " + name + "?",
        body: "This dissolves the org, its members, teams, and repo links. Linked repos are not deleted.",
        confirmLabel: "Delete organization",
      })) return;
      guard(() => orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name), {}).then(showOrgsList));
    });

    root.querySelector("[data-org-world-settings]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const worldAccess = {
        lobby: root.querySelector("[data-org-world-lobby]")?.value || "public",
        floors: root.querySelector("[data-org-world-floors]")?.value || "restricted",
        offices: root.querySelector("[data-org-world-offices]")?.value || "restricted",
      };
      guard(() => orgApiRequest("PATCH", "/api/orgs/" + encodeURIComponent(name), {
        displayName: (root.querySelector("[data-org-world-display]")?.value || "").trim(),
        description: (root.querySelector("[data-org-world-description]")?.value || "").trim(),
        logoUrl: (root.querySelector("[data-org-world-logo]")?.value || "").trim(),
        worldAccess,
      }));
    });

    root.querySelector("[data-org-member-add]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const member = (root.querySelector("[data-member-name]")?.value || "").trim().toLowerCase();
      const role = root.querySelector("[data-member-role]")?.value || "member";
      if (!member) return;
      guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/members", { member, role }));
    });
    wireOrgMemberAutocomplete(root, members);
    root.querySelectorAll("[data-org-member-role]").forEach((select) => {
      select.addEventListener("change", () => {
        const member = select.dataset.orgMemberRole;
        guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/members", { member, role: select.value }));
      });
    });
    root.querySelectorAll("[data-org-member-remove]").forEach((button) => {
      button.addEventListener("click", async () => {
        const member = button.dataset.orgMemberRemove;
        if (!await confirmAccountAction({ title: "Remove " + member + " from " + name + "?", confirmLabel: "Remove" })) return;
        guard(() => orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name) + "/members", { member }));
      });
    });
    root.querySelectorAll("[data-org-member-floor-group]").forEach((button) => {
      button.addEventListener("click", () => {
        const member = String(button.dataset.orgMember || "").trim().toLowerCase();
        const groupId = String(button.dataset.orgMemberFloorGroup || "");
        const group = ORG_OFFICE_FLOOR_GROUPS.find((item) => item.id === groupId);
        const memberRecord = members.find(
          (item) => String(item?.name || "").trim().toLowerCase() === member);
        if (!member || !group || !memberRecord) return;
        const memberships = new Set(
          (Array.isArray(memberRecord.teams) ? memberRecord.teams : [])
            .map((team) => String(team || "").trim().toLowerCase())
            .filter(Boolean),
        );
        const matchingTeams = group.aliases.filter((team) => memberships.has(team));
        const knownTeams = new Set(
          (Array.isArray(teams) ? teams : [])
            .map((team) => String(team?.team || "").trim().toLowerCase())
            .filter(Boolean),
        );
        button.disabled = true;
        button.setAttribute("aria-busy", "true");
        guard(async () => {
          if (matchingTeams.length) {
            for (const team of matchingTeams) {
              await orgApiRequest(
                "DELETE",
                "/api/orgs/" + encodeURIComponent(name) + "/teams/" +
                  encodeURIComponent(team) + "/members",
                { member },
              );
            }
            return;
          }
          const team = group.id;
          if (!knownTeams.has(team)) {
            await orgApiRequest(
              "POST",
              "/api/orgs/" + encodeURIComponent(name) + "/teams",
              { team, permission: "read" },
            );
          }
          await orgApiRequest(
            "POST",
            "/api/orgs/" + encodeURIComponent(name) + "/teams/" +
              encodeURIComponent(team) + "/members",
            { member },
          );
        });
      });
    });

    root.querySelector("[data-org-team-add]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const team = (root.querySelector("[data-team-name]")?.value || "").trim().toLowerCase();
      const permission = root.querySelector("[data-team-perm]")?.value || "read";
      if (!team) return;
      guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/teams", { team, permission }));
    });
    root.querySelectorAll("[data-org-team-perm]").forEach((select) => {
      select.addEventListener("change", () => {
        const team = select.dataset.orgTeamPerm;
        guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/teams", { team, permission: select.value }));
      });
    });
    root.querySelectorAll("[data-org-team-delete]").forEach((button) => {
      button.addEventListener("click", async () => {
        const team = button.dataset.orgTeamDelete;
        if (!await confirmAccountAction({ title: "Delete team " + team + "?", confirmLabel: "Delete team" })) return;
        guard(() => orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name) + "/teams", { team }));
      });
    });
    root.querySelectorAll("[data-org-team-open]").forEach((button) => {
      button.addEventListener("click", () => showOrgTeamDetail(name, button.dataset.orgTeamOpen, members));
    });

    root.querySelector("[data-org-repo-add]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const repo = (root.querySelector("[data-repo-name]")?.value || "").trim().toLowerCase();
      const node = (root.querySelector("[data-repo-node]")?.value || "").trim().toLowerCase();
      if (!repo) return;
      const body = { repo };
      if (node) body.node = node;
      guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/repos", body));
    });
    root.querySelectorAll("[data-org-repo-unlink]").forEach((button) => {
      button.addEventListener("click", async () => {
        const repo = button.dataset.orgRepoUnlink;
        if (!await confirmAccountAction({ title: "Unlink " + name + "/" + repo + "?", confirmLabel: "Unlink" })) return;
        guard(() => orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name) + "/repos", { repo }));
      });
    });
    root.querySelector("[data-org-fediverse-enabled]")?.addEventListener(
      "change", (event) => {
        guard(() => orgApiRequest(
          "POST", "/api/orgs/" + encodeURIComponent(name) + "/fediverse",
          { enabled: Boolean(event.target.checked) }));
      });

    const botProvider = root.querySelector("[data-org-bot-provider]");
    const botLabel = root.querySelector("[data-org-bot-label]");
    botProvider?.addEventListener("change", () => {
      if (!botLabel) return;
      botLabel.value = botProvider.value === "claude-code"
        ? "Claude bot"
        : "Codex bot";
    });
    root.querySelector("[data-org-bot-create]")?.addEventListener(
      "submit", async (event) => {
        event.preventDefault();
        const secretPanel = root.querySelector("[data-org-bot-secret]");
        const scopes = [...root.querySelectorAll("[data-org-bot-scope]:checked")]
          .map((input) => input.value);
        try {
          const created = await orgApiRequest(
            "POST",
            "/api/orgs/" + encodeURIComponent(name) + "/bot-tokens",
            {
              provider: botProvider?.value || "codex",
              label: (botLabel?.value || "").trim(),
              expiresDays: Number(
                root.querySelector("[data-org-bot-expiry]")?.value || 90),
              scopes,
            },
          );
          if (!secretPanel) return;
          secretPanel.hidden = false;
          secretPanel.replaceChildren();
          const heading = document.createElement("strong");
          heading.className = "block text-sm text-foreground";
          heading.textContent = "Copy this token now — it cannot be recovered";
          const token = document.createElement("code");
          token.className =
            "mt-2 block select-all break-all rounded border border-border bg-background p-2 text-xs text-foreground";
          token.textContent = created.token || "";
          const instructions = document.createElement("p");
          instructions.className = "mt-2 text-xs leading-5 text-muted-foreground";
          instructions.textContent =
            "On the attended local computer, store it in FORKMESH_BOT_TOKEN, then POST /api/bot/session with a deviceName to bind and verify the connection. Revoking this row stops the credential immediately.";
          const copy = document.createElement("button");
          copy.type = "button";
          copy.className = ORG_BTN_SECONDARY + " fm-btn-sm mt-2";
          copy.textContent = "Copy token";
          copy.addEventListener("click", async () => {
            const copied = await copyTextToClipboard(created.token || "");
            copy.textContent = copied ? "Copied" : "Copy failed";
            window.setTimeout(() => { copy.textContent = "Copy token"; }, 1500);
          });
          secretPanel.append(heading, token, instructions, copy);
          orgStatus(root, "Bot token created. Copy it before leaving this page.", true);
        } catch (error) {
          orgStatus(root, error.message, false);
        }
      },
    );
    root.querySelectorAll("[data-org-bot-revoke]").forEach((button) => {
      button.addEventListener("click", async () => {
        if (!await confirmAccountAction({
          title: "Revoke this bot token?",
          body: "Its local connection and API access will stop immediately.",
          confirmLabel: "Revoke token",
        })) return;
        guard(() => orgApiRequest(
          "DELETE",
          "/api/orgs/" + encodeURIComponent(name) + "/bot-tokens",
          { tokenId: button.dataset.orgBotRevoke },
        ));
      });
    });
    root.querySelectorAll("[data-org-remote-mcp-generate]").forEach((button) => {
      button.addEventListener("click", async () => {
        const mode = button.dataset.orgRemoteMcpGenerate || "pr";
        const output = [...root.querySelectorAll("[data-org-remote-mcp-output]")]
          .find((element) => element.dataset.orgRemoteMcpOutput === mode);
        const status = root.querySelector("[data-org-remote-mcp-status]");
        button.disabled = true;
        if (status) {
          status.textContent = "Creating a revocable task-only credential…";
          status.className = "mt-3 min-h-4 text-xs text-muted-foreground";
        }
        try {
          const created = await orgApiRequest(
            "POST",
            "/api/orgs/" + encodeURIComponent(name) + "/bot-tokens",
            {
              provider: "codex",
              label: "Remote MCP " + (mode === "deploy" ? "deploy" : "pull request"),
              expiresDays: 90,
              scopes: [
                "organization.tasks.read",
                "organization.tasks.write",
              ],
            },
          );
          const prompt = orgRemoteMcpPrompt(name, mode, created.token || "");
          const textarea = output?.querySelector("textarea");
          if (textarea) textarea.value = prompt;
          if (output) output.hidden = false;
          await navigator.clipboard.writeText(prompt).catch(() => {});
          if (status) {
            status.textContent = "Complete remote setup generated and copied. Paste it into the agent once; no fields need editing.";
            status.className = "mt-3 min-h-4 text-xs text-muted-foreground";
          }
        } catch (error) {
          if (status) {
            status.textContent = error.message;
            status.className = "mt-3 min-h-4 text-xs text-destructive";
          }
        } finally {
          button.disabled = false;
        }
      });
    });
    root.querySelectorAll("[data-org-remote-mcp-copy]").forEach((button) => {
      button.addEventListener("click", async () => {
        const mode = button.dataset.orgRemoteMcpCopy || "pr";
        const output = [...root.querySelectorAll("[data-org-remote-mcp-output]")]
          .find((element) => element.dataset.orgRemoteMcpOutput === mode);
        const textarea = output?.querySelector("textarea");
        if (!textarea) return;
        try {
          await navigator.clipboard.writeText(textarea.value || "");
          const label = button.innerHTML;
          button.textContent = "Copied";
          window.setTimeout(() => { button.innerHTML = label; }, 1500);
        } catch (_) {
          textarea.focus();
          textarea.select();
        }
      });
    });
  }

  function wireOrgMemberAutocomplete(root, members) {
    const input = root.querySelector("[data-member-name]");
    const suggestions = root.querySelector("[data-org-member-suggestions]");
    const status = root.querySelector("[data-org-member-search-status]");
    if (!input || !suggestions || !status) return;
    const existing = new Set(
      (Array.isArray(members) ? members : [])
        .map((member) => String(member?.name || "").trim().toLowerCase())
        .filter(Boolean),
    );
    let timer = 0;
    let generation = 0;
    input.addEventListener("input", () => {
      window.clearTimeout(timer);
      const query = String(input.value || "").trim().toLowerCase();
      const currentGeneration = ++generation;
      suggestions.replaceChildren();
      if (!query) {
        status.textContent = `Type a name to search active ${platformName()} accounts.`;
        return;
      }
      status.textContent = "Searching active accounts…";
      timer = window.setTimeout(async () => {
        try {
          const data = await orgApiRequest(
            "GET",
            "/api/chat/direct-messages/users?query=" +
              encodeURIComponent(query),
          );
          if (currentGeneration !== generation) return;
          const names = (Array.isArray(data?.users) ? data.users : [])
            .map((user) => String(user?.name || "").trim().toLowerCase())
            .filter((account, index, all) =>
              account &&
              !existing.has(account) &&
              all.indexOf(account) === index
            )
            .slice(0, 12);
          const fragment = document.createDocumentFragment();
          names.forEach((account) => {
            const option = document.createElement("option");
            option.value = account;
            fragment.appendChild(option);
          });
          suggestions.replaceChildren(fragment);
          status.textContent = names.length
            ? `${names.length} matching active account${names.length === 1 ? "" : "s"}.`
            : "No matching account outside this organization.";
        } catch (_) {
          if (currentGeneration !== generation) return;
          status.textContent =
            "Account search is unavailable; an exact account name still works.";
        }
      }, 180);
    });
  }

  async function showOrgTeamDetail(name, team, orgMembers) {
    const root = orgsRoot();
    if (!root || !team) return;
    root.innerHTML = '<p class="text-sm text-muted-foreground">' + loadingHtml("Loading team…") + "</p>";
    let data;
    try {
      data = await orgApiRequest("GET", "/api/orgs/" + encodeURIComponent(name) + "/teams/" + encodeURIComponent(team) + "/members");
    } catch (error) {
      root.innerHTML =
        '<button type="button" data-org-team-back class="mb-4 inline-flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground">' +
        '<i data-lucide="arrow-left" class="h-4 w-4"></i> Back</button>' +
        '<p class="text-sm text-destructive">' + escapeHtml(error.message) + "</p>";
      window.lucide?.createIcons();
      root.querySelector("[data-org-team-back]")?.addEventListener("click", () => showOrgDetail(name));
      return;
    }
    const teamMembers = data.members || [];
    const onTeam = new Set(teamMembers.map((member) => member.name));
    const candidates = (orgMembers || []).filter((member) => !onTeam.has(member.name));
    const rows = teamMembers.map((member) => {
      const memberName = escapeHtml(member.name || "");
      return '<div class="flex items-center gap-2 rounded-md border border-border bg-card px-3 py-2">' +
        '<span class="min-w-0 flex-1 truncate text-sm font-medium text-foreground">' + userNameHtml(member.name || "") +
          (member.external
            ? ' <small class="ml-1 rounded-full border border-border px-1.5 py-0.5 text-[10px] font-semibold text-muted-foreground">outside collaborator</small>'
            : "") + "</span>" +
        '<button type="button" data-org-team-member-remove="' + memberName + '" title="Remove from team" ' +
          'class="fm-btn fm-btn-sm fm-btn-icon fm-btn-danger">' +
          '<i data-lucide="x" class="h-4 w-4"></i></button></div>';
    }).join("") || '<p class="text-sm text-muted-foreground">No members on this team yet.</p>';
    const addForm =
      '<form data-org-team-member-add class="mt-3 grid gap-2 rounded-md border border-border bg-card p-3">' +
        '<div class="flex flex-wrap items-center gap-2">' +
          '<input data-team-member-name list="org-team-member-candidates" autocomplete="off" placeholder="' + escapeHtml(platformName()) + ' account name" class="' + ORG_INPUT_CLASS + ' flex-1 min-w-[10rem]" />' +
          '<datalist id="org-team-member-candidates">' +
            candidates.map((member) => '<option value="' + escapeHtml(member.name) + '"></option>').join("") +
          "</datalist>" +
          '<button type="submit" class="' + ORG_BTN_SECONDARY + '">Add to team</button>' +
        "</div>" +
        '<label class="flex items-start gap-2 text-xs leading-5 text-muted-foreground">' +
          '<input type="checkbox" data-team-member-external class="mt-1 h-4 w-4" />' +
          '<span><strong class="text-foreground">Outside collaborator</strong> — team-limited access only. This does not make the account an organization member and does not grant Office entry or repository permissions.</span>' +
        "</label>" +
      "</form>";
    root.innerHTML =
      '<button type="button" data-org-team-back class="inline-flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground">' +
        '<i data-lucide="arrow-left" class="h-4 w-4"></i> ' + escapeHtml(name) + "</button>" +
      '<div class="mt-4 flex items-center gap-3"><h3 class="text-xl font-semibold text-foreground">' + escapeHtml(team) + "</h3>" +
        '<span class="inline-flex items-center rounded-full border border-border bg-secondary px-2 py-0.5 text-xs font-semibold text-muted-foreground">' + escapeHtml(data.permission || "") + " permission</span></div>" +
      '<p data-org-status class="mt-2 min-h-4 text-xs text-muted-foreground"></p>' +
      '<div class="mt-4 grid gap-2">' + rows + "</div>" + addForm;
    window.lucide?.createIcons();

    const guard = async (fn) => {
      try {
        await fn();
        showOrgTeamDetail(name, team, orgMembers);
      } catch (error) {
        orgStatus(root, error.message, false);
      }
    };
    root.querySelector("[data-org-team-back]")?.addEventListener("click", () => showOrgDetail(name));
    root.querySelector("[data-org-team-member-add]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      const member = (root.querySelector("[data-team-member-name]")?.value || "").trim().toLowerCase();
      const external = root.querySelector("[data-team-member-external]")?.checked === true;
      if (!member) return;
      guard(() => orgApiRequest("POST", "/api/orgs/" + encodeURIComponent(name) + "/teams/" + encodeURIComponent(team) + "/members", { member, external }));
    });
    root.querySelectorAll("[data-org-team-member-remove]").forEach((button) => {
      button.addEventListener("click", () => {
        const member = button.dataset.orgTeamMemberRemove;
        guard(() => orgApiRequest("DELETE", "/api/orgs/" + encodeURIComponent(name) + "/teams/" + encodeURIComponent(team) + "/members", { member }));
      });
    });
  }

  function setMobileSidebarOpen(open) {
    document.body.classList.toggle("dashboard-sidebar-open", open);
    $$("[data-mobile-menu-toggle]").forEach((button) => {
      button.setAttribute("aria-expanded", open ? "true" : "false");
      button.setAttribute("aria-label", open ? "Close dashboard menu" : "Open dashboard menu");
    });
  }

  function closeMobileDrawers() {
    setMobileSidebarOpen(false);
  }

  function currentSection() {
    // The legacy section name is baked into the page document at build time
    // (dashboard_shell.PAGES[page]["section"] -> data-dashboard-section).
    return $("[data-dashboard-root]")?.dataset?.dashboardSection
      || $("[data-view].active")?.dataset?.view
      || "home";
  }

  function repoDocumentTitle(label, title) {
    const text = String(title || "");
    const at = text.lastIndexOf(" - ");
    return `${label}${at >= 0 ? text.slice(at) : ""}`;
  }

  function restoreDocumentTitle() {
    if (state.documentTitleBase === undefined) return;
    document.title = state.documentTitleBase;
    state.documentTitleBase = undefined;
  }

  function renderHeaderContext(section = currentSection()) {
    const headerContext = $("[data-dashboard-header-context]");
    if (!headerContext) return;
    const repoMeta = $("[data-repo-header-meta]");
    const repoVisibility = $("[data-repo-header-visibility]");
    const repoTerms = $("[data-repo-header-terms]");
    const repoAvailability = $("[data-repo-availability-status]");
    const repo = section === "explore" ? state.selectedRepo : null;
    if (repo) {
      const live = repoIsLive(repo);
      const viaMirror = repoServedByMirror(repo);
      // Owner and repository are separate links: the owner segment opens the
      // user's or organization's /@name page, the name opens the repository.
      // repoDisplayKey(repo) stays the single source of both labels.
      const [owner, ...nameParts] = repoDisplayKey(repo).split("/");
      const repoHref = repoLinkUrl(repo);
      const shortAddress = repoHref.split("/").filter(Boolean).length === 1;
      const repoLink = `<a href="${escapeHtml(repoHref)}" class="hover:underline">${escapeHtml(nameParts.join("/"))}</a>`;
      headerContext.innerHTML = owner && !shortAddress
        ? `<a href="${escapeHtml(publicProfileUrl(owner))}" data-repo-header-owner class="hover:underline">${escapeHtml(owner)}</a><span class="px-1 font-normal text-muted-foreground" aria-hidden="true">/</span>${repoLink}`
        : repoLink;
      if (shortAddress) {
        if (state.documentTitleBase === undefined) state.documentTitleBase = document.title;
        document.title = repoDocumentTitle(nameParts.join("/"), document.title);
      } else {
        restoreDocumentTitle();
      }
      repoMeta?.classList.add("sm:flex");
      if (repoVisibility) {
        repoVisibility.textContent = repo.isPrivate ? "private" : "public";
      }
      if (repoTerms) repoTerms.innerHTML = repositoryTermsBadge(repo, true);
      if (repoAvailability && dashboardOnInstance()) {
        repoAvailability.textContent = "";
        repoAvailability.classList.add("hidden");
      } else if (repoAvailability) {
        repoAvailability.classList.remove("hidden");
        const servedBy = String(state.repoServedBy?.name || "").trim();
        repoAvailability.textContent = servedBy
          ? `served by ${servedBy}`
          : viaMirror
            ? "served by mirror"
          : live
            ? "mirror online"
            : "mirror offline";
        repoAvailability.classList.toggle("text-success", live);
        repoAvailability.classList.toggle("text-muted-foreground", !live);
      }
      return;
    }
    restoreDocumentTitle();
    repoMeta?.classList.remove("sm:flex");
    if (repoVisibility) repoVisibility.textContent = "";
    if (repoTerms) repoTerms.innerHTML = "";
    if (repoAvailability) repoAvailability.textContent = "";
    if (section === "profile-overview" || section === "profile-repositories") {
      const renderedName = ($("[data-profile-page-node-name]")?.textContent || "").trim();
      headerContext.textContent = (state.session?.nodeName ? userDisplayName(state.session.nodeName) : "") || state.session?.email || renderedName || "Profile";
      return;
    }
    if (section === "profile") {
      headerContext.textContent = "Settings";
      return;
    }
    if (section === "repos") {
      headerContext.textContent = "Repositories";
      return;
    }
    if (section === "network") {
      headerContext.textContent = "Network";
      return;
    }
    if (section === "chat") {
      headerContext.textContent = "Chat";
      return;
    }
    if (section === "explore" && state.selectedRepo) {
      headerContext.textContent = repoKey(state.selectedRepo);
      return;
    }
    // No label for the dashboard itself: the logo already says where you are,
    // so the header keeps a page name only where it names something narrower.
    headerContext.textContent = "";
  }

  // ---- Public-profile mode (/@name) ---------------------------------------
  // The worker serves the SAME prebuilt profile documents at /@name; the
  // profile-page machinery renders whatever profileSubject() returns, so
  // public mode is: fetch the named account's public payload, park it in
  // state.publicProfile, and strip the owner-only chrome.

  function publicProfileNameFromPath() {
    const match = /^\/@([a-z][a-z0-9-]{0,62})(?:\/repositories)?\/?$/
      .exec(location.pathname.toLowerCase());
    return match ? match[1] : "";
  }

  function profileSubject() {
    return state.publicProfile || state.session;
  }

  // On /@name pages the profile markup belongs to the fetched PUBLIC profile;
  // the shared-chrome boot path still calls the profile renderers with the
  // session, which must not overwrite (or briefly flash) the wrong identity.
  function profileMarkupOwnedByPublicProfile(session) {
    const publicName = publicProfileNameFromPath();
    return Boolean(publicName) && session !== state.publicProfile &&
      publicName !== String(session?.nodeName || "").toLowerCase();
  }

  async function loadPublicProfile(name) {
    document.body.classList.add("public-profile-mode");
    let body = null;
    try {
      const viewer = state.session?.nodeName
        ? "?viewer=" + encodeURIComponent(state.session.nodeName) : "";
      body = await fetchJson("/api/accounts/" + encodeURIComponent(name) + viewer);
    } catch (_) {}
    if (!body || body.exists === false) {
      const organization = await fetchJson("/api/orgs/" + encodeURIComponent(name)).catch(() => null);
      if (organization?.ok) {
        loadPublicOrganizationProfile(name, organization);
        return;
      }
      $$("[data-profile-page-node-name]").forEach((el) => {
        el.textContent = "@" + name + " was not found";
      });
      return;
    }
    const profile = sessionFromAccountPayload(body, { nodeName: name });
    profile.nodeName = profile.nodeName || name;
    // Never show a mailbox on someone else's page — the handle is the
    // public identity here.
    profile.email = "@" + userDisplayName(profile.nodeName);
    profile.isFollowing = Boolean(body?.social?.isFollowing);
    state.publicProfile = profile;
    renderProfilePage(profile);
    applyPublicProfileChrome(profile);
    // Repository lists still use the catalog, while the native contribution
    // card reads its own account ledger after the public subject is installed.
    renderProfileContributionGraph();
    // Both the list AND the tab count are filtered by the profile subject, so
    // both have to be recomputed here: when the catalog lands before this
    // fetch, it counted against an empty subject and pinned the tab at 0.
    renderProfileRepositories();
    renderProfileRepositoryCount();
  }

  function loadPublicOrganizationProfile(name, organization) {
    const orgName = String(organization.org || name).toLowerCase();
    const profile = {
      kind: "organization",
      nodeName: orgName,
      email: "@" + orgName,
      status: "active",
      createdAt: Number(organization.createdAt) || 0,
      profileBio: organization.description || organization.displayName || "",
      displayName: String(organization.displayName || ""),
      logoUrl: String(organization.logoUrl || ""),
      memberCount: Number(organization.members) || 0,
      teamCount: Number(organization.teams) || 0,
    };
    state.publicProfile = profile;
    document.body.classList.add("organization-profile-mode");
    renderProfilePage(profile);
    applyPublicProfileChrome(profile);
    $$("[data-profile-contribution-card], [data-profile-overview-layout], [data-profile-sidebar-slot]")
      .forEach((el) => el.classList.add("hidden"));
    renderOrganizationProfileHeader(profile);
    renderProfileRepositories();
    renderProfileRepositoryCount();
  }

  function organizationSafeLogoUrl(value) {
    const url = String(value || "").trim();
    return /^(?:https:\/\/|\/(?!\/))[^\s"'<>]+$/i.test(url) ? url : "";
  }

  function renderOrganizationProfileHeader(profile) {
    const title = profile.displayName || profile.nodeName;
    $$("[data-org-profile-header]").forEach((header) => {
      header.hidden = false;
      const avatar = header.querySelector("[data-org-profile-avatar]");
      if (avatar) {
        const logo = organizationSafeLogoUrl(profile.logoUrl);
        avatar.innerHTML = logo
          ? `<img src="${escapeHtml(logo)}" alt="${escapeHtml(title)} logo" class="h-full w-full object-cover" />`
          : escapeHtml((title[0] || "F").toUpperCase());
      }
      const nameEl = header.querySelector("[data-org-profile-name]");
      if (nameEl) nameEl.textContent = title;
      const description = header.querySelector("[data-org-profile-description]");
      if (description) {
        description.textContent = profile.profileBio && profile.profileBio !== title ? profile.profileBio : "";
        description.hidden = !description.textContent;
      }
      const meta = header.querySelector("[data-org-profile-meta]");
      if (meta) {
        const url = publicProfileUrl(profile.nodeName);
        const items = [
          `<li class="inline-flex items-center gap-1.5"><i data-lucide="users" class="h-4 w-4"></i><span class="font-semibold text-foreground">${formatCount(profile.memberCount)}</span> ${profile.memberCount === 1 ? "member" : "members"}</li>`,
          `<li class="inline-flex items-center gap-1.5"><i data-lucide="link" class="h-4 w-4"></i><a href="${escapeHtml(url)}" class="text-foreground hover:text-accent hover:underline">${escapeHtml(url.replace(/^https?:\/\//, ""))}</a></li>`,
        ];
        if (profile.createdAt) {
          items.push(`<li class="inline-flex items-center gap-1.5"><i data-lucide="calendar" class="h-4 w-4"></i>Since ${escapeHtml(new Date(profile.createdAt).toLocaleDateString([], { month: "long", year: "numeric" }))}</li>`);
        }
        meta.innerHTML = items.join("");
      }
    });
    $$("[data-org-profile-people-count]").forEach((el) => { el.textContent = formatCount(profile.memberCount); });
    $$("[data-org-profile-teams-count]").forEach((el) => { el.textContent = formatCount(profile.teamCount); });
    $$("[data-org-profile-people-label]").forEach((el) => {
      el.textContent = profile.memberCount === 1 ? "member" : "members";
    });
    $$("[data-org-profile-teams-label]").forEach((el) => {
      el.textContent = profile.teamCount === 1 ? "team" : "teams";
    });
    $$("[data-org-profile-overview]").forEach((el) => { el.hidden = false; });
    window.lucide?.createIcons();
  }

  function organizationRepositoryLanguages(group) {
    const repos = [sourceOfTruth(group), ...(group.members || [])];
    const found = repos.map((repo) => repo?.logoMetadata?.languages)
      .find((languages) => languages && typeof languages === "object" && Object.keys(languages).length);
    return Object.entries(found || {})
      .filter(([language, weight]) => language !== "C/C++" && Number(weight) > 0)
      .map(([language, weight]) => [language, Number(weight)])
      .sort((a, b) => b[1] - a[1]);
  }

  function organizationPopularGroups(groups) {
    const score = (group) => (group.members || []).length * 1e9 +
      (Number(sourceOfTruth(group).commitCount) || 0);
    return [...groups].sort((a, b) => score(b) - score(a)).slice(0, 6);
  }

  const organizationPopularStarCounts = new Map();

  function organizationPopularStarCount(repo) {
    const key = repoKey(repo).toLowerCase();
    if (!organizationPopularStarCounts.has(key)) {
      organizationPopularStarCounts.set(key, fetch(`${repoApiBase(repo)}/star`, {
        headers: { accept: "application/json" },
      })
        .then((response) => response.ok ? response.json() : null)
        .then((data) => (data && data.ok !== false ? Number(data.count) || 0 : 0))
        .catch(() => {
          organizationPopularStarCounts.delete(key);
          return 0;
        }));
    }
    return organizationPopularStarCounts.get(key);
  }

  function hydrateOrganizationPopularStars(root) {
    root.querySelectorAll("[data-org-popular-star][data-repo-key]").forEach(async (holder) => {
      const repo = findRepository(holder.dataset.repoKey || "");
      if (!repo) return;
      const count = await organizationPopularStarCount(repo);
      if (!holder.isConnected || count <= 0) return;
      const value = holder.querySelector("[data-org-popular-star-count]");
      if (value) value.textContent = formatCount(count);
      holder.classList.remove("hidden");
    });
  }

  function organizationPopularCard(group) {
    const repo = sourceOfTruth(group);
    const languages = organizationRepositoryLanguages(group);
    const language = languages[0]?.[0] || String(repo.language || repo.primaryLanguage || "").trim();
    const color = REPO_LANGUAGE_COLORS[language] || "#8b949e";
    const description = repo.description ||
      (group.members || []).map((member) => member.description).find(Boolean) || "";
    return `<article data-org-popular-repo class="flex min-w-0 flex-col rounded-md border border-border bg-card p-4">
      <div class="flex min-w-0 items-center gap-2">
        <i data-lucide="book-marked" class="h-4 w-4 shrink-0 text-muted-foreground"></i>
        <a href="${escapeHtml(groupLinkUrl(group))}" class="min-w-0 truncate text-sm font-semibold text-accent hover:underline">${escapeHtml(repo.name || "repository")}</a>
        <span class="inline-flex shrink-0 items-center rounded-full border border-border px-[7px] text-xs font-medium leading-[18px] text-muted-foreground">${repo.isPrivate ? "Private" : "Public"}</span>
      </div>
      <p class="mb-4 mt-2 flex-1 break-words text-xs leading-5 text-muted-foreground">${escapeHtml(description)}</p>
      <p class="flex flex-wrap items-center gap-4 text-xs text-muted-foreground">
        ${language ? `<span class="inline-flex items-center gap-1"><span data-repo-language-dot class="inline-block h-3 w-3 shrink-0 rounded-full border border-black/10" style="background-color:${escapeHtml(color)}"></span><span>${escapeHtml(language)}</span></span>` : ""}
        <span data-org-popular-star data-repo-key="${escapeHtml(repoKey(repo))}" class="hidden inline-flex items-center gap-1"><i data-lucide="star" class="h-4 w-4"></i><span data-org-popular-star-count>0</span></span>
      </p>
    </article>`;
  }

  function renderOrganizationRepositories() {
    const popular = $("[data-org-profile-popular]");
    const languagesEl = $("[data-org-profile-languages]");
    if (profileSubject()?.kind !== "organization" || (!popular && !languagesEl)) return;
    const groups = profileRepositoryGroups();
    if (popular) {
      popular.innerHTML = groups.length
        ? organizationPopularGroups(groups).map(organizationPopularCard).join("")
        : `<p class="text-sm text-muted-foreground">This organization has no public repositories yet.</p>`;
      hydrateOrganizationPopularStars(popular);
    }
    $$("[data-org-profile-all-repos]").forEach((link) => {
      link.href = publicProfileUrl(profileSubject().nodeName, "/repositories");
    });
    if (languagesEl) {
      const totals = new Map();
      groups.forEach((group) => {
        organizationRepositoryLanguages(group).forEach(([language, weight]) => {
          totals.set(language, (totals.get(language) || 0) + weight);
        });
      });
      const ranked = [...totals.entries()].sort((a, b) => b[1] - a[1]).slice(0, 5);
      languagesEl.innerHTML = ranked.length
        ? ranked.map(([language]) => `<span class="inline-flex items-center gap-1.5"><span class="h-2 w-2 rounded-full" style="background-color:${REPO_LANGUAGE_COLORS[language] || "#8a8a8a"}"></span>${escapeHtml(language)}</span>`).join("")
        : `<span>No language data yet.</span>`;
    }
    window.lucide?.createIcons();
  }

  function applyPublicProfileChrome(profile) {
    const name = profile.nodeName;
    // Tabs point at the public URLs, not the session dashboard pages.
    $$("[data-profile-tabs] a[href='/dashboard/profile']").forEach((a) => {
      a.href = publicProfileUrl(name);
    });
    $$("[data-profile-tabs] a[href='/dashboard/profile/repositories']").forEach((a) => {
      a.href = publicProfileUrl(name, "/repositories");
    });
    if (profile.kind === "organization") {
      $$("[data-profile-tabs] a[href='/dashboard/network']").forEach((a) => a.remove());
    }
    // Owner-only affordances become a Follow button (or disappear).
    $$("[data-profile-about-edit]").forEach((el) => el.classList.add("hidden"));
    $$("[data-profile-about-owner]").forEach((el) => { el.textContent = userDisplayName(name); });
    $$("[data-profile-sidebar-slot] a[href='/dashboard/settings']").forEach((edit) => {
      const wrap = edit.parentElement;
      edit.remove();
      if (!wrap || !state.session?.nodeName || profile.kind === "organization" ||
          state.session.nodeName.toLowerCase() === name) return;
      const follow = document.createElement("button");
      follow.type = "button";
      follow.setAttribute("data-profile-follow", name);
      follow.className = "flex w-full items-center justify-center rounded-md " +
        "border border-border bg-secondary px-3 py-1.5 text-sm font-semibold " +
        "text-foreground hover:bg-background";
      follow.textContent = profile.isFollowing ? "Following" : "Follow";
      follow.addEventListener("click", async () => {
        const following = follow.textContent === "Following";
        follow.disabled = true;
        try {
          const response = await fetch(
            "/api/accounts/" + encodeURIComponent(name) + "/follow", {
              method: following ? "DELETE" : "POST",
              headers: {
                "Content-Type": "application/json",
                Authorization: "Bearer " + (state.session?.sessionToken || ""),
              },
              body: JSON.stringify({
                sessionToken: state.session?.sessionToken || "",
              }),
            });
          if (response.ok) {
            follow.textContent = following ? "Follow" : "Following";
            const count = $("[data-profile-followers-count]");
            if (count) {
              const current = parseInt(count.textContent, 10) || 0;
              count.textContent = String(Math.max(0, current + (following ? -1 : 1)));
            }
          }
        } catch (_) {}
        follow.disabled = false;
      });
      wrap.append(follow);
      const official = new Set([
        "app.forkmesh.com", "forkmesh.com", "world.forkmesh.com",
      ]).has(location.hostname.toLowerCase());
      const message = document.createElement("a");
      message.setAttribute("data-profile-message", name);
      message.href = (official ? "https://app.forkmesh.com" : "") +
        "/chat?dm=" + encodeURIComponent(name);
      message.className = follow.className + " mt-2";
      message.textContent = "Message";
      message.title = "Open a private conversation with @" + name;
      wrap.append(message);
    });
  }

  function nodeNeedsReconnect(session) {
    // A guest has nothing to reconnect; a node-kind session IS the node.
    const signedIn = Boolean(session && (session.nodeName || session.email));
    if (!signedIn || session.kind === "node") return false;
    if (dashboardOnInstance()) return offlineLinkedNodes(session).length > 0;
    // Only flag when the account affirmatively has zero linked nodes. An absent
    // list means "unknown" (older payload), not "none" — stay quiet then.
    return Array.isArray(session.nodes) && session.nodes.length === 0;
  }

  function linkedNodeNames(session) {
    const nodes = Array.isArray(session?.nodes) ? session.nodes : [];
    return [...new Set(nodes.map((node) => String(node || "").trim().toLowerCase()).filter(Boolean))];
  }

  function offlineLinkedNodes(session) {
    const presence = state.linkedNodePresence || {};
    return linkedNodeNames(session).filter((node) => presence[node] === false);
  }

  let linkedNodePresenceKey = "";

  async function refreshLinkedNodePresence(session) {
    if (!dashboardOnInstance()) return;
    const nodes = linkedNodeNames(session).slice(0, 8);
    const key = nodes.join(",");
    if (key === linkedNodePresenceKey) return;
    linkedNodePresenceKey = key;
    const presence = {};
    await Promise.all(nodes.map(async (node) => {
      const body = await fetchJson("/api/accounts/" + encodeURIComponent(node), { fresh: true }).catch(() => null);
      if (body && body.exists !== false && body.kind === "node") presence[node] = body.online === true;
    }));
    if (linkedNodePresenceKey !== key) return;
    state.linkedNodePresence = presence;
    renderNodeReconnect(state.session);
  }

  function renderNodeReconnect(session) {
    const reconnect = $("[data-node-reconnect]");
    if (!reconnect) return;
    const show = nodeNeedsReconnect(session);
    reconnect.classList.toggle("hidden", !show);
    if (!show || !dashboardOnInstance()) return;
    const offline = offlineLinkedNodes(session);
    const names = offline.join(", ");
    const label = reconnect.querySelector("[data-node-reconnect-label]");
    if (label) label.textContent = offline.length > 1 ? "Desktops offline" : "Desktop offline";
    const message = (offline.length > 1 ? "Your desktop nodes " + names + " are" : "Your desktop node " + names + " is") +
      ` offline. Open the ${desktopBrand()}desktop app on that machine to reconnect it.`;
    reconnect.title = message;
    reconnect.setAttribute("aria-label", message);
  }

  function setEmailVerificationHint(message, kind = "") {
    const hint = $("[data-email-verification-hint]");
    if (!hint) return;
    hint.textContent = message ? " " + message : "";
    // Only utilities already present in the built dashboard/tailwind.css.
    hint.className = kind === "bad" ? "text-destructive"
      : kind === "good" ? "text-muted-foreground"
        : "text-attention";
  }

  // An unverified address blocks node renames and every account email, and the
  // resend control used to be buried in the profile modal behind a password
  // prompt. Surface it on every dashboard page until the address is confirmed.
  function renderEmailVerificationBanner(session) {
    const banner = $("[data-email-verification-banner]");
    if (!banner) return;
    const email = session?.email || "";
    const signedIn = Boolean(session && (session.nodeName || email));
    const show = signedIn && Boolean(email) && !session?.emailVerified;
    banner.classList.toggle("hidden", !show);
    banner.classList.toggle("flex", show);
    if (!show) {
      setEmailVerificationHint("");
      return;
    }
    const message = $("[data-email-verification-message]");
    if (message) {
      message.textContent =
        `${email} is not verified yet. Confirm it to rename your node and receive account email.`;
    }
    window.lucide?.createIcons();
  }

  // Away flag + custom status (user-status.js): the emoji sits on the header
  // avatar's corner, an amber dot marks away, and the account menu's two
  // entries say what a press will do.
  function renderDashboardUserStatus() {
    const userStatus = window.ForkMeshUserStatus;
    const statusItem = $("[data-account-menu-status]");
    const awayItem = $("[data-account-menu-away]");
    if (!userStatus) {
      statusItem?.remove();
      awayItem?.remove();
      return;
    }
    const own = userStatus.mine();
    const label = userStatus.describe({ ...own, away: false });
    const badge = $("[data-dashboard-user-status]");
    if (badge) {
      badge.textContent = userStatus.hasCustom(own) ? (own.emoji || "💬") : "";
      badge.hidden = !badge.textContent;
    }
    const awayDot = $("[data-dashboard-away-dot]");
    if (awayDot) awayDot.hidden = !own.away;
    const settingsButton = $("[data-profile-settings-button]");
    if (settingsButton) {
      settingsButton.title = ["Settings", own.away ? "Away" : "", label]
        .filter(Boolean).join(" · ");
    }
    if (statusItem) {
      statusItem.textContent = label ? "Edit status: " + label : "Set a status…";
    }
    if (awayItem) {
      awayItem.textContent = own.away ? "Set yourself active" : "Set yourself away";
    }
  }

  function watchDashboardUserStatus() {
    const userStatus = window.ForkMeshUserStatus;
    if (userStatus && state.session?.nodeName) {
      userStatus.subscribe(renderDashboardUserStatus);
      userStatus.loadMine();
    }
    renderDashboardUserStatus();
  }

  let pluginNavigationAccount = null;

  async function renderPluginNavigation(session) {
    const account = String(session?.nodeName || "");
    if (account === pluginNavigationAccount) return;
    pluginNavigationAccount = account;
    let installed = [];
    if (account) {
      try {
        installed = (await pluginRequest("/api/plugins/")).plugins || [];
        window.ForkMeshAPI?.rememberPlugins?.(account, installed);
        window.ForkMeshAPI?.rememberStatusPage?.(
          account, installed.includes("status-page"));
      } catch {
        pluginNavigationAccount = null;
      }
      if (pluginNavigationAccount !== account) return;
    }
    const visible = new Set(installed);
    let shown = 0;
    for (const link of $$("[data-plugin-nav]")) {
      const show = visible.has(link.dataset.pluginNav);
      link.classList.toggle("hidden", !show);
      if (show) shown += 1;
    }
    for (const group of $$("[data-plugin-navigation]")) {
      group.classList.toggle("hidden", !shown);
    }
  }

  window.addEventListener("forkmesh:plugins-changed", () => {
    pluginNavigationAccount = null;
    void renderPluginNavigation(state.session);
  });

  function renderProfile(session) {
    const name = session?.nodeName ? userDisplayName(session.nodeName) : (session?.email || "My Profile");
    const nameEl = $("[data-dashboard-profile-name]");
    const statusEl = $("[data-dashboard-profile-status]");
    const avatar = $("[data-dashboard-profile-avatar]");
    const adminButton = $("[data-admin-button]");
    const sidebarName = $("[data-sidebar-user-name]");

    if (nameEl) nameEl.textContent = name;
    if (sidebarName) sidebarName.textContent = name;
    if (statusEl) {
      statusEl.textContent = session?.emailVerified
        ? "Email verified"
        : "Verify email in profile";
    }
    applyAvatar(avatar, session);
    const publicProfileLink = $("[data-account-menu-public-profile]");
    if (publicProfileLink && session?.nodeName) {
      publicProfileLink.href = publicProfileUrl(session.nodeName);
    }
    renderAdminOnlyNavigation(session);
    void renderPluginNavigation(session);
    if (adminButton) {
      let adminUrl = session?.isAdmin ? (session?.adminUrl || "") : "";
      if (adminUrl && session?.nodeName && !/[?&]admin=/.test(adminUrl)) {
        adminUrl += (adminUrl.includes("?") ? "&" : "?") +
          "admin=" + encodeURIComponent(session.nodeName);
      }
      // Only show the button once we actually have somewhere to send it -
      // an admin session without adminUrl (ADMIN_PATH not picked up from the
      // Worker env yet) would otherwise show a button that links to "#".
      adminButton.classList.toggle("hidden", !adminUrl);
      adminButton.href = adminUrl || "#";
    }
    const adminMenuLink = $("[data-account-menu-admin]");
    if (adminMenuLink) {
      const consoleUrl = session?.isAdmin ? (session?.adminUrl || "") : "";
      adminMenuLink.classList.toggle("hidden", !consoleUrl);
      adminMenuLink.href = consoleUrl || "#";
    }
    // Reconnect affordance next to the avatar: a signed-in user account with no
    // node linked has nothing authenticated to drain its issues/chats/etc. to a
    // desktop, so surface the re-link flow (the Settings > Nodes claim/link
    // panel) instead of leaving the data stuck online. Shown only when we can
    // affirmatively tell there are zero linked nodes (session.nodes present and
    // empty) for a user-like account — never for a node session or when the
    // link state is simply unknown, to avoid a false alarm.
    renderNodeReconnect(session);
    void refreshLinkedNodePresence(session);
    renderEmailVerificationBanner(session);
    renderProfileModal(session);
    renderProfilePage(session);
    renderProfileAbout(session);
    // Canonical hydration can add linked-node aliases after the repository
    // catalog has already rendered. Recompute both profile repository views
    // from the newly stored identity so the tab count and list stay together.
    renderProfileRepositories();
    renderProfileRepositoryCount();
    renderHeaderContext();
  }

  function dashboardMockRepositoriesEnabled() {
    const value = new URLSearchParams(location.search).get("mockRepos");
    return ["1", "true", "yes"].includes(String(value || "").trim().toLowerCase());
  }

  function dashboardMockRepositories() {
    const now = Date.now();
    const day = 24 * 60 * 60 * 1000;
    return [
      {
        owner: "demo-alice",
        name: "mesh-workbench",
        description: "A busy collaboration workspace with issues, pulls, discussions, releases, and active mirrors.",
        source: "local-node",
        liveHost: true,
        cloneOnline: true,
        isPrivate: false,
        language: "TypeScript",
        license: "Apache-2.0",
        channel: "stable",
        updatedAt: now - day,
        lastSync: now - 38 * 60 * 1000,
        sizeBytes: 18_400_000,
        issueCount: 14,
        openIssues: 14,
        closedIssueCount: 31,
        pullCount: 5,
        openPulls: 5,
        closedPullCount: 18,
        discussionCount: 9,
        commitCount: 428,
        branchCount: 7,
        releaseCount: 4,
        activityWeeks: [0, 1, 3, 2, 5, 1, 0, 4, 6, 2, 8, 3, 5, 9, 4, 2, 7, 10, 6, 5, 12, 8, 4, 9, 13, 7, 15, 10, 8, 12, 16, 11, 9, 14, 18, 12, 16, 20, 13, 15, 19, 17, 12, 10, 14, 9, 8, 11, 7, 6, 4, 8],
      },
      {
        owner: "demo-bravo",
        name: "mobile-mirror-client",
        description: "Mobile-first mirror node shell with offline queueing and handoff screens.",
        source: "remote-clone",
        liveHost: false,
        cloneOnline: true,
        isPrivate: false,
        language: "Swift",
        license: "MIT License",
        channel: "beta",
        updatedAt: now - 4 * day,
        lastSync: now - 2 * 60 * 60 * 1000,
        cloneUrl: `${publicSiteOrigin()}/demo-bravo/mobile-mirror-client.git`,
        sizeBytes: 9_800_000,
        issueCount: 7,
        openIssues: 7,
        closedIssueCount: 12,
        pullCount: 2,
        openPulls: 2,
        closedPullCount: 6,
        discussionCount: 4,
        commitCount: 156,
        branchCount: 4,
        releaseCount: 2,
        activityWeeks: [0, 0, 1, 0, 2, 1, 3, 0, 2, 4, 1, 3, 5, 2, 4, 3, 6, 2, 5, 4, 7, 3, 4, 6, 8, 5, 7, 6, 4, 8, 9, 5, 7, 6, 10, 8, 5, 7, 9, 4, 6, 8, 5, 7, 6, 4, 5, 3, 6, 4, 2, 5],
      },
      {
        owner: "demo-cora",
        name: "security-review-lab",
        description: "Private security review sandbox showing locked repository states and quieter activity.",
        source: "local-node",
        liveHost: false,
        cloneOnline: false,
        isPrivate: true,
        language: "Rust",
        license: "Proprietary",
        channel: "internal",
        updatedAt: now - 13 * day,
        lastSync: now - 8 * day,
        sizeBytes: 4_200_000,
        issueCount: 3,
        openIssues: 3,
        closedIssueCount: 8,
        pullCount: 1,
        openPulls: 1,
        closedPullCount: 3,
        discussionCount: 2,
        commitCount: 64,
        branchCount: 3,
        releaseCount: 0,
        activityWeeks: [0, 0, 0, 1, 0, 0, 2, 0, 1, 0, 3, 1, 0, 2, 0, 1, 3, 0, 2, 1, 0, 4, 1, 0, 2, 0, 3, 1, 0, 2, 4, 1, 0, 2, 1, 3, 0, 1, 2, 0, 3, 1, 0, 2, 0, 1, 3, 0, 1, 0, 2, 0],
      },
    ];
  }

  function setProfileModalOpen(open) {
    const modal = $("[data-profile-modal]");
    if (!modal) return;
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) {
      renderProfileModal(state.session);
      window.setTimeout(() => $("[data-profile-password]")?.focus(), 0);
    }
  }

  function setProfileHint(text, cls) {
    const hint = $("[data-profile-hint]");
    if (!hint) return;
    hint.textContent = text || "";
    hint.className = "min-h-4 text-xs " + (cls === "bad" ? "text-destructive" : cls === "good" ? "text-success" : "text-muted-foreground");
  }

  function setProfilePageHint(selector, text, cls) {
    const hint = $(selector);
    if (!hint) return;
    hint.textContent = text || "";
    hint.className = "min-h-4 text-xs " + (cls === "bad" ? "text-destructive" : cls === "good" ? "text-success" : "text-muted-foreground");
  }

  function profilePassword(selector) {
    return ($(selector || "[data-profile-password]")?.value || "").trim();
  }

  function profileTxtValue(session = state.session) {
    const name = String(session?.nodeName || "").trim().toLowerCase();
    return name ? `forkmesh-profile=${name}` : "forkmesh-profile=username";
  }

  function profilePublicUrl(session = profileSubject()) {
    const name = String(session?.nodeName || "").trim().toLowerCase();
    return publicProfileUrl(name);
  }

  const PROFILE_TIMEZONE_FALLBACKS = [
    "UTC",
    "Africa/Cairo",
    "Africa/Johannesburg",
    "America/Anchorage",
    "America/Argentina/Buenos_Aires",
    "America/Bogota",
    "America/Chicago",
    "America/Denver",
    "America/Los_Angeles",
    "America/Mexico_City",
    "America/New_York",
    "America/Phoenix",
    "America/Sao_Paulo",
    "America/Toronto",
    "Asia/Bangkok",
    "Asia/Dubai",
    "Asia/Hong_Kong",
    "Asia/Kolkata",
    "Asia/Seoul",
    "Asia/Singapore",
    "Asia/Tokyo",
    "Australia/Melbourne",
    "Australia/Sydney",
    "Europe/Amsterdam",
    "Europe/Berlin",
    "Europe/London",
    "Europe/Madrid",
    "Europe/Paris",
    "Pacific/Auckland",
  ];

  function profileTimezoneOptions() {
    try {
      if (typeof Intl.supportedValuesOf === "function") {
        const zones = Intl.supportedValuesOf("timeZone");
        if (Array.isArray(zones) && zones.length) {
          return ["UTC", ...zones.filter((zone) => zone !== "UTC")];
        }
      }
    } catch {
      // Use the curated fallback below when browser support is unavailable.
    }
    return PROFILE_TIMEZONE_FALLBACKS;
  }

  function profileTimezoneGmtOffset(zone) {
    if (!zone) return "";
    try {
      const parts = new Intl.DateTimeFormat("en", {
        hour: "2-digit",
        minute: "2-digit",
        hour12: false,
        timeZone: zone,
        timeZoneName: "shortOffset",
      }).formatToParts(new Date());
      const value = parts.find((part) => part.type === "timeZoneName")?.value || "";
      if (value === "GMT") return "GMT+00:00";
      const match = value.match(/^GMT([+-])(\d{1,2})(?::?(\d{2}))?$/);
      if (!match) return value.startsWith("GMT") ? value : "";
      return `GMT${match[1]}${match[2].padStart(2, "0")}:${match[3] || "00"}`;
    } catch {
      return "";
    }
  }

  function profileTimezoneGmtLabel(zone) {
    const offset = profileTimezoneGmtOffset(zone);
    return offset ? `${offset} - ${zone}` : zone.replace(/_/g, " ");
  }

  function renderProfileTimezoneOptions(session = state.session) {
    const select = $("[data-profile-page-timezone]");
    if (!select) return;
    if (document.activeElement === select && select.options.length > 1) return;
    const current = String(session?.profileTimezone || select.value || "").trim();
    const zones = profileTimezoneOptions();
    const values = current && !zones.includes(current) ? [current, ...zones] : zones;
    const signature = values.join("\n");
    if (select.dataset.timezoneOptionsKey !== signature) {
      const placeholder = document.createElement("option");
      placeholder.value = "";
      placeholder.textContent = "Use browser time zone";
      select.replaceChildren(placeholder);
      values.forEach((zone) => {
        const option = document.createElement("option");
        option.value = zone;
        option.textContent = profileTimezoneGmtLabel(zone);
        select.append(option);
      });
      select.dataset.timezoneOptionsKey = signature;
    }
    select.value = current;
  }

  function defaultProfileAbout(session = profileSubject()) {
    const name = String(session?.nodeName || session?.email || platformName()).trim() || platformName();
    if (session?.kind === "node") {
      const owner = String(session?.owner || "").trim();
      return `# ${name} is a ForkMesh node\n\nIt mirrors Git repositories and helps serve them to the network.` +
        (owner ? `\n\nOperated by @${owner}.` : "");
    }
    return `# Hi, I'm ${name}\n\nPinned profile content and public activity live here.`;
  }

  function profileAboutMarkdown(session = profileSubject()) {
    const value = String(session?.profileAbout || session?.profileReadme || "");
    return value || defaultProfileAbout(session);
  }

  function flushProfileAboutParagraph(out, paragraph) {
    if (!paragraph.length) return;
    out.push(`<p>${paragraph.map(escapeHtml).join("<br>")}</p>`);
    paragraph.length = 0;
  }

  function flushProfileAboutList(out, list) {
    if (!list.length) return;
    out.push(`<ul class="list-disc space-y-1 pl-5">${list.map((item) => `<li>${escapeHtml(item)}</li>`).join("")}</ul>`);
    list.length = 0;
  }

  function renderProfileMarkdown(markdown) {
    const lines = String(markdown || "").replace(/\r\n/g, "\n").split("\n");
    const out = [];
    const paragraph = [];
    const list = [];
    let inCode = false;
    let code = [];
    const flushText = () => {
      flushProfileAboutParagraph(out, paragraph);
      flushProfileAboutList(out, list);
    };
    for (const line of lines) {
      if (/^```/.test(line.trim())) {
        if (inCode) {
          out.push(`<pre class="overflow-auto rounded-md border border-border bg-background p-3 font-mono text-xs leading-5 text-muted-foreground"><code>${escapeHtml(code.join("\n"))}</code></pre>`);
          code = [];
          inCode = false;
        } else {
          flushText();
          inCode = true;
          code = [];
        }
        continue;
      }
      if (inCode) {
        code.push(line);
        continue;
      }
      if (!line.trim()) {
        flushText();
        continue;
      }
      const heading = line.match(/^(#{1,3})\s+(.+)$/);
      if (heading) {
        flushText();
        const level = heading[1].length;
        const size = level === 1 ? "text-2xl" : level === 2 ? "text-xl" : "text-base";
        out.push(`<h${level} class="${size} font-semibold text-foreground">${escapeHtml(heading[2])}</h${level}>`);
        continue;
      }
      const item = line.match(/^\s*[-*]\s+(.+)$/);
      if (item) {
        flushProfileAboutParagraph(out, paragraph);
        list.push(item[1]);
        continue;
      }
      flushProfileAboutList(out, list);
      paragraph.push(line);
    }
    flushText();
    if (inCode && code.length) {
      out.push(`<pre class="overflow-auto rounded-md border border-border bg-background p-3 font-mono text-xs leading-5 text-muted-foreground"><code>${escapeHtml(code.join("\n"))}</code></pre>`);
    }
    return out.length ? out.join("") : `<p>${escapeHtml(defaultProfileAbout()).replace(/\n/g, "<br>")}</p>`;
  }

  function renderProfileAbout(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const owner = $("[data-profile-about-owner]");
    const label = $("[data-profile-about-label]");
    const body = $("[data-profile-about-body]");
    const name = userDisplayName(String(session?.nodeName || session?.email || "forkmesh").trim() || "forkmesh");
    if (owner) owner.textContent = name;
    if (label) label.textContent = session?.kind === "node" ? "About this node" : "About yourself";
    if (body) {
      body.className = "grid gap-4 p-5 text-sm leading-6 text-foreground";
      body.innerHTML = renderProfileMarkdown(profileAboutMarkdown(session));
    }
  }

  function renderProfileReferrals(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const panel = $("[data-profile-referrals-panel]");
    const count = $("[data-profile-referrals-count]");
    const list = $("[data-profile-referrals-list]");
    if (!panel || !count || !list) return;
    const isUser = session?.kind !== "node";
    panel.classList.toggle("hidden", !isUser);
    if (!isUser) return;
    const total = Math.max(0, Number(session?.creditedReferrals) || 0);
    const names = Array.from(new Set(
      (Array.isArray(session?.referredUsers) ? session.referredUsers : [])
        .map((name) => String(name || "").trim().toLowerCase())
        .filter(validNodeName)
    ));
    count.textContent = `${formatCount(total)} credited`;
    const links = names.map((name) =>
      `<a class="font-medium text-accent hover:underline" href="${escapeHtml(publicProfileUrl(name))}">${userNameHtml(name)}</a>`
    ).join('<span class="text-muted-foreground">, </span>');
    if (!total) {
      list.innerHTML = '<p class="text-center text-sm text-muted-foreground">No credited referrals yet.</p>';
      return;
    }
    list.innerHTML = links
      ? `<p class="text-sm leading-6 text-foreground">${links}</p>`
      : "";
  }

  function repoBelongsToProfile(repo, aliases) {
    const owner = String(repo?.owner || "").trim().toLowerCase();
    const canonical = repoCanonicalIdentity(repo);
    const canonicalOwner = String(canonical.owner || "").trim().toLowerCase();
    // An organization's repositories are served by members' nodes and reach
    // its /@name page only through the public (alias) owner.
    const publicOwner = String(repoPublicOwner(repo) || "").trim().toLowerCase();
    return Boolean((owner && aliases.has(owner)) || (canonicalOwner && aliases.has(canonicalOwner)) ||
      (publicOwner && aliases.has(publicOwner)));
  }

  const PROFILE_CONTRIBUTION_DAY_MS = 24 * 60 * 60 * 1000;
  const PROFILE_CONTRIBUTION_MAX_LEVEL = 6;
  const PROFILE_CONTRIBUTION_CACHE_PREFIX = "forkmesh.profileContributions:v1:";
  let profileContributionRequestSequence = 0;
  const PROFILE_CONTRIBUTION_CATEGORIES = [
    { key: "commits", label: "Commits", singular: "commit", color: "var(--contribution-commits)", icon: "git-commit-horizontal" },
    { key: "issues", label: "Issues", singular: "issue", color: "var(--contribution-issues)", icon: "circle-dot" },
    { key: "pulls", label: "Pull requests", singular: "pull request", color: "var(--contribution-pulls)", icon: "git-pull-request" },
    { key: "reviews", label: "Reviews", singular: "review", color: "var(--contribution-reviews)", icon: "message-square-check" },
    { key: "repositories", label: "Repositories", singular: "repository publication", color: "var(--contribution-repositories)", icon: "book-marked" },
  ];

  function profileRepositoryAliases(session = profileSubject()) {
    const aliases = new Set();
    const add = (value) => {
      const text = String(value || "").trim().toLowerCase();
      if (text) aliases.add(text);
    };
    add(session?.nodeName);
    add(session?.email);
    (Array.isArray(session?.nodes) ? session.nodes : []).forEach(add);
    return aliases;
  }

  function profileContributionIsoDate(date) {
    return new Date(date).toISOString().slice(0, 10);
  }

  function profileContributionDate(value) {
    const text = String(value || "");
    if (!/^\d{4}-\d{2}-\d{2}$/.test(text)) return null;
    const date = new Date(`${text}T00:00:00.000Z`);
    return Number.isNaN(date.getTime()) || profileContributionIsoDate(date) !== text ? null : date;
  }

  function profileContributionRange(period = "rolling", now = new Date()) {
    const value = String(period || "rolling");
    const yearMatch = /^(?:year:)?(\d{4})$/.exec(value);
    if (yearMatch) {
      const year = Math.max(1971, Math.min(9998, Number(yearMatch[1])));
      return {
        value: String(year),
        from: `${year}-01-01`,
        to: `${year}-12-31`,
        label: String(year),
      };
    }
    const end = new Date(Date.UTC(now.getUTCFullYear(), now.getUTCMonth(), now.getUTCDate()));
    const start = new Date(end.getTime() - (364 * PROFILE_CONTRIBUTION_DAY_MS));
    return {
      value: "rolling",
      from: profileContributionIsoDate(start),
      to: profileContributionIsoDate(end),
      label: "the last year",
    };
  }

  function profileContributionCount(value) {
    const number = Number(value);
    return Number.isSafeInteger(number) && number > 0 ? number : 0;
  }

  function profileContributionSubjectName() {
    const subject = profileSubject();
    return String(publicProfileNameFromPath() || subject?.nodeName || subject?.email || "")
      .trim().replace(/^@/, "");
  }

  function profileContributionRequestKey(name, range) {
    return `${String(name).toLowerCase()}:${range.from}:${range.to}`;
  }

  function profileContributionCacheKey(requestKey) {
    return PROFILE_CONTRIBUTION_CACHE_PREFIX + requestKey;
  }

  function readProfileContributionCache(requestKey, range) {
    try {
      const parsed = JSON.parse(sessionStorage.getItem(profileContributionCacheKey(requestKey)) || "null");
      const data = parsed?.data;
      if (!data || data.ok !== true || data.range?.from !== range.from || data.range?.to !== range.to) return null;
      return data;
    } catch (_) {
      return null;
    }
  }

  function writeProfileContributionCache(requestKey, data) {
    try {
      sessionStorage.setItem(profileContributionCacheKey(requestKey), JSON.stringify({ data, savedAt: Date.now() }));
    } catch (_) {
      // A disabled or full session store only removes the offline fallback.
    }
  }

  function removeProfileContributionCache(requestKey) {
    try {
      sessionStorage.removeItem(profileContributionCacheKey(requestKey));
    } catch (_) {}
    delete state.profileContributions.cache[requestKey];
  }

  function profileContributionCategoryKnown(coverage, category, dateValue = "") {
    if (category === "repositories") return true;
    if (coverage?.status === "complete") return true;
    if (coverage?.status === "unavailable") return false;
    const missing = new Set(Array.isArray(coverage?.missing) ? coverage.missing : []);
    if (category === "commits" && missing.has("commits")) return false;
    if (["issues", "pulls", "reviews"].includes(category) && missing.has("collaboration")) return false;
    if (category === "languages" && missing.has("languages")) return false;
    if (["commits", "issues", "pulls", "reviews"].includes(category) &&
        missing.has("history")) {
      const verifiedFrom = profileContributionDate(coverage?.verifiedFrom);
      const date = profileContributionDate(dateValue);
      if (!verifiedFrom || !date || date < verifiedFrom) return false;
    }
    return true;
  }

  function normalizeProfileContributionDays(data, range = state.profileContributions.range) {
    const start = profileContributionDate(range?.from);
    const end = profileContributionDate(range?.to);
    if (!start || !end || end < start) return [];
    const sparse = new Map();
    (Array.isArray(data?.days) ? data.days : []).forEach((row) => {
      const date = profileContributionDate(row?.date);
      if (!date || date < start || date > end) return;
      const normalized = { date: profileContributionIsoDate(date) };
      PROFILE_CONTRIBUTION_CATEGORIES.forEach(({ key }) => {
        normalized[key] = profileContributionCount(row?.[key]);
      });
      sparse.set(normalized.date, normalized);
    });
    const coverage = data?.coverage || {};
    const length = Math.min(366, Math.floor((end - start) / PROFILE_CONTRIBUTION_DAY_MS) + 1);
    return Array.from({ length }, (_, index) => {
      const date = new Date(start.getTime() + (index * PROFILE_CONTRIBUTION_DAY_MS));
      const key = profileContributionIsoDate(date);
      const explicit = sparse.get(key);
      const day = explicit || { date: key };
      PROFILE_CONTRIBUTION_CATEGORIES.forEach(({ key: category }) => {
        day[category] = profileContributionCount(day[category]);
      });
      day.known = Object.fromEntries(PROFILE_CONTRIBUTION_CATEGORIES.map(({ key: category }) => [
        category,
        profileContributionCategoryKnown(coverage, category, key),
      ]));
      day.total = PROFILE_CONTRIBUTION_CATEGORIES.reduce(
        (sum, { key: category }) => sum + day[category], 0,
      );
      day.verified = PROFILE_CONTRIBUTION_CATEGORIES.every(
        ({ key: category }) => day.known[category],
      );
      return day;
    });
  }

  function profileContributionStackLevel(count) {
    const value = profileContributionCount(count);
    return value ? Math.min(PROFILE_CONTRIBUTION_MAX_LEVEL, 1 + Math.floor(Math.log2(value))) : 0;
  }

  function profileContributionCubeFaces(x, y, height, halfWidth = 7, depth = 4) {
    const topY = y - height;
    return {
      top: `${x},${topY} ${x + halfWidth},${topY + depth} ${x},${topY + (2 * depth)} ${x - halfWidth},${topY + depth}`,
      left: `${x - halfWidth},${topY + depth} ${x},${topY + (2 * depth)} ${x},${y + (2 * depth)} ${x - halfWidth},${y + depth}`,
      right: `${x + halfWidth},${topY + depth} ${x},${topY + (2 * depth)} ${x},${y + (2 * depth)} ${x + halfWidth},${y + depth}`,
    };
  }

  function profileContributionTooltipText(day) {
    const date = profileContributionDate(day?.date);
    const dateLabel = date
      ? new Intl.DateTimeFormat(undefined, { dateStyle: "long", timeZone: "UTC" }).format(date)
      : "Unknown date";
    const counts = PROFILE_CONTRIBUTION_CATEGORIES.map(({ key, label, singular }) => {
      const count = profileContributionCount(day?.[key]);
      const known = day?.known?.[key] ?? Boolean(day?.verified);
      if (!known) {
        return count
          ? `${formatCount(count)} known ${singular}${count === 1 ? "" : "s"} (partial)`
          : `${label.toLowerCase()} not fully verified`;
      }
      return `${formatCount(count)} ${singular}${count === 1 ? "" : "s"}`;
    });
    return `${dateLabel}: ${counts.join(", ")}`;
  }

  function profileContributionRadarPoints(typeTotals, coverage = {}, radius = 62, centerX = 120, centerY = 88) {
    const counts = PROFILE_CONTRIBUTION_CATEGORIES.map(({ key }) => profileContributionCount(typeTotals?.[key]));
    const total = counts.reduce((sum, count) => sum + count, 0);
    const shares = counts.map((count) => total ? count / total : 0);
    return PROFILE_CONTRIBUTION_CATEGORIES.map((category, index) => {
      const angle = (-Math.PI / 2) + ((Math.PI * 2 * index) / PROFILE_CONTRIBUTION_CATEGORIES.length);
      const visualShare = shares[index];
      return {
        ...category,
        count: counts[index],
        share: shares[index],
        known: profileContributionCategoryKnown(coverage, category.key),
        x: centerX + (Math.cos(angle) * radius * visualShare),
        y: centerY + (Math.sin(angle) * radius * visualShare),
        axisX: centerX + (Math.cos(angle) * radius),
        axisY: centerY + (Math.sin(angle) * radius),
        labelX: centerX + (Math.cos(angle) * (radius + 24)),
        labelY: centerY + (Math.sin(angle) * (radius + 19)),
      };
    });
  }

  function profileContributionLanguageSegments(languages) {
    const byName = new Map();
    (Array.isArray(languages) ? languages : []).forEach((language) => {
      const name = String(language?.name || "").trim().slice(0, 48);
      const bytes = profileContributionCount(language?.bytes);
      if (!name || !bytes) return;
      const previous = byName.get(name) || { name, bytes: 0, color: "" };
      previous.bytes += bytes;
      if (/^#[0-9a-f]{6}$/i.test(String(language?.color || ""))) previous.color = language.color;
      byName.set(name, previous);
    });
    const explicitOther = byName.get("Other");
    byName.delete("Other");
    const ranked = [...byName.values()].sort((a, b) => b.bytes - a.bytes || a.name.localeCompare(b.name));
    const top = ranked.slice(0, 5);
    const otherBytes = ranked.slice(5).reduce((sum, item) => sum + item.bytes, 0) + (explicitOther?.bytes || 0);
    if (otherBytes) top.push({ name: "Other", bytes: otherBytes, color: "var(--contribution-unverified)" });
    const total = top.reduce((sum, item) => sum + item.bytes, 0);
    let offset = 0;
    return top.map((item) => {
      const percentage = total ? (item.bytes / total) * 100 : 0;
      const segment = {
        ...item,
        color: item.color || "var(--contribution-unverified)",
        percentage,
        offset,
      };
      offset += percentage;
      return segment;
    });
  }

  function profileContributionCoverageMessage(coverage, range, { cached = false } = {}) {
    if (cached) return "Showing saved verified data because the live ledger is unavailable.";
    const verifiedFrom = profileContributionDate(coverage?.verifiedFrom);
    const verifiedLabel = verifiedFrom
      ? new Intl.DateTimeFormat(undefined, { dateStyle: "medium", timeZone: "UTC" }).format(verifiedFrom)
      : range?.from;
    const missing = (Array.isArray(coverage?.missing) ? coverage.missing : [])
      .map((item) => String(item || "").trim()).filter(Boolean).slice(0, 4);
    const missingLabels = missing.map((item) => ({
      commits: "commit history",
      collaboration: "collaboration events",
      languages: "language footprint",
      history: "earlier history",
    })[item] || item);
    if (coverage?.status === "partial") {
      return `Partial verified history from ${verifiedLabel || "the available sources"}${missingLabels.length ? `. Missing ${missingLabels.join(", ")}.` : "."}`;
    }
    if (coverage?.status === "unavailable") return "Verified contribution history is not available yet.";
    return `Complete verified public history for ${range?.label || "this period"}.`;
  }

  function renderProfileContributionStatus(status, { hasData = false } = {}) {
    const card = $("[data-profile-contribution-card]");
    const loading = $("[data-profile-contribution-loading]");
    const empty = $("[data-profile-contribution-empty]");
    const error = $("[data-profile-contribution-error]");
    const panorama = $("[data-profile-contribution-panorama]");
    const activityItems = $("[data-profile-activity-items]");
    const activityEmpty = $("[data-profile-activity-empty]");
    if (card) card.setAttribute("aria-busy", status === "loading" ? "true" : "false");
    if (loading) loading.hidden = status !== "loading";
    if (empty) empty.hidden = status !== "empty";
    if (error) error.hidden = status !== "error";
    if (panorama) panorama.hidden = !hasData;
    if (status === "loading" || status === "error") {
      if (activityItems) activityItems.innerHTML = "";
      if (activityEmpty) {
        activityEmpty.classList.remove("hidden");
        activityEmpty.textContent = status === "loading"
          ? "Loading verified contribution activity..."
          : "Contribution activity is unavailable right now.";
      }
    }
  }

  function profileContributionPeriods(data, activeValue) {
    const periods = [{ value: "rolling", label: "Last year" }];
    const currentYear = new Date().getUTCFullYear();
    for (let year = currentYear - 1; year >= currentYear - 5; year -= 1) {
      periods.push({ value: String(year), label: String(year) });
    }
    if (/^\d{4}$/.test(String(activeValue)) && !periods.some((period) => period.value === String(activeValue))) {
      periods.push({ value: String(activeValue), label: String(activeValue) });
    }
    return periods;
  }

  function renderProfileContributionPeriods(data, activeValue) {
    const container = $("[data-profile-contribution-periods]");
    if (!container) return;
    container.innerHTML = profileContributionPeriods(data, activeValue).map((period) => {
      const active = period.value === activeValue;
      return `<button data-profile-contribution-period="${escapeHtml(period.value)}" type="button" aria-current="${active ? "true" : "false"}" class="shrink-0 rounded-md px-3 py-1.5 font-semibold ${active ? "bg-secondary text-foreground" : "text-muted-foreground hover:bg-secondary hover:text-foreground"}">${escapeHtml(period.label)}</button>`;
    }).join("");
  }

  function showProfileContributionTooltip(day, target, event) {
    const tooltip = $("[data-profile-contribution-tooltip]");
    const panel = $("[data-profile-contribution-skyline-panel]");
    if (!tooltip || !panel) return;
    tooltip.textContent = profileContributionTooltipText(day);
    tooltip.hidden = false;
    const panelRect = panel.getBoundingClientRect();
    const targetRect = target?.getBoundingClientRect?.() || panelRect;
    const pointerX = Number(event?.clientX) || targetRect.left + (targetRect.width / 2);
    const pointerY = Number(event?.clientY) || targetRect.top;
    const left = Math.max(12, Math.min(panelRect.width - tooltip.offsetWidth - 12, pointerX - panelRect.left + 10));
    const top = Math.max(56, Math.min(panelRect.height - tooltip.offsetHeight - 12, pointerY - panelRect.top - tooltip.offsetHeight - 8));
    tooltip.style.left = `${left}px`;
    tooltip.style.top = `${top}px`;
    state.profileContributions.selectedDay = day.date;
  }

  function hideProfileContributionTooltip() {
    const tooltip = $("[data-profile-contribution-tooltip]");
    if (tooltip) tooltip.hidden = true;
    state.profileContributions.selectedDay = "";
  }

  function renderProfileContributionSkyline(data, range) {
    const svg = $("[data-profile-contribution-skyline]");
    if (!svg) return;
    const days = normalizeProfileContributionDays(data, range).map((day, index) => {
      const week = Math.floor(index / 7);
      const weekday = index % 7;
      return {
        ...day,
        week,
        weekday,
        x: 96 + (week * 15) - (weekday * 8),
        y: 78 + (week * 3.4) + (weekday * 4.5),
        depth: week + weekday,
      };
    }).sort((a, b) => a.y - b.y || a.x - b.x);
    const content = days.map((day) => {
      const base = profileContributionCubeFaces(day.x, day.y, 0);
      const fill = day.verified ? "var(--contribution-empty)" : "var(--contribution-unverified)";
      let height = 0;
      const segments = PROFILE_CONTRIBUTION_CATEGORIES.map((category) => {
        const level = profileContributionStackLevel(day[category.key]);
        if (!level) return "";
        const segmentHeight = level * 2.5;
        const faces = profileContributionCubeFaces(day.x, day.y - height, segmentHeight);
        height += segmentHeight;
        return `<g data-contribution-kind="${category.key}"><polygon points="${faces.left}" fill="${category.color}" opacity="0.62"></polygon><polygon points="${faces.right}" fill="${category.color}" opacity="0.82"></polygon><polygon points="${faces.top}" fill="${category.color}"></polygon></g>`;
      }).join("");
      const focus = day.total ? ` tabindex="0" role="img" aria-label="${escapeHtml(profileContributionTooltipText(day))}"` : "";
      return `<g data-profile-contribution-day="${day.date}"${focus}><polygon points="${base.top}" fill="${fill}" stroke="var(--contribution-grid-edge)" stroke-width="0.65"></polygon>${segments}</g>`;
    }).join("");
    svg.innerHTML = `<title id="profile-contribution-skyline-title">Daily contribution skyline</title><desc id="profile-contribution-skyline-desc">Verified commits, issues, pull requests, reviews, and repository publications from ${escapeHtml(range.from)} through ${escapeHtml(range.to)}.</desc>${content}`;
    const byDate = new Map(days.map((day) => [day.date, day]));
    svg.querySelectorAll("[data-profile-contribution-day][tabindex='0']").forEach((group) => {
      const day = byDate.get(group.dataset.profileContributionDay);
      group.addEventListener("mouseenter", (event) => showProfileContributionTooltip(day, group, event));
      group.addEventListener("mousemove", (event) => showProfileContributionTooltip(day, group, event));
      group.addEventListener("mouseleave", hideProfileContributionTooltip);
      group.addEventListener("focus", () => showProfileContributionTooltip(day, group));
      group.addEventListener("blur", hideProfileContributionTooltip);
    });
    const viewport = $("[data-profile-contribution-skyline-viewport]");
    window.requestAnimationFrame(() => {
      const scrollToRecent = () => {
        if (viewport && viewport.scrollWidth > viewport.clientWidth) {
          viewport.scrollLeft = viewport.scrollWidth - viewport.clientWidth;
        }
      };
      scrollToRecent();
      window.requestAnimationFrame(scrollToRecent);
    });
  }

  function renderProfileContributionRadar(data) {
    const svg = $("[data-profile-contribution-radar]");
    if (!svg) return;
    const points = profileContributionRadarPoints(
      data?.typeTotals || {}, data?.coverage || {},
    );
    const rings = [0.25, 0.5, 0.75, 1].map((scale) => {
      const ring = points.map((point) => {
        const x = 120 + ((point.axisX - 120) * scale);
        const y = 88 + ((point.axisY - 88) * scale);
        return `${x.toFixed(1)},${y.toFixed(1)}`;
      }).join(" ");
      return `<polygon points="${ring}" fill="none" stroke="var(--contribution-grid-edge)" stroke-width="1"></polygon>`;
    }).join("");
    const axes = points.map((point) => `<line x1="120" y1="88" x2="${point.axisX.toFixed(1)}" y2="${point.axisY.toFixed(1)}" stroke="var(--contribution-grid-edge)" stroke-width="1"></line>`).join("");
    const shape = points.map((point) => `${point.x.toFixed(1)},${point.y.toFixed(1)}`).join(" ");
    const labels = points.map((point) => {
      const anchor = point.labelX < 112 ? "end" : point.labelX > 128 ? "start" : "middle";
      const countLabel = `${formatCount(point.count)}${point.known ? "" : "+"}`;
      return `<text x="${point.labelX.toFixed(1)}" y="${point.labelY.toFixed(1)}" text-anchor="${anchor}" fill="currentColor" font-size="9"><tspan x="${point.labelX.toFixed(1)}">${escapeHtml(point.label)}</tspan><tspan x="${point.labelX.toFixed(1)}" dy="11" fill="rgb(var(--dashboard-muted-foreground-rgb))">${countLabel}</tspan></text>`;
    }).join("");
    const description = points.map((point) => point.known
      ? `${point.label}: ${formatCount(point.count)}`
      : `${point.label}: at least ${formatCount(point.count)}, partial coverage`).join(", ");
    svg.innerHTML = `<title id="profile-contribution-radar-title">Contribution activity mix</title><desc id="profile-contribution-radar-desc">${escapeHtml(description)}</desc><g>${rings}${axes}<polygon points="${shape}" fill="var(--contribution-commits)" fill-opacity="0.24" stroke="var(--contribution-commits)" stroke-width="2"></polygon>${labels}</g>`;
  }

  function renderProfileContributionLanguages(data) {
    const svg = $("[data-profile-contribution-languages]");
    const legend = $("[data-profile-contribution-language-legend]");
    if (!svg || !legend) return;
    const segments = profileContributionLanguageSegments(data?.languages);
    const complete = profileContributionCategoryKnown(data?.coverage, "languages");
    if (!segments.length) {
      const message = complete ? "No language data" : "Language data partial";
      const description = complete
        ? "No supported language bytes are available."
        : "Language coverage is not fully verified for this profile.";
      svg.innerHTML = `<title id="profile-contribution-languages-title">Contribution language donut</title><desc id="profile-contribution-languages-desc">${description}</desc><circle cx="120" cy="82" r="48" fill="none" stroke="var(--contribution-empty)" stroke-width="18"></circle><text x="120" y="86" text-anchor="middle" fill="currentColor" font-size="11">${message}</text>`;
      legend.innerHTML = complete ? "" : '<p class="text-xs text-muted-foreground">Partial language data</p>';
      return;
    }
    const circles = segments.map((segment) => `<circle cx="120" cy="82" r="48" pathLength="100" fill="none" stroke="${segment.color}" stroke-width="18" stroke-dasharray="${segment.percentage.toFixed(4)} ${(100 - segment.percentage).toFixed(4)}" stroke-dashoffset="${(-segment.offset).toFixed(4)}" transform="rotate(-90 120 82)"><title>${escapeHtml(segment.name)}: ${segment.percentage.toFixed(1)}%, ${formatCount(segment.bytes)} bytes</title></circle>`).join("");
    const description = `${complete ? "" : "Partial language coverage. "}${segments.map((segment) => `${segment.name}: ${segment.percentage.toFixed(1)} percent`).join(", ")}`;
    svg.innerHTML = `<title id="profile-contribution-languages-title">Contribution language donut</title><desc id="profile-contribution-languages-desc">${escapeHtml(description)}</desc>${circles}<text x="120" y="78" text-anchor="middle" fill="currentColor" font-size="11">Public code</text><text x="120" y="94" text-anchor="middle" fill="rgb(var(--dashboard-muted-foreground-rgb))" font-size="9">by bytes</text>`;
    legend.innerHTML = `${complete ? "" : '<p class="mb-1 text-xs text-muted-foreground">Partial language data</p>'}${segments.map((segment) => `<div class="flex min-w-0 items-center justify-between gap-2"><span class="flex min-w-0 items-center gap-2"><span aria-hidden="true" class="h-2.5 w-2.5 shrink-0 rounded-full" style="background:${segment.color}"></span><span class="truncate">${escapeHtml(segment.name)}</span></span><span class="shrink-0 tabular-nums">${segment.percentage.toFixed(1)}%</span></div>`).join("")}`;
  }

  function renderProfileContributionMetrics(data) {
    const metrics = $("[data-profile-contribution-metrics]");
    if (!metrics) return;
    metrics.innerHTML = PROFILE_CONTRIBUTION_CATEGORIES.map((category) => {
      const known = profileContributionCategoryKnown(data?.coverage, category.key);
      const count = formatCount(profileContributionCount(data?.typeTotals?.[category.key]));
      return `<div><dt class="truncate text-xs text-muted-foreground">${escapeHtml(category.label)}</dt><dd class="mt-1 text-lg font-semibold tabular-nums text-foreground">${count}${known ? "" : "+"}</dd>${known ? "" : '<span class="text-[10px] text-muted-foreground">partial</span>'}</div>`;
    }).join("");
  }

  function renderProfileContributionCoverage(data, range, { cached = false } = {}) {
    const coverage = $("[data-profile-contribution-coverage]");
    if (!coverage) return;
    coverage.textContent = profileContributionCoverageMessage(data?.coverage, range, { cached });
    if (cached) {
      const retry = document.createElement("button");
      retry.type = "button";
      retry.setAttribute("data-profile-contribution-retry", "");
      retry.className = "ml-2 font-semibold text-accent hover:underline";
      retry.textContent = "Retry live data";
      coverage.append(retry);
    }
  }

  function profileContributionRepositoryUrl(repository) {
    const supplied = String(repository?.url || "");
    if (supplied.startsWith("/") && !supplied.startsWith("//")) return repoPublicHref(supplied);
    const owner = String(repository?.owner || "").trim();
    const name = String(repository?.name || "").trim();
    return owner && name ? repoPathUrl({ owner, name }) : "/dashboard/repos";
  }

  function renderProfileActivity(data) {
    const container = $("[data-profile-activity-items]");
    const empty = $("[data-profile-activity-empty]");
    if (!container || !empty) return;
    const events = (Array.isArray(data?.recentActivity) ? data.recentActivity : []).slice(0, 20);
    empty.classList.toggle("hidden", Boolean(events.length));
    empty.textContent = "No contribution activity found for this period yet.";
    container.innerHTML = events.map((event) => {
      const category = PROFILE_CONTRIBUTION_CATEGORIES.find((item) => item.key === event?.kind) || PROFILE_CONTRIBUTION_CATEGORIES[0];
      const count = profileContributionCount(event?.count);
      const repository = event?.repository || {};
      const repoLabel = repository.owner && repository.name
        ? repoPublicKey(`${repository.owner}/${repository.name}`)
        : [repository.owner, repository.name].filter(Boolean).join("/") || "Public repository";
      const date = profileContributionDate(event?.date);
      const dateLabel = date
        ? new Intl.DateTimeFormat(undefined, { month: "short", day: "numeric", year: "numeric", timeZone: "UTC" }).format(date)
        : String(event?.date || "");
      return `<div data-profile-activity-item class="grid grid-cols-[2.5rem_minmax(0,1fr)] gap-4 md:grid-cols-[2.5rem_minmax(0,1fr)_8rem]"><div class="flex flex-col items-center"><span class="flex h-10 w-10 items-center justify-center rounded-full bg-secondary" style="color:${category.color}"><i data-lucide="${category.icon}" class="h-5 w-5"></i></span><span class="h-12 w-px bg-border"></span></div><div class="min-w-0"><p class="text-base font-semibold leading-6 text-foreground">${formatCount(count)} ${escapeHtml(category.singular)}${count === 1 ? "" : "s"}</p><p class="mt-2 flex min-w-0 items-center gap-2 text-sm"><i data-lucide="git-fork" class="h-4 w-4 shrink-0 text-muted-foreground"></i><a href="${escapeHtml(profileContributionRepositoryUrl(repository))}" class="truncate text-accent hover:underline">${escapeHtml(repoLabel)}</a></p><time datetime="${escapeHtml(event?.date || "")}" class="mt-2 block text-xs text-muted-foreground md:hidden">${escapeHtml(dateLabel)}</time></div><div class="hidden items-start justify-end text-sm text-muted-foreground md:flex"><time datetime="${escapeHtml(event?.date || "")}">${escapeHtml(dateLabel)}</time></div></div>`;
    }).join("");
    window.lucide?.createIcons();
  }

  function renderProfileContributionData(data, range, { cached = false } = {}) {
    const total = profileContributionCount(data?.total);
    const repositoryCount = profileContributionCount(data?.repositoryCount);
    const summary = $("[data-profile-contribution-summary]");
    const rangeLabel = $("[data-profile-contribution-range]");
    if (summary) {
      summary.textContent = `${formatCount(total)} verified public contribution${total === 1 ? "" : "s"} recorded across ${formatCount(repositoryCount)} repositor${repositoryCount === 1 ? "y" : "ies"} in ${range.label}`;
    }
    if (rangeLabel) rangeLabel.textContent = `${range.from} to ${range.to} · UTC`;
    renderProfileContributionPeriods(data, range.value);
    renderProfileContributionSkyline(data, range);
    renderProfileContributionRadar(data);
    renderProfileContributionLanguages(data);
    renderProfileContributionMetrics(data);
    renderProfileContributionCoverage(data, range, { cached });
    renderProfileActivity(data);
    const coverageStatus = ["complete", "partial", "unavailable"].includes(data?.coverage?.status)
      ? data.coverage.status
      : "unavailable";
    const status = cached ? "cached" : (!total && coverageStatus === "complete" ? "empty" : coverageStatus);
    renderProfileContributionStatus(status, { hasData: true });
  }

  function renderProfileContributionPlaceholder(range) {
    const placeholder = {
      typeTotals: Object.fromEntries(PROFILE_CONTRIBUTION_CATEGORIES.map(({ key }) => [key, 0])),
      days: [],
      languages: [],
      coverage: {
        status: "unavailable",
        verifiedFrom: null,
        missing: ["commits", "collaboration", "languages", "history"],
      },
    };
    const summary = $("[data-profile-contribution-summary]");
    const rangeLabel = $("[data-profile-contribution-range]");
    const coverage = $("[data-profile-contribution-coverage]");
    if (summary) summary.textContent = `Loading verified contributions for ${range.label}`;
    if (rangeLabel) rangeLabel.textContent = `${range.from} to ${range.to} · UTC`;
    if (coverage) coverage.textContent = "Waiting for the verified public ledger.";
    renderProfileContributionSkyline(placeholder, range);
    renderProfileContributionRadar(placeholder);
    renderProfileContributionLanguages(placeholder);
    renderProfileContributionMetrics(placeholder);
  }

  async function renderProfileContributionGraph({ period, force = false } = {}) {
    if (!$("[data-profile-contribution-card]")) return;
    if (profileSubject()?.kind === "organization") return;
    const activeValue = String(period || state.profileContributions.range?.value || "rolling");
    const range = profileContributionRange(activeValue);
    const name = profileContributionSubjectName();
    renderProfileContributionPeriods(state.profileContributions.data, range.value);
    renderProfileContributionPlaceholder(range);
    if (!name) {
      state.profileContributions.error = "Profile identity is unavailable.";
      renderProfileContributionStatus("error", { hasData: true });
      return;
    }
    const requestKey = profileContributionRequestKey(name, range);
    const memory = state.profileContributions.cache[requestKey];
    state.profileContributions.range = range;
    if (!force && memory) {
      profileContributionRequestSequence += 1;
      state.profileContributions.requestKey = requestKey;
      state.profileContributions.data = memory;
      state.profileContributions.loading = false;
      state.profileContributions.error = "";
      renderProfileContributionData(memory, range);
      return;
    }
    if (!force && state.profileContributions.loading && state.profileContributions.requestKey === requestKey) return;
    const requestSequence = ++profileContributionRequestSequence;
    state.profileContributions.requestKey = requestKey;
    state.profileContributions.loading = true;
    state.profileContributions.error = "";
    renderProfileContributionStatus("loading", { hasData: true });
    const path = "/api/accounts/" + encodeURIComponent(name) + "/contributions?from=" +
      encodeURIComponent(range.from) + "&to=" + encodeURIComponent(range.to);
    try {
      const data = await fetchJson(path, { fresh: force, cacheBust: false });
      if (state.profileContributions.requestKey !== requestKey || requestSequence !== profileContributionRequestSequence) return;
      if (data?.ok !== true || data?.range?.from !== range.from || data?.range?.to !== range.to) {
        throw new Error("Contribution response did not match the requested period.");
      }
      state.profileContributions.cache[requestKey] = data;
      state.profileContributions.data = data;
      state.profileContributions.loading = false;
      writeProfileContributionCache(requestKey, data);
      renderProfileContributionData(data, range);
    } catch (error) {
      if (state.profileContributions.requestKey !== requestKey || requestSequence !== profileContributionRequestSequence) return;
      state.profileContributions.loading = false;
      state.profileContributions.error = String(error?.message || "Contribution activity is unavailable.");
      const status = Number(error?.status) || 0;
      const temporaryFailure = status === 0 || status === 408 || status === 425 ||
        status === 429 || status >= 500;
      if (!temporaryFailure) removeProfileContributionCache(requestKey);
      const cached = temporaryFailure
        ? readProfileContributionCache(requestKey, range)
        : null;
      if (cached) {
        state.profileContributions.data = cached;
        renderProfileContributionData(cached, range, { cached: true });
      } else {
        const summary = $("[data-profile-contribution-summary]");
        if (summary) summary.textContent = `Contribution activity is unavailable for ${range.label}`;
        renderProfileContributionStatus("error", { hasData: true });
      }
    }
  }

  function setProfileAboutHint(text, cls) {
    setProfilePageHint("[data-profile-about-hint]", text, cls);
  }

  function setProfileAboutModalOpen(open) {
    const modal = $("[data-profile-about-modal]");
    if (!modal) return;
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) {
      const textarea = $("[data-profile-about-textarea]");
      if (textarea) textarea.value = profileAboutMarkdown(state.session);
      setProfileAboutHint("", "");
      window.setTimeout(() => textarea?.focus(), 0);
    }
  }

  function validNodeName(value) {
    return /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(String(value || ""));
  }

  // A node's Ed25519 public key (raw 32 bytes, base64url, unpadded) - the
  // value the desktop app's own profile card labels "Node ID" (issue #351),
  // so the claim-node input below must accept it alongside the account name.
  function validNodePubkey(value) {
    return /^[A-Za-z0-9_-]{43}$/.test(String(value || ""));
  }

  // Email switch defaults per emailable kind, mirroring NOTIFICATION_EMAIL_DEFAULTS
  // in the Worker. general_chat is the #general digest, not a ping kind.
  const NOTIFICATION_PREFERENCE_DEFAULTS = {
    mention: true,
    subscribed: true,
    issue_opened: true,
    pull_submitted: true,
    issue_assigned: true,
    repo_shared: true,
    security_advisory: true,
    bounty_funded: true,
    bounty_paid: true,
    release_published: true,
    pending_inbox: true,
    credits_refilled: true,
    general_chat: true,
    host_online: false,
    host_offline: false,
    mirror_request: true,
    pending_reward: true,
    org_succession: true,
    tip_sent: true,
    calendar_invitation: true,
    calendar_reminder: true,
    calendar_rsvp: true,
    channel_unread: true,
    direct_unread: true,
  };

  function normalizedNotificationPreferences(session = state.session) {
    const raw = session?.notificationPreferences;
    const prefs = { ...NOTIFICATION_PREFERENCE_DEFAULTS };
    if (raw && typeof raw === "object") {
      Object.keys(raw).forEach((key) => { prefs[key] = Boolean(raw[key]); });
    }
    return prefs;
  }

  // How the emails are paced: one rollup per interval, or one mail per ping
  // the moment it lands. Anything unrecognised reads as the digest default so
  // an older session payload can never silently switch somebody to instant.
  const NOTIFICATION_DELIVERY_MODES = ["digest", "instant"];
  const NOTIFICATION_DELIVERY_DEFAULT = "digest";

  function normalizedNotificationDelivery(session = state.session) {
    const raw = String(session?.notificationDelivery || "").trim().toLowerCase();
    return NOTIFICATION_DELIVERY_MODES.includes(raw) ? raw : NOTIFICATION_DELIVERY_DEFAULT;
  }

  function renderNotificationDelivery(session = state.session) {
    const mode = normalizedNotificationDelivery(session);
    $$("[data-notification-delivery-mode]").forEach((input) => {
      input.checked = (input.dataset.notificationDeliveryMode || "") === mode;
    });
  }

  function collectNotificationDelivery() {
    const picked = $$("[data-notification-delivery-mode]").find((input) => input.checked);
    const mode = picked?.dataset.notificationDeliveryMode || "";
    return NOTIFICATION_DELIVERY_MODES.includes(mode) ? mode : NOTIFICATION_DELIVERY_DEFAULT;
  }

  // ---- The one Pings settings page (#347) ----------------------------------
  // Every ping switch lives here: which kinds are filed at all, which are
  // emailed, pause, quiet hours, pacing, and (administrators only) the /status
  // monitor alert matrix that used to be the admin console's "Alert delivery"
  // page. Each switch auto-saves to POST /api/notifications {pingSettings} and
  // re-renders from the reply, which carries the whole settings shape.
  //
  // The Worker sends pingKindCatalog with the account payload, so a kind added
  // there shows up here without a dashboard release. This copy is only for an
  // older cached session that predates the catalog.
  const PING_KIND_CATALOG_FALLBACK = [
    { key: "conversations", label: "Conversations", kinds: [
      { kind: "mention", label: "Mentions", email: true },
      { kind: "subscribed", label: "Activity on things you follow", email: true },
      { kind: "chat_webhook", label: "Chat webhook posts", email: false },
      { kind: "channel_unread", label: "Unread channel messages", email: true },
      { kind: "direct_unread", label: "Unread direct messages", email: true },
      { kind: "calendar_invitation", label: "Calendar invitations", email: true },
      { kind: "calendar_reminder", label: "Calendar reminders", email: true },
      { kind: "calendar_rsvp", label: "Calendar replies", email: true },
    ] },
    { key: "repositories", label: "Repositories", kinds: [
      { kind: "issue_opened", label: "New issues", email: true },
      { kind: "pull_submitted", label: "New pull requests", email: true },
      { kind: "issue_assigned", label: "Issue assignments", email: true },
      { kind: "pending_inbox", label: "Inbox updates", email: true },
      { kind: "code_push", label: "Code pushes", email: false },
      { kind: "repo_activity", label: "Merged pull requests", email: false },
      { kind: "release_published", label: "Releases", email: true },
      { kind: "repo_shared", label: "Repositories shared with you", email: true },
      { kind: "security_advisory", label: "Security advisories", email: true },
      { kind: "mirror_request", label: "Mirror requests", email: true },
      { kind: "repository_hosted", label: "Imports finished hosting", email: false },
    ] },
    { key: "organizations", label: "Organizations", kinds: [
      { kind: "org_succession", label: "Organization succession", email: true },
      { kind: "organization_task_update", label: "Tasks", email: false },
    ] },
    { key: "rewards", label: "Money & rewards", kinds: [
      { kind: "bounty_funded", label: "Bounties funded", email: true },
      { kind: "bounty_paid", label: "Bounties paid", email: true },
      { kind: "pending_reward", label: "Pending rewards", email: true },
      { kind: "credits_refilled", label: "Credits refilled", email: true },
      { kind: "tip_sent", label: "Tips", email: true },
      { kind: "hackathon_squad", label: "Hackathon squad progress", email: false },
      { kind: "hackathon_reward", label: "Hackathon rewards", email: false },
      { kind: "badge", label: "Badges earned", email: false },
    ] },
    { key: "devices", label: "Devices & apps", kinds: [
      { kind: "host_online", label: "Host came online", email: true },
      { kind: "host_offline", label: "Host went offline", email: true },
      { kind: "device_added", label: "New device signed in", email: false },
      { kind: "timer_finished", label: "Timers and alarms", email: false },
      { kind: "desktop_ping", label: "Pings from your desktop", email: false },
      { kind: "plugin_activity", label: "App package activity", email: false },
    ] },
  ];
  const PING_QUIET_DEFAULTS = { enabled: false, start: 22 * 60, end: 7 * 60, days: [0, 1, 2, 3, 4, 5, 6], timezone: "" };
  const PING_QUIET_DAY_NAMES = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

  function pingKindCatalog(session = state.session) {
    const raw = session?.pingKindCatalog;
    if (Array.isArray(raw) && raw.length) {
      return raw
        .filter((group) => group && Array.isArray(group.kinds) && group.kinds.length)
        .map((group) => ({
          key: String(group.key || ""),
          label: String(group.label || group.key || ""),
          kinds: group.kinds
            .filter((entry) => entry && entry.kind)
            .map((entry) => ({ kind: String(entry.kind), label: String(entry.label || entry.kind), email: Boolean(entry.email) })),
        }));
    }
    return PING_KIND_CATALOG_FALLBACK;
  }

  function pingKindLabel(kind, session = state.session) {
    const key = String(kind || "");
    for (const group of pingKindCatalog(session)) {
      const found = group.kinds.find((entry) => entry.kind === key);
      if (found) return found.label;
    }
    return "";
  }

  function pingMutedKinds(session = state.session) {
    return new Set(Array.isArray(session?.pingMutedKinds) ? session.pingMutedKinds.map(String) : []);
  }

  function pingBrowserTimezone() {
    try {
      return Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC";
    } catch (_) {
      return "UTC";
    }
  }

  function pingQuietHours(session = state.session) {
    const raw = session?.notificationQuietHours && typeof session.notificationQuietHours === "object"
      ? session.notificationQuietHours
      : {};
    const minute = (value, fallback) => {
      const n = Number(value);
      return Number.isInteger(n) && n >= 0 && n < 1440 ? n : fallback;
    };
    const days = Array.isArray(raw.days)
      ? [...new Set(raw.days.map(Number).filter((day) => Number.isInteger(day) && day >= 0 && day <= 6))].sort()
      : PING_QUIET_DEFAULTS.days.slice();
    let timezone = String(raw.timezone || "");
    // The Worker falls back to UTC for an account that never picked a zone.
    // Until quiet hours are switched on, show (and on the first save, send)
    // the zone this browser is in instead, which is what almost everyone wants.
    if (!raw.enabled && (!timezone || timezone === "UTC")) timezone = pingBrowserTimezone();
    return {
      enabled: Boolean(raw.enabled),
      start: minute(raw.start, PING_QUIET_DEFAULTS.start),
      end: minute(raw.end, PING_QUIET_DEFAULTS.end),
      days,
      timezone: timezone || pingBrowserTimezone(),
    };
  }

  function pingMinutesToTime(minutes) {
    const n = Math.max(0, Math.min(1439, Number(minutes) || 0));
    return `${String(Math.floor(n / 60)).padStart(2, "0")}:${String(n % 60).padStart(2, "0")}`;
  }

  function pingTimeToMinutes(value) {
    const match = /^(\d{1,2}):(\d{2})/.exec(String(value || ""));
    if (!match) return null;
    const minutes = Number(match[1]) * 60 + Number(match[2]);
    return minutes >= 0 && minutes < 1440 ? minutes : null;
  }

  function pingQuietDaysSummary(days) {
    if (days.length === 7) return "every day";
    const key = days.join(",");
    if (key === "0,1,2,3,4") return "on weekdays";
    if (key === "5,6") return "on weekends";
    return `on ${days.map((day) => PING_QUIET_DAY_NAMES[day]).join(", ")}`;
  }

  function pingQuietStateLabel(session = state.session) {
    const quiet = pingQuietHours(session);
    const until = Number(session?.notificationQuietUntil) || 0;
    if (quiet.enabled && until > Date.now()) {
      const time = new Date(until).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
      return `Quiet until ${time}`;
    }
    if (!quiet.enabled) return "Quiet hours are off";
    if (!quiet.days.length) return "Quiet hours are on, but no days are picked";
    return `Quiet ${pingMinutesToTime(quiet.start)}–${pingMinutesToTime(quiet.end)} ${pingQuietDaysSummary(quiet.days)} (${quiet.timezone})`;
  }

  function pingSwitchHtml(attrs, checked, { disabled = false, label = "" } = {}) {
    return `<button type="button" role="switch" ${attrs} aria-checked="${checked ? "true" : "false"}" aria-label="${escapeHtml(label)}" class="ping-switch"${disabled ? " disabled" : ""}></button>`;
  }

  function setPingSwitch(el, checked, disabled = false) {
    if (!el) return;
    el.setAttribute("aria-checked", checked ? "true" : "false");
    el.disabled = Boolean(disabled);
  }

  function renderPingKinds(session = state.session) {
    const list = $("[data-ping-kind-list]");
    if (!list) return;
    const muted = pingMutedKinds(session);
    const prefs = normalizedNotificationPreferences(session);
    const emailOn = session?.emailNotifications !== false;
    list.innerHTML = pingKindCatalog(session).map((group) => `
      <div data-ping-kind-group="${escapeHtml(group.key)}" class="border-t border-border">
        <div class="ping-kind-head bg-secondary/40">
          <span>${escapeHtml(group.label)}</span>
          <span class="ping-cell">Ping</span>
          <span class="ping-cell">Email</span>
        </div>
        ${group.kinds.map((entry) => {
          const on = !muted.has(entry.kind);
          const kind = escapeHtml(entry.kind);
          const label = escapeHtml(entry.label);
          return `
            <div id="ping-kind-${kind}" data-ping-kind-row="${kind}" data-ping-muted="${on ? "false" : "true"}" class="ping-kind-row">
              <span class="ping-kind-label min-w-0 truncate text-sm text-foreground">${label}</span>
              <span class="ping-cell">${pingSwitchHtml(`data-ping-kind-toggle="${kind}"`, on, { label: `${entry.label} pings` })}</span>
              <span class="ping-cell">${entry.email
                ? pingSwitchHtml(`data-notification-pref="${kind}"`, on && emailOn && Boolean(prefs[entry.kind]), {
                  disabled: !on || !emailOn,
                  label: `Email ${entry.label}`,
                })
                : '<span class="text-xs text-muted-foreground" title="Not emailed">—</span>'}</span>
            </div>
          `;
        }).join("")}
      </div>
    `).join("");
  }

  function renderPingQuietHours(session = state.session) {
    const quiet = pingQuietHours(session);
    setPingSwitch($("[data-ping-quiet-enabled]"), quiet.enabled);
    $$("[data-ping-quiet-state]").forEach((el) => { el.textContent = pingQuietStateLabel(session); });
    const start = $("[data-ping-quiet-start]");
    if (start && document.activeElement !== start) start.value = pingMinutesToTime(quiet.start);
    const end = $("[data-ping-quiet-end]");
    if (end && document.activeElement !== end) end.value = pingMinutesToTime(quiet.end);
    $$("[data-ping-quiet-day]").forEach((button) => {
      button.setAttribute("aria-pressed", quiet.days.includes(Number(button.dataset.pingQuietDay)) ? "true" : "false");
    });
    const zone = $("[data-ping-quiet-timezone]");
    if (zone && document.activeElement !== zone) {
      if (!zone.options.length || !Array.from(zone.options).some((option) => option.value === quiet.timezone)) {
        let zones = [];
        try { zones = typeof Intl.supportedValuesOf === "function" ? Intl.supportedValuesOf("timeZone") : []; } catch (_) {}
        const wanted = new Set([...zones, "UTC", pingBrowserTimezone(), quiet.timezone].filter(Boolean));
        zone.innerHTML = Array.from(wanted).sort().map((name) => (
          `<option value="${escapeHtml(name)}">${escapeHtml(name.replace(/_/g, " "))}</option>`
        )).join("");
      }
      zone.value = quiet.timezone;
    }
  }

  function renderMarketingEmail(session = state.session) {
    const on = Boolean(session?.marketingEmail);
    setPingSwitch($("[data-marketing-email]"), on);
    const since = $("[data-marketing-email-since]");
    if (since) {
      const at = Number(session?.marketingEmailSince) || 0;
      since.textContent = on && at ? `On since ${new Date(at).toLocaleDateString()}` : "";
    }
  }

  function renderPingEmail(session = state.session) {
    const emailOn = session?.emailNotifications !== false;
    const prefs = normalizedNotificationPreferences(session);
    setPingSwitch($("[data-ping-email-master]"), emailOn);
    setPingSwitch($('[data-ping-email] [data-notification-pref="general_chat"]'), emailOn && prefs.general_chat, !emailOn);
    $$("[data-notification-delivery-mode]").forEach((input) => { input.disabled = !emailOn; });
    $("[data-notification-delivery]")?.classList.toggle("opacity-60", !emailOn);
  }

  function renderPingAdminAlerts() {
    const card = $("[data-ping-admin-alerts]");
    const body = $("[data-ping-admin-alert-rows]");
    const rows = Array.isArray(state.pingAdminAlerts) ? state.pingAdminAlerts : null;
    if (!card || !body) return;
    card.hidden = !rows;
    if (!rows) return;
    body.innerHTML = rows.map((row) => {
      const id = escapeHtml(row.id || "");
      const label = String(row.label || row.id || "");
      const cell = (channel, title) => `<td>${pingSwitchHtml(
        `data-ping-monitor-alert="${id}" data-ping-monitor-channel="${channel}"`,
        Boolean(row[channel]),
        { label: `${label}: ${title}` },
      )}</td>`;
      return `<tr><td class="text-foreground">${escapeHtml(label)}</td>${cell("pings", "Ping")}${cell("emails", "Email")}${cell("continual", "Continual ping")}</tr>`;
    }).join("");
  }

  // The kind rows and the admin table are rebuilt on every render, so keep
  // keyboard focus on the switch that was just flipped and keep a deep-link
  // highlight alive across the rebuild.
  function pingFocusSelector(el) {
    if (!(el instanceof Element) || !el.closest("[data-ping-settings]")) return "";
    for (const attr of ["data-ping-kind-toggle", "data-notification-pref"]) {
      if (el.hasAttribute(attr)) return `[${attr}="${CSS.escape(el.getAttribute(attr) || "")}"]`;
    }
    if (el.hasAttribute("data-ping-monitor-alert")) {
      return `[data-ping-monitor-alert="${CSS.escape(el.getAttribute("data-ping-monitor-alert") || "")}"][data-ping-monitor-channel="${CSS.escape(el.getAttribute("data-ping-monitor-channel") || "")}"]`;
    }
    return "";
  }

  function applyPingHighlight() {
    const mark = state.pingHighlight;
    if (!mark || mark.until <= Date.now()) return;
    const target = document.getElementById(mark.id);
    if (!target) return;
    target.classList.add("ping-highlight");
    window.setTimeout(() => target.classList.remove("ping-highlight"), Math.max(0, mark.until - Date.now()));
  }

  function renderPingSettings(session = state.session) {
    if (!$("[data-ping-settings]")) return;
    const focusSelector = pingFocusSelector(document.activeElement);
    renderPingKinds(session);
    renderPingQuietHours(session);
    renderPingEmail(session);
    renderMarketingEmail(session);
    renderNotificationDelivery(session);
    renderPingPause(session);
    renderPingAdminAlerts();
    if (focusSelector) $(`[data-ping-settings] ${focusSelector}`)?.focus();
    applyPingHighlight();
  }

  function setPingSettingsStatus(text, tone = "") {
    const el = $("[data-ping-settings-status]");
    if (!el) return;
    el.textContent = text;
    el.dataset.tone = tone;
    window.clearTimeout(state.pingSettingsStatusTimer);
    if (tone === "good") {
      state.pingSettingsStatusTimer = window.setTimeout(() => {
        if (el.textContent === text) el.textContent = "";
      }, 2500);
    }
  }

  const PING_SETTINGS_FIELDS = [
    "emailNotifications",
    "marketingEmail",
    "marketingEmailSince",
    "notificationPreferences",
    "notificationDelivery",
    "notificationPauseUntil",
    "pingMutedKinds",
    "notificationQuietHours",
    "notificationQuietUntil",
    "pingKindCatalog",
  ];

  // Fold a settings payload (GET ?settings=1 or a POST reply) into the session
  // so the header bell and every other surface read the same values.
  function applyPingSettingsPayload(reply) {
    if (!reply || typeof reply !== "object") return;
    const next = { ...(state.session || {}) };
    PING_SETTINGS_FIELDS.forEach((field) => {
      if (Object.prototype.hasOwnProperty.call(reply, field)) next[field] = reply[field];
    });
    writeSession(next);
    if (Object.prototype.hasOwnProperty.call(reply, "statusMonitorAlerts")) {
      state.pingAdminAlerts = Array.isArray(reply.statusMonitorAlerts) ? reply.statusMonitorAlerts : null;
    }
  }

  // Optimistic local merge of a pingSettings patch, so a flipped switch moves
  // at once; the reply (or a revert on failure) is what sticks.
  function mergePingSettingsPatch(session, patch) {
    const next = { ...(session || {}) };
    if ("emailNotifications" in patch) next.emailNotifications = Boolean(patch.emailNotifications);
    if ("marketingEmail" in patch) next.marketingEmail = Boolean(patch.marketingEmail);
    if (patch.notificationPreferences) {
      next.notificationPreferences = { ...(next.notificationPreferences || {}), ...patch.notificationPreferences };
    }
    if (patch.notificationDelivery) next.notificationDelivery = patch.notificationDelivery;
    if (patch.pingMutedKinds && typeof patch.pingMutedKinds === "object") {
      const muted = pingMutedKinds(next);
      Object.entries(patch.pingMutedKinds).forEach(([kind, enabled]) => {
        if (enabled) muted.delete(kind); else muted.add(kind);
      });
      next.pingMutedKinds = Array.from(muted).sort();
    }
    if (patch.notificationQuietHours) {
      next.notificationQuietHours = { ...pingQuietHours(next), ...patch.notificationQuietHours };
    }
    return next;
  }

  async function savePingSettings(patch) {
    const node = state.session?.nodeName || "";
    if (!node || !patch || typeof patch !== "object") return false;
    const previousSession = state.session;
    const previousAdmin = state.pingAdminAlerts;
    const seq = (state.pingSettingsSaveSeq = (Number(state.pingSettingsSaveSeq) || 0) + 1);
    state.session = mergePingSettingsPatch(state.session, patch);
    if (patch.statusMonitorAlerts && Array.isArray(state.pingAdminAlerts)) {
      state.pingAdminAlerts = state.pingAdminAlerts.map((row) => (
        patch.statusMonitorAlerts[row.id] ? { ...row, ...patch.statusMonitorAlerts[row.id] } : row
      ));
    }
    renderPingSettings();
    setPingSettingsStatus("Saving…");
    try {
      const response = await fetch("/api/notifications", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          node,
          sessionToken: state.session?.sessionToken || "",
          pingSettings: patch,
        }),
      });
      const reply = await response.json().catch(() => ({}));
      if (!response.ok || !reply?.ok) {
        throw new Error(String(reply?.error || `HTTP ${response.status}`));
      }
      // A later switch already went out; its reply is the newer truth.
      if (seq === state.pingSettingsSaveSeq) {
        applyPingSettingsPayload(reply);
        renderPingSettings();
        setPingSettingsStatus("Saved", "good");
      }
      return true;
    } catch (error) {
      if (seq === state.pingSettingsSaveSeq) {
        state.session = previousSession;
        state.pingAdminAlerts = previousAdmin;
        renderPingSettings();
        const reason = String(error?.message || "");
        setPingSettingsStatus(
          reason === "unauthorized"
            ? "Sign in again to change ping settings."
            : reason === "forbidden"
              ? "Only administrators can change administrator alerts."
              : "Could not save. Check your connection and try again.",
          "bad",
        );
      }
      return false;
    }
  }

  async function loadPingSettings() {
    const node = state.session?.nodeName || "";
    // Paint from the session first; the account payload already carries every
    // field except the admin matrix.
    renderPingSettings();
    focusPingSettingsTarget();
    if (!node) return;
    try {
      const reply = await fetchJson(`/api/notifications?node=${encodeURIComponent(node)}&settings=1`, { fresh: true });
      if (reply?.ok) {
        applyPingSettingsPayload(reply);
        if (!Object.prototype.hasOwnProperty.call(reply, "statusMonitorAlerts")) state.pingAdminAlerts = null;
        renderPingSettings();
        focusPingSettingsTarget();
      }
    } catch (_) {}
  }

  // /dashboard/settings/notifications#ping-kind-mention (from a ping's gear in
  // the bell or the pings modal) scrolls to that row and flashes it.
  function focusPingSettingsTarget(hash = location.hash) {
    const id = decodeURIComponent(String(hash || "").replace(/^#/, ""));
    if (!/^(ping-kind-[a-z0-9_-]+|admin-alerts|ping-quiet-hours|ping-kinds|ping-email)$/.test(id)) return;
    const target = document.getElementById(id);
    if (!target || target.hidden || target.closest(".hidden")) return;
    if (state.pingSettingsFocused === `${id}@${state.pingSettingsFocusNonce || 0}`) return;
    state.pingSettingsFocused = `${id}@${state.pingSettingsFocusNonce || 0}`;
    target.scrollIntoView({ block: "center", behavior: "smooth" });
    state.pingHighlight = { id, until: Date.now() + 2200 };
    applyPingHighlight();
  }

  // Ping settings links go to /dashboard/settings/notifications[#ping-kind-X].
  // On the settings page itself that is an in-place tab switch, not a reload.
  function openPingSettingsInPlace(link) {
    if (currentPage() !== "settings") return false;
    const kind = String(link?.dataset?.pingSettingsLink || "");
    const hash = kind ? `#ping-kind-${kind}` : "";
    setSettingsSection("notifications", { scroll: !hash, push: true });
    try { history.replaceState(history.state, "", `/dashboard/settings/notifications${hash}`); } catch (_) {}
    state.pingSettingsFocusNonce = (Number(state.pingSettingsFocusNonce) || 0) + 1;
    focusPingSettingsTarget(hash);
    return true;
  }

  function pingQuietPatch(extra) {
    const zone = $("[data-ping-quiet-timezone]")?.value || pingQuietHours().timezone;
    return { notificationQuietHours: { ...extra, timezone: zone } };
  }

  // Delegated from the document click handler; true when it handled the click.
  function handlePingSettingsClick(event) {
    const root = event.target.closest("[data-ping-settings]");
    if (!root) return false;
    const kindToggle = event.target.closest("[data-ping-kind-toggle]");
    if (kindToggle) {
      const kind = kindToggle.dataset.pingKindToggle || "";
      const enabled = kindToggle.getAttribute("aria-checked") !== "true";
      void savePingSettings({ pingMutedKinds: { [kind]: enabled } });
      return true;
    }
    const emailPref = event.target.closest("button[data-notification-pref]");
    if (emailPref) {
      if (emailPref.disabled) return true;
      const kind = emailPref.dataset.notificationPref || "";
      void savePingSettings({ notificationPreferences: { [kind]: emailPref.getAttribute("aria-checked") !== "true" } });
      return true;
    }
    const marketing = event.target.closest("[data-marketing-email]");
    if (marketing) {
      void savePingSettings({ marketingEmail: marketing.getAttribute("aria-checked") !== "true" });
      return true;
    }
    const master = event.target.closest("[data-ping-email-master]");
    if (master) {
      void savePingSettings({ emailNotifications: master.getAttribute("aria-checked") !== "true" });
      return true;
    }
    const quietToggle = event.target.closest("[data-ping-quiet-enabled]");
    if (quietToggle) {
      const enabled = quietToggle.getAttribute("aria-checked") !== "true";
      const patch = pingQuietPatch({ enabled });
      // Switching on with no day picked would store "off" again; pick all.
      if (enabled && !pingQuietHours().days.length) patch.notificationQuietHours.days = PING_QUIET_DEFAULTS.days.slice();
      void savePingSettings(patch);
      return true;
    }
    const dayButton = event.target.closest("[data-ping-quiet-day]");
    if (dayButton) {
      const day = Number(dayButton.dataset.pingQuietDay);
      const days = new Set(pingQuietHours().days);
      if (days.has(day)) days.delete(day); else days.add(day);
      void savePingSettings(pingQuietPatch({ days: Array.from(days).sort() }));
      return true;
    }
    const monitorSwitch = event.target.closest("[data-ping-monitor-alert]");
    if (monitorSwitch) {
      const id = monitorSwitch.dataset.pingMonitorAlert || "";
      const channel = monitorSwitch.dataset.pingMonitorChannel || "";
      if (!id || !["pings", "emails", "continual"].includes(channel)) return true;
      void savePingSettings({
        statusMonitorAlerts: { [id]: { [channel]: monitorSwitch.getAttribute("aria-checked") !== "true" } },
      });
      return true;
    }
    return false;
  }

  document.addEventListener("change", (event) => {
    if (!(event.target instanceof Element) || !event.target.closest("[data-ping-settings]")) return;
    if (event.target.matches("[data-notification-delivery-mode]")) {
      void savePingSettings({ notificationDelivery: collectNotificationDelivery() });
      return;
    }
    if (event.target.matches("[data-ping-quiet-start], [data-ping-quiet-end]")) {
      const minutes = pingTimeToMinutes(event.target.value);
      if (minutes === null) return;
      const field = event.target.matches("[data-ping-quiet-start]") ? "start" : "end";
      void savePingSettings(pingQuietPatch({ [field]: minutes }));
      return;
    }
    if (event.target.matches("[data-ping-quiet-timezone]")) {
      void savePingSettings(pingQuietPatch({}));
    }
  });

  // Pause notifications: a snooze deadline rather than a mode, so it expires
  // on its own and one stored instant reads the same on every surface — this
  // page, the header bell, and the desktop app over its signed heartbeat.
  // Pings still land in the inbox and bell; their emails and visible desktop
  // notifications stay quiet until the deadline.
  const PING_PAUSE_HOUR_MS = 60 * 60 * 1000;
  const PING_PAUSE_PRESETS = [
    { key: "hour", label: "For 1 hour", ms: PING_PAUSE_HOUR_MS },
    { key: "day", label: "For 1 day", ms: 24 * PING_PAUSE_HOUR_MS },
    { key: "week", label: "Until next week", ms: 7 * 24 * PING_PAUSE_HOUR_MS },
  ];
  // Mirrors PING_PAUSE_MAX_MS in the Worker, which clamps anything longer. The
  // check here is only so the custom field can say why before a round trip.
  const PING_PAUSE_MAX_MS = 30 * 24 * PING_PAUSE_HOUR_MS;

  function pingPauseUntil(session = state.session) {
    const until = Number(session?.notificationPauseUntil) || 0;
    // A deadline already in the past is not a pause: it is a pause that ended,
    // and every surface renders that the same as never having set one.
    return until > Date.now() ? until : 0;
  }

  function pingPauseLabel(until) {
    const deadline = new Date(until);
    const sameDay = deadline.toDateString() === new Date().toDateString();
    const time = deadline.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
    if (sameDay) return `Notifications paused until ${time}`;
    const day = deadline.toLocaleDateString([], { weekday: "short", month: "short", day: "numeric" });
    return `Notifications paused until ${day}, ${time}`;
  }

  // datetime-local wants a local-clock "YYYY-MM-DDTHH:mm", not an ISO instant.
  function pingPauseInputValue(ms) {
    const at = new Date(ms - new Date(ms).getTimezoneOffset() * 60 * 1000);
    return at.toISOString().slice(0, 16);
  }

  function renderPingPause(session = state.session) {
    const until = pingPauseUntil(session);
    const label = until ? pingPauseLabel(until) : "Notifications are on";
    $$("[data-ping-pause-state]").forEach((el) => { el.textContent = label; });
    $$("[data-ping-pause-resume]").forEach((el) => { el.hidden = !until; });
    $$("[data-ping-pause-preset]").forEach((el) => {
      el.classList.toggle("text-accent", !until);
    });
    // Two icons rather than a swapped data-lucide attribute: lucide replaces
    // the <i> with an <svg> on first render, so the attribute is long gone.
    $$("[data-notification-bell-on]").forEach((el) => { el.classList.toggle("hidden", Boolean(until)); });
    $$("[data-notification-bell-off]").forEach((el) => { el.classList.toggle("hidden", !until); });
    const toggle = $("#notificationToggle");
    if (toggle) toggle.title = until ? label : "Ping notifications";
    // The status line right above already says how long, so while a pause runs
    // this only has to offer the way to change it.
    $$("[data-ping-pause-toggle]").forEach((el) => {
      el.textContent = until ? "Change notification pause" : "Pause notifications";
    });
    $$("[data-ping-pause-custom]").forEach((input) => {
      if (document.activeElement === input) return;
      input.min = pingPauseInputValue(Date.now() + 60 * 1000);
      input.max = pingPauseInputValue(Date.now() + PING_PAUSE_MAX_MS);
      input.value = until ? pingPauseInputValue(until) : "";
    });
  }

  function setPingPauseMenuOpen(open) {
    $$("[data-ping-pause-menu]").forEach((el) => { el.hidden = !open; });
  }

  async function savePingPause(until) {
    const node = state.session?.nodeName || "";
    if (!node) return;
    const requested = Math.max(0, Math.round(Number(until) || 0));
    // Optimistic: the deadline is this client's own clock either way, and the
    // response carries the value the relay actually stored (clamped, or 0 for
    // a resume), so the reconcile below is what sticks.
    const previous = Number(state.session?.notificationPauseUntil) || 0;
    state.session = { ...(state.session || {}), notificationPauseUntil: requested };
    renderPingPause();
    setPingPauseMenuOpen(false);
    // A failed save changed nothing on the relay, so fall back to what was
    // there before rather than to "on" — otherwise a dropped request would
    // quietly report a running pause as ended.
    let stored = previous;
    try {
      const response = await fetch("/api/notifications", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          node,
          pauseUntil: requested,
          sessionToken: state.session?.sessionToken || "",
        }),
      });
      const reply = await response.json().catch(() => ({}));
      if (response.ok) stored = Number(reply?.pauseUntil) || 0;
    } catch (_) {}
    writeSession({ ...(state.session || {}), notificationPauseUntil: stored });
    renderPingPause();
  }

  function setRenameStatus(text, cls) {
    setProfilePageHint("[data-profile-rename-status]", text, cls);
  }

  function updateRenameButton() {
    const button = $("[data-profile-rename-save]");
    if (!button) return;
    button.disabled = !(
      state.nodeNameAvailability.available &&
      Boolean(state.session?.emailVerified) &&
      Boolean(profilePassword("[data-profile-rename-password]"))
    );
    button.classList.toggle("opacity-40", button.disabled);
  }

  function renderProfileModal(session) {
    const emailStatus = $("[data-profile-email-status]");
    const verifyButton = $("[data-profile-verify-email]");
    const solanaInput = $("[data-profile-solana]");
    if (emailStatus) {
      emailStatus.textContent = session?.emailVerified
        ? `${session.email || "Email"} is verified.`
        : `${session?.email || "Your email"} is not verified yet.`;
    }
    if (verifyButton) {
      verifyButton.disabled = Boolean(session?.emailVerified);
      verifyButton.classList.toggle("opacity-40", Boolean(session?.emailVerified));
      verifyButton.textContent = session?.emailVerified ? "Verified" : "Send link";
    }
    if (solanaInput && document.activeElement !== solanaInput) {
      solanaInput.value = session?.solana || "";
    }
  }

  function profileSidebarMarkup(session) {
    const template = $("[data-profile-sidebar-template]");
    return template ? template.innerHTML.trim() : "";
  }

  function renderProfileSidebars(session) {
    const markup = profileSidebarMarkup(session);
    $$("[data-profile-sidebar-slot]").forEach((slot) => {
      if (slot.dataset.profileSidebarRendered === "true" && slot.dataset.profileSidebarMarkup === markup) return;
      slot.innerHTML = markup;
      slot.dataset.profileSidebarRendered = "true";
      slot.dataset.profileSidebarMarkup = markup;
    });
  }

  function renderProfileLocalTimes(session = profileSubject()) {
    const localTimeEls = $$("[data-profile-local-time]");
    if (!localTimeEls.length) return;
    const profileTimezone = session?.profileTimezone || "";
    let localTime = profileTimezone || "No time zone";
    try {
      localTime = new Intl.DateTimeFormat([], {
        hour: "2-digit",
        minute: "2-digit",
        hour12: false,
        ...(profileTimezone ? { timeZone: profileTimezone } : {}),
        timeZoneName: "shortOffset",
      }).format(new Date());
    } catch {
      // Keep the timezone name visible when an older or malformed profile has
      // a timezone that this browser does not recognize.
    }
    localTimeEls.forEach((localTimeEl) => {
      localTimeEl.textContent = localTime;
    });
  }

  function renderProfileMembership(session = profileSubject()) {
    const joined = Number(session?.createdAt) || 0;
    const membership = $$('[data-profile-member-for]');
    if (!membership.length) return;
    if (!joined) {
      membership.forEach((element) => {
        element.textContent = "Member since join date unavailable";
      });
      return;
    }
    const elapsedDays = Math.max(
      0, Math.floor((Date.now() - joined) / (24 * 60 * 60 * 1000)));
    const joinedLabel = new Intl.DateTimeFormat(undefined, {
      year: "numeric", month: "short", day: "numeric",
    }).format(new Date(joined));
    const text = `Member for ${formatCount(elapsedDays)} ${elapsedDays === 1 ? "day" : "days"} · Joined ${joinedLabel}`;
    membership.forEach((element) => { element.textContent = text; });
  }

  // The Public email dropdown offers the addresses this account has confirmed
  // — today that is the single account address — plus the "don't show" default.
  // An unconfirmed address leaves the control disabled: ForkMesh only ever
  // offers to publish an address the account has proven it controls.
  const PROFILE_PUBLIC_EMAIL_READY_CLASS =
    "h-10 w-fit max-w-full cursor-pointer rounded-md border border-border bg-card px-3 text-sm font-normal text-foreground focus:outline-none focus:ring-1 focus:ring-primary";
  const PROFILE_PUBLIC_EMAIL_LOCKED_CLASS =
    "h-10 w-fit max-w-full cursor-not-allowed rounded-md border border-border bg-secondary px-3 text-sm font-normal text-muted-foreground";

  function renderProfilePublicEmail(session) {
    const select = $("[data-profile-public-email]");
    if (!select) return;
    const email = String(session?.email || "").trim();
    // /@name pages swap the address out for the handle, so require a real
    // mailbox before treating the subject as publishable.
    const verified = Boolean(session?.emailVerified) && email.includes("@");
    const options = verified
      ? [["", "Don't show my email"], [email, email]]
      : [["", "Select a verified email to display"]];
    const markup = options
      .map(([value, label]) =>
        `<option value="${escapeHtml(value)}">${escapeHtml(label)}</option>`)
      .join("");
    if (select.dataset.profilePublicEmailMarkup !== markup) {
      select.innerHTML = markup;
      select.dataset.profilePublicEmailMarkup = markup;
    }
    select.disabled = !verified;
    select.className = verified
      ? PROFILE_PUBLIC_EMAIL_READY_CLASS
      : PROFILE_PUBLIC_EMAIL_LOCKED_CLASS;
    select.title = verified
      ? ""
      : "Confirm your email address under Verification and payout to display it here.";
    if (document.activeElement === select) return;
    const chosen = String(session?.publicEmail || "").trim();
    select.value = verified && chosen.toLowerCase() === email.toLowerCase()
      ? email
      : "";
  }

  // The published address rides along on the public profile payload, so the
  // same row serves the owner's own page and a visitor's /@name view.
  function renderProfilePublicEmailRow(session) {
    const rows = $$("[data-profile-email-row]");
    if (!rows.length) return;
    const publicEmail = String(session?.publicEmail || "").trim();
    rows.forEach((row) => {
      row.hidden = !publicEmail;
      const link = row.querySelector("[data-profile-email-text]");
      if (!link) return;
      link.textContent = publicEmail;
      link.href = publicEmail ? `mailto:${publicEmail}` : "#";
    });
  }

  const PROFILE_LOCAL_TIME_UPDATE_INTERVAL_MS = 60 * 1000;
  window.setInterval(() => renderProfileLocalTimes(), PROFILE_LOCAL_TIME_UPDATE_INTERVAL_MS);

  // The profile subject's away flag and custom status, on a line under the
  // name ("🌴 Vacationing · until Mon, Sep 21 9:00 AM · Away").
  function renderProfileUserStatus(nameEls, subject) {
    const userStatus = window.ForkMeshUserStatus;
    if (userStatus && subject?.nodeName && subject.userStatus) {
      userStatus.remember(subject.nodeName, subject.userStatus);
    }
    const label = userStatus && subject?.nodeName
      ? userStatus.describe(userStatus.forName(subject.nodeName))
      : "";
    nameEls.forEach((nameEl) => {
      let line = nameEl.nextElementSibling;
      if (!line?.hasAttribute("data-profile-user-status")) {
        if (!label) return;
        line = document.createElement("p");
        line.setAttribute("data-profile-user-status", "");
        line.className = "text-xs text-muted-foreground";
        nameEl.after(line);
      }
      line.textContent = label;
      line.hidden = !label;
    });
  }

  function renderProfilePage(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const name = session?.nodeName || "My Profile";
    const email = session?.email || "No email on file";
    renderProfileSidebars(session);
    const avatars = $$('[data-profile-page-avatar]');
    const nameEls = $$('[data-profile-page-node-name]');
    const emailEls = $$('[data-profile-page-email]');
    const bioEls = $$("[data-profile-bio]");
    const followersEls = $$("[data-profile-followers-count]");
    const followingEls = $$("[data-profile-following-count]");
    const mirrorsEls = $$("[data-profile-mirrors-count]");
    const locationEls = $$("[data-profile-location-text]");
    const websiteEls = $$("[data-profile-website]");
    const accountStatus = $("[data-profile-page-account-status]");
    const payoutStatus = $("[data-profile-page-payout-status]");
    const adminStatus = $("[data-profile-page-admin-status]");
    const emailStatus = $("[data-profile-page-email-status]");
    const verifyButton = $("[data-profile-page-verify-email]");
    const solanaInput = $("[data-profile-page-solana]");
    const renameInput = $("[data-profile-rename-input]");
    const bioInput = $("[data-profile-page-bio]");
    const locationInput = $("[data-profile-page-location]");
    const nameCaseInput = $("[data-profile-page-name-case]");
    const timezoneInput = $("[data-profile-page-timezone]");
    const mastodonInput = $("[data-profile-page-mastodon]");
    const privateInput = $("[data-profile-page-private]");
    const followersPublicInput = $("[data-profile-page-followers-public]");
    const publicUrl = $("[data-profile-public-url]");
    const txtValue = $("[data-profile-txt-value]");

    const isNode = session?.kind === "node";
    avatars.forEach((avatar) => applyAvatar(avatar, session));
    nameEls.forEach((nameEl) => {
      nameEl.textContent = session?.nodeName ? userDisplayName(name) : name;
    });
    renderProfileUserStatus(nameEls, session);
    emailEls.forEach((emailEl) => {
      emailEl.textContent = email;
    });
    $$("[data-profile-kind-badge]").forEach((badge) => {
      badge.classList.toggle("hidden", !isNode);
    });
    $$("[data-profile-node-status]").forEach((row) => {
      row.classList.toggle("hidden", !isNode);
      const text = row.querySelector("[data-profile-node-status-text]");
      if (text) text.textContent = session?.online ? "Online" : "Offline";
    });
    bioEls.forEach((bioEl) => {
      bioEl.textContent = session?.profileBio || "No bio yet.";
    });
    followersEls.forEach((followersEl) => {
      followersEl.textContent = String(Number(session?.profileFollowers ?? session?.followers ?? 0).toLocaleString());
    });
    followingEls.forEach((followingEl) => {
      followingEl.textContent = String(Number(session?.profileFollowing ?? session?.following ?? 0).toLocaleString());
    });
    mirrorsEls.forEach((mirrorsEl) => {
      mirrorsEl.textContent = String(Number(session?.profileMirrorCount ?? session?.mirrorCount ?? 0).toLocaleString());
    });
    locationEls.forEach((locationEl) => {
      locationEl.textContent = session?.profileLocation || "No location";
    });
    renderProfileLocalTimes(session);
    renderProfileMembership(session);
    renderProfilePublicEmail(session);
    renderProfilePublicEmailRow(session);
    websiteEls.forEach((websiteEl) => {
      const firstLink = Array.isArray(session?.profileLinks) ? session.profileLinks.find((link) => link?.url) : null;
      const url = firstLink?.url || profilePublicUrl(session);
      websiteEl.textContent = url;
      websiteEl.href = url;
    });
    if (accountStatus) accountStatus.textContent = session?.status || "active";
    if (payoutStatus) payoutStatus.textContent = session?.hasPayoutAddress ? "Configured" : "Not configured";
    if (adminStatus) adminStatus.textContent = session?.isAdmin ? "Yes" : "No";
    $("[data-admin-test-email-row]")?.classList.toggle("hidden", session?.isAdmin !== true);
    if (emailStatus) {
      emailStatus.textContent = session?.emailVerified
        ? `${email} is verified.`
        : `${email} is not verified yet.`;
    }
    if (verifyButton) {
      verifyButton.disabled = Boolean(session?.emailVerified);
      verifyButton.classList.toggle("opacity-40", Boolean(session?.emailVerified));
      verifyButton.textContent = session?.emailVerified ? "Verified" : "Send link";
    }
    if (solanaInput && document.activeElement !== solanaInput) {
      solanaInput.value = session?.solana || "";
    }
    if (renameInput) {
      if (document.activeElement !== renameInput) {
        renameInput.value = session?.nodeName || "";
      }
      renameInput.placeholder = name;
    }
    if (bioInput && document.activeElement !== bioInput) {
      bioInput.value = session?.profileBio || "";
    }
    if (locationInput && document.activeElement !== locationInput) {
      locationInput.value = session?.profileLocation || "";
    }
    if (nameCaseInput && document.activeElement !== nameCaseInput) {
      nameCaseInput.value = session?.nameCase || session?.nodeName || "";
      nameCaseInput.placeholder = session?.nodeName || "username";
    }
    renderProfileTimezoneOptions(session);
    if (timezoneInput && document.activeElement !== timezoneInput) {
      timezoneInput.value = session?.profileTimezone || "";
    }
    if (mastodonInput && document.activeElement !== mastodonInput) {
      mastodonInput.value = session?.mastodon || "";
    }
    if (privateInput) {
      privateInput.checked = Boolean(session?.profilePrivate);
    }
    if (followersPublicInput) {
      followersPublicInput.checked = Boolean(session?.followersPublic);
    }
    if (publicUrl) publicUrl.textContent = profilePublicUrl(session);
    if (txtValue) txtValue.textContent = profileTxtValue(session);
    renderProfileLinksEditor(session);
    renderPingSettings(session);
    renderPingPause(session);
    renderProfileReferrals(session);
    if (!session?.emailVerified) {
      state.nodeNameAvailability.available = false;
      setRenameStatus("Verify your email before changing your username.", "bad");
    } else if (!($("[data-profile-rename-input]")?.value || "").trim()) {
      state.nodeNameAvailability.available = false;
      setRenameStatus("Enter a new username to check availability.", "");
    }
    updateRenameButton();
    renderClaimNodePanel(session);
    renderProfileContributionGraph();
    renderProfileTips(session);
    renderProfileBadges(session);
    void renderProfileSecurityCredits(session);
    renderProfileFediverse(session);
  }

  const PROFILE_BADGES_CACHE_TTL_MS = 60 * 1000;
  const profileBadgesCache = new Map();
  const profileBadgesLoading = new Set();

  const PROFILE_BADGE_SLUG_RE = /^[a-z][a-z0-9_]{1,39}$/;
  const PROFILE_BADGE_TIERS = ["bronze", "silver", "gold"];

  function profileBadgeTier(badge) {
    const tier = String(badge?.tier || "").trim().toLowerCase();
    return PROFILE_BADGE_TIERS.includes(tier) ? tier : "";
  }

  function profileBadgeMedalSources(slug, tier) {
    const files = tier ? [slug + "-" + tier, slug] : [slug];
    const sources = [];
    files.forEach((file) => {
      const path = "/assets/badges/" + file + ".svg";
      try {
        const routed = window.ForkMeshAPI?.url?.(path);
        if (routed && !sources.includes(routed)) sources.push(routed);
      } catch (_) {}
      const local = new URL(path, location.href).href;
      if (!sources.includes(local)) sources.push(local);
    });
    return sources;
  }

  function profileBadgeTierText(badge) {
    const tier = String(badge?.tier || "");
    if (!tier || !Array.isArray(badge?.tiers)) return "";
    const unit = String(badge.unit || "");
    const progress = Number(badge.progress) || 0;
    const parts = [];
    if (progress > 0) parts.push(progress.toLocaleString() + (unit ? " " + unit : ""));
    if (badge.nextTier && Number(badge.nextThreshold) > 0) {
      parts.push(badge.nextTier.charAt(0).toUpperCase() + badge.nextTier.slice(1) + " at " + Number(badge.nextThreshold).toLocaleString());
    } else {
      parts.push("Highest level");
    }
    return parts.join(" \u00b7 ");
  }

  function profileBadgeMedalFallback(badge) {
    const fallback = document.createElement("span");
    fallback.className = "flex h-12 w-12 shrink-0 items-center justify-center rounded-full border-2 border-border bg-secondary text-base font-semibold text-muted-foreground";
    fallback.setAttribute("aria-hidden", "true");
    fallback.dataset.profileBadgeMedal = "";
    fallback.textContent = String(badge.name || badge.slug || "?").trim().charAt(0).toUpperCase();
    return fallback;
  }

  function profileBadgeMedal(badge) {
    const slug = String(badge.slug || "").trim().toLowerCase();
    if (!PROFILE_BADGE_SLUG_RE.test(slug)) return profileBadgeMedalFallback(badge);
    const sources = profileBadgeMedalSources(slug, profileBadgeTier(badge));
    const medal = document.createElement("img");
    medal.className = "h-12 w-12 shrink-0 select-none";
    medal.width = 48;
    medal.height = 48;
    medal.alt = "";
    medal.decoding = "async";
    medal.draggable = false;
    medal.dataset.profileBadgeMedal = slug;
    let attempt = 0;
    medal.addEventListener("error", () => {
      attempt += 1;
      if (attempt < sources.length) {
        medal.src = sources[attempt];
        return;
      }
      medal.replaceWith(profileBadgeMedalFallback(badge));
    });
    medal.src = sources[0];
    return medal;
  }

  function renderProfileBadgesData(name, badges) {
    $$("[data-profile-badges]").forEach((card) => {
      if (card.dataset.profileBadgesFor !== name) return;
      const list = card.querySelector("[data-profile-badges-list]");
      const count = card.querySelector("[data-profile-badges-count]");
      if (!badges.length || !list) {
        card.hidden = true;
        return;
      }
      if (count) count.textContent = String(badges.length);
      list.textContent = "";
      badges.forEach((badge) => {
        const item = document.createElement("li");
        item.className = "flex items-center gap-3";
        item.dataset.profileBadge = String(badge.slug || "");
        item.title = String(badge.description || "");
        const icon = profileBadgeMedal(badge);
        const text = document.createElement("div");
        text.className = "min-w-0";
        const title = document.createElement("p");
        title.className = "flex items-center gap-2 font-semibold leading-5 text-foreground";
        const titleText = document.createElement("span");
        titleText.className = "truncate";
        titleText.textContent = String(badge.name || badge.slug || "");
        title.append(titleText);
        const tier = profileBadgeTier(badge);
        if (tier) {
          const chip = document.createElement("span");
          chip.className = "shrink-0 rounded-full border border-border px-1.5 text-xs font-medium leading-4 text-muted-foreground";
          chip.dataset.profileBadgeTier = tier;
          chip.textContent = tier.charAt(0).toUpperCase() + tier.slice(1);
          title.append(chip);
        }
        const detail = document.createElement("p");
        detail.className = "text-xs leading-5 text-muted-foreground";
        detail.textContent = String(badge.description || "");
        text.append(title, detail);
        const tierText = profileBadgeTierText(badge);
        if (tierText) {
          const level = document.createElement("p");
          level.className = "text-xs leading-5 text-muted-foreground";
          level.dataset.profileBadgeProgress = "";
          level.textContent = tierText;
          text.append(level);
        }
        if (Number(badge.awardedAt) > 0) {
          const when = document.createElement("p");
          when.className = "text-xs leading-5 text-muted-foreground";
          when.textContent = "Earned " + formatDate(Number(badge.awardedAt));
          text.append(when);
        }
        item.append(icon, text);
        list.append(item);
      });
      card.hidden = false;
    });
  }

  function renderProfileBadges(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const name = String(session?.nodeName || "").trim().toLowerCase();
    $$("[data-profile-badges]").forEach((card) => {
      card.dataset.profileBadgesFor = name;
    });
    if (!name) {
      renderProfileBadgesData("", []);
      return;
    }
    const cached = profileBadgesCache.get(name);
    if (cached) renderProfileBadgesData(name, cached.badges);
    const fresh = cached && Date.now() - cached.at < PROFILE_BADGES_CACHE_TTL_MS;
    if (fresh || profileBadgesLoading.has(name)) return;
    profileBadgesLoading.add(name);
    fetchJson("/api/badges/" + encodeURIComponent(name))
      .then((body) => {
        const badges = Array.isArray(body?.badges) ? body.badges : [];
        profileBadgesCache.set(name, { at: Date.now(), badges });
        renderProfileBadgesData(name, badges);
      })
      .catch(() => {})
      .finally(() => profileBadgesLoading.delete(name));
  }

  // ---- Tips ---------------------------------------------------------------
  // A payout address is deliberately public profile data (the account lookup
  // returns it to anyone), so a visitor can tip the author wallet to wallet
  // with nothing in between: the card shows the address and a QR of it that a
  // phone wallet can scan. ForkMesh never holds the funds or the key. An
  // account that has published no address says so instead of showing an empty
  // box, and an organization - whose public record carries no payout field at
  // all - hides the card rather than claiming an address is missing.
  const PROFILE_TIP_ADDRESS_RE = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/;

  function profileTipAddress(session) {
    const address = String(session?.solana || "").trim();
    return PROFILE_TIP_ADDRESS_RE.test(address) ? address : "";
  }

  // The QR script arrives after an await, by which time the visitor may have
  // navigated to another profile. The holder's recorded value is the identity
  // check: paint only if it is still the address this call was asked for.
  async function drawProfileTipQr(holder, address) {
    const loaded = await loadQrGenerator();
    if (holder.dataset.profileTipsQrValue !== address) return;
    // The address printed above is the payable thing; the code only saves
    // typing it, so a failed load degrades to a note rather than an error.
    if (!loaded) {
      holder.textContent = "QR unavailable";
      return;
    }
    try {
      holder.replaceChildren(window.ForkMeshQR.toCanvas(address, 148));
    } catch (_) {
      holder.textContent = "QR unavailable";
    }
  }

  function renderProfileTips(session) {
    const cards = $$("[data-profile-tips]");
    if (!cards.length) return;
    const address = profileTipAddress(session);
    const name = String(session?.nodeName || "");
    const viewerIsSubject = Boolean(name) &&
      String(state.session?.nodeName || "").toLowerCase() === name.toLowerCase();
    cards.forEach((card) => {
      card.hidden = session?.kind === "organization";
      if (card.hidden) return;
      const empty = card.querySelector("[data-profile-tips-empty]");
      const detail = card.querySelector("[data-profile-tips-detail]");
      const code = card.querySelector("[data-profile-tips-address]");
      const holder = card.querySelector("[data-profile-tips-qr]");
      if (empty) {
        empty.hidden = Boolean(address);
        empty.textContent = viewerIsSubject
          ? "You have not entered a Solana address for tips."
          : `${name ? "@" + name : "This user"} has not entered a Solana address for tips.`;
      }
      if (detail) detail.hidden = !address;
      if (code) code.textContent = address;
      if (holder && holder.dataset.profileTipsQrValue !== address) {
        holder.dataset.profileTipsQrValue = address;
        holder.replaceChildren();
        if (address) void drawProfileTipQr(holder, address);
      }
      const copy = card.querySelector("[data-profile-tips-copy]");
      // renderProfileSidebars rebuilds the card from its template, so the flag
      // dies with the element it was set on: one listener per live button.
      if (copy && copy.dataset.profileTipsCopyBound !== "true") {
        copy.dataset.profileTipsCopyBound = "true";
        copy.addEventListener("click", async () => {
          const value = code?.textContent || "";
          if (!value) return;
          const copied = await copyTextToClipboard(value);
          copy.textContent = copied ? "Copied" : "Copy failed";
          setTimeout(() => { copy.textContent = "Copy address"; }, 1500);
        });
      }
    });
  }

  // ---- Fediverse presence ------------------------------------------------
  // A profile with a linked Mastodon handle surfaces that account here: the
  // account's header image becomes a banner across the overview page and the
  // newest public posts fill a sidebar card. The browser talks straight to
  // the user's home instance (public CORS API, credentials omitted) so none
  // of this spends the worker's request quota, and a 10-minute localStorage
  // snapshot keeps repeat visits from hammering small instances.
  const PROFILE_FEDIVERSE_CACHE_PREFIX = "forkmesh.profileFediverse:v1:";
  const PROFILE_FEDIVERSE_CACHE_TTL_MS = 10 * 60 * 1000;
  const PROFILE_FEDIVERSE_POST_LIMIT = 3;
  const profileFediverseLoading = new Set();

  function parseMastodonHandle(raw) {
    const match = /^@?([\w.-]{1,80})@([a-z0-9-]+(?:\.[a-z0-9-]+)+)$/i
      .exec(String(raw || "").trim());
    return match ? { user: match[1], domain: match[2].toLowerCase() } : null;
  }

  function fediverseFollowUrl(handle) {
    const home = parseMastodonHandle(state.session?.mastodon);
    return `https://${home?.domain || "mastodon.social"}/${handle}`;
  }

  function fediverseHttpsUrl(value) {
    try {
      const url = new URL(String(value || "").trim());
      return url.protocol === "https:" && !url.username && !url.password
        ? url.href : "";
    } catch {
      return "";
    }
  }

  // Mastodon serves statuses and notes as sanitized HTML; the card renders
  // plain text only. A detached textarea decodes entities without ever
  // constructing elements from the remote markup.
  function fediversePlainText(value, limit = 280) {
    const stripped = String(value ?? "")
      .replace(/<br\s*\/?>/gi, "\n")
      .replace(/<\/(?:p|div|blockquote|li)>/gi, "\n")
      .replace(/<[^>]*>/g, "");
    const decoder = document.createElement("textarea");
    decoder.innerHTML = stripped;
    const text = decoder.value
      .replace(/[\u0000-\u0008\u000b-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/g, " ")
      .replace(/[ \t]+/g, " ")
      .replace(/ ?\n ?/g, "\n")
      .replace(/\n{2,}/g, "\n")
      .trim();
    return text.length > limit ? text.slice(0, limit - 1).trimEnd() + "…" : text;
  }

  function profileFediverseCacheKey(handle) {
    return PROFILE_FEDIVERSE_CACHE_PREFIX + handle;
  }

  function readProfileFediverseCache(handle) {
    try {
      const data = JSON.parse(localStorage.getItem(profileFediverseCacheKey(handle)) || "");
      return data && typeof data === "object" && Array.isArray(data.posts) ? data : null;
    } catch {
      return null;
    }
  }

  function writeProfileFediverseCache(handle, data) {
    try {
      localStorage.setItem(profileFediverseCacheKey(handle), JSON.stringify(data));
    } catch {}
  }

  async function fetchFediverseJson(url) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 10000);
    try {
      const response = await fetch(url, {
        credentials: "omit",
        headers: { Accept: "application/json" },
        signal: controller.signal,
      });
      if (!response.ok) throw new Error("fediverse HTTP " + response.status);
      return await response.json();
    } finally {
      clearTimeout(timer);
    }
  }

  async function loadProfileFediverse(parsed) {
    const base = "https://" + parsed.domain;
    const account = await fetchFediverseJson(
      base + "/api/v1/accounts/lookup?acct=" + encodeURIComponent(parsed.user));
    const id = String(account?.id || "").trim().slice(0, 64);
    if (!id || account?.suspended) return null;
    let statuses = [];
    try {
      statuses = await fetchFediverseJson(
        base + "/api/v1/accounts/" + encodeURIComponent(id) +
        "/statuses?limit=10&exclude_replies=true");
    } catch {
      // The account still renders; the posts list just stays empty.
    }
    const posts = (Array.isArray(statuses) ? statuses : [])
      .map((status) => {
        const boost = status?.reblog && typeof status.reblog === "object"
          ? status.reblog : null;
        const source = boost || status || {};
        const imageCount = (Array.isArray(source.media_attachments)
          ? source.media_attachments : [])
          .filter((media) => String(media?.type || "") === "image").length;
        return {
          url: fediverseHttpsUrl(source.url || status?.url),
          text: fediversePlainText(source.content),
          imageCount,
          boosted: Boolean(boost),
          createdAt: String(source.created_at || status?.created_at || ""),
        };
      })
      .filter((post) => post.url && (post.text || post.imageCount))
      .slice(0, PROFILE_FEDIVERSE_POST_LIMIT);
    // Instances answer with a placeholder "missing.png" header when the
    // account never uploaded one — that is not a banner worth showing.
    const header = fediverseHttpsUrl(account.header_static || account.header);
    return {
      at: Date.now(),
      url: fediverseHttpsUrl(account.url) || base + "/@" + parsed.user,
      acct: String(account.acct || parsed.user).slice(0, 120),
      banner: /\/missing\.png$/i.test(header) ? "" : header,
      posts,
    };
  }

  function renderProfileFediverseData(handle, data) {
    const stamp = handle + ":" + String(data?.at || 0);
    const banner = $("[data-profile-fediverse-banner]");
    if (banner) {
      if (data?.banner) {
        banner.style.backgroundImage = 'url("' + data.banner + '")';
        banner.href = data.url;
        banner.hidden = false;
      } else {
        banner.hidden = true;
      }
    }
    $$("[data-profile-fediverse]").forEach((card) => {
      if (!data) {
        card.hidden = true;
        return;
      }
      if (card.dataset.fediverseStamp === stamp) {
        card.hidden = false;
        return;
      }
      card.dataset.fediverseStamp = stamp;
      const link = card.querySelector("[data-profile-fediverse-link]");
      if (link) {
        link.textContent = "@" + data.acct;
        link.href = data.url;
      }
      // Pops the feed out into its own window so it can sit beside the
      // dashboard instead of replacing the tab.
      const popout = card.querySelector("[data-profile-fediverse-popout]");
      if (popout) {
        popout.onclick = () => {
          window.open(
            data.url, "forkmesh-fediverse-feed",
            "noopener,width=520,height=860");
        };
      }
      const list = card.querySelector("[data-profile-fediverse-posts]");
      if (list) {
        list.textContent = "";
        data.posts.forEach((post, index) => {
          const item = document.createElement("a");
          item.href = post.url;
          item.target = "_blank";
          item.rel = "noopener noreferrer";
          item.className = "block px-3 py-2 hover:bg-secondary" +
            (index ? " border-t border-border" : "");
          const text = document.createElement("p");
          text.className = "whitespace-pre-wrap break-words leading-5 text-foreground";
          text.textContent = post.text ||
            (post.imageCount === 1 ? "Shared an image." : "Shared " + post.imageCount + " images.");
          const meta = document.createElement("p");
          meta.className = "mt-1 text-xs text-muted-foreground";
          meta.textContent = (post.boosted ? "Boosted · " : "") + formatTimeAgo(post.createdAt);
          item.append(text, meta);
          list.append(item);
        });
        if (!data.posts.length) {
          const empty = document.createElement("p");
          empty.className = "px-3 py-2 text-muted-foreground";
          empty.textContent = "No public posts yet.";
          list.append(empty);
        }
      }
      card.hidden = false;
      window.lucide?.createIcons();
    });
  }

  function renderProfileFediverse(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const parsed = parseMastodonHandle(session?.mastodon);
    if (!parsed) {
      renderProfileFediverseData("", null);
      return;
    }
    const handle = parsed.user + "@" + parsed.domain;
    const cached = readProfileFediverseCache(handle);
    if (cached) renderProfileFediverseData(handle, cached);
    const fresh = cached && Date.now() - Number(cached.at || 0) < PROFILE_FEDIVERSE_CACHE_TTL_MS;
    if (fresh || profileFediverseLoading.has(handle)) return;
    profileFediverseLoading.add(handle);
    loadProfileFediverse(parsed)
      .then((data) => {
        if (!data) return;
        writeProfileFediverseCache(handle, data);
        renderProfileFediverseData(handle, data);
      })
      .catch(() => {
        // Instance unreachable (CORS, rate limit, downtime): keep whatever
        // the stale snapshot already painted instead of flashing it away.
      })
      .finally(() => profileFediverseLoading.delete(handle));
  }

  function renderProfileLinksEditor(session = state.session) {
    const links = Array.isArray(session?.profileLinks) ? session.profileLinks : [];
    $$("[data-profile-link-row]").forEach((row, index) => {
      const link = links[index] || {};
      const label = row.querySelector("[data-profile-link-label]");
      const url = row.querySelector("[data-profile-link-url]");
      const status = row.querySelector("[data-profile-link-status]");
      if (label && document.activeElement !== label) label.value = link.label || "";
      if (url && document.activeElement !== url) url.value = link.url || "";
      if (status) {
        if (link.url) {
          status.textContent = link.verified
            ? `Verified for ${link.domain || "domain"}.`
            : `Add TXT ${link.txtValue || profileTxtValue(session)} on ${link.txtName || link.domain || "the domain"} to verify.`;
          status.className = "sm:col-span-2 text-[11px] text-muted-foreground";
        } else {
          status.textContent = "";
          status.className = "sm:col-span-2 text-[11px] text-muted-foreground";
        }
      }
    });
  }

  function profilePayload(extra = {}, passwordOverride) {
    const password = passwordOverride ?? ($("[data-profile-password]")?.value || "");
    return {
      nodeName: state.session?.nodeName || "",
      email: state.session?.email || "",
      sessionToken: state.session?.sessionToken || "",
      password,
      ...extra,
    };
  }

  async function postProfile(extra, passwordOverride) {
    // Supersede any boot-time canonical read before starting the mutation. A
    // stale GET must never overwrite this POST's response if it finishes later.
    state.profileMutationVersion += 1;
    const response = await fetch("/api/accounts/profile", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify(profilePayload(extra, passwordOverride)),
    });
    const body = await response.json().catch(() => ({}));
    if (!response.ok || body.ok !== true) {
      throw new Error(body.error || `HTTP ${response.status}`);
    }
    const nextSession = sessionFromAccountPayload(body, state.session || {});
    writeSession(nextSession);
    renderProfile(nextSession);
    return body;
  }

  async function refreshRepositories() {
    const data = await fetchJson("/api/repositories", { fresh: true });
    renderRepositories(data.repositories, state.session);
  }

  async function saveProfile(options = {}) {
    const passwordSelector = options.passwordSelector || "[data-profile-password]";
    const solanaSelector = options.solanaSelector || "[data-profile-solana]";
    const hintSelector = options.hintSelector || "[data-profile-hint]";
    const buttonSelector = options.buttonSelector || "[data-profile-save]";
    const password = profilePassword(passwordSelector);
    const solana = ($(solanaSelector)?.value || "").trim();
    if (!password) {
      if (hintSelector === "[data-profile-hint]") {
        setProfileHint("Enter your current password to save payout changes.", "bad");
      } else {
        setProfilePageHint(hintSelector, "Enter your current password to save payout changes.", "bad");
      }
      return;
    }
    const button = $(buttonSelector);
    if (button) { button.disabled = true; button.textContent = "Saving…"; }
    try {
      await postProfile({ solana }, password);
      if (hintSelector === "[data-profile-hint]") {
        setProfileHint(solana ? "Payout address saved." : "Payout address cleared.", "good");
      } else {
        setProfilePageHint(hintSelector, solana ? "Payout address saved." : "Payout address cleared.", "good");
      }
    } catch (error) {
      const message = error.message === "bad_solana"
        ? "Enter a valid public Solana address."
        : "Could not save payout address. Check your password and try again.";
      if (hintSelector === "[data-profile-hint]") {
        setProfileHint(message, "bad");
      } else {
        setProfilePageHint(hintSelector, message, "bad");
      }
    } finally {
      if (button) {
        button.disabled = false;
        button.textContent = options.buttonText || "Save profile";
      }
    }
  }

  function collectProfileLinks() {
    const links = [];
    $$("[data-profile-link-row]").forEach((row) => {
      const label = (row.querySelector("[data-profile-link-label]")?.value || "").trim();
      const url = (row.querySelector("[data-profile-link-url]")?.value || "").trim();
      if (!label && !url) return;
      links.push({ label, url });
    });
    return links;
  }

  async function savePublicProfile() {
    const button = $("[data-profile-public-save]");
    const publicEmail = $("[data-profile-public-email]");
    if (button) { button.disabled = true; button.textContent = "Saving…"; }
    try {
      await postProfile({
        // Sent only from an enabled dropdown. A control that is absent, or
        // locked because nothing is verified yet, has no opinion about the
        // stored choice and must never read as "unpublish my address".
        ...(publicEmail && !publicEmail.disabled
          ? { publicEmail: (publicEmail.value || "").trim() }
          : {}),
        profileBio: ($("[data-profile-page-bio]")?.value || "").trim(),
        profileLocation: ($("[data-profile-page-location]")?.value || "").trim(),
        ...($("[data-profile-page-name-case]")
          ? { nameCase: ($("[data-profile-page-name-case]")?.value || "").trim() }
          : {}),
        profileTimezone: ($("[data-profile-page-timezone]")?.value || "").trim(),
        mastodon: ($("[data-profile-page-mastodon]")?.value || "").trim(),
        profilePrivate: Boolean($("[data-profile-page-private]")?.checked),
        followersPublic: Boolean($("[data-profile-page-followers-public]")?.checked),
        profileLinks: collectProfileLinks(),
      });
      setProfilePageHint("[data-profile-public-hint]", "Public profile saved.", "good");
    } catch (error) {
      const messages = {
        bad_public_email: "Choose one of your verified email addresses.",
        bad_name_case: "Name capitalization can only change capital letters of your username.",
        bad_mastodon: "Enter a Mastodon handle like @you@example.social.",
        bad_profile_timezone: "Enter a valid IANA time zone like Asia/Kolkata.",
        bad_profile_links: "Check your profile links and try again.",
        bad_profile_link_url: "Profile links must be http or https URLs on a real domain.",
      };
      setProfilePageHint(
        "[data-profile-public-hint]",
        messages[error.message] || "Could not save public profile. Sign in again and try once more.",
        "bad");
    } finally {
      if (button) {
        button.disabled = false;
        button.textContent = "Save public profile";
      }
    }
  }

  function setProfileAvatarHint(message, tone = "") {
    const hint = $("[data-profile-avatar-hint]");
    if (!hint) return;
    hint.textContent = message;
    hint.className = `mt-3 min-h-4 text-xs ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
  }

  async function saveProfileAvatar(file, trigger) {
    if (!file) return;
    if (!["image/png", "image/jpeg", "image/webp"].includes(file.type)) {
      setProfileAvatarHint("Choose a PNG, JPEG, or WebP image.", "bad");
      return;
    }
    if (file.size > PROFILE_AVATAR_RAW_MAX_BYTES) {
      setProfileAvatarHint("Choose an image smaller than 20 MB.", "bad");
      return;
    }

    const result = await openImageResizeModal(file, PROFILE_AVATAR_MAX_BYTES, {
      dialogLabel: "Crop profile picture",
      title: "Crop profile picture",
      description: `Drag to choose a square crop. ${platformName()} will resize it to a PNG that fits the profile picture limit.`,
      confirmLabel: "Save profile picture",
      outputType: "image/png",
      aspectRatio: 1,
      maxDimension: 512,
      returnFocus: trigger,
    });
    if (!result) return;

    if (trigger) {
      trigger.disabled = true;
      trigger.classList.add("opacity-60");
    }
    setProfileAvatarHint("Saving profile picture…");
    try {
      await postProfile({ avatarPng: result.dataUrl });
      setProfileAvatarHint("Profile picture saved.", "good");
    } catch (error) {
      const message = error.message === "avatar_too_large"
        ? "The cropped picture is still too large. Try a smaller crop."
        : error.message === "bad_avatar"
          ? "That image could not be saved. Choose another image."
          : "Could not save the profile picture. Sign in again and try once more.";
      setProfileAvatarHint(message, "bad");
    } finally {
      if (trigger) {
        trigger.disabled = false;
        trigger.classList.remove("opacity-60");
      }
    }
  }

  function initProfileAvatarEditor() {
    const input = $("[data-profile-avatar-input]");
    if (!input || input.dataset.profileAvatarWired === "true") return;
    input.dataset.profileAvatarWired = "true";
    input.addEventListener("change", async () => {
      const file = input.files?.[0];
      const trigger = $("[data-profile-avatar-edit]");
      input.value = "";
      await saveProfileAvatar(file, trigger);
    });
  }

  async function saveProfileAbout() {
    const profileAbout = String($("[data-profile-about-textarea]")?.value || "").replace(/\r\n/g, "\n");
    const button = $("[data-profile-about-save]");
    if (button) { button.disabled = true; button.textContent = "Saving..."; }
    try {
      const body = await postProfile({ profileAbout });
      const nextSession = sessionFromAccountPayload(body, state.session || {});
      renderProfileAbout(nextSession);
      setProfileAboutHint("About saved.", "good");
      setProfileAboutModalOpen(false);
    } catch (error) {
      const message = error.message === "profile_about_too_large" || error.message === "profile_readme_too_large"
        ? "About is too large. Keep it under 32 KB."
        : "Could not save about. Sign in again and try once more.";
      setProfileAboutHint(message, "bad");
    } finally {
      if (button) {
        button.disabled = false;
        button.textContent = "Save about";
      }
    }
  }

  function verificationResendMessage(error, retryAfterMs) {
    const minutes = Math.ceil((Number(retryAfterMs) || 0) / 60000);
    if (error === "resend_too_soon") {
      return "A verification email just went out. Check your inbox and spam folder, then try again in a minute.";
    }
    if (error === "resend_limit_reached") {
      return `Too many verification emails today. Try again in ${minutes > 60 ? `${Math.ceil(minutes / 60)} hours` : `${Math.max(1, minutes)} minutes`}, or ask an administrator to verify you by hand.`;
    }
    if (error === "already_verified") return "This email is already verified.";
    if (error === "no_email") return "Add an email address to your account first.";
    if (error === "unauthorized") return "Sign in again, then resend the verification email.";
    return "Could not send the verification email. Try again in a moment.";
  }

  // Re-sending a confirmation link only needs the signed-in session, so it goes
  // to /api/accounts/resend-verification rather than the password-gated profile
  // POST. The Worker records every send on the account and pings administrators.
  async function resendVerification(options = {}) {
    const hintSelector = options.hintSelector || "";
    const buttonSelector = options.buttonSelector || "[data-email-verification-resend]";
    const buttonText = options.buttonText || "Resend verification email";
    const showHint = (message, kind) => {
      setEmailVerificationHint(message, kind);
      if (!hintSelector) return;
      if (hintSelector === "[data-profile-hint]") {
        setProfileHint(message, kind);
      } else {
        setProfilePageHint(hintSelector, message, kind);
      }
    };
    const button = $(buttonSelector);
    if (button) { button.disabled = true; button.textContent = "Sending…"; }
    try {
      const response = await fetch("/api/accounts/resend-verification", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          ...(state.session?.sessionToken
            ? { authorization: "Bearer " + state.session.sessionToken }
            : {}),
        },
        body: JSON.stringify({ sessionToken: state.session?.sessionToken || "" }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) {
        showHint(verificationResendMessage(body.error, body.retryAfterMs), "bad");
        return;
      }
      showHint(body.verificationSent
        ? "Verification email sent. Check your inbox and spam folder."
        : "Mail provider unavailable — an administrator was pinged to verify you by hand.",
        body.verificationSent ? "good" : "");
    } catch (_) {
      showHint(verificationResendMessage(""), "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = buttonText; }
      renderProfileModal(state.session);
      renderProfilePage(state.session);
    }
  }

  async function sendAdminTestEmail() {
    const button = $("[data-admin-test-email]");
    const hint = "[data-admin-test-email-status]";
    if (button) { button.disabled = true; button.textContent = "Sending…"; }
    try {
      const response = await fetch("/api/accounts/test-email", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          ...(state.session?.sessionToken
            ? { authorization: "Bearer " + state.session.sessionToken }
            : {}),
        },
        body: JSON.stringify({ sessionToken: state.session?.sessionToken || "" }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) {
        const wait = Math.ceil(Number(body.retryAfterMs || 0) / 1000);
        setProfilePageHint(hint, {
          forbidden: "Only administrators can send a test email.",
          no_email: "Add an email address to your account first.",
          resend_too_soon: `Wait ${wait || 60}s before sending another test email.`,
          unauthorized: "Sign in again to send a test email.",
        }[body.error] || "Could not send the test email.", "bad");
        return;
      }
      setProfilePageHint(hint, body.sent
        ? `Test email sent to ${body.email}. Check your inbox and spam folder.`
        : body.configured
          ? "The mail provider did not accept the test email."
          : "Outgoing mail is not configured on this server.",
        body.sent ? "" : "bad");
      renderAccountActivity({ account: body });
    } catch (_) {
      setProfilePageHint(hint, "Could not send the test email.", "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Send test email"; }
    }
  }

  async function checkNodeNameAvailability() {
    const input = $("[data-profile-rename-input]");
    const candidate = (input?.value || "").trim().toLowerCase();
    state.nodeNameAvailability.candidate = candidate;
    state.nodeNameAvailability.available = false;
    if (input && input.value !== candidate) input.value = candidate;
    if (!candidate) {
      setRenameStatus("Enter a new username to check availability.", "");
      updateRenameButton();
      return;
    }
    if (!validNodeName(candidate)) {
      setRenameStatus("Use lowercase letters, numbers, and hyphens. Start with a letter.", "bad");
      updateRenameButton();
      return;
    }
    if (candidate === String(state.session?.nodeName || "").toLowerCase()) {
      setRenameStatus("This is already your current username.", "bad");
      updateRenameButton();
      return;
    }
    if (!state.session?.emailVerified) {
      setRenameStatus("Verify your email before changing your username.", "bad");
      updateRenameButton();
      return;
    }

    const seq = state.nodeNameAvailability.seq + 1;
    state.nodeNameAvailability.seq = seq;
    state.nodeNameAvailability.checking = true;
    setRenameStatus("Checking availability…", "");
    updateRenameButton();
    try {
      const response = await fetch(`/api/accounts/${encodeURIComponent(candidate)}`, {
        headers: { accept: "application/json" },
      });
      const body = await response.json().catch(() => ({}));
      if (seq !== state.nodeNameAvailability.seq) return;
      state.nodeNameAvailability.available = Boolean(response.ok && body.available);
      setRenameStatus(
        state.nodeNameAvailability.available
          ? "Username is available."
          : "That username is already taken.",
        state.nodeNameAvailability.available ? "good" : "bad",
      );
    } catch (_) {
      if (seq !== state.nodeNameAvailability.seq) return;
      setRenameStatus("Could not check availability right now.", "bad");
    } finally {
      if (seq === state.nodeNameAvailability.seq) {
        state.nodeNameAvailability.checking = false;
        updateRenameButton();
      }
    }
  }

  async function renameNodeName() {
    const input = $("[data-profile-rename-input]");
    const newNodeName = (input?.value || "").trim().toLowerCase();
    const password = profilePassword("[data-profile-rename-password]");
    if (!state.nodeNameAvailability.available || !newNodeName) {
      setRenameStatus("Choose an available username first.", "bad");
      return;
    }
    if (!password) {
      setRenameStatus("Enter your current password to update your username.", "bad");
      updateRenameButton();
      return;
    }
    const button = $("[data-profile-rename-save]");
    if (button) { button.disabled = true; button.textContent = "Updating…"; }
    try {
      await postProfile({ newNodeName }, password);
      if (input) input.value = "";
      $("[data-profile-rename-password]") && ($("[data-profile-rename-password]").value = "");
      state.nodeNameAvailability.available = false;
      setRenameStatus("Username updated. Repositories are refreshing.", "good");
      await refreshRepositories();
      renderProfilePage(state.session);
    } catch (error) {
      const messages = {
        email_not_verified: "Verify your email before changing your username.",
        invalid_node_name: "Enter a valid username.",
        inappropriate_node_name: "That username was flagged by our automated filter. Try a different one, or email founders@forkmesh.com if that looks wrong.",
        node_name_taken: "That username is already taken.",
        node_name_unchanged: "Enter a different username.",
        repo_namespace_conflict: "That namespace already has repository data.",
      };
      setRenameStatus(messages[error.message] || "Could not update username. Check your password and try again.", "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Update username"; }
      updateRenameButton();
    }
  }

  // The header's "Reconnect node" shortcut deep-links here as
  // /dashboard/settings/nodes?from=reconnect.
  function arrivedFromNodeReconnect() {
    return new URLSearchParams(location.search).get("from") === "reconnect";
  }

  // Users vs nodes (adhoc #53): claim a node (e.g. a headless mirror you
  // installed) by its node ID, then confirm the code that appears on that
  // node itself to complete the link.
  function renderClaimNodePanel(session) {
    // Landing on a bare node-ID field explained nothing (issue #640), so the
    // panel opens with why this account was sent here. The same zero-linked-
    // nodes state that shows the header shortcut reveals it; the ?from marker
    // covers the click arriving before the session payload does.
    const callout = $("[data-node-reconnect-callout]");
    if (callout) {
      // A node linked while this page was open (or since the click) clears it,
      // so the marker never outlives the problem it describes.
      const linked = Array.isArray(session?.nodes) && session.nodes.length > 0;
      const show = !linked && !dashboardOnInstance() &&
        (nodeNeedsReconnect(session) || arrivedFromNodeReconnect());
      callout.classList.toggle("hidden", !show);
    }
    const list = $("[data-claim-node-list]");
    if (!list) return;
    const nodes = Array.isArray(session?.nodes) ? session.nodes : [];
    list.innerHTML = "";
    if (!nodes.length) {
      const li = document.createElement("li");
      li.className = "text-muted-foreground";
      li.textContent = "No linked nodes yet.";
      list.appendChild(li);
      return;
    }
    for (const node of nodes) {
      const li = document.createElement("li");
      li.className = "font-mono";
      li.textContent = node;
      list.appendChild(li);
    }
  }

  // "Enter a valid node ID" left web-only users guessing (issue #640). Name the
  // accepted form the typed value came closest to, and always carry an example.
  const NODE_ID_FORMS =
    "A node ID is either the node's name (starts with a letter; lowercase " +
    "letters, digits and hyphens, e.g. \"mirror-01\") or its 43-character " +
    "public key, both shown on that node's Profile page in the app.";

  function claimNodeIdHint(value) {
    const nodeId = String(value || "").trim();
    if (!nodeId) return "Enter a node ID first. " + NODE_ID_FORMS;
    if (nodeId.includes("@")) {
      return "That is an email address, and email identifies you rather than a node. " +
        NODE_ID_FORMS;
    }
    // A base64 paste keeps its "=" padding, which no node ID carries.
    const unpadded = nodeId.replace(/=+$/, "");
    // An over-long value, a padded one, or a long one carrying characters no
    // node name may hold was aimed at the public-key form - answer about that
    // one instead of node names. Short values fall through: "MIRROR_01" is a
    // mistyped name, not a truncated key.
    const keyShaped = /^[A-Za-z0-9_-]+$/.test(unpadded) &&
      (unpadded.length > 63 || unpadded !== nodeId ||
        (unpadded.length >= 20 && /[A-Z_]/.test(unpadded)));
    if (keyShaped) {
      const off = unpadded.length === 43
        ? "Drop the trailing \"=\" padding."
        : `This one is ${unpadded.length}.`;
      return "A node public key is exactly 43 characters, unpadded. " + off +
        " Use \"Copy node ID\" on that node's Profile page to paste the whole value.";
    }
    return "A node name starts with a letter and uses only lowercase letters, digits " +
      "and hyphens, e.g. \"mirror-01\". You can also paste the node's 43-character public key.";
  }

  async function claimNode() {
    const input = $("[data-claim-node-input]");
    // Not lowercased up front: a node's public-key ID is case-sensitive, and
    // only the plain-name form is meant to be case-insensitive.
    const nodeId = (input?.value || "").trim();
    const password = profilePassword("[data-claim-node-password]");
    if (!validNodeName(nodeId.toLowerCase()) && !validNodePubkey(nodeId)) {
      setProfilePageHint("[data-claim-node-status]", claimNodeIdHint(nodeId), "bad");
      return;
    }
    if (!password) {
      setProfilePageHint("[data-claim-node-status]", "Enter your current password to claim a node.", "bad");
      return;
    }
    const button = $("[data-claim-node-send]");
    if (button) { button.disabled = true; button.textContent = "Sending…"; }
    try {
      const response = await fetch("/api/accounts/claim-node", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          identifier: state.session?.email || state.session?.nodeName || "",
          password, nodeId,
        }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) throw new Error(body.error || `HTTP ${response.status}`);
      const codeRow = $("[data-claim-code-row]");
      if (body.alreadyLinked) {
        state.claimNode.pendingNodeId = "";
        if (codeRow) codeRow.classList.add("hidden");
        setProfilePageHint("[data-claim-node-status]", `"${nodeId}" is already linked to your account.`, "good");
        await refreshPublicProfile(state.session, { force: true });
      } else {
        state.claimNode.pendingNodeId = nodeId;
        if (codeRow) codeRow.classList.remove("hidden");
        setProfilePageHint(
          "[data-claim-node-status]",
          `Confirmation code sent to "${nodeId}". Check that node's app for the code, then enter it below.`,
          "good");
      }
    } catch (error) {
      const messages = {
        invalid_credentials: "Incorrect password.",
        invalid_node_id: claimNodeIdHint(nodeId),
        cannot_claim_self: "You can't claim your own account.",
        no_such_node: `No node called "${nodeId}" has registered with the relay. Check the ID on the node itself - or, if you have not installed one yet, see "No node yet?" below.`,
        not_a_node: "That ID belongs to a user account, not a claimable node. Node IDs come from the ForkMesh app running on a machine, not from a website username.",
        node_already_owned: "That node is already linked to another account.",
      };
      setProfilePageHint(
        "[data-claim-node-status]",
        messages[error.message] || "Could not send a claim code. Check the node ID and your password.",
        "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Send claim code"; }
    }
  }

  async function confirmClaimCode() {
    const nodeId = state.claimNode.pendingNodeId;
    const code = ($("[data-claim-code-input]")?.value || "").trim();
    const password = profilePassword("[data-claim-node-password]");
    if (!nodeId) {
      setProfilePageHint("[data-claim-node-status]", "Send a claim code first.", "bad");
      return;
    }
    if (!/^[0-9]{6}$/.test(code)) {
      setProfilePageHint("[data-claim-node-status]", "Enter the 6-digit code shown on the node.", "bad");
      return;
    }
    if (!password) {
      setProfilePageHint("[data-claim-node-status]", "Enter your current password to link this node.", "bad");
      return;
    }
    const button = $("[data-claim-code-confirm]");
    if (button) { button.disabled = true; button.textContent = "Linking…"; }
    try {
      const response = await fetch("/api/accounts/claim-confirm", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          identifier: state.session?.email || state.session?.nodeName || "",
          password, nodeId, code,
        }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) throw new Error(body.error || `HTTP ${response.status}`);
      const nextSession = {
        ...(state.session || {}),
        nodes: Array.isArray(body.nodes) ? body.nodes : state.session?.nodes,
      };
      writeSession(nextSession);
      renderProfile(nextSession);
      state.claimNode.pendingNodeId = "";
      const codeRow = $("[data-claim-code-row]");
      if (codeRow) codeRow.classList.add("hidden");
      const nodeInput = $("[data-claim-node-input]");
      if (nodeInput) nodeInput.value = "";
      const codeInput = $("[data-claim-code-input]");
      if (codeInput) codeInput.value = "";
      setProfilePageHint("[data-claim-node-status]", `Linked "${nodeId}" to your account.`, "good");
    } catch (error) {
      const messages = {
        invalid_credentials: "Incorrect password.",
        no_such_node: "No node with that ID was found.",
        no_pending_claim: "No pending claim for that node. Send a new claim code.",
        bad_code: "That code is incorrect.",
      };
      setProfilePageHint(
        "[data-claim-node-status]",
        messages[error.message] || "Could not link the node. Check the code and try again.",
        "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Link node"; }
    }
  }

  // "Link this node to your account" (adhoc #120): the desktop app opens
  // /dashboard?link_node=<node>&link_ts=<ts>&link_sig=<sig> - a short-lived
  // grant signed with the node's own key. The signature proves node-key
  // control and consents to the link, so whoever is logged in HERE becomes the
  // owner with no password re-entry or confirmation code.
  function pendingLinkGrant() {
    const params = new URLSearchParams(location.search);
    const nodeName = (params.get("link_node") || "").trim();
    const ts = (params.get("link_ts") || "").trim();
    const sig = (params.get("link_sig") || "").trim();
    if (!validNodeName(nodeName.toLowerCase()) || !ts || !sig) return null;
    return { nodeName, ts, sig };
  }

  function offerLinkGrant(grant) {
    // Strip the one-time grant from the address bar first so refresh/back
    // can't replay it (and it doesn't linger in the visible URL), then show
    // the settings page's Nodes panel and ask for one explicit "Authenticate &
    // link" click. The grant overrides any existing association, so the click
    // is the moment of consent on the browser side. (Boot redirects the grant
    // to the settings document before calling this, so the panel exists here.)
    const params = new URLSearchParams(location.search);
    for (const key of ["link_node", "link_ts", "link_sig"]) params.delete(key);
    const rest = params.toString();
    window.history.replaceState(null, "", location.pathname + (rest ? `?${rest}` : ""));
    state.linkGrant = grant;
    setSettingsSection("nodes", { scroll: false });
    const row = $("[data-link-grant-row]");
    if (row) row.classList.remove("hidden");
    const text = $("[data-link-grant-text]");
    if (text) {
      text.textContent =
        `Link node "${grant.nodeName}" to this account (` +
        `${state.session?.nodeName || "you"})? This node will belong to you - ` +
        "any existing link is replaced.";
    }
  }

  async function redeemLinkGrant() {
    const grant = state.linkGrant;
    if (!grant) return;
    const button = $("[data-link-grant-confirm]");
    if (button) { button.disabled = true; button.textContent = "Linking…"; }
    setProfilePageHint("[data-claim-node-status]", `Linking "${grant.nodeName}" to your account…`, "");
    try {
      const token = String(state.session?.sessionToken || "");
      const payload = {
        nodeName: grant.nodeName,
        ts: grant.ts,
        sig: grant.sig,
        user: state.session?.nodeName || "",
      };
      if (token && token !== "cookie") payload.sessionToken = token;
      const response = await fetch("/api/accounts/link-grant", {
        method: "POST",
        credentials: "same-origin",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          ...(token && token !== "cookie" ? { authorization: "Bearer " + token } : {}),
        },
        body: JSON.stringify(payload),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) throw new Error(body.error || `HTTP ${response.status}`);
      state.linkGrant = null;
      $("[data-link-grant-row]")?.classList.add("hidden");
      const nextSession = {
        ...(state.session || {}),
        nodes: Array.isArray(body.nodes) ? body.nodes : state.session?.nodes,
      };
      writeSession(nextSession);
      renderProfile(nextSession);
      setProfilePageHint(
        "[data-claim-node-status]",
        body.selfAccount
          ? `"${body.nodeId || grant.nodeName}" is this account - already yours.`
          : body.alreadyLinked
            ? `"${body.nodeId || grant.nodeName}" is already linked to your account.`
            : `Linked "${body.nodeId || grant.nodeName}" to your account.`,
        "good");
    } catch (error) {
      state.linkGrant = null;
      $("[data-link-grant-row]")?.classList.add("hidden");
      const messages = {
        unauthorized: "The link expired - click \"Link this node to your account\" in the node's app again.",
        grant_expired: "The link expired - click \"Link this node to your account\" in the node's app again.",
        session_required: "Log in with your user account first, then open the link again.",
        grant_user_mismatch: "This link names a different account - log in as that account to accept it.",
        bad_signature: "The link couldn't be verified - click the button in the node's app again.",
        grant_used: "That link was already used - click the button in the node's app again.",
        no_such_node: "That node isn't registered with the relay yet.",
        not_a_user: "This login can't own nodes - sign up as a user (email + password) first.",
        no_such_user: "Log in with a user account first, then open the link again.",
      };
      setProfilePageHint(
        "[data-claim-node-status]",
        messages[error.message] || "Could not link the node. Click the button in the node's app and try again.",
        "bad");
    } finally {
      if (button) { button.disabled = false; button.textContent = "Authenticate & link"; }
    }
  }

  async function deleteAccount() {
    const password = profilePassword("[data-profile-delete-password]");
    const confirm = ($("[data-profile-delete-confirm]")?.value || "").trim();
    if (!password) {
      setProfilePageHint("[data-profile-delete-hint]", "Enter your current password to delete this account.", "bad");
      return;
    }
    if (confirm !== "DELETE") {
      setProfilePageHint("[data-profile-delete-hint]", "Type DELETE to confirm account deletion.", "bad");
      return;
    }
    const button = $("[data-profile-delete-account]");
    if (button) { button.disabled = true; button.textContent = "Deleting..."; }
    try {
      await postProfile({ deleteAccount: true }, password);
      logout();
    } catch (_) {
      setProfilePageHint("[data-profile-delete-hint]", "Could not delete account. Check your password and try again.", "bad");
      if (button) { button.disabled = false; button.textContent = "Delete account"; }
    }
  }
