#!/usr/bin/env python3
"""install.sh prebuilt fast path (issue #304).

Drives install_prebuilt_release() with mocked `git` (serves the committed
SHASUMS256.txt manifest over a fake clone) and `curl` (serves the
content-addressed blob), asserting the installer:

  * resolves the platform asset's sha256 from the manifest,
  * downloads it from the relay's release endpoint,
  * VERIFIES the sha256 before installing,
  * refuses to install a blob whose bytes don't match the manifest.
"""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


INSTALLER = Path(__file__).resolve().parents[2] / "www" / "public" / "install.sh"


def _release_functions():
    """Extract the contiguous prebuilt-install helper block from install.sh."""
    script = INSTALLER.read_text(encoding="utf-8")
    start = script.index("_sparse_fetch_file() {")
    # install_prebuilt_release is the last function in the block; cut at its
    # closing brace (first line that is exactly "}" after its definition).
    body_at = script.index("install_prebuilt_release() {", start)
    end = script.index("\n}\n", body_at) + len("\n}\n")
    return script[start:end]


PREAMBLE = """
set -u
say()  { :; }
warn() { printf 'WARN:%s\\n' "$1" >&2; }
_sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" 2>/dev/null | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'
  else
    echo ""
  fi
}
RELEASE_CHANNEL="latest"
ASSET_NAME="forkmesh-linux-x86_64"
ASSET_OS="linux"
ASSET_ARCH="x86_64"
ASSET_REL_PATH=".forkmesh/releases/${RELEASE_CHANNEL}/${ASSET_NAME}"
FORKMESH_HOST="https://relay.test"
FORKMESH_EXPECTED_BUILD_COMMIT="${FORKMESH_EXPECTED_BUILD_COMMIT:-}"
FORKMESH_EXPECTED_RELEASE_VERSION="${FORKMESH_EXPECTED_RELEASE_VERSION:-}"
FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256="${FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256:-}"
FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE="${FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE:-}"
FORKMESH_LOCAL_BINARY_SHA256="${FORKMESH_LOCAL_BINARY_SHA256:-}"
FORKMESH_MIN_RELEASE_VERSION="0.1.0"
DEFER_PINNED_REINSTALL=0
ensure_qt_runtime() { :; }
ensure_mirror_candidates() { :; }
uninstall_forkmesh() { :; }
BIN_DIR="$OUT_DIR/bin"
BIN="$BIN_DIR/forkmesh"
REPO_CANDIDATES=("https://relay.test/alice/forkmesh")
"""


def _signed_manifest_contract(
    sums_line,
    *,
    repo="forkmesh/forkmesh",
    build_commit=None,
    version="0.7.0",
    channel="latest",
):
    """Return release-v2 bytes suitable for controller-digest authentication."""
    return (
        '{"schema":"forkmesh-release-v2","repo":"%s",'
        '"tag":"v%s","tag_commit":"%s","build_commit":"%s",'
        '"channel":"%s","checksums_sha256":"%s",'
        '"assets":[{"name":"forkmesh-linux-x86_64"}]}'
        % (
            repo,
            version,
            "f0" * 20,
            build_commit or "a1" * 20,
            channel,
            hashlib.sha256(sums_line.encode()).hexdigest(),
        )
    )


