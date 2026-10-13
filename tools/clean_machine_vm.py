#!/usr/bin/env python3
"""Run the clean-machine transcript inside a disposable QEMU VM (issue #705).

The VM is a fresh Debian 13 cloud image booted with a copy-on-write overlay, so
nothing on the runner is installed, stopped, or uninstalled: the published
installer, the desktop client, and uninstall.sh (which pkills every process
named forkmesh) only ever touch the guest.

Everything is captured from outside the guest:

* the VM's framebuffer is read over QMP ``screendump`` at a fixed rate and piped
  into ffmpeg, so the video is exactly what the virtual monitor showed, from
  firmware to power-off;
* the guest harness asks for named screenshots by writing ``@@SHOT <name>`` to
  a control serial port, and the host saves a fresh screendump for each;
* when the harness finishes, the guest streams a tarball of its transcript over
  a data serial port and writes ``@@DONE <exit status>``.

Subcommands:

    clean_machine_vm.py provision-qemu --dest DIR
        Unpack QEMU from the Debian archive into DIR (no root). InRelease is
        checked with gpgv against the local Debian archive keyring, and every
        Packages index and .deb against the SHA-256 it names.

    clean_machine_vm.py run --artifacts DIR [--mode run|verify] ...
        Boot the VM, run tools/clean_machine_transcript.sh <mode> in it, and
        write screenshots/, clean-machine.mp4, guest/, serial-console.log, and
        summary.json to DIR. Exit status is the guest harness's.
"""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import io
import json
import lzma
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import forkmesh_safe_delete  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tools" / "clean_machine_transcript.sh"
EXPECTATIONS = ROOT / "tools" / "clean_machine_expectations.json"

IMAGE_BASE = "https://cloud.debian.org/images/cloud/trixie/latest"
IMAGE_NAME = "debian-13-generic-amd64.qcow2"
DEBIAN_MIRROR = "https://deb.debian.org/debian"
DEBIAN_SUITE = "trixie"
DEBIAN_KEYRING = Path("/usr/share/keyrings/debian-archive-keyring.gpg")

# QEMU itself plus the firmware it loads. Shared libraries the runner lacks are
# added afterwards from ldd's "not found" lines (see missing_library_packages).
QEMU_PACKAGES = (
    "qemu-system-x86",
    "qemu-system-common",
    "qemu-system-data",
    "qemu-utils",
    "seabios",
    "ipxe-qemu",
)

OCR_PACKAGES = ("tesseract-ocr", "tesseract-ocr-eng", "libtesseract5")

GUEST_USER = "tester"
TERMINAL_TITLE = "ForkMesh clean-machine transcript"
SCREEN = (1280, 800)
MARKER = re.compile(r"@@(SHOT|DONE)\s+([A-Za-z0-9_.-]+)")


def utc() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def log(message: str) -> None:
    print(f"==> {utc()} {message}", flush=True)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url: str, timeout: int = 120) -> bytes:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return response.read()


def download(url: str, dest: Path, attempts: int = 8) -> None:
    """Resumable download: a transfer that stalls for 60 s is resumed with Range."""
    tmp = dest.with_name(dest.name + ".part")
    for attempt in range(1, attempts + 1):
        have = tmp.stat().st_size if tmp.exists() else 0
        request = urllib.request.Request(url, headers={"Range": f"bytes={have}-"} if have else {})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                if have and response.status != 206:
                    have = 0  # the server ignored Range; start over
                with tmp.open("ab" if have else "wb") as out:
                    shutil.copyfileobj(response, out, 1 << 20)
            tmp.replace(dest)
            return
        except (OSError, urllib.error.URLError) as error:
            size = tmp.stat().st_size if tmp.exists() else 0
            log(f"Download attempt {attempt} stopped at {size} bytes: {error}")
            time.sleep(min(30, 5 * attempt))
    raise SystemExit(f"could not download {url}")


# --- QEMU provisioning -------------------------------------------------------


def parse_release_sha256(release: str) -> dict[str, str]:
    """Map index path -> SHA-256 from a Release/InRelease body."""
    sums: dict[str, str] = {}
    section = False
    for line in release.splitlines():
        if not line.startswith(" "):
            section = line.strip() == "SHA256:"
            continue
        if section:
            parts = line.split()
            if len(parts) == 3:
                sums[parts[2]] = parts[0]
    return sums


def parse_packages(text: str) -> dict[str, dict[str, str]]:
    """Package name -> stanza fields; the last stanza for a name wins."""
    packages: dict[str, dict[str, str]] = {}
    for stanza in text.split("\n\n"):
        fields: dict[str, str] = {}
        key = ""
        for line in stanza.splitlines():
            if line.startswith((" ", "\t")) and key:
                fields[key] += "\n" + line.strip()
            elif ":" in line:
                key, _, value = line.partition(":")
                fields[key] = value.strip()
        if "Package" in fields:
            packages[fields["Package"]] = fields
    return packages


