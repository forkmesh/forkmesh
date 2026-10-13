import apps as _apps
import apps_api as _api
import apps_core as _core
import apps_events as _events

_GATED = {}
_GATE_TTL_MS = 30 * 1000


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


class _AppsMixin:
    def header(self, name):
        if self.request is None:
            return ""
        return str(self.request.headers.get(name) or "")

    def config(self, name):
        return str(getattr(self.env, name, "") or "")

    def new_hex(self, nbytes):
        return bytes(js_crypto.getRandomValues(  # noqa: F821
            Uint8Array.new(int(nbytes))).to_py()).hex()  # noqa: F821

    async def form_body(self, limit):
        content_type = self.header("content-type").lower()
        if "application/json" in content_type:
            return await self.json_body(limit)
        try:
            raw = await self.request.text()
        except Exception:
            return None, "invalid_body"
        if len(str(raw or "").encode("utf-8")) > int(limit):
            return None, "payload_too_large"
        pairs = parse_qsl(str(raw or ""), keep_blank_values=True)  # noqa: F821
        out = {}
        for key, value in pairs:
            if key in out:
                return None, "duplicate_parameter"
            out[key] = value
        return out, ""

    async def org_lookup(self, name):
        name = str(name or "").strip().lower()
        if not name:
            return ""
        org_bi, row = await _org_row(self.env, name)  # noqa: F821
        return org_bi if row else ""

    async def org_role(self, org_bi, account):
        if not org_bi or not account:
            return ""
        return await _org_role(self.env, org_bi, account)  # noqa: F821

    async def orgs_for(self, account):
        member_bi = await blind_index(self.env, account)  # noqa: F821
        rows = await d1_all(  # noqa: F821
            self.env,
            "SELECT o.name AS name, m.role AS role FROM org_members m "
            "JOIN orgs o ON o.org_bi=m.org_bi WHERE m.member_bi=? "
            "ORDER BY o.name LIMIT 50", member_bi)
        return [(str(row.get("name") or "").lower(),
                 str(row.get("role") or "")) for row in rows or []]

    async def org_admins(self, org_bi):
        rows = await d1_all(  # noqa: F821
            self.env,
            "SELECT name FROM org_members WHERE org_bi=? "
            "AND role IN ('owner','admin') ORDER BY created_at LIMIT 20",
            org_bi)
        return [str(row.get("name") or "").lower() for row in rows or []]

    async def notify(self, recipient, title, body="", href="", dedupe=""):
        try:
            await enqueue_notification(  # noqa: F821
                self.env, recipient, "plugin_activity", title, body=body,
                href=href, source="apps", dedupe=dedupe)
        except Exception:
            pass

    async def account_owns(self, actor, owner):
        return await _account_owns_node(self.env, actor, owner)  # noqa: F821

    async def repo_accounts(self, owner):
        owner = str(owner or "").strip().lower()
        _bi, rec = await _account_row(self.env, owner)  # noqa: F821
        if not rec:
            return []
        accounts = [owner] if _account_kind(rec) == "user" else []  # noqa: F821
        linked = str(rec.get("owner") or "").strip().lower()
        if linked and linked not in accounts:
            accounts.append(linked)
        return accounts

    async def chat_channel(self, channel_id):
        row = await d1_first(  # noqa: F821
            self.env,
            "SELECT data, key_version FROM chat_channels WHERE channel_id=?",
            channel_id)
        if not row:
            return None
        record = await decrypt_row(self.env, row.get("data"))  # noqa: F821
        if not isinstance(record, dict):
            return None
        return {
            "public": chat_channels_api._visibility(record) == "public",  # noqa: F821
            "name": str(record.get("name") or "channel"),
            "key_version": int(row.get("key_version") or 1),
        }

    async def chat_post(self, channel_id, key_version, plain):
        await self.deliver_webhook(channel_id, key_version, plain)

    async def _inbox_key(self, owner, repo):
        env = self.env
        await ensure_schema(env)  # noqa: F821
        host_owner = await _ap_org_alias_owner(env, owner, repo)  # noqa: F821
        repo_bi = await blind_index(env, host_owner + "/" + repo)  # noqa: F821
        row = await d1_first(  # noqa: F821
            env, "SELECT key_bi FROM repositories WHERE key_bi=?", repo_bi)
        if not row:
            return host_owner, "", "repository_not_found"
        await _forkbot_rekey_alias_inbox(  # noqa: F821
            env, owner, host_owner, repo, repo_bi)
        return host_owner, repo_bi, ""

    async def _inbox_room(self, table, limit, repo_bi, author):
        env = self.env
        count = await d1_first(  # noqa: F821
            env, "SELECT COUNT(*) AS c FROM %s WHERE repo_bi=?" % table,
            repo_bi)
        if count and int(count.get("c", 0) or 0) >= limit:
            return "", "inbox_full"
        submitter_bi = await blind_index(env, author)  # noqa: F821
        if await _inbox_author_over_quota(  # noqa: F821
                env, table, repo_bi, submitter_bi):
            return "", "author_quota"
        return submitter_bi, ""

    async def issue_open(self, owner, repo, title, body, author,
                         author_name, slug):
        env = self.env
        host_owner, repo_bi, error = await self._inbox_key(owner, repo)
        if error:
            return False, error
        submitter_bi, error = await self._inbox_room(
            "issue_inbox", MAX_PENDING_ISSUES, repo_bi, author)  # noqa: F821
        if error:
            return False, error
        now = int(Date.now())  # noqa: F821
        number = await _forkbot_next_issue_number(  # noqa: F821
            env, repo_bi, owner, repo)
        event = {"type": "open", "id": "app-%s-%d" % (slug, now),
                 "author": author, "authorName": author_name, "ts": now,
                 "title": title, "body": body, "attachments": [], "sig": ""}
        item = {
            "number": number, "titleIfNew": title, "event": event,
            "meta": {"labels": ["app"], "milestone": "", "priority": 0,
                     "assignees": [], "reporter": "", "wantsAgent": False,
                     "model": "", "provider": ""},
            "submitter": author, "submittedAt": now, "source": "app",
            "requestedBy": author, "issueNumber": number,
        }
        await d1_run(  # noqa: F821
            env, "INSERT INTO issue_inbox (repo_bi, data, submitter_bi) "
            "VALUES (?,?,?)", repo_bi, await encrypt_row(env, item),  # noqa: F821
            submitter_bi)
        await _best_effort_inbox_side_effect(notify_pending_inbox(  # noqa: F821
            env, host_owner, repo, "issue", author, title, number,
            public_owner=owner, opened=True))
        await _best_effort_inbox_side_effect(notify_mentions(  # noqa: F821
            env, owner, repo, author, title, body,
            repo_web_href(owner, repo), "issue", number=number))  # noqa: F821
        await notify_repo_host(env, host_owner, repo, "issues")  # noqa: F821
        await emit_repo_event(env, owner, repo, "issue.opened", {
            "number": number, "title": title, "author": author,
            "authorName": author_name, "body": body[:2000]},
            source_actor=author)
        return True, number

    async def issue_comment(self, owner, repo, number, body, author,
                            author_name, wants_agent=False):
        env = self.env
        host_owner, repo_bi, error = await self._inbox_key(owner, repo)
        if error:
            return False, error
        submitter_bi, error = await self._inbox_room(
            "issue_inbox", MAX_PENDING_ISSUES, repo_bi, author)  # noqa: F821
        if error:
            return False, error
        now = int(Date.now())  # noqa: F821
        event = {"type": "comment", "id": "app-%d" % now, "author": author,
                 "authorName": author_name, "ts": now, "body": body,
                 "attachments": [], "sig": ""}
        item = {
            "number": int(number), "titleIfNew": "", "event": event,
            "meta": {"labels": [], "milestone": "", "priority": 0,
                     "assignees": [], "wantsAgent": bool(wants_agent),
                     "model": "", "provider": ""},
            "submitter": author, "submittedAt": now, "source": "app",
            "requestedBy": author,
        }
        await d1_run(  # noqa: F821
            env, "INSERT INTO issue_inbox (repo_bi, data, submitter_bi) "
            "VALUES (?,?,?)", repo_bi, await encrypt_row(env, item),  # noqa: F821
            submitter_bi)
        await _best_effort_inbox_side_effect(notify_pending_inbox(  # noqa: F821
            env, host_owner, repo, "issue", author, "", int(number),
            public_owner=owner, text=body))
        await notify_repo_host(env, host_owner, repo, "issues")  # noqa: F821
        await emit_repo_event(env, owner, repo, "issue.comment", {
            "number": int(number), "author": author,
            "authorName": author_name, "body": body[:2000]},
            source_actor=author)
        return True, int(number)

    async def pull_comment(self, owner, repo, number, body, author,
                           author_name):
        env = self.env
        host_owner, repo_bi, error = await self._inbox_key(owner, repo)
        if error:
            return False, error
        submitter_bi, error = await self._inbox_room(
            "pull_inbox", MAX_PENDING_PULLS, repo_bi, author)  # noqa: F821
        if error:
            return False, error
        now = int(Date.now())  # noqa: F821
        event = {"type": "comment", "id": "app-%d" % now, "author": author,
                 "authorName": author_name, "ts": now, "body": body,
                 "sig": ""}
        item = {"number": int(number), "event": event, "submitter": author,
                "submittedAt": now, "source": "app"}
        await d1_run(  # noqa: F821
            env, "INSERT INTO pull_inbox (repo_bi, data, submitter_bi) "
            "VALUES (?,?,?)", repo_bi, await encrypt_row(env, item),  # noqa: F821
            submitter_bi)
        await _best_effort_inbox_side_effect(notify_pending_inbox(  # noqa: F821
            env, host_owner, repo, "pull", author, "", int(number),
            text=body))
        await notify_repo_host(env, host_owner, repo, "pulls")  # noqa: F821
        await emit_repo_event(env, owner, repo, "pull.comment", {
            "number": int(number), "author": author,
            "authorName": author_name, "body": body[:2000]},
            source_actor=author)
        return True, int(number)

    async def http_post(self, url, headers, body, timeout):
        try:
            response = await js_fetch_with_timeout(  # noqa: F821
                url, {"method": "POST", "headers": headers, "body": body,
                      "redirect": "manual"}, timeout)
        except Exception:
            return 0, ""
        if response is None:
            return 0, ""
        status = int(getattr(response, "status", 0) or 0)
        try:
            text = str(await response.text() or "")[:16384]
        except Exception:
            text = ""
        return status, text