def _run(
    sums_line,
    payload,
    *,
    omit_curl=False,
    manifest=None,
    expected_build_commit="",
    expected_version="",
    existing_binary=None,
    no_checksum_tool=False,
    trust_manifest=True,
    git_failures=0,
    no_git=False,
    no_sums=False,
    curl_fails=False,
    details=None,
):
    """Run install_prebuilt_release with a fake git+curl.

    Returns (rc, BIN bytes, stderr, blob_url). When `manifest` is given it is
    committed as .forkmesh/releases/latest/release.json in the fake repo, so the test can
    assert the blob URL is built from the manifest's canonical repo.

    `git_failures` makes the first N `git clone` calls fail like a restarting
    mirror (HTTP 503); each fetch attempt makes two clones (blobless, then
    plain). `details`, when a dict, receives the recorded failure cause
    ("kind") and metadata attempt count ("attempts").
    """
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        fake_repo = tmp / "fake_repo" / ".forkmesh" / "releases" / "latest"
        fake_repo.mkdir(parents=True)
        if not no_sums:
            (fake_repo / "SHASUMS256.txt").write_text(sums_line, encoding="utf-8")
        if manifest is None:
            manifest = _signed_manifest_contract(
                sums_line,
                build_commit=expected_build_commit or None,
                version=expected_version or "0.7.0",
            )
        if manifest is not None:
            (fake_repo / "release.json").write_text(manifest, encoding="utf-8")
        payload_file = tmp / "payload.bin"
        payload_file.write_bytes(payload)
        url_file = tmp / "curl_url"

        bindir = tmp / "stubbin"
        bindir.mkdir()
        # Fake git: clone makes the dir; sparse-checkout records the rel path(s)
        # (one per line); checkout copies each that exists out of $FAKE_REPO.
        (bindir / "git").write_text(
            """#!/bin/sh
if [ "$1" = "clone" ]; then
  n=$(cat "$GIT_CLONE_COUNT" 2>/dev/null || echo 0)
  echo $((n + 1)) > "$GIT_CLONE_COUNT"
  if [ "$n" -lt "$GIT_FAILURES" ]; then
    echo "fatal: unable to access 'https://relay.test/alice/forkmesh/': The requested URL returned error: 503" >&2
    exit 128
  fi
  eval "dest=\\${$#}"; mkdir -p "$dest"; exit 0
fi
if [ "$1" = "-C" ]; then
  dir="$2"; shift 2
  if [ "$1" = "sparse-checkout" ]; then
    shift 3; printf '%s\\n' "$@" > "$dir/.rel"; exit 0
  fi
  if [ "$1" = "checkout" ]; then
    while IFS= read -r rel; do
      [ -n "$rel" ] || continue
      if [ -f "$FAKE_REPO/$rel" ]; then
        mkdir -p "$dir/$(dirname "$rel")"; cp "$FAKE_REPO/$rel" "$dir/$rel"
      fi
    done < "$dir/.rel"
    exit 0
  fi
fi
exit 0
""",
            encoding="utf-8",
        )
        (bindir / "git").chmod(0o755)
        if no_git:
            (bindir / "git").unlink()
        if no_checksum_tool:
            (bindir / "sha256sum").write_text(
                "#!/bin/sh\nexit 127\n", encoding="utf-8"
            )
            (bindir / "sha256sum").chmod(0o755)
        if not omit_curl:
            # Fake curl: `curl -fsSL <url> -o <out>` → record the URL and write
            # the payload to <out>.
            (bindir / "curl").write_text(
                """#!/bin/sh
out=""; url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift ;;
    -*) ;;
    *) url="$1" ;;
  esac
  shift
done
[ -n "$url" ] && printf '%s\\n' "$url" > "$FORKMESH_TEST_URL_FILE"
[ "${FORKMESH_TEST_CURL_FAILS:-0}" = "1" ] && exit 22
[ -n "$out" ] && cp "$FORKMESH_TEST_PAYLOAD" "$out"
exit 0
""",
                encoding="utf-8",
            )
            (bindir / "curl").chmod(0o755)

        out_dir = tmp / "out"
        out_dir.mkdir()
        if existing_binary is not None:
            existing = out_dir / "bin" / "forkmesh"
            existing.parent.mkdir()
            existing.write_bytes(existing_binary)
            existing.chmod(0o755)
        env = os.environ.copy()
        if no_git:
            # Keep the stubs but hide every real git on PATH.
            env["PATH"] = str(bindir)
        else:
            env["PATH"] = str(bindir) + os.pathsep + env["PATH"]
        env["OUT_DIR"] = str(out_dir)
        env["FAKE_REPO"] = str(tmp / "fake_repo")
        env["FORKMESH_TEST_PAYLOAD"] = str(payload_file)
        env["FORKMESH_TEST_URL_FILE"] = str(url_file)
        env["GIT_FAILURES"] = str(git_failures)
        env["GIT_CLONE_COUNT"] = str(tmp / "git_clone_count")
        env["FORKMESH_TEST_CURL_FAILS"] = "1" if curl_fails else "0"
        kind_file = tmp / "failure_kind"
        env["FORKMESH_TEST_KIND_FILE"] = str(kind_file)
        # Exercise the full retry schedule without real sleeps.
        env["FORKMESH_METADATA_RETRY_DELAYS"] = "0 0"
        env["FORKMESH_DOWNLOAD_RETRY_DELAYS"] = "0"
        env["FORKMESH_EXPECTED_BUILD_COMMIT"] = expected_build_commit
        env["FORKMESH_EXPECTED_RELEASE_VERSION"] = expected_version
        env["FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256"] = (
            hashlib.sha256(manifest.encode()).hexdigest()
            if trust_manifest and manifest is not None
            else ""
        )
        env["FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE"] = ""

        script = (
            PREAMBLE
            + _release_functions()
            + "\ninstall_prebuilt_release\nrc=$?\n"
            + 'printf "%s %s\\n" "${RELEASE_PREBUILT_FAILURE:-}" '
            + '"${RELEASE_METADATA_ATTEMPTS:-0}" > "$FORKMESH_TEST_KIND_FILE"\n'
            + "exit $rc\n"
        )
        proc = subprocess.run(
            [shutil.which("bash"), "-c", script], env=env, text=True,
            capture_output=True)
        bin_path = out_dir / "bin" / "forkmesh"
        installed = bin_path.read_bytes() if bin_path.exists() else None
        blob_url = url_file.read_text().strip() if url_file.exists() else None
        if details is not None and kind_file.exists():
            kind, _, attempts = kind_file.read_text().strip().partition(" ")
            if not attempts:  # empty kind: only the attempt count was printed
                kind, attempts = "", kind
            details["kind"] = kind
            details["attempts"] = int(attempts or 0)
        return proc.returncode, installed, proc.stderr, blob_url