def library_package_candidates(soname: str) -> list[str]:
    """Debian package names that conventionally ship a shared object.

    libcapstone.so.5 -> libcapstone5; librdmacm.so.1 -> librdmacm1t64;
    libaio.so.1t64 -> libaio1t64; libfoo-2.0.so.0 -> libfoo-2.0-0.
    """
    match = re.match(r"^(lib[^/]+?)\.so\.([0-9][0-9a-z.]*)$", soname)
    if not match:
        return []
    stem, version = match.group(1).lower(), match.group(2)
    major = version.split(".")[0]
    if major.endswith("t64"):
        major = major[:-3]
    joiner = "-" if stem[-1].isdigit() else ""
    base = f"{stem}{joiner}{major}"
    return [base, f"{base}t64"]


def missing_library_packages(ldd_output: str, index: dict[str, dict[str, str]]) -> tuple[list[str], list[str]]:
    wanted: list[str] = []
    unresolved: list[str] = []
    for line in ldd_output.splitlines():
        if "not found" not in line:
            continue
        soname = line.split("=>", 1)[0].strip()
        for name in library_package_candidates(soname):
            if name in index:
                if name not in wanted:
                    wanted.append(name)
                break
        else:
            unresolved.append(soname)
    return wanted, unresolved


def unpacked_environment(prefix: Path) -> dict[str, str]:
    env = dict(os.environ)
    libdir = prefix / "usr" / "lib" / "x86_64-linux-gnu"
    env["LD_LIBRARY_PATH"] = ":".join(
        part for part in (str(libdir), env.get("LD_LIBRARY_PATH", "")) if part
    )
    env["PATH"] = f"{prefix / 'usr' / 'bin'}:{env.get('PATH', '')}"
    tessdata = sorted((prefix / "usr" / "share" / "tesseract-ocr").glob("*/tessdata"))
    if tessdata:
        env["TESSDATA_PREFIX"] = str(tessdata[-1])
    return env


# Kept as the old name for QEMU call sites.
qemu_environment = unpacked_environment


def firmware_dirs(prefix: Path) -> list[Path]:
    return [
        prefix / "usr" / "share" / "qemu",
        prefix / "usr" / "share" / "seabios",
        prefix / "usr" / "lib" / "ipxe" / "qemu",
    ]


def archive_index(dest: Path) -> dict[str, dict[str, str]]:
    """The signed trixie/main package index: gpgv on InRelease, SHA-256 on Packages."""
    for tool in ("gpgv", "dpkg-deb", "ldd"):
        if not shutil.which(tool):
            raise SystemExit(f"provisioning needs {tool} on the runner")
    if not DEBIAN_KEYRING.exists():
        raise SystemExit(f"provisioning needs the Debian archive keyring at {DEBIAN_KEYRING}")

    log(f"Fetching {DEBIAN_MIRROR}/dists/{DEBIAN_SUITE}/InRelease")
    inrelease = dest / "InRelease"
    inrelease.write_bytes(fetch(f"{DEBIAN_MIRROR}/dists/{DEBIAN_SUITE}/InRelease"))
    verified = subprocess.run(
        ["gpgv", "--keyring", str(DEBIAN_KEYRING), "--output", "-", str(inrelease)],
        capture_output=True,
    )
    if verified.returncode != 0:
        raise SystemExit("InRelease signature did not verify:\n" + verified.stderr.decode(errors="replace"))
    release_sums = parse_release_sha256(verified.stdout.decode())
    index_path = "main/binary-amd64/Packages.xz"
    packages_xz = fetch(f"{DEBIAN_MIRROR}/dists/{DEBIAN_SUITE}/{index_path}")
    if sha256_bytes(packages_xz) != release_sums.get(index_path):
        raise SystemExit(f"{index_path} does not match the signed InRelease")
    index = parse_packages(lzma.decompress(packages_xz).decode("utf-8", errors="replace"))
    log(f"InRelease verified with {DEBIAN_KEYRING.name}; {len(index)} packages indexed")
    return index


def dependency_library_packages(stanza: dict[str, str], index: dict[str, dict[str, str]]) -> list[str]:
    """Library packages a stanza depends on, for sonames whose package name differs."""
    names = []
    for clause in stanza.get("Depends", "").replace("\n", " ").split(","):
        name = clause.strip().split()[0].split("|")[0].strip() if clause.strip() else ""
        if name.startswith("lib") and name in index and name not in names:
            names.append(name)
    return names


