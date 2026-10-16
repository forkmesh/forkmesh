(() => {
  // Must match valid_node_name in app/src/entry.py and the desktop
  // client: a single DNS-like label, lowercase, hyphens allowed, no underscores.
  const NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
  // Keep these practical mailbox/domain rules aligned with valid_account_email.
  const EMAIL_LOCAL_RE = /^[a-z0-9.!#$%&'*+/=?^_`{|}~-]+$/i;
  const EMAIL_DOMAIN_RE = /^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/i;

  const $ = (sel) => document.querySelector(sel);
  const isLive = location.protocol !== "file:";

  // Referral attribution comes from a server-issued, signed, one-time click
  // claim. Remember the pair so a short detour before signup does not lose it;
  // a browser-supplied referrer name without its claim earns no credit.
  function referralCode() {
    let ref = "";
    try {
      ref = (new URLSearchParams(location.search).get("ref") || "").toLowerCase();
      const claim = new URLSearchParams(location.search).get("rid") || "";
      if (ref && NAME_RE.test(ref) && claim) {
        localStorage.setItem("forkmesh.referral", ref);
        localStorage.setItem("forkmesh.referral.claim", claim);
      } else {
        ref = localStorage.getItem("forkmesh.referral") || "";
      }
    } catch (_) {}
    return NAME_RE.test(ref) ? ref : "";
  }

  function referralToken() {
    try {
      referralCode();
      return localStorage.getItem("forkmesh.referral.claim") || "";
    } catch (_) {
      return "";
    }
  }
  let nameOk = false;
  let availTimer = null;

  const form = $("#signup-form");
  const signupReferralCodeField = $("#signup-referral-code-field");
  const signupReferralCodeInput = $("#signup-referral-code");
  const nameInput = $("#node-name");
  const emailInput = $("#acct-email");
  const passwordInput = $("#acct-pass");
  const termsAgree = $("#terms-agree");
  const updatesAgree = $("#updates-agree");
  const createButton = $("#signup-create");
  const nameHint = $("#name-hint");
  const emailHint = $("#email-hint");
  const signupHint = $("#signup-hint");

  const challengeBox = $("#signup-turnstile");
  const CHALLENGE_FRESH_MS = 240 * 1000;
  const CHALLENGE_REFRESH_WAIT_MS = 30 * 1000;
  const CHALLENGE_RETRY_ERRORS = 3;
  let challengeToken = "";
  let challengeAt = 0;
  let challengeWidget = null;
  let challengeWaiters = [];
  let challengeErrors = 0;
  let challengeErrorCode = "";
  let challengeErrorReported = false;

  function reportProblem(problem, detail) {
    if (!isLive) return;
    try {
      fetch("/api/accounts/signup/problem", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        credentials: "same-origin",
        keepalive: true,
        body: JSON.stringify({
          problem,
          detail: String(detail || "").slice(0, 40),
          nodeName: nameInput.value.trim().toLowerCase(),
        }),
      }).catch(() => {});
    } catch (_) {}
  }

  function challengeDone(token) {
    challengeToken = String(token || "");
    challengeAt = challengeToken ? Date.now() : 0;
    if (challengeToken && /security check/i.test(signupHint.textContent)) {
      setSignupHint("", "");
    }
    const waiters = challengeWaiters;
    challengeWaiters = [];
    waiters.forEach((resolve) => resolve(challengeToken));
  }

  function renderChallenge() {
    if (!challengeBox || challengeWidget !== null || !window.turnstile) return;
    try {
      challengeWidget = window.turnstile.render(challengeBox, {
        sitekey: challengeBox.dataset.sitekey || "",
        action: challengeBox.dataset.action || "signup",
        cData: challengeBox.dataset.cdata || "",
        theme: "auto",
        "response-field": false,
        callback: challengeDone,
        "expired-callback": () => { challengeToken = ""; },
        "error-callback": (code) => {
          challengeToken = "";
          challengeErrors += 1;
          challengeErrorCode = String(code || "");
          const retrying = /^[36]\d{5}$/.test(challengeErrorCode)
            && challengeErrors < CHALLENGE_RETRY_ERRORS;
          if (retrying) {
            setSignupHint("The security check hit a snag and is retrying…", "");
            return true;
          }
          setSignupHint("The security check could not load. Reload the page and try again.", "bad");
          if (!challengeErrorReported) {
            challengeErrorReported = true;
            reportProblem("challenge_error", challengeErrorCode + ":" + challengeErrors);
          }
          return true;
        },
      });
    } catch (error) {
      reportProblem("challenge_render_failed", error && error.name);
    }
  }

  function widgetToken() {
    if (challengeToken) return challengeToken;
    try {
      const turnstile = window.turnstile;
      const widget = challengeWidget !== null ? challengeWidget : challengeBox;
      if (turnstile && widget && !(turnstile.isExpired && turnstile.isExpired(widget))) {
        challengeToken = String(turnstile.getResponse(widget) || "");
        challengeAt = challengeToken ? Date.now() : 0;
      }
    } catch (_) {}
    return challengeToken;
  }

  function resetChallenge() {
    challengeToken = "";
    challengeAt = 0;
    try {
      if (challengeWidget !== null && window.turnstile) window.turnstile.reset(challengeWidget);
    } catch (_) {}
  }

  function challengeStale() {
    if (!challengeToken) return false;
    if (Date.now() - challengeAt > CHALLENGE_FRESH_MS) return true;
    try {
      const turnstile = window.turnstile;
      return Boolean(challengeWidget !== null && turnstile && turnstile.isExpired
        && turnstile.isExpired(challengeWidget));
    } catch (_) {
      return false;
    }
  }

  function waitForChallenge() {
    if (!challengeBox || challengeWidget === null) return Promise.resolve("");
    if (challengeToken) return Promise.resolve(challengeToken);
    return new Promise((resolve) => {
      challengeWaiters.push(resolve);
      setTimeout(() => resolve(""), CHALLENGE_REFRESH_WAIT_MS);
    });
  }

  function refreshChallenge() {
    resetChallenge();
    return waitForChallenge();
  }

  const SIGNUP_INTENT_RENEW_MS = 12 * 60 * 1000;
  const SIGNUP_PAGE_RENEW_ERRORS = ["signup_page_required", "turnstile_stale_page"];
  let signupIntentAt = Date.now();
  let signupIntentRenewing = null;

  function signupIntentStale() {
    return isLive && Date.now() - signupIntentAt > SIGNUP_INTENT_RENEW_MS;
  }

  async function requestSignupIntent() {
    let result = null;
    try {
      result = await api("/api/accounts/signup/intent", { method: "POST", body: "{}" });
    } catch (_) {
      return false;
    }
    const field = String((result && result.body && result.body.emailField) || "");
    if (!result.ok || !/^contact_[0-9a-f]{18}$/.test(field)) return false;
    signupIntentAt = Date.now();
    emailInput.name = field;
    if (challengeBox) {
      challengeBox.dataset.cdata = String(result.body.challengeCdata || "");
      challengeToken = "";
      challengeAt = 0;
      try {
        if (challengeWidget !== null && window.turnstile) window.turnstile.remove(challengeWidget);
      } catch (_) {}
      challengeWidget = null;
      renderChallenge();
    }
    return true;
  }

  function renewSignupIntent() {
    if (!signupIntentRenewing) {
      signupIntentRenewing = requestSignupIntent().finally(() => {
        signupIntentRenewing = null;
      });
    }
    return signupIntentRenewing;
  }

  async function renewSignupPage() {
    createButton.disabled = true;
    createButton.textContent = "Checking…";
    setSignupHint("Refreshing the signup page…", "");
    const renewed = await renewSignupIntent();
    const token = renewed ? await waitForChallenge() : "";
    createButton.disabled = false;
    createButton.textContent = "Create account";
    if (!renewed) {
      setSignupHint("Couldn’t refresh the signup page. Check your connection and try again.", "bad");
      reportProblem("signup_page_renew_failed", "");
      return false;
    }
    if (challengeBox && challengeWidget !== null && !token) {
      setSignupHint("Complete the security check above, then create your account.", "bad");
      return false;
    }
    setSignupHint("", "");
    return true;
  }

  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState !== "visible") return;
    if (signupIntentStale()) {
      renewSignupIntent();
    } else if (challengeWidget !== null && challengeStale()) {
      resetChallenge();
    }
  });

  window.forkmeshSignupChallengeReady = renderChallenge;
  renderChallenge();

  function setNameHint(text, cls) {
    // Only surface this hint for an actual error; success/neutral states stay quiet.
    nameHint.textContent = cls === "bad" ? text : "";
    nameHint.className = "hint" + (cls ? " " + cls : "");
  }

  function setSignupHint(text, cls) {
    signupHint.textContent = text || "";
    signupHint.className = "hint" + (cls ? " " + cls : "");
  }

  function validEmail(value) {
    if (!value || value.length > 254) return false;
    const at = value.indexOf("@");
    if (at <= 0 || at !== value.lastIndexOf("@")) return false;
    const local = value.slice(0, at);
    const domain = value.slice(at + 1);
    return local.length <= 64
      && !local.startsWith(".")
      && !local.endsWith(".")
      && !local.includes("..")
      && EMAIL_LOCAL_RE.test(local)
      && domain.length <= 253
      && EMAIL_DOMAIN_RE.test(domain);
  }

  function validateEmail(showError) {
    const value = emailInput.value.trim();
    const ok = validEmail(value);
    const displayError = Boolean(value) && !ok && showError;
    emailInput.setAttribute("aria-invalid", displayError ? "true" : "false");
    emailHint.textContent = displayError
      ? "Enter a valid email address with a domain ending like .com or .org."
      : "";
    emailHint.className = "hint" + (displayError ? " bad" : "");
    updateCreateState();
    return ok;
  }

  function updateCreateState() {
    const email = emailInput.value.trim();
    const password = passwordInput.value;
    const referralCodeOk = !signupReferralCodeField
      || signupReferralCodeField.hidden
      || Boolean(signupReferralCodeInput.value.trim());
    createButton.disabled = !(nameOk && validEmail(email) && password.length >= 8
      && termsAgree.checked && referralCodeOk);
  }

  async function api(path, options) {
    const res = await fetch(path, {
      headers: { "content-type": "application/json", accept: "application/json" },
      credentials: "same-origin",
      ...options,
    });
    let body = {};
    try { body = await res.json(); } catch (_) { /* ignore */ }
    return { ok: res.ok, status: res.status, body };
  }

  function storeSession(body) {
    try {
      localStorage.setItem("forkmesh.session", JSON.stringify({
        nodeName: body.nodeName,
        email: body.email,
        status: body.status,
        pubkey: body.pubkey,
        emailVerified: Boolean(body.emailVerified),
        isAdmin: Boolean(body.isAdmin),
        adminUrl: body.adminUrl || "",
        hasPayoutAddress: Boolean(body.hasPayoutAddress),
        sessionToken: (
          location.protocol === "https:" &&
          body.sessionToken &&
          window.ForkMeshAPI?.isSameSite !== false
            ? "cookie"
            : body.sessionToken || ""
        ),
        kind: body.kind || "user",
        owner: body.owner || "",
        nodes: Array.isArray(body.nodes) ? body.nodes : [],
        at: Date.now(),
      }));
      document.cookie = "forkmesh_session=1; Path=/; Max-Age=2592000; SameSite=Lax"
        + (location.protocol === "https:" ? "; Secure" : "");
    } catch (_) {}
  }

  function validateName() {
    const value = nameInput.value.trim().toLowerCase();
    nameInput.value = value;
    nameOk = false;
    updateCreateState();
    if (!value) {
      setNameHint("Lowercase letters, numbers and hyphens. Start with a letter, end with a letter or number. Your username is public.", "");
      return;
    }
    if (!NAME_RE.test(value)) {
      setNameHint("Use lowercase letters, numbers and hyphens - start with a letter, end with a letter or number, no spaces or underscores.", "bad");
      return;
    }
    setNameHint("Checking availability…", "");
    clearTimeout(availTimer);
    availTimer = setTimeout(() => checkAvailability(value), 300);
  }

  async function checkAvailability(value) {
    if (!isLive) {
      setNameHint("Looks good (preview).", "good");
      nameOk = true;
      updateCreateState();
      return;
    }
    try {
      const { body } = await api("/api/accounts/" + encodeURIComponent(value));
      if (body.exists && body.available === false) {
        setNameHint("That username is already taken - try another.", "bad");
        nameOk = false;
      } else {
        setNameHint("“" + value + "” is available.", "good");
        nameOk = true;
      }
    } catch (_) {
      setNameHint("Couldn’t check availability - you can still create the account.", "");
      nameOk = true;
    }
    updateCreateState();
  }

  async function createAccount(event) {
    event.preventDefault();
    const nodeName = nameInput.value.trim().toLowerCase();
    const email = emailInput.value.trim();
    const password = passwordInput.value;
    setSignupHint("", "");
    if (!NAME_RE.test(nodeName)) {
      setNameHint("Choose a valid username first.", "bad");
      return;
    }
    if (!validateEmail(true)) {
      return;
    }
    if (password.length < 8) {
      setSignupHint("Password must be at least 8 characters.", "bad");
      return;
    }
    if (!termsAgree.checked) {
      setSignupHint("Accept the Terms and Privacy policy to continue.", "bad");
      return;
    }
    if (signupIntentStale() && !(await renewSignupPage())) {
      return;
    }
    if (challengeBox && challengeWidget !== null && challengeStale()) {
      createButton.disabled = true;
      createButton.textContent = "Checking…";
      setSignupHint("Refreshing the security check…", "");
      const fresh = await refreshChallenge();
      createButton.disabled = false;
      createButton.textContent = "Create account";
      if (!fresh) {
        setSignupHint("Complete the security check above, then create your account.", "bad");
        reportProblem("challenge_stale", "");
        return;
      }
      setSignupHint("", "");
    }
    if (challengeBox && !widgetToken()) {
      if (!window.turnstile) {
        setSignupHint("The security check could not load. Reload the page and try again.", "bad");
        reportProblem("challenge_unavailable", "");
      } else if (challengeWidget === null) {
        renderChallenge();
        setSignupHint("Complete the security check above, then create your account.", "bad");
        reportProblem("challenge_not_rendered", "");
      } else {
        setSignupHint("Complete the security check above, then create your account.", "bad");
        reportProblem("challenge_token_missing", challengeErrorCode);
      }
      return;
    }

    const postSignup = () => api("/api/accounts/signup", {
      method: "POST",
      body: JSON.stringify({
        nodeName,
        email: "",
        [emailInput.name]: email,
        password,
        signupReferralCode: signupReferralCodeInput
          ? signupReferralCodeInput.value.trim()
          : "",
        ref: referralCode(),
        referralToken: referralToken(),
        turnstileToken: challengeToken,
        marketingEmail: Boolean(updatesAgree && updatesAgree.checked),
      }),
    });
    createButton.disabled = true;
    createButton.textContent = "Creating…";
    let result = null;
    let created = false;
    try {
      result = await postSignup();
      if (!result.ok && isLive && SIGNUP_PAGE_RENEW_ERRORS.includes(result.body.error)) {
        if (!(await renewSignupPage())) return;
        createButton.disabled = true;
        createButton.textContent = "Creating…";
        result = await postSignup();
      }
      created = Boolean(result.ok);
    } catch (error) {
      resetChallenge();
      setSignupHint("Network error - please try again.", "bad");
      reportProblem("network_error", error && error.name);
      return;
    } finally {
      if (!created) {
        createButton.disabled = false;
        createButton.textContent = "Create account";
      }
    }
    const { ok, body } = result;
    if (!ok) {
      resetChallenge();
      if (!body.error) reportProblem("unexpected_response", "http_" + result.status);
      setSignupHint(
        body.error === "node_name_taken" ? "That username was just taken - try another."
          : body.error === "inappropriate_node_name" ? "That username was flagged by our automated filter. Try a different one, or email founders@forkmesh.com if that looks wrong."
          : body.error === "email_taken" ? "That email is already registered."
          : body.error === "account_pending_verification" ? "This account is pending verification. Contact support if you believe this is a mistake."
          : body.error === "valid_email_required" ? "Enter a valid email address with a plausible domain."
          : body.error === "password_too_short" ? "Password must be at least 8 characters."
          : body.error === "signup_page_required" ? "Your signup page expired. Reload /signup and try again."
          : body.error === "invalid_signup_referral_code" ? "That referral code is invalid or was already used. Ask an administrator for the latest code."
          : body.error === "turnstile_required" ? "Complete the security check above, then try again."
          : body.error === "turnstile_expired" ? "The security check expired. Solve it again and resubmit."
          : body.error === "turnstile_failed" ? "The security check could not be verified. Try it again."
          : body.error === "turnstile_stale_page" ? "This signup page was replaced by a newer one (another tab or a reload). Reload this page and try again."
          : body.error === "turnstile_unavailable" ? "Signup is temporarily unavailable (security check offline). Please try again in a few minutes."
          : "Could not create the account. Please try again.",
        "bad");
      return;
    }
    try {
      localStorage.removeItem("forkmesh.referral");
      localStorage.removeItem("forkmesh.referral.claim");
    } catch (_) {}
    storeSession(body);
    window.ForkMeshAnalytics?.recordProductAction?.("signup-completed");
    showVerifyView(nodeName, email);
  }

  // Popular webmail providers keyed by email domain, so the matching inbox
  // link is surfaced first on the "check your inbox" screen.
  const MAIL_PROVIDERS = {
    "gmail.com": "https://mail.google.com/",
    "googlemail.com": "https://mail.google.com/",
    "outlook.com": "https://outlook.live.com/mail/",
    "hotmail.com": "https://outlook.live.com/mail/",
    "live.com": "https://outlook.live.com/mail/",
    "yahoo.com": "https://mail.yahoo.com/",
    "proton.me": "https://mail.proton.me/",
    "protonmail.com": "https://mail.proton.me/",
    "icloud.com": "https://www.icloud.com/mail/",
    "me.com": "https://www.icloud.com/mail/",
    "aol.com": "https://mail.aol.com/",
  };

  function showVerifyView(nodeName, email) {
    const signupView = $("#signup-view");
    const verifyView = $("#verify-view");
    if (!verifyView) {
      // Fallback if markup is missing: the pending screen, never the product —
      // an unconfirmed address must not reach the dashboard (issue #592).
      location.href = "/verify-pending";
      return;
    }
    $("#recap-name").textContent = nodeName;
    $("#recap-email").textContent = email;
    // Move the provider matching the user's email domain to the front.
    const domain = (email.split("@")[1] || "").toLowerCase();
    const inbox = MAIL_PROVIDERS[domain];
    const links = $("#mail-links");
    if (inbox && links) {
      const match = Array.prototype.find.call(
        links.querySelectorAll("a"), (a) => a.getAttribute("href") === inbox);
      if (match) links.insertBefore(match, links.firstChild);
    }
    if (signupView) signupView.hidden = true;
    verifyView.hidden = false;
    document.title = "Verify your email · ForkMesh";
    window.scrollTo(0, 0);
  }

  nameInput.addEventListener("input", validateName);
  emailInput.addEventListener("input", () => {
    validateEmail(emailInput.getAttribute("aria-invalid") === "true");
  });
  emailInput.addEventListener("blur", () => validateEmail(true));
  passwordInput.addEventListener("input", updateCreateState);
  if (signupReferralCodeInput) {
    signupReferralCodeInput.addEventListener("input", updateCreateState);
  }
  termsAgree.addEventListener("change", updateCreateState);
  form.addEventListener("submit", createAccount);

  // Browser history can restore the visible username without replaying its
  // input event, leaving the transient availability state stale. Reconcile
  // both normal history restoration and BFCache restoration when this page is
  // shown again.
  window.addEventListener("pageshow", () => {
    validateName();
    updateCreateState();
  });
})();
