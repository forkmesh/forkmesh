"""Voice Changer: voices designed by AI for the desktop package.

The voice itself is changed on the desktop (content/desktop/VoiceEngine.cpp):
no audio ever reaches this Worker. The package is free; the desktop page signs
its requests with REQUEST_PROOF through the generic native-package host,
because a desktop usually has no website session.

* ``GET /api/voice-changer``: is the caller signed in, and is AI voice design
  available to them?
* ``POST /api/voice-changer/design``: turn a description ("a calm, deep
  late-night radio voice") into engine parameters with the host's bounded AI
  capability (``runtime.ai_json``). Every number the model returns is clamped
  to the engine's own bounds before it is sent back.

There is no server-side data: voices a person saves live on their desktop.
"""

import re


PREFIX = "/api/voice-changer"
PLUGIN_ID = "voice-changer"
REQUEST_PROOF = "forkmesh-voice-changer-request-v1"
MAX_BODY_BYTES = 8 * 1024
MAX_PROMPT = 400
NAME_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")

# Engine parameter bounds: (default, low, high). Must match paramBounds() in
# content/desktop/VoiceEngine.cpp (tests/test_voice_changer.py holds them
# together).
PARAMS = {
    "targetPitchHz": (0, 0, 400),
    "semitones": (0, -12, 12),
    "formant": (1, 0.7, 1.45),
    "breath": (0, 0, 1),
    "whisper": (0, 0, 1),
    "brightness": (0, -1, 1),
    "roughness": (0, 0, 1),
    "gainDb": (0, -12, 12),
    "mix": (1, 0, 1),
}

DESIGN_SYSTEM_PROMPT = (
    "You design settings for a real-time voice changer. The engine moves the "
    "speaker's pitch to targetPitchHz (typical adult male speech 85-155 Hz, "
    "adult female 165-255 Hz, children 250-400 Hz; 0 keeps the speaker's own "
    "pitch and applies only semitones), scales the vocal-tract formants by "
    "formant (0.7 = a very large person, 1.0 = unchanged, 1.45 = a small "
    "child or cartoon), adds breath noise (0-1), whisper (0-1, replaces the "
    "voiced tone), brightness as spectral tilt (-1 dark to 1 bright), "
    "roughness as gravel or age (0-1), and gainDb (-12 to 12). Keep mix at 1. "
    "Pitch and formant must move together for a believable different person: "
    "a lower voice has formant below 1, a higher voice above 1. Never try to "
    "reproduce a specific real, named person's voice. If the description names "
    "one, design a generic voice with only broad traits (for example deep, "
    "calm, raspy) and say so in the note. Reply with JSON only."
)

DESIGN_SCHEMA = {
    "type": "object",
    "properties": {
        "name": {"type": "string"},
        "note": {"type": "string"},
        "params": {
            "type": "object",
            "properties": {name: {"type": "number"} for name in PARAMS},
        },
    },
    "required": ["name", "params"],
}


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _error(runtime, code, status=400):
    return _response(runtime, {"error": code}, status)


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def clean_params(value):
    """Clamp a voice to the engine's bounds; unknown keys are dropped."""
    source = value if isinstance(value, dict) else {}
    result = {}
    for name, (default, low, high) in PARAMS.items():
        raw = source.get(name)
        if isinstance(raw, bool) or not isinstance(raw, (int, float)) or raw != raw:
            number = default
        else:
            number = min(max(float(raw), low), high)
        result[name] = round(number, 3)
    if 0 < result["targetPitchHz"] < 60:
        result["targetPitchHz"] = 60
    return result


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _status(runtime, actor):
    return _response(runtime, {
        "ok": True,
        "actor": actor,
        "plugin": PLUGIN_ID,
        "ai": getattr(runtime, "ai_json", None) is not None,
    })


async def _design(runtime, account_bi, data):
    prompt = _clean(data.get("prompt"), MAX_PROMPT)
    if len(prompt) < 3:
        return _error(runtime, "prompt_required")
    ai_json = getattr(runtime, "ai_json", None)
    if ai_json is None:
        return _error(runtime, "ai_unavailable", 503)
    result = await ai_json(
        account_bi, DESIGN_SYSTEM_PROMPT,
        "Design a voice for this description: <description>" + prompt + "</description>",
        DESIGN_SCHEMA, max_tokens=400,
    )
    if not isinstance(result, dict) or not result.get("ok"):
        error = str((result or {}).get("error") or "ai_unavailable")
        payload = {"error": error}
        if result and result.get("retryAfterMs"):
            payload["retryAfterMs"] = int(result.get("retryAfterMs") or 0)
        return _response(runtime, payload, 429 if error == "rate_limited" else 502)
    parsed = result.get("data") if isinstance(result.get("data"), dict) else {}
    params = clean_params(parsed.get("params"))
    params["mix"] = 1
    name = _clean(parsed.get("name"), 40) or _clean(prompt, 40)
    return _response(runtime, {
        "ok": True,
        "voice": {"name": name, "params": params},
        "note": _clean(parsed.get("note"), 240),
    })


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    return parts if len(parts) == 1 and len(parts[0]) <= 32 else None


async def handle(runtime, path):
    parts = _route(path)
    if parts is None:
        return _error(runtime, "not_found", 404)
    allowed = {(): ("GET",), ("design",): ("POST",)}.get(parts)
    if not allowed:
        return _error(runtime, "not_found", 404)
    method = runtime.method()
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    data = {}
    if method != "GET":
        if not runtime.same_origin():
            return _error(runtime, "origin_not_allowed", 403)
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _error(runtime, error, 413 if error == "payload_too_large" else 400)
        if not isinstance(data, dict):
            return _error(runtime, "invalid_json")
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor or not NAME_RE.fullmatch(actor):
        return _error(runtime, "invalid_session", 401)
    if not parts:
        return await _status(runtime, actor)
    return await _design(runtime, account_bi, data)
