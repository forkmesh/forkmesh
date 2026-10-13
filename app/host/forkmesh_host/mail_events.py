from __future__ import annotations

import asyncio
import contextlib
import json
import os
from pathlib import Path

INTERVAL_S = 15
BATCH = 200
MAX_BYTES = 16 * 1024


def spool_dir(environ=None):
    environ = os.environ if environ is None else environ
    return Path(environ.get("FORKMESH_MAIL_SPOOL") or "/var/lib/fmnext-mail-spool")


def cursor_path(environ=None):
    environ = os.environ if environ is None else environ
    base = environ.get("FORKMESH_HOST_DATA_DIR") or ""
    return Path(base) / "mail-events.cursor" if base else None


class MailEvents:
    def __init__(self, runtime, directory, cursor):
        self.runtime = runtime
        self.directory = directory
        self.cursor = cursor
        self._task = None

    def position(self):
        try:
            return self.cursor.read_text().strip()
        except OSError:
            return ""

    def advance(self, name):
        tmp = self.cursor.with_name(self.cursor.name + ".tmp")
        tmp.write_text(name)
        os.replace(tmp, self.cursor)

    def pending(self):
        after = self.position()
        try:
            names = sorted(p for p in self.directory.iterdir()
                           if p.suffix == ".json" and p.name[:1] != "."
                           and p.name > after)
        except OSError:
            return []
        return names[:BATCH]

    async def drain(self):
        entry = self.runtime.entry
        env = self.runtime.env
        handled = 0
        scope = getattr(self.runtime, "_db_scope", None)
        for path in self.pending():
            try:
                if path.stat().st_size > MAX_BYTES:
                    raise ValueError("oversized")
                event = json.loads(path.read_text())
            except (OSError, ValueError):
                self.advance(path.name)
                continue
            with (scope() if scope is not None else contextlib.nullcontext()):
                try:
                    await entry.record_core_mail_event(env, event)
                except Exception as error:
                    print("mail event %s failed: %r" % (path.name, error),
                          flush=True)
                    return handled
            self.advance(path.name)
            handled += 1
        return handled

    async def loop(self):
        while True:
            await asyncio.sleep(INTERVAL_S)
            if getattr(self.runtime, "sealed", False):
                continue
            with contextlib.suppress(Exception):
                await self.drain()

    def start(self):
        self._task = asyncio.get_running_loop().create_task(self.loop())

    async def stop(self):
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(BaseException):
                await self._task


def from_runtime(runtime):
    directory = spool_dir()
    cursor = cursor_path()
    if cursor is None or not directory.is_dir():
        return None
    return MailEvents(runtime, directory, cursor)
