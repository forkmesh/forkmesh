import base64
import hashlib
import hmac
import json
import math
import re
from decimal import Context, Decimal, ROUND_CEILING, ROUND_FLOOR, ROUND_HALF_UP

import workflow_script as script

ASCII_SPACE = " \t\n\r\x0b\x0c"
VARIABLE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
OFFSET_RE = re.compile(r"^([+-])([0-9]{2}):?([0-9]{2})$")
DATE_RE = re.compile(
    r"^([0-9]{4})-([0-9]{2})-([0-9]{2})"
    r"(?:[T ]([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?)?"
    r"[ ]*(Z|z|[+-][0-9]{2}:?[0-9]{2})?$")
NUMBER_TEXT_RE = re.compile(r"^[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$")
URL_RE = re.compile(r"https?://[^ \t\n\r\x0b\x0c<>\"'`]+", re.I)
EMAIL_RE = re.compile(
    r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}")
NUMBER_RE = re.compile(
    r"-?[0-9]{1,3}(?:,[0-9]{3})+(?:\.[0-9]+)?|-?[0-9]+(?:\.[0-9]+)?")
PHONE_RE = re.compile(r"\+?[0-9][0-9 ().-]{5,}[0-9]")
HASHTAG_RE = re.compile(r"(?<![A-Za-z0-9_&])#[A-Za-z0-9_]+")
MENTION_RE = re.compile(r"(?<![A-Za-z0-9._%+-])@[A-Za-z0-9_]+")
URI_RE = re.compile(
    r"^(?:([A-Za-z][A-Za-z0-9+.-]*):)?(?://([^/?#]*))?([^?#]*)"
    r"(?:\?([^#]*))?(?:#(.*))?$", re.S)
TOKEN_RE = re.compile(
    r"\[[^\]]*\]|YYYY|YY|MMMM|MMM|MM|M|DD|D|dddd|ddd|HH|H|hh|h|mm|m|ss|s"
    r"|SSS|A|a|ZZ|Z|X|x")
MONTHS = ("January", "February", "March", "April", "May", "June", "July",
          "August", "September", "October", "November", "December")
WEEKDAYS = ("Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
            "Saturday")
UNIT_MS = {"seconds": 1000, "minutes": 60000, "hours": 3600000,
           "days": 86400000, "weeks": 604800000}
MAX_WAIT_SECONDS = 60
MAX_REPEAT = 100
MAX_WORKFLOW_DEPTH = 3
MAX_TEXT = 1000000
DATE_MIN = -62135596800000
DATE_MAX = 253402300799999
DECIMALS = Context(prec=1000)
DEFAULT_SCRIPT = (
    "// items holds every incoming item. Change them, or build new ones,\n"
    "// and return the items this step passes on.\n"
    "for (const item of items) {\n"
    "  item.checkedAt = $now;\n"
    "}\n"
    "return items;\n"
)

SCRIPTING = "#9333ea"
TEXT = "#0d9488"
NUMBERS = "#4f46e5"
LISTS = "#ca8a04"
DATES = "#db2777"


def _f(key, label, kind="string", **extra):
    field = {"key": key, "label": label, "type": kind}
    field.update(extra)
    return field


def _options(*pairs):
    return [{"value": value, "label": label} for value, label in pairs]


def _save(default):
    return _f("field", "Save as", required=True, default=default,
              placeholder=default)


def _value(label="Text", default="{{ $json.text }}", kind="string"):
    return _f("value", label, kind, default=default, placeholder=default)


TIME_ZONE = _f("timeZone", "Time zone", placeholder="UTC, Europe/Berlin or +02:00")
UNITS = _options(("seconds", "Seconds"), ("minutes", "Minutes"),
                 ("hours", "Hours"), ("days", "Days"), ("weeks", "Weeks"),
                 ("months", "Months"), ("years", "Years"))

