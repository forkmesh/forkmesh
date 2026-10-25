#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[2]
TEMPLATES = REPOSITORY / "www" / "templates"
PAGES = TEMPLATES / "pages"
PUBLIC = REPOSITORY / "www" / "public"

TAG_LINE_RE = re.compile(r"^[ \t]*(\{%[^%\n]*%\})[ \t]*\n", re.M)
TOKEN_RE = re.compile(r"\{%\s*(.*?)\s*%\}|\{\{\s*(.*?)\s*\}\}", re.S)
NAME_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
QUOTED_RE = re.compile(r'"([^"]*)"\Z')
SET_RE = re.compile(r'set\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"([^"]*)"\Z')


class TemplateError(Exception):
    pass


class Template:
    def __init__(self, name: str, source: str):
        self.name = name
        self.parent: str | None = None
        self.variables: dict[str, str] = {}
        self.blocks: dict[str, list] = {}
        tokens = self._tokens(TAG_LINE_RE.sub(r"\1", source))
        self.nodes = self._parse(tokens, ())
        if tokens:
            raise TemplateError(f"{name}: unexpected {{% {tokens[0][1]} %}}")
        if self.parent:
            for node in self.nodes:
                if node[0] == "text" and node[1].strip():
                    raise TemplateError(
                        f"{name}: text outside a block in a template that extends {self.parent}")

    def _tokens(self, source: str) -> list[tuple[str, str]]:
        tokens = []
        position = 0
        for match in TOKEN_RE.finditer(source):
            if match.start() > position:
                tokens.append(("text", source[position:match.start()]))
            if match.group(1) is not None:
                tokens.append(("tag", match.group(1)))
            else:
                tokens.append(("var", match.group(2)))
            position = match.end()
        if position < len(source):
            tokens.append(("text", source[position:]))
        tokens.reverse()
        return tokens

    def _parse(self, tokens: list, until: tuple[str, ...]) -> list:
        nodes = []
        while tokens:
            kind, value = tokens[-1]
            if kind == "tag" and value.split()[0] in until:
                return nodes
            tokens.pop()
            if kind == "text":
                nodes.append(("text", value))
            elif kind == "var":
                if value != "super()" and not NAME_RE.match(value):
                    raise TemplateError(f"{self.name}: bad variable {{{{ {value} }}}}")
                nodes.append(("var", value))
            else:
                nodes.append(self._tag(value, tokens))
        if until:
            raise TemplateError(f"{self.name}: missing {{% {until[-1]} %}}")
        return nodes

    def _tag(self, value: str, tokens: list):
        word, _, rest = value.partition(" ")
        rest = rest.strip()
        if word == "extends":
            if self.parent:
                raise TemplateError(f"{self.name}: extends twice")
            self.parent = self._quoted(rest)
            return ("text", "")
        if word == "include":
            return ("include", self._quoted(rest))
        if word == "set":
            match = SET_RE.match(value)
            if not match:
                raise TemplateError(f"{self.name}: bad {{% {value} %}}")
            self.variables[match.group(1)] = match.group(2)
            return ("text", "")
        if word == "block":
            if not NAME_RE.match(rest):
                raise TemplateError(f"{self.name}: bad block name {rest!r}")
            if rest in self.blocks:
                raise TemplateError(f"{self.name}: block {rest} defined twice")
            body = self._parse(tokens, ("endblock",))
            tokens.pop()
            self.blocks[rest] = body
            return ("block", rest)
        if word == "if":
            if not NAME_RE.match(rest):
                raise TemplateError(f"{self.name}: bad condition {rest!r}")
            then = self._parse(tokens, ("else", "endif"))
            otherwise: list = []
            if tokens.pop()[1] == "else":
                otherwise = self._parse(tokens, ("endif",))
                tokens.pop()
            return ("if", rest, then, otherwise)
        raise TemplateError(f"{self.name}: unknown tag {{% {value} %}}")

    def _quoted(self, text: str) -> str:
        match = QUOTED_RE.match(text)
        if not match:
            raise TemplateError(f"{self.name}: expected a quoted name, got {text!r}")
        return match.group(1)


