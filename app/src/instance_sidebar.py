"""The instance's Top repositories setting (instance_settings row "sidebar").

{"pinned": ["<repo>" | "<owner>/<repo>", ...], "forks": N}: a non-empty pinned
list shows only those repositories, in that order, and forks is how many forks
each one shows before its toggle. `parse` reads a stored document leniently;
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
    return {"pinned": pinned, "forks": max(0, min(FORKS_MAX, forks))}


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
    return {"pinned": pinned, "forks": forks}, ""