def test_installs_verified_blob():
    payload = b"\x7fELF fake forkmesh binary bytes" * 50
    digest = hashlib.sha256(payload).hexdigest()
    rc, installed, _, _ = _run("%s  forkmesh-linux-x86_64\n" % digest, payload)
    assert rc == 0
    assert installed == payload


def test_prebuilt_install_fails_closed_without_checksum_tool():
    payload = b"\x7fELF ordinary unpinned install" * 20
    digest = hashlib.sha256(payload).hexdigest()
    rc, installed, stderr, _ = _run(
        "%s  forkmesh-linux-x86_64\n" % digest,
        payload,
        no_checksum_tool=True,
    )
    assert rc != 0
    assert installed is None
    assert "sha-256 tool is required" in stderr.lower()


def test_rejects_checksum_mismatch():
    # Manifest advertises one hash, the relay serves different bytes → no install.
    payload = b"tampered bytes that do not match the manifest hash"
    wrong = hashlib.sha256(b"the expected bytes").hexdigest()
    rc, installed, stderr, _ = _run("%s  forkmesh-linux-x86_64\n" % wrong, payload)
    assert rc != 0
    assert installed is None
    assert "mismatch" in stderr.lower()


def test_trusted_ed25519_key_accepts_signature_and_rejects_tampering(tmp_path):
    if shutil.which("openssl") is None:
        return
    sums = tmp_path / "SHASUMS256.txt"
    sums.write_text(
        hashlib.sha256(b"asset").hexdigest()
        + "  forkmesh-linux-x86_64\n",
        encoding="utf-8",
    )
    manifest = tmp_path / "release.json"
    manifest.write_text(
        _signed_manifest_contract(sums.read_text(encoding="utf-8")),
        encoding="utf-8",
    )
    private_key = tmp_path / "private.pem"
    public_key = tmp_path / "public.pem"
    signature = tmp_path / "release.json.sig"
    subprocess.run(
        ["openssl", "genpkey", "-algorithm", "Ed25519", "-out", private_key],
        check=True,
    )
    subprocess.run(
        ["openssl", "pkey", "-in", private_key, "-pubout", "-out", public_key],
        check=True,
    )
    subprocess.run(
        [
            "openssl",
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            private_key,
            "-in",
            manifest,
            "-out",
            signature,
        ],
        check=True,
    )
    script = (
        PREAMBLE
        + _release_functions()
        + '\n_verify_release_metadata "$MANIFEST" "$SIGNATURE" "$SUMS"\n'
    )
    env = os.environ.copy()
    env.update(
        {
            "OUT_DIR": str(tmp_path / "out"),
            "MANIFEST": str(manifest),
            "SIGNATURE": str(signature),
            "SUMS": str(sums),
            "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256": "",
            "FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE": str(public_key),
        }
    )
    accepted = subprocess.run(
        ["bash", "-c", script], env=env, text=True, capture_output=True
    )
    assert accepted.returncode == 0, accepted.stderr

    manifest.write_bytes(manifest.read_bytes() + b" ")
    rejected = subprocess.run(
        ["bash", "-c", script], env=env, text=True, capture_output=True
    )
    assert rejected.returncode != 0
    assert "signature is missing or invalid" in rejected.stderr


def _sandboxed_env(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    home = tmp_path / "home"
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        path = home if name == "HOME" else home / name.lower()
        path.mkdir(parents=True, exist_ok=True)
        env[name] = str(path)
    return env


def _verify_signed(tmp_path, *, version="0.7.0", channel="latest",
                   wanted_channel="latest", minimum=None, build_commit=""):
    sums = tmp_path / "SHASUMS256.txt"
    sums.write_text(hashlib.sha256(b"asset").hexdigest()
                    + "  forkmesh-linux-x86_64\n", encoding="utf-8")
    manifest = tmp_path / "release.json"
    manifest.write_text(_signed_manifest_contract(
        sums.read_text(encoding="utf-8"), version=version, channel=channel,
        build_commit=build_commit or None), encoding="utf-8")
    private_key = tmp_path / "private.pem"
    public_key = tmp_path / "public.pem"
    signature = tmp_path / "release.json.sig"
    if not private_key.exists():
        subprocess.run(["openssl", "genpkey", "-algorithm", "Ed25519",
                        "-out", private_key], check=True)
        subprocess.run(["openssl", "pkey", "-in", private_key, "-pubout",
                        "-out", public_key], check=True)
    subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey",
                    private_key, "-in", manifest, "-out", signature],
                   check=True)
    override = ""
    if minimum is not None:
        override = 'FORKMESH_MIN_RELEASE_VERSION="%s"\n' % minimum
    script = (PREAMBLE + override + 'RELEASE_CHANNEL="%s"\n' % wanted_channel
              + _release_functions()
              + '\n_verify_release_metadata "$MANIFEST" "$SIGNATURE" "$SUMS"\n')
    env = _sandboxed_env(tmp_path)
    env.update({
        "OUT_DIR": str(tmp_path / "out"),
        "MANIFEST": str(manifest),
        "SIGNATURE": str(signature),
        "SUMS": str(sums),
        "FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256": "",
        "FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE": str(public_key),
        "FORKMESH_EXPECTED_BUILD_COMMIT": build_commit,
        "FORKMESH_EXPECTED_RELEASE_VERSION": version if build_commit else "",
    })
    return subprocess.run(["bash", "-c", script], env=env, text=True,
                          capture_output=True)