def provision_packages(dest: Path, packages: tuple[str, ...], binaries: tuple[str, ...], label: str) -> Path:
    """Unpack packages (and the shared libraries they still miss) into dest/root."""
    dest.mkdir(parents=True, exist_ok=True)
    prefix = dest / "root"
    stamp = dest / "provisioned.json"
    first = prefix / "usr" / "bin" / binaries[0]
    if stamp.exists() and first.exists():
        log(f"Using provisioned {label} in {prefix}")
        return prefix

    index = archive_index(dest)
    installed: dict[str, str] = {}

    def unpack(name: str) -> None:
        stanza = index.get(name)
        if stanza is None:
            raise SystemExit(f"package {name} is not in {DEBIAN_SUITE}/main")
        data = fetch(f"{DEBIAN_MIRROR}/{stanza['Filename']}", timeout=300)
        if sha256_bytes(data) != stanza["SHA256"]:
            raise SystemExit(f"{name} does not match its signed SHA-256")
        deb = dest / Path(stanza["Filename"]).name
        deb.write_bytes(data)
        subprocess.run(["dpkg-deb", "-x", str(deb), str(prefix)], check=True)
        deb.unlink()
        installed[name] = stanza["Version"]
        log(f"Unpacked {name} {stanza['Version']} (sha256 {stanza['SHA256'][:16]}…)")

    for name in packages:
        unpack(name)
    env = unpacked_environment(prefix)
    for _ in range(8):
        ldd = "".join(
            subprocess.run(["ldd", str(prefix / "usr" / "bin" / tool)], capture_output=True, text=True, env=env).stdout
            for tool in binaries
        )
        wanted, unresolved = missing_library_packages(ldd, index)
        if unresolved:
            # A soname whose package is named differently (liblept.so.6 lives in
            # libleptonica6): take the library dependencies of what we unpacked.
            for name in list(installed):
                wanted += [n for n in dependency_library_packages(index[name], index) if n not in wanted]
        wanted = [name for name in wanted if name not in installed]
        if not wanted:
            if unresolved:
                raise SystemExit("cannot map missing libraries to packages: " + ", ".join(unresolved))
            break
        for name in wanted:
            unpack(name)
    else:
        raise SystemExit(f"{label} still has unresolved libraries after 8 passes")

    version = subprocess.run([str(first), "--version"], capture_output=True, text=True, env=env)
    banner = (version.stdout or version.stderr).splitlines()[0] if (version.stdout or version.stderr) else label
    stamp.write_text(json.dumps({"packages": installed, "version": banner}, indent=2))
    log(banner)
    return prefix


def provision_qemu(dest: Path) -> Path:
    return provision_packages(dest, QEMU_PACKAGES, ("qemu-system-x86_64", "qemu-img"), "QEMU")


def provision_ocr(dest: Path) -> Path:
    return provision_packages(dest, OCR_PACKAGES, ("tesseract",), "tesseract")


# --- observer ----------------------------------------------------------------


def normalize(text: str) -> str:
    return " ".join(text.split()).lower()


def loose(text: str) -> str:
    """Normalized text with punctuation dropped, for OCR near-misses."""
    return re.sub(r"[^a-z0-9 ]+", "", normalize(text))


def contains(haystack: str, needle: str) -> bool:
    return normalize(needle) in haystack or loose(needle) in loose(haystack)


def evaluate(text: str | None, rules: dict | None) -> dict:
    """Decide whether a screen counts as observed, and say exactly why."""
    if rules is None:
        return {"verdict": "unchecked", "matched": [], "missing": [], "detail": "no expectation for this step"}
    if text is None:
        return {"verdict": "not-observed", "matched": [], "missing": [], "detail": "no OCR available on this runner"}
    haystack = normalize(text)
    matched: list[str] = []
    missing: list[str] = []
    for phrase in rules.get("require_all", []):
        (matched if contains(haystack, phrase) else missing).append(phrase)
    any_phrases = rules.get("require_any", [])
    any_matched = [phrase for phrase in any_phrases if contains(haystack, phrase)]
    matched += any_matched
    forbidden = [phrase for phrase in rules.get("forbid", []) if contains(haystack, phrase)]
    ok = not missing and not forbidden and (not any_phrases or any_matched)
    detail = ""
    if not ok:
        if missing:
            detail = "missing " + "; ".join(missing)
        elif any_phrases and not any_matched:
            detail = "none of " + "; ".join(any_phrases)
        if forbidden:
            detail = (detail + " " if detail else "") + "forbidden text on screen: " + "; ".join(forbidden)
        if not any_phrases and not missing and not forbidden:
            detail = "expectation not met"
    else:
        detail = "read on screen: " + "; ".join(matched) if matched else "no forbidden text on screen"
    return {
        "verdict": "observed" if ok else "not-observed",
        "matched": matched,
        "missing": missing + forbidden,
        "detail": detail,
    }


