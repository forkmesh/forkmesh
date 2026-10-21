"""Host implementations of the non-database Worker bindings.

* ``LocalKV``: ``REPOSITORY_METADATA`` (get/put/delete/list with
  ``expirationTtl``/``expiration``) on a local sqlite file.
* ``LocalQueue``: ``NOTIFICATION_QUEUE`` producer (send/sendBatch) that
  batches in process and delivers to the entrypoint's ``queue()`` handler
  with max_batch_size / max_batch_timeout / max_retries from wrangler.toml.
* ``StaticAssets``: ``ASSETS.fetch`` over app/dist with
  ``html_handling = "drop-trailing-slash"`` and ``not_found_handling =
  "404-page"``. A browser request is answered from a file's brotli or gzip
  copy when its Accept-Encoding allows one (``precompressed_variant``).
* ``OpenAICompatibleAI``: ``AI.run(model, inputs)`` against an
  OpenAI-compatible HTTP endpoint, only when ``AI_API_URL`` is configured.
"""

from __future__ import annotations

import asyncio
import hashlib
import itertools
import json
import mimetypes
import re
import sqlite3
import time
import urllib.parse
from pathlib import Path

from . import js as _js
from .ffi import JsArray, JsNull, JsObject

# --------------------------------------------------------------------------
# KV


class LocalKV:
    def __init__(self, path):
        self.path = str(path)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self._conn = sqlite3.connect(self.path, isolation_level=None,
                                     check_same_thread=False)
        self._conn.execute(
            "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v BLOB NOT "
            "NULL, expires REAL, metadata TEXT)")

    def _live(self, key):
        row = self._conn.execute(
            "SELECT v, expires, metadata FROM kv WHERE k=?", (key,)
        ).fetchone()
        if row is None:
            return None
        if row[1] is not None and row[1] <= time.time():
            self._conn.execute("DELETE FROM kv WHERE k=?", (key,))
            return None
        return row

    async def get(self, key, options=None):
        row = self._live(str(key))
        if row is None:
            return None
        kind = options if isinstance(options, str) else \
            _js._opt(options, "type", "text")
        value = bytes(row[0])
        if kind == "json":
            return _js._jsonify_tree(json.loads(value))
        if kind == "arrayBuffer":
            return _js.ArrayBuffer(value)
        if kind == "stream":
            return _js.ReadableStream(data=value)
        return value.decode("utf-8")

    async def getWithMetadata(self, key, options=None):
        row = self._live(str(key))
        value = await self.get(key, options) if row is not None else None
        metadata = json.loads(row[2]) if row is not None and row[2] else None
        return JsObject(value=value, metadata=metadata)

    async def put(self, key, value, options=None):
        if isinstance(value, _js.ReadableStream):
            value = await value.read_all()
        elif isinstance(value, str):
            value = value.encode("utf-8")
        else:
            value = _js._as_bytes(value)
        expires = None
        ttl = _js._opt(options, "expirationTtl")
        at = _js._opt(options, "expiration")
        if ttl is not None:
            expires = time.time() + max(60, int(ttl))
        elif at is not None:
            expires = float(at)
        metadata = _js._opt(options, "metadata")
        self._conn.execute(
            "INSERT INTO kv (k, v, expires, metadata) VALUES (?,?,?,?) "
            "ON CONFLICT(k) DO UPDATE SET v=excluded.v, "
            "expires=excluded.expires, metadata=excluded.metadata",
            (str(key), value, expires,
             json.dumps(metadata) if metadata is not None else None))

    async def delete(self, key):
        self._conn.execute("DELETE FROM kv WHERE k=?", (str(key),))

    async def list(self, options=None):
        prefix = str(_js._opt(options, "prefix", "") or "")
        limit = int(_js._opt(options, "limit", 1000))
        rows = self._conn.execute(
            "SELECT k, expires FROM kv WHERE k >= ? AND (expires IS NULL OR "
            "expires > ?) ORDER BY k LIMIT ?",
            (prefix, time.time(), limit + 1)).fetchall()
        keys = [JsObject(name=k, expiration=e) for k, e in rows
                if k.startswith(prefix)][:limit]
        return JsObject(keys=JsArray(keys), list_complete=True, cursor="")


# --------------------------------------------------------------------------
# Queue

_message_ids = itertools.count(1)


