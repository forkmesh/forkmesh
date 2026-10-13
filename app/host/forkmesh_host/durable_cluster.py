"""Durable Objects across a cluster of hosts (Phase 3).

``ClusterDurableHost`` extends the single-process ``LocalDurableHost``:

* placement: ``cluster.Membership.owner`` (rendezvous hash over the live
  members; the cron coordinator and CronWatchdog rotate round-robin across
  them every cron minute; the fetch-only ingress objects on every node).
  No object depends on the rqlite leader;
* routing: an object owned here runs locally, anything else is proxied to
  the owner's mesh listener (HTTP relayed, WebSocket frames bridged);
* state: ``ReplicatedStorage`` keeps ``ctx.storage`` in rqlite
  ``do_storage`` with a write-through cache per instance, alarms in
  ``do_alarms`` plus an asyncio timer on the owner;
* ownership change / quorum loss: instances no longer owned here close their
  sockets with 1012 and are dropped; a scan re-arms alarms for objects this
  node now owns -- at once on every view change and at every cron hand-off
  instant, and every 10 s as a safety net;
* exactly-once alarms: an alarm is delivered only by the node whose
  ``DELETE FROM do_alarms ... AND at_ms=?`` actually removed the row (rqlite
  serializes it), so two nodes that briefly disagree about the owner cannot
  both run it. A dropped rotating object's storage refuses further writes
  (a fence), so a stale owner cannot start another cron phase either.

The mesh listener is a second uvicorn ``Server`` in the same event loop.
"""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
import urllib.parse

import httpx

from . import cluster as _cluster
from . import d1_caller
from . import error_log
from . import js as _js
from .asgi import HOP_BY_HOP, HostASGI, _RESERVED_CLOSE
from .durable import (DurableObjectId, DurableObjectStorage,
                      LocalDurableHost, _clone)
from .ffi import JsException, JsObject

ID_PREFIX = "#id:"
MESH_STATE_HEADER = "x-forkmesh-mesh"
JOIN_WAIT_SECONDS = 2.0
# app/src/entry.py CRON_TICK_STATE_KEY: the coordinator's persisted tick.
CRON_TICK_STATE_KEY = "tick_state_v5"
CRON_RUN_RETAIN_MS = 24 * 60 * 60 * 1000


def _rows_affected(results):
    try:
        return int((results or [{}])[0].get("rows_affected") or 0)
    except (AttributeError, IndexError, TypeError, ValueError):
        return 0
_INTERNAL = ("x-fm-do-url", "x-fm-do-id", "x-fm-do-cf")
# Mesh body caps (audit M4). Control requests are small; a forwarded Durable
# Object fetch may carry a public request body (capped at the public edge).
MESH_MAX_BODY = 4 * 1024 * 1024
MESH_MAX_DO_BODY = _cluster._env_int("FORKMESH_MESH_MAX_DO_BODY",
                                     32 * 1024 * 1024)


def object_name(object_id):
    name = getattr(object_id, "name", None)
    return str(name) if name is not None else ID_PREFIX + object_id.toString()


