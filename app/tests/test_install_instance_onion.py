import json
import os
import subprocess
from pathlib import Path

from test_install_instance_script import parse, sandbox_env, settings, values

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "www" / "public" / "install-instance.sh"
TEXT = SCRIPT.read_text(encoding="utf-8")
ADDRESS = "abcdefghijklmnopqrstuvwxyz234567abcdefghijklmnopqrstuvwx.onion"
FUNCTIONS = ("say", "warn", "files_digest", "onion_torrc", "onion_caddy", "onion_location",
             "remember_onion_setting", "reload_caddy_if_running", "remove_onion_service", "setup_onion")


def function(name):
    for line in TEXT.splitlines():
        if line.startswith(name + "() {") and line.endswith("}"):
            return line + "\n"
    start = TEXT.index("\n%s() {" % name) + 1
    body = TEXT[start:TEXT.index("\n}\n", start)]
    marker = "\nEOF\n}\n" if "<<EOF" in body else "\nPY\n}\n" if "<<'PY'" in body else "\n}\n"
    return TEXT[start:TEXT.index(marker, start) + len(marker)]


def harness(tmp_path, onion, body):
    fake = tmp_path / "bin"
    fake.mkdir(exist_ok=True)
    log = tmp_path / "calls.log"
    (fake / "systemctl").write_text(
        "#!/bin/bash\n"
        "printf '%s\\n' \"$*\" >> \"$CALLS\"\n"
        "case \"$*\" in\n"
        "  'restart tor') mkdir -p \"$FAKE_ONION_DIR\"; printf '%s\\n' \"$FAKE_ADDRESS\" > \"$FAKE_ONION_DIR/hostname\" ;;\n"
        "  'is-active --quiet tor') [ -s \"$FAKE_ONION_DIR/hostname\" ] ;;\n"
        "  'is-active --quiet caddy') exit 0 ;;\n"
        "esac\n")
    (fake / "tor").write_text("#!/bin/bash\nexit 0\n")
    (fake / "journalctl").write_text("#!/bin/bash\nexit 0\n")
    for item in fake.iterdir():
        item.chmod(0o755)
    etc = tmp_path / "etc"
    (etc / "tor").mkdir(parents=True, exist_ok=True)
    torrc = etc / "tor" / "torrc"
    if not torrc.exists():
        torrc.write_text("SocksPort 0\n")
    instance = etc / "instance.json"
    if not instance.exists():
        instance.write_text(json.dumps({"domain": "code.example.com"}))
    prelude = "\n".join([
        "set -euo pipefail",
        'die() { printf "Error: %s\\n" "$1"; exit 9; }',
        'DOMAIN="code.example.com"; APP_PORT=8080; GATEWAY_PORT=8790',
        'INSTANCE_JSON="%s"' % instance,
        'ONION_DIR="%s"' % (tmp_path / "lib" / "tor" / "forkmesh-instance"),
        'ONION_PORT=8091',
        'ONION_TORRC_DIR="%s"' % (etc / "tor" / "torrc.d"),
        'ONION_TORRC="$ONION_TORRC_DIR/forkmesh-instance.conf"',
        'TOR_CONFIG="%s"' % torrc,
        'ONION_CADDY="%s"' % (etc / "caddy" / "conf.d" / "forkmesh-onion.caddy"),
        'ONION_HEADER="%s"' % (etc / "caddy" / "site.d" / "forkmesh-onion-location.caddy"),
        'ONION_WAIT_SECONDS=4',
        'ONION_HOST=""',
        'CFG_onionService="%s"' % ("1" if onion else "0"),
    ]) + "\n"
    script = prelude + "".join(function(name) for name in FUNCTIONS) + body
    env = sandbox_env(tmp_path)
    env.update({"PATH": "%s:%s" % (fake, os.environ["PATH"]), "CALLS": str(log),
                "FAKE_ONION_DIR": str(tmp_path / "lib" / "tor" / "forkmesh-instance"),
                "FAKE_ADDRESS": ADDRESS})
    result = subprocess.run(["bash", "-c", script], capture_output=True, text=True, env=env, timeout=60)
    calls = log.read_text().splitlines() if log.exists() else []
    log.write_text("")
    return result, calls, etc


def test_onion_service_is_an_opt_in_boolean_setting(tmp_path):
    result = parse(tmp_path, settings())
    assert result.returncode == 0, result.stdout + result.stderr
    assert values(result.stdout)["CFG_onionService"] == "0"
    result = parse(tmp_path, settings(onionService=True))
    assert values(result.stdout)["CFG_onionService"] == "1"
    result = parse(tmp_path, settings(onionService="yes"))
    assert result.returncode != 0
    assert "onionService must be true or false" in result.stdout + result.stderr


