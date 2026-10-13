#!/usr/bin/env python3
"""Contracts for the clean-machine VM evidence workflow (issue #705)."""

import base64
import importlib.util
import re
import io
import json
import subprocess
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "clean_machine_vm.py"
HARNESS = ROOT / "tools" / "clean_machine_transcript.sh"
WORKFLOW = ROOT / ".forkmesh" / "clean-machine-vm.yml"
EXPECTATIONS = ROOT / "tools" / "clean_machine_expectations.json"

_spec = importlib.util.spec_from_file_location("clean_machine_vm", TOOL)
vm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(vm)


def test_library_sonames_map_to_debian_package_names():
    assert vm.library_package_candidates("libcapstone.so.5") == ["libcapstone5", "libcapstone5t64"]
    assert vm.library_package_candidates("libaio.so.1t64") == ["libaio1", "libaio1t64"]
    assert vm.library_package_candidates("libfoo-2.0.so.0") == ["libfoo-2.0-0", "libfoo-2.0-0t64"]
    assert vm.library_package_candidates("ld-linux-x86-64.so.2") == []

    index = {"libcapstone5": {}, "librdmacm1t64": {}}
    ldd = (
        "\tlibcapstone.so.5 => not found\n"
        "\tlibrdmacm.so.1 => not found\n"
        "\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x0)\n"
        "\tlibmystery.so.9 => not found\n"
    )
    wanted, unresolved = vm.missing_library_packages(ldd, index)
    assert wanted == ["libcapstone5", "librdmacm1t64"]
    assert unresolved == ["libmystery.so.9"]


def test_archive_indexes_are_read_from_the_signed_release():
    release = (
        "Origin: Debian\n"
        "MD5Sum:\n"
        " 00 1 main/binary-amd64/Packages.xz\n"
        "SHA256:\n"
        " abc123 42 main/binary-amd64/Packages.xz\n"
        " def456 7 main/binary-amd64/Release\n"
    )
    assert vm.parse_release_sha256(release) == {
        "main/binary-amd64/Packages.xz": "abc123",
        "main/binary-amd64/Release": "def456",
    }
    packages = vm.parse_packages(
        "Package: seabios\nVersion: 1.16.3-2\nFilename: pool/main/s/seabios/seabios_1.16.3-2_all.deb\n"
        "SHA256: 2b59\nDescription: x\n more\n\n"
        "Package: libfdt1\nVersion: 1.7.2-2+b1\nSHA256: 8e37\n"
    )
    assert packages["seabios"]["Filename"].endswith("seabios_1.16.3-2_all.deb")
    assert packages["seabios"]["Description"] == "x\nmore"
    assert packages["libfdt1"]["SHA256"] == "8e37"


def test_cloud_config_carries_the_checked_out_harness_and_capture_hooks():
    user_data = vm.cloud_config("run", HARNESS.read_text(encoding="utf-8"))
    assert user_data.startswith("#cloud-config\n")
    config = json.loads(user_data.split("\n", 1)[1])
    files = {entry["path"]: entry for entry in config["write_files"]}

    harness = files["/home/tester/clean_machine_transcript.sh"]
    assert base64.b64decode(harness["content"]).decode() == HARNESS.read_text(encoding="utf-8")
    assert harness["defer"] is True

    drive = base64.b64decode(files["/home/tester/drive.sh"]["content"]).decode()
    assert "FORKMESH_TRANSCRIPT_AUTOMATED=1" in drive
    assert "FORKMESH_SCREENSHOT_HOOK=/home/tester/shot.sh" in drive
    assert "/home/tester/clean_machine_transcript.sh run" in drive
    assert "@@OUT-BEGIN" in drive and "sudo tee /dev/ttyS2" in drive
    assert 'echo "@@DONE $rc" | sudo tee /dev/ttyS1' in drive
    assert "systemctl poweroff" in drive

    shot = base64.b64decode(files["/home/tester/shot.sh"]["content"]).decode()
    assert 'echo "@@SHOT $name $focus" | sudo tee /dev/ttyS1' in shot
    # The hook waits for the observer's verdict and never invents one.
    assert "head -n 1 /dev/ttyS1" in shot
    assert "not-observed the host observer did not answer" in shot
    assert "stty -F /dev/ttyS1 raw -echo" in drive

    autologin = files["/etc/systemd/system/getty@tty1.service.d/autologin.conf"]
    assert "--autologin tester" in autologin["content"]
    assert "defer" not in autologin
    for package in ("xserver-xorg-core", "xinit", "openbox", "xterm", "xdotool", "bsdutils"):
        assert package in config["packages"]
    assert ["systemctl", "restart", "getty@tty1.service"] in config["runcmd"]


