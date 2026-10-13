"""Safety contract for deploy.sh's explicit same-version binary refresh."""

import hashlib
import json
from pathlib import Path
import subprocess


DEPLOY = Path(__file__).resolve().parents[1] / "deploy.sh"
PUBLISH = DEPLOY.parents[1] / "tools" / "forkmesh-release-publish.sh"
WORKFLOW = DEPLOY.parents[1] / ".forkmesh" / "release.yml"


def _source() -> str:
    return DEPLOY.read_text(encoding="utf-8")


def _publish_function() -> str:
    source = _source()
    start = source.index("publish_release_binary() {")
    end = source.index("\n}\n\ncommit_release_metadata()", start)
    return source[start:end]


def _release_signing_key(directory: Path) -> Path:
    key = directory / "release-signing-key.pem"
    subprocess.run(
        ["openssl", "genpkey", "-algorithm", "Ed25519", "-out", str(key)],
        check=True,
    )
    return key


def test_deploy_script_has_valid_shell_syntax():
    result = subprocess.run(
        ["bash", "-n", str(DEPLOY)],
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr


def test_only_explicit_command_forces_existing_asset():
    source = _source()
    function = _publish_function()

    assert 'local force="${1:-0}"' in function
    assert 'if [ "$force" != "1" ]; then' in function
    assert "publish_release_binary 0" not in source
    assert "republish-release-binary)" in source
    assert "publish_release_binary 1" in source
    assert source.count("publish_release_binary 1") == 1
    assert source.index("republish-release-binary)") < source.index(
        "publish_release_binary 1", source.index("republish-release-binary)")
    )


def test_forced_refresh_is_clean_binary_only_and_served_cas_bound():
    function = _publish_function()

    cas_guard = (
        'if [ -z "${FORKMESH_RELEASE_CAS:-}" ]; then'
    )
    clean_guard = "git -C .. status --porcelain=v1"
    configure = "cmake -S ../desktop -B ../desktop/build-release"
    assert cas_guard in function
    assert clean_guard in function
    assert "--untracked-files=all" in function
    assert "could not verify that the release source worktree is clean" in function
    assert function.index(cas_guard) < function.index(configure)
    assert function.index(clean_guard) < function.index(configure)

    assert '-DFORKMESH_BUILD_TESTS=OFF' in function
    assert '-DFORKMESH_VERSION_OVERRIDE="$release_version"' in function
    assert '-DFORKMESH_BUILD_COMMIT_OVERRIDE="$build_commit"' in function
    assert ":(exclude).forkmesh/releases/**" in function
    assert "git -C .. log -1 --format=%H" in function
    assert '"$built" --version' in function
    assert '"$built" --build-commit' in function
    assert '"ForkMesh ${release_version}"' in function
    assert '[ "$reported_commit" != "$build_commit" ]' in function


def test_refresh_publishes_from_repo_root_and_verifies_revision_hash_and_cas():
    function = _publish_function()

    assert "cd .. && tools/forkmesh-release-publish.sh" in function
    assert 'publish_args+=("app/$asset")' in function
    assert 'publish_args+=("app/$mirror_asset")' in function
    assert '--tag-commit "$tag_commit"' in function
    assert '--build-commit "$build_commit"' in function
    assert '\\"tag_commit\\": \\"$tag_commit\\"' in function
    assert '\\"build_commit\\": \\"$build_commit\\"' in function
    assert '\\"name\\":\\"$asset\\",\\"blob_sha256\\":\\"$asset_hash\\"' in function
    assert (
        '$cas_dir/sha256/${asset_hash:0:2}/$asset_hash/data'
        in function
    )
    assert "git add ../.forkmesh/releases/latest/SHASUMS256.txt" in function
    assert "../.forkmesh/releases/latest/release.json.sig" in function


def test_publisher_records_explicit_binary_source_commit(tmp_path):
    artifact = tmp_path / "forkmesh-linux-x86_64"
    payload = b"\x7fELF commit-bound release test"
    artifact.write_bytes(payload)
    tag_commit = "d4" * 20
    build_commit = "e5" * 20
    signing_key = _release_signing_key(tmp_path)
    result = subprocess.run(
        [
            str(PUBLISH),
            "--channel",
            "latest",
            "--tag",
            "v0.7.0",
            "--tag-commit",
            tag_commit,
            "--build-commit",
            build_commit,
            "--repo",
            "forkmesh/forkmesh",
            "--cas-dir",
            str(tmp_path / "cas"),
            "--signing-key",
            str(signing_key),
            str(artifact),
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    manifest = json.loads(
        (tmp_path / ".forkmesh/releases/latest/release.json").read_text()
    )
    assert manifest["tag_commit"] == tag_commit
    assert manifest["build_commit"] == build_commit
    assert manifest["assets"][0]["blob_sha256"] == hashlib.sha256(payload).hexdigest()


def test_publisher_labels_the_companion_asset_platform(tmp_path):
    """One publish call carries the client plus the Go mirror-node companion.

    The updater filters manifest assets by os/arch before splitting client vs
    companion by name prefix, so the companion's platform fields must be parsed
    from the END of `forkmesh-mirror-node-<os>-<arch>` — a forward
    `forkmesh-<os>-<arch>` split would label it os="mirror" and the companion
    would never be offered to any platform."""
    client = tmp_path / "forkmesh-linux-x86_64"
    client.write_bytes(b"\x7fELF client release test")
    companion = tmp_path / "forkmesh-mirror-node-linux-x86_64"
    companion_payload = b"\x7fELF go mirror-node companion"
    companion.write_bytes(companion_payload)
    signing_key = _release_signing_key(tmp_path)
    result = subprocess.run(
        [
            str(PUBLISH),
            "--channel",
            "latest",
            "--tag",
            "v0.7.0",
            "--tag-commit",
            "d4" * 20,
            "--build-commit",
            "e5" * 20,
            "--repo",
            "forkmesh/forkmesh",
            "--cas-dir",
            str(tmp_path / "cas"),
            "--signing-key",
            str(signing_key),
            str(client),
            str(companion),
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    manifest = json.loads(
        (tmp_path / ".forkmesh/releases/latest/release.json").read_text()
    )
    by_name = {asset["name"]: asset for asset in manifest["assets"]}
    assert set(by_name) == {
        "forkmesh-linux-x86_64",
        "forkmesh-mirror-node-linux-x86_64",
    }
    mirror = by_name["forkmesh-mirror-node-linux-x86_64"]
    assert mirror["os"] == "linux"
    assert mirror["arch"] == "x86_64"
    assert mirror["blob_sha256"] == hashlib.sha256(companion_payload).hexdigest()
    sums = (
        tmp_path / ".forkmesh/releases/latest/SHASUMS256.txt"
    ).read_text()
    assert f"{mirror['blob_sha256']}  forkmesh-mirror-node-linux-x86_64" in sums


def test_publisher_rejects_unprovable_source_commit(tmp_path):
    artifact = tmp_path / "forkmesh-linux-x86_64"
    artifact.write_bytes(b"\x7fELF invalid provenance")
    result = subprocess.run(
        [
            str(PUBLISH),
            "--tag",
            "v0.7.0",
            "--tag-commit",
            "a1" * 20,
            "--build-commit",
            "not-a-commit",
            "--cas-dir",
            str(tmp_path / "cas"),
            str(artifact),
        ],
        cwd=tmp_path,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0
    assert "--build-commit must identify" in result.stderr
    assert not (tmp_path / ".forkmesh/releases/latest/release.json").exists()


def test_repo_root_build_revision_includes_qt_only_commits(tmp_path):
    repo = tmp_path / "repo"
    worker = repo / "cloudflare_worker"
    qt = repo / "desktop"
    worker.mkdir(parents=True)
    qt.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.email", "test@example.invalid"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.name", "ForkMesh Test"],
        check=True,
    )
    (worker / "deploy.sh").write_text("first\n")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "commit", "-qm", "worker"], check=True
    )
    worker_commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
    ).strip()
    (qt / "CMakeLists.txt").write_text("second\n")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-qm", "qt"], check=True)
    qt_commit = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
    ).strip()

    wrong_cwd_revision = subprocess.check_output(
        ["git", "-C", str(worker), "log", "-1", "--format=%H", "--", "."],
        text=True,
    ).strip()
    root_revision = subprocess.check_output(
        [
            "git",
            "-C",
            str(repo),
            "log",
            "-1",
            "--format=%H",
            "--",
            ".",
            ":(exclude).forkmesh/releases/**",
        ],
        text=True,
    ).strip()
    assert wrong_cwd_revision == worker_commit
    assert root_revision == qt_commit