class QueueMessage:
    def __init__(self, body, attempts=1, content_type="json"):
        self.id = "%x-%d" % (int(time.time() * 1000), next(_message_ids))
        self.timestamp = _js.JsDate()
        self.body = body
        self.attempts = attempts
        self.contentType = content_type
        self._outcome = None
        self._delay = 0

    def ack(self):
        self._outcome = "ack"

    def retry(self, options=None):
        self._outcome = "retry"
        self._delay = int(_js._opt(options, "delaySeconds", 0) or 0)


class MessageBatch:
    def __init__(self, queue_name, messages):
        self.queue = queue_name
        self.messages = JsArray(messages)

    def ackAll(self):
        for message in self.messages:
            message.ack()

    def retryAll(self, options=None):
        for message in self.messages:
            message.retry(options)


class LocalQueue:
    """In-process queue; ``consumer(batch)`` is set by the host app."""

    def __init__(self, name, max_batch_size=10, max_batch_timeout=1.0,
                 max_retries=3):
        self.name = name
        self.max_batch_size = int(max_batch_size)
        self.max_batch_timeout = float(max_batch_timeout)
        self.max_retries = int(max_retries)
        self.consumer = None
        self._pending: list = []
        self._flush_handle = None
        self._running = None
        self.delivered = 0
        self.dead_lettered = 0

    async def send(self, body, options=None):
        content_type = _js._opt(options, "contentType", "json")
        delay = int(_js._opt(options, "delaySeconds", 0) or 0)
        self._enqueue(QueueMessage(_js._structured_clone(body), 1,
                                   content_type), delay)

    async def sendBatch(self, messages, options=None):
        for item in messages or []:
            body = _js._opt(item, "body")
            delay = int(_js._opt(item, "delaySeconds",
                                 _js._opt(options, "delaySeconds", 0)) or 0)
            self._enqueue(QueueMessage(_js._structured_clone(body), 1,
                                       _js._opt(item, "contentType",
                                                "json")), delay)

    def _enqueue(self, message, delay=0):
        loop = asyncio.get_running_loop()
        if delay > 0:
            loop.call_later(delay, self._enqueue, message, 0)
            return
        self._pending.append(message)
        if len(self._pending) >= self.max_batch_size:
            self._schedule(0)
        elif self._flush_handle is None:
            self._schedule(self.max_batch_timeout)

    def _schedule(self, delay):
        if self._flush_handle is not None:
            self._flush_handle.cancel()
        loop = asyncio.get_running_loop()
        self._flush_handle = loop.call_later(
            delay, lambda: _js._spawn(self.flush()))

    async def flush(self):
        """Deliver everything pending (max_concurrency = 1)."""
        self._flush_handle = None
        if self._running is not None and not self._running.done():
            await self._running
        while self._pending and self.consumer is not None:
            batch_messages = self._pending[:self.max_batch_size]
            del self._pending[:self.max_batch_size]
            self._running = asyncio.ensure_future(
                self._deliver(batch_messages))
            await self._running

    async def _deliver(self, messages):
        if self.consumer is None:
            self._pending[:0] = messages
            return
        batch = MessageBatch(self.name, messages)
        failed = False
        try:
            await self.consumer(batch)
        except Exception as error:
            print("queue %s consumer failed: %r" % (self.name, error))
            failed = True
        for message in messages:
            retry = message._outcome == "retry" or (
                failed and message._outcome != "ack")
            if not retry:
                self.delivered += 1
                continue
            if message.attempts > self.max_retries:
                self.dead_lettered += 1
                continue
            again = QueueMessage(message.body, message.attempts + 1,
                                 message.contentType)
            again.id = message.id
            self._enqueue(again, message._delay or 1)


# --------------------------------------------------------------------------
# Static assets

_CONTROL_FILES = frozenset({"_headers", "_redirects", "_routes.json"})
# Precompressed copies (app/tools/precompress_assets.py), best first: a file
# is answered from ``<file>.br`` or ``<file>.gz`` when the request accepts
# that coding and the copy is not older than the file.
PRECOMPRESSED = (("br", ".br"), ("gzip", ".gz"))


def accepted_codings(header):
    """{coding: q} from an Accept-Encoding value (``*`` kept as is)."""
    out = {}
    for part in str(header or "").split(","):
        name, _, params = part.strip().partition(";")
        name = name.strip().lower()
        if not name:
            continue
        q = 1.0
        for param in params.split(";"):
            key, _, value = param.strip().partition("=")
            if key.strip().lower() == "q":
                try:
                    q = float(value)
                except ValueError:
                    q = 0.0
        out[name] = max(0.0, min(1.0, q))
    return out