class ReplicatedStorage(DurableObjectStorage):
    """``ctx.storage`` on rqlite with a write-through cache."""

    def __init__(self, state, host, ns, name):
        super().__init__(state)
        self._host = host
        self._rq = host.membership.rqlite
        self._ns = ns
        self._name = name
        self._loaded = False
        self._load_lock = asyncio.Lock()
        self._write_lock = asyncio.Lock()
        self._closed = False
        self._fenced = _cluster.placement(ns, name) == "rotate"

    async def _load(self):
        if self._loaded:
            return
        async with self._load_lock:
            if self._loaded:
                return
            # Leader read: a new owner must see the old owner's last write.
            rows = await self._rq.query(
                "SELECT key, value FROM do_storage WHERE ns=? AND name=?",
                self._ns, self._name, level="weak")
            alarms = await self._rq.query(
                "SELECT at_ms FROM do_alarms WHERE ns=? AND name=?",
                self._ns, self._name, level="weak")
            self._data = {row["key"]: _cluster.decode_value(row["value"])
                          for row in rows}
            self._loaded = True
            self._host.stats["storage_loads"] += 1
            if alarms and not self._closed and self._alarm_at is None:
                self._alarm_at = int(alarms[0]["at_ms"])
                self._arm(self._alarm_at, 0)

    async def _write(self, statements):
        if self._closed and self._fenced:
            # This node no longer owns the object (cron hand-off): refuse
            # the write so a stale owner cannot advance the new owner's
            # state or start another phase.
            raise JsException("Durable Object %s/%s moved to another node"
                              % (self._ns, self._name), "Error")
        async with self._write_lock:
            results = await self._rq.execute(statements)
        self._host.stats["storage_writes"] += 1
        return results

    def _put_statement(self, key, value):
        return ["INSERT INTO do_storage(ns, name, key, value) "
                "VALUES(?, ?, ?, ?) ON CONFLICT(ns, name, key) "
                "DO UPDATE SET value=excluded.value",
                self._ns, self._name, key, _cluster.encode_value(value)]

    async def get(self, key, options=None):
        await self._load()
        return await super().get(key, options)

    async def list(self, options=None):
        await self._load()
        return await super().list(options)

    async def put(self, key, value=None, options=None):
        await self._load()
        if isinstance(key, dict):
            items = {str(k): _clone(v) for k, v in key.items()}
        else:
            if value is None:
                raise TypeError("put() needs a value")
            items = {str(key): _clone(value)}
        statements = [self._put_statement(k, v) for k, v in items.items()]
        self._data.update(items)
        if statements:
            await self._write(statements)
        return None

    async def delete(self, key, options=None):
        await self._load()
        keys = [str(k) for k in key] if isinstance(key, (list, tuple)) \
            else [str(key)]
        present = [k for k in keys if self._data.pop(k, None) is not None]
        if present:
            await self._write([
                ["DELETE FROM do_storage WHERE ns=? AND name=? AND key=?",
                 self._ns, self._name, k] for k in present])
        if isinstance(key, (list, tuple)):
            return len(present)
        return bool(present)

    async def deleteAll(self, options=None):
        del options
        await self._load()
        self._data.clear()
        await super().deleteAlarm()
        await self._write([
            ["DELETE FROM do_storage WHERE ns=? AND name=?",
             self._ns, self._name],
            ["DELETE FROM do_alarms WHERE ns=? AND name=?",
             self._ns, self._name]])

    async def getAlarm(self, options=None):
        await self._load()
        return await super().getAlarm(options)

    async def setAlarm(self, scheduled_time, options=None):
        await self._load()
        if isinstance(scheduled_time, _js.JsDate):
            scheduled_time = scheduled_time.getTime()
        when = int(float(scheduled_time))
        await self._write([self._alarm_statement(when)])
        await super().setAlarm(when, options)

    async def deleteAlarm(self, options=None):
        await self._load()
        had = self._alarm_at is not None
        await super().deleteAlarm(options)
        if had:
            await self._write([
                ["DELETE FROM do_alarms WHERE ns=? AND name=?",
                 self._ns, self._name]])

    def _alarm_statement(self, when):
        return ["INSERT INTO do_alarms(ns, name, at_ms) VALUES(?, ?, ?) "
                "ON CONFLICT(ns, name) DO UPDATE SET at_ms=excluded.at_ms",
                self._ns, self._name, int(when)]

    def _arm(self, when, retry):
        if self._closed:
            return
        super()._arm(when, retry)

    async def _alarm_fired(self, when):
        if self._closed or not self._host.owns(self._ns, self._name):
            return False
        try:
            results = await self._write([
                ["DELETE FROM do_alarms WHERE ns=? AND name=? AND at_ms=?",
                 self._ns, self._name, int(when)]])
        except Exception as error:
            # No quorum: leave the row for whichever node owns it next.
            print("alarm row clear failed (%s/%s): %r"
                  % (self._ns, self._name, error), flush=True)
            return False
        if not _rows_affected(results):
            # Another node claimed (and ran) this alarm first, or it was
            # moved/cancelled meanwhile: never deliver it twice.
            self._host.stats["alarms_skipped"] += 1
            return False
        self._host.stats["alarms_fired"] += 1
        return True

    async def _fire(self, when, retry):
        if (self._ns, self._name) != _cluster.CRON_COORDINATOR:
            return await super()._fire(when, retry)
        before = self._data.get(CRON_TICK_STATE_KEY)
        try:
            return await super()._fire(when, retry)
        finally:
            after = self._data.get(CRON_TICK_STATE_KEY)
            if after is not None and after != before:
                await self._host.record_cron_run(after, before)

    async def _alarm_retry(self, when):
        with contextlib.suppress(Exception):
            await self._write([self._alarm_statement(when)])

    def _close(self):
        self._closed = True
        super()._close()


class _RelayEnd(_js.WebSocket):
    """Server end of a local WebSocketPair whose peer lives on the owner."""

    def __init__(self, conn):
        super().__init__("server")
        self.readyState = _js.OPEN
        self._accepted = True
        self._conn = conn
        self._outbox = asyncio.Queue()

    def _deliver(self, event):
        self._outbox.put_nowait(event)


async def _relay(conn, client, relay, stats):
    """Bridge frames between the mesh connection and the local client end."""
    from websockets.exceptions import ConnectionClosed

    async def upstream():
        while True:
            event = await relay._outbox.get()
            if event[0] == "message":
                await conn.send(event[1])
            elif event[0] == "close":
                code = int(event[1] or 1000)
                if code in _RESERVED_CLOSE or code < 1000:
                    code = 1000
                await conn.close(code, str(event[2] or "")[:120])
                return

    writer = asyncio.ensure_future(upstream())
    try:
        async for message in conn:
            stats["relayed_frames"] += 1
            client._deliver(("message", message))
    except ConnectionClosed:
        pass
    except Exception as error:
        print("mesh websocket relay failed: %r" % (error,), flush=True)
    finally:
        writer.cancel()
    code = conn.close_code
    reason = conn.close_reason or ""
    if code is None or code in _RESERVED_CLOSE:
        # The owner vanished: tell the client to reconnect elsewhere.
        code, reason = 1012, "durable object owner unreachable"
    relay.readyState = _js.CLOSED
    if client.readyState != _js.CLOSED:
        client._deliver(("close", code, reason, True))