def test_publisher_rejects_tag_commit_that_is_not_the_peeled_tag(tmp_path):
    repo = tmp_path / "repo"
    repo.mkdir()
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.email", "test@example.invalid"],
        check=True,
    )
    subprocess.run(
        ["git", "-C", str(repo), "config", "user.name", "ForkMesh Test"],
        check=True,
    )
    artifact = repo / "forkmesh-linux-x86_64"
    artifact.write_bytes(b"\x7fELF tag target verification")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(["git", "-C", str(repo), "commit", "-qm", "release"], check=True)
    subprocess.run(
        ["git", "-C", str(repo), "tag", "-a", "v0.7.0", "-m", "release"],
        check=True,
    )
    target = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
    ).strip()
    wrong = "f" * 40 if target != "f" * 40 else "e" * 40
    signing_key = _release_signing_key(repo)

    result = subprocess.run(
        [
            str(PUBLISH),
            "--tag",
            "v0.7.0",
            "--tag-commit",
            wrong,
            "--build-commit",
            target,
            "--cas-dir",
            str(tmp_path / "cas"),
            "--signing-key",
            str(signing_key),
            str(artifact),
        ],
        cwd=repo,
        text=True,
        capture_output=True,
        check=False,
    )
    assert result.returncode != 0
    assert "does not match the peeled target" in result.stderr
    assert not (repo / ".forkmesh/releases/latest/release.json").exists()

    accepted = subprocess.run(
        [
            str(PUBLISH),
            "--tag",
            "v0.7.0",
            "--build-commit",
            target,
            "--cas-dir",
            str(tmp_path / "cas"),
            "--signing-key",
            str(signing_key),
            str(artifact),
        ],
        cwd=repo,
        text=True,
        capture_output=True,
        check=False,
    )
    assert accepted.returncode == 0, accepted.stderr
    manifest = json.loads(
        (repo / ".forkmesh/releases/latest/release.json").read_text()
    )
    assert manifest["tag_commit"] == target


