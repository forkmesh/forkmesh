"""Explicit dependencies shared by isolated Worker AST test harnesses."""


async def json_from_request_double(request, max_bytes=None):
    """Read JSON from an in-memory request double.

    Production size-limit behavior is covered by ``test_bounded_json_requests``.
    Handler-isolation tests inject this helper by name so missing dependencies
    remain visible instead of being hidden through process-global state.
    """
    del max_bytes
    return await request.json()