class Environment:
    def __init__(self, root: Path = TEMPLATES):
        self.root = root
        self.cache: dict[str, Template] = {}

    def load(self, name: str) -> Template:
        if name not in self.cache:
            path = (self.root / name).resolve()
            if self.root.resolve() not in path.parents:
                raise TemplateError(f"{name}: outside {self.root}")
            self.cache[name] = Template(name, path.read_text(encoding="utf-8"))
        return self.cache[name]

    def render(self, name: str, variables: dict[str, str] | None = None) -> str:
        chain = [self.load(name)]
        while chain[-1].parent:
            if len(chain) > 16:
                raise TemplateError(f"{name}: extends loop")
            chain.append(self.load(chain[-1].parent))
        scope: dict[str, str] = {}
        blocks: dict[str, list[list]] = {}
        for template in reversed(chain):
            scope.update(template.variables)
        for template in chain:
            for block, body in template.blocks.items():
                blocks.setdefault(block, []).append(body)
        scope.update(variables or {})
        output: list[str] = []
        self._emit(chain[-1].nodes, scope, blocks, [], output, name, 0)
        return "".join(output)

    def _emit(self, nodes, scope, blocks, supers, output, name, depth):
        if depth > 32:
            raise TemplateError(f"{name}: templates nest too deeply")
        for node in nodes:
            kind = node[0]
            if kind == "text":
                output.append(node[1])
            elif kind == "var":
                if node[1] == "super()":
                    if not supers:
                        raise TemplateError(f"{name}: super() outside an overridden block")
                    self._emit(supers[0], scope, blocks, supers[1:], output, name, depth + 1)
                elif node[1] in scope:
                    output.append(scope[node[1]])
                else:
                    raise TemplateError(f"{name}: undefined variable {node[1]}")
            elif kind == "block":
                definitions = blocks[node[1]]
                self._emit(definitions[0], scope, blocks, definitions[1:], output, name, depth + 1)
            elif kind == "include":
                included = self.load(node[1])
                for block, body in included.blocks.items():
                    blocks.setdefault(block, []).append(body)
                self._emit(included.nodes, scope, blocks, [], output, name, depth + 1)
            elif kind == "if":
                branch = node[2] if scope.get(node[1]) else node[3]
                self._emit(branch, scope, blocks, supers, output, name, depth + 1)


def page_templates() -> list[Path]:
    return sorted(PAGES.rglob("*.html"))


def rendered_pages(environment: Environment | None = None) -> dict[Path, str]:
    environment = environment or Environment()
    pages = {}
    for path in page_templates():
        relative = path.relative_to(TEMPLATES).as_posix()
        pages[PUBLIC / path.relative_to(PAGES)] = environment.render(relative)
    return pages


def stale_pages() -> list[Path]:
    stale = []
    for target, text in rendered_pages().items():
        if not target.exists() or target.read_text(encoding="utf-8") != text:
            stale.append(target)
    return stale


def write_pages() -> list[Path]:
    written = []
    for target, text in rendered_pages().items():
        if target.exists() and target.read_text(encoding="utf-8") == text:
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
        written.append(target)
    return written


def main(arguments: list[str]) -> int:
    if "--check" in arguments:
        stale = stale_pages()
        for path in stale:
            print(f"{path.relative_to(REPOSITORY)} is out of date with www/templates", file=sys.stderr)
        if stale:
            print("run python3 app/tools/site_templates.py", file=sys.stderr)
            return 1
        print(f"{len(page_templates())} pages up to date")
        return 0
    written = write_pages()
    for path in written:
        print(f"rendered {path.relative_to(REPOSITORY)}")
    print(f"{len(page_templates())} pages, {len(written)} rewritten")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