def precompressed_variant(found, accept_encoding):
    """(coding, copy path, has fresh copies) for a file and Accept-Encoding.

    The coding is None when no fresh copy is acceptable; the flag says the
    response varies by Accept-Encoding at all."""
    try:
        mtime = found.stat().st_mtime_ns
    except OSError:
        return None, None, False
    copies = []
    for coding, suffix in PRECOMPRESSED:
        copy = found.with_name(found.name + suffix)
        try:
            stat = copy.stat()
        except OSError:
            continue
        if stat.st_mtime_ns >= mtime and copy.is_file():
            copies.append((coding, copy))
    if not copies:
        return None, None, False
    accepted = accepted_codings(accept_encoding)
    best = (0.0, None, None)
    for coding, copy in copies:
        q = accepted.get(coding, accepted.get("*", 0.0))
        if q > best[0]:
            best = (q, coding, copy)
    return best[1], best[2], True


_HEADER_NAME = re.compile(r"^[A-Za-z0-9-]+$")


def _header_pattern(pattern):
    segments = []
    for segment in pattern.split("/"):
        if re.fullmatch(r":[A-Za-z_][A-Za-z0-9_]*", segment):
            segments.append("[^/]+")
        else:
            segments.append(".*".join(
                re.escape(part) for part in segment.split("*")))
    return re.compile("/".join(segments))


def parse_headers_file(text):
    rules = []
    current = None
    for raw in text.splitlines():
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not line[:1].isspace():
            current = None
            if stripped.startswith("/"):
                current = (_header_pattern(
                    urllib.parse.unquote(stripped)), [])
                rules.append(current)
            continue
        if current is None:
            continue
        if stripped.startswith("!"):
            op = ("unset", stripped[1:].strip(), "")
        else:
            name, sep, value = stripped.partition(":")
            if not sep:
                continue
            op = ("set", name.strip(), value.strip())
        if _HEADER_NAME.match(op[1]):
            current[1].append(op)
    return rules


class StaticAssets:
    """``env.ASSETS``: Workers Static Assets semantics over a directory."""

    def __init__(self, root, html_handling="drop-trailing-slash",
                 not_found_handling="404-page"):
        self.root = Path(root).resolve()
        self.html_handling = html_handling
        self.not_found_handling = not_found_handling
        self._header_rules = None

    def _file(self, relative):
        relative = relative.lstrip("/")
        if not relative or relative in _CONTROL_FILES:
            return None
        try:
            candidate = (self.root / relative).resolve()
            candidate.relative_to(self.root)
            return candidate if candidate.is_file() else None
        except (ValueError, OSError):
            return None

    def resolve(self, path, redirects=True):
        """Return ("file", Path) | ("redirect", location) | (None, None).

        ``redirects=False`` (the ``env.ASSETS.fetch`` binding) serves an
        exact file such as ``/index.html`` directly: the app reads its
        documents that way, and only browser navigations get the
        drop-trailing-slash redirects.
        """
        path = urllib.parse.unquote(path or "/")
        if "\0" in path:
            return None, None
        if not redirects:
            exact = self._file(path)
            if exact:
                return "file", exact
        if self.html_handling == "none":
            found = self._file(path)
            return ("file", found) if found else (None, None)
        if path.endswith("/index.html"):
            if self._file(path):
                target = path[:-len("index.html")]
                if target != "/":
                    target = target.rstrip("/")
                return "redirect", target
        elif path.endswith(".html"):
            if self._file(path):
                return "redirect", path[:-len(".html")]
        if path == "/":
            found = self._file("index.html")
            return ("file", found) if found else (None, None)
        if path.endswith("/"):
            base = path.rstrip("/")
            if self._file(base + ".html") or self._file(base + "/index.html"):
                return "redirect", base
            found = self._file(path + "index.html")
            return ("file", found) if found else (None, None)
        found = self._file(path)
        if found and found.suffix != ".html":
            return "file", found
        for candidate in (path + ".html", path + "/index.html"):
            found = self._file(candidate)
            if found:
                return "file", found
        return None, None

    def not_found_page(self, path):
        if self.not_found_handling == "404-page":
            directory = urllib.parse.unquote(path or "/").rstrip("/")
            while True:
                found = self._file(directory + "/404.html")
                if found:
                    return found
                if not directory:
                    return None
                directory = directory.rsplit("/", 1)[0]
        if self.not_found_handling == "single-page-application":
            return self._file("index.html")
        return None

    def exists(self, path):
        return self.resolve(path)[0] is not None

    @staticmethod
    def _headers(found, stat):
        content_type = mimetypes.guess_type(found.name)[0] or \
            "application/octet-stream"
        if content_type.startswith("text/") or content_type in (
                "application/javascript", "application/json",
                "image/svg+xml"):
            content_type += "; charset=utf-8"
        etag = '"%s"' % hashlib.sha1(
            ("%s:%d:%d" % (found, stat.st_size, stat.st_mtime_ns)).encode()
        ).hexdigest()[:20]
        return {
            "content-type": content_type,
            "etag": etag,
            "cache-control": "public, max-age=0, must-revalidate",
        }

    def header_rules(self):
        if self._header_rules is None:
            try:
                text = (self.root / "_headers").read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                text = ""
            self._header_rules = parse_headers_file(text)
        return self._header_rules

    def apply_header_rules(self, path, headers):
        path = urllib.parse.unquote(path or "/")
        for pattern, ops in self.header_rules():
            if not pattern.fullmatch(path):
                continue
            for op, name, value in ops:
                if op == "unset":
                    headers.pop(name.lower(), None)
            for op, name, value in ops:
                if op == "set":
                    headers[name.lower()] = value
        return headers

    def serve(self, path, method="GET", if_none_match=None, redirects=True,
              accept_encoding=None):
        kind, value = self.resolve(path, redirects)
        status = 200
        if kind == "redirect":
            return _js.Response(None, 307, "", _js.Headers(
                {"location": value}))
        if kind is None:
            value = self.not_found_page(path)
            if value is None:
                return _js.Response(b"", 404, "", _js.Headers())
            status = 404 if self.not_found_handling == "404-page" else 200
        stat = value.stat()
        headers = self._headers(value, stat)
        source = value
        if status == 200 and accept_encoding is not None:
            coding, copy, varies = precompressed_variant(value,
                                                         accept_encoding)
            if varies:
                headers["vary"] = "accept-encoding"
            if coding:
                source = copy
                headers["content-encoding"] = coding
                headers["etag"] = headers["etag"][:-1] + "-%s\"" % coding
        if redirects:
            self.apply_header_rules(path, headers)
        if status == 200 and if_none_match and \
                headers["etag"] in if_none_match:
            return _js.Response(None, 304, "", _js.Headers(headers))
        body = None if method == "HEAD" else source.read_bytes()
        return _js.Response(body, status, "", _js.Headers(headers))

    async def fetch(self, resource, init=None):
        if isinstance(resource, _js.Request) and init is None:
            request = resource
        else:
            request = _js.Request.new(
                resource.href if isinstance(resource, _js.URL) else resource,
                init)
        url = _js.URL(request.url)
        return self.serve(url.pathname, request.method,
                          request.headers.get("if-none-match"),
                          redirects=False)


