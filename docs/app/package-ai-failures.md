# Package AI failures (Calorie Counter)

Package server code reaches AI only through the host capability
`runtime.ai_json` (`_PluginApiRuntime.ai_json` → `_plugin_ai_json` in
`app/src/entry.py`). Calorie Counter
(`marketplace/packages/calories/content/worker/calories.py`) calls it on
`POST /api/calories/analyze` and `POST /api/calories/receipt`.

The host creates `env.AI` only when `AI_API_URL` is set
([docs/host/README.md](../host/README.md)). Without it, `_plugin_ai_json`
returns `{"ok": false, "error": "missing_binding"}`. AI features without a
provider must degrade quietly and not write to `error_log`.

## Responses

`_ai_failure` in `calories.py` maps every `ai_json` failure:

| `ai_json` error | Status | Body | Headers |
| --- | --- | --- | --- |
| `missing_binding` (or no `ai_json`) | 503 | `{"error": "ai_unavailable", "reason": "missing_binding", "retryable": false}` | `x-forkmesh-expected-degraded: 1` |
| `provider_error`, `unusable_response`, other | 503 | `{"error": "ai_unavailable", "reason": <error>, "retryable": true, "retryAfterMs": 30000}` | `retry-after: 30`, `x-forkmesh-expected-degraded: 1` |
| `rate_limited` | 429 | `{"error": "rate_limited", "retryable": true, "retryAfterMs": <quota>}` | `retry-after` in whole seconds |
| `invalid_image` | 400 | `{"error": "invalid_image"}` | |

- 503, not 502: the capability is unavailable; no upstream answer is being
  relayed.
- `ai_unavailable` is the code every client already maps to "AI estimates are
  unavailable on this host"; `reason` and `retryable` let a client decide
  whether to retry.
- `x-forkmesh-expected-degraded` (`EXPECTED_DEGRADED_HEADER` in `entry.py`)
  makes the generic 5xx logger skip the response; the host logs provider
  failures itself.

Food search (`GET /api/calories/foods?q=`) answers 502
`food_search_unavailable` when every third-party food database fails.

## Enabling AI

Set `AI_API_URL` (plus `AI_API_KEY`, `AI_MODEL_MAP` / `AI_DEFAULT_MODEL`) on
the core nodes. For photos, the mapped model must accept OpenAI-style
`image_url` parts; vision ids not in the map fall back to `AI_DEFAULT_MODEL`.

## Tests

`app/tests/test_calories_ai_upstream.py` (every row above, no 502 on AI paths,
header name matches `entry.EXPECTED_DEGRADED_HEADER`);
`marketplace/packages/calories/tests/test_calories.py` (success path).
