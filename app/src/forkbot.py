"""forkbot routes, loaded on demand.

Split out of entry.py so Cloudflare's startup-memory validation (error
10021) does not pay for this domain on every isolate; entry.py loads it
through a _LazyModule proxy on the first request that needs it.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _forkbot_extract_mention_command(message):
    text = clean_string(message, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return ""
    match = re.search(
        r"(?is)(?:^|[^A-Za-z0-9_-])@?forkbot\b[:,]?\s*(.*)$", text)
    return match.group(1).strip() if match else ""
def _forkbot_parse_issue_command(command):
    # Fast, offline path for an explicitly phrased request ("create an issue
    # to ..."). A miss here is NOT a rejection: the handler falls back to the
    # AI intent classifier (_forkbot_ai_interpret), which recognises the same
    # intent from natural phrasing ("forkbot can you track the flaky login
    # test?") and from the surrounding conversation. This regex just spares an
    # AI round trip for the obvious wording and still works when AI is off.
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    # Verb-first ("create an issue to ...") or noun-first ("issue to ...",
    # "bug: ...") — both common phrasings, both answerable offline.
    match = re.match(
        r"(?is)^(?:please\s+|can\s+you\s+|could\s+you\s+|would\s+you\s+|"
        r"pls\s+|plz\s+)*"
        r"(?:(?:create|open|file|make|report|log|raise|add|track|submit)\s+)?"
        r"(?:an?\s+|a\s+new\s+)?"
        r"(?:issue|bug|ticket|task|feature\s+request)\b"
        r"\s*(?:(?:to|for|about|that|saying|called|titled|re|regarding)\s+|"
        r"[:\-]\s*)?"
        r"(.+)$",
        text,
    )
    if not match:
        return None
    description = re.sub(r"\s+", " ", match.group(1)).strip(" .:-")
    if len(description) < 3:
        return None
    return {"description": description}
def _forkbot_command_hints_issue(command):
    """Cheap offline signal that a ForkBot mention is about recording work —
    used ONLY when the AI classifier is unreachable, so an unreachable model
    degrades to filing the raw request instead of refusing with the help hint
    (that refusal is exactly what users saw while the AI binding was silently
    failing)."""
    return bool(re.search(
        r"(?i)\b(issues?|bugs?|tickets?|tasks?|todo|feature\s+request|"
        r"create|open|file|track|log|report)\b",
        clean_string(command, FORKBOT_MAX_COMMAND)))
def _forkbot_polite_prefix():
    # Shared lead-in the offline parsers strip: "please can you ..." etc.
    return (r"(?:please\s+|can\s+you\s+|could\s+you\s+|would\s+you\s+|"
            r"pls\s+|plz\s+)*")
def _forkbot_parse_list_command(command):
    """Offline fast path for "list the last N issues" phrasings. Mirrors the
    create-issue fast path: a miss is NOT a rejection (the AI classifier still
    gets a shot at natural wording), a hit spares the AI round trip and keeps
    the feature working when AI is off. Returns {"count": n} or None."""
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    match = re.match(
        r"(?is)^" + _forkbot_polite_prefix() +
        r"(?:list|show(?:\s+me)?|display|give\s+me|what\s+are)\s+"
        r"(?:the\s+)?(?:last|latest|most\s+recent|recent|newest)?\s*"
        r"(\d{1,3})?\s*(?:few\s+)?issues\b",
        text,
    )
    if not match:
        return None
    try:
        count = int(match.group(1) or FORKBOT_LIST_DEFAULT)
    except (TypeError, ValueError):
        count = FORKBOT_LIST_DEFAULT
    return {"count": max(1, min(count, FORKBOT_LIST_MAX))}
def _forkbot_parse_count_command(command):
    """Offline fast path for "how many issues are there" phrasings. These ask
    for a total, not a listing, so they must not fall through to the list
    parser above (which would hand back a few issues and never say how many
    exist in total). Returns True or None."""
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    match = re.match(
        r"(?is)^" + _forkbot_polite_prefix() +
        r"(?:how\s+many\s+issues\b(?:\s+are\s+there|\s+exist|"
        r"\s+do\s+we\s+have)?|"
        r"(?:what(?:'s|\s+is)\s+the\s+)?(?:number|count)\s+of\s+issues|"
        r"issue\s+count)\s*[?.!]*$",
        text,
    )
    return {} if match else None
def _forkbot_parse_search_command(command):
    """Offline fast path for "search issues for X" phrasings. Requires the
    word issue(s) so a bare "find the bug" still flows to the create/AI paths.
    Returns {"query": text} or None."""
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    match = re.match(
        r"(?is)^" + _forkbot_polite_prefix() +
        r"(?:search|find|look\s+(?:for|up)|grep)\s+"
        r"(?:the\s+|any\s+|all\s+)?(?:open\s+|closed\s+)?issues?\b\s*"
        r"(?:(?:for|about|regarding|matching|mentioning|containing|on|with)"
        r"\s+)?[:\-]?\s*(.+)$",
        text,
    )
    if not match:
        return None
    query = re.sub(r"\s+", " ", match.group(1)).strip(" .:-\"'")
    if len(query) < 2:
        return None
    return {"query": query[:100]}
def _forkbot_parse_agent_command(command):
    """Offline fast path for "start an agent on the latest issue / issue #N".
    Returns {"issueNumber": n} (0 = the most recent issue) or None."""
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    match = re.match(
        r"(?is)^" + _forkbot_polite_prefix() +
        r"(?:start|run|launch|kick\s+off|spin\s+up|put|assign|set|sic)\s+"
        r"(?:an?\s+)?(?:coding\s+|code\s+)?agent\s+"
        r"(?:on|to|at|for|onto|against)?\s*(?:the\s+)?"
        r"(?:most\s+recent|latest|newest|last|recent)?\s*"
        r"issue\s*(?:#\s*)?(\d+)?",
        text,
    )
    if not match:
        return None
    try:
        number = int(match.group(1) or 0)
    except (TypeError, ValueError):
        number = 0
    return {"issueNumber": number}
def _forkbot_parse_show_command(command):
    """Offline fast path for "show issue #N" / "show the latest issue".
    "open issue ..." intentionally stays with the create-issue parser (there
    "open" is the filing verb). Returns {"issueNumber": n} (0 = latest) or
    None."""
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip()
    if not text:
        return None
    match = re.match(
        r"(?is)^" + _forkbot_polite_prefix() +
        r"(?:show(?:\s+me)?|view|describe|summarize|"
        r"what(?:'s|\s+is)\s+(?:in|the\s+status\s+of))\s+"
        r"(?:the\s+)?(?:most\s+recent\s+|latest\s+|newest\s+|last\s+)?"
        r"issue\s*(?:#\s*)?(\d+)?\s*$",
        text,
    )
    if not match:
        return None
    try:
        number = int(match.group(1) or 0)
    except (TypeError, ValueError):
        number = 0
    return {"issueNumber": number}
def _forkbot_parse_help_command(command):
    text = clean_string(command, FORKBOT_MAX_COMMAND).strip().lower()
    if not text:
        return False
    return bool(re.match(
        r"^(?:help|\?+|what\s+can\s+you\s+do|what\s+do\s+you\s+do|"
        r"commands|usage)\s*[!.?]*$", text))
def _forkbot_help_message():
    # Also the fallback reply for anything ForkBot could not map to an action,
    # so it doubles as discoverable usage text.
    return (
        "I can open an issue from the conversation (\"forkbot open an issue "
        "for the flaky login test\"), list recent issues (\"forkbot list the "
        "last 5 issues\" - up to %d), say how many issues there are "
        "(\"forkbot how many issues are there?\"), search issues (\"forkbot "
        "search issues for relay retries\"), show one issue (\"forkbot show "
        "issue #12\"), and - for the repo owner - start a coding agent "
        "(\"forkbot start an agent on the latest issue\")." % FORKBOT_LIST_MAX
    )
def _forkbot_context_text(context):
    """Flatten the recent conversation the client forwarded into a short,
    plain-text transcript for the AI. Each entry is {sender, text}; the client
    already excludes ForkBot's own lines and caps the count, but we bound it
    again here so a hostile client can't blow the prompt budget."""
    if not isinstance(context, list):
        return ""
    lines = []
    for entry in context[-FORKBOT_CONTEXT_MAX_MESSAGES:]:
        if not isinstance(entry, dict):
            continue
        sender = clean_string(entry.get("sender", ""), MAX_NODE_NAME).strip() or "user"
        text = clean_string(entry.get("text", ""), FORKBOT_CONTEXT_MAX_CHARS).strip()
        if not text:
            continue
        lines.append("%s: %s" % (sender, text))
    return "\n".join(lines)
def _forkbot_issue_title(description):
    text = re.sub(r"\s+", " ", clean_string(description, 240)).strip(" .:-")
    if not text:
        return "Issue from #general"
    for sep in (". ", "! ", "? "):
        idx = text.find(sep)
        if 12 <= idx <= 120:
            text = text[:idx]
            break
    if len(text) > 120:
        text = text[:117].rstrip() + "..."
    return text or "Issue from #general"
def _forkbot_fallback_issue_fields(description):
    body = clean_string(description, MAX_ISSUE_BYTES).strip()
    title = _forkbot_issue_title(body)
    return {"title": title, "body": body}
