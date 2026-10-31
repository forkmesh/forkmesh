(() => {
  const $ = (sel) => document.querySelector(sel);
  const hint = $("#forgot-hint");
  const btn = $("#forgot-btn");
  const input = $("#identifier");

  function setHint(text, cls) {
    hint.textContent = text || "";
    hint.className = "hint" + (cls ? " " + cls : "");
  }

  const challengeBox = $("#forgot-turnstile");
  const CHALLENGE_URL = "/api/accounts/forgot-password/challenge";
  const CHALLENGE_FRESH_MS = 240 * 1000;
  const CHALLENGE_WAIT_MS = 30 * 1000;
  const CHALLENGE_RETRY_ERRORS = 3;
  const CHALLENGE_LOAD_WAIT_MS = 10 * 1000;
  let challengeToken = "";
  let challengeAt = 0;
  let challengeWidget = null;
  let challengeWaiters = [];
  let challengeErrors = 0;
  let siteKey = null;
  let backupCdata = "";
  let configLoading = null;

  const BACKUP_PREFIX = "forkmesh-signup-backup-v1:";
  const BACKUP_BITS = 18;
  const BACKUP_BATCH = 20000;
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

  function loadConfig() {
    if (!configLoading) {
      configLoading = fetch(CHALLENGE_URL, { headers: { accept: "application/json" }, cache: "no-store" })
        .then((res) => res.json())
        .then((body) => {
          siteKey = body && body.siteKey ? String(body.siteKey) : "";
          backupCdata = body && body.cdata ? String(body.cdata) : "";
          return true;
        })
        .catch(() => false)
        .finally(() => { configLoading = null; });
    }
    return configLoading;
  }

  function backupRunFor(cdata) {
    if (!backupRun || backupRun.cdata !== cdata) {
      backupRun = { cdata, promise: solveBackupProof(BACKUP_PREFIX + cdata + ":") };
    }
    return backupRun.promise;
  }

  async function backupAnswer() {
    while (backupReason) {
      if (!backupCdata) await loadConfig();
      const cdata = backupCdata;
      if (!cdata) return null;
      if (backupProof && backupProof.cdata === cdata) return backupProof;
      const nonce = await backupRunFor(cdata);
      if (!nonce) {
        backupRun = null;
        return null;
      }
      if (backupCdata === cdata) backupProof = { cdata, nonce };
    }
    return null;
  }

  function useBackupCheck(reason) {
    if (!backupReason) {
      backupReason = String(reason || "unknown").toLowerCase().replace(/[^a-z0-9_]/g, "").slice(0, 24) || "unknown";
      challengeToken = "";
      challengeAt = 0;
      try {
        if (challengeWidget !== null && window.turnstile) window.turnstile.remove(challengeWidget);
      } catch (_) {}
      challengeWidget = null;
      challengeBox.hidden = true;
      if (/security check/i.test(hint.textContent) && !btn.disabled) setHint("", "");
      const waiters = challengeWaiters;
      challengeWaiters = [];
      waiters.forEach((resolve) => resolve(""));
    }
    return backupAnswer();
  }

  function challengeDone(token) {
    challengeToken = String(token || "");
    challengeAt = challengeToken ? Date.now() : 0;
    if (challengeToken && /security check/i.test(hint.textContent) && !btn.disabled) setHint("", "");
    const waiters = challengeWaiters;
    challengeWaiters = [];
    waiters.forEach((resolve) => resolve(challengeToken));
  }

  function renderChallenge() {
    if (challengeWidget !== null || backupReason || !siteKey || !window.turnstile) return;
    try {
      challengeWidget = window.turnstile.render(challengeBox, {
        sitekey: siteKey,
        action: "password-reset",
        theme: "auto",
        "response-field": false,
        callback: challengeDone,
        "expired-callback": () => { challengeToken = ""; challengeAt = 0; },
        "error-callback": (code) => {
          challengeToken = "";
          challengeAt = 0;
          challengeErrors += 1;
          const errorCode = String(code || "");
          if (/^[36]\d{5}$/.test(errorCode) && challengeErrors < CHALLENGE_RETRY_ERRORS) {
            if (!btn.disabled) setHint("The security check hit a snag and is retrying…", "");
            return true;
          }
          useBackupCheck("e" + errorCode);
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
      if (turnstile && challengeWidget !== null
          && !(turnstile.isExpired && turnstile.isExpired(challengeWidget))) {
        challengeToken = String(turnstile.getResponse(challengeWidget) || "");
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
    if (challengeWidget === null || backupReason) return Promise.resolve("");
    if (challengeToken) return Promise.resolve(challengeToken);
    return new Promise((resolve) => {
      challengeWaiters.push(resolve);
      setTimeout(() => resolve(widgetToken()), CHALLENGE_WAIT_MS);
    });
  }

  async function challengeAnswer() {
    if (siteKey === null) await loadConfig();
    if (!backupReason && !siteKey) useBackupCheck("unavailable");
    if (!backupReason && challengeWidget !== null && challengeStale()) {
      setHint("Refreshing the security check…", "");
      resetChallenge();
      if (!(await waitForChallenge())) useBackupCheck("stale");
    }
    if (!backupReason && !widgetToken()) {
      if (!window.turnstile) {
        useBackupCheck("unavailable");
      } else {
        renderChallenge();
        if (challengeWidget === null) {
          useBackupCheck("not_rendered");
        } else {
          setHint("Finishing the security check above…", "");
          if (!(await waitForChallenge())) useBackupCheck("token_missing");
        }
      }
    }
    if (backupReason) {
      setHint("Running a backup security check…", "");
      const proof = await backupAnswer();
      return proof ? { backupProof: { nonce: proof.nonce, reason: backupReason, cdata: proof.cdata } } : null;
    }
    const token = challengeToken;
    resetChallenge();
    return { turnstileToken: token };
  }

  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible" && challengeWidget !== null && challengeStale()) {
      resetChallenge();
    }
  });
  window.addEventListener("pageshow", (event) => {
    if (event && event.persisted && challengeWidget !== null) resetChallenge();
  });

  window.forkmeshForgotChallengeReady = renderChallenge;
  loadConfig().then((loaded) => {
    if (loaded && !siteKey) useBackupCheck("unavailable");
    else renderChallenge();
  });
  setTimeout(() => {
    if (!window.turnstile && challengeWidget === null) useBackupCheck("unavailable");
  }, CHALLENGE_LOAD_WAIT_MS);

  // Deliberately generic: whether or not the identifier matches an account, we
  // show the same confirmation so this page can't be used to probe which emails
  // are registered (the API returns {ok:true} either way).
  const SENT_MESSAGE =
    "If that account exists, a reset link is on its way. Check your email.";

  async function post(identifier, answer) {
    const res = await fetch("/api/accounts/forgot-password", {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ identifier, ...answer }),
    });
    let error = "";
    if (!res.ok) {
      try { error = String((await res.json()).error || ""); } catch (_) {}
    }
    return { ok: res.ok, error };
  }

  function errorMessage(error) {
    if (error === "rate_limited") return "Too many reset requests right now. Try again later.";
    if (error === "turnstile_unavailable") {
      return "Password reset is temporarily unavailable (security check offline). Try again in a few minutes.";
    }
    if (error.startsWith("turnstile_")) return "The security check did not pass. Try sending again.";
    return "Could not send a reset link. Please try again.";
  }

  async function send(identifier) {
    let answer = await challengeAnswer();
    if (!answer) return { ok: false, error: "backup_failed" };
    setHint("", "");
    btn.textContent = "Sending…";
    let result = await post(identifier, answer);
    const retry = !result.ok && (
      (!answer.backupProof && result.error === "turnstile_unavailable")
      || (answer.backupProof && result.error === "turnstile_expired"));
    if (retry) {
      btn.textContent = "Checking…";
      if (answer.backupProof) {
        backupCdata = "";
        backupProof = null;
        await loadConfig();
      } else {
        useBackupCheck("server_unavailable");
      }
      answer = await challengeAnswer();
      if (!answer) return { ok: false, error: "backup_failed" };
      setHint("", "");
      btn.textContent = "Sending…";
      result = await post(identifier, answer);
    }
    return result;
  }

  async function submit() {
    if (btn.disabled) return;
    const identifier = input.value.trim();
    if (!identifier) {
      setHint("Enter the email for your account.", "bad");
      return;
    }
    btn.disabled = true;
    btn.textContent = "Checking…";
    let result;
    try {
      result = await send(identifier);
    } catch (_) {
      result = { ok: false, error: "network" };
    }
    btn.disabled = false;
    btn.textContent = "Send reset link";
    if (result.ok) {
      setHint(SENT_MESSAGE, "good");
      input.value = "";
      return;
    }
    if (result.error === "network") {
      setHint("Network error - please try again.", "bad");
    } else if (result.error === "backup_failed") {
      setHint("The security check could not run. Reload the page and try again.", "bad");
    } else {
      setHint(errorMessage(result.error), "bad");
    }
  }

  btn.addEventListener("click", submit);
  input.addEventListener("keydown", (e) => { if (e.key === "Enter") submit(); });
})();

