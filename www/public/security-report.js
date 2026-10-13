(() => {
  const $ = (sel) => document.querySelector(sel);
  const btn     = $("#sr-btn");
  const hint    = $("#sr-hint");
  const titleEl = $("#sr-title");
  const bodyEl  = $("#sr-body");
  const contact = $("#sr-contact");
  const comp    = $("#sr-component");

  function setHint(text, cls) {
    hint.textContent = text || "";
    hint.className = "hint" + (cls ? " " + cls : "");
  }

  async function submit() {
    const title = titleEl.value.trim();
    const body  = bodyEl.value.trim();

    if (!title) {
      setHint("Please enter a title for the vulnerability.", "bad");
      titleEl.focus();
      return;
    }
    if (!body) {
      setHint("Please describe the vulnerability in the Description field.", "bad");
      bodyEl.focus();
      return;
    }

    btn.disabled = true;
    btn.textContent = "Sending…";
    setHint("");

    let res;
    try {
      res = await fetch("/api/security/report", {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({
          title,
          body,
          component: comp.value,
          contact: contact.value.trim(),
        }),
      });
    } catch (_) {
      btn.disabled = false;
      btn.textContent = "Submit report";
      setHint("Network error — please try again, or email security@forkmesh.com.", "bad");
      return;
    }

    btn.disabled = false;
    btn.textContent = "Submit report";

    if (res.ok) {
      setHint(
        "Your report has been submitted. Thank you for helping keep ForkMesh secure. " +
        "We’ll follow up if you provided contact details.",
        "good",
      );
      titleEl.value = "";
      bodyEl.value  = "";
      contact.value = "";
      return;
    }

    if (res.status === 400) {
      setHint("Invalid report data. Please check all fields and try again.", "bad");
    } else {
      setHint("Could not submit your report. Please try again, or email security@forkmesh.com.", "bad");
    }
  }

  btn.addEventListener("click", submit);
})();