def _forkbot_json_object_from_text(text):
    if not isinstance(text, str):
        return None
    raw = text.strip()
    if not raw:
        return None
    try:
        parsed = json.loads(raw)
        return parsed if isinstance(parsed, dict) else None
    except Exception:
        pass
    start = raw.find("{")
    end = raw.rfind("}")
    if start >= 0 and end > start:
        try:
            parsed = json.loads(raw[start:end + 1])
            return parsed if isinstance(parsed, dict) else None
        except Exception:
            return None
    return None
def _forkbot_clean_ai_issue_fields(parsed):
    if not isinstance(parsed, dict):
        return None
    title = clean_string(parsed.get("title", ""), 240).strip()
    body = clean_string(parsed.get("body", ""), MAX_ISSUE_BYTES).strip()
    if not title or not body:
        return None
    return {"title": _forkbot_issue_title(title), "body": body}
def _forkbot_ai_default_model(env):
    """The model used when the caller picks nothing: the deployment's
    FORKBOT_AI_MODEL var, else the code default."""
    return clean_string(getattr(env, "FORKBOT_AI_MODEL", ""), 120) or \
        FORKBOT_AI_DEFAULT_MODEL
def _forkbot_ai_model_options(env):
    """The pickable Workers AI models, default first, for the picker UI.

    The deployment default is always offered even when FORKBOT_AI_MODEL names a
    model missing from FORKBOT_AI_MODEL_CHOICES — otherwise a self-hosted relay
    that points the var at its own model would show a picker that cannot select
    what it is actually running."""
    default_model = _forkbot_ai_default_model(env)
    options = []
    for model_id, label, description in FORKBOT_AI_MODEL_CHOICES:
        options.append({
            "id": model_id,
            "label": label,
            "description": description,
            "default": model_id == default_model,
        })
    if not any(option["default"] for option in options):
        options.insert(0, {
            "id": default_model,
            "label": default_model.rsplit("/", 1)[-1],
            "description": "This relay's configured model.",
            "default": True,
        })
    options.sort(key=lambda option: 0 if option["default"] else 1)
    return options
def _forkbot_resolve_ai_model(env, requested=""):
    """Map a caller-supplied model pick onto an allowed Workers AI model id.

    Unknown, empty, or malformed picks fall back to the deployment default
    rather than erroring: a stale picker value in someone's browser must not
    turn ForkBot off for them."""
    wanted = clean_string(requested or "", 120).strip()
    if not wanted:
        return _forkbot_ai_default_model(env)
    for option in _forkbot_ai_model_options(env):
        if option["id"] == wanted:
            return option["id"]
    return _forkbot_ai_default_model(env)
def _forkbot_ai_model_not_found_error(error):
    text = _safe_error_text(error).lower()
    if not text:
        return False
    return any(marker in text for marker in (
        "not found",
        "not_found",
        "model not found",
        "unknown model",
        "does not exist",
        # What Workers AI actually says for a retired/mistyped id
        # ("5007: No such model @cf/... or task"); the wordings above never
        # matched it, so the fallback chain sat unused in production.
        "no such model",
        "no such task",
        "invalid model",
        "unsupported model",
    ))
def _forkbot_ai_fallback_models(env, requested=""):
    """Models we can try for one call, in the order they should be billed."""
    default = _forkbot_ai_default_model(env)
    fallback = []
    wanted = clean_string(requested or "", 120).strip()
    if wanted and wanted not in fallback:
        fallback.append(wanted)
    if default and default not in fallback:
        fallback.append(default)
    for model_id, _, _ in FORKBOT_AI_MODEL_CHOICES:
        if model_id not in fallback:
            fallback.append(model_id)
    return fallback
async def forkbot_models_handler(env, request):
    """List the Workers AI models a chat client may send its prompt to."""
    if method_name(request) not in ("GET", "HEAD"):
        return json_response({"error": "method_not_allowed"}, status=405)
    options = _forkbot_ai_model_options(env)
    default_model = _forkbot_ai_default_model(env)
    return json_response({
        "ok": True,
        "provider": "cloudflare-workers-ai",
        "default": default_model,
        "models": options,
    })
async def _forkbot_run_ai(
    env, system_prompt, user_prompt, schema=None, model="", max_tokens=512,
    outcome=None,
):
    """Run the Workers AI chat model and return the raw response text/object
    (or None). Shared by the issue-drafting and intent-classification helpers.

    Pass a dict as `outcome` to learn what happened beyond None/not-None:
    "model" is the id that actually answered (a fallback when the pick was
    rejected) and "failure" is one of missing_binding / model_not_found /
    provider_error / unusable_response. POST /api/ai/ask needs
    model_not_found separated out because that is the one failure the person
    at the composer can fix by picking another model.

    When `schema` is given, the first attempt requests Workers AI JSON mode
    (response_format json_schema) so a supporting model MUST return valid
    JSON; models/plans without JSON mode fall back to a plain prompt-only
    attempt.

    `model` is an optional caller pick (see _forkbot_resolve_ai_model); an
    unknown one silently uses the deployment default. Every failure path logs
    the reason — the original implementation
    swallowed all exceptions, which left ForkBot silently degraded (raw-echo
    issue titles, natural requests answered with the help hint) with nothing
    in the logs to say why."""
    if outcome is None:
        outcome = {}
    ai = getattr(env, "AI", None)
    if ai is None or js_nullish(ai) or not hasattr(ai, "run"):
        outcome["failure"] = "missing_binding"
        return None
    model = _forkbot_resolve_ai_model(env, model)
    candidates = _forkbot_ai_fallback_models(env, model)
    base_payload = {
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt},
        ],
        "max_tokens": max_tokens,
    }
    attempts = []
    if schema:
        with_format = dict(base_payload)
        with_format["response_format"] = {
            "type": "json_schema", "json_schema": schema,
        }
        attempts.append(with_format)
    attempts.append(base_payload)
    result = None
    ran = False
    last_error = None
    # Every candidate rejected as "no such model" means the deployment's model
    # ids are stale, not that inference is broken — worth saying so distinctly
    # both in the log and to the caller.
    all_missing = True
    for candidate in candidates:
        for payload in attempts:
            try:
                result = await ai.run(candidate, to_js(payload))
                model = candidate
                ran = True
                break
            except Exception as error:
                last_error = error
                if _forkbot_ai_model_not_found_error(error):
                    break
                all_missing = False
        if ran:
            break
    if not ran:
        outcome["failure"] = ("model_not_found" if all_missing
                              else "provider_error")
        await log_error(
            env, 500, "AI", "forkbot/ai",
            "ForkBot AI call failed (%s%s): %s"
            % (model,
               ", no listed model exists on this account" if all_missing
               else "",
               _safe_error_text(last_error)[:300]))
        return None
    outcome["model"] = model
    try:
        if hasattr(result, "to_py"):
            result = result.to_py()
    except Exception:
        pass
    if isinstance(result, dict):
        for key in ("response", "text", "output"):
            value = result.get(key)
            if isinstance(value, str):
                return value
            # JSON mode returns the parsed object under "response".
            if isinstance(value, dict):
                return value
        # Current Workers AI chat models such as Llama 4 Scout and Gemma 4
        # return the OpenAI Chat Completions shape instead of the older
        # top-level {"response": "..."} shape. Accept the first usable choice
        # and both content encodings Cloudflare documents: a string or an array
        # of typed text parts.
        choices = result.get("choices")
        if isinstance(choices, list):
            for choice in choices:
                if not isinstance(choice, dict):
                    continue
                value = choice.get("text")
                if isinstance(value, str) and value.strip():
                    return value
                message = choice.get("message")
                if not isinstance(message, dict):
                    continue
                content = message.get("content")
                if isinstance(content, str) and content.strip():
                    return content
                if isinstance(content, list):
                    parts = []
                    for part in content:
                        if isinstance(part, str):
                            parts.append(part)
                        elif isinstance(part, dict):
                            text = part.get("text")
                            if isinstance(text, str):
                                parts.append(text)
                    joined = "".join(parts)
                    if joined.strip():
                        return joined
        # Some bindings return the parsed object directly rather than a text
        # field; hand the dict back so the caller can read fields off it.
        return result
    if isinstance(result, str):
        return result
    outcome["failure"] = "unusable_response"
    await log_error(
        env, 500, "AI", "forkbot/ai",
        "ForkBot AI returned an unusable %s response (%s)"
        % (type(result).__name__, model))
    return None
# Cloudflare models as coding agents (adhoc #1634): POST /api/ai/agent runs
# one tool-use turn for the desktop's bundled Workers AI agent script. Unlike
# /api/ai/ask (one prompt, one plain-text answer) a turn carries the whole
# conversation so far plus the tool declarations, and the answer keeps the
# model's tool_calls so the script can execute them in the session worktree
# and send the results back as the next turn.
AI_AGENT_MAX_MESSAGES = 200
AI_AGENT_MAX_CONTENT = 60000
AI_AGENT_MAX_TOOLS = 24
# write_file / replace_text calls carry file content inside their JSON
# arguments; at 2048 a mid-sized edit was cut off mid-string and reached the
# script as a call with no usable arguments.
AI_AGENT_MAX_TOKENS = 4096
AI_AGENT_MAX_REPLY = 16000
# One Ed25519 ticket covers a whole agent run (the conversation grows every
# turn, so the signature cannot bind the payload the way /api/ai/ask's does);
# instead the ticket is time-boxed to the longest plausible run.
AI_AGENT_TICKET_MAX_AGE_MS = 6 * 60 * 60 * 1000
# Agent loops make many model calls per task, so they get their own rolling
# window instead of draining the composer's much smaller ask allowance.
AI_AGENT_RATE_WINDOW_MS = 60 * 60 * 1000
AI_AGENT_MAX_PER_WINDOW = 500
# Provider wordings for a conversation longer than the model's context window
# (Workers AI reports it as error 5021). Every fallback model has the same or
# a smaller window, so this failure is answered with 413 context_too_long and
# the script compacts its history instead of the relay billing more attempts.
AI_AGENT_CONTEXT_OVERFLOW_MARKERS = (
    "5021:",
    "context window",
    "context length",
    "maximum context",
    "too many tokens",
    "input is too long",
    "prompt is too long",
)
def _ai_agent_tool_arguments(raw):
    """Tool-call arguments as a dict: Workers AI returns them either parsed
    (legacy {response, tool_calls} shape) or as the OpenAI JSON string."""
    if isinstance(raw, dict):
        return raw
    if isinstance(raw, str) and raw.strip():
        try:
            parsed = json.loads(raw)
            return parsed if isinstance(parsed, dict) else {}
        except Exception:
            return {}
    return {}