_RUNTIME_CLASS = []


def _AppsRuntime(env, request):
    if not _RUNTIME_CLASS:
        _RUNTIME_CLASS.append(type(
            "_AppsRuntime", (_AppsMixin, _ChatChannelsRuntime), {}))  # noqa: F821
    return _RUNTIME_CLASS[0](env, request)


async def apps_handler(env, request, path):
    return await _apps.handle(_AppsRuntime(env, request), path)


async def oauth_token_handler(env, request):
    runtime = _AppsRuntime(env, request)
    await runtime.ensure_schema()
    if runtime.method() != "POST":
        return runtime.response({"error": "invalid_request"}, status=405)
    return await _apps.oauth_token(runtime)


async def oauth_revoke_handler(env, request):
    runtime = _AppsRuntime(env, request)
    await runtime.ensure_schema()
    if runtime.method() != "POST":
        return runtime.response({"error": "invalid_request"}, status=405)
    return await _apps.oauth_revoke(runtime)


def token_from_request(request):
    header = str(request.headers.get("authorization") or "").strip()
    if header.lower().startswith("bearer "):
        token = header[7:].strip()
        if token.startswith(_core.ACCESS_PREFIX):
            return token
    return ""


async def rest_gate(env, request, path):
    token = token_from_request(request)
    if not token or str(path or "").startswith("/api/apps/"):
        return None
    runtime = _AppsRuntime(env, request)
    await runtime.ensure_schema()
    method = runtime.method()
    context, denied = await _api.gate(
        runtime, token, method, path, runtime.query_params())
    if denied is not None:
        return denied
    now = int(Date.now())  # noqa: F821
    for key in [key for key, value in _GATED.items()
                if value[2] <= now][:64]:
        _GATED.pop(key, None)
    _GATED[token] = (context.get("identity") or "", str(path), now
                     + _GATE_TTL_MS)
    return None


