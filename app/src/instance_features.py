import json
import re

FEATURES = (
    ("world", "World and Office",
     "The 3D World, its plots, the Office and their chat.",
     ("/world", "/api/world", "/office", "/api/office"), ()),
    ("hackathon", "Hackathon",
     "ForkMesh hackathon pages, squads and challenges.",
     ("/hackathon", "/api/hackathon", "/admin/hackathon",
      "/api/admin/hackathon"), ()),
    ("challenges", "Challenges",
     "ForkMesh community challenges.",
     ("/api/challenges", "/dashboard/challenges"), ()),
    ("pitch-deck", "Pitch deck",
     "The ForkMesh pitch deck viewer.",
     ("/pitch-deck", "/api/pitch-deck"), ()),
    ("ads", "Community ads",
     "ForkMesh's ad network and World billboards.",
     ("/api/ads", "/dashboard/ads", "/api/world/community-ads"), ()),
    ("patreon", "Patreon",
     "Patreon membership sync.",
     ("/api/integrations/patreon",), ()),
    ("discord", "Discord",
     "Discord sign-in, bug intake and organization channels.",
     ("/api/integrations/discord",),
     (r"^/api/orgs/[^/]+/discord(?:/|$)",)),
    ("microjobs", "Microjobs",
     "Paid microjobs between ForkMesh members.",
     ("/microjobs", "/api/microjobs"), ()),
    ("rewards", "Rewards and ledger",
     "Mirror rewards, the Solana ledger, wallets and the World store.",
     ("/ledger", "/api/ledger", "/api/rewards", "/api/relay-mesh",
      "/api/leaderboards/wallets", "/api/world/wallet", "/api/world/store",
      "/mirror-payouts"), ()),
    ("referrals", "Referrals and bounties",
     "Referral links, referral rewards and repository bounties.",
     ("/referral", "/referrals", "/api/referrals", "/dashboard/links",
      "/dashboard/bounties"), ()),
    ("forkbot", "ForkBot",
     "ForkMesh's own chat bot.",
     ("/api/forkbot", "/api/genie"), ()),
    ("guestbook", "Agent guestbook",
     "The agent guestbook in ForkMesh chat.",
     ("/api/chat/agent-guestbook",), ()),
    ("ecosystem", "Ecosystem directory",
     "ForkMesh's ecosystem and partner directory.",
     ("/api/ecosystem",), ()),
    ("blog", "Latest from the blog",
     "The forkmesh.com blog feed and the dashboard's Latest from the blog "
     "card.",
     ("/api/blog", "/rss.xml", "/feed.xml", "/blog/rss.xml",
      "/blog/feed.xml"), ()),
    ("genie", "Genie spider",
     "The Genie spider that walks along the bottom of every page and offers "
     "tips.",
     (), ()),
    ("status", "Status page",
     "The public status page and its uptime history.",
     ("/status", "/api/status"), ()),
    ("developer-docs", "Developer docs",
     "The dashboard's Developer docs page: every Markdown file under docs/ "
     "in forkmesh/forkmesh, editable by admins.",
     ("/dashboard/docs", "/api/docs-book"), ()),
    ("ios-testers", "iOS test builds",
     "ForkMesh's iOS tester enrollment.",
     ("/api/ios-udid",), ()),
    ("website", "ForkMesh website pages",
     "forkmesh.com's own pages: about, careers, press, pricing, features, "
     "the ecosystem, leaderboards, the blog and the changelog.",
     ("/about", "/careers", "/press", "/pricing", "/features", "/ecosystem",
      "/leaderboards", "/blog", "/changelog", "/market"), ()),
)
OFFSITE_HOSTS = ("forkmesh.com", "www.forkmesh.com", "app.forkmesh.com",
                 "world.forkmesh.com", "www.patreon.com")
