def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class _ProgramRuntime:
    def __init__(self, env, request=None):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request) if self.request is not None else "GET"

    def same_origin(self):
        return bool(
            self.request is not None
            and _request_origin_allowed(self.env, self.request))

    def now(self):
        return int(Date.now())

    def new_id(self):
        return _ap_uuid()

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data, status=status, cache_control=cache_control,
            extra_headers=extra_headers)

    async def ensure_schema(self):
        await ensure_schema(self.env)

    async def json_body(self, limit):
        if self.request is None:
            return None, "invalid_json"
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

    async def is_admin(self, name):
        return await _is_admin(self.env, name)

    async def blind(self, value):
        return await blind_index(self.env, str(value or ""))

    async def seal(self, value):
        return await encrypt_row(self.env, value)

    async def open(self, value):
        try:
            return await decrypt_row(self.env, value)
        except Exception:
            return None

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def audit(self, actor, action, resource_type, resource_id, details):
        await _audit_sensitive_action(
            self.env, actor, action, resource_type, resource_id, "recorded",
            details)


class _AffiliatesRuntime(_ProgramRuntime):
    async def deliver_lead(self, lead):
        org = str(getattr(self.env, "AFFILIATE_PIPELINE_ORG", None) or "").strip()
        org = "" if org in ("undefined", "null") else org[:64].lower()
        try:
            return await plugin_data.deliver_lead(
                _PluginApiRuntime(self.env, None), org or "forkmesh", lead)
        except Exception as error:
            await log_error(self.env, 500, "POST", "/api/affiliates/leads",
                            "Affiliate lead did not reach the sales pipeline: "
                            + str(error)[:300])
            return {"ok": False, "status": "delivery_failed"}


class _PluginRaceRuntime(_ProgramRuntime):
    async def account_record(self, account_bi):
        row = await d1_first(
            self.env, "SELECT data FROM users WHERE user_bi=?", str(account_bi))
        return await self.open((row or {}).get("data"))

    async def member_flags(self, account_bi, record):
        return await _account_users._referral_qualification_flags(
            self.env, str(account_bi), record or {}, int(Date.now()))

    async def discord_linked(self, account_bi):
        row = await d1_first(
            self.env, "SELECT discord_bi FROM users WHERE user_bi=?",
            str(account_bi))
        return bool(str((row or {}).get("discord_bi") or "").strip())

    async def publishes_package(self, account_bi, market, package, since):
        row = await d1_first(
            self.env,
            "SELECT 1 AS ok FROM marketplace_submissions WHERE marketplace_id=? "
            "AND package_id=? AND state='published' AND submitted_by_bi=? "
            "AND published_at>=? LIMIT 1", market, package, str(account_bi),
            int(since))
        return bool(row)

    async def payout_wallet(self, account_bi):
        if not account_bi:
            return ""
        record = await self.account_record(account_bi)
        wallet = clean_string((record or {}).get("solana") or "", 80).strip()
        return wallet if SOLANA_RE.match(wallet) else ""

    async def read_public_page(self, url, max_bytes):
        try:
            response = await js_fetch_with_timeout(
                str(url), {"method": "GET", "redirect": "manual"}, 8)
            if int(getattr(response, "status", 0) or 0) != 200:
                return None
            text = await response.text()
        except Exception:
            return None
        return str(text or "")[:int(max_bytes)]