class ClusterDurableHost(LocalDurableHost):
    def __init__(self, resolve_class, env_getter, config, idle_evict_seconds=600):
        super().__init__(resolve_class, env_getter, idle_evict_seconds)
        self.config = config
        self.node_id = config.node_id
        self.membership = _cluster.Membership(config, self._on_view_change)
        self.verifier = _cluster.Verifier(config.secret)
        self._namespaces: dict = {}
        # vault.Vault.serve_peer when peer unseal is on (mesh /_vault/...).
        self.peer_unseal_handler = None
        self._http = None
        self._mesh_server = None
        self._mesh_task = None
        self._scan_task = None
        self.stats = {"local_fetches": 0, "proxied_fetches": 0,
                      "proxied_websockets": 0, "mesh_requests": 0,
                      "mesh_rejected": 0, "relayed_frames": 0,
                      "dropped_instances": 0, "alarms_fired": 0,
                      "alarms_skipped": 0, "alarms_rearmed": 0,
                      "storage_loads": 0, "storage_writes": 0,
                      "rotations": 0, "cron_runs": 0, "drains": 0}
        self.cron_runs: list = []          # recent (slot, node) this node ran
        self._rotation_task = None

    # -- namespaces / ids ----------------------------------------------
    def namespace(self, binding, class_name):
        namespace = super().namespace(binding, class_name)
        self._namespaces[class_name] = namespace
        return namespace

    def _id_for(self, namespace, name, hex_id=None):
        if name.startswith(ID_PREFIX):
            hex_id = hex_id or name[len(ID_PREFIX):]
            known = self._ids.get((namespace.class_name, hex_id))
            return known or DurableObjectId(namespace.binding, hex_id)
        return namespace.idFromName(name)

    def owns(self, ns, name):
        return (self.membership.hosting
                and self.membership.owner(ns, name) == self.node_id)

    def _storage_factory(self, class_name, object_id):
        name = object_name(object_id)
        if _cluster.placement(class_name, name) == "local":
            return None
        return lambda state: ReplicatedStorage(state, self, class_name, name)

    async def _local_state(self, namespace, object_id):
        state = self._state(namespace, object_id)
        if isinstance(state.storage, ReplicatedStorage):
            await state.storage._load()
        return state

    # -- lifecycle -----------------------------------------------------
    def start(self):
        loop = asyncio.get_running_loop()
        self._http = httpx.AsyncClient(
            timeout=httpx.Timeout(60.0, connect=3.0),
            limits=httpx.Limits(max_connections=64,
                                max_keepalive_connections=16))
        self.membership.start()
        self._mesh_task = loop.create_task(self._serve_mesh())
        self._scan_task = loop.create_task(self._alarm_scan_loop())
        self._rotation_task = loop.create_task(self._rotation_loop())

    async def _serve_mesh(self):
        import uvicorn

        class _MeshServer(uvicorn.Server):
            @contextlib.contextmanager
            def capture_signals(self):
                # The main server owns SIGINT/SIGTERM.
                yield

        host, _, port = self.config.mesh_listen.rpartition(":")
        config = uvicorn.Config(
            MeshASGI(self), host=host.strip("[]") or "127.0.0.1",
            port=int(port), lifespan="off", ws="websockets-sansio",
            log_level="warning", access_log=False, server_header=False,
            proxy_headers=False, ws_max_size=2 * 1024 * 1024,
            timeout_keep_alive=30)
        self._mesh_server = _MeshServer(config)
        await self._mesh_server.serve()

    async def drain(self):
        membership = self.membership
        if membership.replica or not membership.hosting \
                or self.node_id not in membership.active:
            return False
        try:
            await asyncio.wait_for(membership.leave(), _cluster.DRAIN_SECONDS)
        except Exception as error:
            print("cluster drain: leave failed: %r" % (error,), flush=True)
            return False
        self.stats["drains"] += 1
        await asyncio.sleep(_cluster.DRAIN_SECONDS)
        return True

    async def stop(self):
        await self.drain()
        for task in (self._scan_task, self._rotation_task):
            if task is not None:
                task.cancel()
        if self._mesh_server is not None:
            self._mesh_server.should_exit = True
        if self._mesh_task is not None:
            with contextlib.suppress(BaseException):
                await asyncio.wait_for(self._mesh_task, 5)
        for state in list(self._states.values()):
            self._drop(state, 1001, "server shutting down")
        await self.membership.stop()
        if self._http is not None:
            await self._http.aclose()

    async def readiness(self):
        return await self.membership.readiness()

    def set_available(self, available):
        """Sealed (False) / unsealed (True) host: forkmesh_host.vault.

        Sealed, the node heart-beats with status ``sealed`` and never
        hosts, so placement skips it; every instance it holds is dropped
        (their app code needs the vault's secrets). The membership view
        keeps running (it needs no DATA_KEY); nothing else depends on it.
        """
        membership = self.membership
        membership.paused = not available
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            loop = None
        if not available:
            membership.hosting = False
            membership.routing = False
            for state in list(self._states.values()):
                self._drop(state, 1012, "node sealed")
        if loop is not None:
            # Tell the cluster right away ('sealed' / back to 'joining').
            loop.create_task(self._quietly(membership.beat()))

    @staticmethod
    async def _quietly(coroutine):
        try:
            await coroutine
        except Exception as error:
            print("cluster availability update failed: %r" % (error,),
                  flush=True)

    def snapshot(self, ns=None, name=None):
        out = self.membership.snapshot()
        out["stats"] = dict(out["stats"], **self.stats)
        out["instances"] = sorted(
            "%s/%s" % (key[0], object_name(state.id))
            for key, state in self._states.items())
        out["cron"] = dict(out.get("cron") or {},
                           ranHere=list(self.cron_runs[-16:]))
        if ns and name is not None:
            out["owner"] = self.membership.owner(ns, name)
            out["placement"] = _cluster.placement(ns, name)
        return out

    # -- ownership changes ---------------------------------------------
    def _drop(self, state, code=1012, reason="durable object moved"):
        for ws in list(state._sockets):
            with contextlib.suppress(Exception):
                ws.close(code, reason)
        state._sockets.clear()
        state.storage._close()
        for key, value in list(self._states.items()):
            if value is state:
                del self._states[key]
        self.stats["dropped_instances"] += 1

    async def _on_view_change(self):
        for (ns, _hex), state in list(self._states.items()):
            name = object_name(state.id)
            if _cluster.placement(ns, name) == "local":
                continue
            if not self.owns(ns, name):
                self._drop(state)
        if self.membership.hosting:
            await self._scan_alarms()

    async def _alarm_scan_loop(self):
        await self.membership.wait_view(5.0)
        while True:
            await asyncio.sleep(_cluster.ALARM_SCAN_SECONDS)
            try:
                await self._scan_alarms()
            except Exception as error:
                print("alarm scan failed: %r" % (error,), flush=True)

    async def _rotation_loop(self):
        """At every cron hand-off instant: drop rotating objects this node
        no longer owns and re-arm the ones it now owns right away, instead
        of waiting for the periodic scan."""
        await self.membership.wait_view(5.0)
        while True:
            now = _cluster.now_ms()
            wait_ms = _cluster.next_handoff_ms(now) - now
            await asyncio.sleep(max(0.0, wait_ms / 1000.0) + 0.05)
            try:
                await self._on_rotation()
            except Exception as error:
                print("cron rotation failed: %r" % (error,), flush=True)

    async def _on_rotation(self):
        self.stats["rotations"] += 1
        for (ns, _hex), state in list(self._states.items()):
            name = object_name(state.id)
            if _cluster.placement(ns, name) == "rotate" and \
                    not self.owns(ns, name):
                self._drop(state)
        view = self.membership.cron_view()
        if view["owner"] == self.node_id:
            print("cron: minute index %d is owned by %s (next: %s)"
                  % (view["index"], self.node_id, view["nextOwner"]),
                  flush=True)
        if self.membership.hosting:
            await self._scan_alarms()

    async def record_cron_run(self, raw_state, raw_before=None):
        """Audit which node ran (part of) which cron minute: a log line and
        a host_cron_runs row (migration 0196) with the phases this node ran
        in it. A tick handed over mid-minute (continuation alarm on the new
        owner) shows up as two rows with disjoint phases. Best effort."""
        try:
            state = json.loads(str(raw_state))
            slot = int(state.get("slot"))
        except (TypeError, ValueError):
            return
        try:
            before = json.loads(str(raw_before)) if raw_before else {}
        except (TypeError, ValueError):
            before = {}
        already = set((before.get("ran") or {}).keys()) | set(
            (before.get("failed") or {}).keys()) \
            if before.get("slot") == slot else set()
        now = _cluster.now_ms()
        self.stats["cron_runs"] += 1
        first = slot not in self.cron_runs
        if first:
            self.cron_runs = (self.cron_runs + [slot])[-64:]
        ran = sorted((state.get("ran") or {}).keys())
        mine = sorted((set(ran) | set((state.get("failed") or {}).keys()))
                      - already)
        done = bool(state.get("completedAt"))
        if first or done:
            print("cron minute %d ran on %s (%d phases%s)" % (
                slot, self.node_id, len(ran), ", done" if done else ""),
                flush=True)
        statements = [[
            "INSERT INTO host_cron_runs(ns, name, slot, node_id, first_ms, "
            "last_ms, invocations, phases) VALUES(?, ?, ?, ?, ?, ?, 1, ?) "
            "ON CONFLICT(ns, name, slot, node_id) DO UPDATE SET "
            "last_ms=excluded.last_ms, invocations=invocations+1, "
            "phases=CASE WHEN excluded.phases = '' THEN phases "
            "WHEN phases = '' THEN excluded.phases "
            "ELSE phases || ',' || excluded.phases END",
            _cluster.CRON_COORDINATOR[0], _cluster.CRON_COORDINATOR[1],
            slot, self.node_id, now, now, ",".join(mine)[:2000]]]
        if self.stats["cron_runs"] % 60 == 1:
            statements.append(["DELETE FROM host_cron_runs WHERE first_ms < ?",
                               now - CRON_RUN_RETAIN_MS])
        try:
            await self.membership.rqlite.execute(statements,
                                                 transaction=False)
        except Exception as error:
            print("cron run record failed: %r" % (error,), flush=True)

    async def _scan_alarms(self):
        if not self.membership.hosting:
            return
        rows = await self.membership.rqlite.query(
            "SELECT ns, name, at_ms FROM do_alarms")
        for row in rows:
            ns, name = row["ns"], row["name"]
            namespace = self._namespaces.get(ns)
            if namespace is None or not self.owns(ns, name):
                continue
            object_id = self._id_for(namespace, name)
            if (ns, object_id.toString()) in self._states:
                continue
            try:
                await self._local_state(namespace, object_id)
                self.stats["alarms_rearmed"] += 1
            except Exception as error:
                print("alarm re-arm failed (%s/%s): %r"
                      % (ns, name, error), flush=True)

    def evict_idle(self):
        cutoff = time.monotonic() - self.idle_evict_seconds
        for key, state in list(self._states.items()):
            if (state.last_used < cutoff and not state._sockets
                    and state.storage._alarm_at is None):
                state.storage._close()
                del self._states[key]

    # -- routing --------------------------------------------------------
    def _owner(self, ns, name):
        membership = self.membership
        kind = _cluster.placement(ns, name)
        if kind == "local":
            return self.node_id
        if not (membership.routing if membership.replica
                else membership.hosting):
            raise JsException(
                "Durable Object unavailable: node %s has no quorum"
                % self.node_id, "Error")
        owner = membership.owner(ns, name)
        if owner is None:
            raise JsException("Durable Object unavailable: no active "
                              "cluster members", "Error")
        return owner

    async def _joining(self):
        membership = self.membership
        if not membership.replica and not membership.hosting:
            await membership.wait_hosting(JOIN_WAIT_SECONDS)

    async def fetch(self, namespace, object_id, request):
        ns, name = namespace.class_name, object_name(object_id)
        await self._joining()
        owner = self._owner(ns, name)
        if owner == self.node_id:
            self.stats["local_fetches"] += 1
            state = await self._local_state(namespace, object_id)
            await state._gate_wait()
            response = await state._instance.fetch(request)
            if response is None:
                raise RuntimeError("Durable Object fetch returned no Response")
            return response
        return await self._remote_fetch(owner, ns, name, object_id, request)

    async def call(self, namespace, object_id, method, args, kwargs):
        ns, name = namespace.class_name, object_name(object_id)
        await self._joining()
        owner = self._owner(ns, name)
        if owner == self.node_id:
            state = await self._local_state(namespace, object_id)
            await state._gate_wait()
            result = getattr(state._instance, method)(*args, **kwargs)
            return await result if asyncio.iscoroutine(result) else result
        body = _cluster.encode_value(
            {"args": list(args), "kwargs": dict(kwargs)}).encode()
        path = "/_rpc/%s/%s/%s" % (urllib.parse.quote(ns, safe=""),
                                   urllib.parse.quote(name, safe=""),
                                   urllib.parse.quote(method, safe=""))
        response = await self._mesh_http(owner, "POST", path, body, [
            ("x-fm-do-id", object_id.toString()),
            ("content-type", "application/json")] + d1_caller.header())
        payload = json.loads(await response.aread() or b"{}")
        await response.aclose()
        if response.status_code != 200 or "error" in payload:
            raise JsException(str(payload.get("error") or
                                  "remote call failed (%d)"
                                  % response.status_code), "Error")
        return _cluster._untag(payload.get("value"))

    async def _mesh_http(self, owner, method, path, body, headers,
                         retry=True):
        url = self.membership.mesh_url(owner)
        if not url:
            raise JsException("Durable Object owner %s has no mesh address"
                              % owner, "Error")
        headers = list(headers) + [(_cluster.AUTH_HEADER, _cluster.sign(
            self.config.secret, method, path, body))]
        request = self._http.build_request(method, url + path,
                                           headers=headers, content=body)
        try:
            response = await self._http.send(request, stream=True)
        except httpx.ConnectError as error:
            moved = await self._moved_owner(owner, path) if retry else None
            if moved is None:
                raise JsException("Network connection lost to Durable "
                                  "Object owner %s: %r" % (owner, error),
                                  "Error")
            return await self._mesh_http(moved, method, path, body,
                                         headers[:-1], retry=False)
        except httpx.HTTPError as error:
            raise JsException("Network connection lost to Durable Object "
                              "owner %s: %r" % (owner, error), "Error")
        if retry and _mesh_retryable(response.status_code, response.headers):
            await response.aclose()
            await asyncio.sleep(_cluster.VIEW_SECONDS)
            new_owner = self._owner(*self._split_path(path))
            if new_owner == self.node_id:
                raise JsException("Durable Object ownership moved here "
                                  "mid-request", "Error")
            return await self._mesh_http(new_owner, method, path, body,
                                         headers[:-1], retry=False)
        return response

    async def _moved_owner(self, owner, path):
        await asyncio.sleep(_cluster.VIEW_SECONDS)
        with contextlib.suppress(JsException):
            new_owner = self._owner(*self._split_path(path))
            if new_owner not in (owner, self.node_id):
                return new_owner
        return None

    @staticmethod
    def _split_path(path):
        parsed = _cluster.parse_mesh_path(path)
        return parsed[1], parsed[2]

    def _mesh_headers(self, object_id, request):
        headers = _cluster.forward_headers(request.headers)
        headers.append(("x-fm-do-url", request.url))
        headers.append(("x-fm-do-id", object_id.toString()))
        cf = getattr(request, "cf", None)
        if cf:
            with contextlib.suppress(Exception):
                headers.append(("x-fm-do-cf", _cluster.encode_value(cf)))
        return headers + d1_caller.header()

    async def _remote_fetch(self, owner, ns, name, object_id, request):
        upgrade = str(request.headers.get("upgrade") or "").lower()
        target = urllib.parse.urlsplit(request.url)
        path = _cluster.do_path(ns, name, target.path or "/")
        if target.query:
            path += "?" + target.query
        if upgrade == "websocket":
            return await self._remote_websocket(owner, path, object_id,
                                                request)
        self.stats["proxied_fetches"] += 1
        body = b"" if request._body is None else await request._read()
        response = await self._mesh_http(
            owner, request.method, path, body,
            self._mesh_headers(object_id, request))
        headers = _js.Headers()
        for key, value in response.headers.multi_items():
            if key.lower() not in HOP_BY_HOP:
                headers.append(key, value)
        length = response.headers.get("content-length")
        if length is not None and int(length) <= 256 * 1024:
            data = await response.aread()
            await response.aclose()
            return _js.Response(data, response.status_code,
                                response.reason_phrase, headers)

        async def chunks():
            try:
                async for chunk in response.aiter_raw():
                    yield chunk
            finally:
                await response.aclose()
        return _js.Response(_js.ReadableStream(chunks()),
                            response.status_code, response.reason_phrase,
                            headers)

    async def _remote_websocket(self, owner, path, object_id, request,
                                retry=True):
        from websockets.asyncio.client import connect
        from websockets.exceptions import InvalidStatus, WebSocketException
        url = self.membership.mesh_url(owner)
        if not url:
            raise JsException("Durable Object owner %s has no mesh address"
                              % owner, "Error")
        headers = self._mesh_headers(object_id, request)
        headers.append((_cluster.AUTH_HEADER, _cluster.sign(
            self.config.secret, "GET", path, b"")))
        protocols = [p.strip() for p in str(
            request.headers.get("sec-websocket-protocol") or "").split(",")
            if p.strip()]
        ws_url = "ws" + url[len("http"):] + path
        try:
            conn = await connect(
                ws_url, additional_headers=headers,
                subprotocols=protocols or None, open_timeout=10,
                max_size=2 * 1024 * 1024, compression=None,
                ping_interval=20, ping_timeout=20, user_agent_header=None,
                close_timeout=5)
        except InvalidStatus as error:
            status = error.response.status_code
            if retry and _mesh_retryable(status, error.response.headers):
                await asyncio.sleep(_cluster.VIEW_SECONDS)
                ns, name = self._split_path(path)
                new_owner = self._owner(ns, name)
                if new_owner == self.node_id:
                    namespace = self._namespaces[ns]
                    return await self.fetch(namespace, object_id, request)
                return await self._remote_websocket(
                    new_owner, path, object_id, request, retry=False)
            headers_out = _js.Headers()
            for key, value in error.response.headers.raw_items():
                if key.lower() not in HOP_BY_HOP:
                    headers_out.append(key, value)
            return _js.Response(bytes(error.response.body or b""), status,
                                "", headers_out)
        except (asyncio.TimeoutError, WebSocketException) as error:
            raise JsException("Network connection lost to Durable Object "
                              "owner %s: %r" % (owner, error), "Error")
        except OSError as error:
            moved = await self._moved_owner(owner, path) if retry else None
            if moved is None:
                raise JsException("Network connection lost to Durable "
                                  "Object owner %s: %r" % (owner, error),
                                  "Error")
            return await self._remote_websocket(moved, path, object_id,
                                                request, retry=False)
        self.stats["proxied_websockets"] += 1
        pair = _js.WebSocketPair()
        client = pair[0]
        relay = _RelayEnd(conn)
        client.peer = relay
        relay.peer = client
        _js._spawn(_relay(conn, client, relay, self.stats))
        out_headers = _js.Headers()
        if conn.subprotocol:
            out_headers.set("sec-websocket-protocol", conn.subprotocol)
        return _js.Response(None, 101, "", out_headers, client)

    # -- mesh listener side ----------------------------------------------
    async def serve_mesh(self, kind, ns, name, rest, request, id_hex=None):
        """Dispatch one authenticated mesh request to a local object."""
        self.stats["mesh_requests"] += 1
        namespace = self._namespaces.get(ns)
        if namespace is None:
            return _text_response(404, "unknown namespace")
        if _cluster.placement(ns, name) == "local":
            return _text_response(400, "local-placement object")
        if not self.membership.hosting:
            response = _text_response(503, "no quorum")
            response.headers.set(MESH_STATE_HEADER, "no-quorum")
            return response
        if self.membership.owner(ns, name) != self.node_id:
            response = _text_response(421, "not the owner")
            response.headers.set("x-forkmesh-owner",
                                 str(self.membership.owner(ns, name)))
            return response
        object_id = self._id_for(namespace, name, id_hex)
        state = await self._local_state(namespace, object_id)
        await state._gate_wait()
        if kind == "_rpc":
            try:
                args = _cluster.decode_value(await request.text() or "{}")
                result = getattr(state._instance, rest)(
                    *(args.get("args") or []), **dict(args.get("kwargs")
                                                      or {}))
                if asyncio.iscoroutine(result):
                    result = await result
                payload = {"value": _cluster._tag(result)}
                status = 200
            except Exception as error:
                payload = {"error": repr(error)}
                status = 500
            return _js.Response(json.dumps(payload).encode(), status, "",
                                _js.Headers({"content-type":
                                             "application/json"}))
        self.stats["local_fetches"] += 1
        response = await state._instance.fetch(request)
        if response is None:
            raise RuntimeError("Durable Object fetch returned no Response")
        return response


