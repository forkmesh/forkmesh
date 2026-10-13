import importlib.util
import os
from pathlib import Path
import subprocess


REPOSITORY = Path(__file__).resolve().parents[2]
MODULE = REPOSITORY / "app/tools/deploy_targets.py"
SPEC = importlib.util.spec_from_file_location("deploy_targets", MODULE)
deploy_targets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deploy_targets)


def manifest(target):
    return {
        path.relative_to(REPOSITORY).as_posix()
        for path in deploy_targets.target_files(target)
    }


def test_each_worker_has_distinct_inputs():
    manifests = {target: manifest(target) for target in deploy_targets.TARGETS}

    assert deploy_targets.TARGETS == ("app", "world", "www")
    assert "app/wrangler.toml" in manifests["app"]
    assert "app/migrations/0125_public_api_snapshots.sql" in manifests["app"]
    assert "app/src/entry.py" in manifests["app"]
    assert "app/public/dashboard/repo.html" in manifests["app"]
    assert "marketplace/catalog.json" in manifests["app"]
    assert "app/tools/build_app_plugins.py" in manifests["app"]
    assert "marketplace/tools/plugin_sockets.py" in manifests["app"]
    assert "marketplace/packages/capture/manifest.json" in manifests["app"]
    assert "www/wrangler.production.toml" in manifests["www"]
    assert "www/wrangler.toml" not in manifests["www"]
    assert "www/public/blog.html" in manifests["www"]
    assert "app/src/blog_feed.py" in manifests["www"]
    assert "app/src/dashboard_shell.py" in manifests["www"]
    assert "app/public/membership-prompt.js" in manifests["www"]
    assert "world/wrangler.toml" in manifests["world"]
    assert "world/public/world/world.js" in manifests["world"]
    assert (
        "marketplace/packages/x-banner-board/content/world/index.js"
        in manifests["world"]
    )
    assert (
        "marketplace/packages/reddit-banner-board/manifest.json"
        in manifests["world"]
    )
    assert "app/src/entry.py" not in manifests["world"]
    assert "app/src/entry.py" not in manifests["www"]
    assert "app/src/dashboard_shell.py" in manifests["world"]
    assert "world/public/world/world.js" not in manifests["app"]
    assert "app/.env.production" not in deploy_targets.target_inputs("app")
    assert "app/pylock.toml" not in deploy_targets.target_inputs("app")


def test_shared_input_only_invalidates_consumers():
    assert "www/public/api-client.js" in manifest("app")
    assert "www/public/api-client.js" in manifest("www")
    assert "www/public/api-client.js" in manifest("world")
    assert "www/public/_headers" in manifest("world")
    assert "www/public/home-header-auth.js" not in manifest("world")
    assert "www/public/assets/video/network.mp4" not in manifest("world")
    assert "www/public/blog/one-app-one-mesh/index.html" not in manifest("app")
    assert "www/public/blog/one-app-one-mesh/index.html" in manifest("www")
    world = manifest("world")
    assert "app/public/dashboard-chat.js" in world
    assert "app/public/account-events.js" not in world
    assert "app/public/member-welcome.js" not in world


def test_app_fingerprint_covers_staged_homepage_dependencies():
    app = manifest("app")
    assert "www/public/home-header-auth.js" in app
    assert "www/public/homepage-downloads.js" in app
    assert "www/public/assets/video/network.mp4" in app


def test_deploy_state_marks_only_successful_target(tmp_path, monkeypatch):
    state_path = tmp_path / "deploy-targets.json"
    monkeypatch.setattr(deploy_targets, "STATE_PATH", state_path)

    assert deploy_targets.target_status("world")["changed"] is True
    marked = deploy_targets.mark_deployed("world", "abc123", state_path)
    assert marked["revision"] == "abc123"
    assert deploy_targets.target_status("world")["changed"] is False
    assert deploy_targets.target_status("www")["changed"] is True


def test_fingerprint_changes_with_input(tmp_path, monkeypatch):
    source = tmp_path / "world.js"
    source.write_text("one", encoding="utf-8")
    monkeypatch.setattr(deploy_targets, "target_files", lambda target: [source])
    monkeypatch.setattr(deploy_targets, "REPOSITORY", tmp_path)

    first = deploy_targets.fingerprint("world")
    source.write_text("two", encoding="utf-8")
    assert deploy_targets.fingerprint("world") != first