class Observer:
    """Reads the VM's screen from outside the guest and checks it against
    tools/clean_machine_expectations.json."""

    def __init__(self, ocr_dir: Path | None, provision: bool = True) -> None:
        self.rules = {
            key: value
            for key, value in json.loads(EXPECTATIONS.read_text()).items()
            if not key.startswith("_")
        }
        self.command: str | None = shutil.which("tesseract")
        self.env = dict(os.environ)
        if self.command is None and ocr_dir is not None and provision:
            try:
                prefix = provision_ocr(ocr_dir)
                self.command = str(prefix / "usr" / "bin" / "tesseract")
                self.env = unpacked_environment(prefix)
            except SystemExit as error:
                log(f"OCR unavailable, screens cannot be observed: {error}")
                self.command = None
        self.observations: list[dict] = []

    def ocr(self, png: Path) -> str | None:
        if not self.command:
            return None
        result = subprocess.run(
            [self.command, str(png), "stdout", "--psm", "6"],
            capture_output=True, text=True, env=self.env,
        )
        if result.returncode != 0:
            log(f"tesseract failed on {png.name}: {result.stderr.strip()[:200]}")
            return None
        return result.stdout

    def observe(self, png: Path, step: str) -> dict:
        text = self.ocr(png)
        if text is not None:
            png.with_suffix(".txt").write_text(text)
        outcome = evaluate(text, self.rules.get(step))
        outcome["step"] = step
        outcome["screenshot"] = f"screenshots/{png.name}"
        outcome["at"] = utc()
        self.observations.append(outcome)
        return outcome


# --- guest configuration -----------------------------------------------------


def guest_scripts(mode: str, pins: dict[str, str] | None = None) -> dict[str, str]:
    home = f"/home/{GUEST_USER}"
    width, height = SCREEN
    # Unpinned by default: the guest verifies whatever the signed latest
    # manifest publishes, so the same workflow covers any release.
    exports = "".join(
        f'export {name}="{value}"\n' for name, value in sorted((pins or {}).items()) if value
    )
    return {
        f"{home}/.bash_profile": f"""\
[ -f ~/.bashrc ] && . ~/.bashrc
if [ -z "${{DISPLAY:-}}" ] && [ "$(tty)" = /dev/tty1 ] && [ ! -e ~/.session-started ]; then
  touch ~/.session-started
  exec startx {home}/session.sh -- -nolisten tcp
fi
""",
        f"{home}/session.sh": f"""\
#!/bin/bash
xrandr -s {width}x{height} >/dev/null 2>&1 || true
xsetroot -solid '#1e272e' >/dev/null 2>&1 || true
openbox &
sleep 3
exec xterm -geometry 158x46+8+8 -fa 'DejaVu Sans Mono' -fs 10 -bg black -fg '#dfe6e9' \\
  -T '{TERMINAL_TITLE}' -e {home}/drive.sh
""",
        # Screenshot hook for the harness: raise the requested window, then ask
        # the host for a screendump over the control port and give it time.
        f"{home}/shot.sh": f"""\
#!/bin/bash
name="${{1:?screenshot name}}"
focus="${{2:-terminal}}"
if [ "$focus" = app ]; then
  win="$(xdotool search --onlyvisible --class forkmesh 2>/dev/null | tail -1)"
else
  win="$(xdotool search --name '{TERMINAL_TITLE}' 2>/dev/null | head -1)"
fi
[ -n "$win" ] && xdotool windowactivate --sync "$win" >/dev/null 2>&1
sleep 2
# Ask the host observer to look at the screen and wait for its verdict, so the
# transcript records what was actually read there rather than an assumption.
sleep 1
echo "@@SHOT $name $focus" | sudo tee /dev/ttyS1 >/dev/null
verdict="$(sudo timeout 120 head -n 1 /dev/ttyS1 2>/dev/null | tr -d '\r')"
echo "${{verdict:-@@VERDICT $name not-observed the host observer did not answer}}"
""",
        f"{home}/drive.sh": f"""\
#!/bin/bash
set -u
{exports}out={home}/out
mkdir -p "$out"
cd {home}
sudo stty -F /dev/ttyS1 raw -echo 2>/dev/null || true
echo "Guest $(hostname) $(date -u +%Y-%m-%dT%H:%M:%SZ): starting clean_machine_transcript.sh {mode}"
{home}/shot.sh 00-desktop-ready
FORKMESH_TRANSCRIPT="$out/transcript.log" \\
FORKMESH_TRANSCRIPT_AUTOMATED=1 \\
FORKMESH_SCREENSHOT_HOOK={home}/shot.sh \\
  {home}/clean_machine_transcript.sh {mode}
rc=$?
echo "clean_machine_transcript.sh exited $rc"
{home}/shot.sh 99-finished
{{ echo "exit=$rc"; date -u +%Y-%m-%dT%H:%M:%SZ; }} > "$out/exit-status.txt"
tar -C {home} -czf /tmp/out.tgz out
{{ echo @@OUT-BEGIN; base64 -w 76 /tmp/out.tgz; echo @@OUT-END; }} | sudo tee /dev/ttyS2 >/dev/null
echo "@@DONE $rc" | sudo tee /dev/ttyS1 >/dev/null
sleep 3
sudo systemctl poweroff
""",
    }


