"""Build the Worker ``env`` for the CPython host.

Variables, in increasing precedence:

1. ``[vars]`` of app/wrangler.toml;
2. an env file (``--env-file`` / ``FORKMESH_ENV_FILE``): ``NAME=value`` lines;
3. process environment ``FORKMESH_VAR_<NAME>=value``;
4. secrets: one file per name in ``$CREDENTIALS_DIRECTORY`` (systemd
   ``LoadCredential=``/``LoadCredentialEncrypted=``).

With ``FORKMESH_VAULT=1`` only the bootstrap credentials
(``vault_crypto.BOOTSTRAP_NAMES``) are read and ``DATA_KEY`` is dropped from
every source: the host starts sealed and gets its secrets from the
``host_secrets`` vault when the operator unseals it (``vault.py``).

``FORKMESH_PLATFORM`` defaults to ``"vps"``.

Bindings:

* ``DB``: pluggable. ``HostConfig.db_factory`` (a callable returning a
  D1-shaped object) wins; otherwise ``FORKMESH_HOST_DB`` chooses:
  ``sqlite:<path>`` (default, local development), ``rqlite:<url>`` (uses
  ``forkmesh_host.d1_rqlite.make_d1``) or ``<module>:<callable>``.
* ``REPOSITORY_METADATA``: ``LocalKV`` on ``<data>/kv.sqlite``.
* ``NOTIFICATION_QUEUE``: ``LocalQueue`` (settings from [[queues.consumers]]).
* ``ASSETS``: ``StaticAssets`` on app/dist.
* ``AI``: only when ``AI_API_URL`` is set.
* Durable Object namespaces from ``[[durable_objects.bindings]]`` on a
  pluggable host (``HostConfig.durable_host_factory``); with cluster
  settings (``FORKMESH_NODE_ID`` + ``FORKMESH_MESH_LISTEN``, see
  ``cluster.py``) ``durable_cluster.ClusterDurableHost``, otherwise the
  single-process ``durable.LocalDurableHost``.
"""

from __future__ import annotations

import importlib
import json
import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from . import paths
from .bindings import LocalKV, LocalQueue, OpenAICompatibleAI, StaticAssets

VAR_PREFIX = "FORKMESH_VAR_"


def _as_var(value):
    if isinstance(value, str):
        return value
    return json.dumps(value)


