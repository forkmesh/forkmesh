import base64
import hashlib
import hmac
import json
import math
import re
from urllib.parse import urlparse


HUB_ID = "forkmesh"
HUB_NAME = "ForkMesh"
DEFAULT_HUB_API = "https://app.forkmesh.com"
DEFAULT_HUB_WORLD = "https://world.forkmesh.com"
SLOTS = 12
HEX_SLOTS = 5
# The hub may also say which of its town's desert petals a hex World stands
# on ("petal", 0-5); an instance passes it through so its own World can stand
# in the same place.
PETALS = 6
SHAPE_FLOWER = "flower"
SHAPE_HEX = "hex"
SCENE_PATH = "/api/world/neighbour-scene"
SCENE_VERSION = 1
SCENE_MAX_BYTES = 64 * 1024
SCENE_MAX_OBJECTS = 48
SCENE_MAX_AGE = 300
SCENE_TEXT_MAX = 40
SCENE_LAMP_RING = 30
SCENE_TREE_RING = 280
SCENE_TREES = 12
ICON_MAX_BYTES = 64 * 1024
ICON_MAX_AGE = 3600
ICON_FAILURE_MS = 60 * 1000
ICON_PATH_RE = re.compile(r"^/(?!/)[A-Za-z0-9._~!$&'()*+,;=:@%/?-]{0,299}$")
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


def ring(count, radius, offset=0.0):
    return [
        (round(radius * math.cos(offset + 2 * math.pi * i / count), 1),
         round(radius * math.sin(offset + 2 * math.pi * i / count), 1))
        for i in range(count)]


def default_scene(name, host):
    objects = [
        {"type": "plaza", "x": 0, "z": 0, "radius": 34, "color": "#d8d0bf"},
        {"type": "sign", "x": 0, "z": 0, "title": name[:SCENE_TEXT_MAX],
         "text": host[:60]},
    ]
    objects += [{"type": "lamp", "x": x, "z": z, "color": "#fff3c4"}
                for x, z in ring(4, SCENE_LAMP_RING, math.pi / 4)]
    objects += [{"type": "tree", "x": x, "z": z, "scale": 1.4}
                for x, z in ring(SCENE_TREES, SCENE_TREE_RING, math.pi / 12)]
    return {
        "v": SCENE_VERSION,
        "ground": {"color": "#9fc584", "accent": "#9ef7c6",
                   "pattern": "plain"},
        "objects": objects,
    }


def configured_scene(raw):
    if not raw or len(raw.encode("utf-8")) > SCENE_MAX_BYTES:
        return None
    try:
        scene = json.loads(raw)
    except ValueError:
        return None
    if not isinstance(scene, dict) or scene.get("v") != SCENE_VERSION:
        return None
    objects = scene.get("objects")
    if not isinstance(objects, list) or not objects \
            or len(objects) > SCENE_MAX_OBJECTS:
        return None
    if not all(isinstance(item, dict) for item in objects):
        return None
    return scene


def neighbour_scene(env):
    if is_hub(env):
        return None
    world = own_world_origin(env)
    if not world:
        return None
    host = origin_host(world)
    return configured_scene(setting(env, "WORLD_NEIGHBOUR_SCENE")) \
        or default_scene(clean_name(setting(env, "INSTANCE_NAME"), host), host)


def neighbour_icon_source(env, worlds, world_id, path):
    path = str(path or "")
    if not ICON_PATH_RE.fullmatch(path):
        return ""
    own = self_id(env, worlds)
    for entry in worlds:
        if entry["id"] == str(world_id or "") and entry["id"] != own \
                and entry.get("scene"):
            return entry["world"] + path
    return ""


def icon_type(raw):
    raw = bytes(raw or b"")
    if raw.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if raw.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if raw[:6] in (b"GIF87a", b"GIF89a"):
        return "image/gif"
    if raw[:4] == b"RIFF" and raw[8:12] == b"WEBP":
        return "image/webp"
    if raw[:4] == b"\x00\x00\x01\x00" and len(raw) > 6:
        return "image/x-icon"
    return ""


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


def petal_of(item):
    petal = item.get("petal")
    if isinstance(petal, int) and not isinstance(petal, bool) \
            and 0 <= petal < PETALS and item.get("shape") == SHAPE_HEX:
        return {"petal": petal}
    return {}


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
        **petal_of(item),
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


