"""Single-process Durable Objects for the CPython host.

Interface (kept small so Phase 3 can swap in a multi-node router):

* ``DurableObjectNamespace(binding, resolve_class, env_getter, host)`` is what
  ``env.FORKMESH_*`` holds: ``idFromName``, ``idFromString``, ``newUniqueId``,
  ``get(id)`` / ``getByName(name)`` -> stub with ``fetch``.
* ``LocalDurableHost`` owns the live instances, one per (class, id), each with
  a ``DurableObjectState`` (``ctx``): in-memory ``storage`` (get/put/delete/
  list, alarms on asyncio timers), the WebSocket Hibernation API
  (``acceptWebSocket``, ``getWebSockets``, attachments, auto-response,
  dispatch to ``webSocketMessage``/``webSocketClose``/``webSocketError``) and
  ``blockConcurrencyWhile``.

``durable_cluster.ClusterDurableHost`` subclasses the host for a multi-node
cluster (rendezvous placement, mesh proxying, rqlite-backed storage); it is
only used when cluster mode is configured (see ``cluster.py``).
"""

from __future__ import annotations

import asyncio
import copy
import hashlib
import os
import time

from . import d1_caller
from . import js as _js
from .ffi import JsArray, JsNull, JsObject

ALARM_MAX_RETRIES = 6


def _nullish(value):
    return value is None or isinstance(value, JsNull)


def _clone(value):
    """Structured clone for storage values."""
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, _js.Uint8Array):
        return _js.Uint8Array.new(value)
    if isinstance(value, _js.ArrayBuffer):
        return _js.ArrayBuffer(bytes(value._data))
    if isinstance(value, JsObject):
        return JsObject((k, _clone(v)) for k, v in value.items())
    if isinstance(value, dict):
        return {k: _clone(v) for k, v in value.items()}
    if isinstance(value, JsArray):
        return JsArray(_clone(v) for v in value)
    if isinstance(value, (list, tuple)):
        return [_clone(v) for v in value]
    return copy.deepcopy(value)


class DurableObjectId:
    __slots__ = ("name", "_hex", "binding")

    def __init__(self, binding, hex_id, name=None):
        self.binding = binding
        self._hex = hex_id
        self.name = name

    def toString(self):
        return self._hex

    __str__ = toString

    def equals(self, other):
        return isinstance(other, DurableObjectId) and other._hex == self._hex

    def __eq__(self, other):
        return self.equals(other)

    def __hash__(self):
        return hash(self._hex)

    def __repr__(self):
        return "DurableObjectId(%s, %r)" % (self.binding, self.name)


