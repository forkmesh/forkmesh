"""Small Discord response-header parser and projection cache.

Bot calls are globally serialized by ``ForkMeshDiscordGate``. This helper
interprets provider headers inside that object and keeps only short-lived rate
metadata plus a minimized public-channel preview in the stateless adapter.
No token, message body, OAuth credential, or raw provider response is retained.
"""

from __future__ import annotations


CATALOG_CACHE_MS = 5_000
MAX_COOLDOWN_MS = 60 * 60 * 1000
MAX_BUCKETS = 512
MAX_CATALOG = 32


def _number(value, default=0):
    try:
        value = float(str(value or "").strip())
    except (TypeError, ValueError, OverflowError):
        return default
    if value != value or value in (float("inf"), float("-inf")):
        return default
    return value


def _milliseconds(value):
    return max(0, min(MAX_COOLDOWN_MS, int(_number(value) * 1000)))


def _bucket(value):
    value = str(value or "").strip()
    if not value or len(value) > 160:
        return ""
    return "".join(char for char in value if char.isalnum() or char in "._:-")


class DiscordRateCoordinator:
    """Coordinate route buckets/global cooldowns and bounded catalog caching."""

    def __init__(self):
        self.global_until = 0
        self.path_buckets = {}
        self.bucket_until = {}
        self.catalog = {}

    def reserve(self, path, now):
        """Return remaining milliseconds before this request may leave Worker."""

        path = str(path or "")
        now = max(0, int(now or 0))
        bucket = self.path_buckets.get(path, "")
        until = max(self.global_until, self.bucket_until.get(bucket, 0))
        return max(0, until - now)

    def observe(self, path, status, headers, payload, now):
        """Record provider bucket/reset/global headers and return retry delay."""

        path = str(path or "")
        now = max(0, int(now or 0))
        headers = headers if isinstance(headers, dict) else {}
        bucket = _bucket(headers.get("x-ratelimit-bucket"))
        if bucket:
            self.path_buckets[path] = bucket
            if len(self.path_buckets) > MAX_BUCKETS:
                self.path_buckets = dict(list(self.path_buckets.items())[-MAX_BUCKETS:])
        else:
            bucket = self.path_buckets.get(path, "")
        # Discord does not always include a bucket on a 429. Fixed request
        # paths are themselves a safe bounded fallback, preventing a hot
        # caller from immediately retrying the same route in that case.
        if not bucket and path:
            bucket = "path:" + path[:120]
            self.path_buckets[path] = bucket
            if len(self.path_buckets) > MAX_BUCKETS:
                self.path_buckets = dict(list(self.path_buckets.items())[-MAX_BUCKETS:])
        header_retry = _milliseconds(headers.get("retry-after"))
        reset_after = _milliseconds(headers.get("x-ratelimit-reset-after"))
        reset_at = max(
            0,
            min(
                MAX_COOLDOWN_MS,
                int(_number(headers.get("x-ratelimit-reset")) * 1000) - now,
            ),
        )
        reset_after = max(reset_after, reset_at)
        body_retry = (
            _milliseconds(payload.get("retry_after"))
            if isinstance(payload, dict) else 0)
        retry = max(header_retry, body_retry)
        is_global = (
            str(headers.get("x-ratelimit-global") or "").lower() == "true"
            or str(headers.get("x-ratelimit-scope") or "").lower() == "global"
            or (bool(payload.get("global")) if isinstance(payload, dict) else False)
        )
        try:
            remaining = int(str(headers.get("x-ratelimit-remaining") or "-1"))
        except (TypeError, ValueError, OverflowError):
            remaining = -1
        if int(status or 0) == 429:
            cooldown = retry or reset_after or 1_000
            if is_global:
                self.global_until = max(self.global_until, now + cooldown)
            elif bucket:
                self.bucket_until[bucket] = max(
                    self.bucket_until.get(bucket, 0), now + cooldown)
                if len(self.bucket_until) > MAX_BUCKETS:
                    self.bucket_until = dict(
                        list(self.bucket_until.items())[-MAX_BUCKETS:])
            return cooldown
        # When Discord says this was the final remaining request, honor the
        # reset window proactively instead of causing an avoidable 429.
        if remaining == 0 and reset_after and bucket:
            self.bucket_until[bucket] = max(
                self.bucket_until.get(bucket, 0), now + reset_after)
            if len(self.bucket_until) > MAX_BUCKETS:
                self.bucket_until = dict(
                    list(self.bucket_until.items())[-MAX_BUCKETS:])
            return reset_after
        return 0

    def catalog_get(self, key, now):
        record = self.catalog.get(str(key or ""))
        if not record or int(record.get("until") or 0) <= int(now or 0):
            self.catalog.pop(str(key or ""), None)
            return None
        return record.get("data")

    def catalog_put(self, key, data, now, ttl_ms=CATALOG_CACHE_MS):
        ttl = max(1, min(CATALOG_CACHE_MS, int(ttl_ms or CATALOG_CACHE_MS)))
        self.catalog[str(key or "")] = {
            "data": data,
            "until": max(0, int(now or 0)) + ttl,
        }
        if len(self.catalog) > MAX_CATALOG:
            for stale in list(self.catalog)[:-MAX_CATALOG]:
                self.catalog.pop(stale, None)
