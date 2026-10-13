"""ForkMesh Remote MCP: the Streamable HTTP endpoint served at /mcp.

Organization bot credentials reach the scoped organization task tools here.
Split out of entry.py so Cloudflare's startup-memory validation (error 10021)
does not pay for this domain on every isolate; entry.py loads it through a
_LazyModule proxy on the first /mcp request.
"""

import re


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


REMOTE_MCP_PROTOCOL_VERSION = "2025-06-18"
REMOTE_MCP_TASK_ID_RE = re.compile(r"^[a-f0-9]{32}$")
REMOTE_MCP_TOOLS = [
    {
        "name": "list_org_tasks",
        "title": "List organization tasks",
        "description": (
            "List unfinished private tasks for the organization authorized "
            "by this ForkMesh credential."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "department": {"type": "string"},
                "team": {"type": "string"},
                "limit": {
                    "type": "integer", "minimum": 1, "maximum": 200,
                    "default": 50,
                },
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "get_org_task",
        "title": "Get organization task",
        "description": (
            "Read one private organization task and its repository, "
            "assignment, completion, and QA details."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "task_id": {
                    "type": "string", "pattern": "^[a-f0-9]{32}$",
                },
            },
            "required": ["task_id"],
            "additionalProperties": False,
        },
    },
    {
        "name": "complete_org_task",
        "title": "Complete organization task",
        "description": (
            "Send completed work to human QA with a concrete evidence note. "
            "Do not call this before the pull request or deploy is verified."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "task_id": {
                    "type": "string", "pattern": "^[a-f0-9]{32}$",
                },
                "completion_note": {
                    "type": "string", "minLength": 1, "maxLength": 4000,
                },
            },
            "required": ["task_id", "completion_note"],
            "additionalProperties": False,
        },
    },
]


def _remote_mcp_response(data, status=200):
    return json_response(
        data,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={
            "x-content-type-options": "nosniff",
            "vary": "authorization, accept",
        },
    )


def _remote_mcp_result(request_id, result):
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def _remote_mcp_error(request_id, code, message):
    return {
        "jsonrpc": "2.0",
        "id": request_id,
        "error": {"code": int(code), "message": str(message)},
    }


def _remote_mcp_tool_result(data, failed=False):
    result = {
        "content": [{
            "type": "text",
            "text": json.dumps(
                data, separators=(",", ":"), ensure_ascii=False),
        }],
        "structuredContent": data,
    }
    if failed:
        result["isError"] = True
    return result


async def _remote_mcp_task_request(
        env, request, context, method, path, body=None):
    """Reuse the task service and its authorization with a JS-owned request."""

    headers = {
        "authorization": str(
            request.headers.get("authorization") or "").strip(),
        "accept": "application/json",
    }
    init = {"method": method, "headers": headers}
    if body is not None:
        headers["content-type"] = "application/json"
        init["body"] = json.dumps(body, separators=(",", ":"))
    internal = JsRequest.new(
        _public_base_url(env, request).rstrip("/") + path, to_js(init))
    response = await world_office_tasks.handle(
        _OfficeMarketingTasksRuntime(env, internal, context),
        urlparse(path).path,
    )
    data = await response.json()
    if hasattr(data, "to_py"):
        data = data.to_py()
    try:
        data = data if isinstance(data, dict) else dict(data)
    except Exception:
        data = {"error": "invalid_task_service_response"}
    return int(getattr(response, "status", 500) or 500), data


