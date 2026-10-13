from __future__ import annotations

import json
import math
import re
import shutil
from collections import Counter
from pathlib import Path

QSS_PROPERTIES = frozenset({
    "background", "background-color", "border", "border-top", "border-bottom",
    "border-left", "border-right", "border-color", "border-width", "border-style",
    "border-radius", "color", "padding", "padding-top", "padding-bottom",
    "padding-left", "padding-right", "font-weight", "font-size", "min-height",
    "max-height", "min-width", "text-align",
})
BUTTON_SELECTOR = re.compile(
    r"^QPushButton(?:#(?P<name>[A-Za-z_]\w*))?(?P<attrs>(?:\[[^\]]+\])*)(?P<state>(?::[a-z-]+)*)$"
)
CSS_VARIABLE = re.compile(r"(--[a-z0-9-]+)\s*:\s*([^;]+);")
ICON_SUFFIXES = (".svg", ".png")
GLYPHS = ("add", "more_vert")
ICON_PREFIXES = ("/icons/", "/app", "/agent-ui")
MATERIAL_PATTERNS = (
    re.compile(r'FmIcon\(\s*"([a-z0-9_]+)"'),
    re.compile(r'\bicon\s*[:=]\s*"([a-z0-9_]+)"'),
    re.compile(r'\b(?:return|->)\s*"([a-z0-9_]+)"'),
    re.compile(r'HubItem\(\s*"[^"]*"\s*,\s*"[^"]*"\s*,\s*"([a-z0-9_]+)"'),
)
SF_PATTERNS = (
    re.compile(r'\b(?:systemName|systemImage|symbol)\s*:\s*"([a-z0-9.]+)"'),
)
SF_ROUTE_RETURN = re.compile(r'\breturn\s+"([a-z0-9.]+)"')
LUCIDE_PATTERNS = (
    re.compile(r'data-lucide="([a-z0-9-]+)"'),
    re.compile(r'"icon"\s*:\s*"([a-z0-9-]+)"'),
)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return ""


def _relative(repository: Path, path: Path) -> str:
    try:
        return path.relative_to(repository).as_posix()
    except ValueError:
        return path.as_posix()


def _css_block(source: str, selector: str) -> dict:
    start = source.find(selector)
    if start < 0:
        return {}
    open_brace = source.find("{", start)
    close_brace = source.find("}", open_brace)
    if open_brace < 0 or close_brace < 0:
        return {}
    return {name: " ".join(value.split()) for name, value in
            CSS_VARIABLE.findall(source[open_brace + 1:close_brace])}


def web_tokens(repository: Path) -> dict:
    styles = repository / "www" / "public" / "styles.css"
    source = _read(styles)
    dark = _css_block(source, ":root,\nhtml.dark {") or _css_block(source, ":root")
    light = _css_block(source, "html.light {")
    tokens = []
    for name, value in dark.items():
        tokens.append({"name": name, "dark": value, "light": light.get(name, value)})
    classes = Counter(re.findall(r"\.([a-z][a-z0-9-]*(?:button|btn)[a-z0-9-]*)\b", source))
    return {
        "source": _relative(repository, styles),
        "tokens": tokens,
        "buttonClasses": sorted(classes),
    }


def web_icons(repository: Path) -> dict:
    roots = [repository / "app" / "public", repository / "www" / "public"]
    roots += sorted((repository / "marketplace" / "packages").glob("*/content/web"))
    counts: Counter = Counter()
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in {".js", ".html"} or "vendor" in path.parts:
                continue
            text = _read(path)
            for name in LUCIDE_PATTERNS[0].findall(text):
                counts[name] += 1
    for manifest in sorted((repository / "marketplace" / "packages").glob("*/manifest.json")):
        for name in LUCIDE_PATTERNS[1].findall(_read(manifest)):
            counts[name] += 1
    return {
        "library": "lucide",
        "script": "/assets/vendor/lucide-0.468.0.min.js",
        "icons": [{"name": name, "uses": counts[name]} for name in sorted(counts)],
    }


def _raw_string(source: str, name: str) -> str:
    marker = "inline const char *%s = R\"(" % name
    start = source.find(marker)
    if start < 0:
        return ""
    start += len(marker)
    end = source.find(")\";", start)
    return source[start:end] if end > start else ""


def _qss_gradient(value: str) -> str:
    match = re.search(r"qlineargradient\(([^;]*)\)", value)
    if not match:
        return value
    body = match[1]
    points = dict(re.findall(r"\b(x1|y1|x2|y2)\s*:\s*([-\d.]+)", body))
    stops = re.findall(r"stop\s*:\s*([\d.]+)\s+(#[0-9a-fA-F]{3,8}|rgba?\([^)]*\))", body)
    if len(points) != 4 or not stops:
        return ""
    dx = float(points["x2"]) - float(points["x1"])
    dy = float(points["y2"]) - float(points["y1"])
    angle = round(math.degrees(math.atan2(dx, -dy))) % 360
    return "linear-gradient(%ddeg, %s)" % (angle, ", ".join(
        "%s %g%%" % (color, float(stop) * 100) for stop, color in stops))


