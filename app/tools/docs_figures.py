from __future__ import annotations

import html
import json
import re
import tomllib


DATA_LANGUAGES = {"json": "JSON", "toml": "TOML", "ini": "INI", "dotenv": "Environment"}
DIAGRAM_LANGUAGES = {"", "text", "txt", "ascii"}
DIAGRAM_RE = re.compile(r"[─-╿▲▶▼◀►←-↓]")
PLACEHOLDER_RE = re.compile(r"^<[^<>]+>$")
CONFIG_SECTION_RE = re.compile(r"^\[\s*([^\]]+?)\s*\]$")
CONFIG_PAIR_RE = re.compile(r"^(?:export\s+)?([A-Za-z0-9_.%-]+)\s*[=:]\s*(.*)$")
FLOW_ARROW_RE = re.compile(r"\s*->\s*")
MAX_DEPTH = 6
MAX_TABLE_COLUMNS = 8


def _e(text: str) -> str:
    return html.escape(text, quote=True)


def _plural(count: int, word: str) -> str:
    return f"{count} {word}{'' if count == 1 else 's'}"


def _scalar(value) -> str:
    if value is None:
        return '<span class="dp-v dp-v-null">null</span>'
    if isinstance(value, bool):
        state = "true" if value else "false"
        return f'<span class="dp-v dp-v-bool dp-v-{state}">{state}</span>'
    if isinstance(value, (int, float)):
        return f'<span class="dp-v dp-v-num">{_e(json.dumps(value))}</span>'
    text = str(value)
    if PLACEHOLDER_RE.match(text):
        return f'<span class="dp-v dp-v-hole">{_e(text)}</span>'
    if text == "":
        return '<span class="dp-v dp-v-empty">empty</span>'
    return f'<span class="dp-v dp-v-str">{_e(text)}</span>'


def _is_scalar(value) -> bool:
    return not isinstance(value, (dict, list))


def _record_table(rows: list[dict], depth: int) -> str:
    columns: list[str] = []
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    parts = ['<div class="dp-data-table"><table><thead><tr>']
    parts.extend(f'<th scope="col"><code>{_e(str(key))}</code></th>' for key in columns)
    parts.append("</tr></thead><tbody>")
    for row in rows:
        parts.append("<tr>")
        for key in columns:
            cell = _value(row[key], depth + 1) if key in row else '<span class="dp-v dp-v-missing">—</span>'
            parts.append(f'<td data-label="{_e(str(key))}">{cell}</td>')
        parts.append("</tr>")
    parts.append("</tbody></table></div>")
    return "".join(parts)


def _value(value, depth: int = 0) -> str:
    if _is_scalar(value):
        return _scalar(value)
    if depth >= MAX_DEPTH:
        return f'<code class="dp-v-json">{_e(json.dumps(value, ensure_ascii=False))}</code>'
    if isinstance(value, dict):
        if not value:
            return '<span class="dp-v dp-v-empty">empty object</span>'
        rows = []
        for key, item in value.items():
            nested = " dp-kv-nested" if not _is_scalar(item) and item else ""
            rows.append(
                f'<div class="dp-kv-row{nested}"><dt><code>{_e(str(key))}</code></dt>'
                f"<dd>{_value(item, depth + 1)}</dd></div>"
            )
        return f'<dl class="dp-kv">{"".join(rows)}</dl>'
    if not value:
        return '<span class="dp-v dp-v-empty">empty list</span>'
    if all(_is_scalar(item) for item in value):
        items = "".join(f"<li>{_scalar(item)}</li>" for item in value)
        return f'<ul class="dp-chips">{items}</ul>'
    if all(isinstance(item, dict) and item for item in value):
        keys = {key for item in value for key in item}
        if len(keys) <= MAX_TABLE_COLUMNS and (len(value) > 1 or len(keys) > 1):
            return _record_table(value, depth)
    items = "".join(f"<li>{_value(item, depth + 1)}</li>" for item in value)
    return f'<ol class="dp-data-list">{items}</ol>'


def _summary(value) -> str:
    if isinstance(value, dict):
        return "Object · " + _plural(len(value), "field")
    if isinstance(value, list):
        return "List · " + _plural(len(value), "item")
    return "Value"


