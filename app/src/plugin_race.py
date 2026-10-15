import calendar
import re
from urllib.parse import quote, urlsplit


PREFIX = "/api/plugin-race"
ENTRANT_PATH = PREFIX + "/entrant"
PLUGINS_PATH = PREFIX + "/plugins"
ADMIN_PATH = "/api/admin/plugin-race"
BODY_MAX_BYTES = 16 * 1024
MAX_FETCH_BYTES = 256 * 1024


def _utc_ms(year, month, day, hour=0, minute=0, second=0):
    return calendar.timegm((year, month, day, hour, minute, second)) * 1000


NAME = "ForkMesh Plugin Race"
HASHTAG = "#ForkMeshPluginRace"
STARTS_AT = _utc_ms(2026, 10, 19)
ENDS_AT = _utc_ms(2026, 12, 18, 23, 59, 59)
MARKET = "forkmesh-community"
PRIZE_LAMPORTS = 100_000_000
PRIZE_SOL = "0.1"
PRIZE_SLOTS = 10
RATINGS_NEEDED = 10
MIN_STARS = 3
MAX_PLUGINS_PER_ENTRANT = 3
NETWORKS = ("x", "reddit", "mastodon")
PLATFORMS = ("desktop", "ios", "web")
POST_STATES = ("confirmed", "pending", "missing", "rejected")
PLUGIN_STATUSES = ("review", "accepted", "rejected")

X_HOSTS = frozenset({"x.com", "www.x.com", "mobile.x.com", "twitter.com",
                     "www.twitter.com", "mobile.twitter.com"})
X_PATH_RE = re.compile(r"^/([A-Za-z0-9_]{1,15})/status/(\d{5,25})/?$")
REDDIT_PATH_RE = re.compile(
    r"^/(r|u|user)/([A-Za-z0-9_-]{2,30})/comments/([a-z0-9]{3,12})(?:/[^?#]*)?$")
MASTODON_PATH_RE = re.compile(r"^/@([A-Za-z0-9_]{1,30})/(\d{6,25})/?$")
HOST_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$")
DISCORD_RE = re.compile(r"^[a-z0-9_.]{2,32}$")
PACKAGE_RE = re.compile(r"^[a-z0-9][a-z0-9.-]{0,79}$")
USERNAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,63}$")
SIGNATURE_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{64,90}$")


def rules():
    return {
        "name": NAME,
        "hashtag": HASHTAG,
        "startsAt": STARTS_AT,
        "endsAt": ENDS_AT,
        "market": MARKET,
        "prizeSol": PRIZE_SOL,
        "prizeLamports": PRIZE_LAMPORTS,
        "prizeSlots": PRIZE_SLOTS,
        "ratingsNeeded": RATINGS_NEEDED,
        "minStars": MIN_STARS,
        "platforms": list(PLATFORMS),
        "networks": list(NETWORKS),
        "maxPluginsPerEntrant": MAX_PLUGINS_PER_ENTRANT,
    }


def line(value, maximum):
    clean = "".join(
        character if character.isprintable() else " "
        for character in " ".join(str(value or "").split()))
    return clean.strip()[:maximum]


def _public_host(host):
    return bool(
        HOST_RE.fullmatch(host)
        and not host.replace(".", "").isdigit()
        and not host.endswith((".local", ".internal", ".localhost"))
        and host not in {"localhost"})


