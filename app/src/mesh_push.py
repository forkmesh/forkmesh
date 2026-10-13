import hashlib
import hmac
import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


MESH_WAKE_TOKEN_PREFIX = "mw1"
MESH_WAKE_TOKEN_PURPOSE = ":mesh-wake-token-v1"
MESH_WAKE_TOKEN_TTL_MS = 7 * 24 * 60 * 60 * 1000
MESH_WAKE_TOKEN_MAX_CHARS = 200
MESH_WAKE_MAX_TOKENS = 32
MESH_WAKE_MINT_PROOF = "forkmesh-mesh-wake-mint-v1"
MESH_WAKE_SEND_PROOF = "forkmesh-mesh-wake-v1"
MESH_WAKE_PATH_RE = re.compile(r"^/api/push/mesh/?$")
MESH_WAKE_MINT_PATH_RE = re.compile(r"^/api/push/mesh-wake/?$")
MESH_WAKE_CHANNEL = "mesh"
MESH_WAKE_COALESCE_MS = 5000
_MESH_WAKE_MEMO = {}
_MESH_WAKE_MEMO_MAX = 512


def _signature(env, canonical):
    return hmac.new(
        (_require_data_secret(env) + MESH_WAKE_TOKEN_PURPOSE).encode(),
        canonical.encode(),
        hashlib.sha256,
    ).hexdigest()


def mesh_wake_token(env, account_bi, now):
    expires = int(now) + MESH_WAKE_TOKEN_TTL_MS
    canonical = ".".join((MESH_WAKE_TOKEN_PREFIX, str(account_bi), str(expires)))
    return canonical + "." + _signature(env, canonical), expires


def mesh_wake_token_account(env, token, now):
    if not isinstance(token, str) or len(token) > MESH_WAKE_TOKEN_MAX_CHARS:
        return ""
    parts = token.split(".")
    if len(parts) != 4 or parts[0] != MESH_WAKE_TOKEN_PREFIX:
        return ""
    _, account_bi, expires_raw, signature = parts
    if not re.fullmatch(r"[0-9a-f]{64}", account_bi):
        return ""
    if not re.fullmatch(r"[0-9]{1,16}", expires_raw):
        return ""
    expires = int(expires_raw)
    if expires <= int(now) or expires - int(now) > MESH_WAKE_TOKEN_TTL_MS:
        return ""
    expected = _signature(env, ".".join(parts[:3]))
    if not hmac.compare_digest(signature, expected):
        return ""
    return account_bi


def mesh_wake_mint_canonical(user, ts):
    return "\n".join([MESH_WAKE_MINT_PROOF, user, str(ts)]).encode()


def mesh_wake_send_canonical(user, ts, tokens):
    return "\n".join([MESH_WAKE_SEND_PROOF, user] + list(tokens) + [str(ts)]).encode()


async def _signed_account(env, request, path_re, canonical_for):
    url = urlparse(request.url)
    if not path_re.match(url.path):
        return ""
    params = parse_qs(url.query)
    user = clean_string(params.get("user", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    if not valid_node_name(user) or not sig or not _ts_ok(ts):
        return ""
    if not await _verify_owner_signature(env, user, sig, canonical_for(user, ts)):
        return ""
    _, record = await _account_row(env, user)
    if not record or record.get("status") != "active":
        return ""
    return user


def _response(payload, status=200, allow=""):
    extra = {"allow": allow} if allow else None
    if extra:
        return json_response(payload, status=status, cache_control="no-store",
                             extra_headers=extra)
    return json_response(payload, status=status, cache_control="no-store")


async def _request_json(request):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return None
    return data if isinstance(data, dict) else None


async def mesh_wake_mint_handler(env, request):
    if method_name(request) != "POST":
        return _response({"error": "method_not_allowed"}, 405, "POST")
    data = await _request_json(request)
    if data is None:
        return _response({"error": "invalid_json"}, 400)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _signed_account(
            env, request, MESH_WAKE_MINT_PATH_RE, mesh_wake_mint_canonical)
    if not valid_node_name(account):
        return _response({"error": "unauthorized"}, 401)
    token, expires = mesh_wake_token(
        env, await blind_index(env, account), int(Date.now()))
    return _response({"token": token, "expiresAt": expires})


def _mesh_wake_tokens(data):
    raw = data.get("tokens")
    if not isinstance(raw, list) or len(raw) > MESH_WAKE_MAX_TOKENS:
        return None
    tokens = []
    for value in raw:
        if not isinstance(value, str) or len(value) > MESH_WAKE_TOKEN_MAX_CHARS:
            return None
        if value not in tokens:
            tokens.append(value)
    return tokens


def _mesh_wake_due(account_bi, now):
    last = _MESH_WAKE_MEMO.get(account_bi, 0)
    if last and now - last < MESH_WAKE_COALESCE_MS:
        return False
    if len(_MESH_WAKE_MEMO) >= _MESH_WAKE_MEMO_MAX:
        _MESH_WAKE_MEMO.clear()
    _MESH_WAKE_MEMO[account_bi] = now
    return True


async def _mesh_wake_devices(env, account_bi):
    apns = _apns_module()
    rows = await d1_all(
        env,
        "SELECT token_bi, environment, bundle_id, data "
        "FROM apns_device_tokens "
        "WHERE account_bi=? ORDER BY last_seen DESC LIMIT ?",
        account_bi,
        apns.APNS_MAX_DEVICES_PER_PUSH,
    )
    return await _apns_tokens_from_rows(env, rows)


async def mesh_wake_handler(env, request):
    if method_name(request) != "POST":
        return _response({"error": "method_not_allowed"}, 405, "POST")
    data = await _request_json(request)
    if data is None:
        return _response({"error": "invalid_json"}, 400)
    tokens = _mesh_wake_tokens(data)
    if tokens is None:
        return _response({"error": "bad_request"}, 400)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _signed_account(
            env, request, MESH_WAKE_PATH_RE,
            lambda user, ts: mesh_wake_send_canonical(user, ts, data["tokens"]))
    if not valid_node_name(account):
        return _response({"error": "unauthorized"}, 401)
    apns = _apns_module()
    if not apns.apns_configured(env):
        return _response({"ok": True, "woken": 0, "pushConfigured": False})
    now = int(Date.now())
    sender_bi = await blind_index(env, account)
    recipients = []
    for token in tokens:
        account_bi = mesh_wake_token_account(env, token, now)
        if account_bi and account_bi != sender_bi and account_bi not in recipients:
            recipients.append(account_bi)
    await ensure_schema(env)
    alert = apns.chat_message_alert(
        MESH_WAKE_CHANNEL, apns.APNS_MESH_CONVERSATION)
    woken = 0
    for account_bi in recipients:
        if not _mesh_wake_due(account_bi, now):
            continue
        try:
            devices = await _mesh_wake_devices(env, account_bi)
        except Exception:
            continue
        if not devices:
            continue
        if await _apns_send_tokens(env, devices, apns.APNS_CHAT_TOPIC, alert):
            woken += 1
    return _response({"ok": True, "woken": woken})
