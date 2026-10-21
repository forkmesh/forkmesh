import json
import re
from pathlib import Path

MODES = ("dark", "light")
SELECTORS = {
    "dark": ':root,[data-dashboard-theme="dark"]',
    "light": '[data-dashboard-theme="light"]',
}
RGB_TOKENS = (
    "header", "sidebar", "background", "foreground", "card",
    "card-foreground", "popover", "popover-foreground", "primary",
    "primary-foreground", "secondary", "secondary-foreground", "muted",
    "muted-foreground", "accent", "accent-foreground", "destructive",
    "border", "ring", "canvas", "subtle", "scrollbar",
)
HEX_TOKENS = ("link", "link-hover", "tab-underline", "open")
TOKENS = RGB_TOKENS + HEX_TOKENS
THEME_RGB_TOKENS = RGB_TOKENS + (
    "cta", "cta-foreground", "success", "attention")
THEME_TOKENS = THEME_RGB_TOKENS + HEX_TOKENS
DEFAULT_THEME = "default"
THEMES_DIR = Path(__file__).resolve().parents[1] / "public" / "instance-themes"
MAX_STYLESHEET_BYTES = 64 * 1024
ALIASES = (
    ("background", "background"), ("foreground", "foreground"),
    ("card", "card"), ("primary", "primary"),
    ("primary-foreground", "primary-foreground"),
    ("secondary", "secondary"), ("muted-foreground", "muted-foreground"),
)
MAX_DOC_BYTES = 8 * 1024
_HEX_RE = re.compile(r"^#[0-9a-f]{6}$")
_PRESET_RE = re.compile(r"^[a-z][a-z0-9-]{0,40}$")

PRESETS = {
    "crimson": {
        "dark": {
            "header": "#09090b", "sidebar": "#18181b",
            "background": "#09090b", "foreground": "#f4f4f5",
            "card": "#18181b", "card-foreground": "#f4f4f5",
            "popover": "#27272a", "popover-foreground": "#f4f4f5",
            "primary": "#ef4444", "primary-foreground": "#ffffff",
            "secondary": "#27272a", "secondary-foreground": "#e4e4e7",
            "muted": "#27272a", "muted-foreground": "#a1a1aa",
            "accent": "#ef4444", "accent-foreground": "#ffffff",
            "destructive": "#f87171", "border": "#27272a",
            "ring": "#ef4444", "canvas": "#18181b", "subtle": "#18181b",
            "scrollbar": "#a1a1aa", "link": "#f87171",
            "link-hover": "#fca5a5", "tab-underline": "#ef4444",
        },
        "light": {
            "header": "#ffffff", "sidebar": "#f9fafb",
            "background": "#ffffff", "foreground": "#111827",
            "card": "#ffffff", "card-foreground": "#111827",
            "popover": "#ffffff", "popover-foreground": "#111827",
            "primary": "#dc2626", "primary-foreground": "#ffffff",
            "secondary": "#f9fafb", "secondary-foreground": "#111827",
            "muted": "#f9fafb", "muted-foreground": "#4b5563",
            "accent": "#dc2626", "accent-foreground": "#ffffff",
            "destructive": "#b91c1c", "border": "#e5e7eb",
            "ring": "#dc2626", "canvas": "#ffffff", "subtle": "#f9fafb",
            "scrollbar": "#71717a", "link": "#dc2626",
            "link-hover": "#b91c1c", "tab-underline": "#dc2626",
        },
    },
}


class ThemeError(Exception):
    pass


def _theme_colours(value):
    colours = {}
    for mode in MODES:
        tokens = (value or {}).get(mode) or {}
        cleaned = {name: str(tokens[name]).strip().lower()
                   for name in THEME_TOKENS
                   if name in tokens and _HEX_RE.fullmatch(
                       str(tokens[name]).strip().lower())}
        if cleaned:
            colours[mode] = cleaned
    return colours


def _theme_stylesheet(name):
    if not name or not re.fullmatch(r"[a-z][a-z0-9-]{0,40}\.css", name):
        return ""
    try:
        text = (THEMES_DIR / name).read_text(encoding="utf-8")
    except OSError:
        return ""
    if len(text.encode("utf-8")) > MAX_STYLESHEET_BYTES or "</" in text:
        return ""
    return text.strip()


