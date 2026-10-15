(() => {
  // The practical mailbox/domain rules signup.js applies, kept in step with
  // valid_account_email in app/src/entry.py (issue #622). Login used to guard
  // only on a bare "@", so "test@" and "test@.com" were posted to
  // /api/accounts/login and came back as a credential error while
  // "testgmail.com" was refused here -- one class of malformed input behaving
  // two different ways, and avoidable traffic on the auth endpoint.
  const EMAIL_LOCAL_RE = /^[a-z0-9.!#$%&'*+/=?^_`{|}~-]+$/i;
  const EMAIL_DOMAIN_RE = /^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/i;

  const $ = (sel) => document.querySelector(sel);
  const hint = $("#login-hint");
  const btn = $("#login-btn");
  const password = $("#password");
  const passwordToggle = $("#password-toggle");
  const expiry = document.querySelector("[data-instance-login-expiry]");

  function startInstanceExpiry(deadline) {
    if (!expiry || !Number.isFinite(deadline) || deadline <= Date.now()) return;
    const update = () => {
      const seconds = Math.max(0, Math.ceil((deadline - Date.now()) / 1000));
      const minutes = Math.floor(seconds / 60);
      expiry.textContent = `This new instance will be removed unless its administrator signs in within ${minutes}:${String(seconds % 60).padStart(2, "0")}.`;
      expiry.hidden = false;
      if (!seconds) clearInterval(timer);
    };
    const timer = setInterval(update, 1000);
    update();
  }

  fetch("/api/instance", { cache: "no-store" }).then(async (response) => {
    if (!response.ok) return;
    const instance = await response.json();
    startInstanceExpiry(Number(instance.loginDeadline));
  }).catch(() => {});

  function loadTotpQr() {
    if (window.ForkMeshQR) return Promise.resolve(true);
    return new Promise((resolve) => {
      const script = document.createElement("script");
      script.src = "/qr.js";
      script.onload = () => resolve(Boolean(window.ForkMeshQR));
      script.onerror = () => resolve(false);
      document.head.append(script);
    });
  }

  function showTotpQr(uri) {
    const holder = document.querySelector("#totp-setup-qr");
    if (!holder) return;
    holder.replaceChildren();
    if (!uri) return;
    loadTotpQr().then((ok) => {
      if (ok) holder.replaceChildren(window.ForkMeshQR.toCanvas(uri, 184));
    });
  }

  function setHint(text, cls) {
    hint.textContent = text || "";
    hint.className = "hint" + (cls ? " " + cls : "");
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

  const DEMO_EMAIL = "demo@forkmesh.local";
  const DEMO_PASSWORD = "forkmesh-demo";

  // Where to land after a successful login. Only same-site paths are honored
  // (an absolute URL here would be an open redirect). Used by flows that bounce
  // through login, e.g. the desktop app's "Link this node to your account"
  // grant URL (adhoc #120), so they resume exactly where they left off.
  function nextPath() {
    let value = "";
    try {
      value = new URLSearchParams(location.search).get("next") || "";
    } catch (_) {}
    // Must be a same-origin path. Reject "//host" and "/\host" (browsers
    // normalize the backslash to "/", so "/\evil.com" resolves off-origin) and
    // confirm the resolved origin matches ours before honoring it.
    if (!value.startsWith("/") || value.startsWith("//") || value.startsWith("/\\")) {
      return "";
    }
    try {
      if (new URL(value, location.origin).origin !== location.origin) return "";
    } catch (_) {
      return "";
    }
    return value;
  }

  // Fallback destination when no ?next= was passed: the same-origin page the
  // visitor came from (e.g. they hit a login-required page and clicked the
  // plain header "Login" link). Auth pages are excluded so a login ↔ signup ↔
  // forgot-password shuffle doesn't bounce straight back into a form.
  function referrerPath() {
    let url = null;
    try {
      url = new URL(document.referrer);
    } catch (_) {
      return "";
    }
    if (url.origin !== location.origin) return "";
    if (/^\/(login|signup|forgot-password|reset-password)(\.html)?$/.test(url.pathname)) {
      return "";
    }
    return url.pathname + url.search + url.hash;
  }

  function demoLoginAllowed() {
    return location.hostname === "localhost" || location.hostname === "127.0.0.1";
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
        solana: body.solana || "",
        hasPayoutAddress: Boolean(body.hasPayoutAddress),
        sessionToken: (
          location.protocol === "https:" &&
          body.sessionToken &&
          window.ForkMeshAPI?.isSameSite !== false
            ? "cookie"
            : body.sessionToken || ""
        ),
        avatarPng: body.avatarPng || "",
        avatarUpdatedAt: Number(body.avatarUpdatedAt) || 0,
        createdAt: Number(body.createdAt) || 0,
        profileBio: body.profileBio || "",
        profileAbout: body.profileAbout || body.profileReadme || "",
        profileReadme: body.profileReadme || body.profileAbout || "",
        profileLocation: body.profileLocation || "",
        profileTimezone: body.profileTimezone || "",
        profileLinks: Array.isArray(body.profileLinks) ? body.profileLinks : [],
        profileFollowers: Number(body.followers) || 0,
        profileFollowing: Number(body.following) || 0,
        profileMirrorCount: Number(body.mirrorCount) || 0,
        referralClicks: Number(body.referralClicks) || 0,
        creditedReferrals: Number(body.creditedReferrals) || 0,
        referralLastAt: Number(body.referralLastAt) || 0,
        referredUsers: Array.isArray(body.referredUsers) ? body.referredUsers : [],
        legacyUnattributed: Number(body.legacyUnattributed) || 0,
        profilePrivate: Boolean(body.profilePrivate),
        followersPublic: Boolean(body.followersPublic),
        publicEmail: body.publicEmail || "",
        mastodon: body.mastodon || "",
        mastodonUrl: body.mastodonUrl || "",
        emailNotifications: body.emailNotifications !== false,
        notificationPreferences:
          body.notificationPreferences && typeof body.notificationPreferences === "object"
            ? body.notificationPreferences
            : {},
        notificationDelivery: body.notificationDelivery || "digest",
        notificationPauseUntil: Number(body.notificationPauseUntil) || 0,
        kind: body.kind || "",
        owner: body.owner || "",
        online: Boolean(body.online),
        nodes: Array.isArray(body.nodes) ? body.nodes : [],
        // The login response is the canonical account payload. Dashboard
        // documents can trust it instead of repeating GET /api/accounts/name
        // after this redirect (bump when that payload contract changes).
        accountSnapshotVersion: 1,
        at: Date.now(),
      }));
      document.cookie = "forkmesh_session=1; Path=/; Max-Age=2592000; SameSite=Lax"
        + (location.protocol === "https:" ? "; Secure" : "");
    } catch (_) {}
  }

  async function login() {
    const email = $("#email").value.trim();
    const password = $("#password").value;
    const totp = $("#totp").value.trim();
    if (!email || !password) {
      setHint("Enter your email and password.", "bad");
      return;
    }
    if (!validEmail(email)) {
      setHint("Enter a valid email address with a domain ending like .com or .org.", "bad");
      return;
    }
    btn.disabled = true;
    btn.textContent = "Logging in…";
    let res, body = {};
    try {
      res = await fetch("/api/accounts/login", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ email, password, totp }),
      });
      body = await res.json();
    } catch (_) {
      btn.disabled = false; btn.textContent = "Log in";
      setHint("Network error - please try again.", "bad");
      return;
    }
    btn.disabled = false;
    btn.textContent = "Log in";

    if (res.ok) {
      setHint("Logged in as “" + (body.nodeName || email) + "”.", "good");
      // Persist a minimal, non-secret session marker for the static site.
      storeSession(body);
      // An unconfirmed address goes to the pending screen, not to whatever the
      // caller asked for: the dashboard would only bounce it there anyway, and
      // the Worker refuses its writes regardless (issue #592).
      const destination = body.emailVerified === false && body.email
        ? "/verify-pending"
        : nextPath() || referrerPath() || "/dashboard";
      setTimeout(() => (location.href = destination), 700);
      return;
    }
    if (body.error === "totp_setup_required" && body.totpSecret) {
      $("#totp-field").style.display = "block";
      const setup = $("#totp-setup");
      if (setup) {
        $("#totp-setup-secret").textContent = body.totpSecret;
        $("#totp-setup-link").href = body.otpauthUri || "#";
        showTotpQr(body.otpauthUri);
        setup.hidden = false;
      }
      $("#totp").focus();
      setHint("Set up your authenticator app to finish logging in.", "");
      return;
    }
    if (body.error === "bad_totp") {
      $("#totp-field").style.display = "block";
      setHint(totp ? "Incorrect authenticator code." : "Enter your authenticator code.", totp ? "bad" : "");
      return;
    }
    setHint(
      // The relay returns one generic code for a bad email/password/unknown
      // account so attackers can't enumerate which accounts exist.
      body.error === "invalid_credentials" ? "Incorrect email or password."
        : body.error === "too_many_attempts"
          ? "Too many failed attempts. Wait a few minutes and try again."
        : body.error === "account_disabled"
          ? "This account has been disabled."
        : "Could not log in. Please try again.", "bad");
  }

  // If we were bounced here from a gated page (e.g. the admin dashboard, whose
  // short-lived HttpOnly cookie lapses well before the 30-day login session
  // does) and we still hold a valid account session, re-establish the admin
  // page cookie from that session token instead of forcing a password re-entry
  // (adhoc #163). On any failure we silently fall back to the normal form.
  const ADMIN_RESUME_KEY = "forkmesh.adminResumeAt";

  async function resumeAdminSession() {
    const next = nextPath();
    if (!next) return;
    let session = null;
    try {
      session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
    } catch (_) {}
    if (!session || !session.isAdmin || !session.sessionToken) return;
    // If we resumed seconds ago and got bounced straight back here, the gated
    // page is rejecting the freshly minted cookie — fall through to the
    // password form instead of redirect-looping forever (adhoc #168). A
    // resume from hours ago (cookie lapsed again) still retries silently.
    try {
      if (Date.now() - Number(sessionStorage.getItem(ADMIN_RESUME_KEY) || 0) < 15000) return;
    } catch (_) {}
    setHint("Resuming your session…", "");
    try {
      const res = await fetch("/api/accounts/admin-session", {
        method: "POST",
        headers: {
          "content-type": "application/json",
          accept: "application/json",
          authorization: "Bearer " + session.sessionToken,
        },
        body: JSON.stringify({ sessionToken: session.sessionToken }),
      });
      if (res.ok) {
        try { sessionStorage.setItem(ADMIN_RESUME_KEY, String(Date.now())); } catch (_) {}
        location.href = next;
        return;
      }
    } catch (_) {}
    setHint("", "");
  }
  resumeAdminSession().catch(() => {});

  // The credential fields sit in a real <form> so password managers recognize
  // them, which means the submit event -- Log in, or Enter in any field -- is
  // what runs the request. A browser or intermediary can briefly pair a cached
  // pre-form document with this script during a rollout; there the button is
  // still a plain one, so keep the old click/Enter wiring as the fallback.
  function submitLogin() {
    login().catch(() => {
      btn.disabled = false;
      btn.textContent = "Log in";
      setHint("Could not log in. Reload the page and try again.", "bad");
    });
  }
  const form = $("#login-form");
  if (form) {
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      submitLogin();
    });
  } else {
    btn.addEventListener("click", submitLogin);
    for (const id of ["email", "password", "totp"]) {
      $("#" + id)?.addEventListener("keydown", (e) => { if (e.key === "Enter") submitLogin(); });
    }
  }
  // A browser or intermediary can briefly pair a cached pre-toggle login
  // document with the current fingerprinted script during a rollout. Treat
  // the optional enhancement as absent in that mixed-version window instead
  // of turning login itself into an uncaught TypeError.
  if (password && passwordToggle) {
    passwordToggle.addEventListener("click", () => {
      const isVisible = password.type === "text";
      password.type = isVisible ? "password" : "text";
      passwordToggle.setAttribute("aria-label", isVisible ? "Show password" : "Hide password");
      passwordToggle.setAttribute("title", isVisible ? "Show password" : "Hide password");
      passwordToggle.setAttribute("aria-pressed", String(!isVisible));
      const showIcon = $(".password-toggle-show");
      const hideIcon = $(".password-toggle-hide");
      if (showIcon) showIcon.hidden = !isVisible;
      if (hideIcon) hideIcon.hidden = isVisible;
    });
  }
  if (demoLoginAllowed()) {
    const demoBox = $("#demo-credentials");
    const demoFill = $("#demo-fill-btn");
    if (demoBox) demoBox.style.display = "block";
    if (demoFill) {
      demoFill.addEventListener("click", () => {
        $("#email").value = DEMO_EMAIL;
        $("#password").value = DEMO_PASSWORD;
        setHint("Demo credentials filled. Click Log in.", "");
      });
    }
  }
})();
