import hashlib
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
OPUS = ROOT / "native" / "opus"
SMOKE = OPUS / "test" / "opus_smoke.c"
OUT = Path(os.environ.get("OPUS_OUT", OPUS / "build" / "out"))
HOST = OUT / "host"
ANDROID = OUT / "android"
NDK_CLANG = (Path.home() / "Android" / "Sdk" / "ndk" / "28.2.13676358" / "toolchains" / "llvm" / "prebuilt"
             / "linux-x86_64" / "bin")
ANDROID_TRIPLES = {"arm64-v8a": "aarch64-linux-android26", "armeabi-v7a": "armv7a-linux-androideabi26",
                   "x86_64": "x86_64-linux-android26"}


def _pin() -> dict[str, str]:
    values = {}
    for line in (OPUS / "PIN").read_text().splitlines():
        key, _, value = line.partition("=")
        values[key] = value.strip('"')
    return values


def test_pin_names_one_release_commit():
    pin = _pin()
    assert pin["OPUS_URL"] == "https://gitlab.xiph.org/xiph/opus.git"
    assert pin["OPUS_TAG"] == "v" + pin["OPUS_VERSION"]
    for key in ("OPUS_TAG_OBJECT", "OPUS_COMMIT", "CMAKE_TAG_OBJECT", "CMAKE_COMMIT"):
        assert re.fullmatch(r"[0-9a-f]{40}", pin[key]), key
    assert re.fullmatch(r"[0-9a-f]{64}", pin["OPUS_TARBALL_SHA256"])
    assert pin["OPUS_TARBALL_URL"].endswith("/opus-%s.tar.gz" % pin["OPUS_VERSION"])


def test_build_turns_on_the_deep_learning_features_with_pinned_weights():
    pin = _pin()
    assert pin["OPUS_FEATURES"].split() == ["deep_plc", "dred", "osce"]
    assert re.fullmatch(r"[0-9a-f]{64}", pin["OPUS_MODEL_SHA256"])
    assert pin["OPUS_MODEL_URL"] == (
        "https://media.xiph.org/opus/models/opus_data-%s.tar.gz" % pin["OPUS_MODEL_SHA256"])
    script = (OPUS / "build.sh").read_text()
    for flag in ("OPUS_DEEP_PLC", "OPUS_DRED", "OPUS_OSCE"):
        assert '-D%s="$(feature ' % flag in script
    assert "-DOPUS_FIXED_POINT=OFF" in script
    assert "OPUS_MODEL_SHA256" in script and "autogen.sh" in script


def _run_smoke(tmp_path: Path, link: list[str], env: dict[str, str] | None = None) -> dict:
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        pytest.skip("no C compiler")
    binary = tmp_path / "opus_smoke"
    subprocess.run([compiler, "-std=c11", "-D_DEFAULT_SOURCE", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-I", str(HOST / "include"), str(SMOKE), *link, "-lm", "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    result = subprocess.run([str(binary)], check=False, capture_output=True, text=True, timeout=60, env=env)
    report = json.loads(result.stdout)
    assert result.returncode == 0, report
    return report


def _check(report: dict) -> None:
    pin = _pin()
    assert report["ok"] is True
    assert report["version"] == "libopus " + pin["OPUS_VERSION"]
    assert report["fec_enabled"] == 1
    assert report["lbrr_packets"] >= report["frames"] // 2
    assert 20_000 <= report["bytes"] * 8 <= 48_000
    assert report["snr_db"] >= 20
    assert report["fec_samples"] == 960
    assert report["fec_snr_db"] >= 10
    assert report["fec_snr_db"] > report["plc_snr_db"]
    assert report["plc_samples"] == 960
    assert report["dtx_packets"] >= report["frames"] // 2
    dred = report["dred"]
    assert dred["duration"] == 100
    assert 0.25 <= dred["lost"] / dred["frames"] <= 0.35
    assert dred["dred_packets"] >= dred["frames"] - 2
    assert dred["max_dred_samples"] >= 5 * 960
    assert dred["recovered"] == dred["lost"]
    assert dred["dred_lsd_db"] + 1 <= dred["plc_lsd_db"]
    assert dred["clean_lsd_db"] < dred["dred_lsd_db"]
    assert dred["osce_bwe_ctl"] == 0


@pytest.mark.skipif(not (HOST / "lib" / "libopus.a").is_file(), reason="native/opus/build.sh host has not run")
def test_host_static_build_encodes_fec_dtx_dred_and_plc(tmp_path):
    _check(_run_smoke(tmp_path, [str(HOST / "lib" / "libopus.a")]))


@pytest.mark.skipif(not (HOST / "lib" / "libopus.so").exists(), reason="native/opus/build.sh host has not run")
def test_host_shared_build_encodes_fec_dtx_dred_and_plc(tmp_path):
    env = dict(os.environ, LD_LIBRARY_PATH=str(HOST / "lib"))
    _check(_run_smoke(tmp_path, ["-L", str(HOST / "lib"), "-lopus"], env))


@pytest.mark.skipif(not (HOST / "SHA256SUMS").is_file(), reason="native/opus/build.sh host has not run")
def test_host_checksums_match_the_outputs():
    for line in (HOST / "SHA256SUMS").read_text().splitlines():
        digest, _, name = line.partition("  ")
        assert hashlib.sha256((HOST / name).read_bytes()).hexdigest() == digest, name


@pytest.mark.skipif(not (ANDROID / "SHA256SUMS").is_file() or not NDK_CLANG.is_dir(),
                    reason="native/opus/build.sh android has not run or the NDK is absent")
@pytest.mark.parametrize("abi", sorted(ANDROID_TRIPLES))
def test_android_libraries_link(tmp_path, abi):
    clang = NDK_CLANG / (ANDROID_TRIPLES[abi] + "-clang")
    for kind, link in (("static", [str(ANDROID / "lib" / abi / "libopus.a")]),
                       ("shared", ["-L", str(ANDROID / "lib" / abi), "-lopus"])):
        result = subprocess.run([str(clang), "-std=c11", "-D_DEFAULT_SOURCE", "-O2", "-I", str(ANDROID / "include"),
                                 str(SMOKE), *link, "-lm", "-o", str(tmp_path / ("smoke-" + kind))],
                                capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
