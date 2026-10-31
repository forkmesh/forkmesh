import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "forkmesh-apt-repo.py"
DEB_SH = ROOT / "tools" / "package" / "deb.sh"
APT_FILES = ROOT / "tools" / "package" / "apt"

spec = importlib.util.spec_from_file_location("forkmesh_apt_repo", TOOL)
apt = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = apt
spec.loader.exec_module(apt)

HAVE_GPG = all(shutil.which(name) for name in ("gpg", "gpgv", "gpgconf"))
HAVE_DPKG = all(shutil.which(name) for name in ("dpkg-deb", "dpkg", "readelf", "objdump"))
needs_gpg = pytest.mark.skipif(not HAVE_GPG, reason="gpg, gpgv and gpgconf are required")
needs_dpkg = pytest.mark.skipif(not HAVE_DPKG, reason="dpkg-deb and binutils are required")

CONTROL = """Package: forkmesh
Version: 1.5.0+2
Architecture: amd64
Maintainer: ForkMesh <founders@forkmesh.com>
Depends: libc6 (>= 2.35)
Description: collaborate on Git repositories
 Second line of the description.
"""


def sandbox_env(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "GNUPGHOME")}
    home = tmp_path / "home"
    for sub in ("", ".config", ".local/share", ".cache"):
        (home / sub).mkdir(parents=True, exist_ok=True)
    env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
               XDG_DATA_HOME=str(home / ".local/share"),
               XDG_CACHE_HOME=str(home / ".cache"), TMPDIR=str(tmp_path))
    return env


def tiny_deb(tmp_path, version="1.5.0", control=None):
    root = tmp_path / f"pkg-{version}"
    (root / "DEBIAN").mkdir(parents=True)
    (root / "usr/bin").mkdir(parents=True)
    (root / "usr/bin/forkmesh").write_text("#!/bin/sh\necho ForkMesh\n")
    (root / "DEBIAN/control").write_text(
        control or CONTROL.replace("1.5.0+2", version))
    deb = tmp_path / f"forkmesh_{version}.deb"
    subprocess.run(["dpkg-deb", "--root-owner-group", "-Zxz", "--build", str(root), str(deb)],
                   check=True, capture_output=True)
    return deb


@pytest.fixture(scope="module")
def archive_key(tmp_path_factory):
    if not HAVE_GPG:
        pytest.skip("gpg is required")
    keys = tmp_path_factory.mktemp("keys")
    apt.init_key(keys / "secret.asc", keys / "public.asc")
    return keys / "secret.asc", keys / "public.asc"


def test_parse_control_keeps_continuation_lines_and_checks_identity():
    fields = apt.parse_control(CONTROL)
    assert fields["Version"] == "1.5.0+2"
    assert fields["Description"] == ("collaborate on Git repositories\n"
                                     " Second line of the description.")
    for bad in (CONTROL.replace("Package: forkmesh", "Package: other"),
                CONTROL.replace("1.5.0+2", "1.5"),
                CONTROL.replace("amd64", "AMD 64"),
                CONTROL.replace("Description: collaborate on Git repositories\n", "")):
        with pytest.raises(apt.AptError):
            apt.parse_control(bad)


def test_packages_stanza_points_into_the_hash_addressed_pool():
    digest = "ab" * 32
    stanza = apt.packages_stanza(apt.parse_control(CONTROL), digest, 1234)
    assert f"Filename: pool/sha256/{digest}/forkmesh_1.5.0+2_amd64.deb\n" in stanza
    assert "Size: 1234\n" in stanza and f"SHA256: {digest}\n" in stanza
    assert stanza.index("Depends:") < stanza.index("Filename:") < stanza.index("Description:")
    assert stanza.endswith(" Second line of the description.\n")
    with pytest.raises(apt.AptError):
        apt.packages_stanza(apt.parse_control(CONTROL), "not-a-hash", 1)


def test_release_lists_every_index_with_its_sha256_and_size():
    files = apt.repository_files([(apt.parse_control(CONTROL), "cd" * 32, 9)], 0)
    packages = files["dists/stable/main/binary-amd64/Packages"]
    release = files["dists/stable/Release"].decode()
    assert "Suite: stable\n" in release and "Architectures: amd64\n" in release
    assert "Date: Thu, 01 Jan 1970 00:00:00 GMT\n" in release
    assert (f" {hashlib.sha256(packages).hexdigest()} {len(packages):>16} "
            "main/binary-amd64/Packages\n") in release


def test_shipped_sources_file_matches_the_route_and_keyring():
    sources = (APT_FILES / "forkmesh.sources").read_text()
    assert "URIs: https://app.forkmesh.com/apt\n" in sources
    assert "Suites: stable\n" in sources and "Components: main\n" in sources
    assert "Signed-By: /usr/share/keyrings/forkmesh-archive-keyring.asc\n" in sources
    keyring = (APT_FILES / "forkmesh-archive-keyring.asc").read_text()
    assert keyring.startswith("-----BEGIN PGP PUBLIC KEY BLOCK-----")
    assert "PRIVATE KEY" not in keyring


