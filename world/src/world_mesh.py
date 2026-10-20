import base64
import hashlib
import hmac
import json
import re
from urllib.parse import urlparse


HUB_ID = "forkmesh"
HUB_NAME = "ForkMesh"
DEFAULT_HUB_API = "https://app.forkmesh.com"
DEFAULT_HUB_WORLD = "https://world.forkmesh.com"
SLOTS = 12
HEX_SLOTS = 5
SHAPE_FLOWER = "flower"
SHAPE_HEX = "hex"
SCENE_PATH = "/api/world/neighbour-scene"
MAX_WORLDS = SLOTS + 1
DIRECTORY_MAX_AGE = 60
DIRECTORY_CACHE_MS = 5 * 60 * 1000
DIRECTORY_FAILURE_CACHE_MS = 30 * 1000
VISA_TTL_MS = 60 * 1000
VISA_MAX_CHARS = 2048
VISITOR_STATUS = "Registered"
ID_RE = re.compile(r"^(?:forkmesh|[0-9a-f]{32})$")
NONCE_RE = re.compile(r"^[A-Za-z0-9_-]{8,32}$")
PAYLOAD_RE = re.compile(r"^[A-Za-z0-9_-]+$")
SIGNATURE_RE = re.compile(r"^[0-9a-f]{64}$")
LOGO_PATH_RE = re.compile(r"^/api/start/instances/[0-9a-f]{32}/logo\?v=\d+$")
HOST_RE = re.compile(
    r"^(?=.{1,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)*"
    r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?::\d{1,5})?$")
NAME_MAX = 60
VISA_LABEL = b"forkmesh-world-visa-v1\n"


def clean_origin(value):
    try:
        parsed = urlparse(str(value or "").strip())
        port = parsed.port
    except ValueError:
        return ""
    if parsed.scheme != "https" or not parsed.hostname:
        return ""
    if parsed.username is not None or parsed.password is not None:
        return ""
    if parsed.query or parsed.fragment:
        return ""
    host = parsed.hostname.lower().rstrip(".")
    if port and port != 443:
        host = "%s:%d" % (host, port)
    if not HOST_RE.fullmatch(host):
        return ""
    return "https://" + host


def origin_host(origin):
    origin = clean_origin(origin)
    return origin[len("https://"):] if origin else ""


def clean_name(value, fallback):
    name = " ".join(str(value or "").split())[:NAME_MAX]
    name = "".join(ch for ch in name if ch.isprintable() and ch not in "<>\"'`")
    return name.strip() or fallback


def setting(env, name):
    return str(getattr(env, name, "") or "").strip()


def is_hub(env):
    return not " ".join(setting(env, "INSTANCE_NAME").split())


def hex_slots(env):
    slots = set()
    for part in setting(env, "WORLD_MESH_HEX_SLOTS").split(","):
        part = part.strip()
        if part.isdigit() and int(part) < HEX_SLOTS:
            slots.add(int(part))
    return frozenset(slots)


def shape_for(value, slot):
    return SHAPE_HEX if value == SHAPE_HEX and 0 <= slot < HEX_SLOTS \
        else SHAPE_FLOWER


def hub_api(env):
    return clean_origin(setting(env, "WORLD_MESH_HUB")) or DEFAULT_HUB_API


def own_world_origin(env):
    for name in ("WORLD_ORIGIN", "PUBLIC_BASE_URL", "APP_ORIGIN"):
        origin = clean_origin(setting(env, name))
        if origin:
            return origin
    return DEFAULT_HUB_WORLD if is_hub(env) else ""


def own_api_origin(env):
    for name in ("APP_ORIGIN", "API_ORIGIN", "PUBLIC_BASE_URL"):
        origin = clean_origin(setting(env, name))
        if origin:
            return origin
    return DEFAULT_HUB_API if is_hub(env) else own_world_origin(env)


def hub_entry(env):
    world = own_world_origin(env) or DEFAULT_HUB_WORLD
    return {
        "id": HUB_ID,
        "name": HUB_NAME,
        "host": origin_host(world),
        "world": world,
        "api": own_api_origin(env) or DEFAULT_HUB_API,
        "logo": "",
        "slot": -1,
        "shape": SHAPE_FLOWER,
    }


def instance_entry(row, api_base, hexes=frozenset()):
    entry_id = str(row.get("id") or "")
    world = clean_origin(row.get("url"))
    try:
        slot = int(row.get("slot", -1))
    except (TypeError, ValueError):
        slot = -1
    if not re.fullmatch(r"[0-9a-f]{32}", entry_id) or not world:
        return None
    if not 0 <= slot < SLOTS:
        return None
    logo = str(row.get("logo") or "")
    if row.get("has_logo") and not logo:
        logo = "/api/start/instances/%s/logo?v=%d" % (
            entry_id, int(row.get("updated_at") or 0))
    return {
        "id": entry_id,
        "name": clean_name(row.get("name"), origin_host(world)),
        "host": origin_host(world),
        "world": world,
        "api": world,
        "logo": api_base + logo if LOGO_PATH_RE.fullmatch(logo) else "",
        "slot": slot,
        "shape": SHAPE_HEX if slot in hexes else SHAPE_FLOWER,
        **({"scene": world + SCENE_PATH} if slot in hexes else {}),
    }