(() => {
  const $ = (sel) => document.querySelector(sel);
  const button = $("#recover-btn");
  if (!button) return;
  const hint = $("#recover-hint");
  const say = (text, tone) => {
    hint.textContent = text;
    hint.className = "hint" + (tone ? " " + tone : "");
  };
  const messages = {
    invalid_recovery_code: "That username and recovery code do not match.",
    password_too_short: "The new password needs at least 8 characters.",
    too_many_attempts: "Too many attempts. Wait a few minutes and try again.",
  };
  button.addEventListener("click", async () => {
    const nodeName = $("#recover-name").value.trim().toLowerCase();
    const recoveryCode = $("#recover-code").value.trim();
    const newPassword = $("#recover-pass").value;
    if (!nodeName || !recoveryCode) {
      say("Enter your username and recovery code.", "bad");
      return;
    }
    if (newPassword.length < 8) {
      say(messages.password_too_short, "bad");
      return;
    }
    button.disabled = true;
    say("Checking…", "");
    let body = {};
    let ok = false;
    try {
      const response = await fetch("/api/accounts/recover", {
        method: "POST",
        credentials: "same-origin",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ nodeName, recoveryCode, newPassword }),
      });
      ok = response.ok;
      body = await response.json().catch(() => ({}));
    } catch (_) {
      say("Network error. Try again.", "bad");
      button.disabled = false;
      return;
    }
    button.disabled = false;
    if (!ok || !body.recoveryCode) {
      say(messages[body.error] || "Could not reset the password. Try again.", "bad");
      return;
    }
    $("#recover-pass").value = "";
    $("#recover-code").value = "";
    say("", "");
    button.hidden = true;
    $("#recover-new-code").textContent = String(body.recoveryCode);
    $("#recover-done").hidden = false;
  });
})();