def _load_themes(directory=None):
    try:
        data = json.loads(((directory or THEMES_DIR) / "themes.json")
                          .read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    themes = {}
    for entry in (data or {}).get("themes") or []:
        if not isinstance(entry, dict):
            continue
        theme_id = str(entry.get("id") or "")
        if not _PRESET_RE.fullmatch(theme_id) or theme_id in themes:
            continue
        themes[theme_id] = {
            "id": theme_id,
            "name": str(entry.get("name") or theme_id)[:40],
            "description": str(entry.get("description") or "")[:200],
            "status": "ready" if entry.get("status") == "ready"
            else "planned",
            "palette": [colour for colour in entry.get("palette") or []
                        if isinstance(colour, str)
                        and _HEX_RE.fullmatch(colour.lower())][:8],
            "colors": _theme_colours(entry.get("colors")),
            "stylesheet": _theme_stylesheet(entry.get("stylesheet")),
        }
    return themes


THEMES = _load_themes()


def theme_ready(name):
    entry = THEMES.get(name)
    return bool(entry) and entry["status"] == "ready"


def catalogue():
    return [{key: entry[key] for key in
             ("id", "name", "description", "status", "palette")}
            for entry in THEMES.values()]


def clean(value):
    if isinstance(value, str):
        name = value.strip().lower()
        if name == DEFAULT_THEME:
            return {}
        if _PRESET_RE.fullmatch(name) and theme_ready(name):
            return {"preset": name}
        if not _PRESET_RE.fullmatch(name) or name not in PRESETS:
            raise ThemeError("unknown theme preset")
        value = PRESETS[name]
    if not isinstance(value, dict):
        raise ThemeError("the theme is not an object")
    unknown = sorted(set(value) - set(MODES) - {"preset"})
    if unknown:
        raise ThemeError("unknown theme mode: " + str(unknown[0])[:40])
    theme = {}
    if "preset" in value:
        name = str(value.get("preset") or "").strip().lower()
        if name == DEFAULT_THEME or not name:
            pass
        elif _PRESET_RE.fullmatch(name) and theme_ready(name):
            theme["preset"] = name
        else:
            raise ThemeError("unknown theme preset")
    for mode in MODES:
        tokens = value.get(mode)
        if tokens is None:
            continue
        if not isinstance(tokens, dict):
            raise ThemeError("the " + mode + " theme is not an object")
        cleaned = {}
        for name, colour in tokens.items():
            if name not in TOKENS:
                raise ThemeError("unknown theme token: " + str(name)[:40])
            colour = str(colour or "").strip().lower()
            if not _HEX_RE.fullmatch(colour):
                raise ThemeError(
                    "theme token " + name + " must be a #rrggbb colour")
            cleaned[name] = colour
        if cleaned:
            theme[mode] = {name: cleaned[name] for name in TOKENS
                           if name in cleaned}
    return theme


def parse(raw):
    try:
        return clean(json.loads(raw)) if raw else {}
    except (ValueError, ThemeError):
        return {}


def dump(theme):
    doc = json.dumps(theme, separators=(",", ":"), sort_keys=True)
    if len(doc.encode("utf-8")) > MAX_DOC_BYTES:
        raise ThemeError("the theme is too large")
    return doc


def _rgb(colour):
    return "%d %d %d" % tuple(
        int(colour[index:index + 2], 16) for index in (1, 3, 5))


def css(theme):
    preset = THEMES.get(theme.get("preset") or "")
    if not preset:
        return _token_css(theme, RGB_TOKENS)
    blocks = [_token_css(preset["colors"], THEME_RGB_TOKENS),
              preset["stylesheet"], _token_css(theme, RGB_TOKENS)]
    return "".join(block.rstrip("\n") + "\n" for block in blocks if block)


def preview_css(name):
    name = str(name or "").strip().lower()
    if name == DEFAULT_THEME:
        return ""
    if not theme_ready(name):
        return None
    return css({"preset": name})


def _token_css(theme, rgb_tokens):
    blocks = []
    for mode in MODES:
        tokens = theme.get(mode) or {}
        lines = []
        for name in rgb_tokens:
            if name in tokens:
                lines.append("--dashboard-%s-rgb:%s" % (
                    name, _rgb(tokens[name])))
        for name in HEX_TOKENS:
            if name in tokens:
                lines.append("--dashboard-%s:%s" % (name, tokens[name]))
        for alias, name in ALIASES:
            if name in tokens:
                lines.append("--%s:%s" % (alias, tokens[name]))
        if lines:
            blocks.append(SELECTORS[mode] + "{" + ";".join(lines) + "}")
    return "\n".join(blocks) + ("\n" if blocks else "")
