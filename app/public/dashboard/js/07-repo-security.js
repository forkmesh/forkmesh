
  const SECURITY_CREDIT_TYPES = [
    ["finder", "Finder"], ["reporter", "Reporter"], ["analyst", "Analyst"],
    ["coordinator", "Coordinator"], ["remediation_developer", "Remediation developer"],
    ["remediation_reviewer", "Remediation reviewer"], ["remediation_verifier", "Remediation verifier"],
    ["tool", "Tool"], ["sponsor", "Sponsor"], ["other", "Other"],
  ];
  const SECURITY_ECOSYSTEMS = [
    "", "npm", "PyPI", "Go", "Maven", "NuGet", "crates.io", "RubyGems", "Packagist",
    "Pub", "Hex", "GitHub Actions", "SwiftURL", "Hackage", "CRAN", "ConanCenter",
  ];
  const SECURITY_LEVELS = ["low", "moderate", "high", "critical"];
  const SECURITY_STATE_LABELS = {
    triage: "Triage", draft: "Draft", published: "Published", closed: "Closed", withdrawn: "Withdrawn",
  };
  const SECURITY_POLICY_PATHS = ["SECURITY.md", ".github/SECURITY.md", "docs/SECURITY.md"];
  const SECURITY_ERRORS = {
    not_found: "This advisory does not exist or you do not have access to it.",
    sign_in_required: "Sign in to report a vulnerability.",
    email_unverified: "Verify your email address before reporting a vulnerability.",
    reporting_disabled: "Private vulnerability reporting is turned off for this repository.",
    title_required: "Give the report a title.",
    invalid_cvss: "That CVSS vector is not a valid CVSS 3.1 or 4.0 base vector.",
    invalid_severity: "Choose low, moderate, high or critical.",
    invalid_cwe: "CWE IDs look like CWE-79.",
    invalid_cve: "CVE IDs look like CVE-2026-12345.",
    invalid_reference: "References must be http(s) links.",
    invalid_ecosystem: "Choose an ecosystem from the list.",
    invalid_fix: "The fix branch, pull request number or commit is not valid.",
    too_many_open_reports: "You already have ten open reports for this repository.",
    inbox_full: "This repository has too many reports waiting for triage.",
    bad_signature: "The signature did not verify. Reload the page and try again.",
    content_changed: "The advisory changed while you were publishing. Try again.",
    stale_prepare: "The advisory changed while you were publishing. Try again.",
    repository_private: "Only advisories of public repositories can be published.",
    signature_required: "A published advisory changes only through a signed update.",
    unknown_account: "There is no account with that name.",
    forbidden: "You do not have permission to do that.",
  };
  const securityView = { key: "", overview: null, policy: null, detail: null, bound: false };

  function securityApi(repo, path = "") {
    return `${repoApiBase(repo)}/security${path}`;
  }

  function securityUrl(repo, sub = "") {
    return `${repoLinkUrl(repo)}/security${sub}`;
  }

  function securityRouteParts() {
    const parts = repoRouteParts();
    return parts[2] === "security" ? parts.slice(3).map((part) => safeDecodeURIComponent(part)) : [];
  }

  function securityErrorText(code) {
    return SECURITY_ERRORS[code] || `Something went wrong (${code}).`;
  }

  async function securityRequest(url, payload) {
    const response = await fetch(url, repoRemoteForksRequestInit(payload ? "POST" : "GET", payload));
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(securityErrorText(data.error || `HTTP ${response.status}`));
      error.code = data.error || "";
      error.status = response.status;
      throw error;
    }
    return data;
  }

  function securitySeverityPill(severity) {
    const level = String(severity?.level || "");
    const score = typeof severity?.score === "number" ? ` ${severity.score.toFixed(1)}` : "";
    const tone = {
      critical: "bg-destructive text-white border-destructive",
      high: "border-destructive text-destructive",
      moderate: "border-attention text-attention",
      low: "border-border text-muted-foreground",
    }[level] || "border-border text-muted-foreground";
    const label = level ? level.charAt(0).toUpperCase() + level.slice(1) : "Unrated";
    return `<span data-security-severity="${escapeHtml(level || "none")}" class="inline-flex h-6 shrink-0 items-center rounded-full border px-2 text-[12px] font-medium ${tone}">${escapeHtml(label + score)}</span>`;
  }

  function securityStatePill(stateName) {
    const tone = {
      published: "bg-success text-white",
      withdrawn: "bg-[#6e7681] text-white",
      closed: "bg-[#6e7681] text-white",
      draft: "border border-border text-foreground",
      triage: "border border-attention text-attention",
    }[stateName] || "border border-border text-foreground";
    const icon = { published: "shield-check", withdrawn: "shield-off", closed: "circle-slash", draft: "file-pen-line", triage: "shield-alert" }[stateName] || "shield";
    return `<span data-security-state="${escapeHtml(stateName)}" class="inline-flex h-8 items-center gap-1.5 rounded-full px-3 text-[14px] font-medium ${tone}"><i data-lucide="${icon}" class="h-4 w-4"></i>${escapeHtml(SECURITY_STATE_LABELS[stateName] || stateName)}</span>`;
  }

  function bindSecurityPanel(container) {
    if (container.dataset.securityBound === "1") return;
    container.dataset.securityBound = "1";
    container.addEventListener("click", (event) => {
      const nav = event.target.closest("[data-security-nav]");
      if (nav && !event.metaKey && !event.ctrlKey && !event.shiftKey && event.button === 0) {
        event.preventDefault();
        openSecurityRoute(state.selectedRepo, nav.dataset.securityNav);
        return;
      }
      const action = event.target.closest("[data-security-action]");
      if (action) {
        event.preventDefault();
        void runSecurityAction(state.selectedRepo, action);
      }
    });
    container.addEventListener("submit", (event) => {
      const inline = event.target.closest("[data-security-inline]");
      if (inline) {
        event.preventDefault();
        const control = inline.querySelector("[data-security-action]");
        if (control) void runSecurityAction(state.selectedRepo, control);
        return;
      }
      const form = event.target.closest("[data-security-form]");
      if (!form) return;
      event.preventDefault();
      void submitSecurityForm(state.selectedRepo, form);
    });
  }

  function openSecurityRoute(repo, sub) {
    if (!repo) return;
    navigateHistory(securityUrl(repo, sub || ""));
    void loadRepoSecurity(repo);
    $("[data-dashboard-repo-tab-panel=\"security\"]")?.scrollIntoView?.({ block: "nearest" });
  }

  async function loadRepoSecurity(repo) {
    const container = $("[data-repo-security]");
    if (!container || !repo) return;
    bindSecurityPanel(container);
    const key = repoKey(repo);
    if (securityView.key !== key) {
      securityView.key = key;
      securityView.overview = null;
      securityView.policy = null;
    }
    const parts = securityRouteParts();
    container.innerHTML = `<div class="py-6 text-sm text-muted-foreground">${loadingHtml("Loading security...")}</div>`;
    try {
      securityView.overview = await securityRequest(securityApi(repo));
    } catch (error) {
      container.innerHTML = securityNotice(error.message);
      return;
    }
    if (parts[0] === "advisories" && parts[1] === "new") {
      renderSecurityForm(repo, null);
    } else if (parts[0] === "advisories" && parts[1]) {
      await loadSecurityAdvisory(repo, parts[1]);
    } else {
      renderSecurityOverview(repo);
      void loadSecurityPolicy(repo);
    }
    window.lucide?.createIcons();
  }

  function securityNotice(message, tone = "") {
    return `<div data-security-notice class="my-4 rounded-md border ${tone === "bad" ? "border-destructive text-destructive" : "border-border text-muted-foreground"} px-4 py-3 text-sm">${escapeHtml(message)}</div>`;
  }

  async function loadSecurityPolicy(repo) {
    const target = $("[data-security-policy]");
    if (!target) return;
    if (!securityView.policy) {
      let found = { path: "", text: "" };
      for (const path of SECURITY_POLICY_PATHS) {
        try {
          const blob = await fetchRepoJson(repoLiveUrl(repo, "blob", { path }));
          const text = blobText(blob);
          if (text && text.trim()) {
            found = { path, text };
            break;
          }
        } catch (_) {}
      }
      securityView.policy = found;
    }
    const policy = securityView.policy;
    const source = $("[data-security-policy-source]");
    if (source) source.textContent = policy.path || "Default policy";
    target.innerHTML = policy.text
      ? `<div class="${MD_BODY_CLASS}">${renderRecordMarkdown(policy.text, repo)}</div>`
      : `<div class="grid gap-2 text-[14px] leading-6 text-foreground">
          <p>This repository has no <code>SECURITY.md</code>. Please do not report security vulnerabilities in public issues, pull requests or discussions.</p>
          <p>${securityView.overview?.reportingEnabled
            ? "Use <strong>Report a vulnerability</strong> to send a private report. Only the reporter, the maintainers of this repository and the people they invite can read it."
            : "Private vulnerability reporting is turned off here; contact the maintainers privately instead."}</p>
          <p>Maintainers can add a <code>SECURITY.md</code> file with their supported versions and disclosure timeline.</p>
        </div>`;
  }

  function securityRow(repo, item) {
    const href = securityUrl(repo, `/advisories/${encodeURIComponent(item.id)}`);
    const when = item.publishedAt ? `Published ${formatDate(item.publishedAt)}` : `Updated ${relativeTimeLabel(item.updatedAt)}`;
    return `<li class="flex flex-wrap items-center gap-3 px-4 py-3">
      ${securitySeverityPill(item.severity)}
      <div class="min-w-0 flex-1">
        <a href="${escapeHtml(href)}" data-security-nav="/advisories/${escapeHtml(encodeURIComponent(item.id))}" class="font-semibold text-foreground hover:text-accent hover:underline [overflow-wrap:anywhere]">${escapeHtml(item.title || item.id)}</a>
        <div class="mt-0.5 flex flex-wrap gap-x-3 text-[12px] text-muted-foreground">
          <span class="font-mono">${escapeHtml(item.id)}</span>
          ${item.cve ? `<span class="font-mono">${escapeHtml(item.cve)}</span>` : ""}
          <span>${escapeHtml(when)}</span>
          ${item.reporter ? `<span>reported by ${issuePersonNameHtml(item.reporter, "text-muted-foreground")}</span>` : ""}
        </div>
      </div>
      ${item.state && item.state !== "published" ? securityStatePill(item.state) : ""}
    </li>`;
  }

  function securityList(repo, items, empty) {
    return items.length
      ? `<ul class="divide-y divide-border">${items.map((item) => securityRow(repo, item)).join("")}</ul>`
      : `<p class="px-4 py-6 text-center text-sm text-muted-foreground">${escapeHtml(empty)}</p>`;
  }

  function renderSecurityOverview(repo) {
    const container = $("[data-repo-security]");
    const data = securityView.overview || {};
    const signedIn = Boolean(state.session?.nodeName);
    const reportButton = data.canReport
      ? `<a href="${escapeHtml(securityUrl(repo, "/advisories/new"))}" data-security-nav="/advisories/new" data-security-report class="inline-flex h-8 items-center gap-1.5 rounded-md bg-cta px-3 text-[14px] font-medium text-cta-foreground hover:bg-cta/90"><i data-lucide="shield-alert" class="h-4 w-4"></i>${data.maintainer ? "New draft advisory" : "Report a vulnerability"}</a>`
      : !signedIn && data.reportingEnabled
        ? `<a href="/login?next=${encodeURIComponent(location.pathname)}" class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border px-3 text-[14px] text-foreground hover:bg-secondary"><i data-lucide="log-in" class="h-4 w-4"></i>Sign in to report a vulnerability</a>`
        : "";
    const privateItems = Array.isArray(data.private) ? data.private : [];
    const privateSection = data.maintainer || privateItems.length ? `
      <section data-security-private class="overflow-hidden rounded-md border border-border">
        <div class="flex items-center gap-2 border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-2 text-[14px] font-semibold text-foreground"><i data-lucide="lock" class="h-4 w-4 text-muted-foreground"></i>${data.maintainer ? "Reports and drafts" : "Your private reports"}<span class="fm-counter">${privateItems.length}</span><span class="ml-auto text-[12px] font-normal text-muted-foreground">Only visible to you${data.maintainer ? ", the reporters and collaborators" : " and the maintainers"}</span></div>
        ${securityList(repo, privateItems, "No reports are waiting for triage.")}
      </section>` : "";
    const settings = data.admin ? `
      <section data-security-settings class="flex flex-wrap items-center gap-3 rounded-md border border-border px-4 py-3">
        <i data-lucide="settings" class="h-4 w-4 text-muted-foreground"></i>
        <div class="min-w-0 flex-1">
          <p class="text-[14px] font-semibold text-foreground">Private vulnerability reporting</p>
          <p class="text-[12px] text-muted-foreground">${data.reportingEnabled ? "On: any signed-in account with a verified email can send a private report." : "Off: only maintainers can open advisories."}</p>
        </div>
        <button type="button" data-security-action="reporting" data-enable="${data.reportingEnabled ? "0" : "1"}" class="inline-flex h-8 items-center rounded-md border border-border px-3 text-[14px] text-foreground hover:bg-secondary">${data.reportingEnabled ? "Turn off" : "Turn on"}</button>
      </section>` : "";
    container.innerHTML = `
      <div data-security-overview class="grid gap-5 py-2">
        <div class="flex flex-wrap items-start justify-between gap-3">
          <div class="min-w-0">
            <h2 class="text-xl font-semibold text-foreground">Security</h2>
            <p class="mt-1 text-sm text-muted-foreground">Security policy, published advisories and private vulnerability reports for ${escapeHtml(data.repository || repoKey(repo))}.</p>
          </div>
          <div class="flex flex-wrap gap-2">${reportButton}</div>
        </div>
        <section class="overflow-hidden rounded-md border border-border">
          <div class="flex items-center gap-2 border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-2 text-[14px] font-semibold text-foreground"><i data-lucide="shield" class="h-4 w-4 text-muted-foreground"></i>Security policy<span data-security-policy-source class="ml-auto font-mono text-[12px] font-normal text-muted-foreground"></span></div>
          <div data-security-policy class="px-4 py-4 text-sm text-muted-foreground">${loadingHtml("Reading SECURITY.md...")}</div>
        </section>
        <section data-security-published class="overflow-hidden rounded-md border border-border">
          <div class="flex items-center gap-2 border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-2 text-[14px] font-semibold text-foreground"><i data-lucide="shield-check" class="h-4 w-4 text-muted-foreground"></i>Published advisories<span class="fm-counter">${(data.published || []).length}</span><a href="/advisories" class="ml-auto text-[12px] font-normal text-primary hover:underline">All advisories on this site</a></div>
          ${securityList(repo, data.published || [], "There are no published security advisories.")}
        </section>
        ${privateSection}
        ${settings}
      </div>`;
  }

  function securityFieldClass() {
    return "h-8 w-full rounded-md border border-border bg-background px-2 text-[14px] text-foreground";
  }

  function securityAffectedRow(entry = {}) {
    const field = securityFieldClass();
    return `<div data-security-affected class="grid gap-2 rounded-md border border-border p-3 sm:grid-cols-2">
      <label class="grid gap-1 text-[12px] text-muted-foreground">Ecosystem<select name="ecosystem" class="${field}">${SECURITY_ECOSYSTEMS.map((name) => `<option value="${escapeHtml(name)}"${name === (entry.ecosystem || "") ? " selected" : ""}>${escapeHtml(name || "Not a package (source folder)")}</option>`).join("")}</select></label>
      <label class="grid gap-1 text-[12px] text-muted-foreground">Package name<input name="package" value="${escapeHtml(entry.package || "")}" class="${field}" placeholder="widget" /></label>
      <label class="grid gap-1 text-[12px] text-muted-foreground">Folder in the repository<input name="path" value="${escapeHtml(entry.path || "")}" class="${field}" placeholder="src/server" /></label>
      <label class="grid gap-1 text-[12px] text-muted-foreground">Affected versions<input name="vulnerable" value="${escapeHtml(entry.vulnerable || "")}" class="${field}" placeholder="&gt;= 1.0, &lt; 1.4.2" /></label>
      <label class="grid gap-1 text-[12px] text-muted-foreground sm:col-span-2">Patched versions<input name="patched" value="${escapeHtml(entry.patched || "")}" class="${field}" placeholder="1.4.2" /></label>
    </div>`;
  }

  function renderSecurityForm(repo, detail) {
    const container = $("[data-repo-security]");
    const overview = securityView.overview || {};
    const advisory = detail?.advisory || {};
    const editing = Boolean(detail);
    if (!editing && !overview.canReport) {
      container.innerHTML = securityNotice(state.session?.nodeName
        ? securityErrorText(overview.reportingEnabled ? "email_unverified" : "reporting_disabled")
        : securityErrorText("sign_in_required"), "bad");
      return;
    }
    const field = securityFieldClass();
    const maintainer = editing ? detail.role === "maintainer" : overview.maintainer;
    const severity = advisory.severity || {};
    const fix = advisory.fix || {};
    const affected = (advisory.affected && advisory.affected.length) ? advisory.affected : [{}];
    const back = editing ? `/advisories/${encodeURIComponent(advisory.id)}` : "";
    const heading = editing ? `Edit ${advisory.id}` : maintainer ? "New draft advisory" : "Report a vulnerability";
    container.innerHTML = `
      <form data-security-form="${editing ? "edit" : "create"}" data-advisory-id="${escapeHtml(advisory.id || "")}" data-published="${advisory.state === "published" ? "1" : "0"}" class="grid max-w-3xl gap-4 py-2">
        <div>
          <a href="${escapeHtml(securityUrl(repo, back))}" data-security-nav="${escapeHtml(back)}" class="text-[13px] text-primary hover:underline">&larr; ${editing ? "Back to the advisory" : "Security"}</a>
          <h2 class="mt-2 text-xl font-semibold text-foreground">${escapeHtml(heading)}</h2>
          ${editing ? "" : `<p class="mt-1 text-sm text-muted-foreground">${maintainer
            ? "Drafts stay private to the maintainers and the collaborators you add until you publish them."
            : "This report is private. Only you, the maintainers of this repository and the people they invite can read it; the site's security contact is also told about it."}</p>`}
        </div>
        <label class="grid gap-1 text-[14px] font-semibold text-foreground">Title<input name="title" required maxlength="200" value="${escapeHtml(advisory.title || "")}" class="${field}" placeholder="SQL injection in the search endpoint" /></label>
        <label class="grid gap-1 text-[14px] font-semibold text-foreground">Description<textarea name="description" rows="10" class="w-full rounded-md border border-border bg-background px-2 py-1.5 font-mono text-[13px] text-foreground" placeholder="Impact, affected code, steps to reproduce, proof of concept and a suggested fix. Markdown is supported.">${escapeHtml(advisory.description || "")}</textarea></label>
        <fieldset class="grid gap-2 rounded-md border border-border p-3">
          <legend class="px-1 text-[14px] font-semibold text-foreground">Severity</legend>
          <label class="grid gap-1 text-[12px] text-muted-foreground">CVSS 3.1 or 4.0 vector (the score is computed for you)<input name="vector" value="${escapeHtml(severity.vector || "")}" class="${field} font-mono" placeholder="CVSS:3.1/AV:N/AC:L/PR:N/UI:N/S:U/C:H/I:H/A:H" /></label>
          <label class="grid gap-1 text-[12px] text-muted-foreground">Or choose a level<select name="level" class="${field}"><option value="">Not rated</option>${SECURITY_LEVELS.map((level) => `<option value="${level}"${!severity.vector && severity.level === level ? " selected" : ""}>${level.charAt(0).toUpperCase() + level.slice(1)}</option>`).join("")}</select></label>
        </fieldset>
        <label class="grid gap-1 text-[14px] font-semibold text-foreground">Weaknesses (CWE IDs)<input name="cwes" value="${escapeHtml((advisory.cwes || []).join(", "))}" class="${field}" placeholder="CWE-89, CWE-20" /></label>
        <fieldset class="grid gap-2">
          <legend class="text-[14px] font-semibold text-foreground">Affected products</legend>
          <div data-security-affected-list class="grid gap-2">${affected.map((entry) => securityAffectedRow(entry)).join("")}</div>
          <button type="button" data-security-action="add-affected" class="w-fit text-[13px] text-primary hover:underline">Add another product</button>
        </fieldset>
        <label class="grid gap-1 text-[14px] font-semibold text-foreground">References<textarea name="references" rows="3" class="w-full rounded-md border border-border bg-background px-2 py-1.5 text-[13px] text-foreground" placeholder="One link per line">${escapeHtml((advisory.references || []).join("\n"))}</textarea></label>
        ${maintainer ? `
        <label class="grid gap-1 text-[14px] font-semibold text-foreground">CVE ID<input name="cve" value="${escapeHtml(advisory.cve || "")}" class="${field} font-mono" placeholder="CVE-2026-12345" /></label>
        <fieldset class="grid gap-2 rounded-md border border-border p-3 sm:grid-cols-3">
          <legend class="px-1 text-[14px] font-semibold text-foreground">Fix</legend>
          <label class="grid gap-1 text-[12px] text-muted-foreground">Branch<input name="fixBranch" value="${escapeHtml(fix.branch || "")}" class="${field} font-mono" placeholder="security/fix-search" /></label>
          <label class="grid gap-1 text-[12px] text-muted-foreground">Pull request #<input name="fixPull" inputmode="numeric" value="${escapeHtml(fix.pull ? String(fix.pull) : "")}" class="${field}" /></label>
          <label class="grid gap-1 text-[12px] text-muted-foreground">Commit<input name="fixCommit" value="${escapeHtml(fix.commit || "")}" class="${field} font-mono" /></label>
        </fieldset>` : ""}
        <p data-security-form-status class="text-sm text-muted-foreground" role="status"></p>
        <div class="flex flex-wrap gap-2">
          <button type="submit" class="inline-flex h-8 items-center gap-1.5 rounded-md ${editing ? "bg-primary text-primary-foreground hover:bg-primary/90" : "bg-cta text-cta-foreground hover:bg-cta/90"} px-3 text-[14px] font-medium">${editing ? (advisory.state === "published" ? "Sign and update" : "Save changes") : maintainer ? "Create draft advisory" : "Submit report"}</button>
          <a href="${escapeHtml(securityUrl(repo, back))}" data-security-nav="${escapeHtml(back)}" class="inline-flex h-8 items-center rounded-md border border-border px-3 text-[14px] text-foreground hover:bg-secondary">Cancel</a>
        </div>
      </form>`;
    window.lucide?.createIcons();
  }

  function securityFormFields(form, maintainer) {
    const value = (name) => String(form.elements[name]?.value || "").trim();
    const fields = {
      title: value("title"),
      description: String(form.elements.description?.value || ""),
      severity: value("vector") ? { vector: value("vector") } : { level: value("level") },
      cwes: value("cwes").split(/[\s,]+/).filter(Boolean),
      references: value("references").split(/\s+/).filter(Boolean),
      affected: [...form.querySelectorAll("[data-security-affected]")].map((row) => ({
        ecosystem: row.querySelector("[name=ecosystem]").value,
        package: row.querySelector("[name=package]").value.trim(),
        path: row.querySelector("[name=path]").value.trim(),
        vulnerable: row.querySelector("[name=vulnerable]").value.trim(),
        patched: row.querySelector("[name=patched]").value.trim(),
      })).filter((entry) => Object.values(entry).some(Boolean)),
    };
    if (maintainer && form.elements.cve) {
      fields.cve = value("cve");
      fields.fix = { branch: value("fixBranch"), pull: Number(value("fixPull") || 0), commit: value("fixCommit") };
    }
    return fields;
  }

  async function submitSecurityForm(repo, form) {
    const status = form.querySelector("[data-security-form-status]");
    const button = form.querySelector("[type=submit]");
    const editing = form.dataset.securityForm === "edit";
    const advisoryId = form.dataset.advisoryId;
    const maintainer = editing ? securityView.detail?.role === "maintainer" : securityView.overview?.maintainer;
    const fields = securityFormFields(form, maintainer);
    button.disabled = true;
    status.textContent = editing ? "Saving..." : "Sending...";
    try {
      if (!editing) {
        const created = await securityRequest(securityApi(repo, "/advisories"), fields);
        openSecurityRoute(repo, `/advisories/${encodeURIComponent(created.id)}`);
        return;
      }
      if (form.dataset.published === "1") {
        await signSecurityAdvisory(repo, advisoryId, "update", fields);
      } else {
        await securityRequest(securityApi(repo, `/advisories/${encodeURIComponent(advisoryId)}`), { action: "edit", fields });
      }
      openSecurityRoute(repo, `/advisories/${encodeURIComponent(advisoryId)}`);
    } catch (error) {
      status.textContent = error.message;
      status.className = "text-sm text-destructive";
      button.disabled = false;
    }
  }

  async function signSecurityAdvisory(repo, advisoryId, kind, fields = null) {
    const url = securityApi(repo, `/advisories/${encodeURIComponent(advisoryId)}`);
    const prepared = await securityRequest(url, { action: "prepare", kind, fields });
    const { privateKey, pub } = await getWebIssueKey();
    const ts = Date.now();
    const canonical = `${prepared.signingPrefix}\n${prepared.repoKey}\n${kind}\n${advisoryId}\n${pub}\n${ts}\n${prepared.contentHash}`;
    const sig = bytesToB64url(await crypto.subtle.sign({ name: "Ed25519" }, privateKey, new TextEncoder().encode(canonical)));
    return securityRequest(url, {
      action: "sign", kind, fields, preparedAt: prepared.preparedAt, revision: prepared.revision,
      event: { author: pub, ts, contentHash: prepared.contentHash, sig },
    });
  }

  async function loadSecurityAdvisory(repo, advisoryId) {
    const container = $("[data-repo-security]");
    try {
      securityView.detail = await securityRequest(securityApi(repo, `/advisories/${encodeURIComponent(advisoryId)}`));
    } catch (error) {
      container.innerHTML = `<div class="py-2"><a href="${escapeHtml(securityUrl(repo))}" data-security-nav="" class="text-[13px] text-primary hover:underline">&larr; Security</a>${securityNotice(error.message, "bad")}</div>`;
      return;
    }
    const parts = securityRouteParts();
    if (parts[2] === "edit" && securityView.detail.permissions?.edit) renderSecurityForm(repo, securityView.detail);
    else renderSecurityAdvisory(repo);
  }

  function securitySideSection(title, body) {
    return `<section class="border-b border-border pb-3"><h3 class="mb-1.5 text-[12px] font-semibold text-muted-foreground">${escapeHtml(title)}</h3><div class="text-[14px] text-foreground">${body}</div></section>`;
  }

  function securityTimeline(repo, detail) {
    const items = [
      ...(detail.comments || []).map((comment) => ({ ts: comment.ts, comment })),
      ...((detail.advisory?.history) || []).filter((entry) => entry.action !== "reported" && entry.action !== "created").map((entry) => ({ ts: entry.ts, entry })),
    ].sort((a, b) => a.ts - b.ts);
    return items.map((item) => {
      if (item.comment) {
        const comment = item.comment;
        return `<div data-security-comment class="grid min-w-0 gap-4 sm:grid-cols-[40px_minmax(0,1fr)]">
          <span class="hidden sm:block">${issueActorAvatarHtml(comment.author, comment.author, "h-10 w-10")}</span>
          <div class="min-w-0 overflow-hidden rounded-md border border-border">
            <div class="border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-1.5 text-[14px] text-muted-foreground">${issuePersonNameHtml(comment.author)} commented ${escapeHtml(relativeTimeLabel(comment.ts))}</div>
            <div class="px-4 py-3 ${MD_BODY_CLASS}">${renderRecordMarkdown(comment.body, repo)}</div>
          </div>
        </div>`;
      }
      const entry = item.entry;
      return `<div data-security-event class="flex items-center gap-2 text-[13px] text-muted-foreground sm:ml-14"><i data-lucide="dot" class="h-4 w-4"></i>${issuePersonNameHtml(entry.actor)} ${escapeHtml(entry.action)}${entry.detail ? ` <span class="text-foreground">${escapeHtml(entry.detail)}</span>` : ""} ${escapeHtml(relativeTimeLabel(entry.ts))}</div>`;
    }).join("");
  }

  function securityCreditLabel(type) {
    return (SECURITY_CREDIT_TYPES.find(([key]) => key === type) || [type, type])[1];
  }

  function renderSecurityAdvisory(repo) {
    const container = $("[data-repo-security]");
    const detail = securityView.detail || {};
    const advisory = detail.advisory || {};
    const perms = detail.permissions || {};
    const member = Boolean(detail.role);
    const stateName = advisory.state || "";
    const severity = advisory.severity || {};
    const fix = advisory.fix || {};
    const ledger = detail.ledger || {};
    const button = (action, label, tone = "", extra = "") => `<button type="button" data-security-action="${action}"${extra} class="inline-flex h-8 items-center gap-1.5 rounded-md px-3 text-[14px] font-medium ${tone || "border border-border text-foreground hover:bg-secondary"}">${label}</button>`;
    const actions = [];
    if (perms.manage) {
      if (stateName === "triage") actions.push(button("state", "Accept as draft", "", " data-state=\"draft\""), button("state", "Close", "", " data-state=\"closed\""));
      if (stateName === "draft") actions.push(button("publish", "Publish advisory", "bg-primary text-primary-foreground hover:bg-primary/90"), button("state", "Close", "", " data-state=\"closed\""));
      if (stateName === "closed") actions.push(button("state", "Reopen as draft", "", " data-state=\"draft\""));
      if (stateName === "published") actions.push(button("withdraw", "Withdraw", "border border-destructive text-destructive hover:bg-destructive/10"));
    }
    if (perms.edit) actions.unshift(`<a href="${escapeHtml(securityUrl(repo, `/advisories/${encodeURIComponent(advisory.id)}/edit`))}" data-security-nav="/advisories/${escapeHtml(encodeURIComponent(advisory.id))}/edit" class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border px-3 text-[14px] text-foreground hover:bg-secondary"><i data-lucide="pencil" class="h-4 w-4"></i>Edit</a>`);
    const warnings = (detail.warnings || []).includes("no_patched_version") && member && stateName !== "withdrawn"
      ? `<div data-security-warning class="rounded-md border border-attention px-4 py-2 text-[13px] text-attention"><i data-lucide="triangle-alert" class="mr-1 inline h-4 w-4"></i>No patched version is set. You can still publish, but scanners will report every version as affected.</div>` : "";
    const credit = detail.viewerCredit;
    const creditBanner = credit && credit.state === "pending" && ["published", "withdrawn"].includes(stateName) || credit && credit.state === "pending" && member
      ? `<div data-security-credit-banner class="flex flex-wrap items-center gap-3 rounded-md border border-primary px-4 py-3 text-[14px]"><i data-lucide="award" class="h-4 w-4 text-primary"></i><span class="min-w-0 flex-1">You are credited as <strong>${escapeHtml(securityCreditLabel(credit.type))}</strong> on this advisory. Accepted credits appear on your profile and in the security researchers ranking.</span>${button("credit-decision", "Accept credit", "bg-primary text-primary-foreground hover:bg-primary/90", " data-decision=\"accept\"")}${button("credit-decision", "Decline", "", " data-decision=\"decline\"")}</div>` : "";
    const affected = (advisory.affected || []).map((entry) => `<li class="py-1"><span class="font-semibold">${escapeHtml(entry.package || entry.path || "Repository")}</span>${entry.ecosystem ? ` <span class="text-muted-foreground">(${escapeHtml(entry.ecosystem)})</span>` : ""}${entry.path && entry.package ? ` <span class="font-mono text-[12px] text-muted-foreground">${escapeHtml(entry.path)}</span>` : ""}<div class="text-[12px] text-muted-foreground">Affected: ${escapeHtml(entry.vulnerable || "all versions")} · Patched: ${escapeHtml(entry.patched || "none yet")}</div></li>`).join("");
    const credits = (detail.credits || []).map((item) => `<li class="flex items-center gap-2 py-0.5">${issueActorAvatarHtml(item.user, item.user, "h-5 w-5")}${issuePersonNameHtml(item.user, "font-semibold")}<span class="text-[12px] text-muted-foreground">${escapeHtml(securityCreditLabel(item.type))}${member && item.state !== "accepted" ? ` · ${escapeHtml(item.state)}` : ""}</span>${perms.manage ? `<button type="button" data-security-action="credit-remove" data-user="${escapeHtml(item.user)}" aria-label="Remove credit" class="ml-auto text-muted-foreground hover:text-destructive"><i data-lucide="x" class="h-3.5 w-3.5"></i></button>` : ""}</li>`).join("");
    const creditForm = perms.manage ? `<form data-security-inline="credit" class="mt-2 grid gap-1.5"><input name="user" placeholder="username" class="${securityFieldClass()}" /><select name="type" class="${securityFieldClass()}">${SECURITY_CREDIT_TYPES.map(([key, label]) => `<option value="${key}">${label}</option>`).join("")}</select>${button("credit-add", "Add credit")}</form>` : "";
    const collaborators = (advisory.collaborators || []).map((name) => `<li class="flex items-center gap-2 py-0.5">${issueActorAvatarHtml(name, name, "h-5 w-5")}${issuePersonNameHtml(name, "")}${perms.manage ? `<button type="button" data-security-action="collaborator-remove" data-user="${escapeHtml(name)}" aria-label="Remove collaborator" class="ml-auto text-muted-foreground hover:text-destructive"><i data-lucide="x" class="h-3.5 w-3.5"></i></button>` : ""}</li>`).join("");
    const collaboratorForm = perms.manage ? `<form data-security-inline="collaborator" class="mt-2 flex gap-1.5"><input name="user" placeholder="username" class="${securityFieldClass()}" />${button("collaborator-add", "Add")}</form>` : "";
    const fixLinks = [
      fix.branch ? `<a class="font-mono text-primary hover:underline" href="${escapeHtml(repoLinkUrl(repo, "tree", "") + "?branch=" + encodeURIComponent(fix.branch))}">${escapeHtml(fix.branch)}</a>` : "",
      fix.pull ? `<a class="text-primary hover:underline" href="${escapeHtml(`${repoLinkUrl(repo)}/pulls/${fix.pull}`)}">#${escapeHtml(String(fix.pull))}</a>` : "",
      fix.commit ? `<a class="font-mono text-primary hover:underline" href="${escapeHtml(`${repoLinkUrl(repo)}/commits/${fix.commit}`)}">${escapeHtml(fix.commit.slice(0, 10))}</a>` : "",
    ].filter(Boolean).join(" · ");
    const ledgerBody = ledger.state === "recorded"
      ? `Signed and recorded in <a class="font-mono text-primary hover:underline" href="${escapeHtml(repoLinkUrl(repo, "blob", ledger.path))}">${escapeHtml(ledger.path)}</a> at <a class="font-mono text-primary hover:underline" href="${escapeHtml(`${repoLinkUrl(repo)}/commits/${ledger.commit}`)}">${escapeHtml(String(ledger.commit || "").slice(0, 10))}</a>`
      : ledger.state === "pending"
        ? `Signed; not yet written to the repository (${escapeHtml(ledger.error || "waiting")}). ${perms.manage ? button("ledger-retry", "Retry") : ""}`
        : "Recorded in the repository when it is published.";
    const description = advisory.description
      ? `<div class="px-4 py-4 ${MD_BODY_CLASS}">${renderRecordMarkdown(advisory.description, repo)}</div>`
      : `<p class="px-4 py-4 text-[14px] italic text-muted-foreground">No description provided.</p>`;
    const opener = advisory.reporter || advisory.createdBy || "";
    container.innerHTML = `
      <article data-security-advisory="${escapeHtml(advisory.id || "")}" class="grid gap-4 py-2">
        <a href="${escapeHtml(securityUrl(repo))}" data-security-nav="" class="text-[13px] text-primary hover:underline">&larr; Security</a>
        <header class="border-b border-border pb-4">
          <h1 class="text-[26px] font-normal leading-tight text-foreground [overflow-wrap:anywhere]">${escapeHtml(advisory.title || "")}</h1>
          <div class="mt-2 flex flex-wrap items-center gap-2 text-[14px] text-muted-foreground">
            ${securityStatePill(stateName)}
            ${securitySeverityPill(severity)}
            <span class="font-mono">${escapeHtml(advisory.id || "")}</span>
            ${advisory.cve ? `<span class="font-mono">${escapeHtml(advisory.cve)}</span>` : ""}
            ${opener && member ? `<span>${advisory.reporter ? "Reported" : "Opened"} by ${issuePersonNameHtml(opener)} ${escapeHtml(relativeTimeLabel(advisory.createdAt))}</span>` : ""}
            ${advisory.publishedAt ? `<span>Published ${escapeHtml(formatDate(advisory.publishedAt))}</span>` : ""}
          </div>
          ${actions.length ? `<div data-security-actions class="mt-3 flex flex-wrap gap-2">${actions.join("")}</div>` : ""}
          <p data-security-status class="mt-2 text-sm text-muted-foreground" role="status"></p>
        </header>
        ${creditBanner}
        ${warnings}
        ${member && stateName !== "published" && stateName !== "withdrawn" ? `<div class="flex items-center gap-2 rounded-md border border-border px-4 py-2 text-[13px] text-muted-foreground"><i data-lucide="lock" class="h-4 w-4"></i>Private: visible only to the reporter, the maintainers and the collaborators listed here.</div>` : ""}
        <div class="grid gap-8 lg:grid-cols-[minmax(0,1fr)_18rem]">
          <div class="grid min-w-0 gap-4 self-start">
            <div class="grid min-w-0 gap-4 sm:grid-cols-[40px_minmax(0,1fr)]">
              <span class="hidden sm:block">${opener && member ? issueActorAvatarHtml(opener, opener, "h-10 w-10") : `<span class="grid h-10 w-10 place-items-center rounded-full border border-border text-muted-foreground"><i data-lucide="shield" class="h-5 w-5"></i></span>`}</span>
              <div class="min-w-0 overflow-hidden rounded-md border border-border">
                <div class="border-b border-border bg-[rgb(var(--dashboard-subtle-rgb))] px-4 py-1.5 text-[14px] text-muted-foreground">Description</div>
                ${description}
              </div>
            </div>
            ${member ? securityTimeline(repo, detail) : ""}
            ${perms.comment ? `<form data-security-inline="comment" class="grid gap-2 sm:ml-14"><label class="text-[14px] font-semibold text-foreground" for="security-comment">Private comment</label><textarea id="security-comment" name="body" rows="4" class="w-full rounded-md border border-border bg-background px-2 py-1.5 text-[14px] text-foreground" placeholder="Only the people on this advisory can read comments. Markdown and @mentions work."></textarea><div>${button("comment", "Comment", "bg-primary text-primary-foreground hover:bg-primary/90")}</div></form>` : ""}
          </div>
          <aside class="grid content-start gap-3">
            ${securitySideSection("Severity", severity.vector ? `${securitySeverityPill(severity)}<div class="mt-1 break-all font-mono text-[12px] text-muted-foreground">${escapeHtml(severity.vector)}</div>` : securitySeverityPill(severity))}
            ${securitySideSection("Weaknesses", (advisory.cwes || []).length ? (advisory.cwes || []).map((cwe) => `<a class="mx-1 font-mono text-primary hover:underline" href="https://cwe.mitre.org/data/definitions/${escapeHtml(cwe.replace("CWE-", ""))}.html" rel="noopener" target="_blank">${escapeHtml(cwe)}</a>`).join("") : `<span class="text-muted-foreground">None</span>`)}
            ${securitySideSection("Affected products", affected ? `<ul>${affected}</ul>` : `<span class="text-muted-foreground">Not specified</span>`)}
            ${securitySideSection("Fix", fixLinks || `<span class="text-muted-foreground">No fix linked</span>`)}
            ${(advisory.references || []).length ? securitySideSection("References", `<ul>${advisory.references.map((url) => `<li class="truncate"><a class="text-primary hover:underline" rel="noopener nofollow" target="_blank" href="${escapeHtml(url)}">${escapeHtml(url)}</a></li>`).join("")}</ul>`) : ""}
            ${securitySideSection("Credits", (credits ? `<ul>${credits}</ul>` : `<span class="text-muted-foreground">None</span>`) + creditForm)}
            ${member ? securitySideSection("Collaborators", (collaborators ? `<ul>${collaborators}</ul>` : `<span class="text-muted-foreground">None</span>`) + collaboratorForm) : ""}
            ${member || stateName === "published" || stateName === "withdrawn" ? securitySideSection("Repository ledger", `<span class="text-[13px]">${ledgerBody}</span>`) : ""}
            ${detail.osvUrl ? securitySideSection("Data", `<a class="text-primary hover:underline" href="${escapeHtml(detail.osvUrl)}">OSV JSON</a>`) : ""}
          </aside>
        </div>
      </article>`;
    window.lucide?.createIcons();
  }

  async function runSecurityAction(repo, control) {
    const kind = control.dataset.securityAction;
    if (kind === "add-affected") {
      control.closest("form")?.querySelector("[data-security-affected-list]")?.insertAdjacentHTML("beforeend", securityAffectedRow());
      return;
    }
    const status = $("[data-security-status]");
    const detail = securityView.detail || {};
    const advisoryId = detail.advisory?.id || "";
    const url = securityApi(repo, `/advisories/${encodeURIComponent(advisoryId)}`);
    const inline = control.closest("[data-security-inline]");
    const setStatus = (text, bad = false) => {
      if (!status) return;
      status.textContent = text;
      status.className = `mt-2 text-sm ${bad ? "text-destructive" : "text-muted-foreground"}`;
    };
    control.disabled = true;
    try {
      if (kind === "reporting") {
        await securityRequest(securityApi(repo, "/settings"), { reporting: control.dataset.enable === "1" });
      } else if (kind === "state") {
        await securityRequest(url, { action: "state", state: control.dataset.state });
      } else if (kind === "publish" || kind === "withdraw") {
        if (kind === "publish" && (detail.warnings || []).includes("no_patched_version") && control.dataset.confirmed !== "1") {
          control.dataset.confirmed = "1";
          control.disabled = false;
          control.textContent = "Publish without a patched version";
          setStatus("No patched version is set. Click again to publish anyway.", true);
          return;
        }
        if (kind === "withdraw" && control.dataset.confirmed !== "1") {
          control.dataset.confirmed = "1";
          control.disabled = false;
          control.textContent = "Confirm withdrawal";
          return;
        }
        setStatus("Signing with this browser's key...");
        const result = await signSecurityAdvisory(repo, advisoryId, kind);
        if (result.ledger?.state !== "recorded") setStatus("Published. The repository ledger will be written when a host accepts the commit.");
      } else if (kind === "comment") {
        const body = inline.elements.body.value.trim();
        if (!body) return;
        await securityRequest(url, { action: "comment", body });
      } else if (kind === "credit-add") {
        await securityRequest(url, { action: "credit", user: inline.elements.user.value.trim(), type: inline.elements.type.value });
      } else if (kind === "credit-remove") {
        await securityRequest(url, { action: "credit", user: control.dataset.user, remove: true });
      } else if (kind === "collaborator-add") {
        await securityRequest(url, { action: "collaborator", name: inline.elements.user.value.trim() });
      } else if (kind === "collaborator-remove") {
        await securityRequest(url, { action: "collaborator", name: control.dataset.user, remove: true });
      } else if (kind === "ledger-retry") {
        await securityRequest(url, { action: "ledger-retry" });
      } else if (kind === "credit-decision") {
        const me = String(state.session?.nodeName || "").toLowerCase();
        await securityRequest(`/api/security-advisories/credits/${encodeURIComponent(me)}`, { advisoryId, decision: control.dataset.decision });
      }
      await loadRepoSecurity(repo);
    } catch (error) {
      setStatus(error.message, true);
      control.disabled = false;
    }
  }

  async function renderProfileSecurityCredits(session) {
    if (profileMarkupOwnedByPublicProfile(session)) return;
    const name = String(session?.nodeName || "").trim().toLowerCase();
    const section = $("[data-profile-security-credits]");
    if (!section || !name) return;
    section.dataset.profileSecurityCreditsFor = name;
    let body = null;
    try {
      body = await fetchJson(`/api/security-advisories/credits/${encodeURIComponent(name)}`, { fresh: true });
    } catch (_) {
      return;
    }
    if (section.dataset.profileSecurityCreditsFor !== name) return;
    const own = name === String(state.session?.nodeName || "").toLowerCase();
    const credits = (body?.credits || []).filter((item) => own ? item.state !== "declined" : item.state === "accepted");
    section.hidden = credits.length === 0;
    const count = section.querySelector("[data-profile-security-credits-count]");
    if (count) count.textContent = String(body?.count || 0);
    const list = section.querySelector("[data-profile-security-credits-list]");
    if (!list) return;
    list.innerHTML = credits.map((item) => `<li class="grid gap-0.5">
      <div class="flex items-center gap-2">${securitySeverityPill({ level: item.severity })}<a href="${escapeHtml(item.href || "#")}" class="min-w-0 truncate font-semibold text-foreground hover:text-accent hover:underline">${escapeHtml(item.title || item.id)}</a></div>
      <div class="text-[12px] text-muted-foreground">${escapeHtml(securityCreditLabel(item.type))} · ${escapeHtml(item.repository || "")} · <span class="font-mono">${escapeHtml(item.id)}</span></div>
      ${own && item.state === "pending" ? `<div class="flex gap-2 pt-1"><button type="button" data-profile-security-credit="accept" data-advisory="${escapeHtml(item.id)}" class="inline-flex h-7 items-center rounded-md bg-primary px-2 text-[12px] font-medium text-primary-foreground hover:bg-primary/90">Accept credit</button><button type="button" data-profile-security-credit="decline" data-advisory="${escapeHtml(item.id)}" class="inline-flex h-7 items-center rounded-md border border-border px-2 text-[12px] text-foreground hover:bg-secondary">Decline</button></div>` : ""}
    </li>`).join("");
    list.querySelectorAll("[data-profile-security-credit]").forEach((control) => {
      control.addEventListener("click", async () => {
        control.disabled = true;
        try {
          await securityRequest(`/api/security-advisories/credits/${encodeURIComponent(name)}`, {
            advisoryId: control.dataset.advisory, decision: control.dataset.profileSecurityCredit,
          });
        } catch (_) {}
        void renderProfileSecurityCredits(session);
      });
    });
    window.lucide?.createIcons();
  }