def test_signed_manifest_must_be_for_the_wanted_channel(tmp_path):
    if shutil.which("openssl") is None:
        return
    assert _verify_signed(tmp_path, channel="latest").returncode == 0
    assert _verify_signed(tmp_path, channel="candidate-0.7.0").returncode == 0
    for channel, wanted in (("candidate-0.6.0", "latest"), ("beta", "latest"),
                            ("latest", "beta"), ("", "latest")):
        refused = _verify_signed(tmp_path, channel=channel,
                                 wanted_channel=wanted)
        assert refused.returncode != 0, (channel, wanted)
        assert "release channel" in refused.stderr
    assert _verify_signed(tmp_path, channel="beta",
                          wanted_channel="beta").returncode == 0


def test_signed_manifest_older_than_the_installer_minimum_is_refused(tmp_path):
    if shutil.which("openssl") is None:
        return
    refused = _verify_signed(tmp_path, version="1.2.7", minimum="1.2.8")
    assert refused.returncode != 0
    assert "older than this installer's minimum" in refused.stderr
    assert _verify_signed(tmp_path, version="1.2.8",
                          minimum="1.2.8").returncode == 0
    assert _verify_signed(tmp_path, version="1.10.0",
                          minimum="1.9.3").returncode == 0
    assert _verify_signed(tmp_path, version="1.2.10",
                          minimum="1.2.9").returncode == 0
    assert _verify_signed(tmp_path, version="0.7.0",
                          minimum="").returncode != 0
    pinned = _verify_signed(tmp_path, version="1.2.7", minimum="1.2.8",
                            build_commit="a1" * 20)
    assert pinned.returncode == 0, pinned.stderr


def test_installer_minimum_release_is_set_and_not_above_latest():
    import json
    import re
    source = INSTALLER.read_text(encoding="utf-8")
    found = re.findall(r'^FORKMESH_MIN_RELEASE_VERSION="([0-9.]+)"$', source,
                       re.M)
    assert len(found) == 1
    latest = json.loads((INSTALLER.parents[2] / ".forkmesh" / "releases"
                         / "latest" / "release.json").read_text())
    def parts(text):
        return [int(p) for p in text.lstrip("v").split(".")]
    assert parts(found[0]) <= parts(latest["tag"])


def test_builtin_release_key_survives_sparse_checkout_staging_reset():
    source = INSTALLER.read_text(encoding="utf-8")
    start = source.index("install_prebuilt_release() {")
    end = source.index("\n}\n\n# Direct-upload fast path", start)
    install = source[start:end]

    checkout = install.index('_sparse_fetch_file "$repo" "$tmp"')
    key = install.index("'-----BEGIN PUBLIC KEY-----'", checkout)
    verify = install.index("_verify_release_metadata", key)
    assert 'builtin_release_trust=1' in install[:checkout]
    assert checkout < key < verify


def test_ignores_other_platform_entries():
    # Manifest has no line for this platform → prebuilt path declines (rc != 0).
    payload = b"irrelevant"
    other = hashlib.sha256(payload).hexdigest()
    details = {}
    rc, installed, _, _ = _run(
        "%s  forkmesh-windows-x86_64.exe\n" % other, payload, details=details)
    assert rc != 0
    assert installed is None
    # Only authenticated metadata without our asset may be reported as
    # "no prebuilt is published".
    assert details["kind"] == "no-asset"


# --- mirror blips and failure causes (2026-09-12 install failure) ------------
# forkmesh.com redirects each git request to one of several mirrors; a lazy
# blob fetch that hits a restarting gateway (503) used to end the install with
# a false "No prebuilt ForkMesh binary is published".


def test_metadata_fetch_retries_through_a_mirror_blip():
    payload = b"\x7fELF survives a gateway restart" * 20
    digest = hashlib.sha256(payload).hexdigest()
    details = {}
    # Both clone modes of the first attempt fail; the second attempt succeeds.
    rc, installed, stderr, _ = _run(
        "%s  forkmesh-linux-x86_64\n" % digest, payload, git_failures=2,
        details=details)
    assert rc == 0, stderr
    assert installed == payload
    assert "Retrying in 0s (attempt 2 of 3)" in stderr
    assert "returned error: 503" in stderr


def test_unreachable_metadata_is_not_reported_as_missing_release():
    payload = b"\x7fELF"
    digest = hashlib.sha256(payload).hexdigest()
    details = {}
    rc, installed, stderr, blob_url = _run(
        "%s  forkmesh-linux-x86_64\n" % digest, payload, git_failures=1000,
        details=details)
    assert rc != 0
    assert installed is None
    assert blob_url is None
    assert details == {"kind": "metadata-unreachable", "attempts": 3}
    # Bounded: exactly one announcement per re-attempt, nothing more.
    assert stderr.count("Retrying in") == 2


