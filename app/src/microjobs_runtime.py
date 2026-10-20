"""microjobs runtime routes, loaded on demand.

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


class _MicrojobsRuntime:
    """Least-privilege adapter for paid microjobs.

    Deliberately absent: any wallet, seed, vault, or signing primitive; any
    platform-admin authorization; and any repository mutation other than
    handing one already-signed issue event to the ordinary issue inbox. The
    verification helpers it does expose (Workers AI, the public repository
    gateway, one bounded outbound page fetch) are all read-only, and the
    payout path can only author the same public unsigned plan the community
    reward pool already authors.
    """

    def __init__(self, env, request=None):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request) if self.request is not None else "GET"

    def same_origin(self):
        return bool(
            self.request is not None
            and _request_origin_allowed(self.env, self.request)
        )

    def now(self):
        return int(Date.now())

    def new_id(self):
        # Carries no account, wallet, time, or repository information, so it is
        # safe as the public claim identifier.
        return _ap_uuid()

    def public_origin(self):
        return (
            _public_base_url(self.env, self.request).rstrip("/")
            or "https://forkmesh.com"
        )

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return json_response(
            data, status=status, cache_control=cache_control,
            extra_headers=extra_headers)

    def encode_plan(self, plan):
        return json.dumps(plan, sort_keys=True, separators=(",", ":"))

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

    async def blind(self, value):
        return await blind_index(self.env, str(value or ""))

    async def seal(self, value):
        return await encrypt_row(self.env, value)

    async def open(self, value):
        return await decrypt_row(self.env, value)

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)

    async def batch(self, statements):
        return await _contribution_run_batch(self.env, statements)

    async def audit(self, actor, action, resource_type, resource_id, details):
        await _audit_sensitive_action(
            self.env, actor, action, resource_type, resource_id, "recorded",
            details)

    async def payout_wallet(self, account_bi):
        """The claimant's own public self-custodial payout address, or "".

        Read from the account record the settings page writes. ForkMesh never
        creates a wallet, so an account without one simply stays approved and
        unpaid until its owner supplies an address.
        """
        if not account_bi:
            return ""
        row = await d1_first(
            self.env, "SELECT data FROM users WHERE user_bi=?",
            str(account_bi))
        record = await decrypt_row(self.env, (row or {}).get("data"))
        wallet = clean_string((record or {}).get("solana") or "", 80).strip()
        return wallet if SOLANA_RE.match(wallet) else ""

    async def payout_source(self):
        """Public pool address/network/signer, or empty when unconfigured."""
        record = await _central_fund_record(self.env)
        if not record:
            return {}
        return {
            "address": record.get("address", ""),
            "network": record.get("network", ""),
            "signerAccount": record.get("signer_account", ""),
        }

    async def repository_is_public(self, owner, repo):
        _repo_bi, row = await _security_scan_repository_row(
            self.env, owner, repo)
        if not row:
            return False
        return not await _repo_is_private(self.env, owner, repo)

    async def account_controls_repository(self, account, owner, repo):
        account = clean_string(account or "", MAX_NODE_NAME).strip().lower()
        if not account:
            return True
        names = {clean_string(owner or "", MAX_NODE_NAME).strip().lower()}
        names.add(clean_string(
            await _ap_org_alias_owner(self.env, owner, repo) or "",
            MAX_NODE_NAME).strip().lower())
        for name in names - {""}:
            if name == account or await _account_owns_node(
                    self.env, account, name):
                return True
            org_bi, org = await _org_row(self.env, name)
            if org and await _org_role(self.env, org_bi, account):
                return True
        return False

    async def recent_issue_numbers(self, owner, repo):
        return await _forkbot_recent_issue_numbers(self.env, owner, repo)

    async def load_issue_records(self, owner, repo, numbers):
        return await _forkbot_load_issue_records(
            self.env, owner, repo, numbers)

    async def search_issues(self, owner, repo, term):
        return await _forkbot_search_issues(self.env, owner, repo, term)

    async def search_code(self, owner, repo, term):
        """Fixed-string repository grep, so the reviewer can see whether the
        thing being reported is already handled in the tree."""
        data = await _forkbot_repo_gateway_json(
            self.env, owner, repo,
            "search?path=" + quote(str(term or "")[:120], safe=""))
        hits = (data or {}).get("code")
        return hits if isinstance(hits, list) else []

    async def read_pull_record(self, owner, repo, number):
        """One pull's ``pull.md``, or None when no host could serve it.

        Read with no ``ref``: the pull ledger lives on its own metadata branch
        and asking for a ref returns the stale copy from the default branch.
        """
        data = await _forkbot_repo_gateway_json(
            self.env, owner, repo,
            "blob?path=" + quote(
                ".forkmesh/pulls/%d/pull.md" % int(number), safe=""))
        if data is None:
            return None
        return _forkbot_blob_text(data)

    async def verify_node_signature(self, public_key, signature, canonical):
        if not valid_node_pubkey(str(public_key or "")):
            return False
        return bool(await ed25519_verify(
            public_key, signature, str(canonical or "").encode("utf-8")))

    async def account_owns_node_key(self, account_bi, public_key):
        """True when the signing node (or account key) belongs to this account.

        Both shapes count: a registered node row linked to the account, and the
        account's own identity key, which is what a single-machine contributor
        signs with.
        """
        account_bi = str(account_bi or "")
        if not account_bi or not valid_node_pubkey(str(public_key or "")):
            return False
        node = await d1_first(
            self.env,
            "SELECT node_bi,user_bi FROM nodes WHERE pubkey=?", public_key)
        if node and str(node.get("user_bi") or "") == account_bi:
            return True
        if node and str(node.get("node_bi") or "") == account_bi:
            return True
        row = await d1_first(
            self.env, "SELECT data FROM users WHERE user_bi=?", account_bi)
        record = await decrypt_row(self.env, (row or {}).get("data"))
        return str((record or {}).get("pubkey") or "") == str(public_key)

    async def review(self, system_prompt, user_prompt, schema):
        return await _forkbot_run_ai(
            self.env, system_prompt, user_prompt, schema=schema,
            max_tokens=400)

    async def read_public_page(self, url, max_bytes):
        """One bounded GET of a public page, as text. None on any failure.

        The URL has already been reduced to an ordinary public HTTPS origin
        with no credentials, no bare IP literal, no non-443 port, and no
        internal/loopback name by ``microjobs.normalize_public_url``.
        Redirects are rejected. Otherwise a safe public URL could redirect the
        edge fetch toward a private or link-local destination after validation.
        """
        try:
            response = await js_fetch_with_timeout(
                str(url), {"method": "GET", "redirect": "manual"}, 8)
            if int(getattr(response, "status", 0) or 0) != 200:
                return None
            text = await response.text()
        except Exception:
            return None
        return str(text or "")[:int(max_bytes)]

    async def file_issue(self, owner, repo, submission):
        """Hand one already-signed ``open`` event to the ordinary issue inbox.

        The same signature check, inbox bounds, and host/mirror notifications
        the public issues endpoint applies. It intentionally cannot carry
        owner-only issue state (status, labels, assignment, agent dispatch):
        a microjob report opens a plain issue and nothing more.
        """
        event = submission.get("event")
        if not isinstance(event, dict):
            return False
        title = clean_string(submission.get("titleIfNew", ""), 240)
        if len((event.get("body", "") or "").encode("utf-8")) > MAX_ISSUE_BYTES:
            return False
        if not await verify_issue_event(
                0, event,
                _event_signing_repo_keys(self.request, owner, repo)):
            return False
        if await _bind_session_author(
                self.env, self.request, None, event,
                session=str(submission.get("account") or "")) is not None:
            return False
        try:
            repo_bi = await _inbox_repo_key(self.env, self.request, owner, repo)
        except _OrgAliasUnresolved:
            return False
        count = await d1_first(
            self.env,
            "SELECT COUNT(*) AS c FROM issue_inbox WHERE repo_bi=?", repo_bi)
        if count and count.get("c", 0) >= MAX_PENDING_ISSUES:
            return False
        submitter_bi = await blind_index(self.env, event.get("author", ""))
        if await _inbox_author_over_quota(
                self.env, "issue_inbox", repo_bi, submitter_bi):
            return False
        number = await _forkbot_next_issue_number(
            self.env, repo_bi, owner, repo, "issue")
        if number <= 0:
            return False
        item = {
            "number": number,
            # The browser/node signature was checked against the unnumbered
            # open event. The relay-owned reservation is attached only after
            # verification, matching the ordinary web issue endpoint.
            "signedNumber": 0,
            "titleIfNew": title,
            "event": event,
            "meta": {},
            "attachmentData": [],
            "submitter": clean_string(event.get("author", ""), 120),
            "submittedAt": int(Date.now()),
        }
        await d1_run(
            self.env,
            "INSERT INTO issue_inbox (repo_bi, data, submitter_bi) "
            "VALUES (?,?,?)",
            repo_bi, await encrypt_row(self.env, item), submitter_bi,
        )
        await _record_contributor(
            self.env, event.get("author", ""), "issues",
            event.get("authorName", ""))
        actor = clean_string(
            event.get("authorName", "") or event.get("author", ""),
            MAX_NODE_NAME).lower()
        await _best_effort_inbox_side_effect(
            notify_pending_inbox(
                self.env, owner, repo, "issue", actor, title, number,
                opened=True))
        await notify_repo_host(self.env, owner, repo, "issues")
        await notify_repo_mirrors(self.env, owner, repo, "issues")
        return number
