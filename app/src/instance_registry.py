import base64
import ipaddress
import json
import re

import instance_directory

DOC_PATH = "/.well-known/forkmesh-instance.json"
REGISTER_PATH = "/api/start/instances/register"
CONTEXT = "forkmesh-instance-directory-v1"
DOC_VERSION = 1
SETTING_KEY = "directory"
DEFAULT_DIRECTORY = "https://app.forkmesh.com"
MAX_SKEW_MS = 10 * 60 * 1000
BODY_MAX_BYTES = 2048
DOC_MAX_BYTES = 16 * 1024
LOGO_MAX_BYTES = 200 * 1024
REPLY_MAX_BYTES = 8 * 1024
HOUR_MS = 60 * 60 * 1000
CLIENT_PER_HOUR = 30
DOMAIN_PER_HOUR = 12
REANNOUNCE_MS = 6 * HOUR_MS
RETRY_MS = 50 * 60 * 1000
NAME_MAX = 80
MESSAGE_MAX = 300
NONCE_RE = re.compile(r"^[0-9a-f]{32}$")
KEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIGNATURE_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
LOGO_PATH_RE = re.compile(r"^/instance-logo\.(?:png|jpg|jpeg|webp|svg)$")
DIRECTORY_RE = re.compile(r"^https://[a-z0-9.-]+(?::[0-9]{1,5})?$")
WORLD_HOME_RE = re.compile(r"^https://[a-z0-9.-]+(?::[0-9]{1,5})?/world/$")
OWN_DOMAINS = ("forkmesh.com",)


class Refused(Exception):
    def __init__(self, code, status, message):
        super().__init__(message)
        self.code, self.status, self.message = code, status, message


def own_domain(domain):
    return any(domain == item or domain.endswith("." + item)
               for item in OWN_DOMAINS)


def valid_domain(domain):
    return bool(instance_directory.DOMAIN_RE.fullmatch(str(domain or "")))


def domain_of(origin):
    match = re.fullmatch(r"https://([a-z0-9.-]+)/?", str(origin or "").lower())
    return match.group(1) if match and valid_domain(match.group(1)) else ""


def message(domain, ts, nonce):
    return ("%s\n%s\n%d\n%s" % (CONTEXT, domain, int(ts), nonce)).encode(
        "utf-8")


def flag(value):
    return value if isinstance(value, bool) else None


