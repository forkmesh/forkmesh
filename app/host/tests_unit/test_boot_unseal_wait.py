import asyncio
import sys
import time
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from forkmesh_host import asgi  # noqa: E402


class _Runtime:
    def __init__(self, unseals=0, quarantined=False):
        self.vault = types.SimpleNamespace(sealed=True,
                                           stats={"unseals": unseals})
        self.quarantined = quarantined

    @property
    def sealed(self):
        return self.vault.sealed or self.quarantined


def _host(runtime, age=0.0):
    host = asgi.HostASGI.__new__(asgi.HostASGI)
    host.runtime = runtime
    host._born = time.monotonic() - age
    host.boot_unseal_waits = 0
    return host


def _timed(coro):
    started = time.monotonic()
    result = asyncio.run(coro)
    return result, time.monotonic() - started


def test_boot_request_waits_for_the_first_unseal():
    runtime = _Runtime()
    host = _host(runtime)

    async def run():
        async def unseal():
            await asyncio.sleep(0.2)
            runtime.vault.sealed = False
        task = asyncio.ensure_future(unseal())
        sealed = await host._still_sealed()
        await task
        return sealed

    sealed, took = _timed(run())
    assert sealed is False
    assert 0.15 < took < asgi.BOOT_UNSEAL_WAIT_SECONDS
    assert host.boot_unseal_waits == 1


def test_boot_wait_gives_up_after_the_wait(monkeypatch):
    monkeypatch.setattr(asgi, "BOOT_UNSEAL_WAIT_SECONDS", 0.2)
    sealed, took = _timed(_host(_Runtime())._still_sealed())
    assert sealed is True
    assert 0.15 < took < 1.0


def test_no_wait_after_the_boot_window_a_manual_seal_or_quarantine():
    cases = [_host(_Runtime(), age=asgi.BOOT_UNSEAL_WINDOW_SECONDS + 1),
             _host(_Runtime(unseals=1)),
             _host(_Runtime(quarantined=True))]
    for host in cases:
        sealed, took = _timed(host._still_sealed())
        assert sealed is True
        assert took < 0.05
        assert host.boot_unseal_waits == 0


def test_unsealed_host_never_waits():
    runtime = _Runtime()
    runtime.vault.sealed = False
    sealed, took = _timed(_host(runtime)._still_sealed())
    assert sealed is False and took < 0.05