def _ai_agent_clean_tool_calls(raw):
    """Validate the assistant tool_calls a client echoes back into history,
    rebuilt in the OpenAI wire shape (arguments as a JSON string)."""
    calls = []
    if not isinstance(raw, list):
        return calls
    for call in raw[:AI_AGENT_MAX_TOOLS]:
        if not isinstance(call, dict):
            continue
        function = call.get("function")
        function = function if isinstance(function, dict) else {}
        name = clean_string(function.get("name", call.get("name", "")), 64)
        name = name.strip()
        if not name:
            continue
        arguments = function.get("arguments", call.get("arguments"))
        if isinstance(arguments, dict):
            arguments = json.dumps(arguments)
        if not isinstance(arguments, str):
            arguments = "{}"
        calls.append({
            "id": clean_string(str(call.get("id", "") or ""), 64)
            or "call_%d" % len(calls),
            "type": "function",
            "function": {
                "name": name,
                "arguments": arguments[:AI_AGENT_MAX_CONTENT],
            },
        })
    return calls
def _ai_agent_clean_messages(raw):
    """Rebuild the conversation from client JSON: only the four chat roles,
    string content, and (per role) the tool-call fields the models expect."""
    messages = []
    if not isinstance(raw, list):
        return messages
    if len(raw) > AI_AGENT_MAX_MESSAGES:
        # Over the cap, keep the leading system prompt and the NEWEST turns.
        # Cutting the tail instead hid the model's latest tool results from
        # it, so a long run re-issued the same calls until it stalled.
        head = []
        for message in raw[:4]:
            if not isinstance(message, dict) or message.get("role") != "system":
                break
            head.append(message)
        tail = raw[len(raw) - (AI_AGENT_MAX_MESSAGES - len(head)):]
        # A tool result whose call fell off the front answers nothing.
        while tail and isinstance(tail[0], dict) and \
                tail[0].get("role") == "tool":
            tail = tail[1:]
        raw = head + tail
    for message in raw:
        if not isinstance(message, dict):
            continue
        role = clean_string(message.get("role", ""), 16).strip()
        if role not in ("system", "user", "assistant", "tool"):
            continue
        content = message.get("content")
        if not isinstance(content, str):
            content = ""
        entry = {"role": role, "content": content[:AI_AGENT_MAX_CONTENT]}
        if role == "assistant":
            calls = _ai_agent_clean_tool_calls(message.get("tool_calls"))
            if calls:
                entry["tool_calls"] = calls
        elif role == "tool":
            call_id = clean_string(message.get("tool_call_id", ""), 64).strip()
            if call_id:
                entry["tool_call_id"] = call_id
            name = clean_string(message.get("name", ""), 64).strip()
            if name:
                entry["name"] = name
        messages.append(entry)
    return messages
def _ai_agent_clean_tools(raw):
    """Validate the client's tool declarations (OpenAI function format)."""
    tools = []
    if not isinstance(raw, list):
        return tools
    for tool in raw[:AI_AGENT_MAX_TOOLS]:
        if not isinstance(tool, dict):
            continue
        function = tool.get("function")
        function = function if isinstance(function, dict) else {}
        name = clean_string(function.get("name", ""), 64).strip()
        if not name:
            continue
        cleaned = {"name": name}
        description = function.get("description")
        if isinstance(description, str) and description.strip():
            cleaned["description"] = description[:2000]
        parameters = function.get("parameters")
        if isinstance(parameters, dict):
            cleaned["parameters"] = parameters
        tools.append({"type": "function", "function": cleaned})
    return tools
def _ai_agent_normalized_result(result):
    """Reduce the two Workers AI response shapes — legacy {response,
    tool_calls} and OpenAI chat completions — to the one
    {reply, toolCalls, usage} object the agent script consumes. toolCalls
    arguments come back parsed so the script never re-parses JSON strings."""
    reply = ""
    calls_raw = []
    usage = {}
    if isinstance(result, str):
        reply = result
    elif isinstance(result, dict):
        value = result.get("response")
        if isinstance(value, str):
            reply = value
        raw_calls = result.get("tool_calls")
        if isinstance(raw_calls, list):
            calls_raw = raw_calls
        choices = result.get("choices")
        if isinstance(choices, list) and not (reply or calls_raw):
            for choice in choices:
                if not isinstance(choice, dict):
                    continue
                message = choice.get("message")
                if not isinstance(message, dict):
                    continue
                content = message.get("content")
                if isinstance(content, str):
                    reply = content
                elif isinstance(content, list):
                    parts = []
                    for part in content:
                        if isinstance(part, str):
                            parts.append(part)
                        elif isinstance(part, dict):
                            text = part.get("text")
                            if isinstance(text, str):
                                parts.append(text)
                    reply = "".join(parts)
                message_calls = message.get("tool_calls")
                if isinstance(message_calls, list):
                    calls_raw = message_calls
                if reply or calls_raw:
                    break
        raw_usage = result.get("usage")
        if isinstance(raw_usage, dict):
            for source, target in (("prompt_tokens", "inputTokens"),
                                   ("completion_tokens", "outputTokens")):
                try:
                    usage[target] = int(raw_usage.get(source) or 0)
                except (TypeError, ValueError):
                    pass
    calls = []
    for call in calls_raw[:AI_AGENT_MAX_TOOLS]:
        if not isinstance(call, dict):
            continue
        function = call.get("function")
        function = function if isinstance(function, dict) else {}
        name = clean_string(function.get("name", call.get("name", "")), 64)
        name = name.strip()
        if not name:
            continue
        raw_arguments = function.get("arguments", call.get("arguments"))
        entry = {
            "id": clean_string(str(call.get("id", "") or ""), 64)
            or "call_%d" % len(calls),
            "name": name,
            "arguments": _ai_agent_tool_arguments(raw_arguments),
        }
        # Arguments that do not parse (usually cut off by max_tokens) are
        # flagged rather than passed on as {}, so the script can tell the
        # model its call was malformed instead of "missing argument: path".
        if isinstance(raw_arguments, str) and raw_arguments.strip() and \
                not entry["arguments"]:
            try:
                valid = isinstance(json.loads(raw_arguments), dict)
            except Exception:
                valid = False
            if not valid:
                entry["invalidArguments"] = raw_arguments[:2000]
        calls.append(entry)
    return {
        "reply": (reply or "")[:AI_AGENT_MAX_REPLY],
        "toolCalls": calls,
        "usage": usage,
    }
async def _forkbot_run_ai_agent(
    env, messages, tools=None, model="", max_tokens=AI_AGENT_MAX_TOKENS,
    outcome=None, temperature=None,
):
    """Run one agent turn against Workers AI and return the normalized
    {reply, toolCalls, usage} (or None). Shares the model allowlist and
    fallback chain with _forkbot_run_ai but keeps the raw tool_calls, which
    the plain-text helper deliberately flattens away."""
    if outcome is None:
        outcome = {}
    ai = getattr(env, "AI", None)
    if ai is None or js_nullish(ai) or not hasattr(ai, "run"):
        outcome["failure"] = "missing_binding"
        return None
    model = _forkbot_resolve_ai_model(env, model)
    candidates = _forkbot_ai_fallback_models(env, model)
    payload = {"messages": messages, "max_tokens": max_tokens}
    if tools:
        payload["tools"] = tools
    if temperature is not None:
        payload["temperature"] = temperature
    result = None
    ran = False
    last_error = None
    all_missing = True
    for candidate in candidates:
        try:
            result = await ai.run(candidate, to_js(payload))
            model = candidate
            ran = True
            break
        except Exception as error:
            last_error = error
            error_text = _safe_error_text(error).lower()
            if any(marker in error_text
                   for marker in AI_AGENT_CONTEXT_OVERFLOW_MARKERS):
                outcome["failure"] = "context_too_long"
                outcome["model"] = candidate
                return None
            if not _forkbot_ai_model_not_found_error(error):
                all_missing = False
    if not ran:
        outcome["failure"] = ("model_not_found" if all_missing
                              else "provider_error")
        await log_error(
            env, 500, "AI", "forkbot/ai-agent",
            "AI agent turn failed (%s%s): %s"
            % (model,
               ", no listed model exists on this account" if all_missing
               else "",
               _safe_error_text(last_error)[:300]))
        return None
    outcome["model"] = model
    try:
        if hasattr(result, "to_py"):
            result = result.to_py()
    except Exception:
        pass
    if not isinstance(result, (str, dict)):
        outcome["failure"] = "unusable_response"
        await log_error(
            env, 500, "AI", "forkbot/ai-agent",
            "AI agent turn returned an unusable %s response (%s)"
            % (type(result).__name__, model))
        return None
    return _ai_agent_normalized_result(result)
