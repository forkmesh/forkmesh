  let feedbackReturnFocus = null;
  let feedbackCloseTimer = 0;
  let feedbackPlugin = null;
  const FEEDBACK_GENERAL_COPY = Object.freeze({
    title: "Send feedback",
    desc: "Tell us what worked, what was confusing, or the one thing you want us to improve next. Only the ForkMesh team reads it; nothing is published.",
    placeholder: "What should we know?",
  });

  function feedbackCopy(plugin) {
    if (!plugin) return FEEDBACK_GENERAL_COPY;
    return {
      title: `Feedback on ${plugin.name}`,
      desc: `Report a bug or suggest an improvement for ${plugin.name}. It is filed as a public issue tagged with this plugin; your contact details are never published.`,
      placeholder: "What happened, or what would make it better?",
    };
  }

  function setFeedbackStatus(text, tone = "") {
    const status = $("[data-feedback-status]");
    if (!status) return;
    status.textContent = text || "";
    status.className = `min-h-4 text-xs ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
  }

  function setFeedbackModalOpen(open, plugin = null) {
    const modal = $("[data-feedback-modal]");
    if (!modal) return;
    window.clearTimeout(feedbackCloseTimer);
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) {
      feedbackPlugin = plugin && plugin.id ? { id: String(plugin.id), name: String(plugin.name || plugin.id) } : null;
      const copy = feedbackCopy(feedbackPlugin);
      const title = $("#feedbackModalTitle");
      if (title) title.textContent = copy.title;
      const desc = modal.querySelector(".fm-dialog-desc");
      if (desc) desc.textContent = copy.desc;
      const field = $("[data-feedback-message]");
      if (field) field.placeholder = copy.placeholder;
      feedbackReturnFocus = document.activeElement;
      const submit = $("[data-feedback-submit]");
      if (submit) {
        submit.disabled = false;
        submit.textContent = "Send feedback";
      }
      setFeedbackStatus("");
      window.setTimeout(() => $("[data-feedback-message]")?.focus(), 0);
      return;
    }
    if (feedbackReturnFocus && document.body.contains(feedbackReturnFocus)) {
      feedbackReturnFocus.focus();
    }
    feedbackReturnFocus = null;
  }

  async function submitDashboardFeedback(form) {
    const field = form.querySelector("[data-feedback-message]");
    const submit = form.querySelector("[data-feedback-submit]");
    const message = String(field?.value || "").trim();
    if (message.length < (feedbackPlugin ? 4 : 2)) {
      setFeedbackStatus("Write a few words first.", "bad");
      field?.focus();
      return;
    }
    if (submit) submit.disabled = true;
    setFeedbackStatus("Sending...");
    try {
      const plugin = feedbackPlugin;
      const headers = { accept: "application/json", "content-type": "application/json" };
      const token = state.session?.sessionToken || "";
      if (plugin && token && token !== "cookie") headers.authorization = `Bearer ${token}`;
      const response = await fetch(plugin ? "/api/bug-reports" : "/api/feedback", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers,
        body: JSON.stringify(plugin ? pluginFeedbackPayload(plugin, message) : { source: "web", vote: "feedback", message }),
      });
      if (response.status === 429) {
        if (submit) submit.disabled = false;
        setFeedbackStatus("Too many reports from here in the last hour. Please try again later.", "bad");
        return;
      }
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      form.reset();
      setFeedbackStatus(plugin ? `Thank you. Your report on ${plugin.name} was filed.` : "Thank you. Your feedback reached the ForkMesh team.", "good");
      if (submit) submit.textContent = "Sent";
      feedbackCloseTimer = window.setTimeout(() => setFeedbackModalOpen(false), 1400);
    } catch (_) {
      if (submit) submit.disabled = false;
      setFeedbackStatus("Feedback could not be sent. Please try again.", "bad");
    }
  }

  function pluginFeedbackPayload(plugin, message) {
    return {
      message,
      package: plugin.id,
      screen: currentPage(),
      source: "web",
      platform: "web",
      severity: "normal",
      device: String(navigator.userAgent || "").slice(0, 120),
    };
  }

  function openFeedbackFromQuery() {
    let params;
    try {
      params = new URLSearchParams(location.search);
    } catch (_) {
      return;
    }
    if (params.get("feedback") !== "1") return;
    params.delete("feedback");
    const query = params.toString();
    window.history.replaceState(window.history.state, "", `${location.pathname}${query ? `?${query}` : ""}${location.hash}`);
    setFeedbackModalOpen(true);
  }

  function initDashboardFeedback() {
    if (!$("[data-feedback-modal]")) return;
    document.addEventListener("click", (event) => {
      if (event.target.closest("[data-feedback-open]")) {
        event.preventDefault();
        setAccountMenuOpen(false);
        setFeedbackModalOpen(true);
        return;
      }
      if (event.target.closest("[data-feedback-close]")) {
        setFeedbackModalOpen(false);
      }
    });
    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      if ($("[data-feedback-modal]")?.classList.contains("hidden")) return;
      setFeedbackModalOpen(false);
    });
    $("[data-feedback-form]")?.addEventListener("submit", (event) => {
      event.preventDefault();
      void submitDashboardFeedback(event.currentTarget);
    });
    openFeedbackFromQuery();
  }
