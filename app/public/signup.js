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
  const termsAccepted = () => termsAgree.checked || Boolean(termsAgree.closest("[hidden]"));
  const updatesAgree = $("#updates-agree");
  const createButton = $("#signup-create");
  const nameHint = $("#name-hint");
  const emailHint = $("#email-hint");
  const signupHint = $("#signup-hint");
  const suggestButton = $("[data-signup-suggest]");
  const signupPolicy = (() => {
    try {
      const parsed = JSON.parse(form.dataset.signupPolicy || "{}");
      return parsed && typeof parsed === "object" ? parsed : {};
    } catch (_) {
      return {};
    }
  })();
  const emailOptional = signupPolicy.emailOptional === true;
  const handleMode = ["suggest", "require"].includes(signupPolicy.handles)
    ? signupPolicy.handles : "off";

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
  let challengeMissed = 0;

  const BACKUP_PREFIX = "forkmesh-signup-backup-v1:";
  const BACKUP_BITS = 18;
  const BACKUP_BATCH = 20000;
  const CHALLENGE_LOAD_WAIT_MS = 10 * 1000;
  const SHA256_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];
  const SHA256_W = new Uint32Array(64);
  let backupReason = "";
  let backupProof = null;
  let backupRun = null;

  function sha256Head(text) {
    const length = text.length;
    const words = new Uint32Array((((length + 8) >> 6) + 1) * 16);
    for (let i = 0; i < length; i += 1) words[i >> 2] |= text.charCodeAt(i) << (24 - (i & 3) * 8);
    words[length >> 2] |= 0x80 << (24 - (length & 3) * 8);
    words[words.length - 1] = length * 8;
    const W = SHA256_W;
    let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
    for (let base = 0; base < words.length; base += 16) {
      for (let t = 0; t < 64; t += 1) {
        if (t < 16) {
          W[t] = words[base + t];
        } else {
          const x = W[t - 15];
          const y = W[t - 2];
          W[t] = W[t - 16] + W[t - 7]
            + (((x >>> 7) | (x << 25)) ^ ((x >>> 18) | (x << 14)) ^ (x >>> 3))
            + (((y >>> 17) | (y << 15)) ^ ((y >>> 19) | (y << 13)) ^ (y >>> 10));
        }
      }
      let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
      for (let t = 0; t < 64; t += 1) {
        const t1 = (h + (((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7)))
          + ((e & f) ^ (~e & g)) + SHA256_K[t] + W[t]) | 0;
        const t2 = ((((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10)))
          + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      h0 = (h0 + a) | 0; h1 = (h1 + b) | 0; h2 = (h2 + c) | 0; h3 = (h3 + d) | 0;
      h4 = (h4 + e) | 0; h5 = (h5 + f) | 0; h6 = (h6 + g) | 0; h7 = (h7 + h) | 0;
    }
    return h0 >>> 0;
  }

  function solveBackupProof(prefix) {
    return new Promise((resolve) => {
      let nonce = 0;
      const step = () => {
        try {
          for (const stop = nonce + BACKUP_BATCH; nonce < stop; nonce += 1) {
            if ((sha256Head(prefix + nonce) >>> (32 - BACKUP_BITS)) === 0) {
              resolve(String(nonce));
              return;
            }
          }
          setTimeout(step, 0);
        } catch (_) {
          resolve("");
        }
      };
      setTimeout(step, 0);
    });
  }

  function backupRunFor(cdata) {
    if (!backupRun || backupRun.cdata !== cdata) {
      backupRun = { cdata, promise: solveBackupProof(BACKUP_PREFIX + cdata + ":") };
    }
    return backupRun.promise;
  }

  async function backupAnswer() {
    while (challengeBox && backupReason) {
      const cdata = challengeBox.dataset.cdata || "";
      if (backupProof && backupProof.cdata === cdata) return backupProof;
      const nonce = await backupRunFor(cdata);
      if (!nonce) {
        backupRun = null;
        return null;
      }
      if ((challengeBox.dataset.cdata || "") === cdata) backupProof = { cdata, nonce };
    }
    return null;
  }

  function useBackupCheck(reason) {
    if (!challengeBox) return Promise.resolve(null);
    if (!backupReason) {
      backupReason = String(reason || "unknown").toLowerCase().replace(/[^a-z0-9_]/g, "").slice(0, 24) || "unknown";
      challengeToken = "";
      challengeAt = 0;
      try {
        if (challengeWidget !== null && window.turnstile) window.turnstile.remove(challengeWidget);
      } catch (_) {}
      challengeWidget = null;
      challengeBox.hidden = true;
      if (/security check/i.test(signupHint.textContent)) setSignupHint("", "");
      const waiters = challengeWaiters;
      challengeWaiters = [];
      waiters.forEach((resolve) => resolve(""));
    }
    return backupAnswer();
  }

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
    if (!challengeBox || challengeWidget !== null || backupReason || !window.turnstile) return;
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
          useBackupCheck("e" + challengeErrorCode);
          return true;
        },
      });
    } catch (error) {
      challengeWidget = null;
      useBackupCheck("render_" + String((error && error.name) || "failed"));
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
      backupProof = null;
      if (backupReason) {
        backupAnswer();
        return true;
      }
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
    const token = renewed ? await (backupReason ? backupAnswer() : waitForChallenge()) : "";
    createButton.disabled = false;
    createButton.textContent = "Create account";
    if (!renewed) {
      setSignupHint("Couldn’t refresh the signup page. Check your connection and try again.", "bad");
      reportProblem("signup_page_renew_failed", "");
      return false;
    }
    if (challengeBox && (backupReason || challengeWidget !== null) && !token) {
      setSignupHint(backupReason
        ? "The security check could not run. Reload the page and try again."
        : "Complete the security check above, then create your account.", "bad");
      if (backupReason) reportProblem("challenge_backup_failed", backupReason);
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
  if (challengeBox && isLive) {
    setTimeout(() => {
      if (!window.turnstile && challengeWidget === null) useBackupCheck("unavailable");
    }, CHALLENGE_LOAD_WAIT_MS);
  }

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

  function emailAcceptable(value) {
    return (emailOptional && !value) || validEmail(value);
  }

  function validateEmail(showError) {
    const value = emailInput.value.trim();
    const ok = emailAcceptable(value);
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
    createButton.disabled = !(nameOk && emailAcceptable(email) && password.length >= 8
      && termsAccepted() && referralCodeOk);
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

  async function solveBackupCheck() {
    createButton.disabled = true;
    createButton.textContent = "Checking…";
    setSignupHint("Running a backup security check…", "");
    const proof = await backupAnswer();
    createButton.disabled = false;
    createButton.textContent = "Create account";
    if (!proof) {
      setSignupHint("The security check could not run. Reload the page and try again.", "bad");
      reportProblem("challenge_backup_failed", backupReason);
      return false;
    }
    setSignupHint("", "");
    return true;
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
    if (!termsAccepted()) {
      setSignupHint(termsAgree.closest("[data-instance-legal]")
        ? "Tick the agreement box to continue."
        : "Accept the Terms and Privacy policy to continue.", "bad");
      return;
    }
    if (signupIntentStale() && !(await renewSignupPage())) {
      return;
    }
    if (challengeBox && !backupReason && challengeWidget !== null && challengeStale()) {
      createButton.disabled = true;
      createButton.textContent = "Checking…";
      setSignupHint("Refreshing the security check…", "");
      const fresh = await refreshChallenge();
      createButton.disabled = false;
      createButton.textContent = "Create account";
      setSignupHint("", "");
      if (!fresh) useBackupCheck("stale");
    }
    if (challengeBox && !backupReason && !widgetToken()) {
      if (!window.turnstile) {
        useBackupCheck("unavailable");
      } else if (challengeWidget === null) {
        renderChallenge();
        if (challengeWidget === null) useBackupCheck("not_rendered");
      } else if (challengeMissed < 1) {
        challengeMissed += 1;
        setSignupHint("Complete the security check above, then create your account.", "bad");
        reportProblem("challenge_token_missing", challengeErrorCode);
        return;
      } else {
        useBackupCheck("token_missing");
      }
      if (!backupReason) {
        setSignupHint("Complete the security check above, then create your account.", "bad");
        return;
      }
    }
    if (challengeBox && backupReason && !(await solveBackupCheck())) {
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
        turnstileToken: backupReason ? "" : challengeToken,
        ...(backupReason && backupProof
          ? { backupProof: { nonce: backupProof.nonce, reason: backupReason } }
          : {}),
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
      if (!result.ok && isLive && challengeBox && !backupReason
          && result.body.error === "turnstile_unavailable") {
        useBackupCheck("server_unavailable");
        if (!(await solveBackupCheck())) return;
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
          : body.error === "rate_limited" ? "Too many signups from this network right now. Please try again in an hour."
          : body.error === "signup_closed" ? "Sign-up is closed on this instance."
          : body.error === "random_handle_required" ? "This instance only accepts suggested random names. Press Suggest a random name."
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
    if (body.recoveryCode) {
      showRecoveryView(String(body.recoveryCode));
      return;
    }
    if (!email) {
      openEmailFreeAccount();
      return;
    }
    showVerifyView(nodeName, email);
  }

  function openEmailFreeAccount() {
    location.href = "/dashboard";
  }

  function showRecoveryView(code) {
    const signupView = $("#signup-view");
    const recoveryView = $("#recovery-view");
    if (!recoveryView) {
      openEmailFreeAccount();
      return;
    }
    $("#recovery-code").textContent = code;
    const saved = $("#recovery-saved");
    const next = $("#recovery-continue");
    saved.addEventListener("change", () => { next.disabled = !saved.checked; });
    next.addEventListener("click", openEmailFreeAccount);
    $("#recovery-copy").addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code);
        $("#recovery-copy").textContent = "Copied";
      } catch (_) {
        $("#recovery-copy").textContent = "Select the code above and copy it";
      }
    });
    if (signupView) signupView.hidden = true;
    recoveryView.hidden = false;
    document.title = "Save your recovery code";
    window.scrollTo(0, 0);
  }

  async function suggestHandle() {
    if (!isLive) return;
    suggestButton.disabled = true;
    try {
      const { ok, body } = await api("/api/accounts/handle-suggestion");
      if (ok && body.handle) {
        nameInput.value = String(body.handle);
        validateName();
      }
    } catch (_) {
      setNameHint("Couldn’t get a suggestion. Try again.", "bad");
    } finally {
      suggestButton.disabled = false;
    }
  }

  function applySignupPolicy() {
    if (signupPolicy.signup === "closed") {
      form.hidden = true;
      $("#signup-closed").hidden = false;
      return;
    }
    if (emailOptional) {
      emailInput.required = false;
      emailInput.placeholder = "Optional";
      document.querySelector("[data-signup-email-optional]").hidden = false;
      document.querySelector("[data-signup-email-note]").hidden = false;
    }
    if (handleMode !== "off") {
      suggestButton.hidden = false;
      document.querySelector("[data-signup-handle-note]").hidden = false;
      suggestButton.addEventListener("click", suggestHandle);
      if (handleMode === "require") nameInput.readOnly = true;
      if (!nameInput.value && signupPolicy.suggestion) {
        nameInput.value = String(signupPolicy.suggestion);
      }
    }
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

  applySignupPolicy();
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
