"""The instance's Top repositories setting (instance_settings row "sidebar").

{"pinned": ["<repo>" | "<owner>/<repo>", ...], "forks": N, "devto": "<user>"}:
a non-empty pinned list shows only those repositories, in that order, forks is
how many forks each one shows before its toggle, and devto names the dev.to
account whose newest posts fill the Home page's Latest from the blog card
("" keeps the instance's own blog feed). `parse` reads a stored document leniently;
`clean` validates an admin's write strictly and says what is wrong.
"""

import re

SETTING_KEY = "sidebar"
PINNED_MAX = 10
FORKS_MAX = 50
FORKS_DEFAULT = 3

_NAME = r"[a-z0-9](?:[a-z0-9._-]{0,98}[a-z0-9_])?"
_OWNER = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
PINNED_RE = re.compile(r"(?:" + _OWNER + r"/)?" + _NAME)
DEVTO_RE = re.compile(r"[a-z0-9_]{1,30}")


def parse(doc):
    doc = doc if isinstance(doc, dict) else {}
    pinned = []
    values = doc.get("pinned")
    for value in values if isinstance(values, list) else []:
        text = str(value or "").strip().lower()
        if 0 < len(text) <= 161 and text.count("/") <= 1 \
                and text not in pinned:
            pinned.append(text)
        if len(pinned) >= PINNED_MAX:
            break
    try:
        forks = int(doc.get("forks", FORKS_DEFAULT))
    except (TypeError, ValueError):
        forks = FORKS_DEFAULT
    devto = str(doc.get("devto") or "").strip().lower()
    return {"pinned": pinned, "forks": max(0, min(FORKS_MAX, forks)),
            "devto": devto if DEVTO_RE.fullmatch(devto) else ""}


def clean(value):
    """Return (setting, "") for a valid write, or (None, error code)."""
    if not isinstance(value, dict):
        return None, "invalid_sidebar"
    raw = value.get("pinned", [])
    if not isinstance(raw, list):
        return None, "invalid_pinned"
    pinned = []
    for item in raw:
        if not isinstance(item, str):
            return None, "invalid_pinned"
        text = item.strip().lower()
        if not text:
            continue
        if len(text) > 161 or not PINNED_RE.fullmatch(text):
            return None, "invalid_pinned"
        if text not in pinned:
            pinned.append(text)
    if len(pinned) > PINNED_MAX:
        return None, "too_many_pinned"
    forks = value.get("forks", FORKS_DEFAULT)
    if isinstance(forks, bool) or not isinstance(forks, int) \
            or not 0 <= forks <= FORKS_MAX:
        return None, "invalid_forks"
    devto = value.get("devto", "")
    if not isinstance(devto, str):
        return None, "invalid_devto"
    devto = devto.strip().lstrip("@").lower()
    if devto and not DEVTO_RE.fullmatch(devto):
        return None, "invalid_devto"
    return {"pinned": pinned, "forks": forks, "devto": devto}, ""
