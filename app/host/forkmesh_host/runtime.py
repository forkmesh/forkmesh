"""Load the unchanged Worker entry module and drive its handlers.

``HostRuntime`` imports ``entry`` from app/src_build (shims first on
``sys.path``), builds ``env`` and calls ``Default(ctx, env)`` for each fetch,
scheduled and queue event, exactly like the Workers runtime creates one
entrypoint per event.
"""

from __future__ import annotations

import asyncio
import importlib
import os
import sys
import time

from . import d1_caller
from . import js as _js
from . import paths
from .env import HostConfig, build_env
from .ffi import JsObject
from .workers import ExecutionContext

CRON_EXPRESSION = "* * * * *"
READ_ONLY_POLL_SECONDS = 1.0


def vm_rss_kib():
    try:
        with open("/proc/self/status", encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return 0


def _no_d1_log(sql, args=(), kind="query"):
    del sql, args, kind


def cron_minute_ms():
    try:
        value = int(os.environ.get("FORKMESH_HOST_CRON_MINUTE_MS") or 0)
    except ValueError:
        value = 0
    return value if value > 0 else 60_000


def _scale_cron_minute(entry):
    """Tests only (FORKMESH_HOST_CRON_MINUTE_MS): shrink the app's cron
    minute, and the tick bounds that hang off it, by the same factor, so a
    cluster test can watch many cron minutes rotate between nodes."""
    minute = cron_minute_ms()
    if minute == 60_000 or not hasattr(entry, "CRON_RUNNER_INTERVAL_MS"):
        return
    scale = minute / 60_000.0
    entry.CRON_RUNNER_INTERVAL_MS = minute
    for name in ("CRON_DISPATCH_TIMEOUT_MS", "CRON_RUNNER_ALARM_OFFSET_MS",
                 "CRON_RUNNER_KICK_DELAY_MS"):
        if hasattr(entry, name):
            setattr(entry, name, max(50, int(getattr(entry, name) * scale)))


class HostRuntime:
    def __init__(self, config: HostConfig | None = None, src_dir=None,
                 entry_module="entry"):
        self.config = config or HostConfig()
        paths.ensure_import_paths(src_dir)
        self.entry = importlib.import_module(entry_module)
        d1_caller.disable_done()
        if os.environ.get("FORKMESH_HOST_D1_LOG") != "1" and hasattr(
                self.entry, "_log_d1_query"):
            # entry.py prints every D1 statement for the Cloudflare tail. On
            # the host that is one stdout write per query; swap the logger in
            # the entry globals (lazy modules bind it from there on load).
            original = self.entry._log_d1_query
            for module in list(sys.modules.values()):
                if getattr(module, "_log_d1_query", None) is original:
                    module._log_d1_query = _no_d1_log
        elif hasattr(self.entry, "_D1_QUERY_CALLER"):
            self.entry._D1_QUERY_CALLER = d1_caller
            d1_caller.enable_done(
                getattr(self.entry, "D1_QUERY_LOG_MAX_CHARS", 360))
        _scale_cron_minute(self.entry)
        self.env, self.durable, self.queues = build_env(
            self.config, self._resolve_class)
        from . import vault as _vault
        self.vault = _vault.Vault(self) if _vault.enabled(
            self.config.environ) else None
        self._set_queues(not self.sealed)
        if self.sealed:
            self.set_available(False)
        self._vault_task = None
        self.dns = None
        self.ids_alerts = None
        self.mail_events = None
        self.autoscaler = None
        self.metrics_publisher = None
        self.host_metrics = None
        membership = getattr(self.durable, "membership", None)
        if getattr(membership, "quarantine", None) is not None:
            membership.on_quarantine = self._on_quarantine
        self._tasks: set = set()
        self._cron_task = None
        self._maintenance_task = None
        self.cron_runs = 0
        self.cron_failures = 0
        db_module = sys.modules.get(type(self.env.DB).__module__)
        self._db_scope = getattr(db_module, "request_scope", None)
        from .background import BackgroundRefresh
        self.background = BackgroundRefresh(self._track, self._db_scope)
        self.env.HOST_BACKGROUND = self.background
        self.env.HOST_STOPPING = asyncio.Event()
        self._warm_task = None
        self.read_only_override = None
        self._read_only_on = False
        self._read_only_task = None
        if hasattr(self.env.DB, "read_only_probe"):
            self.env.DB.read_only_probe = lambda: self.read_only

    # -- vault ---------------------------------------------------------
    @property
    def sealed(self):
        return (self.vault is not None and self.vault.sealed) or \
            self.quarantined

    @property
    def quarantined(self):
        """This node is quarantined by the IDS (quarantine.py, docs/IDS.md):
        it serves nothing, like a sealed node, whatever the vault says."""
        membership = getattr(self.durable, "membership", None)
        return bool(getattr(membership, "self_quarantined", False))

    @property
    def read_only(self):
        if self.read_only_override is not None:
            return bool(self.read_only_override)
        membership = getattr(self.durable, "membership", None)
        return bool(getattr(membership, "read_only", False))

    def sync_read_only(self):
        active = self.read_only and not self.sealed
        if active == self._read_only_on:
            return False
        self._read_only_on = active
        self._set_queues(not active and not self.sealed)
        if active:
            print("read-only mode ON: no rqlite leader; serving reads from "
                  "the local copy and refusing writes (docs/PLAN.md)",
                  flush=True)
        else:
            print("read-only mode OFF: writes are back", flush=True)
        return True

    async def read_only_loop(self):
        while True:
            await asyncio.sleep(READ_ONLY_POLL_SECONDS)
            try:
                self.sync_read_only()
            except Exception as error:
                print("read-only sync failed: %r" % (error,), flush=True)

    async def _on_quarantine(self, quarantined):
        if quarantined:
            print("ids: THIS NODE IS QUARANTINED; sealing and refusing to "
                  "serve (docs/IDS.md)", flush=True)
            self.set_available(False)
            if self.vault is not None:
                # DATA_KEY leaves memory; an unsealed process also exits
                # (it restarts sealed and stays sealed while quarantined).
                await self.vault.seal_and_exit()
        else:
            print("ids: quarantine released; %s" % (
                "unseal from the desktop (next_ids.py release does it)"
                if self.vault is not None else "serving again"), flush=True)
            if self.vault is None:
                self.set_available(True)

    def _set_queues(self, active):
        for queue in self.queues.values():
            queue.consumer = self._queue_consumer if active else None
            if active and queue._pending:
                try:
                    queue._schedule(0)
                except RuntimeError:  # no running loop yet
                    pass

    def set_available(self, available):
        """Called by the vault on unseal (True) and seal (False)."""
        self._set_queues(available)
        setter = getattr(self.durable, "set_available", None)
        if setter is not None:
            setter(available)

    # -- plumbing ------------------------------------------------------
    def _resolve_class(self, class_name):
        cls = getattr(self.entry, class_name, None)
        if cls is None:
            raise RuntimeError("Durable Object class %s is not exported by "
                               "the entry module" % class_name)
        return cls

    def _track(self, task):
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    def context(self):
        return ExecutionContext(self._track)

    def entrypoint(self, ctx=None):
        return self.entry.Default(ctx or self.context(), self.env)

    # -- events --------------------------------------------------------
    async def fetch(self, request):
        if self.sealed:
            raise RuntimeError("host is sealed")
        ctx = self.context()
        self._remember_instance()
        from . import privacy
        with d1_caller.request_scope(request, privacy.active(self.env)):
            if self._db_scope is not None:
                with self._db_scope():
                    response = await self.entrypoint(ctx).fetch(request)
            else:
                response = await self.entrypoint(ctx).fetch(request)
        return self._instance_error_text(response)

    def _remember_instance(self):
        features = sys.modules.get("instance_features")
        if features is not None and hasattr(features, "remember_instance"):
            features.remember_instance(self.env)

    def _instance_error_text(self, response):
        features = sys.modules.get("instance_features")
        try:
            status = int(getattr(response, "status", 0) or 0)
        except (TypeError, ValueError):
            return response
        if features is None or status < 400 or not features.branded(self.env):
            return response
        content_type = str(response.headers.get("content-type") or "").lower()
        body = getattr(response, "_body", None)
        if "json" not in content_type or not isinstance(body, (bytes, str)):
            return response
        renamed = features.rename_error_body(self.env, body)
        if renamed is not body:
            response._body = renamed
            if response.headers.has("content-length"):
                response.headers.set("content-length", str(len(
                    renamed if isinstance(renamed, bytes) else renamed.encode("utf-8"))))
        return response

    async def scheduled(self, scheduled_time=None, cron=CRON_EXPRESSION):
        if self.sealed:
            raise RuntimeError("host is sealed")
        ctx = self.context()
        self._remember_instance()
        controller = JsObject(
            scheduledTime=int(scheduled_time or _js.Date.now()),
            cron=cron, type="scheduled")
        controller["noRetry"] = lambda: None
        with d1_caller.scope("cron"):
            return await self.entrypoint(ctx).scheduled(
                controller, self.env, ctx)

    async def _queue_consumer(self, batch):
        ctx = self.context()
        with d1_caller.scope("queue %s" % (getattr(batch, "queue", "") or "")):
            return await self.entrypoint(ctx).queue(batch, self.env, ctx)

    # -- lifecycle -----------------------------------------------------
    async def cron_loop(self):
        """Call scheduled() at the top of every minute, like a Cron Trigger.

        Every node does this; the kick lands on whichever node owns the
        cron coordinator for this minute (cluster.rotation_owner)."""
        minute = cron_minute_ms() / 1000.0
        while True:
            now = time.time()
            await asyncio.sleep(minute - (now % minute) + 0.05)
            scheduled = int(time.time() // minute * minute * 1000)
            if self.sealed or self.read_only:
                continue
            try:
                await self.scheduled(scheduled)
                self.cron_runs += 1
            except Exception as error:
                self.cron_failures += 1
                print("scheduled() failed: %r" % (error,))

    async def maintenance_loop(self):
        while True:
            await asyncio.sleep(60)
            evict = getattr(self.durable, "evict_idle", None)
            if evict is not None:
                evict()

    def start(self, cron=True):
        loop = asyncio.get_running_loop()
        if self.vault is not None and self._vault_task is None:
            self._vault_task = loop.create_task(self.vault.serve())
        if cron and self._cron_task is None:
            self._cron_task = loop.create_task(self.cron_loop())
        from . import background
        if self._warm_task is None and background.warm_enabled(
                self.config.environ):
            self._warm_task = loop.create_task(background.warm_caches(self))
            self._track(self._warm_task)
        if self._read_only_task is None:
            self._read_only_task = loop.create_task(self.read_only_loop())
        if self._maintenance_task is None:
            self._maintenance_task = loop.create_task(
                self.maintenance_loop())
            start = getattr(self.durable, "start", None)
            if start is not None:
                start()
            from . import dns_reconciler
            self.dns = dns_reconciler.from_runtime(self)
            if self.dns is not None:
                self.dns.start()
            from . import ids_alerts
            self.ids_alerts = ids_alerts.from_runtime(self)
            if self.ids_alerts is not None:
                self.ids_alerts.start()
            from . import mail_events
            self.mail_events = mail_events.from_runtime(self)
            if self.mail_events is not None:
                self.mail_events.start()
            from . import autoscaler
            self.autoscaler, self.metrics_publisher = \
                autoscaler.from_runtime(self)
            for worker in (self.autoscaler, self.metrics_publisher):
                if worker is not None:
                    worker.start()

    async def stop(self, timeout=10.0):
        for task in (self._cron_task, self._maintenance_task,
                     self._warm_task, self._read_only_task):
            if task is not None:
                task.cancel()
        self._cron_task = self._maintenance_task = self._warm_task = None
        self._read_only_task = None
        if self.dns is not None:
            await self.dns.stop()
        if self.ids_alerts is not None:
            await self.ids_alerts.stop()
        if self.mail_events is not None:
            await self.mail_events.stop()
        for worker in (self.autoscaler, self.metrics_publisher):
            if worker is not None:
                await worker.stop()
        if self.vault is not None:
            await self.vault.close()
        for queue in self.queues.values():
            try:
                await asyncio.wait_for(queue.flush(), timeout)
            except Exception:
                pass
        pending = [t for t in self._tasks if not t.done()]
        if pending:
            await asyncio.wait(pending, timeout=timeout)
        stop = getattr(self.durable, "stop", None)
        if stop is not None:
            try:
                await asyncio.wait_for(stop(), timeout)
            except Exception as error:
                print("durable host stop failed: %r" % (error,))
        await _js.close_http_client()
        close = getattr(self.env.DB, "aclose", None)
        if close is not None:
            await close()

    def import_lazy_modules(self):
        """Import every module the entry loads lazily (for warm-up / RSS)."""
        loaded = 0
        for name in dir(self.entry):
            value = getattr(self.entry, name, None)
            if type(value).__name__ == "_LazyModule":
                try:
                    value._load()
                    loaded += 1
                except Exception as error:
                    print("lazy module %s failed: %r" % (value._name, error))
        return loaded
