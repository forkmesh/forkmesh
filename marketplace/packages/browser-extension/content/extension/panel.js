// ForkMesh CRM panel and header, shared by every page the extension reads.
//
// Holds what does not depend on one site's markup: what an address or a
// fediverse handle says about a contact, the compact header that shows the
// record above the open message, and the panel that looks the contact up when
// its Look up button is pressed, links the message, and moves the contact
// through a pipeline. Nothing is requested just because a message was opened.
// content.js mounts it above an open email (Gmail, Zoho Mail), mastodon.js
// above an open Mastodon conversation, and popup.js in the toolbar popup.
// Everything that talks to ForkMesh goes through background.js; the panel
// never sees the session token and never sends the message body.
(() => {
  if (globalThis.ForkMeshCrm) return;

  const EMAIL_RE = /[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/;
  // Personal mailbox domains say nothing about where someone works.
  const FREE_MAIL = new Set([
    "gmail.com", "googlemail.com", "yahoo.com", "ymail.com", "rocketmail.com",
    "hotmail.com", "outlook.com", "live.com", "msn.com", "icloud.com", "me.com",
    "mac.com", "aol.com", "proton.me", "protonmail.com", "pm.me", "zoho.com",
    "zohomail.com", "gmx.com", "gmx.net", "yandex.com", "mail.com", "fastmail.com",
    "hey.com", "tutanota.com", "qq.com", "163.com", "comcast.net", "att.net",
  ]);
  const DETAIL_FIELDS = [
    ["company", "Company"], ["title", "Title"], ["phone", "Phone"],
    ["website", "Website"], ["linkedin", "LinkedIn"], ["mastodon", "Mastodon"],
    ["twitter", "Twitter"], ["github", "GitHub"],
  ];
  // A fediverse address, kept as "user@host" the way the CRM stores it:
  // "@grace@mastodon.social", the bare form, or a profile URL. A remote
  // profile served by another instance keeps its own host.
  const HANDLE_RE = /^@?([A-Za-z0-9_.-]{1,64})@([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)$/;
  const HANDLE_URL_RE = /^https:\/\/([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+)\/@([A-Za-z0-9_.-]{1,64})(?:@([A-Za-z0-9-]{1,63}(?:\.[A-Za-z0-9-]{1,63})+))?$/;

  const address = (value) => (String(value || "").match(EMAIL_RE) || [""])[0].toLowerCase();

  function handleOf(value) {
    const clean = String(value || "").replace(/\s+/g, "").replace(/\/+$/, "");
    const url = clean.match(HANDLE_URL_RE);
    if (url) return `${url[2]}@${url[3] || url[1]}`.toLowerCase();
    const plain = clean.match(HANDLE_RE);
    return plain ? `${plain[1]}@${plain[2]}`.toLowerCase() : "";
  }

  const domainOf = (email) => String(email || "").split("@")[1] || "";
  const titleCase = (slug) => String(slug || "").replace(/-/g, " ").replace(/\b\w/g, (char) => char.toUpperCase());
  const esc = (value) => String(value ?? "").replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[char]));

  function send(message) {
    return new Promise((resolve) => {
      try {
        chrome.runtime.sendMessage(message, (response) => {
          if (chrome.runtime.lastError) {
            resolve({ ok: false, error: "extension_unavailable", message: chrome.runtime.lastError.message });
          } else resolve(response || { ok: false, error: "no_response", message: "No response" });
        });
      } catch (error) {
        resolve({ ok: false, error: "extension_unavailable", message: String(error?.message || error) });
      }
    });
  }

  // A display name that is only the address's or handle's local part
  // ("grace") or the mail client's "Me" is not a name worth keeping.
  function realName(name, email) {
    const clean = String(name || "").replace(/^["']+|["']+$/g, "").replace(/\s+/g, " ").trim();
    if (!clean || clean.includes("@") || clean.toLowerCase() === "me") return "";
    const squash = (value) => value.toLowerCase().replace(/[\s._'-]/g, "");
    return squash(clean) === squash(String(email || "").split("@")[0]) ? "" : clean.slice(0, 160);
  }

  // A work address names the company and its website.
  function domainDetails(email) {
    const domain = domainOf(email).replace(/^(?:mail|email|e|mx)\./, "");
    if (!domain || FREE_MAIL.has(domain) || /^(yahoo|hotmail|outlook|live|aol|gmx|yandex)\./.test(domain)) return {};
    const labels = domain.split(".");
    const secondLevel = labels.length > 2 && /^(co|com|org|net|ac|gov|edu)$/.test(labels[labels.length - 2]);
    const core = labels[labels.length - (secondLevel ? 3 : 2)] || "";
    return { company: core ? core.charAt(0).toUpperCase() + core.slice(1) : "", website: `https://${domain}` };
  }

  // Put one participant on the message as the contact, with the name and
  // details the page offers for them. A participant is an address, a
  // fediverse handle, or both.
  function applyContact(message, person) {
    const email = address(person?.email);
    const handle = handleOf(person?.handle);
    const samePerson = (item) => Boolean(item)
      && ((email && address(item.email) === email) || (handle && handleOf(item.handle) === handle));
    const wroteIt = message.direction === "inbound" && samePerson(message.from);
    const signature = wroteIt ? message.signature || {} : {};
    // An instance host is where someone posts, never where they work: only a
    // mail domain says anything about a company.
    const domain = domainDetails(email);
    message.contactEmail = email;
    message.handle = handle;
    message.contactName = realName(person?.name, email || handle)
      || (wroteIt ? signature.name || "" : "")
      || (message.direction === "outbound" && message.greeting ? message.greeting : "");
    // A signature's "Engines Co" beats the domain's "Enginesco" when they agree.
    const squash = (value) => String(value || "").toLowerCase().replace(/[^a-z0-9]/g, "");
    const sameCompany = signature.company && domain.company && squash(signature.company).startsWith(squash(domain.company).slice(0, 4));
    message.contact = {
      company: (sameCompany ? signature.company : domain.company || signature.company) || "",
      title: signature.title || "",
      phone: signature.phone || "",
      website: signature.website || domain.website || "",
      linkedin: signature.linkedin || "",
      mastodon: handle ? `@${handle}` : signature.mastodon || "",
      twitter: signature.twitter || "",
      github: signature.github || "",
    };
    return message;
  }

  // CRM stamps are milliseconds; a seconds value from an import reads right too.
  const millis = (value) => {
    const number = Number(value) || 0;
    return number > 0 && number < 1e12 ? number * 1000 : number;
  };

  // The newest email on a record: its "last contacted" stamp, any linked
  // email, or the email or conversation on screen, whichever is later. The one
  // being read is contact with them even before it is linked, so a record
  // imported without a stamp does not say "None" over a message from today.
  function lastEmailAt(contact, emails = [], message = null) {
    return Math.max(0, millis(contact?.lastContactedAt),
      ...emails.map((email) => millis(email.sentAt || email.createdAt)),
      Math.min(millis(message?.sentAt), Date.now()));
  }

  // "Sep 13 · yesterday"; the year only when it is not this one.
  function shortDate(stamp, now = new Date()) {
    const date = new Date(stamp);
    if (!stamp || Number.isNaN(date.getTime())) return "";
    const label = date.toLocaleDateString([], {
      month: "short", day: "numeric",
      ...(date.getFullYear() === now.getFullYear() ? {} : { year: "numeric" }),
    });
    const day = (value) => new Date(value.getFullYear(), value.getMonth(), value.getDate()).getTime();
    const days = Math.round((day(now) - day(date)) / 86400000);
    const ago = days === 0 ? "today" : days === 1 ? "yesterday" : days > 1 && days < 60 ? `${days}d ago` : "";
    return ago ? `${label} · ${ago}` : label;
  }

  // A stamp as a date input reads it, and back: the day itself in the
  // reader's own timezone, so the date typed in is the date shown.
  function dateValue(stamp) {
    const date = new Date(Number(stamp) || 0);
    if (!stamp || Number.isNaN(date.getTime())) return "";
    return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
  }

  function dateStamp(value) {
    const parts = String(value || "").match(/^(\d{4})-(\d{2})-(\d{2})$/);
    return parts ? new Date(Number(parts[1]), Number(parts[2]) - 1, Number(parts[3]), 12).getTime() : 0;
  }

  // Colors are variables so the page header can follow the mail client's
  // theme (data-theme on the host) rather than the system's.
  const STYLE = `
    :host { all: initial; --fm-bg: #fff; --fm-fg: #111; --fm-muted: #5f6b7a; --fm-line: #e3e6ea; --fm-field: #fff; --fm-field-line: #d7dbe0; --fm-soft: #f1f3f5; --fm-accent: #2f81f7; --fm-chip: #e7f0fe; --fm-chip-fg: #1a5bbf; --fm-ok: #1a7f37; --fm-ok-soft: #e6f4ea; --fm-danger: #d1242f; }
    :host([data-theme="dark"]) { --fm-bg: #14171c; --fm-fg: #e8eaed; --fm-muted: #9aa3ad; --fm-line: #2c3239; --fm-field: #0d1014; --fm-field-line: #2c3239; --fm-soft: #1d2229; --fm-chip: #17263b; --fm-chip-fg: #9cc2ff; --fm-ok: #56d364; --fm-ok-soft: #13291a; --fm-danger: #ff7b72; }
    * { box-sizing: border-box; font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
    [hidden] { display: none !important; }
    .panel { border-radius: 10px; background: var(--fm-bg); color: var(--fm-fg); border: 1px solid var(--fm-line); box-shadow: 0 6px 24px rgba(0,0,0,.14); font-size: 13px; }
    .panel.embedded { border: 0; border-radius: 0; box-shadow: none; }
    .head { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 8px 12px; border-bottom: 1px solid var(--fm-line); }
    .head strong { font-size: 13px; }
    .close { border: 0; background: transparent; color: inherit; cursor: pointer; font-size: 16px; line-height: 1; padding: 2px 6px; }
    .body { padding: 10px 12px; display: grid; gap: 8px; }
    .embedded .body { padding: 0; }
    .muted { color: var(--fm-muted); font-size: 12px; }
    .row { border: 1px solid var(--fm-line); border-radius: 8px; padding: 8px; display: grid; gap: 6px; }
    label { display: grid; gap: 3px; font-size: 11px; color: var(--fm-muted); }
    input, select { width: 100%; border: 1px solid var(--fm-field-line); border-radius: 6px; padding: 6px 8px; font-size: 12px; background: var(--fm-field); color: var(--fm-fg); }
    .subject { font-weight: 600; font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 10px; word-break: break-all; }
    .actions { display: flex; flex-wrap: wrap; gap: 6px; }
    .embedded .actions.footer { position: sticky; bottom: 0; margin: 0 -12px; padding: 8px 12px; background: var(--fm-bg); border-top: 1px solid var(--fm-line); }
    button.primary, button.secondary, button.danger { border-radius: 6px; padding: 7px 10px; font-size: 12px; font-weight: 600; cursor: pointer; border: 1px solid transparent; }
    button.primary { background: var(--fm-accent); color: #fff; }
    .dated { display: flex; align-items: flex-end; gap: 6px; }
    .dated label { flex: 1; }
    button.secondary { background: var(--fm-soft); color: var(--fm-fg); border-color: var(--fm-field-line); }
    button.danger { background: transparent; color: var(--fm-danger); border-color: var(--fm-danger); }
    button[disabled] { opacity: .5; cursor: not-allowed; }
    .split { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
    .rowhead { font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: .04em; color: var(--fm-muted); }
    details summary { cursor: pointer; font-size: 11px; color: var(--fm-muted); }
    details[open] summary { margin-bottom: 6px; }
    details .split { margin-bottom: 4px; }
    .ok { color: var(--fm-ok); font-size: 12px; margin: 0; }
    .error { color: var(--fm-danger); font-size: 12px; margin: 0; white-space: pre-wrap; }
    a { color: var(--fm-accent); }
    button.link { border: 0; background: none; color: var(--fm-accent); padding: 0; font-size: 12px; cursor: pointer; text-align: left; }
    button.link:hover { text-decoration: underline; }
    .picks { display: grid; gap: 4px; max-height: 220px; overflow: auto; }
    button.pick { display: grid; gap: 1px; width: 100%; text-align: left; border: 1px solid var(--fm-line); border-radius: 6px; padding: 5px 8px; background: var(--fm-field); color: var(--fm-fg); font-size: 12px; cursor: pointer; }
    button.pick:hover, button.pick:focus { border-color: var(--fm-accent); outline: none; }
  `;

  // A shadow root keeps the mail client's CSS and ours apart.
  function mount(container, extraStyle = "") {
    const root = container.attachShadow({ mode: "open" });
    const css = STYLE + extraStyle;
    try {
      const sheet = new CSSStyleSheet();
      sheet.replaceSync(css);
      root.adoptedStyleSheets = [sheet];
    } catch {
      const style = document.createElement("style");
      style.textContent = css;
      root.appendChild(style);
    }
    return root;
  }

  // `read` returns the message on screen (or throws why it cannot);
  // `onChange` runs after every render so a header can mirror the state.
  function create({ element, read, send: request = send, embedded = false, onChange = () => {}, onLinked = () => {} }) {
    element.classList.add("panel");
    if (embedded) element.classList.add("embedded");
    const ui = {
      open: embedded, busy: false, looking: false, status: null, message: null, contact: null,
      emails: [], flows: [], flow: "", stage: "", detailsOpen: false,
      error: "", notice: "",
      directory: null, choosing: false, query: "", picked: null,
      // How the CRM found the contact's record when it was not by this very
      // address or handle: "name", "profile", or "phone".
      matchedBy: "",
      // The message on screen has not been looked up in the CRM. Moving from
      // one email or toot to the next never asks ForkMesh anything: the
      // record arrives when "Look up" is pressed.
      stale: false,
    };
    // Every load and write bumps this; an answer meant for an older one is
    // dropped, so a fast click through the inbox never shows a stale contact.
    let generation = 0;

    const PROVIDER_NAMES = { gmail: "Gmail", zoho: "Zoho Mail", mastodon: "Mastodon" };
    const LOOSE_MATCHES = { name: "their name", profile: "a profile link", phone: "their phone number" };

    function providerLabel() {
      return PROVIDER_NAMES[ui.message?.provider] || "Gmail, Zoho Mail or Mastodon";
    }

    // A Mastodon conversation is tracked like an email, but its people are
    // handles: the contact's address may never be known.
    const isThread = () => ui.message?.provider === "mastodon";

    // What the contact is looked up and linked by.
    function identity() {
      return ui.message?.contactEmail || ui.message?.handle || "";
    }

    // The person on the message, keyed the way the picker lists them.
    function participantKey(person) {
      return String(person?.email || person?.handle || "");
    }

    function linkedEmail() {
      const { messageId, provider } = ui.message || {};
      return messageId ? ui.emails.find((email) => email.externalId === messageId && email.provider === provider) || null : null;
    }

    // The pipeline and stage the selects show: what was picked, else the
    // contact's own, else the first. A pick sticks across emails of new
    // contacts so a batch of outreach lands in one stage.
    function pipelineChoice() {
      const flows = ui.flows || [];
      const flow = flows.find((item) => item.id === ui.flow) || flows[0];
      if (!flow) return null;
      const stages = flow.stages || [];
      return { flow, stage: stages.includes(ui.stage) ? ui.stage : stages[0] || "" };
    }

    function followContact(contact) {
      if (!contact) return;
      ui.flow = contact.flow || ui.flow;
      ui.stage = contact.stage || ui.stage;
    }

    function contactLabel(contact) {
      return contact?.name || contact?.email || (contact?.mastodon ? contact.mastodon : "Unnamed");
    }

    function kinship(contact) {
      const email = ui.message?.contactEmail || "";
      const squash = (value) => String(value || "").toLowerCase().replace(/[^a-z0-9]/g, "");
      const base = (value) => String(value || "").split("@")[0].replace(/\+.*$/, "").toLowerCase();
      const site = domainDetails(email).website;
      let score = 0;
      if (site && site === domainDetails(contact.email).website) score += 2;
      if (email && contact.email && base(email) === base(contact.email)) score += 1;
      const company = squash(ui.message?.contact?.company);
      if (company && company === squash(contact.company)) score += 1;
      const name = squash(ui.message?.contactName);
      if (name && name === squash(contact.name)) score += 2;
      return score;
    }

    function matches() {
      const list = ui.directory || [];
      const words = ui.query.toLowerCase().split(/\s+/).filter(Boolean);
      if (!words.length) {
        return list.map((contact) => [kinship(contact), contact]).filter(([score]) => score > 0)
          .sort((one, two) => two[0] - one[0] || contactLabel(one[1]).localeCompare(contactLabel(two[1])))
          .slice(0, 8).map(([, contact]) => contact);
      }
      return list.filter((contact) => {
        const text = [contact.name, contact.email, contact.company, contact.mastodon].join(" ").toLowerCase();
        return words.every((word) => text.includes(word));
      }).slice(0, 8);
    }

    function matchesHtml() {
      if (!ui.directory) return `<div class="muted">Reading the CRM's contacts…</div>`;
      const found = matches();
      if (!found.length) {
        return `<div class="muted">${ui.query.trim() ? `No contact matches "${esc(ui.query.trim())}".` : "Type a name, address or company."}</div>`;
      }
      return found.map((contact) => `<button class="pick" data-pick="${esc(contact.id)}" type="button"><strong>${esc(contactLabel(contact))}</strong><span class="muted">${esc([contact.name ? contact.email : "", contact.company, contact.flow ? `${flowName(contact.flow)} · ${titleCase(contact.stage || "lead")}` : ""].filter(Boolean).join(" · "))}</span></button>`).join("");
    }

    function chooserHtml(contact) {
      if (!ui.choosing) {
        return `<div><button class="link" data-act="choose" type="button"${ui.busy ? " disabled" : ""}>${contact ? "Link to another contact instead…" : "Link to an existing contact…"}</button></div>`;
      }
      return `<label>Link to an existing contact<input data-find type="search" value="${esc(ui.query)}" placeholder="Search by name, email or company" autocomplete="off" /></label>
        <div class="picks" data-matches>${matchesHtml()}</div>
        <div><button class="link" data-act="unchoose" type="button">Cancel</button></div>`;
    }

    function pickedHtml() {
      const picked = ui.picked;
      const adds = !isThread() && ui.message?.contactEmail && (!ui.contact || LOOSE_MATCHES[ui.matchedBy]);
      return `<div>Linking to <strong>${esc(contactLabel(picked))}</strong>${picked.company ? ` <span class="muted">· ${esc(picked.company)}</span>` : ""}</div>
        ${picked.name && picked.email ? `<div class="muted">${esc(picked.email)}</div>` : ""}
        ${adds ? `<div class="muted">Linking adds ${esc(ui.message.contactEmail)} to their record, so the next email from it finds them.</div>` : ""}
        <div class="actions"><button class="link" data-act="choose" type="button">Pick another</button><button class="link" data-act="unpick" type="button">Keep ${ui.contact ? esc(contactLabel(ui.contact)) : "a new contact"}</button></div>`;
    }

    function flowName(id) {
      return (ui.flows || []).find((flow) => flow.id === id)?.name || titleCase(id);
    }

    function paint() {
      if (!ui.open) { element.hidden = true; return; }
      element.hidden = false;
      const status = ui.status;
      const message = ui.message;
      const linked = linkedEmail();
      let body;
      if (!status) {
        body = `<p class="muted">Checking the extension…</p>`;
      } else if (!status.connected) {
        body = `<p class="muted">Sign in first: click the ForkMesh CRM icon in the Chrome toolbar and enter your ForkMesh account.</p>`;
      } else if (!status.organization) {
        body = `<p class="muted">Signed in as <strong>${esc(status.account)}</strong>. Pick an organization with the CRM installed from the toolbar popup.</p>`;
      } else if (!message || !message.messageId) {
        body = `<p class="muted">Open an email in Gmail or Zoho Mail, or a conversation in Mastodon, and this panel will read it.</p>
          <div class="actions"><button class="secondary" data-act="refresh" type="button">Read again</button></div>`;
      } else {
        const choice = pipelineChoice();
        const contact = ui.contact;
        const target = ui.picked || contact;
        const sameFlow = Boolean(target && choice && target.flow === choice.flow.id);
        const when = Number.isFinite(message.sentAt) && message.sentAt > 0
          ? ` · ${esc(new Date(message.sentAt).toLocaleString([], { dateStyle: "medium", timeStyle: "short" }))}` : "";
        const details = message.contact || {};
        const found = DETAIL_FIELDS.filter(([key]) => details[key]).length;
        const pipelineLabel = !choice ? "Add to pipeline"
          : sameFlow ? (target.stage === choice.stage ? "Save to pipeline" : `Move to ${titleCase(choice.stage)}`)
          : "Add to pipeline";
        const last = contact ? lastEmailAt(contact, ui.emails, ui.message) : 0;
        body = `
          ${embedded ? "" : `<p class="muted">Signed in as <strong>${esc(status.account)}</strong> · CRM of <strong>${esc(status.organization)}</strong></p>`}
          <div class="row">
            ${isThread() ? `<div class="subject" title="${esc(message.subject)}">${esc(message.subject || "No subject")}</div>`
              : `<label>Subject<input class="subject" data-field="subject" value="${esc(message.subject)}" placeholder="No subject" /></label>`}
            <div class="muted">${esc(providerLabel())} · ${message.direction === "outbound" ? (isThread() ? "posted by you" : "sent by you") : (isThread() ? "posted by the contact" : "received")}${message.mailbox || message.account ? ` · ${esc(message.mailbox || `@${message.account}`)}` : ""}${when}</div>
            <div class="mono muted">id ${esc(message.messageId)}</div>
            ${(message.participants || []).length > 1 ? `<label>Contact on this ${isThread() ? "conversation" : "email"}<select data-participant>${message.participants.map((person) => {
              const key = participantKey(person);
              const shown = person.handle ? `@${person.handle}` : person.email;
              return `<option value="${esc(key)}"${key === identity() ? " selected" : ""}>${esc(person.name ? `${person.name} <${shown}>` : shown)}</option>`;
            }).join("")}</select></label>` : ""}
            ${isThread() || message.handle ? `<label>Mastodon handle<input data-field="handle" value="${esc(message.handle ? `@${message.handle}` : "")}" placeholder="@name@instance" /></label>` : ""}
            <label>Contact email${isThread() ? " (optional)" : ""}<input data-field="contactEmail" type="email" value="${esc(message.contactEmail)}" placeholder="${isThread() ? "if you know it" : "who the email is with"}" /></label>
            <label>Contact name<input data-field="contactName" value="${esc(message.contactName)}" placeholder="used when the contact is new" /></label>
            <label>Direction<select data-field="direction"><option value="inbound"${message.direction === "inbound" ? " selected" : ""}>${isThread() ? "Posted by the contact" : "Received from the contact"}</option><option value="outbound"${message.direction === "outbound" ? " selected" : ""}>${isThread() ? "Posted by you" : "Sent to the contact"}</option></select></label>
            <details data-details${ui.detailsOpen ? " open" : ""}>
              <summary>Contact details${found ? ` (${found} found on the ${isThread() ? "conversation" : "email"})` : ""}</summary>
              <div class="split">${DETAIL_FIELDS.map(([key, label]) =>
                `<label>${label}<input data-detail="${key}" value="${esc(details[key] || "")}" /></label>`).join("")}</div>
              <div class="muted">On a contact the CRM already has, only empty fields are filled.</div>
            </details>
          </div>
          <div class="row">
            ${ui.stale
              ? `<div class="muted">Not looked up yet: the CRM is only read when you ask.</div>
                 <div class="actions"><button class="secondary" data-act="lookup" type="button"${ui.busy || !identity() ? " disabled" : ""}>Look up ${esc(identity() || "this contact")}</button></div>`
              : ui.looking ? `<div class="muted">Looking up ${esc(message.contactEmail)}…</div>`
              : ui.picked ? pickedHtml()
              : contact
              ? `<div><strong>${esc(contact.name || contact.email)}</strong> <span class="muted">· ${esc(flowName(contact.flow))} · ${esc(titleCase(contact.stage || "lead"))}${contact.company ? ` · ${esc(contact.company)}` : ""}</span></div>
                 <div class="muted">Owner ${esc(contact.owner || "unassigned")}</div>
                 ${LOOSE_MATCHES[ui.matchedBy] ? `<div class="muted">Found by ${LOOSE_MATCHES[ui.matchedBy]}.${isThread() ? "" : " Linking adds this address to the record."}</div>` : ""}
                 <div class="muted">${ui.emails.length} linked ${ui.emails.length === 1 ? "message" : "messages"} on this record${linked ? " · this one is linked" : ""}</div>
                 <div class="dated"><label>Last contacted<input data-contact="lastContactedAt" type="date" value="${esc(dateValue(contact.lastContactedAt))}" /></label><button class="secondary" data-act="contacted-now" type="button" title="Record now as the last time you were in touch"${ui.busy ? " disabled" : ""}>Now</button></div>`
              : `<div class="muted">${identity() ? `No contact for ${esc(identity())} yet; linking creates one.` : `Enter the contact's ${isThread() ? "handle" : "email address"}.`}</div>`}
            ${ui.stale || ui.looking || ui.picked ? "" : chooserHtml(contact)}
          </div>
          ${choice ? `<div class="row">
            <div class="rowhead">Pipeline</div>
            <div class="split">
              <label>Pipeline<select data-pipeline="flow">${ui.flows.map((flow) =>
                `<option value="${esc(flow.id)}"${flow.id === choice.flow.id ? " selected" : ""}>${esc(flow.name)}</option>`).join("")}</select></label>
              <label>Stage<select data-pipeline="stage">${(choice.flow.stages || []).map((stage) =>
                `<option value="${esc(stage)}"${stage === choice.stage ? " selected" : ""}>${esc(titleCase(stage))}</option>`).join("")}</select></label>
            </div>
          </div>` : ""}
          <div class="actions footer">
            <button class="primary" data-act="pipeline" type="button"${ui.busy || !identity() || !choice ? " disabled" : ""}>${esc(pipelineLabel)}</button>
            <button class="secondary" data-act="link" type="button"${ui.busy || !identity() ? " disabled" : ""}>${ui.picked ? `Link to ${esc(String(ui.picked.name || "contact").split(/\s+/)[0])}` : linked ? "Update link" : "Link only"}</button>
            ${linked ? `<button class="danger" data-act="unlink" type="button"${ui.busy ? " disabled" : ""}>Unlink</button>` : ""}
            ${embedded ? "" : `<button class="secondary" data-act="open" type="button">Open CRM</button>`}
            <button class="secondary" data-act="refresh" type="button"${ui.busy ? " disabled" : ""}>Read again</button>
          </div>`;
      }
      element.innerHTML = `
        ${embedded ? "" : `<div class="head"><strong>ForkMesh CRM</strong><button class="close" data-act="close" type="button" aria-label="Close">×</button></div>`}
        <div class="body">
          ${body}
          ${ui.notice ? `<p class="ok">${esc(ui.notice)}</p>` : ""}
          ${ui.error ? `<p class="error">${esc(ui.error)}</p>` : ""}
        </div>`;
    }

    function render() {
      paint();
      onChange(ui);
    }

    async function lookupContact(mine = ++generation) {
      ui.contact = null; ui.emails = []; ui.stale = false; ui.matchedBy = "";
      ui.picked = null; ui.choosing = false; ui.query = "";
      ui.looking = Boolean(ui.status?.connected && ui.status?.organization && identity());
      render();
      if (!ui.looking) return;
      const found = await request({
        type: "fm-lookup", email: ui.message.contactEmail, handle: ui.message.handle,
        name: ui.message.contactName, contact: ui.message.contact,
      });
      if (mine !== generation) return;
      ui.looking = false;
      if (found?.ok) {
        ui.contact = found.contact; ui.emails = found.emails || [];
        ui.matchedBy = found.matchedBy || "";
        if (found.flows?.length) ui.flows = found.flows;
        followContact(found.contact);
      } else ui.error = found?.message || "Lookup failed";
      render();
    }

    // Show `message` (null when there is none) without asking ForkMesh about
    // it: the sign-in state is local to the extension, so this costs no
    // request at all. Used by the mail and Mastodon readers, which follow the
    // page from message to message on their own.
    async function present(message) {
      const mine = ++generation;
      const status = await request({ type: "fm-status" });
      if (mine !== generation) return;
      ui.error = ""; ui.notice = "";
      ui.status = status;
      ui.message = message || null;
      ui.contact = null; ui.emails = []; ui.looking = false; ui.matchedBy = "";
      ui.picked = null; ui.choosing = false; ui.query = "";
      ui.stale = Boolean(message?.messageId);
      render();
    }

    // Show `message` (null when there is none) and look its contact up.
    async function load(message) {
      const mine = ++generation;
      const status = await request({ type: "fm-status" });
      if (mine !== generation) return;
      ui.error = ""; ui.notice = "";
      ui.status = status;
      ui.message = message || null;
      if (status?.ok === false) {
        ui.contact = null; ui.emails = []; ui.looking = false;
        ui.picked = null; ui.choosing = false; ui.query = "";
        ui.error = status.message || "Extension unavailable";
        render();
        return;
      }
      await lookupContact(mine);
    }

    async function refresh() {
      let message = null;
      let failure = "";
      try { message = await read(); } catch (error) { failure = String(error?.message || error); }
      await load(message);
      if (failure && !ui.error) { ui.error = failure; render(); }
    }

    async function linkCurrent({ pipeline = false } = {}) {
      if (!identity()) return;
      const choice = pipeline ? pipelineChoice() : null;
      if (pipeline && !choice) return;
      const mine = ++generation;
      ui.busy = true; ui.error = ""; ui.notice = ""; render();
      const picked = ui.picked;
      const message = {
        ...ui.message,
        ...(picked ? { contactId: picked.id } : {}),
        ...(choice ? { flow: choice.flow.id, stage: choice.stage } : {}),
      };
      const result = await request({ type: "fm-link", message });
      ui.busy = false;
      if (mine !== generation) { render(); return; }
      if (result?.ok) {
        ui.contact = result.contact; ui.emails = result.emails || [];
        ui.stale = false; ui.matchedBy = "";
        ui.picked = null; ui.choosing = false; ui.query = "";
        if (result.contactCreated) ui.directory = null;
        if (result.flows?.length) ui.flows = result.flows;
        followContact(result.contact);
        const who = result.contact?.name || result.contact?.email || "the contact";
        ui.notice = picked && !choice ? `Linked to ${who}.`
          : choice
          ? `${result.contactCreated ? "Created" : "Moved"} ${who} in ${choice.flow.name} · ${titleCase(result.contact?.stage || choice.stage)}; email linked.`
          : result.contactCreated ? `Linked and created ${who} in the CRM.`
            : result.relinked ? "Link updated." : `Linked to ${who}.`;
        onLinked();
      } else {
        ui.error = result?.error === "not_signed_in"
          ? "Signed out. Sign in again from the toolbar icon."
          : `Could not ${choice ? "add to the pipeline" : "link"}: ${result?.message || "unknown error"}`;
        if (result?.error === "not_signed_in") ui.status = await request({ type: "fm-status" });
      }
      render();
    }

    // A date is a judgement call as often as it is a message: linking moves
    // it forward on its own, and this sets it to whenever you last spoke, a
    // picked day or (from the Now button) this minute.
    async function setLastContacted(value) {
      const contact = ui.contact;
      if (!contact?.id) return;
      const stamp = typeof value === "number" ? value : dateStamp(value);
      if (value && !stamp) return;
      const mine = ++generation;
      ui.busy = true; ui.error = ""; ui.notice = ""; render();
      const result = await request({
        type: "fm-contact-update", contactId: contact.id,
        patch: { lastContactedAt: stamp },
      });
      ui.busy = false;
      if (mine !== generation) { render(); return; }
      if (result?.ok && result.contact) {
        ui.contact = result.contact;
        ui.notice = stamp ? `Last contacted set to ${shortDate(stamp)}.` : "Last contacted cleared.";
        onLinked();
      } else ui.error = `Could not set the date: ${result?.message || "unknown error"}`;
      render();
    }

    async function choose() {
      if (!ui.message) return;
      ui.choosing = true; ui.picked = null; ui.error = "";
      render();
      if (!ui.directory) {
        const result = await request({ type: "fm-contacts" });
        if (result?.ok) ui.directory = result.contacts || [];
        if (!ui.choosing) return;
        if (!result?.ok) {
          ui.choosing = false;
          ui.error = `Could not read the contacts: ${result?.message || "unknown error"}`;
        }
        render();
      }
      element.querySelector("[data-find]")?.focus();
    }

    function pick(id) {
      const contact = (ui.directory || []).find((item) => item.id === id);
      if (!contact) return;
      ui.picked = contact; ui.choosing = false; ui.query = "";
      if (ui.message && !ui.message.contactName && contact.name) ui.message.contactName = contact.name;
      followContact(contact);
      render();
    }

    async function unlinkCurrent() {
      const linked = linkedEmail();
      if (!linked) return;
      const mine = ++generation;
      ui.busy = true; ui.error = ""; ui.notice = ""; render();
      const result = await request({ type: "fm-unlink", emailId: linked.id });
      ui.busy = false;
      if (mine !== generation) { render(); return; }
      if (result?.ok) {
        ui.emails = ui.emails.filter((email) => email.id !== linked.id);
        ui.notice = "Unlinked from the contact.";
        onLinked();
      } else ui.error = `Could not unlink: ${result?.message || "unknown error"}`;
      render();
    }

    element.addEventListener("click", (event) => {
      const choice = event.target.closest("[data-pick]");
      if (choice) { pick(choice.dataset.pick); return; }
      const button = event.target.closest("[data-act]");
      if (!button) return;
      const act = button.dataset.act;
      if (act === "close") { ui.open = false; render(); }
      else if (act === "refresh") void refresh();
      else if (act === "lookup") void lookupContact();
      else if (act === "pipeline") void linkCurrent({ pipeline: true });
      else if (act === "link") void linkCurrent();
      else if (act === "unlink") void unlinkCurrent();
      else if (act === "choose") void choose();
      else if (act === "unchoose") { ui.choosing = false; ui.query = ""; render(); }
      else if (act === "unpick") { ui.picked = null; followContact(ui.contact); render(); }
      else if (act === "contacted-now") void setLastContacted(Date.now());
      else if (act === "open") void request({ type: "fm-open-crm" });
    });
    element.addEventListener("change", (event) => {
      if (!ui.message) return;
      const picked = event.target.closest("[data-participant]");
      if (picked) {
        const person = ui.message.participants.find((item) => participantKey(item) === picked.value);
        applyContact(ui.message, person || { email: picked.value, name: "" });
        void lookupContact();
        return;
      }
      const dated = event.target.closest('[data-contact="lastContactedAt"]');
      if (dated) { void setLastContacted(dated.value); return; }
      const pipeline = event.target.closest("[data-pipeline]");
      if (pipeline) {
        if (pipeline.dataset.pipeline === "flow") { ui.flow = pipeline.value; ui.stage = ""; }
        else ui.stage = pipeline.value;
        render();
        return;
      }
      const field = event.target.closest("[data-field]");
      if (!field) return;
      // Either half of the contact's identity can be typed in; the other one
      // stays put and the record is looked up again.
      if (field.dataset.field === "contactEmail" || field.dataset.field === "handle") {
        applyContact(ui.message, {
          email: field.dataset.field === "contactEmail" ? field.value.trim() : ui.message.contactEmail,
          handle: field.dataset.field === "handle" ? field.value.trim() : ui.message.handle,
          name: ui.message.contactName,
        });
        void lookupContact();
      } else ui.message[field.dataset.field] = field.value.trim();
      if (field.dataset.field === "direction") render();
    });
    element.addEventListener("input", (event) => {
      if (!ui.message) return;
      const find = event.target.closest("[data-find]");
      if (find) {
        ui.query = find.value;
        const list = element.querySelector("[data-matches]");
        if (list) list.innerHTML = matchesHtml();
        return;
      }
      const detail = event.target.closest("[data-detail]");
      if (detail) { ui.message.contact = { ...ui.message.contact, [detail.dataset.detail]: detail.value }; return; }
      const field = event.target.closest("[data-field]");
      if (field && !["contactEmail", "handle"].includes(field.dataset.field)) {
        ui.message[field.dataset.field] = field.value;
      }
    });
    element.addEventListener("keydown", (event) => {
      if (!event.target.closest?.("[data-find]")) return;
      if (event.key === "Enter") {
        event.preventDefault();
        const first = matches()[0];
        if (first) pick(first.id);
      } else if (event.key === "Escape") {
        event.preventDefault();
        ui.choosing = false; ui.query = ""; render();
      }
    });
    // <details> toggles do not bubble; keep the section open across renders.
    element.addEventListener("toggle", (event) => {
      if (event.target.matches?.("[data-details]")) ui.detailsOpen = event.target.open;
    }, true);

    return {
      ui, render, load, present, lookup: lookupContact, refresh, linkedEmail,
      flowName, isThread, choose,
    };
  }

  // --- the compact CRM header above the open message -----------------------
  // One strip of chips carrying the record: who the contact is, the pipeline
  // they are in, and when they were last heard from, with the FM button that
  // opens the panel underneath it. content.js and mastodon.js each read their
  // own page and say where the strip goes.

  const HEADER_STYLE = `
    .strip { display: flex; flex-wrap: wrap; align-items: center; gap: 4px 6px; margin: 2px 0 10px; padding: 4px 6px; border: 1px solid var(--fm-line); border-radius: 8px; background: var(--fm-bg); color: var(--fm-fg); font-size: 12px; line-height: 18px; }
    .fm { border: 0; border-radius: 5px; padding: 1px 7px; background: var(--fm-accent); color: #fff; font-size: 11px; font-weight: 700; line-height: 18px; cursor: pointer; }
    .fm[aria-expanded="true"] { box-shadow: inset 0 0 0 2px rgba(0,0,0,.25); }
    .who { border: 0; padding: 0; background: none; color: inherit; font-size: 12px; font-weight: 600; line-height: 18px; cursor: pointer; max-width: 240px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .who:hover { text-decoration: underline; }
    .chip { display: inline-flex; align-items: baseline; gap: 4px; max-width: 280px; padding: 0 7px; border-radius: 999px; background: var(--fm-soft); white-space: nowrap; }
    .chip b { font-size: 10px; font-weight: 600; letter-spacing: .04em; text-transform: uppercase; color: var(--fm-muted); }
    .chip span { overflow: hidden; text-overflow: ellipsis; }
    .chip.stage { background: var(--fm-chip); color: var(--fm-chip-fg); }
    .chip.linked { background: var(--fm-ok-soft); color: var(--fm-ok); }
    .note { color: var(--fm-muted); }
    .add { border: 1px solid var(--fm-field-line); border-radius: 5px; padding: 0 7px; background: var(--fm-soft); color: inherit; font-size: 11px; font-weight: 600; line-height: 18px; cursor: pointer; }
    .panel { margin: -4px 0 10px; max-width: 560px; }
  `;

  // Record fields worth a chip when the contact has them.
  const EXTRA_CHIPS = [
    ["company", "Company"], ["title", "Title"], ["value", "Value"], ["nextStep", "Next"],
    ["source", "Source"], ["location", "Location"], ["tags", "Tags"],
  ];

  function stripHtml(ui, panel) {
    const thread = panel.isThread();
    const what = thread ? "conversation" : "email";
    const toggle = `<button class="fm" data-strip="toggle" type="button" aria-expanded="${ui.open}" title="ForkMesh CRM: link this ${what} or change the contact's pipeline">FM</button>`;
    const note = (value) => `<span class="note">${esc(value)}</span>`;
    const status = ui.status;
    const message = ui.message;
    if (!status) return toggle + note("ForkMesh CRM…");
    if (status.ok === false) return toggle + note(status.message || "ForkMesh CRM is unavailable");
    if (!status.connected) return toggle + note("Not signed in to ForkMesh CRM: click its toolbar icon");
    if (!status.organization) return toggle + note("Pick an organization from the ForkMesh CRM toolbar icon");
    const identity = message?.contactEmail || message?.handle || "";
    if (!identity) return toggle + note(`No contact ${thread ? "handle" : "address"} found on this ${what}`);
    const contact = ui.contact;
    const shown = contact?.email || identity;
    const who = `<button class="who" data-strip="open-crm" type="button" title="${esc(shown)} · open the CRM">${esc(contact?.name || message.contactName || shown)}</button>`;
    if (ui.stale) {
      return toggle + who + note("Not looked up")
        + `<button class="add" data-strip="lookup" type="button" title="Read this contact's CRM record now">Look up</button>`;
    }
    if (ui.looking) return toggle + who + note("Looking up…");
    if (!contact) {
      return toggle + who + note(ui.error || "Not in the CRM yet")
        + `<button class="add" data-strip="toggle" type="button">Add to pipeline</button>`
        + `<button class="add" data-strip="choose" type="button" title="File this ${what} under a contact the CRM already has">Link to contact</button>`;
    }
    const chip = (label, value, kind = "", title = "") =>
      `<span class="chip${kind ? ` ${kind}` : ""}" title="${esc(title || `${label}: ${value}`)}"><b>${esc(label)}</b><span>${esc(value)}</span></span>`;
    const last = lastEmailAt(contact, ui.emails, ui.message);
    const linked = panel.linkedEmail();
    const parts = [
      toggle, who,
      chip("Pipeline", panel.flowName(contact.flow)),
      chip("Stage", titleCase(contact.stage || "lead"), "stage"),
      chip("Owner", contact.owner || "Unassigned"),
      chip(thread ? "Last contact" : "Last email", last ? shortDate(last) : "None", "",
        last ? `Last message: ${new Date(last).toLocaleString([], { dateStyle: "full", timeStyle: "short" })}` : "Nothing on the record yet"),
      chip("Linked", `${ui.emails.length}${linked ? " · this one" : ""}`, linked ? "linked" : "",
        linked ? `This ${what} is linked to the contact` : "Emails and conversations linked to the contact"),
    ];
    if (thread && contact.mastodon) parts.push(chip("Mastodon", contact.mastodon));
    for (const [key, label] of EXTRA_CHIPS) {
      const raw = contact[key];
      const value = key === "value" ? (raw > 0 ? Number(raw).toLocaleString() : "")
        : Array.isArray(raw) ? raw.join(", ") : String(raw || "");
      if (value) parts.push(chip(label, value));
    }
    return parts.join("");
  }

  // The header follows the page's own theme, which need not be the system's.
  function pageTheme(node) {
    for (let item = node; item && item.nodeType === 1; item = item.parentElement) {
      const color = getComputedStyle(item).backgroundColor.match(/rgba?\(([\d.]+),\s*([\d.]+),\s*([\d.]+)(?:,\s*([\d.]+))?/);
      if (!color || (color[4] !== undefined && Number(color[4]) === 0)) continue;
      const [red, green, blue] = color.slice(1, 4).map(Number);
      return 0.2126 * red + 0.7152 * green + 0.0722 * blue < 128 ? "dark" : "light";
    }
    return matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }

  // `read` returns the message on screen. `place(anchor)` puts the header
  // right above that node, wherever the page has moved it to.
  function header({ read }) {
    const host = document.createElement("div");
    host.id = "forkmesh-crm-host";
    const root = mount(host, HEADER_STYLE);
    const strip = document.createElement("div");
    strip.className = "strip";
    const element = document.createElement("div");
    root.append(strip, element);
    const panel = create({
      element, read,
      onChange: (ui) => { strip.innerHTML = stripHtml(ui, panel); },
    });
    strip.addEventListener("click", (event) => {
      const button = event.target.closest("[data-strip]");
      if (!button) return;
      const act = button.dataset.strip;
      if (act === "open-crm") void send({ type: "fm-open-crm" });
      else if (act === "lookup") void panel.lookup();
      else if (act === "choose") { panel.ui.open = true; void panel.choose(); }
      else { panel.ui.open = !panel.ui.open; panel.render(); }
    });
    function place(anchor) {
      if (anchor.previousSibling === host) return;
      host.dataset.theme = pageTheme(anchor.parentNode);
      anchor.parentNode.insertBefore(host, anchor);
    }
    return { host, panel, place };
  }

  globalThis.ForkMeshCrm = {
    FREE_MAIL, address, handleOf, domainOf, titleCase, esc, send, realName,
    domainDetails, applyContact, lastEmailAt, shortDate, mount, create, header,
  };
})();
