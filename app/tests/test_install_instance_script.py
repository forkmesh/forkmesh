import base64
import json
import os
import re
import shlex
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "www" / "public" / "install-instance.sh"
TEXT = SCRIPT.read_text(encoding="utf-8")
ADMIN_PASSWORD = "admin-pass-word-7"  # forkmesh-secret-scan:ignore-line
MAIL_KEY = "SG.abc.def123456"  # forkmesh-secret-scan:ignore-line


def settings(**changes):
    value = {
        "projectName": "Acme  Code",
        "domain": "Code.Example.com",
        "domainProvided": True,
        "publicAddress": "203.0.113.10",
        "adminUsername": "ada",
        "adminEmail": "ada@example.com",
        "adminPassword": ADMIN_PASSWORD,
        "ownerType": "org",
        "ownerName": "acme",
        "mailProvider": "sendgrid",
        "mailApiKey": MAIL_KEY,
        "mailFrom": "no-reply@code.example.com",
        "sourceUrl": "https://app.forkmesh.com/forkmesh/forkmesh",
        "sourceRef": "main",
    }
    value.update(changes)
    return value


def sandbox_env(tmp_path):
    env = {key: value for key, value in os.environ.items()
           if key not in ("FORKMESH_HOME", "FORKMESH_PROJECTS_DIR")}
    for name in ("HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
        path = tmp_path / name.lower()
        path.mkdir(exist_ok=True)
        env[name] = str(path)
    env["FORKMESH_INSTANCE_PARSE_ONLY"] = "1"
    return env


def parse(tmp_path, value, stdin=False):
    env = sandbox_env(tmp_path)
    if stdin:
        line = base64.b64encode(json.dumps(value).encode()).decode() + "\n"
        return subprocess.run(["bash", str(SCRIPT), "--config-stdin"], input=line,
                              capture_output=True, text=True, env=env, timeout=60)
    path = tmp_path / "settings.json"
    path.write_text(json.dumps(value))
    return subprocess.run(["bash", str(SCRIPT), "--config", str(path)],
                          capture_output=True, text=True, env=env, timeout=60)


def values(output):
    found = {}
    for line in output.splitlines():
        key, _, rest = line.partition("=")
        found[key] = shlex.split(rest)[0] if rest.strip() else ""
    return found


def test_settings_from_a_file_and_from_stdin_are_normalized(tmp_path):
    for stdin in (False, True):
        result = parse(tmp_path, settings(), stdin=stdin)
        assert result.returncode == 0, result.stdout + result.stderr
        found = values(result.stdout)
        assert found["CFG_projectName"] == "Acme Code"
        assert found["CFG_domain"] == "code.example.com"
        assert found["CFG_domainProvided"] == "1"
        assert found["CFG_ownerName"] == "acme"
        assert found["ADMIN_PASSWORD_LENGTH"] == str(len(ADMIN_PASSWORD))
        assert found["MAIL_KEY_LENGTH"] == str(len(MAIL_KEY))
        assert found["REPO_URL"] == "https://code.example.com/acme/forkmesh"
        assert ADMIN_PASSWORD not in result.stdout and MAIL_KEY not in result.stdout


def test_user_owner_uses_the_admin_name(tmp_path):
    result = parse(tmp_path, settings(ownerType="user", ownerName="whatever",
                                      domainProvided=False))
    found = values(result.stdout)
    assert found["CFG_ownerName"] == "ada"
    assert found["CFG_domainProvided"] == "0"


@pytest.mark.parametrize("change", [
    {"projectName": "bad;name"},
    {"domain": "not a domain"},
    {"adminUsername": "Bad Name"},
    {"adminEmail": "nope"},
    {"adminPassword": "short"},
    {"ownerType": "team"},
    {"ownerName": "ada"},
    {"ownerName": "forkmesh"},
    {"mailProvider": "postmark"},
    {"mailApiKey": "has spaces in it"},
    {"mailApiKey": ""},
    {"mailProvider": "mailgun", "mailgunDomain": ""},
    {"sourceUrl": "http://example.com/x"},
    {"sourceRef": "-bad"},
])
def test_bad_settings_stop_before_anything_runs(tmp_path, change):
    result = parse(tmp_path, settings(**change))
    assert result.returncode == 2
    assert "is missing" in result.stdout or "not valid" in result.stdout or \
        "kept for" in result.stdout or "must differ" in result.stdout
    assert 'FMI {"step":"system","state":"failed"}' in result.stdout


def test_settings_that_reach_a_shell_are_quoted(tmp_path):
    result = parse(tmp_path, settings(projectName="Acme & Co"))
    assert result.returncode == 0
    assert values(result.stdout)["CFG_projectName"] == "Acme & Co"


def test_every_page_step_is_announced_in_order():
    order = ["system", "packages", "source", "build", "configure", "database",
             "services", "https", "admin", "repo", "done"]
    positions = [TEXT.index('begin %s "' % step) for step in order]
    assert positions == sorted(positions)
    assert 'printf \'FMI {"done":true,"url":"%s"}\\n\' "$REPO_URL"' in TEXT
    assert "progress \"$CURRENT_STEP\" failed" in TEXT


def test_secrets_never_reach_a_command_line_or_a_log():
    bootstrap = TEXT.split("create_admin() {", 1)[1].split("\n}\n", 1)[0]
    assert ("printf '%s\\n%s\\n%s\\n%s' \"$OWNERS_JSON\" \"${CFG_legalPages:-}\" "
            "\"${CFG_directory:-}\" \"$ADMIN_PASSWORD\" | python3") in bootstrap
    assert "host_cli instance-bootstrap" in bootstrap
    assert 'ADMIN_PASSWORD=""' in bootstrap and 'OWNERS_JSON="[]"' in bootstrap
    settings_writer = TEXT.split("write_settings() {", 1)[1].split("\n}\n", 1)[0]
    assert 'chmod 0640 "$tmp"' in settings_writer
    assert 'chown root:"$APP_USER" "$tmp"' in settings_writer
    assert "adminPassword" not in settings_writer.split("keys = [", 1)[1].split("]", 1)[0]
    assert "mailApiKey" not in settings_writer.split("keys = [", 1)[1].split("]", 1)[0]


def test_the_instance_runs_white_label_with_plugins_and_its_own_mail():
    for line in ("INSTANCE_NAME=$CFG_projectName", "INSTANCE_PLUGINS=$CFG_plugins",
                 "INSTANCE_FLAGSHIP_NODE=$NODE_NAME", "HOSTED_GIT_CORE_NODES=$NODE_NAME",
                 "MAIL_PROVIDER=$CFG_mailProvider", "MAIL_TRANSPORT=http",
                 "FORKMESH_PLATFORM=vps"):
        assert 'echo "%s"' % line in TEXT
    assert 'FORKMESH_EXCLUDED_PLUGINS="$EXCLUDED_PACKAGES"' in TEXT
    assert "apply_instance_brand.py" in TEXT
    assert "--node \"$NODE_NAME\" --owner forkmesh --repository forkmesh" in TEXT


def test_the_installer_deletes_no_folders():
    assert "rm -rf" not in TEXT
    assert "rm -r " not in TEXT


def function(name):
    return "%s() {%s\n}\n" % (name, TEXT.split("\n%s() {" % name, 1)[1].split("\n}\n", 1)[0])


def run_functions(tmp_path, names, body, stdin="", path_dir=None):
    script = tmp_path / "harness.sh"
    script.write_text("set -euo pipefail\nsay() { printf '%s\\n' \"$*\"; }\n"
                      + "".join(function(name) for name in names) + body)
    env = sandbox_env(tmp_path)
    if path_dir:
        env["PATH"] = "%s:%s" % (path_dir, env["PATH"])
    return subprocess.run(["bash", str(script)], input=stdin, capture_output=True,
                          text=True, env=env, timeout=60)


def test_command_output_streams_as_short_progress_lines(tmp_path):
    apt = "\n".join(["Reading package lists..."]
                    + ["0 upgraded, 30 newly installed, 0 to remove and 0 not upgraded.",
                       "Need to get 12.0 MB of archives."]
                    + ["Get:%d http://deb.debian.org/debian trixie/main x" % n for n in range(1, 51)]
                    + ["Setting up pkg%d (1.0) ..." % n for n in range(1, 31)]
                    + ["E: Could not get lock", "Waiting for cache lock: x\rWaiting for cache lock: y",
                       "Waiting for cache lock: z"]) + "\n"
    result = run_functions(tmp_path, ["apt_lines"], "apt_lines\n", apt)
    assert result.returncode == 0, result.stderr
    assert result.stdout.splitlines() == [
        "0 upgraded, 30 newly installed, 0 to remove and 0 not upgraded.",
        "Need to get 12.0 MB of archives.", "Downloaded 50 packages",
        "Set up 25 of 30 packages", "Set up 30 of 30 packages", "E: Could not get lock",
        "Another program is installing packages; waiting for it to finish."]
    pip = ("Requirement already satisfied: a\nCollecting httpx\nCollecting anyio\n"
           "Successfully installed anyio-4 httpx-0.28\n")
    result = run_functions(tmp_path, ["pip_lines"], "pip_lines\n", pip)
    assert result.stdout.splitlines() == [
        "Collecting httpx", "Collecting anyio", "Installed 2 Python packages.",
        "1 Python packages were already installed."]
    result = run_functions(tmp_path, ["lines"], "lines\n", "a\r\n\n   \n" + "b" * 600 + "\nlast")
    assert result.stdout.splitlines() == ["a ", "b" * 400, "last"]


def test_the_setup_runs_detached_and_a_new_connection_follows_it(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    run_dir, log = tmp_path / "run", tmp_path / "setup.log"
    (bin_dir / "systemd-run").write_text(
        "#!/bin/bash\nprintf '%s\\n' \"$@\" > \"$CALLS\"\n"
        "printf 'FMI {\"step\":\"system\",\"state\":\"running\"}\\nchecked\\n' >> \"$LOG\"\n"
        "printf '0\\n' > \"$RUN/status\"\n")
    (bin_dir / "systemctl").write_text("#!/bin/bash\n[ \"$1\" = show ] && echo 0\nexit 3\n")
    for tool in ("systemd-run", "systemctl"):
        (bin_dir / tool).chmod(0o755)
    body = """
CONFIG_MODE=stdin
RAW_CONFIG='{"adminPassword":"%s"}'
SETUP_UNIT=forkmesh-instance-setup
SETUP_RUN_DIR=%s
SETUP_LOG=%s
FORKMESH_DNS_WAIT_SECONDS=60
export CALLS=%s LOG=%s RUN=%s
echo old > "$SETUP_LOG"
start_detached
stat -c '%%a' "$SETUP_RUN_DIR/settings.json"
follow_setup
""" % (ADMIN_PASSWORD, run_dir, log, tmp_path / "calls", log, run_dir)
    result = run_functions(tmp_path, ["setup_running", "start_detached", "follow_setup"],
                           body, path_dir=bin_dir)
    assert result.returncode == 0, result.stdout + result.stderr
    out = result.stdout.splitlines()
    assert out[0] == "600"
    assert out[1:] == ['FMI {"step":"system","state":"running"}', "checked"]
    assert not (run_dir / "settings.json").exists()
    assert (tmp_path / "setup.log.1").read_text() == "old\n"
    calls = (tmp_path / "calls").read_text().splitlines()
    assert calls[:4] == ["--quiet", "--unit", "forkmesh-instance-setup", "--service-type=exec"]
    assert "--setenv=FORKMESH_DNS_WAIT_SECONDS=60" in calls
    assert "StandardOutput=append:%s" % log in calls
    assert calls[-3:] == ["--detached", "--config", "%s/settings.json" % run_dir]
    assert ADMIN_PASSWORD not in "\n".join(calls)


def test_starting_again_skips_finished_steps_and_follows_a_running_setup():
    main = TEXT.split("\nif can_detach; then\n", 1)[1].split("\nfi\n", 1)[0]
    assert main.index('flock -w 60 8') < main.index("if setup_running; then") < main.index("if start_detached; then")
    assert main.count("exec 8>&-") == 3
    assert "following it from its first line" in main
    assert 'tail -n +1 -s 0.5 -F --pid="$pid" "$SETUP_LOG"' in function("follow_setup")
    assert 'printf \'%s\\n\' "$code" > "$SETUP_RUN_DIR/status"' in function("on_exit")
    assert 'rm -f "$CONFIG_FILE"' in TEXT.split('if [ "$DETACHED" = "1" ] && [ "$CONFIG_FILE" = "$SETUP_RUN_DIR/settings.json" ]', 1)[1].split("\n", 1)[0]
    assert "The system packages are already installed." in function("install_packages")
    assert "The source is already at the latest" in function("fetch_source")
    assert '[ "$(cat "$BUILD_STAMP")" = "$(build_stamp "$SRC")" ]' in function("build_app")
    assert "The app is already running this version" in function("start_app")
    assert "is already live over HTTPS." in function("setup_https")
    assert "already installed" in function("install_daemon_binary")
    assert "The git node is already running" in function("start_git_node")
    assert "PYTHONUNBUFFERED=1" in TEXT


def test_https_is_verified_with_a_real_certificate_and_a_redirect():
    caddyfile = TEXT.split("\nwrite_caddyfile() {", 1)[1].split("\nEOF\n", 1)[0]
    assert 'Strict-Transport-Security "max-age=31536000; includeSubDomains"' in caddyfile
    live = function("https_live")
    assert '--resolve "$DOMAIN:443:127.0.0.1" "$ORIGIN/api/version"' in live
    assert " -k" not in live and "--insecure" not in live
    https = function("setup_https")
    assert 'caddy_news "$cursor" "$started"' in https
    assert "check_https" in https
    assert '"http://$DOMAIN/"' in function("check_https")


def test_caddy_sends_the_only_strict_transport_security_header():
    caddyfile = TEXT.split("\nwrite_caddyfile() {", 1)[1].split("\nEOF\n", 1)[0]
    assert caddyfile.lower().count("strict-transport-security") == 1
    assert (
        '\theader {\n\t\tdefer\n'
        '\t\tStrict-Transport-Security "max-age=31536000; includeSubDomains"\n\t}\n'
    ) in caddyfile
    site = caddyfile.split("\n$DOMAIN {\n", 1)[1]
    assert site.index("\theader {\n\t\tdefer") < site.index("handle @gateway")


DNS_FAKES = {
    "curl": """#!/bin/bash
case "$*" in
  *cloudflare-dns.com*) printf '%s' "$CLOUDFLARE_ANSWER" ;;
  *dns.google*) printf '%s' "$GOOGLE_ANSWER" ;;
  *http://code.example.com/cdn-cgi/trace*) [ -n "$PROXY" ] || exit 22; printf 'fl=1\r\nh=code.example.com\r\nvisit_scheme=http\r\n' ;;
  *https://code.example.com/api/version*) printf '%s' "$PUBLIC" ;;
  *cdn-cgi/trace*) printf 'fl=1\\nip=%s\\nts=1\\n' "$EGRESS" ;;
  *) exit 7 ;;
esac
""",
    "getent": "#!/bin/bash\n[ -n \"$HOSTS\" ] && printf '%s STREAM x\\n' $HOSTS\nexit 0\n",
    "ip": "#!/bin/bash\nprintf '2: eth0    inet %s/24 brd x scope global eth0\\n' \"$LOCAL\"\n",
}


def dns_answer(*addresses):
    return json.dumps({"Status": 0, "Answer": [{"name": "code.example.com.", "type": 5, "data": "edge.example.net."}]
                       + [{"name": "edge.example.net.", "type": 1, "data": a} for a in addresses]})


def dns_env(env):
    values = {"CLOUDFLARE_ANSWER": "", "GOOGLE_ANSWER": "", "HOSTS": "", "EGRESS": "", "LOCAL": "192.0.2.1",
              "PROXY": "", "PUBLIC": ""}
    values.update(env)
    return ("DOMAIN=code.example.com\nORIGIN=https://code.example.com\nCFG_publicAddress=203.0.113.10\n"
            "CFG_domainProvided=1\nPROXIED=0\nPROXY_WAIT_SECONDS=0\n%s\n" % "\n".join(
                "export %s=%s" % (k, shlex.quote(v)) for k, v in values.items()))


def fake_bin(tmp_path):
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    for name, text in DNS_FAKES.items():
        (bin_dir / name).write_text(text)
        (bin_dir / name).chmod(0o755)
    return bin_dir


@pytest.mark.parametrize("env, here", [
    ({"CLOUDFLARE_ANSWER": dns_answer("203.0.113.10")}, True),
    ({"CLOUDFLARE_ANSWER": json.dumps({"Status": 3}), "GOOGLE_ANSWER": dns_answer("203.0.113.10")}, True),
    ({"HOSTS": "127.0.1.1 203.0.113.10"}, True),
    ({"CLOUDFLARE_ANSWER": dns_answer("198.51.100.7"), "EGRESS": "198.51.100.7"}, True),
    ({"CLOUDFLARE_ANSWER": dns_answer("10.0.0.5"), "LOCAL": "10.0.0.5"}, True),
    ({"HOSTS": "127.0.1.1"}, False),
    ({"CLOUDFLARE_ANSWER": dns_answer("104.16.1.1"), "EGRESS": "203.0.113.10"}, False),
    ({}, False),
])
def test_dns_counts_as_ready_when_any_resolver_points_at_this_server(tmp_path, env, here):
    bin_dir = fake_bin(tmp_path)
    body = dns_env(env)
    body += "if resolves_here; then echo here; else echo elsewhere; fi\ndns_wait_message\n"
    names = ["dns_record_type", "dns_answers", "server_addresses", "resolves_here", "dns_wait_message"]
    result = run_functions(tmp_path, names, body, path_dir=bin_dir)
    assert result.returncode == 0, result.stdout + result.stderr
    out = result.stdout.splitlines()
    assert out[0] == ("here" if here else "elsewhere")
    assert out[-1] == "    code.example.com  A  203.0.113.10"
    if "104.16.1.1" in env.get("CLOUDFLARE_ANSWER", ""):
        assert out[1].startswith("code.example.com points at 104.16.1.1, not at this server.")
    if not env:
        assert out[1] == "Waiting for DNS. Add this record at your DNS provider:"


def test_a_live_certificate_skips_the_dns_wait():
    https = function("setup_https")
    assert https.index("https_live; then live_before=\"1\"") < https.index("write_caddyfile")
    assert https.index('if [ "$live_before" = "1" ]; then') < https.index("! dns_ready")


DNS_NAMES = ["dns_record_type", "dns_answers", "server_addresses", "resolves_here", "behind_cloudflare", "dns_ready"]


@pytest.mark.parametrize("env, ready", [
    ({"CLOUDFLARE_ANSWER": dns_answer("104.21.58.188", "172.67.206.66"), "PROXY": "1"}, "ready 1"),
    ({"CLOUDFLARE_ANSWER": dns_answer("203.0.113.10")}, "ready 0"),
    ({"CLOUDFLARE_ANSWER": dns_answer("104.16.1.1")}, "waiting 0"),
])
def test_a_name_behind_the_cloudflare_proxy_is_ready(tmp_path, env, ready):
    body = dns_env(env) + 'if dns_ready; then echo "ready $PROXIED"; else echo "waiting $PROXIED"; fi\n'
    result = run_functions(tmp_path, DNS_NAMES, body, path_dir=fake_bin(tmp_path))
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.splitlines() == [ready]


@pytest.mark.parametrize("public, code, text", [
    ("200 app ", 0, "https://code.example.com answers through Cloudflare."),
    ("308  https://code.example.com/api/version", 32, "Full (strict)"),
    ("521  ", 32, "Cloudflare error 521"),
    ("200  ", 32, "does not reach this server"),
])
def test_cloudflare_must_reach_this_server(tmp_path, public, code, text):
    body = (dns_env({"PROXY": "1", "PUBLIC": public})
            + "die() { printf 'Error: %s\\n' \"$1\"; exit \"${2:-1}\"; }\nsleep() { :; }\ncheck_cloudflare\n")
    result = run_functions(tmp_path, DNS_NAMES + ["check_cloudflare"], body, path_dir=fake_bin(tmp_path))
    assert result.returncode == code, result.stdout + result.stderr
    assert text in result.stdout


def test_a_name_without_the_proxy_skips_the_cloudflare_check(tmp_path):
    body = dns_env({}) + "check_cloudflare\necho checked\n"
    result = run_functions(tmp_path, DNS_NAMES + ["check_cloudflare"], body, path_dir=fake_bin(tmp_path))
    assert result.stdout.splitlines() == ["checked"], result.stdout + result.stderr


def test_visitors_behind_cloudflare_keep_their_address():
    caddyfile = TEXT.split("\nwrite_caddyfile() {", 1)[1].split("\nEOF\n", 1)[0]
    assert "@cloudflare remote_ip $CLOUDFLARE_RANGES" in caddyfile
    proxied = caddyfile.split("handle @cloudflare {", 1)[1].split("\n\t}\n", 1)[0]
    assert "Cf-Connecting-Ip" not in proxied and "header_up -X-Forwarded-For" in proxied
    assert caddyfile.index("handle @cloudflare {") < caddyfile.index("\thandle {")
    assert "header_up Cf-Connecting-Ip {remote_host}" in caddyfile.split("\thandle {", 1)[1]


def test_a_logo_is_checked_saved_and_used_by_the_build(tmp_path):
    png = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24
    logo = "data:image/jpeg;base64," + base64.b64encode(png).decode()
    result = parse(tmp_path, settings(logo=logo), stdin=True)
    assert result.returncode == 0, result.stdout + result.stderr
    found = values(result.stdout)
    assert found["CFG_logoType"] == "png"
    assert "CFG_logoData" not in found
    assert found["LOGO_DATA_LENGTH"] == str(len(base64.b64encode(png)))
    assert values(parse(tmp_path, settings()).stdout)["CFG_logoType"] == ""
    for bad in ("data:image/png;base64," + base64.b64encode(b"GIF89a" + b"\x00" * 9).decode(),
                "data:image/svg+xml;base64," + base64.b64encode(b"<svg onload=x()></svg>").decode(),
                "https://example.com/logo.png"):
        result = parse(tmp_path, settings(logo=bad))
        assert result.returncode == 2 and "Setting logo" in result.stdout, bad
    save = TEXT.split("save_logo() {", 1)[1].split("\n}\n", 1)[0]
    assert 'if [ -z "$CFG_logoType" ]; then return 0; fi' in save
    assert 'base64 -d > "$ETC/logo.$CFG_logoType.new"' in save
    build = TEXT.split("build_app() {", 1)[1].split("\n}\n", 1)[0]
    assert build.index("save_logo") < build.index("build_stamp")
    assert 'brand_args+=(--logo "$logo")' in build
    assert '"$(files_digest "$(logo_file)")"' in TEXT.split("build_stamp() {", 1)[1].split("\n}\n", 1)[0]
    assert 'echo "INSTANCE_LOGO=/instance-logo.' in TEXT


OWNER_PASSWORD = "grace-pass-word-8"  # forkmesh-secret-scan:ignore-line


def test_other_owners_are_checked_and_only_their_names_reach_the_shell(tmp_path):
    owners = [{"username": "Grace", "email": "grace@example.com", "password": OWNER_PASSWORD},
              {"username": "linus", "email": "linus@example.com", "password": OWNER_PASSWORD}]
    result = parse(tmp_path, settings(owners=owners))
    assert result.returncode == 0, result.stdout + result.stderr
    assert values(result.stdout)["CFG_owners"] == "grace,linus"
    assert OWNER_PASSWORD not in result.stdout
    assert values(parse(tmp_path, settings()).stdout)["CFG_owners"] == ""
    for bad in ("grace", [{"username": "ada", "email": "x@example.com", "password": OWNER_PASSWORD}],
                [{"username": "acme", "email": "x@example.com", "password": OWNER_PASSWORD}],
                [{"username": "grace", "email": "ada@example.com", "password": OWNER_PASSWORD}],
                [{"username": "grace", "email": "grace@example.com", "password": "short"}],
                owners[:1] * 2):
        result = parse(tmp_path, settings(owners=bad))
        assert result.returncode != 0, bad
    result = parse(tmp_path, settings(owners=owners, ownerType="user"))
    assert result.returncode != 0 and "ownerType org" in result.stdout + result.stderr


def test_the_bootstrap_settings_carry_the_owners_from_stdin():
    bootstrap = TEXT.split("create_admin() {", 1)[1].split("\n}\n", 1)[0]
    code = bootstrap.split("| python3 -c '", 1)[1].split("\n' ", 1)[0]
    owners = [{"username": "grace", "email": "grace@example.com", "password": OWNER_PASSWORD}]
    stdin = json.dumps(owners) + "\n\n\n" + ADMIN_PASSWORD + "\nwith a line break"
    result = subprocess.run(
        ["python3", "-c", code, "ada", "ada@example.com", "org", "acme", "Acme Code", "K" * 43, "", "1"],
        input=stdin, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    spec = json.loads(result.stdout)
    assert spec["singleOrganization"] is True
    assert spec["owners"] == owners
    assert spec["adminPassword"] == ADMIN_PASSWORD + "\nwith a line break"
    assert spec["ownerName"] == "acme"


def test_a_theme_is_a_preset_name_or_a_set_of_colours(tmp_path):
    result = parse(tmp_path, settings(theme="crimson"))
    assert result.returncode == 0, result.stdout
    assert values(result.stdout)["CFG_theme"] == "crimson"
    colours = {"dark": {"primary": "#F97316", "link": "#fb923c"}, "light": {"primary": "#ea580c"}}
    result = parse(tmp_path, settings(theme=colours), stdin=True)
    assert result.returncode == 0, result.stdout
    assert json.loads(values(result.stdout)["CFG_theme"]) == colours
    for bad in ({"sepia": {"primary": "#000000"}}, {"dark": {"primary": "red"}},
                {"dark": {"Primary;": "#000000"}}, {"dark": ["#000000"]},
                {"dark": {"primary": "#000000\"; rm"}}, "Not A Preset"):
        result = parse(tmp_path, settings(theme=bad))
        assert result.returncode == 2, bad
        assert "Setting theme must" in result.stdout


def test_the_bootstrap_settings_carry_a_colour_theme_as_an_object():
    bootstrap = TEXT.split("create_admin() {", 1)[1].split("\n}\n", 1)[0]
    code = bootstrap.split("| python3 -c '", 1)[1].split("\n' ", 1)[0]
    stdin = "[]\n\n\n" + ADMIN_PASSWORD
    colours = {"dark": {"primary": "#f97316"}}
    for theme, expected in ((json.dumps(colours), colours), ("crimson", "crimson"), ("", "")):
        result = subprocess.run(
            ["python3", "-c", code, "ada", "ada@example.com", "org", "acme", "Acme Code", "K" * 43, theme, "0"],
            input=stdin, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        assert json.loads(result.stdout)["theme"] == expected
        assert json.loads(result.stdout)["singleOrganization"] is False


def test_legal_pages_are_off_unless_named_and_never_set_by_an_update(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_legalPages"] == ""
    legal = {"terms": {"markdown": "# Terms\n\nIt's 'quoted'."},
             "privacy": "https://example.org/privacy", "security": None}
    result = parse(tmp_path, settings(legalPages=legal), stdin=True)
    assert result.returncode == 0, result.stdout
    assert json.loads(values(result.stdout)["CFG_legalPages"]) == {
        "terms": {"markdown": "# Terms\n\nIt's 'quoted'."},
        "privacy": {"url": "https://example.org/privacy"}}
    for bad in ({"cookies": {"url": "https://example.org"}},
                {"terms": {"markdown": "x", "url": "https://example.org"}},
                {"terms": {"url": "javascript:alert(1)"}},
                {"privacy": {"url": "https://example.org", "contact": "a@b.co"}},
                {"terms": {"markdown": "x" * 40000}}, ["terms"]):
        result = parse(tmp_path, settings(legalPages=bad))
        assert result.returncode == 2, bad
        assert "Setting legalPages" in result.stdout
    parser = TEXT.split("parse_config() {", 1)[1].split("\n' \"$CONFIG_MODE\"", 1)[0]
    assert 'legal = c.get("legalPages") or {}\nif update:\n    legal = {}' in parser
    assert "instance-bootstrap" not in function("update_admin")
    writer = TEXT.split('keys = ["projectName"', 1)[1].split('> "$INSTANCE_JSON.new"', 1)[0]
    assert "legalPages" not in writer


def test_the_bootstrap_settings_carry_the_legal_pages_from_stdin():
    bootstrap = TEXT.split("create_admin() {", 1)[1].split("\n}\n", 1)[0]
    assert ('"$OWNERS_JSON" "${CFG_legalPages:-}" "${CFG_directory:-}" '
            '"$ADMIN_PASSWORD"') in bootstrap
    code = bootstrap.split("| python3 -c '", 1)[1].split("\n' ", 1)[0]
    legal = {"terms": {"markdown": "# Terms\nline two"}}
    stdin = "[]\n" + json.dumps(legal) + "\n\n" + ADMIN_PASSWORD
    result = subprocess.run(
        ["python3", "-c", code, "ada", "ada@example.com", "org", "acme", "Acme Code", "K" * 43, "", "0"],
        input=stdin, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    spec = json.loads(result.stdout)
    assert spec["legalPages"] == legal
    assert spec["adminPassword"] == ADMIN_PASSWORD
    assert spec["directory"] == {}
    stdin = "[]\n\n" + json.dumps({"listed": True, "world": False}) + "\n" + \
        ADMIN_PASSWORD
    result = subprocess.run(
        ["python3", "-c", code, "ada", "ada@example.com", "org", "acme", "Acme Code", "K" * 43, "", "0"],
        input=stdin, capture_output=True, text=True, timeout=30)
    spec = json.loads(result.stdout)
    assert spec["directory"] == {"listed": True, "world": False}
    assert spec["adminPassword"] == ADMIN_PASSWORD


def test_the_directory_listing_is_opt_in_and_never_set_by_an_update(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_directory"] == ""
    result = parse(tmp_path, settings(directory={"listed": True, "world": True}),
                   stdin=True)
    assert result.returncode == 0, result.stdout
    assert json.loads(values(result.stdout)["CFG_directory"]) == {
        "listed": True, "world": True}
    for bad in ({"listed": "yes"}, {"hidden": True}, ["listed"], "on"):
        result = parse(tmp_path, settings(directory=bad))
        assert result.returncode == 2, bad
        assert "Setting directory" in result.stdout
    parser = TEXT.split("parse_config() {", 1)[1].split("\n' \"$CONFIG_MODE\"", 1)[0]
    assert 'directory = c.get("directory") or {}\nif update:\n    directory = {}' in parser
    writer = TEXT.split('keys = ["projectName"', 1)[1].split('> "$INSTANCE_JSON.new"', 1)[0]
    assert "directory" not in writer


def test_the_app_may_reach_the_directory_and_nothing_else_at_forkmesh_com():
    unit = function("write_app_unit")
    assert "Environment=FORKMESH_EGRESS_PRODUCTION_ALLOW=$DIRECTORY_HOST" in unit
    assert 'DIRECTORY_URL="https://app.forkmesh.com"' in TEXT
    assert 'DIRECTORY_HOST="app.forkmesh.com"' in TEXT
    settings_writer = TEXT.split("write_settings() {", 1)[1].split("\n}\n", 1)[0]
    assert 'echo "INSTANCE_DIRECTORY_URL=$DIRECTORY_URL"' in settings_writer


def test_the_admin_console_can_ask_for_an_update():
    updater = TEXT.split("write_updater() {", 1)[1].split("\n}\n", 1)[0]
    assert 'install -m 0700 "$SRC/tools/instance-update.sh"' in updater
    assert 'mv -f "$UPDATE_LIB/instance-update.sh.new" "$UPDATE_LIB/instance-update.sh"' in updater
    assert 'install -d -m 2750 -o root -g "$APP_USER" "$UPDATE_STATE"' in updater
    assert 'install -d -m 0750 -o "$APP_USER" -g "$APP_USER" "$UPDATE_REQUEST_DIR"' in updater
    assert "PathExists=$UPDATE_REQUEST_DIR/request" in updater
    assert "ExecStart=/bin/bash $UPDATE_LIB/instance-update.sh" in updater
    start = TEXT.split("start_app() {", 1)[1].split("\n}\n", 1)[0]
    assert start.index("write_updater") < start.index("write_app_unit")
    settings_writer = TEXT.split("write_settings() {", 1)[1].split("\n}\n", 1)[0]
    for line in ('INSTANCE_UPDATE_REQUEST=$UPDATE_REQUEST_DIR/request',
                 'INSTANCE_UPDATE_STATUS=$UPDATE_STATE/status.json',
                 'BUILD_INFO_FILE=$BUILD_INFO', 'APP_VERSION=$(release_version)'):
        assert 'echo "%s"' % line in settings_writer
    assert 'UPDATE_REQUEST_DIR="$VAR/update"' in TEXT
    assert 'UPDATE_STATE="/var/lib/forkmesh-instance-update"' in TEXT
    cleanup = TEXT.split("write_first_login_cleanup() {", 1)[1].split("\n}\n", 1)[0]
    assert "/etc/systemd/system/forkmesh-instance-update.path" in cleanup
    assert "fm_safe_rm_tree /usr/local/lib /usr/local/lib/forkmesh-instance-update" in cleanup


def test_the_build_records_its_version_and_commit(tmp_path):
    src = tmp_path / "src"
    (src / "desktop").mkdir(parents=True)
    (src / "desktop" / "CMakeLists.txt").write_text(
        "project(ForkMesh VERSION 1.4.2 LANGUAGES CXX)\n")
    env = sandbox_env(tmp_path)
    env.update(GIT_CONFIG_GLOBAL="/dev/null", GIT_AUTHOR_NAME="t",
               GIT_AUTHOR_EMAIL="t@example.com", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@example.com")
    for args in (["init", "-q"], ["add", "-A"], ["commit", "-qm", 'Ship "notes"']):
        subprocess.run(["git", "-C", str(src), *args], env=env, check=True)
    info = tmp_path / "build-info.json"
    body = ('SRC=%s\nBUILD_INFO=%s\nCFG_sourceUrl=https://example.com/f\n'
            'CFG_sourceRef=main\nwrite_build_info\n' % (shlex.quote(str(src)),
                                                       shlex.quote(str(info))))
    script = tmp_path / "harness.sh"
    script.write_text("set -euo pipefail\n" + function("release_version")
                      + function("write_build_info") + "write_daemon_state() { :; }\n" + body)
    result = subprocess.run(["bash", str(script)], env=env, capture_output=True,
                            text=True, timeout=60)
    assert result.returncode == 0, result.stderr
    value = json.loads(info.read_text())
    assert value["version"] == "1.4.2" and value["subject"] == 'Ship "notes"'
    assert len(value["commit"]) == 40 and value["ref"] == "main"
    assert oct(info.stat().st_mode & 0o777) == "0o644"


def _git_node_config_patch():
    body = TEXT[TEXT.index("setup_git_node() {"):]
    start = body.index("<<'PY'\n") + len("<<'PY'\n")
    return body[start:body.index("\nPY\n", start)]


def _upstream_page(tmp_path, source, platform=""):
    body = function("platform_source") + function("upstream_page")
    return subprocess.run(
        ["bash", "-c", body + 'DOMAIN=code.example.com; '
         'RELEASE_SOURCE=https://app.forkmesh.com/forkmesh/forkmesh; '
         'CFG_sourceUrl="$1"; CFG_platformUrl="$2"; upstream_page', "x",
         source, platform], capture_output=True, text=True,
        env=sandbox_env(tmp_path), timeout=30, check=True).stdout.strip()


def test_the_platform_copy_is_configured_as_a_labelled_read_only_mirror(tmp_path):
    assert _upstream_page(tmp_path, "https://app.forkmesh.com/forkmesh/forkmesh") == \
        "https://forkmesh.com/forkmesh/forkmesh"
    assert _upstream_page(tmp_path, "https://git.example.org/acme/forkmesh.git") == \
        "https://git.example.org/acme/forkmesh"
    assert _upstream_page(
        tmp_path, "https://code.example.com/acme/forkmesh-acme.git") == \
        "https://forkmesh.com/forkmesh/forkmesh"
    assert _upstream_page(
        tmp_path, "https://code.example.com/acme/forkmesh-acme",
        "https://git.example.org/acme/forkmesh.git") == \
        "https://git.example.org/acme/forkmesh"
    assert '--upstream "$(platform_source)"' in TEXT
    assert 'echo "INSTANCE_UPSTREAM=$(upstream_page)"' in TEXT
    script = tmp_path / "patch.py"
    script.write_text(_git_node_config_patch())
    path = tmp_path / "mirror-node.json"
    upstream = "https://app.forkmesh.com/forkmesh/forkmesh"
    path.write_text(json.dumps({
        "schemaVersion": 1, "upstreams": {"forkmesh/forkmesh": [upstream]},
        "gatewayConfig": str(tmp_path / "missing-gateway.json")}))
    subprocess.run(
        ["python3", str(script), str(path), "https://code.example.com", "abc",
         "KEY", "acme", "https://forkmesh.com/forkmesh/forkmesh"],
        check=True, env=sandbox_env(tmp_path), timeout=30)
    full = json.loads((tmp_path / "mirror-node.json.new").read_text())
    assert full["mirrorOf"] == {
        "forkmesh/forkmesh": "https://forkmesh.com/forkmesh/forkmesh"}
    assert full["eventDrivenUpstreams"] == [upstream]
    assert full["role"] == "core"
    without_mirror = json.loads(
        (tmp_path / "mirror-node.json.nomirror").read_text())
    assert "mirrorOf" not in without_mirror and without_mirror["role"] == "core"
    without_role = json.loads(
        (tmp_path / "mirror-node.json.norole").read_text())
    assert not {"mirrorOf", "role", "hosted"} & set(without_role)
    checks = TEXT[TEXT.index('chown "$NODE_USER:$NODE_USER" "$DAEMON_CONFIG.new"'):]
    assert checks.index('"$DAEMON_CONFIG.new" --check') < checks.index(
        '"$DAEMON_CONFIG.nomirror" --check') < checks.index(
        'mv "$DAEMON_CONFIG.norole" "$DAEMON_CONFIG"')
def test_actions_default_on_and_must_be_a_boolean(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_actions"] == "1"
    found = values(parse(tmp_path, settings(actions=False)).stdout)
    assert found["CFG_actions"] == "0"
    result = parse(tmp_path, settings(actions="yes"))
    assert result.returncode == 2
    assert "Setting actions must be true or false." in result.stdout


def test_single_organization_is_off_by_default_and_reaches_the_bootstrap(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_singleOrganization"] == "0"
    assert found["REPO_URL"] == "https://code.example.com/acme/forkmesh"
    found = values(parse(tmp_path, settings(singleOrganization=True)).stdout)
    assert found["CFG_singleOrganization"] == "1"
    assert found["REPO_URL"] == "https://code.example.com/forkmesh"
    result = parse(tmp_path, settings(singleOrganization="no"))
    assert result.returncode == 2
    assert "Setting singleOrganization must be true or false." in result.stdout
    result = parse(tmp_path, settings(singleOrganization=True, ownerType="user"))
    assert result.returncode == 2
    assert "Setting singleOrganization needs ownerType org." in result.stdout
    create = function("create_admin")
    assert '"singleOrganization": sys.argv[8] == "1"' in create
    assert '"$CFG_theme" "$CFG_singleOrganization"' in create
    assert "instance-bootstrap" not in function("update_admin")


def test_actions_install_job_tools_and_configure_the_app():
    packages = TEXT.split("ACTIONS_PACKAGES=(", 1)[1].split(")", 1)[0].split()
    for name in ("git", "curl", "jq", "python3-venv", "nodejs", "npm",
                 "build-essential", "unzip"):
        assert name in packages
    assert 'PACKAGES+=("${ACTIONS_PACKAGES[@]}")' in TEXT
    writer = function("write_settings")
    assert 'bot_seed="$(openssl rand -hex 32)"' in writer
    assert 'bot_seed="$(env_value ACTIONS_BOT_SEED)"' in writer
    assert 'echo "ACTIONS_BOT_SEED=$bot_seed"' in writer
    assert 'echo "RUNNER_POOL_TARGET_NODES=$NODE_NAME"' in writer
    assert 'echo "ACTIONS_INSTANCE_RUNNER=$RUNNER_NAME"' in writer
    assert '"plugins", "actions", "poweredBy", "platformUrl",' in writer
    assert '"webFirewall", "whiteLabel"]' in writer
    assert 'RUNNER_NAME=""' in TEXT
    assert "  resolve_runner_name\n" in writer
    assert '[ -n "$RUNNER_NAME" ] || resolve_runner_name' in function("setup_actions_runner")


@pytest.mark.parametrize("kept, owner, expected", [
    ("", "acme", "acme-runner"),
    ("forkmesh-runner", "acme", "forkmesh-runner"),
    ("Bad Name", "acme", "acme-runner"),
    ("", "a" * 60, "instance-runner"),
])
def test_the_instance_runner_is_named_after_the_owner_and_keeps_its_name(
        tmp_path, kept, owner, expected):
    app_env = tmp_path / "app.env"
    app_env.write_text("ACTIONS_INSTANCE_RUNNER=%s\n" % kept if kept else "")
    result = run_functions(
        tmp_path, ["env_value", "resolve_runner_name"],
        "APP_ENV=%s\nCFG_ownerName=%s\nresolve_runner_name\necho \"$RUNNER_NAME\"\n"
        % (shlex.quote(str(app_env)), shlex.quote(owner)))
    assert result.stdout.strip().splitlines()[-1] == expected


def test_the_actions_runner_is_preregistered_after_the_git_node():
    main = TEXT.rsplit("\ncheck_system\n", 1)[1]
    assert main.index("start_git_node") < main.index("setup_actions_runner") < main.index("finish")
    runner = function("setup_actions_runner")
    assert '"$SRC/$RUNNER_INSTALLER" --preregistered' in runner
    assert '--name "$RUNNER_NAME" --owner "$CFG_ownerName" --app "$ORIGIN"' in runner
    assert '--binary "$DAEMON"' in runner
    assert "host_cli instance-runner --name \"$RUNNER_NAME\"" in runner
    assert '--public-key "$runner_key" --owner "$CFG_ownerName"' in runner
    assert "FM_RUNNER_PUBLIC_KEY=" in runner
    assert 'install -m 0755 "$SRC/tools/forkmesh-actions.sh" /usr/local/bin/forkmesh-actions' in runner
    assert "/usr/local/bin/forkmesh-actions" in function("write_first_login_cleanup")
    assert '[[ "$runner_key" =~ ^[A-Za-z0-9_-]{43}$ ]]' in runner
    assert 'systemctl disable --now "$RUNNER_UNIT"' in runner
    assert 'RUNNER_INSTALLER="www/public/install-runner.sh"' in TEXT
    assert 'RUNNER_UNIT="forkmesh-runner@$CFG_ownerName.service"' in TEXT


def test_an_unclaimed_instance_also_removes_its_runner():
    cleanup = function("write_first_login_cleanup")
    assert 'fm_safe_rm_tree /etc/forkmesh-runner/accounts "/etc/forkmesh-runner/accounts/$RUNNER_OWNER"' in cleanup
    assert 'fm_safe_rm_tree /var/lib/forkmesh-runner/accounts "/var/lib/forkmesh-runner/accounts/$RUNNER_OWNER"' in cleanup
    assert "fm_safe_rm_tree /etc /etc/forkmesh-runner" in cleanup
    assert "fm_safe_rm_tree /var/lib /var/lib/forkmesh-runner" in cleanup
    assert 'systemctl disable --now "forkmesh-runner@$RUNNER_OWNER.service"' in cleanup
    assert "userdel forkmesh-job" in cleanup
    assert cleanup.count('"$CLEANUP_DIR/created-runner-root"') >= 3
    baseline = function("capture_first_login_baseline")
    assert ': > "$CLEANUP_DIR/created-runner-root"' in baseline
    assert ': > "$CLEANUP_DIR/created-job-user"' in baseline


def test_only_a_start_page_launch_arms_the_first_login_removal():
    arm = function("arm_first_login_timeout")
    assert '[ -n "${CFG_launchId:-}" ] || return 0' in arm
    assert arm.index("CFG_launchId") < arm.index("INSTANCE_LOGIN_DEADLINE")
    assert "Environment=FORKMESH_EGRESS_PRODUCTION_ALLOW=$DIRECTORY_HOST,$DOMAIN" in TEXT


def test_the_first_login_cleanup_removes_only_what_the_install_created(tmp_path):
    cleanup = function("write_first_login_cleanup")
    script = cleanup.split("<<'EOF'\n", 1)[1].split("\nEOF\n", 1)[0]
    part = 'RUNNER_OWNER=""\n' + script.split('RUNNER_OWNER=""\n', 1)[1].split(
        'if [ -e "$CLEANUP_DIR/caddyfile.before" ]', 1)[0]
    root = tmp_path / "root"
    for path in ("/etc/forkmesh-runner", "/var/lib/forkmesh-runner",
                 "/etc/systemd/system"):
        part = part.replace(path, str(root) + path)
    assert part.count(str(root)) >= 8
    assert "fm_safe_rm_tree /etc " + str(root) in part
    part = part.replace("fm_safe_rm_tree /etc ", "fm_safe_rm_tree %s/etc " % root)
    part = part.replace("fm_safe_rm_tree /var/lib ", "fm_safe_rm_tree %s/var/lib " % root)
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    calls = tmp_path / "calls"
    for tool in ("systemctl", "nft"):
        (bin_dir / tool).write_text(
            "#!/bin/bash\nprintf '%%s %%s\\n' %s \"$*\" >> %s\n" % (tool, calls))
        (bin_dir / tool).chmod(0o755)
    cleanup_dir = tmp_path / "cleanup"
    cleanup_dir.mkdir()
    (cleanup_dir / "created-runner").write_text("acme\n")
    for name in ("acme", "other"):
        for base in ("etc/forkmesh-runner/accounts", "var/lib/forkmesh-runner/accounts"):
            (root / base / name).mkdir(parents=True)
            (root / base / name / "keep").write_text("x")
    harness = tmp_path / "cleanup.sh"
    harness.write_text("set -euo pipefail\nCLEANUP_DIR=%s\nsource %s\n%s" % (
        cleanup_dir, REPO / "tools/forkmesh-safe-rm.sh", part))
    env = sandbox_env(tmp_path)
    env["PATH"] = "%s:%s" % (bin_dir, env["PATH"])
    result = subprocess.run(["bash", str(harness)], capture_output=True, text=True,
                            env=env, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not (root / "etc/forkmesh-runner/accounts/acme").exists()
    assert not (root / "var/lib/forkmesh-runner/accounts/acme").exists()
    assert (root / "etc/forkmesh-runner/accounts/other/keep").exists()
    assert (root / "var/lib/forkmesh-runner/accounts/other/keep").exists()
    log = calls.read_text()
    assert "disable --now forkmesh-runner@acme.service" in log
    assert "forkmesh-runner-firewall" not in log
    (cleanup_dir / "created-runner-root").write_text("")
    result = subprocess.run(["bash", str(harness)], capture_output=True, text=True,
                            env=env, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert not (root / "etc/forkmesh-runner").exists()
    assert not (root / "var/lib/forkmesh-runner").exists()
    assert "forkmesh-runner-firewall" in calls.read_text()

def test_the_installer_writes_and_keeps_the_turnstile_hostname():
    writer = function("write_settings")
    assert "grep -qE '^TURNSTILE_HOSTNAMES=.' \"$APP_ENV\"" in writer
    assert 'echo "TURNSTILE_HOSTNAMES=$DOMAIN"' in writer


def test_platform_commits_carry_the_instance_name_and_a_noreply_address():
    assert 'Environment="FORKMESH_COMMIT_NAME=$CFG_projectName"' in TEXT
    assert "Environment=FORKMESH_COMMIT_EMAIL=noreply@$DOMAIN" in TEXT
    identity = function("write_runner_identity")
    assert '/etc/systemd/system/$RUNNER_UNIT.d' in identity
    assert 'FORKMESH_COMMIT_EMAIL=noreply@%s' in identity
    assert "  write_runner_identity\n" in function("setup_actions_runner")
    assert 'rm -f "/etc/systemd/system/forkmesh-runner@$RUNNER_OWNER.service.d/identity.conf"' in TEXT
    go = (REPO / "server" / "commit_identity.go").read_text(encoding="utf-8")
    assert 'os.Getenv("FORKMESH_COMMIT_NAME")' in go
    assert 'os.Getenv("FORKMESH_COMMIT_EMAIL")' in go
    for name in ("merge.go", "intake.go", "runner.go", "agent_workspace.go",
                 "issue_reconcile.go", "pull_metadata_refs.go"):
        source = (REPO / "server" / name).read_text(encoding="utf-8")
        assert '+ "@users.noreply.forkmesh.com"' not in source, name
        assert '+"@users.noreply.forkmesh.com"' not in source, name


def test_the_platform_mirror_source_is_an_optional_setting(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_platformUrl"] == ""
    found = values(parse(tmp_path, settings(
        platformUrl="https://app.forkmesh.com/forkmesh/forkmesh")).stdout)
    assert found["CFG_platformUrl"] == "https://app.forkmesh.com/forkmesh/forkmesh"
    result = parse(tmp_path, settings(platformUrl="http://insecure.example/x"))
    assert result.returncode != 0 and "platformUrl" in result.stdout + result.stderr


PRIVATE_PATH = "/manage-x7k2q9"


def test_an_admin_path_setting_is_checked_and_never_echoed(tmp_path):
    result = parse(tmp_path, settings(adminPath=PRIVATE_PATH))
    assert result.returncode == 0, result.stdout
    assert PRIVATE_PATH not in result.stdout
    assert values(result.stdout)["ADMIN_PATH_LENGTH"] == str(len(PRIVATE_PATH))
    assert values(parse(tmp_path, settings()).stdout)["ADMIN_PATH_LENGTH"] == "0"
    for bad in ("manage-x7k2q9", "/short", "/a/b-c-d-e-f", "/manage.x7k2q9",
                "/" + "a" * 65, "/admin", "/dashboard", "/Forgot-Password"):
        refused = parse(tmp_path, settings(adminPath=bad))
        assert refused.returncode == 2, bad
        assert "adminPath" in refused.stdout and "not valid" in refused.stdout


def _choose(tmp_path, mode, new, setting, existing):
    app_env = tmp_path / "app.env"
    app_env.write_text("DATA_KEY=x\n" + ("ADMIN_PATH=%s\n" % existing if existing else ""))
    body = ('APP_ENV=%s\nCONFIG_MODE=%s\nNEW_INSTANCE=%s\nCFG_adminPath=%s\n'
            'choose_admin_path\nprintf "%%s|%%s\\n" "$ADMIN_PATH_SOURCE" "$ADMIN_PATH_VALUE"\n'
            % (shlex.quote(str(app_env)), mode, new, shlex.quote(setting)))
    result = run_functions(tmp_path, ["env_value", "new_admin_path", "choose_admin_path"], body)
    assert result.returncode == 0, result.stderr
    return tuple(result.stdout.strip().split("|"))


def test_a_fresh_install_writes_a_path_and_an_update_never_changes_or_adds_one(tmp_path):
    import re
    source, value = _choose(tmp_path, "file", "1", "", "")
    assert source == "generated" and re.fullmatch(r"/admin-[a-z0-9]{20}", value)
    assert _choose(tmp_path, "file", "1", "", "")[1] != value
    assert _choose(tmp_path, "file", "1", PRIVATE_PATH, "") == ("settings", PRIVATE_PATH)
    assert _choose(tmp_path, "file", "0", "", "") == ("", "")
    assert _choose(tmp_path, "file", "0", PRIVATE_PATH, "") == ("settings", PRIVATE_PATH)
    assert _choose(tmp_path, "file", "1", "/other-x7k2q9z", PRIVATE_PATH) == ("kept-other", PRIVATE_PATH)
    assert _choose(tmp_path, "file", "1", PRIVATE_PATH, PRIVATE_PATH) == ("kept", PRIVATE_PATH)
    assert _choose(tmp_path, "update", "0", "", PRIVATE_PATH) == ("kept", PRIVATE_PATH)
    assert _choose(tmp_path, "update", "0", "", "") == ("", "")
    writer = function("write_settings")
    assert "choose_admin_path" in writer
    assert 'if [ -n "$ADMIN_PATH_VALUE" ]; then echo "ADMIN_PATH=$ADMIN_PATH_VALUE"; fi' in writer
    assert "adminPath" not in TEXT.split('keys = ["projectName"', 1)[1].split("]", 1)[0]


def _summary(tmp_path, detached, mode, owner="0"):
    log = tmp_path / "setup.log"
    log.write_text("")
    fake = tmp_path / "bin"
    fake.mkdir(exist_ok=True)
    (fake / "stat").write_text(
        '#!/bin/sh\ncase "$2" in %%u) echo %s ;; %%a) echo %s ;; esac\n' % (owner, mode))
    (fake / "stat").chmod(0o755)
    body = ('SETUP_LOG=%s\nDETACHED=%s\nAPP_ENV=/etc/forkmesh-instance/app.env\n'
            'ORIGIN=https://code.example.com\nADMIN_PATH_VALUE=%s\n'
            'for ADMIN_PATH_SOURCE in generated settings kept-other kept ""; do say_admin_path; done\n'
            % (shlex.quote(str(log)), detached, PRIVATE_PATH))
    result = run_functions(tmp_path, ["setup_log_private", "say_admin_path"], body,
                           path_dir=str(fake))
    assert result.returncode == 0, result.stderr
    return result.stdout


def test_the_summary_prints_a_generated_path_only_where_it_stays_private(tmp_path):
    shown = _summary(tmp_path, "0", "644")
    assert "Admin console: https://code.example.com%s" % PRIVATE_PATH in shown
    assert shown.count(PRIVATE_PATH) == 1
    assert "from your settings" in shown and "was kept" in shown
    private_log = _summary(tmp_path, "1", "600")
    assert private_log.count(PRIVATE_PATH) == 1
    for mode, owner in (("640", "0"), ("644", "0"), ("600", "1000")):
        hidden = _summary(tmp_path, "1", mode, owner)
        assert PRIVATE_PATH not in hidden
        assert "it is ADMIN_PATH in /etc/forkmesh-instance/app.env" in hidden
    finish = function("finish")
    assert finish.index("Sign in as @") < finish.index("say_admin_path")


def _write_settings(tmp_path, mode, new, before):
    etc = tmp_path / "etc"
    etc.mkdir(exist_ok=True)
    app_env = etc / "app.env"
    app_env.write_text(before)
    body = "\n".join([
        "begin() { :; }", "chown() { :; }", "git() { echo 0123abc; }",
        "upstream_page() { :; }", "release_version() { echo 1.0.0; }",
        "logo_file() { :; }", "files_digest() { cat \"$@\" 2>/dev/null | sha256sum; }",
        "APP_ENV=%s" % shlex.quote(str(app_env)),
        "INSTANCE_JSON=%s" % shlex.quote(str(etc / "instance.json")),
        " ".join("CFG_%s=" % key for key in (
            "mailApiKey", "mailFrom", "mailgunDomain", "mailgunRegion",
            "domainProvided", "publicAddress", "ownerType", "ownerName",
            "sourceUrl", "sourceRef", "plugins", "poweredBy")),
        "LOGIN_MARKER=/x UPDATE_REQUEST_DIR=/x UPDATE_STATE=/x BUILD_INFO=/x",
        "RUNNER_NAME=forkmesh-runner INSTANCE_INSTALLER_VERSION=2",
        "CONFIG_MODE=%s" % mode, "NEW_INSTANCE=%s" % new, "CFG_adminPath=",
        "APP_USER=forkmesh-app VENV=/nonexistent SRC=/nonexistent NODE_NAME=forkmesh",
        "CFG_projectName=Acme CFG_domain=code.example.com CFG_mailProvider=none",
        "CFG_actions=0 CFG_adminUsername=ada CFG_adminEmail=ada@example.com",
        "ORIGIN=https://code.example.com DOMAIN=code.example.com",
        "DIRECTORY_URL=https://directory.example.org",
        "write_settings", ""])
    result = run_functions(
        tmp_path, ["env_value", "resolve_runner_name", "new_admin_path",
                   "choose_admin_path", "write_settings"], body)
    assert result.returncode == 0, result.stderr + result.stdout
    return app_env.read_text(), (etc / "instance.json").read_text()


def test_an_update_rewrites_app_env_but_keeps_a_hand_set_admin_path(tmp_path):
    keys = ("DATA_KEY=d\nADMIN_PASS=p\nMIRROR_ROUTER_SIGNING_SEED=s\n"
            "MIRROR_ROUTER_PUBLIC_KEY=k\n")
    after, instance = _write_settings(
        tmp_path, "update", "0", keys + "ADMIN_PATH=%s\n" % PRIVATE_PATH)
    assert after.count("ADMIN_PATH=") == 1
    assert "ADMIN_PATH=%s\n" % PRIVATE_PATH in after
    assert "INSTANCE_NAME=Acme\n" in after and "DATA_KEY=d\n" in after
    assert PRIVATE_PATH not in instance
    after, _ = _write_settings(tmp_path, "update", "0", keys)
    assert "ADMIN_PATH=" not in after
    after, _ = _write_settings(tmp_path, "file", "1", keys)
    assert re.search(r"^ADMIN_PATH=/admin-[a-z0-9]{20}$", after, re.M)


def test_white_label_is_off_unless_the_settings_ask_for_it(tmp_path):
    found = values(parse(tmp_path, settings()).stdout)
    assert found["CFG_whiteLabel"] == "0"
    found = values(parse(tmp_path, settings(whiteLabel=True)).stdout)
    assert found["CFG_whiteLabel"] == "1"
    assert parse(tmp_path, settings(whiteLabel="yes")).returncode != 0
    assert 'brand_args+=(--white-label)' in TEXT
    assert 'echo "INSTANCE_WHITE_LABEL=${CFG_whiteLabel:-0}"' in TEXT