def parse_env_file(path):
    values = {}
    for raw in Path(path).read_text("utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        if line.startswith("export "):
            line = line[len("export "):]
        name, _, value = line.partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        values[name.strip()] = value
    return values


def read_credentials(directory):
    values = {}
    if not directory:
        return values
    root = Path(directory)
    if not root.is_dir():
        return values
    for item in sorted(root.iterdir()):
        if item.is_file() and not item.name.startswith("."):
            values[item.name] = item.read_text("utf-8").rstrip("\r\n")
    return values


@dataclass
class HostConfig:
    wrangler_toml: Path = field(default_factory=lambda: paths.APP_DIR /
                                "wrangler.toml")
    assets_dir: Path = field(default_factory=lambda: Path(os.environ.get(
        "FORKMESH_HOST_ASSETS_DIR") or paths.APP_DIR / "dist"))
    data_dir: Path = field(default_factory=paths.default_data_dir)
    env_file: str | None = None
    overrides: dict = field(default_factory=dict)
    db_factory: Callable[[], Any] | None = None
    durable_host_factory: Callable | None = None
    environ: dict = field(default_factory=lambda: dict(os.environ))

    def toml(self):
        with open(self.wrangler_toml, "rb") as handle:
            return tomllib.load(handle)


class Env:
    """Attribute bag like the Workers ``env`` (missing -> AttributeError)."""

    def __init__(self, values=None):
        for name, value in (values or {}).items():
            object.__setattr__(self, name, value)

    def __js_keys__(self):
        return [k for k in vars(self) if not k.startswith("_")]

    def __repr__(self):
        return "Env(%s)" % ", ".join(sorted(self.__js_keys__()))


def collect_vars(config: HostConfig, manifest=None):
    manifest = manifest if manifest is not None else config.toml()
    values = {k: _as_var(v) for k, v in (manifest.get("vars") or {}).items()}
    env_file = config.env_file or config.environ.get("FORKMESH_ENV_FILE")
    if env_file:
        values.update(parse_env_file(env_file))
    for name, value in config.environ.items():
        if name.startswith(VAR_PREFIX) and len(name) > len(VAR_PREFIX):
            values[name[len(VAR_PREFIX):]] = value
    credentials = read_credentials(config.environ.get(
        "CREDENTIALS_DIRECTORY"))
    vault_on = str(config.environ.get("FORKMESH_VAULT") or "") == "1"
    if vault_on:
        # Sealed start (forkmesh_host.vault): only the bootstrap credentials
        # rqlite and the mesh need; everything else comes from the vault.
        from .vault_crypto import BOOTSTRAP_NAMES
        credentials = {k: v for k, v in credentials.items()
                       if k in BOOTSTRAP_NAMES}
    values.update(credentials)
    values.update({k: _as_var(v) for k, v in config.overrides.items()})
    if vault_on:
        # DATA_KEY only ever arrives over the unseal socket, whatever an
        # env file or FORKMESH_VAR_DATA_KEY says.
        values.pop("DATA_KEY", None)
    values.setdefault("FORKMESH_PLATFORM", "vps")
    return values


def make_db(config: HostConfig, values):
    if config.db_factory is not None:
        return config.db_factory()
    spec = config.environ.get("FORKMESH_HOST_DB") or values.get(
        "FORKMESH_HOST_DB") or ""
    if not spec or spec.startswith("sqlite:"):
        from .d1_sqlite import D1Sqlite
        path = spec[len("sqlite:"):] if spec else str(
            Path(config.data_dir) / "d1.sqlite")
        return D1Sqlite(path)
    if spec.startswith(("rqlite:", "http://", "https://")):
        url = spec[len("rqlite:"):] if spec.startswith("rqlite:") else spec
        from . import d1_rqlite
        options = {"url": url}
        for key in ("RQLITE_USERNAME", "RQLITE_PASSWORD", "RQLITE_READ_MODE",
                    "RQLITE_DB_PATH", "RQLITE_TIMEOUT", "RQLITE_FRESHNESS",
                    "RQLITE_CONFIRM_MISSES", "RQLITE_CONFIRM_RATE",
                    "RQLITE_CONFIRM_BURST"):
            value = config.environ.get(key) or values.get(key)
            if value:
                options[key] = value
        return d1_rqlite.make_d1(options)
    module_name, _, attr = spec.partition(":")
    return getattr(importlib.import_module(module_name), attr)()


def _queue_settings(manifest, queue_name):
    for consumer in (manifest.get("queues") or {}).get("consumers") or []:
        if consumer.get("queue") == queue_name:
            return consumer
    return {}


def build_env(config: HostConfig, resolve_class):
    """Return (env, durable_host, queues) for the entry module."""
    manifest = config.toml()
    values = collect_vars(config, manifest)
    env = Env(values)
    data_dir = Path(config.data_dir)
    data_dir.mkdir(parents=True, exist_ok=True)

    env.DB = make_db(config, values)

    assets = manifest.get("assets") or {}
    env.ASSETS = StaticAssets(
        config.assets_dir,
        assets.get("html_handling", "auto-trailing-slash"),
        assets.get("not_found_handling", "none"))

    for kv in manifest.get("kv_namespaces") or []:
        setattr(env, kv["binding"], LocalKV(data_dir / (
            "kv-%s.sqlite" % kv["binding"].lower())))

    queues = {}
    for producer in (manifest.get("queues") or {}).get("producers") or []:
        settings = _queue_settings(manifest, producer["queue"])
        queue = LocalQueue(
            producer["queue"],
            settings.get("max_batch_size", 10),
            settings.get("max_batch_timeout", 1),
            settings.get("max_retries", 3))
        setattr(env, producer["binding"], queue)
        queues[producer["binding"]] = queue

    ai_url = values.get("AI_API_URL") or config.environ.get("AI_API_URL")
    ai_binding = (manifest.get("ai") or {}).get("binding")
    if ai_url and ai_binding:
        try:
            model_map = json.loads(values.get("AI_MODEL_MAP") or "{}")
        except ValueError:
            model_map = {}
        setattr(env, ai_binding, OpenAICompatibleAI(
            ai_url, values.get("AI_API_KEY", ""), model_map,
            values.get("AI_DEFAULT_MODEL", "")))

    from .cluster import ClusterConfig
    cluster_config = ClusterConfig.from_environ(config.environ, values)
    if config.durable_host_factory is not None:
        durable_host = config.durable_host_factory(resolve_class, lambda: env)
    elif cluster_config is not None:
        from .durable_cluster import ClusterDurableHost
        durable_host = ClusterDurableHost(resolve_class, lambda: env,
                                          cluster_config)
    else:
        from .durable import LocalDurableHost
        durable_host = LocalDurableHost(resolve_class, lambda: env)
    for binding in (manifest.get("durable_objects") or {}).get(
            "bindings") or []:
        setattr(env, binding["name"], durable_host.namespace(
            binding["name"], binding["class_name"]))
    return env, durable_host, queues