def test_default_metadata_retry_window_is_bounded():
    source = INSTALLER.read_text(encoding="utf-8")
    marker = 'FORKMESH_METADATA_RETRY_DELAYS="${FORKMESH_METADATA_RETRY_DELAYS-'
    start = source.index(marker) + len(marker)
    delays = [int(d) for d in source[start:source.index("}", start)].split()]
    assert 30 <= sum(delays) <= 45
    assert len(delays) <= 4


def test_missing_checksum_list_is_authoritative_and_not_retried():
    details = {}
    rc, installed, stderr, _ = _run(
        "", b"x", no_sums=True, details=details)
    assert rc != 0
    assert installed is None
    assert details["kind"] == "no-release"
    assert "Retrying in" not in stderr


def test_missing_git_is_reported_as_such():
    details = {}
    rc, installed, _, _ = _run("", b"x", no_git=True, details=details)
    assert rc != 0
    assert installed is None
    assert details["kind"] == "git-missing"


def test_blob_download_failure_is_retried_and_reported():
    payload = b"\x7fELF"
    digest = hashlib.sha256(payload).hexdigest()
    details = {}
    rc, installed, stderr, _ = _run(
        "%s  forkmesh-linux-x86_64\n" % digest, payload, curl_fails=True,
        details=details)
    assert rc != 0
    assert installed is None
    assert details["kind"] == "asset-download"
    assert "retrying in 0s (attempt 2 of 2)" in stderr


def _dispatch_block():
    """The prebuilt dispatch: failure explainers through the if-chain."""
    script = INSTALLER.read_text(encoding="utf-8")
    start = script.index("_prebuilt_metadata_unavailable() {")
    end = script.index("\n# Everything from here to the build retry", start)
    return script[start:end]


DISPATCH_PREAMBLE = """
set -euo pipefail
say()  { printf 'SAY:%s\\n' "$1"; }
diag() { printf 'DIAG:%s:%s:%s\\n' "$1" "$2" "${3:-}"; }
die_diag() {
  printf 'DIAG_SYNC:%s:%s\\n' "$1" "$2"
  printf 'ERR:%s\\n' "$3"
  exit 1
}
detect_release_asset() { :; }
ensure_qt_runtime() { :; }
install_local_binary() { return 1; }
install_prebuilt_release() {
  RELEASE_PREBUILT_FAILURE="$KIND"
  RELEASE_CANDIDATE_FAILURE="$CANDIDATE_FAILURE"
  RELEASE_FRESHNESS_MATCH=0
  RELEASE_METADATA_REASON="The requested URL returned error: 503"
  RELEASE_METADATA_ATTEMPTS=4
  return 1
}
ASSET_OS="linux"; ASSET_ARCH="x86_64"; ASSET_NAME="forkmesh-linux-x86_64"
RELEASE_CHANNEL="latest"
FORKMESH_HOST="https://forkmesh.com"
REPO_CANDIDATES=("https://forkmesh.com/forkmesh/forkmesh")
BIN_DIR="/home/u/.local/bin"; BIN="$BIN_DIR/forkmesh"
INSTALLED_PREBUILT=0
CURRENT_STEP=""
"""


def _dispatch(kind, *, fallback="1", commit="", candidate_failure="0"):
    env = os.environ.copy()
    env.update({
        "KIND": kind,
        "CANDIDATE_FAILURE": candidate_failure,
        "FORKMESH_NO_SOURCE_FALLBACK": fallback,
        "FORKMESH_EXPECTED_BUILD_COMMIT": commit,
    })
    env.pop("FORKMESH_FROM_SOURCE", None)
    proc = subprocess.run(
        ["bash", "-c", DISPATCH_PREAMBLE + _dispatch_block() + "\n"],
        env=env, text=True, capture_output=True)
    return proc.returncode, proc.stdout


def test_dispatch_metadata_unreachable_says_retry_not_unpublished():
    rc, out = _dispatch("metadata-unreachable")
    assert rc == 1
    assert "DIAG_SYNC:prebuilt:metadata-unreachable" in out
    assert "Could not reach a ForkMesh mirror to read the release metadata" in out
    assert "returned error: 503" in out
    assert "retry in a minute" in out
    assert "No prebuilt ForkMesh binary is published" not in out


def test_dispatch_unreachable_metadata_is_not_a_commit_mismatch():
    rc, out = _dispatch("metadata-unreachable", commit="ab" * 20)
    assert rc == 1
    assert "DIAG_SYNC:prebuilt:metadata-unreachable" in out
    assert "matches required build commit" not in out


def test_dispatch_git_missing():
    rc, out = _dispatch("git-missing")
    assert rc == 1
    assert "DIAG_SYNC:prebuilt:git-missing" in out
    assert "git is required to install the prebuilt ForkMesh binary" in out
    assert "No prebuilt ForkMesh binary is published" not in out


