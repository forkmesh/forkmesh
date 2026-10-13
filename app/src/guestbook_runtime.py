"""guestbook runtime routes, loaded on demand.

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


class _AgentGuestbookRuntime:
    """Bridge the pure guestbook contract to D1 and public World #general."""

    def __init__(self, env, request):
        self.env = env
        self.request = request

    def method(self):
        return method_name(self.request)

    def query_params(self):
        try:
            values = parse_qs(
                urlparse(self.request.url).query,
                keep_blank_values=True,
            )
            return {
                str(key): str(items[0])
                for key, items in values.items()
                if isinstance(items, list) and items
            }
        except Exception:
            return {}

    @staticmethod
    def response(payload, status=200, headers=None,
                 cache_control="no-store, max-age=0, must-revalidate"):
        return json_response(
            payload, status=status, cache_control=cache_control,
            extra_headers=dict(headers or {}))

    @staticmethod
    def now():
        return int(Date.now())

    @staticmethod
    def new_id():
        return new_socket_id()

    async def reserve(self, maximum, window_ms,
                      scope="agent-guestbook-rate-v1"):
        """Atomically spend one DATA_KEY-blinded per-address write."""
        await ensure_schema(self.env)
        try:
            address = str(
                self.request.headers.get("cf-connecting-ip") or "").strip()
        except Exception:
            address = ""
        # Production always supplies CF-Connecting-IP. Keep stripped-proxy and
        # local/self-host traffic in one conservative bucket instead of making
        # missing attribution an unlimited bypass.
        subject = address[:64] or "unattributed"
        visitor_bi = await blind_index(
            self.env, str(scope) + "\n" + subject)
        now = int(Date.now())
        row = await d1_first(
            self.env,
            "INSERT INTO agent_guestbook_rate "
            "(visitor_bi,count,window_start_ts) VALUES (?,1,?) "
            "ON CONFLICT(visitor_bi) DO UPDATE SET "
            "count=CASE WHEN ?-agent_guestbook_rate.window_start_ts>=? "
            "THEN 1 ELSE agent_guestbook_rate.count+1 END,"
            "window_start_ts=CASE WHEN "
            "?-agent_guestbook_rate.window_start_ts>=? "
            "THEN ? ELSE agent_guestbook_rate.window_start_ts END "
            "RETURNING count,window_start_ts",
            visitor_bi, now, now, int(window_ms), now, int(window_ms), now,
        )
        count = int((row or {}).get("count") or 1)
        window_start = int((row or {}).get("window_start_ts") or now)
        if count == 1:
            # Only blinded counters live here. Opportunistically remove expired
            # subjects so rotating addresses cannot grow the table forever.
            await d1_run(
                self.env,
                "DELETE FROM agent_guestbook_rate WHERE window_start_ts<?",
                now - (2 * int(window_ms)),
            )
        if count <= int(maximum):
            return 0
        return max(1000, int(window_ms) - (now - window_start))

    async def publish(self, plain):
        passphrase = await _room_chat_passphrase(
            self.env, "mainnode", "forkmesh", PUBLIC_WORLD_GENERAL_ROOM)
        envelope = await _chat_encrypted_envelope(
            PUBLIC_WORLD_GENERAL_ROOM, passphrase, plain)
        body = json.dumps(envelope, separators=(",", ":"), ensure_ascii=False)
        signature = hmac.new(
            (_require_data_secret(self.env)
             + ":agent-guestbook-delivery-v1").encode(),
            body.encode(),
            "sha256",
        ).hexdigest()
        room_id = self.env.FORKMESH_MAINNODE_ROOM.idFromName(
            PUBLIC_WORLD_GENERAL_ROOM_KEY)
        request = JsRequest.new(
            "https://forkmesh.internal/api/chat/agent-guestbook",
            to_js({
                "method": "POST",
                "headers": {
                    "content-type": "application/json",
                    "x-forkmesh-agent-guestbook": signature,
                },
                "body": body,
            }),
        )
        response = await self.env.FORKMESH_MAINNODE_ROOM.get(room_id).fetch(
            request)
        if int(getattr(response, "status", 500) or 500) >= 400:
            raise RuntimeError("agent_guestbook_delivery_failed")


class _AgentNotesRuntime(_AgentGuestbookRuntime):

    async def pending_count(self):
        row = await d1_first(
            self.env,
            "SELECT COUNT(*) AS n FROM agent_note_queue WHERE status='pending'")
        return int((row or {}).get("n") or 0)

    async def enqueue(self, entry):
        entry_id = _ap_uuid()
        sealed = await encrypt_row(self.env, dict(entry))
        await d1_run(
            self.env,
            "INSERT INTO agent_note_queue(entry_id,status,created_at,"
            "reviewed_at,note_id,data) VALUES(?,'pending',?,0,'',?)",
            entry_id, int(Date.now()), sealed)
        return entry_id
