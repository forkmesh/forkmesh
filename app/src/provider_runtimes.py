"""provider runtimes routes, loaded on demand.

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class _OrganizationDiscordRuntime:
    """Narrow adapter for the organization Discord policy module."""

    def __init__(self, env, request):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request)

    def now(self):
        return int(Date.now())

    def new_id(self):
        return _ap_uuid()

    def query(self, name):
        try:
            values = parse_qs(
                urlparse(str(self.request.url)).query,
                keep_blank_values=True,
            )
            first = values.get(str(name), [""])[0]
            return str(first or "")
        except Exception:
            return ""

    def same_origin(self):
        return _request_origin_allowed(self.env, self.request)

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data,
            status=status,
            cache_control=cache_control,
            extra_headers=extra_headers,
        )

    async def ensure_schema(self):
        await ensure_schema(self.env)

    async def json_body(self, limit):
        try:
            raw_length = self.request.headers.get("content-length") or ""
            if raw_length and int(raw_length) > int(limit):
                return None, "payload_too_large"
        except (TypeError, ValueError):
            return None, "invalid_content_length"
        try:
            raw = await self.request.text()
        except Exception:
            return None, "invalid_json"
        if len(str(raw or "").encode("utf-8")) > int(limit):
            return None, "payload_too_large"
        if not str(raw or "").strip():
            return {}, ""
        try:
            data = json.loads(raw)
        except Exception:
            return None, "invalid_json"
        return (data, "") if isinstance(data, dict) else (None, "invalid_json")

    async def session(self, data=None):
        return await _account_session_record(
            self.env, self.request, data if isinstance(data, dict) else {})

    async def session_id(self):
        """Return only the currently validated session's opaque id."""

        try:
            token, _cookie_auth = _request_account_session_token(
                self.request, {})
            _account_bi, _record, session_id = await _account_session_lookup(
                self.env, token)
            return str(session_id or "")
        except Exception:
            return ""

    async def session_active(self, account_bi, session_id):
        row = await d1_first(
            self.env,
            "SELECT session_id FROM account_sessions WHERE account_bi=? "
            "AND session_id=? AND revoked_at=0 AND expires_at>?",
            str(account_bi or ""), str(session_id or ""), int(Date.now()),
        )
        return bool(row)

    async def organization(self, org):
        return await _org_row(self.env, org)

    async def org_role(self, org_bi, account):
        return await _org_role(self.env, org_bi, account)

    async def seal(self, value):
        return await encrypt_row(self.env, value)

    async def open(self, value):
        return await decrypt_row(self.env, value)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def audit(self, actor, action, target_type="", target="",
                    outcome="success", details=None):
        await _audit_sensitive_action(
            self.env, actor, action, target_type, target, outcome, details)

    def discord_ready(self):
        return bool(_discord_bot_token(self.env))

    def discord_oauth_ready(self):
        return bool(_discord_oauth_config(self.env))

    def discord_oauth_redirect_uri(self):
        config = _discord_oauth_config(self.env) or {}
        return str(config.get("redirectUri") or "")

    def discord_oauth_authorization_url(self, state, challenge):
        config = _discord_oauth_config(self.env) or {}
        state = str(state or "")
        challenge = str(challenge or "")
        if (
            not config
            or not _DISCORD_OAUTH_TRANSACTION_RE.fullmatch(state)
            or not challenge
        ):
            return ""
        return DISCORD_OAUTH_AUTHORIZE_ORIGIN + "?" + urlencode({
            "client_id": config["clientId"],
            "redirect_uri": config["redirectUri"],
            "response_type": "code",
            # Keep the user authorization-code grant separate from Discord's
            # callback-less bot-install shortcut. Combining `bot` here causes
            # some Discord clients to return an install result without the
            # code/state required to verify the organization owner.
            "scope": "identify guilds",
            "state": state,
            "code_challenge": challenge,
            "code_challenge_method": "S256",
            "prompt": "consent",
        })

    def oauth_transaction_cookie(self):
        return _cookie_value(self.request, DISCORD_OAUTH_TRANSACTION_COOKIE)

    def oauth_start_response(self, data, transaction):
        cookie = _discord_oauth_transaction_cookie(transaction)
        if not cookie:
            return self.response({"error": "oauth_transaction_required"}, 500)
        return self.response(
            data, cache_control="no-store, max-age=0, must-revalidate",
            extra_headers={
                "set-cookie": cookie,
                "referrer-policy": "no-referrer",
                "x-content-type-options": "nosniff",
            })

    def oauth_callback_response(self, outcome):
        outcome = str(outcome or "invalid")
        if outcome not in {
            "connected", "denied", "failed", "invalid", "setup",
            "invalid_state_format", "invalid_state_missing",
            "invalid_state_expired", "invalid_state_claim",
            "invalid_record", "invalid_record_storage",
            "invalid_record_decrypt", "invalid_record_state",
            "invalid_record_verifier", "invalid_record_guild",
            "invalid_context", "invalid_code",
        }:
            outcome = "invalid"
        # Strip OAuth code/state/error from the browser address before any
        # World UI loads. The one-time callback cookie is cleared regardless of
        # outcome, and the response is non-cacheable/non-referring.
        return Response("", status=303, headers={
            "location": "/world/?discord=" + outcome,
            "cache-control": "no-store, max-age=0, must-revalidate",
            "referrer-policy": "no-referrer",
            "x-content-type-options": "nosniff",
            "set-cookie": _clear_discord_oauth_transaction_cookie(),
        })

    async def discord_guild(self, guild_id):
        guild_id = _discord_snowflake(guild_id)
        if not guild_id:
            return {"status": 400, "data": None, "retryAfterMs": 0}
        result = await _discord_api_request(
            self.env, "GET", "/guilds/" + guild_id + "?with_counts=true")
        if int(result.get("status") or 0) != 200:
            return result
        guild = result.get("data") if isinstance(result.get("data"), dict) else {}
        return {
            "status": 200,
            "data": {key: guild.get(key) for key in (
                "id", "name", "description", "vanity_url_code",
                "approximate_member_count", "approximate_presence_count")},
            "retryAfterMs": 0,
        }

    async def discord_channels(self, guild_id):
        guild_id = _discord_snowflake(guild_id)
        if not guild_id:
            return {"status": 400, "data": None, "retryAfterMs": 0}
        return await _discord_api_request(
            self.env, "GET", "/guilds/" + guild_id + "/channels")

    def discord_public_channels_cache(self, guild_id):
        guild_id = _discord_snowflake(guild_id)
        if not guild_id:
            return None
        return _discord_rate_coordinator().catalog_get(
            "public-channels:" + guild_id, int(Date.now()))

    def cache_discord_public_channels(self, guild_id, channels):
        guild_id = _discord_snowflake(guild_id)
        if not guild_id or not isinstance(channels, list):
            return
        # The policy module has already removed private/NSFW/thread metadata;
        # cache only its small id/name/type public projection.
        _discord_rate_coordinator().catalog_put(
            "public-channels:" + guild_id, channels, int(Date.now()))

    async def discord_guild_roles(self, guild_id):
        guild_id = _discord_snowflake(guild_id)
        if not guild_id:
            return {"status": 400, "data": None, "retryAfterMs": 0}
        result = await _discord_api_request(
            self.env, "GET", "/guilds/" + guild_id + "/roles")
        if int(result.get("status") or 0) != 200:
            return result
        roles = result.get("data") if isinstance(result.get("data"), list) else []
        # Only the @everyone base role participates in ForkMesh's public
        # channel calculation. Discard every other role before short caching.
        projected = []
        for role in roles:
            if not isinstance(role, dict) or _discord_snowflake(role.get("id")) != guild_id:
                continue
            projected.append({
                "id": guild_id,
                "permissions": str(role.get("permissions") or "0")[:40],
            })
            break
        return {
            "status": 200,
            "data": projected,
            "retryAfterMs": 0,
        }

    async def discord_oauth_exchange(self, code, verifier):
        config = _discord_oauth_config(self.env) or {}
        if not config:
            return {"status": 0, "accessToken": ""}
        result = await _discord_oauth_request(
            self.env, "POST", _DISCORD_OAUTH_TOKEN_PATH, {
                "client_id": config["clientId"],
                "client_secret": config["clientSecret"],
                "grant_type": "authorization_code",
                "code": str(code or ""),
                "redirect_uri": config["redirectUri"],
                "code_verifier": str(verifier or ""),
            })
        data = result.get("data") if isinstance(result, dict) else None
        access_token = str((data or {}).get("access_token") or "")
        # Do not return provider response fields such as refresh_token,
        # scope, or token_type beyond this transient call frame.
        return {
            "status": int((result or {}).get("status") or 0),
            "accessToken": access_token[:4096],
        }

    async def discord_oauth_identity(self, access_token):
        return await _discord_oauth_request(
            self.env, "GET", _DISCORD_OAUTH_IDENTITY_PATH,
            access_token=access_token)

    async def discord_oauth_guilds(self, access_token):
        return await _discord_oauth_request(
            self.env, "GET", _DISCORD_OAUTH_GUILDS_PATH,
            access_token=access_token)

    async def discord_messages(self, channel_id, limit):
        channel_id = _discord_snowflake(channel_id)
        try:
            limit = max(1, min(50, int(limit)))
        except (TypeError, ValueError, OverflowError):
            limit = 50
        if not channel_id:
            return {"status": 400, "data": None, "retryAfterMs": 0}
        return await _discord_api_request(
            self.env, "GET", "/channels/" + channel_id
            + "/messages?limit=" + str(limit))

    async def discord_send(self, channel_id, content):
        channel_id = _discord_snowflake(channel_id)
        if not channel_id:
            return {"status": 400, "data": None, "retryAfterMs": 0}
        # Explicitly disable all Discord mention parsing.  A ForkMesh owner can
        # choose where a text message goes, never who it pings.
        return await _discord_api_request(
            self.env,
            "POST",
            "/channels/" + channel_id + "/messages",
            {
                "content": str(content or ""),
                "allowed_mentions": {"parse": []},
            },
        )
class _AccountPlanRuntime:
    """Narrow session + D1 adapter for the account plan surfaces.

    Nothing here talks to a payment provider: plan_services reads the caller's
    session, the plan registers and the role grants through it.
    """

    def __init__(self, env, request):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request)

    def now(self):
        return int(Date.now())

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data,
            status=status,
            cache_control=cache_control,
            extra_headers=extra_headers,
        )

    async def ensure_schema(self):
        await ensure_schema(self.env)

    async def json_body(self, limit):
        raw, error = await self.raw_body(limit)
        if error:
            return None, error
        if not str(raw or "").strip():
            return {}, ""
        try:
            data = json.loads(raw)
        except Exception:
            return None, "invalid_json"
        return (data, "") if isinstance(data, dict) else (None, "invalid_json")

    async def session(self, data=None):
        return await _account_session_record(
            self.env, self.request,
            data if isinstance(data, dict) else {},
        )

    async def blind(self, value):
        return await blind_index(self.env, str(value or ""))

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def batch(self, statements):
        return await _contribution_run_batch(self.env, statements)
