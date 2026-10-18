#!/usr/bin/env python3

import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import workflow_script as script  # noqa: E402

CORPUS = json.loads((ROOT / "tests" / "fixtures" / "workflow_script_corpus.json")
                    .read_text(encoding="utf-8"))
NOW = 1_790_000_000_000


def _run(code, values=None):
    interp = script.Interpreter(now_ms=NOW)
    return script.to_json(interp.run(script.parse(code), values or {})), interp


def _error(code):
    with pytest.raises(script.ScriptError) as raised:
        _run(code)
    return str(raised.value)


@pytest.mark.parametrize("case", CORPUS, ids=[c["code"][:50] for c in CORPUS])
def test_scripts_give_the_same_results_as_javascript(case):
    assert _run(case["code"])[0] == case["expected"]


def test_strings_count_unicode_characters():
    assert _run("return ['😀a'.length, '😀a'[1], [...'😀a'].length]")[0] == \
        [2, "a", 2]


def test_console_log_is_collected_and_bounded():
    _, interp = _run("console.log('a', 1, {b: [2]}, null); "
                     "for (let i = 0; i < 200; i++) console.log(i)")
    assert interp.logs[0] == 'a 1 {"b":[2]} null'
    assert len(interp.logs) == script.MAX_LOGS
    _, interp = _run("console.log('x'.repeat(5000))")
    assert len(interp.logs[0]) == script.MAX_LOG_CHARS + 1


def test_errors_carry_the_line_number():
    assert _error("let a = 1;\n\nnull.b") == \
        "Line 3: TypeError: Cannot read properties of null (reading 'b')"
    assert _error("const x = 1;\nx = 2") == \
        "Line 2: TypeError: Assignment to constant variable."
    assert _error("throw new RangeError('far')") == "Line 1: RangeError: far"
    assert _error("throw 'plain'") == "Line 1: Uncaught plain"
    assert _error("throw {code: 4}") == 'Line 1: Uncaught {"code":4}'
    assert _error("function f() {\n  return g()\n}\nf()") == \
        "Line 2: ReferenceError: g is not defined"
    assert _error("let a = [1]\na.nope()") == \
        "Line 2: TypeError: a.nope is not a function"
    assert _error("const n = 5\nfor (const x of n) {}") == \
        "Line 2: TypeError: n is not iterable"


@pytest.mark.parametrize("code,message", [
    ("return (", "Line 1: Unexpected end of script"),
    ("let x = 1 2", "Line 1: Unexpected token number"),
    ("if (x) {", "Line 1: Unexpected end of script"),
    ("'open", "Line 1: Unterminated string"),
    ("`open ${1}", "Line 1: Unterminated template"),
    ("/* never closed", "Line 1: Unterminated comment"),
    ("break", "Line 1: Illegal break statement"),
    ("function f() { continue }", "Line 1: Illegal continue statement"),
    ("class A {}", "Line 1: class is not supported in scripts"),
    ("await x", "Line 1: await is not supported in scripts"),
    ("import fs from 'fs'", "Line 1: import is not supported in scripts"),
    ("const x;", "Line 1: Missing initializer in const declaration"),
    ("return {a = 1}", "Line 1: Invalid shorthand property initializer"),
    ("1 = 2", "Line 1: Invalid assignment target"),
    ("return /(/", "Line 1: Invalid regular expression /(/"),
    ("return 1n", "Line 1: Invalid number"),
    ("a\n.b\n.c(", "Line 3: Unexpected end of script"),
])
def test_syntax_errors(code, message):
    assert _error(code) == message


def test_automatic_semicolons_and_restricted_returns():
    assert _run("let a = 1\nlet b = 2\nreturn a + b")[0] == 3
    assert _run("function f() {\n  return\n  42\n}\nreturn f()")[0] is None
    assert _run("let i = 0\ni\n++i\nreturn i")[0] == 1
    assert _run("const s = 'x'\n;[1, 2].forEach(n => n)\nreturn s")[0] == "x"


def test_limits_stop_runaway_scripts():
    assert _error("while (true) {}") == \
        "Line 1: The script ran more than %d steps" % script.MAX_STEPS
    assert _error("function f() { return f() }\nreturn f()") == \
        "Line 1: Maximum call stack size exceeded"
    assert _error("return 'x'.repeat(2e6)") == \
        "Line 1: RangeError: Invalid string length"
    assert _error("let s = 'xxxxxxxxxx'; while (true) s += s") == \
        "Line 1: RangeError: Invalid string length"
    assert _error("return new Array(1e9)") == \
        "Line 1: RangeError: Invalid array length"
    assert _error("return Array.from({length: 1e9})") == \
        "Line 1: RangeError: Invalid array length"
    assert _error("const a = []; a[1e7] = 1") == \
        "Line 1: RangeError: Invalid array length"
    assert _error("return " + "(" * 80 + "1" + ")" * 80) == \
        "Line 1: The script nests too deeply"


