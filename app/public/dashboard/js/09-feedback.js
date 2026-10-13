  let feedbackReturnFocus = null;
  let feedbackCloseTimer = 0;

  function setFeedbackStatus(text, tone = "") {
    const status = $("[data-feedback-status]");
    if (!status) return;
    status.textContent = text || "";
    status.className = `min-h-4 text-xs ${tone === "bad" ? "text-red-500" : tone === "good" ? "text-emerald-500" : "text-muted-foreground"}`;
  }

  function setFeedbackModalOpen(open) {
    const modal = $("[data-feedback-modal]");
    if (!modal) return;
    window.clearTimeout(feedbackCloseTimer);
    modal.classList.toggle("hidden", !open);
    modal.classList.toggle("flex", open);
    if (open) {
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
    if (message.length < 2) {
      setFeedbackStatus("Write a few words first.", "bad");
      field?.focus();
      return;
    }
    if (submit) submit.disabled = true;
    setFeedbackStatus("Sending...");
    try {
      const response = await fetch("/api/feedback", {
        method: "POST",
        cache: "no-store",
        credentials: "same-origin",
        headers: { accept: "application/json", "content-type": "application/json" },
        body: JSON.stringify({ source: "web", vote: "feedback", message }),
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      form.reset();
      setFeedbackStatus("Thank you. Your feedback reached the ForkMesh team.", "good");
      if (submit) submit.textContent = "Sent";
      feedbackCloseTimer = window.setTimeout(() => setFeedbackModalOpen(false), 1400);
    } catch (_) {
      if (submit) submit.disabled = false;
      setFeedbackStatus("Feedback could not be sent. Please try again.", "bad");
    }
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
