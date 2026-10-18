"""Mirror Actions control requests and summary v2 revalidation.

The wire contract is www/docs/operations/mirror-actions-control.md. Split out
of entry.py so Cloudflare's startup-memory validation (error 10021) does not
pay for this domain on every isolate; entry.py loads it through a _LazyModule
proxy on the first control request or the first version 2 summary.
"""

import hashlib
import json
import re


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


ACTIONS_CONTROL_OPERATION = "actions-control"
ACTIONS_CONTROL_WIRE_TYPE = "forkmesh.actions-control-v1"
ACTIONS_CONTROL_BODY_MAX_BYTES = 4 * 1024
ACTIONS_CONTROL_RESULT_MAX_BYTES = 4 * 1024
# JSON numbers the browser and the Qt executor both hold exactly.
ACTIONS_MAX_SAFE_INTEGER = 2 ** 53 - 1
# Extra body keys per action; requestId, action and node are always required.
ACTIONS_CONTROL_ACTIONS = {
    "dispatch": frozenset({"workflow", "ref"}),
    "rerun": frozenset({"runId"}),
    "cancel": frozenset({"runId"}),
    "approve": frozenset({"runId"}),
    "reject": frozenset({"runId"}),
    "set-workflow-enabled": frozenset({"workflow", "enabled"}),
    "set-repository-enabled": frozenset({"enabled"}),
}
ACTIONS_CONTROL_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")
ACTIONS_CONTROL_ACTOR_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")
ACTIONS_CONTROL_NODE_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ACTIONS_CONTROL_REF_RE = re.compile(r"^refs/heads/[A-Za-z0-9._/-]{1,200}$")
ACTIONS_WORKFLOW_PATH_RE = re.compile(
    r"^\.forkmesh/[A-Za-z0-9._-]{1,96}\.ya?ml$")

ACTIONS_RUN_STEP_LIMIT = 64
ACTIONS_STEP_NAME_MAX_CHARS = 120
ACTIONS_WORKFLOW_LIMIT = 32
ACTIONS_WORKFLOW_NAME_MAX_CHARS = 160
ACTIONS_WORKFLOW_EVENT_LIMIT = 8
ACTIONS_WORKFLOW_EVENT_MAX_CHARS = 40
ACTIONS_WORKFLOW_RUNNER_LABEL_LIMIT = 8
ACTIONS_WORKFLOW_RUNNER_LABEL_MAX_CHARS = 64
ACTIONS_CONTROL_RESULT_LIMIT = 32
ACTIONS_STEP_STATUSES = frozenset({
    "pending", "running", "success", "failed", "skipped", "cancelled",
})
ACTIONS_CONTROL_REJECTION_REASONS = frozenset({
    "actions_disabled", "invalid_request", "not_awaiting_approval",
    "not_cancellable", "not_manual", "operator_disabled", "ref_not_found",
    "repository_not_found", "run_not_found", "workflow_disabled",
    "workflow_invalid", "workflow_not_found", "wrong_node",
})
ACTIONS_RESULT_V1_FIELDS = frozenset({
    "ok", "node", "updatedAt", "expiresAt", "runs",
})
ACTIONS_RESULT_V2_FIELDS = frozenset({
    "schemaVersion", "actionsEnabled", "workflows", "controls",
})
ACTIONS_RUN_V2_FIELDS = frozenset({"workflowPath", "pullNumber", "steps"})
ACTIONS_WORKFLOW_FIELDS = frozenset({
    "path", "name", "events", "manual", "enabled", "runsOn", "valid",
})
ACTIONS_CONTROL_RESULT_FIELDS = frozenset({
    "requestId", "action", "status", "reason", "runId", "finishedAt",
})


def _actions_integer(value, low, high):
    """True for a real JSON integer in range; a boolean is not an integer."""
    return type(value) is int and low <= value <= high


def _actions_line(value, low, high):
    """True for a bounded single-line string without control characters."""
    return (
        type(value) is str
        and low <= len(value) <= high
        and not any(ord(char) < 32 or ord(char) == 127 for char in value)
    )


def _actions_line_list(value, limit, high):
    return (
        isinstance(value, list)
        and len(value) <= limit
        and all(_actions_line(item, 1, high) for item in value)
    )


