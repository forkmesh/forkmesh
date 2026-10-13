"""Focused contract tests for atomic Worker-secret publication."""

import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess


ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "deploy.sh"
PYWRANGLER = ROOT / "pywrangler.sh"

REQUIRED = {
    "ADMIN_PATH": "/admin-test",
    "ADMIN_PASS": "admin-pass-secret",
    "MAILTRAP_API_TOKEN": "mail-token",
    "MAILTRAP_WEBHOOK_SECRET": "mail-webhook-secret",
    "DATA_KEY": 'quotes-" slash-\\ tab-\t unicode-☃',
    "TREASURY_SOLANA_ADDRESS": "solana-address",
    "MIRROR_ROUTER_PUBLIC_KEY": "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg",
    "MIRROR_ROUTER_SIGNING_SEED": "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
    "DISCORD_CLIENT_ID": "1531910102578102322",
    "DISCORD_CLIENT_SECRET": "discord-client-secret",
    "DISCORD_BOT_TOKEN": "discord-bot-token",
    "PATREON_CLIENT_ID": "patreon-client-id",
    "PATREON_CLIENT_SECRET": "patreon-client-secret",
    "PATREON_WEBHOOK_SECRET": "patreon-webhook-secret",
    "PATREON_CAMPAIGN_ID": "patreon-campaign-id",
    "PATREON_CREATOR_ACCESS_TOKEN": "patreon-creator-token",
}

APNS_SECRETS = {
    "APNS_AUTH_KEY": "apns-auth-key",
    "APNS_KEY_ID": "apns-key-id",
    "APNS_TEAM_ID": "apns-team-id",
}


def _sandbox(tmp_path: Path) -> tuple[Path, Path, Path]:
    worker = tmp_path / "worker"
    worker.mkdir()
    shutil.copy2(DEPLOY, worker / "deploy.sh")
    shutil.copy2(PYWRANGLER, worker / "pywrangler.sh")

    env_lines = [
        "CLOUDFLARE_ACCOUNT_ID=test-account",
        "CLOUDFLARE_API_TOKEN=cloudflare-token-must-not-be-published",
        "VULTR_API_KEY=vultr-key-must-not-be-published",
        *(f"{key}={value}" for key, value in REQUIRED.items()),
        *(f"{key}={value}" for key, value in APNS_SECRETS.items()),
        "OPTIONAL_SECRET=optional=value with spaces",
        "ADMIN_USER=obsolete-admin-user",
        "EMPTY_SECRET=",
    ]
    (worker / ".env.production").write_text(
        "\n".join(env_lines) + "\n", encoding="utf-8"
    )

    fake_venv = tmp_path / "fake-pywrangler"
    fake_bin = fake_venv / "bin"
    fake_bin.mkdir(parents=True)
    log = tmp_path / "calls.log"
    captured = tmp_path / "captured.json"
    fake = fake_bin / "pywrangler"
    fake.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
printf '<%s>' "$@" >> "$FAKE_WRANGLER_LOG"
printf '\\n' >> "$FAKE_WRANGLER_LOG"
if [ "$1" = "secret" ] && [ "$2" = "bulk" ]; then
    payload="${@: -1}"
    python3 - "$payload" "$FAKE_WRANGLER_CAPTURE" <<'PY'
import json
import os
import shutil
import stat
import sys

mode = stat.S_IMODE(os.stat(sys.argv[1]).st_mode)
if mode != 0o600:
    raise SystemExit(f"secret payload mode is {mode:o}, expected 600")
with open(sys.argv[1], encoding="utf-8") as stream:
    json.load(stream)