def parse_setting(doc):
    try:
        data = json.loads(doc) if doc else {}
    except (TypeError, ValueError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    status = data.get("status") if isinstance(data.get("status"), dict) else {}
    return {
        "listed": data.get("listed") is True,
        "world": data.get("world") is True,
        "status": clean_status(status),
    }


def clean_status(status):
    sent = status.get("sent") if isinstance(status.get("sent"), dict) else {}
    world_home = str(status.get("worldHome") or "")
    try:
        slot = int(status.get("slot", -1))
    except (TypeError, ValueError):
        slot = -1
    return {
        "state": str(status.get("state") or "")
        if status.get("state") in ("listed", "removed", "failed") else "",
        "message": str(status.get("message") or "")[:MESSAGE_MAX],
        "at": int(status.get("at") or 0)
        if isinstance(status.get("at"), int) else 0,
        "slot": slot if 0 <= slot < instance_directory.WORLD_SLOTS else -1,
        "id": str(status.get("id") or "")
        if instance_directory.ID_RE.fullmatch(str(status.get("id") or ""))
        else "",
        "worldHome": world_home if WORLD_HOME_RE.fullmatch(world_home) else "",
        "sent": {"listed": sent.get("listed") is True,
                 "world": sent.get("world") is True},
    }


def dump_setting(setting):
    return json.dumps({
        "listed": bool(setting.get("listed")),
        "world": bool(setting.get("world")),
        "status": clean_status(setting.get("status") or {}),
    }, separators=(",", ":"), sort_keys=True)


def from_install(value):
    if value in (None, "", {}):
        return None
    if not isinstance(value, dict) or not set(value) <= {"listed", "world"}:
        raise ValueError("directory may only set listed and world")
    if any(not isinstance(item, bool) for item in value.values()):
        raise ValueError("directory.listed and directory.world must be true "
                         "or false")
    return {"listed": value.get("listed") is True,
            "world": value.get("world") is True, "status": {}}


def participating(setting):
    return bool(setting.get("listed") or setting.get("world"))


def choice(setting):
    return {"listed": bool(setting.get("listed")),
            "world": bool(setting.get("world"))}


def due(setting, now):
    status = setting["status"]
    if not participating(setting):
        return bool(status["state"]) and status["state"] != "removed"
    if status["state"] == "failed":
        return now - status["at"] >= RETRY_MS
    if status["state"] != "listed" or status["sent"] != choice(setting):
        return True
    return now - status["at"] >= REANNOUNCE_MS


def directory_url(value):
    value = str(value or DEFAULT_DIRECTORY).strip().rstrip("/")
    return value if DIRECTORY_RE.fullmatch(value) else ""


def document(domain, name, logo_path, setting, world_on, public_key):
    origin = "https://%s" % domain
    world = bool(setting.get("world"))
    return {
        "forkmeshInstance": DOC_VERSION,
        "domain": domain,
        "url": origin + "/",
        "name": " ".join(str(name or "").split())[:NAME_MAX] or domain,
        "logo": logo_path if LOGO_PATH_RE.fullmatch(str(logo_path or ""))
        else "",
        "listed": bool(setting.get("listed")),
        "world": world,
        "worldUrl": origin + "/world/" if world and world_on else "",
        "publicKey": public_key,
    }


def parse_announcement(data, now):
    if not isinstance(data, dict):
        raise Refused("invalid_json", 400, "Send a JSON object.")
    domain = str(data.get("domain") or "").strip().lower()
    nonce = str(data.get("nonce") or "")
    signature = str(data.get("signature") or "")
    ts = data.get("ts")
    if not valid_domain(domain):
        raise Refused("invalid_domain", 400, "Give the instance's domain.")
    if own_domain(domain):
        raise Refused("invalid_domain", 400,
                      "forkmesh.com is not a self-hosted instance.")
    if not isinstance(ts, int) or isinstance(ts, bool) or \
            abs(now - ts) > MAX_SKEW_MS:
        raise Refused("stale", 400, "The announcement time is more than ten "
                      "minutes off. Check the server's clock.")
    if not NONCE_RE.fullmatch(nonce) or not SIGNATURE_RE.fullmatch(signature):
        raise Refused("invalid_signature", 400,
                      "The announcement needs a nonce and a signature.")
    return {"domain": domain, "ts": ts, "nonce": nonce,
            "signature": signature}


def parse_document(raw, domain):
    try:
        data = json.loads(bytes(raw).decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise Refused("invalid_document", 400,
                      "%s is not a JSON document." % DOC_PATH) from None
    if not isinstance(data, dict) or data.get("forkmeshInstance") != \
            DOC_VERSION:
        raise Refused("invalid_document", 400,
                      "%s is not a ForkMesh instance document." % DOC_PATH)
    if data.get("domain") != domain or \
            data.get("url") != "https://%s/" % domain:
        raise Refused("invalid_document", 400,
                      "The document names another domain.")
    key = str(data.get("publicKey") or "")
    listed, world = flag(data.get("listed")), flag(data.get("world"))
    name = " ".join(str(data.get("name") or "").split())
    logo = str(data.get("logo") or "")
    world_url = str(data.get("worldUrl") or "")
    if not KEY_RE.fullmatch(key) or listed is None or world is None or \
            not isinstance(data.get("name"), str) or not name or \
            len(name) > NAME_MAX or (logo and not LOGO_PATH_RE.fullmatch(
                logo)) or (world_url and (
                    not world or world_url != "https://%s/world/" % domain)):
        raise Refused("invalid_document", 400,
                      "The document is missing a field or has a bad value.")
    return {"name": name, "logo": logo, "listed": listed, "world": world,
            "worldUrl": world_url, "publicKey": key}


def logo_data_url(raw, logo_kind):
    raw = bytes(raw or b"")
    kind = logo_kind(raw) if 0 < len(raw) <= LOGO_MAX_BYTES else ""
    if not kind:
        return ""
    return "data:image/%s;base64,%s" % (
        kind, base64.b64encode(raw).decode("ascii"))


def public_addresses(addresses):
    checked = []
    for item in addresses or []:
        try:
            address = ipaddress.ip_address(str(item).split("%", 1)[0])
        except ValueError:
            return False
        if not address.is_global or address.is_multicast:
            return False
        checked.append(address)
    return bool(checked)


async def _limit(runtime, client, domain, now):
    await runtime.d1_run(
        "DELETE FROM instance_register_attempts WHERE at<?", now - HOUR_MS)
    by_client = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM instance_register_attempts WHERE "
        "client_key=? AND at>=?", client, now - HOUR_MS)
    by_domain = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM instance_register_attempts WHERE "
        "domain=? AND at>=?", domain, now - HOUR_MS)
    if int((by_client or {}).get("n") or 0) >= CLIENT_PER_HOUR or \
            int((by_domain or {}).get("n") or 0) >= DOMAIN_PER_HOUR:
        raise Refused("rate_limited", 429, "Too many announcements in one "
                      "hour. The next scheduled one will go through.")
    await runtime.d1_run(
        "INSERT INTO instance_register_attempts (client_key,domain,at) "
        "VALUES (?,?,?)", client, domain, now)