def test_qemu_command_isolates_the_guest_and_wires_three_serial_ports(tmp_path):
    common = dict(
        qemu="qemu-system-x86_64",
        firmware=[tmp_path],
        overlay=tmp_path / "overlay.qcow2",
        seed=tmp_path / "seed",
        work=tmp_path,
        cpus=4,
        memory_mb=4096,
    )
    kvm = vm.qemu_command(kvm=True, **common)
    tcg = vm.qemu_command(kvm=False, **common)
    joined = " ".join(kvm)
    assert "-accel kvm -accel tcg" in joined
    assert "-accel tcg,thread=multi" in " ".join(tcg) and "kvm" not in " ".join(tcg)
    assert "-display none" in joined and "-no-reboot" in joined
    # The control port is a socket so the observer can answer the guest.
    assert "-chardev socket,id=control" in joined and "server=on,wait=off" in joined
    assert "file.driver=vvfat" in joined and "file.label=cidata" in joined
    assert "-nic user,model=virtio-net-pci" in joined
    serials = [kvm[i + 1] for i, arg in enumerate(kvm) if arg == "-serial"]
    assert serials == ["chardev:console", "chardev:control", "chardev:data"]
    assert f"-L {tmp_path}" in joined
    assert "hostfwd" not in joined and "-virtfs" not in joined  # nothing on the host is shared


def test_markers_and_guest_output_round_trip(tmp_path):
    assert vm.parse_markers("[  3.1] boot\r\n@@SHOT 01-first-launch\r\n@@DONE 0\r\n") == [
        ("SHOT", "01-first-launch"),
        ("DONE", "0"),
    ]

    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w:gz") as archive:
        data = b"== machine ==\n"
        info = tarfile.TarInfo("out/transcript.log")
        info.size = len(data)
        archive.addfile(info, io.BytesIO(data))
    encoded = base64.encodebytes(buffer.getvalue()).decode()
    log = tmp_path / "serial-data.log"
    log.write_text("noise\r\n@@OUT-BEGIN\r\n" + encoded.replace("\n", "\r\n") + "@@OUT-END\r\n")
    assert vm.extract_guest_output(log, tmp_path / "guest")
    assert (tmp_path / "guest" / "out" / "transcript.log").read_bytes() == data
    assert not vm.extract_guest_output(tmp_path / "missing.log", tmp_path / "other")


def test_workflow_is_dispatch_only_and_keeps_artifacts_of_failed_transcripts():
    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "on: [workflow_dispatch]" in workflow
    assert "python3 tools/clean_machine_vm.py run" in workflow
    assert 'dest="$HOME/forkmesh-artifacts/clean-machine/' in workflow
    assert '--provision-dir "$cache/qemu"' in workflow
    assert "FORKMESH_ACTIONS_ALLOW_NETWORK" in workflow
    # A failing transcript must still land its evidence: only a run that
    # captured nothing fails the workflow.
    assert 'if [ ! -f "$dest/summary.json" ]; then' in workflow
    assert "RESULT.txt" in workflow
    # The run log states what the observer read, step by step.
    assert '--ocr-dir "$cache/ocr"' in workflow
    assert "Screen checks:" in workflow


def test_observer_only_calls_a_screen_observed_when_it_read_the_expected_text():
    rules = {"require_all": ["Signature Verified Successfully"], "forbid": ["ABORT"]}
    good = vm.evaluate("...\nSignature  Verified   Successfully\nexit=0\n", rules)
    assert good["verdict"] == "observed"
    assert good["matched"] == ["Signature Verified Successfully"]

    # OCR drops punctuation often enough that the loose pass must still match.
    assert vm.evaluate("sha256|installed forkmesh)", {"require_all": ["sha256(installed forkmesh)"]})["verdict"] == "observed"

    missing = vm.evaluate("nothing useful here", rules)
    assert missing["verdict"] == "not-observed"
    assert "Signature Verified Successfully" in missing["detail"]

    forbidden = vm.evaluate("Signature Verified Successfully\nABORT: no", rules)
    assert forbidden["verdict"] == "not-observed"
    assert "forbidden" in forbidden["detail"]

    any_rules = {"require_any": ["Stop work", "paused"]}
    assert vm.evaluate("the loop is Paused", any_rules)["verdict"] == "observed"
    assert vm.evaluate("something else", any_rules)["verdict"] == "not-observed"

    # No expectation, or no OCR, must never read as observed.
    assert vm.evaluate("anything", None)["verdict"] == "unchecked"
    assert vm.evaluate(None, rules)["verdict"] == "not-observed"