def post_link(network, value):
    raw = line(value, 600)
    if not raw:
        return "", ""
    try:
        parsed = urlsplit(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
        port = parsed.port
    except ValueError:
        return "", ""
    if (parsed.scheme != "https" or parsed.username or parsed.password
            or port not in (None, 443)):
        return "", ""
    if network == "x":
        match = X_PATH_RE.fullmatch(parsed.path)
        if host not in X_HOSTS or not match:
            return "", ""
        url = "https://x.com/%s/status/%s" % (match.group(1), match.group(2))
        return url, ("https://publish.twitter.com/oembed?omit_script=1&url="
                     + quote(url, safe=""))
    if network == "reddit":
        match = REDDIT_PATH_RE.fullmatch(parsed.path)
        if not (host == "reddit.com" or host.endswith(".reddit.com")) or not match:
            return "", ""
        kind = "r" if match.group(1) == "r" else "user"
        url = "https://www.reddit.com/%s/%s/comments/%s/" % (
            kind, match.group(2), match.group(3))
        return url, url + ".json"
    if network == "mastodon":
        match = MASTODON_PATH_RE.fullmatch(parsed.path)
        if not match or not _public_host(host) or host in X_HOSTS:
            return "", ""
        url = "https://%s/@%s/%s" % (host, match.group(1), match.group(2))
        return url, "https://%s/api/v1/statuses/%s" % (host, match.group(2))
    return "", ""


def carries_hashtag(page_text):
    return HASHTAG[1:].lower() in str(page_text or "").lower()


def discord_handle(value):
    handle = line(value, 40).lower().lstrip("@")
    return handle if DISCORD_RE.fullmatch(handle) else ""


def checklist(entrant, account_ok, member_flags, discord_linked):
    entrant = entrant if isinstance(entrant, dict) else {}
    posts = entrant.get("posts") if isinstance(entrant.get("posts"), dict) else {}
    discord = entrant.get("discord") if isinstance(
        entrant.get("discord"), dict) else {}
    discord_state = "confirmed" if discord_linked else str(
        discord.get("state") or ("pending" if discord.get("handle") else ""))
    items = {
        "account": "confirmed" if account_ok else "missing",
        "member": "confirmed" if not member_flags else "pending",
        "discord": discord_state or "missing",
    }
    for network in NETWORKS:
        post = posts.get(network) if isinstance(posts.get(network), dict) else {}
        items[network] = str(post.get("state") or "missing") if post.get(
            "url") else "missing"
    return items


def is_qualified(items):
    return all(state == "confirmed" for state in items.values())


def standings(plugins, ratings, frozen=None):
    plugins = list(plugins or [])
    owners = {(item["market"], item["package"]): item.get("ownerBi", "")
              for item in plugins}
    counted = {key: [] for key in owners}
    seen = set()
    for rating in ratings or []:
        key = (str(rating.get("market") or ""), str(rating.get("package_id") or ""))
        reviewer = str(rating.get("reviewer_bi") or "")
        if key not in counted or not reviewer or reviewer == owners[key]:
            continue
        if (key, reviewer) in seen:
            continue
        seen.add((key, reviewer))
        at = max(int(rating.get("created_at") or 0),
                 int(rating.get("qualified_at") or 0), STARTS_AT)
        if at <= ENDS_AT:
            counted[key].append(at)
    frozen = dict(frozen or {})
    board = []
    for item in plugins:
        key = (item["market"], item["package"])
        times = sorted(counted[key])
        board.append(dict(
            item,
            ratings=len(times),
            reachedAt=times[RATINGS_NEEDED - 1] if len(times) >= RATINGS_NEEDED else 0,
            rank=int(frozen.get(key) or 0),
            final=bool(frozen.get(key)),
        ))
    taken = {entry["rank"] for entry in board if entry["rank"]}
    live = sorted(
        (entry for entry in board if entry["reachedAt"] and not entry["rank"]),
        key=lambda entry: (entry["reachedAt"], entry["package"]))
    next_rank = 1
    for entry in live:
        while next_rank in taken:
            next_rank += 1
        if next_rank > PRIZE_SLOTS:
            break
        entry["rank"] = next_rank
        taken.add(next_rank)
    board.sort(key=lambda entry: (
        entry["rank"] or PRIZE_SLOTS + 1, -entry["ratings"], entry["package"]))
    return board


def _response(runtime, payload, status=200):
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"})


async def _body(runtime):
    if not runtime.same_origin():
        return None, _response(runtime, {"error": "origin_not_allowed"}, 403)
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime, {"error": error},
            413 if error == "payload_too_large" else 400)
    return data, None


async def _account(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    name = line((record or {}).get("name"), 64).lower()
    if not account_bi or not name:
        return "", "", None
    return str(account_bi), name, record


def _verified(record):
    return bool(record) and bool(record.get("email_verified")) and str(
        record.get("status") or "active") == "active"


async def _entrant_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_bi,username,status,qualified_at,data,created_at "
        "FROM plugin_race_entrants WHERE account_bi=?", account_bi)


