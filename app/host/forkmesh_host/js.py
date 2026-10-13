"""The JavaScript global surface the Worker app imports via ``from js import``.

Everything here models the Workers runtime closely enough for the unchanged
app code in app/src and world/src. ``tools/js_surface_inventory.py`` lists
what the app touches; ``app/tests/host/test_js_surface.py`` fails when a name
in that inventory is missing here.

Promises are coroutines: anything the app ``await``s on the Workers runtime is
an ``async def`` here. Byte results (``ArrayBuffer``/``Uint8Array``) expose
``to_py()`` returning a ``memoryview`` like Pyodide, and ``to_bytes()``.
"""

from __future__ import annotations

import asyncio
import collections
import email.utils
import itertools
import math
import os
import re
import time
import urllib.parse
import weakref

try:
    import orjson as _orjson
except Exception:  # pragma: no cover - orjson is a pinned requirement
    _orjson = None
import json as _json

from .ffi import JsArray, JsException, JsNull, JsObject, JsProxy, jsnull
from .ffi import to_py as _to_py

__all__ = [
    "AbortController", "AbortSignal", "ArrayBuffer", "Date", "Headers",
    "JSON", "Object", "ReadableStream", "Request", "Response",
    "TransformStream", "URL", "URLSearchParams", "Uint8Array", "WebSocket",
    "WebSocketPair", "WebSocketRequestResponsePair", "caches",
    "clearTimeout", "console", "crypto", "fetch", "scheduler",
    "setTimeout", "setInterval", "clearInterval",
]


def _nullish(value):
    return value is None or isinstance(value, JsNull)


def _opt(init, name, default=None):
    """Read one option from a JS-ish init object (dict, JsObject, object)."""
    if _nullish(init):
        return default
    if isinstance(init, dict):
        value = init.get(name, default)
    else:
        value = getattr(init, name, default)
    return default if value is None else value


class JsFunction:
    """A callable that also has JS ``bind``/``call``/``apply``."""

    __slots__ = ("_fn",)

    def __init__(self, fn):
        self._fn = fn

    def __call__(self, *args, **kwargs):
        return self._fn(*args, **kwargs)

    def bind(self, this=None, *args):
        del this
        fn = self._fn
        if not args:
            return JsFunction(fn)
        return JsFunction(lambda *more: fn(*args, *more))

    def call(self, this=None, *args):
        del this
        return self._fn(*args)

    def apply(self, this=None, args=()):
        del this
        return self._fn(*list(args or ()))


# --------------------------------------------------------------------------
# console

class _Console:
    @staticmethod
    def _emit(*args):
        print(*[str(a) for a in args], flush=False)

    log = info = warn = error = debug = _emit


console = _Console()


# --------------------------------------------------------------------------
# timers

_timer_ids = itertools.count(1)
_timers: dict[int, asyncio.TimerHandle] = {}


def _run_callback(callback, args):
    try:
        result = callback(*args)
    except Exception as error:  # a JS timer callback cannot raise upward
        print("setTimeout callback failed: %r" % (error,))
        return
    if asyncio.iscoroutine(result):
        _spawn(result)


_background: set = set()


def _spawn(coro):
    task = asyncio.get_running_loop().create_task(coro)
    _background.add(task)
    task.add_done_callback(_background.discard)
    return task


def setTimeout(callback, delay=0, *args):
    loop = asyncio.get_running_loop()
    timer_id = next(_timer_ids)

    def fire():
        _timers.pop(timer_id, None)
        _run_callback(callback, args)

    _timers[timer_id] = loop.call_later(
        max(0.0, float(delay or 0)) / 1000.0, fire)
    return timer_id


def clearTimeout(timer_id):
    if _nullish(timer_id):
        return
    handle = _timers.pop(timer_id, None)
    if handle is not None:
        handle.cancel()


def setInterval(callback, delay=0, *args):
    loop = asyncio.get_running_loop()
    timer_id = next(_timer_ids)
    period = max(1.0, float(delay or 0)) / 1000.0

    def fire():
        if timer_id not in _timers:
            return
        _timers[timer_id] = loop.call_later(period, fire)
        _run_callback(callback, args)

    _timers[timer_id] = loop.call_later(period, fire)
    return timer_id


clearInterval = clearTimeout


class _Scheduler:
    @staticmethod
    async def wait(delay=0, options=None):
        del options
        await asyncio.sleep(max(0.0, float(delay or 0)) / 1000.0)


scheduler = _Scheduler()


# --------------------------------------------------------------------------
# Abort

class AbortSignal(JsProxy):
    def __init__(self):
        self._aborted = False
        self.reason = None
        self._deadline = None
        self._listeners = []
        self._events = []

    @classmethod
    def new(cls):
        return cls()

    @classmethod
    def timeout(cls, delay):
        signal = cls()
        signal._deadline = time.monotonic() + max(0.0, float(delay)) / 1000.0
        return signal

    @classmethod
    def abort(cls, reason=None):
        signal = cls()
        signal._abort(reason)
        return signal

    @classmethod
    def any(cls, signals):
        combined = cls()
        for signal in signals:
            if signal is None:
                continue
            if signal.aborted:
                combined._abort(signal.reason)
                break
            if signal._deadline is not None:
                if combined._deadline is None or \
                        signal._deadline < combined._deadline:
                    combined._deadline = signal._deadline
            signal.addEventListener(
                "abort", lambda *_a, s=signal: combined._abort(s.reason))
        return combined

    @property
    def aborted(self):
        if self._aborted:
            return True
        if self._deadline is not None and time.monotonic() >= self._deadline:
            self._abort(JsException("The operation timed out.",
                                    "TimeoutError"))
            return True
        return False

    def throwIfAborted(self):
        if self.aborted:
            raise self.reason or JsException("aborted", "AbortError")

    def _abort(self, reason=None):
        if self._aborted:
            return
        self._aborted = True
        self.reason = reason if reason is not None else JsException(
            "The operation was aborted.", "AbortError")
        for event in self._events:
            event.set()
        for listener in list(self._listeners):
            try:
                listener(JsObject(type="abort"))
            except Exception:
                pass

    def addEventListener(self, kind, listener, options=None):
        del options
        if kind == "abort":
            self._listeners.append(listener)

    def removeEventListener(self, kind, listener, options=None):
        del options
        if kind == "abort" and listener in self._listeners:
            self._listeners.remove(listener)

    def remaining(self):
        """Seconds until the timeout fires, or None."""
        if self._deadline is None:
            return None
        return max(0.0, self._deadline - time.monotonic())

    async def wait(self):
        """Resolve when aborted (by abort() or by the timeout)."""
        if self.aborted:
            return
        event = asyncio.Event()
        self._events.append(event)
        try:
            remaining = self.remaining()
            if remaining is None:
                await event.wait()
            else:
                try:
                    await asyncio.wait_for(event.wait(), remaining)
                except asyncio.TimeoutError:
                    self.aborted  # noqa: B018 - marks the timeout reason
        finally:
            self._events.remove(event)