def test_app_fingerprint_excludes_local_environment_secrets(tmp_path, monkeypatch):
    app = tmp_path / "app"
    app.mkdir()
    (app / "wrangler.toml").write_text("name = 'test'\n", encoding="utf-8")
    environment = app / ".env.production"
    environment.write_text("DATA_KEY=first-secret\n", encoding="utf-8")
    monkeypatch.setattr(deploy_targets, "REPOSITORY", tmp_path)

    first = deploy_targets.fingerprint("app")
    environment.write_text("DATA_KEY=second-secret\n", encoding="utf-8")

    assert deploy_targets.fingerprint("app") == first


def test_live_worker_fingerprint_is_the_cross_runner_source_of_truth():
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    entry = (REPOSITORY / "app/src/entry.py").read_text(encoding="utf-8")
    www = (REPOSITORY / "www/worker.js").read_text(encoding="utf-8")
    world = (REPOSITORY / "world/worker.js").read_text(encoding="utf-8")

    assert 'tools/deploy_targets.py fingerprint "$target"' in deploy
    assert '"deployFingerprint"' in entry
    assert "x-forkmesh-deploy-fingerprint" in www
    assert "x-forkmesh-deploy-fingerprint" in world
    assert '--var "DEPLOY_FINGERPRINT:${DEPLOY_TARGET_FINGERPRINT}"' in deploy
    assert "flagship_routing_refs_sha256()" in deploy
    assert deploy.count(
        '--var "FLAGSHIP_ROUTING_REFS_SHA256:${FLAGSHIP_ROUTING_REFS_SHA256}"'
    ) == 2
    # WWW intentionally lets `/` bypass Worker execution as a static asset;
    # deployment verification must use a Worker-first asset to read its env.
    assert '[ "$target" = "www" ] && probe_path="/rss.xml"' in deploy


def test_static_worker_skip_requires_matching_live_fingerprint(tmp_path):
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    helper_start = deploy.index("_static_fingerprint_check() {")
    helper = deploy[helper_start:deploy.index(
        "\n}\n\n_secret_bulk_error_is_transient()", helper_start
    ) + 2]
    changed = deploy[deploy.index("deploy_target_is_changed() {"):deploy.index(
        "\n}\n\nmark_deploy_target()"
    ) + 2]
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    curl = fake_bin / "curl"
    curl.write_text(
        """#!/usr/bin/env bash
case "${LIVE_PROBE_MODE:-matching}" in
  matching) printf 'HTTP/2 200\\nx-forkmesh-deploy-fingerprint: %s\\n\\n' "$LIVE_FINGERPRINT" ;;
  stale) printf 'HTTP/2 200\\nx-forkmesh-deploy-fingerprint: %064d\\n\\n' 0 ;;
  missing) printf 'HTTP/2 404\\n\\n' ;;
  unreachable) exit 28 ;;
esac
""",
        encoding="utf-8",
    )
    curl.chmod(0o755)
    fingerprint = deploy_targets.fingerprint("www")
    script = helper + "\n" + changed + "\ndeploy_target_is_changed www; exit $?\n"
    base_env = {
        **os.environ,
        "PATH": str(fake_bin) + os.pathsep + os.environ.get("PATH", ""),
        "LIVE_FINGERPRINT": fingerprint,
        "DEPLOY_VERIFY_WWW_URL": "https://www.example",
        "FORKMESH_DEPLOY_VERIFY_ATTEMPTS": "2",
        "FORKMESH_DEPLOY_VERIFY_BACKOFF": "0",
    }

    matching = subprocess.run(
        ["bash", "-c", script],
        cwd=REPOSITORY / "app",
        env=base_env,
        check=False,
    )
    assert matching.returncode == 3

    for mode in ("stale", "missing", "unreachable"):
        attempted = subprocess.run(
            ["bash", "-c", script],
            cwd=REPOSITORY / "app",
            env={**base_env, "LIVE_PROBE_MODE": mode},
            check=False,
        )
        assert attempted.returncode == 0, mode