async def _evaluate(runtime, account_bi, record, entrant):
    flags = await runtime.member_flags(account_bi, record)
    linked = await runtime.discord_linked(account_bi)
    return checklist(entrant, _verified(record), flags, linked), flags


async def _store_entrant(runtime, account_bi, name, row, entrant, items, now):
    qualified_at = int((row or {}).get("qualified_at") or 0)
    status = str((row or {}).get("status") or "pending")
    if status != "disqualified":
        if is_qualified(items):
            status = "qualified"
            qualified_at = qualified_at or now
        else:
            status = "pending"
    sealed = await runtime.seal(entrant)
    if row:
        await runtime.d1_run(
            "UPDATE plugin_race_entrants SET status=?,qualified_at=?,data=?,"
            "updated_at=? WHERE account_bi=?",
            status, qualified_at, sealed, now, account_bi)
    else:
        await runtime.d1_run(
            "INSERT INTO plugin_race_entrants (account_bi,username,status,"
            "qualified_at,data,created_at,updated_at) VALUES (?,?,?,?,?,?,?)",
            account_bi, name, status, qualified_at, sealed, now, now)
    return status, qualified_at


async def _check_post(runtime, network, url, check_url):
    page = await runtime.read_public_page(check_url, MAX_FETCH_BYTES)
    if page is None:
        return "pending", "We could not open the post from here. We will check it by hand."
    if carries_hashtag(page):
        return "confirmed", ""
    return "missing", "The post is there but %s is not in it." % HASHTAG


async def _enter(runtime):
    data, error = await _body(runtime)
    if error:
        return error
    account_bi, name, record = await _account(runtime, data)
    if not account_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    if not _verified(record):
        return _response(runtime, {"error": "email_not_verified"}, 403)
    row = await _entrant_row(runtime, account_bi)
    if not row and data.get("acceptRules") is not True:
        return _response(runtime, {"error": "rules_required"}, 400)
    if row and row.get("status") == "disqualified":
        return _response(runtime, {"error": "disqualified"}, 403)
    entrant = await runtime.open((row or {}).get("data")) if row else None
    entrant = entrant if isinstance(entrant, dict) else {}
    entrant.setdefault("username", name)
    posts = dict(entrant.get("posts") or {})
    now = int(runtime.now())
    for network in NETWORKS:
        if network not in data:
            continue
        url, check_url = post_link(network, data.get(network))
        if not url:
            return _response(runtime, {"error": "invalid_post", "network": network}, 400)
        previous = posts.get(network) if isinstance(posts.get(network), dict) else {}
        if previous.get("url") == url and previous.get("state") in (
                "confirmed", "rejected"):
            continue
        post_bi = await runtime.blind("plugin-race-post:" + url.lower())
        owner = await runtime.d1_first(
            "SELECT account_bi FROM plugin_race_posts WHERE post_bi=?", post_bi)
        if owner and str(owner.get("account_bi") or "") != account_bi:
            return _response(runtime, {"error": "post_already_used", "network": network}, 409)
        if not owner:
            await runtime.d1_run(
                "INSERT INTO plugin_race_posts (post_bi,account_bi,network,"
                "created_at) VALUES (?,?,?,?)", post_bi, account_bi, network, now)
        state, detail = await _check_post(runtime, network, url, check_url)
        posts[network] = {"url": url, "state": state, "detail": detail,
                          "checkedAt": now}
    entrant["posts"] = posts
    if "discord" in data:
        handle = discord_handle(data.get("discord"))
        if not handle:
            return _response(runtime, {"error": "invalid_discord"}, 400)
        previous = entrant.get("discord") if isinstance(entrant.get("discord"), dict) else {}
        if previous.get("handle") != handle:
            entrant["discord"] = {"handle": handle, "state": "pending"}
    items, flags = await _evaluate(runtime, account_bi, record, entrant)
    status, qualified_at = await _store_entrant(
        runtime, account_bi, name, row, entrant, items, now)
    if not row:
        await runtime.audit(name, "plugin_race.enter", "plugin_race_entrant", name, {})
    return _response(runtime, {"ok": True, "me": await _me(
        runtime, account_bi, name, record)}, 200 if row else 201)


