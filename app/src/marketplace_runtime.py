"""marketplace runtime routes, loaded on demand.

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""

from hidden_unicode import clean_value as clean_hidden_unicode



def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class _MarketplaceGovernanceRuntime:
    """Narrow Worker adapter for marketplace publication governance."""

    def __init__(self, env, request):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request)

    def same_origin(self):
        return _request_origin_allowed(self.env, self.request)

    def now(self):
        return int(Date.now())

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data, status=status, cache_control=cache_control,
            extra_headers=extra_headers)

    def public_catalog_url(self):
        return (
            _public_base_url(self.env, self.request).rstrip("/")
            or "https://forkmesh.com"
        ) + "/api/market/catalog.json"

    async def ensure_schema(self):
        await ensure_schema(self.env)

    async def json_body(self, limit):
        try:
            announced = int(
                self.request.headers.get("content-length") or 0)
        except (AttributeError, TypeError, ValueError, OverflowError):
            return None, "invalid_content_length"
        if announced > int(limit):
            return None, "payload_too_large"
        try:
            raw = str(await self.request.text())
        except Exception:
            return None, "invalid_json"
        if len(raw.encode("utf-8")) > int(limit):
            return None, "payload_too_large"
        if not raw.strip():
            return {}, ""
        try:
            data = clean_hidden_unicode(json.loads(raw))
        except Exception:
            return None, "invalid_json"
        return (data, "") if isinstance(data, dict) else (None, "invalid_json")

    async def session(self, data=None):
        return await _account_session_record(
            self.env, self.request,
            data if isinstance(data, dict) else {})

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def fetch_manifest(self, url, max_bytes):
        try:
            response = await js_fetch_with_timeout(
                str(url), {
                    "method": "GET",
                    "headers": {"accept": "application/json"},
                    "redirect": "manual",
                }, 8)
        except Exception:
            return "", "manifest_fetch_failed"
        status = int(getattr(response, "status", 0) or 0)
        if 300 <= status < 400:
            return "", "manifest_redirect_rejected"
        if status != 200:
            return "", "manifest_fetch_failed"
        try:
            announced = int(response.headers.get("content-length") or 0)
        except (AttributeError, TypeError, ValueError, OverflowError):
            announced = 0
        if announced > int(max_bytes):
            return "", "manifest_too_large"
        try:
            raw = str(await response.text())
        except Exception:
            return "", "manifest_fetch_failed"
        if len(raw.encode("utf-8")) > int(max_bytes):
            return "", "manifest_too_large"
        return raw, ""

    async def account(self, account_bi):
        return await _account_identity_rec_by_bi(self.env, str(account_bi or ""))

    async def is_forkmesh_repo_owner(self, viewer_name):
        org_bi, org = await _org_row(self.env, "forkmesh")
        if not org or await _org_role(
                self.env, org_bi, str(viewer_name or "")) != "owner":
            return False
        linked = await d1_first(
            self.env,
            "SELECT 1 AS ok FROM org_repos WHERE org_bi=? AND repo=?",
            org_bi, "forkmesh")
        return bool(linked)

    async def audit(self, actor, action, submission_id, outcome):
        await _audit_sensitive_action(
            self.env, actor, action, "marketplace_submission",
            submission_id, outcome, {})


class _MarketplaceReviewsRuntime(_MarketplaceGovernanceRuntime):
    """Worker adapter for Market star ratings and improvement reviews.

    Adds the query string, blind indexes, the administrator flag, and the
    ordinary Pings rail (enqueue_notification: inbox row, account socket,
    phone banner, and the instant/digest email doorbell) to the governance
    adapter. It exposes no other account or organization mutation.
    """

    def query(self):
        try:
            values = parse_qs(urlparse(str(self.request.url)).query)
        except Exception:
            return {}
        return {key: (items[0] if items else "")
                for key, items in values.items()}

    async def blind_index(self, value):
        return await blind_index(self.env, str(value or ""))

    async def is_admin(self, name):
        return await _is_admin(self.env, str(name or ""))

    async def notify(self, recipient, title, body="", href="", actor="",
                     dedupe="", meta=None):
        return await enqueue_notification(
            self.env, recipient, "package_review", title, body=body,
            href=href, actor=actor, source="marketplace_review",
            dedupe=dedupe, meta=meta)

    async def audit(self, actor, action, review_id, outcome="success"):
        await _audit_sensitive_action(
            self.env, actor, action, "marketplace_review",
            review_id, outcome, {})