class AbortController(JsProxy):
    def __init__(self):
        self.signal = AbortSignal()
        self.abort = JsFunction(self._abort)

    @classmethod
    def new(cls):
        return cls()

    def _abort(self, reason=None):
        self.signal._abort(reason)


async def _race_signal(awaitable, signal):
    """Await ``awaitable`` unless ``signal`` aborts first (then raise)."""
    if signal is None or _nullish(signal):
        return await awaitable
    if signal.aborted:
        if asyncio.iscoroutine(awaitable):
            awaitable.close()
        raise signal.reason
    work = asyncio.ensure_future(awaitable)
    watcher = asyncio.ensure_future(signal.wait())
    try:
        done, _pending = await asyncio.wait(
            {work, watcher}, return_when=asyncio.FIRST_COMPLETED)
    except BaseException:
        work.cancel()
        watcher.cancel()
        raise
    if work in done:
        watcher.cancel()
        return work.result()
    work.cancel()
    raise signal.reason or JsException("aborted", "AbortError")


# --------------------------------------------------------------------------
# Date, JSON, Object

class JsDate(JsProxy):
    __slots__ = ("_ms",)

    def __init__(self, ms=None):
        self._ms = Date.now() if ms is None else ms

    def getTime(self):
        return self._ms

    valueOf = getTime

    def toISOString(self):
        seconds, millis = divmod(int(self._ms), 1000)
        return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(seconds)) + \
            ".%03dZ" % millis

    def to_py(self):
        import datetime
        return datetime.datetime.fromtimestamp(
            self._ms / 1000.0, datetime.timezone.utc)

    def __int__(self):
        return int(self._ms)

    def __float__(self):
        return float(self._ms)


_ISO_RE = re.compile(
    r"^(\d{4})(?:-(\d{2})(?:-(\d{2}))?)?"
    r"(?:[T ](\d{2}):(\d{2})(?::(\d{2})(?:\.(\d{1,9}))?)?)?"
    r"\s*(Z|[+-]\d{2}:?\d{2})?$")


class Date:
    @staticmethod
    def now():
        return time.time_ns() // 1_000_000

    @staticmethod
    def new(value=None):
        if value is None:
            return JsDate()
        if isinstance(value, (int, float)):
            return JsDate(value)
        return JsDate(Date.parse(value))

    @staticmethod
    def parse(text):
        """ISO 8601 / RFC 2822 to epoch ms; NaN when unparseable (like JS)."""
        text = str(text or "").strip()
        match = _ISO_RE.match(text)
        if match:
            year, month, day, hour, minute, sec, frac, zone = match.groups()
            import calendar
            try:
                base = calendar.timegm((
                    int(year), int(month or 1), int(day or 1),
                    int(hour or 0), int(minute or 0), int(sec or 0), 0, 0, 0))
            except (ValueError, OverflowError):
                return math.nan
            ms = base * 1000 + (int((frac or "0")[:3].ljust(3, "0"))
                                if frac else 0)
            if zone and zone != "Z":
                sign = 1 if zone[0] == "+" else -1
                digits = zone[1:].replace(":", "")
                offset = int(digits[:2]) * 60 + int(digits[2:])
                ms -= sign * offset * 60_000
            elif zone is None and hour is not None:
                # JS treats a date-time without offset as local time; the
                # host runs in UTC, which is what the Worker isolates use.
                pass
            return ms
        try:
            parsed = email.utils.parsedate_to_datetime(text)
        except (TypeError, ValueError, IndexError):
            return math.nan
        if parsed is None:
            return math.nan
        return int(parsed.timestamp() * 1000)

    @staticmethod
    def UTC(year, month=0, day=1, hours=0, minutes=0, seconds=0, ms=0):
        import calendar
        return calendar.timegm(
            (year, month + 1, day, hours, minutes, seconds, 0, 0, 0)
        ) * 1000 + ms


def _json_default(value):
    if isinstance(value, JsNull):
        return None
    converter = getattr(value, "to_py", None)
    if callable(converter):
        return converter()
    if isinstance(value, (set, frozenset, tuple)):
        return list(value)
    raise TypeError("not JSON serializable: %r" % type(value))


def _jsonify_tree(value):
    """JSON.parse result: objects become JsObject, arrays JsArray."""
    if isinstance(value, dict):
        return JsObject((k, _jsonify_tree(v)) for k, v in value.items())
    if isinstance(value, list):
        return JsArray(_jsonify_tree(v) for v in value)
    return value


class JSON:
    @staticmethod
    def stringify(value, replacer=None, space=None):
        del replacer
        if _nullish(value):
            return "null" if isinstance(value, JsNull) else None
        if space:
            return _json.dumps(value, default=_json_default,
                               indent=int(space) if str(space).isdigit()
                               else space, ensure_ascii=False)
        if _orjson is not None:
            try:
                return _orjson.dumps(
                    value, default=_json_default,
                    option=_orjson.OPT_NON_STR_KEYS).decode("utf-8")
            except TypeError:
                pass
        return _json.dumps(value, default=_json_default,
                           separators=(",", ":"), ensure_ascii=False)

    @staticmethod
    def parse(text, reviver=None):
        del reviver
        if isinstance(text, (bytes, bytearray, memoryview)):
            text = bytes(text)
        try:
            value = (_orjson.loads(text) if _orjson is not None
                     else _json.loads(text))
        except Exception as error:
            raise JsException(str(error), "SyntaxError") from None
        return _jsonify_tree(value)


def _own_keys(value):
    if isinstance(value, dict):
        return [str(k) for k in value.keys()]
    keys = getattr(value, "__js_keys__", None)
    if callable(keys):
        return list(keys())
    try:
        return [k for k in vars(value) if not k.startswith("_")]
    except TypeError:
        return []