async def _remote_mcp_call_tool(env, request, context, name, arguments):
    if not isinstance(arguments, dict):
        return _remote_mcp_tool_result(
            {"error": "arguments_must_be_an_object"}, failed=True)
    scopes = set(context.get("scopes") or [])
    required = (
        "organization.tasks.write"
        if name == "complete_org_task"
        else "organization.tasks.read"
    )
    if required not in scopes:
        return _remote_mcp_tool_result({
            "error": "permission_denied", "requiredScope": required,
        }, failed=True)

    if name == "list_org_tasks":
        department = clean_string(arguments.get("department"), 64).lower()
        team = clean_string(arguments.get("team"), 64).lower()
        try:
            limit = max(1, min(200, int(arguments.get("limit") or 50)))
        except (TypeError, ValueError):
            return _remote_mcp_tool_result(
                {"error": "invalid_limit"}, failed=True)
        query = []
        if department:
            query.append(("department", department))
        if team:
            query.append(("team", team))
        path = "/api/tasks" + (
            "?" + urlencode(query) if query else "")
        status, data = await _remote_mcp_task_request(
            env, request, context, "GET", path)
        if status >= 400:
            return _remote_mcp_tool_result(data, failed=True)
        tasks = [
            task for task in list(data.get("tasks") or [])
            if not int(task.get("completedAt") or 0)
            and str(task.get("status") or "") != "done"
        ]
        return _remote_mcp_tool_result({
            "ok": True,
            "organization": context.get("org") or "",
            "tasks": tasks[:limit],
            "returned": min(limit, len(tasks)),
            "available": len(tasks),
        })

    task_id = clean_string(arguments.get("task_id"), 32).lower()
    if not REMOTE_MCP_TASK_ID_RE.fullmatch(task_id):
        return _remote_mcp_tool_result(
            {"error": "invalid_task_id"}, failed=True)
    if name == "get_org_task":
        status, data = await _remote_mcp_task_request(
            env, request, context, "GET", "/api/tasks/" + task_id)
        return _remote_mcp_tool_result(data, failed=status >= 400)
    if name == "complete_org_task":
        note = clean_string(arguments.get("completion_note"), 4000).strip()
        if not note:
            return _remote_mcp_tool_result(
                {"error": "completion_note_required"}, failed=True)
        status, data = await _remote_mcp_task_request(
            env,
            request,
            context,
            "POST",
            "/api/tasks/" + task_id + "/complete",
            {"completionNote": note},
        )
        return _remote_mcp_tool_result(data, failed=status >= 400)
    return None


async def remote_mcp_handler(env, request):
    """Authenticated, stateless MCP Streamable HTTP endpoint."""

    method = method_name(request)
    if method == "GET":
        return _remote_mcp_response(
            {"error": "sse_not_supported"}, status=405)
    if method != "POST":
        return _remote_mcp_response(
            {"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    context = await _org_bot_token_context(env, request, touch=True)
    if not context:
        return _remote_mcp_response(
            {"error": "invalid_bot_token"}, status=401)
    try:
        message = await bounded_json_request(request, max_bytes=64 * 1024)
    except RequestBodyTooLarge:
        return _remote_mcp_response(
            _remote_mcp_error(None, -32600, "Request too large"), status=413)
    except Exception:
        return _remote_mcp_response(
            _remote_mcp_error(None, -32700, "Parse error"), status=400)
    if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
        return _remote_mcp_response(
            _remote_mcp_error(
                message.get("id") if isinstance(message, dict) else None,
                -32600,
                "Invalid Request",
            ),
            status=400,
        )
    request_id = message.get("id")
    rpc_method = str(message.get("method") or "")
    params = message.get("params") or {}
    if rpc_method.startswith("notifications/"):
        return Response(
            None,
            status=202,
            headers={
                "cache-control": "no-store",
                "x-content-type-options": "nosniff",
            },
        )
    if rpc_method == "initialize":
        return _remote_mcp_response(_remote_mcp_result(request_id, {
            "protocolVersion": REMOTE_MCP_PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {
                "name": "ForkMesh Remote MCP", "version": "1.0.0",
            },
            "instructions": (
                "Use these scoped organization task tools only for work "
                "authorized by the revocable ForkMesh credential."
            ),
        }))
    if rpc_method == "ping":
        return _remote_mcp_response(_remote_mcp_result(request_id, {}))
    if rpc_method == "tools/list":
        return _remote_mcp_response(_remote_mcp_result(
            request_id, {"tools": REMOTE_MCP_TOOLS}))
    if rpc_method == "tools/call":
        if not isinstance(params, dict):
            return _remote_mcp_response(
                _remote_mcp_error(request_id, -32602, "Invalid params"))
        name = clean_string(params.get("name"), 80)
        result = await _remote_mcp_call_tool(
            env, request, context, name, params.get("arguments") or {})
        if result is None:
            return _remote_mcp_response(
                _remote_mcp_error(request_id, -32601, "Unknown tool"))
        return _remote_mcp_response(_remote_mcp_result(request_id, result))
    return _remote_mcp_response(
        _remote_mcp_error(request_id, -32601, "Method not found"))
