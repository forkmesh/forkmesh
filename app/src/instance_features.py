import html
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
      "/api/accounts/central-fund",
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
    ("instance-launcher", "Instance launcher",
     "The start page that sets up new instances on other servers.",
     ("/start", "/api/start"), ()),
    ("website", "ForkMesh website pages",
     "forkmesh.com's own pages: about, careers, press, pricing, features, "
     "the ecosystem, leaderboards, the blog and the changelog.",
     ("/about", "/careers", "/press", "/pricing", "/features", "/ecosystem",
      "/leaderboards", "/blog", "/changelog", "/market"), ()),
    ("issue-ai-prompt", "Issue AI prompt",
     "The copyable \"Fix <issue>: read the issue, make the change\u2026\" "
     "prompt under an open issue's title.",
     (), ()),
    ("fediverse-ui", "Fediverse panels",
     "The Fediverse thread under issues, pull requests and discussions, and "
     "the Watch on the fediverse menu of a repository.",
     (), (r"^/api/repo/[^/]+/[^/]+/fedi-comments$",)),
    ("read-surfaces", "Feeds and lite view",
     "Atom feeds of a repository's issues, threads, labels and activity, a "
     "no-JavaScript lite view of every thread, and the forum home page.",
     (), ()),
    ("forum-list", "Forum-style issue lists",
     "Issue lists open on Newest first, as on GitHub, and each row shows the "
     "thread's last few commenters.",
     (), ()),
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
    "instance-launcher": "forkmesh.com's page that installs new instances.",
}
INSTANCE_NOTES = {
    "world": "Your own 3D World at /world, with the Office and its chat. "
             "With a World listing it is joined to the ForkMesh World by a "
             "portal.",
    "guestbook": "Lets agents sign a guestbook in this instance's chat.",
    "genie": "The spider at the bottom of each page that offers tips.",
    "developer-docs": "Shows the docs/ folder of this instance's read-only "
                      "forkmesh mirror in the dashboard.",
    "read-surfaces": "Serves /<owner>/<repo>/issues.atom, /lite and the home "
                     "page for readers on slow or filtered links and feed readers.",
    "forum-list": "Lists a repository's issues newest first and shows who "
                  "commented last, like a forum.",
    "issue-ai-prompt": "Offers readers a prompt to paste into a coding agent "
                       "on each open issue.",
    "fediverse-ui": "Shows replies from the fediverse beside a thread and "
                    "how to follow a repository from Mastodon. Off, the "
                    "replies endpoint answers 404 too.",
}
DEFAULT_ON = frozenset({"issue-ai-prompt", "fediverse-ui"})
OPT_IN = frozenset({"forum-list", "read-surfaces"})
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


PLATFORM = "ForkMesh"
_RENAME_RE = re.compile(r"(?<![A-Za-z0-9_.-])ForkMesh(?![A-Za-z0-9_-])(?! [Dd]esktop)(?!, Inc)")
_REMEMBERED = {"name": "", "origin": "", "env": None}


def remember_instance(env):
    if not branded(env):
        return
    _REMEMBERED["env"] = env
    origin = (_setting(env, "PUBLIC_BASE_URL") or _setting(env, "APP_ORIGIN")).rstrip("/")
    _REMEMBERED["name"] = display_name(env)
    _REMEMBERED["origin"] = origin if origin.startswith("https://") else ""


def user_agent(default, product):
    name = _REMEMBERED["name"]
    if not name:
        return default
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "git"
    origin = _REMEMBERED["origin"]
    return "%s-%s/1.0%s" % (slug, product, " (+%s)" % origin if origin else "")


def powered_by(env):
    return _setting(env, "INSTANCE_POWERED_BY") != "0"


def display_name(env):
    return " ".join(_setting(env, "INSTANCE_NAME").split())[:60] or PLATFORM


def rename(env, text, name=None):
    if not text or not branded(env):
        return text
    replacement = display_name(env) if name is None else name
    return _RENAME_RE.sub(lambda _: replacement, str(text))


def rename_html(env, text):
    return rename(env, text, html.escape(display_name(env), quote=True))


def installed_ids(env):
    raw = _setting(env, "INSTANCE_PLUGINS").lower()
    if raw == "all":
        return set(IDS)
    chosen = {item.strip() for item in raw.split(",")}
    refused = {item[1:] for item in chosen if item.startswith("-")}
    return {item for item in chosen if _ID_RE.fullmatch(item) and item in IDS} \
        | (DEFAULT_ON - refused)


def core_ids(env):
    raw = _setting(env, "FORKMESH_CORE_PLUGINS").lower()
    chosen = {item.strip() for item in raw.split(",")}
    return (set(IDS) - OPT_IN) | {item for item in chosen if item in OPT_IN}