class Object:
    @staticmethod
    def fromEntries(entries):
        if isinstance(entries, dict):
            entries = entries.items()
        return JsObject((str(k) if not isinstance(k, str) else k, v)
                        for k, v in entries)

    @staticmethod
    def keys(value):
        return JsArray(_own_keys(value))

    @staticmethod
    def getOwnPropertyNames(value):
        return JsArray(_own_keys(value))

    @staticmethod
    def entries(value):
        if isinstance(value, dict):
            return JsArray(JsArray([k, v]) for k, v in value.items())
        return JsArray(JsArray([k, getattr(value, k)])
                       for k in _own_keys(value))

    @staticmethod
    def values(value):
        if isinstance(value, dict):
            return JsArray(value.values())
        return JsArray(getattr(value, k) for k in _own_keys(value))

    @staticmethod
    def assign(target, *sources):
        for source in sources:
            if isinstance(source, dict):
                target.update(source)
        return target

    @staticmethod
    def new():
        return JsObject()


# --------------------------------------------------------------------------
# URL

class URLSearchParams(JsProxy):
    def __init__(self, init=""):
        if isinstance(init, str):
            self._pairs = urllib.parse.parse_qsl(
                init[1:] if init.startswith("?") else init,
                keep_blank_values=True)
        elif isinstance(init, dict):
            self._pairs = [(str(k), str(v)) for k, v in init.items()]
        else:
            self._pairs = [(str(k), str(v)) for k, v in (init or [])]
        self._owner = None

    @classmethod
    def new(cls, init=""):
        return cls(init)

    def _changed(self):
        if self._owner is not None:
            text = self.toString()
            self._owner._search = ("?" + text) if text else ""

    def get(self, name):
        for key, value in self._pairs:
            if key == name:
                return value
        return None

    def getAll(self, name):
        return JsArray(v for k, v in self._pairs if k == name)

    def has(self, name):
        return any(k == name for k, _ in self._pairs)

    def set(self, name, value):
        out, done = [], False
        for key, current in self._pairs:
            if key == name:
                if not done:
                    out.append((key, str(value)))
                    done = True
            else:
                out.append((key, current))
        if not done:
            out.append((name, str(value)))
        self._pairs = out
        self._changed()

    def append(self, name, value):
        self._pairs.append((name, str(value)))
        self._changed()

    def delete(self, name):
        self._pairs = [(k, v) for k, v in self._pairs if k != name]
        self._changed()

    def keys(self):
        return iter([k for k, _ in self._pairs])

    def entries(self):
        return iter([JsArray([k, v]) for k, v in self._pairs])

    def __iter__(self):
        return self.entries()

    def toString(self):
        return urllib.parse.urlencode(self._pairs, quote_via=urllib.parse.quote)

    __str__ = toString

    def to_py(self):
        return dict(self._pairs)


_DEFAULT_PORTS = {"http:": "80", "https:": "443", "ws:": "80", "wss:": "443"}


class URL(JsProxy):
    def __init__(self, url, base=None):
        url = str(url)
        if base is not None and not _nullish(base):
            url = urllib.parse.urljoin(str(base), url)
        parts = urllib.parse.urlsplit(url)
        if not parts.scheme or (parts.scheme in ("http", "https", "ws", "wss")
                                and not parts.netloc):
            raise JsException("Invalid URL: %s" % url, "TypeError")
        self.protocol = parts.scheme.lower() + ":"
        self.username = urllib.parse.unquote(parts.username or "")
        self.password = urllib.parse.unquote(parts.password or "")
        self.hostname = (parts.hostname or "").lower()
        try:
            port = parts.port
        except ValueError:
            raise JsException("Invalid URL: %s" % url, "TypeError") from None
        port_text = "" if port is None else str(port)
        if _DEFAULT_PORTS.get(self.protocol) == port_text:
            port_text = ""
        self.port = port_text
        path = parts.path
        if self.protocol in _DEFAULT_PORTS and not path:
            path = "/"
        self.pathname = path
        self._search = ("?" + parts.query) if parts.query else ""
        self.hash = ("#" + parts.fragment) if parts.fragment else ""

    @classmethod
    def new(cls, url, base=None):
        return cls(url, base)

    @staticmethod
    def canParse(url, base=None):
        try:
            URL(url, base)
            return True
        except JsException:
            return False

    @property
    def search(self):
        return self._search

    @search.setter
    def search(self, value):
        value = str(value or "")
        self._search = "" if value in ("", "?") else (
            value if value.startswith("?") else "?" + value)

    @property
    def searchParams(self):
        params = URLSearchParams(self._search)
        params._owner = self
        return params

    @property
    def host(self):
        host = self.hostname
        if ":" in host and not host.startswith("["):
            host = "[" + host + "]"
        return host + (":" + self.port if self.port else "")

    @property
    def origin(self):
        if self.protocol in _DEFAULT_PORTS:
            return self.protocol + "//" + self.host
        return "null"

    @property
    def href(self):
        auth = ""
        if self.username or self.password:
            auth = urllib.parse.quote(self.username, safe="")
            if self.password:
                auth += ":" + urllib.parse.quote(self.password, safe="")
            auth += "@"
        return (self.protocol + "//" + auth + self.host + self.pathname
                + self._search + self.hash)

    def toString(self):
        return self.href

    toJSON = toString

    def __str__(self):
        return self.href

    def to_py(self):
        return self.href


# --------------------------------------------------------------------------
# Binary