def _mesh_retryable(status, headers):
    return status == 421 or (
        status == 503 and headers.get(MESH_STATE_HEADER) == "no-quorum")


def _text_response(status, text):
    return _js.Response(text.encode(), status, "", _js.Headers(
        {"content-type": "text/plain; charset=utf-8"}))


class MeshASGI:
    """The internal listener: ``/_do/...``, ``/_rpc/...`` and the vault's
    ``POST /_vault/peer-unseal`` (vault.py), all HMAC-authenticated."""

    _header_list = staticmethod(HostASGI._header_list)
    _send_response = HostASGI._send_response
    _deny = HostASGI._deny

    def __init__(self, host: ClusterDurableHost):
        self.host = host

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http":
            await self._http(scope, receive, send)
        elif scope["type"] == "websocket":
            with d1_caller.scope(self._header(scope, d1_caller.HEADER)):
                await self._websocket(scope, receive, send)

    @staticmethod
    def _raw_path(scope):
        raw = scope.get("raw_path")
        path = raw.decode("latin-1") if raw else scope.get("path", "/")
        query = scope.get("query_string") or b""
        if query:
            path += "?" + query.decode("latin-1")
        return path

    @staticmethod
    def _header(scope, name):
        name = name.encode()
        for key, value in scope.get("headers") or []:
            if key == name:
                return value.decode("latin-1")
        return None

    def _request(self, scope, method, body):
        headers = _js.Headers()
        for key, value in scope.get("headers") or []:
            key = key.decode("latin-1")
            if key in _INTERNAL or key == _cluster.AUTH_HEADER or \
                    key == "host":
                continue
            headers.append(key, value.decode("latin-1"))
        url = self._header(scope, "x-fm-do-url") or "http://mesh.invalid/"
        host = urllib.parse.urlsplit(url).netloc
        if host:
            headers.set("host", host)
        cf = JsObject()
        raw_cf = self._header(scope, "x-fm-do-cf")
        if raw_cf:
            with contextlib.suppress(Exception):
                cf = _cluster.decode_value(raw_cf)
        return _js.Request(url, method, headers, body, None, cf)

    def _body_limit(self, path):
        """Mesh body cap: control traffic (RPC, peer unseal) 4 MiB; a
        forwarded Durable Object fetch carries a public request body, which
        the public listener already capped, so allow up to that cap."""
        if path.startswith("/_do/"):
            return MESH_MAX_DO_BODY
        return MESH_MAX_BODY

    def _quarantined_source(self, scope):
        """A quarantined node's mesh requests are refused (docs/IDS.md)."""
        client = scope.get("client") or (None, None)
        check = getattr(getattr(self.host, "membership", None),
                        "source_quarantined", None)
        if check is not None and check(client[0]):
            self.host.stats["mesh_rejected"] += 1
            self.host.stats["mesh_quarantined"] = \
                self.host.stats.get("mesh_quarantined", 0) + 1
            return True
        return False

    async def _http(self, scope, receive, send):
        method = scope.get("method", "GET").upper()
        path = self._raw_path(scope)
        if self._quarantined_source(scope):
            await self._send_response(
                send, _text_response(403, "quarantined"), method)
            return
        # Reject bad or stale auth and oversized bodies before reading the
        # body (audit M4); the full HMAC over the body is checked below.
        if not self.host.verifier.precheck(
                self._header(scope, _cluster.AUTH_HEADER)):
            self.host.stats["mesh_rejected"] += 1
            await self._send_response(
                send, _text_response(401, "mesh authentication failed"),
                method)
            return
        limit = self._body_limit(path)
        length = self._header(scope, "content-length")
        if length is not None:
            try:
                too_big = int(length) > limit or int(length) < 0
            except ValueError:
                too_big = True
            if too_big:
                self.host.stats["mesh_rejected"] += 1
                await self._send_response(
                    send, _text_response(413, "mesh body too large"), method)
                return
        chunks = []
        size = 0
        while True:
            message = await receive()
            if message["type"] == "http.disconnect":
                return
            chunk = message.get("body") or b""
            size += len(chunk)
            if size > limit:
                self.host.stats["mesh_rejected"] += 1
                await self._send_response(
                    send, _text_response(413, "mesh body too large"), method)
                return
            chunks.append(chunk)
            if not message.get("more_body"):
                break
        body = b"".join(chunks)
        parsed = _cluster.parse_mesh_path(path)
        if not self.host.verifier.verify(self._header(
                scope, _cluster.AUTH_HEADER), method, path, body):
            self.host.stats["mesh_rejected"] += 1
            await self._send_response(
                send, _text_response(401, "mesh authentication failed"),
                method)
            return
        if path == "/_vault/peer-unseal":
            await self._peer_unseal(scope, send, method, body)
            return
        if parsed is None:
            await self._send_response(send, _text_response(404, "not found"),
                                      method)
            return
        request = self._request(
            scope, method, None if method in ("GET", "HEAD") else body)
        try:
            with d1_caller.scope(self._header(scope, d1_caller.HEADER)):
                response = await self.host.serve_mesh(
                    *parsed, request, self._header(scope, "x-fm-do-id"))
        except Exception:
            error_log.report("mesh request failed on %s -> 500" % (path,))
            response = _text_response(500, "durable object failed")
        await self._send_response(send, response, method)

    async def _peer_unseal(self, scope, send, method, body):
        handler = self.host.peer_unseal_handler
        if handler is None or method != "POST":
            await self._send_response(send, _text_response(404, "not found"),
                                      method)
            return
        client = scope.get("client") or (None, None)
        try:
            status, payload = await handler(client[0], body)
        except Exception as error:
            print("ERROR peer unseal handler failed: %r" % (type(error).__name__,),
                  flush=True)
            status, payload = 500, {"ok": False, "error": "internal"}
        await self._send_response(send, _js.Response(
            json.dumps(payload).encode(), status, "", _js.Headers(
                {"content-type": "application/json",
                 "cache-control": "no-store"})), method)

    async def _websocket(self, scope, receive, send):
        message = await receive()
        if message["type"] != "websocket.connect":
            return
        path = self._raw_path(scope)
        parsed = _cluster.parse_mesh_path(path)
        if self._quarantined_source(scope):
            await self._deny(scope, send, _text_response(403, "quarantined"))
            return
        if not self.host.verifier.verify(self._header(
                scope, _cluster.AUTH_HEADER), "GET", path, b""):
            self.host.stats["mesh_rejected"] += 1
            await self._deny(scope, send,
                             _text_response(401, "mesh authentication failed"))
            return
        if parsed is None or parsed[0] != "_do":
            await self._deny(scope, send, _text_response(404, "not found"))
            return
        request = self._request(scope, "GET", None)
        request.headers.set("upgrade", "websocket")
        protocols = scope.get("subprotocols") or []
        if protocols:
            request.headers.set("sec-websocket-protocol",
                                ", ".join(protocols))
        try:
            response = await self.host.serve_mesh(
                *parsed, request, self._header(scope, "x-fm-do-id"))
        except Exception as error:
            print("mesh websocket failed on %s: %r" % (path, error),
                  flush=True)
            response = _text_response(500, "durable object failed")
        client = getattr(response, "webSocket", None)
        if response.status != 101 or client is None:
            await self._deny(scope, send, response)
            return
        accept = {"type": "websocket.accept"}
        protocol = response.headers.get("sec-websocket-protocol")
        if protocol:
            accept["subprotocol"] = protocol.split(",")[0].strip()
        await send(accept)
        await HostASGI._bridge(client, receive, send)