async def gated_session(env, request, token):
    entry = _GATED.get(token)
    if entry is None:
        return "", None
    identity, path, expires = entry
    try:
        current = str(urlparse(request.url).path or "")  # noqa: F821
    except Exception:
        return "", None
    if not identity or expires <= int(Date.now()) or current != path or \
            method_name(request) not in ("GET", "HEAD"):  # noqa: F821
        return "", None
    row = await d1_first(  # noqa: F821
        env, "SELECT data FROM users WHERE user_bi=?", identity)
    rec = await decrypt_row(env, (row or {}).get("data", ""))  # noqa: F821
    if not rec or rec.get("status") != "active" or \
            _account_kind(rec) != "user":  # noqa: F821
        return "", None
    return identity, rec


async def emit_repo_event(env, owner, repo, event_type, data,
                          source_actor=""):
    mirror = globals().get("provider_mirror_note_event")
    if mirror is not None:
        await mirror(env, owner, repo, event_type, data)
    try:
        runtime = _AppsRuntime(env, None)
        await runtime.ensure_schema()
        return await _events.emit_repo_event(
            runtime, owner, repo, event_type, data, source_actor)
    except Exception:
        return 0


async def emit_ping(env, recipient, title, href, source=""):
    if source == "apps":
        return 0
    try:
        return await _events.emit_ping(_AppsRuntime(env, None), recipient,
                                       title, href)
    except Exception:
        return 0


async def drain_events(env, limit):
    runtime = _AppsRuntime(env, None)
    await runtime.ensure_schema()
    return await _events.drain(runtime, limit)