NODES = {
    "core.code": {
        "name": "Run script", "group": "Scripting", "icon": "code-xml",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "Run your own JavaScript on the items.",
        "fields": [
            _f("mode", "Run", "select", required=True, default="all",
               options=_options(("all", "Once for all items"),
                                ("each", "Once for each item"))),
            _f("code", "Script", "code", required=True, default=DEFAULT_SCRIPT),
        ],
    },
    "core.variable": {
        "name": "Set variable", "group": "Scripting", "icon": "variable",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "Keep a value for later steps as {{ $vars.name }}.",
        "fields": [
            _f("name", "Variable name", required=True, placeholder="total"),
            _f("value", "Value", "value", placeholder="{{ $json.amount }}"),
            _f("mode", "Action", "select", required=True, default="set",
               options=_options(("set", "Set to the value"),
                                ("append", "Add the value to a list"),
                                ("add", "Add the value to a number"))),
        ],
    },
    "core.switch": {
        "name": "Choose by value", "group": "Scripting", "icon": "signpost-big",
        "color": SCRIPTING, "outputs": ["case1", "case2", "case3", "otherwise"],
        "description": "Send each item down the branch whose value matches.",
        "fields": [
            _f("value", "Value", "value", required=True,
               placeholder="{{ $json.status }}"),
            _f("case1", "Case 1", placeholder="new"),
            _f("case2", "Case 2", placeholder="open"),
            _f("case3", "Case 3", placeholder="closed"),
        ],
    },
    "core.repeat": {
        "name": "Repeat", "group": "Scripting", "icon": "repeat",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "Pass every item on several times, counting up.",
        "fields": [
            _f("times", "Times", "number", required=True, default="3"),
            _f("field", "Count field", required=True, default="repeatIndex"),
        ],
    },
    "core.wait": {
        "name": "Wait", "group": "Scripting", "icon": "timer",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "Pause before the next step.",
        "fields": [_f("seconds", "Seconds", "number", required=True,
                      default="5")],
    },
    "core.runWorkflow": {
        "name": "Run workflow", "group": "Scripting", "icon": "workflow",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "Run another of your workflows with each item.",
        "fields": [_f("workflow", "Workflow name or ID", required=True,
                      placeholder="Send weekly report")],
    },
    "core.stop": {
        "name": "Stop workflow", "group": "Scripting", "icon": "octagon-x",
        "color": SCRIPTING, "outputs": ["main"],
        "description": "End the run here, as a success or with an error.",
        "fields": [
            _f("outcome", "End as", "select", required=True, default="success",
               options=_options(("success", "Success"), ("error", "Error"))),
            _f("message", "Error message", placeholder="Nothing to do today"),
        ],
    },
    "core.comment": {
        "name": "Comment", "group": "Scripting", "icon": "message-square-text",
        "color": "#64748b", "outputs": ["main"],
        "description": "A note on the canvas. Items pass through.",
        "fields": [_f("text", "Comment", "text",
                      placeholder="What this part of the workflow does")],
    },
    "core.textCase": {
        "name": "Change case", "group": "Text", "icon": "case-sensitive",
        "color": TEXT, "outputs": ["main"],
        "description": "Make text upper case, lower case or capitalized.",
        "fields": [
            _value(),
            _f("case", "Case", "select", required=True, default="upper",
               options=_options(("upper", "UPPER CASE"), ("lower", "lower case"),
                                ("capitalize", "Capitalize first letter"),
                                ("words", "Capitalize Every Word"))),
            _save("text"),
        ],
    },
    "core.textReplace": {
        "name": "Replace text", "group": "Text", "icon": "replace",
        "color": TEXT, "outputs": ["main"],
        "description": "Find text and replace every match.",
        "fields": [
            _value(),
            _f("find", "Find", required=True, placeholder="colour"),
            _f("replace", "Replace with", placeholder="color"),
            _f("regex", "Regular expression", "boolean", default="false"),
            _f("ignoreCase", "Ignore case", "boolean", default="false"),
            _save("text"),
        ],
    },
    "core.textMatch": {
        "name": "Match text", "group": "Text", "icon": "text-search",
        "color": TEXT, "outputs": ["main"],
        "description": "Find what a regular expression matches.",
        "fields": [
            _value(),
            _f("pattern", "Regular expression", required=True,
               placeholder="[0-9]+"),
            _f("result", "Get", "select", required=True, default="all",
               options=_options(("all", "All matches"),
                                ("first", "First match"),
                                ("groups", "Groups of the first match"),
                                ("test", "Whether it matches"))),
            _f("ignoreCase", "Ignore case", "boolean", default="false"),
            _save("matches"),
        ],
    },
    "core.textSplit": {
        "name": "Split text", "group": "Text", "icon": "scissors",
        "color": TEXT, "outputs": ["main"],
        "description": "Turn text into a list.",
        "fields": [
            _value(),
            _f("separator", "Split at", "select", required=True,
               default="newlines",
               options=_options(("newlines", "New lines"), ("spaces", "Spaces"),
                                ("commas", "Commas"),
                                ("characters", "Every character"),
                                ("custom", "Custom text"))),
            _f("custom", "Custom separator", placeholder=";"),
            _f("skipEmpty", "Leave out empty parts", "boolean", default="true"),
            _save("parts"),
        ],
    },
    "core.textCombine": {
        "name": "Combine text", "group": "Text", "icon": "combine",
        "color": TEXT, "outputs": ["main"],
        "description": "Join a list into one text.",
        "fields": [
            _value("List", "{{ $json.parts }}", "value"),
            _f("separator", "Join with", "select", required=True,
               default="newlines",
               options=_options(("newlines", "New lines"), ("spaces", "Spaces"),
                                ("commas", "Commas"), ("none", "Nothing"),
                                ("custom", "Custom text"))),
            _f("custom", "Custom separator", placeholder=" | "),
            _save("text"),
        ],
    },
    "core.textEncode": {
        "name": "Encode text", "group": "Text", "icon": "binary",
        "color": TEXT, "outputs": ["main"],
        "description": "Base64, URL or HTML encode and decode.",
        "fields": [
            _value(),
            _f("method", "Method", "select", required=True,
               default="base64Encode",
               options=_options(("base64Encode", "Base64 encode"),
                                ("base64Decode", "Base64 decode"),
                                ("urlEncode", "URL encode"),
                                ("urlDecode", "URL decode"),
                                ("htmlEscape", "HTML escape"),
                                ("htmlUnescape", "HTML unescape"))),
            _save("text"),
        ],
    },
    "core.hash": {
        "name": "Hash text", "group": "Text", "icon": "hash",
        "color": TEXT, "outputs": ["main"],
        "description": "MD5, SHA or HMAC of a text.",
        "fields": [
            _value(),
            _f("algorithm", "Algorithm", "select", required=True,
               default="sha256",
               options=_options(("md5", "MD5"), ("sha1", "SHA-1"),
                                ("sha256", "SHA-256"), ("sha512", "SHA-512"))),
            _f("key", "HMAC key", placeholder="Leave empty for a plain hash"),
            _f("encoding", "Output", "select", required=True, default="hex",
               options=_options(("hex", "Hex"), ("base64", "Base64"))),
            _save("hash"),
        ],
    },
    "core.textExtract": {
        "name": "Get from text", "group": "Text", "icon": "at-sign",
        "color": TEXT, "outputs": ["main"],
        "description": "Pick out links, emails, numbers, phone numbers or tags.",
        "fields": [
            _value(),
            _f("kind", "Get", "select", required=True, default="urls",
               options=_options(("urls", "Links"), ("emails", "Email addresses"),
                                ("numbers", "Numbers"),
                                ("phones", "Phone numbers"),
                                ("hashtags", "Hashtags"),
                                ("mentions", "Mentions"))),
            _save("found"),
        ],
    },
    "core.count": {
        "name": "Count", "group": "Text", "icon": "list-ordered",
        "color": TEXT, "outputs": ["main"],
        "description": "Count characters, words, lines or list entries.",
        "fields": [
            _value("Value", "{{ $json.text }}", "value"),
            _f("what", "Count", "select", required=True, default="characters",
               options=_options(("characters", "Characters"), ("words", "Words"),
                                ("lines", "Lines"), ("items", "List entries"))),
            _save("count"),
        ],
    },
    "core.math": {
        "name": "Calculate", "group": "Numbers", "icon": "calculator",
        "color": NUMBERS, "outputs": ["main"],
        "description": "Add, subtract, multiply, divide and more.",
        "fields": [
            _f("a", "Number", "value", required=True,
               placeholder="{{ $json.price }}"),
            _f("operator", "Operation", "select", required=True, default="add",
               options=_options(("add", "+ Add"), ("subtract", "− Subtract"),
                                ("multiply", "× Multiply"),
                                ("divide", "÷ Divide"),
                                ("modulo", "Remainder"), ("power", "Power"),
                                ("min", "Smaller of the two"),
                                ("max", "Larger of the two"),
                                ("sqrt", "Square root"),
                                ("abs", "Absolute value"))),
            _f("b", "Second number", "value", placeholder="1.2"),
            _save("result"),
        ],
    },
    "core.round": {
        "name": "Round number", "group": "Numbers", "icon": "sigma",
        "color": NUMBERS, "outputs": ["main"],
        "description": "Round to whole numbers or decimal places.",
        "fields": [
            _f("value", "Number", "value", required=True,
               placeholder="{{ $json.price }}"),
            _f("decimals", "Decimal places", "number", required=True,
               default="0"),
            _f("mode", "Round", "select", required=True, default="nearest",
               options=_options(("nearest", "To the nearest"),
                                ("up", "Always up"), ("down", "Always down"))),
            _save("result"),
        ],
    },
    "core.formatNumber": {
        "name": "Format number", "group": "Numbers", "icon": "binary",
        "color": NUMBERS, "outputs": ["main"],
        "description": "Write a number with fixed decimals and separators.",
        "fields": [
            _f("value", "Number", "value", required=True,
               placeholder="{{ $json.total }}"),
            _f("decimals", "Decimal places", "number", required=True,
               default="2"),
            _f("grouping", "Thousands separators", "boolean", default="true"),
            _save("text"),
        ],
    },
    "core.random": {
        "name": "Random number", "group": "Numbers", "icon": "dices",
        "color": NUMBERS, "outputs": ["main"],
        "description": "Pick a random number between two values.",
        "fields": [
            _f("min", "From", "number", required=True, default="1"),
            _f("max", "To", "number", required=True, default="100"),
            _f("whole", "Whole numbers only", "boolean", default="true"),
            _save("number"),
        ],
    },
    "core.statistics": {
        "name": "Calculate statistics", "group": "Lists", "icon": "chart-column",
        "color": LISTS, "outputs": ["main"],
        "description": "Sum, average, median and more over all items.",
        "fields": [
            _value("Number", "{{ $json.value }}", "value"),
            _f("operation", "Calculate", "select", required=True, default="sum",
               options=_options(("count", "Count"), ("sum", "Sum"),
                                ("average", "Average"), ("median", "Median"),
                                ("min", "Smallest"), ("max", "Largest"),
                                ("range", "Range"),
                                ("stdev", "Standard deviation"))),
            _save("result"),
        ],
    },
    "core.combineItems": {
        "name": "Combine items", "group": "Lists", "icon": "list-start",
        "color": LISTS, "outputs": ["main"],
        "description": "Gather all items, or one field of each, into one list.",
        "fields": [
            _f("value", "Value from each item", "value",
               placeholder="Leave empty to keep whole items"),
            _save("items"),
        ],
    },
    "core.sort": {
        "name": "Sort items", "group": "Lists", "icon": "arrow-down-up",
        "color": LISTS, "outputs": ["main"],
        "description": "Order items by a field, reverse or shuffle them.",
        "fields": [
            _f("by", "Sort by field", placeholder="name or data.total"),
            _f("order", "Order", "select", required=True, default="ascending",
               options=_options(("ascending", "Ascending"),
                                ("descending", "Descending"),
                                ("reverse", "Reverse the order"),
                                ("shuffle", "Shuffle"))),
        ],
    },
    "core.dedupe": {
        "name": "Remove duplicates", "group": "Lists", "icon": "list-x",
        "color": LISTS, "outputs": ["main"],
        "description": "Keep the first item for each value.",
        "fields": [_f("by", "Compare field", placeholder="Leave empty to compare whole items")],
    },
    "core.pick": {
        "name": "Get items", "group": "Lists", "icon": "list-ordered",
        "color": LISTS, "outputs": ["main"],
        "description": "Keep the first, last, a numbered or a random item.",
        "fields": [
            _f("which", "Get", "select", required=True, default="first",
               options=_options(("first", "First item"), ("last", "Last item"),
                                ("index", "Item number"),
                                ("range", "Items in a range"),
                                ("random", "A random item"))),
            _f("index", "Item number", "number", default="1"),
            _f("to", "Up to item number", "number", default="2"),
        ],
    },
    "core.json": {
        "name": "JSON", "group": "Lists", "icon": "braces",
        "color": LISTS, "outputs": ["main"],
        "description": "Read JSON text into fields, or write fields as JSON.",
        "fields": [
            _f("operation", "Action", "select", required=True, default="parse",
               options=_options(("parse", "Read JSON text"),
                                ("stringify", "Write as JSON text"))),
            _value("Value", "{{ $json.body }}", "value"),
            _f("pretty", "Indent the JSON", "boolean", default="false"),
            _save("data"),
        ],
    },
    "core.url": {
        "name": "Get URL parts", "group": "Lists", "icon": "link",
        "color": LISTS, "outputs": ["main"],
        "description": "Split a link into host, path and query.",
        "fields": [_value("URL", "{{ $json.url }}"), _save("link")],
    },
    "core.dateFormat": {
        "name": "Format date", "group": "Dates", "icon": "calendar",
        "color": DATES, "outputs": ["main"],
        "description": "Write a date in the format you choose.",
        "fields": [
            _value("Date", "{{ $now }}"),
            _f("format", "Format", "select", required=True, default="iso",
               options=_options(("iso", "ISO 8601 (2026-10-17T09:30:00Z)"),
                                ("date", "Date (2026-10-17)"),
                                ("time", "Time (09:30)"),
                                ("datetime", "Date and time (2026-10-17 09:30)"),
                                ("long", "Long (Saturday, 17 October 2026)"),
                                ("unix", "Unix seconds"),
                                ("unixMs", "Unix milliseconds"),
                                ("custom", "Custom pattern"))),
            _f("pattern", "Custom pattern", placeholder="DD/MM/YYYY HH:mm"),
            TIME_ZONE,
            _save("date"),
        ],
    },
    "core.dateAdjust": {
        "name": "Adjust date", "group": "Dates", "icon": "calendar-clock",
        "color": DATES, "outputs": ["main"],
        "description": "Add or subtract time, or go to the start of a day.",
        "fields": [
            _value("Date", "{{ $now }}"),
            _f("operation", "Action", "select", required=True, default="add",
               options=_options(("add", "Add"), ("subtract", "Subtract"),
                                ("startOf", "Start of"), ("endOf", "End of"))),
            _f("amount", "Amount", "number", default="1"),
            _f("unit", "Unit", "select", required=True, default="days",
               options=UNITS),
            TIME_ZONE,
            _save("date"),
        ],
    },
    "core.dateDiff": {
        "name": "Time between dates", "group": "Dates", "icon": "calendar-range",
        "color": DATES, "outputs": ["main"],
        "description": "How many seconds, days or months lie between two dates.",
        "fields": [
            _f("start", "From", required=True, default="{{ $json.start }}",
               placeholder="{{ $json.start }}"),
            _f("end", "To", required=True, default="{{ $now }}",
               placeholder="{{ $now }}"),
            _f("unit", "In", "select", required=True, default="days",
               options=UNITS),
            _save("difference"),
        ],
    },
}