shutil.copyfile(sys.argv[1], sys.argv[2])
PY
    printf '%s' "$payload" > "$FAKE_WRANGLER_CAPTURE.path"
    transient="${FAKE_WRANGLER_TRANSIENT_BULK_FAILURES:-0}"
    if [ "$transient" != "0" ]; then
        counter="$FAKE_WRANGLER_CAPTURE.transient"
        seen=0
        if [ -f "$counter" ]; then
            seen="$(cat "$counter")"
        fi
        if [ "$transient" = "always" ] || [ "$seen" -lt "$transient" ]; then
            echo $((seen + 1)) > "$counter"
            echo "🚨 Secrets failed to upload" >&2
            echo "✘ [ERROR] A request to the Cloudflare API (/accounts/a/workers/scripts/forkmesh-app/settings) failed." >&2
            echo "  An unknown error has occurred. [code: 10013]" >&2
            exit 1
        fi
    fi
    [ "${FAKE_WRANGLER_FAIL_BULK:-0}" != "1" ] || exit 23
elif [ "$1" = "secret" ] && [ "$2" = "list" ]; then
    python3 - "$FAKE_WRANGLER_CAPTURE" "${FAKE_WRANGLER_REMOTE_NAMES:-}" <<'PY'
import json
import os
import sys

names = {name for name in sys.argv[2].split(",") if name}
if os.path.exists(sys.argv[1]):
    with open(sys.argv[1], encoding="utf-8") as stream:
        names.update(json.load(stream))
print(json.dumps([{"name": key} for key in sorted(names)]))
PY
else
    echo "unexpected fake pywrangler invocation" >&2
    exit 97
fi
""",
        encoding="utf-8",
    )
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    curl = fake_bin / "curl"
    curl.write_text(
        f"""#!/usr/bin/env bash
set -euo pipefail
url="${{@: -1}}"
for arg in "$@"; do
    if [[ "$arg" == -*I* ]]; then
        printf 'HTTP/2 200\\nx-forkmesh-worker: app\\n\\n'
        exit 0
    fi