FORKMESH_ONLY = {
    "hackathon": "Runs ForkMesh's own hackathon, its squads and prizes.",
    "challenges": "ForkMesh's community challenges and their rewards.",
    "pitch-deck": "ForkMesh's own pitch deck.",
    "ads": "Sells and shows ads through ForkMesh's ad network.",
    "patreon": "Syncs memberships of ForkMesh's Patreon page.",
    "discord": "Uses ForkMesh's Discord app; its package is not built on "
               "instances.",
    "microjobs": "Pays microjobs through the ForkMesh ledger.",
    "rewards": "Mirror rewards and wallets on ForkMesh's Solana ledger.",
    "referrals": "Referral rewards and bounties paid by ForkMesh.",
    "forkbot": "ForkMesh's chat bot and its model keys.",
    "ecosystem": "forkmesh.com's curated ecosystem directory.",
    "blog": "Reads the forkmesh.com blog.",
    "status": "Monitors forkmesh.com's own cores, mirrors and app releases.",
    "ios-testers": "Enrolls testers in ForkMesh's iOS builds.",
    "website": "forkmesh.com's own marketing pages.",
}
INSTANCE_NOTES = {
    "world": "Your own 3D World at /world, with the Office and its chat. "
             "With a World listing it is joined to the ForkMesh World by a "
             "portal.",
    "guestbook": "Lets agents sign a guestbook in this instance's chat.",
    "genie": "The spider at the bottom of each page that offers tips.",
    "developer-docs": "Shows the docs/ folder of this instance's read-only "
                      "forkmesh mirror in the dashboard.",
}
SETTING_KEY = "plugins"
CACHE_MS = 30 * 1000
IDS = tuple(feature[0] for feature in FEATURES)
CHOOSABLE = tuple(item for item in IDS if item not in FORKMESH_ONLY)
_PATTERNS = {
    feature[0]: tuple(re.compile(pattern) for pattern in feature[4])
    for feature in FEATURES
}
_ID_RE = re.compile(r"^[a-z][a-z0-9-]{0,40}$")
_cache = {"at": -CACHE_MS, "value": None, "loaded": False}


def _setting(env, name):
    return str(getattr(env, name, "") or "").strip()


def branded(env):
    return bool(" ".join(_setting(env, "INSTANCE_NAME").split()))


def installed_ids(env):
    raw = _setting(env, "INSTANCE_PLUGINS").lower()
    if raw == "all":
        return set(IDS)
    chosen = {item.strip() for item in raw.split(",")}
    return {item for item in chosen if _ID_RE.fullmatch(item) and item in IDS}


def enabled_ids(env):
    if not branded(env):
        return set(IDS)
    if _cache["loaded"] and _cache["value"] is not None:
        return set(_cache["value"])
    return installed_ids(env)


def parse(doc):
    try:
        data = json.loads(doc) if doc else None
    except (TypeError, ValueError):
        return None
    if not isinstance(data, dict) or not isinstance(data.get("enabled"), list):
        return None
    return {item for item in data["enabled"]
            if isinstance(item, str) and item in IDS}


def dump(ids):
    return json.dumps({"enabled": sorted(set(ids) & set(IDS))},
                      separators=(",", ":"))


def choose(current, chosen):
    kept = {item for item in current if item in FORKMESH_ONLY}
    picked = {item for item in chosen if item in CHOOSABLE}
    return kept | picked


def fresh(now_ms):
    return _cache["loaded"] and now_ms - _cache["at"] < CACHE_MS


def current():
    return _cache["value"]


def remember(value, now_ms):
    _cache["value"] = set(value) if value is not None else None
    _cache["at"] = now_ms
    _cache["loaded"] = True
    return value


def forget():
    _cache["value"] = None
    _cache["at"] = -CACHE_MS
    _cache["loaded"] = False


def _matches(path, prefix):
    return path == prefix or path.startswith(prefix.rstrip("/") + "/")


def feature_for(path):
    path = str(path or "")
    for feature in FEATURES:
        if any(_matches(path, prefix) for prefix in feature[3]):
            return feature[0]
        if any(pattern.match(path) for pattern in _PATTERNS[feature[0]]):
            return feature[0]
    return ""


def blocked(env, path):
    if not branded(env):
        return ""
    feature = feature_for(path)
    return feature if feature and feature not in enabled_ids(env) else ""


def describe(env):
    on = enabled_ids(env)
    return [{
        "id": feature[0],
        "title": feature[1],
        "description": feature[2],
        "enabled": feature[0] in on,
        "scope": "forkmesh" if feature[0] in FORKMESH_ONLY else "instance",
        "note": FORKMESH_ONLY.get(feature[0]) or INSTANCE_NOTES.get(
            feature[0], ""),
        "paths": list(feature[3]),
    } for feature in FEATURES]