def test_tag_commit_has_one_definition_matching_the_publish_flow():
    root = DEPLOY.parents[1]
    readme = " ".join(
        (root / ".forkmesh/releases/README.md").read_text(encoding="utf-8").split()
    )
    assert "This is the one definition of `tag_commit`" in readme
    assert "git merge-base --is-ancestor <tag_commit> <tag>" in readme
    for path in (
        ".forkmesh/releases/README.md",
        "www/public/install.sh",
        "tools/forkmesh-release-publish.sh",
        "app/src/releases.py",
    ):
        text = " ".join((root / path).read_text(encoding="utf-8").split())
        assert "remains the peeled" not in text, path
        assert "peeled-tag-target" not in text, path
        assert "Peeled commit targeted by" not in text, path
    publisher = " ".join(
        (root / "tools/forkmesh-release-publish.sh").read_text(encoding="utf-8").split()
    )
    assert ".forkmesh/releases/README.md for the definition" in publisher


def test_release_workflow_uses_the_tag_version_and_verifies_binary():
    workflow = WORKFLOW.read_text(encoding="utf-8")
    assert "The tag is the release version" in workflow
    assert "does not match committed project version" not in workflow
    assert "Commit the version bump before creating the release tag" not in workflow
    assert '"$built" --version' in workflow
    assert '"$built" --build-commit' in workflow
    assert '[ "$reported_version" != "ForkMesh $version" ]' in workflow
    assert '[ "$reported_commit" != "$revision" ]' in workflow
    assert 'release_tag="${FORKMESH_TAG:-v${project_version}}"' in workflow
    assert '"refs/tags/${release_tag}^{commit}"' in workflow
    assert workflow.count('--tag "$release_tag"') == 2
    assert '--tag "${FORKMESH_TAG:-}"' not in workflow
    assert "Sync the version header to the release tag" not in workflow
    assert "sed -i.bak" not in workflow