def test_limits_cannot_be_caught():
    assert _error("try { while (true) {} } catch (e) { return 'caught' }") == \
        "Line 1: The script ran more than %d steps" % script.MAX_STEPS
    assert _error("function f() { return f() }\ntry { f() } catch (e) {}") == \
        "Line 1: Maximum call stack size exceeded"


def test_the_clock_deadline_stops_a_slow_script():
    ticks = iter(range(0, 10 ** 9, 500))
    interp = script.Interpreter(now_ms=NOW, clock=lambda: next(ticks),
                                deadline=10_000)
    with pytest.raises(script.ScriptError) as raised:
        interp.run(script.parse("for (;;) {}"), {})
    assert str(raised.value) == "Line 1: This run ran out of time"


@pytest.mark.parametrize("code", [
    "return ''.__class__",
    "return [].__proto__",
    "return ({}).constructor",
    "return Math.__dict__",
    "return (() => 1).__code__",
    "return JSON.parse.__globals__",
    "return 'x'.constructor",
    "return Object.prototype",
])
def test_host_internals_are_not_reachable(code):
    assert _run(code)[0] is None


@pytest.mark.parametrize("code,message", [
    ("return eval('1')", "Line 1: ReferenceError: eval is not defined"),
    ("return require('fs')", "Line 1: ReferenceError: require is not defined"),
    ("return process.env", "Line 1: ReferenceError: process is not defined"),
    ("return globalThis", "Line 1: ReferenceError: globalThis is not defined"),
    ("return fetch('https://example.com')",
     "Line 1: ReferenceError: fetch is not defined"),
    ("return Function('return 1')()", "Line 1: TypeError: Function is not a function"),
    ("return setTimeout", "Line 1: ReferenceError: setTimeout is not defined"),
])
def test_there_is_no_way_out_of_the_sandbox(code, message):
    assert _error(code) == message


def test_values_cross_into_and_out_of_scripts():
    item = {"n": 1, "f": 2.5, "big": 12345678901234567, "list": [True, None],
            "nested": {"x": "é"}}
    result, _ = _run("item.n += 1; item.added = [1.0, 2 ** 60, NaN, undefined, "
                     "() => 1, new Date(0)]; return item",
                     {"item": script.from_json(item)})
    assert result == {"n": 2, "f": 2.5, "big": 12345678901234568,
                      "list": [True, None], "nested": {"x": "é"},
                      "added": [1, 1152921504606846976, None, None, None,
                                "1970-01-01T00:00:00.000Z"]}
    assert item["n"] == 1
    assert script.unwrap_items(script.UNDEFINED, [{"a": 1.0}]) == [{"a": 1}]
    assert script.unwrap_items(None, []) == []
    assert script.unwrap_items("x", []) == [{"value": "x"}]
    assert script.unwrap_items([{"json": {"a": 1.0}, "pairedItem": 0}, 2.0],
                               []) == [{"a": 1}, {"value": 2}]
    with pytest.raises(script.ScriptLimit):
        _run("const a = {}; a.self = a; return a")


def test_dates_use_the_run_clock():
    assert _run("return [Date.now(), new Date().toISOString()]")[0] == \
        [NOW, "2026-09-21T14:13:20.000Z"]
    assert _run("return String(new Date(0))")[0] == \
        "Thu Jan 01 1970 00:00:00 GMT+0000 (Coordinated Universal Time)"


def test_number_to_string_matches_javascript():
    cases = {0.1: "0.1", 1e21: "1e+21", 1e-7: "1e-7", 123.456: "123.456",
             -1.5e-10: "-1.5e-10", 2 ** 53: "9007199254740992",
             1.7976931348623157e308: "1.7976931348623157e+308",
             5e-324: "5e-324", 100.0: "100", 1e20: "100000000000000000000"}
    for value, text in cases.items():
        assert script.number_to_string(float(value)) == text


def test_destructuring_assignment_with_defaults():
    assert _run("let a, b; [a = 1, b = 2] = [undefined, 5]; return [a, b]")[0] == [1, 5]
    assert _run("let x; ({y: x = 3} = {}); return x")[0] == 3


@pytest.mark.parametrize("pattern", ["/\\u{41/", "/\\u{zz}/", "/\\u{110000}/"])
def test_bad_unicode_escapes_in_regular_expressions_are_errors(pattern):
    assert _error("return " + pattern) == "Line 1: Invalid regular expression " + pattern