async def _submit_plugin(runtime):
    data, error = await _body(runtime)
    if error:
        return error
    account_bi, name, record = await _account(runtime, data)
    if not account_bi:
        return _response(runtime, {"error": "sign_in_required"}, 401)
    row = await _entrant_row(runtime, account_bi)
    if not row or row.get("status") != "qualified":
        return _response(runtime, {"error": "qualify_first"}, 403)
    now = int(runtime.now())
    if now < STARTS_AT:
        return _response(runtime, {"error": "race_not_open"}, 403)
    if now > ENDS_AT:
        return _response(runtime, {"error": "race_closed"}, 403)
    package = line(data.get("package"), 80).lower()
    if not PACKAGE_RE.fullmatch(package):
        return _response(runtime, {"error": "invalid_package"}, 400)
    if not await runtime.publishes_package(
            account_bi, MARKET, package, STARTS_AT):
        return _response(runtime, {"error": "package_not_yours"}, 403)
    summary = line(data.get("summary"), 300)
    if not summary:
        return _response(runtime, {"error": "summary_required"}, 400)
    platforms = data.get("platforms") if isinstance(data.get("platforms"), dict) else {}
    tried = {platform: line(platforms.get(platform), 300) for platform in PLATFORMS}
    missing = [platform for platform in PLATFORMS if not tried[platform]]
    if missing:
        return _response(runtime, {"error": "platforms_required", "missing": missing}, 400)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM plugin_race_plugins WHERE account_bi=? "
        "AND status<>'rejected'", account_bi)
    if int((count or {}).get("count") or 0) >= MAX_PLUGINS_PER_ENTRANT:
        return _response(runtime, {"error": "plugin_limit"}, 429)
    sealed = await runtime.seal({"summary": summary, "platforms": tried,
                                 "builder": name})
    try:
        await runtime.d1_run(
            "INSERT INTO plugin_race_plugins (market,package_id,account_bi,"
            "username,status,data,award_rank,paid_tx,created_at,updated_at) "
            "VALUES (?,?,?,?,'review',?,0,'',?,?)",
            MARKET, package, account_bi, name, sealed, now, now)
    except Exception:
        return _response(runtime, {"error": "already_entered"}, 409)
    await runtime.audit(name, "plugin_race.plugin", "plugin_race_plugin", package, {})
    return _response(runtime, {"ok": True, "me": await _me(
        runtime, account_bi, name, record)}, 201)


async def _plugin_rows(runtime, status=None):
    sql = ("SELECT market,package_id,account_bi,username,status,data,award_rank,"
           "paid_tx,created_at FROM plugin_race_plugins")
    args = ()
    if status:
        sql += " WHERE status=?"
        args = (status,)
    return await runtime.d1_all(sql + " ORDER BY created_at LIMIT 500", *args) or []


async def _board(runtime, rows):
    plugins = []
    frozen = {}
    for row in rows:
        if row.get("status") != "accepted":
            continue
        record = await runtime.open(row.get("data"))
        record = record if isinstance(record, dict) else {}
        key = (str(row.get("market") or ""), str(row.get("package_id") or ""))
        if int(row.get("award_rank") or 0):
            frozen[key] = int(row["award_rank"])
        plugins.append({
            "market": key[0], "package": key[1],
            "builder": str(row.get("username") or ""),
            "ownerBi": str(row.get("account_bi") or ""),
            "summary": line(record.get("summary"), 300),
            "paid": bool(row.get("paid_tx")),
            "paidTx": str(row.get("paid_tx") or ""),
        })
    ratings = await runtime.d1_all(
        "SELECT r.market AS market,r.package_id AS package_id,"
        "r.reviewer_bi AS reviewer_bi,r.created_at AS created_at,"
        "e.qualified_at AS qualified_at FROM marketplace_reviews r "
        "JOIN plugin_race_plugins p ON p.market=r.market "
        "AND p.package_id=r.package_id "
        "JOIN plugin_race_entrants e ON e.account_bi=r.reviewer_bi "
        "WHERE p.status='accepted' AND r.hidden=0 AND r.stars>=? "
        "AND e.status='qualified' LIMIT 10000", MIN_STARS) if plugins else []
    board = standings(plugins, ratings, frozen)
    for entry in board:
        entry.pop("ownerBi", None)
    return board


