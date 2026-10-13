"""Gate: every js/pyodide/workers/env/ctx name the app uses exists on the host.

``tools/js_surface_inventory.py`` scans app/src, world/src and marketplace
worker modules. Each chain (``js.crypto.subtle.digest()``,
``js.Uint8Array.new().to_py()``, ``env.DB.prepare().run()``) is walked on the
host objects; ``()`` steps use the sample results below. A chain whose call
result has no sample here fails the test, so a new call shape needs a sample
(and usually a shim change) before it can land.
"""

from __future__ import annotations

import asyncio
import importlib.util
import sys

import pytest

from forkmesh_host import js, workers
from forkmesh_host import paths
from forkmesh_host.durable import DurableObjectState, LocalDurableHost
from forkmesh_host.ffi import JsArray, JsObject
from forkmesh_host.env import HostConfig, build_env

_spec = importlib.util.spec_from_file_location(
    "js_surface_inventory", paths.REPO_DIR / "tools" / "js_surface_inventory.py")
inventory_module = importlib.util.module_from_spec(_spec)
sys.modules["js_surface_inventory"] = inventory_module
_spec.loader.exec_module(inventory_module)


def _run(coro):
    return asyncio.new_event_loop().run_until_complete(coro)


def _pair():
    return js.WebSocketPair.new()


async def _cache_hit():
    await js.caches.default.put("https://inventory.test/x", js.Response.new(
        "{}", {"headers": {"cache-control": "max-age=60",
                           "content-type": "application/json"}}))
    return await js.caches.default.match("https://inventory.test/x")


async def _generate_key():
    return await js.crypto.subtle.generateKey(
        {"name": "Ed25519"}, True, ["sign", "verify"])


def _fetch_response():
    return js.Response.new("{\"a\":1}", {"status": 200, "headers": {
        "content-type": "application/json"}})


# Results of the call chains the app makes, keyed by chain (``()`` = call).
SAMPLES = {
    "js.AbortController.new()": lambda: js.AbortController.new(),
    "js.AbortController.new().abort.bind()":
        lambda: js.AbortController.new().abort.bind(None),
    "js.AbortSignal.timeout()": lambda: js.AbortSignal.timeout(10),
    "js.Date.now()": lambda: js.Date.now(),
    "js.Date.parse()": lambda: js.Date.parse("2026-01-01T00:00:00Z"),
    "js.JSON.stringify()": lambda: js.JSON.stringify({"a": 1}),
    "js.Object.getOwnPropertyNames()":
        lambda: js.Object.getOwnPropertyNames({}),
    "js.Object.keys()": lambda: js.Object.keys({}),
    "js.Request.new()": lambda: js.Request.new("https://a.test/"),
    "js.Request.new().headers.set()": lambda: None,
    "js.Response.new()": lambda: js.Response.new("x"),
    "js.URL.new()": lambda: js.URL.new("https://a.test/p?q=1#h"),
    "js.Uint8Array.new()": lambda: js.Uint8Array.new(4),
    "js.Uint8Array.new().to_py()": lambda: js.Uint8Array.new(4).to_py(),
    "js.WebSocketPair.new()": _pair,
    "js.WebSocketPair.new().object_values()": lambda: _pair().object_values(),
    "js.caches.default.delete()": lambda: False,
    "js.caches.default.match()": lambda: _run(_cache_hit()),
    "js.caches.default.match().headers.get()": lambda: None,
    "js.caches.default.put()": lambda: None,
    "js.clearTimeout()": lambda: None,
    "js.crypto.getRandomValues()":
        lambda: js.crypto.getRandomValues(js.Uint8Array.new(4)),
    "js.crypto.getRandomValues().to_py()": lambda: memoryview(b""),
    "js.crypto.subtle.generateKey()": lambda: _run(_generate_key()),
    "js.crypto.subtle.exportKey()": lambda: _run(
        js.crypto.subtle.exportKey("jwk", _run(_generate_key()).privateKey)),
    "js.fetch()": _fetch_response,
    "js.fetch().json()": lambda: _run(_fetch_response().json()),
    "js.scheduler.wait()": lambda: None,
    "js.setTimeout()": lambda: 1,
    "pyodide.ffi.to_js()": lambda: JsObject(),
    "workers.Response()": lambda: workers.Response("x"),
    "workers.Response().headers.items()": lambda: [],
}
# Attribute names on objects called ``request`` that are not Worker requests
# (dict-shaped records named request in a few modules).
REQUEST_NON_WORKER_ATTRS = {"get"}


def _root(name):
    module, _, attr = name.partition(".")
    if module == "js":
        return js if not attr else getattr(js, attr)
    if name.startswith("pyodide.ffi"):
        from forkmesh_host import ffi
        rest = name[len("pyodide.ffi"):].lstrip(".")
        return ffi if not rest else getattr(ffi, rest)
    if module == "workers":
        return workers if not attr else getattr(workers, attr)
    raise KeyError(name)


def _segments(chain):
    out = []
    for part in chain.replace("()", ".()").split("."):
        if not part:
            continue
        out.append(part)
    return out


