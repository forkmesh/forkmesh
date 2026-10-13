# Package AI failures (Calorie Counter)

Package Workers reach Workers AI only through the host capability
`runtime.ai_json` (`_PluginApiRuntime.ai_json` → `_plugin_ai_json` in
`app/src/entry.py`). The only package that calls it today is Calorie Counter
(`marketplace/packages/calories/content/worker/calories.py`), on
`POST /api/calories/analyze` (meal photo or description) and
`POST /api/calories/receipt` (receipt photo).

## Why `/api/calories/analyze` answered 502

`error_log` had `response status 502` for `/api/calories/analyze` at 01:43 on
2026-09-27. Nothing had crashed. The served stack runs the App under the
CPython host, which creates `env.AI` only when `AI_API_URL` is set, and
production leaves it unset ([docs/host/README.md](../host/README.md)). So
`_plugin_ai_json` returned `{"ok": false, "error": "missing_binding"}`, and the
worker turned every AI failure that was not a rate limit into a 502. The App's
generic 5xx logger (`app_runtime.py`) then recorded it as an anonymous
`response status 502` row and Sentry event. That broke the host rule that AI
features without a provider skip quietly and do not write to `error_log`.

## What the worker answers now

`_ai_failure` in `calories.py` maps every `ai_json` failure to one of these
answers, on both routes:

| `ai_json` error | Status | Body | Headers |
| --- | --- | --- | --- |
| `missing_binding` (or the runtime has no `ai_json`) | 503 | `{"error": "ai_unavailable", "reason": "missing_binding", "retryable": false}` | `x-forkmesh-expected-degraded: 1` |
| `provider_error`, `unusable_response`, anything else or no result | 503 | `{"error": "ai_unavailable", "reason": <error>, "retryable": true, "retryAfterMs": 30000}` | `retry-after: 30`, `x-forkmesh-expected-degraded: 1` |
| `rate_limited` | 429 | `{"error": "rate_limited", "retryable": true, "retryAfterMs": <quota>}` | `retry-after` in whole seconds, rounded up (only when the quota gave a wait) |
| `invalid_image` | 400 | `{"error": "invalid_image"}` | |

- **Why 503, not 502:** the App is not relaying a broken upstream answer. The
  capability is unavailable, either for good on this host or for a moment.
- **Why `error` stays `ai_unavailable`:** web (`estimate()` keys on status
  503), desktop (`aiError` in `CaloriesFeature.cpp`), Android
  (`CaloriesPrompt.kt`) and iOS already map that code to "AI estimates are
  unavailable on this host", so no client had to change.
- **What `reason` and `retryable` add:** a client can tell a host with no
  provider (do not retry) from a provider hiccup (retry after
  `retryAfterMs`).
- **Why the degraded marker:** the host already logs provider failures
  itself (`log_error(..., "plugin/ai", ...)` in `_plugin_ai_json`), and a
  missing binding is an expected state. `x-forkmesh-expected-degraded` is the
  App's existing marker (`EXPECTED_DEGRADED_HEADER` in `entry.py`), and the
  generic 5xx logger skips responses that carry it. A 429 is not a 5xx, so it
  does not carry the marker.

Food search (`GET /api/calories/foods?q=`) still answers 502
`food_search_unavailable` when every Open Food Facts and USDA request fails.
That upstream is a third-party service, and nothing else logs its outages.

## Enabling AI on the served stack

Set `AI_API_URL` (plus `AI_API_KEY`, and `AI_MODEL_MAP` / `AI_DEFAULT_MODEL`)
on the core nodes. For photos and receipts, the mapped model must accept
OpenAI-style `image_url` message parts. Workers AI vision ids
(`PLUGIN_AI_VISION_MODELS`) fall back to `AI_DEFAULT_MODEL` when they are not
in the map.

## Tests

- `app/tests/test_calories_ai_upstream.py` drives the real worker through the
  package's test runtime. It checks every row of the table above and that no
  AI failure path answers 502. It also checks that the marker header matches
  `entry.EXPECTED_DEGRADED_HEADER`.
- `marketplace/packages/calories/tests/test_calories.py` still covers the
  success path.
