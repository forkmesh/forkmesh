import json
import os
import signal
import socket
import sys
import threading
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import mirror_gateway as gateway  # noqa: E402
from test_mirror_gateway import make_bare_repository, write_config  # noqa: E402


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


@pytest.fixture
def config_path(tmp_path):
    bare, _commit = make_bare_repository(tmp_path)
    path = write_config(tmp_path, bare)
    value = json.loads(path.read_text(encoding="utf-8"))
    value["listen"]["port"] = free_port()
    path.write_text(json.dumps(value), encoding="utf-8")
    return path


def test_gateway_listens_before_warming_and_survives_an_early_invalidation(
    config_path, monkeypatch
):
    release_prewarm = threading.Event()
    prewarm_started = threading.Event()
    observed = {}
    original_init = gateway.GatewayApplication.__init__

    def init_then_signal(self, *args, **kwargs):
        original_init(self, *args, **kwargs)
        os.kill(os.getpid(), signal.SIGUSR1)
        os.kill(os.getpid(), signal.SIGUSR1)

    def slow_prewarm(self):
        prewarm_started.set()
        release_prewarm.wait(10)

    def serve_forever(self, poll_interval=0.5):
        observed["prewarm_started"] = prewarm_started.wait(5)
        observed["prewarm_running"] = self._prewarm_thread is not None
        with socket.create_connection(self.server_address, timeout=2):
            observed["accepting"] = True
        observed["invalidation"] = (
            self._cache_invalidation_requested.is_set())
        observed["usr1_handler"] = signal.getsignal(signal.SIGUSR1)
        release_prewarm.set()
        self.wait_for_prewarm(timeout=5)

    monkeypatch.setattr(gateway.GatewayApplication, "__init__", init_then_signal)
    monkeypatch.setattr(
        gateway.GatewayApplication, "prewarm_memory_caches", slow_prewarm)
    monkeypatch.setattr(gateway.MirrorGatewayServer, "serve_forever", serve_forever)
    previous = signal.getsignal(signal.SIGUSR1)

    assert gateway.main(["--config", str(config_path)]) == 0

    assert observed["prewarm_started"] is True
    assert observed["prewarm_running"] is True
    assert observed["accepting"] is True
    assert observed["invalidation"] is True
    assert callable(observed["usr1_handler"])
    assert signal.getsignal(signal.SIGUSR1) == previous