def test_dispatch_no_asset_keeps_the_published_wording():
    # desktop/src/ControlNode.cpp vultrInstallNeedsLocalBinary matches this.
    rc, out = _dispatch("no-asset")
    assert rc == 1
    assert "DIAG_SYNC:prebuilt:no-asset" in out
    assert "No prebuilt ForkMesh binary is published for linux/x86_64." in out


def test_dispatch_authentication_failure_outranks_cause_tracking():
    rc, out = _dispatch("metadata-unreachable", candidate_failure="1")
    assert rc == 1
    assert "DIAG_SYNC:prebuilt:release-authentication" in out
    assert "No prebuilt ForkMesh release passed" in out


def test_dispatch_source_fallback_explains_cause_and_continues():
    rc, out = _dispatch("asset-download", fallback="0")
    assert rc == 0
    assert "DIAG:prebuilt:0:asset-download" in out
    assert "building from source" in out
    assert "DIAG_SYNC" not in out


def test_blob_url_uses_manifest_repo_not_mirror():
    # The clone came from the mirror "alice/forkmesh" (REPO_CANDIDATES), but the
    # release.json manifest records the canonical "forkmesh/forkmesh" — the only
    # node that holds the out-of-git blob. The blob URL MUST target the canonical
    # repo, otherwise the mirror 404s and a published binary falls back to source.
    payload = b"\x7fELF prebuilt" * 20
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, repo="forkmesh/forkmesh")
    rc, installed, _, blob_url = _run(
        sums, payload, manifest=manifest)
    assert rc == 0
    assert installed == payload
    assert blob_url == (
        "https://relay.test/api/repo/forkmesh/forkmesh"
        "/releases/blob/sha256/%s" % digest)


def test_blob_url_uses_authenticated_mirror_repo_when_manifest_records_it():
    payload = b"\x7fELF legacy" * 20
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, repo="alice/forkmesh")
    rc, installed, _, blob_url = _run(sums, payload, manifest=manifest)
    assert rc == 0
    assert installed == payload
    assert blob_url == (
        "https://relay.test/api/repo/alice/forkmesh"
        "/releases/blob/sha256/%s" % digest)


def test_commit_pinned_install_accepts_exact_release_revision():
    commit = "a1" * 20
    payload = (
        "#!/bin/sh\n"
        'case "$1" in\n'
        '  --version) printf "ForkMesh 0.7.0\\n" ;;\n'
        '  --build-commit) printf "%s\\n" ;;\n'
        "  *) exit 2 ;;\n"
        "esac\n" % commit
    ).encode()
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, build_commit=commit)
    rc, installed, stderr, _ = _run(
        sums,
        payload,
        manifest=manifest,
        expected_build_commit=commit,
        expected_version="0.7.0",
        existing_binary=b"old-known-good-binary",
    )
    assert rc == 0, stderr
    assert installed == payload


def test_pinned_reinstall_defers_destruction_until_verified_stage():
    source = INSTALLER.read_text(encoding="utf-8")
    dispatch = source[source.index('if [ "$FORKMESH_REINSTALL" = "1" ]'): ]
    assert 'if [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ]; then' in dispatch
    assert "DEFER_PINNED_REINSTALL=1" in dispatch
    install_start = source.index("_install_binary() {")
    install_end = source.index("\n}\n", install_start)
    install_fn = source[install_start:install_end]
    assert install_fn.index('staged="$(mktemp') < install_fn.index(
        "uninstall_forkmesh reinstall"
    )
    assert install_fn.index("uninstall_forkmesh reinstall") < install_fn.index(
        'mv -f "$staged" "$BIN"'
    )


def test_commit_pinned_install_rejects_stale_same_semver_release():
    # Version strings are deliberately absent from this gate: two v0.7.0
    # artifacts can be different.  Even with valid bytes/checksum, release.json
    # from an older commit must be rejected before installation.
    payload = b"\x7fELF stale but checksum-valid v0.7.0 artifact" * 20
    digest = hashlib.sha256(payload).hexdigest()
    current_commit = "b2" * 20
    stale_commit = "c3" * 20
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, build_commit=stale_commit)
    rc, installed, stderr, blob_url = _run(
        sums,
        payload,
        manifest=manifest,
        expected_build_commit=current_commit,
        expected_version="0.7.0",
    )
    assert rc != 0
    assert installed is None
    assert blob_url is None
    assert "does not match required build commit" in stderr


def test_commit_pinned_install_rejects_manifest_free_legacy_release():
    payload = b"\x7fELF unprovable legacy artifact" * 20
    digest = hashlib.sha256(payload).hexdigest()
    rc, installed, stderr, blob_url = _run(
        "%s  forkmesh-linux-x86_64\n" % digest,
        payload,
        trust_manifest=False,
        expected_build_commit="d4" * 20,
        expected_version="0.7.0",
    )
    assert rc != 0
    assert installed is None
    assert blob_url is None
    assert "signature is missing or invalid" in stderr.lower()