def build_directory(hub, rows, api_base, hexes=frozenset()):
    worlds = [hub]
    slots = set()
    hosts = {hub["host"]}
    for row in rows or []:
        entry = instance_entry(dict(row), api_base, hexes)
        if not entry or entry["slot"] in slots or entry["host"] in hosts:
            continue
        slots.add(entry["slot"])
        hosts.add(entry["host"])
        worlds.append(entry)
        if len(worlds) >= MAX_WORLDS:
            break
    return worlds


def normalize_world(item):
    if not isinstance(item, dict):
        return None
    entry_id = str(item.get("id") or "")
    world = clean_origin(item.get("world"))
    api = clean_origin(item.get("api"))
    try:
        slot = int(item.get("slot", -1))
    except (TypeError, ValueError):
        return None
    if not ID_RE.fullmatch(entry_id) or not world or not api:
        return None
    if entry_id == HUB_ID:
        if slot != -1:
            return None
    elif not 0 <= slot < SLOTS or api != world:
        return None
    logo = str(item.get("logo") or "")
    try:
        parsed = urlparse(logo)
        logo_ok = (
            parsed.scheme == "https"
            and clean_origin("https://" + (parsed.netloc or ""))
            and LOGO_PATH_RE.fullmatch(
                parsed.path + ("?" + parsed.query if parsed.query else "")))
    except ValueError:
        logo_ok = False
    return {
        "id": entry_id,
        "name": clean_name(item.get("name"), origin_host(world)),
        "host": origin_host(world),
        "world": world,
        "api": api,
        "logo": logo if logo_ok else "",
        "slot": slot,
        "shape": shape_for(item.get("shape"), slot),
        **({"scene": world + SCENE_PATH}
           if shape_for(item.get("shape"), slot) == SHAPE_HEX
           and item.get("scene") == world + SCENE_PATH else {}),
    }


def normalize_directory(payload):
    items = payload.get("worlds") if isinstance(payload, dict) else None
    worlds = []
    ids = set()
    slots = set()
    hosts = set()
    for item in items if isinstance(items, list) else []:
        entry = normalize_world(item)
        if not entry or entry["id"] in ids or entry["host"] in hosts:
            continue
        if entry["slot"] >= 0 and entry["slot"] in slots:
            continue
        ids.add(entry["id"])
        hosts.add(entry["host"])
        if entry["slot"] >= 0:
            slots.add(entry["slot"])
        worlds.append(entry)
        if len(worlds) >= MAX_WORLDS:
            break
    if not any(entry["id"] == HUB_ID for entry in worlds):
        return []
    return worlds


def self_id(env, worlds):
    if is_hub(env):
        return HUB_ID
    hosts = {origin_host(own_world_origin(env)), origin_host(own_api_origin(env))}
    hosts.discard("")
    for entry in worlds:
        if entry["id"] != HUB_ID and entry["host"] in hosts:
            return entry["id"]
    return ""


def directory_payload(env, worlds):
    return {"ok": True, "self": self_id(env, worlds), "worlds": worlds}


def neighbor_for_origin(env, worlds, origin):
    origin = clean_origin(origin)
    if not origin:
        return None
    own = self_id(env, worlds)
    for entry in worlds:
        if entry["id"] != own and origin in (entry["world"], entry["api"]):
            return entry
    return None


def visa_signature(secret, payload):
    return hmac.new(secret, VISA_LABEL + payload.encode(), hashlib.sha256).hexdigest()


def visa_encode(secret, claim):
    payload = base64.urlsafe_b64encode(
        json.dumps(claim, separators=(",", ":"), sort_keys=True).encode()
    ).decode().rstrip("=")
    return payload + "." + visa_signature(secret, payload)


def visa_claim(token):
    value = str(token or "").strip()
    if not value or len(value) > VISA_MAX_CHARS or "." not in value:
        return None, "", ""
    payload, signature = value.rsplit(".", 1)
    if not PAYLOAD_RE.fullmatch(payload) or not SIGNATURE_RE.fullmatch(signature):
        return None, "", ""
    try:
        padded = payload + "=" * ((4 - len(payload) % 4) % 4)
        claim = json.loads(base64.urlsafe_b64decode(padded).decode())
    except (ValueError, UnicodeError):
        return None, "", ""
    if not isinstance(claim, dict):
        return None, "", ""
    return claim, payload, signature


def visa_unverified_home(token):
    claim, _payload, _signature = visa_claim(token)
    return clean_origin((claim or {}).get("home"))


def visa_verify(secret, token, now, home, destination):
    claim, payload, signature = visa_claim(token)
    if claim is None:
        return None
    if not hmac.compare_digest(signature, visa_signature(secret, payload)):
        return None
    try:
        issued = int(claim.get("issuedAt", 0) or 0)
        expires = int(claim.get("expiresAt", 0) or 0)
    except (TypeError, ValueError):
        return None
    if issued <= 0 or expires <= now or expires - issued > VISA_TTL_MS:
        return None
    if issued > now + 5000:
        return None
    if clean_origin(claim.get("home")) != clean_origin(home):
        return None
    if clean_origin(claim.get("destination")) != clean_origin(destination):
        return None
    nonce = str(claim.get("nonce") or "")
    name = str(claim.get("name") or "").strip()
    if not NONCE_RE.fullmatch(nonce) or not name:
        return None
    return {"name": name[:64], "nonce": nonce}


def visa_for(name, home, destination, nonce, now):
    return {
        "v": 1,
        "name": str(name or "")[:64],
        "home": clean_origin(home),
        "destination": clean_origin(destination),
        "nonce": nonce,
        "issuedAt": int(now),
        "expiresAt": int(now) + VISA_TTL_MS,
    }