async def _fetch(runtime, url, limit, what):
    answer = await runtime.fetch_bytes(url, limit)
    if answer is None:
        raise Refused("unreachable", 502, "%s did not answer over HTTPS."
                      % what)
    status, raw = answer
    if status != 200:
        raise Refused("unreachable", 502, "%s answered HTTP %d." % (
            what, status))
    if len(raw) > limit:
        raise Refused("too_large", 400, "%s is larger than %d KB." % (
            what, limit // 1024))
    return raw


async def _verified(runtime, announcement):
    domain = announcement["domain"]
    if not public_addresses(await runtime.resolve_host(domain)):
        raise Refused("address_not_public", 400,
                      "%s does not resolve to public addresses only." % domain)
    raw = await _fetch(runtime, "https://%s%s" % (domain, DOC_PATH),
                       DOC_MAX_BYTES, DOC_PATH)
    doc = parse_document(raw, domain)
    if not await runtime.verify(doc["publicKey"], announcement["signature"],
                                message(domain, announcement["ts"],
                                        announcement["nonce"])):
        raise Refused("bad_signature", 401, "The signature does not match "
                      "the key in %s." % DOC_PATH)
    return doc


async def remove(runtime, domain):
    await instance_directory.ensure(runtime)
    await runtime.d1_run(
        "DELETE FROM instance_directory WHERE domain=?", domain)
    await runtime.d1_run(
        "DELETE FROM instance_registrations WHERE domain=?", domain)


async def register(runtime, logo_kind):
    if runtime.method() != "POST":
        return _error(runtime, Refused("method_not_allowed", 405,
                                       "Use POST."))
    raw = await runtime.body_bytes(BODY_MAX_BYTES)
    if raw is None:
        return _error(runtime, Refused("request_too_large", 413,
                                       "The announcement is too large."))
    try:
        data = json.loads(bytes(raw).decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        data = None
    now = runtime.now()
    try:
        announcement = parse_announcement(data, now)
        await instance_directory.ensure(runtime)
        domain = announcement["domain"]
        await _limit(runtime, await runtime.client_key(), domain, now)
        known = await runtime.d1_first(
            "SELECT announced_at FROM instance_registrations WHERE domain=?",
            domain)
        if known and announcement["ts"] <= int(known["announced_at"] or 0):
            raise Refused("replayed", 409, "This announcement was already "
                          "used.")
        doc = await _verified(runtime, announcement)
    except Refused as refused:
        return _error(runtime, refused)
    if not (doc["listed"] or doc["world"]):
        await remove(runtime, domain)
        return runtime.response({"ok": True, "status": "removed"})
    logo = ""
    if doc["logo"]:
        answer = await runtime.fetch_bytes(
            "https://%s%s" % (domain, doc["logo"]), LOGO_MAX_BYTES)
        if answer and answer[0] == 200:
            logo = logo_data_url(answer[1], logo_kind)
    row = await runtime.d1_first(
        "SELECT id FROM instance_directory WHERE domain=?", domain)
    entry_id = row["id"] if row else runtime.new_id()
    slot = await instance_directory.publish(runtime, entry_id, domain, {
        "name": doc["name"], "logo": logo, "world": doc["world"],
        "listed": doc["listed"]})
    await runtime.d1_run(
        "INSERT INTO instance_registrations (domain,public_key,world_url,"
        "announced_at,created_at,updated_at) VALUES (?,?,?,?,?,?) "
        "ON CONFLICT(domain) DO UPDATE SET public_key=excluded.public_key,"
        "world_url=excluded.world_url,announced_at=excluded.announced_at,"
        "updated_at=excluded.updated_at", domain, doc["publicKey"],
        doc["worldUrl"], announcement["ts"], now, now)
    hidden = await runtime.d1_first(
        "SELECT hidden FROM instance_directory WHERE domain=?", domain)
    return runtime.response({
        "ok": True, "status": "listed", "id": entry_id,
        "listed": doc["listed"], "world": doc["world"],
        "slot": slot if slot is not None else -1,
        "hidden": bool(int((hidden or {}).get("hidden") or 0)),
        "worldHome": runtime.world_home(),
    })


def _error(runtime, refused):
    return runtime.response({"ok": False, "error": refused.code,
                             "message": refused.message}, refused.status)


def announcement_body(domain, ts, nonce, signature):
    return {"domain": domain, "ts": int(ts), "nonce": nonce,
            "signature": signature}


def status_from_reply(status_code, reply, sent, now):
    reply = reply if isinstance(reply, dict) else {}
    if status_code == 200 and reply.get("ok") is True and \
            reply.get("status") in ("listed", "removed"):
        return clean_status({
            "state": reply["status"], "at": now, "sent": sent,
            "slot": reply.get("slot", -1), "id": reply.get("id") or "",
            "worldHome": reply.get("worldHome") or "",
            "message": "Hidden by a forkmesh.com admin."
            if reply.get("hidden") is True else "",
        })
    text = str(reply.get("message") or "")
    if not text:
        text = ("forkmesh.com did not answer." if not status_code else
                "forkmesh.com answered HTTP %d." % status_code)
    return clean_status({"state": "failed", "at": now, "sent": sent,
                         "message": text})


async def announce(runtime, setting=None):
    if setting is None:
        doc = await runtime.setting_doc()
        if doc is None:
            return {"announced": False, "reason": "not_configured"}
        setting = parse_setting(doc)
    domain = runtime.domain()
    target = runtime.directory_url()
    key = runtime.public_key()
    now = runtime.now()
    sent = choice(setting)
    if not domain or not target or not key:
        setting["status"] = clean_status({
            "state": "failed", "at": now, "sent": sent,
            "message": "This server has no domain, directory address or "
                       "identity key to announce with."})
        await runtime.save_setting_doc(dump_setting(setting))
        return {"announced": False, "reason": "unconfigured"}
    nonce = runtime.nonce()
    signature = await runtime.sign(message(domain, now, nonce))
    status_code, reply = await runtime.post_json(
        target + REGISTER_PATH,
        announcement_body(domain, now, nonce, signature), REPLY_MAX_BYTES)
    fresh = parse_setting(await runtime.setting_doc() or "")
    fresh["status"] = status_from_reply(status_code, reply, sent, now)
    await runtime.save_setting_doc(dump_setting(fresh))
    return {"announced": True, "state": fresh["status"]["state"]}


async def announce_if_due(runtime):
    doc = await runtime.setting_doc()
    if doc is None:
        return {"announced": False, "reason": "not_configured"}
    setting = parse_setting(doc)
    if not due(setting, runtime.now()):
        return {"announced": False, "reason": "not_due"}
    return await announce(runtime, setting)


def world_portal(setting, world_on):
    status = setting["status"]
    if not (world_on and setting.get("world") and status["state"] == "listed"
            and status["sent"]["world"] and status["worldHome"]):
        return None
    return {"home": status["worldHome"], "slot": status["slot"]}