GROUP_ORDER = ("Triggers", "Core", "Flow", "Scripting", "Text", "Numbers",
               "Lists", "Dates", "Desktop")


class NodeError(Exception):
    pass


class StopRun(Exception):
    def __init__(self, out):
        Exception.__init__(self)
        self.out = out


def stringify(value):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (dict, list)):
        return json.dumps(value)
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return str(value)


def number_out(value):
    if isinstance(value, float) and value.is_integer() and abs(value) < 1e18:
        return int(value)
    return value


def to_number(value, label):
    if isinstance(value, bool) or value is None:
        raise NodeError(label + " must be a number")
    if isinstance(value, (int, float)):
        number = float(value)
    else:
        text = str(value).strip()
        if not NUMBER_TEXT_RE.match(text):
            raise NodeError(label + " must be a number")
        number = float(text)
    if number != number or number in (math.inf, -math.inf):
        raise NodeError(label + " must be a number")
    return number


def copy_json(value):
    if isinstance(value, dict):
        return {key: copy_json(item) for key, item in value.items()}
    if isinstance(value, list):
        return [copy_json(item) for item in value]
    return value


def set_path(target, dotted, value):
    parts = str(dotted).split(".")
    for part in parts[:-1]:
        nxt = target.get(part)
        nxt = dict(nxt) if isinstance(nxt, dict) else {}
        target[part] = nxt
        target = nxt
    target[parts[-1]] = value


