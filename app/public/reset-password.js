(() => {
  const $ = (sel) => document.querySelector(sel);
  const hint = $("#reset-hint");
  const btn = $("#reset-btn");

  function setHint(text, cls) {
    hint.textContent = text || "";
    hint.className = "hint" + (cls ? " " + cls : "");
  }

  const params = new URLSearchParams(location.search);
  const node = (params.get("node") || "").trim();
  const exp = Number(params.get("exp")) || 0;
  const token = (params.get("token") || "").trim();

  // A link that's missing its pieces (or manually opened) can't reset anything.
  if (!node || !exp || !token) {
    setHint("This reset link is invalid. Request a new one.", "bad");
    btn.disabled = true;
  } else {
    const nodeLabel = $("#reset-node");
    if (nodeLabel) nodeLabel.textContent = "Set a new password for “" + node + "”.";
    if (Date.now() > exp) {
      setHint("This reset link has expired. Request a new one.", "bad");
    }
  }

  async function submit() {
    const password = $("#password").value;
    const confirm = $("#confirm").value;
    if (password.length < 8) {
      setHint("Password must be at least 8 characters.", "bad");
      return;
    }
    if (password !== confirm) {
      setHint("Passwords don't match.", "bad");
      return;
    }
    btn.disabled = true;
    btn.textContent = "Saving…";
    let res, body = {};
    try {
      res = await fetch("/api/accounts/reset-password", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ node, exp, token, password }),
      });
      body = await res.json();
    } catch (_) {
      btn.disabled = false;
      btn.textContent = "Set new password";
      setHint("Network error - please try again.", "bad");
      return;
    }
    if (res.ok) {
      setHint("Password updated. Redirecting to log in…", "good");
      setTimeout(() => (location.href = "/login"), 900);
      return;
    }
    btn.disabled = false;
    btn.textContent = "Set new password";
    setHint(
      body.error === "password_too_short" ? "Password must be at least 8 characters."
        : body.error === "reset_link_expired"
          ? "This reset link has expired. Request a new one."
        : body.error === "invalid_reset_token"
          ? "This reset link is invalid or has already been used. Request a new one."
        : "Could not reset your password. Please try again.", "bad");
  }

  btn.addEventListener("click", submit);
  for (const id of ["password", "confirm"]) {
    $("#" + id).addEventListener("keydown", (e) => { if (e.key === "Enter") submit(); });
  }
})();