class DurableObjectStorage:
    """In-memory transactional-enough storage with alarms."""

    def __init__(self, state):
        self._state = state
        self._data: dict = {}
        self._alarm_at = None
        self._alarm_handle = None
        self.sql = None

    async def get(self, key, options=None):
        del options
        if isinstance(key, (list, tuple)):
            return {k: _clone(self._data[k]) for k in key if k in self._data}
        value = self._data.get(key)
        return None if value is None else _clone(value)

    async def put(self, key, value=None, options=None):
        del options
        if isinstance(key, dict):
            for k, v in key.items():
                self._data[str(k)] = _clone(v)
            return None
        if value is None:
            raise TypeError("put() needs a value")
        self._data[str(key)] = _clone(value)
        return None

    async def delete(self, key, options=None):
        del options
        if isinstance(key, (list, tuple)):
            count = 0
            for k in key:
                if self._data.pop(k, None) is not None:
                    count += 1
            return count
        return self._data.pop(key, None) is not None

    async def deleteAll(self, options=None):
        del options
        self._data.clear()
        await self.deleteAlarm()

    async def list(self, options=None):
        prefix = str(_js._opt(options, "prefix", "") or "")
        start = _js._opt(options, "start")
        start_after = _js._opt(options, "startAfter")
        end = _js._opt(options, "end")
        limit = _js._opt(options, "limit")
        reverse = bool(_js._opt(options, "reverse", False))
        keys = sorted(k for k in self._data if k.startswith(prefix))
        if start is not None:
            keys = [k for k in keys if k >= start]
        if start_after is not None:
            keys = [k for k in keys if k > start_after]
        if end is not None:
            keys = [k for k in keys if k < end]
        if reverse:
            keys.reverse()
        if limit is not None:
            keys = keys[:int(limit)]
        return {k: _clone(self._data[k]) for k in keys}

    async def transaction(self, closure):
        return await closure(self)

    async def sync(self):
        return None

    # -- alarms --------------------------------------------------------
    async def getAlarm(self, options=None):
        del options
        return self._alarm_at

    async def setAlarm(self, scheduled_time, options=None):
        del options
        if isinstance(scheduled_time, _js.JsDate):
            scheduled_time = scheduled_time.getTime()
        when = int(float(scheduled_time))
        self._alarm_at = when
        self._arm(when, 0)

    async def deleteAlarm(self, options=None):
        del options
        self._alarm_at = None
        if self._alarm_handle is not None:
            self._alarm_handle.cancel()
            self._alarm_handle = None

    def _arm(self, when, retry):
        if self._alarm_handle is not None:
            self._alarm_handle.cancel()
        loop = asyncio.get_running_loop()
        delay = max(0.0, (when - _js.Date.now()) / 1000.0)
        self._alarm_handle = loop.call_later(
            delay, lambda: _js._spawn(self._fire(when, retry)))

    async def _fire(self, when, retry):
        if self._alarm_at != when:
            return
        self._alarm_handle = None
        # The runtime clears the alarm before delivery; the handler may set a
        # new one, which must survive.
        self._alarm_at = None
        instance = self._state._instance
        handler = getattr(instance, "alarm", None)
        if not await self._alarm_fired(when) or handler is None:
            return
        try:
            await self._state._gate_wait()
            with d1_caller.scope("alarm " + type(instance).__name__):
                await handler(JsObject(retryCount=retry, isRetry=retry > 0))
        except Exception as error:
            print("durable object alarm failed (%s): %r"
                  % (self._state.id, error))
            if retry < ALARM_MAX_RETRIES and self._alarm_at is None:
                backoff = min(60_000, 2000 * (2 ** retry))
                self._alarm_at = _js.Date.now() + backoff
                await self._alarm_retry(self._alarm_at)
                self._arm(self._alarm_at, retry + 1)

    # Hooks for a replicated storage (durable_cluster.ReplicatedStorage).
    async def _alarm_fired(self, when):
        """Called before delivery; False skips the handler."""
        del when
        return True

    async def _alarm_retry(self, when):
        del when

    def _close(self):
        """Cancel timers when the instance is dropped (storage is kept)."""
        if self._alarm_handle is not None:
            self._alarm_handle.cancel()
            self._alarm_handle = None


class DurableObjectState:
    """The ``ctx`` passed to ``DurableObject.__init__(ctx, env)``."""

    def __init__(self, object_id, host, storage_factory=None):
        self.id = object_id
        self._host = host
        self.storage = (storage_factory or DurableObjectStorage)(self)
        self._instance = None
        self._sockets: list = []
        self._auto_response = None
        self._gate = None
        self._gate_owner = None
        self.last_used = time.monotonic()

    # -- hibernation API -----------------------------------------------
    def acceptWebSocket(self, ws, tags=None):
        if ws._hibernation is not None:
            return
        ws._hibernation = (self, [str(t) for t in (tags or [])])
        ws._accepted = True
        ws.readyState = _js.OPEN
        self._sockets.append(ws)

    def getWebSockets(self, tag=None):
        out = []
        for ws in self._sockets:
            if ws.readyState in (_js.CLOSED,):
                continue
            if tag is None or _nullish(tag) or str(tag) in ws._hibernation[1]:
                out.append(ws)
        return JsArray(out)

    def getTags(self, ws):
        return JsArray(ws._hibernation[1] if ws._hibernation else [])

    def setWebSocketAutoResponse(self, pair=None):
        self._auto_response = None if _nullish(pair) else pair

    def getWebSocketAutoResponse(self):
        return self._auto_response

    def getHibernatableWebSocketEventTimeout(self):
        return None

    def setHibernatableWebSocketEventTimeout(self, timeout=None):
        del timeout

    def _forget_socket(self, ws):
        try:
            self._sockets.remove(ws)
        except ValueError:
            pass

    async def _dispatch_message(self, ws, data):
        handler = getattr(self._instance, "webSocketMessage", None)
        if handler is None:
            return
        await self._gate_wait()
        if not isinstance(data, str):
            data = _js.ArrayBuffer(data)
        await handler(ws, data)

    async def _dispatch_close(self, ws, code, reason, clean):
        handler = getattr(self._instance, "webSocketClose", None)
        if handler is None:
            return
        await self._gate_wait()
        await handler(ws, code, reason, clean)

    async def _dispatch_error(self, ws, error):
        handler = getattr(self._instance, "webSocketError", None)
        if handler is None:
            return
        await self._gate_wait()
        await handler(ws, error)

    # -- concurrency ---------------------------------------------------
    async def blockConcurrencyWhile(self, callback):
        if self._gate is None:
            self._gate = asyncio.Lock()
        task = asyncio.current_task()
        if self._gate_owner is task:
            result = callback()
            return await result if asyncio.iscoroutine(result) else result
        async with self._gate:
            self._gate_owner = task
            try:
                result = callback()
                if asyncio.iscoroutine(result):
                    result = await asyncio.wait_for(result, 30.0)
                return result
            finally:
                self._gate_owner = None

    async def _gate_wait(self):
        gate = self._gate
        if gate is not None and gate.locked() and \
                self._gate_owner is not asyncio.current_task():
            async with gate:
                pass

    def waitUntil(self, awaitable):
        if asyncio.iscoroutine(awaitable) or asyncio.isfuture(awaitable):
            return _js._spawn(_await(awaitable))
        return None

    def abort(self, reason=None):
        self._host._evict(self, reason)