def _actions_control_ref(value):
    return bool(
        type(value) is str
        and ACTIONS_CONTROL_REF_RE.fullmatch(value)
        and ".." not in value
        and "//" not in value
        and not value.endswith("/")
        and not value.endswith(".lock")
    )


def _actions_workflow_path(value):
    return bool(
        type(value) is str and ACTIONS_WORKFLOW_PATH_RE.fullmatch(value))


def _actions_control_body(raw):
    """Validate the small browser request before any node sees it."""
    if not isinstance(raw, str) or not raw or (
            len(raw.encode("utf-8")) > ACTIONS_CONTROL_BODY_MAX_BYTES):
        return None

    def reject_duplicates(pairs):
        output = {}
        for key, value in pairs:
            if key in output:
                raise ValueError("duplicate")
            output[key] = value
        return output

    try:
        value = json.loads(raw, object_pairs_hook=reject_duplicates)
    except Exception:
        return None
    if not isinstance(value, dict):
        return None
    action = value.get("action")
    if type(action) is not str or action not in ACTIONS_CONTROL_ACTIONS:
        return None
    extra = ACTIONS_CONTROL_ACTIONS[action]
    required = {"requestId", "action", "node"} | extra
    if set(value) - {"sessionToken"} != required:
        return None
    request_id = value.get("requestId")
    node = value.get("node")
    session_token = value.get("sessionToken", "")
    if (
        type(request_id) is not str
        or not ACTIONS_CONTROL_REQUEST_ID_RE.fullmatch(request_id)
        or type(node) is not str
        or not ACTIONS_CONTROL_NODE_RE.fullmatch(node)
        or type(session_token) is not str
        or len(session_token) > 512
        or ("sessionToken" in value and not session_token.strip())
        or ("workflow" in extra
            and not _actions_workflow_path(value.get("workflow")))
        or ("ref" in extra and not _actions_control_ref(value.get("ref")))
        or ("runId" in extra and not _actions_integer(
            value.get("runId"), 1, ACTIONS_MAX_SAFE_INTEGER))
        or ("enabled" in extra and type(value.get("enabled")) is not bool)
    ):
        return None
    output = {"requestId": request_id, "action": action, "node": node}
    for key in sorted(extra):
        output[key] = value[key]
    if session_token:
        output["sessionToken"] = session_token.strip()
    return output


