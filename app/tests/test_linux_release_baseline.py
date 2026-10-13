import importlib.util
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / "tools" / "package" / "linux-baseline"
DRIVER = ROOT / "tools" / "package" / "linux-baseline.sh"
CHECK = RECIPE / "check_glibc.py"


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


check = _load("forkmesh_check_glibc", CHECK)
rootfs = _load("forkmesh_oci_rootfs", RECIPE / "oci_rootfs.py")

READELF = """
Version symbols section '.gnu.version' contains 3 entries:
  000:   0 (*local*)       2 (GLIBC_2.99)

Version needs section '.gnu.version_r' contains 2 entries:
 Addr: 0x0000000000000000  Offset: 0x000500  Link: 7 (.dynstr)
  000000: Version: 1  File: libstdc++.so.6  Cnt: 2
  0x0010:   Name: GLIBCXX_3.4.30  Flags: none  Version: 6
  0x0020:   Name: CXXABI_1.3.13  Flags: none  Version: 5
  0x0030: Version: 1  File: libc.so.6  Cnt: 3
  0x0040:   Name: GLIBC_2.34  Flags: none  Version: 4
  0x0050:   Name: GLIBC_2.2.5  Flags: none  Version: 3
  0x0060:   Name: GLIBC_2.35  Flags: none  Version: 2
"""


def _env():
    return {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}


def test_needed_versions_reads_only_the_version_needs_section():
    versions = check.needed_versions(READELF)
    assert versions["GLIBC"] == (2, 35)
    assert versions["GLIBCXX"] == (3, 4, 30)
    assert versions["CXXABI"] == (1, 3, 13)


def test_a_newer_glibc_than_the_baseline_is_a_violation():
    report = {"forkmesh-linux-x86_64": {"GLIBC": (2, 38)},
              "forkmesh-linux-x86_64.AppImage!/usr/lib/libz.so.1": {"GLIBC": (2, 36)}}
    limits = {"GLIBC": (2, 35), "GLIBCXX": (3, 4, 30)}
    problems = check.violations(report, limits)
    assert len(problems) == 2
    assert "needs GLIBC_2.38 (baseline GLIBC_2.35)" in problems[0] + problems[1]


def test_the_baseline_itself_passes():
    report = {"a": {"GLIBC": (2, 35), "GLIBCXX": (3, 4, 30)}, "b": {}}
    assert check.violations(report, {"GLIBC": (2, 35), "GLIBCXX": (3, 4, 30)}) == []


@pytest.mark.skipif(not shutil.which("readelf"), reason="needs binutils")
def test_the_checker_fails_a_real_binary_above_the_limit(tmp_path):
    target = Path(shutil.which("ls")).resolve()
    ok = subprocess.run([sys.executable, str(CHECK), "--glibc-max", "99.0",
                         "--glibcxx-max", "99.0", str(target)],
                        capture_output=True, text=True, env=_env(), cwd=tmp_path)
    assert ok.returncode == 0, ok.stderr
    assert "highest GLIBC_2." in ok.stdout
    bad = subprocess.run([sys.executable, str(CHECK), "--glibc-max", "2.0",
                          "--glibcxx-max", "3.4", str(target)],
                         capture_output=True, text=True, env=_env(), cwd=tmp_path)
    assert bad.returncode == 1
    assert "FAIL" in bad.stderr


def test_the_checker_refuses_a_file_that_is_not_elf(tmp_path):
    script = tmp_path / "not-elf"
    script.write_text("#!/bin/sh\n")
    result = subprocess.run([sys.executable, str(CHECK), "--glibc-max", "2.35",
                             "--glibcxx-max", "3.4.30", str(script)],
                            capture_output=True, text=True, env=_env())
    assert result.returncode == 2


def test_the_pins_target_debian_12_and_the_ubuntu_22_04_glibc():
    env = dict(line.split("=", 1) for line in
               (RECIPE / "image.env").read_text().splitlines() if "=" in line)
    assert env["FORKMESH_BASELINE_IMAGE"].startswith("docker.io/library/debian:bookworm")
    assert re.fullmatch(r"sha256:[0-9a-f]{64}", env["FORKMESH_BASELINE_IMAGE_DIGEST"])
    assert env["FORKMESH_BASELINE_GLIBC_MAX"] == "2.35"
    assert env["FORKMESH_BASELINE_QT_VERSION"] == "6.8.2"
    assert env["FORKMESH_BASELINE_OPENSSL_VERSION"] == "3.5.8"
    for key, value in env.items():
        if key.endswith("_SHA256"):
            assert re.fullmatch(r"[0-9a-f]{64}", value), key
    containerfile = (RECIPE / "Containerfile").read_text()
    assert (f"FROM {env['FORKMESH_BASELINE_IMAGE']}@{env['FORKMESH_BASELINE_IMAGE_DIGEST']}"
            in containerfile)