async def _me(runtime, account_bi, name, record):
    row = await _entrant_row(runtime, account_bi)
    if not row:
        return {"account": name, "emailVerified": _verified(record),
                "entrant": None, "plugins": []}
    entrant = await runtime.open(row.get("data"))
    entrant = entrant if isinstance(entrant, dict) else {}
    items, flags = await _evaluate(runtime, account_bi, record, entrant)
    posts = entrant.get("posts") if isinstance(entrant.get("posts"), dict) else {}
    discord = entrant.get("discord") if isinstance(entrant.get("discord"), dict) else {}
    plugins = []
    for plugin in await runtime.d1_all(
            "SELECT package_id,status,data,award_rank,paid_tx FROM "
            "plugin_race_plugins WHERE account_bi=? ORDER BY created_at",
            account_bi) or []:
        record_data = await runtime.open(plugin.get("data"))
        plugins.append({
            "package": str(plugin.get("package_id") or ""),
            "status": str(plugin.get("status") or ""),
            "note": line((record_data or {}).get("note"), 300),
            "rank": int(plugin.get("award_rank") or 0),
            "paid": bool(plugin.get("paid_tx")),
        })
    return {
        "account": name,
        "emailVerified": _verified(record),
        "entrant": {
            "status": str(row.get("status") or ""),
            "qualifiedAt": int(row.get("qualified_at") or 0),
            "checks": items,
            "memberFlags": list(flags or []),
            "discord": line(discord.get("handle"), 40),
            "posts": {network: {
                "url": line((posts.get(network) or {}).get("url"), 600),
                "state": items[network],
                "detail": line((posts.get(network) or {}).get("detail"), 200),
            } for network in NETWORKS},
        },
        "plugins": plugins,
        "payoutWallet": bool(await runtime.payout_wallet(account_bi)),
    }


async def _public(runtime):
    account_bi, name, record = await _account(runtime)
    rows = await _plugin_rows(runtime, "accepted")
    qualified = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM plugin_race_entrants "
        "WHERE status='qualified'")
    board = await _board(runtime, rows)
    payload = {
        "ok": True,
        "race": rules(),
        "open": STARTS_AT <= int(runtime.now()) <= ENDS_AT,
        "qualifiedEntrants": int((qualified or {}).get("count") or 0),
        "prizesLeft": max(0, PRIZE_SLOTS - sum(1 for entry in board if entry["rank"])),
        "plugins": board,
        "me": None,
    }
    if account_bi:
        payload["me"] = await _me(runtime, account_bi, name, record)
    return _response(runtime, payload)


async def _admin(runtime):
    method = runtime.method()
    data = {}
    if method == "POST":
        data, error = await _body(runtime)
        if error:
            return error
    account_bi, name, _record = await _account(runtime, data)
    if not account_bi or not await runtime.is_admin(name):
        return _response(runtime, {"error": "admin_required"}, 403)
    if method == "GET":
        return _response(runtime, await _admin_listing(runtime))
    action = line(data.get("action"), 20).lower()
    handler = ADMIN_ACTIONS.get(action)
    if handler is None:
        return _response(runtime, {"error": "unknown_action"}, 400)
    return await handler(runtime, name, data)