def with_field(item, field, value):
    out = dict(item)
    set_path(out, field, value)
    return out


def code_points(text):
    return list(text)


def change_case(text, mode):
    if mode == "upper":
        return text.upper()
    if mode == "lower":
        return text.lower()
    if mode == "capitalize":
        return text[:1].upper() + text[1:]
    out = []
    previous_space = True
    for ch in text:
        out.append(ch.upper() if previous_space else ch)
        previous_space = ch in ASCII_SPACE
    return "".join(out)


def regex(pattern, ignore_case, flags="g"):
    try:
        compiled, names = script.compile_regex(pattern, flags + (
            "i" if ignore_case else ""))
    except script.ScriptError:
        raise NodeError("Invalid regular expression " + pattern)
    return script.JSRegExp(pattern, flags, compiled, names)


def replace_text(text, find, replacement, use_regex, ignore_case):
    if not find:
        raise NodeError("Find is required")
    if not use_regex and not ignore_case:
        return text.replace(find, replacement)
    pattern = regex(find if use_regex else _escape(find), ignore_case)
    out = []
    last = 0
    for match in script.regex_all(pattern, text):
        groups = [script.UNDEFINED if g is None else g for g in match.groups()]
        named = {name: (script.UNDEFINED if match.group(name) is None
                        else match.group(name)) for name in pattern.names} \
            if pattern.names else None
        out.append(text[last:match.start()])
        out.append(script.expand_replacement(
            replacement if use_regex else replacement.replace("$", "$$"),
            match.group(0), match.start(), text, groups, named))
        last = match.end()
    out.append(text[last:])
    result = "".join(out)
    if len(result) > MAX_TEXT:
        raise NodeError("The text is too long")
    return result


def _escape(text):
    return "".join("\\" + ch if ch in "\\^$.|?*+()[]{}/" else ch for ch in text)


def match_text(text, pattern, mode, ignore_case):
    compiled = regex(pattern, ignore_case)
    if mode == "test":
        return compiled.compiled.search(text) is not None
    if mode == "all":
        return [match.group(0) for match in script.regex_all(compiled, text)]
    match = compiled.compiled.search(text)
    if mode == "first":
        return match.group(0) if match else ""
    if not match:
        return []
    return ["" if group is None else group for group in match.groups()]