def _parse_json(source: str):
    for candidate in (source, "{" + source.strip().rstrip(",") + "}"):
        try:
            value = json.loads(candidate)
        except ValueError:
            continue
        if isinstance(value, (dict, list)) and value:
            return value
    return None


def _strip_quotes(text: str) -> str:
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    return text


def _config_value(text: str):
    lowered = text.lower()
    if lowered in {"true", "false"}:
        return lowered == "true"
    if re.fullmatch(r"-?\d+(\.\d+)?", text):
        return float(text) if "." in text else int(text)
    return _strip_quotes(text)


def _split_inline_comment(text: str) -> tuple[str, str]:
    quote = ""
    for index, char in enumerate(text):
        if quote:
            if char == quote:
                quote = ""
        elif char in "\"'":
            quote = char
        elif char == "#" and (index == 0 or text[index - 1].isspace()):
            return text[:index].rstrip(), text[index + 1:].strip()
    return text, ""


def _parse_config(language: str, source: str):
    if language == "toml":
        try:
            tomllib.loads(source)
        except tomllib.TOMLDecodeError:
            return None
    sections: list[tuple[str, list[tuple[str, object, str]]]] = [("", [])]
    pairs = 0
    for raw in source.split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line[0] in "#;":
            sections[-1][1].append(("", None, line.lstrip("#;").strip()))
            continue
        section = CONFIG_SECTION_RE.match(line)
        if section and language != "dotenv":
            sections.append((section.group(1), []))
            continue
        pair = CONFIG_PAIR_RE.match(line)
        if not pair:
            return None
        value, note = pair.group(2).strip(), ""
        if language in {"toml", "dotenv"}:
            value, note = _split_inline_comment(value)
        sections[-1][1].append((pair.group(1), _config_value(value), note))
        pairs += 1
    if not pairs:
        return None
    return [(name, rows) for name, rows in sections if rows]


def _config_view(sections) -> str:
    parts: list[str] = []
    for name, rows in sections:
        body: list[str] = []
        for key, value, note in rows:
            note_html = f'<small class="dp-kv-note">{_e(note)}</small>' if note else ""
            if not key:
                body.append(f'<div class="dp-kv-row dp-kv-comment"><dd>{note_html}</dd></div>')
                continue
            body.append(
                f'<div class="dp-kv-row"><dt><code>{_e(key)}</code></dt>'
                f"<dd>{_scalar(value)}{note_html}</dd></div>"
            )
        title = f'<p class="dp-conf-name"><code>[{_e(name)}]</code></p>' if name else ""
        parts.append(f'<div class="dp-conf-section">{title}<dl class="dp-kv">{"".join(body)}</dl></div>')
    return "".join(parts)


def _config_summary(sections) -> str:
    named = sum(1 for name, _ in sections if name)
    settings = sum(1 for _, rows in sections for key, _, _ in rows if key)
    pieces = [_plural(named, "section")] if named else []
    pieces.append(_plural(settings, "setting"))
    return " · ".join(pieces)


def _figure_head(kind: str, summary: str, label: str) -> str:
    return (
        f'<figcaption class="dp-figure-head"><span class="dp-figure-kind">{_e(kind)}</span>'
        f'<span class="dp-figure-summary">{_e(summary)}</span>'
        f'<span class="dp-figure-tools" data-figure-tools data-figure-label="{_e(label)}"></span></figcaption>'
    )


def data_figure(language: str, source: str, code_html: str) -> str | None:
    language = language.lower()
    if language not in DATA_LANGUAGES:
        return None
    if language == "json":
        value = _parse_json(source)
        if value is None:
            return None
        view, summary = _value(value), _summary(value)
    else:
        sections = _parse_config(language, source)
        if sections is None:
            return None
        view, summary = _config_view(sections), _config_summary(sections)
    kind = DATA_LANGUAGES[language]
    return (
        f'<figure class="dp-figure dp-data" data-format="{_e(language)}">'
        + _figure_head(kind, summary, kind)
        + f'<div class="dp-data-view">{view}</div>'
        + f'<details class="dp-data-source"><summary>Show {_e(kind)} source</summary>{code_html}</details>'
        + "</figure>"
    )


