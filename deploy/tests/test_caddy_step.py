import base64
import hashlib
import io
import json
import re
import subprocess
import tarfile
from pathlib import Path

import pytest

pytest.importorskip("cryptography")

import next_fleet  # noqa: E402
import next_ids  # noqa: E402
import render  # noqa: E402
from fakes import FakeSsh  # noqa: E402
from next_common import GuardError, Runner, ToolError  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
MASTER = "ids-master-SECRET-" + "m" * 30
BINARY = b"\x7fELF fake caddy binary"


def quiet(execute=True):
    return Runner(execute=execute, out=io.StringIO())


def make_tarball(home, version="2.11.4", binary=BINARY):
    dl = home / "projects" / "forkmesh" / "data" / "fleet" / "dl"
    dl.mkdir(parents=True, exist_ok=True)
    path = dl / ("caddy_%s_linux_amd64.tar.gz" % version)
    with tarfile.open(path, "w:gz") as archive:
        for name, data in (("caddy", binary), ("LICENSE", b"Apache"),
                           ("README.md", b"readme")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o755
            archive.addfile(info, io.BytesIO(data))
    return path, hashlib.sha256(path.read_bytes()).hexdigest()


def fleet(tmp_path, sha=None):
    data = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    if sha is not None:
        data["caddy"]["sha256"] = sha
    return data


def with_master(sandbox):
    (sandbox / "secrets.env").write_text(
        "RQLITE_PASSWORD=rq-SECRET\nFMNEXT_IDS_MASTER=%s\n" % MASTER)


def ok_expect(record):
    def answer(host, script, stdin):
        record.append(json.loads(stdin))
        return subprocess.CompletedProcess(["ssh"], 0, b'{"ok": true}', b"")
    return (lambda h, s: "fmnext-ids expect" in s, answer)


def home(sandbox):
    return Path(sandbox).parent / "home"


# --------------------------------------------------------------------------
# settings and rendering


def test_fleet_pins_a_version_and_tarball_sha(tmp_path):
    settings = render.caddy_settings(fleet(tmp_path))
    assert re.fullmatch(r"2\.[0-9]+\.[0-9]+", settings["version"])
    assert re.fullmatch(r"[0-9a-f]{64}", settings["sha256"])
    assert settings["tarball"] == "caddy_%s_linux_amd64.tar.gz" % settings["version"]
    real = tomllib_load(REPO / "deploy" / "fleet.toml")["caddy"]
    assert real == {"version": settings["version"], "sha256": settings["sha256"]}


def tomllib_load(path):
    import tomllib
    return tomllib.loads(path.read_text())


@pytest.mark.parametrize("caddy", [
    {"version": "latest", "sha256": "a" * 64},
    {"version": "2.11.4; rm -rf /", "sha256": "a" * 64},
    {"version": "2.11.4", "sha256": "A" * 64},
    {"version": "2.11.4", "sha256": "a" * 63},
    {"version": "2.11.4"},
])
def test_bad_caddy_settings_are_refused(tmp_path, caddy):
    data = fleet(tmp_path)
    data["caddy"] = caddy
    with pytest.raises(render.RenderError):
        render.caddy_settings(data)


def test_no_caddy_table_means_debians_caddy(tmp_path):
    data = fleet(tmp_path)
    del data["caddy"]
    assert render.caddy_settings(data) is None
    with pytest.raises(render.RenderError):
        render.caddy_binary_dropin(data)
    with pytest.raises(ToolError):
        next_fleet.caddy_release(data)


def test_binary_dropin_replaces_start_and_reload_only(tmp_path):
    dropin = render.caddy_binary_dropin(fleet(tmp_path))
    lines = [line for line in dropin.splitlines() if line and not line.startswith("#")]
    assert lines == [
        "[Service]",
        "ExecStart=",
        "ExecStart=/usr/local/bin/caddy run --environ --config /etc/caddy/Caddyfile",
        "ExecReload=",
        "ExecReload=/usr/local/bin/caddy reload --config /etc/caddy/Caddyfile --force",
    ]
    assert render.CADDY_BINARY_DROPIN.startswith(
        "/etc/systemd/system/caddy.service.d/")
    files, _ = render.render_core(fleet(tmp_path), "n1")
    assert render.CADDY_BINARY_DROPIN not in files


def test_ids_accepts_either_caddy_binary_for_the_caddy_user(tmp_path):
    config = json.loads(render.ids_config(fleet(tmp_path), "n1", "core",
                                          render.fleet_paths(fleet(tmp_path))))
    caddy = config["processes"]["service_users"]["caddy"]
    assert caddy["exe"] == ["/usr/local/bin/caddy", "/usr/bin/caddy"]
    assert "/usr/local/bin" in config["integrity"]["lockdown"]


# --------------------------------------------------------------------------
# the local tarball


def test_caddy_release_checks_the_tarball_and_hashes_the_binary(tmp_path, sandbox):
    _, sha = make_tarball(home(sandbox))
    settings, path, binary_sha = next_fleet.caddy_release(fleet(tmp_path, sha))
    assert path.name == settings["tarball"]
    assert binary_sha == hashlib.sha256(BINARY).hexdigest()
    with pytest.raises(GuardError, match="sha256"):
        next_fleet.caddy_release(fleet(tmp_path, "0" * 64))


def test_missing_tarball_is_an_error_only_when_executing(tmp_path, sandbox):
    data = fleet(tmp_path)
    with pytest.raises(ToolError, match="missing"):
        next_fleet.caddy_release(data)
    settings, _, binary_sha = next_fleet.caddy_release(data, required=False)
    assert binary_sha is None and settings["version"]


def test_tarball_whose_caddy_is_not_a_file_is_refused(tmp_path, sandbox):
    dl = home(sandbox) / "projects" / "forkmesh" / "data" / "fleet" / "dl"
    dl.mkdir(parents=True)
    path = dl / "caddy_2.11.4_linux_amd64.tar.gz"
    with tarfile.open(path, "w:gz") as archive:
        info = tarfile.TarInfo("caddy")
        info.type = tarfile.SYMTYPE
        info.linkname = "/bin/sh"
        archive.addfile(info)
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    with pytest.raises(GuardError, match="regular file"):
        next_fleet.caddy_release(fleet(tmp_path, sha))


# --------------------------------------------------------------------------
# the node script


def script_for(tmp_path):
    data = fleet(tmp_path)
    settings = render.caddy_settings(data)
    return next_fleet.caddy_script(settings, "b" * 64,
                                   render.caddy_binary_dropin(data),
                                   "n1.next.forkmesh.com"), settings, data


def position(script, text):
    assert text in script, text
    return script.index(text)


def test_caddy_script_verifies_and_validates_before_it_changes_anything(tmp_path):
    script, settings, data = script_for(tmp_path)
    tar_sha = position(script, "echo %s'  '\"$tarball\" | sha256sum -c -"
                       % settings["sha256"])
    extract = position(script, 'tar -xzf "$tarball" -C "$stage" caddy')
    bin_sha = position(script, "echo %s'  '\"$stage/caddy\" | sha256sum -c -" % ("b" * 64))
    version = position(script, "grep -q '^v%s '" % settings["version"])
    validate = position(script, '"$stage/caddy" validate --config /etc/caddy/Caddyfile')
    installed = position(script, "installed=1\n")
    install = position(script, 'install -m 0755 -o root -g root "$stage/caddy" /usr/local/bin/caddy')
    dropin = position(script, render.CADDY_BINARY_DROPIN)
    restart = position(script, "systemctl restart caddy\ncaddy_serving")
    assert tar_sha < extract < bin_sha < version < validate < installed < install < dropin < restart
    assert "set -eu" in script
    assert "mktemp -d /var/lib/fmnext-caddy." in script
    assert "/tmp/fmnext-caddy" not in script
    assert 'HOME="$stage/home"' in script
    encoded = re.search(r"echo '?([A-Za-z0-9+/=]+)'? \| base64 -d", script).group(1)
    assert base64.b64decode(encoded).decode() == render.caddy_binary_dropin(data)
    assert '[ "$exe" = /usr/local/bin/caddy ]' in script
    assert "/usr/bin/caddy" not in script.replace("/usr/bin/caddy.", "")


def test_caddy_script_asks_for_a_restore_only_after_installing(tmp_path):
    script, _, _ = script_for(tmp_path)
    body = script[script.index("set -eu"):]
    fake = tmp_path / "bin"
    fake.mkdir()
    for name in ("systemctl", "curl", "install", "tar", "sha256sum", "base64",
                 "readlink", "chmod"):
        (fake / name).write_text("#!/bin/sh\nexit 0\n")
        (fake / name).chmod(0o755)
    (fake / "mktemp").write_text("#!/bin/sh\necho %s\n" % (tmp_path / "stage"))
    (fake / "mktemp").chmod(0o755)
    (tmp_path / "stage").mkdir()
    (tmp_path / "stage" / "caddy").write_text("#!/bin/sh\necho 'v0.0.1 h1:x'\n")
    (tmp_path / "stage" / "caddy").chmod(0o755)
    env = {"PATH": "%s:/usr/bin:/bin" % fake}
    done = subprocess.run(["bash", "-c", body], env=env, capture_output=True,
                          text=True, timeout=30)
    assert done.returncode == 5
    assert "restore-needed" not in done.stdout
    assert not (tmp_path / "stage").exists()


def test_debian_restore_removes_the_override_before_the_binary():
    script = next_fleet.caddy_debian_script("n1.next.forkmesh.com")
    dropin = position(script, "rm -f %s" % render.CADDY_BINARY_DROPIN)
    reload = position(script, "systemctl daemon-reload")
    restart = position(script, "systemctl restart caddy\ncaddy_serving")
    binary = position(script, "rm -f /usr/local/bin/caddy")
    assert dropin < reload < restart < binary
    assert "/usr/bin/caddy version" in script


# --------------------------------------------------------------------------
# install flow and IDS


def test_caddy_step_tells_the_ids_the_exact_binary_and_dropin(tmp_path, sandbox,
                                                              monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance", lambda *a: None)
    with_master(sandbox)
    tarball, sha = make_tarball(home(sandbox))
    data = fleet(tmp_path, sha)
    sent = []
    ssh = FakeSsh([ok_expect(sent)])
    next_fleet.install(data, "n1", ssh, quiet(), ("caddy",), vultr=None)
    manifest = sent[0]
    assert manifest["files"] == {
        "/usr/local/bin/caddy": hashlib.sha256(BINARY).hexdigest(),
        render.CADDY_BINARY_DROPIN: hashlib.sha256(
            render.caddy_binary_dropin(data).encode()).hexdigest()}
    assert manifest["rescan"] == []
    assert manifest["allow"] == ["listeners", "processes"]
    assert ssh.puts == [("203.0.113.11", str(tarball), "/tmp/" + tarball.name)]
    assert "fmnext-ids expect" in ssh.runs[0][1]
    assert "sha256sum -c -" in ssh.runs[1][1]
    assert len(ssh.runs) == 2


def test_caddy_step_dry_run_sends_nothing_and_needs_no_tarball(tmp_path, sandbox):
    with_master(sandbox)
    ssh = FakeSsh()
    runner = quiet(execute=False)
    next_fleet.install(fleet(tmp_path), "n1", ssh, runner, ("caddy",))
    assert ssh.runs == [] and ssh.puts == []
    assert len(runner.skipped) == 2


def test_failed_caddy_after_install_restores_debians(tmp_path, sandbox,
                                                     monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance", lambda *a: None)
    with_master(sandbox)
    _, sha = make_tarball(home(sandbox))
    sent = []
    failed = (lambda h, s: "installed=1" in s, subprocess.CompletedProcess(
        ["ssh"], 7, b"fmnext-caddy: restore-needed\n",
        b"fmnext-caddy: caddy is not serving on v2.11.4\n"))
    restored = (lambda h, s: "rm -f /usr/local/bin/caddy" in s,
                subprocess.CompletedProcess(
                    ["ssh"], 0, b"fmnext-caddy: back on Debian's v2.6.2\n", b""))
    ssh = FakeSsh([ok_expect(sent), failed, restored])
    with pytest.raises(ToolError, match="not installed"):
        next_fleet.install(fleet(tmp_path, sha), "n1", ssh, quiet(), ("caddy",),
                           vultr=None)
    assert sent[1]["files"] == {"/usr/local/bin/caddy": None,
                                render.CADDY_BINARY_DROPIN: None}
    assert "rm -f /usr/local/bin/caddy" in ssh.runs[-1][1]


def test_failed_caddy_before_install_restores_nothing(tmp_path, sandbox,
                                                      monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance", lambda *a: None)
    with_master(sandbox)
    _, sha = make_tarball(home(sandbox))
    sent = []
    failed = (lambda h, s: "installed=1" in s, subprocess.CompletedProcess(
        ["ssh"], 6, b"", b"fmnext-caddy: /etc/caddy/Caddyfile does not validate\n"))
    ssh = FakeSsh([ok_expect(sent), failed])
    with pytest.raises(ToolError, match="does not validate"):
        next_fleet.install(fleet(tmp_path, sha), "n1", ssh, quiet(), ("caddy",),
                           vultr=None)
    assert len(sent) == 1 and len(ssh.runs) == 2


def test_caddy_debian_step_expects_both_files_to_go(tmp_path, sandbox, monkeypatch):
    monkeypatch.setattr(next_fleet, "verify_target_instance", lambda *a: None)
    with_master(sandbox)
    sent = []
    ssh = FakeSsh([ok_expect(sent)])
    next_fleet.install(fleet(tmp_path), "n2", ssh, quiet(), ("caddy-debian",),
                       vultr=None)
    assert sent[0]["files"] == {"/usr/local/bin/caddy": None,
                                render.CADDY_BINARY_DROPIN: None}
    assert "rm -f /usr/local/bin/caddy" in ssh.runs[-1][1]


def test_caddy_is_an_install_step_after_files():
    steps = next_fleet.INSTALL_STEPS
    assert steps.index("files") < steps.index("caddy") < steps.index("start")
    assert "caddy-debian" in next_fleet.ALL_STEPS
    assert "caddy-debian" not in steps
    assert "caddy" not in next_fleet.RUNNER_STEPS


@pytest.mark.parametrize("step", ["caddy", "caddy-debian"])
def test_caddy_steps_refuse_runners(tmp_path, sandbox, step, capsys):
    assert next_fleet.main(["--fleet", str(FLEET), "install", "r1", "--steps",
                            step]) != 0
    assert "for core nodes only" in capsys.readouterr().err


# --------------------------------------------------------------------------
# the files step keeps the release's routes


def test_files_step_reinstalls_the_current_releases_routes(tmp_path, sandbox):
    files, manifest = render.render_core(fleet(tmp_path), "n1")
    script = next_fleet.files_script(manifest, "/tmp/fmnext-files.tar.gz")
    for site in ("app", "world"):
        target = "/etc/forkmesh-next/caddy/%s-routes.caddy" % site
        source = "/opt/forkmesh-next/app/current/caddy/%s-routes.caddy" % site
        assert manifest["release_routes"][target] == source
        rendered = script.index('install -D -m 0644 "$stage"%s %s' % (target, target))
        release = script.index("if [ -s %s ]; then install -m 0644 %s %s; fi"
                               % (source, source, target))
        assert rendered < release < script.index("fmnext-certsync prepare")
    expected, rescan = next_ids.files_expectation(fleet(tmp_path), "n1")
    for target in manifest["release_routes"]:
        assert target not in expected and target in rescan