def test_uploads_retry_only_transient_transport_and_cloudflare_api_failures():
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    helper = deploy.split("deploy_upload_with_retry() {", 1)[1].split(
        "\n}\n\npywrangler_deploy_with_retry()", 1
    )[0]
    classifier = deploy.split("_deploy_upload_is_transient() {", 1)[1].split(
        "\n}\n", 1
    )[0]
    app_deploy = deploy.split("deploy_app_version() {", 1)[1].split(
        "\n}\n\nrequire_edge_router_public_key()", 1
    )[0]
    split_deploy = deploy.split("deploy_static_target() {", 1)[1].split(
        "\n}\n", 1
    )[0]

    assert "FORKMESH_DEPLOY_UPLOAD_ATTEMPTS:-3" in helper
    assert '"$@" 2>&1 | tee "$output_file"' in helper
    assert "_deploy_upload_is_transient" in helper
    assert "pyodide-capnp-bin\\.edgeworker\\.net" in classifier
    assert "DNS lookup failed" in classifier
    # Cloudflare's API answered a read during the world upload with a 503
    # (run 8598, 2026-09-13); that is a transport hiccup, not a bad build.
    assert "Received a malformed response from the API" in classifier
    assert "upstream connect error" in classifier
    assert "Service Unavailable" in classifier
    assert "Python startup exceeded memory limits" not in classifier
    assert "Authentication error" not in classifier
    assert 'deploy_upload_with_retry pywrangler deploy "$@"' in deploy
    assert app_deploy.count("pywrangler_deploy_with_retry") == 2
    # The static asset Workers upload through plain wrangler and share the
    # same retry envelope.
    assert "deploy_upload_with_retry \\\n            npm exec" in split_deploy
    assert split_deploy.count('wrangler deploy --config "$config"') == 1
    assert '[ "$target" = "www" ] && config=wrangler.production.toml' in split_deploy


def test_deploy_recursion_keeps_an_absolute_script_path():
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    assert 'DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"' in deploy
    assert 'DEPLOY_SELF="$DEPLOY_DIR/$(basename "${BASH_SOURCE[0]}")"' in deploy
    recursive = deploy.split("deploy_changed_workers() {", 1)[1].split(
        "\n}\n\ncase ", 1
    )[0]
    assert '"$0" app' not in recursive
    assert '"$DEPLOY_SELF" app' in recursive


def test_static_fingerprint_verification_handles_rollout_and_requires_stability(
    tmp_path,
):
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    helper_start = deploy.index("_static_fingerprint_check() {")
    helper = deploy[helper_start:deploy.index(
        "\n}\n\n_secret_bulk_error_is_transient()", helper_start
    ) + 2]
    static_start = deploy.index("deploy_static_target() {")
    static = deploy[static_start:deploy.index(
        "\n}\n\ndeploy_app_version()", static_start
    )]
    changed_start = deploy.index("deploy_target_is_changed() {")
    changed = deploy[changed_start:deploy.index(
        "\n}\n\nmark_deploy_target()", changed_start
    )]
    assert '_static_fingerprint_check \\' in static
    assert '"$target" "$base" "$DEPLOY_TARGET_FINGERPRINT"' in static
    assert '_static_fingerprint_check "$target" "$base" "$current" probe' in changed

    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    curl = fake_bin / "curl"
    curl.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
seen=0
if [ -f "$FINGERPRINT_COUNTER" ]; then seen="$(<"$FINGERPRINT_COUNTER")"; fi
IFS=',' read -r -a outcomes <<<"$FINGERPRINT_SEQUENCE"
index="$seen"
if [ "$index" -ge "${#outcomes[@]}" ]; then
    index="$((${#outcomes[@]} - 1))"
fi
outcome="${outcomes[$index]}"
printf '%s' "$((seen + 1))" >"$FINGERPRINT_COUNTER"
case "$outcome" in
    new)
        printf 'HTTP/2 200\\nx-forkmesh-deploy-fingerprint: %s\\n\\n' "$EXPECTED_FINGERPRINT"
        ;;
    old)
        printf 'HTTP/2 200\\nx-forkmesh-deploy-fingerprint: %064d\\n\\n' 0
        ;;
    missing)
        printf 'HTTP/2 200\\n\\n'
        ;;
    transport)
        exit 28
        ;;
