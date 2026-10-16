// ForkMesh CRM inside Gmail and Zoho Mail.
//
// Reads the message on screen (ids, subject, participants, date) plus the
// contact details an email gives away: the name in the greeting, a company
// domain, and a signature's title, phone, and profile links. Above the open
// email it places panel.js's compact CRM header, whose "Look up" button reads
// the contact's record (pipeline, stage, owner, last email, and the rest) and
// whose FM button opens the panel to link the email or move the contact
// through a pipeline. Opening an email asks ForkMesh for nothing by itself.
// Everything that talks to ForkMesh goes through background.js; this script
// never sees the session token, and the message body is only read here to
// prefill editable fields: it is never sent anywhere.
(() => {
  if (window.__forkmeshCrmPanel) return;
  window.__forkmeshCrmPanel = true;

  const crm = globalThis.ForkMeshCrm;
  if (!crm) return;
  const { FREE_MAIL, address, domainOf, realName, applyContact } = crm;
  const PROVIDER = location.hostname === "mail.google.com" ? "gmail" : "zoho";

  const text = (node) => String(node?.textContent || "").replace(/\s+/g, " ").trim();
  const attr = (node, name) => String(node?.getAttribute?.(name) || "").trim();
  const visible = (node) => Boolean(node && node.offsetParent !== null);

  // --- what an email says about its people --------------------------------

  // "Ada Lovelace" <ada@engines.example>, Bob <bob@engines.example>, or a bare
  // address, in the order they appear.
  function addressList(value) {
    const result = [];
    const re = /(?:"([^"]{0,120})"|([^,;<>"]{1,120}))?\s*<\s*([^<>\s]+@[^<>\s]+)\s*>|([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})/g;
    for (const match of String(value || "").matchAll(re)) {
      const email = address(match[3] || match[4]);
      if (!email || result.some((item) => item.email === email)) continue;
      const name = String(match[1] || match[2] || "").replace(/^\s*(?:to|from|cc):\s*/i, "").trim();
      result.push({ email, name });
    }
    return result;
  }

  // The newest message only: quoted replies and forwards below it belong to
  // someone else.
  function ownLines(body) {
    const lines = String(body || "").replace(/\r/g, "").split("\n").map((line) => line.trim());
    const cut = lines.findIndex((line) => /^(>|On .{4,200} wrote:$|-{2,} ?(Original|Forwarded) Message|From: .+@|Sent from my )/i.test(line));
    return (cut >= 0 ? lines.slice(0, cut) : lines).filter(Boolean)
      .filter((line) => !/^(reply|reply all|forward|edit as new)(\s*[•·|]\s*(reply|reply all|forward|edit as new))*$/i.test(line));
  }

  const NOT_NAMES = new Set(["all", "team", "there", "everyone", "folks", "guys", "friends", "both", "again", "sir", "madam", "y'all"]);
  const NAME_RE = /^\p{Lu}[\p{L}'’.-]{0,30}(?:\s+\p{Lu}[\p{L}'’.-]{0,30}){0,2}$/u;

  // "Hi Gracie," names whoever the message was written to.
  function greetingName(lines) {
    for (const line of lines.slice(0, 12)) {
      const match = line.match(/^(?:hi|hello|hey|dear|greetings|good (?:morning|afternoon|evening))[\s,]+(.+?)\s*[,!.:]?$/iu);
      if (!match) continue;
      const name = match[1].replace(/^(?:mr|mrs|ms|dr|prof)\.?\s+/i, "").trim();
      if (NAME_RE.test(name) && !NOT_NAMES.has(name.toLowerCase())) return name;
      return "";
    }
    return "";
  }

  const SIGN_OFF_RE = /^(thanks|thank you|many thanks|thx|best|best regards|all the best|regards|kind regards|warm regards|warmly|cheers|sincerely|talk soon|yours|take care)\b[\p{L}\s]{0,20}[,.!]?$/iu;

  // Who wrote the message, from its closing lines: the name under the
  // sign-off, the title and company under that, and any phone or profile
  // links. Only meaningful for mail the contact wrote.
  function signatureDetails(lines) {
    const tail = lines.slice(-14);
    const joined = tail.join("\n");
    const found = {};
    const linkedin = joined.match(/(?:[a-z]{2,3}\.)?linkedin\.com\/(?:in|company)\/[A-Za-z0-9_%-]+/i);
    if (linkedin) found.linkedin = linkedin[0].replace(/^[a-z]{2,3}\.(?=linkedin)/i, "");
    const twitter = joined.match(/(?:^|[\s/])(?:twitter|x)\.com\/([A-Za-z0-9_]{1,15})\b/i);
    if (twitter && !["home", "intent", "share", "search"].includes(twitter[1].toLowerCase())) found.twitter = "@" + twitter[1];
    const github = joined.match(/github\.com\/([A-Za-z0-9-]{1,39})\b/i);
    if (github && !["orgs", "sponsors", "features", "about"].includes(github[1].toLowerCase())) found.github = github[1];
    for (const line of tail) {
      const phone = line.match(/\+?\d[\d\s().-]{7,}\d/);
      const digits = phone ? phone[0].replace(/\D/g, "").length : 0;
      if (phone && digits >= 9 && digits <= 15) { found.phone = phone[0].trim(); break; }
    }
    const website = [...joined.matchAll(/\b(?:https?:\/\/|www\.)[^\s<>()"',]+/gi)].map((match) => match[0])
      .find((url) => !/linkedin\.com|twitter\.com|\/\/x\.com|github\.com|calendly\.com|zoom\.us|mailto:/i.test(url));
    if (website) found.website = /^https?:/i.test(website) ? website.replace(/[.)]+$/, "") : `https://${website.replace(/[.)]+$/, "")}`;
    const signOff = tail.findLastIndex((line) => SIGN_OFF_RE.test(line));
    if (signOff >= 0) {
      const plain = tail.slice(signOff + 1).filter((line) =>
        line.length <= 60 && !/@|https?:|www\.|\.com\b|\d{3}/i.test(line));
      const [name, title, company] = plain;
      if (name && NAME_RE.test(name)) {
        found.name = name;
        if (title) found.title = title;
        if (company) found.company = company;
      }
    }
    return found;
  }

  // Zoho and Gmail both show a relative stamp ("10:40 AM" today, "Wed Sep 2
  // 2:57 PM" this year) and often a full date in a title attribute.
  function stampTime(value, now = new Date()) {
    const raw = String(value || "").replace(/\s+/g, " ").trim();
    if (!raw || !/\d{1,2}:\d{2}/.test(raw)) return NaN;
    const direct = Date.parse(raw.replace(/\bat\b/i, ""));
    if (Number.isFinite(direct) && /\b(19|20)\d{2}\b/.test(raw)) return direct;
    const clock = raw.match(/(\d{1,2}):(\d{2})\s*([AP]M)?/i);
    let hours = Number(clock[1]) % (clock[3] ? 12 : 24);
    if (clock[3] && clock[3].toUpperCase() === "PM") hours += 12;
    const date = new Date(now.getTime());
    const day = raw.match(/\b(jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\.? (\d{1,2})\b/i)
      || raw.match(/\b(\d{1,2}) (jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec)[a-z]*\b/i);
    if (day) {
      const [monthName, dayOfMonth] = /\d/.test(day[1]) ? [day[2], day[1]] : [day[1], day[2]];
      date.setMonth("janfebmaraprmayjunjulaugsepoctnovdec".indexOf(monthName.slice(0, 3).toLowerCase()) / 3, Number(dayOfMonth));
    } else if (/yesterday/i.test(raw)) date.setDate(date.getDate() - 1);
    else if (!/^\d{1,2}:\d{2}\s*([AP]M)?$/i.test(raw)) return NaN;
    date.setHours(hours, Number(clock[2]), 0, 0);
    // "Dec 30" read in January is last year.
    if (date.getTime() > now.getTime() + 86400000) date.setFullYear(date.getFullYear() - 1);
    return date.getTime();
  }

  // --- Gmail ---------------------------------------------------------------
  // The open conversation is in the URL hash; each expanded message carries
  // Gmail's legacy hex id, which the #all/<id> permalink reopens.

  function gmailMailbox() {
    const account = document.querySelector('a[aria-label*="Google Account"], a[aria-label*="Google Workspace"]');
    const fromLabel = address(attr(account, "aria-label") || attr(account, "title"));
    if (fromLabel) return fromLabel;
    const me = [...document.querySelectorAll("span[email][name]")]
      .find((node) => attr(node, "name").toLowerCase() === "me");
    return address(attr(me, "email"));
  }

  function readGmail() {
    const accountIndex = Number((location.pathname.match(/\/mail\/u\/(\d+)\//) || [])[1] || 0);
    const segments = location.hash.replace(/^#/, "").split("?")[0].split("/").filter(Boolean);
    const threadId = segments.length >= 2 ? segments[segments.length - 1] : "";
    const containers = [...document.querySelectorAll("div[data-legacy-message-id], div[data-message-id]")]
      .filter(visible);
    const container = containers[containers.length - 1] || null;
    const messageId = attr(container, "data-legacy-message-id")
      || attr(container, "data-message-id").replace(/^#?msg-[a-z]:/, "")
      || threadId;
    const subject = text(document.querySelector("h2.hP"))
      || document.title.replace(/^\(\d+\)\s*/, "").replace(/\s-\s[^-]*\s-\sGmail$/, "");
    const scope = container || document;
    const bodyNode = scope.querySelector(".a3s");
    const senderNode = scope.querySelector("h3 span[email], .gD[email], span[email][name]");
    const from = { email: address(attr(senderNode, "email")), name: attr(senderNode, "name") };
    const mailbox = gmailMailbox();
    const to = [];
    for (const node of scope.querySelectorAll("span[email]")) {
      if (bodyNode?.contains(node)) continue;
      const email = address(attr(node, "email"));
      if (!email || email === from.email || to.some((item) => item.email === email)) continue;
      const name = attr(node, "name");
      to.push({ email, name: name.toLowerCase() === "me" ? "" : name });
    }
    const stampNode = scope.querySelector("span.g3[title], span[title*=':']");
    const stamp = attr(stampNode, "title");
    const sentAt = stamp ? Date.parse(stamp.replace(/\bat\b/i, "")) : stampTime(text(stampNode));
    return {
      provider: "gmail", accountIndex, messageId, threadId, subject, from, to, mailbox, sentAt,
      lines: ownLines(bodyNode?.innerText || ""), sentHint: false, anchor: bodyNode,
      providerUrl: messageId ? `https://mail.google.com/mail/u/${accountIndex}/#all/${messageId}` : "",
    };
  }

  // --- Zoho Mail -----------------------------------------------------------
  // Zoho puts the message id in the hash (…/p/<id>) and the folder before it
  // (#mail/folder/sent/p/<id>). Its page markup changes often, so every field
  // is best-effort and editable in the panel.

  const ZOHO_SENT_FOLDERS = new Set(["sent", "outbox", "drafts"]);
  const ZOHO_FOLDER_NAMES = /^(inbox|sent|sent items|spam|junk|drafts?|outbox|trash|archive|templates|snoozed|starred|flagged|unread|all messages|newsletter|notifications?|zoho mail)$/i;

  function zohoSubject(value, folder) {
    const clean = String(value || "").replace(/\s+/g, " ").replace(/^subject\s*:\s*/i, "").trim();
    const bare = clean.replace(/\s*\(\d+\)$/, "").toLowerCase();
    if (!bare || bare === "subject" || bare === folder || ZOHO_FOLDER_NAMES.test(bare)) return "";
    return clean;
  }

  function zohoName(node, email, pane) {
    const own = attr(node, "data-name") || attr(node, "data-displayname");
    if (own && !own.includes("@") && NAME_RE.test(own.trim())) return own.trim();
    const escaped = email.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    let up = node.parentElement;
    for (let depth = 0; up && up !== pane && depth < 3; depth += 1, up = up.parentElement) {
      const found = addressList(text(up)).find((item) => item.email === email);
      if (found?.name && !found.name.includes("@")) return found.name;
      const rest = text(up).replace(new RegExp(escaped, "ig"), "").replace(/[<>()"]/g, " ").replace(/\s+/g, " ").trim()
        .replace(/^(?:from|to|cc|bcc)\b\s*:?\s*/i, "");
      if (rest && NAME_RE.test(rest)) return rest;
    }
    return "";
  }

  function zohoPane(messageId) {
    const byId = messageId && document.querySelector(
      `[data-msgid="${messageId}"], [data-messageid="${messageId}"], #msg_${messageId}`);
    if (byId) return byId;
    for (const selector of ['[class*="zmPV"]', '[class*="zmPreview"]', '[class*="MailPreview"]', '[class*="preview"]']) {
      const node = [...document.querySelectorAll(selector)].find(visible);
      if (node) return node;
    }
    return document.body;
  }

  function zohoBody(pane) {
    for (const frame of pane.querySelectorAll("iframe")) {
      try {
        const body = frame.contentDocument?.body;
        if (body && body.innerText.trim()) return { node: frame, text: body.innerText };
      } catch { /* a cross-origin frame is not the message */ }
    }
    for (const selector of ['[class*="zmMailContent"]', '[class*="MailContent"]', '[class*="mailContent"]',
      '[class*="zmPVContent"]', '[class*="msgContent"]', '[class*="MsgContent"]', '[class*="mailBody"]']) {
      const node = [...pane.querySelectorAll(selector)].find((item) => visible(item) && item.innerText.trim());
      if (node) return { node, text: node.innerText };
    }
    return { node: null, text: String(pane.innerText || "") };
  }

  function xpathNodes(expression, root) {
    const snapshot = document.evaluate(expression, root, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
    return Array.from({ length: snapshot.snapshotLength }, (_, index) => snapshot.snapshotItem(index));
  }

  function decoded(value) {
    try { return decodeURIComponent(value); } catch { return value; }
  }

  function attributePeople(root, skip = () => false) {
    const found = [];
    if (!root) return found;
    for (const node of xpathNodes("descendant-or-self::*[@*[contains(., '@') or contains(., '%40')]]", root)) {
      if (skip(node) || node.tagName === "IFRAME") continue;
      for (const { value } of node.attributes) {
        for (const person of addressList(decoded(value))) {
          if (found.some((item) => item.email === person.email)) continue;
          const label = text(node);
          const name = person.name || (label && !label.includes("@") && NAME_RE.test(label) ? label : zohoName(node, person.email, root));
          found.push({ email: person.email, name: name.slice(0, 120) });
        }
      }
    }
    return found;
  }

  function zohoRow(messageId, pane) {
    const outside = (node) => visible(node) && !pane.contains(node) && !node.contains(pane);
    const hit = /^\d+$/.test(messageId)
      ? xpathNodes(`//*[@*[contains(., "${messageId}")]]`, document).find(outside) : null;
    if (hit) {
      let row = hit;
      for (let depth = 0; depth < 8; depth += 1) {
        const parent = row.parentElement;
        if (!parent || parent.contains(pane)) break;
        const alike = [...parent.children].filter((child) => child.tagName === row.tagName).length;
        if (alike >= 3) return row;
        row = parent;
      }
      return hit;
    }
    return [...document.querySelectorAll('[aria-selected="true"], [class*="selected"], [class*="Selected"], [class*="zmLSel"]')]
      .find((node) => outside(node) && node.textContent.includes("@")) || null;
  }

  function rowPeople(row) {
    if (!row) return [];
    const people = [];
    for (const person of [...addressList(row.textContent), ...attributePeople(row)]) {
      const known = people.find((item) => item.email === person.email);
      if (!known) people.push(person);
      else if (!known.name) known.name = person.name;
    }
    return people;
  }

  function zohoMailbox() {
    const node = document.querySelector('[class*="zmUserEmail"], [class*="userEmail"], [class*="accountEmail"]');
    return address(attr(node, "title") || text(node));
  }

  function readZoho() {
    const hash = decodeURIComponent(location.hash || "");
    const messageId = (hash.match(/\/p\/(\d{6,})/) || hash.match(/\/(\d{15,})(?:[/?#]|$)/) || [])[1] || "";
    const threadId = (hash.match(/\/t\/(\d{6,})/) || [])[1] || "";
    const folder = ((hash.match(/#mail\/folder\/([^/?]+)/i) || [])[1] || "").toLowerCase();
    const pane = zohoPane(messageId);
    const body = zohoBody(pane);
    const inBody = (node) => Boolean(body.node && body.node !== pane && body.node.contains(node));
    const subjectOf = (nodes) => {
      for (const node of nodes) {
        const found = zohoSubject(text(node), folder);
        if (found) return found;
      }
      return "";
    };
    const SUBJECT_SELECTORS = ['[class*="zmPVSub"]', '[class*="Subject"]', '[class*="subject"]', "[data-subject]"];
    let subject = zohoSubject(attr(pane.querySelector("[data-subject]"), "data-subject"), folder);
    for (const selector of [...SUBJECT_SELECTORS, "h1", "h2"]) {
      if (subject) break;
      subject = subjectOf([...pane.querySelectorAll(selector)].filter((node) => visible(node) && !inBody(node)));
    }
    if (!subject) subject = subjectOf([...pane.querySelectorAll(SUBJECT_SELECTORS.join(","))].filter(visible));
    if (!subject) {
      const rows = [...document.querySelectorAll('[aria-selected="true"], [class*="zmLSel"], [class*="selected"], [class*="Selected"]')]
        .filter((node) => visible(node) && !pane.contains(node) && !node.contains(pane));
      subject = subjectOf(rows.flatMap((row) => [...row.querySelectorAll(SUBJECT_SELECTORS.join(","))]));
    }
    if (!subject) {
      subject = zohoSubject(document.title.replace(/\s*[-|]\s*Zoho Mail.*$/i, "").replace(/^Zoho Mail\s*[-|]\s*/i, ""), folder);
    }
    let mailbox = zohoMailbox();
    // The header's people, in page order: sender first, then recipients.
    // Zoho shows your own address as "Me", which also names the mailbox.
    const header = [];
    let meFirst = false;
    for (const node of pane.querySelectorAll("[title*='@'], [data-email], [data-emailid], [data-address], a[href^='mailto:']")) {
      if (inBody(node)) continue;
      const email = address(attr(node, "data-email") || attr(node, "data-emailid") || attr(node, "data-address")
        || attr(node, "href").replace(/^mailto:/, "") || attr(node, "title"));
      if (!email || header.some((item) => item.email === email)) continue;
      const label = text(node);
      if (label.toLowerCase() === "me") {
        mailbox = mailbox || email;
        if (!header.length) meFirst = true;
      }
      const name = label.includes("@") || !label ? zohoName(node, email, pane) : label;
      header.push({ email, name: name.slice(0, 120) });
    }
    if (!header.length) header.push(...attributePeople(pane, inBody));
    if (!header.length) {
      const headerText = String(pane.innerText || "").replace(body.node ? body.text : "", "");
      header.push(...addressList(headerText));
    }
    const sentHint = ZOHO_SENT_FOLDERS.has(folder) || meFirst;
    const listed = sentHint || !header.length ? rowPeople(zohoRow(messageId, pane)) : [];
    if (!sentHint && !header.length) header.push(...listed.filter((person) => person.email !== mailbox));
    let from;
    let to;
    if (sentHint) {
      from = header.find((item) => item.email === mailbox) || { email: mailbox, name: "" };
      to = [];
      for (const person of [...listed, ...header]) {
        if (person.email !== from.email && !to.some((item) => item.email === person.email)) to.push(person);
      }
    } else {
      from = header[0] || { email: "", name: "" };
      to = header.slice(1);
    }
    let sentAt = NaN;
    for (const node of pane.querySelectorAll("[title], time, span, div")) {
      if (inBody(node) || node.children.length > 2) continue;
      sentAt = stampTime(attr(node, "title")) || NaN;
      if (!Number.isFinite(sentAt)) sentAt = stampTime(text(node));
      if (Number.isFinite(sentAt)) break;
    }
    return {
      provider: "zoho", accountIndex: 0, messageId, threadId, subject, from, to, mailbox,
      sentAt, lines: ownLines(body.text), sentHint, providerUrl: messageId ? location.href : "",
      anchor: body.node,
    };
  }

  // The open message, and the node the CRM header goes above: the newest
  // message's body. List views and unreadable layouts have no anchor.
  function readPage() {
    const read = PROVIDER === "gmail" ? readGmail() : readZoho();
    const { lines, sentHint, anchor, ...message } = read;
    const mailbox = message.mailbox;
    // Which side is the contact: whoever is not this mailbox.
    const outbound = sentHint || (Boolean(mailbox) && message.from.email === mailbox);
    message.direction = outbound ? "outbound" : "inbound";
    message.greeting = greetingName(lines);
    message.signature = outbound ? {} : signatureDetails(lines);
    // Everyone on the message except this mailbox, most likely contact first:
    // the recipients of mail you sent, the sender of mail you received, and
    // people outside your own company domain before teammates on copy.
    const ownDomain = FREE_MAIL.has(domainOf(mailbox)) ? "" : domainOf(mailbox);
    const people = outbound ? [...message.to] : [message.from, ...message.to];
    const participants = [];
    for (const person of people) {
      if (person?.email && person.email !== mailbox && !participants.some((item) => item.email === person.email)) {
        participants.push({ email: person.email, name: realName(person.name, person.email) });
      }
    }
    if (ownDomain) participants.sort((a, b) => (domainOf(a.email) === ownDomain) - (domainOf(b.email) === ownDomain));
    message.participants = participants;
    return { message: applyContact(message, participants[0] || { email: "", name: "" }), anchor };
  }

  // --- CRM header above the open email -------------------------------------
  // The strip of chips and the panel under it are panel.js's; this script
  // only reads Gmail and Zoho and says which node the header sits above.

  const { host, panel, place } = crm.header({ read: async () => readPage().message });

  // Gmail and Zoho are single-page apps that redraw the reading pane at will:
  // put the header back above the newest message whenever it moves, and show
  // the new message in the header when the message or its contact changes.
  // Reading the page is free; the CRM record behind it is only read when the
  // header's "Look up" button is pressed, so clicking through an inbox sends
  // nothing to ForkMesh. `asked` carries the one exception: a link or a
  // sign-in change made in the toolbar popup, which reads the record again.
  let shown = "";
  let asked = false;
  function sync() {
    let page;
    try { page = readPage(); } catch { return; }
    const { message, anchor } = page;
    if (!message.messageId || !anchor?.parentNode) { host.remove(); return; }
    place(anchor);
    const key = `${message.messageId}|${message.contactEmail}`;
    if (key === shown) return;
    shown = key;
    if (asked) { asked = false; void panel.load(message); }
    else void panel.present(message);
  }

  // At most one read of the page per interval, however busy the client is.
  let timer = 0;
  function schedule(delay = 400) {
    if (timer) return;
    timer = setTimeout(() => { timer = 0; sync(); }, delay);
  }
  new MutationObserver(() => schedule()).observe(document.body || document.documentElement, { childList: true, subtree: true });
  window.addEventListener("hashchange", () => schedule(150));
  schedule(0);

  const SEND_GRACE = 10000;
  const SEND_RECIPIENTS = 5;
  const COMPOSE = PROVIDER === "gmail"
    ? ['[role="dialog"]', ".M9", ".ip", "form"]
    : ['[role="dialog"]', '[class*="compose" i]', "form"];
  const RECIPIENT_FIELD = /^(to|cc|bcc)\b|recipient/i;
  const outgoing = { pending: null, timer: 0, poll: 0 };

  function composeOf(node) {
    for (const selector of COMPOSE) {
      const box = node?.closest?.(selector);
      if (box) return box;
    }
    return null;
  }

  function sendControl(node) {
    const control = node?.closest?.('[role="button"], button, input[type="submit"], [data-tooltip]');
    if (!control || !composeOf(control)) return null;
    if (PROVIDER === "gmail" && control.classList.contains("aoO")) return control;
    const label = [attr(control, "aria-label"), attr(control, "data-tooltip"), attr(control, "title"),
      attr(control, "value"), text(control)].find(Boolean) || "";
    if (!/^send\b/i.test(label) || /^send\s+(later|options|feedback)|schedule/i.test(label)) return null;
    return control;
  }

  function composeRecipients(box, mailbox) {
    const found = [];
    const add = (email, name = "") => {
      const clean = address(email);
      if (!clean || clean === mailbox || found.some((item) => item.email === clean)) return;
      found.push({ email: clean, name: realName(name, clean) });
    };
    const from = address(box.querySelector('[name="from"]')?.value || "");
    for (const node of box.querySelectorAll("[data-hovercard-id], [email], [data-email], [data-emailid], [data-address]")) {
      if (node.closest('[contenteditable="true"], [name="from"]')) continue;
      const email = attr(node, "data-hovercard-id") || attr(node, "email") || attr(node, "data-email")
        || attr(node, "data-emailid") || attr(node, "data-address");
      if (address(email) === from) continue;
      add(email, attr(node, "data-name") || attr(node, "name") || text(node));
    }
    for (const field of box.querySelectorAll("input, textarea")) {
      const hint = attr(field, "name") || attr(field, "aria-label") || attr(field, "placeholder");
      if (!RECIPIENT_FIELD.test(hint)) continue;
      for (const person of addressList(field.value)) if (person.email !== from) add(person.email, person.name);
    }
    const ownDomain = FREE_MAIL.has(domainOf(mailbox)) ? "" : domainOf(mailbox);
    if (ownDomain) found.sort((a, b) => (domainOf(a.email) === ownDomain) - (domainOf(b.email) === ownDomain));
    return found.slice(0, SEND_RECIPIENTS);
  }

  function composeSubject(box) {
    const field = box.querySelector('input[name="subjectbox"], input[name*="subject" i], input[aria-label*="subject" i], '
      + 'input[placeholder*="subject" i], input[id*="subject" i]');
    return String(field?.value || "").replace(/\s+/g, " ").trim().slice(0, 300);
  }

  function readCompose(box) {
    const mailbox = address(box.querySelector('[name="from"]')?.value || "")
      || (PROVIDER === "gmail" ? gmailMailbox() : zohoMailbox());
    const recipients = composeRecipients(box, mailbox);
    if (!recipients.length) return null;
    return {
      provider: PROVIDER, mailbox, recipients, subject: composeSubject(box), sentAt: Date.now(),
      accountIndex: Number((location.pathname.match(/\/mail\/u\/(\d+)\//) || [])[1] || 0),
    };
  }

  function sentToast() {
    return attr(document.querySelector('#link_vsm[param], [id^="link_vsm"][param]'), "param");
  }

  function sentMessageId(param) {
    const hex = param.match(/\b[0-9a-f]{16}\b/i);
    if (hex) return { id: hex[0].toLowerCase(), permalink: true };
    const message = param.match(/msg-[a-z]:(r-?\d{6,})/);
    return message ? { id: message[1], permalink: false } : { id: "", permalink: false };
  }

  function sentSearchUrl(sent) {
    if (sent.provider !== "gmail") return `${location.origin}/zm/#mail/folder/sent`;
    const query = ["in:sent", sent.subject ? `subject:"${sent.subject.replace(/"/g, "")}"`
      : `to:${sent.recipients[0].email}`].join(" ");
    return `https://mail.google.com/mail/u/${sent.accountIndex}/#search/${encodeURIComponent(query)}`;
  }

  function trackSent() {
    const sent = outgoing.pending;
    if (!sent) return;
    outgoing.pending = null;
    clearTimeout(outgoing.timer);
    clearInterval(outgoing.poll);
    const found = sent.provider === "gmail" ? sentMessageId(sent.toast) : { id: "", permalink: false };
    const messageId = found.id || `fm-sent-${sent.sentAt.toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
    const providerUrl = found.permalink
      ? `https://mail.google.com/mail/u/${sent.accountIndex}/#all/${messageId}` : sentSearchUrl(sent);
    const { toast, before, ...message } = sent;
    try {
      chrome.runtime.sendMessage({ type: "fm-track-sent", message: { ...message, messageId, providerUrl } }, (response) => {
        if (chrome.runtime.lastError || !response?.tracked) return;
        shown = "";
        asked = true;
        schedule(0);
      });
    } catch { }
  }

  function captureSend(box) {
    const sent = readCompose(box);
    if (!sent) return;
    if (outgoing.pending) trackSent();
    outgoing.pending = { ...sent, before: sentToast(), toast: "" };
    outgoing.timer = setTimeout(trackSent, SEND_GRACE);
    if (PROVIDER !== "gmail") return;
    outgoing.poll = setInterval(() => {
      const param = sentToast();
      if (outgoing.pending && param && param !== outgoing.pending.before) outgoing.pending.toast = param;
    }, 300);
  }

  function undoSend(node) {
    const control = node?.closest?.('[id^="link_undo"], [role="button"], button, [role="link"]');
    return Boolean(control && (attr(control, "id").startsWith("link_undo") || /^undo$/i.test(text(control))));
  }

  document.addEventListener("click", (event) => {
    if (outgoing.pending && undoSend(event.target)) {
      outgoing.pending = null;
      clearTimeout(outgoing.timer);
      clearInterval(outgoing.poll);
      return;
    }
    const control = sendControl(event.target);
    if (control) captureSend(composeOf(control));
  }, true);
  document.addEventListener("keydown", (event) => {
    if (event.key !== "Enter" || !(event.ctrlKey || event.metaKey) || event.isComposing) return;
    const box = composeOf(event.target);
    if (box) captureSend(box);
  }, true);
  window.addEventListener("pagehide", trackSent);

  chrome.runtime.onMessage.addListener((request, _sender, sendResponse) => {
    // The popup linked the email or changed the sign-in: read it all again.
    if (request?.type === "fm-refresh-page") { shown = ""; asked = true; schedule(0); return false; }
    if (request?.type !== "fm-read-message") return false;
    try { sendResponse({ ok: true, message: readPage().message }); }
    catch (error) { sendResponse({ ok: false, error: "read_failed", message: String(error?.message || error) }); }
    return false;
  });
})();