VISITOR_PREFIX = "fmwv1."
VISITOR_LABEL = b"forkmesh-world-visitor-v1\n"
VISITOR_TTL_MS = 15 * 60 * 1000
VISITOR_SCOPE = "world"


def visitor_signature(secret, payload):
    return hmac.new(
        secret, VISITOR_LABEL + payload.encode(), hashlib.sha256).hexdigest()


def visitor_for(name, home_host, audience, nonce, now):
    return {
        "v": 1,
        "sub": str(name or "")[:64],
        "home": str(home_host or "")[:253],
        "aud": clean_origin(audience),
        "scope": VISITOR_SCOPE,
        "nonce": nonce,
        "iat": int(now),
        "exp": int(now) + VISITOR_TTL_MS,
    }


def visitor_encode(secret, claim):
    payload = base64.urlsafe_b64encode(
        json.dumps(claim, separators=(",", ":"), sort_keys=True).encode()
    ).decode().rstrip("=")
    return VISITOR_PREFIX + payload + "." + visitor_signature(secret, payload)


def visitor_verify(secret, token, now, audience):
    value = str(token or "").strip()
    if not value.startswith(VISITOR_PREFIX):
        return None
    claim, payload, signature = visa_claim(value[len(VISITOR_PREFIX):])
    if claim is None:
        return None
    if not hmac.compare_digest(signature, visitor_signature(secret, payload)):
        return None
    try:
        issued = int(claim.get("iat", 0) or 0)
        expires = int(claim.get("exp", 0) or 0)
    except (TypeError, ValueError):
        return None
    if issued <= 0 or expires <= now or expires - issued > VISITOR_TTL_MS:
        return None
    if issued > now + 5000:
        return None
    if claim.get("scope") != VISITOR_SCOPE:
        return None
    if clean_origin(claim.get("aud")) != clean_origin(audience) or not audience:
        return None
    name = str(claim.get("sub") or "").strip()
    home = str(claim.get("home") or "").strip().lower()
    nonce = str(claim.get("nonce") or "")
    if not name or not HOST_RE.fullmatch(home) or not NONCE_RE.fullmatch(nonce):
        return None
    return {"name": name[:64], "home": home, "nonce": nonce,
            "expiresAt": expires}


def bearer_visitor_token(header):
    value = str(header or "").strip()
    if value[:7].lower() != "bearer ":
        return ""
    token = value[7:].strip()
    return token if token.startswith(VISITOR_PREFIX) else ""


FEDERATED_PATHS = frozenset({
    "/api/world/visa/session",
    "/api/world/mesh",
    "/api/world/neighbour-scene",
    "/api/world/organizations",
    "/api/world/events",
    "/api/world/office/floors",
    "/api/world/office/attendance",
    "/api/world/office/general/status",
    "/api/world/office/general/entry",
    "/api/repositories",
    "/api/leaderboards",
    "/api/accounts/users",
    "/api/world/inactive",
    "/api/world/visitors",
    "/api/world/fediverse",
    "/api/world/fediverse-mentions",
    "/api/network/overview",
})


FEDERATED_READ_ONLY_PATHS = frozenset({
    "/api/accounts/users",
    "/api/world/inactive",
    "/api/world/visitors",
    "/api/world/fediverse",
    "/api/world/fediverse-mentions",
    "/api/network/overview",
})


def federated_path(path):
    return str(path or "").rstrip("/") in FEDERATED_PATHS


def federated_method_allowed(path, method, requested=""):
    if str(path or "").rstrip("/") not in FEDERATED_READ_ONLY_PATHS:
        return True
    verb = str(method or "").upper()
    if verb == "OPTIONS":
        verb = str(requested or "").upper()
    return verb in ("GET", "HEAD")


def neighbour_world_origin(env, worlds, origin):
    origin = clean_origin(origin)
    if not origin:
        return None
    own = self_id(env, worlds)
    for entry in worlds:
        if entry["id"] != own and origin == entry["world"]:
            return entry
    return None


def federation_cors_headers(origin, preflight=False, read_only=False):
    headers = {
        "access-control-allow-origin": origin,
        "vary": "Origin",
    }
    if preflight:
        headers.update({
            "access-control-allow-methods":
                "GET, OPTIONS" if read_only else "GET, POST, OPTIONS",
            "access-control-allow-headers":
                "authorization, content-type, accept",
            "access-control-max-age": "600",
        })
    return headers