def cloud_config(mode: str, harness: str, pins: dict[str, str] | None = None) -> str:
    """#cloud-config user-data. JSON is valid YAML, so no YAML library is needed."""
    files = []
    for path, content in guest_scripts(mode, pins).items():
        files.append(
            {
                "path": path,
                "encoding": "b64",
                "content": base64.b64encode(content.encode()).decode(),
                "owner": f"{GUEST_USER}:{GUEST_USER}",
                "permissions": "0644" if path.endswith(".bash_profile") else "0755",
                "defer": True,
            }
        )
    files.append(
        {
            "path": f"/home/{GUEST_USER}/clean_machine_transcript.sh",
            "encoding": "b64",
            "content": base64.b64encode(harness.encode()).decode(),
            "owner": f"{GUEST_USER}:{GUEST_USER}",
            "permissions": "0755",
            "defer": True,
        }
    )
    # Written early (not deferred); runcmd restarts tty1 once the X packages
    # and the deferred home-directory scripts exist, so the autologin shell
    # that starts the session is the second one.
    files.append(
        {
            "path": "/etc/systemd/system/getty@tty1.service.d/autologin.conf",
            "content": (
                "[Service]\nExecStart=\n"
                f"ExecStart=-/sbin/agetty --autologin {GUEST_USER} --noclear %I $TERM\n"
            ),
        }
    )
    config = {
        "hostname": "forkmesh-clean-machine",
        "users": [
            {
                "name": GUEST_USER,
                "groups": ["sudo", "video", "input", "audio"],
                "shell": "/bin/bash",
                "sudo": "ALL=(ALL) NOPASSWD:ALL",
                "lock_passwd": True,
            }
        ],
        "package_update": True,
        "packages": [
            "xserver-xorg-core",
            "xserver-xorg-input-libinput",
            "xinit",
            "openbox",
            "xterm",
            "xdotool",
            "x11-xserver-utils",
            "fonts-dejavu-core",
            "curl",
            "git",
            "openssl",
            "python3",
            "ca-certificates",
            "bsdutils",
        ],
        "write_files": files,
        "runcmd": [
            ["systemctl", "daemon-reload"],
            ["systemctl", "restart", "getty@tty1.service"],
        ],
    }
    return "#cloud-config\n" + json.dumps(config, indent=2) + "\n"


def write_seed(seed: Path, mode: str, pins: dict[str, str] | None = None) -> None:
    seed.mkdir(parents=True, exist_ok=True)
    (seed / "meta-data").write_text("instance-id: forkmesh-clean-machine\nlocal-hostname: forkmesh-clean-machine\n")
    (seed / "user-data").write_text(cloud_config(mode, HARNESS.read_text(encoding="utf-8"), pins))


# --- VM run ------------------------------------------------------------------


def kvm_usable() -> bool:
    return os.access("/dev/kvm", os.R_OK | os.W_OK)


def qemu_command(
    qemu: str,
    firmware: list[Path],
    overlay: Path,
    seed: Path,
    work: Path,
    cpus: int,
    memory_mb: int,
    kvm: bool,
) -> list[str]:
    cmd = [qemu]
    for directory in firmware:
        if directory.is_dir():
            cmd += ["-L", str(directory)]
    cmd += ["-machine", "q35"]
    cmd += ["-accel", "kvm", "-accel", "tcg"] if kvm else ["-accel", "tcg,thread=multi"]
    cmd += [
        "-cpu", "max",
        "-smp", str(cpus),
        "-m", str(memory_mb),
        "-display", "none",
        "-vga", "std",
        "-nic", "user,model=virtio-net-pci",
        "-drive", f"file={overlay},if=virtio,format=qcow2",
        # cloud-init NoCloud reads a filesystem labelled cidata; vvfat builds
        # one from the seed directory, so no ISO or FAT tooling is needed.
        "-drive", f"if=virtio,format=raw,readonly=on,file.driver=vvfat,file.dir={seed},file.label=cidata",
        "-chardev", f"file,id=console,path={work / 'serial-console.log'}",
        "-serial", "chardev:console",
        # A socket (not a file) so the observer can answer the guest.
        "-chardev", f"socket,id=control,path={work / 'control.sock'},server=on,wait=off",
        "-serial", "chardev:control",
        "-chardev", f"file,id=data,path={work / 'serial-data.log'}",
        "-serial", "chardev:data",
        "-qmp", f"unix:{work / 'qmp.sock'},server=on,wait=off",
        "-no-reboot",
    ]
    return cmd