async def _ai_agent_rate_check(env, account_bi):
    """Throttle agent turns per signed account: at most
    AI_AGENT_MAX_PER_WINDOW turns per rolling AI_AGENT_RATE_WINDOW_MS.
    Reuses the ai_ask_rate table with a suffixed key so agent loops and the
    composer's one-shot asks meter independently; the UPSERT owns
    reset+increment+readback exactly like _ai_ask_rate_check."""
    if not account_bi:
        return None
    now = int(Date.now())
    row = await d1_first(
        env,
        "INSERT INTO ai_ask_rate (account_bi,count,window_start_ts) "
        "VALUES (?,1,?) "
        "ON CONFLICT(account_bi) DO UPDATE SET "
        "count=CASE WHEN ?-ai_ask_rate.window_start_ts>=? "
        "THEN 1 ELSE ai_ask_rate.count+1 END,"
        "window_start_ts=CASE WHEN ?-ai_ask_rate.window_start_ts>=? "
        "THEN ? ELSE ai_ask_rate.window_start_ts END "
        "RETURNING count,window_start_ts",
        account_bi + ":agent",
        now,
        now,
        AI_AGENT_RATE_WINDOW_MS,
        now,
        AI_AGENT_RATE_WINDOW_MS,
        now,
    )
    count = int((row or {}).get("count") or 1)
    window_start = int((row or {}).get("window_start_ts") or now)
    if count > AI_AGENT_MAX_PER_WINDOW:
        retry_ms = max(1000, AI_AGENT_RATE_WINDOW_MS - (now - window_start))
        return json_response(
            {"error": "rate_limited", "retryAfterMs": retry_ms},
            status=429,
            extra_headers={"Retry-After": str(max(1, (retry_ms + 999) // 1000))},
        )
    return None
async def ai_agent_handler(env, request):
    """One tool-use turn for the desktop's Cloudflare Workers AI coding agent
    (adhoc #1634).

    The desktop's AgentRunner materializes a bundled Python script into the
    session worktree's environment; that script drives the agent loop by
    POSTing the conversation and its bash tool declaration here each turn,
    executing the returned toolCalls locally, and looping until the model
    stops calling tools. The relay only ever runs the model — every tool
    executes on the desktop, in the session's own worktree and branch.

    Authorization is a per-run Ed25519 ticket: the desktop signs
    "forkmesh-ai-agent-v1\\n<account>\\n<ts>" once at launch and the script
    replays it for every turn of that run. The conversation grows every turn,
    so the signature cannot bind the payload the way /api/ai/ask's prompt
    digest does; the ticket is instead time-boxed
    (AI_AGENT_TICKET_MAX_AGE_MS) and every turn still passes the per-account
    agent rate window."""
    await ensure_schema(env)
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    account = clean_string(
        data.get("nodeName", ""), MAX_NODE_NAME).strip().lower()
    if not valid_node_name(account):
        return json_response({"error": "node_name_required"}, status=400)
    ts = clean_string(data.get("ts", ""), 20).strip()
    sig = clean_string(data.get("sig", ""), 200).strip()
    if not ts or not sig:
        return json_response({"error": "signature_required"}, status=401)
    try:
        age = int(Date.now()) - int(ts)
    except (TypeError, ValueError):
        return json_response({"error": "signature_required"}, status=401)
    # The ticket lives for a whole agent run: reject it only once it outlives
    # the run window, or when it claims to come from the future.
    if age > AI_AGENT_TICKET_MAX_AGE_MS or age < -LOGIN_MAX_SKEW_MS:
        return json_response({"error": "stale_signature"}, status=401)
    canonical = ("forkmesh-ai-agent-v1\n" + account + "\n" + ts).encode()
    if not await _verify_owner_signature(env, account, sig, canonical):
        return json_response({"error": "unauthorized"}, status=401)
    messages = _ai_agent_clean_messages(data.get("messages"))
    if not messages:
        return json_response({"error": "messages_required"}, status=400)
    tools = _ai_agent_clean_tools(data.get("tools"))
    model = clean_string(data.get("model", ""), 120).strip()
    try:
        max_tokens = int(data.get("max_tokens") or AI_AGENT_MAX_TOKENS)
    except (TypeError, ValueError):
        max_tokens = AI_AGENT_MAX_TOKENS
    max_tokens = max(256, min(AI_AGENT_MAX_TOKENS, max_tokens))
    # Tool calling is markedly more reliable at a low temperature than at the
    # Workers AI default, so the script's setting is honoured (clamped).
    temperature = data.get("temperature")
    try:
        temperature = (
            None if temperature is None or isinstance(temperature, bool)
            else float(temperature))
    except (TypeError, ValueError):
        temperature = None
    if temperature is not None:
        temperature = (None if temperature != temperature
                       else max(0.0, min(1.5, temperature)))
    limited = await _ai_agent_rate_check(env, await blind_index(env, account))
    if limited is not None:
        return limited
    outcome = {}
    result = await _forkbot_run_ai_agent(
        env, messages, tools=tools, model=model, max_tokens=max_tokens,
        outcome=outcome, temperature=temperature)
    if result is None:
        if outcome.get("failure") == "context_too_long":
            return json_response(
                {"error": "context_too_long",
                 "model": outcome.get("model") or model},
                status=413)
        # model_not_found is kept separate exactly as in ai_ask_handler: it is
        # the one failure the person at the composer can fix by re-picking.
        error = ("model_not_found"
                 if outcome.get("failure") == "model_not_found"
                 else "ai_unavailable")
        return json_response(
            {"error": error, "model": outcome.get("model") or model},
            status=502)
    return json_response({
        "ok": True,
        # The model that actually answered (a fallback when the pick was
        # rejected — see _forkbot_ai_fallback_models).
        "model": outcome.get("model") or model,
        "reply": result.get("reply", ""),
        "toolCalls": result.get("toolCalls", []),
        "usage": result.get("usage", {}),
    })
async def _forkbot_ai_issue_fields(env, description, context_text="", model=""):
    system_prompt = (
        "You turn a chat request into a ForkMesh issue. Use the conversation "
        "context to resolve what the user is referring to. Respond with ONLY "
        'one JSON object, no other text: {"title":"short summary",'
        '"body":"what is wrong or what needs doing, with detail from the '
        'conversation"}.'
    )
    user_prompt = clean_string(description, FORKBOT_MAX_COMMAND)
    if context_text:
        user_prompt = (
            "Recent conversation:\n" + context_text +
            "\n\nRequest: " + user_prompt)
    result = await _forkbot_run_ai(
        env, system_prompt, user_prompt, schema=FORKBOT_ISSUE_FIELDS_SCHEMA,
        model=model)
    if result is None:
        return None
    if isinstance(result, dict):
        return _forkbot_clean_ai_issue_fields(result)
    return _forkbot_clean_ai_issue_fields(_forkbot_json_object_from_text(result))
async def _forkbot_ai_interpret(env, command, context_text="", model=""):
    """Decide from meaning (not fixed phrasing) which ForkBot action the user
    wants — pulling the subject from the recent conversation when the mention
    itself is only a pointer ("forkbot log that").

    Returns one of:
      {"intent": "create_issue", "title", "body"}
      {"intent": "list_issues", "count"}
      {"intent": "count_issues"}
      {"intent": "search_issues", "query"}
      {"intent": "show_issue", "issueNumber"}   (0 = the most recent issue)
      {"intent": "start_agent", "issueNumber"}  (0 = the most recent issue)
      {"intent": "help"} / {"intent": "none"}
    or None when the model is unavailable/unparseable — callers treat None as
    "AI could not decide" and fall back to a heuristic, NOT as a refusal."""
    system_prompt = (
        "You are ForkBot, an assistant in a ForkMesh chat room. Classify what "
        "the user wants and respond with ONLY one JSON object, no other text. "
        "Actions: record a bug/task/feature as an issue -> "
        '{"intent":"create_issue","title":"short summary","body":"the problem '
        'or task, with detail from the conversation"}; list recent issues -> '
        '{"intent":"list_issues","count":N} (how many they asked for, 5 if '
        "unspecified); asking for a TOTAL count of issues rather than a list "
        '-> {"intent":"count_issues"}; search existing issues for a topic -> '
        '{"intent":"search_issues","query":"the search words"}; show one '
        'existing issue -> {"intent":"show_issue","issueNumber":N} (0 for the '
        "most recent); start a coding agent working on an issue -> "
        '{"intent":"start_agent","issueNumber":N} (0 for the most recent); '
        'asking what ForkBot can do -> {"intent":"help"}. When in doubt and '
        "the message describes a problem, task, or request, treat it as "
        "create_issue; a greeting or small talk is "
        '{"intent":"none"}. Examples: "issue to add dark mode" -> '
        '{"intent":"create_issue","title":"Add dark mode","body":"Add dark '
        'mode."}; "what came in this week?" -> '
        '{"intent":"list_issues","count":5}; "how many issues are there?" -> '
        '{"intent":"count_issues"}; "anything about relay retries?" '
        '-> {"intent":"search_issues","query":"relay retries"}; "get an '
        'agent going on that new issue" -> '
        '{"intent":"start_agent","issueNumber":0}; "hello!" -> '
        '{"intent":"none"}.'
    )
    user_prompt = command
    if context_text:
        user_prompt = (
            "Recent conversation:\n" + context_text +
            "\n\nMessage to ForkBot: " + command)
    result = await _forkbot_run_ai(
        env, system_prompt, user_prompt, schema=FORKBOT_INTENT_SCHEMA,
        model=model)
    if result is None:
        return None
    parsed = result if isinstance(result, dict) else \
        _forkbot_json_object_from_text(result)
    if not isinstance(parsed, dict):
        return None
    intent = clean_string(parsed.get("intent", ""), 40).strip().lower()
    if intent == "list_issues":
        try:
            count = int(parsed.get("count", FORKBOT_LIST_DEFAULT)
                        or FORKBOT_LIST_DEFAULT)
        except (TypeError, ValueError):
            count = FORKBOT_LIST_DEFAULT
        return {"intent": "list_issues",
                "count": max(1, min(count, FORKBOT_LIST_MAX))}
    if intent == "count_issues":
        return {"intent": "count_issues"}
    if intent == "search_issues":
        query = clean_string(parsed.get("query", ""), 100).strip()
        if not query:
            return None
        return {"intent": "search_issues", "query": query}
    if intent in ("show_issue", "start_agent"):
        try:
            number = int(parsed.get("issueNumber", 0) or 0)
        except (TypeError, ValueError):
            number = 0
        return {"intent": intent, "issueNumber": max(0, number)}
    if intent == "help":
        return {"intent": "help"}
    if intent not in ("create_issue", "none"):
        # Some models omit the field but still return title/body when they
        # decided to draft an issue; treat a usable draft as create_issue.
        intent = "create_issue" if (parsed.get("title") or parsed.get("body")) \
            else "none"
    if intent != "create_issue":
        return {"intent": "none"}
    fields = _forkbot_clean_ai_issue_fields(parsed)
    if not fields:
        # The model wanted an issue but produced no usable draft — let the
        # caller's heuristic take over instead of refusing.
        return None
    return {"intent": "create_issue", "title": fields["title"],
            "body": fields["body"]}
async def _forkbot_repo_gateway_json(env, owner, repo, action_query):
    """Read public repository data through the normal HTTPS mirror gateway."""
    owner = safe_segment(owner)
    repo = safe_segment(repo)
    raw_action = str(action_query or "")
    action_limit = (
        FORKBOT_GATEWAY_MAX_ACTION
        if raw_action.startswith("blobs?") else 400)
    action = clean_string(raw_action, action_limit).lstrip("/")
    if (
        not owner or not repo
        or not re.match(
            r"^(tree|blobs|blob|raw|history|commit|branches|search|stats|sizes)"
            r"(?:\?|$)",
            action,
        )
    ):
        return None
    try:
        origin = _public_base_url(env).rstrip("/")
        # Native AbortSignal timeout rather than asyncio.wait_for: cancelling
        # a JS-backed await leaves the Pyodide task pending forever and wedges
        # the isolate for every later request (see the no-concurrent-tasks
        # rule at the top of entry.py).
        response = await js_fetch_with_timeout(
            "%s/api/repo/%s/%s/%s"
            % (origin, quote(owner), quote(repo), action),
            {"method": "GET"},
            FORKBOT_GATEWAY_TIMEOUT_MS / 1000,
        )
        if int(getattr(response, "status", 0) or 0) != 200:
            return None
        data = await response.json()
        if hasattr(data, "to_py"):
            data = data.to_py()
        return data if isinstance(data, dict) else None
    except Exception:
        return None
def _forkbot_missing_tree(data):
    # The host answers "that folder isn't in git yet" in a few git-flavored
    # ways (same set the dashboard's isMissingMirrorFolder knows); all of them
    # mean "no issues filed", not "host unreachable".
    error = str((data or {}).get("error", "")).lower()
    return any(marker in error for marker in (
        "not_found", "not a valid object name", "pathspec",
        "does not exist", "unknown revision"))
def _forkbot_issue_json_path(number, subdir=""):
    # Issues are split by status into .forkmesh/issues/open/<N>/ and
    # .forkmesh/issues/closed/<N>/ (adhoc #14); pre-split mirrors keep the
    # numbered folder directly under the root (subdir "").
    base = ".forkmesh/issues"
    if subdir:
        base += "/" + subdir
    return "%s/%d/issue-%d.json" % (base, int(number), int(number))
def _forkbot_issue_json_candidates(number):
    """Every path issue <number>'s record may live at, most likely first."""
    return [_forkbot_issue_json_path(number, subdir)
            for subdir in ("open", "closed", "")]
def _forkbot_blob_text(blob):
    if not isinstance(blob, dict) or not blob.get("ok"):
        return ""
    content = blob.get("content", "")
    if not isinstance(content, str):
        return ""
    if blob.get("encoding") == "base64":
        try:
            return base64.b64decode(content).decode("utf-8", "replace")
        except Exception:
            return ""
    return content
def _forkbot_parse_issue_record(text, number):
    """Reduce one .forkmesh/issues/<N>/issue-<N>.json blob to the fields a chat
    line needs. Tolerant of junk: a malformed record still yields a usable
    "#N" stub rather than dropping the issue from the list."""
    try:
        parsed = json.loads(text or "{}")
    except Exception:
        parsed = None
    if not isinstance(parsed, dict):
        parsed = {}
    events = parsed.get("events")
    open_event = {}
    if isinstance(events, list):
        for event in events:
            if isinstance(event, dict) and event.get("type") == "open":
                open_event = event
                break
    title = clean_string(
        parsed.get("title") or open_event.get("title") or "", 240).strip()
    return {
        "number": int(parsed.get("number") or number or 0),
        "title": title or ("issue #%d" % int(number or 0)),
        "status": clean_string(
            parsed.get("status") or parsed.get("state") or "open", 40),
        "author": clean_string(
            parsed.get("authorName") or open_event.get("authorName") or
            parsed.get("author") or open_event.get("author") or "unknown",
            MAX_NODE_NAME),
        "body": clean_string(
            open_event.get("body") or parsed.get("body") or "", 600).strip(),
    }
async def _forkbot_recent_issue_numbers(env, owner, repo):
    """Issue numbers committed to the live mirror, newest first. Returns None
    when no host could serve the tree (offline), [] when the repo simply has
    no issues folder yet. Issues are split into open/ and closed/ status
    folders (adhoc #14); numbered folders directly under the root are the
    pre-split legacy layout."""
    root = await _forkbot_repo_gateway_json(
        env, owner, repo, "tree?path=" + quote(".forkmesh/issues", safe=""))
    if root is None:
        return None
    entries = root.get("entries")
    if not isinstance(entries, list):
        return [] if _forkbot_missing_tree(root) else None
    numbers = set()
    subdirs = []
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("type") != "tree":
            continue
        name = str(entry.get("name", ""))
        if name.isdigit():
            numbers.add(int(name))
        elif name in ("open", "closed"):
            subdirs.append(name)
    for subdir in subdirs:
        tree = await _forkbot_repo_gateway_json(
            env, owner, repo,
            "tree?path=" + quote(".forkmesh/issues/" + subdir, safe=""))
        sub_entries = tree.get("entries") if isinstance(tree, dict) else None
        if not isinstance(sub_entries, list):
            continue
        for entry in sub_entries:
            if not isinstance(entry, dict) or entry.get("type") != "tree":
                continue
            name = str(entry.get("name", ""))
            if name.isdigit():
                numbers.add(int(name))
    return sorted(numbers, reverse=True)
async def _forkbot_load_issue_records(env, owner, repo, numbers):
    """Batched read of the given issues' JSON records over the live tunnel
    (one /blobs round trip, like the website's issue list). Unreadable issues
    are skipped; returns records in the order requested."""
    numbers = [int(n) for n in numbers][:FORKBOT_LIST_MAX]
    if not numbers:
        return []
    # A record lives at open/<N>/, closed/<N>/, or the pre-split legacy <N>/
    # depending on its status; ask for every candidate in the one batch (3 ×
    # FORKBOT_LIST_MAX stays within MAX_BLOB_BATCH) and keep whichever answered.
    query = "&".join(
        "path=" + quote(path, safe="")
        for n in numbers for path in _forkbot_issue_json_candidates(n))
    data = await _forkbot_repo_gateway_json(env, owner, repo, "blobs?" + query)
    blobs = data.get("blobs") if isinstance(data, dict) else None
    if not isinstance(blobs, dict):
        return []
    records = []
    for number in numbers:
        text = ""
        for path in _forkbot_issue_json_candidates(number):
            text = _forkbot_blob_text(blobs.get(path))
            if text:
                break
        if not text:
            continue
        records.append(_forkbot_parse_issue_record(text, number))
    return records


async def _forkbot_all_issue_records(env, owner, repo):
    """Read every canonical issue JSON record without generating a snapshot.

    ``None`` means the live mirror could not provide a complete tracker; an
    empty list means the repository has no issue records. Partial data is not
    returned, because a public attribution fold must never silently undercount.
    """
    root_path = ".forkmesh/issues"
    root = await _forkbot_repo_gateway_json(
        env, owner, repo, "tree?path=" + quote(root_path, safe=""))
    entries = root.get("entries") if isinstance(root, dict) else None
    if not isinstance(entries, list):
        return [] if _forkbot_missing_tree(root) else None

    paths = {}
    status_dirs = set()
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("type") != "tree":
            continue
        name = str(entry.get("name") or "")
        if name in ("open", "closed"):
            status_dirs.add(name)
            continue
        if not name.isdigit():
            continue
        number = int(name)
        if not 1 <= number <= 1_000_000_000 or number in paths:
            return None
        paths[number] = _forkbot_issue_json_path(number)

    for status in ("open", "closed"):
        if status not in status_dirs:
            continue
        tree_path = root_path + "/" + status
        tree = await _forkbot_repo_gateway_json(
            env, owner, repo, "tree?path=" + quote(tree_path, safe=""))
        entries = tree.get("entries") if isinstance(tree, dict) else None
        if not isinstance(entries, list):
            return None
        for entry in entries:
            if not isinstance(entry, dict) or entry.get("type") != "tree":
                continue
            name = str(entry.get("name") or "")
            if not name.isdigit():
                continue
            number = int(name)
            if not 1 <= number <= 1_000_000_000 or number in paths:
                return None
            paths[number] = _forkbot_issue_json_path(number, status)

    records = []
    numbers = sorted(paths)
    for start in range(0, len(numbers), MAX_BLOB_BATCH):
        batch = numbers[start:start + MAX_BLOB_BATCH]
        query = "&".join(
            "path=" + quote(paths[number], safe="") for number in batch)
        data = await _forkbot_repo_gateway_json(
            env, owner, repo, "blobs?" + query)
        blobs = data.get("blobs") if isinstance(data, dict) else None
        if not isinstance(blobs, dict):
            return None
        for number in batch:
            text = _forkbot_blob_text(blobs.get(paths[number]))
            try:
                record = json.loads(text)
            except Exception:
                return None
            if not isinstance(record, dict):
                return None
            try:
                recorded_number = int(record.get("number") or 0)
            except (TypeError, ValueError):
                return None
            if recorded_number != number:
                return None
            records.append(record)
    return records


async def _forkbot_search_issues(env, owner, repo, query):
    """One host-side git grep over the mirror (the /search tunnel op, issue
    #360); ForkBot only reads the issues bucket. Returns the matches, or None
    when no host answered."""
    data = await _forkbot_repo_gateway_json(
        env, owner, repo, "search?q=" + quote(query, safe=""))
    if not isinstance(data, dict) or not data.get("ok"):
        return None
    issues = data.get("issues")
    if not isinstance(issues, list):
        return []
    matches = []
    for hit in issues:
        if not isinstance(hit, dict):
            continue
        try:
            number = int(hit.get("number", 0) or 0)
        except (TypeError, ValueError):
            continue
        if number <= 0:
            continue
        matches.append({
            "number": number,
            "title": clean_string(hit.get("title", ""), 240).strip()
            or ("issue #%d" % number),
            "snippet": clean_string(hit.get("snippet", ""), 200).strip(),
        })
    return matches
def _forkbot_issue_lines(records):
    lines = []
    for record in records:
        line = "#%d %s (%s" % (
            record["number"], record["title"], record["status"])
        if record.get("author") and record["author"] != "unknown":
            line += ", by " + record["author"]
        lines.append(line + ")")
    return "\n".join(lines)
async def _forkbot_next_issue_number(env, repo_bi, owner, repo, kind="issue"):
    """Atomically reserve the next issue or pull-request number.

    Desktop nodes keep this reserved number when its slot is free and assign
    the next free one on a genuine collision.  Issues seed from the catalog's
    published issueMaxNumber; pulls use pullCount until clients publish a
    dedicated maximum.  The pull counter shares issue_seq under a namespaced
    opaque key, so deployed databases need no second sequence table.

    One UPSERT both advances and returns the number.  The previous
    SELECT-then-UPDATE allocator could hand the same number to concurrent web
    submissions.  Best-effort: storage failures still return 0 so legacy bot
    callers can let a desktop assign the number."""
    kind = "pull" if kind == "pull" else "issue"
    counter_key = repo_bi if kind == "issue" else "pull:" + repo_bi
    catalog_keys = (
        ("issueMaxNumber", "issueCount")
        if kind == "issue" else ("pullCount",)
    )
    seed_next = 1
    try:
        row = await d1_first(
            env, "SELECT data FROM repositories WHERE key_bi=?", repo_bi)
        rec = await decrypt_row(env, row.get("data")) if row else None
        if rec:
            for key in catalog_keys:
                value = rec.get(key)
                if value in (None, ""):
                    continue
                try:
                    seed_next = max(seed_next, int(value) + 1)
                    break
                except (TypeError, ValueError):
                    continue
    except Exception:
        pass
    try:
        # Seed on first use / re-anchor upward to the catalog max, never
        # backward (MAX keeps already-handed-out numbers monotonic). The value
        # stored in next_number is always the next unreserved number.
        seq_row = await d1_first(
            env,
            "INSERT INTO issue_seq (repo_bi, next_number) VALUES (?, ?) "
            "ON CONFLICT(repo_bi) DO UPDATE SET "
            "next_number = MAX(issue_seq.next_number, ?) + 1 "
            "RETURNING next_number - 1 AS number",
            counter_key, seed_next + 1, seed_next,
        )
        return int((seq_row or {}).get("number", 0) or 0)
    except Exception:
        return 0
def _forkbot_attributed_body(body, source, actor, user_opened=False):
    """Append a footer explaining ForkBot's role in a chat-created issue.

    Authenticated users are already recorded as the opener, so their footer
    only notes the bot's assistance. Guest requests retain the fuller ForkBot
    attribution. The fediverse path builds its own attribution before calling
    in, so only the default chat source gets a footer here.
    """
    if source != "forkbot":
        return body
    if user_opened:
        footer = "ForkBot helped file this issue via chat."
    elif actor and actor != FORKBOT_AUTHOR:
        footer = "Filed by ForkBot at @%s's request via chat." % actor
    else:
        footer = "Filed by ForkBot via chat."
    body = (body or "").rstrip()
    if not body:
        return "_%s_" % footer
    return "%s\n\n---\n_%s_" % (body, footer)
async def _forkbot_rekey_alias_inbox(env, alias_owner, host_owner, repo,
                                     repo_bi):
    """Recover issue-inbox rows an earlier release filed under an organization
    alias's blind index. No drain ever looked there (see _forkbot_enqueue_issue),
    so those submissions are stranded; move them onto the backing node's key so
    the next sync delivers them. Idempotent (the alias key ends up empty) and
    best-effort — a hiccup here must never fail the new submission. The stored
    item carries no owner binding, so re-keying it is a pure routing fix."""
    if not host_owner or host_owner == alias_owner:
        return
    try:
        alias_bi = await blind_index(env, alias_owner + "/" + repo)
        if alias_bi and alias_bi != repo_bi:
            await d1_run(
                env, "UPDATE issue_inbox SET repo_bi=? WHERE repo_bi=?",
                repo_bi, alias_bi)
    except Exception:
        pass
async def _forkbot_enqueue_issue(env, owner, repo, title, body, requester,
                                 source="forkbot", labels=None,
                                 attachments=None,
                                 federated_mention_id="",
                                 federated_remote_url="",
                                 authenticated_requester="", reward=None):
    """Queue an issue for the owner's desktop node. Authenticated ForkBot chat
    requests open as the session user; guest requests retain the ForkBot
    opener. A manually reviewed fediverse item can reuse this path with its
    random review id so the owner node can later confirm the exact issue it
    materialized; inbound posts never call this directly."""
    await ensure_schema(env)
    # ForkBot names the repo by its PUBLIC url (forkmesh/forkmesh), which is an
    # organization alias. Every drain path — the node's per-repo GET and the
    # consolidated GET /api/sync — reads the inbox under the backing node's
    # blind index, because /api/repo/... is org_alias_rewrite'd before routing
    # and /api/sync selects the repositories rows the account actually owns.
    # Keying the insert on the alias therefore dead-letters the row: it sits in
    # issue_inbox forever and never reaches the repository. Resolve org->node
    # for the storage key and for who the owner-directed notifications are
    # DELIVERED to (a no-op for a plain node name); every string a human reads
    # — here, in those notifications, and in the caller's confirmation — stays
    # on the alias, so one submission never gets described as two repositories
    # (issue #650).
    host_owner = await _ap_org_alias_owner(env, owner, repo)
    repo_bi = await blind_index(env, host_owner + "/" + repo)
    await _forkbot_rekey_alias_inbox(env, owner, host_owner, repo, repo_bi)
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM issue_inbox WHERE repo_bi=?", repo_bi)
    if count and int(count.get("c", 0) or 0) >= MAX_PENDING_ISSUES:
        return False, "inbox_full"
    session_actor = clean_string(
        authenticated_requester, MAX_NODE_NAME).strip().lower()
    actor = session_actor or (
        clean_string(requester, MAX_NODE_NAME).lower() or FORKBOT_AUTHOR)
    # A linked Discord account is a verified ForkMesh identity, not merely a
    # display name. Make it the author of the transcribed report so issue
    # history, the reporter leaderboard, and the reward recipient agree.
    issue_author = session_actor or (
        actor if source == "discord" and actor != FORKBOT_AUTHOR
        else FORKBOT_AUTHOR)
    submitter_bi = await blind_index(env, issue_author)
    if await _inbox_author_over_quota(env, "issue_inbox", repo_bi, submitter_bi):
        return False, "author_quota"

    now = int(Date.now())
    body = _forkbot_attributed_body(
        body, source, actor, user_opened=bool(session_actor))
    reward_meta = {}
    if source == "discord" and isinstance(reward, dict):
        recipient = clean_string(
            reward.get("recipient", ""), MAX_NODE_NAME).strip().lower()
        reward_id = clean_string(reward.get("id", ""), 96).strip().lower()
        condition = clean_string(reward.get("condition", ""), 40).strip()
        status = clean_string(reward.get("status", ""), 40).strip()
        try:
            amount = int(reward.get("amountLamports") or 0)
        except (TypeError, ValueError):
            amount = 0
        # This is a narrow trusted path: only the Discord watcher can attach
        # it, it must pay the resolved identity, and it stays within the
        # published 0.001--0.005 SOL issue-report range.
        if (recipient == actor and re.fullmatch(r"reward-[a-z0-9-]{8,88}", reward_id)
                and 1_000_000 <= amount <= 5_000_000
                and condition == "accepted_report"
                and status == "pending_wallet"):
            reward_meta = {
                "rewardEstimateLamports": amount,
                "rewardRecipient": recipient,
                "rewardId": reward_id,
                "rewardCondition": condition,
                "rewardStatus": status,
            }
    # Proposed number the desktop honors when the slot is free (0 = let the
    # desktop assign). Allocated before the insert so it lands in the stored
    # item and can be echoed straight back to the chat.
    number = await _forkbot_next_issue_number(env, repo_bi, owner, repo)
    tracked_mention_id = clean_string(federated_mention_id, 32).lower()
    if not re.fullmatch(r"[a-f0-9]{32}", tracked_mention_id):
        tracked_mention_id = ""
    event = {
        "type": "open",
        "id": (
            "fediverse-review-" + tracked_mention_id
            if tracked_mention_id else "forkbot-%d" % now
        ),
        # The session proves the logical user attribution. The empty signature,
        # source, label, and body note preserve that ForkBot relayed the event
        # rather than falsely claiming the user signed the bot-authored draft.
        "author": issue_author,
        "authorName": issue_author,
        "ts": now,
        "title": clean_string(title, 240),
        "body": clean_string(body, MAX_ISSUE_BYTES),
        "attachments": [entry["name"] for entry in (attachments or [])],
        "sig": "",
    }
    item = {
        "number": number,
        "titleIfNew": event["title"],
        "event": event,
        "meta": {
            "labels": list(labels) if labels else ["forkbot"],
            "milestone": "",
            "priority": 0,
            "assignees": [],
            # Display credit also becomes the issue-level author name when the
            # owner node materializes the relay event.
            "reporter": actor if actor != FORKBOT_AUTHOR else "",
            "wantsAgent": False,
            "model": "",
            "provider": "",
            **reward_meta,
        },
        "submitter": issue_author,
        "submittedAt": now,
        "source": source,
        "requestedBy": actor,
        "issueNumber": number,
    }
    if tracked_mention_id:
        item["fediverseMentionId"] = tracked_mention_id
        item["fediverseRemoteUrl"] = clean_string(
            federated_remote_url, 1600)
    if attachments:
        item["attachmentData"] = [
            {"name": entry["name"], "data": entry["data"]}
            for entry in attachments]
    await d1_run(
        env,
        "INSERT INTO issue_inbox (repo_bi, data, submitter_bi) VALUES (?,?,?)",
        repo_bi, await encrypt_row(env, item), submitter_bi,
    )
    await _record_contributor(env, actor, "issues")
    await _best_effort_inbox_side_effect(
        notify_pending_inbox(
            env, host_owner, repo, "issue", issue_author,
            item.get("titleIfNew", ""), number, public_owner=owner,
            opened=True))
    # A mention ping is pure display for whoever was named — it is not routed
    # by this owner — so it reads the public repo, matching the href it has
    # always carried.
    await _best_effort_inbox_side_effect(
        notify_mentions(
            env, owner, repo, issue_author,
            item.get("titleIfNew", ""),
            event.get("body", ""), repo_web_href(owner, repo), "issue",
            number=number))
    # Same push every other inbox write does: the owner's node syncs on the
    # event frame instead of waiting out the 5-15 minute fallback poll.
    await notify_repo_host(env, host_owner, repo, "issues")
    return True, item
async def _forkbot_enqueue_agent_request(env, owner, repo, number, requester):
    """Queue a "start a coding agent on issue #number" request through the
    same signed-inbox channel as everything else: a ForkBot comment event on
    the existing issue whose meta carries wantsAgent. The owner's desktop node
    starts the agent when it merges the inbox — the relay never runs anything
    itself. Caller has already verified the requester is the repo owner/admin
    (the same privilege gate issues_handler applies to wantsAgent)."""
    await ensure_schema(env)
    # Key the row the way every drain reads it — see _forkbot_enqueue_issue.
    host_owner = await _ap_org_alias_owner(env, owner, repo)
    repo_bi = await blind_index(env, host_owner + "/" + repo)
    await _forkbot_rekey_alias_inbox(env, owner, host_owner, repo, repo_bi)
    count = await d1_first(
        env, "SELECT COUNT(*) AS c FROM issue_inbox WHERE repo_bi=?", repo_bi)
    if count and int(count.get("c", 0) or 0) >= MAX_PENDING_ISSUES:
        return False, "inbox_full"
    submitter_bi = await blind_index(env, FORKBOT_AUTHOR)
    if await _inbox_author_over_quota(env, "issue_inbox", repo_bi, submitter_bi):
        return False, "author_quota"

    now = int(Date.now())
    actor = clean_string(requester, MAX_NODE_NAME).lower() or FORKBOT_AUTHOR
    event = {
        "type": "comment",
        "id": "forkbot-agent-%d" % now,
        "author": FORKBOT_AUTHOR,
        "authorName": FORKBOT_NAME,
        "ts": now,
        "body": "@%s asked ForkBot to start a coding agent on this issue."
                % actor,
        "attachments": [],
        "sig": "",
    }
    item = {
        "number": int(number),
        "titleIfNew": "",
        "event": event,
        "meta": {
            "labels": [],
            "milestone": "",
            "priority": 0,
            "assignees": [],
            "wantsAgent": True,
            "model": "",
            "provider": "",
        },
        "submitter": FORKBOT_AUTHOR,
        "submittedAt": now,
        "source": "forkbot",
        "requestedBy": actor,
    }
    await d1_run(
        env,
        "INSERT INTO issue_inbox (repo_bi, data, submitter_bi) VALUES (?,?,?)",
        repo_bi, await encrypt_row(env, item), submitter_bi,
    )
    await _best_effort_inbox_side_effect(
        notify_pending_inbox(
            env, host_owner, repo, "issue", FORKBOT_AUTHOR,
            "agent request for issue #%d" % int(number), int(number),
            public_owner=owner))
    await notify_repo_host(env, host_owner, repo, "issues")
    return True, item
def _forkbot_gateway_offline_reply(owner, repo):
    return json_response({
        "ok": True,
        "action": "issues_unavailable",
        "botMessage": (
            "I couldn't reach a live host for %s/%s — issues are served from "
            "the owner's desktop node. Try again once it's back online."
        ) % (owner, repo),
    })
async def _forkbot_action_count(env, owner, repo):
    # A "how many" question wants a total, not a capped listing — reusing
    # _forkbot_action_list here would silently truncate at FORKBOT_LIST_MAX
    # and never actually answer the question asked.
    numbers = await _forkbot_recent_issue_numbers(env, owner, repo)
    if numbers is None:
        return _forkbot_gateway_offline_reply(owner, repo)
    total = len(numbers)
    if not total:
        message = "No issues have been filed in %s/%s yet." % (owner, repo)
    else:
        message = "There %s %d issue%s in %s/%s." % (
            "is" if total == 1 else "are", total,
            "" if total == 1 else "s", owner, repo)
    return json_response({
        "ok": True, "action": "issues_counted", "count": total,
        "botMessage": message,
    })
async def _forkbot_action_list(env, owner, repo, count):
    numbers = await _forkbot_recent_issue_numbers(env, owner, repo)
    if numbers is None:
        return _forkbot_gateway_offline_reply(owner, repo)
    issue_url = repo_web_href(owner, repo) + "/issues"
    if not numbers:
        return json_response({
            "ok": True, "action": "issues_listed", "issues": [],
            "botMessage": "No issues have been filed in %s/%s yet."
                          % (owner, repo),
        })
    records = await _forkbot_load_issue_records(env, owner, repo,
                                                numbers[:count])
    if not records:
        return _forkbot_gateway_offline_reply(owner, repo)
    lines = _forkbot_issue_lines(records)
    header = "Last %d issue%s in %s/%s:" % (
        len(records), "" if len(records) == 1 else "s", owner, repo)
    footer = "Full list: " + issue_url
    if len(numbers) > len(records):
        footer = ("Say \"list the last N issues\" for more (up to %d). "
                  % FORKBOT_LIST_MAX) + footer
    return json_response({
        "ok": True,
        "action": "issues_listed",
        "owner": owner,
        "repo": repo,
        "issues": records,
        "botMessage": header + "\n" + lines + "\n" + footer,
    })
async def _forkbot_action_search(env, owner, repo, query):
    matches = await _forkbot_search_issues(env, owner, repo, query)
    if matches is None:
        return _forkbot_gateway_offline_reply(owner, repo)
    if not matches:
        return json_response({
            "ok": True, "action": "issues_searched", "query": query,
            "issues": [],
            "botMessage": "No issues matching \"%s\" in %s/%s."
                          % (query, owner, repo),
        })
    top = matches[:FORKBOT_SEARCH_MAX]
    lines = []
    for hit in top:
        line = "#%d %s" % (hit["number"], hit["title"])
        if hit.get("snippet"):
            line += " — " + hit["snippet"]
        lines.append(line)
    header = "Issues matching \"%s\" in %s/%s:" % (query, owner, repo)
    if len(matches) > len(top):
        header = "Top %d of %d issues matching \"%s\" in %s/%s:" % (
            len(top), len(matches), query, owner, repo)
    return json_response({
        "ok": True,
        "action": "issues_searched",
        "owner": owner,
        "repo": repo,
        "query": query,
        "issues": top,
        "botMessage": header + "\n" + "\n".join(lines),
    })
async def _forkbot_action_show(env, owner, repo, number):
    if number <= 0:
        numbers = await _forkbot_recent_issue_numbers(env, owner, repo)
        if numbers is None:
            return _forkbot_gateway_offline_reply(owner, repo)
        if not numbers:
            return json_response({
                "ok": True, "action": "issue_shown", "issue": None,
                "botMessage": "No issues have been filed in %s/%s yet."
                              % (owner, repo),
            })
        number = numbers[0]
    records = await _forkbot_load_issue_records(env, owner, repo, [number])
    if not records:
        return json_response({
            "ok": True, "action": "issue_shown", "issue": None,
            "botMessage": (
                "I couldn't read issue #%d from a live %s/%s host — it may "
                "not exist, or the owner's node is offline."
            ) % (number, owner, repo),
        })
    record = records[0]
    bot_message = "#%d %s (%s, opened by %s)." % (
        record["number"], record["title"], record["status"], record["author"])
    if record.get("body"):
        bot_message += " " + record["body"]
    return json_response({
        "ok": True,
        "action": "issue_shown",
        "owner": owner,
        "repo": repo,
        "issue": record,
        "botMessage": bot_message,
    })
async def _forkbot_action_start_agent(env, owner, repo, number, sender):
    # Starting a coding agent on the owner's machine is an immediate,
    # unreviewed side effect, so it keeps the same privilege gate the web
    # issue form's wantsAgent has (adhoc #225): the repo owner's account or a
    # network admin.
    requester = clean_string(sender, MAX_NODE_NAME).strip().lower()
    # When the repo is fronted by an organization alias, the account that owns
    # it (and whose node would run the agent) is the backing node, not the org
    # name in the URL — check against both so the real owner isn't refused.
    host_owner = (await _ap_org_alias_owner(env, owner, repo)).lower()
    authorized = bool(requester) and (
        requester == owner.lower() or requester == host_owner
        or await _is_admin(env, requester))
    if not authorized:
        return json_response({
            "ok": True,
            "action": "agent_denied",
            "botMessage": (
                "Starting a coding agent is limited to the repo owner (or a "
                "network admin) — ask %s to kick it off."
            ) % owner,
        })
    numbers = await _forkbot_recent_issue_numbers(env, owner, repo)
    if numbers is None:
        return _forkbot_gateway_offline_reply(owner, repo)
    if not numbers:
        return json_response({
            "ok": True, "action": "agent_denied",
            "botMessage": "There are no issues in %s/%s to start an agent on."
                          % (owner, repo),
        })
    if number <= 0:
        number = numbers[0]
    elif number not in numbers:
        return json_response({
            "ok": True, "action": "agent_denied",
            "botMessage": "Issue #%d isn't in the %s/%s mirror."
                          % (number, owner, repo),
        })
    records = await _forkbot_load_issue_records(env, owner, repo, [number])
    title = records[0]["title"] if records else ""
    ok, result = await _forkbot_enqueue_agent_request(
        env, owner, repo, number, requester)
    if not ok:
        return json_response({"error": result}, status=429)
    bot_message = (
        "Queued a coding agent for issue #%d%s in %s/%s. The owner's node "
        "will start it when it next syncs the inbox."
    ) % (number, (": " + title) if title else "", owner, repo)
    return json_response({
        "ok": True,
        "action": "agent_requested",
        "owner": owner,
        "repo": repo,
        "issueNumber": number,
        "title": title,
        "botMessage": bot_message,
    }, status=201)
async def forkbot_chat_handler(env, request):
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)

    message = clean_string(data.get("message", ""), FORKBOT_MAX_COMMAND)
    command = _forkbot_extract_mention_command(message)
    if not command:
        return json_response({"ok": True, "ignored": True})

    # Recent (client-decrypted) conversation the mention sits in, so ForkBot can
    # resolve "that bug" / "the issue we discussed" instead of only the one line.
    context_text = _forkbot_context_text(data.get("context"))
    sender = clean_string(data.get("sender", ""), MAX_NODE_NAME)
    # Optional Cloudflare Workers AI model pick from the chat composer's model
    # picker. Only an allowlisted id is honored; anything else (including a
    # stale picker value) resolves back to the deployment default so ForkBot
    # keeps working rather than erroring on the caller's behalf.
    ai_model = _forkbot_resolve_ai_model(env, data.get("model", ""))
    owner = FORKBOT_DEFAULT_OWNER
    repo = FORKBOT_DEFAULT_REPO

    # Decide the action. Prefer meaning over fixed wording:
    #  1. Cheap regexes catch explicitly phrased commands offline (list /
    #     search / start-agent / show / help / create), sparing an AI round
    #     trip and keeping every command working when AI is off.
    #  2. Otherwise the AI classifier reads the command + conversation and
    #     picks the intent (drafting the issue when one is wanted).
    #  3. AI unreachable (None — distinct from a confident "none") but the
    #     message plainly talks about issues/bugs/tracking: file the raw text
    #     rather than refusing. Only a confident "none" or a message with no
    #     work-recording signal at all gets the help hint.
    action = None
    for intent_name, parser in (
            ("count_issues", _forkbot_parse_count_command),
            ("list_issues", _forkbot_parse_list_command),
            ("search_issues", _forkbot_parse_search_command),
            ("start_agent", _forkbot_parse_agent_command),
            ("show_issue", _forkbot_parse_show_command)):
        parsed_intent = parser(command)
        if parsed_intent is not None:
            action = {"intent": intent_name, **parsed_intent}
            break
    if action is None and _forkbot_parse_help_command(command):
        action = {"intent": "help"}

    fields = None
    if action is None:
        parsed = _forkbot_parse_issue_command(command)
        if parsed:
            action = {"intent": "create_issue"}
            fields = await _forkbot_ai_issue_fields(
                env, parsed["description"], context_text, model=ai_model)
            if not fields:
                fields = _forkbot_fallback_issue_fields(parsed["description"])
        else:
            interpreted = await _forkbot_ai_interpret(
                env, command, context_text, model=ai_model)
            if interpreted and interpreted.get("intent") == "create_issue":
                action = {"intent": "create_issue"}
                fields = {"title": interpreted["title"],
                          "body": interpreted["body"]}
            elif interpreted and interpreted.get("intent") != "none":
                action = interpreted
            elif interpreted is None and _forkbot_command_hints_issue(command):
                action = {"intent": "create_issue"}
                fields = _forkbot_fallback_issue_fields(command)

    intent = (action or {}).get("intent", "")
    if intent == "count_issues":
        return await _forkbot_action_count(env, owner, repo)
    if intent == "list_issues":
        return await _forkbot_action_list(env, owner, repo, action["count"])
    if intent == "search_issues":
        return await _forkbot_action_search(env, owner, repo, action["query"])
    if intent == "show_issue":
        return await _forkbot_action_show(
            env, owner, repo, int(action.get("issueNumber", 0) or 0))
    if intent == "start_agent":
        _, agent_session = await _account_session_record(env, request, data)
        return await _forkbot_action_start_agent(
            env, owner, repo, int(action.get("issueNumber", 0) or 0),
            (agent_session or {}).get("name", ""))

    if intent != "create_issue" or not fields or not fields.get("body"):
        return json_response({
            "ok": True,
            "action": "help",
            "model": ai_model,
            "botMessage": _forkbot_help_message(),
        })

    # A chat display name is client-authored and can be spoofed. Only a valid,
    # revocable account session may make the resulting issue display that user
    # as its opener; guests continue to file through ForkBot.
    _, session_record = await _account_session_record(env, request, data)
    session_sender = clean_string(
        (session_record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    ok, result = await _forkbot_enqueue_issue(
        env, owner, repo, fields["title"], fields["body"], sender,
        authenticated_requester=session_sender)
    if not ok:
        return json_response({"error": result}, status=429)
    title = result.get("titleIfNew", fields["title"])
    number = int(result.get("issueNumber", 0) or 0)
    # /owner/repo/issues is the web route for the repo's issue list; the number
    # is the proposed one the desktop honors when it merges the inbox.
    issue_url = repo_web_href(owner, repo) + "/issues"
    if number > 0:
        bot_message = (
            "Opened issue #%d in %s/%s: %s. It'll show at %s once the owner "
            "node syncs the inbox."
        ) % (number, owner, repo, title, issue_url)
    else:
        bot_message = (
            "Opened an issue in %s/%s: %s. It'll show at %s once the owner "
            "node syncs the inbox."
        ) % (owner, repo, title, issue_url)
    return json_response({
        "ok": True,
        "action": "issue_created",
        "owner": owner,
        "repo": repo,
        "title": title,
        "issueNumber": number,
        "issueUrl": issue_url,
        "model": ai_model,
        "botMessage": bot_message,
    }, status=201)
