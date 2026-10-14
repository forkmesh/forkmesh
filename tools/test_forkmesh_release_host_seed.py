import hashlib
import importlib.util
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SPEC = importlib.util.spec_from_file_location(
    "forkmesh_release_host", HERE / "forkmesh-release-host.py")
release_host = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release_host)


def test_seed_targets_pin_known_hosts_only_when_given(tmp_path):
    mirrors = release_host.seed_targets([("m1", "10.0.0.1")], tmp_path / "vultr")
    assert mirrors == [("m1", "10.0.0.1",
                        ["-i", str(tmp_path / "vultr"), "-o", "IdentitiesOnly=yes"])]
    cores = release_host.seed_targets(
        [("n1", "10.0.0.2")], tmp_path / "fleet", tmp_path / "known_hosts")
    assert cores[0][2] == [
        "-i", str(tmp_path / "fleet"), "-o", "IdentitiesOnly=yes",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "UserKnownHostsFile=%s" % (tmp_path / "known_hosts")]


def test_seed_mirrors_uses_each_hosts_own_key(tmp_path, monkeypatch):
    data = b"release"
    digest = hashlib.sha256(data).hexdigest()
    blob = tmp_path / "cas" / "sha256" / digest[:2] / digest
    blob.mkdir(parents=True)
    (blob / "data").write_bytes(data)
    calls = []

    def run(command, **_kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, "present", "")

    monkeypatch.setattr(release_host.subprocess, "run", run)
    hosts = (release_host.seed_targets([("m1", "10.0.0.1")], tmp_path / "vultr")
             + release_host.seed_targets([("n1", "10.0.0.2")], tmp_path / "fleet",
                                         tmp_path / "kh"))
    release_host.seed_mirrors(tmp_path / "cas", [digest], hosts, "/srv/repo.git")
    by_host = {command[-2]: command for command in calls}
    assert str(tmp_path / "vultr") in by_host["root@10.0.0.1"]
    assert str(tmp_path / "fleet") in by_host["root@10.0.0.2"]
    assert "UserKnownHostsFile=%s" % (tmp_path / "kh") in by_host["root@10.0.0.2"]
    assert "BASE=/srv/repo.git/forkmesh-releases" in by_host["root@10.0.0.2"][-1]


def test_seed_hosts_follow_the_fleet_and_keep_other_lines():
    text = ("# cores\nfmnext-n1 192.0.2.1\nfmnext-n2 192.0.2.2\n"
            "mirror9 198.51.100.9\n")
    cores = [("fmnext-n2", "192.0.2.2"), ("fmnext-n3", "192.0.2.3"),
             ("fmnext-n4", "192.0.2.4")]
    assert release_host.seed_hosts(text, cores) == cores + [("mirror9", "198.51.100.9")]
    assert release_host.seed_hosts(text, []) == [
        ("fmnext-n1", "192.0.2.1"), ("fmnext-n2", "192.0.2.2"),
        ("mirror9", "198.51.100.9")]
    assert release_host.seed_hosts("", cores) == cores