def split_text(text, separator, custom, skip_empty):
    if separator == "newlines":
        parts = re.split(r"\r\n|\n|\r", text)
    elif separator == "spaces":
        parts = re.split("[" + re.escape(ASCII_SPACE) + "]+", text)
    elif separator == "commas":
        parts = [part.strip(ASCII_SPACE) for part in text.split(",")]
    elif separator == "characters":
        parts = code_points(text)
    else:
        if not custom:
            raise NodeError("Enter a custom separator")
        parts = text.split(custom)
    if skip_empty:
        parts = [part for part in parts if part != ""]
    return parts


def combine_text(value, separator, custom):
    joiner = {"newlines": "\n", "spaces": " ", "commas": ", ", "none": "",
              "custom": custom}[separator]
    items = value if isinstance(value, list) else (
        [] if value is None or value == "" else [value])
    result = joiner.join(stringify(item) for item in items)
    if len(result) > MAX_TEXT:
        raise NodeError("The text is too long")
    return result


def base64_encode(text):
    return base64.b64encode(text.encode("utf-8")).decode("ascii")


def base64_decode(text):
    clean = "".join(ch for ch in text if ch not in ASCII_SPACE)
    clean = clean.replace("-", "+").replace("_", "/").rstrip("=")
    if len(clean) % 4 == 1 or any(
            ch not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            "0123456789+/" for ch in clean):
        raise NodeError("The text is not valid Base64")
    data = base64.b64decode(clean + "=" * (-len(clean) % 4))
    return data.decode("utf-8", "replace")


def url_encode(text):
    out = []
    for byte in text.encode("utf-8"):
        ch = chr(byte)
        if ch.isascii() and (ch.isalnum() or ch in "_.-~"):
            out.append(ch)
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def percent_decode(text, plus=False):
    data = bytearray()
    out = []
    index = 0
    length = len(text)
    while index < length:
        ch = text[index]
        pair = text[index + 1:index + 3]
        if ch == "%" and len(pair) == 2 and all(
                c in "0123456789abcdefABCDEF" for c in pair):
            data.append(int(pair, 16))
            index += 3
            continue
        if data:
            out.append(data.decode("utf-8", "replace"))
            data = bytearray()
        out.append(" " if plus and ch == "+" else ch)
        index += 1
    if data:
        out.append(data.decode("utf-8", "replace"))
    return "".join(out)


def html_escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;").replace("'", "&#x27;"))


HTML_NAMED = {"amp": "&", "lt": "<", "gt": ">", "quot": '"', "apos": "'",
              "nbsp": "\xa0"}
HTML_ENTITY_RE = re.compile(r"&(#[0-9]{1,7}|#[xX][0-9A-Fa-f]{1,6}|[A-Za-z]+);")


def html_unescape(text):
    def replace(match):
        body = match.group(1)
        if body[0] == "#":
            code = int(body[2:], 16) if body[1] in "xX" else int(body[1:])
            if code == 0 or code > 0x10FFFF or 0xD800 <= code <= 0xDFFF:
                return match.group(0)
            return chr(code)
        return HTML_NAMED.get(body, match.group(0))
    return HTML_ENTITY_RE.sub(replace, text)


def encode_text(text, method):
    if method == "base64Encode":
        return base64_encode(text)
    if method == "base64Decode":
        return base64_decode(text)
    if method == "urlEncode":
        return url_encode(text)
    if method == "urlDecode":
        return percent_decode(text)
    if method == "htmlEscape":
        return html_escape(text)
    return html_unescape(text)


HASHES = {"md5": hashlib.md5, "sha1": hashlib.sha1, "sha256": hashlib.sha256,
          "sha512": hashlib.sha512}


def hash_text(text, algorithm, key="", encoding="hex"):
    if algorithm not in HASHES:
        raise NodeError("Unknown hash algorithm " + str(algorithm))
    data = text.encode("utf-8")
    if key:
        digest = hmac.new(key.encode("utf-8"), data, HASHES[algorithm]).digest()
    else:
        digest = HASHES[algorithm](data).digest()
    if encoding == "base64":
        return base64.b64encode(digest).decode("ascii")
    return digest.hex()


def _trim_url(url):
    url = url.rstrip(".,;:!?'\"")
    while url.endswith(")") and url.count("(") < url.count(")"):
        url = url[:-1].rstrip(".,;:!?'\"")
    return url


def extract(text, kind):
    if kind == "urls":
        return [_trim_url(m.group(0)) for m in URL_RE.finditer(text)]
    if kind == "emails":
        return [m.group(0) for m in EMAIL_RE.finditer(text)]
    if kind == "numbers":
        return [number_out(float(m.group(0).replace(",", "")))
                for m in NUMBER_RE.finditer(text)]
    if kind == "phones":
        found = []
        for m in PHONE_RE.finditer(text):
            digits = sum(ch in "0123456789" for ch in m.group(0))
            if 7 <= digits <= 15:
                found.append(m.group(0))
        return found
    if kind == "hashtags":
        return [m.group(0) for m in HASHTAG_RE.finditer(text)]
    return [m.group(0) for m in MENTION_RE.finditer(text)]


def count(value, what):
    if what == "items":
        if isinstance(value, (list, dict)):
            return len(value)
        return 0 if value is None or value == "" else 1
    text = stringify(value)
    if what == "characters":
        return len(text)
    if what == "words":
        return len([part for part in re.split(
            "[" + re.escape(ASCII_SPACE) + "]+", text) if part])
    if not text:
        return 0
    lines = re.split(r"\r\n|\n|\r", text)
    if lines and lines[-1] == "":
        lines.pop()
    return len(lines)


def py_mod(a, b):
    result = math.fmod(a, b)
    if result and (b < 0) != (result < 0):
        result += b
    return result