async def _admin_listing(runtime):
    entrants = []
    for row in await runtime.d1_all(
            "SELECT username,status,qualified_at,data,created_at FROM "
            "plugin_race_entrants ORDER BY created_at DESC LIMIT 1000") or []:
        entrant = await runtime.open(row.get("data"))
        entrant = entrant if isinstance(entrant, dict) else {}
        posts = entrant.get("posts") if isinstance(entrant.get("posts"), dict) else {}
        entrants.append({
            "username": str(row.get("username") or ""),
            "status": str(row.get("status") or ""),
            "qualifiedAt": int(row.get("qualified_at") or 0),
            "discord": (entrant.get("discord") or {}),
            "posts": {network: posts.get(network) for network in NETWORKS
                      if posts.get(network)},
        })
    plugins = []
    rows = await _plugin_rows(runtime)
    for row in rows:
        record = await runtime.open(row.get("data"))
        plugins.append({
            "package": str(row.get("package_id") or ""),
            "builder": str(row.get("username") or ""),
            "status": str(row.get("status") or ""),
            "summary": line((record or {}).get("summary"), 300),
            "platforms": (record or {}).get("platforms") or {},
            "rank": int(row.get("award_rank") or 0),
            "paidTx": str(row.get("paid_tx") or ""),
        })
    board = await _board(runtime, rows)
    for entry in board:
        if entry["rank"]:
            owner = await runtime.d1_first(
                "SELECT account_bi FROM plugin_race_plugins WHERE market=? "
                "AND package_id=?", entry["market"], entry["package"])
            entry["wallet"] = await runtime.payout_wallet(
                str((owner or {}).get("account_bi") or ""))
    return {"ok": True, "race": rules(), "entrants": entrants,
            "plugins": plugins, "standings": board}


async def _admin_entrant(runtime, account):
    account = line(account, 64).lower()
    if not USERNAME_RE.fullmatch(account):
        return "", None, None
    account_bi = await runtime.blind(account)
    row = await _entrant_row(runtime, account_bi)
    if not row:
        return account_bi, None, None
    entrant = await runtime.open(row.get("data"))
    return account_bi, row, entrant if isinstance(entrant, dict) else {}


async def _admin_recheck(runtime, actor, account_bi, row, entrant, action, details):
    record = await runtime.account_record(account_bi)
    items, _flags = await _evaluate(runtime, account_bi, record, entrant)
    status, _at = await _store_entrant(
        runtime, account_bi, str(row.get("username") or ""), row, entrant,
        items, int(runtime.now()))
    await runtime.audit(actor, action, "plugin_race_entrant",
                        str(row.get("username") or ""), details)
    return _response(runtime, {"ok": True, "status": status, "checks": items})


async def _admin_post(runtime, actor, data):
    network = line(data.get("network"), 20).lower()
    state = line(data.get("state"), 20).lower()
    if network not in NETWORKS or state not in ("confirmed", "rejected"):
        return _response(runtime, {"error": "invalid_request"}, 400)
    account_bi, row, entrant = await _admin_entrant(runtime, data.get("account"))
    if not row:
        return _response(runtime, {"error": "entrant_not_found"}, 404)
    posts = dict(entrant.get("posts") or {})
    post = posts.get(network) if isinstance(posts.get(network), dict) else {}
    if not post.get("url"):
        return _response(runtime, {"error": "post_not_found"}, 404)
    posts[network] = dict(post, state=state, detail="Checked by the ForkMesh team.")
    entrant["posts"] = posts
    return await _admin_recheck(runtime, actor, account_bi, row, entrant,
                                "plugin_race.post", {"network": network, "state": state})


async def _admin_discord(runtime, actor, data):
    state = line(data.get("state"), 20).lower()
    if state not in ("confirmed", "rejected"):
        return _response(runtime, {"error": "invalid_request"}, 400)
    account_bi, row, entrant = await _admin_entrant(runtime, data.get("account"))
    if not row:
        return _response(runtime, {"error": "entrant_not_found"}, 404)
    discord = entrant.get("discord") if isinstance(entrant.get("discord"), dict) else {}
    if not discord.get("handle"):
        return _response(runtime, {"error": "discord_not_given"}, 404)
    entrant["discord"] = dict(discord, state=state)
    return await _admin_recheck(runtime, actor, account_bi, row, entrant,
                                "plugin_race.discord", {"state": state})


async def _admin_entrant_status(runtime, actor, data):
    status = line(data.get("status"), 20).lower()
    if status not in ("disqualified", "pending"):
        return _response(runtime, {"error": "invalid_request"}, 400)
    account_bi, row, entrant = await _admin_entrant(runtime, data.get("account"))
    if not row:
        return _response(runtime, {"error": "entrant_not_found"}, 404)
    if status == "disqualified":
        await runtime.d1_run(
            "UPDATE plugin_race_entrants SET status='disqualified',updated_at=? "
            "WHERE account_bi=?", int(runtime.now()), account_bi)
        await runtime.audit(actor, "plugin_race.disqualify", "plugin_race_entrant",
                            str(row.get("username") or ""), {})
        return _response(runtime, {"ok": True, "status": "disqualified"})
    row = dict(row, status="pending")
    return await _admin_recheck(runtime, actor, account_bi, row, entrant,
                                "plugin_race.reinstate", {})