# --------------------------------------------------------------------------
# AI (optional)

class OpenAICompatibleAI:
    """``env.AI.run(model, {messages|prompt, ...})`` over HTTP.

    Maps Workers AI model ids through ``model_map`` (``AI_MODEL_MAP`` JSON)
    and returns ``{"response": text}`` like Workers AI text generation.
    """

    def __init__(self, url, api_key="", model_map=None, default_model=""):
        self.url = url.rstrip("/")
        self.api_key = api_key
        self.model_map = dict(model_map or {})
        self.default_model = default_model

    async def run(self, model, inputs, options=None):
        del options
        inputs = inputs.to_py() if hasattr(inputs, "to_py") else dict(inputs)
        target = self.model_map.get(str(model)) or self.default_model or \
            str(model)
        messages = inputs.get("messages")
        if not messages:
            messages = [{"role": "user", "content": inputs.get("prompt", "")}]
        payload = {"model": target, "messages": messages}
        for key in ("max_tokens", "temperature", "top_p"):
            if key in inputs:
                payload[key] = inputs[key]
        headers = {"content-type": "application/json"}
        if self.api_key:
            headers["authorization"] = "Bearer " + self.api_key
        response = await _js.fetch(self.url + "/chat/completions", {
            "method": "POST", "headers": headers,
            "body": json.dumps(payload),
            "signal": _js.AbortSignal.timeout(60_000)})
        data = await response.json()
        if response.status != 200:
            raise RuntimeError("AI endpoint answered %d" % response.status)
        choice = (data.get("choices") or [{}])[0]
        text = (choice.get("message") or {}).get("content") or ""
        return JsObject(response=text, usage=data.get("usage"))


def _nullish(value):
    return value is None or isinstance(value, JsNull)


__all__ = ["LocalKV", "LocalQueue", "MessageBatch", "QueueMessage",
           "StaticAssets", "OpenAICompatibleAI"]
