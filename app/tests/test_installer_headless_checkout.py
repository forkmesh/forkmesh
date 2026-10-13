"""Display-less installs use the native mirror service, never the desktop."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
INSTALLER = ROOT / "www" / "public" / "install.sh"
SETUP = ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
MAIN_WINDOW = ROOT / "desktop" / "src" / "MainWindow.cpp"
CONTROL_NODE = ROOT / "desktop" / "src" / "MainWindowControlNode.cpp"
SERVER_DAEMON = ROOT / "server" / "daemon.go"


def test_restart_cleans_only_managed_mirror_runtime_temp_directories():
    script = INSTALLER.read_text(encoding="utf-8")

    assert "cleanup_stale_service_mirror_tmp" in script
    assert "-name 'ForkMesh-*'" in script
    assert "-name 'forkmesh-mirror-runtime-*'" in script
    cleanup = script.split("cleanup_stale_service_mirror_tmp()", 1)[1]
    cleanup = cleanup.split("\n}", 1)[0]
    assert "forkmesh-agent-*" not in cleanup
    restart = script.split('if [ "$FORKMESH_RESTART" = "1" ]', 1)[1]
    assert restart.index("stop_forkmesh_daemons") < restart.index(
        "cleanup_stale_service_mirror_tmp")


def test_installer_bootstraps_native_mirror_before_service_start():
    script = INSTALLER.read_text(encoding="utf-8")
    provision = script.split("provision_direct_https()", 1)[1].split(
        "\nprovision_cloudflare_tunnel()", 1
    )[0]

    assert '--config "$SYSTEMD_CONFIG" --init' in provision
    assert "managed native mirror service" in provision
    assert provision.index('--config "$SYSTEMD_CONFIG" --init') < provision.index(
        "systemctl enable --now forkmesh-mirror-node.service"
    )
    assert "ExecStart=%s --config %s" in script
    assert "forkmesh-mirror-node.service" in script
    assert "publishToNetwork=true" not in script


def test_desktop_setup_has_no_service_checkout_or_server_bootstrap():
    source = SETUP.read_text(encoding="utf-8")

    assert "serviceManagedCheckout" not in source
    assert "installerLinkPending" not in source
    assert "maybeAutoStartDirectMirrorServices" not in source


def test_failed_headless_install_is_resumable_only_with_root_owned_marker():
    script = INSTALLER.read_text(encoding="utf-8")

    assert 'SYSTEMD_BOOTSTRAP_MARKER=' in script
    assert "forkmesh-installing-service-v1" in script
    assert 'grep -Fqx "path=$SYSTEMD_STATE_DIR" "$SYSTEMD_BOOTSTRAP_MARKER"' in script
    assert '[_path_owner_uid "$SYSTEMD_BOOTSTRAP_MARKER"' not in script
    assert '[ "$(_path_owner_uid "$SYSTEMD_BOOTSTRAP_MARKER")" = "0" ]' in script
    assert 'rm -f -- "$SYSTEMD_BOOTSTRAP_MARKER"' in script


def test_direct_https_helpers_are_staged_for_the_unprivileged_service():
    script = INSTALLER.read_text(encoding="utf-8")

    assert "direct_https_bootstrap.py" in script
    assert 'chmod 0755 "$(dirname "$tools_dir")" "$tools_dir"' in script
    direct = script.split("provision_direct_https()", 1)[1].split(
        "\nprovision_cloudflare_tunnel()", 1
    )[0]
    assert "--disable-cloudflared" in direct
    assert "reload-or-restart caddy.service" in direct
    assert "ufw allow 80/tcp" in direct
    assert "ufw allow 443/tcp" in direct
    assert "python3-cryptography" in direct
    assert "python3 -c 'import cryptography'" in direct
    assert "provision_cloudflare_tunnel" not in script.split(
        "# Automatic host-owned HTTPS provisioning", 1
    )[1]


def test_native_gateway_installs_the_shared_dashboard_replica_contract():
    script = INSTALLER.read_text(encoding="utf-8")
    staging = script.split("_tunnel_stage_tools()", 1)[1].split(
        "\n}\n\nprovision_cloudflare_tunnel()", 1
    )[0]

    assert "app/src/mirror_replica.py" in staging
    assert "app/src/public_snapshots.py" in staging
    assert "app/src/portable_sessions.py" in staging
    assert 'install -m 0644 "$source_root/app/src/$f" "$tools_dir/$f"' in staging
    assert '[ -f "$tools_dir/mirror_replica.py" ]' in staging
    assert '[ -f "$tools_dir/public_snapshots.py" ]' in staging
    assert '[ -f "$tools_dir/portable_sessions.py" ]' in staging


def test_provisioned_mirror_service_is_native_and_not_started_by_main_window():
    source = MAIN_WINDOW.read_text(encoding="utf-8")
    script = INSTALLER.read_text(encoding="utf-8")

    assert "maybeAutoStartDirectMirrorServices" not in source
    assert "ExecStart=%s --config %s" in script
    assert "systemctl enable --now forkmesh-mirror-node.service" in script
    assert 'setsid "$BIN"' not in script.split(
        "# --- automatic Cloudflare Tunnel provisioning", 1
    )[0].split("launch_forkmesh()", 1)[1].split("if [ -n", 1)[0]


def test_native_daemon_retries_dirty_endpoint_state_until_active():
    source = SERVER_DAEMON.read_text(encoding="utf-8")

    assert "endpointStateRetryDelay = 7 * time.Second" in source
    assert "d.markEndpointStateDirty()" in source
    assert "retryEndpoint := d.endpointDirty" in source
    assert "endpoint.Publish(ctx)" in source


def test_direct_https_works_with_dns_the_operator_points_by_hand():
    script = INSTALLER.read_text(encoding="utf-8")
    direct = script.split("provision_direct_https()", 1)[1].split(
        "\nprovision_cloudflare_tunnel()", 1
    )[0]
    tail = script.split("# Automatic host-owned HTTPS provisioning", 1)[1]

    assert 'if [ -n "$FORKMESH_PUBLIC_HOSTNAME" ]; then' in tail
    assert "CLOUDFLARE_API_TOKEN is not" not in tail
    assert '[ -n "${CLOUDFLARE_API_TOKEN:-}" ] || dns_mode="manual"' in direct
    assert "--adopt-dedicated-host --skip-dns" in direct
    assert '_wait_for_public_dns "$hostname" "$address"' in direct
    assert direct.index("--register-link-code \"$FORKMESH_LINK_CODE\"") < direct.index(
        "systemctl enable --now forkmesh-mirror-node.service"
    ) < direct.index("_wait_for_public_dns")
    assert direct.index("_wait_for_public_dns") < direct.index(
        "systemctl reload-or-restart caddy.service"
    )
    assert "service|service-staged) ;;" in direct
    assert "SYSTEMD_SERVICE_USER" not in script
    assert "$FORKMESH_VERSION" not in direct


def test_dns_wait_checks_public_resolution_for_the_record_type():
    import subprocess

    script = INSTALLER.read_text(encoding="utf-8")
    helper = script.split("_dns_record_type() {", 1)[1].split("\n}\n", 1)[0]
    body = "_dns_record_type() {" + helper + "\n}\n"
    for address, expected in (("203.0.113.10", "A"), ("2001:db8::1", "AAAA")):
        result = subprocess.run(
            ["bash", "-c", body + '_dns_record_type "$1"', "_", address],
            capture_output=True, text=True, check=True,
        )
        assert result.stdout == expected
    assert "https://cloudflare-dns.com/dns-query?name=$hostname&type=$type" in script
    assert "FORKMESH_DNS_WAIT_SECONDS" in script