def _qss_rules(sheet: str) -> dict:
    sheet = re.sub(r"/\*.*?\*/", "", sheet, flags=re.S)
    found: dict = {}
    for selectors, body in re.findall(r"([^{}]+)\{([^{}]*)\}", sheet):
        declarations = {}
        for item in body.split(";"):
            name, _, value = item.partition(":")
            name = name.strip().lower()
            value = " ".join(value.split())
            if "qlineargradient" in value:
                value = _qss_gradient(value)
            if name in QSS_PROPERTIES and value:
                declarations[name] = value
        if not declarations:
            continue
        for selector in selectors.split(","):
            match = BUTTON_SELECTOR.match(selector.strip())
            if not match:
                continue
            key = (match["name"] or "", match["attrs"] or "", match["state"] or "")
            found.setdefault(key, {}).update(declarations)
    return found


def desktop_buttons(repository: Path, theme: str, sources: str) -> list:
    dark = _qss_rules(_raw_string(theme, "kStyleSheet"))
    light = _qss_rules(_raw_string(theme, "kLightStyleSheet"))
    uses = Counter(re.findall(r'setObjectName\(\s*(?:QStringLiteral\(\s*|QLatin1String\(\s*)?"([A-Za-z_]\w*)"', sources))
    variants: dict = {}
    for key in sorted(set(dark) | set(light)):
        name, attrs, state = key
        variant = variants.setdefault((name, attrs), {
            "name": name, "attributes": attrs, "uses": uses.get(name, 0) if name else 0,
            "states": {},
        })
        variant["states"][state or "default"] = {
            "dark": dark.get(key, {}), "light": light.get(key, {}),
        }
    rows = list(variants.values())
    rows.sort(key=lambda row: (2 if row["name"] else 1 if row["attributes"] else 0,
                               row["name"].lower(), row["attributes"]))
    return rows


def desktop_palette(theme: str) -> dict:
    body_start = theme.find("inline QPalette applicationPalette")
    body = theme[body_start:theme.find("return palette;", body_start)] if body_start >= 0 else ""
    roles = []
    for disabled, role, dark, light in re.findall(
        r'palette\.setColor\((QPalette::Disabled,\s*)?QPalette::(\w+),\s*(?:\n\s*)?'
        r'color\(dark \? "(#[0-9a-fA-F]{6})" : "(#[0-9a-fA-F]{6})"\)\)', body):
        roles.append({"role": ("Disabled " if disabled else "") + role,
                      "dark": dark.lower(), "light": light.lower()})
    placeholder = re.search(
        r'placeholder = color\(dark \? "(#[0-9a-fA-F]{6})" : "(#[0-9a-fA-F]{6})"\)', body)
    if placeholder:
        roles.append({"role": "PlaceholderText", "dark": placeholder[1].lower(),
                      "light": placeholder[2].lower()})
    constants = [{"name": name, "value": value.lower()} for name, value in re.findall(
        r'inline const char \*(k\w+) = "(#[0-9a-fA-F]{6})";', theme)]
    senders = re.search(r"kSenderPalette\[\] = \{([^}]*)\}", theme)
    sender_colors = re.findall(r'"(#[0-9a-fA-F]{6})"', senders[1]) if senders else []
    base = _qss_rules(_raw_string(theme, "kStyleSheet"))
    widget = re.search(r"\nQWidget \{([^}]*)\}", _raw_string(theme, "kStyleSheet"))
    font_size = re.search(r"font-size:\s*(\d+)px", widget[1]) if widget else None
    return {
        "roles": roles,
        "constants": constants,
        "senders": [color.lower() for color in sender_colors],
        "baseFontSize": int(font_size[1]) if font_size else None,
        "buttonRuleCount": len(base),
    }


def desktop_fonts(repository: Path) -> list:
    source = _read(repository / "desktop" / "src" / "main.cpp")
    start = source.find("for (const QString &preferred :")
    if start < 0:
        return []
    block = source[start:source.find("})", start)]
    return re.findall(r'QStringLiteral\("([^"]+)"\)', block)


