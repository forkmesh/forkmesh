import json
import shlex
import subprocess

from test_install_instance_script import (TEXT, function, parse, run_functions,
                                          sandbox_env, settings, values)


def test_privacy_mode_is_off_by_default_and_must_be_a_boolean(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_privacyMode"] == "0"
    found = values(parse(tmp_path, settings(privacyMode=True)).stdout)
    assert found["CFG_privacyMode"] == "1"
    result = parse(tmp_path, settings(privacyMode="yes"))
    assert result.returncode == 2
    assert "Setting privacyMode must be true or false." in result.stdout


def test_an_update_keeps_privacy_mode_from_instance_json():
    parser = TEXT.split("parse_config() {", 1)[1].split(
        "\n' \"$CONFIG_MODE\"", 1)[0]
    block = parser.split('privacy_mode = c.get("privacyMode", False)', 1)[1]
    assert "if update" not in block.split("values[\"privacyMode\"]", 1)[0]
    writer = TEXT.split('keys = ["projectName"', 1)[1].split(
        '> "$INSTANCE_JSON.new"', 1)[0]
    assert '"webFirewall", "whiteLabel", "privacyMode"]' in writer
    assert 'values["privacyMode"] = values["privacyMode"] == "1"' in writer
    assert '"${CFG_privacyMode:-0}" "$INSTANCE_INSTALLER_VERSION"' in writer


def _write_settings(tmp_path, privacy):
    etc = tmp_path / "etc"
    etc.mkdir(exist_ok=True)
    app_env = etc / "app.env"
    app_env.write_text("DATA_KEY=d\nADMIN_PASS=p\nMIRROR_ROUTER_SIGNING_SEED=s\n"
                       "MIRROR_ROUTER_PUBLIC_KEY=k\n")
    body = "\n".join([
        "begin() { :; }", "chown() { :; }", "git() { echo 0123abc; }",
        "upstream_page() { :; }", "release_version() { echo 1.0.0; }",
        "logo_file() { :; }",
        "files_digest() { cat \"$@\" 2>/dev/null | sha256sum; }",
        "APP_ENV=%s" % shlex.quote(str(app_env)),
        "INSTANCE_JSON=%s" % shlex.quote(str(etc / "instance.json")),
        " ".join("CFG_%s=" % key for key in (
            "mailApiKey", "mailFrom", "mailgunDomain", "mailgunRegion",
            "domainProvided", "publicAddress", "ownerType", "ownerName",
            "sourceUrl", "sourceRef", "plugins", "poweredBy")),
        "CFG_privacyMode=%s" % privacy,
        "LOGIN_MARKER=/x UPDATE_REQUEST_DIR=/x UPDATE_STATE=/x BUILD_INFO=/x",
        "RUNNER_NAME=forkmesh-runner INSTANCE_INSTALLER_VERSION=2",
        "CONFIG_MODE=update NEW_INSTANCE=0 CFG_adminPath=",
        "APP_USER=forkmesh-app VENV=/nonexistent SRC=/nonexistent "
        "NODE_NAME=forkmesh",
        "CFG_projectName=Acme CFG_domain=code.example.com "
        "CFG_mailProvider=none",
        "CFG_actions=0 CFG_adminUsername=ada CFG_adminEmail=ada@example.com",
        "ORIGIN=https://code.example.com DOMAIN=code.example.com",
        "DIRECTORY_URL=https://directory.example.org",
        "write_settings", ""])
    result = run_functions(
        tmp_path, ["env_value", "resolve_runner_name", "new_admin_path",
                   "choose_admin_path", "write_settings"], body)
    assert result.returncode == 0, result.stderr + result.stdout
    return app_env.read_text(), json.loads((etc / "instance.json").read_text())


def test_the_setting_reaches_app_env_and_instance_json(tmp_path):
    after, instance = _write_settings(tmp_path, "1")
    assert "INSTANCE_PRIVACY_MODE=1\n" in after
    assert instance["privacyMode"] is True
    after, instance = _write_settings(tmp_path, "0")
    assert "INSTANCE_PRIVACY_MODE=0\n" in after
    assert instance["privacyMode"] is False


def _caddyfile(tmp_path, privacy):
    root = tmp_path / ("caddy" + privacy)
    root.mkdir()
    body = "\n".join([
        "CFG_privacyMode=%s" % privacy, "CFG_adminEmail=ada@example.com",
        "DOMAIN=code.example.com", "GATEWAY_PORT=8790", "APP_PORT=8080",
        "CLOUDFLARE_RANGES=173.245.48.0/20", "write_caddyfile", ""])
    script = tmp_path / ("caddy%s.sh" % privacy)
    writer = "write_caddyfile() {" + TEXT.split(
        "\nwrite_caddyfile() {", 1)[1].split("\nEOF\n}\n", 1)[0] + "\nEOF\n}\n"
    script.write_text("set -euo pipefail\n" + writer.replace(
        "/etc/caddy", str(root)) + body)
    result = subprocess.run(["bash", str(script)], capture_output=True,
                            text=True, env=sandbox_env(tmp_path), timeout=60)
    assert result.returncode == 0, result.stderr
    return (root / "Caddyfile").read_text().replace(str(root), "/etc/caddy")


def test_caddy_writes_no_request_lines_in_privacy_mode(tmp_path):
    plain = _caddyfile(tmp_path, "0")
    private = _caddyfile(tmp_path, "1")
    assert "log" not in plain.split("import /etc/caddy", 1)[0].replace(
        "logo", "")
    assert "{\n\temail ada@example.com\n}\n" in plain
    assert ("\tlog default {\n\t\texclude http.log.access http.log.error "
            "http.handlers.reverse_proxy http.stdlib\n\t}\n}") in private
    assert private.replace(
        "\n\tlog default {\n\t\texclude http.log.access http.log.error "
        "http.handlers.reverse_proxy http.stdlib\n\t}", "") == plain


def test_the_git_node_and_the_journal_follow_the_setting():
    unit = function("write_daemon_unit")
    assert ("Environment=FORKMESH_COMMIT_EMAIL=noreply@$DOMAIN$( [ "
            "\"${CFG_privacyMode:-0}\" = \"1\" ] && printf "
            "'\\nEnvironment=FORKMESH_PRIVACY_MODE=1' )") in unit
    journal = function("write_journald_privacy")
    assert "MaxRetentionSec=$JOURNALD_PRIVACY_RETENTION" in journal
    assert 'JOURNALD_PRIVACY_RETENTION="3day"' in TEXT
    assert "grep -q 'managed by install-instance.sh' \"$JOURNALD_PRIVACY\"" \
        in journal
    assert "systemctl restart systemd-journald" in journal
    order = TEXT.split("\ncheck_system\n", 1)[1]
    assert order.index("write_journald_privacy") < order.index("start_app")


def test_the_journald_drop_in_is_written_and_removed(tmp_path):
    target = tmp_path / "journald.conf.d" / "forkmesh-privacy.conf"
    body = "\n".join([
        "warn() { :; }", "systemctl() { echo restart >> %s; }" % shlex.quote(
            str(tmp_path / "calls")),
        "JOURNALD_PRIVACY=%s" % shlex.quote(str(target)),
        "JOURNALD_PRIVACY_RETENTION=3day",
        "CFG_privacyMode=1 write_journald_privacy",
        "CFG_privacyMode=1 write_journald_privacy",
        "cp %s %s" % (shlex.quote(str(target)),
                      shlex.quote(str(tmp_path / "copy"))),
        "CFG_privacyMode=0 write_journald_privacy", ""])
    result = run_functions(
        tmp_path, ["files_digest", "write_journald_privacy"], body)
    assert result.returncode == 0, result.stderr + result.stdout
    written = (tmp_path / "copy").read_text()
    assert "[Journal]\nMaxRetentionSec=3day\nMaxFileSec=1day\n" in written
    assert not target.exists()
    assert (tmp_path / "calls").read_text().count("restart") == 2
