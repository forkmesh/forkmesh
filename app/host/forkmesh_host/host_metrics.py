from __future__ import annotations

import asyncio
import collections
import os
import time

WINDOW_S = 300
MAX_SAMPLES = 60_000
LAG_INTERVAL_S = 0.5
SKIPPED_PATHS = ("/readyz", "/health", "/health/mobile")


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


class HostMetrics:
    def __init__(self, *, window_s=WINDOW_S, clock=time.monotonic,
                 loadavg=None, cpu_count=None):
        self.window_s = window_s
        self.clock = clock
        self.loadavg = loadavg or os.getloadavg
        self.cpu_count = cpu_count or os.cpu_count
        self.samples = collections.deque(maxlen=MAX_SAMPLES)
        self.lags = collections.deque(maxlen=int(window_s / LAG_INTERVAL_S) + 8)
        self.started = clock()

    def record(self, path, duration_ms, now=None):
        if str(path or "") in SKIPPED_PATHS:
            return False
        self.samples.append((self.clock() if now is None else now,
                             max(0.0, float(duration_ms))))
        return True

    def record_lag(self, lag_ms, now=None):
        self.lags.append((self.clock() if now is None else now,
                          max(0.0, float(lag_ms))))

    def _prune(self, now):
        cutoff = now - self.window_s
        while self.samples and self.samples[0][0] < cutoff:
            self.samples.popleft()
        while self.lags and self.lags[0][0] < cutoff:
            self.lags.popleft()

    def snapshot(self, now=None):
        now = self.clock() if now is None else now
        self._prune(now)
        durations = [ms for _, ms in self.samples]
        lags = [ms for _, ms in self.lags]
        span = max(1.0, min(self.window_s, now - self.started))
        try:
            load1 = float(self.loadavg()[0])
        except (OSError, AttributeError, IndexError):
            load1 = None
        cpus = int(self.cpu_count() or 1)
        p50 = percentile(durations, 0.50)
        p95 = percentile(durations, 0.95)
        lag95 = percentile(lags, 0.95)
        return {
            "windowS": int(span),
            "requests": len(durations),
            "rps": round(len(durations) / span, 3),
            "p50Ms": None if p50 is None else round(p50, 1),
            "p95Ms": None if p95 is None else round(p95, 1),
            "loopLagMs": None if lag95 is None else round(lag95, 1),
            "loopLagMaxMs": round(max(lags), 1) if lags else None,
            "load1": None if load1 is None else round(load1, 2),
            "cpus": cpus,
            "loadPerCpu": None if load1 is None else round(load1 / cpus, 3),
        }

    async def run_lag_probe(self, interval=LAG_INTERVAL_S):
        loop = asyncio.get_running_loop()
        while True:
            started = loop.time()
            await asyncio.sleep(interval)
            self.record_lag((loop.time() - started - interval) * 1000.0)
