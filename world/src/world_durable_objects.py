from workers import DurableObject


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__"):
            namespace[name] = value


WORLD_MIRROR_TRAFFIC_MAX_AGE_MS = 60_000


class ForkMeshWorld(DurableObject):
    """Ephemeral Town Square presence and movement relay.

    The object uses the WebSocket Hibernation API, with the current allowlisted
    public state held only in each live socket's attachment. It never opens
    Durable Object storage, KV, R2, or chat history, and the only D1 rows it
    writes are content-free aggregates (moderation blocks, account step totals,
    and the System Capacity byte counters). Once a socket closes there is no
    movement history to read back and its random peer id is gone.
    """

    traffic_binding = "FORKMESH_WORLD"

    async def fetch(self, request):
        path = urlparse(request.url).path
        if path.rstrip("/") == "/api/status/push":
            return await self._status_push(request)
        if path.rstrip("/") == "/api/status/ws":
            return self._status_socket(request)
        if path.rstrip("/") == "/api/world/online":
            if method_name(request) != "GET":
                return json_response(
                    {"error": "method_not_allowed"},
                    status=405,
                    extra_headers={"allow": "GET"},
                )
            return json_response(
                {
                    "ok": True,
                    "online": len(self._live_sockets(cleanup=True)),
                    "now": int(Date.now()),
                },
                cache_seconds=10,
            )
        if path.rstrip("/") == "/api/world/manual-block":
            return await self._manual_block(request)
        if path.rstrip("/") == "/api/world/mirror-push":
            return await self._mirror_push(request)
        if path.rstrip("/") == "/api/world/mirror-traffic":
            return await self._mirror_traffic(request)
        if path.rstrip("/") == "/api/world/new-member":
            return await self._new_member(request)
        if path.rstrip("/") == "/api/world/floor-notes-changed":
            return await self._floor_notes_changed(request)
        if path.rstrip("/") != "/api/world/ws":
            return json_response({"error": "not_found"}, status=404)

        upgrade = request.headers.get("upgrade")
        if not (upgrade and upgrade.lower() == "websocket"):
            return json_response(
                {"error": "websocket_required"},
                status=426,
                extra_headers={"upgrade": "websocket"},
            )

        now = int(Date.now())
        try:
            mesh_home = str(
                request.headers.get("x-forkmesh-world-home") or "")
            watch = (
                str(request.headers.get("x-forkmesh-world-watch") or "")
                == "1")
        except Exception:
            mesh_home = ""
            watch = False
        if not re.fullmatch(
                r"[a-z0-9.-]{1,253}(?::[0-9]{1,5})?", mesh_home):
            mesh_home = ""
        if watch and mesh_home:
            return self._watch_socket(now)
        peers = self._live_sockets(cleanup=True)
        if len(peers) >= world_protocol.WORLD_MAX_CONNECTIONS:
            return json_response({"error": "world_full"}, status=429)
        if not self._connection_admitted(now):
            return json_response(
                {"error": "connection_rate_limited"},
                status=429,
                extra_headers={"retry-after": "10"},
            )

        try:
            raw_country = request.headers.get("x-forkmesh-country") or ""
        except Exception:
            raw_country = ""
        live_ids = {
            str(_ws_attr(peer, "id", "") or "") for peer in peers
        }
        peer_id = ""
        for _attempt in range(4):
            candidate = new_world_peer_id()
            if (
                re.fullmatch(r"[A-Za-z0-9_-]{1,32}", candidate or "")
                and candidate not in live_ids
            ):
                peer_id = candidate
                break
        if not peer_id:
            return json_response({"error": "identity_unavailable"}, status=503)
        country_source = world_protocol.approximate_country_code(raw_country)
        trusted_claim = {}
        is_admin = False
        ticket_nonce = ""
        account_bi = ""
        try:
            encoded_claim = (
                request.headers.get("x-forkmesh-world-claim") or "")
            if encoded_claim and re.fullmatch(
                    r"[A-Za-z0-9_-]{1,1024}", encoded_claim):
                padded = encoded_claim + "=" * (
                    (4 - len(encoded_claim) % 4) % 4)
                candidate = json.loads(
                    base64.urlsafe_b64decode(padded).decode())
                if isinstance(candidate, dict):
                    candidate_nonce = str(
                        candidate.get("ticketNonce") or "")
                    if re.fullmatch(
                            r"[A-Za-z0-9_-]{8,32}", candidate_nonce):
                        trusted_claim = (
                            world_protocol.trusted_presence_claim(
                                candidate.get("name", ""),
                                candidate.get(
                                    "accountStatus", "Guest"),
                                candidate.get("nodeCount", 0)))
                        is_admin = candidate.get("isAdmin") is True
                        ticket_nonce = candidate_nonce
                        candidate_account_bi = str(
                            candidate.get("accountBi") or "")
                        account_bi = (
                            candidate_account_bi
                            if re.fullmatch(
                                r"[a-f0-9]{64}", candidate_account_bi)
                            else "")
        except Exception:
            trusted_claim = {}
            is_admin = False
            ticket_nonce = ""
            account_bi = ""
        if ticket_nonce and any(
                hmac.compare_digest(
                    str(_ws_attr(peer, "ticket_nonce", "") or ""),
                    ticket_nonce)
                for peer in peers):
            return json_response({"error": "ticket_replayed"}, status=409)

        # One account is one avatar. When the same signed-in person joins from
        # a second device (or a second visible tab), their earlier socket is
        # retired here instead of standing beside them as a twin: every viewer
        # keeps seeing exactly one figure for that account, and it is the one
        # the person is actually driving. Only the ticket-verified account name
        # can fold two sockets together — a guest may type any display name, so
        # guests are never combined and no unverified name can evict anyone.
        account_key = world_protocol.account_presence_key(
            trusted_claim.get("name", ""), mesh_home)
        if account_key:
            replaced = [
                peer for peer in peers
                if _ws_attr(peer, "account_key", "") == account_key
            ]
            for peer in replaced:
                await self._flush_steps(peer, now, force=True)
                self._depart(
                    peer, WORLD_ACCOUNT_TAKEOVER_CODE,
                    "moved to your newest device")
            if replaced:
                # The retired sockets release their arrival cells and drop out
                # of the newcomer's welcome snapshot.
                peers = self._live_sockets()

        state = world_protocol.default_presence(peer_id, now)
        state["home"] = mesh_home
        blocked_slots = []
        for peer in peers:
            blocked_slots.append(_ws_attr(peer, "arrival_slot", -1))
            if _ws_attr(peer, "space", "") == "town-square":
                # A visitor who restored a saved spot onto an arrival cell
                # blocks that cell even without a matching reservation.
                blocked_slots.append(
                    world_protocol.arrival_slot_near_position(
                        _ws_attr(peer, "x", None), _ws_attr(peer, "z", None)))
        arrival_slot = world_protocol.first_available_arrival_slot(
            blocked_slots)
        state.update(world_protocol.arrival_position(arrival_slot))
        moderation_tokens = {}
        for target_type in ("ip", "agent"):
            try:
                token = str(request.headers.get(
                    "x-forkmesh-world-%s-token" % target_type) or "")
            except Exception:
                token = ""
            moderation_tokens[target_type] = (
                token if re.fullmatch(r"[a-f0-9]{64}", token) else "")
        try:
            client_ip = str(
                request.headers.get("x-forkmesh-world-private-ip") or "")
            client_ip = str(ipaddress.ip_address(client_ip))[:64]
        except Exception:
            client_ip = ""
        try:
            client_user_agent = clean_string(
                request.headers.get(
                    "x-forkmesh-world-private-agent") or "", 1024)
        except Exception:
            client_user_agent = ""
        if trusted_claim:
            # The name and operator count remain private attachment fields
            # until the owner's first presence frame opts into each one.
            state["accountStatus"] = trusted_claim["accountStatus"]

        client, server = WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js(["world"]))
        self._save_attachment(
            server, state, last=now, rate_start=now, rate_count=0,
            country_source=country_source,
            trusted_name=trusted_claim.get("name", ""),
            trusted_node_count=trusted_claim.get("nodeCount", 0),
            arrival_slot=arrival_slot, is_admin=is_admin,
            ip_token=moderation_tokens["ip"],
            agent_token=moderation_tokens["agent"],
            client_ip=client_ip,
            client_user_agent=client_user_agent,
            ticket_nonce=ticket_nonce,
            account_bi=account_bi,
            step_flushed_at=now)

        # The only snapshot is the state of sockets alive right now. It is sent
        # directly from runtime attachments and is never persisted or replayed.
        snapshot = [
            self._presence_for_viewer(server, peer)
            for peer in peers
        ]
        self._safe_send(server, {
            "type": "welcome",
            "id": peer_id,
            "self": world_protocol.public_presence(state),
            "peers": snapshot,
        })
        self._broadcast({
            "type": "join",
            "peer": world_protocol.public_presence(state),
        }, exclude_id=peer_id, budgeted=True, moderation_subject=server)

        return JsResponse.new(
            None, to_js({"status": 101, "webSocket": client}))

    def _watch_socket(self, now):
        if len(self._live_watchers(cleanup=True)) >= (
                world_protocol.WORLD_MAX_WATCHERS):
            return json_response({"error": "world_full"}, status=429)
        client, server = WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js(["watch"]))
        try:
            server.serializeAttachment(
                to_js({"channel": "watch", "last": int(now)}))
        except Exception:
            pass
        self._safe_send(server, {
            "type": "welcome",
            "id": "",
            "watch": True,
            "peers": [
                world_protocol.public_presence(self._socket_state(peer))
                for peer in self._live_sockets(cleanup=True)
            ],
        })
        return JsResponse.new(
            None, to_js({"status": 101, "webSocket": client}))

    def _live_watchers(self, cleanup=False):
        now = int(Date.now())
        try:
            sockets = self.ctx.getWebSockets("watch")
        except Exception:
            sockets = []
        live = []
        for socket in sockets:
            if world_protocol.presence_is_stale(
                    _ws_attr(socket, "last", 0), now):
                if cleanup:
                    self._safe_close(socket, 1001, "stale")
                continue
            live.append(socket)
        return live

    def _watch_message(self, ws, message):
        if not isinstance(message, str) or len(message.encode("utf-8")) > 128:
            self._safe_close(ws, 1003, "watch sockets only ping")
            return
        try:
            payload = json.loads(message)
        except Exception:
            return
        if not isinstance(payload, dict) or payload.get("type") != "ping":
            return
        now = int(Date.now())
        if now - int(_ws_attr(ws, "last", 0) or 0) < 5000:
            return
        try:
            ws.serializeAttachment(
                to_js({"channel": "watch", "last": now}))
        except Exception:
            pass
        self._safe_send(ws, {"type": "pong", "serverTimeMs": now})

    def _status_socket(self, request):
        """Accept a public, receive-only status-generation signal socket."""
        if method_name(request) != "GET":
            return json_response(
                {"error": "method_not_allowed"}, status=405,
                extra_headers={"allow": "GET"})
        upgrade = request.headers.get("upgrade")
        if not (upgrade and upgrade.lower() == "websocket"):
            return json_response(
                {"error": "websocket_required"}, status=426,
                extra_headers={"upgrade": "websocket"})
        try:
            listeners = self.ctx.getWebSockets("status")
        except Exception:
            listeners = []
        if len(listeners) >= world_protocol.STATUS_SUBSCRIBER_MAX_CONNECTIONS:
            return json_response({"error": "status_stream_full"}, status=429)

        client, server = WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js(["status"]))
        try:
            server.serializeAttachment(to_js({"channel": "status"}))
        except Exception:
            pass
        self._safe_send(server, {"type": "ready"})
        return JsResponse.new(
            None, to_js({"status": 101, "webSocket": client}))

    async def _status_push(self, request):
        """Relay a signed new-snapshot signal to status and World clients."""
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        try:
            generated_at = int(data.get("generatedAt") or 0)
            signature = str(
                request.headers.get("x-forkmesh-status-control") or "")
        except Exception:
            generated_at = 0
            signature = ""
        now = int(Date.now())
        expected = _status_update_signature(self.env, generated_at)
        if (
            not isinstance(data, dict)
            or generated_at <= 0
            or generated_at > now + 30_000
            or now - generated_at > 10 * 60 * 1000
            or not hmac.compare_digest(signature, expected)
        ):
            return json_response({"error": "unauthorized"}, status=401)

        frame = {"type": "status-update", "generatedAt": generated_at}
        delivered = 0
        try:
            listeners = self.ctx.getWebSockets("status")
        except Exception:
            listeners = []
        for listener in listeners:
            self._safe_send(listener, frame)
            delivered += 1
        for peer in self._live_sockets(cleanup=True):
            self._safe_send(peer, frame)
            delivered += 1
        return json_response({"ok": True, "delivered": delivered})

    async def _mirror_push(self, request):
        """Relay a signed outer-Worker head-advance notice to every visitor.

        Only the outer Worker holds the control HMAC, so browser sockets can
        never inject or forge this frame. The frame itself carries no facts a
        client acts on directly — viewers re-read the signed public mirror
        payload and animate from that verified state.
        """
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
        node = clean_string(data.get("node", ""), MAX_NODE_NAME).strip()
        repo = clean_string(data.get("repo", ""), 120).strip()
        commit = str(data.get("commit") or "").strip().lower()
        changed_files = _world_mirror_push_changed_files(
            data.get("changedFiles"))
        try:
            signature = str(
                request.headers.get("x-forkmesh-world-control") or "")
        except Exception:
            signature = ""
        expected = _world_mirror_push_signature(
            self.env, node, repo, commit, changed_files)
        if (
            not node
            or not re.fullmatch(r"[0-9a-f]{40,64}", commit)
            or not hmac.compare_digest(signature, expected)
        ):
            return json_response({"error": "unauthorized"}, status=401)
        self._broadcast({
            "type": "mirror-push",
            "node": node,
            "repo": repo,
            # A short id is plenty for the announcement; the full head comes
            # from the signed mirror payload each client refreshes.
            "commit": commit[:12],
            "changedFiles": changed_files,
        })
        return json_response({"ok": True, "delivered": True})

    async def _mirror_traffic(self, request):
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
        try:
            at = int(data.get("at") or 0)
        except (TypeError, ValueError, OverflowError):
            at = 0
        entries = _world_mirror_traffic_entries(data.get("nodes"))
        try:
            signature = str(
                request.headers.get("x-forkmesh-world-control") or "")
        except Exception:
            signature = ""
        now = int(Date.now())
        if (
            not entries
            or abs(now - at) > WORLD_MIRROR_TRAFFIC_MAX_AGE_MS
            or not hmac.compare_digest(
                signature,
                _world_mirror_traffic_signature(self.env, at, entries))
        ):
            return json_response({"error": "unauthorized"}, status=401)
        self._broadcast({
            "type": "mirror-traffic",
            "nodes": [[name, count] for name, count in entries],
        })
        return json_response({"ok": True, "delivered": True})

    async def _new_member(self, request):
        """Relay a signed "someone just joined ForkMesh" notice to visitors.

        Only the outer Worker holds the control HMAC, so a browser socket can
        neither forge a welcome nor replay a captured one past its minute of
        freshness. The frame carries a single public account name, which every
        client sanitizes again before it reaches the screen.
        """
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        try:
            signature = str(
                request.headers.get("x-forkmesh-new-member") or "")
        except Exception:
            signature = ""
        name = _new_member_announcement(
            self.env, data, signature, int(Date.now()))
        if not name:
            return json_response({"error": "unauthorized"}, status=401)
        self._broadcast({"type": "new-member", "name": name})
        return json_response({"ok": True, "delivered": True})

    async def _floor_notes_changed(self, request):
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        try:
            signature = str(
                request.headers.get("x-forkmesh-world-control") or "")
        except Exception:
            signature = ""
        note_id = _world_floor_notes_announcement(
            self.env, data, signature, int(Date.now()))
        if not note_id:
            return json_response({"error": "unauthorized"}, status=401)
        self._broadcast({"type": "floor-notes", "id": note_id})
        return json_response({"ok": True, "delivered": True})

    async def _manual_block(self, request):
        """Disconnect sockets selected by a signed outer-Worker command."""
        if method_name(request) != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            return json_response({"error": "invalid_json"}, status=400)
        if not isinstance(data, dict):
            return json_response({"error": "invalid_json"}, status=400)
        target_type = str(data.get("targetType") or "").strip().lower()
        subject_token = str(data.get("subjectToken") or "").strip().lower()
        try:
            expires_at = int(data.get("expiresAt") or 0)
        except (TypeError, ValueError):
            expires_at = 0
        try:
            signature = str(
                request.headers.get("x-forkmesh-world-control") or "")
        except Exception:
            signature = ""
        expected = _world_manual_control_signature(
            self.env, target_type, subject_token, expires_at)
        now = int(Date.now())
        if (
            target_type not in ("ip", "agent")
            or not re.fullmatch(r"[a-f0-9]{64}", subject_token)
            or expires_at <= now
            or expires_at > now + WORLD_MANUAL_BLOCK_MAX_MS
            or not hmac.compare_digest(signature, expected)
        ):
            return json_response({"error": "unauthorized"}, status=401)
        disconnected = 0
        token_field = target_type + "_token"
        for peer in list(self._live_sockets(cleanup=True)):
            if _ws_attr(peer, token_field, "") != subject_token:
                continue
            disconnected += 1
            self._depart(peer, 1008, "temporary manual moderation")
        return json_response({"ok": True, "disconnected": disconnected})

    def _socket_state(self, ws):
        attachment = _ws_attachment(ws)
        if attachment is None:
            return {}
        return {
            field: getattr(attachment, field, None)
            for field in world_protocol.WORLD_PUBLIC_FIELDS
        }

    def _save_attachment(self, ws, state, last, rate_start, rate_count,
                         departed=False, country_source=None,
                         trusted_name=None, trusted_node_count=None,
                         pending_knocks=None, arrival_slot=None, is_admin=None,
                         ip_token=None, agent_token=None, ticket_nonce=None,
                         client_ip=None, client_user_agent=None,
                         pending_handshakes=None, account_bi=None,
                         step_carry=None, pending_steps=None,
                         step_observed_at=None, step_flushed_at=None):
        if country_source is None:
            country_source = _ws_attr(ws, "country_source", "")
        if trusted_name is None:
            trusted_name = _ws_attr(ws, "trusted_name", "")
        if trusted_node_count is None:
            trusted_node_count = _ws_attr(ws, "trusted_node_count", 0)
        if pending_knocks is None:
            pending_knocks = _ws_attr(ws, "pending_knocks", [])
        if pending_handshakes is None:
            pending_handshakes = _ws_attr(ws, "pending_handshakes", [])
        if arrival_slot is None:
            arrival_slot = _ws_attr(ws, "arrival_slot", -1)
        if is_admin is None:
            is_admin = bool(_ws_attr(ws, "is_admin", False))
        if ip_token is None:
            ip_token = _ws_attr(ws, "ip_token", "")
        if agent_token is None:
            agent_token = _ws_attr(ws, "agent_token", "")
        if ticket_nonce is None:
            ticket_nonce = _ws_attr(ws, "ticket_nonce", "")
        if client_ip is None:
            client_ip = _ws_attr(ws, "client_ip", "")
        if client_user_agent is None:
            client_user_agent = _ws_attr(ws, "client_user_agent", "")
        if account_bi is None:
            account_bi = _ws_attr(ws, "account_bi", "")
        if step_carry is None:
            step_carry = _ws_attr(ws, "step_carry", 0.0)
        if pending_steps is None:
            pending_steps = _ws_attr(ws, "pending_steps", 0)
        if step_observed_at is None:
            step_observed_at = _ws_attr(ws, "step_observed_at", 0)
        if step_flushed_at is None:
            step_flushed_at = _ws_attr(ws, "step_flushed_at", 0)
        pending_knocks = [
            str(peer_id)
            for peer_id in list(pending_knocks or [])[-8:]
            if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", str(peer_id or ""))
        ]
        pending_handshakes = [
            str(peer_id)
            for peer_id in list(pending_handshakes or [])[-8:]
            if re.fullmatch(r"[A-Za-z0-9_-]{1,64}", str(peer_id or ""))
        ]
        trusted_fields = world_protocol.trusted_presence_claim(
            trusted_name or "", "Guest", trusted_node_count)
        attachment = {
            **world_protocol.public_presence(state),
            # Kept only in this live socket attachment so a user's privacy
            # control can restore their edge-derived flag after hiding it.
            "country_source":
                world_protocol.approximate_country_code(country_source),
            # These server-validated sources never leave the attachment
            # directly. sanitize_message exposes them only when the user opts
            # into name/operator-belt sharing.
            "trusted_name": trusted_fields["name"] if trusted_name else "",
            "trusted_node_count": trusted_fields["nodeCount"],
            # The private "one account, one avatar" key. It never reaches a
            # frame; it only lets a later connection from the same verified
            # account retire this socket instead of standing beside it.
            "account_key": (
                world_protocol.account_presence_key(
                    trusted_fields["name"], state.get("home") or "")
                if trusted_name else ""),
            # These opaque rotating subjects and authorization bit remain
            # private attachment data. Only `_presence_for_viewer` can project
            # the handles, and only to a server-verified administrator.
            "is_admin": bool(is_admin),
            "ip_token": (
                str(ip_token)
                if re.fullmatch(r"[a-f0-9]{64}", str(ip_token or ""))
                else ""),
            "agent_token": (
                str(agent_token)
                if re.fullmatch(r"[a-f0-9]{64}", str(agent_token or ""))
                else ""),
            # Ephemeral guest-moderation detail. These fields live only in the
            # active socket attachment and are projected only to a
            # server-verified platform administrator by
            # `_presence_for_viewer`.
            "client_ip": clean_string(client_ip or "", 64),
            "client_user_agent": clean_string(
                client_user_agent or "", 1024),
            "ticket_nonce": (
                str(ticket_nonce)
                if re.fullmatch(
                    r"[A-Za-z0-9_-]{8,32}",
                    str(ticket_nonce or ""))
                else ""),
            # The blind account key and partial aggregate never enter a public
            # frame. Coordinates remain only in the existing live presence;
            # D1 receives whole-step increments, never a route or sample.
            "account_bi": (
                str(account_bi)
                if re.fullmatch(r"[a-f0-9]{64}", str(account_bi or ""))
                else ""),
            "step_carry": max(0.0, float(step_carry or 0.0)),
            "pending_steps": max(0, int(pending_steps or 0)),
            "step_observed_at": max(0, int(step_observed_at or 0)),
            "step_flushed_at": max(0, int(step_flushed_at or 0)),
            # One-use, live-socket-only consent requests. They are never
            # persisted, broadcast, or exposed in the public presence record.
            "pending_knocks": pending_knocks,
            # The same one-use shape for handshake offers this socket has
            # received: only an offer parked here can be shaken back or
            # declined, so nobody can answer a greeting they were never sent.
            "pending_handshakes": pending_handshakes,
            # A room-local collision-avoidance index. It is never broadcast,
            # persisted, or derived from an account/network identifier.
            "arrival_slot": int(arrival_slot),
            "last": int(last or 0),
            "rl_start": int(rate_start or 0),
            "rl_count": int(rate_count or 0),
            "departed": bool(departed),
        }
        try:
            ws.serializeAttachment(to_js(attachment))
        except Exception:
            pass

    async def _flush_steps(self, ws, now, force=False):
        """Persist one blind-indexed aggregate and tell only its owner."""
        account_bi = str(_ws_attr(ws, "account_bi", "") or "")
        pending = max(0, int(_ws_attr(ws, "pending_steps", 0) or 0))
        flushed_at = max(0, int(_ws_attr(ws, "step_flushed_at", 0) or 0))
        if not re.fullmatch(r"[a-f0-9]{64}", account_bi) or pending <= 0:
            return
        if (
            not force
            and pending < world_protocol.WORLD_STEP_FLUSH_COUNT
            and int(now) - flushed_at < world_protocol.WORLD_STEP_FLUSH_MS
        ):
            return
        try:
            row = await d1_first(
                self.env,
                "INSERT INTO world_user_activity "
                "(account_bi,total_steps,updated_at) VALUES (?,?,?) "
                "ON CONFLICT(account_bi) DO UPDATE SET "
                "total_steps=MIN(?,world_user_activity.total_steps+"
                "excluded.total_steps),updated_at=MAX("
                "world_user_activity.updated_at,excluded.updated_at) "
                "RETURNING total_steps",
                account_bi, pending, int(now),
                world_protocol.WORLD_STEP_MAX_TOTAL,
            )
            total = int((row or {}).get("total_steps", -1))
        except Exception:
            return
        if total < 0:
            return
        self._save_attachment(
            ws,
            self._socket_state(ws),
            last=_ws_attr(ws, "last", now),
            rate_start=_ws_attr(ws, "rl_start", now),
            rate_count=_ws_attr(ws, "rl_count", 0),
            departed=bool(_ws_attr(ws, "departed", False)),
            pending_steps=0,
            step_flushed_at=int(now),
        )
        self._safe_send(ws, {
            "type": "step-total",
            "totalSteps": max(
                0, min(world_protocol.WORLD_STEP_MAX_TOTAL, total)),
        })

    def _presence_for_viewer(self, viewer, subject):
        public = world_protocol.public_presence(self._socket_state(subject))
        is_admin = bool(_ws_attr(viewer, "is_admin", False))
        if not is_admin:
            return public
        handles = {}
        for target_type in ("ip", "agent"):
            token = str(_ws_attr(subject, target_type + "_token", "") or "")
            if re.fullmatch(r"[a-f0-9]{64}", token):
                handles[target_type] = token
        if handles:
            public["moderationHandles"] = handles
        if str(public.get("accountStatus") or "Guest") == "Guest":
            ip_value = clean_string(
                _ws_attr(subject, "client_ip", "") or "", 64)
            agent_value = clean_string(
                _ws_attr(subject, "client_user_agent", "") or "", 1024)
            if ip_value or agent_value:
                public["adminGuestNetwork"] = {
                    "ipAddress": ip_value,
                    "userAgent": agent_value,
                }
        return public

    def _mark_departed(self, ws):
        self._save_attachment(
            ws,
            self._socket_state(ws),
            last=_ws_attr(ws, "last", 0),
            rate_start=_ws_attr(ws, "rl_start", 0),
            rate_count=_ws_attr(ws, "rl_count", 0),
            departed=True,
        )

    def _live_sockets(self, cleanup=False):
        now = int(Date.now())
        try:
            sockets = self.ctx.getWebSockets("world")
        except Exception:
            sockets = []
        live = []
        stale_ids = []
        for socket in sockets:
            # A close/error path marks the attachment before asking the runtime
            # to close it. Hibernation may still return that socket briefly;
            # exclude it without producing a duplicate leave frame.
            if bool(_ws_attr(socket, "departed", False)):
                if cleanup:
                    self._safe_close(socket, 1000, "")
                continue
            if world_protocol.presence_is_stale(
                    _ws_attr(socket, "last", 0), now):
                stale_id = _ws_attr(socket, "id")
                if stale_id:
                    stale_ids.append(stale_id)
                if cleanup:
                    self._mark_departed(socket)
                    self._safe_close(socket, 1001, "stale")
                continue
            live.append(socket)

        # Tell remaining clients to remove stale avatars immediately. Marking
        # the socket departed before close prevents webSocketClose from sending
        # the same leave twice.
        if cleanup and stale_ids:
            watchers = self._live_watchers()
            for stale_id in stale_ids:
                frame = {"type": "leave", "id": stale_id}
                for peer in live + watchers:
                    self._safe_send(peer, frame)
        return live

    def _rate_step(self, ws, state, now):
        allowed, start, count = world_protocol.advance_rate_window(
            _ws_attr(ws, "rl_start", 0),
            _ws_attr(ws, "rl_count", 0),
            now,
        )
        # Invalid or over-limit frames do not refresh last-seen and therefore
        # cannot keep a dead/spamming avatar alive indefinitely.
        self._save_attachment(
            ws, state,
            last=_ws_attr(ws, "last", 0),
            rate_start=start,
            rate_count=count,
            departed=bool(_ws_attr(ws, "departed", False)),
        )
        return allowed, start, count

    def _connection_admitted(self, now):
        allowed, start, count = world_protocol.advance_connection_window(
            getattr(self, "_connect_window", 0),
            getattr(self, "_connect_count", 0),
            now,
        )
        self._connect_window = start
        self._connect_count = count
        return allowed

    def _broadcast_admitted(self, now):
        allowed, start, count = world_protocol.advance_broadcast_window(
            getattr(self, "_broadcast_window", 0),
            getattr(self, "_broadcast_count", 0),
            now,
        )
        self._broadcast_window = start
        self._broadcast_count = count
        return allowed

    async def webSocketMessage(self, ws, message):
        if _maintenance_socket_closed(self.env, ws):
            return
        if _ws_attr(ws, "channel", "") == "status":
            # Status sockets are server-to-client signals. A small ping is the
            # only accepted client frame and is not required for hibernation.
            if not isinstance(message, str):
                self._safe_close(ws, 1003, "text frames only")
                return
            if len(message.encode("utf-8")) > 128:
                self._safe_close(ws, 1009, "message too large")
                return
            try:
                payload = json.loads(message)
            except Exception:
                return
            if isinstance(payload, dict) and payload.get("type") == "ping":
                self._safe_send(
                    ws, {"type": "pong", "serverTimeMs": int(Date.now())})
            return
        if _ws_attr(ws, "channel", "") == "watch":
            self._watch_message(ws, message)
            return
        if bool(_ws_attr(ws, "departed", False)):
            self._safe_close(ws, 1000, "")
            return
        if not isinstance(message, str):
            self._safe_close(ws, 1003, "text frames only")
            return
        if len(message.encode("utf-8")) > world_protocol.WORLD_MESSAGE_MAX_BYTES:
            self._safe_close(ws, 1009, "message too large")
            return
        durable_object_traffic_note(
            self, bytes_in=len(message.encode("utf-8")), messages=1)
        await durable_object_traffic_flush(self)

        state = self._socket_state(ws)
        if not state.get("id"):
            self._safe_close(ws, 1008, "invalid presence")
            return
        now = int(Date.now())
        if world_protocol.presence_is_stale(
                _ws_attr(ws, "last", 0), now):
            self._depart(ws, 1001, "stale")
            return
        allowed, rate_start, rate_count = self._rate_step(
            ws, state, now)
        # Every frame (including a heartbeat or invalid JSON) is an opportunity
        # to reap peers that vanished without a close event.
        self._live_sockets(cleanup=True)

        try:
            payload = json.loads(message)
        except Exception:
            if not allowed:
                self._safe_close(ws, 1008, "rate limit")
            return
        if not allowed:
            kind = (
                str(payload.get("type") or "").strip().lower()
                if isinstance(payload, dict) else ""
            )
            disposable = kind in ("move", "presence", "ping")
            hard_limit = world_protocol.WORLD_RATE_HARD_MAX_PER_WINDOW
            if disposable and rate_count <= hard_limit:
                # Keep the latest accepted attachment as the coalesced state.
                # A subsequent frame in a fresh window will carry the browser's
                # newest position/profile, so a normal connect+movement burst
                # sheds traffic instead of tearing down the multiplayer socket.
                return
            self._safe_close(
                ws,
                1008,
                "sustained rate limit" if disposable else "rate limit",
            )
            return
        interaction = world_protocol.sanitize_interaction(payload, state)
        if interaction is not None:
            # Home and handshake interactions are targeted, text-free consent
            # frames. The server keeps at most eight one-use knocks and eight
            # one-use handshake offers inside the live socket attachment;
            # hibernation/disconnect naturally expires them. Sender identity
            # always comes from the attachment, never JSON.
            self._save_attachment(
                ws, state, last=now,
                rate_start=rate_start, rate_count=rate_count)
            if interaction["kind"] == "knock":
                for peer in self._live_sockets(cleanup=True):
                    if (
                        _ws_attr(peer, "id") == interaction["target"]
                        and _ws_attr(peer, "publicDoor", "closed") == "knock"
                    ):
                        pending = list(
                            _ws_attr(peer, "pending_knocks", []) or [])
                        if state.get("id") not in pending:
                            pending.append(state.get("id"))
                        self._save_attachment(
                            peer,
                            self._socket_state(peer),
                            last=_ws_attr(peer, "last", now),
                            rate_start=_ws_attr(peer, "rl_start", now),
                            rate_count=_ws_attr(peer, "rl_count", 0),
                            pending_knocks=pending[-8:],
                        )
                        self._safe_send(peer, {
                            "type": "interaction",
                            "kind": "knock",
                            "from": state.get("id"),
                        })
                        break
            elif interaction["kind"] == "handshake-offer":
                # A handshake offer reaches one live peer and nobody else, so
                # an unanswered greeting stays between the two of them.
                for peer in self._live_sockets(cleanup=True):
                    if _ws_attr(peer, "id") == interaction["target"]:
                        pending = list(
                            _ws_attr(peer, "pending_handshakes", []) or [])
                        if state.get("id") not in pending:
                            pending.append(state.get("id"))
                        self._save_attachment(
                            peer,
                            self._socket_state(peer),
                            last=_ws_attr(peer, "last", now),
                            rate_start=_ws_attr(peer, "rl_start", now),
                            rate_count=_ws_attr(peer, "rl_count", 0),
                            pending_handshakes=pending[-8:],
                        )
                        self._safe_send(peer, {
                            "type": "interaction",
                            "kind": "handshake-offer",
                            "from": state.get("id"),
                        })
                        break
            elif interaction["kind"] in (
                "handshake-accept", "handshake-decline",
            ):
                pending = list(
                    _ws_attr(ws, "pending_handshakes", []) or [])
                if interaction["target"] not in pending:
                    return
                pending = [
                    peer_id for peer_id in pending
                    if peer_id != interaction["target"]
                ]
                self._save_attachment(
                    ws, state, last=now,
                    rate_start=rate_start, rate_count=rate_count,
                    pending_handshakes=pending,
                )
                if interaction["kind"] == "handshake-decline":
                    for peer in self._live_sockets(cleanup=True):
                        if _ws_attr(peer, "id") == interaction["target"]:
                            self._safe_send(peer, {
                                "type": "interaction",
                                "kind": "handshake-decline",
                                "from": state.get("id"),
                            })
                            break
                else:
                    # Two people shaking hands is a public gesture: the room
                    # animates the pair the same way it animates an emote. The
                    # frame carries the two peer ids and nothing else.
                    self._broadcast({
                        "type": "interaction",
                        "kind": "handshake",
                        "from": state.get("id"),
                        "with": interaction["target"],
                    }, exclude_id=state.get("id"), budgeted=True)
            elif interaction["kind"] in ("home-grant", "home-decline"):
                pending = list(
                    _ws_attr(ws, "pending_knocks", []) or [])
                if interaction["target"] not in pending:
                    return
                pending = [
                    peer_id for peer_id in pending
                    if peer_id != interaction["target"]
                ]
                self._save_attachment(
                    ws, state, last=now,
                    rate_start=rate_start, rate_count=rate_count,
                    pending_knocks=pending,
                )
                for peer in self._live_sockets(cleanup=True):
                    if _ws_attr(peer, "id") == interaction["target"]:
                        self._safe_send(peer, {
                            "type": "interaction",
                            "kind": interaction["kind"],
                            "from": state.get("id"),
                        })
                        break
            else:
                self._broadcast({
                    "type": "interaction",
                    "kind": "emote",
                    "emote": interaction["emote"],
                    "from": state.get("id"),
                }, exclude_id=state.get("id"), budgeted=True)
            return
        sanitized = world_protocol.sanitize_message(
            payload, state, now,
            country_source=_ws_attr(ws, "country_source", ""),
            trusted_name=_ws_attr(ws, "trusted_name", ""),
            trusted_node_count=_ws_attr(ws, "trusted_node_count", 0))
        if sanitized is None:
            return
        kind, state = sanitized
        step_carry = _ws_attr(ws, "step_carry", 0.0)
        pending_steps = _ws_attr(ws, "pending_steps", 0)
        step_observed_at = _ws_attr(ws, "step_observed_at", 0)
        if kind == "move" and _ws_attr(ws, "account_bi", ""):
            credited, step_carry, step_observed_at = (
                world_protocol.movement_step_credit(
                    self._socket_state(ws), state, step_observed_at, now,
                    step_carry)
            )
            pending_steps = min(
                world_protocol.WORLD_STEP_MAX_TOTAL,
                max(0, int(pending_steps or 0)) + credited,
            )
        self._save_attachment(
            ws, state, last=now,
            rate_start=rate_start, rate_count=rate_count,
            step_carry=step_carry,
            pending_steps=pending_steps,
            step_observed_at=step_observed_at)

        if kind in ("move", "ping"):
            await self._flush_steps(ws, now)

        if kind == "ping":
            self._safe_send(ws, {"type": "pong", "serverTimeMs": now})
            return
        if kind == "presence":
            frame = {
                "type": "presence",
                "peer": world_protocol.public_presence(state),
            }
        else:
            frame = world_protocol.movement_delta(state)
        self._broadcast(
            frame, exclude_id=state.get("id"), budgeted=True,
            moderation_subject=ws if kind == "presence" else None)

    async def webSocketClose(self, ws, code, reason, was_clean):
        if _ws_attr(ws, "channel", "") in ("status", "watch"):
            self._safe_close(ws, 1000, "")
            await durable_object_traffic_flush(self, force=True)
            return
        await self._flush_steps(ws, int(Date.now()), force=True)
        self._depart(ws, 1000, "")
        # A closing socket is the last chance to bank this instance's pending
        # byte counts before the object hibernates.
        await durable_object_traffic_flush(self, force=True)

    async def webSocketError(self, ws, error):
        if _ws_attr(ws, "channel", "") in ("status", "watch"):
            self._safe_close(ws, 1011, "socket error")
            return
        await self._flush_steps(ws, int(Date.now()), force=True)
        self._depart(ws, 1011, "socket error")

    # Hibernation events may be dispatched under either naming convention.
    web_socket_message = webSocketMessage
    web_socket_close = webSocketClose
    web_socket_error = webSocketError

    def _depart(self, ws, code, reason):
        peer_id = _ws_attr(ws, "id")
        departed = bool(_ws_attr(ws, "departed", False))
        self._mark_departed(ws)
        self._safe_close(ws, code, reason)
        if peer_id and not departed:
            self._broadcast(
                {"type": "leave", "id": peer_id},
                exclude_id=peer_id,
            )

    def _broadcast(self, frame, exclude_id=None, budgeted=False,
                   moderation_subject=None):
        if budgeted and not self._broadcast_admitted(int(Date.now())):
            # State still updates in the sender's attachment, so the next
            # welcome snapshot is current even when a hot movement delta drops.
            return
        for peer in self._live_sockets(cleanup=True):
            if exclude_id and _ws_attr(peer, "id") == exclude_id:
                continue
            outgoing = frame
            if (
                moderation_subject is not None
                and isinstance(frame.get("peer"), dict)
                and bool(_ws_attr(peer, "is_admin", False))
            ):
                outgoing = {
                    **frame,
                    "peer": self._presence_for_viewer(
                        peer, moderation_subject),
                }
            self._safe_send(peer, outgoing)
        if frame.get("type") in world_protocol.WORLD_WATCH_FRAME_TYPES:
            for watcher in self._live_watchers():
                self._safe_send(watcher, frame)

    def _safe_send(self, ws, frame):
        payload = json.dumps(frame, separators=(",", ":"))
        try:
            ws.send(payload)
        except Exception:
            self._safe_close(ws, 1011, "unavailable")
            return
        durable_object_traffic_note(
            self, bytes_out=len(payload.encode("utf-8")))

    def _safe_close(self, ws, code, reason):
        try:
            ws.close(code, reason)
        except Exception:
            pass