def desktop_icons(repository: Path, destination: Path) -> list:
    resources = repository / "desktop" / "resources"
    qrc = _read(resources / "icons.qrc")
    icons = []
    seen = set()
    for prefix, block in re.findall(r'<qresource prefix="([^"]+)">(.*?)</qresource>', qrc, re.S):
        if not prefix.startswith(ICON_PREFIXES):
            continue
        for alias, file_name in re.findall(r'<file alias="([^"]+)">([^<]+)</file>', block):
            if not alias.endswith(ICON_SUFFIXES):
                continue
            source = (resources / file_name).resolve()
            if not source.is_file():
                continue
            family = prefix.strip("/").split("/")[-1]
            stem = alias.rsplit(".", 1)[0]
            key = (family, stem)
            if key in seen:
                continue
            seen.add(key)
            target = Path("icons") / "desktop" / family / alias
            output = destination / target
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, output)
            icons.append({
                "name": stem,
                "family": family,
                "resource": ":%s/%s" % (prefix.rstrip("/"), alias),
                "file": target.as_posix(),
                "format": alias.rsplit(".", 1)[1],
                "source": _relative(repository, source),
            })
    return icons


def _swift_scalars(source: str, enum: str) -> list:
    match = re.search(r"enum %s \{(.*?)\n\}" % enum, source, re.S)
    if not match:
        return []
    return [{"name": name, "value": float(value)} for name, value in re.findall(
        r"static let (\w+): (?:CGFloat|Double) = ([\d.]+)", match[1])]


def _kotlin_scalars(source: str, name: str) -> list:
    match = re.search(r"object %s \{(.*?)\n\}" % name, source, re.S)
    if not match:
        return []
    return [{"name": key, "value": float(value)} for key, value in re.findall(
        r"(?:const )?val (\w+) = ([\d.]+)(?:\.dp|\.sp|f)?\s*$", match[1], re.M)]


def _argb(value: str) -> str:
    value = value.upper()
    alpha = int(value[:2], 16)
    rgb = "#" + value[2:].lower()
    if alpha == 255:
        return rgb
    return "rgba(%d, %d, %d, %.2f)" % (int(value[2:4], 16), int(value[4:6], 16),
                                        int(value[6:8], 16), alpha / 255)


def _colors(source: str, pattern: str) -> list:
    rows = []
    for name, value in re.findall(pattern, source):
        rows.append({"name": name, "value": _argb(value), "argb": "0x" + value.upper()})
    return rows


def _material_table(repository: Path) -> set:
    table = repository / "native" / "android" / "app" / "src" / "main" / "assets" / "fonts" / "MaterialIcons.codepoints"
    names = {}
    for line in _read(table).splitlines():
        parts = line.split()
        if len(parts) == 2:
            names[parts[0]] = parts[1]
    return names


def _material_code(table: dict, name: str) -> str:
    return table.get(name) or table.get(name + "_baseline") or ""


def _material_icons(files, table: dict) -> list:
    counts: Counter = Counter()
    for path in files:
        text = _read(path)
        for pattern in MATERIAL_PATTERNS:
            for name in pattern.findall(text):
                if _material_code(table, name):
                    counts[name] += 1
    return [{"name": name, "codepoint": _material_code(table, name), "uses": counts[name]}
            for name in sorted(counts)]


def ios_spec(repository: Path, table: dict) -> dict:
    root = repository / "native" / "ios"
    theme = root / "ForkMesh" / "Theme"
    tokens = _read(theme / "FmTokens.swift")
    colors = _read(theme / "FmColors.swift")
    text_style = _read(theme / "FmTextStyle.swift")
    roles = []
    for role, size, weight, height, tracking, tone in re.findall(
        r"case \.(\w+):\s*return FmTextSpec\(size: FmType\.(\w+), weight: FmType\.(\w+), "
        r"height: ([\w.]+), tracking: ([-\d.]+), tone: \.(\w+)\)", text_style):
        roles.append({"role": role, "size": size, "weight": weight,
                      "height": height.replace("FmType.", ""), "tracking": float(tracking),
                      "tone": tone})
    accents = []
    accent_block = re.search(r"enum FmAccent\b(.*?)\n\}", colors, re.S)
    if accent_block:
        shades = {}
        for shade, body in re.findall(r"var (\w+): Color \{(.*?)\n    \}", accent_block[1], re.S):
            shades[shade] = dict(re.findall(
                r"case \.(\w+): return Color\(fmARGB: 0x([0-9A-Fa-f]{8})\)", body))
        for name, value in shades.get("light", {}).items():
            accents.append({"name": name, "light": _argb(value),
                            "dark": _argb(shades.get("onDark", {}).get(name, value)),
                            "subtle": _argb(shades.get("subtle", {}).get(name, value))})
    swift_files = sorted(path for path in root.rglob("*.swift")
                         if "Tests" not in path.parts)
    buttons = []
    for path in swift_files:
        for number, line in enumerate(_read(path).splitlines(), 1):
            match = re.match(r"struct (Fm\w*Button)\b", line)
            if match:
                buttons.append({"name": match[1], "source": "%s:%d" % (_relative(repository, path), number)})
    sf_counts: Counter = Counter()
    for path in swift_files:
        text = _read(path)
        for pattern in SF_PATTERNS:
            sf_counts.update(pattern.findall(text))
        if path.name == "RouteCatalog.swift":
            sf_counts.update(name for name in SF_ROUTE_RETURN.findall(text)
                             if "." in name or not _material_code(table, name))
    return {
        "source": _relative(repository, theme),
        "space": _swift_scalars(tokens, "FmSpace"),
        "type": _swift_scalars(tokens, "FmType"),
        "radius": _swift_scalars(tokens, "FmRadius"),
        "motion": _swift_scalars(tokens, "FmMotion"),
        "colors": _colors(colors.split("enum FmAccent")[0],
                          r"static let (\w+) = Color\(fmARGB: 0x([0-9A-Fa-f]{8})\)"),
        "accents": accents,
        "textRoles": roles,
        "buttons": buttons,
        "materialIcons": _material_icons(swift_files, table),
        "materialGlyphs": {name: _material_code(table, name) for name in GLYPHS},
        "sfSymbols": [{"name": name, "uses": sf_counts[name]} for name in sorted(sf_counts)],
    }


