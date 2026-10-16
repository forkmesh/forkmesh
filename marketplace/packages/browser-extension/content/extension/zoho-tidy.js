(() => {
  if (globalThis.ForkMeshZohoTidy) return;

  const REPLY_PREFIX = /^(?:(?:re|fwd?|aw|sv|wg|antw)\s*(?:\[\d+\])?\s*:\s*)+/i;
  const collator = new Intl.Collator(undefined, { sensitivity: "base", numeric: true });
  const ACTIONS = [
    ["delete", /^(?:delete|move to trash|trash)$/i],
    ["archive", /^archive$/i],
    ["read", /^mark\s+(?:as\s+)?read$/i],
  ];
  const VERBS = { select: "Select", read: "Mark read", archive: "Archive", delete: "Delete" };

  const squash = (value) => String(value ?? "").replace(/\s+/g, " ").trim();
  const fold = (value) => squash(value).normalize("NFD").replace(/\p{M}/gu, "").toLowerCase();

  function senderName(text) {
    return squash(text).replace(/^["'\s]+|["'\s]+$/g, "").replace(/\s*\(\d+\)$/, "").replace(/^["'\s]+|["'\s]+$/g, "");
  }

  function subjectText(text) {
    return squash(text).replace(REPLY_PREFIX, "").trim();
  }

  function senderKey(name) {
    return fold(senderName(name));
  }

  function parseQuery(text) {
    const query = { any: [], from: [], subject: [] };
    const pattern = /(?:(from|subject):)?(?:"([^"]*)"?|(\S+))/gi;
    for (const match of String(text ?? "").matchAll(pattern)) {
      const field = (match[1] || "").toLowerCase();
      const word = fold(match[2] ?? match[3] ?? "");
      if (!word) continue;
      query[field || "any"].push(word);
    }
    return query;
  }

  function matches(item, query) {
    const sender = fold(item.sender);
    const subject = fold(item.subject);
    return query.from.every((word) => sender.includes(word))
      && query.subject.every((word) => subject.includes(word))
      && query.any.every((word) => sender.includes(word) || subject.includes(word));
  }

  function filtered(items, { query = "", sender = "" } = {}) {
    const parsed = parseQuery(query);
    return items.filter((item) => matches(item, parsed) && (!sender || senderKey(item.sender) === sender));
  }

  function shown(items, options = {}) {
    return new Set(filtered(items, options).map((item) => item.index));
  }

  function groups(items) {
    const found = new Map();
    for (const item of items) {
      const key = senderKey(item.sender);
      if (!found.has(key)) found.set(key, { key, label: senderName(item.sender) || "(no sender)", count: 0, first: item.index });
      found.get(key).count += 1;
    }
    return [...found.values()].sort((a, b) => b.count - a.count || collator.compare(a.label, b.label) || a.first - b.first);
  }

  function order(items, { by = "date", descending = false, group = false } = {}) {
    const rank = new Map();
    if (group) groups(items).forEach((entry, position) => rank.set(entry.key, position));
    const direction = descending ? -1 : 1;
    const key = (item) => (by === "sender" ? senderName(item.sender) : subjectText(item.subject));
    return [...items].sort((a, b) => {
      if (group) {
        const step = rank.get(senderKey(a.sender)) - rank.get(senderKey(b.sender));
        if (step) return step;
      }
      if (by === "date") return (a.index - b.index) * direction;
      return (collator.compare(key(a), key(b)) * direction) || a.index - b.index;
    });
  }

  function ticked({ checked = false, ariaChecked = null, ariaSelected = null, boxClass = "", rowClass = "" } = {}) {
    if (checked === true || ariaChecked === "true" || ariaSelected === "true") return true;
    if (ariaChecked === "false") return false;
    return /\b(?:checked|selected|zmLSel\w*)\b/i.test(`${boxClass} ${rowClass}`);
  }

  function actionOf(label) {
    const text = squash(label).replace(/\s*\([^)]*\)$/, "");
    for (const [kind, pattern] of ACTIONS) if (pattern.test(text)) return kind;
    return "";
  }

  function plural(count, word = "message") {
    return `${count} ${word}${count === 1 ? "" : "s"}`;
  }

  function summary(shownCount, total) {
    return shownCount === total ? `${total} loaded` : `${shownCount} of ${total} loaded`;
  }

  function verb(kind, count) {
    return `${VERBS[kind]} ${count}`;
  }

  function confirmText(count) {
    return `Delete ${plural(count)}? Zoho Mail moves them to Trash.`;
  }

  function doneText(kind, count) {
    if (kind === "select") return `Selected ${plural(count)} in Zoho Mail.`;
    if (kind === "read") return `Marked ${plural(count)} as read.`;
    if (kind === "archive") return `Archived ${plural(count)}.`;
    return `Deleted ${plural(count)}.`;
  }

  globalThis.ForkMeshZohoTidy = Object.freeze({
    senderName, subjectText, senderKey, parseQuery, matches, filtered, shown, groups, order,
    ticked, actionOf, plural, summary, verb, confirmText, doneText,
  });
})();
