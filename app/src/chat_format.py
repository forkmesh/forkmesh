import re
from urllib.parse import quote, urlsplit


MAX_SOURCE = 16000
MAX_DEPTH = 8
MARKER_REACH = 1000
INLINE_MARKERS = (
    ("**", "strong"),
    ("~~", "s"),
    ("++", "u"),
    ("*", "strong"),
    ("_", "em"),
    ("~", "s"),
)
OPENS_AFTER = set(" \t\n([{\"'“‘<>-–—/:;,.!?*_~+")
SLACK_LINK_RE = re.compile(
    r"<((?:https?|mailto):[^\s|<>]+)(?:\|([^<>\n]+))?>", re.IGNORECASE)
RAW_URL_RE = re.compile(r"https?://[^\s<>]+", re.IGNORECASE)
MENTION_RE = re.compile(r"@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b", re.IGNORECASE)
BULLET_RE = re.compile(r"^\s*[-*•]\s+(.*)$")
NUMBERED_RE = re.compile(r"^\s*\d+[.)]\s+(.*)$")
QUOTE_RE = re.compile(r"^>\s?(.*)$")
PROFILE_ORIGIN = "https://forkmesh.com"

LINK_STYLE = "color:#0969da;text-decoration:underline"
CODE_STYLE = (
    "font-family:SFMono-Regular,Menlo,Consolas,monospace;font-size:12px;"
    "background:#eff1f3;border:1px solid #d0d7de;border-radius:4px;"
    "padding:1px 4px;color:#24292f")
PRE_STYLE = (
    "margin:6px 0;padding:8px 10px;background:#f6f8fa;"
    "border:1px solid #d0d7de;border-radius:6px;white-space:pre-wrap;"
    "font-family:SFMono-Regular,Menlo,Consolas,monospace;font-size:12px;"
    "line-height:1.45;color:#24292f")
LIST_STYLE = "margin:4px 0 4px 20px;padding:0"
QUOTE_STYLE = (
    "margin:6px 0;padding:0 0 0 10px;border-left:3px solid #d0d7de;"
    "color:#57606a")