def android_spec(repository: Path, table: dict) -> dict:
    root = repository / "native" / "android"
    theme_file = root / "app" / "src" / "main" / "kotlin" / "com" / "forkmesh" / "nativeapp" / "ui" / "theme" / "ForkMeshTheme.kt"
    theme = _read(theme_file)
    kotlin_files = sorted(path for path in root.rglob("*.kt")
                          if "test" not in path.parts and "androidTest" not in path.parts)
    buttons = []
    for path in kotlin_files:
        for number, line in enumerate(_read(path).splitlines(), 1):
            match = re.match(r"(?:internal |public )?fun (Fm\w*Button)\(", line)
            if match:
                buttons.append({"name": match[1], "source": "%s:%d" % (_relative(repository, path), number)})
    radius = {row["name"]: row["value"] for row in _kotlin_scalars(theme, "FmRadius")}
    shapes = re.search(r"val FmShapes = Shapes\((.*?)\n\)", theme, re.S)
    shape_rows = []
    for name, token, value in re.findall(
            r"(\w+) = RoundedCornerShape\((?:FmRadius\.(\w+)|([\d.]+)\.dp)\)",
            shapes[1] if shapes else ""):
        size = radius.get(token) if token else float(value)
        if size is not None:
            shape_rows.append({"name": name, "value": size})
    return {
        "source": _relative(repository, theme_file),
        "space": _kotlin_scalars(theme, "FmSpace"),
        "type": _kotlin_scalars(theme, "FmType"),
        "radius": _kotlin_scalars(theme, "FmRadius"),
        "motion": _kotlin_scalars(theme, "FmMotion"),
        "shapes": shape_rows,
        "colors": _colors(re.search(r"object FmColors \{(.*?)\n\}", theme, re.S)[1]
                          if re.search(r"object FmColors \{", theme) else "",
                          r"val (\w+) = Color\(0x([0-9A-Fa-f]{8})\)"),
        "buttons": buttons,
        "materialIcons": _material_icons(kotlin_files, table),
        "materialGlyphs": {name: _material_code(table, name) for name in GLYPHS},
    }


def material_font(repository: Path, destination: Path) -> str:
    candidates = (
        repository / "native" / "android" / "app" / "src" / "main" / "res" / "font" / "material_icons_regular.otf",
        repository / "native" / "ios" / "ForkMesh" / "Resources" / "Fonts" / "material_icons_regular.otf",
    )
    for source in candidates:
        if source.is_file():
            target = Path("fonts") / "material-icons-regular.otf"
            (destination / target).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination / target)
            return target.as_posix()
    return ""


def inventory(repository: Path, destination: Path) -> dict:
    repository = Path(repository).resolve()
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    theme_path = repository / "desktop" / "src" / "Theme.h"
    theme = _read(theme_path)
    sources = "\n".join(_read(path) for path in sorted(
        list((repository / "desktop" / "src").glob("*.cpp")) +
        list((repository / "marketplace" / "packages").glob("*/content/desktop/*.cpp"))))
    table = _material_table(repository)
    desktop = desktop_palette(theme)
    desktop.update({
        "source": _relative(repository, theme_path),
        "fonts": desktop_fonts(repository),
        "buttons": desktop_buttons(repository, theme, sources),
        "icons": desktop_icons(repository, destination),
    })
    return {
        "schemaVersion": 1,
        "web": {**web_tokens(repository), **web_icons(repository)},
        "desktop": desktop,
        "ios": ios_spec(repository, table),
        "android": android_spec(repository, table),
        "materialFont": material_font(repository, destination),
    }


def build(repository: Path, destination: Path) -> Path:
    data = inventory(repository, destination)
    target = Path(destination) / "inventory.json"
    target.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    return target