def test_every_expectation_matches_a_step_the_harness_captures():
    rules = {k: v for k, v in json.loads(EXPECTATIONS.read_text()).items() if not k.startswith("_")}
    harness = HARNESS.read_text(encoding="utf-8")
    captured = harness + TOOL.read_text(encoding="utf-8")  # drive.sh asks for the context frames
    for step, rule in rules.items():
        assert step in captured, f"expectation {step} is never captured"
        assert rule.get("require_all") or rule.get("require_any"), step
        assert rule["focus"] in {"terminal", "app"}
    # Phrases the harness itself prints must stay in step with it.
    assert "identity and repositories were kept" in (ROOT / "www" / "public" / "uninstall.sh").read_text()
    assert "sha256(installed forkmesh)" in harness


def test_automated_checkpoint_reports_the_observer_verdict(tmp_path):
    source = HARNESS.read_text(encoding="utf-8")
    functions = source[source.index("utc() {"):source.index("release_verify() {")]

    def run_checkpoint(reply: str) -> str:
        hook = tmp_path / "hook.sh"
        hook.write_text(f"#!/bin/sh\nprintf '%s\\n' '{reply}'\n")
        hook.chmod(0o755)
        transcript = tmp_path / f"transcript-{abs(hash(reply))}.log"
        script = (
            "set -euo pipefail\n" + functions
            + f'TRANSCRIPT="{transcript}"; CHECKSUMS_SHA256=abc; RELEASE_TAG=v9.9.9\n'
            + "checkpoint 'Stop work pauses the loop' 'Choose Stop work.' stop-work\n"
        )
        subprocess.run(
            ["bash", "-c", script], check=True, capture_output=True,
            env={"PATH": "/usr/bin:/bin", "FORKMESH_TRANSCRIPT_AUTOMATED": "1",
                 "FORKMESH_SCREENSHOT_HOOK": str(hook)},
        )
        return transcript.read_text()

    observed = run_checkpoint("@@VERDICT stop-work observed read on screen: Stop work")
    assert "observed " in observed and "NOT OBSERVED" not in observed
    assert "read on screen: Stop work" in observed

    for reply in ("@@VERDICT stop-work not-observed none of Stop work; paused", ""):
        text = run_checkpoint(reply)
        assert "NOT OBSERVED" in text
        assert "\nobserved " not in text


def test_library_packages_fall_back_to_declared_dependencies():
    """liblept.so.6 lives in libleptonica6, so the soname rule alone cannot find it."""
    index = {"libtesseract5": {"Depends": "libc6 (>= 2.34), libleptonica6,\n libarchive13t64"}, "libleptonica6": {}}
    assert vm.dependency_library_packages(index["libtesseract5"], index) == ["libleptonica6"]
    assert vm.dependency_library_packages({"Depends": "libmissing9"}, index) == []
    assert vm.dependency_library_packages({}, index) == []


def test_the_tooling_names_no_release_version():
    """Any published release must be testable without editing the tooling."""
    for path in (HARNESS, TOOL, EXPECTATIONS):
        text = path.read_text(encoding="utf-8")
        assert not re.search(r"\bv\d+\.\d+\.\d+\b", text), f"{path.name} pins a release tag"
        assert not re.search(r"\b[0-9a-f]{64}\b", text), f"{path.name} pins a checksum"

    harness = HARNESS.read_text(encoding="utf-8")
    # Unpinned runs still verify, and record what they verified.
    assert 'EXPECTED_TAG="${FORKMESH_EXPECTED_TAG:-}"' in harness
    assert "no release pinned: this transcript covers whatever latest published" in harness
    assert "release=$RELEASE_TAG checksums_sha256=$CHECKSUMS_SHA256" in harness
    # A pin that does not match latest stops the run rather than testing the wrong bytes.
    assert 'die "latest tag is $tag, not the pinned $EXPECTED_TAG"' in harness
    # The installed bytes are matched against whatever assets that release carries.
    assert 'asset.get("os") == "linux" and asset.get("arch") == "x86_64"' in harness

    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "--expect-tag $CLEAN_MACHINE_TAG" in workflow
    assert "Release under test:" in workflow


def test_release_under_test_is_read_back_from_the_guest_transcript(tmp_path):
    transcript = tmp_path / "transcript.log"
    transcript.write_text(
        "manifest tag:              v1.2.3\n"
        "manifest checksums_sha256: " + "a" * 64 + "\n"
        "manifest tag:              ignored-second-line\n"
    )
    assert vm.verified_release(transcript) == {"tag": "v1.2.3", "checksums_sha256": "a" * 64}
    assert vm.verified_release(tmp_path / "missing.log") == {}