def test_openssl_matches_the_desktop_pin():
    local = (ROOT / "tools" / "openssl-local.sh").read_text()
    env = (RECIPE / "image.env").read_text()
    sha = re.search(r"FORKMESH_OPENSSL_SHA256:-([0-9a-f]{64})", local).group(1)
    assert f"FORKMESH_BASELINE_OPENSSL_SHA256={sha}" in env


def test_every_stage_runs_in_the_containerfile_in_driver_order():
    containerfile = (RECIPE / "Containerfile").read_text()
    driver = DRIVER.read_text()
    order = re.search(r"stages=\(([^)]*)\)", driver).group(1).split()
    positions = [containerfile.index(f"stage-{stage}.sh") for stage in order]
    assert positions == sorted(positions)
    for stage in order:
        assert (RECIPE / f"stage-{stage}.sh").is_file()


def test_the_release_build_refuses_system_qt_and_openssl():
    build = (RECIPE / "build-forkmesh.sh").read_text()
    assert "-DFORKMESH_REQUIRE_OPENSSL_3_5=ON" in build
    assert "-DOPENSSL_USE_STATIC_LIBS=ON" in build
    assert "NEEDED.*(libQt6|libssl|libcrypto|libnghttp3|libwebp|libicu)" in build
    assert "nghttp3_conn_client_new" in build
    driver = DRIVER.read_text()
    assert 'cmd_check "$out/forkmesh-linux-x86_64" "$out/forkmesh-linux-x86_64.AppImage"' in driver
    for image in ("ubuntu:22.04", "ubuntu:24.04", "debian:12"):
        assert f"docker.io/library/{image}" in driver


def test_the_smoke_test_sandboxes_home_and_drops_forkmesh_paths():
    smoke = (RECIPE / "smoke-test.sh").read_text()
    assert "env -i" in smoke
    for name in ("HOME=", "XDG_CONFIG_HOME=", "XDG_DATA_HOME=", "XDG_CACHE_HOME="):
        assert name in smoke
    assert "FORKMESH_HOME" not in smoke
    assert "FORKMESH_PROJECTS_DIR" not in smoke


def test_split_reference_defaults_to_docker_hub_library():
    assert rootfs.split_reference("docker.io/library/ubuntu:22.04") == (
        "registry-1.docker.io", "library/ubuntu", "22.04")
    assert rootfs.split_reference("debian") == ("registry-1.docker.io", "library/debian", "latest")


def test_layer_extraction_skips_escapes_and_honours_whiteouts(tmp_path):
    import io
    import tarfile

    root = tmp_path / "root"
    (root / "etc").mkdir(parents=True)
    (root / "etc" / "gone").write_text("x")
    (root / "opaque").mkdir()
    (root / "opaque" / "old").write_text("x")
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        for name in ("etc/.wh.gone", "opaque/.wh..wh..opq", "../escape", "/abs", "etc/new"):
            info = tarfile.TarInfo(name)
            data = b"new"
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    buffer.seek(0)
    with tarfile.open(fileobj=buffer) as archive:
        archive.extractall(root, members=rootfs.safe_members(archive, root),
                           filter="fully_trusted")
    assert not (root / "etc" / "gone").exists()
    assert not (root / "opaque" / "old").exists()
    assert (root / "etc" / "new").read_text() == "new"
    assert not (tmp_path / "escape").exists()


def test_layer_extraction_never_writes_through_a_symlinked_directory(tmp_path):
    import io
    import tarfile

    root = tmp_path / "root"
    root.mkdir()
    outside = tmp_path / "home"
    outside.mkdir()
    (outside / ".bashrc").write_text("mine")
    secret = tmp_path / "secret"
    secret.write_text("key")
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w") as archive:
        link = tarfile.TarInfo("etc")
        link.type = tarfile.SYMTYPE
        link.linkname = str(outside)
        archive.addfile(link)
        info = tarfile.TarInfo("etc/.bashrc")
        info.size = 6
        archive.addfile(info, io.BytesIO(b"pwned!"))
        hard = tarfile.TarInfo("stolen")
        hard.type = tarfile.LNKTYPE
        hard.linkname = str(secret)
        archive.addfile(hard)
        hard = tarfile.TarInfo("stolen2")
        hard.type = tarfile.LNKTYPE
        hard.linkname = "../secret"
        archive.addfile(hard)
    buffer.seek(0)
    with tarfile.open(fileobj=buffer) as archive:
        archive.extractall(root, members=rootfs.safe_members(archive, root),
                           filter="fully_trusted")
    assert (outside / ".bashrc").read_text() == "mine"
    assert not (root / "stolen").exists()
    assert not (root / "stolen2").exists()
    assert (root / "etc").is_symlink()