def _walk(chain, root_resolver, samples):
    """Return None when every step exists, else a failure description."""
    parts = _segments(chain)
    # Find the root: longest dotted prefix the resolver knows.
    for size in range(len(parts), 0, -1):
        if "()" in parts[:size]:
            continue
        try:
            value = root_resolver(".".join(parts[:size]))
            break
        except (KeyError, AttributeError):
            continue
    else:
        return "unknown root"
    prefix = ".".join(parts[:size])
    rest = parts[size:]
    for index, part in enumerate(rest):
        if part == "()":
            if not callable(value):
                return "%s is not callable" % prefix
            prefix += "()"
            if index == len(rest) - 1:
                return None  # a terminal call needs no result sample
            factory = samples.get(prefix)
            if factory is None:
                return "no sample for %s" % prefix
            value = factory()
            continue
        if not hasattr(value, part):
            return "%s has no attribute %s" % (prefix, part)
        value = getattr(value, part)
        prefix += "." + part
    return None


@pytest.fixture(scope="module")
def inventory():
    return inventory_module.scan()


def test_inventory_finds_the_known_surface(inventory):
    names = set(inventory["js"])
    for expected in ("js.Uint8Array.new()", "js.crypto.subtle.digest()",
                     "js.WebSocketPair.new().object_values()",
                     "js.caches.default.match()", "pyodide.ffi.jsnull",
                     "workers.DurableObject"):
        assert expected in names
    assert len(names) >= 100


def test_every_js_chain_resolves_on_the_host(inventory):
    failures = {}
    for chain in inventory["js"]:
        problem = _walk(chain, _root, SAMPLES)
        if problem is not None:
            failures[chain] = problem
    assert not failures, failures


@pytest.fixture(scope="module")
def host_env(tmp_path_factory):
    data = tmp_path_factory.mktemp("surface")

    class _Dummy:
        def __init__(self, ctx, env):
            self.ctx, self.env = ctx, env

    config = HostConfig(data_dir=data, environ={})
    env, durable, _queues = build_env(config, lambda name: _Dummy)
    return env, durable


def test_every_env_binding_chain_resolves(inventory, host_env):
    env, _durable = host_env
    object_id = env.FORKMESH_WORLD.idFromName("x")
    samples = {
        "env.DB.prepare()": lambda: env.DB.prepare("SELECT 1"),
        "env.DB.prepare().run()": lambda: None,
        "env.DB.batch()": lambda: None,
        "env.ASSETS.fetch()": lambda: js.Response.new("x"),
        "js.fetch()": _fetch_response,
        "js.fetch().json()": lambda: JsObject(),
    }
    for chain in inventory["env_chains"]:
        if chain.startswith("env.FORKMESH_"):
            binding = chain.split(".")[1]
            samples["env.%s.idFromName()" % binding] = lambda: object_id
            samples["env.%s.get()" % binding] = (
                lambda b=binding: getattr(env, b).get(object_id))
            samples["env.%s.get().fetch()" % binding] = lambda: None

    def resolve(name):
        if name == "env":
            return env
        if name.startswith("env."):
            value = env
            for part in name.split(".")[1:]:
                value = getattr(value, part)
            return value
        return _root(name)

    failures = {}
    for chain in inventory["env_chains"]:
        problem = _walk(chain, resolve, samples)
        if problem is not None:
            failures[chain] = problem
    assert not failures, failures
    # Every binding name the app reads must be bound (vars are optional).
    bindings = [n for n in inventory["env_names"]
                if n in ("DB", "ASSETS", "REPOSITORY_METADATA")
                or n.startswith("FORKMESH_")]
    missing = [n for n in bindings if not hasattr(env, n)]
    assert not missing


def test_every_ctx_chain_resolves(inventory):
    class _Dummy:
        def __init__(self, ctx, env):
            pass

    host = LocalDurableHost(lambda name: _Dummy, lambda: None)
    namespace = host.namespace("X", "Dummy")
    state = DurableObjectState(namespace.idFromName("n"), host)
    samples = {
        "ctx.acceptWebSocket()": lambda: None,
        "ctx.blockConcurrencyWhile()": lambda: None,
        "ctx.getWebSockets()": lambda: JsArray(),
        "ctx.setWebSocketAutoResponse()": lambda: None,
        "ctx.storage.get()": lambda: None,
        "ctx.storage.put()": lambda: None,
        "ctx.storage.setAlarm()": lambda: None,
    }

    def resolve(name):
        if name == "ctx":
            return state
        raise KeyError(name)

    failures = {}
    for chain in inventory["ctx_chains"]:
        problem = _walk(chain, resolve, samples)
        if problem is not None:
            failures[chain] = problem
    assert not failures, failures
    assert state.id.name == "n"


def test_request_attributes_exist(inventory):
    request = js.Request.new("https://a.test/", {"method": "POST",
                                                 "body": "{}"})
    missing = [name for name in inventory["request_attrs"]
               if name not in REQUEST_NON_WORKER_ATTRS
               and not hasattr(request, name)]
    assert not missing