def _actions_control_gateway_body(data, actor, issued_at):
    """Serialize the exact document the router capability will cover."""
    if not ACTIONS_CONTROL_ACTOR_RE.fullmatch(str(actor or "")):
        return b""
    document = {
        "schemaVersion": 1,
        "type": ACTIONS_CONTROL_WIRE_TYPE,
        "requestId": data["requestId"],
        "action": data["action"],
        "actor": actor,
        "issuedAt": int(issued_at),
    }
    for key in ACTIONS_CONTROL_ACTIONS[data["action"]]:
        document[key] = data[key]
    return json.dumps(
        document, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _actions_control_accepted(value, request_id, node):
    """Accept only the gateway's exact acknowledgement of this request."""
    if (
        not isinstance(value, dict)
        or set(value) != {"ok", "status", "requestId", "node"}
        or value.get("ok") is not True
        or value.get("status") != "accepted"
        or value.get("requestId") != request_id
        or value.get("node") != node
    ):
        return None
    return {
        "ok": True,
        "status": "accepted",
        "requestId": request_id,
        "node": node,
    }


def _actions_workflow(item):
    if (
        not isinstance(item, dict)
        or set(item) != ACTIONS_WORKFLOW_FIELDS
        or not _actions_workflow_path(item.get("path"))
        or not _actions_line(
            item.get("name"), 0, ACTIONS_WORKFLOW_NAME_MAX_CHARS)
        or not _actions_line_list(
            item.get("events"), ACTIONS_WORKFLOW_EVENT_LIMIT,
            ACTIONS_WORKFLOW_EVENT_MAX_CHARS)
        or not _actions_line_list(
            item.get("runsOn"), ACTIONS_WORKFLOW_RUNNER_LABEL_LIMIT,
            ACTIONS_WORKFLOW_RUNNER_LABEL_MAX_CHARS)
        or any(
            type(item.get(field)) is not bool
            for field in ("manual", "enabled", "valid"))
    ):
        return None
    return {
        "path": item["path"],
        "name": item["name"],
        "events": list(item["events"]),
        "manual": item["manual"],
        "enabled": item["enabled"],
        "runsOn": list(item["runsOn"]),
        "valid": item["valid"],
    }


def _actions_control_result(item, latest):
    if (
        not isinstance(item, dict)
        or set(item) != ACTIONS_CONTROL_RESULT_FIELDS
        or type(item.get("requestId")) is not str
        or not ACTIONS_CONTROL_REQUEST_ID_RE.fullmatch(item["requestId"])
        or type(item.get("action")) is not str
        or item["action"] not in ACTIONS_CONTROL_ACTIONS
        or not _actions_integer(
            item.get("runId"), 0, ACTIONS_MAX_SAFE_INTEGER)
        or not _actions_integer(item.get("finishedAt"), 0, latest)
    ):
        return None
    status = item.get("status")
    reason = item.get("reason")
    if not (
        (status == "applied" and reason == "")
        or (status == "rejected" and type(reason) is str
            and reason in ACTIONS_CONTROL_REJECTION_REASONS)
    ):
        return None
    return {
        "requestId": item["requestId"],
        "action": item["action"],
        "status": status,
        "reason": reason,
        "runId": item["runId"],
        "finishedAt": item["finishedAt"],
    }


def _actions_run_v2(item):
    """Validate the version 2 additions of one run."""
    workflow_path = item.get("workflowPath")
    steps = item.get("steps")
    if (
        (workflow_path != "" and not _actions_workflow_path(workflow_path))
        or not _actions_integer(
            item.get("pullNumber"), 0, ACTIONS_MAX_SAFE_INTEGER)
        or not isinstance(steps, list)
        or len(steps) > ACTIONS_RUN_STEP_LIMIT
    ):
        return None
    output_steps = []
    for step in steps:
        if (
            not isinstance(step, dict)
            or set(step) != {"name", "status"}
            or not _actions_line(
                step.get("name"), 0, ACTIONS_STEP_NAME_MAX_CHARS)
            or type(step.get("status")) is not str
            or step["status"] not in ACTIONS_STEP_STATUSES
        ):
            return None
        output_steps.append({"name": step["name"], "status": step["status"]})
    return {
        "workflowPath": workflow_path,
        "pullNumber": item["pullNumber"],
        "steps": output_steps,
    }


def _https_mirror_actions_result_v2(value, expected_node, now):
    """Validate and reduce one version 2 gateway answer.

    The version 1 keys go through entry.py's validator unchanged, so the two
    versions can never disagree about a lease, a run, or a log tail; only the
    additions are checked here.
    """
    if (
        not isinstance(value, dict)
        or set(value) != ACTIONS_RESULT_V1_FIELDS | ACTIONS_RESULT_V2_FIELDS
        or type(value.get("schemaVersion")) is not int
        or value.get("schemaVersion") != 2
        or type(value.get("actionsEnabled")) is not bool
    ):
        return None
    runs = value.get("runs")
    workflows = value.get("workflows")
    controls = value.get("controls")
    if (
        not isinstance(runs, list)
        or not isinstance(workflows, list)
        or len(workflows) > ACTIONS_WORKFLOW_LIMIT
        or not isinstance(controls, list)
        or len(controls) > ACTIONS_CONTROL_RESULT_LIMIT
    ):
        return None
    base_runs = []
    run_additions = []
    for item in runs:
        if not isinstance(item, dict) or not ACTIONS_RUN_V2_FIELDS.issubset(
                item):
            return None
        addition = _actions_run_v2(item)
        if addition is None:
            return None
        run_additions.append(addition)
        base_runs.append({
            key: val for key, val in item.items()
            if key not in ACTIONS_RUN_V2_FIELDS
        })
    checked = _https_mirror_actions_result(
        {
            **{key: value[key] for key in ACTIONS_RESULT_V1_FIELDS},
            "runs": base_runs,
        },
        expected_node, now)
    if checked is None:
        return None
    output_workflows = []
    for item in workflows:
        workflow = _actions_workflow(item)
        if workflow is None:
            return None
        output_workflows.append(workflow)
    if len({item["path"] for item in output_workflows}) != len(
            output_workflows):
        return None
    latest = int(now) + HTTPS_MIRROR_ACTIONS_CLOCK_SKEW_MS
    output_controls = []
    for item in controls:
        control = _actions_control_result(item, latest)
        if control is None:
            return None
        output_controls.append(control)
    return {
        "ok": True,
        "schemaVersion": 2,
        "node": checked["node"],
        "updatedAt": checked["updatedAt"],
        "expiresAt": checked["expiresAt"],
        "actionsEnabled": value["actionsEnabled"],
        "workflows": output_workflows,
        "controls": output_controls,
        "runs": [
            {**run, **addition}
            for run, addition in zip(checked["runs"], run_additions)
        ],
    }


async def _https_mirror_actions_control_proxy(
        env, endpoint, context, data, actor):
    """Hand one signed control request to the node that owns the run ids."""
    if not await _https_mirror_repository_proof(
            env, endpoint, context, ACTIONS_CONTROL_OPERATION):
        return None
    target = https_routing.masked_target_url(
        endpoint["baseUrl"], context["owner"], context["repo"],
        ACTIONS_CONTROL_OPERATION)
    parsed_target = urlparse(target)
    issued_at = int(Date.now())
    body = _actions_control_gateway_body(data, actor, issued_at)
    if not body or len(body) > ACTIONS_CONTROL_BODY_MAX_BYTES:
        return None
    request_id = _b64url_encode(_random_bytes(18))
    body_digest = hashlib.sha256(body).hexdigest()
    message = https_routing.request_message(
        endpoint["node"], "POST", parsed_target.path, body_digest, request_id,
        issued_at)
    router_public_key = _https_mirror_router_public_key(env)
    router_seed = _https_mirror_router_seed(env)
    signature = (
        await ed25519_sign(
            router_public_key, router_seed, message.encode("utf-8"))
        if target and message and router_public_key and router_seed else "")
    if not signature:
        return None
    headers = {
        "Content-Type": "application/json",
        "X-ForkMesh-Node": endpoint["node"],
        "X-ForkMesh-Request-Id": request_id,
        "X-ForkMesh-Issued-At": str(issued_at),
        "X-ForkMesh-Body-Sha256": body_digest,
        "X-ForkMesh-Signature": signature,
    }
    try:
        # Native AbortSignal timeout, not asyncio.wait_for — see the
        # no-concurrent-tasks rule at the top of entry.py.
        upstream = await js_fetch_with_timeout(
            target,
            {
                "method": "POST",
                "headers": headers,
                "body": Uint8Array.new(_to_js(body)),
                "redirect": "manual",
            },
            HTTPS_MIRROR_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(upstream, "status", 0) or 0) != 202:
            return None
        announced = int(upstream.headers.get("content-length") or 0)
        if announced > ACTIONS_CONTROL_RESULT_MAX_BYTES:
            return None
        raw = str(await upstream.text())
        if len(raw.encode("utf-8")) > ACTIONS_CONTROL_RESULT_MAX_BYTES:
            return None
        value = json.loads(raw)
    except Exception:
        return None
    return _actions_control_accepted(
        value, data["requestId"], endpoint["node"])


def _actions_node_unavailable():
    return json_response(
        {"error": "actions_node_unavailable"}, status=503,
        cache_control="no-store", extra_headers=EXPECTED_DEGRADED_HEADERS)


def _instance_actions(env):
    return bool(str(getattr(env, "ACTIONS_INSTANCE_RUNNER", "") or "").strip())


def _runner_runtime(env):
    import runner_leases
    runner_leases._bind_runtime(globals())
    return runner_leases.WorkerRuntime(env, None)


async def _instance_actor(env, request, owner, repo, data=None):
    _, record = await _account_session_record(env, request, data or {})
    actor = clean_string(
        (record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor:
        return "", False
    try:
        import runner_actions
        storage, _public = await runner_actions.identity(
            _runner_runtime(env), owner, repo)
        allowed = bool(
            ACTIONS_CONTROL_ACTOR_RE.fullmatch(actor)
            and (actor == storage
                 or await _account_owns_node(env, actor, storage)
                 or await _org_write_allowed(env, storage, repo, actor)))
    except Exception:
        allowed = False
    return actor, allowed


def _instance_error(error, status, message=""):
    body = {"error": error}
    if message:
        body["message"] = message
    return json_response(body, status=status, cache_control="no-store")


async def _instance_actions_control(env, request, owner, repo):
    owner = str(owner or "").strip().lower()
    try:
        raw = str(await request.text())
    except Exception:
        return _instance_error("invalid_request", 400)
    if len(raw.encode("utf-8")) > ACTIONS_CONTROL_BODY_MAX_BYTES * 4:
        return _instance_error("request_too_large", 413)
    try:
        data = json.loads(raw)
    except Exception:
        data = None
    if not isinstance(data, dict) or data.get("action") not in (
            "dispatch", "cancel", "rerun"):
        return _instance_error("invalid_request", 400)
    actor, allowed = await _instance_actor(env, request, owner, repo, data)
    if not actor:
        return _instance_error("invalid_session", 401)
    target = owner + "/" + repo
    action = data["action"]
    if not allowed:
        await _audit_sensitive_action(
            env, actor, "repository.actions_control", "repository", target,
            "denied", {"action": action})
        return _instance_error("forbidden", 403)
    import runner_actions
    runtime = _runner_runtime(env)
    await runtime.ensure_schema()
    details = {"action": action, "node": "runner"}
    if action == "dispatch":
        workflow = data.get("workflow")
        ref = data.get("ref")
        if isinstance(ref, str) and not ref.startswith("refs/"):
            ref = "refs/heads/" + ref
        if not _actions_workflow_path(workflow) or not _actions_control_ref(
                ref) or (data.get("inputs") is not None
                         and not isinstance(data.get("inputs"), dict)):
            return _instance_error("invalid_request", 400)
        data["ref"] = ref
        job, reason, message = await runner_actions.dispatch(
            runtime, owner, repo, workflow, data["ref"], data.get("inputs"),
            actor)
        details["workflow"] = workflow
        run_id = job * runner_actions.RUN_ID_STRIDE
    else:
        if not _actions_integer(data.get("runId"), 1,
                                ACTIONS_MAX_SAFE_INTEGER):
            return _instance_error("invalid_request", 400)
        details["runId"] = data["runId"]
        if action == "cancel":
            reason = await runner_actions.cancel(
                runtime, owner, repo, data["runId"])
            run_id, message = data["runId"], ""
        else:
            job, reason = await runner_actions.rerun(
                runtime, owner, repo, data["runId"], actor)
            run_id, message = job * runner_actions.RUN_ID_STRIDE, ""
    if reason:
        await _audit_sensitive_action(
            env, actor, "repository.actions_control", "repository", target,
            "failure", {**details, "reason": reason})
        status = {"run_not_found": 404, "workflow_not_found": 404,
                  "input_required": 400, "input_invalid": 400,
                  "invalid_request": 400, "ref_not_found": 400}.get(
                      reason, 409)
        return _instance_error(reason, status, message)
    await _audit_sensitive_action(
        env, actor, "repository.actions_control", "repository", target,
        "success", details)
    return json_response(
        {"ok": True, "status": "applied", "action": action, "runId": run_id},
        status=202, cache_control="no-store")


async def repository_actions_instance_status_handler(env, request, owner,
                                                    repo):
    owner = str(owner or "").strip().lower()
    actor, allowed = await _instance_actor(env, request, owner, repo)
    if not actor:
        return _instance_error("invalid_session", 401)
    if not allowed:
        await _audit_sensitive_action(
            env, actor, "repository.actions_status", "repository",
            owner + "/" + repo, "denied", {})
        return _instance_error("forbidden", 403)
    import runner_actions
    runtime = _runner_runtime(env)
    await runtime.ensure_schema()
    snapshot = await runner_actions.snapshot(runtime, owner, repo, always=True)
    await _audit_sensitive_action(
        env, actor, "repository.actions_status", "repository",
        owner + "/" + repo, "success", {"node": snapshot["node"]})
    return json_response(
        snapshot,
        cache_control="private, no-store, max-age=0, must-revalidate")


async def repository_actions_run_log_handler(env, request, owner, repo,
                                             run_id):
    if method_name(request) != "GET":
        return json_response({"error": "method_not_allowed"}, status=405)
    owner = str(owner or "").strip().lower()
    actor, allowed = await _instance_actor(env, request, owner, repo)
    if not actor:
        return _instance_error("invalid_session", 401)
    if not allowed:
        await _audit_sensitive_action(
            env, actor, "repository.actions_status", "repository",
            owner + "/" + repo, "denied", {"runId": run_id})
        return _instance_error("forbidden", 403)
    import runner_actions
    runtime = _runner_runtime(env)
    await runtime.ensure_schema()
    found = await runner_actions.run_log(runtime, owner, repo, run_id)
    if found is None:
        return _instance_error("run_not_found", 404)
    return json_response(
        {"ok": True, "runId": int(run_id), **found},
        cache_control="private, no-store, max-age=0, must-revalidate")


async def repository_actions_control_handler(env, request, owner, repo):
    """Authorize, audit, and courier one Actions control request.

    Run ids belong to one executor, so the request goes to the node the
    browser names — the one whose summary it is looking at — and never fails
    over. The executor verifies the router capability itself, and the browser
    reads the outcome from the next summary's ``controls``.
    """
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    if _instance_actions(env):
        return await _instance_actions_control(env, request, owner, repo)
    try:
        context = await _https_mirror_public_context(env, owner, repo)
    except _HttpsMirrorContextUnavailable:
        return json_response(
            {"error": "mirror_unavailable"}, status=503,
            cache_control="no-store",
            extra_headers=EXPECTED_DEGRADED_HEADERS)
    if context is None:
        return json_response(
            {"error": "not_found"}, status=404, cache_control="no-store")
    try:
        announced = int(request.headers.get("content-length") or 0)
    except Exception:
        announced = 0
    if announced > ACTIONS_CONTROL_BODY_MAX_BYTES:
        return json_response(
            {"error": "request_too_large"}, status=413,
            cache_control="no-store")
    try:
        raw = str(await request.text())
    except Exception:
        return json_response(
            {"error": "invalid_request"}, status=400,
            cache_control="no-store")
    if len(raw.encode("utf-8")) > ACTIONS_CONTROL_BODY_MAX_BYTES:
        return json_response(
            {"error": "request_too_large"}, status=413,
            cache_control="no-store")
    data = _actions_control_body(raw)
    if data is None:
        return json_response(
            {"error": "invalid_request"}, status=400,
            cache_control="no-store")
    # A cookie session on this POST is refused inside _account_session_record
    # unless the request's Origin is this site or an allowed one.
    _, actor_record = await _account_session_record(env, request, data)
    actor = clean_string(
        (actor_record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not actor:
        return json_response(
            {"error": "invalid_session"}, status=401,
            cache_control="no-store")
    try:
        authorized = bool(
            ACTIONS_CONTROL_ACTOR_RE.fullmatch(actor)
            and (
                actor == context["owner"]
                or await _account_owns_node(env, actor, context["owner"])
                or await _org_write_allowed(
                    env, context["owner"], context["repo"], actor)
            )
        )
    except Exception:
        authorized = False
    target = context["owner"] + "/" + context["repo"]
    if not authorized:
        await _audit_sensitive_action(
            env, actor, "repository.actions_control", "repository", target,
            "denied", {"action": data["action"]})
        return json_response(
            {"error": "forbidden"}, status=403,
            cache_control="no-store")
    candidates = await _https_mirror_candidates(
        env, context, world_request_country(request), sticky=data["node"],
        operation=ACTIONS_CONTROL_OPERATION)
    endpoint = next(
        (candidate for candidate in candidates
         if candidate["node"] == data["node"]),
        None,
    )
    if endpoint is None:
        return _actions_node_unavailable()
    result = await _https_mirror_actions_control_proxy(
        env, endpoint, context, data, actor)
    if result is None:
        # Never another node: its executor would not know this run id, and the
        # named one may already hold the request under the same requestId.
        return _actions_node_unavailable()
    details = {"action": data["action"], "node": endpoint["node"]}
    for key in ("runId", "workflow"):
        if key in data:
            details[key] = data[key]
    await _audit_sensitive_action(
        env, actor, "repository.actions_control", "repository", target,
        "success", details)
    return json_response(result, status=202, cache_control="no-store")