class Qmp:
    def __init__(self, path: Path, deadline: float) -> None:
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(str(path))
                break
            except OSError:
                self.sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(30)
        self.buffer = b""
        self._read()  # greeting
        self.execute("qmp_capabilities")

    def _read(self) -> dict:
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("QMP closed")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def execute(self, command: str, **arguments) -> dict:
        message = {"execute": command}
        if arguments:
            message["arguments"] = arguments
        self.sock.sendall(json.dumps(message).encode() + b"\n")
        while True:
            reply = self._read()
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError(f"QMP {command}: {reply['error']}")
            return reply


class Control:
    """The guest's control serial port: markers in, verdicts out."""

    def __init__(self, path: Path, transcript: Path, deadline: float) -> None:
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(str(path))
                break
            except OSError:
                self.sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)
        self.sock.settimeout(0.2)
        self.buffer = b""
        self.transcript = transcript
        self.transcript.write_text("")

    def lines(self) -> list[str]:
        try:
            chunk = self.sock.recv(65536)
            if chunk:
                self.buffer += chunk
        except (TimeoutError, socket.timeout, BlockingIOError):
            pass
        except OSError:
            return []
        out = []
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            line = raw.decode(errors="replace").strip()
            if line:
                with self.transcript.open("a") as handle:
                    handle.write(f"guest> {line}\n")
                out.append(line)
        return out

    def send(self, line: str) -> None:
        with self.transcript.open("a") as handle:
            handle.write(f"host < {line}\n")
        try:
            self.sock.sendall((line + "\n").encode())
        except OSError as error:
            log(f"could not answer the guest: {error}")


def parse_markers(text: str) -> list[tuple[str, str]]:
    return [(kind, value) for kind, value in MARKER.findall(text)]


def verified_release(transcript: Path) -> dict[str, str]:
    """The tag and checksum the guest transcript says it verified."""
    if not transcript.exists():
        return {}
    found = {}
    for line in transcript.read_text(errors="replace").splitlines():
        for key, prefix in (("tag", "manifest tag:"), ("checksums_sha256", "manifest checksums_sha256:")):
            if line.startswith(prefix) and key not in found:
                found[key] = line[len(prefix):].strip().split()[0]
    return found


def extract_guest_output(data_log: Path, dest: Path) -> bool:
    text = data_log.read_text(errors="replace") if data_log.exists() else ""
    match = re.search(r"@@OUT-BEGIN\r?\n(.*?)@@OUT-END", text, re.S)
    if not match:
        return False
    payload = base64.b64decode(re.sub(r"\s+", "", match.group(1)))
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as archive:
        archive.extractall(dest, filter="data")
    return True


def ensure_image(cache: Path) -> tuple[Path, str]:
    cache.mkdir(parents=True, exist_ok=True)
    sums = fetch(f"{IMAGE_BASE}/SHA512SUMS").decode()
    expected = next(
        (line.split()[0] for line in sums.splitlines() if line.split()[-1:] == [IMAGE_NAME]),
        None,
    )
    if not expected:
        raise SystemExit(f"{IMAGE_NAME} is not listed in {IMAGE_BASE}/SHA512SUMS")
    image = cache / IMAGE_NAME
    if not image.exists() or file_digest(image, "sha512") != expected:
        log(f"Downloading {IMAGE_BASE}/{IMAGE_NAME}")
        download(f"{IMAGE_BASE}/{IMAGE_NAME}", image)
        if file_digest(image, "sha512") != expected:
            image.unlink()
            raise SystemExit(f"{IMAGE_NAME} does not match SHA512SUMS")
    log(f"Base image {IMAGE_NAME} sha512 {expected[:24]}… (matches SHA512SUMS)")
    return image, expected


def locate_qemu(provision_dir: Path | None) -> tuple[str, str, list[Path], dict[str, str]]:
    system_qemu = shutil.which("qemu-system-x86_64")
    system_img = shutil.which("qemu-img")
    if system_qemu and system_img:
        return system_qemu, system_img, [], dict(os.environ)
    if provision_dir is None:
        raise SystemExit("qemu-system-x86_64 and qemu-img are not on PATH; pass --provision-dir")
    prefix = provision_qemu(provision_dir)
    bin_dir = prefix / "usr" / "bin"
    return (
        str(bin_dir / "qemu-system-x86_64"),
        str(bin_dir / "qemu-img"),
        firmware_dirs(prefix),
        qemu_environment(prefix),
    )


