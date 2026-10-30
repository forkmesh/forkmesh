import re

GLYPHS = {
    "actions": (
        '<circle cx="12" cy="12" r="3"></circle>'
        '<path d="M12 2.5v3M12 18.5v3M2.5 12h3M18.5 12h3M5.3 5.3l2.1 2.1M16.6 16.6l2.1 2.1'
        'M5.3 18.7l2.1-2.1M16.6 7.4l2.1-2.1"></path>'
        '<circle cx="12" cy="12" r="6.5"></circle>'),
    "node": (
        '<circle cx="6" cy="6" r="2.25"></circle><circle cx="6" cy="18" r="2.25"></circle>'
        '<circle cx="18" cy="8" r="2.25"></circle>'
        '<path d="M6 8.25v7.5M18 10.25c0 4-5.5 3.5-10.2 6.4"></path>'),
    "runner": (
        '<rect x="3.5" y="4" width="17" height="6.5" rx="1.5"></rect>'
        '<rect x="3.5" y="13.5" width="17" height="6.5" rx="1.5"></rect>'
        '<path d="M7.5 7.25h.01M7.5 16.75h.01M11.5 7.25h5M11.5 16.75h5"></path>'),
    "ai": (
        '<path d="M11 3.5l1.9 4.6 4.6 1.9-4.6 1.9L11 16.5l-1.9-4.6L4.5 10l4.6-1.9z"></path>'
        '<path d="M18 14.5l.9 2.1 2.1.9-2.1.9-.9 2.1-.9-2.1-2.1-.9 2.1-.9z"></path>'),
    "bot": (
        '<rect x="4.5" y="8" width="15" height="11" rx="2.5"></rect>'
        '<path d="M12 8V4.5M9.5 4.5h5M2.5 12.5v3M21.5 12.5v3"></path>'
        '<circle cx="9.5" cy="13" r="1"></circle><circle cx="14.5" cy="13" r="1"></circle>'
        '<path d="M9.5 16.25h5"></path>'),
}

KINDS = tuple(GLYPHS)

AI_RE = re.compile(r"^(?:ai[\s_-]?review(?:er)?|forkbot|ai[\s_-]?agent)(?:\[bot\])?$", re.I)
RUNNER_RE = re.compile(r"(?:^|[\s_-])runner(?:\[bot\])?$", re.I)
NODE_RE = re.compile(r"^forkmesh(?:[\s_-]mirror(?:[\s_-]reconciler)?)?$", re.I)
ACTIONS_RE = (
    re.compile(r"^github[\s_-]?actions?(?:\[bot\])?$", re.I),
    re.compile(r"^[a-z0-9][a-z0-9_-]*-actions(?:\[bot\])?$", re.I),
    re.compile(r"^forkmesh[\s_-](?:actions|deploy)$", re.I),
)
ACTIONS_EMAILS = ("actions@forkmesh.local", "deploy@forkmesh.local")
BOT_RE = (
    re.compile(r"\[bot\]$", re.I),
    re.compile(r"^(?:dependabot|renovate)$", re.I),
    re.compile(r"(?:^|[\s_-])bot$", re.I),
)
FILL = "#6e7681"
STROKE = "#ffffff"


def bot_avatar_kind(name, email=""):
    text = str(name or "").strip()
    mail = str(email or "").strip().lower()
    if not text and not mail:
        return ""
    if AI_RE.match(text):
        return "ai"
    if RUNNER_RE.search(text):
        return "runner"
    if NODE_RE.match(text):
        return "node"
    if any(pattern.search(text) for pattern in ACTIONS_RE) or mail in ACTIONS_EMAILS:
        return "actions"
    if any(pattern.search(text) for pattern in BOT_RE) or "[bot]@" in mail:
        return "bot"
    return ""


def bot_avatar_svg(kind):
    glyph = GLYPHS.get(kind) or GLYPHS["bot"]
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="128" height="128" viewBox="0 0 24 24">'
        '<rect width="24" height="24" rx="12" fill="%s"></rect>'
        '<g transform="translate(5 5) scale(0.5833)" fill="none" stroke="%s" stroke-width="2" '
        'stroke-linecap="round" stroke-linejoin="round">%s</g></svg>' % (FILL, STROKE, glyph))
