"""Local hardening survives an installer update.

install-instance.sh rewrites /etc/caddy/Caddyfile and install-runner.sh
rewrites jobs.nft on every run, so an operator's local changes there were
lost on the next update. Both now include a directory the installers never
touch. The service units also gained the systemd sandboxing an unprivileged
web app and git node can run under.
"""

from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INSTANCE = (REPO / "www/public/install-instance.sh").read_text(encoding="utf-8")
RUNNER = (REPO / "www/public/install-runner.sh").read_text(encoding="utf-8")


def _block(text, start, end="\n}\n"):
    return text.split(start, 1)[1].split(end, 1)[0]


def test_the_caddyfile_imports_local_files_at_the_top_and_in_the_site():
    writer = _block(INSTANCE, "\nwrite_caddyfile() {", "\nEOF\n")
    assert "install -d -m 0755 /etc/caddy/conf.d /etc/caddy/site.d" in writer
    caddyfile = writer.split("<<EOF\n", 1)[1]
    top, site = caddyfile.split("\n$DOMAIN {\n", 1)
    assert "\nimport /etc/caddy/conf.d/*.caddy\n" in top
    assert top.index("email $CFG_adminEmail") < top.index("import /etc/caddy/conf.d")
    assert "\timport /etc/caddy/site.d/*.caddy\n" in site
    assert site.index("import /etc/caddy/site.d") < site.index("handle @gateway")


def test_the_runner_firewall_includes_local_rules():
    writer = _block(RUNNER, "\nwrite_firewall_linux() {")
    assert '''printf 'include "%s/*.nft"\\n' "$(live_path "$NFT_LOCAL")"''' in writer
    assert writer.index("printf '}\\n'") < writer.index("include")
    assert 'install -d -m 0700 "$NFT_LOCAL"' in writer
    assert 'NFT_LOCAL="$ETC_ROOT/jobs.d"' in RUNNER


HARDENING = ("CapabilityBoundingSet=\n", "AmbientCapabilities=\n",
             "ProtectProc=invisible", "ProtectKernelTunables=true",
             "ProtectKernelModules=true", "ProtectKernelLogs=true",
             "ProtectControlGroups=true", "ProtectClock=true",
             "RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6 AF_NETLINK",
             "RestrictSUIDSGID=true", "LockPersonality=true",
             "SystemCallArchitectures=native", "NoNewPrivileges=true")

PROC_HARDENING = ("ProtectProc=invisible", "ProtectKernelTunables=true",
                  "ProtectKernelLogs=true")


def test_the_app_and_git_node_units_are_sandboxed():
    app = _block(INSTANCE, "write_app_unit() {")
    node = INSTANCE.split('cat > "$DAEMON_UNIT" <<EOF', 1)[1].split("\nEOF", 1)[0]
    for line in HARDENING:
        assert line in app, line
    for line in HARDENING:
        if line not in PROC_HARDENING:
            assert line in node, line
    for line in PROC_HARDENING:
        assert line not in node, line
    assert "RestrictNamespaces=true" in app
    # The git node may sandbox jobs with user namespaces (bubblewrap).
    assert "RestrictNamespaces" not in node


def test_a_broken_local_runner_rule_file_keeps_the_built_in_rules():
    runner = RUNNER
    write = _block(runner, "write_firewall_linux() {")
    assert '>"$NFT_BASE.new"' in write
    assert "include" not in write.split('>"$NFT_BASE.new"', 1)[0]
    assert 'cat "$NFT_BASE.new"' in write
    assert ("ExecStart=/bin/sh -c '/usr/sbin/nft -c -f "
            "/etc/forkmesh-runner/jobs.nft && exec /usr/sbin/nft -f "
            "/etc/forkmesh-runner/jobs.nft;") in runner
    assert "exec /usr/sbin/nft -f /etc/forkmesh-runner/jobs-base.nft'" in runner
    start = _block(runner, "start_firewall_linux() {")
    assert start.index('nft -c -f "$(live_path "$NFT")"') < start.index(
        'nft -f "$(live_path "$NFT_BASE")"')