def escape(text):
    return (str(text or "").replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def normalized(source):
    text = str(source or "").replace("\r\n", "\n").replace("\r", "\n")
    return text.replace("\0", "")[:MAX_SOURCE]


def safe_link(value):
    candidate = "".join(
        ch for ch in str(value or "") if ord(ch) >= 0x20 and ord(ch) != 0x7f
    ).strip()
    if not candidate or len(candidate) > 2048:
        return ""
    try:
        parts = urlsplit(candidate)
    except ValueError:
        return ""
    scheme = parts.scheme.lower()
    if scheme == "mailto":
        return candidate if parts.path else ""
    if scheme not in ("http", "https") or not parts.netloc:
        return ""
    return candidate


def raw_url_at(text, index):
    match = RAW_URL_RE.match(text, index)
    if not match:
        return ""
    url = match.group(0)
    while url:
        last = url[-1]
        if last in ",.!?;:":
            url = url[:-1]
            continue
        if last == ")" and url.count(")") > url.count("("):
            url = url[:-1]
            continue
        break
    return url


def link_close_at(text, start):
    depth = 0
    for index in range(start, len(text)):
        ch = text[index]
        if ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                return index
            depth -= 1
    return -1


def _closing_marker(text, start, marker):
    at = text.find(marker, start + 1)
    while 0 <= at and at - start <= MARKER_REACH:
        before = text[at - 1]
        after = text[at + len(marker):at + len(marker) + 1]
        if (not before.isspace() and not (after.isascii() and after.isalnum())
                and not (len(marker) == 1 and after == marker)):
            return at
        at = text.find(marker, at + 1)
    return -1


def _marker_at(text, index):
    if index > 0 and text[index - 1] not in OPENS_AFTER:
        return None
    for marker, tag in INLINE_MARKERS:
        if not text.startswith(marker, index):
            continue
        following = text[index + len(marker):index + len(marker) + 1]
        if not following or following.isspace():
            continue
        close = _closing_marker(text, index + len(marker), marker)
        if close >= 0:
            return marker, tag, close
    return None


def parse_inline(source, depth=0):
    text = str(source or "")
    if depth > MAX_DEPTH:
        return [{"type": "text", "text": text}] if text else []
    nodes = []
    plain = []

    def flush():
        if plain:
            nodes.append({"type": "text", "text": "".join(plain)})
            del plain[:]

    index = 0
    size = len(text)
    while index < size:
        ch = text[index]
        if ch == "\\" and index + 1 < size:
            plain.append(text[index + 1])
            index += 2
            continue
        if ch == "[":
            label_end = text.find("](", index + 1)
            link_end = link_close_at(text, label_end + 2) if label_end >= 0 else -1
            if label_end > index + 1 and link_end > label_end + 2:
                flush()
                children = parse_inline(text[index + 1:label_end], depth + 1)
                href = safe_link(text[label_end + 2:link_end])
                if href:
                    nodes.append({"type": "link", "href": href,
                                  "children": children})
                else:
                    nodes.extend(children)
                index = link_end + 1
                continue
        if ch == "<":
            match = SLACK_LINK_RE.match(text, index)
            if match:
                flush()
                label = (match.group(2) or "").strip()
                shown = label or re.sub(r"^mailto:", "", match.group(1),
                                        flags=re.IGNORECASE)
                href = safe_link(match.group(1))
                child = {"type": "text", "text": shown}
                nodes.append({"type": "link", "href": href, "children": [child]}
                             if href else child)
                index = match.end()
                continue
        if text.startswith("```", index):
            close = text.find("```", index + 3)
            if close > index + 3:
                flush()
                nodes.append({"type": "code", "text": text[index + 3:close]})
                index = close + 3
                continue
        if ch == "`":
            close = text.find("`", index + 1)
            if close > index + 1:
                flush()
                nodes.append({"type": "code", "text": text[index + 1:close]})
                index = close + 1
                continue
        found = _marker_at(text, index)
        if found:
            marker, tag, close = found
            flush()
            nodes.append({"type": tag, "children": parse_inline(
                text[index + len(marker):close], depth + 1)})
            index = close + len(marker)
            continue
        url = raw_url_at(text, index)
        if url:
            flush()
            nodes.append({"type": "url", "href": safe_link(url), "text": url})
            index += len(url)
            continue
        if ch == "@":
            previous = text[index - 1] if index else ""
            match = MENTION_RE.match(text, index)
            if match and not (previous and previous.isascii() and
                              (previous.isalnum() or previous in "_-")):
                flush()
                nodes.append({"type": "mention", "text": match.group(0)})
                index = match.end()
                continue
        plain.append(ch)
        index += 1
    flush()
    return nodes


def _list_item(line):
    match = BULLET_RE.match(line)
    if match:
        return False, match.group(1)
    match = NUMBERED_RE.match(line)
    if match:
        return True, match.group(1)
    return None


def _quote_line(line):
    match = QUOTE_RE.match(line)
    return match.group(1) if match else None


def _fence_at(lines, index):
    first = lines[index][3:]
    if len(first) >= 3 and first.endswith("```"):
        return first[:-3], index + 1
    for end in range(index + 1, len(lines)):
        closing = lines[end].rstrip()
        if not closing.endswith("```"):
            continue
        body = [first] + lines[index + 1:end] + [closing[:-3]]
        if not body[0].strip():
            body.pop(0)
        if body and not body[-1].strip():
            body.pop()
        return "\n".join(body), end + 1
    return None


def parse_blocks(source):
    lines = normalized(source).split("\n")
    blocks = []
    paragraph = None
    index = 0
    while index < len(lines):
        line = lines[index]
        fence = _fence_at(lines, index) if line.startswith("```") else None
        item = None if fence else _list_item(line)
        quoted = None if fence or item else _quote_line(line)
        if not fence and not item and quoted is None:
            if paragraph is None:
                paragraph = {"type": "p", "lines": []}
                blocks.append(paragraph)
            paragraph["lines"].append(line)
            index += 1
            continue
        paragraph = None
        if fence:
            blocks.append({"type": "pre", "text": fence[0]})
            index = fence[1]
        elif item:
            ordered = item[0]
            items = []
            while index < len(lines):
                current = _list_item(lines[index])
                if not current or current[0] != ordered:
                    break
                items.append(current[1])
                index += 1
            blocks.append({"type": "ol" if ordered else "ul", "items": items})
        else:
            quote = []
            while index < len(lines) and _quote_line(lines[index]) is not None:
                quote.append(_quote_line(lines[index]))
                index += 1
            blocks.append({"type": "quote", "lines": quote})
    return blocks


def _inline_text(nodes, links):
    out = []
    for node in nodes:
        if "children" in node:
            label = _inline_text(node["children"], links)
            href = node.get("href", "")
            if (links and node["type"] == "link" and href
                    and href.lower() not in (label.lower(),
                                             "mailto:" + label.lower())):
                label += " (" + href + ")"
            out.append(label)
        else:
            out.append(node["text"])
    return "".join(out)


def to_text(source, links=True):
    out = []
    for block in parse_blocks(source):
        kind = block["type"]
        if kind == "pre":
            out.append(block["text"])
        elif kind in ("ul", "ol"):
            for number, item in enumerate(block["items"], 1):
                bullet = "• " if kind == "ul" else "%d. " % number
                out.append(bullet + _inline_text(parse_inline(item), links))
        elif kind == "quote":
            out.extend("> " + _inline_text(parse_inline(line), links)
                       for line in block["lines"])
        else:
            out.extend(_inline_text(parse_inline(line), links)
                       for line in block["lines"])
    return "\n".join(out)


def _mention_html(text, profile_origin, in_link):
    if in_link:
        return "<strong>" + escape(text) + "</strong>"
    href = (str(profile_origin or PROFILE_ORIGIN).strip().rstrip("/") + "/@"
            + quote(text[1:].lower(), safe=""))
    return ("<a href=\"" + escape(href) + "\" style=\"" + LINK_STYLE + "\">"
            + escape(text) + "</a>")


def _inline_html(nodes, profile_origin="", in_link=False):
    out = []
    for node in nodes:
        kind = node["type"]
        if kind == "text":
            out.append(escape(node["text"]))
        elif kind == "mention":
            out.append(_mention_html(node["text"], profile_origin, in_link))
        elif kind == "code":
            out.append("<code style=\"" + CODE_STYLE + "\">"
                       + escape(node["text"]) + "</code>")
        elif kind == "url":
            if node["href"]:
                out.append("<a href=\"" + escape(node["href"]) + "\" style=\""
                           + LINK_STYLE + "\">" + escape(node["text"]) + "</a>")
            else:
                out.append(escape(node["text"]))
        elif kind == "link":
            out.append("<a href=\"" + escape(node["href"]) + "\" style=\""
                       + LINK_STYLE + "\">"
                       + _inline_html(node["children"], profile_origin, True)
                       + "</a>")
        else:
            out.append("<" + kind + ">"
                       + _inline_html(node["children"], profile_origin, in_link)
                       + "</" + kind + ">")
    return "".join(out)


def to_html(source, profile_origin=""):
    out = []
    for block in parse_blocks(source):
        kind = block["type"]
        if kind == "pre":
            out.append("<pre style=\"" + PRE_STYLE + "\">"
                       + escape(block["text"]) + "</pre>")
        elif kind in ("ul", "ol"):
            out.append("<" + kind + " style=\"" + LIST_STYLE + "\">" + "".join(
                "<li>" + _inline_html(parse_inline(item), profile_origin) + "</li>"
                for item in block["items"]) + "</" + kind + ">")
        elif kind == "quote":
            out.append("<blockquote style=\"" + QUOTE_STYLE + "\">" + "<br>".join(
                _inline_html(parse_inline(line), profile_origin)
                for line in block["lines"]) + "</blockquote>")
        else:
            out.append("<br>".join(
                _inline_html(parse_inline(line), profile_origin)
                for line in block["lines"]))
    return "".join(out)
