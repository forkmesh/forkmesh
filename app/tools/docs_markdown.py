#!/usr/bin/env python3
"""Dependency-free Markdown renderer for the hosted documentation library.

The public site builds on whatever machine runs ``wrangler deploy`` and on
clean CI sandboxes, so the renderer deliberately depends on nothing outside
the standard library. It covers the GitHub-flavoured subset the files under
``www/docs`` actually use: ATX headings, paragraphs, fenced and indented code,
nested ordered/unordered/task lists, block quotes, pipe tables, thematic
breaks, HTML comments, inline code, emphasis, links, images, autolinks and a
small allowlist of raw inline HTML (``<a id="…">`` anchors and the like).

Every heading receives a GitHub-compatible ``id`` so the ``file.md#section``
links already used between the documents keep resolving after rendering.
"""

from __future__ import annotations

import html
import re
from dataclasses import dataclass, field
from typing import Callable


LinkRewriter = Callable[[str, bool], str]

FENCE_RE = re.compile(r"^(\s{0,3})(`{3,}|~{3,})\s*([^`\s]*)\s*(.*)$")
HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*(?:(?<=\s)#+\s*)?$")
HR_RE = re.compile(r"^\s{0,3}([-*_])(?:\s*\1){2,}\s*$")
LIST_RE = re.compile(r"^(\s*)([-*+]|\d{1,9}[.)])(\s+|$)(.*)$")
QUOTE_RE = re.compile(r"^\s{0,3}>\s?(.*)$")
TABLE_DELIMITER_RE = re.compile(
    r"^\s*\|?\s*:?-{1,}:?\s*(\|\s*:?-{1,}:?\s*)*\|?\s*$"
)
TASK_RE = re.compile(r"^\[([ xX])\]\s+(.*)$", re.DOTALL)
COMMENT_START = "<!--"
COMMENT_END = "-->"
ENTITY_RE = re.compile(r"&(?:[a-zA-Z][a-zA-Z0-9]{1,31}|#\d{1,7}|#[xX][0-9a-fA-F]{1,6});")
BARE_URL_RE = re.compile(r"https?://[^\s<>\"'`]+")
INLINE_TAG_RE = re.compile(
    r"<(/?)(a|br|kbd|sub|sup|details|summary|img)\b((?:\s+[a-zA-Z-]+(?:=\"[^\"<>]*\"|='[^'<>]*')?)*)\s*(/?)>"
)
INLINE_ATTRIBUTE_RE = re.compile(r"\s+([a-zA-Z-]+)(?:=\"([^\"]*)\"|='([^']*)')?")
ALLOWED_ATTRIBUTES = {"id", "href", "title", "alt", "src", "name", "open"}
SLUG_STRIP_RE = re.compile(r"[^\w\- ]", re.UNICODE)
PUNCTUATION = set("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~")


@dataclass
class Heading:
    level: int
    id: str
    text: str


@dataclass
class RenderedDocument:
    html: str
    title: str
    description: str
    headings: list[Heading] = field(default_factory=list)
    links: list[str] = field(default_factory=list)
    plain_text: str = ""


class _Renderer:
    def __init__(self, link_rewriter: LinkRewriter | None) -> None:
        self.link_rewriter = link_rewriter
        self.headings: list[Heading] = []
        self.links: list[str] = []
        self.slug_counts: dict[str, int] = {}
        self.first_paragraph: str | None = None
        self.title: str | None = None
        self.plain_parts: list[str] = []

    # ------------------------------------------------------------------ blocks
    def render_document(self, text: str) -> str:
        lines = text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        lines = _strip_front_matter(lines)
        return self.render_blocks(lines, top_level=True)

    def render_blocks(self, lines: list[str], *, top_level: bool = False) -> str:
        out: list[str] = []
        i = 0
        count = len(lines)
        while i < count:
            line = lines[i]
            stripped = line.strip()
            if not stripped:
                i += 1
                continue

            if stripped.startswith(COMMENT_START):
                i = _skip_comment(lines, i)
                continue

            fence = FENCE_RE.match(line)
            if fence:
                i = self._fenced_code(lines, i, fence, out)
                continue

            heading = HEADING_RE.match(line)
            if heading:
                out.append(self._heading(len(heading.group(1)), heading.group(2)))
                i += 1
                continue

            if HR_RE.match(line):
                out.append("<hr />")
                i += 1
                continue

            if QUOTE_RE.match(line):
                i = self._blockquote(lines, i, out)
                continue

            if _is_table_start(lines, i):
                i = self._table(lines, i, out)
                continue

            list_match = LIST_RE.match(line)
            if list_match and _list_item_has_content(list_match):
                i = self._list(lines, i, out)
                continue

            if line.startswith("    ") and not line[4:].isspace():
                i = self._indented_code(lines, i, out)
                continue

            raw_block = _raw_html_block(line)
            if raw_block is not None:
                out.append(raw_block)
                i += 1
                continue

            i = self._paragraph(lines, i, out)
        return "\n".join(out)

    def _fenced_code(self, lines, i, fence, out) -> int:
        indent = len(fence.group(1))
        marker = fence.group(2)
        language = fence.group(3)
        body: list[str] = []
        i += 1
        while i < len(lines):
            line = lines[i]
            candidate = line.strip()
            if (
                candidate.startswith(marker[0] * len(marker))
                and candidate.strip(marker[0]) == ""
            ):
                i += 1
                break
            body.append(line[indent:] if line[:indent].isspace() else line)
            i += 1
        code = html.escape("\n".join(body), quote=False)
        attributes = ""
        if language:
            safe = re.sub(r"[^a-zA-Z0-9_+#.-]", "", language)
            if safe:
                attributes = f' class="language-{safe}" data-language="{safe}"'
        out.append(f"<pre><code{attributes}>{code}\n</code></pre>")
        self.plain_parts.append("\n".join(body))
        return i

    def _indented_code(self, lines, i, out) -> int:
        body: list[str] = []
        while i < len(lines):
            line = lines[i]
            if line.startswith("    "):
                body.append(line[4:])
            elif not line.strip():
                body.append("")
            else:
                break
            i += 1
        while body and not body[-1].strip():
            body.pop()
        out.append("<pre><code>%s\n</code></pre>" % html.escape("\n".join(body), quote=False))
        return i

    def _heading(self, level: int, raw: str) -> str:
        inner = self.render_inline(raw)
        text = _plain_text(inner)
        slug = _slugify(text)
        seen = self.slug_counts.get(slug, 0)
        self.slug_counts[slug] = seen + 1
        if seen:
            slug = f"{slug}-{seen}"
        if self.title is None and level == 1:
            self.title = text
        self.headings.append(Heading(level, slug, text))
        self.plain_parts.append(text)
        return (
            f'<h{level} id="{slug}">{inner}'
            f'<a class="heading-anchor" href="#{slug}" aria-label="Link to this section">#</a>'
            f"</h{level}>"
        )

    def _blockquote(self, lines, i, out) -> int:
        body: list[str] = []
        while i < len(lines):
            match = QUOTE_RE.match(lines[i])
            if match:
                body.append(match.group(1))
            elif lines[i].strip() and body and body[-1].strip() and not _starts_block(lines[i]):
                body.append(lines[i])  # lazy continuation
            else:
                break
            i += 1
        out.append("<blockquote>\n%s\n</blockquote>" % self.render_blocks(body))
        return i

    def _table(self, lines, i, out) -> int:
        header = _split_row(lines[i])
        alignments = [_alignment(cell) for cell in _split_row(lines[i + 1])]
        i += 2
        rows: list[list[str]] = []
        while i < len(lines) and lines[i].strip() and "|" in lines[i]:
            rows.append(_split_row(lines[i]))
            i += 1
        width = len(header)

        def cell(tag: str, index: int, value: str) -> str:
            align = alignments[index] if index < len(alignments) else ""
            attribute = f' style="text-align:{align}"' if align else ""
            return f"<{tag}{attribute}>{self.render_inline(value.strip())}</{tag}>"

        parts = ["<div class=\"table-wrap\"><table>", "<thead><tr>"]
        parts.extend(cell("th", index, value) for index, value in enumerate(header))
        parts.append("</tr></thead>")
        if rows:
            parts.append("<tbody>")
            for row in rows:
                row = (row + [""] * width)[:width]
                parts.append("<tr>")
                parts.extend(cell("td", index, value) for index, value in enumerate(row))
                parts.append("</tr>")
            parts.append("</tbody>")
        parts.append("</table></div>")
        out.append("".join(parts))
        return i

    def _list(self, lines, i, out) -> int:
        first = LIST_RE.match(lines[i])
        ordered = first.group(2)[-1] in ".)"
        base_indent = len(first.group(1))
        items: list[list[str]] = []
        loose = False
        start = int(first.group(2)[:-1]) if ordered else 1

        def continues_list(index: int) -> bool:
            if index >= len(lines):
                return False
            candidate = LIST_RE.match(lines[index])
            return bool(
                candidate
                and _list_item_has_content(candidate)
                and len(candidate.group(1)) == base_indent
                and (candidate.group(2)[-1] in ".)") == ordered
            )

        while continues_list(i):
            match = LIST_RE.match(lines[i])
            marker_width = len(match.group(1)) + len(match.group(2)) + len(match.group(3))
            content_indent = marker_width if match.group(3) else marker_width + 1
            item = [match.group(4)]
            i += 1
            saw_blank = False
            while i < len(lines):
                line = lines[i]
                if not line.strip():
                    item.append("")
                    saw_blank = True
                    i += 1
                    continue
                indent = len(line) - len(line.lstrip(" "))
                if indent >= content_indent:
                    item.append(line[content_indent:])
                    i += 1
                    continue
                nested = LIST_RE.match(line)
                if (
                    nested
                    and _list_item_has_content(nested)
                    and len(nested.group(1)) > base_indent
                    and not saw_blank
                ):
                    # A deeper marker without the full content indent still
                    # nests (`- a` followed by `  - b`).
                    item.append(line[min(indent, content_indent):])
                    i += 1
                    continue
                if not saw_blank and not _starts_block(line):
                    item.append(line.strip())  # lazy paragraph continuation
                    i += 1
                    continue
                break
            ends_with_blank = bool(item) and not item[-1].strip()
            while item and not item[-1].strip():
                item.pop()
            if ends_with_blank and continues_list(i):
                loose = True
            if _has_direct_blank_separation(item):
                loose = True
            items.append(item)
        tag = "ol" if ordered else "ul"
        attributes = f' start="{start}"' if ordered and start != 1 else ""
        parts = [f"<{tag}{attributes}>"]
        for item in items:
            parts.append(self._list_item(item, loose))
        parts.append(f"</{tag}>")
        out.append("\n".join(parts))
        return i

    def _list_item(self, item: list[str], loose: bool) -> str:
        classes = ""
        task = TASK_RE.match(item[0]) if item else None
        if task:
            checked = " checked" if task.group(1).lower() == "x" else ""
            item = [task.group(2)] + item[1:]
            checkbox = f'<input type="checkbox" disabled{checked} /> '
            classes = ' class="task"'
        else:
            checkbox = ""
        paragraph_lines: list[str] = []
        rest_start = len(item)
        for index, line in enumerate(item):
            if not line.strip() or (index > 0 and _starts_block(line)):
                rest_start = index
                break
            paragraph_lines.append(line)
        lead = self.render_inline("\n".join(paragraph_lines)) if paragraph_lines else ""
        remainder = self.render_blocks(item[rest_start:]) if rest_start < len(item) else ""
        if loose and lead:
            lead = f"<p>{checkbox}{lead}</p>"
        else:
            lead = f"{checkbox}{lead}"
        body = lead + ("\n" + remainder if remainder else "")
        return f"<li{classes}>{body}</li>"

    def _paragraph(self, lines, i, out) -> int:
        body: list[str] = []
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                break
            if body and _starts_block(line):
                break
            body.append(line.strip())
            i += 1
        inner = self.render_inline("\n".join(body))
        if self.first_paragraph is None:
            self.first_paragraph = _plain_text(inner)
        self.plain_parts.append(_plain_text(inner))
        out.append(f"<p>{inner}</p>")
        return i

    # ------------------------------------------------------------------ inline
    def render_inline(self, text: str) -> str:
        out: list[str] = []
        i = 0
        n = len(text)
        while i < n:
            c = text[i]
            if c == "\\" and i + 1 < n and text[i + 1] in PUNCTUATION:
                out.append(html.escape(text[i + 1], quote=False))
                i += 2
                continue
            if c == "`":
                run = _run_length(text, i, "`")
                close = _find_run(text, i + run, "`", run)
                if close is not None:
                    code = text[i + run:close].strip(" ") if run else ""
                    out.append(f"<code>{html.escape(code, quote=False)}</code>")
                    i = close + run
                else:
                    out.append("`" * run)
                    i += run
                continue
            if c == "!" and i + 1 < n and text[i + 1] == "[":
                link = self._parse_link(text, i + 1)
                if link:
                    label, target, title, end = link
                    href = self._rewrite(target, True)
                    title_attr = f' title="{html.escape(title)}"' if title else ""
                    out.append(
                        f'<img src="{html.escape(href)}" alt="{html.escape(_plain_text(self.render_inline(label)))}"{title_attr} />'
                    )
                    i = end
                    continue
            if c == "[":
                link = self._parse_link(text, i)
                if link:
                    label, target, title, end = link
                    href = self._rewrite(target, False)
                    self.links.append(target)
                    title_attr = f' title="{html.escape(title)}"' if title else ""
                    out.append(
                        f'<a href="{html.escape(href)}"{title_attr}>{self.render_inline(label)}</a>'
                    )
                    i = end
                    continue
            if c == "<":
                consumed = self._angle(text, i, out)
                if consumed:
                    i = consumed
                    continue
                out.append("&lt;")
                i += 1
                continue
            if c in "*_":
                consumed = self._emphasis(text, i, out)
                if consumed:
                    i = consumed
                    continue
                out.append(c)
                i += 1
                continue
            if c == "&":
                entity = ENTITY_RE.match(text, i)
                if entity:
                    out.append(entity.group(0))
                    i = entity.end()
                else:
                    out.append("&amp;")
                    i += 1
                continue
            if c == "h" and text.startswith(("http://", "https://"), i) and (
                i == 0 or not (text[i - 1].isalnum() or text[i - 1] in "/\"'=")
            ):
                match = BARE_URL_RE.match(text, i)
                url = _trim_url(match.group(0))
                if url:
                    self.links.append(url)
                    out.append(f'<a href="{html.escape(url)}">{html.escape(url, quote=False)}</a>')
                    i += len(url)
                    continue
            if c == ">":
                out.append("&gt;")
                i += 1
                continue
            if c == '"':
                out.append("&quot;")
                i += 1
                continue
            out.append(c)
            i += 1
        return "".join(out)

    def _rewrite(self, target: str, is_image: bool) -> str:
        if self.link_rewriter is None:
            return target
        return self.link_rewriter(target, is_image)

    def _parse_link(self, text: str, start: int):
        """Parse ``[label](target "title")`` at ``start``; return parts or None."""
        depth = 0
        i = start
        n = len(text)
        while i < n:
            c = text[i]
            if c == "\\":
                i += 2
                continue
            if c == "`":
                run = _run_length(text, i, "`")
                close = _find_run(text, i + run, "`", run)
                i = close + run if close is not None else i + run
                continue
            if c == "[":
                depth += 1
            elif c == "]":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        else:
            return None
        label = text[start + 1:i]
        if i + 1 >= n or text[i + 1] != "(":
            return None
        j = i + 2
        paren = 1
        while j < n:
            c = text[j]
            if c == "\\":
                j += 2
                continue
            if c == "(":
                paren += 1
            elif c == ")":
                paren -= 1
                if paren == 0:
                    break
            elif c in " \n" and paren == 1:
                break
            j += 1
        else:
            return None
        target = text[i + 2:j]
        title = ""
        if j < n and text[j] in " \n":
            rest = text[j:].lstrip()
            offset = len(text[j:]) - len(rest)
            if rest.startswith(('"', "'")):
                quote = rest[0]
                end_quote = rest.find(quote, 1)
                if end_quote == -1:
                    return None
                title = rest[1:end_quote]
                closing = rest.find(")", end_quote)
                if closing == -1:
                    return None
                j = j + offset + closing
            else:
                return None
        if j >= n or text[j] != ")":
            return None
        if target.startswith("<") and target.endswith(">"):
            target = target[1:-1]
        return label, target.strip(), title, j + 1

    def _angle(self, text: str, i: int, out: list[str]) -> int | None:
        rest = text[i:]
        close = rest.find(">")
        if close == -1:
            return None
        inner = rest[1:close]
        if re.fullmatch(r"(https?://|mailto:)[^\s<>]+", inner):
            href = inner
            label = inner[7:] if inner.startswith("mailto:") else inner
            self.links.append(href)
            out.append(f'<a href="{html.escape(href)}">{html.escape(label, quote=False)}</a>')
            return i + close + 1
        if re.fullmatch(r"[^\s@<>]+@[^\s@<>]+\.[a-zA-Z]{2,}", inner):
            out.append(f'<a href="mailto:{html.escape(inner)}">{html.escape(inner, quote=False)}</a>')
            return i + close + 1
        tag = INLINE_TAG_RE.match(text, i)
        if tag:
            attributes = []
            for name, double, single in INLINE_ATTRIBUTE_RE.findall(tag.group(3)):
                if name.lower() not in ALLOWED_ATTRIBUTES:
                    continue
                value = double if double else single
                if name.lower() in {"href", "src"}:
                    if re.match(r"^\s*(javascript|data|vbscript):", value, re.IGNORECASE):
                        continue
                    value = self._rewrite(value, name.lower() == "src")
                attributes.append(f' {name.lower()}="{html.escape(value)}"')
            closing = "/" if tag.group(1) else ""
            self_closing = " /" if tag.group(4) else ""
            out.append(f"<{closing}{tag.group(2)}{''.join(attributes)}{self_closing}>")
            return tag.end()
        return None

    def _emphasis(self, text: str, i: int, out: list[str]) -> int | None:
        c = text[i]
        run = _run_length(text, i, c)
        if run > 3:
            return None
        n = len(text)
        after = text[i + run] if i + run < n else ""
        before = text[i - 1] if i > 0 else ""
        if not after or after.isspace():
            return None
        if c == "_" and before.isalnum():
            return None
        if run == 3:
            close = self._find_closer(text, i + run, c, 3)
            if close is None:
                return None
            inner = self.render_inline(text[i + run:close])
            out.append(f"<strong><em>{inner}</em></strong>")
            return close + 3
        if run == 2:
            close = self._find_closer(text, i + run, c, 2)
            if close is None:
                return None
            inner = self.render_inline(text[i + run:close])
            out.append(f"<strong>{inner}</strong>")
            return close + 2
        close = self._find_closer(text, i + run, c, 1)
        if close is None:
            return None
        inner = self.render_inline(text[i + run:close])
        out.append(f"<em>{inner}</em>")
        return close + 1

    def _find_closer(self, text: str, start: int, c: str, width: int) -> int | None:
        i = start
        n = len(text)
        while i < n:
            ch = text[i]
            if ch == "\\":
                i += 2
                continue
            if ch == "`":
                run = _run_length(text, i, "`")
                close = _find_run(text, i + run, "`", run)
                i = close + run if close is not None else i + run
                continue
            if ch == c:
                run = _run_length(text, i, c)
                before = text[i - 1] if i > 0 else ""
                after = text[i + run] if i + run < n else ""
                if (
                    run >= width
                    and before
                    and not before.isspace()
                    and not (c == "_" and after.isalnum())
                ):
                    if run == width:
                        return i
                    if run > width:
                        # ``**bold *both***`` style nesting: take the last
                        # ``width`` characters of the run as our closer only
                        # when nothing opens inside; otherwise prefer the run
                        # start so inner emphasis resolves first.
                        return i + run - width if width == 1 else i
                i += run
                continue
            i += 1
        return None


# ---------------------------------------------------------------- helpers

def _strip_front_matter(lines: list[str]) -> list[str]:
    if lines and lines[0].strip() == "---":
        for index in range(1, len(lines)):
            if lines[index].strip() in {"---", "..."}:
                return lines[index + 1:]
    return lines


def _skip_comment(lines: list[str], i: int) -> int:
    while i < len(lines):
        if COMMENT_END in lines[i]:
            return i + 1
        i += 1
    return i


def _has_direct_blank_separation(item: list[str]) -> bool:
    """True when a blank line separates two direct child blocks of an item.

    Blank lines that only sit inside a nested list (their following line is
    indented) do not make the parent list loose.
    """
    for index, line in enumerate(item[:-1]):
        if line.strip():
            continue
        following = next((text for text in item[index + 1:] if text.strip()), None)
        if following is not None and not following.startswith(" "):
            return True
    return False


def _list_item_has_content(match: re.Match) -> bool:
    # ``-`` alone on a line is an empty item; ``---`` is a rule (handled earlier).
    return bool(match.group(3)) or not match.group(4)


def _starts_block(line: str) -> bool:
    if FENCE_RE.match(line) or HEADING_RE.match(line) or HR_RE.match(line):
        return True
    if QUOTE_RE.match(line):
        return True
    match = LIST_RE.match(line)
    if match and _list_item_has_content(match):
        return True
    return line.lstrip().startswith(COMMENT_START)


def _is_table_start(lines: list[str], i: int) -> bool:
    if "|" not in lines[i] or i + 1 >= len(lines):
        return False
    if not TABLE_DELIMITER_RE.match(lines[i + 1]):
        return False
    return len(_split_row(lines[i])) == len(_split_row(lines[i + 1]))


def _split_row(line: str) -> list[str]:
    cells: list[str] = []
    current: list[str] = []
    in_code = 0
    i = 0
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    n = len(line)
    while i < n:
        c = line[i]
        if c == "\\" and i + 1 < n and line[i + 1] == "|":
            current.append("|")
            i += 2
            continue
        if c == "`":
            run = _run_length(line, i, "`")
            if in_code == 0:
                in_code = run
            elif in_code == run:
                in_code = 0
            current.append("`" * run)
            i += run
            continue
        if c == "|" and in_code == 0:
            cells.append("".join(current))
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    cells.append("".join(current))
    return cells


def _alignment(cell: str) -> str:
    cell = cell.strip()
    left = cell.startswith(":")
    right = cell.endswith(":")
    if left and right:
        return "center"
    if right:
        return "right"
    if left:
        return "left"
    return ""


def _raw_html_block(line: str) -> str | None:
    stripped = line.strip()
    if not stripped.startswith("<"):
        return None
    if re.fullmatch(r'<a id="[\w.-]+"></a>', stripped):
        return stripped
    return None


def _run_length(text: str, i: int, c: str) -> int:
    j = i
    while j < len(text) and text[j] == c:
        j += 1
    return j - i


def _find_run(text: str, start: int, c: str, width: int) -> int | None:
    i = start
    n = len(text)
    while i < n:
        if text[i] == c:
            run = _run_length(text, i, c)
            if run == width:
                return i
            i += run
        else:
            i += 1
    return None


def _trim_url(url: str) -> str:
    while url and url[-1] in ".,;:!?'\"":
        url = url[:-1]
    while url.endswith(")") and url.count("(") < url.count(")"):
        url = url[:-1]
    return url


TAG_RE = re.compile(r"<[^>]+>")


def _plain_text(rendered: str) -> str:
    return html.unescape(TAG_RE.sub("", rendered)).strip()


def _slugify(text: str) -> str:
    slug = SLUG_STRIP_RE.sub("", text.strip().lower()).replace(" ", "-")
    return slug or "section"


def slugify(text: str) -> str:
    """GitHub-style anchor slug for a heading's plain text."""
    return _slugify(text)


def render(text: str, *, link_rewriter: LinkRewriter | None = None) -> RenderedDocument:
    renderer = _Renderer(link_rewriter)
    body = renderer.render_document(text)
    title = renderer.title or ""
    description = renderer.first_paragraph or ""
    return RenderedDocument(
        html=body,
        title=title,
        description=description,
        headings=renderer.headings,
        links=renderer.links,
        plain_text=" ".join(part for part in renderer.plain_parts if part),
    )