def calculate(a, operator, b):
    if operator == "sqrt":
        if a < 0:
            raise NodeError("Cannot take the square root of a negative number")
        return math.sqrt(a)
    if operator == "abs":
        return abs(a)
    if b is None:
        raise NodeError("Second number must be a number")
    if operator == "add":
        result = a + b
    elif operator == "subtract":
        result = a - b
    elif operator == "multiply":
        result = a * b
    elif operator == "divide":
        if b == 0:
            raise NodeError("Cannot divide by zero")
        result = a / b
    elif operator == "modulo":
        if b == 0:
            raise NodeError("Cannot divide by zero")
        result = py_mod(a, b)
    elif operator == "power":
        try:
            result = math.pow(a, b)
        except OverflowError:
            raise NodeError("The result is too large")
        except ValueError:
            if a == 0 and b < 0:
                raise NodeError("The result is too large")
            raise NodeError("The result is not a number")
    elif operator == "min":
        result = min(a, b)
    else:
        result = max(a, b)
    if result in (math.inf, -math.inf):
        raise NodeError("The result is too large")
    return result


def decimal_of(value):
    return Decimal(repr(float(value)))


def round_number(value, decimals, mode):
    places = int(max(-15, min(15, decimals)))
    rounding = {"up": ROUND_CEILING, "down": ROUND_FLOOR}.get(
        mode, ROUND_HALF_UP)
    exact = decimal_of(value)
    step = Decimal(1).scaleb(-places, DECIMALS)
    if places >= 0:
        rounded = exact.quantize(step, rounding=rounding, context=DECIMALS)
    else:
        rounded = DECIMALS.multiply(DECIMALS.divide(exact, step).quantize(
            Decimal(1), rounding=rounding, context=DECIMALS), step)
    return float(rounded) + 0.0


def format_number(value, decimals, grouping):
    places = int(max(0, min(15, decimals)))
    exact = decimal_of(value).quantize(Decimal(1).scaleb(-places, DECIMALS),
                                       rounding=ROUND_HALF_UP, context=DECIMALS)
    negative = exact < 0
    text = format(abs(exact), "f")
    whole, _, fraction = text.partition(".")
    if grouping:
        groups = []
        while len(whole) > 3:
            groups.insert(0, whole[-3:])
            whole = whole[:-3]
        groups.insert(0, whole)
        whole = ",".join(groups)
    result = whole + ("." + fraction if fraction else "")
    if negative and any(ch in "123456789" for ch in text):
        result = "-" + result
    return result


def statistics(values, operation):
    numbers = []
    for value in values:
        for entry in (value if isinstance(value, list) else [value]):
            if isinstance(entry, bool) or entry is None:
                continue
            if isinstance(entry, (int, float)):
                numbers.append(float(entry))
            elif isinstance(entry, str) and NUMBER_TEXT_RE.match(entry.strip()):
                numbers.append(float(entry.strip()))
    if operation == "count":
        return len(numbers)
    total = 0.0
    for number in numbers:
        total += number
    if operation == "sum":
        return total
    if not numbers:
        raise NodeError("There are no numbers to calculate with")
    if operation == "average":
        return total / len(numbers)
    ordered = sorted(numbers)
    if operation == "median":
        middle = len(ordered) // 2
        if len(ordered) % 2:
            return ordered[middle]
        return (ordered[middle - 1] + ordered[middle]) / 2
    if operation == "min":
        return ordered[0]
    if operation == "max":
        return ordered[-1]
    if operation == "range":
        return ordered[-1] - ordered[0]
    if len(numbers) < 2:
        raise NodeError("A standard deviation needs at least two numbers")
    mean = total / len(numbers)
    squares = 0.0
    for number in numbers:
        squares += (number - mean) * (number - mean)
    return math.sqrt(squares / (len(numbers) - 1))


def sort_key(value):
    if value is None:
        return None
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return (0, float(value), "")
    return (1, 0.0, stringify(value))


def sort_items(items, keys, order, random):
    if order == "reverse":
        return list(reversed(items))
    if order == "shuffle":
        out = list(items)
        for index in range(len(out) - 1, 0, -1):
            other = int(math.floor(random() * (index + 1)))
            out[index], out[other] = out[other], out[index]
        return out
    present = [(key, item) for key, item in zip(keys, items) if key is not None]
    missing = [item for key, item in zip(keys, items) if key is None]
    present.sort(key=lambda pair: pair[0], reverse=order == "descending")
    return [item for _, item in present] + missing


def parse_offset(text):
    match = OFFSET_RE.match(text)
    if not match:
        return None
    minutes = int(match.group(2)) * 60 + int(match.group(3))
    if int(match.group(2)) > 23 or int(match.group(3)) > 59:
        return None
    return minutes if match.group(1) == "+" else -minutes


