from __future__ import annotations

import asyncio
import importlib
import json
import time

PREFIX = "web-requests "
WINDOW_S = 10
MAX_GROUPS = 200
FLAGSHIP_GROUP = "repo/forkmesh/forkmesh/content"


def _api_group(path):
    return importlib.import_module("api_metrics")._masked_group(path)


def status_class(status):
    status = int(status or 0)
    if status >= 500 or status <= 0:
        return "5xx"
    if status >= 400:
        return "4xx"
    if status >= 300:
        return "3xx"
    return "2xx"


class RequestTally:
    def __init__(self, *, group=_api_group, clock=time.time, emit=None,
                 window_s=WINDOW_S):
        self.group = group
        self.clock = clock
        self.emit = emit or (lambda line: print(line, flush=True))
        self.window_s = window_s
        self.window = None
        self.rows = {}
        self.groups = set()

    def _window_of(self, now):
        return int(now // self.window_s) * self.window_s

    def record(self, path, status, duration_ms):
        if not str(path or "").startswith("/api/"):
            return False
        try:
            group = self.group(path)
        except Exception:
            group = "other"
        if group not in self.groups:
            if len(self.groups) >= MAX_GROUPS and group != FLAGSHIP_GROUP:
                group = "other"
            self.groups.add(group)
        window = self._window_of(self.clock())
        if self.window is not None and window != self.window:
            self.flush()
        self.window = window
        duration = max(0, int(round(duration_ms)))
        row = self.rows.setdefault((group, status_class(status)), [0, 0, 0])
        row[0] += 1
        row[1] += duration
        row[2] = max(row[2], duration)
        return True

    def line(self):
        if not self.rows or self.window is None:
            return None
        rows = [[group, klass, count, total, peak]
                for (group, klass), (count, total, peak)
                in sorted(self.rows.items())]
        return PREFIX + json.dumps(
            {"t": int(self.window * 1000), "s": self.window_s, "r": rows},
            separators=(",", ":"))

    def flush(self):
        text = self.line()
        self.rows = {}
        self.window = None
        if text is not None:
            try:
                self.emit(text)
            except Exception:
                pass
        return text

    def due(self):
        return (self.window is not None
                and self._window_of(self.clock()) != self.window)

    async def run(self):
        try:
            while True:
                now = self.clock()
                await asyncio.sleep(
                    self.window_s - (now % self.window_s) + 0.05)
                if self.due():
                    self.flush()
        finally:
            self.flush()