esac
""",
        encoding="utf-8",
    )
    curl.chmod(0o755)
    expected = "f" * 64
    script = helper + (
        f'\n_static_fingerprint_check world "https://world.example" "{expected}"\n'
    )
    base_env = {
        **os.environ,
        "PATH": str(fake_bin) + os.pathsep + os.environ.get("PATH", ""),
        "EXPECTED_FINGERPRINT": expected,
        "FORKMESH_DEPLOY_VERIFY_ATTEMPTS": "6",
        "FORKMESH_DEPLOY_VERIFY_BACKOFF": "0",
    }

    for name, sequence, expected_calls in (
        ("delayed", "old,new,missing,new,new", "5"),
        ("alternating", "new,old,new,new", "4"),
    ):
        counter = tmp_path / f"{name}.counter"
        result = subprocess.run(
            ["bash", "-c", script],
            cwd=REPOSITORY / "app",
            env={
                **base_env,
                "FINGERPRINT_COUNTER": str(counter),
                "FINGERPRINT_SEQUENCE": sequence,
            },
            text=True,
            capture_output=True,
            check=False,
        )
        assert result.returncode == 0, (name, result.stderr)
        assert counter.read_text(encoding="utf-8") == expected_calls

    exhausted_counter = tmp_path / "exhausted.counter"
    exhausted = subprocess.run(
        ["bash", "-c", script],
        cwd=REPOSITORY / "app",
        env={
            **base_env,
            "FINGERPRINT_COUNTER": str(exhausted_counter),
            "FINGERPRINT_SEQUENCE": "new,old,new,old,new,old",
        },
        text=True,
        capture_output=True,
        check=False,
    )
    assert exhausted.returncode != 0
    assert exhausted_counter.read_text(encoding="utf-8") == "6"
    assert "twice consecutively after 6 attempts" in exhausted.stderr
    assert expected not in exhausted.stdout + exhausted.stderr


def test_static_worker_skip_does_not_trust_local_state_without_curl(tmp_path):
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    changed = deploy[deploy.index("deploy_target_is_changed() {"):deploy.index(
        "\n}\n\nmark_deploy_target()"
    ) + 2]
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    (fake_bin / "python3").symlink_to(Path(os.environ.get("PYTHON", "/usr/bin/python3")))
    result = subprocess.run(
        ["/bin/bash", "-c", changed + "\ndeploy_target_is_changed world; exit $?\n"],
        cwd=REPOSITORY / "app",
        env={
            **os.environ,
            "PATH": str(fake_bin),
            "DEPLOY_VERIFY_WORLD_URL": "https://world.example",
        },
        check=False,
    )
    assert result.returncode == 0


def _deploy_flag_parser():
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    start = deploy.index('DEPLOY_COMMAND="${1:-deploy}"')
    end = deploy.index('set -- "$DEPLOY_COMMAND"', start)
    return deploy[start : deploy.index("\n", end) + 1]


def test_every_deploy_command_accepts_a_force_flag():
    probe = _deploy_flag_parser() + (
        '\nprintf "%s|%s|%s\\n" "$1" "${FORKMESH_FORCE_DEPLOY:-0}" "${2:-}"\n'
    )

    def run(*args):
        result = subprocess.run(
            ["/bin/bash", "-euo", "pipefail", "-c", probe, "deploy.sh", *args],
            text=True,
            capture_output=True,
            check=True,
        )
        return result.stdout.strip()

    # Without the flag the change check stays in charge of every command.
    assert run() == "deploy|0|"
    assert run("changed") == "changed|0|"
    assert run("world") == "world|0|"
    # --force (or -f) forces the deploy even when no inputs changed, in any
    # position, and defaults to the "deploy" command when given on its own.
    assert run("changed", "--force") == "changed|1|"
    assert run("changed", "-f") == "changed|1|"
    assert run("--force") == "deploy|1|"
    assert run("-f", "changed") == "deploy|1|changed"
    assert run("app", "--force") == "app|1|"
    # Other arguments still reach their command untouched.
    assert run("status", "--force", "--json") == "status|1|--json"
    assert run("dry-run", "world") == "dry-run|0|world"


def test_force_flag_is_exported_to_the_recursive_per_target_deploys():
    deploy = (REPOSITORY / "app/deploy.sh").read_text(encoding="utf-8")
    assert "export FORKMESH_FORCE_DEPLOY=1" in deploy
    # Both skip gates read the same exported variable, so "deploy"/"changed"
    # forcing carries into the app, world, and www child runs.
    assert deploy.count('[ "${FORKMESH_FORCE_DEPLOY:-0}" != "1" ]') == 2