def test_tor_is_installed_only_when_asked_and_runs_before_the_app_starts():
    assert 'if [ "${CFG_onionService:-0}" = "1" ]; then\n  PACKAGES+=(tor)\nfi\n' in TEXT
    main = TEXT[TEXT.rindex("\ncheck_system\n"):]
    assert main.index("write_settings\n") < main.index("setup_onion\n") < main.index("start_app\n")
    unit = TEXT.split("write_app_unit() {", 1)[1].split("\nEOF\n", 1)[0]
    assert "Environment=FORKMESH_PUBLIC_HOSTS=$DOMAIN${ONION_HOST:+,$ONION_HOST}\n" in unit
    assert "Environment=FORKMESH_VAR_INSTANCE_ONION=%s" in unit
    assert "FORKMESH_EGRESS_PRODUCTION_ALLOW=$DIRECTORY_HOST,$DOMAIN\n" in unit


def test_the_onion_site_is_plain_http_on_loopback_and_reaches_the_same_app(tmp_path):
    result, _, _ = harness(tmp_path, True, 'onion_caddy "%s"\nonion_location "%s"\nonion_torrc\n'
                           % (ADDRESS, ADDRESS))
    assert result.returncode == 0, result.stdout + result.stderr
    out = result.stdout
    assert "http://%s:8091 {\n\tbind 127.0.0.1\n" % ADDRESS in out
    assert "\t\treverse_proxy 127.0.0.1:8790\n" in out
    assert "\t\treverse_proxy 127.0.0.1:8080 {\n\t\t\theader_up Host %s\n" % ADDRESS in out
    assert "header_up -X-Forwarded-For" in out
    assert '\t\tContent-Security-Policy "; upgrade-insecure-requests" ""\n' in out
    assert "\t\t-Strict-Transport-Security\n" in out and "tls" not in out.split("\n", 1)[1]
    assert 'header Onion-Location "http://%s{uri}"' % ADDRESS in out
    assert "HiddenServicePort 80 127.0.0.1:8091\n" in out
    assert "HiddenServiceDir %s/\n" % (tmp_path / "lib" / "tor" / "forkmesh-instance") in out


def test_setup_onion_is_idempotent_and_remembers_the_setting(tmp_path):
    body = 'setup_onion\nprintf "HOST=%s\\n" "$ONION_HOST"\n'
    result, calls, etc = harness(tmp_path, True, body)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "HOST=%s" % ADDRESS in result.stdout
    assert "restart tor" in calls and "reload caddy" in calls
    torrc = (etc / "tor" / "torrc").read_text()
    assert torrc.count("%include ") == 1 and torrc.startswith("SocksPort 0\n")
    assert (etc / "caddy" / "conf.d" / "forkmesh-onion.caddy").is_file()
    assert json.loads((etc / "instance.json").read_text())["onionService"] is True
    again, calls, etc = harness(tmp_path, True, body)
    assert again.returncode == 0, again.stdout + again.stderr
    assert "restart tor" not in calls and "reload caddy" not in calls
    assert (etc / "tor" / "torrc").read_text() == torrc
    off, calls, etc = harness(tmp_path, False, body)
    assert off.returncode == 0, off.stdout + off.stderr
    assert "HOST=\n" in off.stdout
    assert not (etc / "caddy" / "conf.d" / "forkmesh-onion.caddy").exists()
    assert not (etc / "caddy" / "site.d" / "forkmesh-onion-location.caddy").exists()
    assert (tmp_path / "lib" / "tor" / "forkmesh-instance" / "hostname").is_file()
    assert "onionService" not in json.loads((etc / "instance.json").read_text())
    assert "reload caddy" in calls


def test_a_missing_onion_address_stops_the_setup(tmp_path):
    result, _, _ = harness(tmp_path, True, "setup_onion\n")
    assert result.returncode == 0
    (tmp_path / "lib" / "tor" / "forkmesh-instance" / "hostname").write_text("not-an-onion\n")
    bad, _, _ = harness(tmp_path, True, "setup_onion\n")
    assert bad.returncode == 9
    assert "does not hold a v3 onion address" in bad.stdout


def test_the_instance_api_and_brand_script_name_the_onion_address():
    runtime = (REPO / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
    route = runtime.split('if url.path in ("/api/instance", "/api/instance/"):', 1)[1].split("cache_control=", 1)[0]
    assert '"onion": instance_onion_address(self.env)' in route
    source = runtime.split("ONION_ALPHABET = ", 1)[1].split("\n\n\nasync def", 1)[0]
    names = {}
    exec("ONION_ALPHABET = " + source, names)
    check = names["instance_onion_address"]

    class Env:
        pass
    env = Env()
    assert check(env) is None
    env.INSTANCE_ONION = ADDRESS.upper()
    assert check(env) == ADDRESS
    env.INSTANCE_ONION = "example.com"
    assert check(env) is None
    env.INSTANCE_ONION = ADDRESS.replace("a", "1", 1)
    assert check(env) is None
    brand = (REPO / "app" / "public" / "instance-brand.js").read_text(encoding="utf-8")
    assert "onionNote(data.onion)" in brand
    assert "/^[a-z2-7]{56}\\.onion$/" in brand
