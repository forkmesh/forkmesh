  // Contributor rewards are reviewed in the normal issue and pull-request
  // workflows. There is intentionally no separate claim form: the repository
  // owner records an estimate on the accepted issue/PR and pays it directly
  // from their local Qt wallet.

  function microjobIssueNoticeHtml() {
    return `<div class="rounded-md border border-dashed border-border bg-secondary/20 px-3 py-2 text-[11px] text-muted-foreground">
      <span class="font-semibold text-foreground">This report may receive a small SOL reward.</span>
      Submit it here normally. The repository owner checks whether it is new and useful, records any estimate on the issue, and pays the reporter directly. No second claim is needed.
    </div>`;
  }

  function microjobPullNoticeHtml() {
    return `<div class="rounded-md border border-dashed border-border bg-secondary/20 px-3 py-2 text-[11px] text-muted-foreground">
      <span class="font-semibold text-foreground">Merged pull requests may receive a small SOL reward.</span>
      Open the pull request normally. The repository owner reviews it, records any merge-contingent estimate on the PR, and pays it after merge. No second claim is needed.
    </div>`;
  }

  function contributorRewardCardHtml(kind) {
    const issue = kind === "issue";
    const href = issue
      ? "/forkmesh/forkmesh/issues/new"
      : "/forkmesh/forkmesh/pulls/new";
    return `<article class="px-4 sm:px-5 py-5">
      <div class="flex flex-wrap items-start justify-between gap-4">
        <div class="min-w-0 max-w-2xl">
          <p class="text-sm font-semibold text-foreground">${issue ? "Report a useful, genuinely new issue" : "Open a focused pull request"}</p>
          <p class="mt-1 text-sm text-muted-foreground">${issue
            ? "Use the repository's ordinary issue form. Include reproducible steps, what happened, what you expected, and enough context to distinguish it from existing reports."
            : "Use the repository's ordinary pull-request flow. Keep the change focused, explain what it fixes, and include verification evidence."}</p>
          <p class="mt-2 text-xs text-muted-foreground">${issue
            ? "After triage, the owner may attach an estimated reporting reward—typically 0.001–0.005 SOL—to the accepted issue."
            : "After review, the owner may attach a small SOL estimate to the PR. It remains conditional and is payable only after merge."}</p>
        </div>
        <a href="${href}" class="inline-flex shrink-0 items-center gap-1.5 rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground hover:opacity-90">
          <i data-lucide="${issue ? "circle-dot" : "git-pull-request"}" class="h-3.5 w-3.5"></i>
          ${issue ? "Submit an issue" : "Open a pull request"}
        </a>
      </div>
    </article>`;
  }

  function renderMicrojobsPage() {
    const session = $("[data-microjobs-session]");
    if (session) {
      session.innerHTML = `<p class="text-sm text-muted-foreground">There is no reward-claim form. Submit the work where maintainers already review it, then follow any owner-authored estimate in <a class="text-accent underline" href="/ledger">Ledger → Pending payments</a>. Add a public Solana payout address in <a class="text-accent underline" href="/dashboard/settings">Settings</a> so an owner can pay you.</p>`;
    }
    const earned = $("[data-microjobs-earned]");
    if (earned) earned.textContent = "Direct owner payments";
    const notice = $("[data-microjobs-notice]");
    if (notice) {
      notice.className = "hidden";
      notice.textContent = "";
    }
    const catalog = $("[data-microjobs-catalog]");
    if (catalog) {
      catalog.innerHTML = contributorRewardCardHtml("issue")
        + contributorRewardCardHtml("pull");
    }
    const claims = $("[data-microjobs-claims]");
    if (claims) {
      claims.innerHTML = `<div class="px-4 sm:px-5 py-6 text-sm text-muted-foreground">Rewards are attached to their issue or pull request—not submitted as separate claims. <a class="text-accent underline" href="/ledger">See pending and paid records in the public ledger.</a></div>`;
    }
    const summary = $("[data-microjobs-claims-summary]");
    if (summary) summary.textContent = "No separate submissions to manage";
    const policy = $("[data-microjobs-policy]");
    if (policy) {
      policy.innerHTML = `<p class="text-xs text-muted-foreground">Estimates are discretionary thank-yous set after review. They are not wages, balances, or guaranteed payments. A ledger row marked paid includes the finalized public Solana transaction.</p>`;
    }
    window.lucide?.createIcons();
  }

  async function loadMicrojobs() {
    renderMicrojobsPage();
  }

  function initMicrojobsPage() {
    if (!$("[data-microjobs-panel]")) return;
    renderMicrojobsPage();
    $("[data-microjobs-refresh]")?.addEventListener("click", renderMicrojobsPage);
  }
