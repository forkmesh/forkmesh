const RICH_TEXT_VERSION = 1;
const MAX_RICH_SOURCE = 16000;
const MAX_RENDER_NODES = 2048;
const SAFE_LINK_PROTOCOLS = new Set(["http:", "https:", "mailto:"]);
const MENTION_RE = /^@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b/i;

function normalizedSource(value) {
  return String(value ?? "")
    .replace(/\r\n?/g, "\n")
    .replace(/\0/g, "")
    .slice(0, MAX_RICH_SOURCE);
}

function boundedRange(value, start, end) {
  const length = value.length;
  const first = Math.max(0, Math.min(length, Number(start) || 0));
  const second = Math.max(first, Math.min(length, Number(end) || first));
  return [first, second];
}

function safeLink(value) {
  try {
    const url = new URL(String(value || "").trim(), window.location.href);
    return SAFE_LINK_PROTOCOLS.has(url.protocol) ? url.href : "";
  } catch (_) {
    return "";
  }
}

function appendText(parent, value, state) {
  if (!value) return;
  if (state.nodes >= MAX_RENDER_NODES) return;
  parent.append(document.createTextNode(value));
  state.nodes += 1;
}

function appendLink(parent, label, href, state) {
  if (state.nodes >= MAX_RENDER_NODES) return;
  const link = document.createElement("a");
  link.href = href;
  link.target = "_blank";
  link.rel = "noopener noreferrer";
  link.append(document.createTextNode(label));
  parent.append(link);
  state.nodes += 2;
}

