from __future__ import annotations

import asyncio
import contextlib
import os
import time

from . import d1_caller

MAX_RUNNING = 16
WARM_WAIT_SECONDS = 300.0
WARM_POLL_SECONDS = 1.0


class BackgroundRefresh:
    def __init__(self, tracker=None, db_scope=None, max_running=MAX_RUNNING):
        self._tracker = tracker
        self._db_scope = db_scope
        self.max_running = int(max_running)
        self.running: dict = {}
        self.flights: dict = {}
        self.started = 0
        self.failed = 0
        self.joined = 0

    def __call__(self, key, factory):
        key = str(key or "")
        if not key:
            return False
        if key in self.running:
            return True
        if len(self.running) >= self.max_running:
            return False
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return False
        task = loop.create_task(self._run(key, factory))
        self.running[key] = task
        self.started += 1
        if self._tracker is not None:
            self._tracker(task)
        return True

    async def join(self, key, factory):
        key = str(key or "")
        task = self.flights.get(key) if key else None
        if task is None:
            task = asyncio.get_running_loop().create_task(factory())
            if key:
                self.flights[key] = task
                task.add_done_callback(
                    lambda done, key=key: self._flight_done(key, done))
            if self._tracker is not None:
                self._tracker(task)
        else:
            self.joined += 1
        return await asyncio.shield(task)

    def _flight_done(self, key, task):
        if self.flights.get(key) is task:
            del self.flights[key]
        if not task.cancelled():
            task.exception()

    async def _run(self, key, factory):
        try:
            with d1_caller.scope("background " + key):
                with (self._db_scope() if self._db_scope is not None
                      else contextlib.nullcontext()):
                    await factory()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.failed += 1
            print("background refresh %s failed: %r" % (key, error))
        finally:
            self.running.pop(key, None)


def warm_enabled(environ=None):
    environ = os.environ if environ is None else environ
    return str(environ.get("FORKMESH_HOST_WARM", "1")).strip() != "0"


async def warm_caches(runtime, wait_seconds=WARM_WAIT_SECONDS,
                      poll_seconds=WARM_POLL_SECONDS):
    warm = getattr(runtime.entry, "host_warm_caches", None)
    if warm is None:
        return False
    deadline = time.monotonic() + wait_seconds
    while runtime.sealed:
        if time.monotonic() >= deadline:
            return False
        await asyncio.sleep(poll_seconds)
    started = time.perf_counter()
    try:
        with d1_caller.scope("warm caches"):
            await warm(runtime.env)
    except Exception as error:
        print("cache warm failed: %r" % (error,))
        return False
    print("cache warm %.0fms" % ((time.perf_counter() - started) * 1000))
    return True