async def _admin_plugin(runtime, actor, data):
    package = line(data.get("package"), 80).lower()
    status = line(data.get("status"), 20).lower()
    if not PACKAGE_RE.fullmatch(package) or status not in ("accepted", "rejected"):
        return _response(runtime, {"error": "invalid_request"}, 400)
    row = await runtime.d1_first(
        "SELECT data,award_rank,paid_tx FROM plugin_race_plugins WHERE market=? "
        "AND package_id=?", MARKET, package)
    if not row:
        return _response(runtime, {"error": "plugin_not_found"}, 404)
    if row.get("paid_tx") and status == "rejected":
        return _response(runtime, {"error": "already_paid"}, 409)
    record = await runtime.open(row.get("data"))
    record = dict(record if isinstance(record, dict) else {},
                  note=line(data.get("note"), 300))
    rank = int(row.get("award_rank") or 0) if status == "accepted" else 0
    await runtime.d1_run(
        "UPDATE plugin_race_plugins SET status=?,data=?,award_rank=?,updated_at=? "
        "WHERE market=? AND package_id=?",
        status, await runtime.seal(record), rank, int(runtime.now()), MARKET,
        package)
    await runtime.audit(actor, "plugin_race.review", "plugin_race_plugin", package,
                        {"status": status})
    return _response(runtime, {"ok": True})


async def _admin_award(runtime, actor, data):
    board = await _board(runtime, await _plugin_rows(runtime, "accepted"))
    awarded = []
    now = int(runtime.now())
    for entry in board:
        if entry["rank"] and not entry["final"]:
            await runtime.d1_run(
                "UPDATE plugin_race_plugins SET award_rank=?,updated_at=? "
                "WHERE market=? AND package_id=? AND award_rank=0",
                entry["rank"], now, entry["market"], entry["package"])
            awarded.append({"package": entry["package"], "rank": entry["rank"]})
    if awarded:
        await runtime.audit(actor, "plugin_race.award", "plugin_race", NAME,
                            {"awarded": awarded})
    return _response(runtime, {"ok": True, "awarded": awarded})


async def _admin_paid(runtime, actor, data):
    package = line(data.get("package"), 80).lower()
    signature = line(data.get("tx"), 100)
    if not PACKAGE_RE.fullmatch(package) or not SIGNATURE_RE.fullmatch(signature):
        return _response(runtime, {"error": "invalid_request"}, 400)
    row = await runtime.d1_first(
        "SELECT award_rank FROM plugin_race_plugins WHERE market=? "
        "AND package_id=?", MARKET, package)
    if not row or not int(row.get("award_rank") or 0):
        return _response(runtime, {"error": "not_awarded"}, 409)
    await runtime.d1_run(
        "UPDATE plugin_race_plugins SET paid_tx=?,updated_at=? WHERE market=? "
        "AND package_id=?", signature, int(runtime.now()), MARKET, package)
    await runtime.audit(actor, "plugin_race.paid", "plugin_race_plugin", package,
                        {"tx": signature, "lamports": PRIZE_LAMPORTS})
    return _response(runtime, {"ok": True})


ADMIN_ACTIONS = {
    "post": _admin_post,
    "discord": _admin_discord,
    "entrant": _admin_entrant_status,
    "plugin": _admin_plugin,
    "award": _admin_award,
    "paid": _admin_paid,
}


async def handle(runtime, path):
    normalized = str(path or "").rstrip("/")
    method = runtime.method()
    await runtime.ensure_schema()
    if normalized == ADMIN_PATH:
        if method not in ("GET", "POST"):
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await _admin(runtime)
    if normalized == PREFIX:
        if method != "GET":
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await _public(runtime)
    if normalized in (ENTRANT_PATH, PLUGINS_PATH):
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405)
        return await (_enter(runtime) if normalized == ENTRANT_PATH
                      else _submit_plugin(runtime))
    return _response(runtime, {"error": "not_found"}, 404)