class ForkMeshOfficeRoom(DurableObject):
    """Ephemeral authorized avatar and chair relay for one Office room."""

    traffic_binding = "FORKMESH_OFFICE_ROOM"

    async def fetch(self, request):
        path = urlparse(request.url).path
        match = OFFICE_INTERNAL_RE.fullmatch(path)
        if not match:
            return json_response({"error": "not_found"}, status=404)
        scope, version_raw, action = match.groups()
        if action == "revoke" and method_name(request) == "POST":
            self._close_all(1008, "room access revoked")
            return json_response({"ok": True})
        if (
            action != "ws"
            or method_name(request) != "GET"
            or (request.headers.get("upgrade") or "").lower() != "websocket"
        ):
            return json_response({"error": "not_found"}, status=404)

        claim = self._claim(request)
        if (
            not claim
            or claim.get("scope") != scope
            or int(claim.get("version") or 0) != int(version_raw)
        ):
            return json_response({"error": "not_found"}, status=404)
        now = int(Date.now())
        peers = self._live_sockets(cleanup=True)
        if len(peers) >= world_protocol.OFFICE_MAX_CONNECTIONS:
            return json_response({"error": "room_full"}, status=429)
        ticket_nonce = str(claim.get("nonce") or "")
        if any(
                hmac.compare_digest(
                    str(_ws_attr(peer, "ticket_nonce", "") or ""),
                    ticket_nonce)
                for peer in peers):
            return json_response({"error": "ticket_replayed"}, status=409)

        # One account is one person in the room, exactly as in the Town Square
        # (see ForkMeshWorld). A member who opens this meeting on a second
        # device replaces their earlier socket instead of occupying two chairs
        # and two rows of "In this room". The fold key is the account binding
        # the room ticket was minted against, never a typed name.
        account_bi = str(claim.get("account_bi") or "")
        if re.fullmatch(r"[0-9a-f]{64}", account_bi):
            replaced = [
                peer for peer in peers
                if hmac.compare_digest(
                    str(_ws_attr(peer, "account_bi", "") or ""), account_bi)
            ]
            for peer in replaced:
                await self._flush_steps(peer, now, force=True)
                self._depart(
                    peer, WORLD_ACCOUNT_TAKEOVER_CODE,
                    "moved to your newest device")
            if replaced:
                # The retired sockets release their seats and drop out of the
                # newcomer's participant snapshot.
                peers = self._live_sockets()

        live_ids = {
            str(_ws_attr(peer, "id", "") or "") for peer in peers
        }
        participant_id = ""
        for _attempt in range(4):
            candidate = new_world_peer_id()
            if (
                re.fullmatch(r"[A-Za-z0-9_-]{1,32}", candidate or "")
                and candidate not in live_ids
            ):
                participant_id = candidate
                break
        if not participant_id:
            return json_response({"error": "identity_unavailable"}, status=503)
        state = world_protocol.default_office_presence(participant_id, now)
        sanitized = world_protocol.sanitize_office_message(
            {"type": "presence"},
            state,
            now,
            trusted_name=claim.get("name", ""),
            trusted_account_status=claim.get("accountStatus", "Guest"),
        )
        if sanitized is not None:
            _kind, state = sanitized
        client, server = WebSocketPair.new().object_values()
        self.ctx.acceptWebSocket(server, to_js(["office"]))
        self._save_socket(
            server,
            state,
            last=now,
            rate_start=now,
            rate_count=0,
            account_bi=claim.get("account_bi", ""),
            session_id=claim.get("session_id", ""),
            scope=scope,
            version=int(version_raw),
            trusted_name=claim.get("name", ""),
            trusted_status=claim.get("accountStatus", "Guest"),
            ticket_nonce=ticket_nonce,
            auth_checked_at=now,
            step_flushed_at=now,
        )
        self._safe_send(server, {
            "type": "welcome",
            "id": participant_id,
            "participants": [
                world_protocol.public_office_presence(
                    self._socket_state(peer))
                for peer in peers
            ],
        })
        self._broadcast({
            "type": "join",
            "participant": world_protocol.public_office_presence(state),
        }, exclude_id=participant_id, budgeted=True)
        return JsResponse.new(
            None, to_js({"status": 101, "webSocket": client}))

    def _claim(self, request):
        try:
            raw = request.headers.get("x-forkmesh-office-claim") or ""
            if not re.fullmatch(r"[A-Za-z0-9_-]{1,2048}", raw):
                return None
            padded = raw + "=" * ((4 - len(raw) % 4) % 4)
            claim = json.loads(base64.urlsafe_b64decode(padded).decode())
        except Exception:
            return None
        if not isinstance(claim, dict):
            return None
        scope = str(claim.get("scope") or "")
        if scope != "world-general" and not re.fullmatch(
                r"[0-9a-f]{32}", scope):
            return None
        account_bi = str(claim.get("account_bi") or "")
        if not re.fullmatch(r"[0-9a-f]{64}", account_bi):
            return None
        session_id = str(claim.get("session_id") or "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{24,64}", session_id):
            return None
        try:
            version = int(claim.get("version") or 0)
        except (TypeError, ValueError):
            return None
        if version < 1:
            return None
        trusted = world_protocol.trusted_presence_claim(
            claim.get("name", ""),
            claim.get("accountStatus", "Guest"),
            0,
        )
        nonce = str(claim.get("nonce") or "")
        if not re.fullmatch(r"[A-Za-z0-9_-]{8,32}", nonce):
            return None
        return {
            "scope": scope,
            "version": version,
            "account_bi": account_bi,
            "session_id": session_id,
            "name": trusted["name"] if claim.get("name") else "",
            "accountStatus": trusted["accountStatus"],
            "nonce": nonce,
        }

    def _socket_state(self, ws):
        return {
            field: _ws_attr(ws, field)
            for field in world_protocol.OFFICE_PUBLIC_FIELDS
        }

    def _save_socket(self, ws, state, last, rate_start, rate_count,
                     departed=False, account_bi=None, session_id=None,
                     scope=None, version=None,
                     trusted_name=None, trusted_status=None,
                     ticket_nonce=None, auth_checked_at=None,
                     step_carry=None, pending_steps=None,
                     step_observed_at=None, step_flushed_at=None):
        if account_bi is None:
            account_bi = _ws_attr(ws, "account_bi", "")
        if session_id is None:
            session_id = _ws_attr(ws, "session_id", "")
        if scope is None:
            scope = _ws_attr(ws, "scope", "")
        if version is None:
            version = _ws_attr(ws, "version", 0)
        if trusted_name is None:
            trusted_name = _ws_attr(ws, "trusted_name", "")
        if trusted_status is None:
            trusted_status = _ws_attr(ws, "trusted_status", "Guest")
        if ticket_nonce is None:
            ticket_nonce = _ws_attr(ws, "ticket_nonce", "")
        if auth_checked_at is None:
            auth_checked_at = _ws_attr(ws, "auth_checked_at", 0)
        if step_carry is None:
            step_carry = _ws_attr(ws, "step_carry", 0.0)
        if pending_steps is None:
            pending_steps = _ws_attr(ws, "pending_steps", 0)
        if step_observed_at is None:
            step_observed_at = _ws_attr(ws, "step_observed_at", 0)
        if step_flushed_at is None:
            step_flushed_at = _ws_attr(ws, "step_flushed_at", 0)
        record = {
            **world_protocol.public_office_presence(state),
            "account_bi": str(account_bi or ""),
            "session_id": (
                str(session_id)
                if re.fullmatch(
                    r"[A-Za-z0-9_-]{24,64}", str(session_id or ""))
                else ""),
            "scope": str(scope or ""),
            "version": int(version or 0),
            "trusted_name": str(trusted_name or ""),
            "trusted_status": str(trusted_status or "Guest"),
            "ticket_nonce": (
                str(ticket_nonce)
                if re.fullmatch(
                    r"[A-Za-z0-9_-]{8,32}",
                    str(ticket_nonce or ""))
                else ""),
            "auth_checked_at": int(auth_checked_at or 0),
            "step_carry": max(0.0, float(step_carry or 0.0)),
            "pending_steps": max(0, int(pending_steps or 0)),
            "step_observed_at": max(0, int(step_observed_at or 0)),
            "step_flushed_at": max(0, int(step_flushed_at or 0)),
            "last": int(last or 0),
            "rl_start": int(rate_start or 0),
            "rl_count": int(rate_count or 0),
            "departed": bool(departed),
        }
        try:
            ws.serializeAttachment(to_js(record))
            return True
        except Exception:
            return False

    async def _flush_steps(self, ws, now, force=False):
        """Persist Office walking into the same route-free World aggregate."""
        account_bi = str(_ws_attr(ws, "account_bi", "") or "")
        pending = max(0, int(_ws_attr(ws, "pending_steps", 0) or 0))
        flushed_at = max(0, int(_ws_attr(ws, "step_flushed_at", 0) or 0))
        if not re.fullmatch(r"[a-f0-9]{64}", account_bi) or pending <= 0:
            return
        if (
            not force
            and pending < world_protocol.WORLD_STEP_FLUSH_COUNT
            and int(now) - flushed_at < world_protocol.WORLD_STEP_FLUSH_MS
        ):
            return
        try:
            row = await d1_first(
                self.env,
                "INSERT INTO world_user_activity "
                "(account_bi,total_steps,updated_at) VALUES (?,?,?) "
                "ON CONFLICT(account_bi) DO UPDATE SET "
                "total_steps=MIN(?,world_user_activity.total_steps+"
                "excluded.total_steps),updated_at=MAX("
                "world_user_activity.updated_at,excluded.updated_at) "
                "RETURNING total_steps",
                account_bi, pending, int(now),
                world_protocol.WORLD_STEP_MAX_TOTAL,
            )
            total = int((row or {}).get("total_steps", -1))
        except Exception:
            return
        if total < 0:
            return
        self._save_socket(
            ws,
            self._socket_state(ws),
            last=_ws_attr(ws, "last", now),
            rate_start=_ws_attr(ws, "rl_start", now),
            rate_count=_ws_attr(ws, "rl_count", 0),
            departed=bool(_ws_attr(ws, "departed", False)),
            pending_steps=0,
            step_flushed_at=int(now),
        )
        self._safe_send(ws, {
            "type": "step-total",
            "totalSteps": max(
                0, min(world_protocol.WORLD_STEP_MAX_TOTAL, total)),
        })

    def _mark_departed(self, ws):
        self._save_socket(
            ws,
            self._socket_state(ws),
            last=_ws_attr(ws, "last", 0),
            rate_start=_ws_attr(ws, "rl_start", 0),
            rate_count=_ws_attr(ws, "rl_count", 0),
            departed=True,
        )

    def _live_sockets(self, cleanup=False):
        now = int(Date.now())
        try:
            sockets = self.ctx.getWebSockets("office")
        except Exception:
            sockets = []
        live = []
        stale_ids = []
        for socket in sockets:
            if bool(_ws_attr(socket, "departed", False)):
                if cleanup:
                    self._safe_close(socket, 1000, "")
                continue
            if world_protocol.office_presence_is_stale(
                    _ws_attr(socket, "last", 0), now):
                stale_id = _ws_attr(socket, "id", "")
                if stale_id:
                    stale_ids.append(stale_id)
                if cleanup:
                    self._mark_departed(socket)
                    self._safe_close(socket, 1001, "stale")
                continue
            live.append(socket)
        if cleanup:
            for stale_id in stale_ids:
                for peer in live:
                    self._safe_send(peer, {"type": "leave", "id": stale_id})
        return live

    def _rate_step(self, ws, state, now):
        allowed, start, count = world_protocol.advance_office_rate_window(
            _ws_attr(ws, "rl_start", 0),
            _ws_attr(ws, "rl_count", 0),
            now,
        )
        self._save_socket(
            ws,
            state,
            last=_ws_attr(ws, "last", 0),
            rate_start=start,
            rate_count=count,
            departed=bool(_ws_attr(ws, "departed", False)),
        )
        return allowed, start, count

    def _broadcast_admitted(self, now):
        allowed, start, count = world_protocol.advance_office_broadcast_window(
            getattr(self, "_broadcast_window", 0),
            getattr(self, "_broadcast_count", 0),
            now,
        )
        self._broadcast_window = start
        self._broadcast_count = count
        return allowed

    def _access_recheck_due(self, ws, now):
        try:
            checked_at = int(_ws_attr(ws, "auth_checked_at", 0) or 0)
            now = int(now)
        except (TypeError, ValueError):
            return True
        return (
            checked_at <= 0
            or now < checked_at
            or now - checked_at >= OFFICE_ACCESS_RECHECK_MS
        )

    async def _access_current_if_due(
            self, ws, state, now, rate_start, rate_count):
        """Revalidate once per bounded interval and persist the check time."""
        if not self._access_recheck_due(ws, now):
            return True
        if not await self._access_current(ws, now):
            self._depart(ws, 1008, "room access revoked")
            return False
        self._save_socket(
            ws,
            state,
            last=_ws_attr(ws, "last", 0),
            rate_start=rate_start,
            rate_count=rate_count,
            departed=bool(_ws_attr(ws, "departed", False)),
            auth_checked_at=now,
        )
        return True

    async def _access_current(self, ws, now=None):
        account_bi = str(_ws_attr(ws, "account_bi", "") or "")
        session_id = str(_ws_attr(ws, "session_id", "") or "")
        scope = str(_ws_attr(ws, "scope", "") or "")
        version = int(_ws_attr(ws, "version", 0) or 0)
        now = int(Date.now()) if now is None else int(now)
        if (
            not re.fullmatch(r"[0-9a-f]{64}", account_bi)
            or not re.fullmatch(r"[A-Za-z0-9_-]{24,64}", session_id)
        ):
            return False
        try:
            if session_id.startswith("desktop_"):
                # Desktop admission was authorized by the account-key signature
                # before its HMAC ticket was issued. It has no browser
                # account_sessions row, so recheck the active account directly;
                # channel version/membership are still checked below each time.
                account = await d1_first(
                    self.env,
                    "SELECT data,is_admin FROM users WHERE user_bi=?",
                    account_bi,
                )
            else:
                account = await d1_first(
                    self.env,
                    "SELECT u.data,u.is_admin FROM account_sessions s "
                    "JOIN users u ON u.user_bi=s.account_bi "
                    "WHERE s.session_id=? AND s.account_bi=? "
                    "AND s.revoked_at=0 AND s.expires_at>?",
                    session_id,
                    account_bi,
                    now,
                )
            if not account:
                return False
            account_record = await decrypt_row(self.env, account.get("data"))
            if (
                not account_record
                or account_record.get("status") != "active"
                or _account_kind(account_record) != "user"
            ):
                return False
            if scope == "world-general":
                return True
            channel = await d1_first(
                self.env,
                "SELECT data,created_by_bi,key_version FROM chat_channels "
                "WHERE channel_id=?",
                scope,
            )
            if (
                not channel
                or int(channel.get("key_version") or 0) != version
            ):
                return False
            channel_record = await decrypt_row(
                self.env, channel.get("data"))
            if (
                isinstance(channel_record, dict)
                and channel_record.get("visibility") == "public"
            ):
                return True
            if hmac.compare_digest(
                str(channel.get("created_by_bi") or ""), account_bi
            ):
                return True
            if (
                isinstance(channel_record, dict)
                and channel_record.get("kind") == "notices"
            ):
                return False
            membership = await d1_first(
                self.env,
                "SELECT 1 AS allowed FROM chat_channel_members "
                "WHERE channel_id=? AND member_bi=?",
                scope,
                account_bi,
            )
            return bool(membership)
        except Exception:
            return False

    async def webSocketMessage(self, ws, message):
        if _maintenance_socket_closed(self.env, ws):
            return
        if bool(_ws_attr(ws, "departed", False)):
            self._safe_close(ws, 1000, "")
            return
        if not isinstance(message, str):
            self._safe_close(ws, 1003, "text frames only")
            return
        if len(message.encode("utf-8")) > world_protocol.OFFICE_MESSAGE_MAX_BYTES:
            self._safe_close(ws, 1009, "message too large")
            return
        durable_object_traffic_note(
            self, bytes_in=len(message.encode("utf-8")), messages=1)
        await durable_object_traffic_flush(self)
        state = self._socket_state(ws)
        if not state.get("id"):
            self._safe_close(ws, 1008, "invalid presence")
            return
        now = int(Date.now())
        if world_protocol.office_presence_is_stale(
                _ws_attr(ws, "last", 0), now):
            self._depart(ws, 1001, "stale")
            return
        allowed, rate_start, rate_count = self._rate_step(ws, state, now)
        if not allowed:
            self._safe_close(ws, 1008, "rate limit")
            return
        if not await self._access_current_if_due(
                ws, state, now, rate_start, rate_count):
            return
        self._live_sockets(cleanup=True)
        try:
            payload = json.loads(message)
        except Exception:
            return
        if isinstance(payload, dict) and payload.get("type") == "seat-request":
            requested = str(payload.get("chairId") or "")
            occupied = {
                str(_ws_attr(peer, "chairId", "")): str(
                    _ws_attr(peer, "id", ""))
                for peer in self._live_sockets(cleanup=True)
                if peer is not ws and _ws_attr(peer, "chairId", "")
            }
            result, next_state = world_protocol.allocate_office_seat(
                state, requested, occupied)
            if result == "invalid":
                return
            if result == "denied":
                self._safe_send(ws, {
                    "type": "seat-denied",
                    "chairId": requested,
                    "message": "Seat just taken.",
                })
                return
            next_state["updatedAt"] = now
            self._save_socket(
                ws,
                next_state,
                last=now,
                rate_start=rate_start,
                rate_count=rate_count,
            )
            self._broadcast({
                "type": "seat",
                "participant": world_protocol.public_office_presence(
                    next_state),
            }, budgeted=True)
            return
        sanitized = world_protocol.sanitize_office_message(
            payload,
            state,
            now,
            trusted_name=_ws_attr(ws, "trusted_name", ""),
            trusted_account_status=_ws_attr(
                ws, "trusted_status", "Guest"),
        )
        if sanitized is None:
            return
        kind, next_state = sanitized
        step_carry = _ws_attr(ws, "step_carry", 0.0)
        pending_steps = _ws_attr(ws, "pending_steps", 0)
        step_observed_at = _ws_attr(ws, "step_observed_at", 0)
        if kind == "move":
            credited, step_carry, step_observed_at = (
                world_protocol.movement_step_credit(
                    state, next_state, step_observed_at, now, step_carry)
            )
            pending_steps = min(
                world_protocol.WORLD_STEP_MAX_TOTAL,
                max(0, int(pending_steps or 0)) + credited,
            )
        self._save_socket(
            ws,
            next_state,
            last=now,
            rate_start=rate_start,
            rate_count=rate_count,
            step_carry=step_carry,
            pending_steps=pending_steps,
            step_observed_at=step_observed_at,
        )
        if kind in ("move", "ping"):
            await self._flush_steps(ws, now)
        if kind == "ping":
            self._safe_send(ws, {"type": "pong", "serverTimeMs": now})
            return
        if kind == "presence":
            frame = {
                "type": "presence",
                "participant": world_protocol.public_office_presence(
                    next_state),
            }
        else:
            frame = world_protocol.office_movement_delta(next_state)
        self._broadcast(
            frame, exclude_id=state.get("id"), budgeted=True)

    async def webSocketClose(self, ws, code, reason, was_clean):
        await self._flush_steps(ws, int(Date.now()), force=True)
        self._depart(ws, 1000, "")
        await durable_object_traffic_flush(self, force=True)

    async def webSocketError(self, ws, error):
        await self._flush_steps(ws, int(Date.now()), force=True)
        self._depart(ws, 1011, "socket error")

    web_socket_message = webSocketMessage
    web_socket_close = webSocketClose
    web_socket_error = webSocketError

    def _depart(self, ws, code, reason):
        participant_id = _ws_attr(ws, "id", "")
        departed = bool(_ws_attr(ws, "departed", False))
        self._mark_departed(ws)
        self._safe_close(ws, code, reason)
        if participant_id and not departed:
            self._broadcast(
                {"type": "leave", "id": participant_id},
                exclude_id=participant_id,
                budgeted=True,
            )

    def _broadcast(self, frame, exclude_id=None, budgeted=False):
        if budgeted and not self._broadcast_admitted(int(Date.now())):
            return
        for peer in self._live_sockets(cleanup=False):
            if exclude_id and _ws_attr(peer, "id", "") == exclude_id:
                continue
            self._safe_send(peer, frame)

    def _safe_send(self, ws, frame):
        payload = json.dumps(frame, separators=(",", ":"))
        try:
            ws.send(payload)
        except Exception:
            self._safe_close(ws, 1011, "unavailable")
            return
        durable_object_traffic_note(
            self, bytes_out=len(payload.encode("utf-8")))

    def _close_all(self, code, reason):
        try:
            peers = self.ctx.getWebSockets("office")
        except Exception:
            peers = []
        for peer in peers:
            self._mark_departed(peer)
            self._safe_close(peer, code, reason)

    def _safe_close(self, ws, code, reason):
        try:
            ws.close(code, reason)
        except Exception:
            pass
