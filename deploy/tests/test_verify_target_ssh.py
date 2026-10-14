from types import SimpleNamespace

import pytest

import next_fleet
from next_common import GuardError

FLEET = {"domain": "next.example.test"}


class FakeSsh:
    def __init__(self, pinned=True, stdout=b"n1.next.example.test\nfmnext-node\n", code=0):
        self._pinned = pinned
        self.stdout = stdout
        self.code = code
        self.calls = []

    def pinned(self, host):
        return self._pinned

    def run_strict(self, host, script, timeout=60):
        self.calls.append(host)
        return SimpleNamespace(returncode=self.code, stdout=self.stdout, stderr=b"")


class NoVultr:
    def find_by_label(self, label):
        raise AssertionError("the provider API must not be called for a pinned node")


def test_pinned_node_is_verified_over_ssh_without_the_provider():
    ssh = FakeSsh()
    assert next_fleet.verify_target(NoVultr(), ssh, FLEET, "n1", "203.0.113.11") is None
    assert ssh.calls == ["203.0.113.11"]


def test_wrong_hostname_is_refused():
    ssh = FakeSsh(stdout=b"n2.next.example.test\nfmnext-node\n")
    with pytest.raises(GuardError):
        next_fleet.verify_target(NoVultr(), ssh, FLEET, "n1", "203.0.113.11")


def test_runner_answering_with_its_instance_label_is_accepted():
    ssh = FakeSsh(stdout=b"fmnext-run-1\nfmnext-node\n")
    assert next_fleet.verify_target(NoVultr(), ssh, FLEET, "r1", "203.0.113.21") is None


def test_another_instance_label_is_refused():
    ssh = FakeSsh(stdout=b"fmnext-run-2\nfmnext-node\n")
    with pytest.raises(GuardError):
        next_fleet.verify_target(NoVultr(), ssh, FLEET, "r1", "203.0.113.21")


def test_missing_fleet_marker_falls_back_to_the_provider(monkeypatch):
    seen = []
    monkeypatch.setattr(next_fleet, "verify_target_instance",
                        lambda vultr, name, ip: seen.append((name, ip)) or {"ok": 1})
    ssh = FakeSsh(stdout=b"fmnext-core-4\n")
    assert next_fleet.verify_target(object(), ssh, FLEET, "n4", "203.0.113.14") == {"ok": 1}
    assert seen == [("n4", "203.0.113.14")]


def test_missing_fleet_marker_with_a_wrong_hostname_is_refused():
    ssh = FakeSsh(stdout=b"somewhere-else\n")
    with pytest.raises(GuardError):
        next_fleet.verify_target(NoVultr(), ssh, FLEET, "n1", "203.0.113.11")


def test_host_key_mismatch_is_refused():
    ssh = FakeSsh(code=255, stdout=b"")
    with pytest.raises(GuardError):
        next_fleet.verify_target(NoVultr(), ssh, FLEET, "n1", "203.0.113.11")


def test_unpinned_node_falls_back_to_the_provider(monkeypatch):
    seen = []
    monkeypatch.setattr(next_fleet, "verify_target_instance",
                        lambda vultr, name, ip: seen.append((name, ip)) or {"ok": 1})
    assert next_fleet.verify_target(object(), FakeSsh(pinned=False), FLEET, "n4",
                                    "203.0.113.14") == {"ok": 1}
    assert seen == [("n4", "203.0.113.14")]