def test_authenticated_candidate_is_never_executed(tmp_path):
    required = "e5" * 20
    marker = tmp_path / "candidate-ran"
    payload = (
        "#!/bin/sh\n"
        'printf ran > "$FORKMESH_CANDIDATE_MARKER"\n'
        "exit 99\n"
    ).encode()
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, build_commit=required)
    old = os.environ.get("FORKMESH_CANDIDATE_MARKER")
    os.environ["FORKMESH_CANDIDATE_MARKER"] = str(marker)
    rc, installed, stderr, _ = _run(
        sums,
        payload,
        manifest=manifest,
        expected_build_commit=required,
        expected_version="0.7.0",
    )
    if old is None:
        os.environ.pop("FORKMESH_CANDIDATE_MARKER", None)
    else:
        os.environ["FORKMESH_CANDIDATE_MARKER"] = old
    assert rc == 0, stderr
    assert installed == payload
    assert not marker.exists()


def test_authenticated_manifest_version_mismatch_preserves_installed_binary():
    required = "a8" * 20
    payload = (
        "#!/bin/sh\n"
        'case "$1" in\n'
        '  --version) printf "ForkMesh 0.6.20\\n" ;;\n'
        '  --build-commit) printf "%s\\n" ;;\n'
        "  *) exit 2 ;;\n"
        "esac\n" % required
    ).encode()
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(
        sums, build_commit=required, version="0.6.20"
    )
    old_binary = b"old-known-good-binary"
    rc, installed, stderr, _ = _run(
        sums,
        payload,
        manifest=manifest,
        expected_build_commit=required,
        expected_version="0.7.0",
        existing_binary=old_binary,
    )
    assert rc != 0
    assert installed == old_binary
    assert "does not match required version" in stderr


def test_commit_pinned_install_requires_a_working_checksum_tool():
    required = "ca" * 20
    payload = (
        "#!/bin/sh\n"
        'case "$1" in\n'
        '  --version) printf "ForkMesh 0.7.0\\n" ;;\n'
        '  --build-commit) printf "%s\\n" ;;\n'
        "  *) exit 2 ;;\n"
        "esac\n" % required
    ).encode()
    digest = hashlib.sha256(payload).hexdigest()
    sums = "%s  forkmesh-linux-x86_64\n" % digest
    manifest = _signed_manifest_contract(sums, build_commit=required)
    old_binary = b"old-known-good-binary"
    rc, installed, stderr, _ = _run(
        sums,
        payload,
        manifest=manifest,
        expected_build_commit=required,
        expected_version="0.7.0",
        existing_binary=old_binary,
        no_checksum_tool=True,
    )
    assert rc != 0
    assert installed == old_binary
    assert "sha-256 tool is required" in stderr.lower()


def test_exact_staged_copy_is_checked_before_atomic_swap(tmp_path):
    out_dir = tmp_path / "out"
    bin_dir = out_dir / "bin"
    bin_dir.mkdir(parents=True)
    old_binary = b"old-known-good-binary"
    (bin_dir / "forkmesh").write_bytes(old_binary)
    candidate = tmp_path / "candidate"
    candidate.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    candidate.chmod(0o755)
    digest = hashlib.sha256(candidate.read_bytes()).hexdigest()

    stub_bin = tmp_path / "stubbin"
    stub_bin.mkdir()
    # Simulate bytes changing during the candidate -> destination-filesystem
    # copy. Verification must run on the staged path, not only on candidate.
    (stub_bin / "install").write_text(
        """#!/bin/sh
dest=""
for arg in "$@"; do dest="$arg"; done
printf '#!/bin/sh\\nexit 99\\n' > "$dest"
chmod 0755 "$dest"
""",
        encoding="utf-8",
    )
    (stub_bin / "install").chmod(0o755)
    env = os.environ.copy()
    env.update(
        {
            "OUT_DIR": str(out_dir),
            "CANDIDATE": str(candidate),
            "EXPECTED_HASH": digest,
            "FORKMESH_EXPECTED_BUILD_COMMIT": "ab" * 20,
            "FORKMESH_EXPECTED_RELEASE_VERSION": "0.7.0",
            "PATH": str(stub_bin) + os.pathsep + env["PATH"],
        }
    )
    script = (
        PREAMBLE
        + _release_functions()
        + '\nif _install_binary "$CANDIDATE" "$EXPECTED_HASH"; then exit 9; fi\n'
    )
    proc = subprocess.run(
        ["bash", "-c", script], env=env, text=True, capture_output=True
    )
    assert proc.returncode == 0, proc.stderr
    assert (bin_dir / "forkmesh").read_bytes() == old_binary
    assert "Staged ForkMesh binary failed its SHA-256 check" in proc.stderr


# --- direct-upload install (adhoc #67) ---------------------------------------
# The desktop app's Hosts panel can stream the release binary over the SSH
# session and hand it to the installer as FORKMESH_LOCAL_BINARY (with the
# uploader's platform in FORKMESH_LOCAL_OS/ARCH). install_local_binary() must
# install a matching upload as-is and decline — falling back to the relay
# download — on a platform mismatch or a missing/empty upload.


