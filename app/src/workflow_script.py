import contextvars
import json
import math
import re
import signal
import threading
import time
import unicodedata

MAX_STEPS = 100000
MAX_DEPTH = 40
MAX_NESTING = 50
MAX_STRING = 1000000
MAX_ARRAY = 100000
MAX_LOGS = 100
MAX_LOG_CHARS = 1000
REGEX_SECONDS = 2.0
REGEX_BUDGET = contextvars.ContextVar("workflow_regex_budget", default=None)
MAX_INT = 1e18
JS_SPACE = ("\t\n\x0b\x0c\r \xa0       "
            "         　﻿")
LINE_ENDS = "\n\r  "
KEYWORDS = {
    "var", "let", "const", "if", "else", "for", "while", "do", "break",
    "continue", "return", "function", "true", "false", "null", "typeof",
    "instanceof", "new", "throw", "try", "catch", "finally", "switch", "case",
    "default", "delete", "this", "void", "in",
}
UNSUPPORTED = {
    "class", "async", "await", "yield", "import", "export", "with",
    "debugger", "super", "enum", "extends", "static",
}
PUNCTUATORS = (
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=",
    "??=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", "{",
    "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%",
    "&", "|", "^", "!", "~", "?", ":", "=", ".",
)
REGEX_AFTER_KEYWORDS = {
    "return", "typeof", "case", "do", "else", "in", "instanceof", "new",
    "delete", "void", "throw",
}
BINARY = {
    "||": 1, "??": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6,
    "===": 6, "!==": 6, "<": 7, ">": 7, "<=": 7, ">=": 7, "in": 7,
    "instanceof": 7, "<<": 8, ">>": 8, ">>>": 8, "+": 9, "-": 9, "*": 10,
    "/": 10, "%": 10, "**": 11,
}
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=",
          "&=", "|=", "^=", "&&=", "||=", "??="}
DECIMAL_RE = re.compile(
    r"[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?")
FLOAT_PREFIX_RE = re.compile(
    r"[+-]?(?:Infinity|(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)")
INDEX_RE = re.compile(r"0|[1-9][0-9]*")
ISO_DATE_RE = re.compile(
    r"([+-][0-9]{6}|[0-9]{4})(?:-([0-9]{2})(?:-([0-9]{2}))?)?"
    r"(?:T([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?"
    r"(Z|[+-][0-9]{2}:[0-9]{2})?)?")
URI_UNRESERVED = ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                  "0123456789-_.!~*'()")
DAY_NAMES = ("Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat")
MONTH_NAMES = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
               "Oct", "Nov", "Dec")
WORD = "[A-Za-z0-9_]"
DIGITS = tuple("0123456789")


class ScriptError(Exception):
    def __init__(self, line, message):
        Exception.__init__(self, "Line %d: %s" % (line, message))
        self.line = line
        self.text = message


class ScriptLimit(Exception):
    pass


class Thrown(Exception):
    def __init__(self, value, line=0):
        Exception.__init__(self)
        self.value = value
        self.line = line


class _ShortCircuit(Exception):
    pass


class _RegexAlarm(Exception):
    pass


def _alarm(signum, frame):
    raise _RegexAlarm()


def regex_budget():
    return REGEX_BUDGET.set({"left": REGEX_SECONDS})


class GuardedPattern:
    __slots__ = ("pattern",)

    def __init__(self, pattern):
        self.pattern = pattern

    def search(self, text, pos=0):
        return _guarded(self.pattern.search, text, pos)

    def match(self, text, pos=0):
        return _guarded(self.pattern.match, text, pos)


def _guarded(fn, text, pos):
    budget = REGEX_BUDGET.get()
    left = budget["left"] if budget is not None else REGEX_SECONDS
    if left <= 0:
        raise ScriptLimit("Regular expressions took too long in this run")
    if threading.current_thread() is not threading.main_thread() or \
            not hasattr(signal, "setitimer"):
        return fn(text, pos)
    started = time.monotonic()
    previous = signal.signal(signal.SIGALRM, _alarm)
    signal.setitimer(signal.ITIMER_REAL, left)
    try:
        return fn(text, pos)
    except _RegexAlarm:
        if budget is not None:
            budget["left"] = 0
        raise ScriptLimit("Regular expressions took too long in this run")
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous)
        if budget is not None and budget["left"] > 0:
            budget["left"] -= time.monotonic() - started


class _Undefined:
    __slots__ = ()

    def __repr__(self):
        return "undefined"


UNDEFINED = _Undefined()
BREAK = object()
CONTINUE = object()


class Return:
    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value


class Function:
    __slots__ = ("name", "params", "rest", "body", "scope", "arrow",
                 "expression", "this")

    def __init__(self, name, params, rest, body, scope, arrow, expression,
                 this=UNDEFINED):
        self.name = name
        self.params = params
        self.rest = rest
        self.body = body
        self.scope = scope
        self.arrow = arrow
        self.expression = expression
        self.this = this


class Native:
    __slots__ = ("name", "fn", "props", "construct")

    def __init__(self, name, fn, props=None, construct=None):
        self.name = name
        self.fn = fn
        self.props = props or {}
        self.construct = construct


class JSDate:
    __slots__ = ("ms",)

    def __init__(self, ms):
        self.ms = time_clip(ms)


class JSRegExp:
    __slots__ = ("source", "flags", "compiled", "last_index", "names")

    def __init__(self, source, flags, compiled, names):
        self.source = source
        self.flags = flags
        self.compiled = compiled
        self.last_index = 0
        self.names = names


class MatchArray(list):
    __slots__ = ("index", "input", "groups")


class JSCollection:
    __slots__ = ("kind", "keys", "values", "index")

    def __init__(self, kind):
        self.kind = kind
        self.keys = []
        self.values = []
        self.index = {}

    def find(self, key):
        return self.index.get(_collection_key(key))

    def put(self, key, value):
        marker = _collection_key(key)
        position = self.index.get(marker)
        if position is None:
            if len(self.keys) >= MAX_ARRAY:
                raise range_error("Invalid array length")
            self.index[marker] = len(self.keys)
            self.keys.append(0.0 if isinstance(key, float) and key == 0 else key)
            self.values.append(value)
        else:
            self.values[position] = value

    def remove(self, key):
        position = self.find(key)
        if position is None:
            return False
        del self.keys[position]
        del self.values[position]
        self.index = {_collection_key(k): i for i, k in enumerate(self.keys)}
        return True

    def entries(self):
        if self.kind == "Set":
            return list(self.keys)
        return [[key, value] for key, value in zip(self.keys, self.values)]


def _collection_key(value):
    if isinstance(value, float):
        if value != value:
            return ("nan",)
        return ("n", value + 0.0)
    if isinstance(value, (str, bool)):
        return (type(value).__name__, value)
    if value is None:
        return ("null",)
    if value is UNDEFINED:
        return ("undefined",)
    return ("object", id(value))


def time_clip(ms):
    if ms != ms or ms in (math.inf, -math.inf) or abs(ms) > 8.64e15:
        return math.nan
    return float(math.trunc(ms)) + 0.0


class Token:
    __slots__ = ("kind", "value", "line", "newline", "extra")

    def __init__(self, kind, value, line, newline, extra=None):
        self.kind = kind
        self.value = value
        self.line = line
        self.newline = newline
        self.extra = extra


def _is_id_start(ch):
    return ch == "_" or ch == "$" or ch.isalpha()


def _is_id_part(ch):
    return ch == "_" or ch == "$" or ch.isalnum()


def _combine_surrogates(text):
    if not any("\ud800" <= ch <= "\udfff" for ch in text):
        return text
    return text.encode("utf-16", "surrogatepass").decode("utf-16", "replace")


class Lexer:
    def __init__(self, source, line=1):
        self.src = source
        self.pos = 0
        self.line = line
        self.tokens = []

    def error(self, message):
        raise ScriptError(self.line, message)

    def skip_space(self, newline):
        src = self.src
        while self.pos < len(src):
            ch = src[self.pos]
            if ch in LINE_ENDS:
                if not (ch == "\r" and src[self.pos + 1:self.pos + 2] == "\n"):
                    self.line += 1
                newline = True
                self.pos += 1
            elif ch in JS_SPACE:
                self.pos += 1
            elif src.startswith("//", self.pos):
                while self.pos < len(src) and src[self.pos] not in LINE_ENDS:
                    self.pos += 1
            elif src.startswith("/*", self.pos):
                end = src.find("*/", self.pos + 2)
                if end < 0:
                    self.error("Unterminated comment")
                for index in range(self.pos, end):
                    c = src[index]
                    if c in LINE_ENDS and not (
                            c == "\r" and src[index + 1:index + 2] == "\n"):
                        self.line += 1
                        newline = True
                self.pos = end + 2
            else:
                break
        return newline

    def regex_allowed(self):
        if not self.tokens:
            return True
        last = self.tokens[-1]
        if last.kind == "punct":
            return last.value not in (")", "]", "}")
        if last.kind == "kw":
            return last.value in REGEX_AFTER_KEYWORDS
        return False

    def number(self):
        src = self.src
        start = self.pos
        if src[start] == "0" and src[start + 1:start + 2] in tuple("xXoObB"):
            base = {"x": 16, "o": 8, "b": 2}[src[start + 1].lower()]
            self.pos += 2
            digits = ""
            while self.pos < len(src) and (src[self.pos] == "_" or (
                    src[self.pos].lower() in "0123456789abcdef"[:base])):
                digits += src[self.pos]
                self.pos += 1
            digits = digits.replace("_", "")
            if not digits:
                self.error("Invalid number")
            value = float(int(digits, base))
        else:
            while self.pos < len(src) and (src[self.pos] in "0123456789_"):
                self.pos += 1
            if src[self.pos:self.pos + 1] == ".":
                self.pos += 1
                while self.pos < len(src) and src[self.pos] in "0123456789_":
                    self.pos += 1
            if src[self.pos:self.pos + 1] in ("e", "E"):
                mark = self.pos
                self.pos += 1
                if src[self.pos:self.pos + 1] in ("+", "-"):
                    self.pos += 1
                if src[self.pos:self.pos + 1] in DIGITS:
                    while self.pos < len(src) and src[self.pos] in "0123456789":
                        self.pos += 1
                else:
                    self.pos = mark
            text = src[start:self.pos].replace("_", "")
            value = float(text)
        if self.pos < len(src) and (_is_id_start(src[self.pos])
                                    or src[self.pos] in "0123456789"):
            self.error("Invalid number")
        return value

    def escape(self, quote):
        src = self.src
        self.pos += 1
        if self.pos >= len(src):
            self.error("Unterminated string")
        ch = src[self.pos]
        self.pos += 1
        simple = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f",
                  "v": "\x0b"}
        if ch in simple:
            return simple[ch]
        if ch == "0" and src[self.pos:self.pos + 1] not in DIGITS:
            return "\x00"
        if ch == "x":
            digits = src[self.pos:self.pos + 2]
            if len(digits) != 2 or any(c not in "0123456789abcdefABCDEF"
                                       for c in digits):
                self.error("Invalid escape")
            self.pos += 2
            return chr(int(digits, 16))
        if ch == "u":
            if src[self.pos:self.pos + 1] == "{":
                end = src.find("}", self.pos)
                digits = src[self.pos + 1:end] if end > 0 else ""
                if not digits or any(c not in "0123456789abcdefABCDEF"
                                     for c in digits) or int(digits, 16) > 0x10FFFF:
                    self.error("Invalid escape")
                self.pos = end + 1
                return chr(int(digits, 16))
            digits = src[self.pos:self.pos + 4]
            if len(digits) != 4 or any(c not in "0123456789abcdefABCDEF"
                                       for c in digits):
                self.error("Invalid escape")
            self.pos += 4
            return chr(int(digits, 16))
        if ch in LINE_ENDS:
            if ch == "\r" and src[self.pos:self.pos + 1] == "\n":
                self.pos += 1
            self.line += 1
            return ""
        return ch

    def string(self, quote):
        src = self.src
        self.pos += 1
        out = []
        while True:
            if self.pos >= len(src) or src[self.pos] in "\n\r":
                self.error("Unterminated string")
            ch = src[self.pos]
            if ch == quote:
                self.pos += 1
                return _combine_surrogates("".join(out))
            if ch == "\\":
                out.append(self.escape(quote))
            else:
                out.append(ch)
                self.pos += 1

    def template(self):
        src = self.src
        self.pos += 1
        strings = []
        parts = []
        out = []
        while True:
            if self.pos >= len(src):
                self.error("Unterminated template")
            ch = src[self.pos]
            if ch == "`":
                self.pos += 1
                strings.append(_combine_surrogates("".join(out)))
                return strings, parts
            if ch == "\\":
                out.append(self.escape("`"))
                continue
            if ch == "$" and src[self.pos + 1:self.pos + 2] == "{":
                strings.append(_combine_surrogates("".join(out)))
                out = []
                self.pos += 2
                line = self.line
                inner = Lexer(src, line)
                inner.pos = self.pos
                depth = 0
                newline = False
                while True:
                    newline = inner.skip_space(newline)
                    if inner.pos >= len(src):
                        self.error("Unterminated template")
                    if src[inner.pos] == "}" and depth == 0:
                        break
                    inner.step_one(newline)
                    newline = False
                    token = inner.tokens[-1]
                    if token.kind == "punct" and token.value == "{":
                        depth += 1
                    elif token.kind == "punct" and token.value == "}":
                        depth -= 1
                inner.tokens.append(Token("eof", None, inner.line, True))
                parts.append(inner.tokens)
                self.pos = inner.pos + 1
                self.line = inner.line
                continue
            if ch in LINE_ENDS:
                if not (ch == "\r" and src[self.pos + 1:self.pos + 2] == "\n"):
                    self.line += 1
                if ch == "\r":
                    if src[self.pos + 1:self.pos + 2] == "\n":
                        self.pos += 1
                    ch = "\n"
            out.append(ch)
            self.pos += 1

    def step_one(self, newline):
        src = self.src
        ch = src[self.pos]
        line = self.line
        if ch in "0123456789" or (
                ch == "." and src[self.pos + 1:self.pos + 2] in DIGITS):
            token = Token("num", self.number(), line, newline)
        elif ch in "\"'":
            token = Token("str", self.string(ch), line, newline)
        elif ch == "`":
            strings, parts = self.template()
            token = Token("tmpl", strings, line, newline, parts)
        elif _is_id_start(ch):
            name = self.identifier()
            if name in UNSUPPORTED:
                self.error(name + " is not supported in scripts")
            token = Token("kw" if name in KEYWORDS else "ident", name, line,
                          newline)
        elif ch == "/" and self.regex_allowed():
            source, flags = self.regex()
            token = Token("regex", source, line, newline, flags)
        else:
            for punct in PUNCTUATORS:
                if src.startswith(punct, self.pos):
                    if punct == "?." and src[self.pos + 2:self.pos + 3] in DIGITS:
                        continue
                    self.pos += len(punct)
                    token = Token("punct", punct, line, newline)
                    break
            else:
                self.error("Unexpected character " + ch)
        self.tokens.append(token)

    def identifier(self):
        src = self.src
        start = self.pos
        if src[self.pos] == "\\":
            self.error("Unexpected character \\")
        while self.pos < len(src) and _is_id_part(src[self.pos]):
            self.pos += 1
        return src[start:self.pos]

    def regex(self):
        src = self.src
        self.pos += 1
        start = self.pos
        in_class = False
        while True:
            if self.pos >= len(src) or src[self.pos] in LINE_ENDS:
                self.error("Unterminated regular expression")
            ch = src[self.pos]
            if ch == "\\":
                self.pos += 2
                continue
            if ch == "[":
                in_class = True
            elif ch == "]":
                in_class = False
            elif ch == "/" and not in_class:
                break
            self.pos += 1
        source = src[start:self.pos]
        self.pos += 1
        flag_start = self.pos
        while self.pos < len(src) and _is_id_part(src[self.pos]):
            self.pos += 1
        return source, src[flag_start:self.pos]