async def _await(awaitable):
    return await awaitable


class DurableObjectStub:
    def __init__(self, namespace, object_id):
        self._namespace = namespace
        self.id = object_id
        self.name = object_id.name

    async def fetch(self, resource, init=None):
        if isinstance(resource, _js.Request) and _nullish(init):
            request = resource
        else:
            request = _js.Request.new(resource, init)
        return await self._namespace._host.fetch(
            self._namespace, self.id, request)

    def __getattr__(self, name):
        # JS RPC: stub.method(...) calls the instance method.
        if name.startswith("_"):
            raise AttributeError(name)

        async def call(*args, **kwargs):
            return await self._namespace._host.call(
                self._namespace, self.id, name, args, kwargs)
        return call


class DurableObjectNamespace:
    def __init__(self, binding, class_name, host):
        self.binding = binding
        self.class_name = class_name
        self._host = host

    def _hex(self, text):
        return hashlib.sha256(
            ("%s\0%s" % (self.class_name, text)).encode()).hexdigest()

    def idFromName(self, name):
        name = str(name)
        return DurableObjectId(self.binding, self._hex("name:" + name), name)

    def idFromString(self, hex_id):
        known = self._host._ids.get((self.class_name, str(hex_id)))
        return known or DurableObjectId(self.binding, str(hex_id))

    def newUniqueId(self, options=None):
        del options
        return DurableObjectId(self.binding, os.urandom(32).hex())

    def get(self, object_id, options=None):
        del options
        if isinstance(object_id, str):
            object_id = self.idFromName(object_id)
        return DurableObjectStub(self, object_id)

    def getByName(self, name, options=None):
        return self.get(self.idFromName(name), options)

    def jurisdiction(self, name):
        del name
        return self


class LocalDurableHost:
    """Owns every Durable Object instance in this process."""

    def __init__(self, resolve_class, env_getter, idle_evict_seconds=600):
        self._resolve_class = resolve_class
        self._env_getter = env_getter
        self._states: dict = {}
        self._ids: dict = {}
        self.idle_evict_seconds = idle_evict_seconds

    def namespace(self, binding, class_name):
        return DurableObjectNamespace(binding, class_name, self)

    def _state(self, namespace, object_id):
        key = (namespace.class_name, object_id.toString())
        state = self._states.get(key)
        if state is None:
            cls = self._resolve_class(namespace.class_name)
            state = DurableObjectState(
                object_id, self,
                self._storage_factory(namespace.class_name, object_id))
            self._ids[key] = object_id
            state._instance = cls(state, self._env_getter())
            self._states[key] = state
        state.last_used = time.monotonic()
        return state

    def _storage_factory(self, class_name, object_id):
        del class_name, object_id
        return None

    async def fetch(self, namespace, object_id, request):
        state = self._state(namespace, object_id)
        await state._gate_wait()
        response = await state._instance.fetch(request)
        if response is None:
            raise RuntimeError("Durable Object fetch returned no Response")
        return response

    async def call(self, namespace, object_id, name, args, kwargs):
        state = self._state(namespace, object_id)
        await state._gate_wait()
        result = getattr(state._instance, name)(*args, **kwargs)
        return await result if asyncio.iscoroutine(result) else result

    def _evict(self, state, reason=None):
        del reason
        for key, value in list(self._states.items()):
            if value is state:
                del self._states[key]

    def evict_idle(self):
        """Drop instances with no sockets, no alarm and no recent use.

        Storage is in memory in this single-process host, so an object with
        stored keys is kept (evicting it would lose them).
        """
        cutoff = time.monotonic() - self.idle_evict_seconds
        for key, state in list(self._states.items()):
            if (state.last_used < cutoff and not state._sockets
                    and state.storage._alarm_at is None
                    and not state.storage._data):
                del self._states[key]

    @property
    def instances(self):
        return len(self._states)
