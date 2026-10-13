(() => {
  // The hold-out screen for an account whose email nobody has confirmed
  // (issue #592). Signup used to hand the new account a "Continue to
  // dashboard" link, so an address that was never proven got the whole
  // authenticated product; the Worker now refuses every write from such a
  // session and every dashboard document bounces here instead.
  //
  // Nothing on this page needs a verified account: it re-reads the public
  // account record to notice the confirmation, and re-sends the link.
  const $ = (sel) => document.querySelector(sel);

  const SESSION_KEY = "forkmesh.session";
  // The confirmation is clicked in another tab (or on a phone), so poll for it
  // rather than making the user find the button. Slow enough to be free, and
  // it stops once the tab has been sitting here unattended for a while.
  const POLL_MS = 6000;
  const POLL_LIMIT_MS = 15 * 60 * 1000;

  const nameEl = $("#pending-name");
  const emailEl = $("#pending-email");
  const recheckButton = $("#pending-recheck");
  const resendButton = $("#pending-resend");
  const logoutButton = $("#pending-logout");
  const hint = $("#pending-hint");

  const startedAt = Date.now();
  let pollTimer = null;

  function readSession() {
    try {
      return JSON.parse(localStorage.getItem(SESSION_KEY) || "null");
    } catch (_) {
      return null;
    }
  }

  function writeSession(session) {
    try {
      localStorage.setItem(SESSION_KEY, JSON.stringify(session));
    } catch (_) {}
  }

  function setHint(message, kind) {
    hint.textContent = message || "";
    hint.className = "hint" + (kind ? " " + kind : "");
  }

  // localStorage keeps only the marker "cookie" for a same-site HTTPS session;
  // the real token is the HttpOnly cookie, and sending the marker as a bearer
  // would just look like a bad token.
  function bearerToken(session) {
    const token = session?.sessionToken || "";
    return token && token !== "cookie" ? token : "";
  }

  function toDashboard() {
    location.replace("/dashboard");
  }

  let session = readSession();
  if (!session || !session.nodeName) {
    location.replace("/login?next=" + encodeURIComponent("/verify-pending"));
    return;
  }
  nameEl.textContent = session.nodeName;
  emailEl.textContent = session.email || "—";

  // Reads the authoritative flag off the public account record. Returns true
  // once the address is confirmed, having navigated on to the dashboard.
  async function checkVerified() {
    const name = String(session.nodeName || "").trim().toLowerCase();
    if (!name) return false;
    let body = {};
    try {
      const response = await fetch(
        "/api/accounts/" + encodeURIComponent(name),
        { headers: { accept: "application/json" }, cache: "no-store" });
      body = await response.json();
    } catch (_) {
      return false;
    }
    if (body.exists === false || !body.emailVerified) return false;
    session = { ...session, emailVerified: true };
    writeSession(session);
    toDashboard();
    return true;
  }

  async function recheck() {
    recheckButton.disabled = true;
    setHint("Checking…", "");
    const verified = await checkVerified();
    recheckButton.disabled = false;
    if (!verified) {
      setHint(
        "That address is still unconfirmed. Open the link in the email — "
        + "check spam too — or send yourself a new one.",
        "bad");
    }
  }

  function resendMessage(error, retryAfterMs) {
    const minutes = Math.max(1, Math.ceil((retryAfterMs || 0) / 60000));
    if (error === "resend_too_soon") {
      return "A link just went out. Try again in a minute.";
    }
    if (error === "resend_limit_reached") {
      return `Too many verification emails today. Try again in ${minutes} minute${minutes === 1 ? "" : "s"}.`;
    }
    if (error === "already_verified") {
      return "This address is already verified — continue to the dashboard.";
    }
    if (error === "no_email") return "Add an email address to your account first.";
    if (error === "unauthorized") return "Sign in again, then resend the verification email.";
    return "Could not send the verification email. Try again in a moment.";
  }

  async function resend() {
    resendButton.disabled = true;
    resendButton.textContent = "Sending…";
    try {
      const token = bearerToken(session);
      const response = await fetch("/api/accounts/resend-verification", {
        method: "POST",
        credentials: "same-origin",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          ...(token ? { authorization: "Bearer " + token } : {}),
        },
        body: JSON.stringify({ sessionToken: token }),
      });
      const body = await response.json().catch(() => ({}));
      if (!response.ok || body.ok === false) {
        // The link may have been clicked between load and this click.
        if (body.error === "already_verified") {
          if (await checkVerified()) return;
        }
        setHint(resendMessage(body.error, body.retryAfterMs), "bad");
        return;
      }
      setHint(body.verificationSent
        ? "Verification email sent. Check your inbox and spam folder."
        : "Mail provider unavailable — an administrator was pinged to verify you by hand.",
        body.verificationSent ? "good" : "");
    } catch (_) {
      setHint(resendMessage(""), "bad");
    } finally {
      resendButton.disabled = false;
      resendButton.textContent = "Resend verification email";
    }
  }

  function signOut() {
    logoutButton.disabled = true;
    try {
      fetch("/api/accounts/logout", {
        method: "POST",
        keepalive: true,
        credentials: "same-origin",
        cache: "no-store",
      }).catch(() => {});
    } catch (_) {}
    try {
      for (let index = localStorage.length - 1; index >= 0; index -= 1) {
        const key = localStorage.key(index);
        if (key?.startsWith("forkmesh.")) localStorage.removeItem(key);
      }
    } catch (_) {}
    document.cookie = "forkmesh_session=; Path=/; Max-Age=0; SameSite=Lax";
    window.setTimeout(() => location.replace("/signup"), 250);
  }

  function schedulePoll() {
    window.clearTimeout(pollTimer);
    if (Date.now() - startedAt > POLL_LIMIT_MS) return;
    pollTimer = window.setTimeout(async () => {
      if (document.visibilityState === "visible") {
        if (await checkVerified()) return;
      }
      schedulePoll();
    }, POLL_MS);
  }

  recheckButton.addEventListener("click", recheck);
  resendButton.addEventListener("click", resend);
  logoutButton.addEventListener("click", signOut);
  // Someone who confirmed in another tab and then landed here should not have
  // to click anything.
  checkVerified().then((verified) => {
    if (!verified) schedulePoll();
  });
})();