def _as_bytes(value):
    """Bytes of any BufferSource-like value (copying)."""
    if isinstance(value, bytes):
        return value
    if isinstance(value, (bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, Uint8Array):
        return value.to_bytes()
    if isinstance(value, ArrayBuffer):
        return bytes(value._data)
    if isinstance(value, str):
        return value.encode("utf-8")
    if isinstance(value, (list, tuple)):
        return bytes(value)
    converter = getattr(value, "to_bytes", None)
    if callable(converter):
        return bytes(converter())
    raise JsException("expected a BufferSource, got %s" % type(value).__name__,
                      "TypeError")


class ArrayBuffer(JsProxy):
    __slots__ = ("_data",)

    def __init__(self, data=b""):
        if isinstance(data, int):
            data = bytearray(data)
        self._data = data if isinstance(data, bytearray) else bytearray(data)

    @classmethod
    def new(cls, length=0):
        return cls(bytearray(int(length)))

    @property
    def byteLength(self):
        return len(self._data)

    def slice(self, begin=0, end=None):
        return ArrayBuffer(bytes(self._data[begin:end]))

    def to_py(self):
        return memoryview(self._data)

    def to_bytes(self):
        return bytes(self._data)

    def to_memoryview(self):
        return memoryview(self._data)

    def __bytes__(self):
        return bytes(self._data)

    def __len__(self):
        return len(self._data)

    def __repr__(self):
        return "ArrayBuffer(%d)" % len(self._data)


class Uint8Array(JsProxy):
    """A view over an ArrayBuffer; ``new`` copies like the JS constructor."""

    __slots__ = ("buffer", "byteOffset", "_length")
    BYTES_PER_ELEMENT = 1

    def __init__(self, buffer, offset=0, length=None):
        self.buffer = buffer
        self.byteOffset = offset
        self._length = (len(buffer._data) - offset) if length is None \
            else length

    @classmethod
    def new(cls, source=0, offset=None, length=None):
        if isinstance(source, ArrayBuffer):
            return cls(source, int(offset or 0),
                       None if length is None else int(length))
        if _nullish(source):
            source = 0
        if isinstance(source, (int, float)) and not isinstance(source, bool):
            return cls(ArrayBuffer(bytearray(int(source))))
        return cls(ArrayBuffer(bytearray(_as_bytes(source))))

    @classmethod
    def of(cls, *values):
        return cls.new(list(values))

    @classmethod
    def from_(cls, values):
        return cls.new(list(values))

    def _view(self):
        return memoryview(self.buffer._data)[
            self.byteOffset:self.byteOffset + self._length]

    @property
    def length(self):
        return self._length

    byteLength = length

    def __len__(self):
        return self._length

    def __getitem__(self, index):
        if isinstance(index, slice):
            return bytes(self._view()[index])
        if index < 0 or index >= self._length:
            return None  # JS reads past the end as undefined
        return self.buffer._data[self.byteOffset + index]

    def __setitem__(self, index, value):
        if 0 <= index < self._length:
            self.buffer._data[self.byteOffset + index] = int(value) & 0xFF

    def __iter__(self):
        return iter(self._view().tobytes())

    def __bytes__(self):
        return self._view().tobytes()

    def __eq__(self, other):
        if isinstance(other, Uint8Array):
            return self.to_bytes() == other.to_bytes()
        return NotImplemented

    __hash__ = None

    def to_py(self):
        return self._view()

    def to_bytes(self):
        return self._view().tobytes()

    def to_memoryview(self):
        return self._view()

    def to_string(self, encoding="utf-8"):
        return self.to_bytes().decode(encoding)

    def subarray(self, begin=0, end=None):
        start, stop, _ = slice(begin, end).indices(self._length)
        return Uint8Array(self.buffer, self.byteOffset + start,
                          max(0, stop - start))

    def slice(self, begin=0, end=None):
        return Uint8Array.new(self.to_bytes()[begin:end])

    def set(self, source, offset=0):
        data = _as_bytes(source)
        start = self.byteOffset + int(offset)
        if int(offset) + len(data) > self._length:
            raise JsException("offset is out of bounds", "RangeError")
        self.buffer._data[start:start + len(data)] = data

    def fill(self, value, start=0, end=None):
        begin, stop, _ = slice(start, end).indices(self._length)
        for i in range(begin, stop):
            self[i] = value
        return self

    def assign(self, source):
        self.set(source)

    def assign_to(self, target):
        target[:] = self.to_bytes()

    def __repr__(self):
        return "Uint8Array(%d)" % self._length


# --------------------------------------------------------------------------
# Headers

class Headers(JsProxy):
    """Case-insensitive, ordered, multi-valued HTTP headers."""

    __slots__ = ("_list", "_guard")

    def __init__(self, init=None):
        self._list: list[list[str]] = []
        self._guard = None
        if _nullish(init):
            return
        if isinstance(init, Headers):
            self._list = [list(pair) for pair in init._list]
            return
        if isinstance(init, dict):
            items = init.items()
        elif hasattr(init, "items") and callable(init.items):
            items = init.items()
        else:
            items = init
        for pair in items:
            name, value = pair[0], pair[1]
            self.append(name, value)

    @classmethod
    def new(cls, init=None):
        return cls(init)

    @staticmethod
    def _norm(value):
        if isinstance(value, bytes):
            value = value.decode("latin-1")
        return str(value).strip()

    def append(self, name, value):
        self._list.append([str(name).lower(), self._norm(value)])

    def set(self, name, value):
        key = str(name).lower()
        self._list = [pair for pair in self._list if pair[0] != key]
        self._list.append([key, self._norm(value)])

    def delete(self, name):
        key = str(name).lower()
        self._list = [pair for pair in self._list if pair[0] != key]

    def get(self, name, default=None):
        key = str(name).lower()
        values = [v for k, v in self._list if k == key]
        if not values:
            return default
        return ", ".join(values)

    def getAll(self, name):
        key = str(name).lower()
        return JsArray(v for k, v in self._list if k == key)

    def getSetCookie(self):
        return self.getAll("set-cookie")

    def has(self, name):
        key = str(name).lower()
        return any(k == key for k, _ in self._list)

    def _combined(self):
        seen: dict[str, list] = {}
        order = []
        for key, value in self._list:
            if key == "set-cookie":
                order.append((key, value))
                continue
            if key not in seen:
                seen[key] = []
                order.append((key, seen[key]))
            seen[key].append(value)
        out = []
        for key, value in sorted(order, key=lambda pair: pair[0]):
            out.append((key, value if isinstance(value, str)
                        else ", ".join(value)))
        return out

    def entries(self):
        return iter([JsArray(pair) for pair in self._combined()])

    def items(self):
        return self._combined()

    def keys(self):
        return iter([k for k, _ in self._combined()])

    def values(self):
        return iter([v for _, v in self._combined()])

    def __iter__(self):
        return self.entries()

    def __getitem__(self, name):
        value = self.get(name)
        if value is None:
            raise KeyError(name)
        return value

    def __setitem__(self, name, value):
        self.set(name, value)

    def __contains__(self, name):
        return self.has(name)

    def forEach(self, callback):
        for key, value in self._combined():
            callback(value, key, self)

    def raw(self):
        """Every header line, set-cookie kept separate (for ASGI output)."""
        return [(k, v) for k, v in self._list]

    def to_py(self):
        return dict(self._combined())

    def __repr__(self):
        return "Headers(%r)" % (self._combined(),)


# --------------------------------------------------------------------------
# Streams

class _TeeSource:
    """Share one async chunk source between several readers."""

    def __init__(self, iterator):
        self._iterator = iterator
        self._chunks = []
        self._done = False
        self._lock = asyncio.Lock()

    async def chunk(self, index):
        while index >= len(self._chunks):
            if self._done:
                return None
            async with self._lock:
                if index < len(self._chunks) or self._done:
                    continue
                try:
                    value = await self._iterator.__anext__()
                except StopAsyncIteration:
                    self._done = True
                    return None
                self._chunks.append(value)
        return self._chunks[index]

    async def iterate(self):
        index = 0
        while True:
            value = await self.chunk(index)
            if value is None:
                return
            index += 1
            yield value


class _ReadResult(JsObject):
    __slots__ = ()


class _Reader(JsProxy):
    def __init__(self, stream):
        self._stream = stream
        self._iterator = stream._take()

    async def read(self):
        try:
            chunk = await self._iterator.__anext__()
        except StopAsyncIteration:
            return _ReadResult(done=True, value=None)
        return _ReadResult(done=False, value=Uint8Array.new(chunk))

    async def cancel(self, reason=None):
        del reason
        await self._stream.cancel()

    def releaseLock(self):
        self._stream.locked = False

    @property
    async def closed(self):
        return None


class ReadableStream(JsProxy):
    """A byte stream over bytes or an async iterator of byte chunks."""

    def __init__(self, source=None, *, data=None, on_close=None):
        self._data = data  # immutable bytes: re-readable, zero-copy
        self._source = source
        self._on_close = on_close
        self.locked = False
        self._taken = False

    @classmethod
    def new(cls, underlying=None, strategy=None):
        del strategy
        if underlying is None:
            return cls(data=b"")
        raise JsException("custom ReadableStream sources are not supported",
                          "TypeError")

    @classmethod
    def of_bytes(cls, data):
        return cls(data=bytes(data))

    def _take(self):
        if self._data is not None:
            return self._single()
        if self._taken:
            raise JsException("ReadableStream is already used", "TypeError")
        self._taken = True
        return self._iterate_source()

    async def _single(self):
        if self._data:
            yield self._data

    async def _iterate_source(self):
        try:
            async for chunk in self._source:
                if isinstance(chunk, str):
                    chunk = chunk.encode("utf-8")
                elif not isinstance(chunk, bytes):
                    chunk = _as_bytes(chunk)
                if chunk:
                    yield chunk
        finally:
            await self._close_source()

    async def _close_source(self):
        callback, self._on_close = self._on_close, None
        if callback is not None:
            try:
                result = callback()
                if asyncio.iscoroutine(result):
                    await result
            except Exception:
                pass

    def __aiter__(self):
        return self._take()

    def getReader(self, options=None):
        del options
        self.locked = True
        return _Reader(self)

    async def cancel(self, reason=None):
        del reason
        self._taken = True
        close = getattr(self._source, "aclose", None)
        if close is not None:
            try:
                await close()
            except Exception:
                pass
        await self._close_source()

    def tee(self):
        if self._data is not None:
            return JsArray([self, ReadableStream(data=self._data)])
        if self._taken:
            raise JsException("ReadableStream is already used", "TypeError")
        self._taken = True
        shared = _TeeSource(self._iterate_source())
        return JsArray([ReadableStream(shared.iterate()),
                        ReadableStream(shared.iterate())])

    async def read_all(self, limit=None):
        if self._data is not None:
            return self._data
        out = bytearray()
        async for chunk in self._take():
            out += chunk
            if limit is not None and len(out) > limit:
                raise JsException("body too large", "RangeError")
        return bytes(out)

    def pipeThrough(self, transform, options=None):
        del options
        return transform.readable._fed_by(self)

    async def pipeTo(self, destination, options=None):
        del options
        writer = destination.getWriter()
        async for chunk in self._take():
            await writer.write(chunk)
        await writer.close()


class _WritableStream(JsProxy):
    def __init__(self, queue):
        self._queue = queue
        self.locked = False

    def getWriter(self):
        self.locked = True
        return _Writer(self._queue)


class _Writer(JsProxy):
    def __init__(self, queue):
        self._queue = queue
        self.closed = False

    async def write(self, chunk):
        await self._queue.put(_as_bytes(chunk))

    async def close(self):
        if not self.closed:
            self.closed = True
            await self._queue.put(None)

    async def abort(self, reason=None):
        del reason
        await self.close()

    def releaseLock(self):
        pass

    @property
    def ready(self):
        return _resolved(None)

    @property
    def desiredSize(self):
        return 1


async def _resolved(value):
    return value


class TransformStream(JsProxy):
    """An identity TransformStream: bytes written come out of ``readable``."""

    def __init__(self):
        self._queue = asyncio.Queue(maxsize=16)
        self.writable = _WritableStream(self._queue)

        async def drain(queue=self._queue):
            while True:
                chunk = await queue.get()
                if chunk is None:
                    return
                yield chunk

        self.readable = ReadableStream(drain())

    @classmethod
    def new(cls, transformer=None, *strategies):
        del strategies
        if transformer is not None:
            raise JsException("custom transformers are not supported",
                              "TypeError")
        return cls()


def _body_from(value):
    """Normalize a BodyInit to None | bytes | ReadableStream (+ type)."""
    if _nullish(value):
        return None, None
    if isinstance(value, ReadableStream):
        if value._data is not None:
            return value._data, None
        return value, None
    if isinstance(value, str):
        return value.encode("utf-8"), "text/plain;charset=UTF-8"
    if isinstance(value, bytes):
        return value, None
    if isinstance(value, (bytearray, memoryview, Uint8Array, ArrayBuffer)):
        return _as_bytes(value), None
    if isinstance(value, URLSearchParams):
        return value.toString().encode("utf-8"), \
            "application/x-www-form-urlencoded;charset=UTF-8"
    if isinstance(value, _Body):
        return value._body, None
    if hasattr(value, "__aiter__"):
        return ReadableStream(value), None
    if isinstance(value, (list, tuple)):
        return bytes(value), None
    return str(value).encode("utf-8"), "text/plain;charset=UTF-8"


class _Body(JsProxy):
    _body = None
    bodyUsed = False

    @property
    def body(self):
        body = self._body
        if body is None:
            return None
        if isinstance(body, bytes):
            return ReadableStream(data=body)
        return body

    async def _read(self):
        body = self._body
        self.bodyUsed = True
        if body is None:
            return b""
        if isinstance(body, bytes):
            return body
        return await body.read_all()

    async def arrayBuffer(self):
        return ArrayBuffer(await self._read())

    async def bytes(self):
        return await self._read()

    async def text(self):
        return (await self._read()).decode("utf-8", errors="replace")

    async def json(self):
        raw = await self._read()
        try:
            value = (_orjson.loads(raw) if _orjson is not None
                     else _json.loads(raw or b"null"))
        except Exception as error:
            raise JsException(str(error), "SyntaxError") from None
        return _jsonify_tree(value)

    async def blob(self):
        return await self.arrayBuffer()

    async def formData(self):
        raw = (await self._read()).decode("utf-8", errors="replace")
        return JsObject(urllib.parse.parse_qsl(raw, keep_blank_values=True))

    def _clone_body(self):
        body = self._body
        if isinstance(body, ReadableStream):
            first, second = body.tee()
            self._body = first
            return second
        return body


# --------------------------------------------------------------------------
# Request / Response

_STATUS_TEXT = {
    100: "Continue", 101: "Switching Protocols", 200: "OK", 201: "Created",
    202: "Accepted", 204: "No Content", 206: "Partial Content",
    301: "Moved Permanently", 302: "Found", 303: "See Other",
    304: "Not Modified", 307: "Temporary Redirect",
    308: "Permanent Redirect", 400: "Bad Request", 401: "Unauthorized",
    403: "Forbidden", 404: "Not Found", 405: "Method Not Allowed",
    409: "Conflict", 410: "Gone", 413: "Payload Too Large",
    415: "Unsupported Media Type", 422: "Unprocessable Content",
    426: "Upgrade Required", 429: "Too Many Requests",
    500: "Internal Server Error", 501: "Not Implemented",
    502: "Bad Gateway", 503: "Service Unavailable",
    504: "Gateway Timeout",
}


class Request(_Body):
    def __init__(self, url, method="GET", headers=None, body=None,
                 signal=None, cf=None, redirect="follow"):
        self.url = str(url)
        self.method = str(method or "GET").upper()
        self.headers = headers if isinstance(headers, Headers) else \
            Headers(headers)
        self._body = body
        self.signal = signal if signal is not None else AbortSignal()
        self.cf = cf
        self.redirect = redirect

    @classmethod
    def new(cls, input, init=None):
        if isinstance(input, Request):
            base = input
            url = base.url
            method = base.method
            headers = Headers(base.headers)
            body = base._clone_body() if init is None or \
                _opt(init, "body") is None else None
            signal = base.signal
            cf = base.cf
            redirect = base.redirect
        else:
            url = str(input)
            method, headers, body, signal, cf = "GET", Headers(), None, \
                None, None
            redirect = "follow"
        if not _nullish(init):
            method = str(_opt(init, "method", method)).upper()
            if _opt(init, "headers") is not None:
                headers = Headers(_opt(init, "headers"))
            raw_body = _opt(init, "body")
            if raw_body is not None:
                body, content_type = _body_from(raw_body)
                if content_type and not headers.has("content-type"):
                    headers.set("content-type", content_type)
            signal = _opt(init, "signal", signal)
            cf = _opt(init, "cf", cf)
            redirect = _opt(init, "redirect", redirect)
        if method in ("GET", "HEAD") and body not in (None, b""):
            raise JsException(
                "Request with a GET or HEAD method cannot have a body.",
                "TypeError")
        return cls(url, method, headers, body, signal, cf, redirect)

    @property
    def js_object(self):
        return self

    def clone(self):
        return Request(self.url, self.method, Headers(self.headers),
                       self._clone_body(), self.signal, self.cf,
                       self.redirect)

    def __repr__(self):
        return "<Request %s %s>" % (self.method, self.url)


class Response(_Body):
    """The JS ``Response``. ``workers.Response`` subclasses it."""

    def __init__(self, body=None, status=200, statusText="", headers=None,
                 webSocket=None, url="", redirected=False):
        self._body = body
        self.status = int(status)
        self.statusText = statusText or ""
        self.headers = headers if isinstance(headers, Headers) else \
            Headers(headers)
        self.webSocket = webSocket
        self.url = url
        self.redirected = redirected
        self.type = "default"

    @classmethod
    def new(cls, body=None, init=None):
        normalized, content_type = _body_from(body)
        status = int(_opt(init, "status", 200))
        headers = Headers(_opt(init, "headers"))
        if content_type and not headers.has("content-type"):
            headers.set("content-type", content_type)
        web_socket = _opt(init, "webSocket")
        if status == 101 and web_socket is None:
            raise JsException("Responses may only be constructed with status "
                              "101 with a webSocket", "RangeError")
        if not (200 <= status <= 599) and status != 101:
            raise JsException("invalid status %d" % status, "RangeError")
        if status in (204, 205, 304) and normalized not in (None, b""):
            raise JsException("Response with null body status cannot have "
                              "body", "TypeError")
        return cls(normalized, status,
                   _opt(init, "statusText", "") or "", headers, web_socket)

    @classmethod
    def redirect(cls, url, status=302):
        return cls(None, int(status), "", Headers({"location": str(url)}))

    @classmethod
    def error(cls):
        return cls(None, 0)

    @property
    def ok(self):
        return 200 <= self.status < 300

    @property
    def status_text(self):
        return self.statusText or _STATUS_TEXT.get(self.status, "")

    def clone(self):
        clone = Response(self._clone_body(), self.status, self.statusText,
                         Headers(self.headers), self.webSocket, self.url,
                         self.redirected)
        return clone

    def __repr__(self):
        return "<Response %d>" % self.status


# --------------------------------------------------------------------------
# WebSockets

CONNECTING, OPEN, CLOSING, CLOSED = 0, 1, 2, 3


class WebSocketRequestResponsePair(JsProxy):
    def __init__(self, request, response):
        self.request = str(request)
        self.response = str(response)

    @classmethod
    def new(cls, request, response):
        return cls(request, response)


class WebSocket(JsProxy):
    """One end of a WebSocketPair.

    ``send`` delivers to the peer. The client end is drained by the ASGI
    bridge (``_inbox``); the server end dispatches to event listeners or,
    after ``ctx.acceptWebSocket``, to the Durable Object's hibernation
    handlers — always in order, one event at a time.
    """

    CONNECTING, OPEN, CLOSING, CLOSED = 0, 1, 2, 3

    def __init__(self, role):
        self.role = role
        self.peer = None
        self.readyState = CONNECTING
        self.protocol = ""
        self.extensions = ""
        self.url = None
        self._listeners = collections.defaultdict(list)
        self._attachment = None
        self._hibernation = None  # set by ctx.acceptWebSocket
        self._inbox = asyncio.Queue() if role == "client" else None
        self._events = None
        self._pump = None
        self._auto_ts = None
        self._accepted = False
        self.close_code = None

    def __js_keys__(self):
        return ["readyState", "protocol", "extensions", "url"]

    # -- API used by the app -------------------------------------------------
    def accept(self, options=None):
        del options
        self._accepted = True
        if self.readyState == CONNECTING:
            self.readyState = OPEN

    def send(self, data):
        if self.readyState in (CLOSING, CLOSED):
            raise JsException("Can't call WebSocket send() after close().",
                              "InvalidStateError")
        if self.role == "server" and not (self._accepted or self._hibernation):
            raise JsException(
                "You must call accept() on this WebSocket before sending "
                "messages.", "TypeError")
        if isinstance(data, (Uint8Array, ArrayBuffer, bytearray,
                             memoryview)):
            data = _as_bytes(data)
        elif not isinstance(data, (str, bytes)):
            data = str(data)
        peer = self.peer
        if peer is not None:
            peer._deliver(("message", data))

    def close(self, code=1000, reason=""):
        if self.readyState == CLOSED:
            return
        first = self.readyState != CLOSING
        self.readyState = CLOSING
        if first and self.peer is not None:
            self.peer._deliver(("close", int(code if code is not None
                                              else 1000),
                                str(reason or ""), True))
        self._finish()

    def addEventListener(self, kind, listener, options=None):
        del options
        self._listeners[str(kind)].append(listener)

    def removeEventListener(self, kind, listener, options=None):
        del options
        try:
            self._listeners[str(kind)].remove(listener)
        except ValueError:
            pass

    def serializeAttachment(self, value):
        self._attachment = None if _nullish(value) else _structured_clone(
            value)

    def deserializeAttachment(self):
        if self._attachment is None:
            return None
        return _structured_clone(self._attachment)

    def getAutoResponseTimestamp(self):
        return None if self._auto_ts is None else JsDate(self._auto_ts)

    # -- host plumbing -------------------------------------------------------
    def _finish(self):
        self.readyState = CLOSED
        hib = self._hibernation
        if hib is not None:
            hib[0]._forget_socket(self)

    def _deliver(self, event):
        if self.readyState == CLOSED and event[0] != "close":
            return
        if self.role == "client":
            self._inbox.put_nowait(event)
            if event[0] == "close":
                self.readyState = CLOSED
            return
        if self._events is None:
            self._events = asyncio.Queue()
        self._events.put_nowait(event)
        if self._pump is None or self._pump.done():
            self._pump = _spawn(self._drain())

    async def _drain(self):
        while not self._events.empty():
            event = self._events.get_nowait()
            try:
                await self._dispatch(event)
            except Exception as error:
                print("websocket handler failed: %r" % (error,))

    async def _dispatch(self, event):
        kind = event[0]
        if kind == "message":
            data = event[1]
            hib = self._hibernation
            if hib is not None:
                auto = hib[0]._auto_response
                if auto is not None and isinstance(data, str) \
                        and data == auto.request:
                    self._auto_ts = Date.now()
                    if self.peer is not None:
                        self.peer._deliver(("message", auto.response))
                    return
                await hib[0]._dispatch_message(self, data)
                return
            payload = data if isinstance(data, str) else ArrayBuffer(data)
            await self._emit("message", JsObject(type="message",
                                                 data=payload))
            return
        if kind == "close":
            _, code, reason, clean = event
            was_open = self.readyState != CLOSED
            self.close_code = code
            if self.readyState != CLOSING:
                self.readyState = CLOSING
            hib = self._hibernation
            try:
                if hib is not None:
                    await hib[0]._dispatch_close(self, code, reason, clean)
                elif was_open:
                    await self._emit("close", JsObject(
                        type="close", code=code, reason=reason,
                        wasClean=clean))
            finally:
                self._finish()
            return
        if kind == "error":
            hib = self._hibernation
            if hib is not None:
                await hib[0]._dispatch_error(self, event[1])
            else:
                await self._emit("error", JsObject(type="error",
                                                   error=event[1]))

    async def _emit(self, kind, event):
        for listener in list(self._listeners.get(kind, ())):
            result = listener(event)
            if asyncio.iscoroutine(result):
                await result

    # client side: used by the ASGI bridge
    def _client_send(self, data):
        """A frame from the real remote client, delivered to the server."""
        if self.peer is not None and self.readyState != CLOSED:
            self.peer._deliver(("message", data))

    def _client_closed(self, code=1005, reason="", clean=True):
        self.readyState = CLOSED
        if self.peer is not None and self.peer.readyState != CLOSED:
            self.peer._deliver(("close", int(code), str(reason or ""),
                                bool(clean)))


def _structured_clone(value):
    if isinstance(value, JsObject):
        return JsObject((k, _structured_clone(v)) for k, v in value.items())
    if isinstance(value, dict):
        return {k: _structured_clone(v) for k, v in value.items()}
    if isinstance(value, JsArray):
        return JsArray(_structured_clone(v) for v in value)
    if isinstance(value, list):
        return [_structured_clone(v) for v in value]
    if isinstance(value, Uint8Array):
        return Uint8Array.new(value)
    if isinstance(value, ArrayBuffer):
        return ArrayBuffer(bytes(value._data))
    return value


class WebSocketPair(JsProxy):
    def __init__(self):
        client = WebSocket("client")
        server = WebSocket("server")
        client.peer = server
        server.peer = client
        client.readyState = OPEN
        self._pair = (client, server)

    @classmethod
    def new(cls):
        return cls()

    def object_values(self):
        return JsArray(self._pair)

    def __getitem__(self, index):
        return self._pair[int(index)]

    def __iter__(self):
        return iter(self._pair)


# --------------------------------------------------------------------------
# Cache API

def _cache_ttl(headers):
    control = str(headers.get("cache-control") or "").lower()
    directives = {}
    for item in control.split(","):
        name, _, value = item.strip().partition("=")
        if name:
            directives[name] = value.strip().strip('"')
    if {"no-store", "private", "no-cache"} & directives.keys():
        return 0
    for name in ("s-maxage", "max-age"):
        if name in directives:
            try:
                return max(0, int(directives[name]))
            except ValueError:
                return 0
    expires = headers.get("expires")
    if expires:
        try:
            when = email.utils.parsedate_to_datetime(expires).timestamp()
        except (TypeError, ValueError, IndexError):
            return 0
        return max(0, int(when - time.time()))
    return 0


class _Cache(JsProxy):
    """Per-process Cache API: byte-capped LRU honoring max-age."""

    def __init__(self, max_bytes=8 * 1024 * 1024):
        self.max_bytes = int(max_bytes)
        self._entries: collections.OrderedDict = collections.OrderedDict()
        self._bytes = 0

    @staticmethod
    def _key(request):
        if isinstance(request, Request):
            if request.method not in ("GET", "HEAD"):
                return None
            request = request.url
        return str(URL(str(request)).href)

    def _drop(self, key):
        entry = self._entries.pop(key, None)
        if entry is not None:
            self._bytes -= entry[4]

    async def match(self, request, options=None):
        del options
        key = self._key(request)
        if key is None:
            return None
        entry = self._entries.get(key)
        if entry is None:
            return None
        status, headers, body, expires, _size = entry
        if time.monotonic() >= expires:
            self._drop(key)
            return None
        self._entries.move_to_end(key)
        out = Headers(headers)
        out.set("cf-cache-status", "HIT")
        return Response(body, status, "", out)

    async def put(self, request, response):
        key = self._key(request)
        if key is None:
            raise JsException("Cache API only supports GET requests",
                              "TypeError")
        if response.status == 206 or response.headers.has("set-cookie") or \
                "*" in str(response.headers.get("vary") or ""):
            await response._read()
            return None
        ttl = _cache_ttl(response.headers)
        body = await response._read()
        if ttl <= 0:
            return None
        size = len(body) + sum(len(k) + len(v)
                               for k, v in response.headers.raw()) + len(key)
        if size > self.max_bytes // 4:
            return None
        self._drop(key)
        self._entries[key] = (response.status, Headers(response.headers),
                              body, time.monotonic() + ttl, size)
        self._bytes += size
        while self._bytes > self.max_bytes and self._entries:
            oldest = next(iter(self._entries))
            self._drop(oldest)
        return None

    async def delete(self, request, options=None):
        del options
        key = self._key(request)
        if key is None or key not in self._entries:
            return False
        self._drop(key)
        return True

    def clear(self):
        self._entries.clear()
        self._bytes = 0

    @property
    def size_bytes(self):
        return self._bytes


class _CacheStorage(JsProxy):
    def __init__(self):
        self.default = _Cache(int(os.environ.get(
            "FORKMESH_HOST_CACHE_BYTES", str(8 * 1024 * 1024))))
        self._named = {}

    async def open(self, name):
        cache = self._named.get(name)
        if cache is None:
            cache = self._named[name] = _Cache(self.default.max_bytes // 4)
        return cache


caches = _CacheStorage()


# --------------------------------------------------------------------------
# fetch

_client = None
_client_loop = None
# Fetches that answer with a known small body are read immediately so an
# unread response never pins a pooled connection.
_EAGER_BODY_BYTES = 256 * 1024
# Routes that bypass the network: host-local handlers keyed by origin
# (the host registers its own public origins so the app's self-fetches stay
# in process, like a Worker service binding).
local_origins: dict = {}


def _http_client():
    global _client, _client_loop
    loop = asyncio.get_running_loop()
    if _client is None or _client_loop is not loop:
        import httpx
        from . import egress
        # Every connection is resolved, checked and pinned by the egress
        # guard (egress.py); every redirect hop passes the request hook.
        _client = httpx.AsyncClient(
            timeout=httpx.Timeout(connect=10.0, read=120.0, write=60.0,
                                  pool=30.0),
            transport=egress.guarded_transport(httpx.Limits(
                max_connections=200, max_keepalive_connections=40)),
            mounts=egress.http2_mounts(httpx.Limits(
                max_connections=20, max_keepalive_connections=4)),
            follow_redirects=False,
            trust_env=False,
            event_hooks={"request": [egress.request_hook]},
            headers={"user-agent": "ForkMesh-Host/1"},
        )
        _client_loop = loop
    return _client


async def close_http_client():
    global _client
    client, _client = _client, None
    if client is not None:
        await client.aclose()


async def _request_body_iter(stream):
    async for chunk in stream._take():
        yield chunk


async def fetch(resource, init=None):
    if isinstance(resource, Request) and _nullish(init):
        request = resource
    else:
        if isinstance(resource, URL):
            resource = resource.href
        request = Request.new(resource, init)
    signal = request.signal
    target = URL(request.url)
    local = local_origins.get(target.origin)
    if local is not None:
        return await _race_signal(local(request), signal)
    if target.protocol not in ("http:", "https:"):
        raise JsException("Fetch API cannot load %s" % request.url,
                          "TypeError")
    if os.environ.get("FORKMESH_HOST_OFFLINE") == "1":
        # Tests and sandboxes: no subrequest leaves the process.
        raise JsException("network access disabled (FORKMESH_HOST_OFFLINE)",
                          "TypeError")
    from . import egress
    try:
        egress.check_url_host(target.hostname, target.port,
                              target.protocol.rstrip(":"))
    except egress.EgressBlocked as error:
        raise JsException("fetch blocked: %s" % error, "TypeError") from None
    client = _http_client()
    headers = [(k, v) for k, v in request.headers.raw()
               if k not in ("host", "content-length")]
    body = request._body
    if isinstance(body, ReadableStream):
        content = _request_body_iter(body)
    else:
        content = body
    built = client.build_request(request.method, request.url,
                                 headers=headers, content=content)
    redirect = str(request.redirect or "follow")
    try:
        response = await _race_signal(client.send(
            built, stream=True, follow_redirects=(redirect == "follow")),
            signal)
    except egress.EgressBlocked as error:
        raise JsException("fetch blocked: %s" % error, "TypeError") from None
    if redirect == "error" and 300 <= response.status_code < 400 and \
            response.headers.get("location"):
        await response.aclose()
        raise JsException("Fetch API redirect: error", "TypeError")
    out_headers = Headers(response.headers.multi_items())
    decoded = str(out_headers.get("content-encoding") or "").lower() not in (
        "", "identity")
    if decoded:
        out_headers.delete("content-encoding")
        out_headers.delete("content-length")
    status = response.status_code
    if request.method == "HEAD" or status in (204, 205, 304):
        await response.aclose()
        out_body = None
    else:
        try:
            announced = int(response.headers.get("content-length") or -1)
        except ValueError:
            announced = -1
        if 0 <= announced <= _EAGER_BODY_BYTES:
            try:
                out_body = await _race_signal(response.aread(), signal)
            finally:
                await response.aclose()
        else:
            out_body = ReadableStream(response.aiter_bytes(),
                                      on_close=response.aclose)
            weakref.finalize(out_body, _close_later, response)
    return Response(out_body, status, response.reason_phrase, out_headers,
                    None, str(response.url), bool(response.history))


def _close_later(response):
    if response.is_closed:
        return
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        return
    loop.create_task(response.aclose())


# --------------------------------------------------------------------------
# crypto

from .webcrypto import crypto  # noqa: E402  (needs the classes above)


# Globals some code reaches through ``js`` as attributes.
undefined = None
null = jsnull
globalThis = None  # set below


class _Global:
    def __getattr__(self, name):
        try:
            return globals()[name]
        except KeyError:
            raise AttributeError(name) from None


globalThis = _Global()
self = globalThis  # noqa: A001 - the JS global name
