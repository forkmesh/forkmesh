"""install-instance.sh never re-opens a web port the operator narrowed.

The installer used to look for a literal ``80/tcp`` rule in ``ufw status``
and, finding none, run ``ufw allow 80/tcp`` (and 443) on every update. An
operator who had limited the web ports to a CDN with rules such as
``80,443/tcp ALLOW <cidr>`` had the origin silently re-opened to everyone.
"""

import importlib.util
import json
import subprocess
from pathlib import Path

import pytest

from test_install_instance_script import parse, settings, values

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "www" / "public" / "install-instance.sh"
TEXT = SCRIPT.read_text(encoding="utf-8")


def _tool(tmp_path):
    body = TEXT.split("<<'WEBFIREWALL'\n", 1)[1].split("\nWEBFIREWALL\n", 1)[0]
    path = tmp_path / "web_firewall.py"
    path.write_text(body, encoding="utf-8")
    spec = importlib.util.spec_from_file_location("web_firewall", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _rules(tmp_path, *tuples):
    path = tmp_path / "user.rules"
    path.write_text("*filter\n" + "".join(
        "### tuple ### %s\n-A ufw-user-input -j ACCEPT\n" % item
        for item in tuples) + "COMMIT\n", encoding="utf-8")
    return (str(path),)


def test_a_narrowed_port_list_is_seen_as_narrow(tmp_path):
    tool = _tool(tmp_path)
    paths = _rules(
        tmp_path,
        "allow any 22 0.0.0.0/0 any 0.0.0.0/0 in",
        "allow tcp 80,443 0.0.0.0/0 any 173.245.48.0/20 in "
        "comment=636c6f7564666c617265")
    assert tool.port_state(80, paths) == "narrow"
    assert tool.port_state(443, paths) == "narrow"


@pytest.mark.parametrize("rule, port, expected", [
    ("allow tcp 80 0.0.0.0/0 any 0.0.0.0/0 in", 80, "open"),
    ("allow tcp 80 ::/0 any ::/0 in", 80, "open"),
    ("allow tcp 80 0.0.0.0/0 any 0.0.0.0/0 in", 443, "none"),
    ("allow tcp 400:500 0.0.0.0/0 any 198.51.100.0/24 in", 443, "narrow"),
    ("allow any any 0.0.0.0/0 any 198.51.100.7 in", 80, "none"),
    ("allow tcp 80 0.0.0.0/0 any 198.51.100.7 in", 80, "narrow"),
    ("allow tcp 80 0.0.0.0/0 any 0.0.0.0/0 out_eth0", 80, "none"),
    ("allow tcp 80,443 0.0.0.0/0 any 0.0.0.0/0 Nginx%20Full - in", 443, "open"),
    ("deny tcp 80 0.0.0.0/0 any 0.0.0.0/0 in", 80, "none"),
    ("allow udp 443 0.0.0.0/0 any 0.0.0.0/0 in", 443, "none"),
    ("allow tcp 80 0.0.0.0/0 any 0.0.0.0/0 out", 80, "none"),
])
def test_port_states(tmp_path, rule, port, expected):
    tool = _tool(tmp_path)
    assert tool.port_state(port, _rules(tmp_path, rule)) == expected


def test_the_update_step_opens_only_ports_no_rule_mentions():
    step = TEXT.split("configure_web_firewall() {", 1)[1].split("\n}\n", 1)[0]
    assert 'narrow) say "Port $port is limited' in step
    assert 'none)\n        if ufw allow "$port/tcp"' in step
    assert "ufw allow 80/tcp" not in TEXT and "ufw allow 443/tcp" not in TEXT
    assert "22" not in step
    packages = TEXT.split("install_packages() {", 1)[1].split("\n}\n", 1)[0]
    assert "configure_web_firewall" in packages


CLOUDFLARE_V4 = "173.245.48.0/20\n103.21.244.0/22\n104.16.0.0/13\n"
CLOUDFLARE_V6 = "2400:cb00::/32\n2606:4700::/32\n"


def test_cloudflare_mode_allows_new_ranges_before_closing_anything(tmp_path):
    tool = _tool(tmp_path)
    state = tmp_path / "state" / "cloudflare"
    state.parent.mkdir()
    state.write_text("198.51.100.0/24\n173.245.48.0/20\n")
    calls = []
    ranges = tool.refresh_cloudflare(
        fetch=lambda url: CLOUDFLARE_V4 if url.endswith("v4") else CLOUDFLARE_V6,
        ufw=lambda *args: calls.append(args), state=str(state))
    assert len(ranges) == 5
    allows = [c for c in calls if c[0] == "allow"]
    deletes = [c for c in calls if c[0] == "delete"]
    assert calls.index(allows[-1]) < calls.index(deletes[0])
    assert ("allow", "proto", "tcp", "from", "2606:4700::/32", "to", "any",
            "port", "80,443", "comment", "forkmesh-cloudflare") in allows
    assert ("delete", "allow", "proto", "tcp", "from", "198.51.100.0/24", "to",
            "any", "port", "80,443") in deletes
    assert ("delete", "allow", "80/tcp") in deletes
    assert not any(part in ("22", "22/tcp", "ssh") for call in calls
                   for part in call)
    assert state.read_text().split() == ranges


@pytest.mark.parametrize("reply", ["", "<html>error</html>", "0.0.0.0/0\n" * 5,
                                   "10.0.0.1/8\n",
                                   CLOUDFLARE_V4 + "100.0.0.0/11\n",
                                   CLOUDFLARE_V6 + "2a00::/27\n"])
def test_a_bad_range_list_changes_nothing(tmp_path, reply):
    tool = _tool(tmp_path)
    calls = []
    with pytest.raises(ValueError):
        tool.refresh_cloudflare(fetch=lambda url: reply,
                                ufw=lambda *args: calls.append(args),
                                state=str(tmp_path / "state"))
    assert calls == []


def test_the_setting_is_validated_and_kept(tmp_path):
    found = values(parse(tmp_path, settings(webFirewall="cloudflare")).stdout)
    assert found["CFG_webFirewall"] == "cloudflare"
    assert values(parse(tmp_path, settings()).stdout)["CFG_webFirewall"] == ""
    refused = parse(tmp_path, settings(webFirewall="closed"))
    assert refused.returncode == 2 and "webFirewall" in refused.stdout
    writer = TEXT.split("write_settings() {", 1)[1].split("\n}\n", 1)[0]
    assert '"webFirewall"]' in writer and '"${CFG_webFirewall:-}"' in writer


def test_the_tool_is_valid_python(tmp_path):
    _tool(tmp_path)
    result = subprocess.run(
        ["python3", str(tmp_path / "web_firewall.py")], capture_output=True,
        text=True)
    assert result.returncode == 2