class Zone:
    def __init__(self, text):
        text = str(text or "").strip()
        self.name = text
        self.fixed = None
        self.info = None
        if not text or text.upper() in ("UTC", "Z", "GMT"):
            self.fixed = 0
            return
        offset = parse_offset(text)
        if offset is not None:
            self.fixed = offset
            return
        try:
            from zoneinfo import ZoneInfo
            if not re.match(r"^[A-Za-z][A-Za-z0-9_+-]*(?:/[A-Za-z0-9_+-]+)*$",
                            text):
                raise ValueError(text)
            self.info = ZoneInfo(text)
        except Exception:
            raise NodeError("Unknown time zone " + text)

    def offset_minutes(self, ms):
        if self.fixed is not None:
            return self.fixed
        from datetime import datetime, timezone
        check_range(ms)
        moment = datetime.fromtimestamp(math.floor(ms / 1000), timezone.utc)
        delta = moment.astimezone(self.info).utcoffset()
        return int(delta.total_seconds() // 60)

    def local_to_utc(self, local_ms):
        if self.fixed is not None:
            return local_ms - self.fixed * 60000
        from datetime import datetime, timedelta
        check_range(local_ms)
        naive = datetime(1970, 1, 1) + timedelta(
            seconds=math.floor(local_ms / 1000))
        offset = naive.replace(tzinfo=self.info, fold=0).utcoffset()
        return local_ms - int(offset.total_seconds()) * 1000


def check_range(ms):
    if not DATE_MIN <= ms <= DATE_MAX:
        raise NodeError("The date is out of range")
    return ms


def parse_date(value, zone):
    if isinstance(value, bool) or value is None or value == "":
        raise NodeError("Cannot read the date " + stringify(value))
    if isinstance(value, (int, float)):
        number = float(value)
    else:
        text = str(value).strip()
        if NUMBER_TEXT_RE.match(text):
            number = float(text)
        else:
            match = DATE_RE.match(text)
            if not match:
                raise NodeError("Cannot read the date " + text[:80])
            year, month, day, hours, minutes, seconds, fraction, offset = \
                match.groups()
            year, month, day = int(year), int(month), int(day)
            hours, minutes, seconds = int(hours or 0), int(minutes or 0), \
                int(seconds or 0)
            millis = int((fraction or "0")[:3].ljust(3, "0"))
            if not (1 <= month <= 12 and 1 <= day <= script._month_days(
                    year, month) and hours <= 23 and minutes <= 59
                    and seconds <= 59):
                raise NodeError("Cannot read the date " + text[:80])
            local = script.date_from_parts(year, month - 1, day, hours, minutes,
                                           seconds, millis)
            if offset in ("Z", "z"):
                return check_range(int(local))
            if offset:
                minutes_offset = parse_offset(offset)
                if minutes_offset is None:
                    raise NodeError("Cannot read the date " + text[:80])
                return check_range(int(local - minutes_offset * 60000))
            return check_range(int(zone.local_to_utc(local)))
    if number != number or number in (math.inf, -math.inf):
        raise NodeError("Cannot read the date " + stringify(value))
    ms = number if abs(number) >= 1e11 else number * 1000
    return check_range(int(math.floor(ms)))


def local_parts(ms, zone):
    offset = zone.offset_minutes(ms)
    parts = script.civil_from_ms(ms + offset * 60000)
    return parts, offset


def offset_text(offset, colon=True):
    sign = "+" if offset >= 0 else "-"
    hours, minutes = divmod(abs(offset), 60)
    return "%s%02d%s%02d" % (sign, hours, ":" if colon else "", minutes)


def iso_date(ms, zone):
    (year, month, day, hours, minutes, seconds, millis, _), offset = \
        local_parts(ms, zone)
    text = "%04d-%02d-%02dT%02d:%02d:%02d" % (year, month, day, hours, minutes,
                                              seconds)
    if millis:
        text += ".%03d" % millis
    return text + ("Z" if zone.fixed == 0 else offset_text(offset))


def format_pattern(ms, zone, pattern):
    (year, month, day, hours, minutes, seconds, millis, weekday), offset = \
        local_parts(ms, zone)
    twelve = hours % 12 or 12

    def token(match):
        text = match.group(0)
        if text.startswith("["):
            return text[1:-1]
        return {
            "YYYY": "%04d" % year, "YY": "%02d" % (year % 100),
            "MMMM": MONTHS[month - 1], "MMM": MONTHS[month - 1][:3],
            "MM": "%02d" % month, "M": str(month), "DD": "%02d" % day,
            "D": str(day), "dddd": WEEKDAYS[weekday],
            "ddd": WEEKDAYS[weekday][:3], "HH": "%02d" % hours,
            "H": str(hours), "hh": "%02d" % twelve, "h": str(twelve),
            "mm": "%02d" % minutes, "m": str(minutes), "ss": "%02d" % seconds,
            "s": str(seconds), "SSS": "%03d" % millis,
            "A": "AM" if hours < 12 else "PM",
            "a": "am" if hours < 12 else "pm",
            "ZZ": offset_text(offset, False), "Z": offset_text(offset),
            "X": str(int(math.floor(ms / 1000))), "x": str(int(ms)),
        }[text]
    return TOKEN_RE.sub(token, pattern)


def format_date(value, fmt, pattern="", zone_name=""):
    zone = Zone(zone_name)
    ms = parse_date(value, zone)
    if fmt == "iso":
        return iso_date(ms, zone)
    if fmt == "unix":
        return int(math.floor(ms / 1000))
    if fmt == "unixMs":
        return int(ms)
    patterns = {"date": "YYYY-MM-DD", "time": "HH:mm",
                "datetime": "YYYY-MM-DD HH:mm", "long": "dddd, D MMMM YYYY"}
    if fmt == "custom":
        if not pattern:
            raise NodeError("Enter a custom pattern")
        return format_pattern(ms, zone, pattern)
    return format_pattern(ms, zone, patterns[fmt])


def add_months(ms, months, zone):
    (year, month, day, hours, minutes, seconds, millis, _), _ = \
        local_parts(ms, zone)
    total = year * 12 + (month - 1) + months
    year, month = divmod(total, 12)
    day = min(day, script._month_days(year, month + 1))
    local = script.date_from_parts(year, month, day, hours, minutes, seconds,
                                   millis)
    return int(zone.local_to_utc(local))


def add_days(ms, days, zone):
    (year, month, day, hours, minutes, seconds, millis, _), _ = \
        local_parts(ms, zone)
    local = script.date_from_parts(year, month - 1, day + days, hours, minutes,
                                   seconds, millis)
    return int(zone.local_to_utc(local))


def start_of(ms, unit, zone):
    (year, month, day, hours, minutes, seconds, _, weekday), _ = \
        local_parts(ms, zone)
    if unit == "seconds":
        local = script.date_from_parts(year, month - 1, day, hours, minutes,
                                       seconds)
    elif unit == "minutes":
        local = script.date_from_parts(year, month - 1, day, hours, minutes)
    elif unit == "hours":
        local = script.date_from_parts(year, month - 1, day, hours)
    elif unit == "days":
        local = script.date_from_parts(year, month - 1, day)
    elif unit == "weeks":
        local = script.date_from_parts(year, month - 1, day - (weekday + 6) % 7)
    elif unit == "months":
        local = script.date_from_parts(year, month - 1, 1)
    else:
        local = script.date_from_parts(year, 0, 1)
    return int(zone.local_to_utc(local))


def shift(ms, amount, unit, zone):
    if unit in ("seconds", "minutes", "hours"):
        return int(ms + math.trunc(amount * UNIT_MS[unit]))
    if not float(amount).is_integer():
        raise NodeError("Use a whole number of " + unit)
    amount = int(amount)
    if unit == "days":
        return add_days(ms, amount, zone)
    if unit == "weeks":
        return add_days(ms, amount * 7, zone)
    if unit == "months":
        return add_months(ms, amount, zone)
    return add_months(ms, amount * 12, zone)


def adjust_date(value, operation, amount, unit, zone_name=""):
    zone = Zone(zone_name)
    ms = parse_date(value, zone)
    if operation in ("startOf", "endOf"):
        result = start_of(ms, unit, zone)
        if operation == "endOf":
            result = shift(result, 1, unit, zone) - 1000
    else:
        if amount is None:
            raise NodeError("Amount is required")
        result = shift(ms, amount if operation == "add" else -amount, unit,
                       zone)
    return iso_date(check_range(result), zone)


def months_between(a, b):
    if b < a:
        return -months_between(b, a)
    utc = Zone("")
    ya, ma = script.civil_from_ms(a)[:2]
    yb, mb = script.civil_from_ms(b)[:2]
    months = (yb - ya) * 12 + (mb - ma)
    while months > 0 and add_months(a, months, utc) > b:
        months -= 1
    return months


def date_difference(start, end, unit):
    utc = Zone("")
    a = parse_date(start, utc)
    b = parse_date(end, utc)
    if unit in UNIT_MS:
        return int((b - a) / UNIT_MS[unit])
    months = months_between(a, b)
    if unit == "months":
        return months
    return int(months / 12)


def url_parts(text):
    match = URI_RE.match(text.strip())
    scheme, authority, path, query, fragment = match.groups()
    host = ""
    port = None
    user = ""
    if authority is not None:
        if "@" in authority:
            user, _, authority = authority.rpartition("@")
        if authority.startswith("["):
            end = authority.find("]")
            host = authority[:end + 1] if end > 0 else authority
            rest = authority[end + 1:] if end > 0 else ""
            if rest.startswith(":") and rest[1:].isdigit() and rest[1:].isascii():
                port = int(rest[1:])
        else:
            head, colon, tail = authority.rpartition(":")
            if colon and tail.isdigit() and tail.isascii():
                host, port = head, int(tail)
            elif colon and not tail:
                host = head
            else:
                host = authority
    params = {}
    for piece in (query or "").split("&"):
        if not piece:
            continue
        name, _, value = piece.partition("=")
        params[percent_decode(name, True)] = percent_decode(value, True)
    out = {
        "scheme": (scheme or "").lower(),
        "host": host.lower(),
        "port": port,
        "path": path,
        "query": params,
        "fragment": fragment or "",
    }
    out["origin"] = (out["scheme"] + "://" + out["host"] + (
        ":" + str(port) if port is not None else "")) if scheme and host else ""
    if user:
        out["user"] = percent_decode(user.partition(":")[0])
    return out


def utils_object(interp):
    def arg(args, index, default=""):
        value = args[index] if index < len(args) else script.UNDEFINED
        if value is script.UNDEFINED or value is None:
            return default
        return script.to_string(value)

    def guard(fn):
        def call(i, this, args):
            try:
                return fn(args)
            except NodeError as error:
                raise script.Thrown(script.error_object("Error", str(error)))
        return call

    def date_value(value):
        if isinstance(value, script.JSDate):
            if value.ms != value.ms:
                raise NodeError("Cannot read the date Invalid Date")
            return int(value.ms)
        if isinstance(value, float):
            return value
        return script.to_string(value)

    def format_date_dispatch(args):
        fmt = arg(args, 1, "iso")
        named = ("iso", "date", "time", "datetime", "long", "unix", "unixMs")
        value = args[0] if args else script.UNDEFINED
        if value is script.UNDEFINED:
            value = interp.now_ms
        if fmt in named:
            result = format_date(date_value(value), fmt, "", arg(args, 2))
        else:
            result = format_date(date_value(value), "custom", fmt, arg(args, 2))
        return float(result) if isinstance(result, int) else result

    def format_number_dispatch(args):
        value = script.to_number(args[0]) if args else math.nan
        if value != value or value in (math.inf, -math.inf):
            return script.to_string(value)
        decimals = script.to_integer(args[1]) if len(args) > 1 and \
            args[1] is not script.UNDEFINED else 2
        grouping = True if len(args) < 3 else script.truthy(args[2])
        return format_number(value, decimals, grouping)

    natives = {
        "hash": lambda args: hash_text(arg(args, 0), arg(args, 1, "sha256"),
                                       "", arg(args, 2, "hex")),
        "hmac": lambda args: hash_text(arg(args, 0), arg(args, 2, "sha256"),
                                       arg(args, 1), arg(args, 3, "hex")),
        "base64Encode": lambda args: base64_encode(arg(args, 0)),
        "base64Decode": lambda args: base64_decode(arg(args, 0)),
        "urlEncode": lambda args: url_encode(arg(args, 0)),
        "urlDecode": lambda args: percent_decode(arg(args, 0)),
        "formatDate": format_date_dispatch,
        "formatNumber": format_number_dispatch,
    }
    return {name: script.Native(name, guard(fn)) for name, fn in natives.items()}