def test_release_workflow_resolves_the_signing_key_before_building():
    """A missing key must fail in seconds, not after the whole client compiles.

    The Actions sandbox mounts no host filesystem, so a secret holding a key
    *path* names nothing inside the run: the key material itself arrives through
    the variables channel and is materialized 0600 in the sandbox.
    """
    workflow = WORKFLOW.read_text(encoding="utf-8")

    # Bare reference — that is what exposes the variable to the sandbox.
    assert 'signing_key_pem="$FORKMESH_RELEASE_SIGNING_KEY_PEM"' in workflow
    assert "(umask 077; printf '%b\\n' \"$signing_key_pem\"" in workflow
    assert "openssl pkeyutl -sign -rawin -inkey \"$signing_key\"" in workflow
    assert '--signing-key "$signing_key"' in workflow
    assert "FORKMESH_RELEASE_SIGNING_KEY must point to" not in workflow
    assert workflow.index('signing_key="${FORKMESH_RELEASE_SIGNING_KEY:-}"') < (
        workflow.index("cmake -S desktop -B desktop/build-release")
    )


def test_release_signing_preflight_accepts_only_a_usable_ed25519_key(tmp_path):
    """Run the workflow's own preflight snippet against real keys."""
    workflow = WORKFLOW.read_text(encoding="utf-8")
    body = workflow.split("run: |\n", 1)[1]
    body = "\n".join(
        line[10:] if line.startswith(" " * 10) else line
        for line in body.split("\n")
    )
    start = body.index("# Resolve the release signing key")
    snippet = "set -e\n" + body[start : body.index("# Map this host")]

    def run(env: dict) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["bash", "-c", snippet],
            cwd=tmp_path,
            env={"PATH": "/usr/bin:/bin", "TMPDIR": str(tmp_path), **env},
            text=True,
            capture_output=True,
            check=False,
        )

    key = _release_signing_key(tmp_path)
    pem = key.read_text(encoding="utf-8")

    assert run({"FORKMESH_RELEASE_SIGNING_KEY": str(key)}).returncode == 0
    assert run({"FORKMESH_RELEASE_SIGNING_KEY_PEM": pem}).returncode == 0
    # The variables dialog is single-line: \n-escaped PEM must work too.
    escaped = run({"FORKMESH_RELEASE_SIGNING_KEY_PEM": pem.replace("\n", "\\n")})
    assert escaped.returncode == 0, escaped.stderr

    missing = run({})
    assert missing.returncode != 0
    assert "no Ed25519 release signing key is configured" in missing.stderr

    rsa = tmp_path / "rsa.pem"
    subprocess.run(
        ["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
         "rsa_keygen_bits:2048", "-out", str(rsa)],
        check=True,
        capture_output=True,
    )
    wrong = run({"FORKMESH_RELEASE_SIGNING_KEY": str(rsa)})
    assert wrong.returncode != 0
    assert "not a usable Ed25519 private key" in wrong.stderr


def test_release_build_parallelism_follows_the_sandbox_budget():
    """The Actions cgroup, not the host, decides how many compilers fit.

    `nproc` inside the sandbox reports every host core: oversubscribing the CPU
    quota ran the build into the step deadline (every release after v0.7.0 was
    SIGTERMed mid-compile), and one g++ per host core can exceed MemoryMax.
    """
    workflow = WORKFLOW.read_text(encoding="utf-8")

    assert 'jobs="${FORKMESH_ACTIONS_CPUS:-}"' in workflow
    assert 'mem_mb="${FORKMESH_ACTIONS_MEMORY_MB:-0}"' in workflow
    assert "mem_jobs=$(((mem_mb - 2048) / 2048))" in workflow
    assert '[ "$jobs" -gt "$mem_jobs" ] && jobs="$mem_jobs"' in workflow
    assert '[ "$jobs" -gt 4 ] && jobs=4' in workflow
    assert workflow.index('jobs="${FORKMESH_ACTIONS_CPUS:-}"') < workflow.index(
        'cmake --build desktop/build-release -j"$jobs"'
    )


def test_local_release_republish_preserves_live_host_memory():
    function = _publish_function()

    assert '[ "$jobs" -gt 4 ] && jobs=4' in function
    assert "MemAvailable:" in function
    assert "available_kb - reserve_kb" in function
    assert function.index("MemAvailable:") < function.index(
        'cmake --build ../desktop/build-release -j"$jobs"'
    )