def _lex_all(source):
    lexer = Lexer(source)
    newline = False
    while True:
        newline = lexer.skip_space(newline)
        if lexer.pos >= len(source):
            lexer.tokens.append(Token("eof", None, lexer.line, True))
            return lexer.tokens
        lexer.step_one(newline)
        newline = False


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.index = 0
        self.nesting = 0
        self.loops = 0
        self.switches = 0
        self.functions = 0
        self.shorthand = {}

    @property
    def tok(self):
        return self.tokens[self.index]

    def peek(self, offset=1):
        index = min(self.index + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def error(self, token=None):
        token = token or self.tok
        if token.kind == "eof":
            raise ScriptError(token.line, "Unexpected end of script")
        if token.kind == "str":
            text = "string"
        elif token.kind == "num":
            text = "number"
        elif token.kind == "tmpl":
            text = "template"
        elif token.kind == "regex":
            text = "regular expression"
        else:
            text = str(token.value)
        raise ScriptError(token.line, "Unexpected token " + text)

    def is_punct(self, value, token=None):
        token = token or self.tok
        return token.kind == "punct" and token.value == value

    def is_kw(self, value, token=None):
        token = token or self.tok
        return token.kind == "kw" and token.value == value

    def advance(self):
        token = self.tokens[self.index]
        if self.index < len(self.tokens) - 1:
            self.index += 1
        return token

    def expect(self, value):
        if not self.is_punct(value):
            self.error()
        return self.advance()

    def eat(self, value):
        if self.is_punct(value):
            self.advance()
            return True
        return False

    def semicolon(self):
        if self.eat(";"):
            return
        if self.is_punct("}") or self.tok.kind == "eof" or self.tok.newline:
            return
        self.error()

    def enter(self):
        self.nesting += 1
        if self.nesting > MAX_NESTING:
            raise ScriptError(self.tok.line, "The script nests too deeply")

    def leave(self):
        self.nesting -= 1

    def program(self):
        body = []
        while self.tok.kind != "eof":
            body.append(self.statement())
        return body

    def statement(self):
        self.enter()
        try:
            return self._statement()
        finally:
            self.leave()

    def _statement(self):
        tok = self.tok
        line = tok.line
        if tok.kind == "punct":
            if tok.value == "{":
                return ("block", line, self.block())
            if tok.value == ";":
                self.advance()
                return ("empty", line)
        if tok.kind == "kw":
            word = tok.value
            if word in ("var", "let", "const"):
                node = self.declaration()
                self.semicolon()
                return node
            if word == "function":
                self.advance()
                fn = self.function_rest(line, True)
                return ("funcdecl", line, fn[2], fn)
            if word == "if":
                self.advance()
                self.expect("(")
                test = self.expression()
                self.expect(")")
                then = self.statement()
                other = None
                if self.is_kw("else"):
                    self.advance()
                    other = self.statement()
                return ("if", line, test, then, other)
            if word == "for":
                return self.for_statement()
            if word == "while":
                self.advance()
                self.expect("(")
                test = self.expression()
                self.expect(")")
                body = self.loop_body()
                return ("while", line, test, body)
            if word == "do":
                self.advance()
                body = self.loop_body()
                if not self.is_kw("while"):
                    self.error()
                self.advance()
                self.expect("(")
                test = self.expression()
                self.expect(")")
                self.eat(";")
                return ("dowhile", line, body, test)
            if word == "return":
                self.advance()
                value = None
                if not (self.is_punct(";") or self.is_punct("}")
                        or self.tok.kind == "eof" or self.tok.newline):
                    value = self.expression()
                self.semicolon()
                return ("return", line, value)
            if word in ("break", "continue"):
                self.advance()
                if word == "break" and not (self.loops or self.switches):
                    raise ScriptError(line, "Illegal break statement")
                if word == "continue" and not self.loops:
                    raise ScriptError(line, "Illegal continue statement")
                self.semicolon()
                return (word, line)
            if word == "throw":
                self.advance()
                if self.tok.newline:
                    raise ScriptError(line, "Illegal newline after throw")
                value = self.expression()
                self.semicolon()
                return ("throw", line, value)
            if word == "try":
                return self.try_statement()
            if word == "switch":
                return self.switch_statement()
        expr = self.expression()
        self.semicolon()
        return ("expr", line, expr)

    def loop_body(self):
        self.loops += 1
        try:
            return self.statement()
        finally:
            self.loops -= 1

    def block(self):
        self.expect("{")
        body = []
        while not self.is_punct("}"):
            if self.tok.kind == "eof":
                self.error()
            body.append(self.statement())
        self.advance()
        return body

    def declaration(self, in_for=False):
        line = self.tok.line
        kind = self.advance().value
        decls = []
        while True:
            target = self.binding_target()
            init = None
            if self.eat("="):
                init = self.assignment(no_in=in_for)
            elif kind == "const" and not (in_for and (
                    self.is_kw("in") or self.is_of())):
                raise ScriptError(self.tok.line,
                                  "Missing initializer in const declaration")
            elif target[0] != "pid" and not (in_for and (
                    self.is_kw("in") or self.is_of())):
                raise ScriptError(self.tok.line,
                                  "Missing initializer in destructuring declaration")
            decls.append((target, init))
            if not self.eat(","):
                break
        return ("var", line, kind, decls)

    def is_of(self):
        return self.tok.kind == "ident" and self.tok.value == "of"

    def for_statement(self):
        line = self.advance().line
        self.expect("(")
        init = None
        if self.tok.kind == "kw" and self.tok.value in ("var", "let", "const"):
            decl = self.declaration(in_for=True)
            if len(decl[3]) == 1 and decl[3][0][1] is None and (
                    self.is_of() or self.is_kw("in")):
                kind = "forof" if self.is_of() else "forin"
                self.advance()
                subject = self.assignment() if kind == "forof" else \
                    self.expression()
                self.expect(")")
                body = self.loop_body()
                return (kind, line, decl[2], decl[3][0][0], subject, body)
            for target, value in decl[3]:
                if value is None and (decl[2] == "const" or target[0] != "pid"):
                    raise ScriptError(line, "Missing initializer in declaration")
            init = decl
        elif not self.is_punct(";"):
            expr = self.expression(no_in=True)
            if self.is_of() or self.is_kw("in"):
                kind = "forof" if self.is_of() else "forin"
                self.advance()
                target = self.to_pattern(expr)
                subject = self.assignment() if kind == "forof" else \
                    self.expression()
                self.expect(")")
                body = self.loop_body()
                return (kind, line, None, target, subject, body)
            init = ("expr", line, expr)
        self.expect(";")
        test = None if self.is_punct(";") else self.expression()
        self.expect(";")
        update = None if self.is_punct(")") else self.expression()
        self.expect(")")
        body = self.loop_body()
        return ("for", line, init, test, update, body)

    def try_statement(self):
        line = self.advance().line
        block = self.block()
        param = None
        handler = None
        final = None
        if self.is_kw("catch"):
            self.advance()
            if self.eat("("):
                param = self.binding_target()
                self.expect(")")
            handler = self.block()
        if self.is_kw("finally"):
            self.advance()
            final = self.block()
        if handler is None and final is None:
            raise ScriptError(line, "Missing catch or finally after try")
        return ("try", line, block, param, handler, final)

    def switch_statement(self):
        line = self.advance().line
        self.expect("(")
        subject = self.expression()
        self.expect(")")
        self.expect("{")
        cases = []
        seen_default = False
        self.switches += 1
        try:
            while not self.is_punct("}"):
                if self.is_kw("case"):
                    self.advance()
                    test = self.expression()
                elif self.is_kw("default"):
                    if seen_default:
                        raise ScriptError(self.tok.line,
                                          "More than one default clause in switch")
                    seen_default = True
                    self.advance()
                    test = None
                else:
                    self.error()
                self.expect(":")
                body = []
                while not (self.is_kw("case") or self.is_kw("default")
                           or self.is_punct("}")):
                    if self.tok.kind == "eof":
                        self.error()
                    body.append(self.statement())
                cases.append((test, body))
        finally:
            self.switches -= 1
        self.advance()
        return ("switch", line, subject, cases)

    def binding_target(self):
        tok = self.tok
        if tok.kind == "ident":
            self.advance()
            return ("pid", tok.line, tok.value)
        if self.is_punct("["):
            self.advance()
            elements = []
            rest = None
            while not self.is_punct("]"):
                if self.is_punct(","):
                    self.advance()
                    elements.append(None)
                    continue
                if self.eat("..."):
                    rest = self.binding_target()
                    break
                target = self.binding_target()
                default = self.assignment() if self.eat("=") else None
                elements.append((target, default))
                if not self.is_punct("]"):
                    self.expect(",")
            self.expect("]")
            return ("parr", tok.line, elements, rest)
        if self.is_punct("{"):
            self.advance()
            props = []
            rest = None
            while not self.is_punct("}"):
                if self.eat("..."):
                    rest = self.binding_target()
                    break
                key_tok = self.tok
                if key_tok.kind in ("ident", "kw"):
                    key = key_tok.value
                elif key_tok.kind == "str":
                    key = key_tok.value
                elif key_tok.kind == "num":
                    key = number_to_string(key_tok.value)
                else:
                    self.error()
                self.advance()
                if self.eat(":"):
                    target = self.binding_target()
                else:
                    if key_tok.kind != "ident":
                        self.error(key_tok)
                    target = ("pid", key_tok.line, key)
                default = self.assignment() if self.eat("=") else None
                props.append((key, target, default))
                if not self.is_punct("}"):
                    self.expect(",")
            self.expect("}")
            return ("pobj", tok.line, props, rest)
        self.error()

    def to_pattern(self, expr):
        kind = expr[0]
        if kind in ("pid", "parr", "pobj", "pexpr"):
            return expr
        if kind == "ident":
            return ("pid", expr[1], expr[2])
        if kind in ("member", "index"):
            if expr[-1]:
                raise ScriptError(expr[1], "Invalid assignment target")
            return ("pexpr", expr[1], expr)
        if kind == "array":
            elements = []
            rest = None
            for index, element in enumerate(expr[2]):
                if element is None:
                    elements.append(None)
                elif element[0] == "spread":
                    if index != len(expr[2]) - 1:
                        raise ScriptError(expr[1], "Invalid assignment target")
                    rest = self.to_pattern(element[2])
                elif element[0] == "assign" and element[2] == "=":
                    elements.append((self.to_pattern(element[3]), element[4]))
                else:
                    elements.append((self.to_pattern(element), None))
            return ("parr", expr[1], elements, rest)
        if kind == "object":
            self.shorthand.pop(id(expr), None)
            props = []
            rest = None
            for prop in expr[2]:
                if prop[0] == "spread":
                    rest = self.to_pattern(prop[1])
                    continue
                _, key, value, computed, shorthand_default = prop
                if computed:
                    raise ScriptError(expr[1], "Invalid assignment target")
                if shorthand_default is not None:
                    props.append((key, ("pid", expr[1], key), shorthand_default))
                elif value[0] == "assign" and value[2] == "=":
                    props.append((key, self.to_pattern(value[3]), value[4]))
                else:
                    props.append((key, self.to_pattern(value), None))
            return ("pobj", expr[1], props, rest)
        raise ScriptError(expr[1], "Invalid assignment target")

    def expression(self, no_in=False):
        line = self.tok.line
        expr = self.assignment(no_in)
        if self.is_punct(","):
            items = [expr]
            while self.eat(","):
                items.append(self.assignment(no_in))
            return ("seq", line, items)
        return expr

    def arrow_ahead(self):
        if self.tok.kind == "ident" and self.is_punct("=>", self.peek()):
            return True
        if not self.is_punct("("):
            return False
        depth = 0
        index = self.index
        while index < len(self.tokens):
            token = self.tokens[index]
            if token.kind == "eof":
                return False
            if token.kind == "punct":
                if token.value in ("(", "[", "{"):
                    depth += 1
                elif token.value in (")", "]", "}"):
                    depth -= 1
                    if depth == 0:
                        following = self.tokens[min(index + 1,
                                                    len(self.tokens) - 1)]
                        return following.kind == "punct" and \
                            following.value == "=>" and not following.newline
            index += 1
        return False

    def assignment(self, no_in=False):
        self.enter()
        try:
            return self._assignment(no_in)
        finally:
            self.leave()

    def _assignment(self, no_in):
        if self.arrow_ahead():
            return self.arrow()
        line = self.tok.line
        left = self.conditional(no_in)
        tok = self.tok
        if tok.kind == "punct" and tok.value in ASSIGN:
            op = tok.value
            if op == "=":
                target = self.to_pattern(left)
            elif left[0] in ("ident", "member", "index") and not (
                    left[0] != "ident" and left[-1]):
                target = left
            else:
                raise ScriptError(line, "Invalid assignment target")
            self.advance()
            value = self.assignment(no_in)
            return ("assign", line, op, target, value)
        return left

    def arrow(self):
        line = self.tok.line
        if self.tok.kind == "ident":
            params = [(("pid", line, self.advance().value), None)]
            rest = None
        else:
            params, rest = self.params()
        self.expect("=>")
        return self.function_body(line, "", params, rest, True)

    def params(self):
        self.expect("(")
        params = []
        rest = None
        while not self.is_punct(")"):
            if self.eat("..."):
                rest = self.binding_target()
                break
            target = self.binding_target()
            default = self.assignment() if self.eat("=") else None
            params.append((target, default))
            if not self.is_punct(")"):
                self.expect(",")
        self.expect(")")
        return params, rest

    def function_body(self, line, name, params, rest, arrow):
        saved = (self.loops, self.switches)
        self.loops = self.switches = 0
        self.functions += 1
        try:
            if arrow and not self.is_punct("{"):
                body = self.assignment()
                return ("func", line, name, params, rest, body, True, True)
            body = self.block()
            return ("func", line, name, params, rest, body, arrow, False)
        finally:
            self.loops, self.switches = saved
            self.functions -= 1

    def function_rest(self, line, declaration):
        name = ""
        if self.tok.kind == "ident":
            name = self.advance().value
        elif declaration:
            self.error()
        params, rest = self.params()
        return self.function_body(line, name, params, rest, False)

    def conditional(self, no_in):
        line = self.tok.line
        test = self.binary(0, no_in)
        if self.eat("?"):
            then = self.assignment()
            self.expect(":")
            other = self.assignment(no_in)
            return ("cond", line, test, then, other)
        return test

    def binary(self, min_prec, no_in):
        left = self.unary()
        while True:
            tok = self.tok
            if tok.kind == "punct" or (tok.kind == "kw" and tok.value in (
                    "in", "instanceof")):
                op = tok.value
            else:
                break
            if op == "in" and no_in:
                break
            prec = BINARY.get(op)
            if prec is None or prec <= min_prec:
                break
            line = tok.line
            self.advance()
            self.enter()
            try:
                right = self.binary(prec - 1 if op == "**" else prec, no_in)
            finally:
                self.leave()
            if op in ("&&", "||", "??"):
                left = ("logic", line, op, left, right)
            else:
                left = ("bin", line, op, left, right)
        return left

    def unary(self):
        tok = self.tok
        line = tok.line
        if tok.kind == "punct" and tok.value in ("!", "-", "+", "~"):
            self.advance()
            self.enter()
            try:
                return ("unary", line, tok.value, self.unary())
            finally:
                self.leave()
        if tok.kind == "punct" and tok.value in ("++", "--"):
            self.advance()
            target = self.unary()
            if target[0] not in ("ident", "member", "index") or (
                    target[0] != "ident" and target[-1]):
                raise ScriptError(line, "Invalid update target")
            return ("update", line, tok.value, True, target)
        if tok.kind == "kw" and tok.value in ("typeof", "delete", "void"):
            self.advance()
            self.enter()
            try:
                return ("unary", line, tok.value, self.unary())
            finally:
                self.leave()
        expr = self.postfix()
        tok = self.tok
        if tok.kind == "punct" and tok.value in ("++", "--") and not tok.newline:
            if expr[0] not in ("ident", "member", "index") or (
                    expr[0] != "ident" and expr[-1]):
                raise ScriptError(line, "Invalid update target")
            self.advance()
            return ("update", line, tok.value, False, expr)
        return expr

    def postfix(self):
        line = self.tok.line
        if self.is_kw("new"):
            expr = self.new_expression()
        else:
            expr = self.primary()
        optional = False
        while True:
            tok = self.tok
            if tok.kind != "punct":
                break
            if tok.value == ".":
                self.advance()
                name = self.property_name()
                expr = ("member", tok.line, expr, name, False)
            elif tok.value == "?.":
                optional = True
                self.advance()
                if self.is_punct("("):
                    expr = ("call", tok.line, expr, self.arguments(), True)
                elif self.eat("["):
                    key = self.expression()
                    self.expect("]")
                    expr = ("index", tok.line, expr, key, True)
                else:
                    expr = ("member", tok.line, expr, self.property_name(),
                            True)
            elif tok.value == "[":
                self.advance()
                key = self.expression()
                self.expect("]")
                expr = ("index", tok.line, expr, key, False)
            elif tok.value == "(":
                expr = ("call", tok.line, expr, self.arguments(), False)
            else:
                break
        if optional:
            return ("chain", line, expr)
        return expr

    def property_name(self):
        tok = self.tok
        if tok.kind not in ("ident", "kw"):
            self.error()
        self.advance()
        return tok.value

    def new_expression(self):
        line = self.advance().line
        if self.is_kw("new"):
            callee = self.new_expression()
        else:
            callee = self.primary()
        while True:
            if self.is_punct("."):
                self.advance()
                callee = ("member", line, callee, self.property_name(), False)
            elif self.is_punct("["):
                self.advance()
                key = self.expression()
                self.expect("]")
                callee = ("index", line, callee, key, False)
            else:
                break
        args = self.arguments() if self.is_punct("(") else []
        return ("new", line, callee, args)

    def arguments(self):
        self.expect("(")
        args = []
        while not self.is_punct(")"):
            if self.eat("..."):
                args.append(("spread", self.tok.line, self.assignment()))
            else:
                args.append(self.assignment())
            if not self.is_punct(")"):
                self.expect(",")
        self.expect(")")
        return args

    def primary(self):
        tok = self.tok
        line = tok.line
        kind = tok.kind
        if kind == "num":
            self.advance()
            return ("lit", line, tok.value)
        if kind == "str":
            self.advance()
            return ("lit", line, tok.value)
        if kind == "tmpl":
            self.advance()
            parts = []
            for tokens in tok.extra:
                sub = Parser(tokens)
                sub.nesting = self.nesting
                sub.loops, sub.switches = self.loops, self.switches
                sub.shorthand = self.shorthand
                expr = sub.expression()
                if sub.tok.kind != "eof":
                    sub.error()
                parts.append(expr)
            return ("tmpl", line, tok.value, parts)
        if kind == "regex":
            self.advance()
            return ("regex", line, tok.value, tok.extra,
                    compile_regex(tok.value, tok.extra, line))
        if kind == "ident":
            self.advance()
            return ("ident", line, tok.value)
        if kind == "kw":
            word = tok.value
            if word in ("true", "false"):
                self.advance()
                return ("lit", line, word == "true")
            if word == "null":
                self.advance()
                return ("lit", line, None)
            if word == "this":
                self.advance()
                return ("this", line)
            if word == "function":
                self.advance()
                return self.function_rest(line, False)
        if kind == "punct":
            if tok.value == "(":
                self.advance()
                expr = self.expression()
                self.expect(")")
                return expr
            if tok.value == "[":
                return self.array_literal()
            if tok.value == "{":
                return self.object_literal()
        self.error()

    def array_literal(self):
        line = self.advance().line
        elements = []
        while not self.is_punct("]"):
            if self.is_punct(","):
                self.advance()
                elements.append(None)
                continue
            if self.eat("..."):
                elements.append(("spread", self.tok.line, self.assignment()))
            else:
                elements.append(self.assignment())
            if not self.is_punct("]"):
                self.expect(",")
        self.advance()
        return ("array", line, elements)

    def object_literal(self):
        line = self.advance().line
        props = []
        while not self.is_punct("}"):
            if self.eat("..."):
                props.append(("spread", self.assignment()))
            else:
                key_tok = self.tok
                computed = False
                if key_tok.kind in ("ident", "kw", "str"):
                    key = key_tok.value
                    self.advance()
                elif key_tok.kind == "num":
                    key = number_to_string(key_tok.value)
                    self.advance()
                elif self.is_punct("["):
                    self.advance()
                    key = self.assignment()
                    self.expect("]")
                    computed = True
                else:
                    self.error()
                if self.eat(":"):
                    props.append(("prop", key, self.assignment(), computed,
                                  None))
                elif self.is_punct("("):
                    params, rest = self.params()
                    fn = self.function_body(key_tok.line,
                                            key if not computed else "",
                                            params, rest, False)
                    props.append(("prop", key, fn, computed, None))
                elif key_tok.kind == "ident" and not computed:
                    default = None
                    if self.is_punct("="):
                        self.advance()
                        default = self.assignment()
                    props.append(("prop", key, ("ident", key_tok.line, key),
                                  False, default))
                else:
                    self.error()
            if not self.is_punct("}"):
                self.expect(",")
        self.advance()
        node = ("object", line, props)
        if any(prop[0] == "prop" and prop[4] is not None for prop in props):
            self.shorthand[id(node)] = (node, line)
        return node


def parse(source):
    parser = Parser(_lex_all(str(source)))
    program = parser.program()
    if parser.shorthand:
        line = min(line for _, line in parser.shorthand.values())
        raise ScriptError(line, "Invalid shorthand property initializer")
    return program


def _translate_regex(source, flags, line):
    out = []
    names = []
    index = 0
    in_class = False
    dotall = "s" in flags
    multiline = "m" in flags
    space = "".join(JS_SPACE)
    space_class = re.escape(space).replace("\\ ", " ")
    length = len(source)
    while index < length:
        ch = source[index]
        if ch == "\\":
            if index + 1 >= length:
                raise ScriptError(line, "Invalid regular expression /" + source + "/")
            nxt = source[index + 1]
            index += 2
            if nxt == "d":
                out.append("0-9" if in_class else "[0-9]")
            elif nxt == "D":
                if in_class:
                    raise ScriptError(line, "Invalid regular expression /" + source + "/")
                out.append("[^0-9]")
            elif nxt == "w":
                out.append("A-Za-z0-9_" if in_class else WORD)
            elif nxt == "W":
                if in_class:
                    raise ScriptError(line, "Invalid regular expression /" + source + "/")
                out.append("[^A-Za-z0-9_]")
            elif nxt == "s":
                out.append(space_class if in_class else "[" + space_class + "]")
            elif nxt == "S":
                if in_class:
                    raise ScriptError(line, "Invalid regular expression /" + source + "/")
                out.append("[^" + space_class + "]")
            elif nxt == "b" and not in_class:
                out.append("(?:(?<=" + WORD + ")(?!" + WORD + ")|(?<!" + WORD
                           + ")(?=" + WORD + "))")
            elif nxt == "B" and not in_class:
                out.append("(?:(?<=" + WORD + ")(?=" + WORD + ")|(?<!" + WORD
                           + ")(?!" + WORD + "))")
            elif nxt == "b":
                out.append("\\x08")
            elif nxt == "k" and source[index:index + 1] == "<":
                end = source.find(">", index)
                if end < 0:
                    raise ScriptError(line, "Invalid regular expression /" + source + "/")
                out.append("(?P=" + source[index + 1:end] + ")")
                index = end + 1
            elif nxt == "u" and source[index:index + 1] == "{":
                end = source.find("}", index)
                digits = source[index + 1:end] if end > 0 else ""
                if not digits or any(c not in "0123456789abcdefABCDEF"
                                     for c in digits) or int(digits, 16) > 0x10FFFF:
                    raise ScriptError(line, "Invalid regular expression /" + source + "/")
                out.append("\\U%08x" % int(digits, 16))
                index = end + 1
            elif nxt == "c" and index < length and source[index].isalpha():
                out.append("\\x%02x" % (ord(source[index]) % 32))
                index += 1
            elif nxt == "0" and not source[index:index + 1].isdigit():
                out.append("\\x00")
            elif nxt == "/":
                out.append("/")
            elif nxt == "v":
                out.append("\\x0b")
            else:
                out.append("\\" + nxt)
            continue
        if in_class:
            if ch == "]":
                in_class = False
            elif ch == "[":
                out.append("\\[")
                index += 1
                continue
            out.append(ch)
            index += 1
            continue
        if ch == "[":
            in_class = True
            if source.startswith("[^]", index):
                out.append("[\\s\\S]")
                index += 3
                in_class = False
                continue
            if source.startswith("[]", index):
                out.append("(?!)")
                index += 2
                in_class = False
                continue
            out.append("[")
            index += 1
            if source[index:index + 1] == "^":
                out.append("^")
                index += 1
            if source[index:index + 1] == "]":
                out.append("\\]")
                index += 1
            continue
        if ch == "(" and source.startswith("(?<", index) and \
                source[index + 3:index + 4] not in ("=", "!"):
            end = source.find(">", index)
            if end < 0:
                raise ScriptError(line, "Invalid regular expression /" + source + "/")
            names.append(source[index + 3:end])
            out.append("(?P<" + source[index + 3:end] + ">")
            index = end + 1
            continue
        if ch == ".":
            out.append("[\\s\\S]" if dotall else "[^\\n\\r\\u2028\\u2029]")
        elif ch == "$":
            out.append("(?=[\\n\\r\\u2028\\u2029]|\\Z)" if multiline else "\\Z")
        elif ch == "^":
            out.append("(?:(?<=[\\n\\r\\u2028\\u2029])|\\A)" if multiline
                       else "\\A")
        else:
            out.append(ch)
        index += 1
    return "".join(out), names


def compile_regex(source, flags, line=0):
    if any(flag not in "gimsuyd" for flag in flags) or len(set(flags)) != len(flags):
        raise ScriptError(line, "Invalid regular expression flags")
    pattern, names = _translate_regex(source, flags, line)
    options = re.IGNORECASE if "i" in flags else 0
    try:
        return GuardedPattern(re.compile(pattern, options)), names
    except (re.error, ValueError, OverflowError):
        raise ScriptError(line, "Invalid regular expression /" + source + "/")


def number_to_string(value):
    if value != value:
        return "NaN"
    if value == math.inf:
        return "Infinity"
    if value == -math.inf:
        return "-Infinity"
    if value == 0:
        return "0"
    sign = "-" if value < 0 else ""
    text = repr(abs(float(value)))
    mantissa, _, exponent = text.partition("e")
    whole, _, fraction = mantissa.partition(".")
    digits = (whole + fraction).lstrip("0")
    point = len(whole) + (int(exponent) if exponent else 0)
    if not whole.strip("0"):
        stripped = fraction.lstrip("0")
        point = (int(exponent) if exponent else 0) - (len(fraction) - len(stripped))
        digits = stripped
    digits = digits.rstrip("0") or "0"
    k = len(digits)
    n = point
    if k <= n <= 21:
        return sign + digits + "0" * (n - k)
    if 0 < n <= 21:
        return sign + digits[:n] + "." + digits[n:]
    if -6 < n <= 0:
        return sign + "0." + "0" * (-n) + digits
    e = n - 1
    exp = ("+" if e >= 0 else "-") + str(abs(e))
    if k == 1:
        return sign + digits + "e" + exp
    return sign + digits[0] + "." + digits[1:] + "e" + exp


def string_to_number(text):
    text = text.strip(JS_SPACE)
    if not text:
        return 0.0
    if text in ("Infinity", "+Infinity"):
        return math.inf
    if text == "-Infinity":
        return -math.inf
    if len(text) > 2 and text[0] == "0" and text[1] in "xXoObB":
        base = {"x": 16, "o": 8, "b": 2}[text[1].lower()]
        try:
            if "_" in text:
                return math.nan
            return float(int(text[2:], base))
        except ValueError:
            return math.nan
    if DECIMAL_RE.fullmatch(text):
        return float(text)
    return math.nan




def date_from_parts(year, month, day=1.0, hours=0.0, minutes=0.0, seconds=0.0,
                    ms=0.0):
    values = (year, month, day, hours, minutes, seconds, ms)
    if any(v != v or v in (math.inf, -math.inf) for v in values):
        return math.nan
    year, month, day, hours, minutes, seconds, ms = (math.trunc(v)
                                                      for v in values)
    year_total = year + month // 12
    month_index = month % 12
    days = _days_from_civil(year_total, month_index + 1, 1) + day - 1
    return float(days * 86400000 + hours * 3600000 + minutes * 60000
                 + seconds * 1000 + ms)


def _days_from_civil(year, month, day):
    year -= month <= 2
    era = (year if year >= 0 else year - 399) // 400
    yoe = year - era * 400
    doy = (153 * (month + (-3 if month > 2 else 9)) + 2) // 5 + day - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def civil_from_ms(ms):
    ms = int(ms)
    days, rem = divmod(ms, 86400000)
    z = days + 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    year = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    day = doy - (153 * mp + 2) // 5 + 1
    month = mp + (3 if mp < 10 else -9)
    if month <= 2:
        year += 1
    weekday = (days + 4) % 7
    return (year, month, day, rem // 3600000, rem // 60000 % 60,
            rem // 1000 % 60, rem % 1000, weekday)


def parse_date_string(text):
    text = text.strip(JS_SPACE)
    match = ISO_DATE_RE.fullmatch(text)
    if not match:
        return math.nan
    year_text, month, day, hours, minutes, seconds, fraction, offset = \
        match.groups()
    if year_text == "-000000":
        return math.nan
    year = int(year_text)
    month = int(month or 1)
    day = int(day or 1)
    hours = int(hours or 0)
    minutes = int(minutes or 0)
    seconds = int(seconds or 0)
    ms = int((fraction or "0")[:3].ljust(3, "0"))
    if not (1 <= month <= 12 and 1 <= day <= 31 and hours <= 24
            and minutes <= 59 and seconds <= 59):
        return math.nan
    if hours == 24 and (minutes or seconds or ms):
        return math.nan
    if day > _month_days(year, month):
        return math.nan
    value = date_from_parts(year, month - 1, day, hours, minutes, seconds, ms)
    if offset and offset != "Z":
        sign = 1 if offset[0] == "+" else -1
        value -= sign * (int(offset[1:3]) * 60 + int(offset[4:6])) * 60000
    return time_clip(value)


def _month_days(year, month):
    if month == 2:
        return 29 if (year % 4 == 0 and year % 100 != 0) or year % 400 == 0 \
            else 28
    return 30 if month in (4, 6, 9, 11) else 31


def iso_string(ms):
    year, month, day, hours, minutes, seconds, millis, _ = civil_from_ms(ms)
    if 0 <= year <= 9999:
        year_text = "%04d" % year
    else:
        year_text = ("+" if year > 0 else "-") + "%06d" % abs(year)
    return "%s-%02d-%02dT%02d:%02d:%02d.%03dZ" % (
        year_text, month, day, hours, minutes, seconds, millis)


def own_keys(obj):
    numeric = []
    other = []
    for key in obj:
        if INDEX_RE.fullmatch(key) and int(key) < 4294967295:
            numeric.append(key)
        else:
            other.append(key)
    if not numeric:
        return other
    numeric.sort(key=int)
    return numeric + other


def from_json(value):
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, dict):
        return {str(key): from_json(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [from_json(item) for item in value]
    return str(value)


def to_json(value, seen=None):
    if value is UNDEFINED:
        return None
    if value is None or isinstance(value, (bool, str)):
        return value
    if isinstance(value, float):
        if value != value or value in (math.inf, -math.inf):
            return None
        if value.is_integer() and abs(value) < MAX_INT:
            return int(value)
        return value
    if isinstance(value, JSDate):
        return None if value.ms != value.ms else iso_string(value.ms)
    if isinstance(value, (Function, Native)):
        return None
    if isinstance(value, (JSRegExp, JSCollection)):
        return {}
    seen = seen if seen is not None else set()
    marker = id(value)
    if marker in seen:
        raise ScriptLimit("The script returned a value that contains itself")
    seen.add(marker)
    try:
        if isinstance(value, dict):
            out = {}
            for key in own_keys(value):
                item = value[key]
                if item is UNDEFINED or isinstance(item, (Function, Native)):
                    continue
                out[key] = to_json(item, seen)
            return out
        if isinstance(value, list):
            return [to_json(item, seen) for item in value]
    finally:
        seen.discard(marker)
    return None


class Scope:
    __slots__ = ("vars", "consts", "parent")

    def __init__(self, parent=None):
        self.vars = {}
        self.consts = set()
        self.parent = parent

    def declare(self, name, value, kind):
        if kind in ("let", "const") and name in self.vars:
            raise Thrown(error_object("SyntaxError", "Identifier '" + name
                                      + "' has already been declared"))
        self.vars[name] = value
        if kind == "const":
            self.consts.add(name)
        else:
            self.consts.discard(name)

    def lookup(self, name):
        scope = self
        while scope is not None:
            if name in scope.vars:
                return scope
            scope = scope.parent
        return None


def error_object(name, message):
    return {"name": name, "message": message}


def type_error(message):
    return Thrown(error_object("TypeError", message))


def range_error(message):
    return Thrown(error_object("RangeError", message))


def is_callable(value):
    return isinstance(value, (Function, Native))


def type_of(value):
    if value is UNDEFINED:
        return "undefined"
    if value is None:
        return "object"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if is_callable(value):
        return "function"
    return "object"


def truthy(value):
    if value is UNDEFINED or value is None:
        return False
    if isinstance(value, bool):
        return value
    if isinstance(value, float):
        return not (value == 0 or value != value)
    if isinstance(value, str):
        return len(value) > 0
    return True


def to_number(value):
    if isinstance(value, float):
        return value
    if isinstance(value, bool):
        return 1.0 if value else 0.0
    if value is None:
        return 0.0
    if value is UNDEFINED:
        return math.nan
    if isinstance(value, str):
        return string_to_number(value)
    if isinstance(value, JSDate):
        return value.ms
    if isinstance(value, list):
        return string_to_number(to_string(value))
    return math.nan


def to_integer(value):
    number = to_number(value)
    if number != number:
        return 0
    if number in (math.inf, -math.inf):
        return number
    return math.trunc(number)


def wrap32(n):
    n %= 4294967296
    return n - 4294967296 if n >= 2147483648 else n


def to_int32(value):
    number = to_number(value)
    if number != number or number in (math.inf, -math.inf):
        return 0
    return wrap32(math.trunc(number))


def to_uint32(value):
    number = to_number(value)
    if number != number or number in (math.inf, -math.inf):
        return 0
    return math.trunc(number) % 4294967296


def date_to_string(ms):
    if ms != ms:
        return "Invalid Date"
    year, month, day, hours, minutes, seconds, _, weekday = civil_from_ms(ms)
    year_text = ("-%06d" % -year) if year < 0 else "%04d" % year
    return "%s %s %02d %s %02d:%02d:%02d GMT+0000 (Coordinated Universal Time)" % (
        DAY_NAMES[weekday], MONTH_NAMES[month - 1], day, year_text, hours,
        minutes, seconds)


def to_string(value, seen=None):
    if isinstance(value, str):
        return value
    if isinstance(value, float):
        return number_to_string(value)
    if isinstance(value, bool):
        return "true" if value else "false"
    if value is None:
        return "null"
    if value is UNDEFINED:
        return "undefined"
    if isinstance(value, list):
        seen = seen if seen is not None else set()
        if id(value) in seen:
            return ""
        seen.add(id(value))
        try:
            return join_list(value, ",", seen)
        finally:
            seen.discard(id(value))
    if isinstance(value, JSDate):
        return date_to_string(value.ms)
    if isinstance(value, JSRegExp):
        return "/" + value.source + "/" + value.flags
    if isinstance(value, Function):
        return "function " + value.name + "() { [code] }"
    if isinstance(value, Native):
        return "function " + value.name + "() { [native code] }"
    if isinstance(value, JSCollection):
        return "[object " + value.kind + "]"
    return "[object Object]"


def join_list(items, separator, seen=None):
    parts = []
    total = 0
    for item in items:
        text = "" if item is None or item is UNDEFINED else to_string(item, seen)
        total += len(text) + len(separator)
        if total > MAX_STRING + len(separator):
            raise range_error("Invalid string length")
        parts.append(text)
    return separator.join(parts)


def to_primitive(value, hint="default"):
    if isinstance(value, JSDate):
        return value.ms if hint == "number" else date_to_string(value.ms)
    if isinstance(value, (list, dict, Function, Native, JSRegExp, JSCollection)):
        return to_string(value)
    return value


def property_key(value):
    if isinstance(value, str):
        return value
    return to_string(value)


def array_index(key):
    if isinstance(key, float):
        if key.is_integer() and 0 <= key < 4294967295:
            return int(key)
        return None
    if isinstance(key, str) and INDEX_RE.fullmatch(key) and \
            int(key) < 4294967295:
        return int(key)
    return None


def strict_equals(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == b
    if isinstance(a, str) and isinstance(b, str):
        return a == b
    if isinstance(a, bool) and isinstance(b, bool):
        return a == b
    return a is b


def same_value_zero(a, b):
    if isinstance(a, float) and isinstance(b, float):
        return a == b or (a != a and b != b)
    return strict_equals(a, b)


def _is_object(value):
    return isinstance(value, (list, dict, JSDate, JSRegExp, Function, Native,
                              JSCollection))


def loose_equals(a, b):
    a_nullish = a is None or a is UNDEFINED
    b_nullish = b is None or b is UNDEFINED
    if a_nullish or b_nullish:
        return a_nullish and b_nullish
    if type_of(a) == type_of(b):
        return strict_equals(a, b)
    if isinstance(a, float) and isinstance(b, str):
        return a == string_to_number(b)
    if isinstance(a, str) and isinstance(b, float):
        return string_to_number(a) == b
    if isinstance(a, bool):
        return loose_equals(1.0 if a else 0.0, b)
    if isinstance(b, bool):
        return loose_equals(a, 1.0 if b else 0.0)
    if _is_object(a) and not _is_object(b):
        return loose_equals(to_primitive(a), b)
    if _is_object(b) and not _is_object(a):
        return loose_equals(a, to_primitive(b))
    return False


def js_pow(a, b):
    if b != b:
        return math.nan
    if b == 0:
        return 1.0
    if a != a:
        return math.nan
    if abs(a) == 1 and b in (math.inf, -math.inf):
        return math.nan
    odd = b.is_integer() and not math.isinf(b) and int(b) % 2 == 1
    try:
        return float(math.pow(a, b))
    except OverflowError:
        return -math.inf if a < 0 and odd else math.inf
    except (ValueError, ZeroDivisionError):
        if a == 0 and b < 0:
            return -math.inf if math.copysign(1, a) < 0 and odd else math.inf
        return math.nan


def js_divide(a, b):
    if b == 0:
        if a == 0 or a != a:
            return math.nan
        return math.copysign(math.inf, a) * math.copysign(1, b)
    return a / b


def js_mod(a, b):
    if b == 0 or a != a or b != b or a in (math.inf, -math.inf):
        return math.nan
    if b in (math.inf, -math.inf):
        return a
    if a == 0:
        return a
    return math.fmod(a, b)


def _check_string(text):
    if len(text) > MAX_STRING:
        raise range_error("Invalid string length")
    return text


def _check_array(items):
    if len(items) > MAX_ARRAY:
        raise range_error("Invalid array length")
    return items


def _relative(value, length, default):
    if value is UNDEFINED:
        return default
    number = to_integer(value)
    if number == -math.inf:
        return 0
    if number == math.inf:
        return length
    if number < 0:
        return max(length + number, 0)
    return min(number, length)


def _clamp_index(value, length):
    number = to_integer(value)
    if number == math.inf:
        return length
    if number == -math.inf:
        return 0
    return int(max(0, min(number, length)))


def compare_strings(a, b):
    return (a > b) - (a < b)


def _js_json_quote(text):
    out = ['"']
    for ch in text:
        code = ord(ch)
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\b":
            out.append("\\b")
        elif ch == "\f":
            out.append("\\f")
        elif code < 0x20 or 0xD800 <= code <= 0xDFFF:
            out.append("\\u%04x" % code)
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def json_stringify(value, indent=""):
    seen = []
    budget = [0]

    def emit(text):
        budget[0] += len(text)
        if budget[0] > MAX_STRING:
            raise range_error("Invalid string length")
        return text

    def serialize(item, current):
        if isinstance(item, JSDate):
            item = None if item.ms != item.ms else iso_string(item.ms)
        if item is None:
            return emit("null")
        if item is True:
            return emit("true")
        if item is False:
            return emit("false")
        if isinstance(item, str):
            return emit(_js_json_quote(item))
        if isinstance(item, float):
            if item != item or item in (math.inf, -math.inf):
                return emit("null")
            return emit(number_to_string(item))
        if item is UNDEFINED or is_callable(item):
            return None
        if isinstance(item, (JSRegExp, JSCollection)):
            return emit("{}")
        if any(other is item for other in seen):
            raise type_error("Converting circular structure to JSON")
        seen.append(item)
        inner = current + indent
        try:
            if isinstance(item, list):
                parts = []
                for element in item:
                    text = serialize(element, inner)
                    parts.append("null" if text is None else text)
                if not parts:
                    return "[]"
                if indent:
                    return "[\n" + inner + (",\n" + inner).join(parts) + \
                        "\n" + current + "]"
                return "[" + ",".join(parts) + "]"
            parts = []
            for key in own_keys(item):
                text = serialize(item[key], inner)
                if text is None:
                    continue
                parts.append(_js_json_quote(key) + (": " if indent else ":")
                             + text)
            if not parts:
                return "{}"
            if indent:
                return "{\n" + inner + (",\n" + inner).join(parts) + "\n" + \
                    current + "}"
            return "{" + ",".join(parts) + "}"
        finally:
            seen.pop()

    result = serialize(value, "")
    return UNDEFINED if result is None else result


def _reject_constant(name):
    raise ValueError(name)


def json_parse(text):
    try:
        value = json.loads(text, parse_constant=_reject_constant)
    except (ValueError, RecursionError):
        raise Thrown(error_object("SyntaxError",
                                  "JSON.parse: the text is not valid JSON"))
    return from_json(value)


def uri_encode(text):
    out = []
    for ch in text:
        if ch in URI_UNRESERVED:
            out.append(ch)
            continue
        if 0xD800 <= ord(ch) <= 0xDFFF:
            raise Thrown(error_object("URIError", "URI malformed"))
        out.extend("%%%02X" % byte for byte in ch.encode("utf-8"))
    return "".join(out)


def _uri_byte(text, index):
    pair = text[index + 1:index + 3]
    if text[index:index + 1] != "%" or len(pair) != 2 or any(
            c not in "0123456789abcdefABCDEF" for c in pair):
        raise Thrown(error_object("URIError", "URI malformed"))
    return int(pair, 16)


def uri_decode(text):
    out = []
    index = 0
    length = len(text)
    while index < length:
        ch = text[index]
        if ch != "%":
            out.append(ch)
            index += 1
            continue
        first = _uri_byte(text, index)
        index += 3
        if first < 0x80:
            out.append(chr(first))
            continue
        if first & 0xE0 == 0xC0:
            count = 1
        elif first & 0xF0 == 0xE0:
            count = 2
        elif first & 0xF8 == 0xF0:
            count = 3
        else:
            raise Thrown(error_object("URIError", "URI malformed"))
        data = bytearray([first])
        for _ in range(count):
            byte = _uri_byte(text, index)
            if byte & 0xC0 != 0x80:
                raise Thrown(error_object("URIError", "URI malformed"))
            data.append(byte)
            index += 3
        try:
            out.append(data.decode("utf-8"))
        except UnicodeDecodeError:
            raise Thrown(error_object("URIError", "URI malformed"))
    return "".join(out)


def expand_replacement(template, matched, position, subject, groups, named):
    out = []
    index = 0
    length = len(template)
    while index < length:
        ch = template[index]
        if ch != "$" or index + 1 >= length:
            out.append(ch)
            index += 1
            continue
        nxt = template[index + 1]
        if nxt == "$":
            out.append("$")
            index += 2
        elif nxt == "&":
            out.append(matched)
            index += 2
        elif nxt == "`":
            out.append(subject[:position])
            index += 2
        elif nxt == "'":
            out.append(subject[position + len(matched):])
            index += 2
        elif nxt in "0123456789":
            two = template[index + 1:index + 3]
            if len(two) == 2 and two.isdigit() and two[1] in "0123456789" \
                    and 1 <= int(two) <= len(groups):
                value = groups[int(two) - 1]
                out.append("" if value is UNDEFINED else value)
                index += 3
            elif 1 <= int(nxt) <= len(groups):
                value = groups[int(nxt) - 1]
                out.append("" if value is UNDEFINED else value)
                index += 2
            else:
                out.append("$" + nxt)
                index += 2
        elif nxt == "<" and named is not None:
            end = template.find(">", index)
            if end < 0:
                out.append("$<")
                index += 2
            else:
                value = named.get(template[index + 2:end], UNDEFINED) \
                    if isinstance(named, dict) else UNDEFINED
                out.append("" if value is UNDEFINED else to_string(value))
                index = end + 1
        else:
            out.append("$")
            index += 1
    return "".join(out)


def _match_array(regex, match, subject):
    result = MatchArray([match.group(0)] + [
        UNDEFINED if group is None else group for group in match.groups()])
    result.index = float(match.start())
    result.input = subject
    if regex.names:
        result.groups = {name: (UNDEFINED if match.group(name) is None
                                else match.group(name))
                         for name in regex.names}
    else:
        result.groups = UNDEFINED
    return result


def regex_exec(regex, subject):
    sticky = "y" in regex.flags
    use_last = "g" in regex.flags or sticky
    start = regex.last_index if use_last else 0
    if start > len(subject):
        regex.last_index = 0
        return None
    if sticky:
        match = regex.compiled.match(subject, start)
    else:
        match = regex.compiled.search(subject, start)
    if match is None:
        if use_last:
            regex.last_index = 0
        return None
    if use_last:
        regex.last_index = match.end()
    return _match_array(regex, match, subject)


def regex_all(regex, subject):
    matches = []
    position = 0
    while position <= len(subject):
        match = regex.compiled.search(subject, position)
        if match is None:
            break
        matches.append(match)
        if len(matches) > MAX_ARRAY:
            raise range_error("Invalid array length")
        position = match.end() if match.end() > match.start() else \
            match.end() + 1
    return matches


class Interpreter:
    def __init__(self, now_ms=0, random=None, clock=None, deadline=None):
        self.now_ms = float(now_ms)
        self.random = random or (lambda: 0.5)
        self.clock = clock
        self.deadline = deadline
        self.steps = 0
        self.depth = 0
        self.line = 1
        self.logs = []
        self.globals = Scope()
        self._install()
        self.eval_table = {
            "lit": self.e_lit, "ident": self.e_ident, "tmpl": self.e_tmpl,
            "regex": self.e_regex, "array": self.e_array,
            "object": self.e_object, "member": self.e_member,
            "index": self.e_index, "call": self.e_call, "chain": self.e_chain,
            "new": self.e_new, "unary": self.e_unary, "update": self.e_update,
            "bin": self.e_bin, "logic": self.e_logic, "cond": self.e_cond,
            "assign": self.e_assign, "func": self.e_func, "seq": self.e_seq,
            "this": self.e_this,
        }
        self.exec_table = {
            "expr": self.s_expr, "var": self.s_var, "if": self.s_if,
            "block": self.s_block, "for": self.s_for, "forof": self.s_forof,
            "forin": self.s_forin, "while": self.s_while,
            "dowhile": self.s_dowhile, "return": self.s_return,
            "break": self.s_break, "continue": self.s_continue,
            "throw": self.s_throw, "try": self.s_try, "switch": self.s_switch,
            "funcdecl": self.s_empty, "empty": self.s_empty,
        }

    def tick(self):
        self.steps += 1
        if self.steps > MAX_STEPS:
            raise ScriptLimit("The script ran more than %d steps" % MAX_STEPS)
        if self.steps % 1000 == 0 and self.clock is not None and \
                self.deadline is not None and self.clock() > self.deadline:
            raise ScriptLimit("This run ran out of time")

    def run(self, program, values):
        scope = Scope(self.globals)
        for name, value in values.items():
            scope.declare(name, value, "const" if name.startswith("$") else "let")
        body = Scope(scope)
        body.declare("this", UNDEFINED, "const")
        self.line = 1
        try:
            signal = self.exec_block(program, body)
        except Thrown as thrown:
            raise ScriptError(thrown.line or self.line,
                              self.describe_error(thrown.value))
        except ScriptLimit as limit:
            raise ScriptError(self.line, str(limit))
        except RecursionError:
            raise ScriptError(self.line, "Maximum call stack size exceeded")
        if isinstance(signal, Return):
            return signal.value
        return UNDEFINED

    def describe_error(self, value):
        if isinstance(value, dict) and "message" in value:
            name = value.get("name")
            message = to_string(value.get("message"))
            if isinstance(name, str) and name and name != "Error":
                return name + ": " + message if message else name
            return message or "Error"
        return "Uncaught " + self.display(value)

    def display(self, value):
        if isinstance(value, str):
            return value
        if isinstance(value, (list, dict)):
            try:
                text = json_stringify(value)
            except Thrown:
                return "[object]"
            return "undefined" if text is UNDEFINED else text
        return to_string(value)

    def log(self, args):
        if len(self.logs) >= MAX_LOGS:
            return
        text = " ".join(self.display(arg) for arg in args)
        if len(text) > MAX_LOG_CHARS:
            text = text[:MAX_LOG_CHARS] + "…"
        self.logs.append(text)

    def throw(self, thrown):
        if not thrown.line:
            thrown.line = self.line
        raise thrown

    def exec_block(self, statements, scope):
        for statement in statements:
            if statement[0] == "funcdecl":
                fn = statement[3]
                scope.declare(statement[2], self.make_function(fn, scope), "var")
        for statement in statements:
            signal = self.exec(statement, scope)
            if signal is not None:
                return signal
        return None

    def exec(self, statement, scope):
        self.line = statement[1]
        self.tick()
        try:
            return self.exec_table[statement[0]](statement, scope)
        except Thrown as thrown:
            if not thrown.line:
                thrown.line = statement[1]
            raise

    def s_empty(self, statement, scope):
        return None

    def s_expr(self, statement, scope):
        self.eval(statement[2], scope)
        return None

    def s_var(self, statement, scope):
        kind = statement[2]
        for target, init in statement[3]:
            value = UNDEFINED if init is None else self.eval(init, scope)
            if init is not None and init[0] == "func" and target[0] == "pid" \
                    and isinstance(value, Function) and not value.name:
                value.name = target[2]
            self.bind(target, value, scope, kind)
        return None

    def s_if(self, statement, scope):
        if truthy(self.eval(statement[2], scope)):
            return self.exec(statement[3], scope)
        if statement[4] is not None:
            return self.exec(statement[4], scope)
        return None

    def s_block(self, statement, scope):
        return self.exec_block(statement[2], Scope(scope))

    def s_while(self, statement, scope):
        while truthy(self.eval(statement[2], scope)):
            signal = self.exec(statement[3], scope)
            if signal is BREAK:
                break
            if signal is CONTINUE or signal is None:
                self.tick()
                continue
            return signal
        return None

    def s_dowhile(self, statement, scope):
        while True:
            signal = self.exec(statement[2], scope)
            if signal is BREAK:
                break
            if signal is not None and signal is not CONTINUE:
                return signal
            self.tick()
            if not truthy(self.eval(statement[3], scope)):
                break
        return None

    def s_for(self, statement, scope):
        _, _, init, test, update, body = statement
        loop = Scope(scope)
        per_iteration = False
        if init is not None:
            if init[0] == "var":
                self.s_var(init, loop)
                per_iteration = init[2] in ("let", "const")
            else:
                self.eval(init[2], loop)
        while True:
            if test is not None and not truthy(self.eval(test, loop)):
                break
            signal = self.exec(body, loop)
            if signal is BREAK:
                break
            if signal is not None and signal is not CONTINUE:
                return signal
            self.tick()
            if per_iteration:
                fresh = Scope(scope)
                fresh.vars = dict(loop.vars)
                fresh.consts = set(loop.consts)
                loop = fresh
            if update is not None:
                self.eval(update, loop)
        return None

    def iterate(self, value, node):
        if isinstance(value, list):
            return value
        if isinstance(value, str):
            return list(value)
        if isinstance(value, JSCollection):
            return value.entries()
        raise type_error(self.describe_node(node) + " is not iterable")

    def s_forof(self, statement, scope):
        _, _, kind, target, subject, body = statement
        sequence = self.iterate(self.eval(subject, scope), subject)
        index = 0
        while index < len(sequence):
            item = sequence[index]
            index += 1
            inner = Scope(scope)
            self.bind(target, item, inner, kind)
            signal = self.exec(body, inner)
            if signal is BREAK:
                break
            if signal is not None and signal is not CONTINUE:
                return signal
            self.tick()
        return None

    def s_forin(self, statement, scope):
        _, _, kind, target, subject, body = statement
        value = self.eval(subject, scope)
        if isinstance(value, dict):
            keys = own_keys(value)
        elif isinstance(value, (list, str)):
            keys = [str(index) for index in range(len(value))]
        else:
            keys = []
        for key in keys:
            if isinstance(value, dict) and key not in value:
                continue
            inner = Scope(scope)
            self.bind(target, key, inner, kind)
            signal = self.exec(body, inner)
            if signal is BREAK:
                break
            if signal is not None and signal is not CONTINUE:
                return signal
            self.tick()
        return None

    def s_return(self, statement, scope):
        return Return(UNDEFINED if statement[2] is None
                      else self.eval(statement[2], scope))

    def s_break(self, statement, scope):
        return BREAK

    def s_continue(self, statement, scope):
        return CONTINUE

    def s_throw(self, statement, scope):
        raise Thrown(self.eval(statement[2], scope), statement[1])

    def s_try(self, statement, scope):
        _, _, block, param, handler, final = statement
        try:
            signal = self.exec_block(block, Scope(scope))
        except Thrown as thrown:
            if handler is None:
                if final is not None:
                    extra = self.exec_block(final, Scope(scope))
                    if extra is not None:
                        return extra
                raise
            inner = Scope(scope)
            if param is not None:
                self.bind(param, thrown.value, inner, "let")
            try:
                signal = self.exec_block(handler, inner)
            except Thrown:
                if final is not None:
                    extra = self.exec_block(final, Scope(scope))
                    if extra is not None:
                        return extra
                raise
        if final is not None:
            extra = self.exec_block(final, Scope(scope))
            if extra is not None:
                return extra
        return signal

    def s_switch(self, statement, scope):
        _, _, subject, cases = statement
        value = self.eval(subject, scope)
        start = None
        for index, (test, _) in enumerate(cases):
            if test is not None and strict_equals(value, self.eval(test, scope)):
                start = index
                break
        if start is None:
            for index, (test, _) in enumerate(cases):
                if test is None:
                    start = index
                    break
        if start is None:
            return None
        inner = Scope(scope)
        statements = [s for _, body in cases[start:] for s in body]
        signal = self.exec_block(statements, inner)
        if signal is BREAK:
            return None
        return signal

    def bind(self, target, value, scope, kind):
        tag = target[0]
        if tag == "pid":
            if kind is None:
                self.assign_name(target[2], value, scope)
            else:
                scope.declare(target[2], value, kind)
            return
        if tag == "pexpr":
            self.assign_target(target[2], value, scope)
            return
        if tag == "parr":
            if not isinstance(value, (list, str, JSCollection)):
                raise type_error(self.display_short(value) + " is not iterable")
            sequence = self.iterate(value, None)
            for index, element in enumerate(target[2]):
                if element is None:
                    continue
                pattern, default = element
                item = sequence[index] if index < len(sequence) else UNDEFINED
                if item is UNDEFINED and default is not None:
                    item = self.eval(default, scope)
                self.bind(pattern, item, scope, kind)
            if target[3] is not None:
                self.bind(target[3], list(sequence[len(target[2]):]), scope,
                          kind)
            return
        if tag == "pobj":
            if value is None or value is UNDEFINED:
                raise type_error("Cannot destructure " + to_string(value))
            used = []
            for key, pattern, default in target[2]:
                used.append(key)
                item = self.get(value, key)
                if item is UNDEFINED and default is not None:
                    item = self.eval(default, scope)
                    if default[0] == "func" and isinstance(item, Function) and \
                            not item.name and pattern[0] == "pid":
                        item.name = pattern[2]
                self.bind(pattern, item, scope, kind)
            if target[3] is not None:
                rest = {}
                if isinstance(value, dict):
                    for key in own_keys(value):
                        if key not in used:
                            rest[key] = value[key]
                self.bind(target[3], rest, scope, kind)
            return
        raise type_error("Invalid assignment target")

    def display_short(self, value):
        if value is UNDEFINED:
            return "undefined"
        if value is None:
            return "null"
        return type_of(value)

    def assign_name(self, name, value, scope):
        owner = scope.lookup(name)
        if owner is None:
            raise Thrown(error_object("ReferenceError", name + " is not defined"))
        if name in owner.consts:
            raise type_error("Assignment to constant variable.")
        owner.vars[name] = value

    def assign_target(self, node, value, scope):
        kind = node[0]
        if kind == "ident":
            self.assign_name(node[2], value, scope)
        elif kind == "member":
            self.put(self.eval(node[2], scope), node[3], value)
        elif kind == "index":
            obj = self.eval(node[2], scope)
            key = self.eval(node[3], scope)
            self.put(obj, key, value)
        else:
            raise type_error("Invalid assignment target")

    def eval(self, node, scope):
        return self.eval_table[node[0]](node, scope)

    def e_lit(self, node, scope):
        return node[2]

    def e_this(self, node, scope):
        owner = scope.lookup("this")
        return owner.vars["this"] if owner is not None else UNDEFINED

    def e_ident(self, node, scope):
        name = node[2]
        current = scope
        while current is not None:
            values = current.vars
            if name in values:
                return values[name]
            current = current.parent
        raise Thrown(error_object("ReferenceError", name + " is not defined"),
                     node[1])

    def e_tmpl(self, node, scope):
        strings = node[2]
        out = [strings[0]]
        total = len(strings[0])
        for index, part in enumerate(node[3]):
            value = self.eval(part, scope)
            text = to_string(to_primitive(value))
            out.append(text)
            out.append(strings[index + 1])
            total += len(text) + len(strings[index + 1])
            if total > MAX_STRING:
                raise range_error("Invalid string length")
        return "".join(out)

    def e_regex(self, node, scope):
        compiled, names = node[4]
        return JSRegExp(node[2], node[3], compiled, names)

    def e_array(self, node, scope):
        out = []
        for element in node[2]:
            if element is None:
                out.append(UNDEFINED)
            elif element[0] == "spread":
                out.extend(self.iterate(self.eval(element[2], scope),
                                        element[2]))
                _check_array(out)
            else:
                out.append(self.eval(element, scope))
        return out

    def e_object(self, node, scope):
        out = {}
        for prop in node[2]:
            if prop[0] == "spread":
                value = self.eval(prop[1], scope)
                if isinstance(value, dict):
                    for key in own_keys(value):
                        out[key] = value[key]
                elif isinstance(value, list):
                    for index, item in enumerate(value):
                        out[str(index)] = item
                elif isinstance(value, str):
                    for index, ch in enumerate(value):
                        out[str(index)] = ch
                continue
            _, key, value_node, computed, _ = prop
            if computed:
                key = property_key(self.eval(key, scope))
            value = self.eval(value_node, scope)
            if value_node[0] == "func" and isinstance(value, Function) and \
                    not value.name:
                value.name = key
            out[key] = value
        return out

    def e_member(self, node, scope):
        obj = self.eval(node[2], scope)
        if node[4] and (obj is None or obj is UNDEFINED):
            raise _ShortCircuit()
        return self.get(obj, node[3], node[2])

    def e_index(self, node, scope):
        obj = self.eval(node[2], scope)
        if node[4] and (obj is None or obj is UNDEFINED):
            raise _ShortCircuit()
        key = self.eval(node[3], scope)
        return self.get(obj, key, node[2])

    def e_chain(self, node, scope):
        try:
            return self.eval(node[2], scope)
        except _ShortCircuit:
            return UNDEFINED

    def arguments(self, nodes, scope):
        args = []
        for arg in nodes:
            if arg[0] == "spread":
                args.extend(self.iterate(self.eval(arg[2], scope), arg[2]))
                _check_array(args)
            else:
                args.append(self.eval(arg, scope))
        return args

    def e_call(self, node, scope):
        callee = node[2]
        this = UNDEFINED
        if callee[0] in ("member", "index"):
            obj = self.eval(callee[2], scope)
            if callee[4] and (obj is None or obj is UNDEFINED):
                raise _ShortCircuit()
            key = callee[3] if callee[0] == "member" else self.eval(callee[3], scope)
            fn = self.get(obj, key, callee[2])
            this = obj
        else:
            fn = self.eval(callee, scope)
        if node[4] and (fn is None or fn is UNDEFINED):
            raise _ShortCircuit()
        args = self.arguments(node[3], scope)
        if not is_callable(fn):
            raise Thrown(error_object("TypeError", self.describe_node(callee)
                                      + " is not a function"), node[1])
        return self.call(fn, this, args)

    def describe_node(self, node):
        kind = node[0]
        if kind == "ident":
            return node[2]
        if kind == "member":
            return self.describe_node(node[2]) + "." + node[3]
        if kind == "index":
            return self.describe_node(node[2]) + "[...]"
        if kind == "call":
            return self.describe_node(node[2]) + "(...)"
        if kind == "this":
            return "this"
        if kind == "chain":
            return self.describe_node(node[2])
        if kind == "lit":
            value = node[2]
            return _js_json_quote(value) if isinstance(value, str) else \
                to_string(value)
        return "expression"

    def call(self, fn, this, args):
        if isinstance(fn, Native):
            return fn.fn(self, this, args)
        self.depth += 1
        if self.depth > MAX_DEPTH:
            self.depth -= 1
            raise ScriptLimit("Maximum call stack size exceeded")
        try:
            self.tick()
            scope = Scope(fn.scope)
            if not fn.arrow:
                scope.declare("this", this, "let")
            for index, (pattern, default) in enumerate(fn.params):
                value = args[index] if index < len(args) else UNDEFINED
                if value is UNDEFINED and default is not None:
                    value = self.eval(default, scope)
                self.bind(pattern, value, scope, "let")
            if fn.rest is not None:
                self.bind(fn.rest, list(args[len(fn.params):]), scope, "let")
            if fn.expression:
                return self.eval(fn.body, scope)
            saved = self.line
            signal = self.exec_block(fn.body, scope)
            self.line = saved
            if isinstance(signal, Return):
                return signal.value
            return UNDEFINED
        finally:
            self.depth -= 1

    def make_function(self, node, scope):
        _, _, name, params, rest, body, arrow, expression = node
        return Function(name, params, rest, body, scope, arrow, expression)

    def e_func(self, node, scope):
        return self.make_function(node, scope)

    def e_new(self, node, scope):
        callee = self.eval(node[2], scope)
        args = self.arguments(node[3], scope)
        if isinstance(callee, Native) and callee.construct is not None:
            return callee.construct(self, args)
        raise Thrown(error_object("TypeError", self.describe_node(node[2])
                                  + " is not a constructor"), node[1])

    def e_seq(self, node, scope):
        value = UNDEFINED
        for item in node[2]:
            value = self.eval(item, scope)
        return value

    def e_cond(self, node, scope):
        if truthy(self.eval(node[2], scope)):
            return self.eval(node[3], scope)
        return self.eval(node[4], scope)

    def e_logic(self, node, scope):
        op = node[2]
        left = self.eval(node[3], scope)
        if op == "&&":
            return self.eval(node[4], scope) if truthy(left) else left
        if op == "||":
            return left if truthy(left) else self.eval(node[4], scope)
        if left is None or left is UNDEFINED:
            return self.eval(node[4], scope)
        return left

    def e_unary(self, node, scope):
        op = node[2]
        if op == "typeof":
            operand = node[3]
            if operand[0] == "ident" and scope.lookup(operand[2]) is None:
                return "undefined"
            return type_of(self.eval(operand, scope))
        if op == "delete":
            operand = node[3]
            if operand[0] in ("member", "index"):
                obj = self.eval(operand[2], scope)
                key = operand[3] if operand[0] == "member" else \
                    property_key(self.eval(operand[3], scope))
                if obj is None or obj is UNDEFINED:
                    raise type_error("Cannot convert undefined or null to object")
                if isinstance(obj, dict):
                    obj.pop(key, None)
                    return True
                if isinstance(obj, list):
                    index = array_index(key)
                    if index is not None and index < len(obj):
                        obj[index] = UNDEFINED
                    return True
                return True
            return True
        value = self.eval(node[3], scope)
        if op == "!":
            return not truthy(value)
        if op == "-":
            return -to_number(value)
        if op == "+":
            return to_number(value)
        if op == "~":
            return float(~to_int32(value))
        if op == "void":
            return UNDEFINED
        raise type_error("Unknown operator " + op)

    def read_target(self, target, scope):
        if target[0] == "ident":
            return None, None, self.e_ident(target, scope)
        obj = self.eval(target[2], scope)
        key = target[3] if target[0] == "member" else self.eval(target[3], scope)
        return obj, key, self.get(obj, key, target[2])

    def write_target(self, target, obj, key, value, scope):
        if target[0] == "ident":
            self.assign_name(target[2], value, scope)
        else:
            self.put(obj, key, value)

    def e_update(self, node, scope):
        _, _, op, prefix, target = node
        obj, key, old = self.read_target(target, scope)
        old = to_number(old)
        new = old + 1 if op == "++" else old - 1
        self.write_target(target, obj, key, new, scope)
        return new if prefix else old

    def e_assign(self, node, scope):
        _, _, op, target, value_node = node
        if op == "=":
            value = self.eval(value_node, scope)
            if value_node[0] == "func" and target[0] == "pid" and isinstance(
                    value, Function) and not value.name:
                value.name = target[2]
            self.bind(target, value, scope, None)
            return value
        obj, key, old = self.read_target(target, scope)
        if op in ("&&=", "||=", "??="):
            if op == "&&=" and not truthy(old):
                return old
            if op == "||=" and truthy(old):
                return old
            if op == "??=" and old is not None and old is not UNDEFINED:
                return old
            value = self.eval(value_node, scope)
        else:
            value = self.binary(op[:-1], old, self.eval(value_node, scope))
        self.write_target(target, obj, key, value, scope)
        return value

    def e_bin(self, node, scope):
        left = self.eval(node[3], scope)
        right = self.eval(node[4], scope)
        return self.binary(node[2], left, right)

    def binary(self, op, a, b):
        if op == "+":
            if isinstance(a, float) and isinstance(b, float):
                return a + b
            pa = to_primitive(a)
            pb = to_primitive(b)
            if isinstance(pa, str) or isinstance(pb, str):
                return _check_string(to_string(pa) + to_string(pb))
            return to_number(pa) + to_number(pb)
        if op == "-":
            return to_number(a) - to_number(b)
        if op == "*":
            x, y = to_number(a), to_number(b)
            try:
                return x * y
            except OverflowError:
                return math.inf
        if op == "/":
            return js_divide(to_number(a), to_number(b))
        if op == "%":
            return js_mod(to_number(a), to_number(b))
        if op == "**":
            return js_pow(to_number(a), to_number(b))
        if op == "===":
            return strict_equals(a, b)
        if op == "!==":
            return not strict_equals(a, b)
        if op == "==":
            return loose_equals(a, b)
        if op == "!=":
            return not loose_equals(a, b)
        if op in ("<", ">", "<=", ">="):
            pa = to_primitive(a, "number")
            pb = to_primitive(b, "number")
            if isinstance(pa, str) and isinstance(pb, str):
                if op == "<":
                    return pa < pb
                if op == ">":
                    return pa > pb
                if op == "<=":
                    return pa <= pb
                return pa >= pb
            x, y = to_number(pa), to_number(pb)
            if op == "<":
                return x < y
            if op == ">":
                return x > y
            if op == "<=":
                return x <= y
            return x >= y
        if op == "&":
            return float(wrap32(to_int32(a) & to_int32(b)))
        if op == "|":
            return float(wrap32(to_int32(a) | to_int32(b)))
        if op == "^":
            return float(wrap32(to_int32(a) ^ to_int32(b)))
        if op == "<<":
            return float(wrap32(to_int32(a) << (to_uint32(b) & 31)))
        if op == ">>":
            return float(to_int32(a) >> (to_uint32(b) & 31))
        if op == ">>>":
            return float(to_uint32(a) >> (to_uint32(b) & 31))
        if op == "in":
            if isinstance(b, dict):
                return property_key(a) in b
            if isinstance(b, list):
                key = property_key(a)
                index = array_index(key)
                return key == "length" or (index is not None and index < len(b))
            raise type_error("Cannot use 'in' operator to search for '"
                             + to_string(a) + "' in " + to_string(b))
        if op == "instanceof":
            return self.instance_of(a, b)
        raise type_error("Unknown operator " + op)

    def instance_of(self, value, kind):
        if not is_callable(kind):
            raise type_error("Right-hand side of 'instanceof' is not callable")
        name = kind.name if isinstance(kind, Native) else ""
        if name == "Array":
            return isinstance(value, list)
        if name == "Object":
            return _is_object(value)
        if name == "Date":
            return isinstance(value, JSDate)
        if name == "RegExp":
            return isinstance(value, JSRegExp)
        if name in ("Set", "Map"):
            return isinstance(value, JSCollection) and value.kind == name
        if name == "Function":
            return is_callable(value)
        if name in ("Error", "TypeError", "RangeError", "SyntaxError",
                    "ReferenceError", "URIError"):
            if not (isinstance(value, dict) and isinstance(value.get("name"), str)
                    and "message" in value):
                return False
            return name == "Error" or value["name"] == name
        return False

    def get(self, obj, key, node=None):
        if isinstance(obj, dict):
            name = key if isinstance(key, str) else property_key(key)
            if name in obj:
                return obj[name]
            method = OBJECT_METHODS.get(name)
            if method is not None:
                return Native(name, method)
            return UNDEFINED
        if isinstance(obj, list):
            index = array_index(key)
            if index is not None:
                return obj[index] if index < len(obj) else UNDEFINED
            name = property_key(key)
            if name == "length":
                return float(len(obj))
            if isinstance(obj, MatchArray) and name in ("index", "input",
                                                         "groups"):
                return getattr(obj, name)
            method = ARRAY_METHODS.get(name)
            if method is not None:
                return Native(name, method)
            return UNDEFINED
        if isinstance(obj, str):
            index = array_index(key)
            if index is not None:
                return obj[index] if index < len(obj) else UNDEFINED
            name = property_key(key)
            if name == "length":
                return float(len(obj))
            method = STRING_METHODS.get(name)
            if method is not None:
                return Native(name, method)
            return UNDEFINED
        if obj is None or obj is UNDEFINED:
            raise type_error("Cannot read properties of " + to_string(obj)
                             + " (reading '" + property_key(key) + "')")
        name = property_key(key)
        if isinstance(obj, float):
            method = NUMBER_METHODS.get(name)
            return Native(name, method) if method else UNDEFINED
        if isinstance(obj, bool):
            if name == "toString":
                return Native(name, lambda i, t, a: to_string(t))
            return UNDEFINED
        if isinstance(obj, JSDate):
            method = DATE_METHODS.get(name)
            return Native(name, method) if method else UNDEFINED
        if isinstance(obj, JSCollection):
            if name == "size":
                return float(len(obj.keys))
            method = (SET_METHODS if obj.kind == "Set" else MAP_METHODS).get(name)
            return Native(name, method) if method else UNDEFINED
        if isinstance(obj, JSRegExp):
            if name == "source":
                return obj.source
            if name == "flags":
                return "".join(sorted(obj.flags, key="dgimsuy".index))
            if name == "global":
                return "g" in obj.flags
            if name == "ignoreCase":
                return "i" in obj.flags
            if name == "multiline":
                return "m" in obj.flags
            if name == "lastIndex":
                return float(obj.last_index)
            method = REGEXP_METHODS.get(name)
            return Native(name, method) if method else UNDEFINED
        if isinstance(obj, Native):
            if name in obj.props:
                return obj.props[name]
            if name == "name":
                return obj.name
            method = FUNCTION_METHODS.get(name)
            return Native(name, method) if method else UNDEFINED
        if isinstance(obj, Function):
            if name == "name":
                return obj.name
            if name == "length":
                return float(len(obj.params))
            method = FUNCTION_METHODS.get(name)
            return Native(name, method) if method else UNDEFINED
        return UNDEFINED

    def put(self, obj, key, value):
        if isinstance(obj, dict):
            obj[property_key(key)] = value
            return
        if isinstance(obj, list):
            index = array_index(key)
            if index is not None:
                if index < len(obj):
                    obj[index] = value
                    return
                if index >= MAX_ARRAY:
                    raise range_error("Invalid array length")
                obj.extend([UNDEFINED] * (index - len(obj)))
                obj.append(value)
                return
            name = property_key(key)
            if name == "length":
                length = to_number(value)
                if length != length or length < 0 or not length.is_integer() \
                        or length > 4294967295:
                    raise range_error("Invalid array length")
                if length > MAX_ARRAY:
                    raise range_error("Invalid array length")
                length = int(length)
                if length < len(obj):
                    del obj[length:]
                else:
                    obj.extend([UNDEFINED] * (length - len(obj)))
                return
            raise type_error("Cannot add property " + name + " to a list")
        if obj is None or obj is UNDEFINED:
            raise type_error("Cannot set properties of " + to_string(obj)
                             + " (setting '" + property_key(key) + "')")
        if isinstance(obj, JSRegExp) and property_key(key) == "lastIndex":
            obj.last_index = int(max(0, to_integer(value))) \
                if to_integer(value) != math.inf else MAX_STRING + 1
            return
        raise type_error("Cannot create property '" + property_key(key)
                         + "' on " + type_of(obj) + " '" + to_string(obj) + "'")

    def _install(self):
        scope = self.globals
        for name, value in make_globals(self).items():
            scope.declare(name, value, "const")


def _arg(args, index):
    return args[index] if index < len(args) else UNDEFINED


def _a_push(interp, this, args):
    this.extend(args)
    _check_array(this)
    return float(len(this))


def _a_pop(interp, this, args):
    return this.pop() if this else UNDEFINED


def _a_shift(interp, this, args):
    return this.pop(0) if this else UNDEFINED


def _a_unshift(interp, this, args):
    this[0:0] = args
    _check_array(this)
    return float(len(this))


def _a_slice(interp, this, args):
    length = len(this)
    start = int(_relative(_arg(args, 0), length, 0))
    end = int(_relative(_arg(args, 1), length, length))
    return this[start:end] if start < end else []


def _a_splice(interp, this, args):
    length = len(this)
    start = int(_relative(_arg(args, 0), length, 0))
    if not args:
        count = 0
    elif len(args) == 1:
        count = length - start
    else:
        count = int(max(0, min(to_integer(args[1]), length - start)))
    removed = this[start:start + count]
    this[start:start + count] = list(args[2:])
    _check_array(this)
    return removed


def _a_concat(interp, this, args):
    out = list(this)
    for arg in args:
        if isinstance(arg, list):
            out.extend(arg)
        else:
            out.append(arg)
        _check_array(out)
    return out


def _a_join(interp, this, args):
    separator = _arg(args, 0)
    separator = "," if separator is UNDEFINED else to_string(separator)
    return join_list(this, separator, {id(this)})


def _a_reverse(interp, this, args):
    this.reverse()
    return this


def _default_compare(a, b):
    if a is UNDEFINED:
        return 0 if b is UNDEFINED else 1
    if b is UNDEFINED:
        return -1
    return compare_strings(to_string(a), to_string(b))


def _sorted(interp, items, comparator):
    import functools
    if comparator is UNDEFINED:
        def compare(a, b):
            return _default_compare(a, b)
    else:
        if not is_callable(comparator):
            raise type_error("The comparison function must be either a "
                             "function or undefined")

        def compare(a, b):
            if a is UNDEFINED:
                return 0 if b is UNDEFINED else 1
            if b is UNDEFINED:
                return -1
            result = to_number(interp.call(comparator, UNDEFINED, [a, b]))
            if result != result:
                return 0
            return (result > 0) - (result < 0)
    return sorted(items, key=functools.cmp_to_key(compare))


def _a_sort(interp, this, args):
    this[:] = _sorted(interp, this, _arg(args, 0))
    return this


def _a_to_sorted(interp, this, args):
    return _sorted(interp, list(this), _arg(args, 0))


def _a_to_reversed(interp, this, args):
    return list(reversed(this))


def _a_index_of(interp, this, args):
    target = _arg(args, 0)
    length = len(this)
    start = int(_relative(_arg(args, 1), length, 0))
    for index in range(start, length):
        if strict_equals(this[index], target):
            return float(index)
    return -1.0


def _a_last_index_of(interp, this, args):
    target = _arg(args, 0)
    length = len(this)
    if len(args) > 1:
        start = to_integer(args[1])
        start = min(start, length - 1) if start >= 0 else length + start
    else:
        start = length - 1
    index = int(start) if start != -math.inf else -1
    while index >= 0:
        if strict_equals(this[index], target):
            return float(index)
        index -= 1
    return -1.0


def _a_includes(interp, this, args):
    target = _arg(args, 0)
    length = len(this)
    start = int(_relative(_arg(args, 1), length, 0))
    return any(same_value_zero(this[index], target)
               for index in range(start, length))


def _iterate_callback(interp, this, args):
    fn = _arg(args, 0)
    if not is_callable(fn):
        raise type_error(interp.display_short(fn) + " is not a function")
    this_arg = _arg(args, 1)
    length = len(this)
    index = 0
    while index < length and index < len(this):
        value = this[index]
        yield index, value, interp.call(fn, this_arg, [value, float(index), this])
        index += 1


def _a_find(interp, this, args):
    for _, value, result in _iterate_callback(interp, this, args):
        if truthy(result):
            return value
    return UNDEFINED


def _a_find_index(interp, this, args):
    for index, _, result in _iterate_callback(interp, this, args):
        if truthy(result):
            return float(index)
    return -1.0


def _a_find_last(interp, this, args):
    fn = _arg(args, 0)
    if not is_callable(fn):
        raise type_error(interp.display_short(fn) + " is not a function")
    for index in range(len(this) - 1, -1, -1):
        value = this[index] if index < len(this) else UNDEFINED
        if truthy(interp.call(fn, _arg(args, 1), [value, float(index), this])):
            return value
    return UNDEFINED


def _a_find_last_index(interp, this, args):
    fn = _arg(args, 0)
    if not is_callable(fn):
        raise type_error(interp.display_short(fn) + " is not a function")
    for index in range(len(this) - 1, -1, -1):
        value = this[index] if index < len(this) else UNDEFINED
        if truthy(interp.call(fn, _arg(args, 1), [value, float(index), this])):
            return float(index)
    return -1.0


def _a_filter(interp, this, args):
    return [value for _, value, result in
            _iterate_callback(interp, this, args) if truthy(result)]


def _a_map(interp, this, args):
    return [result for _, _, result in
            _iterate_callback(interp, this, args)]


def _a_for_each(interp, this, args):
    for _ in _iterate_callback(interp, this, args):
        pass
    return UNDEFINED


def _a_some(interp, this, args):
    for _, _, result in _iterate_callback(interp, this, args):
        if truthy(result):
            return True
    return False


def _a_every(interp, this, args):
    for _, _, result in _iterate_callback(interp, this, args):
        if not truthy(result):
            return False
    return True


def _a_reduce(interp, this, args, reverse=False):
    fn = _arg(args, 0)
    if not is_callable(fn):
        raise type_error(interp.display_short(fn) + " is not a function")
    indexes = list(range(len(this)))
    if reverse:
        indexes.reverse()
    if len(args) >= 2:
        acc = args[1]
    else:
        if not indexes:
            raise type_error("Reduce of empty array with no initial value")
        acc = this[indexes.pop(0)]
    for index in indexes:
        if index >= len(this):
            continue
        acc = interp.call(fn, UNDEFINED, [acc, this[index], float(index), this])
    return acc


def _a_reduce_right(interp, this, args):
    return _a_reduce(interp, this, args, True)


def _flatten(items, depth, out):
    for item in items:
        if isinstance(item, list) and depth >= 1:
            _flatten(item, depth - 1, out)
        else:
            out.append(item)
        _check_array(out)
    return out


def _a_flat(interp, this, args):
    depth = _arg(args, 0)
    depth = 1 if depth is UNDEFINED else to_integer(depth)
    return _flatten(this, depth, [])


def _a_flat_map(interp, this, args):
    return _flatten(_a_map(interp, this, args), 1, [])


def _a_fill(interp, this, args):
    length = len(this)
    start = int(_relative(_arg(args, 1), length, 0))
    end = int(_relative(_arg(args, 2), length, length))
    for index in range(start, end):
        this[index] = _arg(args, 0)
    return this


def _a_at(interp, this, args):
    index = to_integer(_arg(args, 0))
    if index < 0:
        index += len(this)
    if index < 0 or index >= len(this):
        return UNDEFINED
    return this[int(index)]


def _a_keys(interp, this, args):
    return [float(index) for index in range(len(this))]


def _a_values(interp, this, args):
    return list(this)


def _a_entries(interp, this, args):
    return [[float(index), value] for index, value in enumerate(this)]


def _a_to_string(interp, this, args):
    return to_string(this)


ARRAY_METHODS = {
    "push": _a_push, "pop": _a_pop, "shift": _a_shift, "unshift": _a_unshift,
    "slice": _a_slice, "splice": _a_splice, "concat": _a_concat,
    "join": _a_join, "reverse": _a_reverse, "sort": _a_sort,
    "toSorted": _a_to_sorted, "toReversed": _a_to_reversed,
    "indexOf": _a_index_of, "lastIndexOf": _a_last_index_of,
    "includes": _a_includes, "find": _a_find, "findIndex": _a_find_index,
    "findLast": _a_find_last, "findLastIndex": _a_find_last_index,
    "filter": _a_filter, "map": _a_map, "forEach": _a_for_each,
    "some": _a_some, "every": _a_every, "reduce": _a_reduce,
    "reduceRight": _a_reduce_right, "flat": _a_flat, "flatMap": _a_flat_map,
    "fill": _a_fill, "at": _a_at, "keys": _a_keys, "values": _a_values,
    "entries": _a_entries, "toString": _a_to_string,
}


def _this_string(this):
    return this if isinstance(this, str) else to_string(this)


def _s_char_at(interp, this, args):
    index = to_integer(_arg(args, 0))
    return this[int(index)] if 0 <= index < len(this) else ""


def _s_code_point_at(interp, this, args):
    index = to_integer(_arg(args, 0))
    return float(ord(this[int(index)])) if 0 <= index < len(this) else math.nan


def _s_at(interp, this, args):
    index = to_integer(_arg(args, 0))
    if index < 0:
        index += len(this)
    return this[int(index)] if 0 <= index < len(this) else UNDEFINED


def _s_index_of(interp, this, args):
    needle = to_string(_arg(args, 0))
    start = _clamp_index(_arg(args, 1), len(this))
    return float(this.find(needle, start))


def _s_last_index_of(interp, this, args):
    needle = to_string(_arg(args, 0))
    position = to_number(_arg(args, 1))
    if position != position:
        start = len(this)
    else:
        start = _clamp_index(position, len(this))
    return float(this.rfind(needle, 0, start + len(needle)))


def _no_regex(value, name):
    if isinstance(value, JSRegExp):
        raise type_error("First argument to String.prototype." + name
                         + " must not be a regular expression")


def _s_includes(interp, this, args):
    _no_regex(_arg(args, 0), "includes")
    return this.find(to_string(_arg(args, 0)),
                     _clamp_index(_arg(args, 1), len(this))) >= 0


def _s_starts_with(interp, this, args):
    _no_regex(_arg(args, 0), "startsWith")
    return this.startswith(to_string(_arg(args, 0)),
                           _clamp_index(_arg(args, 1), len(this)))


def _s_ends_with(interp, this, args):
    _no_regex(_arg(args, 0), "endsWith")
    end = len(this) if _arg(args, 1) is UNDEFINED else \
        _clamp_index(args[1], len(this))
    return this[:end].endswith(to_string(_arg(args, 0)))


def _s_slice(interp, this, args):
    length = len(this)
    start = int(_relative(_arg(args, 0), length, 0))
    end = int(_relative(_arg(args, 1), length, length))
    return this[start:end] if start < end else ""


def _s_substring(interp, this, args):
    length = len(this)
    start = _clamp_index(_arg(args, 0), length)
    end = length if _arg(args, 1) is UNDEFINED else \
        _clamp_index(args[1], length)
    if start > end:
        start, end = end, start
    return this[start:end]


def _s_substr(interp, this, args):
    length = len(this)
    start = int(_relative(_arg(args, 0), length, 0))
    count = length - start if _arg(args, 1) is UNDEFINED else \
        to_integer(args[1])
    count = int(max(0, min(count, length - start))) \
        if count != math.inf else length - start
    return this[start:start + count]


def _trim(this, start, end):
    if start and end:
        return this.strip(JS_SPACE)
    if start:
        return this.lstrip(JS_SPACE)
    return this.rstrip(JS_SPACE)


def _pad(this, args, at_start):
    target = to_integer(_arg(args, 0))
    filler = _arg(args, 1)
    filler = " " if filler is UNDEFINED else to_string(filler)
    if target <= len(this) or not filler:
        return this
    if target > MAX_STRING:
        raise range_error("Invalid string length")
    needed = int(target) - len(this)
    pad = (filler * (needed // len(filler) + 1))[:needed]
    return pad + this if at_start else this + pad


def _s_repeat(interp, this, args):
    count = to_integer(_arg(args, 0))
    if count < 0 or count == math.inf:
        raise range_error("Invalid count value: " + to_string(
            to_number(_arg(args, 0))))
    if len(this) * count > MAX_STRING:
        raise range_error("Invalid string length")
    return this * int(count)


def _s_split(interp, this, args):
    separator = _arg(args, 0)
    limit = _arg(args, 1)
    limit = 4294967295 if limit is UNDEFINED else to_uint32(limit)
    if limit == 0:
        return []
    if separator is UNDEFINED:
        return [this]
    if isinstance(separator, JSRegExp):
        size = len(this)
        if size == 0:
            return [] if separator.compiled.match(this, 0) else [this]
        out = []
        p = 0
        q = 0
        while q < size:
            match = separator.compiled.match(this, q)
            if match is None:
                q += 1
                continue
            end = min(match.end(), size)
            if end == p:
                q += 1
                continue
            out.append(this[p:q])
            if len(out) >= limit:
                return out
            p = end
            for group in match.groups():
                out.append(UNDEFINED if group is None else group)
                if len(out) >= limit:
                    return out
            _check_array(out)
            q = p
        out.append(this[p:])
        return out
    separator = to_string(separator)
    if separator == "":
        return _check_array(list(this))[:limit]
    return _check_array(this.split(separator))[:limit]


def _replace_one(interp, subject, matched, position, groups, named, replacement):
    if is_callable(replacement):
        call_args = [matched] + list(groups) + [float(position), subject]
        if named is not None and named is not UNDEFINED:
            call_args.append(named)
        return to_string(interp.call(replacement, UNDEFINED, call_args))
    return expand_replacement(to_string(replacement), matched, position,
                              subject, groups, named)


def _s_replace(interp, this, args, replace_all=False):
    pattern = _arg(args, 0)
    replacement = _arg(args, 1)
    if isinstance(pattern, JSRegExp):
        if replace_all and "g" not in pattern.flags:
            raise type_error("replaceAll must be called with a global RegExp")
        if "g" in pattern.flags:
            matches = regex_all(pattern, this)
            pattern.last_index = 0
        else:
            match = pattern.compiled.search(this, 0)
            matches = [match] if match else []
        out = []
        last = 0
        for match in matches:
            groups = [UNDEFINED if g is None else g for g in match.groups()]
            named = {name: (UNDEFINED if match.group(name) is None
                            else match.group(name)) for name in pattern.names} \
                if pattern.names else None
            out.append(this[last:match.start()])
            out.append(_replace_one(interp, this, match.group(0), match.start(),
                                    groups, named, replacement))
            last = match.end()
        out.append(this[last:])
        return _check_string("".join(out))
    needle = to_string(pattern)
    positions = []
    if replace_all:
        start = 0
        step = max(1, len(needle))
        while True:
            found = this.find(needle, start)
            if found < 0:
                break
            positions.append(found)
            start = found + step
            if start > len(this):
                break
    else:
        found = this.find(needle)
        if found >= 0:
            positions.append(found)
    out = []
    last = 0
    for position in positions:
        out.append(this[last:position])
        out.append(_replace_one(interp, this, needle, position, [], None,
                                replacement))
        last = position + len(needle)
    out.append(this[last:])
    return _check_string("".join(out))


def _s_replace_all(interp, this, args):
    return _s_replace(interp, this, args, True)


def _as_regexp(interp, value, flags=""):
    if isinstance(value, JSRegExp):
        return value
    source = "(?:)" if value is UNDEFINED else to_string(value)
    try:
        compiled, names = compile_regex(source, flags, interp.line)
    except ScriptError as error:
        raise Thrown(error_object("SyntaxError", error.text))
    return JSRegExp(source, flags, compiled, names)


def _s_match(interp, this, args):
    regex = _as_regexp(interp, _arg(args, 0))
    if "g" not in regex.flags:
        return regex_exec(regex, this)
    matches = regex_all(regex, this)
    regex.last_index = 0
    if not matches:
        return None
    return [match.group(0) for match in matches]


def _s_match_all(interp, this, args):
    regex = _arg(args, 0)
    if isinstance(regex, JSRegExp) and "g" not in regex.flags:
        raise type_error("matchAll must be called with a global RegExp")
    regex = _as_regexp(interp, regex, "g")
    return [_match_array(regex, match, this) for match in regex_all(regex, this)]


def _s_search(interp, this, args):
    regex = _as_regexp(interp, _arg(args, 0))
    match = regex.compiled.search(this, 0)
    return float(match.start()) if match else -1.0


def _s_concat(interp, this, args):
    return _check_string(this + "".join(to_string(arg) for arg in args))


def _s_locale_compare(interp, this, args):
    return float(compare_strings(this, to_string(_arg(args, 0))))


def _s_normalize(interp, this, args):
    form = _arg(args, 0)
    form = "NFC" if form is UNDEFINED else to_string(form)
    if form not in ("NFC", "NFD", "NFKC", "NFKD"):
        raise range_error("The normalization form should be one of NFC, NFD, "
                          "NFKC, NFKD.")
    return unicodedata.normalize(form, this)


def _string_method(fn):
    def wrapper(interp, this, args):
        if this is None or this is UNDEFINED:
            raise type_error("String.prototype method called on null or "
                             "undefined")
        return fn(interp, _this_string(this), args)
    return wrapper


STRING_METHODS = {name: _string_method(fn) for name, fn in {
    "charAt": _s_char_at,
    "charCodeAt": _s_code_point_at,
    "codePointAt": _s_code_point_at,
    "at": _s_at,
    "indexOf": _s_index_of,
    "lastIndexOf": _s_last_index_of,
    "includes": _s_includes,
    "startsWith": _s_starts_with,
    "endsWith": _s_ends_with,
    "slice": _s_slice,
    "substring": _s_substring,
    "substr": _s_substr,
    "toUpperCase": lambda i, t, a: _check_string(t.upper()),
    "toLowerCase": lambda i, t, a: t.lower(),
    "toLocaleUpperCase": lambda i, t, a: _check_string(t.upper()),
    "toLocaleLowerCase": lambda i, t, a: t.lower(),
    "trim": lambda i, t, a: _trim(t, True, True),
    "trimStart": lambda i, t, a: _trim(t, True, False),
    "trimEnd": lambda i, t, a: _trim(t, False, True),
    "padStart": lambda i, t, a: _pad(t, a, True),
    "padEnd": lambda i, t, a: _pad(t, a, False),
    "repeat": _s_repeat,
    "split": _s_split,
    "replace": _s_replace,
    "replaceAll": _s_replace_all,
    "match": _s_match,
    "matchAll": _s_match_all,
    "search": _s_search,
    "concat": _s_concat,
    "localeCompare": _s_locale_compare,
    "normalize": _s_normalize,
    "toString": lambda i, t, a: t,
    "valueOf": lambda i, t, a: t,
}.items()}


def to_fixed(value, digits):
    from decimal import Context, Decimal, ROUND_HALF_UP
    if value != value:
        return "NaN"
    if abs(value) >= 1e21 or value in (math.inf, -math.inf):
        return number_to_string(value)
    negative = value < 0
    context = Context(prec=1000)
    exact = Decimal(abs(value)).quantize(Decimal(1).scaleb(-digits, context),
                                         rounding=ROUND_HALF_UP, context=context)
    text = format(exact, "f")
    if digits == 0 and "." in text:
        text = text.split(".")[0]
    return ("-" if negative else "") + text


def _n_to_fixed(interp, this, args):
    digits = to_integer(_arg(args, 0))
    if digits < 0 or digits > 100:
        raise range_error("toFixed() digits argument must be between 0 and 100")
    return to_fixed(this, int(digits))


def _n_to_string(interp, this, args):
    radix = _arg(args, 0)
    radix = 10 if radix is UNDEFINED else to_integer(radix)
    if radix < 2 or radix > 36:
        raise range_error("toString() radix must be between 2 and 36")
    if radix == 10 or this != this or this in (math.inf, -math.inf):
        return number_to_string(this)
    if not this.is_integer():
        raise range_error("toString() with a radix needs a whole number")
    number = int(abs(this))
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    out = ""
    while True:
        number, rest = divmod(number, int(radix))
        out = digits[rest] + out
        if number == 0:
            break
    return ("-" if this < 0 else "") + out


NUMBER_METHODS = {
    "toFixed": _n_to_fixed,
    "toString": _n_to_string,
    "valueOf": lambda i, t, a: t,
}


def _date_field(index):
    def getter(interp, this, args):
        if this.ms != this.ms:
            return math.nan
        return float(civil_from_ms(this.ms)[index] - (1 if index == 1 else 0))
    return getter


def _date_setter(index):
    def setter(interp, this, args):
        if this.ms != this.ms and index != 0:
            return math.nan
        parts = list(civil_from_ms(0 if this.ms != this.ms else this.ms)[:7])
        parts[1] -= 1
        values = [to_number(arg) for arg in args]
        if not values:
            this.ms = math.nan
            return math.nan
        limit = {0: 3, 1: 2, 2: 1, 3: 4, 4: 3, 5: 2, 6: 1}[index]
        for offset, value in enumerate(values[:limit]):
            parts[index + offset] = value
        this.ms = time_clip(date_from_parts(*[float(p) for p in parts]))
        return this.ms
    return setter


def _d_to_iso(interp, this, args):
    if this.ms != this.ms:
        raise range_error("Invalid time value")
    return iso_string(this.ms)


DATE_METHODS = {
    "getTime": lambda i, t, a: t.ms,
    "valueOf": lambda i, t, a: t.ms,
    "toISOString": _d_to_iso,
    "toJSON": lambda i, t, a: None if t.ms != t.ms else iso_string(t.ms),
    "toString": lambda i, t, a: date_to_string(t.ms),
    "getTimezoneOffset": lambda i, t, a: math.nan if t.ms != t.ms else 0.0,
}
for _prefix in ("get", "getUTC"):
    DATE_METHODS[_prefix + "FullYear"] = _date_field(0)
    DATE_METHODS[_prefix + "Month"] = _date_field(1)
    DATE_METHODS[_prefix + "Date"] = _date_field(2)
    DATE_METHODS[_prefix + "Hours"] = _date_field(3)
    DATE_METHODS[_prefix + "Minutes"] = _date_field(4)
    DATE_METHODS[_prefix + "Seconds"] = _date_field(5)
    DATE_METHODS[_prefix + "Milliseconds"] = _date_field(6)
    DATE_METHODS[_prefix + "Day"] = _date_field(7)
for _prefix in ("set", "setUTC"):
    DATE_METHODS[_prefix + "FullYear"] = _date_setter(0)
    DATE_METHODS[_prefix + "Month"] = _date_setter(1)
    DATE_METHODS[_prefix + "Date"] = _date_setter(2)
    DATE_METHODS[_prefix + "Hours"] = _date_setter(3)
    DATE_METHODS[_prefix + "Minutes"] = _date_setter(4)
    DATE_METHODS[_prefix + "Seconds"] = _date_setter(5)
    DATE_METHODS[_prefix + "Milliseconds"] = _date_setter(6)


def _d_set_time(interp, this, args):
    this.ms = time_clip(to_number(_arg(args, 0)))
    return this.ms


DATE_METHODS["setTime"] = _d_set_time


def _r_test(interp, this, args):
    return regex_exec(this, to_string(_arg(args, 0))) is not None


def _r_exec(interp, this, args):
    return regex_exec(this, to_string(_arg(args, 0)))


REGEXP_METHODS = {
    "test": _r_test,
    "exec": _r_exec,
    "toString": lambda i, t, a: "/" + t.source + "/" + t.flags,
}


def _callable_arg(interp, args):
    fn = _arg(args, 0)
    if not is_callable(fn):
        raise type_error(interp.display_short(fn) + " is not a function")
    return fn


def _c_for_each(interp, this, args):
    fn = _callable_arg(interp, args)
    for key, value in list(zip(this.keys, this.values)):
        interp.call(fn, _arg(args, 1), [value if this.kind == "Map" else key,
                                        key, this])
    return UNDEFINED


def _c_clear(interp, this, args):
    this.keys = []
    this.values = []
    this.index = {}
    return UNDEFINED


def _set_add(interp, this, args):
    this.put(_arg(args, 0), _arg(args, 0))
    return this


def _map_set(interp, this, args):
    this.put(_arg(args, 0), _arg(args, 1))
    return this


def _map_get(interp, this, args):
    position = this.find(_arg(args, 0))
    return UNDEFINED if position is None else this.values[position]


SET_METHODS = {
    "add": _set_add,
    "has": lambda i, t, a: t.find(_arg(a, 0)) is not None,
    "delete": lambda i, t, a: t.remove(_arg(a, 0)),
    "clear": _c_clear,
    "forEach": _c_for_each,
    "values": lambda i, t, a: list(t.keys),
    "keys": lambda i, t, a: list(t.keys),
    "entries": lambda i, t, a: [[k, k] for k in t.keys],
}
MAP_METHODS = {
    "get": _map_get,
    "set": _map_set,
    "has": lambda i, t, a: t.find(_arg(a, 0)) is not None,
    "delete": lambda i, t, a: t.remove(_arg(a, 0)),
    "clear": _c_clear,
    "forEach": _c_for_each,
    "values": lambda i, t, a: list(t.values),
    "keys": lambda i, t, a: list(t.keys),
    "entries": lambda i, t, a: t.entries(),
}


def _make_collection(kind):
    def construct(interp, args):
        out = JSCollection(kind)
        source = _arg(args, 0)
        if source is UNDEFINED or source is None:
            return out
        for entry in interp.iterate(source, ("ident", 0, "iterable")):
            if kind == "Set":
                out.put(entry, entry)
            else:
                if not isinstance(entry, list):
                    raise type_error("Iterator value " + to_string(entry)
                                     + " is not an entry object")
                out.put(_arg(entry, 0), _arg(entry, 1))
        return out

    def call(interp, this, args):
        raise type_error("Constructor " + kind + " requires 'new'")
    return Native(kind, call, {}, construct)


def _o_has_own(interp, this, args):
    return property_key(_arg(args, 0)) in this


OBJECT_METHODS = {
    "hasOwnProperty": _o_has_own,
    "toString": lambda i, t, a: "[object Object]",
}


def _f_call(interp, this, args):
    return interp.call(this, _arg(args, 0), list(args[1:]))


def _f_apply(interp, this, args):
    extra = _arg(args, 1)
    if extra is UNDEFINED or extra is None:
        extra = []
    if not isinstance(extra, list):
        raise type_error("CreateListFromArrayLike called on non-object")
    return interp.call(this, _arg(args, 0), list(extra))


FUNCTION_METHODS = {"call": _f_call, "apply": _f_apply}




def _unary_math(fn, domain=None):
    def wrapper(interp, this, args):
        value = to_number(_arg(args, 0))
        if value != value:
            return math.nan
        try:
            return float(fn(value))
        except OverflowError:
            return math.inf
        except (ValueError, ZeroDivisionError):
            return domain(value) if domain else math.nan
    return wrapper


def js_round(value):
    if value != value or value in (math.inf, -math.inf) or value.is_integer():
        return value
    if 0 < value < 0.5:
        return 0.0
    if -0.5 <= value < 0:
        return -0.0
    floor = math.floor(value)
    return float(floor + 1 if value - floor >= 0.5 else floor)


def _m_min(interp, this, args):
    result = math.inf
    for arg in args:
        value = to_number(arg)
        if value != value:
            return math.nan
        if value < result or (value == result == 0 and math.copysign(1, value) < 0):
            result = value
    return result


def _m_max(interp, this, args):
    result = -math.inf
    for arg in args:
        value = to_number(arg)
        if value != value:
            return math.nan
        if value > result or (value == result == 0 and math.copysign(1, value) > 0):
            result = value
    return result


def _log_domain(value):
    if value == 0:
        return -math.inf
    return math.nan


def _m_sign(value):
    if value == 0 or value != value:
        return value
    return 1.0 if value > 0 else -1.0


def _m_trunc(value):
    if value in (math.inf, -math.inf):
        return value
    return float(math.trunc(value)) if value != 0 else value


def _m_floor(value):
    if value in (math.inf, -math.inf):
        return value
    return float(math.floor(value))


def _m_ceil(value):
    if value in (math.inf, -math.inf):
        return value
    result = float(math.ceil(value))
    return -0.0 if result == 0 and value < 0 else result


def _m_sqrt(value):
    if value < 0:
        return math.nan
    if value == math.inf:
        return value
    return math.sqrt(value)


def _m_cbrt(value):
    if value in (math.inf, -math.inf) or value == 0:
        return value
    result = math.cbrt(value)
    whole = round(result)
    if whole ** 3 == value:
        return float(whole)
    return result


def _m_exp(value):
    if value == -math.inf:
        return 0.0
    return math.exp(value)


def _m_log(value):
    if value == math.inf:
        return value
    return math.log(value)


def _m_log10(value):
    if value == math.inf:
        return value
    return math.log10(value)


def _m_log2(value):
    if value == math.inf:
        return value
    return math.log2(value)


def _m_hypot(interp, this, args):
    values = [to_number(arg) for arg in args]
    if any(v in (math.inf, -math.inf) for v in values):
        return math.inf
    if any(v != v for v in values):
        return math.nan
    return float(math.hypot(*values))


def _m_atan2(interp, this, args):
    return float(math.atan2(to_number(_arg(args, 0)), to_number(_arg(args, 1))))


def _m_random(interp, this, args):
    return float(interp.random())


def _trig(fn):
    def wrapper(value):
        if value in (math.inf, -math.inf):
            return math.nan
        return fn(value)
    return wrapper


def _make_math():
    props = {
        "PI": math.pi, "E": math.e, "LN2": math.log(2), "LN10": math.log(10),
        "LOG2E": 1 / math.log(2), "LOG10E": 1 / math.log(10),
        "SQRT2": math.sqrt(2), "SQRT1_2": math.sqrt(0.5),
        "abs": Native("abs", _unary_math(abs)),
        "floor": Native("floor", _unary_math(_m_floor)),
        "ceil": Native("ceil", _unary_math(_m_ceil)),
        "round": Native("round", _unary_math(js_round)),
        "trunc": Native("trunc", _unary_math(_m_trunc)),
        "sign": Native("sign", _unary_math(_m_sign)),
        "sqrt": Native("sqrt", _unary_math(_m_sqrt)),
        "cbrt": Native("cbrt", _unary_math(_m_cbrt)),
        "exp": Native("exp", _unary_math(_m_exp)),
        "log": Native("log", _unary_math(_m_log, _log_domain)),
        "log10": Native("log10", _unary_math(_m_log10, _log_domain)),
        "log2": Native("log2", _unary_math(_m_log2, _log_domain)),
        "sin": Native("sin", _unary_math(_trig(math.sin))),
        "cos": Native("cos", _unary_math(_trig(math.cos))),
        "tan": Native("tan", _unary_math(_trig(math.tan))),
        "asin": Native("asin", _unary_math(math.asin)),
        "acos": Native("acos", _unary_math(math.acos)),
        "atan": Native("atan", _unary_math(math.atan)),
        "atan2": Native("atan2", _m_atan2),
        "hypot": Native("hypot", _m_hypot),
        "pow": Native("pow", lambda i, t, a: js_pow(to_number(_arg(a, 0)),
                                                     to_number(_arg(a, 1)))),
        "min": Native("min", _m_min),
        "max": Native("max", _m_max),
        "random": Native("random", _m_random),
    }
    return props


def _not_callable(name):
    raise type_error(name + " is not a function")


def _json_stringify(interp, this, args):
    space = _arg(args, 2)
    indent = ""
    if isinstance(space, float):
        indent = " " * int(max(0, min(10, to_integer(space))))
    elif isinstance(space, str):
        indent = space[:10]
    return json_stringify(_arg(args, 0), indent)


def _json_parse(interp, this, args):
    return json_parse(to_string(_arg(args, 0)))


def _o_keys(interp, this, args):
    value = _arg(args, 0)
    if isinstance(value, dict):
        return own_keys(value)
    if isinstance(value, (list, str)):
        return [str(index) for index in range(len(value))]
    if value is None or value is UNDEFINED:
        raise type_error("Cannot convert undefined or null to object")
    return []


def _o_values(interp, this, args):
    value = _arg(args, 0)
    if isinstance(value, dict):
        return [value[key] for key in own_keys(value)]
    if isinstance(value, list):
        return list(value)
    if isinstance(value, str):
        return list(value)
    if value is None or value is UNDEFINED:
        raise type_error("Cannot convert undefined or null to object")
    return []


def _o_entries(interp, this, args):
    value = _arg(args, 0)
    if isinstance(value, dict):
        return [[key, value[key]] for key in own_keys(value)]
    if isinstance(value, (list, str)):
        return [[str(index), item] for index, item in enumerate(value)]
    if value is None or value is UNDEFINED:
        raise type_error("Cannot convert undefined or null to object")
    return []


def _o_assign(interp, this, args):
    target = _arg(args, 0)
    if not isinstance(target, dict):
        if target is None or target is UNDEFINED:
            raise type_error("Cannot convert undefined or null to object")
        return target
    for source in args[1:]:
        if isinstance(source, dict):
            for key in own_keys(source):
                target[key] = source[key]
        elif isinstance(source, (list, str)):
            for index, item in enumerate(source):
                target[str(index)] = item
    return target


def _o_from_entries(interp, this, args):
    out = {}
    for entry in interp.iterate(_arg(args, 0), ("ident", 0, "entries")):
        if not isinstance(entry, list):
            raise type_error("Iterator value " + to_string(entry)
                             + " is not an entry object")
        out[property_key(_arg(entry, 0))] = _arg(entry, 1)
    return out


def _make_object():
    props = {
        "keys": Native("keys", _o_keys),
        "values": Native("values", _o_values),
        "entries": Native("entries", _o_entries),
        "assign": Native("assign", _o_assign),
        "fromEntries": Native("fromEntries", _o_from_entries),
        "freeze": Native("freeze", lambda i, t, a: _arg(a, 0)),
        "hasOwn": Native("hasOwn", lambda i, t, a: isinstance(_arg(a, 0), dict)
                         and property_key(_arg(a, 1)) in a[0]),
    }

    def construct(interp, args):
        value = _arg(args, 0)
        return value if _is_object(value) else {}
    return Native("Object", lambda i, t, a: construct(i, a), props, construct)


def _new_array(interp, args):
    if len(args) == 1 and isinstance(args[0], float):
        length = args[0]
        if length < 0 or not length.is_integer() or length > MAX_ARRAY:
            raise range_error("Invalid array length")
        return [UNDEFINED] * int(length)
    return list(args)


def _array_from(interp, this, args):
    source = _arg(args, 0)
    fn = _arg(args, 1)
    if isinstance(source, list):
        items = list(source)
    elif isinstance(source, str):
        items = list(source)
    elif isinstance(source, JSCollection):
        items = source.entries()
    elif isinstance(source, dict) and "length" in source:
        length = to_integer(source["length"])
        if length > MAX_ARRAY:
            raise range_error("Invalid array length")
        items = [source.get(str(index), UNDEFINED)
                 for index in range(int(max(0, length)))]
    elif source is None or source is UNDEFINED:
        raise type_error(to_string(source) + " is not iterable")
    else:
        items = []
    if fn is not UNDEFINED:
        if not is_callable(fn):
            raise type_error(interp.display_short(fn) + " is not a function")
        items = [interp.call(fn, UNDEFINED, [item, float(index)])
                 for index, item in enumerate(items)]
    return items


def _make_array():
    props = {
        "isArray": Native("isArray", lambda i, t, a: isinstance(_arg(a, 0), list)),
        "from": Native("from", _array_from),
        "of": Native("of", lambda i, t, a: list(a)),
    }
    return Native("Array", lambda i, t, a: _new_array(i, a), props, _new_array)


def _parse_float(interp, this, args):
    text = to_string(_arg(args, 0)).lstrip(JS_SPACE)
    match = FLOAT_PREFIX_RE.match(text)
    if not match:
        return math.nan
    token = match.group(0)
    if token.lstrip("+-") == "Infinity":
        return -math.inf if token.startswith("-") else math.inf
    return float(token)


def _parse_int(interp, this, args):
    text = to_string(_arg(args, 0)).strip(JS_SPACE)
    radix = to_int32(_arg(args, 1))
    sign = 1
    if text[:1] in ("+", "-"):
        sign = -1 if text[0] == "-" else 1
        text = text[1:]
    strip_prefix = True
    if radix != 0:
        if radix < 2 or radix > 36:
            return math.nan
        if radix != 16:
            strip_prefix = False
    else:
        radix = 10
    if strip_prefix and text[:2].lower() == "0x":
        text = text[2:]
        radix = 16
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"[:radix]
    end = 0
    while end < len(text) and text[end].lower() in digits:
        end += 1
    if end == 0:
        return math.nan
    return float(sign * int(text[:end], radix))


def _make_number():
    props = {
        "isInteger": Native("isInteger", lambda i, t, a: isinstance(
            _arg(a, 0), float) and a[0] == a[0] and a[0] not in (
                math.inf, -math.inf) and a[0].is_integer()),
        "isSafeInteger": Native("isSafeInteger", lambda i, t, a: isinstance(
            _arg(a, 0), float) and a[0] == a[0] and a[0] not in (
                math.inf, -math.inf) and a[0].is_integer()
            and abs(a[0]) <= 9007199254740991),
        "isFinite": Native("isFinite", lambda i, t, a: isinstance(
            _arg(a, 0), float) and a[0] == a[0] and a[0] not in (
                math.inf, -math.inf)),
        "isNaN": Native("isNaN", lambda i, t, a: isinstance(_arg(a, 0), float)
                        and a[0] != a[0]),
        "parseFloat": Native("parseFloat", _parse_float),
        "parseInt": Native("parseInt", _parse_int),
        "MAX_SAFE_INTEGER": 9007199254740991.0,
        "MIN_SAFE_INTEGER": -9007199254740991.0,
        "EPSILON": 2.0 ** -52,
        "MAX_VALUE": 1.7976931348623157e308,
        "MIN_VALUE": 5e-324,
        "POSITIVE_INFINITY": math.inf,
        "NEGATIVE_INFINITY": -math.inf,
        "NaN": math.nan,
    }
    return Native("Number", lambda i, t, a: to_number(a[0]) if a else 0.0,
                  props)


def _from_char_code(interp, this, args):
    units = [to_uint32(arg) % 65536 for arg in args]
    text = "".join(chr(unit) for unit in units)
    return _combine_surrogates(text)


def _from_code_point(interp, this, args):
    out = []
    for arg in args:
        value = to_number(arg)
        if value != value or not value.is_integer() or value < 0 or \
                value > 0x10FFFF:
            raise range_error("Invalid code point " + to_string(arg))
        out.append(chr(int(value)))
    return _combine_surrogates("".join(out))


def _make_string():
    props = {
        "fromCharCode": Native("fromCharCode", _from_char_code),
        "fromCodePoint": Native("fromCodePoint", _from_code_point),
    }
    return Native("String", lambda i, t, a: to_string(a[0]) if a else "",
                  props)


def _make_error(name):
    def construct(interp, args):
        message = _arg(args, 0)
        return error_object(name, "" if message is UNDEFINED else
                            to_string(message))
    return Native(name, lambda i, t, a: construct(i, a), {}, construct)


def _new_date(interp, args):
    if not args:
        return JSDate(interp.now_ms)
    if len(args) == 1:
        value = args[0]
        if isinstance(value, JSDate):
            return JSDate(value.ms)
        value = to_primitive(value)
        if isinstance(value, str):
            return JSDate(parse_date_string(value))
        return JSDate(to_number(value))
    numbers = [to_number(arg) for arg in args[:7]]
    year = numbers[0]
    if year == year and 0 <= math.trunc(year) <= 99:
        year = 1900 + math.trunc(year)
    numbers[0] = year
    return JSDate(date_from_parts(*numbers))


def _date_utc(interp, this, args):
    numbers = [to_number(arg) for arg in args[:7]] or [math.nan]
    year = numbers[0]
    if year == year and 0 <= math.trunc(year) <= 99:
        numbers[0] = 1900 + math.trunc(year)
    if len(numbers) == 1:
        numbers.append(0.0)
    return time_clip(date_from_parts(*numbers))


def _make_date(interp):
    props = {
        "now": Native("now", lambda i, t, a: i.now_ms),
        "parse": Native("parse", lambda i, t, a: parse_date_string(
            to_string(_arg(a, 0)))),
        "UTC": Native("UTC", _date_utc),
    }
    return Native("Date", lambda i, t, a: date_to_string(i.now_ms), props,
                  _new_date)


def _new_regexp(interp, args):
    pattern = _arg(args, 0)
    flags = _arg(args, 1)
    if isinstance(pattern, JSRegExp):
        source = pattern.source
        flags = pattern.flags if flags is UNDEFINED else to_string(flags)
    else:
        source = "(?:)" if pattern is UNDEFINED else to_string(pattern)
        flags = "" if flags is UNDEFINED else to_string(flags)
    try:
        compiled, names = compile_regex(source, flags, interp.line)
    except ScriptError as error:
        raise Thrown(error_object("SyntaxError", error.text))
    return JSRegExp(source, flags, compiled, names)


def _console_log(interp, this, args):
    interp.log(args)
    return UNDEFINED


def make_globals(interp):
    console = {name: Native(name, _console_log)
               for name in ("log", "info", "warn", "error", "debug")}
    return {
        "undefined": UNDEFINED,
        "NaN": math.nan,
        "Infinity": math.inf,
        "Math": _make_math(),
        "JSON": {"parse": Native("parse", _json_parse),
                 "stringify": Native("stringify", _json_stringify)},
        "Object": _make_object(),
        "Array": _make_array(),
        "Number": _make_number(),
        "String": _make_string(),
        "Boolean": Native("Boolean", lambda i, t, a: truthy(_arg(a, 0))),
        "Date": _make_date(interp),
        "RegExp": Native("RegExp", lambda i, t, a: _new_regexp(i, a), {},
                         _new_regexp),
        "Set": _make_collection("Set"),
        "Map": _make_collection("Map"),
        "Function": Native("Function", lambda i, t, a: _not_callable(
            "Function")),
        "Error": _make_error("Error"),
        "TypeError": _make_error("TypeError"),
        "RangeError": _make_error("RangeError"),
        "SyntaxError": _make_error("SyntaxError"),
        "ReferenceError": _make_error("ReferenceError"),
        "URIError": _make_error("URIError"),
        "parseInt": Native("parseInt", _parse_int),
        "parseFloat": Native("parseFloat", _parse_float),
        "isNaN": Native("isNaN", lambda i, t, a: to_number(_arg(a, 0)) !=
                        to_number(_arg(a, 0))),
        "isFinite": Native("isFinite", lambda i, t, a: to_number(_arg(a, 0))
                           not in (math.inf, -math.inf)
                           and to_number(_arg(a, 0)) == to_number(_arg(a, 0))),
        "encodeURIComponent": Native("encodeURIComponent", lambda i, t, a:
                                     uri_encode(to_string(_arg(a, 0)))),
        "decodeURIComponent": Native("decodeURIComponent", lambda i, t, a:
                                     uri_decode(to_string(_arg(a, 0)))),
        "console": console,
    }


def native(name, fn):
    return Native(name, lambda interp, this, args: fn(interp, args))


def unwrap_items(value, fallback):
    if value is UNDEFINED:
        value = fallback
    if value is None:
        return []
    if isinstance(value, dict):
        value = [value]
    if not isinstance(value, list):
        return [{"value": to_json(value)}]
    out = []
    for item in value:
        if isinstance(item, dict) and isinstance(item.get("json"), dict) and \
                set(item) <= {"json", "binary", "pairedItem", "index"}:
            item = item["json"]
        converted = to_json(item)
        out.append(converted if isinstance(converted, dict)
                   else {"value": converted})
    return out
