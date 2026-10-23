from __future__ import annotations

import asyncio
import collections
import os
import time

WINDOW_S = 300
MAX_SAMPLES = 60_000
LAG_INTERVAL_S = 0.5
SKIPPED_PATHS = ("/readyz", "/health", "/health/mobile")
CPU_MIN_INTERVAL_S = 5.0


def read_proc_stat():
    with open("/proc/stat", encoding="ascii") as handle:
        fields = handle.readline().split()
    if not fields or fields[0] != "cpu":
        return None
    ticks = [int(value) for value in fields[1:9]]
    idle = ticks[3] + (ticks[4] if len(ticks) > 4 else 0)
    return sum(ticks), idle


def read_meminfo():
    values = {}
    with open("/proc/meminfo", encoding="ascii") as handle:
        for line in handle:
            name, _, rest = line.partition(":")
            parts = rest.split()
            if parts:
                values[name] = int(parts[0])
    return values


def used_pct(used, total):
    if not total or total <= 0:
        return None
    return round(min(100.0, max(0.0, used * 100.0 / total)), 1)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, int(round(fraction * (len(ordered) - 1)))))
    return ordered[index]


class HostMetrics:
    def __init__(self, *, window_s=WINDOW_S, clock=time.monotonic,
                 loadavg=None, cpu_count=None, proc_stat=None,
                 meminfo=None, statvfs=None, disk_path="/"):
        self.window_s = window_s
        self.clock = clock
        self.loadavg = loadavg or os.getloadavg
        self.cpu_count = cpu_count or os.cpu_count
        self.proc_stat = proc_stat or read_proc_stat
        self.meminfo = meminfo or read_meminfo
        self.statvfs = statvfs or os.statvfs
        self.disk_path = disk_path
        self.cpu_last = None
        self.cpu_pct = None
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

    def cpu_used_pct(self, now):
        if self.cpu_last is not None and now - self.cpu_last[0] < CPU_MIN_INTERVAL_S:
            return self.cpu_pct
        try:
            totals = self.proc_stat()
        except (OSError, ValueError, IndexError):
            totals = None
        if totals is None:
            return self.cpu_pct
        if self.cpu_last is not None:
            total = totals[0] - self.cpu_last[1][0]
            idle = totals[1] - self.cpu_last[1][1]
            if total > 0:
                self.cpu_pct = used_pct(total - idle, total)
        self.cpu_last = (now, totals)
        return self.cpu_pct

    def memory_used_pct(self):
        try:
            info = self.meminfo()
        except (OSError, ValueError):
            return None
        total = info.get("MemTotal")
        available = info.get("MemAvailable")
        if not total or available is None:
            return None
        return used_pct(total - available, total)

    def disk_used_pct(self):
        try:
            stat = self.statvfs(self.disk_path)
        except (OSError, AttributeError):
            return None
        used = (stat.f_blocks - stat.f_bfree) * stat.f_frsize
        usable = used + stat.f_bavail * stat.f_frsize
        return used_pct(used, usable)

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
            "cpuPct": self.cpu_used_pct(now),
            "memPct": self.memory_used_pct(),
            "diskPct": self.disk_used_pct(),
        }

    async def run_lag_probe(self, interval=LAG_INTERVAL_S):
        loop = asyncio.get_running_loop()
        while True:
            started = loop.time()
            await asyncio.sleep(interval)
            self.record_lag((loop.time() - started - interval) * 1000.0)
