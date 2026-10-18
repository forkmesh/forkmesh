  // `parentNumber` files a sub-issue: the description starts with the same
  // portable "Parent issue: #N" line a converted checklist row gets.
  function openIssueCompose(repo, { parentNumber = 0 } = {}) {
    const container = $("[data-repo-issues]");
    if (!container || !repo) return;
    const canAssignAgent = sessionCanAssignAgent(repo);
    const parent = Number(parentNumber) > 0 ? Number(parentNumber) : 0;
    const initialBody = parent ? `Parent issue: #${parent}\n\n` : "";
    container.innerHTML = `
      <form data-repo-issue-form${parent ? ` data-repo-issue-parent="${parent}"` : ""} class="grid gap-3 border-t border-border bg-background p-4">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <span class="inline-flex items-center gap-2 text-sm font-semibold text-foreground"><i data-lucide="${parent ? "list-tree" : "circle-dot"}" class="h-4 w-4 text-muted-foreground"></i>${parent ? `New sub-issue of #${parent}` : "New issue"}</span>
          <button type="button" data-repo-issue-cancel class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to issues</button>
        </div>
        <label class="grid gap-1 text-xs font-medium text-muted-foreground">Title
          <input data-repo-issue-title type="text" required maxlength="240" placeholder="Short, descriptive title" class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent" />
        </label>
        <label class="grid gap-1 text-xs font-medium text-muted-foreground">Description
          <textarea data-repo-issue-body rows="6" placeholder="Describe the issue. Markdown is supported." class="rounded-md border border-border bg-background px-3 py-2 text-sm text-foreground outline-none focus:border-accent">${escapeHtml(initialBody)}</textarea>
        </label>
        <div class="grid gap-2 sm:grid-cols-2">
          <label class="grid gap-1 text-xs font-medium text-muted-foreground">Milestone
            <input data-repo-issue-milestone type="text" maxlength="120" placeholder="Optional (e.g. v1.0)" class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent" />
          </label>
          <label class="grid gap-1 text-xs font-medium text-muted-foreground">Project
            <input data-repo-issue-project type="text" maxlength="120" placeholder="Optional (e.g. Roadmap)" class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent" />
          </label>
        </div>
        ${microjobIssueNoticeHtml()}
        <div class="grid gap-2">
          <div class="flex flex-wrap items-center gap-2">
            <button type="button" data-repo-issue-attach-image class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="paperclip" class="h-3.5 w-3.5"></i>Attach images</button>
            <span data-repo-issue-attach-hint class="text-[11px] text-muted-foreground"></span>
          </div>
          <input type="file" data-repo-issue-file-input multiple accept="image/png,image/jpeg,image/gif,image/webp" class="hidden" />
          <div data-repo-issue-attachments class="grid gap-2 sm:grid-cols-2"></div>
        </div>
        ${canAssignAgent ? `
        <div class="grid gap-2">
          <label class="flex items-center gap-2 text-xs text-muted-foreground">
            <input type="checkbox" data-repo-issue-assign-agent class="h-3.5 w-3.5 rounded border-border" />
            <span>Assign to agent - once filed, ${sessionOwnsRepo(repo) ? "your" : escapeHtml(repo.owner || "the owner") + "'s"} node starts a coding agent on it automatically</span>
          </label>
          <div class="ml-5 flex flex-wrap items-center gap-4">
            <label class="flex items-center gap-2 text-xs text-muted-foreground">
              Agent
              <select data-repo-issue-agent-provider disabled class="h-7 rounded-md border border-border bg-background px-2 text-xs text-foreground outline-none focus:border-accent disabled:opacity-50">
                ${AGENT_PROVIDER_OPTIONS.map((opt) => `<option value="${escapeHtml(opt.value)}">${escapeHtml(opt.label)}</option>`).join("")}
              </select>
            </label>
            <label class="flex items-center gap-2 text-xs text-muted-foreground">
              Model
              <select data-repo-issue-agent-model disabled class="h-7 rounded-md border border-border bg-background px-2 text-xs text-foreground outline-none focus:border-accent disabled:opacity-50">
                ${issueAgentModelOptions(AGENT_PROVIDER_OPTIONS[0].value).map((opt) => `<option value="${escapeHtml(opt.value)}">${escapeHtml(opt.label)}</option>`).join("")}
              </select>
            </label>
          </div>
        </div>` : ""}
        <div class="flex flex-wrap items-center justify-between gap-3">
          <div class="flex min-w-0 flex-col gap-1">
            ${composeIdentityHtml(state.session, "Filing")}
            <span data-repo-issue-hint class="text-[11px] text-muted-foreground">An eligible online mirror will add signed issues directly to this repository.</span>
          </div>
          <button type="submit" data-repo-issue-submit title="Submit issue (Ctrl+Enter / ⌘+Enter)" aria-keyshortcuts="Control+Enter Meta+Enter" class="inline-flex h-9 items-center gap-2 rounded-md bg-cta px-4 text-sm font-medium text-cta-foreground transition-colors hover:bg-cta/90 disabled:opacity-50"><i data-lucide="send" class="h-4 w-4"></i>Submit issue</button>
        </div>
      </form>`;
    window.lucide?.createIcons();

    const form = container.querySelector("[data-repo-issue-form]");
    const attachButton = container.querySelector("[data-repo-issue-attach-image]");
    const fileInput = container.querySelector("[data-repo-issue-file-input]");
    const bodyInput = container.querySelector("[data-repo-issue-body]");
    const attachHint = container.querySelector("[data-repo-issue-attach-hint]");
    const attachmentsList = container.querySelector("[data-repo-issue-attachments]");
    const assignAgentInput = container.querySelector("[data-repo-issue-assign-agent]");
    const agentModelInput = container.querySelector("[data-repo-issue-agent-model]");
    const agentProviderInput = container.querySelector("[data-repo-issue-agent-provider]");
    assignAgentInput?.addEventListener("change", () => {
      if (agentModelInput) agentModelInput.disabled = !assignAgentInput.checked;
      if (agentProviderInput) agentProviderInput.disabled = !assignAgentInput.checked;
    });
    agentProviderInput?.addEventListener("change", () => {
      if (!agentModelInput) return;
      agentModelInput.innerHTML = issueAgentModelOptions(agentProviderInput.value)
        .map((opt) => `<option value="${escapeHtml(opt.value)}">${escapeHtml(opt.label)}</option>`)
        .join("");
    });
    // Queued images: a short placeholder (not the data URL) is inserted into
    // the body textarea so it stays readable/editable; the real data: URL is
    // swapped in right before signing (handleIssueComposeSubmit).
    const images = [];
    if (form) form._pendingIssueImages = images;

    const setAttachHint = (text, tone) => {
      if (!attachHint) return;
      attachHint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : "text-muted-foreground"}`;
      attachHint.textContent = text;
    };
    let editingImageId = "";
    const imageBudget = (except) => Math.min(ISSUE_IMAGE_MAX_BYTES, ISSUE_IMAGE_MAX_TOTAL_BYTES
      - images.reduce((sum, img) => sum + (img === except ? 0 : img.size), 0));
    const renderAttachmentChips = () => {
      if (!attachmentsList) return;
      attachmentsList.innerHTML = images.map((img) => (img.id === editingImageId
        ? `<div data-repo-issue-attachment-markup="${escapeHtml(img.id)}" class="sm:col-span-2"></div>`
        : `
        <figure data-repo-issue-attachment="${escapeHtml(img.id)}" class="group relative m-0 overflow-hidden rounded-md border border-border bg-secondary/30">
          <button type="button" data-repo-issue-attachment-edit="${escapeHtml(img.id)}" title="Mark up ${escapeHtml(img.name)}" class="block w-full cursor-crosshair">
            <img src="${escapeHtml(img.dataUrl)}" alt="${escapeHtml(img.name)}" class="mx-auto block max-h-56 max-w-full object-contain" />
          </button>
          <figcaption class="pointer-events-none absolute inset-x-0 bottom-0 flex items-center gap-2 bg-background/80 px-2 py-1 text-[11px] text-foreground">
            <span class="min-w-0 truncate font-medium">${escapeHtml(img.name)}</span>
            <span class="ml-auto shrink-0 text-muted-foreground">${img.shapes?.length ? "Marked up · " : ""}${escapeHtml(formatSize(img.size))}</span>
          </figcaption>
          <span class="absolute right-1.5 top-1.5 flex gap-1 opacity-0 transition-opacity group-focus-within:opacity-100 group-hover:opacity-100 [@media(hover:none)]:opacity-100">
            <button type="button" data-repo-issue-attachment-edit="${escapeHtml(img.id)}" class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border bg-background px-2 text-xs font-medium text-foreground shadow-sm hover:bg-secondary"><i data-lucide="pencil" class="h-3.5 w-3.5"></i>Mark up</button>
            <button type="button" data-repo-issue-attachment-remove="${escapeHtml(img.id)}" class="inline-flex h-7 w-7 items-center justify-center rounded-md border border-border bg-background text-muted-foreground shadow-sm hover:text-destructive" aria-label="Remove ${escapeHtml(img.name)}"><i data-lucide="x" class="h-3.5 w-3.5"></i></button>
          </span>
        </figure>`)).join("");
      window.lucide?.createIcons();
      const editing = images.find((img) => img.id === editingImageId);
      const slot = attachmentsList.querySelector("[data-repo-issue-attachment-markup]");
      if (!editing || !slot) return;
      const close = () => {
        editingImageId = "";
        renderAttachmentChips();
        attachmentsList.querySelector(`[data-repo-issue-attachment-edit="${CSS.escape(editing.id)}"]`)?.focus();
      };
      mountImageMarkupEditor(slot, {
        src: editing.originalUrl,
        name: editing.name,
        shapes: editing.shapes || [],
        maxBytes: imageBudget(editing),
        encodeOptions: ISSUE_IMAGE_FIT_OPTIONS,
        onCancel: close,
        onDone: ({ dataUrl, size, shapes }) => {
          editing.dataUrl = dataUrl;
          editing.size = size;
          editing.shapes = shapes;
          close();
        },
      });
    };
    attachmentsList?.addEventListener("click", (event) => {
      const editButton = event.target.closest("[data-repo-issue-attachment-edit]");
      if (editButton) {
        editingImageId = editButton.dataset.repoIssueAttachmentEdit;
        renderAttachmentChips();
        return;
      }
      const removeButton = event.target.closest("[data-repo-issue-attachment-remove]");
      if (!removeButton) return;
      const id = removeButton.dataset.repoIssueAttachmentRemove;
      const index = images.findIndex((img) => img.id === id);
      if (index < 0) return;
      const [removed] = images.splice(index, 1);
      if (removed.originalUrl.startsWith("blob:")) URL.revokeObjectURL(removed.originalUrl);
      if (bodyInput) bodyInput.value = bodyInput.value.split(`\n![${removed.name}](${removed.id})\n`).join("\n");
      renderAttachmentChips();
    });

    // Shared by both the file picker and clipboard paste: validate, embed (or
    // crop/compress if oversized), then insert a placeholder into the body at
    // the caret so pasted screenshots land right where the cursor was.
    const addIssueImageFile = async (file, rawName) => {
      const name = rawName.replace(/[[\]]/g, "_");
      if (!file.type.startsWith("image/")) {
        setAttachHint(`${name}: not an image.`, "bad");
        return;
      }
      if (images.length >= ISSUE_IMAGE_MAX_COUNT) {
        setAttachHint(`You can attach up to ${ISSUE_IMAGE_MAX_COUNT} images.`, "bad");
        return;
      }
      if (file.size > ISSUE_IMAGE_RAW_MAX_BYTES) {
        setAttachHint(`${name} is too large to attach (max ${formatSize(ISSUE_IMAGE_RAW_MAX_BYTES)}).`, "bad");
        return;
      }
      const total = images.reduce((sum, img) => sum + img.size, 0);
      const budget = Math.min(ISSUE_IMAGE_MAX_BYTES, ISSUE_IMAGE_MAX_TOTAL_BYTES - total);
      if (budget <= 0) {
        setAttachHint("Attached images already use up the issue's size limit - remove one to add another.", "bad");
        return;
      }
      let dataUrl;
      let size;
      let originalUrl = URL.createObjectURL(file);
      if (file.size <= budget) {
        try {
          dataUrl = await readAsDataUrl(file);
          size = file.size;
        } catch (_) {
          URL.revokeObjectURL(originalUrl);
          setAttachHint(`Could not read ${name}.`, "bad");
          return;
        }
      } else {
        setAttachHint(`Compressing ${name}…`);
        let result = await fitImageWithinBytes(originalUrl, budget, ISSUE_IMAGE_FIT_OPTIONS);
        if (!result) {
          setAttachHint(`${name} is ${formatSize(file.size)} - crop or compress it to fit under ${formatSize(budget)}.`);
          result = await openImageResizeModal(file, budget);
          URL.revokeObjectURL(originalUrl);
          if (!result) {
            setAttachHint("");
            return;
          }
          originalUrl = result.dataUrl;
        }
        dataUrl = result.dataUrl;
        size = result.size;
      }
      const id = `forkmesh-pending-image:${Date.now().toString(36)}${images.length}`;
      images.push({ id, name, dataUrl, size, originalUrl, shapes: [] });
      const start = bodyInput?.selectionStart ?? bodyInput?.value.length ?? 0;
      const end = bodyInput?.selectionEnd ?? start;
      if (bodyInput) {
        const insertion = `\n![${name}](${id})\n`;
        bodyInput.value = bodyInput.value.slice(0, start) + insertion + bodyInput.value.slice(end);
        const cursor = start + insertion.length;
        bodyInput.selectionStart = bodyInput.selectionEnd = cursor;
      }
      setAttachHint("");
    };

    if (attachButton && fileInput) {
      attachButton.addEventListener("click", () => fileInput.click());
      fileInput.addEventListener("change", async () => {
        const files = Array.from(fileInput.files || []);
        fileInput.value = "";
        for (const file of files) await addIssueImageFile(file, file.name);
        renderAttachmentChips();
      });
    }
    bodyInput?.addEventListener("paste", async (event) => {
      const imageItems = Array.from(event.clipboardData?.items || [])
        .filter((item) => item.kind === "file" && item.type.startsWith("image/"));
      if (!imageItems.length) return;
      // A pasted screenshot has no useful text form, so claim the paste instead
      // of letting the browser also dump it in as an inline object/blank text.
      event.preventDefault();
      for (const item of imageItems) {
        const file = item.getAsFile();
        if (file) await addIssueImageFile(file, file.name || "screenshot.png");
      }
      renderAttachmentChips();
    });
    container.querySelector("[data-repo-issue-title]")?.focus();
  }

  // What a failed web submission actually ran into, for the sentence the form
  // shows. "Please try again" on its own left #685 undiagnosable: a rejected
  // signature, a dropped connection, a 5xx page with no JSON body and a browser
  // without Ed25519 all read identically, so a bug report could never say which.
  function webSubmissionErrorDetail(error) {
    const status = Number(error?.status) || 0;
    const code = String(error?.code || "").trim();
    if (code) return status ? `${code}, HTTP ${status}` : code;
    if (status) return `HTTP ${status}`;
    if (isWebSubmissionNetworkError(error)) return "network error";
    if (isWebSubmissionSigningUnsupported(error)) return "signing unsupported";
    return String(error?.message || error?.name || "unknown error").slice(0, 80);
  }

  // fetch() rejects with a bare TypeError when the request never got an answer;
  // the wording differs per engine (Chromium, WebKit, Gecko).
  function isWebSubmissionNetworkError(error) {
    return error?.name === "TypeError"
      && /failed to fetch|load failed|networkerror|network request failed/i.test(String(error?.message || ""));
  }

  // Issues are Ed25519-signed in the browser; an engine without Ed25519 in
  // WebCrypto rejects the key generation or import with NotSupportedError.
  function isWebSubmissionSigningUnsupported(error) {
    return error?.name === "NotSupportedError";
  }

  function webIssueSubmitErrorText(error) {
    const code = String(error?.message || "");
    const status = Number(error?.status) || 0;
    if (code === "inbox_full") return "Issue delivery is temporarily full. Try again later.";
    if (code === "author_quota") return "You've reached the submission limit for this repository.";
    if (code === "issue_too_large" || code === "attachment_too_large"
        || code === "too_many_attachments" || status === 413) {
      return "The description is too large - please shorten it or attach smaller images.";
    }
    if (code === "number_unavailable") return "A durable issue number could not be reserved. Please try again.";
    if (code === "not_authorized") return "Only the repository owner or an admin can assign issues to an agent.";
    if (code === "bad_signature") return "The server could not verify this browser's signature on the issue. Reload the page and try again.";
    if (code === "alias_unresolved") return "This repository's address could not be resolved right now. Try again in a moment.";
    if (status === 429) return "Too many submissions in a short time. Wait a minute and try again.";
    if (isWebSubmissionNetworkError(error)) return "Could not reach ForkMesh - check your connection and try again.";
    if (isWebSubmissionSigningUnsupported(error)) {
      return "This browser cannot sign issues (no Ed25519 support). Update the browser, or file the issue from the desktop app.";
    }
    return `Could not send the issue (${webSubmissionErrorDetail(error)}). Please try again.`;
  }

  // The same sentence, for the pull composer. A web pull request is signed
  // from a mirror's comparison of the two branches, so the two halves of the
  // submission fail for entirely different reasons and must not share a
  // sentence: a failed comparison signed and sent nothing (retry is free and
  // may well work a moment later), while a rejected delivery carries a signed
  // proposal the relay refused. ``base``/``head`` name the branches the
  // comparison could not resolve and ``target`` the repository the proposal is
  // addressed to - for a fork that is the parent, not the repository the form
  // lives on.
  function webPullSubmitErrorText(error, base, head, target) {
    const code = String(error?.message || "");
    if (code === "comparison_unavailable") {
      const status = Number(error?.status) || 0;
      const cause = error?.cause;
      // The compare page says the same thing about the same 404
      // (repoCompareErrorText); a form cannot tell the reader to "refresh".
      if (status === 404) {
        return `Nothing was sent: ${base} or ${head} is not on a serving mirror yet. A branch that was just pushed can take a sync to arrive - try again in a moment.`;
      }
      if (status === 413) {
        return "Nothing was sent: this comparison is larger than a mirror will serve in one read. Narrow the branches and try again.";
      }
      if (isWebSubmissionNetworkError(cause)) {
        return "Could not reach ForkMesh - check your connection and try again.";
      }
      return `Nothing was sent: these branches could not be compared on a live mirror (${webSubmissionErrorDetail(cause || error)}). Try again in a moment.`;
    }
    if (code === "read_only_mirror") return String(error?.data?.message || REPO_READ_ONLY_MIRROR_HINT);
    if (code === "inbox_full") return "The maintainer's inbox is full. Try again later.";
    if (code === "author_quota") return "You've reached the submission limit for this repository.";
    if (code === "pull_too_large") return "The submission is too large.";
    if (code === "number_unavailable") return "A durable pull-request number could not be reserved. Please try again.";
    if (code === "no_changes") return "Those branches do not contain any changes to propose.";
    if (code === "invalid_comparison") return "The mirror returned an invalid branch comparison.";
    if (code === "bad_signature") return "Could not verify the pull request's signature.";
    // The delivery route answers 404 for a repository it has no public record
    // of - a private or unpublished address, or a fork parent this catalog
    // cannot resolve. "Try again" is the one thing that cannot help there.
    if (code === "not_found" || Number(error?.status) === 404) {
      return `ForkMesh has no public record of ${target || "that repository"} to deliver this pull request to. It may be private, or not published yet.`;
    }
    if (isWebSubmissionNetworkError(error)) {
      return "Could not reach ForkMesh - check your connection and try again.";
    }
    if (isWebSubmissionSigningUnsupported(error)) {
      return "This browser cannot sign pull requests (no Ed25519 support). Update the browser, or open the pull request from the desktop app.";
    }
    return `Could not send the pull request (${webSubmissionErrorDetail(error)}). Please try again.`;
  }

  async function handleIssueComposeSubmit(repo, form) {
    if (!repo || !form) return;
    const titleInput = form.querySelector("[data-repo-issue-title]");
    const bodyInput = form.querySelector("[data-repo-issue-body]");
    const milestoneInput = form.querySelector("[data-repo-issue-milestone]");
    const projectInput = form.querySelector("[data-repo-issue-project]");
    const submit = form.querySelector("[data-repo-issue-submit]");
    const assignAgentInput = form.querySelector("[data-repo-issue-assign-agent]");
    const agentModelInput = form.querySelector("[data-repo-issue-agent-model]");
    const agentProviderInput = form.querySelector("[data-repo-issue-agent-provider]");
    const hint = form.querySelector("[data-repo-issue-hint]");
    const setHint = (text, tone) => {
      if (hint) hint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-muted-foreground" : "text-muted-foreground"}`;
      if (hint) hint.textContent = text;
    };
    const title = String(titleInput?.value || "").trim();
    if (!title) {
      setHint("Enter a title for the issue.", "bad");
      titleInput?.focus();
      return;
    }
    const assignAgent = Boolean(assignAgentInput?.checked);
    const agentModel = assignAgent ? String(agentModelInput?.value || "") : "";
    const agentProvider = assignAgent ? String(agentProviderInput?.value || "") : "";
    const milestone = String(milestoneInput?.value || "").trim();
    const project = String(projectInput?.value || "").trim();
    // Swap each attached image's short placeholder back out for its real
    // data: URL now, right before signing - the signed content hash has to
    // cover exactly what gets sent.
    let body = String(bodyInput?.value || "");
    const images = form._pendingIssueImages || [];
    for (const img of images) body = body.split(img.id).join(img.dataUrl);
    setSubmitBusy(submit, true, "Submitting issue…");
    const updateSubmissionProgress = ({ phase, route, retry, number, attempt, attempts, lastError } = {}) => {
      if (phase === "signing") {
        setHint("Preparing and signing your issue…");
      } else if (phase === "delivering" && route === "mirror") {
        setHint(retry
          ? "That mirror did not respond — trying another available mirror…"
          : "Sending your signed issue to an available mirror…");
      } else if (phase === "delivering" && retry) {
        // Say what the last attempt ran into and how far along the bounded
        // retries are, so a slow repository never reads as a frozen form (#698).
        const failure = lastError?.code === "timeout" ? "did not answer in time"
          : lastError?.code === "network_error" ? "could not be reached"
          : lastError ? `was unavailable (${webSubmissionErrorDetail(lastError)})`
          : "did not answer";
        const progress = attempt && attempts ? ` (attempt ${attempt} of ${attempts})` : "";
        setHint(`The repository ${failure} — retrying the same signed issue${progress}…`);
      } else if (phase === "delivering") {
        setHint("Sending your signed issue to the repository…");
      } else if (phase === "accepted") {
        setHint(Number.isInteger(number) && number > 0
          ? `Issue #${number} accepted — synchronizing it with the repository…`
          : "Issue accepted — synchronizing it with the repository…");
      }
    };
    updateSubmissionProgress({ phase: "signing" });
    try {
      const result = await submitWebIssue(
        repo, title, body, assignAgent, agentModel, agentProvider,
        { milestone, project }, updateSubmissionProgress);
      const number = Number(result.number || 0);
      if (!Number.isInteger(number) || number <= 0) {
        throw new Error("number_unavailable");
      }
      const item = {
        number,
        title,
        status: "open",
        state: "open",
        author: state.session?.nodeName || "you",
        date: "just now",
        meta: [milestone && `milestone: ${milestone}`, project && `project: ${project}`].filter(Boolean).join(" · "),
        milestone,
        body,
        wantsAgent: assignAgent,
      };
      const alreadyOpen = state.issuesView.items.some(
        (issue) => Number(issue.number) === number && issue.status !== "closed");
      state.issuesView.items = [
        item,
        ...state.issuesView.items.filter(
          (issue) => Number(issue.number) !== number),
      ];
      if (!alreadyOpen) countFiledWebIssues(1);
      rememberOptimisticRepoRecord(repo, "issues", number, {
        values: {
          title,
          status: "open",
          author: result.event.author,
          authorName: result.event.authorName || item.author,
          ts: result.event.ts,
          milestone,
          project,
          wantsAgent: assignAgent,
        },
        body,
        issueEvents: [result.event],
        issueCreator: result.event.author,
      });
      if (titleInput) titleInput.value = "";
      if (bodyInput) bodyInput.value = "";
      if (milestoneInput) milestoneInput.value = "";
      if (projectInput) projectInput.value = "";
      images.length = 0;
      form.querySelector("[data-repo-issue-attachments]")?.replaceChildren();
      setSubmitBusy(submit, false);
      navigateHistory(repoRecordUrl(repo, "issues", number, "conversation"));
      await loadRepoRecordDetail(repo, "issues", number);
    } catch (error) {
      setSubmitBusy(submit, false);
      console.error("[forkmesh] issue submission failed", error);
      const code = String(error?.message || "");
      const status = Number(error?.status || 0);
      // The retrying signer reports its own outcomes (#685); every other
      // rejection is named by webIssueSubmitErrorText() (#162). The typed
      // draft survives either way.
      const reason =
        code === "signing_unavailable" ? "This browser cannot sign issues (Ed25519 is unsupported). Update your browser and try again."
          : code === "bad_signature" ? "The issue signature was rejected. Reload the page and try again."
          : code === "network_error" ? "Could not reach ForkMesh after several attempts - check your connection."
          : code === "timeout" ? "The repository did not answer in time after several attempts. The issue may still arrive - check the issues list before sending it again."
          : code === "invalid_json" ? "The description is too large - please shorten it or attach smaller images."
          : status >= 500 && (!code || code === `HTTP ${status}`) ? `Could not send the issue (HTTP ${status}).`
          : webIssueSubmitErrorText(error);
      setHint(`${reason} Your draft is still here.`, "bad");
    }
  }

  // --- CSV issue import (adhoc #188) ---------------------------------------
  // Canonical column order for the downloadable template and the parsed import.
  // Unknown columns are ignored; missing ones default to empty. labels and
  // assignees hold multiple values separated by ";" so commas stay free to act
  // as the CSV field delimiter.
  const ISSUE_CSV_COLUMNS = ["title", "body", "milestone", "project", "labels", "priority", "assignees"];
  const ISSUE_CSV_TEMPLATE =
    "title,body,milestone,project,labels,priority,assignees\n" +
    "\"Example: crash on startup\",\"Steps to reproduce...\",v1.0,Roadmap,\"bug;crash\",2,\"alice;bob\"\n" +
    "\"Another issue\",\"Short description here\",,,,0,\n";

  function downloadIssueCsvTemplate() {
    const blob = new Blob([ISSUE_CSV_TEMPLATE], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = "forkmesh-issues-template.csv";
    document.body.appendChild(anchor);
    anchor.click();
    anchor.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  // Minimal RFC-4180-ish CSV parser: handles quoted fields, "" escapes, and
  // commas/newlines inside quotes. Returns an array of rows (arrays of cells).
  function parseCsv(text) {
    const rows = [];
    let row = [];
    let field = "";
    let inQuotes = false;
    const src = String(text || "").replace(/\r\n?/g, "\n");
    for (let i = 0; i < src.length; i++) {
      const ch = src[i];
      if (inQuotes) {
        if (ch === '"') {
          if (src[i + 1] === '"') { field += '"'; i++; }
          else inQuotes = false;
        } else field += ch;
      } else if (ch === '"') {
        inQuotes = true;
      } else if (ch === ",") {
        row.push(field); field = "";
      } else if (ch === "\n") {
        row.push(field); field = "";
        rows.push(row); row = [];
      } else field += ch;
    }
    if (field.length || row.length) { row.push(field); rows.push(row); }
    // Drop trailing blank lines so a file ending in a newline doesn't add a row.
    return rows.filter((r) => r.some((cell) => String(cell).trim() !== ""));
  }

  // Map parsed CSV rows to issue submission objects. When the first row is a
  // recognizable header (has a "title" column) its names decide the mapping;
  // otherwise the file is treated as headerless in canonical column order.
  function csvRowsToIssues(rows) {
    if (!rows.length) return [];
    const header = rows[0].map((h) => String(h).trim().toLowerCase());
    const idx = {};
    ISSUE_CSV_COLUMNS.forEach((name) => { idx[name] = header.indexOf(name); });
    const hasHeader = idx.title >= 0;
    const dataRows = hasHeader ? rows.slice(1) : rows;
    if (!hasHeader) ISSUE_CSV_COLUMNS.forEach((name, i) => { idx[name] = i; });
    const cell = (r, name) => (idx[name] >= 0 ? String(r[idx[name]] ?? "").trim() : "");
    const splitList = (v) => v.split(";").map((x) => x.trim()).filter(Boolean);
    const issues = [];
    for (const r of dataRows) {
      const title = cell(r, "title");
      if (!title) continue;
      const priorityNum = parseInt(cell(r, "priority"), 10);
      issues.push({
        title: title.slice(0, 240),
        body: cell(r, "body"),
        milestone: cell(r, "milestone"),
        project: cell(r, "project"),
        labels: splitList(cell(r, "labels")),
        priority: Number.isFinite(priorityNum) ? priorityNum : 0,
        assignees: splitList(cell(r, "assignees")),
      });
    }
    return issues;
  }

  function openIssueImport(repo) {
    const container = $("[data-repo-issues]");
    if (!container || !repo) return;
    container.innerHTML = `
      <form data-repo-issue-import-form class="grid gap-3 border-t border-border bg-background p-4">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <span class="inline-flex items-center gap-2 text-sm font-semibold text-foreground"><i data-lucide="upload" class="h-4 w-4 text-muted-foreground"></i>Import issues from CSV</span>
          <button type="button" data-repo-issue-cancel class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to issues</button>
        </div>
        <p class="text-xs leading-5 text-muted-foreground">Upload a CSV with columns <code class="rounded bg-secondary px-1 py-0.5 font-mono">title, body, milestone, project, labels, priority, assignees</code>. Only <code class="rounded bg-secondary px-1 py-0.5 font-mono">title</code> is required. Separate multiple labels or assignees with <code class="rounded bg-secondary px-1 py-0.5 font-mono">;</code>. Each row is a signed issue delivered to the first eligible online mirror.</p>
        <div class="flex flex-wrap items-center gap-2">
          <button type="button" data-repo-issue-template class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="download" class="h-3.5 w-3.5"></i>Download template</button>
          <input type="file" data-repo-issue-csv-input accept=".csv,text/csv" class="text-xs text-foreground file:mr-2 file:h-8 file:cursor-pointer file:rounded-md file:border file:border-border file:bg-secondary file:px-3 file:text-xs file:font-medium file:text-foreground" />
        </div>
        <div data-repo-issue-import-preview class="text-xs text-muted-foreground"></div>
        <div class="flex flex-wrap items-center justify-between gap-3">
          <div class="flex min-w-0 flex-col gap-1">
            ${composeIdentityHtml(state.session, "Filing")}
            <span data-repo-issue-import-hint class="text-[11px] text-muted-foreground">Eligible online mirrors add accepted issues directly to the repository.</span>
          </div>
          <button type="submit" data-repo-issue-import-submit disabled class="inline-flex h-9 items-center gap-2 rounded-md bg-cta px-4 text-sm font-medium text-cta-foreground transition-colors hover:bg-cta/90 disabled:opacity-50"><i data-lucide="send" class="h-4 w-4"></i>Import issues</button>
        </div>
      </form>`;
    window.lucide?.createIcons();
    const form = container.querySelector("[data-repo-issue-import-form]");
    const fileInput = container.querySelector("[data-repo-issue-csv-input]");
    const preview = container.querySelector("[data-repo-issue-import-preview]");
    const submit = container.querySelector("[data-repo-issue-import-submit]");
    if (form) form._parsedIssues = [];
    fileInput?.addEventListener("change", async () => {
      const file = fileInput.files?.[0];
      if (form) form._parsedIssues = [];
      if (submit) submit.disabled = true;
      if (!file) { if (preview) preview.textContent = ""; return; }
      let text = "";
      try { text = await file.text(); }
      catch (_) { if (preview) { preview.className = "text-xs text-destructive"; preview.textContent = "Could not read that file."; } return; }
      let issues = [];
      try { issues = csvRowsToIssues(parseCsv(text)); } catch (_) { issues = []; }
      if (form) form._parsedIssues = issues;
      if (submit) submit.disabled = issues.length === 0;
      if (!preview) return;
      if (!issues.length) {
        preview.className = "text-xs text-destructive";
        preview.textContent = "No issues found - make sure the file has a header row with a \"title\" column and at least one titled row.";
        return;
      }
      preview.className = "text-xs text-muted-foreground";
      const sample = issues.slice(0, 5).map((it) => `• ${escapeHtml(it.title)}`).join("<br>");
      preview.innerHTML = `Ready to import <strong class="text-foreground">${issues.length}</strong> issue${issues.length === 1 ? "" : "s"}:<br>${sample}${issues.length > 5 ? `<br>…and ${issues.length - 5} more` : ""}`;
    });
  }

  async function handleIssueImportSubmit(repo, form) {
    if (!repo || !form) return;
    const issues = Array.isArray(form._parsedIssues) ? form._parsedIssues : [];
    const submit = form.querySelector("[data-repo-issue-import-submit]");
    const hint = form.querySelector("[data-repo-issue-import-hint]");
    const setHint = (text, tone) => {
      if (hint) hint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-muted-foreground" : "text-muted-foreground"}`;
      if (hint) hint.textContent = text;
    };
    if (!issues.length) { setHint("Choose a CSV file first.", "bad"); return; }
    if (submit) submit.disabled = true;
    let ok = 0;
    let failed = 0;
    let added = 0;
    for (let i = 0; i < issues.length; i++) {
      const it = issues[i];
      setHint(`Signing and sending ${i + 1} of ${issues.length}…`);
      try {
        const result = await submitWebIssue(
          repo, it.title, it.body, false, "", "", {
            milestone: it.milestone,
            project: it.project,
            labels: it.labels,
            priority: it.priority,
            assignees: it.assignees,
          });
        const number = Number(result.number || 0);
        if (!Number.isInteger(number) || number <= 0) {
          throw new Error("number_unavailable");
        }
        ok++;
        if (!state.issuesView.items.some(
          (issue) => Number(issue.number) === number && issue.status !== "closed")) {
          added++;
        }
        const item = {
          number,
          title: it.title,
          status: "open",
          state: "open",
          author: state.session?.nodeName || "you",
          date: "just now",
          meta: [it.milestone && `milestone: ${it.milestone}`, it.project && `project: ${it.project}`].filter(Boolean).join(" · "),
          milestone: it.milestone,
          body: it.body,
        };
        state.issuesView.items = [
          item,
          ...state.issuesView.items.filter(
            (issue) => Number(issue.number) !== number),
        ];
        rememberOptimisticRepoRecord(repo, "issues", number, {
          values: {
            title: it.title,
            status: "open",
            author: result.event.author,
            authorName: result.event.authorName || item.author,
            ts: result.event.ts,
            labels: it.labels,
            milestone: it.milestone,
            project: it.project,
            priority: it.priority,
            assignees: it.assignees,
          },
          body: it.body,
          issueEvents: [result.event],
          issueCreator: result.event.author,
        });
      } catch (_) {
        failed++;
      }
    }
    countFiledWebIssues(added);
    if (submit) submit.disabled = false;
    renderRepoIssues();
    setHint(
      failed
        ? `Imported ${ok} issue${ok === 1 ? "" : "s"}; ${failed} could not be sent. Go back to review.`
        : `Submitted ${ok} issue${ok === 1 ? "" : "s"} for direct mirror delivery.`,
      failed ? "bad" : "good");
  }

  // Branch-referencing web pull requests: no client-side diff computation
  // (deliberately - that duplicates what git already does correctly). The
  // desktop client reconstructs the patch from base/head on drain, same as any
  // other branch-backed pull request (see renderRepoPullPatch's "Branch-backed
  // PRs are reconstructed by the desktop client" copy).
  //
  // ``preset`` names the base/head the form opens on - the compare page hands
  // over the pair it was showing - and a preset branch the listing has not
  // loaded yet is still offered, since the mirror resolves names on submit.
  function openPullCompose(repo, preset = {}) {
    const container = $("[data-repo-pulls]");
    if (!container || !repo) return;
    state.repoCompare = null;
    setRepoRecordOpen("pulls", true);
    const parent = repoForkParent(repo);
    const targetRepo = pullRequestTargetRepo(repo);
    const sourceLabel = repoDisplayKey(repo);
    const targetLabel = parent?.key || repoDisplayKey(targetRepo);
    const branches = repoBranchList(repo);
    const defaultBranch = repoDefaultBranch(repo);
    const presetBase = String(preset?.base || "").trim();
    const presetHead = String(preset?.head || "").trim();
    const names = branches.map((branch) => branch.name);
    [presetBase, presetHead].forEach((name) => {
      if (name && !names.includes(name)) names.push(name);
    });
    const branchOptions = names.map((name) => `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`).join("");
    const headDefault = presetHead
      || branches.find((branch) => branch.name !== defaultBranch)?.name
      || defaultBranch;
    container.innerHTML = `
      <form data-repo-pull-new-form class="grid gap-3 border-t border-border bg-background p-4">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <span class="inline-flex items-center gap-2 text-sm font-semibold text-foreground"><i data-lucide="git-pull-request" class="h-4 w-4 text-muted-foreground"></i>New pull request${parent ? ` to ${escapeHtml(targetLabel)}` : ""}</span>
          <button type="button" data-repo-pull-cancel class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to pull requests</button>
        </div>
        <label data-repo-pull-target-wrap hidden class="grid gap-1 text-xs font-medium text-muted-foreground">Send to
          <select data-repo-pull-target class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent"><option value="">${escapeHtml(targetLabel)} on this instance</option></select>
        </label>
        <div class="grid gap-2 sm:grid-cols-2">
          <label class="grid gap-1 text-xs font-medium text-muted-foreground">Base
            <select data-repo-pull-base class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent">${branchOptions}</select>
          </label>
          <label class="grid gap-1 text-xs font-medium text-muted-foreground">Head
            <select data-repo-pull-head class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent">${branchOptions}</select>
          </label>
        </div>
        <label class="grid gap-1 text-xs font-medium text-muted-foreground">Title
          <input data-repo-pull-title type="text" required maxlength="240" placeholder="Short, descriptive title" class="h-9 rounded-md border border-border bg-background px-3 text-sm text-foreground outline-none focus:border-accent" />
        </label>
        <label class="grid gap-1 text-xs font-medium text-muted-foreground">Description
          <textarea data-repo-pull-body rows="6" placeholder="Describe the change. Markdown is supported." class="rounded-md border border-border bg-background px-3 py-2 text-sm text-foreground outline-none focus:border-accent"></textarea>
        </label>
        <div class="rounded-md border border-dashed border-border bg-secondary/20 px-3 py-2 text-[11px] text-muted-foreground">${parent
          ? `The diff is resolved on <span class="font-mono text-foreground">${escapeHtml(sourceLabel)}</span>, signed in this browser, and submitted to <span class="font-mono text-foreground">${escapeHtml(targetLabel)}</span>. The portable patch remains deliverable while the parent node is offline.`
          : "The diff is resolved from the selected branches, signed in this browser, and delivered directly to an eligible online mirror."}</div>
        ${microjobPullNoticeHtml()}
        <div class="flex flex-wrap items-center justify-between gap-3">
          <div class="flex min-w-0 flex-col gap-1">
            ${composeIdentityHtml(state.session, "Filing")}
            <span data-repo-pull-hint class="text-[11px] text-muted-foreground">${parent ? `Submitting from your fork to ${escapeHtml(targetLabel)}.` : "Eligible online mirrors commit accepted pull requests directly."}</span>
          </div>
          <button type="submit" data-repo-pull-submit title="Create pull request (Ctrl+Enter / ⌘+Enter)" aria-keyshortcuts="Control+Enter Meta+Enter" class="inline-flex h-9 items-center gap-2 rounded-md bg-cta px-4 text-sm font-medium text-cta-foreground transition-colors hover:bg-cta/90 disabled:opacity-50"><i data-lucide="send" class="h-4 w-4"></i>Create pull request</button>
        </div>
      </form>`;
    window.lucide?.createIcons();
    const baseSelect = container.querySelector("[data-repo-pull-base]");
    const headSelect = container.querySelector("[data-repo-pull-head]");
    if (baseSelect) baseSelect.value = presetBase || defaultBranch;
    if (headSelect) headSelect.value = headDefault;
    container.querySelector("[data-repo-pull-title]")?.focus();
    void fillPullComposeTargets(repo, container.querySelector("[data-repo-pull-new-form]"));
  }

  async function fillPullComposeTargets(repo, form) {
    const forks = await loadRepoRemoteForks(repo);
    const wrap = form?.querySelector("[data-repo-pull-target-wrap]");
    const select = form?.querySelector("[data-repo-pull-target]");
    if (!wrap || !select || !form.isConnected || !forks.length) return;
    select.insertAdjacentHTML("beforeend", forks.map((fork) =>
      `<option value="${escapeHtml(fork.url)}">${escapeHtml(`${fork.owner}/${fork.name}`)} on ${escapeHtml(fork.host)}</option>`).join(""));
    wrap.hidden = false;
  }

  function pullComposeRemoteTarget(form) {
    const url = String(form?.querySelector("[data-repo-pull-target]")?.value || "");
    if (!url) return null;
    return state.repoRemoteForks.forks.find((fork) => fork.url === url) || null;
  }

  // --- Branch comparison: the web diff viewer for a branch --------------------
  //
  // /owner/repo/compare/<base>...<head> reads the mirror's bounded compare
  // endpoint - the same read that signs a web pull request - and shows its
  // commit series and patch through the pull review's Files changed pane
  // (file explorer, filter, unified/split layout, viewed folds; no review
  // comments, there is no pull to hold them), so a branch can be read before,
  // or without, a pull request. The Agents tab
  // links each agent's branch here (its work is on that branch whether or
  // not the run opened a pull), and "Create pull request" carries the pair
  // into the new-pull form. It lives on the Pull requests tab, beside the
  // form it feeds, and is addressable so a refresh or a pasted link lands on
  // the same comparison.

  function repoCompareHeaderHtml(repo, base, head) {
    return `
      <div class="flex flex-wrap items-center justify-between gap-3">
        <button type="button" data-repo-compare-back class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to pull requests</button>
        <div class="flex flex-wrap items-center gap-2">
          <button type="button" data-repo-compare-refresh class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border px-2.5 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="refresh-cw" class="h-3.5 w-3.5"></i>Refresh</button>
          <button type="button" data-repo-compare-pull class="${repoReadOnlyMirror(repo) ? "hidden " : ""}inline-flex h-8 items-center gap-1.5 rounded-md bg-cta px-3 text-xs font-semibold text-cta-foreground hover:bg-cta/90"><i data-lucide="git-pull-request" class="h-3.5 w-3.5"></i>Create pull request</button>
        </div>
      </div>
      <header class="grid gap-1 rounded-lg border border-border bg-secondary/30 p-4">
        <h3 class="flex flex-wrap items-center gap-2 text-base font-semibold text-foreground"><i data-lucide="git-compare-arrows" class="h-4 w-4 text-muted-foreground"></i>Comparing changes</h3>
        <p class="flex flex-wrap items-center gap-2 text-xs text-muted-foreground">
          <span class="rounded-md border border-border bg-background px-2 py-0.5 font-mono text-foreground" title="base">${escapeHtml(base)}</span>
          <i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>
          <span class="rounded-md border border-border bg-background px-2 py-0.5 font-mono text-foreground" title="head">${escapeHtml(head)}</span>
          <span data-repo-compare-summary class="min-w-0"></span>
        </p>
      </header>`;
  }

  function repoCompareErrorText(error, base, head, missing = null) {
    const status = Number(error?.status || 0);
    if (status === 404 && Array.isArray(missing) && missing.length) {
      const names = missing.join(" and ");
      const agentBranch = missing.find((name) => name.startsWith("agent/"));
      if (agentBranch) {
        return `${names} ${missing.length === 1 ? "is" : "are"} not on the mirror. An agent publishes its branch only when its run commits a change, so a run that is still working, or that finished without changing any file, has no branch to compare. Open the agent session to see how it ended.`;
      }
      return `${names} ${missing.length === 1 ? "is not a branch" : "are not branches"} on the mirror. It may have been deleted or renamed; pick another branch to compare.`;
    }
    if (status === 404) {
      return `One of these branches is not on the serving mirror yet: ${base} or ${head}. A branch an agent just pushed can take a sync to arrive; refresh in a moment.`;
    }
    if (status === 413) {
      return "This comparison is larger than a mirror will serve in one read. Open a pull request to review it in bounded pieces, or narrow the branches.";
    }
    return "This comparison could not be read from a live mirror right now. Refresh to try again.";
  }

  async function openRepoCompare(repo, base, head, { navigate = true } = {}) {
    const container = $("[data-repo-pulls]");
    if (!container || !repo || !base || !head) return;
    const compare = { base, head };
    state.repoCompare = compare;
    state.repoRecordDetail = null;
    state.loadedRepoTabs = state.loadedRepoTabs || {};
    state.loadedRepoTabs.pulls = true;
    stopRepoPullActivityRefresh();
    setRepoTab("pulls");
    setRepoRecordOpen("pulls", true);
    if (navigate) navigateHistory(repoCompareUrl(repo, base, head));
    const paint = (bodyHtml) => {
      // A read that lands after the reader left for the list, a record, or
      // another comparison must not paint over what replaced it.
      if (state.repoCompare !== compare) return false;
      container.innerHTML = `
        <article data-repo-compare="${escapeHtml(`${base}...${head}`)}" class="grid gap-4 border-t border-border bg-background p-4">
          ${repoCompareHeaderHtml(repo, base, head)}
          <section data-repo-compare-merge hidden></section>
          ${bodyHtml}
        </article>`;
      renderRepoCompareMergePanel(repo);
      window.lucide?.createIcons();
      return true;
    };
    paint(`<div class="px-1 py-3 text-sm text-muted-foreground">${loadingHtml("Comparing the branches on the live mirror...")}</div>`);
    loadRepoCompareMergePanel(repo, compare);
    if (base === head) {
      paint(`<div data-repo-compare-empty class="rounded-lg border border-dashed border-border px-4 py-6 text-center text-sm text-muted-foreground">Choose two different branches to compare.</div>`);
      return;
    }
    let comparison;
    try {
      comparison = await fetchRepoJson(repoLiveUrl(repo, "compare", { base, head, ref: "" }));
    } catch (error) {
      let missing = null;
      if (Number(error?.status || 0) === 404) {
        try {
          const names = new Set((await fetchRepoBranches(repo, { refresh: true })).map((branch) => branch.name));
          missing = [base, head].filter((name) => !names.has(name) && !/^[0-9a-f]{7,64}$/i.test(name));
        } catch (_) {
          missing = null;
        }
      }
      paint(`<div data-repo-compare-unavailable class="rounded-lg border border-dashed border-border px-4 py-6 text-center text-sm text-muted-foreground">${escapeHtml(repoCompareErrorText(error, base, head, missing))}</div>`);
      return;
    }
    const patch = String(comparison?.patch || "");
    const commits = parsePullCommitMbox(String(comparison?.commits || ""));
    const commitCount = Number(comparison?.commitCount) || commits.length;
    const files = parsePatchStats(patch);
    if (!patch.trim() && !commits.length) {
      paint(`<div data-repo-compare-empty class="rounded-lg border border-dashed border-border px-4 py-6 text-center text-sm text-muted-foreground"><span class="font-mono text-foreground">${escapeHtml(head)}</span> has nothing that is not already on <span class="font-mono text-foreground">${escapeHtml(base)}</span>.</div>`);
      return;
    }
    // The read behind the pane: repoPullFilesContext() serves the layout
    // toggles, the file explorer, the filter and the viewed marks from here,
    // exactly as it does for an open pull's Files changed tab.
    compare.result = {
      patch,
      files,
      commitCount,
      baseOid: String(comparison?.baseOid || ""),
      headOid: String(comparison?.headOid || ""),
      mergeBaseOid: String(comparison?.mergeBaseOid || ""),
    };
    const viewed = loadRepoCompareViewed(repo, compare);
    const painted = paint(`
      <section class="overflow-hidden rounded-lg border border-border">
        <div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3">
          <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="git-commit-horizontal" class="h-3.5 w-3.5 text-muted-foreground"></i>Commits</span>
          <span class="font-mono text-[10px] text-muted-foreground">${escapeHtml(formatCount(commitCount))} ${commitCount === 1 ? "commit" : "commits"}</span>
        </div>
        <div data-repo-compare-commits class="divide-y divide-border">${renderRepoPullCommits(commits)}</div>
      </section>
      <div data-repo-compare-diff>${patch.trim()
        ? renderRepoPullFilesSection({ patch, files }, viewed, { reviewable: false })
        : '<section class="rounded-lg border border-border"><div class="px-4 py-3 text-sm text-muted-foreground">No textual diff is available for this comparison.</div></section>'}</div>`);
    if (!painted) return;
    const summary = container.querySelector("[data-repo-compare-summary]");
    if (summary) {
      summary.textContent = `${formatCount(commitCount)} ${commitCount === 1 ? "commit" : "commits"} · ${formatCount(files.length)} ${files.length === 1 ? "file" : "files"} changed`;
    }
    // A freshly painted pane starts on its first file, as the pull review does.
    const first = files[0]?.path || "";
    if (first) setRepoPullActiveFile(first, { revealRow: true });
  }

  async function handleRemotePullSubmit(repo, remote, { title, body, base, head, submit, setHint, titleInput, bodyInput }) {
    const where = `${remote.owner}/${remote.name} on ${remote.host}`;
    setSubmitBusy(submit, true, "Creating pull request…");
    setHint(`Signing and sending to ${where}…`);
    try {
      const result = await submitRemotePullOpen(repo, remote, title, body, base, head);
      const number = Number(result.number || 0);
      if (!Number.isInteger(number) || number <= 0) throw new Error("number_unavailable");
      if (titleInput) titleInput.value = "";
      if (bodyInput) bodyInput.value = "";
      setSubmitBusy(submit, false);
      const hint = submit?.closest("form")?.querySelector("[data-repo-pull-hint]");
      if (hint) {
        hint.className = "text-[11px] text-muted-foreground";
        hint.innerHTML = `Sent as #${number} to ${escapeHtml(where)}. <a href="${escapeHtml(`${remote.url}/pulls/${number}`)}" target="_blank" rel="noopener noreferrer" class="font-medium text-accent hover:underline">Open it there</a>`;
      }
    } catch (error) {
      setSubmitBusy(submit, false);
      console.error("[forkmesh] remote pull request submission failed", error);
      setHint(String(error?.message || "") === "remote_unreachable"
        ? `Nothing was delivered: ${remote.host} could not be reached, or it runs a ForkMesh version that does not take pull requests from other instances yet.`
        : webPullSubmitErrorText(error, base, head, where), "bad");
    }
  }

  async function handlePullComposeSubmit(repo, form) {
    if (!repo || !form) return;
    const titleInput = form.querySelector("[data-repo-pull-title]");
    const bodyInput = form.querySelector("[data-repo-pull-body]");
    const baseSelect = form.querySelector("[data-repo-pull-base]");
    const headSelect = form.querySelector("[data-repo-pull-head]");
    const submit = form.querySelector("[data-repo-pull-submit]");
    const hint = form.querySelector("[data-repo-pull-hint]");
    const setHint = (text, tone) => {
      if (hint) hint.className = `text-[11px] ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-muted-foreground" : "text-muted-foreground"}`;
      if (hint) hint.textContent = text;
    };
    const title = String(titleInput?.value || "").trim();
    const base = String(baseSelect?.value || "").trim();
    const head = String(headSelect?.value || "").trim();
    if (!title) {
      setHint("Enter a title for the pull request.", "bad");
      titleInput?.focus();
      return;
    }
    if (!base || !head) {
      setHint("Choose a base and head branch.", "bad");
      return;
    }
    if (base === head) {
      setHint("Base and head must be different branches.", "bad");
      return;
    }
    const body = String(bodyInput?.value || "");
    const remote = pullComposeRemoteTarget(form);
    if (remote) {
      await handleRemotePullSubmit(repo, remote, { title, body, base, head, submit, setHint, titleInput, bodyInput });
      return;
    }
    setSubmitBusy(submit, true, "Creating pull request…");
    setHint("Signing and sending…");
    try {
      const result = await submitWebPullOpen(repo, title, body, base, head);
      const number = Number(result.number || 0);
      if (!Number.isInteger(number) || number <= 0) {
        throw new Error("number_unavailable");
      }
      const targetRepo = result.targetRepo || repo;
      const crossFork = Boolean(repoForkParent(repo));
      const item = {
        number,
        title,
        state: "open",
        author: result.pull.authorName || result.pull.author || "you",
        date: "just now",
        meta: `open · ${base} ← ${result.pull.head}`,
        body,
      };
      if (!crossFork) {
        const pulls = Array.isArray(state.repoCollectionItems.pulls)
          ? state.repoCollectionItems.pulls : [];
        state.repoCollectionItems.pulls = [
          item,
          ...pulls.filter((pull) => Number(pull.number) !== number),
        ];
        setRepoTabCount(
          "pulls",
          state.repoCollectionItems.pulls.filter(
            (pull) => pull.state === "open").length);
      }
      // A cross-fork submission lives in the parent's pull list, so its
      // durable optimistic record is keyed on targetRepo, not this fork.
      rememberOptimisticRepoRecord(targetRepo, "pulls", number, {
        values: {
          number,
          title,
          status: "open",
          base,
          head: result.pull.head,
          author: result.pull.author,
          authorName: result.pull.authorName || item.author,
          ts: result.pull.ts,
        },
        body,
        pullPatch: {
          patch: result.pull.patch,
          files: parsePatchStats(result.pull.patch),
          unavailable: false,
        },
        pullConversation: [],
        pullCommits: [],
        pullAgents: [],
        pullTimeline: [],
      });
      if (titleInput) titleInput.value = "";
      if (bodyInput) bodyInput.value = "";
      setSubmitBusy(submit, false);
      const detailUrl = repoRecordUrl(targetRepo, "pulls", number, "conversation");
      if (crossFork) {
        setHint(`Created #${number}; opening it in ${repoDisplayKey(targetRepo)}…`, "good");
        window.location.assign(detailUrl);
        return;
      }
      navigateHistory(detailUrl);
      await loadRepoRecordDetail(targetRepo, "pulls", number);
    } catch (error) {
      setSubmitBusy(submit, false);
      console.error("[forkmesh] pull request submission failed", error);
      setHint(
        webPullSubmitErrorText(
          error, base, head, repoDisplayKey(pullRequestTargetRepo(repo))),
        "bad");
    }
  }

  const REPO_COMMITS_FETCH_TIMEOUT_MS = 30 * 1000;
  const REPO_COMMITS_UNAVAILABLE_HTML = '<div class="px-4 py-3 text-sm text-muted-foreground">Commit history is unavailable until a reachable mirror host serves this repository.</div>';

  async function loadRepoCommits(repo) {
    const container = $("[data-repo-commits]");
    if (!container) return;
    // The catalog already knows when the whole mirror group is offline. Paint
    // that terminal state immediately, while still trying the live route in
    // case a host recovered after the catalog snapshot was built.
    container.innerHTML = repoIsLive(repo)
      ? `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading commits from the live mirror...")}</div>`
      : REPO_COMMITS_UNAVAILABLE_HTML;
    const controller = new AbortController();
    const timer = window.setTimeout(
      () => controller.abort(),
      REPO_COMMITS_FETCH_TIMEOUT_MS,
    );
    try {
      const data = await fetchRepoJson(
        repoLiveUrl(repo, "history"),
        { signal: controller.signal },
      );
      const commits = Array.isArray(data.commits) ? data.commits : [];
      if (!commits.length) {
        container.innerHTML = '<div class="px-4 py-3 text-sm text-muted-foreground">No commits are available from this live mirror yet.</div>';
        return;
      }
      container.innerHTML = commits.slice(0, 60).map((commit) => {
        const hash = String(commit.hash || "");
        const shortHash = hash.slice(0, 7) || "unknown";
        return `
        <div data-dashboard-commit-row class="grid w-full grid-cols-[minmax(0,1fr)_auto] items-stretch gap-3 border-t border-border hover:bg-secondary/30 transition-colors">
          <button type="button" data-dashboard-commit-hash="${escapeHtml(hash)}" class="min-w-0 px-4 py-3 text-left">
            <span class="block truncate text-sm font-medium text-foreground">${escapeHtml(commit.subject || "Commit")}</span>
            <span class="mt-1 block truncate text-xs text-muted-foreground">${escapeHtml(commit.author || "unknown")} committed ${escapeHtml(commit.date || "")}</span>
          </button>
          <div class="flex items-center justify-end gap-2 px-4 py-3">
            <button type="button" data-dashboard-commit-hash="${escapeHtml(hash)}" data-dashboard-commit-short-hash class="font-mono text-xs font-semibold text-foreground hover:underline">${escapeHtml(shortHash)}</button>
            <button type="button" data-dashboard-copy="${escapeHtml(hash)}" data-dashboard-copy-kind="commit-hash" aria-label="Copy commit hash ${escapeHtml(shortHash)}" class="copy-button inline-flex h-7 w-7 items-center justify-center rounded-md border border-border bg-background p-0 text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors">
              <i data-lucide="copy" class="copy-icon h-3.5 w-3.5"></i>
              <i data-lucide="check" class="copy-check h-3.5 w-3.5 text-foreground"></i>
            </button>
          </div>
        </div>`;
      }).join("");
    } catch (_) {
      container.innerHTML = REPO_COMMITS_UNAVAILABLE_HTML;
    } finally {
      window.clearTimeout(timer);
      window.lucide?.createIcons();
    }
  }

  function renderRepoCommitFiles(files) {
    const rows = Array.isArray(files) ? files : [];
    if (!rows.length) return '<div class="px-4 py-3 text-sm text-muted-foreground">No file summary is available for this commit.</div>';
    return rows.slice(0, 100).map((file) => `
      <div class="grid grid-cols-[minmax(0,1fr)_auto_auto] gap-3 border-t border-border px-4 py-2 text-xs">
        <span class="min-w-0 truncate font-mono text-foreground">${escapeHtml(file.path || "file")}</span>
        <span class="font-mono text-success">+${escapeHtml(file.adds ?? "0")}</span>
        <span class="font-mono text-destructive">-${escapeHtml(file.dels ?? "0")}</span>
      </div>`).join("");
  }

  // The section holding this carries no overflow-hidden: that would make it the
  // scroll container for the file headers' position:sticky, so they would never
  // stick while the page scrolls past a long file.
  function renderRepoCommitDiff(diff, imageDiffs) {
    if (!String(diff || "").trim()) return '<div class="px-4 py-3 text-sm text-muted-foreground">No textual diff is available for this commit.</div>';
    return `<div data-repo-commit-diff>${renderDiffFiles(parseDiffFiles(diff), imageDiffs)}</div>`;
  }

  function renderRepoCommitDetail(repo, data) {
    const commit = data.commit || {};
    const hash = String(commit.hash || "");
    const parents = String(commit.parents || "").split(/\s+/).filter(Boolean);
    return `
      <article data-repo-commit-detail class="grid gap-4 border-t border-border bg-background p-4">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <button type="button" data-repo-commit-back class="inline-flex h-8 items-center gap-2 rounded-md border border-border px-3 text-xs font-medium text-foreground hover:bg-secondary"><i data-lucide="arrow-left" class="h-3.5 w-3.5"></i>Back to commits</button>
          <span class="font-mono text-xs text-muted-foreground">${escapeHtml(hash)}</span>
        </div>
        <header class="rounded-lg border border-border bg-secondary/30 p-4">
          <h3 class="text-base font-semibold text-foreground">${escapeHtml(commit.subject || "Commit")}</h3>
          ${commit.body ? `<p class="mt-3 whitespace-pre-wrap text-sm leading-6 text-muted-foreground">${escapeHtml(commit.body)}</p>` : ""}
          <dl class="mt-4 grid gap-2 text-xs text-muted-foreground sm:grid-cols-2">
            <div><dt class="font-semibold text-foreground">Author</dt><dd>${escapeHtml(commit.author || "unknown")}</dd></div>
            <div><dt class="font-semibold text-foreground">Date</dt><dd>${escapeHtml(commit.date || "unknown")}</dd></div>
            <div><dt class="font-semibold text-foreground">Parents</dt><dd class="font-mono">${parents.length ? parents.map((parent) => escapeHtml(parent.slice(0, 12))).join(", ") : "root commit"}</dd></div>
            <div><dt class="font-semibold text-foreground">Repository</dt><dd class="font-mono">${escapeHtml(repo.owner || "owner")}/${escapeHtml(repo.name || "repo")}</dd></div>
          </dl>
        </header>
        <section class="overflow-hidden rounded-lg border border-border">
          <div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="inline-flex items-center gap-2 text-xs font-medium text-foreground"><i data-lucide="files" class="h-3.5 w-3.5 text-muted-foreground"></i>Files changed</span>
            <span class="font-mono text-[10px] text-muted-foreground">${formatCount(Array.isArray(data.files) ? data.files.length : 0)} files</span>
          </div>
          <div data-repo-commit-files>${renderRepoCommitFiles(data.files)}</div>
        </section>
        <section class="rounded-lg border border-border">
          <div class="flex items-center gap-2 rounded-t-lg border-b border-border bg-secondary/50 px-4 py-3 text-xs font-medium text-foreground"><i data-lucide="git-compare-arrows" class="h-3.5 w-3.5 text-muted-foreground"></i>Diff</div>
          ${renderRepoCommitDiff(data.diff, data.imageDiffs)}
        </section>
      </article>`;
  }

  async function loadRepoCommitDetail(repo, hash) {
    const container = $("[data-repo-commits]");
    if (!container || !repo || !hash) return;
    container.innerHTML = `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading commit from the live mirror...")}</div>`;
    try {
      const data = await fetchRepoJson(repoLiveUrl(repo, "commit", { path: hash }));
      state.repoCommitDetail = { repo, data };
      container.innerHTML = renderRepoCommitDetail(repo, data);
    } catch (_) {
      container.innerHTML = '<div class="px-4 py-3 text-sm text-muted-foreground">Commit detail is unavailable until a reachable mirror host serves this commit.</div>';
    } finally {
      window.lucide?.createIcons();
    }
  }

  // A mirror row is ringed green when it's the node that answered the most
  // recent live-mirror fetch (data.servedBy from the tunnel round-robin), with
  // its response time alongside so it's obvious which mirror served the page
  // and how fast.
  function mirrorRowIsServing(mirror, servedBy) {
    const name = String(mirror.owner || mirror.node || mirror.name || "").trim().toLowerCase();
    const servedName = String(servedBy?.name || "").trim().toLowerCase();
    return Boolean(name && servedName && name === servedName);
  }

  // Only a working-copy publication can be a count/commit reference. A
  // remote-clone remains a replica even when it is the freshest node online;
  // promoting it in the UI made mirror24 look authoritative for forkmesh.
  function pickReferenceMirror(mirrors) {
    return (Array.isArray(mirrors) ? mirrors : []).find(
      (mirror) =>
        String(mirror?.source || "").trim().toLowerCase() === "local-node" &&
        mirror?.referenceForMirrors !== false &&
        String(mirror?.commit || "").trim(),
    ) || null;
  }

  // The source is a node too. Older /mirrors responses supplied only its name,
  // while current responses carry the source publication's same bounded facts
  // as every replica. Merge it into the table either way so the authority does
  // not disappear into a summary banner with none of its shared information.
  function repoNodesWithSource(mirrors, source = state.repoSourceOfTruth) {
    const rows = Array.isArray(mirrors) ? mirrors.slice() : [];
    const sourceName = String(
      source?.node || source?.machineName || source?.owner ||
      state.selectedRepo?.owner || "",
    ).trim();
    if (!sourceName) return rows;
    const sourceNode = {
      ...source,
      node: sourceName,
      source: "local-node",
      status: source?.status === "online" ? "online" : "offline",
      cloneAvailable: false,
      isSourceOfTruth: true,
    };
    const sourceKey = sourceName.toLowerCase();
    const existing = rows.findIndex((mirror) =>
      mirror?.isSourceOfTruth === true ||
      String(mirror?.node || mirror?.machineName || "").trim().toLowerCase() === sourceKey,
    );
    if (existing >= 0) rows[existing] = { ...rows[existing], ...sourceNode };
    else rows.unshift(sourceNode);
    return rows;
  }

  // A count differs from the reference node's only when both sides actually
  // reported a value (>= 0) — an em dash on either side means "not tracked", not
  // "out of sync", so it's never flagged.
  function mirrorCountMismatch(value, ref) {
    return Number(ref) >= 0 && Number(value) >= 0 && Number(value) !== Number(ref);
  }

  // Counts arrive as -1 when a node hasn't reported them; show a dash for those
  // (a distinct state from a real 0) so a cell never reads a misleading "0".
  function mirrorCountText(value) {
    const number = Number(value);
    return Number.isFinite(number) && number >= 0 ? formatCount(number) : "";
  }

  // The caption rides next to the icon at the same 10px as the cells; `short`
  // keeps a long column from stretching the row, and the full label stays in
  // the tooltip (and, when it differs, for screen readers). `wide` marks the
  // one free-text column, whose values are left-aligned, so its caption sits
  // over them instead of floating in the middle of a stretched column.
  function mirrorTableHeader(icon, label, wide = false, short = "") {
    const caption = short || label;
    return `
        <th scope="col" class="whitespace-nowrap px-1 py-1 ${wide ? "text-left" : "text-center"} text-[10px] font-medium text-muted-foreground" title="${escapeHtml(label)}">
          <span class="inline-flex items-center gap-1 leading-none"><i data-lucide="${icon}" class="inline-block h-3 w-3" aria-hidden="true"></i>${escapeHtml(caption)}</span>
          ${caption === label ? "" : `<span class="sr-only">${escapeHtml(label)}</span>`}
        </th>`;
  }

  function mirrorTableCell(value, options = {}) {
    const shown = value === "" || value === undefined || value === null ? "-" : value;
    const mismatch = options.mismatch === true;
    const title = options.title || String(shown);
    return `
        <td style="max-width:18rem" class="truncate whitespace-nowrap px-1 py-1 text-[10px] font-mono ${mismatch ? "text-attention underline decoration-attention/60" : "text-muted-foreground"}" title="${escapeHtml(title)}">${escapeHtml(shown)}</td>`;
  }

  // Retiring a node is a platform-administrator action, so the column only
  // exists for a session the relay itself marked isAdmin. The Worker re-checks
  // that on every request; this gate is purely about not showing a control
  // nobody else can use.
  function canDeleteMirrorNodes() {
    return state.session?.isAdmin === true;
  }

  function mirrorDeleteHeader() {
    if (!canDeleteMirrorNodes()) return "";
    return `
        <th scope="col" class="w-px whitespace-nowrap px-1 py-1 text-center text-[10px] font-medium text-muted-foreground" title="Delete node (administrators only)">
          <i data-lucide="trash-2" class="inline-block h-3 w-3" aria-hidden="true"></i>
          <span class="sr-only">Delete node</span>
        </th>`;
  }

  // The row names a machine; deletion is keyed on the account that publishes
  // it, with the machine label and node id carried along so the relay can
  // resolve a half-registered node from whichever identifier still exists.
  function mirrorDeleteCell(mirror) {
    if (!canDeleteMirrorNodes()) return "";
    if (mirror?.isSourceOfTruth === true) {
      return '<td class="w-px whitespace-nowrap px-1 py-1" aria-label="Source node cannot be deleted here"></td>';
    }
    const shown = String(mirror.node || mirror.machineName || mirror.name || "mirror").trim();
    const account = String(mirror.owner || mirror.node || "").trim().toLowerCase();
    return `
        <td class="w-px whitespace-nowrap px-1 py-1 text-center text-[10px] leading-none">
          <button type="button" data-repo-mirror-delete data-mirror-node="${escapeHtml(account)}" data-mirror-machine="${escapeHtml(String(mirror.machineName || mirror.node || ""))}" data-mirror-id="${escapeHtml(String(mirror.id || ""))}" title="Permanently delete ${escapeHtml(shown)} from ForkMesh" class="inline-flex h-4 w-4 items-center justify-center rounded text-muted-foreground transition-colors hover:bg-destructive/10 hover:text-destructive">
            <i data-lucide="trash-2" class="h-3 w-3" aria-hidden="true"></i>
            <span class="sr-only">Delete ${escapeHtml(shown)}</span>
          </button>
        </td>`;
  }

  function mirrorTableHead() {
    const columns = [
      ["server", "Node", true],
      ["radio", "Status"],
      ["activity", "Activity"],
      ["package", "Version"],
      ["git-commit-horizontal", "Revision"],
      ["refresh-cw", "Last synced", false, "Synced"],
      ["globe", "Endpoint"],
      ["badge-check", "Direct URL check", false, "URL check"],
      ["heart-pulse", "Health checked", false, "Checked"],
      ["gauge", "Latency"],
      ["map-pin", "Region"],
      ["shield-check", "Endpoint integrity", false, "Integrity"],
      ["list-checks", "Capabilities", false, "Caps"],
      ["hard-drive", "Repository size", false, "Size"],
      ["database", "Replica database", false, "Replica DB"],
      ["memory-stick", "Cache memory", false, "Cache RAM"],
      ["gauge", "Cache hit rate", false, "Cache hit"],
      ["disc-3", "Backend disk reads", false, "Disk reads"],
      ["zap", "Disk reads avoided", false, "Avoided"],
      ["archive-x", "Cache evictions", false, "Evictions"],
      ["circle-dot", "Issues"],
      ["git-commit-horizontal", "Commits"],
      ["git-branch", "Branches"],
      ["git-pull-request", "Pull requests", false, "Pulls"],
      ["messages-square", "Discussions", false, "Discuss"],
      ["package-open", "Artifacts"],
      ["trees", "Worktrees"],
      ["copy", "Clones served", false, "Clones"],
      ["mouse-pointer-click", "Website requests", false, "Web hits"],
    ];
    return columns
      .map(([icon, label, wide, short]) => mirrorTableHeader(icon, label, wide, short))
      .join("") + mirrorDeleteHeader();
  }

  function mirrorDirectCheckCell(mirror) {
    const node = String(mirror.node || mirror.machineName || "").trim();
    const mirrorUrl = String(mirror.mirrorUrl || "").trim();
    const checkedAt = Number(mirror.directCheckedAt) || 0;
    const integrity = String(mirror.directIntegrity || "unknown").toLowerCase();
    const reachable = mirror.directReachable === true;
    const verified = mirror.directVerified === true || (integrity === "ok" && reachable);
    let label = "check";
    let icon = "shield-question";
    let color = "text-muted-foreground";
    if (!mirrorUrl) {
      label = "tunnel";
      icon = "cloud";
    } else if (verified) {
      label = "verified";
      icon = "badge-check";
      color = "text-success";
    } else if (integrity === "failed") {
      label = "failed";
      icon = "shield-alert";
      color = "text-destructive";
    } else if (checkedAt && integrity === "ok") {
      label = "unreachable";
      icon = "unplug";
      color = "text-attention";
    } else if (checkedAt) {
      label = "unverified";
      icon = "shield-alert";
      color = "text-attention";
    }
    const reason = String(mirror.directReason || "unchecked").replaceAll("_", " ");
    const refs = String(mirror.directRefsSha256 || "").trim();
    const title = mirrorUrl
      ? [
          mirrorUrl,
          checkedAt ? `Checked ${formatTimeAgo(checkedAt)}` : "Not checked yet",
          reason,
          refs ? `Signed refs SHA-256: ${refs}` : "",
        ].filter(Boolean).join(" · ")
      : "No public mirror URL is registered; Git uses the tunnel fallback.";
    return `
        <td class="w-px whitespace-nowrap px-1 py-1 text-center text-[10px]" title="${escapeHtml(title)}">
          <button type="button" data-mirror-integrity-check="${escapeHtml(node)}" class="inline-flex items-center gap-1 font-mono ${color} ${mirrorUrl ? "hover:underline" : "cursor-not-allowed opacity-70"}" ${mirrorUrl && node ? "" : "disabled"} aria-label="Check ${escapeHtml(node || "mirror")} direct Git URL integrity">
            <i data-lucide="${icon}" class="inline-block h-3 w-3" aria-hidden="true"></i>${escapeHtml(label)}
          </button>
        </td>`;
  }

  const MIRROR_HEALTH_CACHE_MS = 5 * 60 * 1000;
  const mirrorHealthCache = new Map();

  function mirrorHealthUrl(mirror) {
    const advertised = String(mirror?.dashboardUrl || "").trim();
    const repository = String(mirror?.mirrorUrl || "").trim();
    try {
      const url = new URL(advertised || repository);
      if (url.protocol !== "https:" || url.username || url.password) return "";
      if (!/^mirror[0-9]+\.forkmesh\.com$/i.test(url.hostname)) return "";
      url.pathname = "/health";
      url.search = "";
      url.hash = "";
      return url.toString();
    } catch (_) {
      return "";
    }
  }

  function mirrorCacheCounters(mirror) {
    const cache = mirror?.cacheDiagnostics;
    if (!cache || typeof cache !== "object") return null;
    const groups = [
      cache.repositoryResponses,
      cache.dashboardAssets,
      cache.repositoryCatalog,
      cache.portableSessions,
      cache.dashboardReplica,
    ].filter((group) => group && typeof group === "object");
    const total = (field) => groups.reduce(
      (sum, group) => sum + Math.max(0, Number(group?.[field]) || 0),
      0,
    );
    const hits = total("hits");
    const misses = total("misses");
    return {
      replicaBytes: Math.max(0, Number(cache.dashboardReplica?.bytes) || 0),
      bytes: total("bytes"),
      entries: total("entries") + Math.max(0, Number(cache.dashboardReplica?.documents) || 0),
      hits,
      misses,
      hitRate: hits + misses ? (hits * 100) / (hits + misses) : null,
      backendReads: total("backendReads") + Math.max(0, Number(cache.dashboardReplica?.diskReads) || 0),
      avoidedReads: total("diskReadsAvoided"),
      evictions: total("evictions"),
      invalidations: total("invalidations"),
    };
  }

  async function hydrateRepoMirrorDiagnostics(mirrors, selectedKey) {
    const candidates = mirrors.filter(
      (mirror) => mirror?.status === "online" && mirrorHealthUrl(mirror),
    );
    if (!candidates.length) return;
    let changed = false;
    await Promise.allSettled(candidates.map(async (mirror) => {
      const url = mirrorHealthUrl(mirror);
      const now = Date.now();
      let cached = mirrorHealthCache.get(url);
      if (!cached || now - cached.fetchedAt >= MIRROR_HEALTH_CACHE_MS) {
        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 4000);
        try {
          const response = await fetch(url, {
            cache: "no-store",
            credentials: "omit",
            headers: { accept: "application/json" },
            signal: controller.signal,
          });
          const payload = await response.json().catch(() => ({}));
          if (!response.ok || payload.ok !== true || payload.transport !== "direct-https") return;
          cached = { fetchedAt: now, payload };
          mirrorHealthCache.set(url, cached);
        } finally {
          clearTimeout(timeout);
        }
      }
      if (!cached?.payload?.cache) return;
      mirror.cacheDiagnostics = cached.payload.cache;
      mirror.cacheCheckedAt = cached.payload.checkedAt || cached.fetchedAt;
      changed = true;
    }));
    if (
      changed &&
      repoKey(state.selectedRepo || {}).toLowerCase() === selectedKey
    ) {
      renderRepoMirrorLists(mirrors, state.repoServedBy);
    }
  }

  // Every node is a single compact table row. Long values stay on that line and
  // the borderless table scrolls horizontally instead of turning into cards.
  function renderMirrorTabRow(mirror, servedBy, refMirror) {
    const online = mirror.status === "online";
    const isServing = online && mirrorRowIsServing(mirror, servedBy);
    const speed = isServing ? formatServeSpeed(servedBy.tookMs) : "";
    const rawVersion = String(mirror.version || mirror.appVersion || mirror.clientVersion || "").trim();
    const version = rawVersion
      ? (rawVersion[0].toLowerCase() === "v" ? rawVersion : `v${rawVersion}`)
      : "";
    const commit = String(mirror.commit || "").trim();
    const refCommit = String(refMirror?.commit || "").trim();
    const refSizeBytes = Number(refMirror?.sizeBytes) || 0;
    const publishedSizeBytes = Number(mirror.sizeBytes) || 0;
    const usesCanonicalSize = Boolean(
      refSizeBytes > 0 && commit && refCommit && commit === refCommit,
    );
    const shownSizeBytes = usesCanonicalSize ? refSizeBytes : publishedSizeBytes;
    const isSource = mirror?.isSourceOfTruth === true || Boolean(refMirror && mirror === refMirror);
    const behind = online && commit && refCommit && commit !== refCommit;
    const integrityRejected = mirror.integrity === "rejected";
    const activity = String(mirror.activity || "").trim().toLowerCase();
    const activityLabel = {
      "running-actions": "running actions",
      syncing: "syncing refs",
      verifying: "verifying",
      "integrity-blocked": "integrity blocked",
      "awaiting-verification": "awaiting verification",
      serving: "serving",
      offline: "offline",
    }[activity] || "";
    const operations = Array.isArray(mirror.operations)
      ? mirror.operations.map((value) => String(value || "").trim()).filter(Boolean)
      : [];
    let endpointHost = "";
    let endpointUrl = "";
    try {
      endpointUrl = String(mirror.mirrorUrl || mirror.endpoint || "").trim();
      endpointHost = endpointUrl ? new URL(endpointUrl).host : "";
    } catch (_) {
      endpointHost = "";
      endpointUrl = "";
    }
    const commitLabel = commit
      ? commit.slice(0, 7) + (mirror.branch ? `/${mirror.branch}` : "")
      : "";
    const rowClass = isServing
      ? "bg-primary/10"
      : behind
        ? "bg-attention/10"
        : "transition-colors hover:bg-secondary/40";
    const statusIcon = online ? "radio" : "circle";
    const statusColor = online ? (behind ? "text-attention" : "text-success") : "text-muted-foreground";
    const nodeFlags = [
      isSource ? '<i data-lucide="database" class="inline-block h-3 w-3 text-muted-foreground" title="Source of truth"></i><span class="sr-only">Source of truth</span>' : "",
      behind ? '<i data-lucide="git-compare-arrows" class="inline-block h-3 w-3 text-attention" title="Out of sync"></i><span class="sr-only">Out of sync</span>' : "",
      integrityRejected ? '<i data-lucide="shield-alert" class="inline-block h-3 w-3 text-destructive" title="Failing integrity pin"></i><span class="sr-only">Failing integrity pin</span>' : "",
    ].join("");
    const capabilityTitle = operations.length
      ? `Capabilities: ${operations.join(", ")}`
      : "Capabilities not reported";
    const cache = mirrorCacheCounters(mirror);
    const cacheChecked = mirror.cacheCheckedAt
      ? ` · checked ${formatTimeAgo(mirror.cacheCheckedAt)}`
      : "";
    const cacheTitle = cache
      ? `${formatCount(cache.entries)} entries · ${formatCount(cache.hits)} hits · ${formatCount(cache.misses)} misses · ${formatCount(cache.invalidations)} invalidations${cacheChecked}`
      : "Direct cache diagnostics have not been loaded";
    return `
        <tr data-mirror-row data-mirror-state="${online ? "online" : "offline"}" class="${rowClass}">
          <th scope="row" style="max-width:16rem" class="truncate whitespace-nowrap px-1 py-1 text-left text-[11px] font-medium text-foreground font-mono">${escapeHtml(mirror.node || mirror.owner || mirror.name || "mirror")} ${nodeFlags}</th>
          <td class="w-px whitespace-nowrap px-1 py-1 text-center text-[10px] leading-none" title="${escapeHtml(mirror.status || "unknown")}"><i data-lucide="${statusIcon}" class="inline-block h-3 w-3 ${statusColor}" aria-hidden="true"></i><span class="sr-only">${escapeHtml(mirror.status || "unknown")}</span></td>
          ${mirrorTableCell(activityLabel || "", { title: activityLabel || "Activity not reported" })}
          ${mirrorTableCell(version)}
          ${mirrorTableCell(commitLabel, { mismatch: behind, title: behind && refCommit ? `Source of truth is at ${refCommit.slice(0, 7)}` : commitLabel })}
          ${mirrorTableCell(mirror.lastSync ? formatTimeAgo(mirror.lastSync) : "")}
          ${mirrorTableCell(endpointHost, { title: endpointUrl || "No endpoint registered" })}
          ${mirrorDirectCheckCell(mirror)}
          ${mirrorTableCell(mirror.checkedAt ? formatTimeAgo(mirror.checkedAt) : "")}
          ${mirrorTableCell(Number.isFinite(Number(mirror.latencyMs)) ? `${Math.max(0, Number(mirror.latencyMs))} ms` : "")}
          ${mirrorTableCell(mirror.region || "")}
          ${mirrorTableCell(mirror.endpointIntegrity || "", { mismatch: Boolean(mirror.endpointIntegrity && mirror.endpointIntegrity !== "ok") })}
          ${mirrorTableCell(operations.length ? formatCount(operations.length) : "", { title: capabilityTitle })}
          ${mirrorTableCell(shownSizeBytes ? formatSize(shownSizeBytes) : "", { title: usesCanonicalSize ? "Canonical content size from the source of truth; physical Git pack size may differ" : "Repository size reported by this node" })}
          ${mirrorTableCell(cache ? formatSize(cache.replicaBytes) : "", { title: cache ? `In-memory replica projection${cacheChecked}` : "Replica database size unavailable" })}
          ${mirrorTableCell(cache ? formatSize(cache.bytes) : "", { title: cacheTitle })}
          ${mirrorTableCell(cache?.hitRate === null || !cache ? "" : `${cache.hitRate.toFixed(1)}%`, { title: cacheTitle })}
          ${mirrorTableCell(cache ? formatCount(cache.backendReads) : "", { title: cacheTitle })}
          ${mirrorTableCell(cache ? formatCount(cache.avoidedReads) : "", { title: cacheTitle })}
          ${mirrorTableCell(cache ? formatCount(cache.evictions) : "", { title: cacheTitle, mismatch: Boolean(cache?.evictions) })}
          ${mirrorTableCell(mirrorCountText(mirror.issueCount), { mismatch: mirrorCountMismatch(mirror.issueCount, refMirror?.issueCount), title: `Source: ${mirrorCountText(refMirror?.issueCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.commitCount), { mismatch: mirrorCountMismatch(mirror.commitCount, refMirror?.commitCount), title: `Source: ${mirrorCountText(refMirror?.commitCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.branchCount), { mismatch: mirrorCountMismatch(mirror.branchCount, refMirror?.branchCount), title: `Source: ${mirrorCountText(refMirror?.branchCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.pullCount), { mismatch: mirrorCountMismatch(mirror.pullCount, refMirror?.pullCount), title: `Source: ${mirrorCountText(refMirror?.pullCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.discussionCount), { mismatch: mirrorCountMismatch(mirror.discussionCount, refMirror?.discussionCount), title: `Source: ${mirrorCountText(refMirror?.discussionCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.artifactCount), { mismatch: mirrorCountMismatch(mirror.artifactCount, refMirror?.artifactCount), title: `Source: ${mirrorCountText(refMirror?.artifactCount) || "-"}` })}
          ${mirrorTableCell(mirrorCountText(mirror.worktreeCount))}
          ${mirrorTableCell(mirrorCountText(mirror.clonesServed), { title: speed ? `${mirrorCountText(mirror.clonesServed) || "-"} clones · ${speed}` : `${mirrorCountText(mirror.clonesServed) || "-"} clones` })}
          ${mirrorTableCell(mirrorCountText(mirror.websiteServed))}
          ${mirrorDeleteCell(mirror)}
        </tr>`;
  }

  function mirrorDirectDashboardUrl(mirror) {
    const advertised = String(mirror?.dashboardUrl || "").trim();
    const gitUrl = String(mirror?.mirrorUrl || "").trim();
    try {
      const url = new URL(advertised || gitUrl);
      if (url.protocol !== "https:" || url.username || url.password) return "";
      if (!advertised) {
        if (!url.pathname.endsWith(".git")) return "";
        url.pathname = url.pathname.slice(0, -4);
      }
      const canonicalOwner = String(state.selectedRepo?.owner || "").trim();
      const canonicalName = String(state.selectedRepo?.name || "").trim();
      if (canonicalOwner && canonicalName) {
        url.pathname = `/${encodeURIComponent(canonicalOwner)}/${encodeURIComponent(canonicalName)}`;
      }
      url.search = "";
      url.searchParams.set("theme", readDashboardTheme());
      url.hash = "";
      return url.toString().replace(/\/$/, "");
    } catch (_) {
      return "";
    }
  }

  function renderMirrorRow(mirror, servedBy, refMirror) {
    const online = mirror.status === "online";
    const isServing = online && mirrorRowIsServing(mirror, servedBy);
    const speed = isServing ? formatServeSpeed(servedBy.tookMs) : "";
    const rawVersion = String(mirror.version || mirror.appVersion || mirror.clientVersion || "").trim();
    const version = rawVersion
      ? (rawVersion[0].toLowerCase() === "v" ? rawVersion : `v${rawVersion}`)
      : "";
    const integrityRejected = mirror.integrity === "rejected";
    const isSource = mirror?.isSourceOfTruth === true;
    const directUrl = mirrorDirectDashboardUrl(mirror);
    const nodeName = escapeHtml(mirror.node || mirror.machineName || mirror.name || "mirror");
    const nodeLabel = directUrl
      ? `<a data-repo-direct-mirror href="${escapeHtml(directUrl)}" rel="noopener noreferrer" title="${nodeName}" class="block min-w-0 truncate font-mono text-xs font-medium text-foreground hover:underline">${nodeName}</a>`
      : `<span title="${nodeName}" class="block min-w-0 truncate font-mono text-xs font-medium text-foreground">${nodeName}</span>`;
    return `
        <div data-repo-live-mirror-row${isServing ? ' data-serving="true"' : ""} class="flex min-w-0 items-center gap-2 rounded-md px-2 py-1 transition-colors hover:bg-secondary/60">
          <span class="flex min-w-0 flex-1 items-center gap-1.5">
            ${nodeLabel}
            ${isSource ? '<span class="shrink-0 rounded-full border border-border px-1.5 text-[10px] font-medium leading-4 text-muted-foreground">source of truth</span>' : ""}
            ${integrityRejected ? '<span class="shrink-0 rounded-full border border-destructive/40 px-1.5 text-[10px] font-medium leading-4 text-destructive">failing integrity pin</span>' : ""}
          </span>
          <span class="flex min-w-0 max-w-[45%] items-center gap-1 font-mono text-[11px] text-muted-foreground">
            ${isServing && speed
              ? `<span class="truncate text-foreground" title="Serving this page">${escapeHtml(speed)}</span>`
              : `<span class="min-w-0 truncate">${version ? escapeHtml(version) : "version unknown"}</span>`}
            ${online ? "" : `<span class="shrink-0">· ${escapeHtml(mirror.status || "unknown")}</span>`}
          </span>
          ${directUrl ? `<a data-repo-direct-mirror href="${escapeHtml(directUrl)}" rel="noopener noreferrer" title="Open direct" aria-label="Open direct on ${nodeName}" class="inline-flex h-6 w-6 shrink-0 items-center justify-center rounded-md text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="external-link" class="h-3.5 w-3.5"></i></a>` : ""}
        </div>`;
  }

  // The "Live mirror" summary in the About aside gets its own compact list of
  // every mirror currently online for this repo (the full tab-level list
  // lives under the Mirrors tab and includes offline ones too). It shares the
  // same source/integrity badges as the Mirrors tab.
  function renderRepoLiveMirrorList(mirrors, servedBy) {
    const container = $("[data-repo-live-mirror-list]");
    if (!container) return;
    const online = mirrors.filter((mirror) => mirror.status === "online");
    const refMirror = pickReferenceMirror(mirrors);
    container.innerHTML = online.length
      ? online.map((mirror) => renderMirrorRow(mirror, servedBy, refMirror)).join("")
      : '<div class="px-2 py-1 text-xs text-muted-foreground">No nodes online right now.</div>';
  }

  // A repository that has been around collects far more retired desktops than
  // live hosts, and they all keep a published catalog row. "Online only" hides
  // them without hiding the fact that they exist: the count next to the toggle
  // always reports the full roster. Off by default so the tab still opens on
  // the complete mesh view.
  function repoMirrorsOnlineOnly() {
    return state.repoMirrorsOnlineOnly === true;
  }

  function mirrorStatusToneClass(tone) {
    return "min-w-0 truncate font-mono text-[10px] " + (
      tone === "bad" ? "text-destructive"
        : tone === "good" ? "text-muted-foreground"
          : "text-muted-foreground");
  }

  // Deleting a node navigates nowhere, so its verdict is kept here rather than
  // in the DOM: the list re-renders on the refresh that follows a successful
  // delete, and the message has to survive that repaint.
  let repoMirrorStatus = { text: "", tone: "" };

  function setRepoMirrorStatus(text, tone = "") {
    repoMirrorStatus = { text: String(text || ""), tone };
    const node = $("[data-mirror-status]");
    if (!node) return;
    node.textContent = repoMirrorStatus.text;
    node.className = mirrorStatusToneClass(tone);
  }

  function renderRepoMirrorToolbar(mirrors) {
    const online = mirrors.filter((mirror) => mirror.status === "online").length;
    const offline = mirrors.length - online;
    const onlineOnly = repoMirrorsOnlineOnly();
    return `
        <div data-mirror-toolbar class="flex flex-wrap items-center justify-between gap-2 px-1 py-2">
          <div class="flex min-w-0 flex-wrap items-center gap-2">
            <span data-mirror-node-count class="font-mono text-[10px] text-muted-foreground">${escapeHtml(formatCount(online))} online${offline ? ` · ${escapeHtml(formatCount(offline))} offline` : ""}</span>
            <span data-mirror-status class="${mirrorStatusToneClass(repoMirrorStatus.tone)}">${escapeHtml(repoMirrorStatus.text)}</span>
          </div>
          <button type="button" data-repo-mirror-online-only aria-pressed="${onlineOnly ? "true" : "false"}" title="Show only the nodes serving this repository right now" class="inline-flex shrink-0 items-center gap-1.5 rounded-md border px-2 py-1 text-[10px] font-medium transition-colors hover:bg-secondary ${onlineOnly ? "border-border text-foreground" : "border-border text-muted-foreground"}">
            <i data-lucide="circle" class="h-2 w-2 ${onlineOnly ? "fill-foreground text-foreground" : "text-muted-foreground"}" aria-hidden="true"></i>
            <span>Online only</span>
          </button>
        </div>${renderRepoSyncStatus()}${renderRepoAvailabilityProof()}`;
  }

  function renderRepoSyncStatus() {
    const sync = state.repoSyncStatus;
    if (!sync) return "";
    const flash = sync.severity === "error" ? "fm-flash-danger" : sync.severity === "warning" ? "fm-flash-warning" : "fm-flash-neutral";
    const icon = sync.severity === "error" ? "circle-x" : sync.severity === "warning" ? "triangle-alert" : "refresh-cw";
    return `<p data-repo-sync-status="${escapeHtml(sync.state)}" role="status" class="fm-flash ${flash} mx-1 mb-2 px-3 py-2 text-xs"><i data-lucide="${icon}"></i><span class="fm-flash-body text-muted-foreground"><span class="font-medium text-foreground">${escapeHtml(sync.label)}</span> · ${escapeHtml(sync.summary)}</span></p>`;
  }

  // Availability proof and repository risk (forkmesh-availability-proof-v1,
  // docs/availability-proof.md). The Worker derives it from the signed
  // receipts and clone probes in this same payload; the page only renders it.
  const AVAILABILITY_CLASS_LABELS = {
    verified: "Verified",
    observed: "Observed",
    declared: "Declared",
    stale: "Stale",
    failing: "Failing",
  };

  function availabilityDocsHref(href) {
    const value = String(href || "");
    return value.startsWith("https://forkmesh.com/docs/")
      ? value
      : "https://forkmesh.com/docs/availability-proof/";
  }

  function renderRepoAvailabilityProof() {
    const proof = state.repoAvailability;
    if (!proof) return "";
    const tone = proof.level === "critical" || proof.level === "high" ? "text-destructive" : "text-foreground";
    const counts = proof.counts || {};
    const quorum = proof.quorum || {};
    const diversity = proof.diversity || {};
    const probes = proof.probes || {};
    const transports = proof.transports || {};
    const coverage = (item) => (item && item.reference != null
      ? `${formatCount(item.complete)} full of ${formatCount(item.reference)}`
      : "not reported");
    const metrics = [
      ["Active copies", `${formatCount(counts.active)} of ${formatCount(counts.total)}`],
      ["Verified", formatCount(counts.verified)],
      ["Trusted quorum", `${formatCount(quorum.operators)} of ${formatCount(quorum.target)} operators`],
      ["Operators", formatCount(diversity.operators)],
      ["Regions", `${formatCount(diversity.regions)}${diversity.regionsUndeclared ? ` · ${formatCount(diversity.regionsUndeclared)} undeclared` : ""}`],
      ["Clone probes", `${formatCount(probes.passing)} pass · ${formatCount(probes.failing)} fail`],
      ["Transports", `${formatCount(transports.router)} router · ${formatCount(transports.directHttps)} direct`],
      ["Branches", coverage(proof.coverage?.branches)],
      ["Artifacts", coverage(proof.coverage?.artifacts)],
    ];
    const risks = Array.isArray(proof.risks) ? proof.risks : [];
    const copies = Array.isArray(proof.copies) ? proof.copies : [];
    const riskItems = risks.map((risk) => `<li data-repo-availability-risk="${escapeHtml(risk.code)}" class="py-1.5">
        <div class="flex flex-wrap items-baseline gap-x-2"><span class="font-medium text-foreground">${escapeHtml(risk.title)}</span><span class="font-mono text-[10px] text-muted-foreground">${escapeHtml(risk.code)} · +${escapeHtml(String(Number(risk.weight) || 0))}</span></div>
        <p class="text-muted-foreground">${escapeHtml(risk.detail)}</p>
        <a data-repo-availability-action="${escapeHtml(risk.action?.kind || "")}" href="${escapeHtml(availabilityDocsHref(risk.action?.href))}" target="_blank" rel="noopener" class="text-accent hover:underline">${escapeHtml(risk.action?.label || "What to do")}</a>
      </li>`).join("");
    const copyRows = copies.map((copy) => `<tr data-repo-availability-copy="${escapeHtml(copy.class)}">
        <td class="px-1 py-0.5 font-mono">${escapeHtml(copy.node || "—")}</td>
        <td class="px-1 py-0.5">${escapeHtml(AVAILABILITY_CLASS_LABELS[copy.class] || copy.class || "")}</td>
        <td class="px-1 py-0.5 font-mono">#${escapeHtml(String(Number(copy.operator) || 0))}</td>
        <td class="px-1 py-0.5">${escapeHtml(copy.region || "undeclared")}</td>
        <td class="px-1 py-0.5">${escapeHtml(copy.observedAt ? relativeTimeLabel(copy.observedAt) : "never observed")}</td>
        <td class="px-1 py-0.5">${escapeHtml(copy.receiptFresh ? "fresh receipt" : "no fresh receipt")} · probe ${escapeHtml(copy.probe || "none")}</td>
        <td class="px-1 py-0.5 font-mono text-[10px]">${escapeHtml((Array.isArray(copy.reasons) ? copy.reasons : []).join(", ") || "—")}</td>
      </tr>`).join("");
    return `<section data-repo-availability-proof="${escapeHtml(proof.proof)}" aria-label="Availability proof" class="mx-1 mb-2 rounded-md border border-border px-3 py-2 text-xs">
        <div class="flex flex-wrap items-baseline justify-between gap-2">
          <p role="status"><span class="font-medium text-foreground">${escapeHtml(proof.label)}</span> · <span data-repo-availability-score class="${tone}">Risk ${escapeHtml(String(Number(proof.score) || 0))}/100 (${escapeHtml(proof.levelLabel || proof.level || "")})</span></p>
          <a href="https://forkmesh.com/docs/availability-proof/" target="_blank" rel="noopener" class="text-[10px] text-muted-foreground hover:underline">How this is calculated</a>
        </div>
        <p class="mt-1 text-muted-foreground">${escapeHtml(proof.summary)}</p>
        <dl class="mt-2 grid grid-cols-2 gap-x-4 gap-y-1 sm:grid-cols-3">${metrics
          .map(([label, value]) => `<div class="flex min-w-0 justify-between gap-2"><dt class="text-muted-foreground">${escapeHtml(label)}</dt><dd class="truncate font-mono text-foreground">${escapeHtml(value)}</dd></div>`)
          .join("")}</dl>
        ${riskItems ? `<ul class="mt-2 divide-y divide-border">${riskItems}</ul>` : ""}
        ${copyRows ? `<details data-repo-availability-evidence class="mt-2"><summary class="cursor-pointer text-muted-foreground">Evidence for ${escapeHtml(formatCount(copies.length))} ${copies.length === 1 ? "copy" : "copies"}</summary><div class="overflow-x-auto"><table class="mt-1 min-w-full whitespace-nowrap text-left"><thead><tr class="text-muted-foreground"><th scope="col" class="px-1">Node</th><th scope="col" class="px-1">Class</th><th scope="col" class="px-1">Operator</th><th scope="col" class="px-1">Region</th><th scope="col" class="px-1">Last evidence</th><th scope="col" class="px-1">Signed evidence</th><th scope="col" class="px-1">Reasons</th></tr></thead><tbody>${copyRows}</tbody></table></div></details>` : ""}
      </section>`;
  }

  // "Ask a node to mirror" remediation goes to the owner's in-page form when
  // it is shown; everyone else follows the link to the documentation.
  document.addEventListener("click", (event) => {
    const action = event.target?.closest?.('[data-repo-availability-action="request-mirror"]');
    if (!action) return;
    const form = document.querySelector("[data-mirror-request]");
    const input = form && !form.hidden ? form.querySelector("[data-mirror-request-target]") : null;
    if (!input) return;
    event.preventDefault();
    input.scrollIntoView({ block: "center" });
    input.focus();
  });

  function renderRepoMirrorLists(mirrors, servedBy) {
    const tabContainer = $("[data-repo-mirrors]");
    if (tabContainer && mirrors.length) {
      // Working-copy source first when it is among the publications, then
      // online before offline, then freshest sync.
      const refMirror = pickReferenceMirror(mirrors);
      const ordered = mirrors.slice().sort(
        (a, b) =>
          Number(b?.isSourceOfTruth === true) -
            Number(a?.isSourceOfTruth === true) ||
          Number(b === refMirror) - Number(a === refMirror) ||
          Number(b.status === "online") - Number(a.status === "online") ||
          (Number(b.lastSync) || 0) - (Number(a.lastSync) || 0) ||
          String(a.node || a.owner || a.name || "").localeCompare(String(b.node || b.owner || b.name || "")),
      );
      const visible = repoMirrorsOnlineOnly()
        ? ordered.filter((mirror) => mirror.status === "online")
        : ordered;
      tabContainer.innerHTML = `${renderRepoMirrorToolbar(ordered)}${visible.length
        ? `<div data-mirror-table-wrap class="overflow-x-auto"><table data-mirror-table class="min-w-full border-separate border-spacing-0 whitespace-nowrap text-left"><caption class="sr-only">Mirror node health</caption><thead class="sticky top-0 z-10 bg-background"><tr>${mirrorTableHead()}</tr></thead><tbody>${visible
          .map((mirror) => renderMirrorTabRow(mirror, servedBy, refMirror))
          .join("")}</tbody></table></div>`
        : '<div class="px-4 py-3 text-sm text-muted-foreground">No mirrors are online right now.</div>'}`;
    }
    renderRepoLiveMirrorList(mirrors, servedBy);
    window.lucide?.createIcons();
  }

  function repaintRepoMirrorLists() {
    renderRepoMirrorLists(
      Array.isArray(state.repoMirrors) ? state.repoMirrors : [],
      state.repoServedBy,
    );
  }

  function toggleRepoMirrorsOnlineOnly() {
    state.repoMirrorsOnlineOnly = !repoMirrorsOnlineOnly();
    repaintRepoMirrorLists();
  }

  const REPO_MIRROR_MIN_REFRESH_MS = 30 * 1000;
  let repoMirrorLoadedKey = "";
  let repoMirrorLoadedAt = 0;

  async function checkMirrorDirectUrl(button) {
    const repo = state.selectedRepo;
    const node = String(button?.dataset?.mirrorIntegrityCheck || "").trim();
    if (!repo || !node || button.disabled) return;
    const selectedKey = repoKey(repo).toLowerCase();
    button.disabled = true;
    button.textContent = "checking…";
    try {
      const headers = { accept: "application/json" };
      const token = state.session?.sessionToken || "";
      if (token) headers.authorization = `Bearer ${token}`;
      const response = await fetch(
        `${repoApiBase(repo)}/mirrors/${encodeURIComponent(node)}/reachability`,
        { method: "POST", cache: "no-store", headers },
      );
      const data = await response.json().catch(() => ({}));
      if (!response.ok || data.ok !== true) {
        throw new Error(data.error || `HTTP ${response.status}`);
      }
      if (repoKey(state.selectedRepo || {}).toLowerCase() !== selectedKey) return;
      const mirrors = Array.isArray(state.repoMirrors) ? state.repoMirrors : [];
      const mirror = mirrors.find(
        (item) => String(item?.node || "").trim().toLowerCase() === node.toLowerCase(),
      );
      if (mirror) Object.assign(mirror, data);
      renderRepoMirrorLists(mirrors, state.repoServedBy);
    } catch (error) {
      button.disabled = false;
      button.textContent = "retry";
      button.title = `Direct URL check failed: ${String(error?.message || error)}`;
    }
  }

  document.addEventListener("click", (event) => {
    const button = event.target?.closest?.("[data-mirror-integrity-check]");
    if (!button) return;
    event.preventDefault();
    void checkMirrorDirectUrl(button);
  });

  async function loadRepoMirrors(repo, options = {}) {
    const background = options.background === true;
    const force = options.force === true;
    const key = repoKey(repo).toLowerCase();
    if (
      background &&
      !force &&
      key === repoMirrorLoadedKey &&
      Date.now() - repoMirrorLoadedAt < REPO_MIRROR_MIN_REFRESH_MS
    ) {
      return false;
    }
    const container = $("[data-repo-mirrors]");
    // Owner-only "ask a node to mirror your repo" control (issue #385).
    renderMirrorRequestForm(repo);
    if (!background) repoMirrorStatus = { text: "", tone: "" };
    if (container && !background) container.innerHTML = `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading mirrors...")}</div>`;
    try {
      const data = await fetchRepoJson(`${repoApiBase(repo)}/mirrors`);
      state.repoSourceOfTruth = data.sourceOfTruth || {
        owner: data.owner || repo.owner || "",
        repo: data.repo || repo.name || "",
        kind: "working-copy",
        isSourceOfTruth: true,
      };
      const mirrors = repoNodesWithSource(
        Array.isArray(data.mirrors) ? data.mirrors : [],
        state.repoSourceOfTruth,
      );
      repoMirrorLoadedKey = key;
      repoMirrorLoadedAt = Date.now();
      // The list is the authoritative membership response.  Its summary can
      // lag a node update, which previously left the Mirrors action badge at
      // the stale summary count even while all returned nodes were rendered.
      const mirrorCount = mirrors.length;
      const onlineMirrors = mirrors.filter(
        (mirror) =>
          mirror?.status === "online" &&
          mirror?.cloneAvailable !== false,
      );
      if (onlineMirrors.length) {
        repo.cloneOnline = true;
        const availability = $("[data-repo-availability-status]");
        if (availability) {
          availability.textContent = repo.liveHost
            ? "mirror online"
            : "served by mirror";
          availability.classList.remove("text-muted-foreground");
          availability.classList.add("text-primary");
        }
      }
      updateRepoLiveCounts(repo, { mirrors: mirrorCount });
      setRepoTabCount("mirrors", mirrorCount);
      state.repoMirrors = mirrors;
      // Where the repository has reached, in the forkmesh-sync-status-v1
      // model the Worker computes for every client (docs/sync-status.md).
      state.repoSyncStatus =
        data.syncStatus && data.syncStatus.schema === "forkmesh-sync-status-v1"
          ? data.syncStatus
          : null;
      // Availability proof and risk (docs/availability-proof.md); absent on
      // an older Worker, which simply hides the panel.
      state.repoAvailability =
        data.availability && data.availability.schema === "forkmesh-availability-proof-v1"
          ? data.availability
          : null;
      // Older gateways reported commit dates at day precision. When the
      // signed mirror record names the same commit, its exact commitAt is the
      // authoritative timestamp for the repository summary.
      if (state.repoLatestCommit) {
        updateRepoCommitSummary(state.repoLatestCommit, repo);
      }
      if (state.repoServedBy) {
        renderRepoServedBy(state.repoServedBy.name, state.repoServedBy.tookMs);
      }
      if (!mirrors.length) {
        if (container) container.innerHTML = '<div class="px-4 py-3 text-sm text-muted-foreground">No mirrors reported yet.</div>';
        renderRepoLiveMirrorList([], state.repoServedBy);
        return true;
      }
      renderRepoMirrorLists(mirrors, state.repoServedBy);
      void hydrateRepoMirrorDiagnostics(mirrors, key);
      return true;
    } catch (_) {
      if (!background) {
        if (container) container.innerHTML = '<div class="px-4 py-3 text-sm text-muted-foreground">Mirror health is unavailable right now.</div>';
        renderRepoLiveMirrorList([], state.repoServedBy);
      }
      return false;
    } finally {
      window.lucide?.createIcons();
    }
  }

  // Live convergence for the open repo's Mirrors tab. The account and room
  // WebSockets carry catalog/mirror invalidations and coalesce a burst into one
  // targeted read. There is deliberately no timer behind them: a fallback
  // poll becomes the steady-state load it was meant to insure against.
  let liveMirrorRefreshTimer = null;
  function repoMirrorsTabVisible() {
    const panel = document.querySelector(
      '[data-dashboard-repo-tab-panel="mirrors"]',
    );
    return (
      document.visibilityState === "visible" &&
      state.activeRepoTab === "mirrors" &&
      Boolean(panel && !panel.classList.contains("hidden"))
    );
  }

  function refreshOpenRepoMirrors() {
    const repo = state.selectedRepo;
    // The panel is always mounted on repository pages, even when hidden.
    // Refresh only when a visitor can see the result.
    if (repo && repoMirrorsTabVisible()) {
      void loadRepoMirrors(repo, { background: true, force: true });
    }
  }
  function onLiveMirrorSignal(event) {
    const repo = state.selectedRepo;
    if (!repo) return;
    if (event?.detail?.catchUp === true) {
      void refreshServedCounts(repo);
      refreshOpenRepoMirrors();
      return;
    }
    const target = String(event?.detail?.repo || "").trim().toLowerCase();
    if (!target) return;
    // The frame carries "<catalog-owner>/<repo>". Match the open repo by its
    // full key, or fall back to the repo-name tail (older/renamed peers).
    const key = repoKey(repo).toLowerCase();
    const name = String(repo.name || "").trim().toLowerCase();
    const tail = target.slice(target.lastIndexOf("/") + 1);
    if (target !== key && !(name && tail === name)) return;
    // Trailing-coalesce a burst of per-node acks into one refetch. A reconnect
    // catch-up and an explicit tab open cover dropped/early signals without an
    // unconditional confirmation request every five minutes.
    if (liveMirrorRefreshTimer) clearTimeout(liveMirrorRefreshTimer);
    liveMirrorRefreshTimer = setTimeout(() => {
      liveMirrorRefreshTimer = null;
      // Issues and pull requests can materialize while any repository tab is
      // open. Their badges belong to the repository header, so refresh them
      // even when the Mirrors panel itself is hidden.
      void refreshServedCounts(repo);
      refreshOpenRepoMirrors();
    }, 1200);
  }
  window.addEventListener("forkmesh:mirror-signal", onLiveMirrorSignal);

  // Platform administrators can retire a node straight from the Mirrors table.
  // This is the same durable deletion the World admin panel performs, so the
  // relay applies one policy for both: it re-checks is_admin, refuses the
  // protected/source nodes, requires the typed confirmation, and writes the
  // audit record. The node's catalog publications go with it, which is what
  // makes the row disappear on the refresh below.
  async function deleteMirrorNode(button) {
    const nodeName = String(button.dataset.mirrorNode || "").trim().toLowerCase();
    if (!nodeName) {
      setRepoMirrorStatus("This node did not publish an account to delete.", "bad");
      return;
    }
    const required = `DELETE ${nodeName}`;
    const typed = window.prompt(
      `Type ${required} to permanently remove this node and everything it hosts.`,
      "",
    );
    if (typed === null) return;
    if (String(typed).trim() !== required) {
      setRepoMirrorStatus(`Confirmation did not match ${required}.`, "bad");
      return;
    }
    button.disabled = true;
    setRepoMirrorStatus(`Deleting ${nodeName}...`);
    try {
      const response = await fetch("/api/world/admin/nodes/delete", {
        method: "POST",
        headers: {
          accept: "application/json",
          authorization: `Bearer ${state.session?.sessionToken || ""}`,
          "content-type": "application/json",
        },
        credentials: "same-origin",
        cache: "no-store",
        body: JSON.stringify({
          nodeName,
          machineName: String(button.dataset.mirrorMachine || ""),
          nodeId: String(button.dataset.mirrorId || ""),
          confirmation: required,
        }),
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok || payload?.ok !== true) {
        throw new Error(String(payload?.error || "delete_failed"));
      }
      if (state.selectedRepo) {
        await loadRepoMirrors(state.selectedRepo, { background: true, force: true });
      }
      setRepoMirrorStatus(`${nodeName} was removed from ForkMesh.`, "good");
    } catch (error) {
      button.disabled = false;
      const code = String(error?.message || "");
      setRepoMirrorStatus(
        code === "not_admin" ? "Only a platform administrator can delete a node."
          : code === "protected_node" ? `${nodeName} is protected and cannot be deleted.`
            : code === "not_a_node" ? `${nodeName} is a user account, not a node.`
              : code === "organization_alias_requires_replacement" ? `${nodeName} still backs an organization alias. Re-home it first.`
                : code === "confirmation_mismatch" ? `Confirmation did not match ${required}.`
                  : `Could not delete ${nodeName}. Please try again.`,
        "bad");
    }
  }

  function renderRepoRelease(repo, release, downloads) {
    const tag = String(release.tag || "untagged");
    const channel = String(release.channel || "");
    const created = release.created_at ? formatDate(release.created_at) : "";
    const commit = String(release.tag_commit || "").slice(0, 7);
    const notes = String(release.body || release.name || "");
    const assets = Array.isArray(release.assets) ? release.assets : [];
    const isLatest = channel === "latest";
    const assetRows = assets.length
      ? assets.map((asset) => {
          const name = String(asset.name || "asset");
          const sha = String(asset.blob_sha256 || "").toLowerCase();
          const count = Number(downloads[sha]);
          const platform = [asset.os, asset.arch].filter(Boolean).join("/");
          const href = sha
            ? `${repoApiBase(repo)}/releases/blob/sha256/${encodeURIComponent(sha)}?filename=${encodeURIComponent(name)}`
            : "";
          return `
            <div class="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-3 border-t border-border px-4 py-2.5 text-sm">
              <div class="flex min-w-0 items-center gap-2">
                <i data-lucide="package" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i>
                ${href
                  ? `<a href="${escapeHtml(href)}" download="${escapeHtml(name)}" class="dashboard-accent-link min-w-0 truncate font-mono text-foreground hover:underline">${escapeHtml(name)}</a>`
                  : `<span class="min-w-0 truncate font-mono text-foreground">${escapeHtml(name)}</span>`}
                ${platform ? `<span class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-mono text-muted-foreground">${escapeHtml(platform)}</span>` : ""}
              </div>
              <div class="flex shrink-0 items-center gap-3 text-xs text-muted-foreground">
                ${Number.isFinite(count) ? `<span class="inline-flex items-center gap-1"><i data-lucide="download" class="h-3 w-3"></i>${formatCount(count)}</span>` : ""}
                <span class="font-mono">${escapeHtml(formatSize(asset.size))}</span>
              </div>
            </div>`;
        }).join("")
      : '<div class="border-t border-border px-4 py-3 text-xs text-muted-foreground">No downloadable assets are attached to this release.</div>';
    return `
      <article class="mt-4 overflow-hidden rounded-lg border border-border bg-background first:mt-0">
        <div class="flex flex-wrap items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3">
          <div class="flex min-w-0 flex-wrap items-center gap-2">
            <i data-lucide="tag" class="h-3.5 w-3.5 text-muted-foreground"></i>
            <span class="font-mono text-sm font-semibold text-foreground">${escapeHtml(tag)}</span>
            ${isLatest
              ? '<span class="rounded-full border border-border bg-primary/10 px-2 py-0.5 text-[10px] font-medium text-muted-foreground">Latest</span>'
              : channel ? `<span class="rounded-full border border-border px-2 py-0.5 text-[10px] font-mono text-muted-foreground">${escapeHtml(channel)}</span>` : ""}
          </div>
          <div class="flex shrink-0 flex-wrap items-center gap-3 text-[11px] text-muted-foreground">
            ${commit ? `<span class="inline-flex items-center gap-1 font-mono"><i data-lucide="git-commit-horizontal" class="h-3 w-3"></i>${escapeHtml(commit)}</span>` : ""}
            ${created ? `<span class="inline-flex items-center gap-1"><i data-lucide="calendar" class="h-3 w-3"></i>${escapeHtml(created)}</span>` : ""}
          </div>
        </div>
        ${notes ? `<p class="whitespace-pre-wrap border-b border-border px-4 py-3 text-sm leading-6 text-muted-foreground">${escapeHtml(notes)}</p>` : ""}
        <div>${assetRows}</div>
      </article>`;
  }

  async function loadRepoReleases(repo) {
    const container = $("[data-repo-releases]");
    if (!container) return;
    container.innerHTML = `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading releases from the live mirror...")}</div>`;
    const empty = '<div class="px-4 py-3 text-sm text-muted-foreground">No releases have been published to this mirror yet.</div>';
    try {
      // Release manifests live in the git tree at .forkmesh/releases/<channel>/release.json
      // (issue #304). List the channels, then batch-read every manifest in one
      // tunnel round-trip so opening the tab doesn't fan out N blob requests.
      let tree;
      try {
        tree = await fetchRepoJson(repoLiveUrl(
          repo,
          "tree",
          { path: ".forkmesh/releases" },
        ));
      } catch (error) {
        if (isMissingMirrorFolder(error)) {
          container.innerHTML = empty;
          return;
        }
        throw error;
      }
      const channels = (Array.isArray(tree.entries) ? tree.entries : [])
        .filter((entry) => entry.type === "tree" && entry.name)
        .map((entry) => String(entry.name));
      if (!channels.length) {
        container.innerHTML = empty;
        return;
      }
      const paths = channels.map((channel) => `.forkmesh/releases/${channel}/release.json`);
      const blobs = await fetchRepoBlobs(repo, paths);
      const releases = [];
      channels.forEach((channel, index) => {
        const blob = blobs[paths[index]];
        if (!blob) return;
        let manifest;
        try {
          manifest = JSON.parse(blobText(blob));
        } catch (_) {
          return;
        }
        if (manifest && typeof manifest === "object") {
          manifest.channel = manifest.channel || channel;
          releases.push(manifest);
        }
      });
      if (!releases.length) {
        container.innerHTML = empty;
        return;
      }
      // Per-asset download counts (keyed by sha256) are a best-effort adornment.
      let downloads = {};
      try {
        const data = await fetchJson(`${repoApiBase(repo)}/releases/downloads`);
        downloads = (data && data.counts) || {};
      } catch (_) {}
      releases.sort(compareRepoReleases);
      container.innerHTML = releases.map((release) => renderRepoRelease(repo, release, downloads)).join("");
    } catch (_) {
      container.innerHTML = '<div class="px-4 py-3 text-sm text-muted-foreground">Releases are unavailable until a reachable mirror host serves the .forkmesh/releases/ folder.</div>';
    } finally {
      window.lucide?.createIcons();
    }
  }

  function repoInsightsContributorRows(commits) {
    const byAuthor = new Map();
    (Array.isArray(commits) ? commits : []).forEach((commit) => {
      const author = String(commit.author || "unknown").trim() || "unknown";
      const current = byAuthor.get(author) || { author, commits: 0, latest: "" };
      current.commits += 1;
      current.latest = current.latest || commit.date || "";
      byAuthor.set(author, current);
    });
    return [...byAuthor.values()].sort((a, b) => b.commits - a.commits || a.author.localeCompare(b.author));
  }

  function repoInsightsStatsContributors(stats, commits) {
    const complete = (Array.isArray(stats?.contributors) ? stats.contributors : [])
      .filter((item) => item && (item.name || item.email))
      .map((item) => ({
        ...item,
        author: String(item.name || item.email).trim(),
        commits: Number(item.commits) || 0,
      }));
    return complete.length ? complete : repoInsightsContributorRows(commits);
  }

  function repoInsightsMetric(label, value, detail) {
    const shown = value === null || value === undefined
      ? "—"
      : typeof value === "number" ? formatCount(value) : String(value);
    return `<div data-repo-insights-metric="${escapeHtml(label.toLowerCase().replace(/\s+/g, "-"))}" class="min-w-0 rounded-md border border-border bg-secondary/30 p-3">
      <div class="text-[10px] font-semibold uppercase tracking-wide text-muted-foreground">${escapeHtml(label)}</div>
      <div class="mt-2 font-mono text-xl font-semibold text-foreground">${escapeHtml(shown)}</div>
      <div class="mt-1 truncate text-[10px] text-muted-foreground">${escapeHtml(detail)}</div>
    </div>`;
  }

  function repoInsightsLanguages(stats) {
    const tally = {};
    Object.entries(stats?.extensions || {}).forEach(([extension, info]) => {
      const language = REPO_LANGUAGE_EXTENSIONS[String(extension).toLowerCase()];
      const bytes = Number(info?.bytes) || 0;
      if (language && bytes > 0) tally[language] = (tally[language] || 0) + bytes;
    });
    const ranked = Object.entries(tally).sort((a, b) => b[1] - a[1]);
    const total = ranked.reduce((sum, item) => sum + item[1], 0);
    if (!total) return "";
    const segments = ranked.map(([language, bytes]) => {
      const pct = bytes / total * 100;
      const color = REPO_LANGUAGE_COLORS[language] || "#8a8a8a";
      return `<span title="${escapeHtml(`${language} ${pct.toFixed(1)}%`)}" style="width:${pct.toFixed(2)}%;background-color:${color}"></span>`;
    }).join("");
    const legend = ranked.map(([language, bytes]) => {
      const pct = bytes / total * 100;
      const color = REPO_LANGUAGE_COLORS[language] || "#8a8a8a";
      return `<span class="inline-flex items-center gap-1.5">
        <span class="h-2 w-2 rounded-full" style="background-color:${color}"></span>
        <span class="font-medium text-foreground">${escapeHtml(language)}</span>
        ${pct.toFixed(1)}% <span class="text-muted-foreground">(${escapeHtml(formatSize(bytes))})</span>
      </span>`;
    }).join("");
    return `<section aria-labelledby="repo-insights-languages">
      <h3 id="repo-insights-languages" class="mb-2 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground">Languages</h3>
      <div class="flex h-2 overflow-hidden rounded-full bg-secondary">${segments}</div>
      <div class="mt-2 flex flex-wrap gap-x-4 gap-y-1 text-[10px] text-muted-foreground">${legend}</div>
    </section>`;
  }

  function repoInsightsContributorActivity(item, sharedMax) {
    const series = (Array.isArray(item.activityWeeks) ? item.activityWeeks : [])
      .slice(-32).map((value) => Math.max(0, Number(value) || 0));
    if (!series.some(Boolean)) {
      return '<span class="text-[10px] text-muted-foreground">No commits in the last 32 weeks</span>';
    }
    const total = series.reduce((sum, value) => sum + value, 0);
    const bars = series.map((value) => {
      const height = value ? Math.max(2, Math.round(value / sharedMax * 28)) : 1;
      return `<span class="min-w-[2px] flex-1 rounded-sm ${value ? "bg-primary/75" : "bg-border"}" style="height:${height}px"></span>`;
    }).join("");
    return `<div title="${formatCount(total)} commits in the last 32 weeks" aria-label="${formatCount(total)} commits in the last 32 weeks" class="flex h-8 min-w-[12rem] items-end gap-px">${bars}</div>`;
  }

  // Git identities ForkMesh itself writes carry the ForkMesh handle as the git
  // author NAME: the desktop commits as `user.name = <account>` and the mirror
  // intake as `user.name = <node>`, so a contributor row resolves to a profile
  // exactly when its author name is an account that exists. An identity some
  // other git config produced ("ForkMesh Deploy", a person's full name, an
  // imported GitHub author) has no account behind it and stays inert text
  // rather than guessing at a profile that belongs to someone else (#636).
  const CONTRIBUTOR_HANDLE_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
  // handle -> resolved profile name ("" when the lookup answered "no profile").
  const contributorProfileNames = new Map();

  async function resolveContributorProfile(author) {
    const handle = String(author || "").trim().toLowerCase();
    // Most author names never reach the network: only a name that is already
    // shaped like a handle can name an account.
    if (!CONTRIBUTOR_HANDLE_RE.test(handle)) return "";
    if (contributorProfileNames.has(handle)) return contributorProfileNames.get(handle);
    let account;
    try {
      account = await fetchJson(`/api/accounts/${encodeURIComponent(handle)}`);
    } catch (_) {
      return ""; // a failed lookup is not an answer, so it is not remembered
    }
    // `exists` alone still covers a name that is merely reserved and a
    // quarantined account (which answers without a kind), and a private
    // profile has no public page to send a reader to.
    const resolved = account?.exists && account.available === false &&
      !account.profilePrivate &&
      (account.kind === "user" || account.kind === "node")
      ? String(account.name || handle).trim().toLowerCase()
      : "";
    contributorProfileNames.set(handle, resolved);
    return resolved;
  }

  // Contributor rows render as plain text and are upgraded to profile links
  // afterwards, so the tab never waits on the lookups. Keep profile discovery
  // bounded even though the table itself shows every contributor returned by
  // the mirror; names beyond this point remain accurate, inert git identities.
  async function linkRepoInsightsContributors(container) {
    const rows = container
      ? [...container.querySelectorAll("[data-repo-insights-contributor]")]
          .slice(0, 50)
      : [];
    for (const row of rows) {
      const author = row.getAttribute("data-repo-insights-contributor") || "";
      const handle = await resolveContributorProfile(author);
      // The panel is replaced wholesale on every repo/branch switch, so a
      // detached row belongs to a view the reader already left.
      if (!handle || !row.isConnected) continue;
      const link = document.createElement("a");
      link.className = "dashboard-accent-link hover:underline";
      link.href = publicProfileUrl(handle);
      link.title = `ForkMesh profile @${handle}`;
      link.textContent = author;
      row.replaceChildren(link);
    }
  }

  function repoTrafficDayLabel(day) {
    const date = new Date(`${day}T00:00:00Z`);
    if (Number.isNaN(date.getTime())) return String(day || "");
    return date.toLocaleDateString(undefined, {
      timeZone: "UTC", month: "short", day: "numeric",
    });
  }

  function repoTrafficChart(traffic) {
    const days = (Array.isArray(traffic?.days) ? traffic.days : [])
      .slice(-30)
      .filter((item) => item && item.day)
      .map((item) => ({
        day: String(item.day),
        clones: Math.max(0, Number(item.clones) || 0),
      }));
    if (!days.length) {
      return '<p class="mt-3 text-xs leading-5 text-muted-foreground">Clone traffic has not been recorded yet.</p>';
    }
    const total = Number.isFinite(Number(traffic?.totalClones))
      ? Math.max(0, Number(traffic.totalClones))
      : days.reduce((sum, item) => sum + item.clones, 0);
    const maximum = Math.max(1, ...days.map((item) => item.clones));
    const bars = days.map((item) => {
      const height = item.clones ? Math.max(3, Math.round(item.clones / maximum * 48)) : 1;
      const count = `${formatCount(item.clones)} ${item.clones === 1 ? "clone" : "clones"}`;
      const label = `${repoTrafficDayLabel(item.day)}: ${count}`;
      return `<span data-repo-traffic-day="${escapeHtml(item.day)}" role="img" aria-label="${escapeHtml(label)}" title="${escapeHtml(label)}" class="group relative flex min-w-[3px] flex-1 items-end"><span class="w-full rounded-sm ${item.clones ? "bg-primary/75" : "bg-border"}" style="height:${height}px"></span><span class="sr-only">${escapeHtml(label)}</span></span>`;
    }).join("");
    return `<div data-repo-traffic-chart class="mt-3" aria-label="Clone traffic over the past 30 days">
      <div class="mb-2 flex items-center justify-between gap-2 text-[10px] text-muted-foreground"><span>Last 30 days</span><span class="font-mono">${formatCount(total)} ${total === 1 ? "clone" : "clones"}</span></div>
      <div class="flex h-12 items-end gap-px" aria-label="Daily clone traffic">${bars}</div>
      <div class="mt-1 flex justify-between text-[10px] text-muted-foreground"><span>${escapeHtml(repoTrafficDayLabel(days[0].day))}</span><span>${escapeHtml(repoTrafficDayLabel(days[days.length - 1].day))}</span></div>
    </div>`;
  }

  function renderRepoInsights(repo, commits = [], stats = null, unavailable = false, traffic = null) {
    const contributors = repoInsightsStatsContributors(stats, commits);
    const statCommitTotal = normalizedCount(stats?.commitCount);
    const catalogCommitTotal = repoCount(repo, ["commits", "commitCount", "commitHistory"]);
    const commitTotal = statCommitTotal === null ? catalogCommitTotal : statCommitTotal;
    const displayedCommitTotal = Number(commitTotal) || contributors.reduce((sum, item) => sum + item.commits, 0);
    const statsContributorTotal = normalizedCount(stats?.contributorCount);
    const contributorTotal = statsContributorTotal === null ? contributors.length : statsContributorTotal;
    const counts = stats?.counts || {};
    const issuesTotal = Number.isFinite(Number(counts.issues))
      ? Number(counts.issues)
      : repoCount(repo, ["issueCount", "issues", "issuesCount"]);
    const pullsTotal = Number.isFinite(Number(counts.pulls))
      ? Number(counts.pulls)
      : repoCount(repo, ["pullCount", "pulls", "pullsCount", "pullRequests"]);
    const fileCount = normalizedCount(stats?.fileCount);
    const totalBytes = normalizedCount(stats?.totalBytes);
    const openIssues = Number(counts.openIssues);
    const closedIssues = Number(counts.closedIssues);
    const openPulls = Number(counts.openPulls);
    const closedPulls = Number(counts.closedPulls);
    const mergedPulls = Number(counts.mergedPulls);
    const closedUnmergedPulls = Number(counts.closedUnmergedPulls);
    const issueDetail = Number.isFinite(openIssues) && Number.isFinite(closedIssues)
      ? `${formatCount(openIssues)} open / ${formatCount(closedIssues)} closed`
      : "repository issues";
    const pullDetail = Number.isFinite(openPulls) && Number.isFinite(mergedPulls) && Number.isFinite(closedUnmergedPulls)
      ? `${formatCount(openPulls)} open / ${formatCount(mergedPulls)} merged / ${formatCount(closedUnmergedPulls)} closed`
      : Number.isFinite(openPulls) && Number.isFinite(closedPulls)
        ? `${formatCount(openPulls)} open / ${formatCount(closedPulls)} closed or merged`
        : "repository pull requests";
    const sharedMax = Math.max(1, ...contributors.flatMap((item) =>
      (Array.isArray(item.activityWeeks) ? item.activityWeeks.slice(-32) : []).map(Number)));
    const rows = contributors.length
      ? contributors.map((item) => {
        const share = displayedCommitTotal > 0 ? item.commits / displayedCommitTotal * 100 : 0;
        return `
        <div data-repo-insights-contributor-row class="grid min-w-[46rem] grid-cols-[minmax(14rem,1fr)_7rem_6rem_minmax(14rem,1fr)] items-center gap-4 border-t border-border px-4 py-2.5 text-sm hover:bg-secondary/20">
          <div class="flex min-w-0 items-center gap-3">
            ${repoContributorAvatar({ name: item.author, email: item.email, commits: item.commits })}
            <div data-repo-insights-contributor="${escapeHtml(item.author)}" class="truncate font-medium text-foreground">${escapeHtml(item.author)}</div>
          </div>
          <span class="text-right font-mono text-xs font-semibold text-foreground">${formatCount(item.commits)}</span>
          <span class="text-right font-mono text-xs text-muted-foreground">${share.toFixed(1)}%</span>
          ${repoInsightsContributorActivity(item, sharedMax)}
        </div>`;
      })
      : ['<div class="border-t border-border px-4 py-3 text-sm text-muted-foreground">No contributor activity is available yet.</div>'];
    const shownLabel = stats?.contributorsTruncated
      ? `${formatCount(contributors.length)} of ${formatCount(contributorTotal)}`
      : formatCount(contributorTotal);
    return `
      <div class="grid gap-4 p-4">
        ${unavailable ? '<div role="status" class="fm-flash fm-flash-warning"><i data-lucide="cloud-off"></i><p class="fm-flash-body text-foreground">The live mirror is unavailable; showing the latest catalog activity.</p></div>' : ""}
        ${!stats && !unavailable ? '<div class="fm-flash fm-flash-neutral"><i data-lucide="info"></i><p class="fm-flash-body text-muted-foreground">This mirror provides only its latest 60 commits. Upgrade it to see complete contributor, language, and code-size insights.</p></div>' : ""}
        <div class="grid gap-3 lg:grid-cols-[minmax(0,2fr)_minmax(16rem,1fr)]">
          <section class="rounded-lg border border-border p-3" aria-labelledby="repo-insights-summary">
            <h3 id="repo-insights-summary" class="mb-3 text-xs font-semibold text-foreground">Repository summary</h3>
            <div class="grid gap-2 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-5">
              ${repoInsightsMetric("Contributors", contributorTotal, "current branch")}
              ${repoInsightsMetric("Files", fileCount, "tracked blobs")}
              ${repoInsightsMetric("Code size", totalBytes === null ? null : formatSize(totalBytes), "tracked bytes")}
              ${repoInsightsMetric("Issues", issuesTotal, issueDetail)}
              ${repoInsightsMetric("Pull requests", pullsTotal, pullDetail)}
            </div>
          </section>
          <aside class="rounded-lg border border-border bg-secondary/20 p-4">
            <div class="flex items-center gap-2 text-xs font-semibold text-foreground"><i data-lucide="network" class="h-4 w-4 text-muted-foreground"></i>ForkMesh traffic</div>
            ${repoTrafficChart(traffic)}
            <p class="mt-3 text-[10px] leading-4 text-muted-foreground">Daily Git clone requests reported by managed mirror nodes.</p>
          </aside>
        </div>
        ${repoInsightsLanguages(stats)}
        <section class="overflow-hidden rounded-lg border border-border">
          <div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="text-xs font-medium text-foreground">Repository activity</span>
            <span class="font-mono text-[10px] text-muted-foreground">52 weeks · ${formatCount(displayedCommitTotal)} commits total</span>
          </div>
          <div class="p-4">${repoActivitySparkline(repo.activityWeeks, {
            totalHint: commitTotal,
            detailed: true,
            // The publisher bucketed these weeks against its own clock when the
            // record was advertised, so date the graph from that stamp.
            anchor: repo.updatedAt || repo.lastSync,
          })}</div>
        </section>
        <section class="overflow-hidden rounded-lg border border-border">
          <div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/50 px-4 py-3">
            <span class="text-xs font-medium text-foreground">Contributors &amp; activity</span>
            <span class="font-mono text-[10px] text-muted-foreground">${shownLabel}</span>
          </div>
          <div class="overflow-x-auto">
            <div class="grid min-w-[46rem] grid-cols-[minmax(14rem,1fr)_7rem_6rem_minmax(14rem,1fr)] gap-4 bg-secondary/20 px-4 py-2 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground">
              <span>Contributor</span><span class="text-right">Commits</span><span class="text-right">Share</span><span>Last 32 weeks</span>
            </div>
            <div class="max-h-[36rem] overflow-y-auto">${rows.join("")}</div>
          </div>
        </section>
      </div>`;
  }

  async function loadRepoInsights(repo) {
    const container = $("[data-repo-insights]");
    if (!container || !repo) return;
    container.innerHTML = `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading insights from the live mirror...")}</div>`;
    try {
      const [historyResult, statsResult, trafficResult] = await Promise.allSettled([
        fetchRepoJson(repoLiveUrl(repo, "history")),
        fetchRepoJson(repoLiveUrl(repo, "stats")),
        fetchJson(`${repoApiBase(repo)}/traffic`),
      ]);
      if (historyResult.status === "rejected" && statsResult.status === "rejected") throw new Error("mirror unavailable");
      const history = historyResult.status === "fulfilled" ? historyResult.value : {};
      const stats = statsResult.status === "fulfilled" ? statsResult.value : null;
      const traffic = trafficResult.status === "fulfilled" && trafficResult.value?.ok
        ? trafficResult.value
        : null;
      const commits = Array.isArray(history?.commits) ? history.commits : [];
      container.innerHTML = renderRepoInsights(repo, commits, stats, false, traffic);
      void linkRepoInsightsContributors(container);
    } catch (_) {
      container.innerHTML = renderRepoInsights(repo, [], null, true);
    } finally {
      hydrateRepoActivityGraphs(container);
      window.lucide?.createIcons();
    }
  }

  function loadRepoFeaturePanels(repo, recordRoute = null, compareRoute = null) {
    loadRepoMirrors(repo);
    // Feature panels load on their FIRST tab view (and reload here after a
    // repo/branch switch if that tab is already active): fetching hidden live
    // data for every repo open fires Worker/host reads for tabs nobody opened.
    // The tab badges stay filled meanwhile from the root tree's bundled counts.
    state.loadedRepoTabs = state.loadedRepoTabs || {};
    const active = state.activeRepoTab || "code";
    if (active === "commits") {
      state.loadedRepoTabs.commits = true;
      loadRepoCommits(repo);
    } else if (active === "issues") {
      state.loadedRepoTabs.issues = true;
      // A refreshed/shared issue deep link (/owner/repo/issues/<N>) opens that
      // issue's detail straight away; Back re-fetches the list lazily.
      if (recordRoute && recordRoute.kind === "issues" && recordRoute.number) {
        loadRepoRecordDetail(repo, "issues", recordRoute.number);
      } else {
        loadRepoIssues(repo);
      }
    } else if (active === "pulls" || active === "discussions") {
      state.loadedRepoTabs[active] = true;
      // A record deep link (/owner/repo/pulls/<N> — e.g. the desktop client's
      // "View on website" button) opens the record's detail page directly
      // instead of the list; Back to the list loads it lazily from there.
      if (recordRoute && recordRoute.kind === active && recordRoute.number) {
        loadRepoRecordDetail(repo, active, recordRoute.number);
      } else if (active === "pulls" && compareRoute) {
        // A comparison deep link (/owner/repo/compare/base...head) opens the
        // diff viewer directly; Back to the list loads it lazily from there.
        openRepoCompare(repo, compareRoute.base, compareRoute.head, { navigate: false });
      } else {
        loadRepoCollection(repo, active, `[data-repo-${active}]`);
      }
    } else if (active === "releases") {
      state.loadedRepoTabs.releases = true;
      loadRepoReleases(repo);
    } else if (active === "insights") {
      state.loadedRepoTabs.insights = true;
      loadRepoInsights(repo);
    } else if (active === "sizemap") {
      state.loadedRepoTabs.sizemap = true;
      loadRepoSizeMapTab(repo);
    } else if (active === "agents") {
      state.loadedRepoTabs.agents = true;
      loadRepoAgents(repo);
    } else if (active === "branches") {
      state.loadedRepoTabs.branches = true;
      loadRepoBranches(repo);
    } else if (active === "actions") {
      void loadRepoActions(repo);
    }
  }

  function updateRepoLiveCounts(repo, counts) {
    if (!counts || typeof counts !== "object") return;
    (["commits", "issues", "pulls", "discussions", "mirrors"]).forEach((key) => {
      const value = Number(counts[key]);
      if (!Number.isFinite(value) || value < 0) return;
      repo[key] = value;
      $$(`[data-dashboard-repo-count="${key}"]`).forEach((badge) => {
        badge.textContent = formatCount(value);
      });
    });
  }

  function repoCount(repo, keys) {
    for (const key of keys) {
      const value = Number(repo[key]);
      if (Number.isFinite(value) && value >= 0) return value;
    }
    return null;
  }

  function repoDefaultBranch(repo) {
    // The mirror's branch listing names the ref HEAD actually points at. It
    // outranks the catalog row, which can still carry the name the
    // repository was published under before its default branch was renamed.
    const listed = repo ? state.repoBranches[repoKey(repo)]?.defaultBranch : "";
    return listed || repo?.defaultBranch || repo?.branch || repo?.rootBranch || "main";
  }

  function normalizeRepoBranch(branch) {
    if (!branch || typeof branch !== "object") {
      const name = String(branch || "").trim();
      return name ? { name } : null;
    }
    const name = String(branch.name || branch.branch || branch.ref || "").replace(/^refs\/heads\//, "").trim();
    if (!name) return null;
    return {
      name,
      commit: branch.commit || branch.hash || branch.sha || "",
      updatedAt: branch.updatedAt || branch.date || branch.committedAt || "",
      // The mirror's for-each-ref listing already carries the tip commit's
      // subject and author; the Branches page shows them, the dropdown
      // ignores them.
      message: branch.message || branch.subject || "",
      author: branch.author || branch.authorName || "",
      // Commits this branch adds to, and is missing from, the default branch.
      // The mirror computes both in one walk; an older mirror sends neither,
      // and null keeps "unknown" distinguishable from a real zero.
      ahead: Number.isFinite(branch.ahead) ? branch.ahead : null,
      behind: Number.isFinite(branch.behind) ? branch.behind : null,
    };
  }

  function repoSelectedBranch(repo) {
    const key = repoKey(repo);
    return state.selectedBranches[key] || repoDefaultBranch(repo);
  }

  function repoBranchList(repo) {
    const loaded = state.repoBranches[repoKey(repo)]?.branches;
    const source = Array.isArray(loaded) && loaded.length
      ? loaded
      : Array.isArray(repo.branches)
        ? repo.branches
        : [repoDefaultBranch(repo)];
    const seen = new Set();
    const branches = [];
    source.forEach((branch) => {
      const normalized = normalizeRepoBranch(branch);
      if (!normalized || seen.has(normalized.name)) return;
      seen.add(normalized.name);
      branches.push(normalized);
    });
    const current = repoSelectedBranch(repo);
    if (current && !seen.has(current)) branches.unshift({ name: current });
    return branches;
  }

  function repoBranchCount(repo) {
    const listed = repoBranchList(repo).length;
    if (state.repoBranches[repoKey(repo)]?.loaded) return listed;
    // A catalog row carries no branch array, so before the live listing
    // arrives repoBranchList() only knows the selected branch and the
    // toolbar would claim "1 Branch" for every repository. The count the
    // mirror publishes alongside it is the honest number to show meanwhile.
    const published = repoCount(repo, ["branchCount"]);
    return Number.isFinite(published) && published > listed ? published : listed;
  }

  function repoBranchesQuery(repo) {
    return state.repoBranchesQueries[repoKey(repo)] || "";
  }

  function setRepoBranchesQuery(repo, query) {
    if (!repo) return;
    state.repoBranchesQueries[repoKey(repo)] = String(query || "");
  }

  function repoBranchQuery(repo) {
    return state.repoBranchQueries[repoKey(repo)] || "";
  }

  function setRepoBranchQuery(repo, query) {
    if (!repo) return;
    state.repoBranchQueries[repoKey(repo)] = String(query || "");
  }

  function filterRepoBranches(repo, branches) {
    const query = repoBranchQuery(repo).trim().toLowerCase();
    if (!query) return branches;
    return branches.filter((branch) => String(branch.name || "").toLowerCase().includes(query));
  }

  function setRepoSelectedBranch(repo, branch) {
    const name = String(branch || "").trim();
    if (!repo || !name) return;
    state.selectedBranches[repoKey(repo)] = name;
  }

  function renderRepoBranchButton(branch) {
    return `<button type="button" data-repo-branch-button aria-haspopup="menu" aria-expanded="false" class="inline-flex h-9 w-full max-w-full items-center justify-center gap-2 rounded-md border border-border bg-secondary px-3 text-xs font-medium text-foreground hover:bg-secondary/80 transition-colors"><i data-lucide="git-branch" class="h-3.5 w-3.5 shrink-0"></i><span data-repo-branch-current class="min-w-0 truncate">${escapeHtml(branch)}</span><i data-lucide="chevron-down" class="h-3 w-3 shrink-0 text-muted-foreground"></i></button>`;
  }

  // The count next to the branch picker is the way into the Branches page
  // (adhoc #2095) - it used to be an inert label, which left the site with no
  // way to see every branch at once the way the desktop and mobile clients do.
  function renderRepoBranchSummary(repo) {
    const count = repoBranchCount(repo);
    const href = `${repoLinkUrl(repo)}/branches`;
    return `<a href="${escapeHtml(href)}" data-repo-branch-summary data-repo-branches-open class="inline-flex h-9 items-center gap-2 whitespace-nowrap text-xs font-semibold text-muted-foreground hover:text-foreground transition-colors"><i data-lucide="git-branch" class="h-3.5 w-3.5"></i><span data-repo-branch-count class="text-foreground">${formatCount(count)}</span><span>${count === 1 ? "Branch" : "Branches"}</span></a>`;
  }

  function repoBranchAgentTone(status) {
    const value = String(status || "").toLowerCase();
    if (value === "success") return "text-success";
    if (value === "failed" || value === "stopped") return "text-destructive";
    if (value === "running") return "text-attention";
    return "text-muted-foreground";
  }

  function findRepoBranchAgent(repo, branch) {
    const key = String(branch || "").trim().toLowerCase();
    if (!key || !repo || !repo.owner || !repo.name) return null;
    const agents = Array.isArray(state.agentsView?.agents) ? state.agentsView.agents : [];
    const candidates = agents.filter(
      (agent) => String(agent.branchName || "").trim().toLowerCase() === key,
    );
    return candidates[0] || null;
  }

  function renderRepoBranchAgentStatus(repo, branch) {
    const agent = findRepoBranchAgent(repo, branch);
    if (!agent || !agent.id) return "";
    return `
      <button type="button" data-repo-agent-open data-repo-agent-id="${escapeHtml(String(agent.id))}" data-repo-agent-composer-mode="add" data-repo-branch-agent-status class="inline-flex min-w-0 items-center gap-1.5 rounded-md border border-border bg-secondary/60 px-2 py-1.5 text-[11px] font-semibold hover:bg-secondary/80">
        <i data-lucide="bot" class="h-3.5 w-3.5 text-muted-foreground shrink-0"></i>
        <span class="rounded-full border border-border px-1.5 py-0.5 ${repoBranchAgentTone(agent.status)}">${escapeHtml(agent.status || "unknown")}</span>
        <span class="hidden truncate min-w-0 sm:inline text-muted-foreground">on this branch</span>
      </button>`;
  }

  function renderRepoBranchToolbar(repo, branch) {
    return `<div data-repo-branch-toolbar class="flex min-w-0 flex-nowrap items-center gap-3"><div data-repo-branch-control class="relative inline-flex min-w-0 max-w-64 shrink">${renderRepoBranchButton(branch)}${renderRepoBranchMenu(repo, repoBranchList(repo), false)}</div>${renderRepoBranchSummary(repo)}${renderRepoBranchAgentStatus(repo, branch)}</div>`;
  }

  function renderRepoBranchMenu(repo, branches, open) {
    const current = repoSelectedBranch(repo);
    const query = repoBranchQuery(repo);
    const defaultBranch = repoDefaultBranch(repo);
    const filtered = new Set(filterRepoBranches(repo, branches).map((branch) => branch.name || ""));
    const rows = branches.map((branch) => {
      const name = branch.name || "main";
      if (!filtered.has(name)) return "";
      const checked = name === current;
      const commit = String(branch.commit || "").slice(0, 7);
      const updated = branch.updatedAt ? formatDate(branch.updatedAt) : "";
      return `
        <button type="button" role="menuitemradio" aria-checked="${checked ? "true" : "false"}" data-repo-branch-name="${escapeHtml(name)}" class="fm-menu-item">
          <i data-lucide="git-branch"></i>
          <span class="min-w-0 flex-1 truncate${checked ? " font-semibold" : ""}">${escapeHtml(name)}</span>
          ${name === defaultBranch ? '<span data-repo-branch-default class="fm-label">default</span>' : ""}
          ${commit || updated ? `<span class="hidden max-w-32 truncate font-mono text-[11px] text-muted-foreground sm:inline">${escapeHtml([commit, updated].filter(Boolean).join(" · "))}</span>` : ""}
        </button>`;
    }).join("");
    return `
      <div data-repo-branch-menu role="menu" class="${open ? "" : "hidden"} fm-popover absolute left-0 top-full z-30 mt-1 w-[22rem] max-w-[calc(100vw-2rem)] overflow-hidden">
        <div class="flex items-center justify-between gap-3 py-2 pl-3 pr-2">
          <h3 class="text-[13px] font-semibold text-foreground">Switch branches</h3>
          <button type="button" data-repo-branch-close aria-label="Close branch picker" class="fm-btn fm-btn-sm fm-btn-icon fm-btn-invisible"><i data-lucide="x"></i></button>
        </div>
        <div class="px-2 pb-2">
          <label class="flex h-8 items-center gap-2 rounded-md border border-border bg-background px-2 text-[13px] text-muted-foreground focus-within:border-accent focus-within:ring-1 focus-within:ring-accent">
            <i data-lucide="search" class="h-4 w-4 shrink-0"></i>
            <input data-repo-branch-search type="search" autocomplete="off" spellcheck="false" value="${escapeHtml(query)}" placeholder="Find a branch..." class="min-w-0 flex-1 bg-transparent text-foreground outline-none placeholder:text-muted-foreground" />
          </label>
        </div>
        <div class="border-t border-border px-3 pt-2 text-[11px] font-medium text-muted-foreground">Branches</div>
        <div class="fm-menu max-h-72 overflow-auto">
          ${rows || '<div class="px-2 py-3 text-center text-[13px] text-muted-foreground">No branches match this search.</div>'}
        </div>
        <a href="${escapeHtml(`${repoLinkUrl(repo)}/branches`)}" data-repo-branch-view-all data-plugin-entry="branches" class="flex h-9 w-full items-center justify-center border-t border-border px-3 text-[13px] font-medium text-accent hover:bg-foreground/5">View all branches</a>
      </div>`;
  }

  function closeRepoBranchMenus() {
    $$("[data-repo-branch-menu]").forEach((menu) => menu.classList.add("hidden"));
    $$("[data-repo-branch-button]").forEach((button) => {
      button.setAttribute("aria-expanded", "false");
    });
  }

  function updateRepoBranchControls(repo, openControl = null) {
    const restoreSearchFocus = openControl && document.activeElement?.matches?.("[data-repo-branch-search]");
    $$("[data-repo-branch-control]").forEach((control) => {
      const open = openControl === control;
      const branch = repoSelectedBranch(repo);
      control.innerHTML = `${renderRepoBranchButton(branch)}${renderRepoBranchMenu(repo, repoBranchList(repo), open)}`;
      control.querySelector("[data-repo-branch-button]")?.setAttribute("aria-expanded", open ? "true" : "false");
      const toolbar = control.closest("[data-repo-branch-toolbar]");
      const status = toolbar?.querySelector("[data-repo-branch-agent-status]");
      const nextStatus = renderRepoBranchAgentStatus(repo, branch);
      if (nextStatus) {
        if (status) status.outerHTML = nextStatus;
        else if (toolbar) toolbar.insertAdjacentHTML("beforeend", nextStatus);
      } else if (status) {
        status.remove();
      }
    });
    $$("[data-repo-branch-summary]").forEach((summary) => {
      summary.outerHTML = renderRepoBranchSummary(repo);
    });
    window.lucide?.createIcons();
    if (restoreSearchFocus) {
      const input = openControl.querySelector("[data-repo-branch-search]");
      input?.focus();
      input?.setSelectionRange?.(input.value.length, input.value.length);
    }
  }

  // One listing serves both the toolbar dropdown and the Branches page, so
  // opening either warms the other instead of asking the mirror twice.
  async function fetchRepoBranches(repo, { refresh = false } = {}) {
    const key = repoKey(repo);
    if (!refresh && state.repoBranches[key]?.loaded) {
      return state.repoBranches[key].branches;
    }
    const data = await fetchRepoJson(`${repoApiBase(repo)}/branches`);
    const branches = (Array.isArray(data.branches) ? data.branches : [])
      .map(normalizeRepoBranch)
      .filter(Boolean);
    const defaultBranch = String(data.defaultBranch || "").trim();
    state.repoBranches[key] = { branches, loaded: true, defaultBranch };
    // A re-read is the answer to "did that operation land?", so the merge
    // trials taken against the previous tips must not outlive it.
    if (refresh) state.repoBranchTrials[key] = {};
    return branches;
  }

  async function toggleRepoBranchMenu(repo, button) {
    if (!repo || !button) return;
    const control = button.closest("[data-repo-branch-control]");
    const wasOpen = control?.querySelector("[data-repo-branch-menu]")?.classList.contains("hidden") === false;
    closeRepoBranchMenus();
    if (!control || wasOpen) return;
    button.setAttribute("aria-expanded", "true");
    control.querySelector("[data-repo-branch-menu]")?.classList.remove("hidden");

    const key = repoKey(repo);
    if (state.repoBranches[key]?.loaded) return;
    try {
      await fetchRepoBranches(repo);
    } catch (_) {
      state.repoBranches[key] = { branches: repoBranchList(repo), loaded: false };
    }
    updateRepoBranchControls(repo, control);
  }

  const REPO_BRANCHES_UNAVAILABLE_HTML = '<div class="px-4 py-3 text-sm text-muted-foreground">Branches are unavailable until a reachable mirror host serves this repository.</div>';

  // The default branch heads the list; the rest are most-recently-updated
  // first, so an active branch is never buried under an alphabetical run of
  // stale ones (the mirror returns them sorted by refname).
  function repoBranchesSorted(repo, branches) {
    const defaultBranch = repoDefaultBranch(repo);
    return branches.slice().sort((left, right) => {
      const leftDefault = left.name === defaultBranch;
      const rightDefault = right.name === defaultBranch;
      if (leftDefault !== rightDefault) return leftDefault ? -1 : 1;
      const leftTime = Date.parse(left.updatedAt || "") || 0;
      const rightTime = Date.parse(right.updatedAt || "") || 0;
      if (leftTime !== rightTime) return rightTime - leftTime;
      return String(left.name || "").localeCompare(String(right.name || ""));
    });
  }

  function filterRepoBranchesPage(repo, branches) {
    const query = repoBranchesQuery(repo).trim().toLowerCase();
    if (!query) return branches;
    return branches.filter((branch) => String(branch.name || "").toLowerCase().includes(query));
  }

  // How a queued operation reads while the owner's node has not taken it yet.
  const REPO_BRANCH_OP_LABELS = {
    delete: "deletion queued",
    update: "update queued",
    create: "creation queued",
    merge: "merge queued",
  };

  const REPO_BRANCH_HELD_LABELS = {
    delete: "deletion waiting",
    update: "update waiting",
    create: "creation waiting",
    merge: "merge waiting",
  };

  const REPO_BRANCH_QUEUE_LIMIT = 1024;
  const REPO_BRANCH_HELD_RETRY_MS = 20000;
  const repoBranchHeldTimers = new Map();
  const repoBranchHeldSending = new Set();

  const REPO_BRANCH_OP_ERRORS = {
    not_authorized: "Only the repository owner can manage its branches.",
    invalid_branch: "That branch name is not one Git can use.",
    invalid_commit: "The branch moved; reload the list and try again.",
    default_branch: "That operation cannot target the default branch.",
    unsupported_operation: "That branch operation is not supported.",
    queue_full: "This repository's node has not taken the branch operations already queued for it. They run when it next comes online; try again after that, or cancel some queued ones.",
  };

  function repoBranchOps(repo) {
    if (!repo) return [];
    const ops = state.repoBranchOps[repoKey(repo)]?.ops;
    return Array.isArray(ops) ? ops : [];
  }

  function repoBranchHeld(repo) {
    if (!repo) return [];
    return state.repoBranchHeld[repoKey(repo)] || [];
  }

  function repoBranchOpFor(repo, branch) {
    return repoBranchOps(repo).find((op) => op.branch === branch)
      || repoBranchHeld(repo).find((op) => op.branch === branch)
      || null;
  }

  function repoBranchQueueLimit(repo) {
    const limit = Number(state.repoBranchOps[repoKey(repo)]?.limit);
    return limit > 0 ? limit : REPO_BRANCH_QUEUE_LIMIT;
  }

  function repoBranchResults(repo) {
    if (!repo) return [];
    const results = state.repoBranchOps[repoKey(repo)]?.results;
    return Array.isArray(results) ? results : [];
  }

  function repoBranchResultFor(repo, branch) {
    return repoBranchResults(repo).find((result) => result.branch === branch) || null;
  }

  function repoBranchTip(repo, name) {
    return String(repoBranchList(repo).find((branch) => branch.name === name)?.commit || "");
  }

  function setRepoBranchOpsState(repo, data) {
    const key = repoKey(repo);
    const previous = state.repoBranchOps[key] || {};
    state.repoBranchOps[key] = {
      ops: Array.isArray(data?.ops) ? data.ops : [],
      results: Array.isArray(data?.results) ? data.results : (previous.results || []),
      limit: Number(data?.limit) > 0 ? Number(data.limit) : previous.limit,
    };
  }

  function announceRepoBranchOp(message, bad = false) {
    setRepoBranchesStatus(message, bad);
    if (state.repoCompare) state.repoCompare.status = { message: String(message || ""), bad };
    const slot = $("[data-repo-compare-merge-status]");
    if (!slot) return;
    slot.textContent = String(message || "");
    slot.hidden = !message;
    slot.className = `text-xs ${bad ? "text-destructive" : "text-muted-foreground"}`;
  }

  function repoBranchTrial(repo, branch) {
    if (!repo) return null;
    return state.repoBranchTrials[repoKey(repo)]?.[branch] || null;
  }

  function repoBranchManageAllowed(repo) {
    return repo && !repoReadOnlyMirror(repo) ? state.repoBranchManage[repoKey(repo)] === true : false;
  }

  function repoBranchBusy(repo) {
    return repo ? state.repoBranchBusy[repoKey(repo)] || "" : "";
  }

  function setRepoBranchesStatus(message, bad = false) {
    const slot = $("[data-repo-branches-status]");
    if (!slot) return;
    slot.textContent = String(message || "");
    slot.hidden = !message;
    slot.className = `border-b border-border px-4 py-2 text-xs ${bad ? "text-destructive" : "text-muted-foreground"}`;
  }

  // "2 behind · 1 ahead", the same divergence summary the desktop app's
  // branch table shows, plus the conflict verdict of a real merge trial on
  // the mirror once one has been taken for a branch that diverged both ways.
  function repoBranchDivergenceText(branch, defaultBranch) {
    if (branch.name === defaultBranch) return "default branch";
    // Unknown, not zero: an older mirror publishes no counts, and a branch
    // the page only knows by name (the one being browsed) has none either.
    if (!Number.isFinite(branch.ahead) || !Number.isFinite(branch.behind)) return "";
    // Contributing nothing of its own means the default branch already has
    // everything this branch ever had, however far behind it has since
    // fallen. That, not its behind count, is what makes it safe to delete.
    if (branch.ahead === 0) return `merged into ${defaultBranch}`;
    const parts = [];
    if (branch.behind > 0) parts.push(`${formatCount(branch.behind)} behind`);
    parts.push(`${formatCount(branch.ahead)} ahead`);
    return parts.join(" · ");
  }

  function renderRepoBranchesRow(repo, branch, current, defaultBranch) {
    const name = branch.name || "";
    const hash = String(branch.commit || "");
    const shortHash = hash.slice(0, 7);
    // Relative, like the tree's commit column: an absolute date makes every
    // branch look identically stale, when recency is the whole point of a
    // branch list.
    const updated = branch.updatedAt ? `updated ${formatTimeAgo(branch.updatedAt)}` : "";
    const tip = [branch.message || "No commit subject reported", branch.author || "", updated]
      .filter(Boolean).join(" · ");
    const divergence = repoBranchDivergenceText(branch, defaultBranch);
    const trial = repoBranchTrial(repo, name);
    const conflicted = trial?.clean === false;
    const pending = repoBranchOpFor(repo, name);
    const manage = repoBranchManageAllowed(repo) && name !== defaultBranch;
    const busy = repoBranchBusy(repo) === name;
    const running = repoBranchRunning(repo);
    const runningHere = running && running.branch === name ? running : null;
    const disabled = Boolean(pending || busy);
    const action = (attribute, icon, label, extra = "") => `
      <button type="button" ${attribute} data-repo-branch-target="${escapeHtml(name)}" data-repo-branch-oid="${escapeHtml(hash)}"${disabled ? " disabled" : ""} class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium ${extra || "text-muted-foreground hover:bg-secondary hover:text-foreground"} transition-colors disabled:cursor-not-allowed disabled:opacity-50"><i data-lucide="${icon}" class="h-3.5 w-3.5"></i>${label}</button>`;
    return `
      <div data-repo-branches-row data-repo-branches-row-name="${escapeHtml(name)}" class="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-3 border-t border-border px-4 py-3 transition-colors hover:bg-secondary/30">
        <div class="min-w-0">
          <div class="flex min-w-0 flex-wrap items-center gap-2">
            <i data-lucide="git-branch" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i>
            <button type="button" data-repo-branch-name="${escapeHtml(name)}" class="min-w-0 truncate text-sm font-semibold text-foreground hover:underline">${escapeHtml(name)}</button>
            ${name === defaultBranch ? '<span data-repo-branch-default class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold text-foreground">default</span>' : ""}
            ${name === current ? '<span data-repo-branches-current class="shrink-0 rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold text-muted-foreground">viewing</span>' : ""}
            ${divergence ? `<span data-repo-branch-divergence class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-medium text-muted-foreground">${escapeHtml(divergence)}</span>` : ""}
            ${conflicted ? `<span data-repo-branch-conflicts title="${escapeHtml(`Conflicts with ${defaultBranch} in ${(trial.conflicts || []).slice(0, 8).join(", ") || "one or more files"}`)}" class="shrink-0 rounded-full border border-destructive/40 px-2 py-0.5 text-[10px] font-semibold text-destructive">conflicts</span>` : ""}
            ${runningHere ? `<span data-repo-branch-running title="${escapeHtml(runningHere.message || "")}" class="inline-flex shrink-0 items-center gap-1 rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold text-foreground"><i data-lucide="loader-circle" class="h-3 w-3 animate-spin"></i>${escapeHtml(REPO_BRANCH_OP_RUNNING_LABELS[runningHere.op] || "working…")}</span>` : ""}
            ${pending ? `<span data-repo-branch-pending class="inline-flex shrink-0 items-center gap-1 rounded-full border border-border bg-secondary px-2 py-0.5 text-[10px] font-semibold text-muted-foreground">${escapeHtml((pending.held ? REPO_BRANCH_HELD_LABELS : REPO_BRANCH_OP_LABELS)[pending.op] || "queued")}${manage ? `<button type="button" data-repo-branch-op-cancel data-repo-branch-op-id="${escapeHtml(String(pending.id))}" aria-label="Cancel the queued ${escapeHtml(pending.op)} of ${escapeHtml(name)}" class="text-muted-foreground hover:text-foreground">✕</button>` : ""}</span>` : ""}
          </div>
          <div class="mt-1 min-w-0 truncate text-xs text-muted-foreground">${escapeHtml(tip)}</div>
        </div>
        <div class="flex shrink-0 items-center gap-2">
          ${shortHash ? `<button type="button" data-dashboard-commit-hash="${escapeHtml(hash)}" data-dashboard-commit-short-hash class="font-mono text-xs font-semibold text-foreground hover:underline">${escapeHtml(shortHash)}</button>` : ""}
          ${manage && branch.behind > 0 ? action("data-repo-branch-update", "git-pull-request-arrow", `Update from ${escapeHtml(defaultBranch)}`) : ""}
          ${manage && branch.ahead > 0 ? action("data-repo-branch-merge", "git-merge", `Merge into ${escapeHtml(defaultBranch)}`, "text-foreground hover:bg-secondary") : ""}
          <button type="button" data-repo-branch-name="${escapeHtml(name)}" class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="code-2" class="h-3.5 w-3.5"></i>Browse</button>
          ${manage ? action("data-repo-branch-delete", "trash-2", "Delete", "text-destructive hover:bg-destructive/10") : ""}
        </div>
      </div>`;
  }

  // Branch management is the owner's action, and the relay refuses anything
  // else, so the controls are only drawn for a viewer the same gate accepts.
  function renderRepoBranchesActions(repo) {
    const slot = $("[data-repo-branches-actions]");
    if (!slot) return;
    if (!repoBranchManageAllowed(repo)) {
      slot.innerHTML = "";
      return;
    }
    const defaultBranch = repoDefaultBranch(repo);
    const merged = repoMergedRepoBranches(repo, defaultBranch);
    const behind = repoBranchesBehindDefault(repo, defaultBranch);
    slot.innerHTML = `
      <button type="button" data-repo-branch-create class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-foreground hover:bg-secondary transition-colors"><i data-lucide="git-branch-plus" class="h-3.5 w-3.5"></i>New branch</button>
      ${behind.length ? `<button type="button" data-repo-branch-update-all class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-foreground hover:bg-secondary transition-colors"><i data-lucide="git-merge" class="h-3.5 w-3.5"></i>Update ${formatCount(behind.length)} from ${escapeHtml(defaultBranch)}</button>` : ""}
      ${merged.length ? `<button type="button" data-repo-branch-delete-merged class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-destructive hover:bg-destructive/10 transition-colors"><i data-lucide="trash-2" class="h-3.5 w-3.5"></i>Delete ${formatCount(merged.length)} merged</button>` : ""}
      <button type="button" data-repo-branches-refresh class="inline-flex h-8 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors"><i data-lucide="refresh-cw" class="h-3.5 w-3.5"></i>Refresh</button>`;
  }

  // Fully contained in the default branch and contributing nothing of their
  // own: the set the desktop app's "Delete merged branches" acts on.
  function repoMergedRepoBranches(repo, defaultBranch) {
    return repoBranchList(repo).filter((branch) => (
      branch.name
      && branch.name !== defaultBranch
      && branch.ahead === 0
      && branch.commit
      && !repoBranchOpFor(repo, branch.name)
    ));
  }

  // Behind the default branch and not already queued: the set the desktop
  // app's "Pull <default> into all branches" acts on. A branch with work of
  // its own is included — the node merges it, or refuses if that conflicts.
  function repoBranchesBehindDefault(repo, defaultBranch) {
    return repoBranchList(repo).filter((branch) => (
      branch.name
      && branch.name !== defaultBranch
      && branch.behind > 0
      && branch.commit
      && !repoBranchOpFor(repo, branch.name)
    ));
  }

  function renderRepoBranchesList(repo) {
    const container = $("[data-repo-branches]");
    if (!container || !repo) return;
    const all = repoBranchesSorted(repo, repoBranchList(repo));
    const rows = filterRepoBranchesPage(repo, all);
    const total = $("[data-repo-branches-total]");
    if (total) total.textContent = `${formatCount(all.length)} ${all.length === 1 ? "branch" : "branches"}`;
    container.innerHTML = rows.length
      ? rows.map((branch) => renderRepoBranchesRow(
        repo, branch, repoSelectedBranch(repo), repoDefaultBranch(repo))).join("")
      : '<div class="px-4 py-3 text-sm text-muted-foreground">No branches match this search.</div>';
    renderRepoBranchesActions(repo);
    renderRepoBranchResults(repo);
    window.lucide?.createIcons();
  }

  function repoBranchResultTone(result) {
    if (result.status === "applied") return { icon: "circle-check", tone: "text-foreground" };
    if (result.status === "already") return { icon: "circle-dot", tone: "text-muted-foreground" };
    return { icon: "circle-x", tone: "text-destructive" };
  }

  function renderRepoBranchResultRow(result) {
    const { icon, tone } = repoBranchResultTone(result);
    const when = result.reportedAt ? formatTimeAgo(new Date(result.reportedAt).toISOString()) : "";
    const merge = String(result.mergeOid || "");
    return `
      <div data-repo-branch-result data-repo-branch-result-status="${escapeHtml(result.status || "")}" class="flex min-w-0 items-center gap-2 px-4 py-2 text-xs">
        <i data-lucide="${icon}" class="h-3.5 w-3.5 shrink-0 ${tone}"></i>
        <span class="min-w-0 flex-1 truncate ${tone}" title="${escapeHtml(result.message || "")}">${escapeHtml(result.message || `${result.op} ${result.branch}`)}</span>
        ${merge ? `<button type="button" data-dashboard-commit-hash="${escapeHtml(merge)}" class="shrink-0 font-mono text-[10px] font-semibold text-foreground hover:underline">${escapeHtml(merge.slice(0, 7))}</button>` : ""}
        ${when ? `<span class="shrink-0 text-[10px] text-muted-foreground">${escapeHtml(when)}</span>` : ""}
      </div>`;
  }

  function renderRepoBranchResults(repo) {
    const slot = $("[data-repo-branch-results]");
    if (!slot) return;
    const results = repoBranchManageAllowed(repo) ? repoBranchResults(repo).slice(0, 5) : [];
    if (!results.length) {
      slot.innerHTML = "";
      slot.hidden = true;
      return;
    }
    slot.hidden = false;
    slot.innerHTML = `
      <div class="flex items-center justify-between gap-3 border-b border-border bg-secondary/30 px-4 py-2">
        <span class="text-xs font-medium text-foreground">Reported by this repository's node</span>
        <button type="button" data-repo-branch-results-clear class="fm-btn fm-btn-sm fm-btn-invisible">Clear</button>
      </div>
      <div class="divide-y divide-border border-b border-border">${results.map(renderRepoBranchResultRow).join("")}</div>`;
  }

  async function clearRepoBranchResults(repo) {
    if (!repo) return;
    try {
      await orgApiRequest("DELETE", `${repoApiBase(repo)}/branches/ops/results`);
      setRepoBranchOpsState(repo, { ops: repoBranchOps(repo), results: [] });
    } catch (_) {
      announceRepoBranchOp("The reported results could not be cleared.", true);
    }
    renderRepoBranchesList(repo);
  }

  function repaintRepoBranchOps(repo) {
    if (!repo || state.selectedRepo !== repo) return;
    if (state.activeRepoTab === "branches") renderRepoBranchesList(repo);
    if (state.repoCompare) renderRepoCompareMergePanel(repo);
  }

  const REPO_BRANCH_OPS_POLL_MS = 2000;
  const REPO_BRANCH_OPS_POLL_ROUNDS = 90;

  function watchRepoBranchOps(repo, requestIds = []) {
    if (!repo || !state.session?.sessionToken) return;
    const key = repoKey(repo);
    const watch = state.repoBranchOpsWatch[key] || { waiting: new Set(), rounds: 0, timer: 0 };
    requestIds.filter(Boolean).forEach((id) => watch.waiting.add(Number(id)));
    watch.rounds = 0;
    state.repoBranchOpsWatch[key] = watch;
    if (watch.timer) return;
    const tick = async () => {
      watch.timer = 0;
      watch.rounds += 1;
      const seen = new Set(repoBranchResults(repo).map((result) => result.id));
      await loadRepoBranchOps(repo);
      const fresh = repoBranchResults(repo).filter((result) => !seen.has(result.id));
      fresh.forEach((result) => watch.waiting.delete(Number(result.requestId)));
      if (fresh.length) {
        const applied = fresh.filter((result) => result.status !== "refused").length;
        const refused = fresh.length - applied;
        announceRepoBranchOp(
          fresh.length === 1
            ? fresh[0].message
            : `The node reported ${fresh.length} results: ${applied} done, ${refused} refused.`,
          refused > 0 && applied === 0,
        );
        try {
          await fetchRepoBranches(repo, { refresh: true });
        } catch (_) {
        }
        if (state.repoCompare) {
          const { base, head } = state.repoCompare;
          state.repoCompare.merge = null;
          if (state.selectedRepo === repo) openRepoCompare(repo, base, head, { navigate: false });
        }
      }
      repaintRepoBranchOps(repo);
      const idle = !repoBranchOps(repo).length && !watch.waiting.size;
      if (idle || watch.rounds >= REPO_BRANCH_OPS_POLL_ROUNDS || state.selectedRepo !== repo) {
        delete state.repoBranchOpsWatch[key];
        return;
      }
      watch.timer = setTimeout(tick, REPO_BRANCH_OPS_POLL_MS);
    };
    watch.timer = setTimeout(tick, REPO_BRANCH_OPS_POLL_MS);
  }

  function ensureRepoBranchMergeDialog() {
    let dialog = $("[data-repo-branch-merge-dialog]");
    if (dialog) return dialog;
    dialog = document.createElement("dialog");
    dialog.setAttribute("data-repo-branch-merge-dialog", "");
    dialog.setAttribute("aria-label", "Merge branch");
    dialog.className = "w-full max-w-lg overflow-visible border-0 bg-transparent p-0 text-foreground backdrop:bg-black/50";
    document.body.appendChild(dialog);
    return dialog;
  }

  function repoMergePreviewHtml(preview, branch, base) {
    if (!preview) {
      return `<span class="text-muted-foreground">${loadingHtml("Testing the merge on the live mirror...")}</span>`;
    }
    if (preview.error) {
      return '<span class="text-muted-foreground">The mirror could not test this merge. The node checks it again before merging.</span>';
    }
    if (preview.ahead === 0) {
      return `<span class="text-muted-foreground"><span class="font-mono text-foreground">${escapeHtml(branch)}</span> is already in <span class="font-mono text-foreground">${escapeHtml(base)}</span>. Nothing new would be merged.</span>`;
    }
    if (preview.clean === false) {
      const files = (preview.conflicts || []).slice(0, 8);
      return `<span class="text-destructive">Conflicts with <span class="font-mono">${escapeHtml(base)}</span>${files.length ? ` in ${files.map((file) => `<span class="font-mono">${escapeHtml(file)}</span>`).join(", ")}` : ""}. Update the branch from ${escapeHtml(base)} and resolve them on the desktop, or have an agent fix them.</span>`;
    }
    const counts = [`${formatCount(preview.ahead || 0)} ${preview.ahead === 1 ? "commit" : "commits"} to merge`];
    if (preview.behind > 0) counts.push(`${formatCount(preview.behind)} behind`);
    return `<span class="text-foreground">No conflicts. ${escapeHtml(counts.join(" · "))}.</span>`;
  }

  async function fetchRepoMergePreview(repo, base, head) {
    try {
      const preview = await fetchRepoJson(
        `${repoApiBase(repo)}/merge-preview?base=${encodeURIComponent(base)}&head=${encodeURIComponent(head)}`,
      );
      return {
        ahead: Number.isFinite(Number(preview.ahead)) ? Number(preview.ahead) : null,
        behind: Number.isFinite(Number(preview.behind)) ? Number(preview.behind) : null,
        clean: preview.clean !== false,
        conflicts: Array.isArray(preview.conflicts) ? preview.conflicts : [],
        baseOid: String(preview.baseOid || ""),
        headOid: String(preview.headOid || ""),
      };
    } catch (_) {
      return { error: true };
    }
  }

  function repoBranchMergeRequest(repo, branch, base, tips, deleteBranch) {
    const defaultBranch = repoDefaultBranch(repo);
    const branchOid = tips.headOid || repoBranchTip(repo, branch);
    const baseOid = tips.baseOid || repoBranchTip(repo, base);
    if (branch === defaultBranch) {
      return {
        request: { op: "update", branch: base, expectedOid: baseOid, base: branch },
        message: `Queued an update of ${base} from ${branch} for this repository's node.`,
        ready: Boolean(baseOid),
      };
    }
    return {
      request: { op: "merge", branch, expectedOid: branchOid, base, baseOid, deleteBranch },
      message: `Queued the merge of ${branch} into ${base}${deleteBranch ? `, then the deletion of ${branch}` : ""}. This repository's node runs it right away.`,
      ready: Boolean(branchOid && baseOid),
    };
  }

  async function openRepoBranchMergeDialog(repo, branch, base, preview = null) {
    if (!repo || !branch || !base || branch === base || repoReadOnlyMirror(repo)) return;
    const dialog = ensureRepoBranchMergeDialog();
    const defaultBranch = repoDefaultBranch(repo);
    const intoOther = branch === defaultBranch;
    const paint = (current) => {
      const blocked = current && !current.error && current.clean === false;
      dialog.innerHTML = `
        <form method="dialog" data-repo-branch-merge-form class="fm-dialog">
          <header class="fm-dialog-header items-start">
            <div class="min-w-0 flex-1">
              <h2 class="fm-dialog-title">${intoOther ? "Update" : "Merge"} branch</h2>
              <p class="mt-1.5 flex flex-wrap items-center gap-1.5 text-xs text-muted-foreground">
                <span class="fm-label font-mono text-foreground">${escapeHtml(branch)}</span>
                <i data-lucide="arrow-right" class="h-3.5 w-3.5"></i>
                <span class="fm-label font-mono text-foreground">${escapeHtml(base)}</span>
              </p>
            </div>
            <button type="button" data-repo-branch-merge-cancel aria-label="Close" class="fm-btn fm-btn-sm fm-btn-icon fm-btn-invisible"><i data-lucide="x"></i></button>
          </header>
          <div class="fm-dialog-body grid gap-3 text-xs">
            <div data-repo-branch-merge-preview>${repoMergePreviewHtml(current, branch, base)}</div>
            <div class="grid gap-1">
              <span class="text-xs font-medium text-foreground">Merge commit</span>
              <code class="rounded-md border border-border bg-secondary/50 px-2 py-1.5 font-mono text-foreground">${escapeHtml(intoOther ? `Merge branch '${branch}' into ${base}` : `Merge ${branch} into ${base}`)}</code>
            </div>
            ${intoOther ? "" : `<label class="flex items-start gap-2 text-foreground"><input type="checkbox" data-repo-branch-merge-delete checked class="mt-0.5" /><span>Delete <span class="font-mono">${escapeHtml(branch)}</span> after merging, with its agent worktree if it has one</span></label>`}
            <p class="text-muted-foreground">The node that hosts this repository runs the merge right away, or as soon as it comes online, the same way the desktop app's merge does. It refuses if either branch moves first, if an agent is still working on the branch, or if the merge conflicts.</p>
          </div>
          <footer class="fm-dialog-footer">
            <button type="button" data-repo-branch-merge-cancel class="fm-btn fm-btn-sm">Cancel</button>
            <button type="submit" data-repo-branch-merge-confirm${blocked ? " disabled" : ""} class="fm-btn fm-btn-sm fm-btn-cta"><i data-lucide="git-merge"></i>${intoOther ? `Update ${escapeHtml(base)}` : `Merge into ${escapeHtml(base)}`}</button>
          </footer>
        </form>`;
      window.lucide?.createIcons();
    };
    let current = preview;
    paint(current);
    dialog.onclick = (event) => {
      if (event.target.closest("[data-repo-branch-merge-cancel]")) dialog.close();
    };
    dialog.onsubmit = async (event) => {
      event.preventDefault();
      const deleteBranch = dialog.querySelector("[data-repo-branch-merge-delete]")?.checked === true;
      const tips = current && !current.error ? current : {};
      const { request, message, ready } = repoBranchMergeRequest(repo, branch, base, tips, deleteBranch);
      dialog.close();
      if (!ready) {
        announceRepoBranchOp(`The current tips of ${branch} and ${base} are unknown, so the merge cannot be pinned yet. Refresh and try again.`, true);
        return;
      }
      await queueRepoBranchOp(repo, request, message);
    };
    if (!dialog.open) dialog.showModal();
    if (!current) {
      current = await fetchRepoMergePreview(repo, base, branch);
      if (dialog.open) paint(current);
    }
  }

  async function updateRepoBranchFrom(repo, branch, base, commit) {
    if (!repo || !branch || !base) return;
    await queueRepoBranchOp(
      repo,
      { op: "update", branch, expectedOid: commit || repoBranchTip(repo, branch), base },
      `Queued an update of ${branch} from ${base}.`,
    );
  }

  function renderRepoCompareMergePanel(repo) {
    const slot = $("[data-repo-compare-merge]");
    const compare = state.repoCompare;
    if (!slot || !repo || !compare) return;
    if (!repoBranchManageAllowed(repo) || compare.base === compare.head) {
      slot.innerHTML = "";
      slot.hidden = true;
      return;
    }
    const { base, head } = compare;
    const defaultBranch = repoDefaultBranch(repo);
    const preview = compare.merge?.preview || null;
    const pending = repoBranchOpFor(repo, head) || repoBranchOpFor(repo, base);
    const result = repoBranchResultFor(repo, head);
    const status = compare.status || null;
    const conflicted = preview && !preview.error && preview.clean === false;
    const nothing = preview && !preview.error && preview.ahead === 0;
    const busy = Boolean(pending || repoBranchBusy(repo));
    const canUpdateHead = head !== defaultBranch && preview && !preview.error && preview.behind > 0;
    const button = (attribute, icon, label, primary = false, disabled = false) => `
      <button type="button" ${attribute}${disabled ? " disabled" : ""} class="fm-btn fm-btn-sm${primary ? " fm-btn-cta" : ""}"><i data-lucide="${icon}"></i>${label}</button>`;
    slot.hidden = false;
    slot.innerHTML = `
      <div class="fm-flash ${conflicted ? "fm-flash-danger" : "fm-flash-neutral"} grid gap-3">
        <div class="flex flex-wrap items-center justify-between gap-3">
          <div class="flex min-w-0 items-start gap-2 text-xs">
            <i data-lucide="${conflicted ? "triangle-alert" : "git-merge"}" class="mt-0.5 h-4 w-4 shrink-0 ${conflicted ? "text-destructive" : "text-muted-foreground"}"></i>
            <div data-repo-compare-merge-preview class="min-w-0">${repoMergePreviewHtml(preview, head, base)}</div>
          </div>
          <div class="flex flex-wrap items-center gap-2">
            ${canUpdateHead ? button("data-repo-compare-update", "git-pull-request-arrow", `Update ${escapeHtml(head)} from ${escapeHtml(base)}`, false, busy) : ""}
            ${button("data-repo-compare-merge-open", "git-merge", head === defaultBranch ? `Update ${escapeHtml(base)}` : `Merge into ${escapeHtml(base)}`, true, busy || conflicted || nothing)}
          </div>
        </div>
        ${pending ? `<div data-repo-compare-merge-pending class="flex items-center gap-2 text-xs text-muted-foreground"><i data-lucide="clock" class="h-3.5 w-3.5"></i>${escapeHtml((pending.held ? REPO_BRANCH_HELD_LABELS : REPO_BRANCH_OP_LABELS)[pending.op] || "queued")} for ${escapeHtml(pending.branch)}: waiting for this repository's node.<button type="button" data-repo-branch-op-cancel data-repo-branch-op-id="${escapeHtml(String(pending.id))}" class="font-medium text-accent hover:underline">Cancel</button></div>` : ""}
        ${result ? `<div data-repo-compare-merge-result class="-mx-4 border-t border-border">${renderRepoBranchResultRow(result)}</div>` : ""}
        <p data-repo-compare-merge-status role="status" aria-live="polite"${status?.message ? "" : " hidden"} class="text-xs ${status?.bad ? "text-destructive" : "text-muted-foreground"}">${escapeHtml(status?.message || "")}${status?.outcome && status.outcome.message === status.message ? `<span data-repo-branch-outcome class="block">${repoBranchOutcomeExtrasHtml(status.outcome)}</span>` : ""}</p>
      </div>`;
    window.lucide?.createIcons();
  }

  async function loadRepoCompareMergePanel(repo, compare) {
    if (!repo || !compare || !state.session?.sessionToken) return;
    if (!(await loadRepoBranchManageAuthorization(repo))) return;
    if (state.repoCompare !== compare) return;
    if (!state.repoBranchOps[repoKey(repo)]) await loadRepoBranchOps(repo);
    compare.merge = { preview: null };
    renderRepoCompareMergePanel(repo);
    const preview = await fetchRepoMergePreview(repo, compare.base, compare.head);
    if (state.repoCompare !== compare) return;
    compare.merge = { preview };
    renderRepoCompareMergePanel(repo);
    if (repoBranchOps(repo).length) watchRepoBranchOps(repo);
  }

  // A diverged branch may or may not still merge. Only the mirror that would
  // run the merge can answer that, and only for one pair at a time, so the
  // trials are taken lazily: branches that diverged BOTH ways (the only ones
  // that can conflict), newest first, and a hard cap so a repository with
  // hundreds of stale agent branches cannot turn one page view into hundreds
  // of merge trials.
  const REPO_BRANCH_TRIAL_LIMIT = 8;

  async function probeRepoBranchTrials(repo) {
    if (!repo) return;
    const key = repoKey(repo);
    const defaultBranch = repoDefaultBranch(repo);
    const trials = state.repoBranchTrials[key] || (state.repoBranchTrials[key] = {});
    const listing = state.repoBranches[key]?.branches;
    const candidates = repoBranchesSorted(repo, repoBranchList(repo)).filter(
      (branch) => branch.name !== defaultBranch
        && branch.ahead > 0
        && branch.behind > 0
        && branch.commit
        && !trials[branch.name],
    ).slice(0, REPO_BRANCH_TRIAL_LIMIT);
    let painted = false;
    for (const branch of candidates) {
      try {
        const preview = await fetchRepoJson(
          `${repoApiBase(repo)}/merge-preview?base=${encodeURIComponent(defaultBranch)}&head=${encodeURIComponent(branch.name)}`,
        );
        // A refresh (or another repository) replaced the listing while this
        // trial was in flight: its verdict describes tips nobody is looking
        // at any more.
        if (state.repoBranches[key]?.branches !== listing) return;
        trials[branch.name] = {
          clean: preview.clean !== false,
          conflicts: Array.isArray(preview.conflicts) ? preview.conflicts : [],
        };
        painted = true;
      } catch (_) {
        // An unreachable mirror is not a conflict verdict. Leave the branch
        // unmarked rather than claiming either answer.
        break;
      }
    }
    if (painted && state.activeRepoTab === "branches") renderRepoBranchesList(repo);
  }

  async function loadRepoBranchOps(repo) {
    if (!repo || !state.session?.sessionToken) return;
    try {
      const data = await orgApiRequest("GET", `${repoApiBase(repo)}/branches/ops`);
      setRepoBranchOpsState(repo, data);
    } catch (_) {
      // Reading the queue is not what the page is for; a failure here leaves
      // the list itself intact.
      state.repoBranchOps[repoKey(repo)] = { ops: [], results: [] };
    }
  }

  async function loadRepoBranchManageAuthorization(repo) {
    if (!repo) return false;
    const key = repoKey(repo);
    if (typeof state.repoBranchManage[key] === "boolean") {
      return state.repoBranchManage[key];
    }
    const allowed = await loadRepoIssueMutationAuthorization(repo);
    state.repoBranchManage[key] = allowed === true;
    return state.repoBranchManage[key];
  }

  const REPO_BRANCH_OP_RETRYABLE = new Set(["merge_status_pending", "merge_node_unavailable"]);

  const REPO_BRANCH_OP_RUNNING_LABELS = {
    merge: "merging…",
    update: "updating…",
    delete: "deleting…",
  };

  async function postRepoBranchOp(repo, body) {
    const token = state.session?.sessionToken || "";
    const headers = { accept: "application/json", "content-type": "application/json" };
    if (token) headers.authorization = "Bearer " + token;
    const response = await fetch(`${repoApiBase(repo)}/branches/ops`, {
      method: "POST",
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: JSON.stringify(body),
    });
    const data = await response.json().catch(() => ({}));
    return { status: response.status, data: data && typeof data === "object" ? data : {} };
  }

  function repoBranchResumeKey(request) {
    return `${request.op || ""}|${request.branch || ""}|${request.base || ""}`;
  }

  function repoBranchResumeBody(body, data) {
    const resume = {
      ...body,
      requestId: String(data.requestId || ""),
      node: String(data.node || ""),
      pinBase: String(data.pinBase || ""),
    };
    if (data.pinOid) resume.pinOid = String(data.pinOid);
    return resume;
  }

  function repoBranchRunning(repo) {
    return repo ? state.repoBranchRunning[repoKey(repo)] || null : null;
  }

  function repoBranchOutcomeExtrasHtml(outcome) {
    if (!outcome) return "";
    const files = Array.isArray(outcome.conflicts) ? outcome.conflicts.slice(0, 20) : [];
    const button = (attribute, icon, label) => `<button type="button" ${attribute} class="inline-flex h-7 items-center gap-1.5 rounded-md border border-border bg-background px-2.5 text-xs font-medium text-foreground hover:bg-secondary transition-colors"><i data-lucide="${icon}" class="h-3.5 w-3.5"></i>${label}</button>`;
    const buttons = [
      files.length && !state.repoCompare
        ? button(`data-repo-branch-outcome-compare data-repo-branch-outcome-base="${escapeHtml(outcome.compareBase || "")}" data-repo-branch-outcome-head="${escapeHtml(outcome.compareHead || "")}"`, "git-compare-arrows", "Compare and check conflicts")
        : "",
      outcome.actions ? button("data-repo-branch-open-actions", "workflow", "Open Actions") : "",
    ].filter(Boolean).join("");
    return [
      files.length ? `<span data-repo-branch-outcome-conflicts class="mt-1 grid gap-0.5 font-mono text-[11px] text-destructive">${files.map((file) => `<span class="block truncate">${escapeHtml(file)}</span>`).join("")}</span>` : "",
      outcome.actions ? `<span class="mt-1 block text-muted-foreground">Workflows that run on a push to ${escapeHtml(outcome.base || "the default branch")} start on the cores now${outcome.mergeOid ? ` for ${escapeHtml(String(outcome.mergeOid).slice(0, 7))}` : ""}.</span>` : "",
      buttons ? `<span data-repo-branch-outcome-actions class="mt-2 flex flex-wrap gap-2">${buttons}</span>` : "",
    ].join("");
  }

  function paintRepoBranchOutcome(repo) {
    const outcome = repo ? state.repoBranchOutcome[repoKey(repo)] : null;
    const extras = repoBranchOutcomeExtrasHtml(outcome);
    if (!extras) return;
    ["[data-repo-branches-status]", "[data-repo-compare-merge-status]"].forEach((selector) => {
      const slot = $(selector);
      if (!slot || slot.hidden || slot.querySelector("[data-repo-branch-outcome]")
        || !slot.textContent.startsWith(outcome.message)) return;
      slot.insertAdjacentHTML("beforeend", `<span data-repo-branch-outcome class="block">${extras}</span>`);
    });
    window.lucide?.createIcons();
  }

  function setRepoBranchOutcome(repo, outcome) {
    state.repoBranchOutcome[repoKey(repo)] = outcome;
    announceRepoBranchOp(outcome.message, outcome.bad);
    if (state.repoCompare?.status) state.repoCompare.status.outcome = outcome;
    paintRepoBranchOutcome(repo);
  }

  function repoBranchOpOutcome(repo, body, answer) {
    const data = answer.data;
    const ok = answer.status >= 200 && answer.status < 300 && data.ok !== false;
    const base = String(data.base || body.base || repoDefaultBranch(repo));
    const merged = ok && body.op === "merge" && data.status === "merged";
    const conflicts = data.error === "merge_conflict" && Array.isArray(data.conflicts) ? data.conflicts.map(String) : [];
    const fallback = ok ? `${body.op} ${body.branch}: ${data.status || "done"}.` : `The core refused it (${data.error || `HTTP ${answer.status}`}).`;
    const sourceBase = body.op === "update" ? String(body.base || repoDefaultBranch(repo)) : base;
    return {
      ok,
      message: `${String(data.message || fallback)}${conflicts.length ? ` Conflicting files (${conflicts.length}):` : ""}`,
      bad: !ok,
      conflicts,
      compareBase: sourceBase,
      compareHead: body.branch || "",
      actions: merged && base === repoDefaultBranch(repo),
      base,
      mergeOid: String(data.mergeOid || ""),
      branchDeleted: data.branchDeleted === true,
    };
  }

  async function refreshRepoBranchesAfterCores(repo, outcome) {
    await loadRepoBranchOps(repo);
    try {
      await fetchRepoBranches(repo, { refresh: true });
    } catch (_) {
    }
    const compare = state.repoCompare;
    if (compare && state.selectedRepo === repo) {
      if (outcome.branchDeleted && outcome.ok) {
        compare.merge = { preview: { ahead: 0, behind: 0, clean: true, conflicts: [], baseOid: "", headOid: "" } };
        renderRepoCompareMergePanel(repo);
      } else {
        await loadRepoCompareMergePanel(repo, compare);
      }
    }
    repaintRepoBranchOps(repo);
    paintRepoBranchOutcome(repo);
  }

  async function runRepoBranchOpOnCores(repo, body, first, { refresh = true } = {}) {
    const key = repoKey(repo);
    const resumeKey = repoBranchResumeKey(body);
    let answer = first;
    let resume = body.requestId ? body : null;
    for (let round = 0; ; round += 1) {
      const data = answer.data;
      const working = answer.status === 202
        || (answer.status === 503 && resume && REPO_BRANCH_OP_RETRYABLE.has(data.error));
      if (!working) break;
      if (answer.status === 202) resume = repoBranchResumeBody(body, data);
      if (round >= REPO_BRANCH_OPS_POLL_ROUNDS) {
        state.repoBranchResume[`${key}|${resumeKey}`] = resume;
        delete state.repoBranchRunning[key];
        setRepoBranchOutcome(repo, {
          ok: false,
          bad: true,
          message: `The core is still working on ${body.branch}. Run the same action again to keep waiting for that request.`,
        });
        return { ok: false, executed: true, data };
      }
      state.repoBranchRunning[key] = {
        op: body.op,
        branch: body.branch,
        message: String(data.message || `Working on ${body.branch} on the cores…`),
      };
      announceRepoBranchOp(state.repoBranchRunning[key].message);
      repaintRepoBranchOps(repo);
      await new Promise((resolve) => setTimeout(resolve, REPO_BRANCH_OPS_POLL_MS));
      answer = await postRepoBranchOp(repo, resume).catch(() => ({
        status: 503,
        data: { ok: false, executed: true, error: "merge_status_pending", message: "The result is not known yet." },
      }));
    }
    delete state.repoBranchRunning[key];
    delete state.repoBranchResume[`${key}|${resumeKey}`];
    const data = answer.data;
    if (Array.isArray(data.results)) {
      state.repoBranchOps[key] = { ops: repoBranchOps(repo), results: data.results };
    }
    const outcome = repoBranchOpOutcome(repo, body, answer);
    setRepoBranchOutcome(repo, outcome);
    if (refresh) await refreshRepoBranchesAfterCores(repo, outcome);
    return { ok: outcome.ok, executed: true, data };
  }

  async function sendRepoBranchOp(repo, request, message, options = {}) {
    if (!repo) return { ok: false, executed: false, data: {} };
    const key = repoKey(repo);
    state.repoBranchBusy[key] = request.branch || "";
    delete state.repoBranchOutcome[key];
    repaintRepoBranchOps(repo);
    try {
      const body = { ...request, defaultBranch: repoDefaultBranch(repo) };
      const pinned = Array.isArray(request.ops) ? null : state.repoBranchResume[`${key}|${repoBranchResumeKey(body)}`];
      const sent = pinned ? { ...body, requestId: pinned.requestId, node: pinned.node, pinBase: pinned.pinBase, ...(pinned.pinOid ? { pinOid: pinned.pinOid } : {}) } : body;
      const answer = await postRepoBranchOp(repo, sent);
      const data = answer.data;
      if (data.executed === true) {
        return await runRepoBranchOpOnCores(repo, sent, answer, options);
      }
      if (answer.status < 200 || answer.status >= 300 || data.ok === false) {
        const error = new Error(orgErrorText(data.error || ""));
        error.code = data.error || `http_${answer.status}`;
        error.data = data;
        throw error;
      }
      setRepoBranchOpsState(repo, data);
      const text = typeof message === "function" ? message(data) : message;
      if (text) announceRepoBranchOp(text);
      const wanted = Array.isArray(request.ops) ? request.ops : [request];
      const queued = repoBranchOps(repo).filter((op) => wanted.some(
        (item) => item.op === op.op && item.branch === op.branch));
      watchRepoBranchOps(repo, queued.map((op) => op.id));
      return { ok: true, executed: false, data };
    } catch (error) {
      if (error?.code === "queue_full") {
        holdRepoBranchOps(repo, Array.isArray(request.ops) ? request.ops : [request]);
        delete state.repoBranchBusy[key];
        announceRepoBranchHeld(repo, await sendRepoBranchHeld(repo, error.data?.room));
        return { ok: true, executed: false, held: true, data: error.data || {} };
      }
      announceRepoBranchOp(
        (error?.code === "read_only_mirror" && error?.data?.message)
          || REPO_BRANCH_OP_ERRORS[error?.code]
          || "The branch operation could not be queued.",
        true,
      );
      return { ok: false, executed: false, data: {} };
    } finally {
      delete state.repoBranchBusy[key];
      delete state.repoBranchRunning[key];
      repaintRepoBranchOps(repo);
      paintRepoBranchOutcome(repo);
    }
  }

  async function queueRepoBranchOp(repo, request, message) {
    const sent = await sendRepoBranchOp(repo, request, message);
    return sent.ok;
  }

  async function runRepoBranchBatch(repo, ops, queuedMessage, verb) {
    if (!repo || !ops.length) return;
    const first = await sendRepoBranchOp(repo, ops[0], null, { refresh: ops.length === 1 });
    if (!first.ok) return;
    if (!first.executed) {
      const firstQueued = Number(first.data?.queuedCount) || 0;
      if (ops.length === 1) {
        announceRepoBranchOp(queuedMessage(firstQueued));
        return;
      }
      await sendRepoBranchOp(
        repo, { ops: ops.slice(1) },
        (data) => queuedMessage(firstQueued + (Number(data.queuedCount) || 0)),
      );
      return;
    }
    if (ops.length === 1) return;
    let done = 1;
    let stopped = null;
    for (const op of ops.slice(1)) {
      const next = await sendRepoBranchOp(repo, op, null, { refresh: false });
      if (!next.ok) {
        stopped = state.repoBranchOutcome[repoKey(repo)] || { message: "", bad: true };
        break;
      }
      done += 1;
    }
    const summary = stopped
      ? { ...stopped, bad: true, message: `Stopped after ${done} of ${ops.length}: ${stopped.message}` }
      : { ok: true, bad: false, message: `${verb} ${done} of ${ops.length} ${ops.length === 1 ? "branch" : "branches"}.` };
    await refreshRepoBranchesAfterCores(repo, summary);
    setRepoBranchOutcome(repo, summary);
  }

  function holdRepoBranchOps(repo, requests) {
    const key = repoKey(repo);
    const held = repoBranchHeld(repo);
    const known = (request) => (
      held.some((entry) => entry.op === request.op && entry.branch === request.branch)
      || repoBranchOps(repo).some((op) => op.op === request.op && op.branch === request.branch)
    );
    const fresh = requests.filter((request) => request?.op && request?.branch && !known(request));
    state.repoBranchHeld[key] = held.concat(fresh.map((request) => ({
      id: `held:${request.op}:${request.branch}`,
      op: request.op,
      branch: request.branch,
      held: true,
      request,
    })));
  }

  function dropRepoBranchHeld(repo, entries) {
    const key = repoKey(repo);
    const left = repoBranchHeld(repo).filter((entry) => !entries.includes(entry));
    if (left.length) state.repoBranchHeld[key] = left;
    else delete state.repoBranchHeld[key];
  }

  async function sendRepoBranchHeld(repo, room) {
    const key = repoKey(repo);
    const outcome = { sent: 0, dropped: 0, error: "" };
    if (repoBranchHeldSending.has(key)) return outcome;
    repoBranchHeldSending.add(key);
    const given = Number(room);
    let space = room !== undefined && room !== null && Number.isFinite(given)
      ? given
      : repoBranchQueueLimit(repo) - repoBranchOps(repo).length;
    try {
      while (space > 0 && repoBranchHeld(repo).length) {
        const batch = repoBranchHeld(repo).slice(0, space);
        try {
          const data = await orgApiRequest(
            "POST", `${repoApiBase(repo)}/branches/ops`, {
              ops: batch.map((entry) => entry.request),
              defaultBranch: repoDefaultBranch(repo),
            });
          setRepoBranchOpsState(repo, data);
          outcome.sent += Number(data.queuedCount) || 0;
          dropRepoBranchHeld(repo, batch);
          const queued = repoBranchOps(repo).filter((op) => batch.some(
            (entry) => entry.op === op.op && entry.branch === op.branch));
          watchRepoBranchOps(repo, queued.map((op) => op.id));
          space = repoBranchQueueLimit(repo) - repoBranchOps(repo).length;
        } catch (error) {
          const code = String(error?.code || "");
          const left = Number(error?.data?.room);
          if (code === "queue_full" && left > 0 && left < batch.length) {
            space = left;
            continue;
          }
          if (code === "invalid_request" || (code !== "queue_full" && REPO_BRANCH_OP_ERRORS[code])) {
            dropRepoBranchHeld(repo, code === "not_authorized" ? repoBranchHeld(repo) : batch);
            outcome.dropped += batch.length;
            outcome.error = code;
          }
          break;
        }
      }
      if (repoBranchHeld(repo).length) scheduleRepoBranchHeld(repo);
    } finally {
      repoBranchHeldSending.delete(key);
      repaintRepoBranchOps(repo);
    }
    return outcome;
  }

  function scheduleRepoBranchHeld(repo) {
    const key = repoKey(repo);
    if (repoBranchHeldTimers.has(key)) return;
    repoBranchHeldTimers.set(key, setTimeout(async () => {
      repoBranchHeldTimers.delete(key);
      if (!repoBranchHeld(repo).length) return;
      await loadRepoBranchOps(repo);
      const outcome = await sendRepoBranchHeld(repo);
      if (outcome.sent || outcome.error) announceRepoBranchHeld(repo, outcome);
    }, REPO_BRANCH_HELD_RETRY_MS));
  }

  function announceRepoBranchHeld(repo, outcome) {
    if (state.selectedRepo !== repo) return;
    const waiting = repoBranchHeld(repo).length;
    const plural = (count, one, many) => (count === 1 ? one : many);
    const parts = [];
    if (outcome.sent) {
      parts.push(`Queued ${formatCount(outcome.sent)} branch ${plural(outcome.sent, "operation", "operations")} for this repository's node.`);
    }
    if (outcome.error) {
      parts.push(`${formatCount(outcome.dropped)} could not be queued: ${REPO_BRANCH_OP_ERRORS[outcome.error] || "the relay refused them."}`);
    }
    if (waiting) {
      parts.push(
        `${formatCount(waiting)} ${outcome.sent ? "more " : ""}${plural(waiting, "waits", "wait")} on this page for room in the node's queue `
        + `(it holds ${formatCount(repoBranchQueueLimit(repo))} at a time) and ${plural(waiting, "is", "are")} sent as the node takes queued ones. `
        + "Keep this page open until then.",
      );
    }
    announceRepoBranchOp(parts.join(" "), Boolean(outcome.error));
  }

  const REPO_BRANCH_DELETE_NO_CONFIRM_QUERY = "(max-width: 767px), (pointer: coarse)";

  async function deleteRepoBranch(repo, branch, commit) {
    if (!repo || !branch) return;
    if (!window.matchMedia?.(REPO_BRANCH_DELETE_NO_CONFIRM_QUERY)?.matches && !window.confirm(
      `Delete branch "${branch}"? Its commits stay recoverable from the reflog `
      + `on the node that hosts ${repo.owner || "this repository"}, but the branch is removed for everyone.`,
    )) return;
    await queueRepoBranchOp(
      repo, { op: "delete", branch, expectedOid: commit },
      `Queued the deletion of ${branch} for this repository's node.`,
    );
  }

  async function updateRepoBranchFromDefault(repo, branch, commit) {
    if (!repo || !branch) return;
    await queueRepoBranchOp(
      repo,
      { op: "update", branch, expectedOid: commit, base: repoDefaultBranch(repo) },
      `Queued an update of ${branch} from ${repoDefaultBranch(repo)}.`,
    );
  }

  async function createRepoBranch(repo) {
    if (!repo) return;
    const defaultBranch = repoDefaultBranch(repo);
    const tip = repoBranchList(repo).find((branch) => branch.name === defaultBranch);
    if (!tip?.commit) {
      setRepoBranchesStatus(
        `The current tip of ${defaultBranch} is unknown, so a branch cannot be started from it yet.`,
        true,
      );
      return;
    }
    const name = String(window.prompt(`Create a branch from ${defaultBranch}:`) || "").trim();
    if (!name) return;
    await queueRepoBranchOp(
      repo, { op: "create", branch: name, startOid: tip.commit },
      `Queued the creation of ${name} from ${defaultBranch}.`,
    );
  }

  function repoBranchBatchStatus(queued, requested, noun, tail) {
    const already = requested - queued;
    return `Queued ${queued} ${noun}${queued === 1 ? "" : "s"} ${tail}`
      + (already > 0 ? ` ${already} ${already === 1 ? "was" : "were"} already queued.` : "");
  }

  async function deleteMergedRepoBranches(repo) {
    if (!repo) return;
    const defaultBranch = repoDefaultBranch(repo);
    const merged = repoMergedRepoBranches(repo, defaultBranch);
    if (!merged.length) return;
    if (!window.confirm(
      `Delete ${merged.length} branch${merged.length === 1 ? "" : "es"} already merged into ${defaultBranch}?\n\n`
      + merged.map((branch) => branch.name).join("\n"),
    )) return;
    await runRepoBranchBatch(
      repo,
      merged.map((branch) => ({
        op: "delete",
        branch: branch.name,
        expectedOid: branch.commit,
      })),
      (queued) => repoBranchBatchStatus(
        queued,
        merged.length,
        "merged branch deletion",
        "for this repository's node.",
      ),
      "Deleted",
    );
  }

  async function updateAllRepoBranchesFromDefault(repo) {
    if (!repo) return;
    const defaultBranch = repoDefaultBranch(repo);
    const behind = repoBranchesBehindDefault(repo, defaultBranch);
    if (!behind.length) return;
    if (!window.confirm(
      `Merge ${defaultBranch} into ${behind.length} branch${behind.length === 1 ? "" : "es"} behind it?\n\n`
      + behind.map((branch) => branch.name).join("\n"),
    )) return;
    await runRepoBranchBatch(
      repo,
      behind.map((branch) => ({
        op: "update",
        branch: branch.name,
        expectedOid: branch.commit,
        base: defaultBranch,
      })),
      (queued) => repoBranchBatchStatus(
        queued,
        behind.length,
        "branch update",
        `from ${defaultBranch}. Any branch that conflicts is reported by this repository's node instead of merged.`,
      ),
      `Updated from ${defaultBranch}:`,
    );
  }

  async function cancelRepoBranchOp(repo, id) {
    if (!repo || !id) return;
    const key = repoKey(repo);
    if (String(id).startsWith("held:")) {
      dropRepoBranchHeld(repo, repoBranchHeld(repo).filter((entry) => entry.id === id));
      announceRepoBranchOp("Removed the waiting branch operation.");
      repaintRepoBranchOps(repo);
      return;
    }
    try {
      const data = await orgApiRequest(
        "DELETE", `${repoApiBase(repo)}/branches/ops?id=${encodeURIComponent(id)}`);
      setRepoBranchOpsState(repo, data);
      state.repoBranchOpsWatch[key]?.waiting?.delete(Number(id));
      announceRepoBranchOp("Cancelled the queued branch operation.");
    } catch (_) {
      announceRepoBranchOp(
        "The queued operation could not be cancelled; the node may already have taken it.",
        true,
      );
    }
    repaintRepoBranchOps(repo);
  }

  async function loadRepoBranches(repo, { refresh = false } = {}) {
    const container = $("[data-repo-branches]");
    if (!container || !repo) return;
    const key = repoKey(repo);
    const cached = !refresh && state.repoBranches[key]?.loaded;
    if (!cached) {
      setRepoBranchesStatus("");
      container.innerHTML = repoIsLive(repo)
        ? `<div class="px-4 py-3 text-sm text-muted-foreground">${loadingHtml("Loading branches from the live mirror...")}</div>`
        : REPO_BRANCHES_UNAVAILABLE_HTML;
      try {
        await fetchRepoBranches(repo, { refresh });
      } catch (_) {
        container.innerHTML = REPO_BRANCHES_UNAVAILABLE_HTML;
        window.lucide?.createIcons();
        return;
      }
      // Until this listing landed the toolbar's "N Branches" was the
      // catalog's published estimate; repaint it from the real list.
      updateRepoBranchControls(repo, null);
    }
    renderRepoBranchesList(repo);
    // The management controls, the queue the node has not taken yet, and the
    // merge trials all describe the list that is now on screen, so they
    // follow it rather than holding it up. They run for a warm listing too:
    // opening the toolbar dropdown first warms it, and that must not leave
    // the page read-only.
    if (await loadRepoBranchManageAuthorization(repo)) {
      if (refresh || !state.repoBranchOps[key]) await loadRepoBranchOps(repo);
      renderRepoBranchesList(repo);
      if (repoBranchOps(repo).length) watchRepoBranchOps(repo);
    }
    await probeRepoBranchTrials(repo);
  }

  function renderRepoCollectionFilter(label, options, issueFilter = "", compact = false) {
    const current = issueFilter ? String(issueFilterState()[issueFilter] || "") : "";
    return `
      <label class="relative inline-flex ${compact ? "h-8 rounded-md px-2 text-[14px] font-normal hover:bg-secondary" : "h-9 rounded-md border border-border bg-background px-3 text-xs font-medium hover:bg-secondary"} shrink-0 items-center gap-1 text-muted-foreground hover:text-foreground transition-colors">
        ${compact && issueFilter === "sort" ? '<i data-lucide="arrow-down-wide-narrow" class="h-4 w-4"></i>' : ""}
        <span>${label}</span>
        <i data-lucide="chevron-down" class="${compact ? "h-4 w-4" : "h-3 w-3"}"></i>
        <select ${issueFilter ? `data-repo-issue-filter="${issueFilter}" ` : ""}aria-label="${label}" class="absolute inset-0 cursor-pointer opacity-0">
          ${options.map((option) => {
            const item = typeof option === "string"
              ? { value: option, label: option } : option;
            return `<option value="${escapeHtml(item.value)}"${item.value === current ? " selected" : ""}>${escapeHtml(item.label)}</option>`;
          }).join("")}
        </select>
      </label>`;
  }

  function issueContributionTipStorageKey(repo) {
    return `forkmesh.issue-contribution-tip.v1:${repoKey(repo)}`;
  }

  function issueContributionTipDismissed(repo) {
    try {
      return localStorage.getItem("forkmesh.issue-contribution-tip.v1:all") === "1"
        || localStorage.getItem(issueContributionTipStorageKey(repo)) === "1";
    } catch (_) {
      return false;
    }
  }

  function renderIssueContributionTip(repo) {
    if (issueContributionTipDismissed(repo)) return "";
    const repository = `${repo.owner || "owner"}/${repo.name || "repository"}`;
    return `
      <aside data-repo-issue-contribution-tip class="fm-flash relative mb-4 flex-wrap">
        <i data-lucide="heart-handshake"></i>
        <div class="fm-flash-body">
          <span class="fm-flash-title">Want to contribute to ${escapeHtml(repository)}?</span>
          <p>If you found a bug or have an idea, read the <a href="${escapeHtml(repoLinkUrl(repo, "blob", "CONTRIBUTING.md"))}" class="font-medium text-accent hover:underline">contributing guide</a> before opening an issue.</p>
        </div>
        <details class="fm-flash-actions relative text-left">
          <summary class="fm-btn fm-btn-sm inline-flex list-none">Dismiss<i data-lucide="chevron-down"></i></summary>
          <div class="fm-popover fm-menu absolute right-0 top-full z-20 mt-1 w-56">
            <button type="button" data-repo-issue-contribution-dismiss="repository" class="fm-menu-item">Dismiss for this repository</button>
            <button type="button" data-repo-issue-contribution-dismiss="all" class="fm-menu-item">Dismiss for all repositories</button>
          </div>
        </details>
      </aside>`;
  }

  function renderRepoCollectionPanel(kind, repo, openCount, closedCount) {
    const isPulls = kind === "pulls";
    if (!isPulls) {
      // GitHub's Issues page: saved views down the left, the search box
      // carrying the filters as qualifiers, and one green action. Everything
      // marked data-repo-collection-chrome steps aside while an issue is open.
      return `
        <section data-dashboard-repo-tab-panel="issues" class="hidden">
          <div data-repo-issues-layout data-repo-collection-layout="issues" data-views-collapsed="${issueViewsCollapsed() ? "true" : "false"}" class="mt-2 grid min-w-0 gap-6">
            <nav data-repo-issue-views data-repo-collection-views data-repo-collection-chrome aria-label="Issue views" class="hidden min-w-0">${renderIssueViewsNav()}</nav>
            <div data-repo-collection-main class="min-w-0">
              <div data-repo-collection-chrome>
                <div class="mb-4 flex min-w-0 flex-wrap items-center justify-between gap-3">
                  <div class="flex min-w-0 items-center gap-2">
                    <button type="button" data-repo-issue-views-toggle aria-label="Show issue views" class="hidden h-8 w-8 items-center justify-center rounded-md border border-border text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="panel-left-open" class="h-4 w-4"></i></button>
                    <h2 data-repo-issue-view-title class="text-[20px] font-semibold leading-8 text-foreground">All issues</h2>
                    <select data-repo-issue-view-select aria-label="Issue view" class="h-8 rounded-md border border-border bg-background px-2 text-[14px] text-foreground lg:hidden">
                      ${ISSUE_VIEWS.map((view) => `<option value="${view.id}">${escapeHtml(view.label)}</option>`).join("")}
                    </select>
                  </div>
                  <div class="flex min-w-0 flex-wrap items-center justify-end gap-2">
                    <button type="button" data-repo-issues-refresh aria-busy="false" title="Refresh from the live mirror" class="inline-flex h-8 w-8 items-center justify-center rounded-md border border-border bg-secondary text-muted-foreground hover:text-foreground disabled:cursor-wait disabled:opacity-60"><i data-lucide="refresh-cw" data-repo-issues-refresh-icon class="h-4 w-4"></i><span data-repo-issues-refresh-label class="sr-only">Refresh</span></button>
                    <div class="inline-flex">
                      <button type="button" data-repo-issue-new class="inline-flex h-8 items-center rounded-l-md bg-cta px-3 text-[14px] font-medium text-cta-foreground hover:bg-cta/90">New issue</button>
                      <details data-repo-issue-new-menu class="relative">
                        <summary aria-label="More ways to add issues" class="inline-flex h-8 cursor-pointer list-none items-center rounded-r-md border-l border-black/25 bg-cta px-2 text-cta-foreground hover:bg-cta/90"><i data-lucide="chevron-down" class="h-4 w-4"></i></summary>
                        <div class="fm-popover fm-menu absolute right-0 top-full z-30 mt-1 w-56">
                          <button type="button" data-repo-issue-import class="fm-menu-item"><i data-lucide="upload"></i>Import from CSV</button>
                          <button type="button" data-repo-issue-template class="fm-menu-item"><i data-lucide="file-down"></i>Download CSV template</button>
                        </div>
                      </details>
                    </div>
                  </div>
                </div>
                ${renderIssueContributionTip(repo)}
                <div data-repo-collection-toolbar="issues" class="mb-4">
                  <label class="flex h-9 min-w-0 items-stretch overflow-hidden rounded-md border border-border bg-background focus-within:border-[var(--dashboard-link)] focus-within:ring-1 focus-within:ring-[var(--dashboard-link)]">
                    <span class="relative min-w-0 flex-1 overflow-hidden">
                      <span data-repo-issue-query-highlight aria-hidden="true" class="pointer-events-none absolute inset-y-0 left-0 flex items-center whitespace-pre px-3 text-[14px] text-foreground"></span>
                      <input data-repo-filter-query="issues" type="search" autocomplete="off" spellcheck="false" aria-label="Search issues" value="${escapeHtml(`${serializeIssueQuery()} `)}" placeholder="Search issues by title, body, author, label, or #number" class="relative h-full w-full bg-transparent px-3 text-[14px] text-transparent caret-foreground outline-none placeholder:text-muted-foreground selection:bg-[color-mix(in_srgb,var(--dashboard-link)_35%,transparent)]" />
                    </span>
                    <span class="inline-flex w-10 shrink-0 items-center justify-center border-l border-border bg-[rgb(var(--dashboard-subtle-rgb))] text-muted-foreground"><i data-lucide="search" class="h-4 w-4"></i></span>
                  </label>
                </div>
              </div>
              <div data-repo-collection-box class="overflow-clip rounded-md border border-border bg-background">
                <div data-repo-issues></div>
              </div>
            </div>
          </div>
        </section>`;
    }
    const repository = `${repo.owner || "owner"}/${repo.name || "repository"}`;
    return `
      <section data-dashboard-repo-tab-panel="pulls" class="hidden">
        <div data-repo-collection-layout="pulls" data-views-collapsed="${issueViewsCollapsed() ? "true" : "false"}" class="mt-2 grid min-w-0 gap-6">
          <nav data-repo-pull-views data-repo-collection-views data-repo-collection-chrome aria-label="Pull request views" class="hidden min-w-0">${renderPullViewsNav()}</nav>
          <div data-repo-collection-main class="min-w-0">
            <div data-repo-collection-chrome>
              <div class="mb-4 flex min-w-0 flex-wrap items-center justify-between gap-3">
                <div class="flex min-w-0 items-center gap-2">
                  <button type="button" data-repo-issue-views-toggle aria-label="Show pull request views" class="hidden h-8 w-8 items-center justify-center rounded-md border border-border text-muted-foreground hover:bg-secondary hover:text-foreground"><i data-lucide="panel-left-open" class="h-4 w-4"></i></button>
                  <h2 data-repo-pull-view-title class="text-[20px] font-semibold leading-8 text-foreground">All pull requests</h2>
                  <select data-repo-pull-view-select aria-label="Pull request view" class="h-8 rounded-md border border-border bg-background px-2 text-[14px] text-foreground lg:hidden">
                    ${PULL_VIEWS.map((view) => `<option value="${view.id}">${escapeHtml(view.label)}</option>`).join("")}
                  </select>
                </div>
                <button type="button" data-repo-pull-new class="${repoReadOnlyMirror(repo) ? "hidden " : ""}inline-flex h-8 items-center rounded-md bg-cta px-3 text-[14px] font-medium text-cta-foreground hover:bg-cta/90">New pull request</button>
              </div>
              ${pullContributionTipDismissed(repo) ? "" : `
              <aside data-repo-pull-contribution-tip class="fm-flash relative mb-4 flex-wrap">
                <i data-lucide="heart-handshake"></i>
                <div class="fm-flash-body">
                  <span class="fm-flash-title">First time contributing to ${escapeHtml(repository)}?</span>
                  <p>Review this repository's <a href="${escapeHtml(repoLinkUrl(repo, "blob", "CONTRIBUTING.md"))}" class="font-medium text-accent hover:underline">contribution notes</a> before opening a pull request.</p>
                </div>
                <div class="fm-flash-actions"><button type="button" data-repo-pull-contribution-dismiss class="fm-btn fm-btn-sm">Dismiss</button></div>
              </aside>`}
              <div data-repo-collection-toolbar="pulls" class="mb-4">
                <label class="flex h-9 min-w-0 items-stretch overflow-hidden rounded-md border border-border bg-background focus-within:border-[var(--dashboard-link)] focus-within:ring-1 focus-within:ring-[var(--dashboard-link)]">
                  <span class="relative min-w-0 flex-1 overflow-hidden">
                    <span data-repo-issue-query-highlight aria-hidden="true" class="pointer-events-none absolute inset-y-0 left-0 flex items-center whitespace-pre px-3 text-[14px] text-foreground"></span>
                    <input data-repo-filter-query="pulls" type="search" autocomplete="off" spellcheck="false" aria-label="Search pull requests" value="${escapeHtml(`${serializePullQuery()} `)}" placeholder="Search pull requests by title, body, author, or #number" class="relative h-full w-full bg-transparent px-3 text-[14px] text-transparent caret-foreground outline-none placeholder:text-muted-foreground selection:bg-[color-mix(in_srgb,var(--dashboard-link)_35%,transparent)]" />
                  </span>
                  <span class="inline-flex w-10 shrink-0 items-center justify-center border-l border-border bg-[rgb(var(--dashboard-subtle-rgb))] text-muted-foreground"><i data-lucide="search" class="h-4 w-4"></i></span>
                </label>
              </div>
            </div>
            <!-- overflow-clip, not overflow-hidden: hidden would make this card
                 the nearest scroll container, and the pull review pane's sticky
                 file explorer and diff headers would never leave the top of it. -->
            <div data-repo-collection-box class="overflow-clip rounded-md border border-border bg-background">
              <div data-repo-pulls></div>
            </div>
          </div>
        </div>
      </section>`;
  }

  function pullContributionTipDismissed(repo) {
    try {
      return localStorage.getItem(`forkmesh.pull-contribution-tip.v1:${repoKey(repo)}`) === "1";
    } catch (_) {
      return false;
    }
  }
