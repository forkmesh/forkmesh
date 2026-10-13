"""World API exponential-backoff contracts (adhoc #1532).

Every World HTTP caller shares one exponential-backoff gate
(public/world/world-backoff.js): a failing endpoint is asked again after
5s, 10s, 20s ... capped at five minutes, Retry-After/429/503 answers are
obeyed, and while an endpoint cools down further attempts are refused
locally instead of hitting the origin. These are source-text pins in the
same style as the rest of the world frontend suite.
"""

from pathlib import Path

WORLD_DIR = Path(__file__).resolve().parents[2] / "world" / "public" / "world"
BACKOFF = (WORLD_DIR / "world-backoff.js").read_text(encoding="utf-8")
WORLD = (WORLD_DIR / "world.js").read_text(encoding="utf-8")


def test_backoff_grows_exponentially_and_obeys_server_hints():
    assert "export const WORLD_BACKOFF_BASE_MS = 5_000;" in BACKOFF
    assert "export const WORLD_BACKOFF_MAX_MS = 5 * 60 * 1_000;" in BACKOFF
    assert "WORLD_BACKOFF_BASE_MS * 2 ** (tries - 1)" in BACKOFF
    # Server hints are floors, not suggestions: an explicit Retry-After and
    # the 429/503 statuses all raise the wait above the exponential step.
    assert 'String(response?.headers?.get?.("retry-after") || "")' in BACKOFF
    assert "status === 429 ? WORLD_BACKOFF_RATE_LIMIT_MS : 0" in BACKOFF
    assert "status === 503 ? WORLD_BACKOFF_UNAVAILABLE_MS : 0" in BACKOFF


def test_backoff_refuses_locally_while_cooling_down():
    # The gate never retries by itself and refuses attempts during the
    # cooldown window, so a degraded Worker is not re-asked at full rate.
    assert "const waiting = gate.waitMs(key);" in BACKOFF
    assert "if (waiting > 0) throw worldCoolingDownError(key, waiting);" in (
        BACKOFF
    )
    assert "error.coolingDown = true;" in BACKOFF
    # Only outage-shaped failures back off by default: a 4xx other than
    # 408/425/429 is an answer about the request, not the origin's health.
    assert "new Set([408, 425, 429])" in BACKOFF
    assert "return status >= 500 || RETRYABLE_STATUSES.has(status);" in (
        BACKOFF
    )


def test_world_fetch_path_rides_the_shared_gate():
    assert 'from "./world-backoff.js";' in WORLD
    # The root fetch wrapper owns one gate keyed per endpoint, seeded with
    # the pre-existing requestFailures map so its identity is preserved.
    assert (
        "this.requestBackoff = createWorldBackoff({"
        " store: this.requestFailures });"
    ) in WORLD
    assert "withWorldBackoff(this.requestBackoff, `${method}:${path}`" in (
        WORLD
    )
    # Raw fetch sites attach status/Retry-After evidence before throwing so
    # they back off on the same evidence fetchJSON does.
    assert "markWorldHTTPFailure(" in WORLD


def test_every_world_module_with_http_calls_imports_the_gate():
    # The wiring must stay complete: any world module that fetches the API
    # goes through world-backoff.js rather than deciding retries alone.
    for name in ("world-notes.js", "world-discord-panel.js",
                 "world-office-tasks.js", "world-office-meeting.js",
                 "scene/core.js"):
        text = (WORLD_DIR / name).read_text(encoding="utf-8")
        assert "world-backoff.js" in text or "requestBackoff" in text, name