def run_vm(args: argparse.Namespace) -> int:
    artifacts = Path(args.artifacts).resolve()
    shots_dir = artifacts / "screenshots"
    shots_dir.mkdir(parents=True, exist_ok=True)
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    started = utc()

    qemu, qemu_img, firmware, env = locate_qemu(Path(args.provision_dir) if args.provision_dir else None)
    qemu_version = subprocess.run([qemu, "--version"], capture_output=True, text=True, env=env).stdout.splitlines()[0]
    image, image_sha512 = ensure_image(Path(args.image_cache))

    overlay = work / "overlay.qcow2"
    overlay.unlink(missing_ok=True)
    subprocess.run(
        [qemu_img, "create", "-q", "-f", "qcow2", "-F", "qcow2", "-b", str(image), str(overlay), "24G"],
        check=True,
        env=env,
    )
    seed = work / "seed"
    forkmesh_safe_delete.remove_tree(seed, work, ignore_errors=True)
    pins = {
        "FORKMESH_EXPECTED_TAG": args.expect_tag or "",
        "FORKMESH_EXPECTED_CHECKSUMS_SHA256": args.expect_checksums or "",
        "FORKMESH_HOST": args.host,
    }
    write_seed(seed, args.mode, pins)
    log(
        "Release under test: "
        + (f"pinned {args.expect_tag or args.expect_checksums}" if (args.expect_tag or args.expect_checksums)
           else f"whatever {args.host} publishes as latest")
    )
    for name in ("serial-console.log", "serial-control.log", "serial-data.log", "qmp.sock", "control.sock"):
        (work / name).unlink(missing_ok=True)

    observer = Observer(Path(args.ocr_dir) if args.ocr_dir else None, provision=not args.no_observer)
    if observer.command:
        log(f"Screen observer: {observer.command}; expectations for {len(observer.rules)} steps")
    else:
        log("Screen observer: no OCR available; every screen check stays NOT OBSERVED")

    kvm = kvm_usable() if args.accel == "auto" else args.accel == "kvm"
    cmd = qemu_command(qemu, firmware, overlay, seed, work, args.cpus, args.memory, kvm)
    (artifacts / "qemu-command.txt").write_text(" ".join(cmd) + "\n")
    log(f"{qemu_version}; acceleration {'kvm (tcg fallback)' if kvm else 'tcg (no usable /dev/kvm)'}")
    vm = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    video = artifacts / "clean-machine.mp4"
    width, height = SCREEN
    encoder = subprocess.Popen(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "image2pipe", "-framerate", str(args.fps), "-i", "-",
            "-vf", f"scale={width}:{height}:force_original_aspect_ratio=decrease,"
                   f"pad={width}:{height}:(ow-iw)/2:(oh-ih)/2,setsar=1",
            "-c:v", "libx264", "-preset", "veryfast", "-crf", "28", "-pix_fmt", "yuv420p",
            "-movflags", "+faststart",
            str(video),
        ],
        stdin=subprocess.PIPE,
    )

    deadline = time.monotonic() + args.timeout_minutes * 60
    frame = work / "frame.png"
    shots: list[dict[str, str]] = []
    guest_rc: int | None = None
    frames = 0
    qmp = None
    interval = 1.0 / args.fps
    try:
        qmp = Qmp(work / "qmp.sock", time.monotonic() + 60)
        control = Control(work / "control.sock", work / "serial-control.log", time.monotonic() + 60)
        next_frame = time.monotonic()
        while vm.poll() is None:
            now = time.monotonic()
            if now > deadline:
                log(f"Timed out after {args.timeout_minutes} minutes; stopping the VM")
                break
            if now >= next_frame:
                next_frame += interval
                try:
                    qmp.execute("screendump", filename=str(frame), format="png")
                    encoder.stdin.write(frame.read_bytes())
                    frames += 1
                except (RuntimeError, OSError, ConnectionError) as error:
                    if vm.poll() is not None:
                        break
                    log(f"screendump failed: {error}")
            for line in control.lines():
                for kind, value in parse_markers(line):
                    if kind == "SHOT":
                        name = f"{len(shots) + 1:02d}-{value}.png"
                        try:
                            qmp.execute("screendump", filename=str(shots_dir / name), format="png")
                        except (RuntimeError, OSError, ConnectionError) as error:
                            log(f"Screenshot {name} failed: {error}")
                            control.send(f"@@VERDICT {value} not-observed the screen could not be captured")
                            continue
                        shots.append({"file": f"screenshots/{name}", "at": utc()})
                        outcome = observer.observe(shots_dir / name, value)
                        log(f"Screenshot {name}: {outcome['verdict']} — {outcome['detail']}")
                        control.send(f"@@VERDICT {value} {outcome['verdict']} {outcome['detail']}")
                    elif kind == "DONE":
                        guest_rc = int(value) if value.isdigit() else 1
                        log(f"Guest harness finished with exit status {guest_rc}")
            time.sleep(min(0.25, interval))
    finally:
        if vm.poll() is None:
            try:
                if qmp:
                    qmp.execute("quit")
            except Exception:
                pass
            try:
                vm.wait(30)
            except subprocess.TimeoutExpired:
                vm.kill()
        vm_output = vm.stdout.read() if vm.stdout else ""
        encoder.stdin.close()
        encoder.wait(600)

    guest_dir = artifacts / "guest"
    got_output = extract_guest_output(work / "serial-data.log", guest_dir)
    shutil.copy2(work / "serial-console.log", artifacts / "serial-console.log")
    if (work / "serial-control.log").exists():
        shutil.copy2(work / "serial-control.log", artifacts / "observer-exchange.log")
    if vm_output.strip():
        (artifacts / "qemu-output.txt").write_text(vm_output)
    summary = {
        "issue": 705,
        "mode": args.mode,
        "host": args.host,
        "pinned": {key: value for key, value in pins.items() if value and key != "FORKMESH_HOST"} or None,
        "release_under_test": verified_release(guest_dir / "out" / "transcript.log") or None,
        "started_at": started,
        "ended_at": utc(),
        "qemu": qemu_version,
        "acceleration": "kvm" if kvm else "tcg",
        "base_image": f"{IMAGE_BASE}/{IMAGE_NAME}",
        "base_image_sha512": image_sha512,
        "harness_sha256": file_digest(HARNESS, "sha256"),
        "guest_exit_status": guest_rc,
        "guest_output_extracted": got_output,
        "video": video.name if video.exists() else None,
        "video_frames": frames,
        "frames_per_second": args.fps,
        "screenshots": shots,
        "observer": {
            "ocr": observer.command,
            "expectations": str(EXPECTATIONS.relative_to(ROOT)),
            "observed": sum(1 for o in observer.observations if o["verdict"] == "observed"),
            "not_observed": sum(1 for o in observer.observations if o["verdict"] == "not-observed"),
            "unchecked": sum(1 for o in observer.observations if o["verdict"] == "unchecked"),
        },
        "observations": observer.observations,
    }
    (artifacts / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    log(f"Artifacts: {artifacts}")
    for path in sorted(artifacts.rglob("*")):
        if path.is_file():
            log(f"  {path.relative_to(artifacts)} ({path.stat().st_size} bytes)")
    if guest_rc is None:
        log("The guest never reported completion; see serial-console.log and the video")
        return 1
    return guest_rc


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    provision = sub.add_parser("provision-qemu")
    provision.add_argument("--dest", required=True)
    provision_ocr_parser = sub.add_parser("provision-ocr")
    provision_ocr_parser.add_argument("--dest", required=True)
    run = sub.add_parser("run")
    run.add_argument("--artifacts", required=True)
    run.add_argument("--work", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "forkmesh-clean-machine-vm"))
    run.add_argument("--image-cache", default=os.path.join(os.environ.get("TMPDIR", "/tmp"), "forkmesh-vm-images"))
    run.add_argument("--provision-dir")
    run.add_argument("--ocr-dir", help="where to unpack tesseract when the runner has none")
    run.add_argument("--no-observer", action="store_true",
                     help="do not provision OCR; screens are captured but not checked")
    run.add_argument("--mode", choices=("run", "verify"), default="run")
    run.add_argument("--host", default="https://forkmesh.com", help="where the guest fetches the release from")
    run.add_argument("--expect-tag", help="pin the release tag (default: whatever latest publishes)")
    run.add_argument("--expect-checksums", help="pin the manifest checksums_sha256")
    run.add_argument("--cpus", type=int, default=4)
    run.add_argument("--memory", type=int, default=4096, help="guest memory in MiB")
    run.add_argument("--accel", choices=("auto", "kvm", "tcg"), default="auto",
                     help="auto uses KVM when /dev/kvm is usable, else TCG emulation")
    run.add_argument("--fps", type=int, default=2)
    run.add_argument("--timeout-minutes", type=int, default=210)
    args = parser.parse_args(argv)
    if args.command == "provision-qemu":
        provision_qemu(Path(args.dest))
        return 0
    if args.command == "provision-ocr":
        provision_ocr(Path(args.dest))
        return 0
    return run_vm(args)


if __name__ == "__main__":
    sys.exit(main())