done
case "$url" in
    */api/mirrors/https)
        router_key='{REQUIRED['MIRROR_ROUTER_PUBLIC_KEY']}'
        if [ -n "${{FAKE_LIVE_ROUTER_AFTER_BULK:-}}" ] && \
           [ -f "$FAKE_WRANGLER_CAPTURE" ]; then
            router_key="$FAKE_LIVE_ROUTER_AFTER_BULK"
        fi
        printf '{{"routerPublicKey":"%s"}}\\n200' "$router_key"
        ;;
    */api/bootstrap/readiness)
        if [ -n "${{FAKE_DATA_KEY_PROBE_LOG:-}}" ]; then
            printf '<%s>' "$@" >> "$FAKE_DATA_KEY_PROBE_LOG"
            printf '\\n' >> "$FAKE_DATA_KEY_PROBE_LOG"
        fi
        outcome="${{FAKE_DATA_KEY_PROBE_STATUS:-200}}"
        if [ -n "${{FAKE_DATA_KEY_PROBE_SEQUENCE:-}}" ]; then
            counter="$FAKE_DATA_KEY_PROBE_LOG.counter"
            seen=0
            if [ -f "$counter" ]; then seen="$(<"$counter")"; fi
            IFS=',' read -r -a outcomes <<<"$FAKE_DATA_KEY_PROBE_SEQUENCE"
            index="$seen"
            if [ "$index" -ge "${{#outcomes[@]}}" ]; then
                index=$((${{#outcomes[@]}} - 1))
            fi
            outcome="${{outcomes[$index]}}"
            printf '%s' "$((seen + 1))" >"$counter"
        fi
        if [ "$outcome" = transport ]; then exit 28; fi
        if [ "$outcome" = no-proof ]; then
            status=200
            body='{{"ok":true,"database":true,"encryption":false}}'
        else
            status="$outcome"
            if [ "$status" = 200 ]; then
                body='{{"ok":true,"database":true,"encryption":true}}'
            else body='{{"error":"unavailable"}}'; fi
        fi
        printf '%s\\n%s' "$body" "$status"
        ;;
    *) exit 2 ;;
esac
""",
        encoding="utf-8",
    )
    curl.chmod(curl.stat().st_mode | stat.S_IXUSR)
    return worker, log, captured


def _run_secrets(
    tmp_path: Path,
    *,
    fail_bulk: bool = False,
    transient_bulk_failures: int | str = 0,
    attempts: int | None = None,
    remote_names: tuple[str, ...] | None = None,
    live_router_after_bulk: str = "",
) -> tuple[subprocess.CompletedProcess[str], Path, Path]:
    worker, log, captured = _sandbox(tmp_path)
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_FAIL_BULK": "1" if fail_bulk else "0",
            "FAKE_WRANGLER_TRANSIENT_BULK_FAILURES": str(transient_bulk_failures),
            # Keep the retry pauses out of the test's wall clock.
            "FORKMESH_SECRET_BULK_BACKOFF": "0",
            "FAKE_WRANGLER_REMOTE_NAMES": ",".join(
                remote_names
                if remote_names is not None
                else ("MIRROR_ROUTER_PUBLIC_KEY", "MIRROR_ROUTER_SIGNING_SEED")
            ),
            "FAKE_LIVE_ROUTER_AFTER_BULK": live_router_after_bulk,
            "FORKMESH_ROUTER_VERIFY_ATTEMPTS": "1",
            "FORKMESH_ROUTER_VERIFY_BACKOFF": "0",
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + os.environ.get("PATH", ""),
        }
    )
    if attempts is not None:
        env["FORKMESH_SECRET_BULK_ATTEMPTS"] = str(attempts)
    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, log, captured


def _run_data_key_sequence(
    tmp_path: Path,
    sequence: str,
    *,
    attempts: int = 3,
    fresh_dates: bool = False,
) -> tuple[subprocess.CompletedProcess[str], Path, Path, Path]:
    worker, log, captured = _sandbox(tmp_path)
    probe_log = tmp_path / "probe.log"
    fake_bin = tmp_path / "fake-pywrangler" / "bin"
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "DATA_KEY,MIRROR_ROUTER_PUBLIC_KEY,MIRROR_ROUTER_SIGNING_SEED"
            ),
            "FAKE_DATA_KEY_PROBE_LOG": str(probe_log),
            "FAKE_DATA_KEY_PROBE_SEQUENCE": sequence,
            "FORKMESH_DATA_KEY_VERIFY_ATTEMPTS": str(attempts),
            "FORKMESH_DATA_KEY_VERIFY_BACKOFF": "0",
            "FORKMESH_SECRET_BULK_BACKOFF": "0",
            "FORKMESH_ROUTER_VERIFY_ATTEMPTS": "1",
            "FORKMESH_ROUTER_VERIFY_BACKOFF": "0",
            "PATH": str(fake_bin) + os.pathsep + env["PATH"],
        }
    )
    if fresh_dates:
        date_counter = tmp_path / "date.counter"
        fake_date = fake_bin / "date"
        fake_date.write_text(
            """#!/usr/bin/env bash
set -euo pipefail
seen=0
if [ -f "$FAKE_DATE_COUNTER" ]; then seen="$(<"$FAKE_DATE_COUNTER")"; fi
printf '%s' "$((seen + 1))" >"$FAKE_DATE_COUNTER"
printf '%s\\n' "$((1700000000 + seen))"
""",
            encoding="utf-8",
        )
        fake_date.chmod(0o755)
        env["FAKE_DATE_COUNTER"] = str(date_counter)
    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )
    return result, log, captured, probe_log


def test_secrets_command_uses_one_secure_bulk_update_and_verifies_names(tmp_path):
    result, log, captured = _run_secrets(tmp_path)

    assert result.returncode == 0, result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><bulk>" in line]) == 1
    assert len([line for line in calls if "<secret><list>" in line]) == 2
    assert all("<secret><put>" not in line for line in calls)

    payload = json.loads(captured.read_text(encoding="utf-8"))
    assert payload == {
        **{
            key: value
            for key, value in REQUIRED.items()
            if not key.startswith("MIRROR_ROUTER_")
        },
        **APNS_SECRETS,
        "WORKERS_OBSERVABILITY_ACCOUNT_ID": "test-account",
    }
    assert "CLOUDFLARE_ACCOUNT_ID" not in payload
    assert "CLOUDFLARE_API_TOKEN" not in payload
    assert "EMPTY_SECRET" not in payload
    assert "OPTIONAL_SECRET" not in payload
    assert "ADMIN_USER" not in payload
    assert "MIRROR_ROUTER_PUBLIC_KEY" not in payload
    assert "MIRROR_ROUTER_SIGNING_SEED" not in payload
    assert "preserving the coordinated remote mirror-router key pair" in result.stdout
    assert "skip (not an App runtime secret): ADMIN_USER" in result.stderr
    # The desktop app keeps its Vultr provisioning key in this file; the Worker
    # has no Vultr code path, so it must never reach the runtime (adhoc #127).
    assert "VULTR_API_KEY" not in payload
    # 10 on main, +5 Patreon credentials, +3 APNS credentials.
    assert "Pushed 18 secret(s) from .env.production in one bulk update." in result.stdout
    combined_output = result.stdout + result.stderr
    for value in (
        *payload.values(),
        REQUIRED["MIRROR_ROUTER_PUBLIC_KEY"],
        REQUIRED["MIRROR_ROUTER_SIGNING_SEED"],
        "cloudflare-token-must-not-be-published",
        "vultr-key-must-not-be-published",
    ):
        assert value not in combined_output

    temporary_payload = Path(
        (captured.with_suffix(captured.suffix + ".path")).read_text()
    )
    assert not temporary_payload.exists()


def test_generic_secrets_refuses_to_create_a_missing_router_pair(tmp_path):
    result, log, captured = _run_secrets(tmp_path, remote_names=())

    assert result.returncode != 0
    assert "Generic secret updates cannot create or rotate" in result.stderr
    assert not captured.exists()
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><list>" in line]) == 1
    assert all("<secret><bulk>" not in line for line in calls)


def test_generic_secrets_fails_if_live_router_identity_changes(tmp_path):
    result, log, _captured = _run_secrets(
        tmp_path,
        live_router_after_bulk="B" * 43,
    )

    assert result.returncode != 0
    assert "did not report the expected mirror-router public key" in result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><bulk>" in line]) == 1
    assert REQUIRED["MIRROR_ROUTER_SIGNING_SEED"] not in (
        result.stdout + result.stderr
    )


def test_validate_secrets_rejects_a_mismatched_router_pair_before_cloudflare(
    tmp_path,
):
    worker, log, _captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    env_file.write_text(
        env_file.read_text(encoding="utf-8").replace(
            REQUIRED["MIRROR_ROUTER_SIGNING_SEED"], "B" * 43
        ),
        encoding="utf-8",
    )
    result = subprocess.run(
        ["bash", "deploy.sh", "validate-secrets"],
        cwd=worker,
        env={
            **os.environ,
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(tmp_path / "captured.json"),
        },
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "matched" in result.stderr
    assert not log.exists()
    assert "B" * 43 not in result.stdout + result.stderr


def test_failed_bulk_update_is_not_retried_per_secret_and_cleans_payload(tmp_path):
    result, log, captured = _run_secrets(tmp_path, fail_bulk=True)

    assert result.returncode != 0
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><bulk>" in line]) == 1
    assert all("<secret><put>" not in line for line in calls)
    assert len([line for line in calls if "<secret><list>" in line]) == 1
    assert "no per-secret retry was attempted" in result.stderr

    temporary_payload = Path(
        (captured.with_suffix(captured.suffix + ".path")).read_text()
    )
    assert not temporary_payload.exists()


def test_transient_cloudflare_error_resends_the_same_bulk_update(tmp_path):
    # Cloudflare's script-settings endpoint 500s with [code: 10013] every so
    # often; the identical payload publishes on a later attempt. Re-sending the
    # one atomic update must recover the deploy without any `secret put`.
    result, log, captured = _run_secrets(tmp_path, transient_bulk_failures=2)

    assert result.returncode == 0, result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><bulk>" in line]) == 3
    assert len([line for line in calls if "<secret><list>" in line]) == 2
    assert all("<secret><put>" not in line for line in calls)
    assert "Bulk secret update succeeded on attempt 3/4." in result.stdout

    payload = json.loads(captured.read_text(encoding="utf-8"))
    assert payload["ADMIN_PATH"] == REQUIRED["ADMIN_PATH"]
    assert "CLOUDFLARE_API_TOKEN" not in payload

    combined_output = result.stdout + result.stderr
    for value in payload.values():
        assert value not in combined_output

    temporary_payload = Path(
        (captured.with_suffix(captured.suffix + ".path")).read_text()
    )
    assert not temporary_payload.exists()


def test_unrelenting_transient_error_stops_at_the_attempt_budget(tmp_path):
    result, log, captured = _run_secrets(
        tmp_path, transient_bulk_failures="always", attempts=3
    )

    assert result.returncode != 0
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><bulk>" in line]) == 3
    assert all("<secret><put>" not in line for line in calls)
    assert len([line for line in calls if "<secret><list>" in line]) == 1
    assert "transient Cloudflare API error on all 3 attempts" in result.stderr
    assert "re-run './deploy.sh secrets'" in result.stderr

    temporary_payload = Path(
        (captured.with_suffix(captured.suffix + ".path")).read_text()
    )
    assert not temporary_payload.exists()


def test_missing_required_secret_fails_before_any_bulk_mutation(tmp_path):
    worker, log, _captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    lines = [
        line
        for line in env_file.read_text(encoding="utf-8").splitlines()
        if not line.startswith("DATA_KEY=")
    ]
    env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(tmp_path / "captured.json"),
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "missing both locally and on the Worker" in result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len(calls) == 1
    assert "<secret><list>" in calls[0]
    assert all("<secret><bulk>" not in line for line in calls)


def test_missing_apns_secret_fails_before_any_bulk_mutation(tmp_path):
    worker, log, _captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    lines = [
        line
        for line in env_file.read_text(encoding="utf-8").splitlines()
        if not line.startswith("APNS_AUTH_KEY=")
    ]
    env_file.write_text("\n".join(lines) + "\n", encoding="utf-8")
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(tmp_path / "captured.json"),
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "APNS_AUTH_KEY" in result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len(calls) == 1
    assert "<secret><list>" in calls[0]
    assert all("<secret><bulk>" not in line for line in calls)


def test_existing_remote_webhook_secret_is_preserved_when_local_value_is_absent(
    tmp_path,
):
    worker, log, captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    env_file.write_text(
        "\n".join(
            line
            for line in env_file.read_text(encoding="utf-8").splitlines()
            if not line.startswith("MAILTRAP_WEBHOOK_SECRET=")
        )
        + "\n",
        encoding="utf-8",
    )
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "MAILTRAP_WEBHOOK_SECRET,MIRROR_ROUTER_PUBLIC_KEY,"
                "MIRROR_ROUTER_SIGNING_SEED"
            ),
            "FORKMESH_SECRET_BULK_BACKOFF": "0",
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len([line for line in calls if "<secret><list>" in line]) == 2
    assert len([line for line in calls if "<secret><bulk>" in line]) == 1
    assert "preserving existing remote secret(s): MAILTRAP_WEBHOOK_SECRET" in result.stdout
    payload = json.loads(captured.read_text(encoding="utf-8"))
    assert "MAILTRAP_WEBHOOK_SECRET" not in payload


def test_existing_remote_data_key_is_authenticated_and_never_bulk_replaced(
    tmp_path,
):
    worker, log, captured = _sandbox(tmp_path)
    probe_log = tmp_path / "probe.log"
    curl = tmp_path / "fake-pywrangler" / "bin" / "curl"
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "DATA_KEY,MIRROR_ROUTER_PUBLIC_KEY,MIRROR_ROUTER_SIGNING_SEED"
            ),
            "FAKE_DATA_KEY_PROBE_LOG": str(probe_log),
            "FORKMESH_SECRET_BULK_BACKOFF": "0",
            "PATH": str(curl.parent) + os.pathsep + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    payload = json.loads(captured.read_text(encoding="utf-8"))
    assert "DATA_KEY" not in payload
    assert "preserving authenticated remote secret: DATA_KEY" in result.stdout
    probe_arguments = probe_log.read_text(encoding="utf-8")
    assert REQUIRED["DATA_KEY"] not in probe_arguments
    assert "X-ForkMesh-Readiness-Timestamp" in probe_arguments


def test_remote_data_key_retries_transport_and_5xx_with_fresh_proofs(tmp_path):
    result, _log, captured, probe_log = _run_data_key_sequence(
        tmp_path,
        "transport,503,200",
        fresh_dates=True,
    )

    assert result.returncode == 0, result.stderr
    assert captured.exists()
    probe_calls = probe_log.read_text(encoding="utf-8")
    assert probe_calls.count("/api/bootstrap/readiness") == 3
    timestamps = re.findall(
        r"X-ForkMesh-Readiness-Timestamp: ([0-9]+)", probe_calls
    )
    proofs = re.findall(r"Authorization: Bearer ([0-9a-f]{64})", probe_calls)
    assert len(set(timestamps)) == 3
    assert len(set(proofs)) == 3
    assert proofs == [
        hmac.new(
            REQUIRED["DATA_KEY"].encode(),
            b"forkmesh-bootstrap-readiness-v1\n" + timestamp.encode(),
            hashlib.sha256,
        ).hexdigest()
        for timestamp in timestamps
    ]
    assert "DATA_KEY readiness transport failed" in result.stderr
    assert "DATA_KEY readiness returned HTTP 503" in result.stderr
    combined = result.stdout + result.stderr
    assert REQUIRED["DATA_KEY"] not in combined
    assert all(proof not in combined for proof in proofs)


def test_remote_data_key_does_not_retry_mismatch_or_missing_proof(tmp_path):
    cases = (
        ("mismatch", "401,200", "HTTP 401"),
        ("missing-proof", "no-proof,200", "did not prove database encryption"),
    )
    for name, sequence, expected_error in cases:
        case_dir = tmp_path / name
        case_dir.mkdir()
        result, _log, captured, probe_log = _run_data_key_sequence(
            case_dir,
            sequence,
        )

        assert result.returncode != 0
        assert expected_error in result.stderr
        assert (
            probe_log.read_text(encoding="utf-8").count(
                "/api/bootstrap/readiness"
            )
            == 1
        )
        assert not captured.exists()
        assert REQUIRED["DATA_KEY"] not in result.stdout + result.stderr


def test_remote_data_key_stops_after_bounded_5xx_retries(tmp_path):
    result, _log, captured, probe_log = _run_data_key_sequence(
        tmp_path,
        "503,502,500",
    )

    assert result.returncode != 0
    assert (
        probe_log.read_text(encoding="utf-8").count("/api/bootstrap/readiness")
        == 3
    )
    assert "remained unavailable (HTTP 500) after 3 attempts" in result.stderr
    assert not captured.exists()
    assert REQUIRED["DATA_KEY"] not in result.stdout + result.stderr


def test_first_cutover_preserves_remote_key_then_requires_postdeploy_proof(
    tmp_path,
):
    worker, log, captured = _sandbox(tmp_path)
    probe_log = tmp_path / "probe.log"
    curl = tmp_path / "fake-pywrangler" / "bin" / "curl"
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "DATA_KEY,MIRROR_ROUTER_PUBLIC_KEY,MIRROR_ROUTER_SIGNING_SEED"
            ),
            "FAKE_DATA_KEY_PROBE_LOG": str(probe_log),
            "FAKE_DATA_KEY_PROBE_STATUS": "404",
            "FORKMESH_SECRET_BULK_BACKOFF": "0",
            "PATH": str(curl.parent) + os.pathsep + env["PATH"],
        }
    )

    preflight = subprocess.run(
        ["bash", "deploy.sh", "validate-secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert preflight.returncode == 0, preflight.stderr
    assert "first-cutover preflight" in preflight.stdout
    assert (
        probe_log.read_text(encoding="utf-8").count("/api/bootstrap/readiness")
        == 1
    )
    assert not captured.exists()
    assert all(
        "<secret><bulk>" not in line
        for line in log.read_text(encoding="utf-8").splitlines()
    )

    env["FAKE_DATA_KEY_PROBE_STATUS"] = "200"
    publish = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert publish.returncode == 0, publish.stderr
    payload = json.loads(captured.read_text(encoding="utf-8"))
    assert "DATA_KEY" not in payload
    assert "preserving authenticated remote secret: DATA_KEY" in publish.stdout


def test_remote_data_key_requires_a_local_recovery_value(tmp_path):
    worker, log, captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    env_file.write_text(
        "\n".join(
            line
            for line in env_file.read_text(encoding="utf-8").splitlines()
            if not line.startswith("DATA_KEY=")
        )
        + "\n",
        encoding="utf-8",
    )
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(captured),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "DATA_KEY,MIRROR_ROUTER_PUBLIC_KEY,MIRROR_ROUTER_SIGNING_SEED"
            ),
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "no local recovery value" in result.stderr
    assert not captured.exists()


def test_invalid_discord_client_id_fails_before_any_bulk_mutation(tmp_path):
    worker, log, _captured = _sandbox(tmp_path)
    env_file = worker / ".env.production"
    env_file.write_text(
        env_file.read_text(encoding="utf-8").replace(
            "DISCORD_CLIENT_ID=1531910102578102322",
            "DISCORD_CLIENT_ID=not-a-snowflake",
        ),
        encoding="utf-8",
    )
    env = os.environ.copy()
    env.update(
        {
            "PYWRANGLER_VENV": str(tmp_path / "fake-pywrangler"),
            "FAKE_WRANGLER_LOG": str(log),
            "FAKE_WRANGLER_CAPTURE": str(tmp_path / "captured.json"),
            "FAKE_WRANGLER_REMOTE_NAMES": (
                "MIRROR_ROUTER_PUBLIC_KEY,MIRROR_ROUTER_SIGNING_SEED"
            ),
            "PATH": str(tmp_path / "fake-pywrangler" / "bin")
            + os.pathsep
            + env["PATH"],
        }
    )

    result = subprocess.run(
        ["bash", "deploy.sh", "secrets"],
        cwd=worker,
        env=env,
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert "DISCORD_CLIENT_ID must be a 17-20 digit" in result.stderr
    calls = log.read_text(encoding="utf-8").splitlines()
    assert len(calls) == 1 and "<secret><list>" in calls[0]


def test_push_secrets_contains_bulk_contract_without_put_loop():
    source = DEPLOY.read_text(encoding="utf-8")
    start = source.index("push_secrets() {")
    end = source.index("\n}\n\npublish_release_binary", start)
    function = source[start:end]

    assert function.count("pywrangler secret bulk") == 1
    assert "pywrangler secret put" not in function
    # Retries re-send that one atomic update; they never fan out per secret.
    assert "_secret_bulk_error_is_transient" in function
    assert 'chmod 0600 "$secret_bulk_file"' in function
    assert "trap 'rm -f -- \"$secret_bulk_file\"' EXIT" in function


def test_clean_secret_inventory_uses_read_only_cloudflare_api_without_token_in_argv():
    source = DEPLOY.read_text(encoding="utf-8")
    start = source.index("cloudflare_worker_secret_inventory() {")
    end = source.index("\n}\n\nlist_worker_secrets", start)
    function = source[start:end]

    assert "/workers/scripts/$worker/secrets" in function
    assert "curl --config -" in function
    assert 'Authorization: Bearer %s' in function
    assert '-H "Authorization:' not in function
    assert 'payload.get("success") is not True' in function


def test_production_deploy_preflights_discord_secrets_before_mutation():
    source = DEPLOY.read_text(encoding="utf-8")
    start = source.index("deploy_app_target() {")
    deploy_app = source[
        start:source.index("\n}\n\ninitial_official_cutover_needed", start)
    ]

    assert deploy_app.index("push_secrets validate") < deploy_app.index("./migrate.sh")
    assert deploy_app.index("push_secrets validate") < deploy_app.index(
        "deploy_app_version"
    )
    assert deploy_app.index("push_secrets validate") < deploy_app.index(
        "push_secrets\n"
    )


def test_production_manifest_sets_canonical_discord_public_urls():
    wrangler = (ROOT / "wrangler.toml").read_text(encoding="utf-8")

    assert 'PUBLIC_BASE_URL = "https://forkmesh.com"' in wrangler
    assert 'API_ORIGIN = "https://app.forkmesh.com"' in wrangler
    assert (
        'DISCORD_OAUTH_REDIRECT_URI = '
        '"https://app.forkmesh.com/api/integrations/discord/callback"'
    ) in wrangler
    assert "DISCORD_BOT_TOKEN" not in wrangler
    assert "DISCORD_CLIENT_SECRET" not in wrangler


def test_production_manifest_sets_canonical_patreon_callback_url():
    wrangler = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    env_example = (ROOT / ".env.production.example").read_text(encoding="utf-8")
    runbook = (
        ROOT.parent / "www" / "docs" / "operations" / "patreon.md"
    ).read_text(encoding="utf-8")

    assert (
        'PATREON_OAUTH_REDIRECT_URI = '
        '"https://forkmesh.com/api/integrations/patreon/callback"'
    ) in wrangler
    for secret_name in (
        "PATREON_CLIENT_ID",
        "PATREON_CLIENT_SECRET",
        "PATREON_WEBHOOK_SECRET",
        "PATREON_CAMPAIGN_ID",
        "PATREON_CREATOR_ACCESS_TOKEN",
    ):
        assert secret_name not in wrangler.split("[vars]", 1)[1].split(
            "[env.dev.vars]", 1)[0]
        assert f"{secret_name}=CHANGE-ME" in env_example
        assert secret_name in (
            DEPLOY.read_text(encoding="utf-8").split("push_secrets() {", 1)[1]
        )
        assert secret_name in runbook


def test_forkmesh_deploy_materializes_runtime_secrets_from_secure_variables():
    workflow = (ROOT.parent / ".forkmesh" / "deploy.yml").read_text(
        encoding="utf-8")

    for name in (
        "DISCORD_CLIENT_ID",
        "DISCORD_CLIENT_SECRET",
        "DISCORD_BOT_TOKEN",
        "PATREON_CLIENT_ID",
        "PATREON_CLIENT_SECRET",
        "PATREON_WEBHOOK_SECRET",
        "PATREON_CAMPAIGN_ID",
        "PATREON_CREATOR_ACCESS_TOKEN",
        "APNS_AUTH_KEY",
        "APNS_KEY_ID",
        "APNS_TEAM_ID",
    ):
        assert f"{name}=${{{{ vars.{name} }}}}" in workflow


def test_forkmesh_deploy_materializes_product_dashboard_credentials():
    """The dashboard's two secrets travel the same road as ADMIN_PASS.

    They are optional: an unset pair keeps /product-analysis a 404, so the
    example file lists them commented out and push_secrets skips the empty
    values a missing secure variable materialises.
    """
    workflow = (ROOT.parent / ".forkmesh" / "deploy.yml").read_text(
        encoding="utf-8")
    push_body = DEPLOY.read_text(encoding="utf-8").split(
        "push_secrets() {", 1)[1]
    allowed = push_body.split("local allowed=\"", 1)[1].split("\"", 1)[0]
    required = push_body.split("local required=\"", 1)[1].split("\"", 1)[0]
    env_example = (ROOT / ".env.production.example").read_text(
        encoding="utf-8")
    for name in ("PRODUCT_ANALYSIS_USER", "PRODUCT_ANALYSIS_PASS"):
        assert f"{name}=${{{{ vars.{name} }}}}" in workflow
        assert f" {name} " in allowed
        assert f" {name} " not in required
        assert f"# {name}=" in env_example
        assert f"\n{name}=" not in env_example
