"""Request-lazy, hard-bounded Worker body readers."""


JSON_BODY_DEFAULT_MAX_BYTES = 2 * 1024 * 1024
JSON_BODY_LARGE_MAX_BYTES = 8 * 1024 * 1024
JSON_BODY_LARGE_PATH_PREFIXES = (
    "/api/security/", "/api/repositories/import")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _json_request_limit(request):
    try:
        path = urlparse(request.url).path
    except Exception:
        path = ""
    return (JSON_BODY_LARGE_MAX_BYTES if any(
        path.startswith(prefix) for prefix in JSON_BODY_LARGE_PATH_PREFIXES)
        else JSON_BODY_DEFAULT_MAX_BYTES)


async def bounded_request_text(request, max_bytes):
    """Read a request/response stream without buffering beyond its limit."""
    limit = int(max_bytes)
    try:
        announced = int(request.headers.get("content-length") or 0)
    except Exception:
        announced = 0
    if announced < 0 or announced > limit:
        raise RequestBodyTooLarge()

    def chunk_bytes(value):
        if isinstance(value, str):
            return value.encode("utf-8")
        converter = getattr(value, "to_py", None)
        return bytes(converter() if callable(converter) else value or b"")

    body = getattr(request, "body", None)
    get_reader = getattr(body, "getReader", None)
    raw = bytearray()
    if callable(get_reader):
        reader = get_reader()
        while True:
            result = await reader.read()
            field = result.get if isinstance(result, dict) else \
                lambda name, default=None: getattr(result, name, default)
            if field("done", False):
                break
            raw.extend(chunk_bytes(field("value")))
            if len(raw) > limit:
                try:
                    cancelled = reader.cancel()
                    if hasattr(cancelled, "__await__"):
                        await cancelled
                except Exception:
                    pass
                raise RequestBodyTooLarge()
    elif hasattr(body, "__aiter__"):
        async for value in body:
            raw.extend(chunk_bytes(value))
            if len(raw) > limit:
                raise RequestBodyTooLarge()
    else:  # Lightweight test adapters expose only text().
        raw.extend(str(await request.text()).encode("utf-8"))
        if len(raw) > limit:
            raise RequestBodyTooLarge()
    return raw.decode("utf-8")


async def bounded_json_request(request, max_bytes=None):
    """Read one JSON body with a hard bound even without Content-Length."""
    raw = await bounded_request_text(
        request, int(max_bytes or _json_request_limit(request)))
    return json.loads(raw or "{}")