def diagram_figure(language: str, source: str, code_html: str) -> str | None:
    if language.lower() not in DIAGRAM_LANGUAGES:
        return None
    lines = [line for line in source.split("\n") if line.strip()]
    if len(lines) < 3 or not DIAGRAM_RE.search(source):
        return None
    width = max(len(line) for line in lines)
    return (
        f'<figure class="dp-figure dp-diagram" data-diagram style="--dp-diagram-columns:{width}">'
        + _figure_head("Diagram", _plural(len(lines), "line"), "Diagram")
        + f'<div class="dp-diagram-canvas">{code_html}</div>'
        + "</figure>"
    )


def parse_flow(source: str) -> tuple[str, list[str], list[tuple[int, int]]] | None:
    title = ""
    nodes: list[str] = []
    index: dict[str, int] = {}
    edges: list[tuple[int, int]] = []

    def node(name: str) -> int:
        if name not in index:
            index[name] = len(nodes)
            nodes.append(name)
        return index[name]

    for raw in source.split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.lower().startswith("title:"):
            title = line[len("title:"):].strip()
            continue
        names = [part.strip() for part in FLOW_ARROW_RE.split(line)]
        if any(not name for name in names):
            return None
        ids = [node(name) for name in names]
        for left, right in zip(ids, ids[1:]):
            if (left, right) not in edges:
                edges.append((left, right))
    if not edges:
        return None
    return title, nodes, edges


def flow_ranks(count: int, edges: list[tuple[int, int]]) -> list[int]:
    outgoing: dict[int, list[int]] = {node: [] for node in range(count)}
    for source, target in edges:
        outgoing[source].append(target)
    state = [0] * count
    back: set[tuple[int, int]] = set()

    def walk(node: int) -> None:
        state[node] = 1
        for target in outgoing[node]:
            if state[target] == 1:
                back.add((node, target))
            elif state[target] == 0:
                walk(target)
        state[node] = 2

    for node in range(count):
        if state[node] == 0:
            walk(node)
    incoming: dict[int, list[int]] = {node: [] for node in range(count)}
    for source, target in edges:
        if (source, target) not in back:
            incoming[target].append(source)
    ranks: list[int | None] = [None] * count

    def rank(node: int) -> int:
        if ranks[node] is None:
            ranks[node] = max((rank(source) + 1 for source in incoming[node]), default=0)
        return ranks[node]

    return [rank(node) for node in range(count)]


def flow_figure(source: str, code_html: str) -> str | None:
    parsed = parse_flow(source)
    if parsed is None:
        return None
    title, nodes, edges = parsed
    ranks = flow_ranks(len(nodes), edges)
    rows: list[list[int]] = [[] for _ in range(max(ranks) + 1)]
    for node, rank in enumerate(ranks):
        rows[rank].append(node)
    parts = [
        '<figure class="dp-figure dp-flow" data-flow>',
        _figure_head("Diagram", title or _plural(len(nodes), "step"), title or "Diagram"),
        '<div class="dp-flow-canvas" data-flow-canvas><div class="dp-flow-ranks">',
    ]
    for row in rows:
        parts.append('<div class="dp-flow-rank">')
        parts.extend(
            f'<span class="dp-flow-node" data-flow-node="{node}">{_e(nodes[node])}</span>' for node in row
        )
        parts.append("</div>")
    parts.append('</div></div><ul class="dp-flow-edges">')
    parts.extend(
        f'<li data-from="{left}" data-to="{right}">{_e(nodes[left])} → {_e(nodes[right])}</li>'
        for left, right in edges
    )
    parts.append(f'</ul><details class="dp-data-source"><summary>Show diagram source</summary>{code_html}</details>')
    parts.append("</figure>")
    return "".join(parts)


def code_figure(language: str, source: str, code_html: str) -> str | None:
    if language.lower() == "flow":
        return flow_figure(source, code_html)
    return data_figure(language, source, code_html) or diagram_figure(language, source, code_html)
