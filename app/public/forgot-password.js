(() => {
  const $ = (sel) => document.querySelector(sel);
  const hint = $("#forgot-hint");
  const btn = $("#forgot-btn");
  const input = $("#identifier");

  function setHint(text, cls) {
    hint.textContent = text || "";
    hint.className = "hint" + (cls ? " " + cls : "");
  }

  // Cloudflare Turnstile (managed). The static page learns the public site
  // key from /api/turnstile, then renders once api.js has loaded. The token
  // is single-use: every submission resets the widget.
  const challengeBox = $("#forgot-turnstile");
  let challengeToken = "";
  let challengeWidget = null;
  let siteKey = null;
  let apiReady = Boolean(window.turnstile);

  function renderChallenge() {
    if (challengeWidget !== null || !siteKey || !apiReady || !window.turnstile) return;
    try {
      challengeWidget = window.turnstile.render(challengeBox, {
        sitekey: siteKey,
        action: "password-reset",
        theme: "auto",
        "response-field": false,
        callback: (token) => { challengeToken = String(token || ""); },
        "expired-callback": () => { challengeToken = ""; },
        "error-callback": () => {
          challengeToken = "";
          setHint("The security check could not load. Reload the page and try again.", "bad");
          return true;
        },
      });
    } catch (_) {}
  }

  function resetChallenge() {
    challengeToken = "";
    try {
      if (challengeWidget !== null && window.turnstile) window.turnstile.reset(challengeWidget);
    } catch (_) {}
  }

  window.forkmeshForgotChallengeReady = () => {
    apiReady = true;
    renderChallenge();
  };
  fetch("/api/turnstile", { headers: { accept: "application/json" } })
    .then((res) => res.json())
    .then((body) => {
      siteKey = body && body.siteKey ? String(body.siteKey) : "";
      renderChallenge();
    })
    .catch(() => { siteKey = ""; });

  // Deliberately generic: whether or not the identifier matches an account, we
  // show the same confirmation so this page can't be used to probe which emails
  // are registered (the API returns {ok:true} either way).
  const SENT_MESSAGE =
    "If that account exists, a reset link is on its way. Check your email.";

  async function submit() {
    const identifier = input.value.trim();
    if (!identifier) {
      setHint("Enter the email for your account.", "bad");
      return;
    }
    if (!challengeToken) {
      setHint("Complete the security check above first.", "bad");
      return;
    }
    const token = challengeToken;
    resetChallenge();
    btn.disabled = true;
    btn.textContent = "Sending…";
    let res;
    try {
      res = await fetch("/api/accounts/forgot-password", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ identifier, turnstileToken: token }),
      });
    } catch (_) {
      btn.disabled = false;
      btn.textContent = "Send reset link";
      setHint("Network error - please try again.", "bad");
      return;
    }
    btn.disabled = false;
    btn.textContent = "Send reset link";
    if (res.ok) {
      setHint(SENT_MESSAGE, "good");
      input.value = "";
      return;
    }
    let error = "";
    try { error = (await res.json()).error || ""; } catch (_) {}
    setHint(
      error === "turnstile_unavailable"
        ? "Password reset is temporarily unavailable (security check offline). Try again in a few minutes."
        : error.startsWith("turnstile_")
          ? "The security check did not pass. Solve it again and resend."
          : "Could not send a reset link. Please try again.",
      "bad");
  }

  btn.addEventListener("click", submit);
  input.addEventListener("keydown", (e) => { if (e.key === "Enter") submit(); });
})();