def enabled_ids(env):
    if not branded(env):
        return core_ids(env)
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
    refused = data.get("disabled")
    refused = {item for item in refused if isinstance(item, str)} \
        if isinstance(refused, list) else set()
    return {item for item in data["enabled"]
            if isinstance(item, str) and item in IDS} | (DEFAULT_ON - refused)


def dump(ids):
    chosen = set(ids) & set(IDS)
    doc = {"enabled": sorted(chosen)}
    if DEFAULT_ON - chosen:
        doc["disabled"] = sorted(DEFAULT_ON - chosen)
    return json.dumps(doc, separators=(",", ":"))


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
    best, length = "", -1
    for feature in FEATURES:
        for prefix in feature[3]:
            if _matches(path, prefix) and len(prefix) > length:
                best, length = feature[0], len(prefix)
    if best:
        return best
    for feature in FEATURES:
        if any(pattern.match(path) for pattern in _PATTERNS[feature[0]]):
            return feature[0]
    return ""


def blocked(env, path):
    if not branded(env):
        return ""
    feature = feature_for(path)
    return feature if feature and feature not in enabled_ids(env) else ""


def switched_off(env=None):
    env = _REMEMBERED["env"] if env is None else env
    if env is None:
        return frozenset(OPT_IN)
    if not branded(env):
        return frozenset(set(IDS) - core_ids(env))
    return frozenset(set(IDS) - enabled_ids(env))


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


ERROR_TEXT_FIELDS = ("message", "hint", "detail", "title", "reason")


def rename_error_body(env, body):
    if not branded(env) or not isinstance(body, (bytes, str)):
        return body
    raw = body.decode("utf-8", "replace") if isinstance(body, bytes) else body
    if "ForkMesh" not in raw:
        return body
    try:
        payload = json.loads(raw)
    except ValueError:
        return body
    if not isinstance(payload, dict):
        return body
    changed = False
    for key in ERROR_TEXT_FIELDS:
        value = payload.get(key)
        if isinstance(value, str):
            renamed = rename(env, value)
            if renamed != value:
                payload[key] = renamed
                changed = True
    if not changed:
        return body
    text = json.dumps(payload, separators=(",", ":"))
    return text.encode("utf-8") if isinstance(body, bytes) else text


CHROME = ":is(header, nav, aside, footer)"
PLUGIN_ELEMENTS = {
    "genie": ".fm-genie, .fm-genie-bubble, .fm-genie-fx, genie-spider",
    "issue-ai-prompt": "[data-repo-issue-ai-prompt]",
    "fediverse-ui": "[data-repo-federated-thread], [data-repo-watch-wrap]",
}
OFFSITE_PAGES = {"/world": "https://world.forkmesh.com"}


def _css_string(value):
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


OFFSITE_CHROME = (
    'a[href^="https://forkmesh.com/"]:not([href="https://forkmesh.com/"])',
    'a[href^="https://www.forkmesh.com/"]', 'a[href^="https://app.forkmesh.com/"]',
    'a[href^="https://www.patreon.com/"]', "a[data-forkmesh-membership]")


def chrome_css(env):
    on = enabled_ids(env)
    rules = [":root{--instance-plugins:ready}"]
    if branded(env) and _setting(env, "INSTANCE_WHITE_LABEL") == "1":
        rules.append("%s{display:none !important}" % ",".join(
            "%s %s:not([data-instance-powered-by] a)" % (CHROME, item)
            for item in OFFSITE_CHROME))
    for feature in FEATURES:
        if feature[0] in on or not branded(env):
            continue
        selectors = ['[data-instance-plugin~=%s]' % _css_string(feature[0])]
        items = []
        if feature[0] in PLUGIN_ELEMENTS:
            selectors.append(PLUGIN_ELEMENTS[feature[0]])
        for path in feature[3]:
            if path.startswith("/api/") or not path.startswith("/"):
                continue
            exact, below = _css_string(path), _css_string(path.rstrip("/") + "/")
            items.append("%s li:has(> a[href=%s])" % (CHROME, exact))
            selectors.append("%s a[href=%s]" % (CHROME, exact))
            selectors.append("%s a[href^=%s]" % (CHROME, below))
            if path in OFFSITE_PAGES:
                selectors.append("%s a[href^=%s]" % (CHROME, _css_string(OFFSITE_PAGES[path])))
        rules.append("%s{display:none !important}" % ",".join(selectors))
        if items:
            rules.append("%s{display:none !important}" % ",".join(items))
    return "\n".join(rules) + "\n"