const INLINE_MARKERS = [
  ["**", "strong"],
  ["~~", "s"],
  ["++", "u"],
  ["*", "strong"],
  ["_", "em"],
  ["~", "s"],
];
const MARKER_OPENS_AFTER = /[\s([{"'“‘<>\-–—/:;,.!?*_~+]/;
const MARKER_CLOSES_BEFORE = /[A-Za-z0-9]/;
const MARKER_REACH = 1000;
const SLACK_LINK_RE = /^<((?:https?|mailto):[^\s|<>]+)(?:\|([^<>\n]+))?>/i;

function closingMarker(source, from, marker) {
  let at = source.indexOf(marker, from + 1);
  while (at >= 0 && at - from <= MARKER_REACH) {
    const before = source[at - 1];
    const after = source[at + marker.length] || "";
    if (!/\s/.test(before) && !MARKER_CLOSES_BEFORE.test(after) &&
        !(marker.length === 1 && after === marker)) {
      return at;
    }
    at = source.indexOf(marker, at + 1);
  }
  return -1;
}

function markerAt(source, index) {
  const before = index > 0 ? source[index - 1] : "";
  if (before && !MARKER_OPENS_AFTER.test(before)) return null;
  for (const [marker, tag] of INLINE_MARKERS) {
    if (!source.startsWith(marker, index)) continue;
    const next = source[index + marker.length] || "";
    if (!next || /\s/.test(next)) continue;
    const close = closingMarker(source, index + marker.length, marker);
    if (close >= 0) return { marker, tag, close };
  }
  return null;
}

function rawUrlAt(source, index) {
  const match = source.slice(index).match(/^https?:\/\/[^\s<>]+/i);
  if (!match) return "";
  let url = match[0];
  // Trailing sentence punctuation is not part of the address. A closing
  // parenthesis is, but only when the address opened one: "…/Foo_(bar)" is a
  // page, while "(see …/status)" is a sentence wrapped around a link.
  while (url) {
    const last = url[url.length - 1];
    if (",.!?;:".includes(last)) {
      url = url.slice(0, -1);
      continue;
    }
    const opened = url.split("(").length;
    const closed = url.split(")").length;
    if (last === ")" && closed > opened) {
      url = url.slice(0, -1);
      continue;
    }
    break;
  }
  return url;
}

function linkCloseAt(source, start) {
  let depth = 0;
  for (let index = start; index < source.length; index += 1) {
    if (source[index] === "(") depth += 1;
    if (source[index] !== ")") continue;
    if (depth === 0) return index;
    depth -= 1;
  }
  return -1;
}

export function parseInline(source, depth = 0) {
  const text = String(source ?? "");
  if (depth > 8) return text ? [{ type: "text", text }] : [];
  const nodes = [];
  let plain = "";
  const flush = () => {
    if (plain) nodes.push({ type: "text", text: plain });
    plain = "";
  };
  let index = 0;
  while (index < text.length) {
    const char = text[index];
    if (char === "\\" && index + 1 < text.length) {
      plain += text[index + 1];
      index += 2;
      continue;
    }

    if (char === "[") {
      const labelEnd = text.indexOf("](", index + 1);
      const linkEnd = labelEnd >= 0 ? linkCloseAt(text, labelEnd + 2) : -1;
      if (labelEnd > index + 1 && linkEnd > labelEnd + 2) {
        flush();
        const children = parseInline(text.slice(index + 1, labelEnd), depth + 1);
        const href = safeLink(text.slice(labelEnd + 2, linkEnd));
        if (href) nodes.push({ type: "link", href, children });
        else nodes.push(...children);
        index = linkEnd + 1;
        continue;
      }
    }

    if (char === "<") {
      const match = text.slice(index).match(SLACK_LINK_RE);
      if (match) {
        flush();
        const label = match[2] ? match[2].trim() : "";
        const href = safeLink(match[1]);
        const shown = label || match[1].replace(/^mailto:/i, "");
        if (href) nodes.push({ type: "link", href, children: [{ type: "text", text: shown }] });
        else nodes.push({ type: "text", text: shown });
        index += match[0].length;
        continue;
      }
    }

    if (text.startsWith("```", index)) {
      const close = text.indexOf("```", index + 3);
      if (close > index + 3) {
        flush();
        nodes.push({ type: "code", text: text.slice(index + 3, close) });
        index = close + 3;
        continue;
      }
    }

    if (char === "`") {
      const close = text.indexOf("`", index + 1);
      if (close > index + 1) {
        flush();
        nodes.push({ type: "code", text: text.slice(index + 1, close) });
        index = close + 1;
        continue;
      }
    }

    const marker = markerAt(text, index);
    if (marker) {
      flush();
      const content = text.slice(index + marker.marker.length, marker.close);
      nodes.push({ type: marker.tag, children: parseInline(content, depth + 1) });
      index = marker.close + marker.marker.length;
      continue;
    }

    const rawUrl = rawUrlAt(text, index);
    if (rawUrl) {
      flush();
      nodes.push({ type: "url", href: safeLink(rawUrl), text: rawUrl });
      index += rawUrl.length;
      continue;
    }

    if (char === "@") {
      const previous = index > 0 ? text[index - 1] : "";
      const mention = text.slice(index).match(MENTION_RE);
      if (mention && !/[A-Za-z0-9_-]/.test(previous)) {
        flush();
        nodes.push({ type: "mention", text: mention[0], name: mention[1] });
        index += mention[0].length;
        continue;
      }
    }

    plain += char;
    index += 1;
  }
  flush();
  return nodes;
}

function inlinePlainText(nodes) {
  return nodes
    .map((node) => node.children ? inlinePlainText(node.children) : node.text)
    .join("");
}

function appendNodes(parent, nodes, options, state) {
  for (const node of nodes) {
    if (state.nodes >= MAX_RENDER_NODES) return;
    if (node.type === "text") {
      appendText(parent, node.text, state);
    } else if (node.type === "url") {
      if (node.href) appendLink(parent, node.text, node.href, state);
      else appendText(parent, node.text, state);
    } else if (node.type === "mention") {
      const rendered = options.renderMention?.(parent, node.text, node.name);
      if (rendered instanceof Node) parent.append(rendered);
      if (!rendered) appendText(parent, node.text, state);
      else state.nodes += 1;
    } else if (node.type === "code") {
      const code = document.createElement("code");
      parent.append(code);
      state.nodes += 1;
      appendText(code, node.text, state);
    } else if (node.type === "link") {
      const link = document.createElement("a");
      link.href = node.href;
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      parent.append(link);
      state.nodes += 1;
      appendNodes(link, node.children, options, state);
    } else {
      const element = document.createElement(node.type);
      parent.append(element);
      state.nodes += 1;
      appendNodes(element, node.children, options, state);
    }
  }
}

function appendLineBreak(parent, state) {
  if (state.nodes >= MAX_RENDER_NODES) return;
  parent.append(document.createElement("br"));
  state.nodes += 1;
}

function listItem(line) {
  const bullet = line.match(/^\s*[-*•]\s+(.*)$/);
  if (bullet) return { ordered: false, text: bullet[1] };
  const numbered = line.match(/^\s*\d+[.)]\s+(.*)$/);
  if (numbered) return { ordered: true, text: numbered[1] };
  return null;
}

function quoteLine(line) {
  const quote = line.match(/^>\s?(.*)$/);
  return quote ? quote[1] : null;
}

function fenceAt(lines, index) {
  const first = lines[index].slice(3);
  if (first.length >= 3 && first.endsWith("```")) {
    return { text: first.slice(0, -3), next: index + 1 };
  }
  for (let end = index + 1; end < lines.length; end += 1) {
    if (!lines[end].trimEnd().endsWith("```")) continue;
    const last = lines[end].trimEnd().slice(0, -3);
    const body = [first, ...lines.slice(index + 1, end), last];
    if (!body[0].trim()) body.shift();
    if (body.length && !body[body.length - 1].trim()) body.pop();
    return { text: body.join("\n"), next: end + 1 };
  }
  return null;
}

export function parseBlocks(source) {
  const lines = normalizedSource(source).split("\n");
  const blocks = [];
  let paragraph = null;
  for (let index = 0; index < lines.length;) {
    const line = lines[index];
    const fence = line.startsWith("```") ? fenceAt(lines, index) : null;
    const item = fence ? null : listItem(line);
    const quoted = fence || item ? null : quoteLine(line);
    if (!fence && !item && quoted === null) {
      if (!paragraph) {
        paragraph = { type: "p", lines: [] };
        blocks.push(paragraph);
      }
      paragraph.lines.push(line);
      index += 1;
      continue;
    }
    paragraph = null;
    if (fence) {
      blocks.push({ type: "pre", text: fence.text });
      index = fence.next;
    } else if (item) {
      const items = [];
      while (index < lines.length) {
        const next = listItem(lines[index]);
        if (!next || next.ordered !== item.ordered) break;
        items.push(next.text);
        index += 1;
      }
      blocks.push({ type: item.ordered ? "ol" : "ul", items });
    } else {
      const quote = [];
      while (index < lines.length && quoteLine(lines[index]) !== null) {
        quote.push(quoteLine(lines[index]));
        index += 1;
      }
      blocks.push({ type: "quote", lines: quote });
    }
  }
  return blocks;
}

export function normalizeRichText(value) {
  if (typeof value === "string") {
    return { v: RICH_TEXT_VERSION, source: normalizedSource(value) };
  }
  if (!value || typeof value !== "object" || value.v !== RICH_TEXT_VERSION) {
    return null;
  }
  return { v: RICH_TEXT_VERSION, source: normalizedSource(value.source) };
}

export function plainTextFromRichSource(source) {
  const out = [];
  for (const block of parseBlocks(source)) {
    if (block.type === "pre") {
      out.push(block.text);
    } else if (block.type === "ul" || block.type === "ol") {
      block.items.forEach((item, index) => {
        const bullet = block.type === "ul" ? "• " : `${index + 1}. `;
        out.push(bullet + inlinePlainText(parseInline(item)));
      });
    } else if (block.type === "quote") {
      block.lines.forEach((line) => out.push("> " + inlinePlainText(parseInline(line))));
    } else {
      block.lines.forEach((line) => out.push(inlinePlainText(parseInline(line))));
    }
  }
  return out.join("\n");
}

export function wrapSelection(value, start, end, before, after) {
  const source = String(value ?? "");
  const opening = String(before ?? "");
  const closing = String(after ?? "");
  const [selectionStart, selectionEnd] = boundedRange(source, start, end);
  const selected = source.slice(selectionStart, selectionEnd);
  return {
    value: `${source.slice(0, selectionStart)}${opening}${selected}${closing}${source.slice(selectionEnd)}`,
    selectionStart: selectionStart + opening.length,
    selectionEnd: selectionEnd + opening.length,
  };
}

export function toggleLinePrefix(value, start, end, prefix) {
  const source = String(value ?? "");
  const token = String(prefix ?? "");
  const [selectionStart, selectionEnd] = boundedRange(source, start, end);
  const firstLine = source.lastIndexOf("\n", Math.max(0, selectionStart - 1)) + 1;
  const selectedEnd = selectionEnd === firstLine
    ? selectionEnd
    : source.indexOf("\n", selectionEnd) < 0
      ? source.length
      : source.indexOf("\n", selectionEnd);
  const block = source.slice(firstLine, selectedEnd);
  const lines = block.split("\n");
  const remove = Boolean(token) && lines.every((line) => line.startsWith(token));
  const transformed = lines
    .map((line) => remove ? line.slice(token.length) : `${token}${line}`)
    .join("\n");
  const deltaPerLine = remove ? -token.length : token.length;
  return {
    value: `${source.slice(0, firstLine)}${transformed}${source.slice(selectedEnd)}`,
    selectionStart: Math.max(firstLine, selectionStart + deltaPerLine),
    selectionEnd: Math.max(firstLine, selectionEnd + (deltaPerLine * lines.length)),
  };
}

export function linkSelection(value, start, end) {
  const source = String(value ?? "");
  const [selectionStart, selectionEnd] = boundedRange(source, start, end);
  const selected = source.slice(selectionStart, selectionEnd) || "link text";
  const replacement = `[${selected}](https://)`;
  const urlStart = selectionStart + selected.length + 3;
  return {
    value: `${source.slice(0, selectionStart)}${replacement}${source.slice(selectionEnd)}`,
    selectionStart: urlStart,
    selectionEnd: urlStart + 8,
  };
}

export function renderRichText(container, message, options = {}) {
  if (!container) return;
  const richText = normalizeRichText(message?.richText);
  const source = richText ? richText.source : normalizedSource(message?.text);
  container.replaceChildren();
  const state = { nodes: 0 };

  for (const block of parseBlocks(source)) {
    if (state.nodes >= MAX_RENDER_NODES) return;
    if (block.type === "pre") {
      const pre = document.createElement("pre");
      const code = document.createElement("code");
      pre.append(code);
      container.append(pre);
      state.nodes += 2;
      appendText(code, block.text, state);
    } else if (block.type === "ul" || block.type === "ol") {
      const list = document.createElement(block.type);
      container.append(list);
      state.nodes += 1;
      for (const item of block.items) {
        const row = document.createElement("li");
        list.append(row);
        state.nodes += 1;
        appendNodes(row, parseInline(item), options, state);
      }
    } else if (block.type === "quote") {
      const blockquote = document.createElement("blockquote");
      container.append(blockquote);
      state.nodes += 1;
      block.lines.forEach((line, index) => {
        if (index) appendLineBreak(blockquote, state);
        appendNodes(blockquote, parseInline(line), options, state);
      });
    } else {
      block.lines.forEach((line, index) => {
        if (index) appendLineBreak(container, state);
        appendNodes(container, parseInline(line), options, state);
      });
    }
  }
}
