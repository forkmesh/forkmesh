from __future__ import annotations

from types import SimpleNamespace

from forkmesh_host import dns_reconciler, dns_rules


def runtime(names):
    environ = {"FORKMESH_DNS_RECONCILER": "dry",
               "FORKMESH_DNS_TARGETS": "n3=192.0.2.3,n4=192.0.2.4",
               "FORKMESH_DNS_NAMES": names}
    return SimpleNamespace(
        config=SimpleNamespace(environ=environ),
        durable=SimpleNamespace(membership=SimpleNamespace(node_id="n3")),
        env=SimpleNamespace(), sealed=False)


def test_env_from_before_the_next_names_retired_still_starts_the_app(capsys):
    reconciler = dns_reconciler.from_runtime(
        runtime("app.next.forkmesh.com,world.next.forkmesh.com"))
    assert reconciler.shared_names == ["app.forkmesh.com", "world.forkmesh.com"]
    assert "app.next.forkmesh.com" in capsys.readouterr().out


def test_listed_production_names_are_kept_app_first():
    reconciler = dns_reconciler.from_runtime(
        runtime("world.forkmesh.com,app.forkmesh.com"))
    assert reconciler.shared_names == ["app.forkmesh.com", "world.forkmesh.com"]


def test_a_mixed_list_keeps_only_what_the_reconciler_may_manage():
    reconciler = dns_reconciler.from_runtime(
        runtime("app.forkmesh.com,n3.next.forkmesh.com"))
    assert reconciler.shared_names == ["app.forkmesh.com"]
    assert all(name in dns_rules.RECONCILER_NAMES for name in reconciler.shared_names)
