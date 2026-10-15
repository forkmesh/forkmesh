(() => {
  const API = "/api/affiliates";
  const ERRORS = {
    sign_in_required: "Sign in first.",
    email_not_verified: "Verify your email address first. There is a link in your account settings.",
    terms_required: "Tick the box to agree to the rules.",
    channel_required: "Tell us where you meet the companies you will bring.",
    affiliate_removed: "This account can no longer join the program.",
    join_first: "Join the program first.",
    affiliate_not_active: "Your affiliate account is paused. Get in touch with us.",
    company_required: "Add the company name.",
    website_required: "Add the company website, like example.com.",
    invalid_contact_email: "That email address does not look right. Leave it empty if you do not have one.",
    consent_required: "Tick the box to confirm they are happy for us to have their email, or leave the email out.",
    self_referral: "You cannot claim yourself or your own company.",
    open_lead_limit: "You already have 10 open claims. One has to close or lapse before you claim another.",
    already_claimed: "Someone else has already claimed this company.",
    note_required: "Write a sentence about the outreach you did.",
    claim_expired: "This claim lapsed after 7 days without activity.",
    claim_closed: "This claim is closed, so there is nothing to keep alive.",
    origin_not_allowed: "Open this page on forkmesh.com and try again.",
  };
  const STATUS = {
    new: "Claimed",
    known: "Already known to us",
    contacted: "In touch",
    trial: "Trying ForkMesh",
    won: "Paying",
    paid: "Commission paid",
    lost: "Not this time",
    rejected: "Not accepted",
    expired: "Claim lapsed",
  };

  const $ = (selector) => document.querySelector(selector);

  function session() {
    try {
      const value = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!value || !value.nodeName || value.kind === "node") return null;
      return value;
    } catch (_) {
      return null;
    }
  }

  async function call(path, body) {
    const headers = { accept: "application/json" };
    if (body) headers["content-type"] = "application/json";
    const token = String(session()?.sessionToken || "");
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(path, {
      method: body ? "POST" : "GET",
      headers,
      credentials: "same-origin",
      cache: "no-store",
      body: body ? JSON.stringify(body) : undefined,
    });
    return { ok: response.ok, data: await response.json().catch(() => ({})) };
  }

  function say(node, text, bad) {
    if (!node) return;
    node.textContent = text || "";
    if (bad) node.dataset.tone = "bad";
    else delete node.dataset.tone;
  }

  function money(cents) {
    const value = Number(cents || 0) / 100;
    return `$${value % 1 ? value.toFixed(2) : value.toFixed(0)}`;
  }

  function day(ms) {
    return new Date(Number(ms)).toLocaleDateString(undefined, { day: "numeric", month: "short" });
  }

  function logForm(lead) {
    const form = cell("form", "aff-log");
    const input = document.createElement("input");
    input.name = "note";
    input.maxLength = 500;
    input.placeholder = "What did you do? e.g. Sent the pricing page to their CTO";
    input.setAttribute("aria-label", `Log outreach for ${lead.company}`);
    const button = cell("button", "", "Log outreach");
    button.type = "submit";
    const message = cell("span", "race-msg");
    form.append(input, button, message);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      const { ok, data } = await call(`${API}/leads/${encodeURIComponent(lead.id)}/activity`, {
        note: String(input.value || "").trim(),
      });
      if (!ok) {
        say(message, ERRORS[data?.error] || "Something went wrong. Try again in a minute.", true);
        return;
      }
      load();
    });
    return form;
  }

  function cell(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function renderLeads(leads) {
    const wrap = $("[data-aff-leads-wrap]");
    const list = $("[data-aff-leads]");
    const count = $("[data-aff-count]");
    if (!wrap || !list) return;
    list.replaceChildren();
    if (!leads.length) {
      wrap.hidden = true;
      return;
    }
    const open = leads.filter((lead) => ["new", "contacted", "trial"].includes(lead.status)).length;
    if (count) count.textContent = `${open} of 10 open`;
    leads.forEach((lead, index) => {
      const row = cell("li");
      if (lead.status === "won" || lead.status === "paid") row.dataset.won = "1";
      row.append(cell("span", "pos", String(index + 1).padStart(2, "0")));
      const name = cell("span", "name");
      name.append(cell("b", "", lead.company));
      const last = (lead.activity || []).slice(-1)[0];
      const detail = [lead.domain, lead.contactName, last ? `Last: ${last.note}` : ""].filter(Boolean).join(" · ");
      name.append(cell("span", "", detail));
      const open = ["new", "contacted", "trial"].includes(lead.status);
      if (open) name.append(logForm(lead));
      row.append(name);
      const meter = cell("span", "meter");
      meter.append(cell("span", "", STATUS[lead.status] || lead.status));
      if (lead.commissionCents) meter.append(cell("span", "", `You earn ${money(lead.commissionCents)}`));
      else if (open && lead.expiresAt) meter.append(cell("span", "", `Keep by ${day(lead.expiresAt)}`));
      else if (lead.note) meter.append(cell("span", "", lead.note));
      row.append(meter);
      list.append(row);
    });
    wrap.hidden = false;
  }

  function render(me) {
    const signedOut = $("[data-aff-signed-out]");
    const join = $("[data-aff-join]");
    const joined = $("[data-aff-joined]");
    const lead = $("[data-aff-lead]");
    const locked = $("[data-aff-locked]");
    const status = $("[data-aff-status]");
    if (!join) return;
    if (!me) {
      signedOut.hidden = false;
      join.hidden = true;
      joined.hidden = true;
      lead.hidden = true;
      locked.hidden = false;
      if (status) status.textContent = "";
      return;
    }
    signedOut.hidden = true;
    const affiliate = me.affiliate;
    const active = affiliate?.status === "active";
    join.hidden = Boolean(affiliate);
    joined.hidden = !affiliate;
    if (affiliate && !active) joined.textContent = "Your affiliate account is paused. Get in touch with us if you think that is a mistake.";
    lead.hidden = !active;
    locked.hidden = active;
    if (status) status.textContent = affiliate ? (active ? "Active" : "Paused") : me.emailVerified ? "Not joined" : "Verify your email";
    renderLeads(Array.isArray(me.leads) ? me.leads : []);
  }

  async function load() {
    try {
      const { ok, data } = await call(API);
      if (ok) render(data.me);
    } catch (_) {}
  }

  function wire() {
    const join = $("[data-aff-join]");
    join?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const message = $("[data-aff-join-msg]");
      say(message, "Joining…");
      const { ok, data } = await call(`${API}/join`, {
        channel: String(join.elements.channel.value || "").trim(),
        acceptTerms: join.elements.acceptTerms.checked,
      });
      if (!ok) {
        say(message, ERRORS[data?.error] || "Something went wrong. Try again in a minute.", true);
        return;
      }
      say(message, "");
      render(data.me);
    });
    const lead = $("[data-aff-lead]");
    lead?.addEventListener("submit", async (event) => {
      event.preventDefault();
      const message = $("[data-aff-lead-msg]");
      const value = (name) => String(lead.elements[name]?.value || "").trim();
      say(message, "Claiming…");
      const { ok, data } = await call(`${API}/leads`, {
        company: value("company"),
        website: value("website"),
        contactName: value("contactName"),
        contactEmail: value("contactEmail"),
        contactTitle: value("contactTitle"),
        notes: value("notes"),
        consent: lead.elements.consent.checked,
      });
      if (!ok) {
        say(message, ERRORS[data?.error] || "Something went wrong. Try again in a minute.", true);
        return;
      }
      lead.reset();
      say(message, data.known
        ? "That contact is already in our CRM, so this claim does not earn commission."
        : "Claimed. It is yours for 7 days; log your outreach below to keep it.");
      load();
    });
  }

  wire();
  load();
})();
