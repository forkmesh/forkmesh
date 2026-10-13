"""The ``workers`` SDK classes the app imports: Response, WorkerEntrypoint,
DurableObject (``from workers import DurableObject, Response,
WorkerEntrypoint``).

``workers.Response`` is the Python-side wrapper: keyword constructor
(``status``, ``status_text``/``statusText``, ``headers``, ``web_socket``),
``js_object`` for the underlying JS response, ``headers.items()``. Handing it
a JS Response wraps that response (``Response(hit)`` in entry.py).
"""

from __future__ import annotations

from . import js as _js
from .ffi import JsObject


class Response(_js.Response):
    def __init__(self, body=None, status=200, status_text="", headers=None,
                 web_socket=None, *, statusText=None, **kwargs):
        del kwargs
        if isinstance(body, _js.Response):
            source = body
            super().__init__(source._body, source.status, source.statusText,
                             _js.Headers(source.headers), source.webSocket,
                             source.url, source.redirected)
            return
        init = {
            "status": int(status),
            "statusText": statusText if statusText is not None
            else status_text,
            "headers": headers,
        }
        if web_socket is not None:
            init["webSocket"] = web_socket
        built = _js.Response.new(body, init)
        super().__init__(built._body, built.status, built.statusText,
                         built.headers, built.webSocket)

    @property
    def js_object(self):
        return self

    @classmethod
    def redirect(cls, url, status=302):
        return cls(None, status=status, headers={"location": str(url)})

    def replace_body(self, body):
        return Response(body, status=self.status,
                        status_text=self.statusText,
                        headers=self.headers)


Request = _js.Request
Headers = _js.Headers
FetchResponse = _js.Response


async def fetch(resource, init=None, **kwargs):
    if kwargs:
        init = dict(init or {}, **kwargs)
    return await _js.fetch(resource, init)


class _Base:
    def __init__(self, ctx, env):
        self.ctx = ctx
        self.env = env


class WorkerEntrypoint(_Base):
    """``Default(ctx, env)``: ``self.env`` and ``self.ctx`` are bound."""


class DurableObject(_Base):
    """``ForkMeshRoom(ctx, env)``: ``self.ctx`` is the DurableObjectState."""


class WorkflowEntrypoint(_Base):
    pass


class ExecutionContext:
    """``ctx`` for fetch/scheduled/queue: waitUntil + passThroughOnException."""

    def __init__(self, tracker=None):
        self._tracker = tracker
        self.props = JsObject()

    def waitUntil(self, awaitable):
        import asyncio
        if asyncio.iscoroutine(awaitable) or asyncio.isfuture(awaitable):
            task = asyncio.ensure_future(awaitable)
            if self._tracker is not None:
                self._tracker(task)
            return task
        return None

    def passThroughOnException(self):
        return None