def _local_binary_function():
    """Extract install_local_binary() from install.sh."""
    script = INSTALLER.read_text(encoding="utf-8")
    start = script.index("install_local_binary() {")
    end = script.index("\n}\n", start) + len("\n}\n")
    return script[start:end]


def _run_local(payload, *, os_decl="", arch_decl="", missing=False, pinned=True):
    """Run install_local_binary against an uploaded temp file.

    Returns (rc, installed BIN bytes or None, stderr). The fake environment
    matches _run's PREAMBLE: the target machine is linux/x86_64.
    """
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        upload = tmp / "upload.bin"
        if not missing:
            upload.write_bytes(payload)
        out_dir = tmp / "out"
        out_dir.mkdir()
        env = os.environ.copy()
        env["OUT_DIR"] = str(out_dir)
        script = (
            PREAMBLE
            + 'FORKMESH_LOCAL_BINARY="%s"\n' % upload
            + 'FORKMESH_LOCAL_OS="%s"\n' % os_decl
            + 'FORKMESH_LOCAL_ARCH="%s"\n' % arch_decl
            + 'FORKMESH_LOCAL_BINARY_SHA256="%s"\n'
            % (hashlib.sha256(payload).hexdigest() if pinned else "")
            + _release_functions()  # provides _install_binary
            + _local_binary_function()
            + "\ninstall_local_binary\n")
        proc = subprocess.run(
            ["bash", "-c", script], env=env, text=True, capture_output=True)
        bin_path = out_dir / "bin" / "forkmesh"
        installed = bin_path.read_bytes() if bin_path.exists() else None
        return proc.returncode, installed, proc.stderr


def test_local_binary_installs_matching_upload():
    payload = b"\x7fELF uploaded forkmesh binary" * 40
    rc, installed, _ = _run_local(payload, os_decl="linux", arch_decl="x86_64")
    assert rc == 0
    assert installed == payload


def test_local_binary_installs_without_declared_platform():
    # Platform declaration is optional, but the controller digest is not.
    payload = b"\x7fELF undeclared platform upload"
    rc, installed, _ = _run_local(payload)
    assert rc == 0
    assert installed == payload


def test_local_binary_rejects_missing_controller_digest():
    payload = b"\x7fELF unauthenticated upload"
    rc, installed, stderr = _run_local(payload, pinned=False)
    assert rc != 0
    assert installed is None
    assert "controller-pinned SHA-256" in stderr


def test_local_binary_rejects_wrong_os():
    payload = b"\xcf\xfa\xed\xfe mach-o bytes"
    rc, installed, stderr = _run_local(
        payload, os_decl="macos", arch_decl="x86_64")
    assert rc != 0
    assert installed is None
    assert "falling back" in stderr


def test_local_binary_rejects_wrong_arch():
    payload = b"\x7fELF arm bytes"
    rc, installed, stderr = _run_local(
        payload, os_decl="linux", arch_decl="arm64")
    assert rc != 0
    assert installed is None
    assert "falling back" in stderr


def test_local_binary_missing_or_empty_upload_declines():
    rc, installed, stderr = _run_local(b"", missing=True)
    assert rc != 0
    assert installed is None
    assert "falling back" in stderr
    rc, installed, _ = _run_local(b"")  # exists but empty
    assert rc != 0
    assert installed is None


def _rebuilt_manifest(sums_line, release_commit, asset_commit):
    return (
        '{"schema":"forkmesh-release-v2","repo":"forkmesh/forkmesh",'
        '"tag":"v0.7.0","tag_commit":"%s","build_commit":"%s",'
        '"channel":"latest","checksums_sha256":"%s",'
        '"assets":[{"name":"forkmesh-linux-x86_64","build":2,"commit":"%s"},'
        '{"name":"forkmesh-linux-x86_64.AppImage"}]}'
        % ("f0" * 20, release_commit, hashlib.sha256(sums_line.encode()).hexdigest(),
           asset_commit)
    )


def test_commit_pinned_install_uses_a_rebuilt_assets_own_commit():
    rebuilt = "d4" * 20
    payload = (
        "#!/bin/sh\n"
        'case "$1" in\n'
        '  --version) printf "ForkMesh 0.7.0\\n" ;;\n'
        '  --build-commit) printf "%s\\n" ;;\n'
        "  *) exit 2 ;;\n"
        "esac\n" % rebuilt
    ).encode()
    sums = "%s  forkmesh-linux-x86_64\n" % hashlib.sha256(payload).hexdigest()
    manifest = _rebuilt_manifest(sums, "a1" * 20, rebuilt.upper())
    rc, installed, stderr, _ = _run(sums, payload, manifest=manifest,
                                    expected_build_commit=rebuilt, expected_version="0.7.0")
    assert rc == 0, stderr
    assert installed == payload
    rc, installed, stderr, _ = _run(sums, payload, manifest=manifest,
                                    expected_build_commit="a1" * 20,
                                    expected_version="0.7.0")
    assert rc != 0 and installed is None
    assert "does not match required build commit" in stderr