@needs_gpg
def test_init_key_never_replaces_an_existing_key(archive_key, tmp_path):
    secret, _ = archive_key
    assert oct(secret.stat().st_mode & 0o777) == "0o600"
    with pytest.raises(apt.AptError):
        apt.init_key(secret, tmp_path / "public.asc")


@needs_gpg
@needs_dpkg
def test_publish_signs_metadata_that_verifies_and_detects_tampering(archive_key, tmp_path):
    secret, public = archive_key
    deb = tiny_deb(tmp_path)
    out = tmp_path / "repo"
    digest = apt.sha256_file(deb)
    apt.publish([deb], out, secret, public, APT_FILES / "forkmesh.sources", [digest])
    dists = out / "dists/stable"
    assert (dists / "InRelease").read_text().startswith("-----BEGIN PGP SIGNED MESSAGE-----")
    assert "Hash: SHA512" in (dists / "InRelease").read_text()
    packages = (dists / "main/binary-amd64/Packages").read_text()
    assert f"Filename: pool/sha256/{digest}/forkmesh_1.5.0_amd64.deb" in packages
    assert (out / "forkmesh-archive-keyring.asc").read_bytes() == public.read_bytes()
    apt.verify_signatures(out, public)
    (dists / "main/binary-amd64/Packages").write_text(packages.replace("1.5.0", "9.9.9"))
    with pytest.raises(apt.AptError, match="does not match"):
        apt.verify_signatures(out, public)


@needs_gpg
@needs_dpkg
def test_publish_refuses_a_key_other_than_the_published_one(archive_key, tmp_path):
    secret, _ = archive_key
    other_secret, other_public = tmp_path / "other.asc", tmp_path / "other.pub.asc"
    apt.init_key(other_secret, other_public)
    deb = tiny_deb(tmp_path)
    with pytest.raises(apt.AptError, match="not the published key"):
        apt.publish([deb], tmp_path / "repo", secret, other_public,
                    APT_FILES / "forkmesh.sources")
    with pytest.raises(apt.AptError, match="does not hash"):
        apt.publish([deb], tmp_path / "repo", secret, other_public,
                    APT_FILES / "forkmesh.sources", ["0" * 64])


@needs_dpkg
def test_deb_sh_packages_the_app_with_apt_source_and_library_depends(tmp_path):
    install = tmp_path / "install"
    (install / "bin").mkdir(parents=True)
    (install / "share/forkmesh/tools").mkdir(parents=True)
    (install / "share/forkmesh/tools/cloudflare_bootstrap.py").write_text("")
    binary = install / "bin/forkmesh"
    shutil.copy2(shutil.which("true"), binary)
    env = sandbox_env(tmp_path)
    env.update(FORKMESH_PKG_VERSION="1.5.0", FORKMESH_PKG_BUILD="3",
               FORKMESH_PKG_INSTALL_ROOT=str(install))
    result = subprocess.run(["bash", str(DEB_SH), str(binary), str(tmp_path / "out")],
                            env=env, capture_output=True, text=True, check=False)
    assert result.returncode == 0, result.stderr
    deb = Path(result.stdout.strip().splitlines()[-1])
    assert deb.name == "forkmesh-linux-x86_64.deb"
    field = lambda name: subprocess.run(  # noqa: E731
        ["dpkg-deb", "-f", str(deb), name], capture_output=True, text=True,
        check=True).stdout.strip()
    assert field("Version") == "1.5.0+3"
    assert field("Architecture") == "amd64"
    assert field("Depends").startswith("libc6 (>= 2.")
    listing = subprocess.run(["dpkg-deb", "-c", str(deb)], capture_output=True,
                             text=True, check=True).stdout
    for path in ("./usr/bin/forkmesh", "./usr/share/forkmesh/tools/cloudflare_bootstrap.py",
                 "./usr/share/applications/forkmesh.desktop",
                 "./usr/share/keyrings/forkmesh-archive-keyring.asc",
                 "./etc/apt/sources.list.d/forkmesh.sources"):
        assert path in listing
    conffiles = subprocess.run(["dpkg-deb", "-I", str(deb), "conffiles"],
                               capture_output=True, text=True, check=True).stdout
    assert conffiles == "/etc/apt/sources.list.d/forkmesh.sources\n"
    assert list(tmp_path.glob("forkmesh-deb.*")) == []
    if HAVE_GPG:
        assert apt.parse_control(apt.read_control(deb))["Version"] == "1.5.0+3"


@needs_dpkg
def test_deb_sh_refuses_a_version_that_is_not_x_y_z(tmp_path):
    env = sandbox_env(tmp_path)
    env.update(FORKMESH_PKG_VERSION="1.5", FORKMESH_PKG_INSTALL_ROOT=str(tmp_path))
    result = subprocess.run(["bash", str(DEB_SH), shutil.which("true"), str(tmp_path / "out")],
                            env=env, capture_output=True, text=True, check=False)
    assert result.returncode != 0
    assert "is not X.Y.Z" in result.stderr
