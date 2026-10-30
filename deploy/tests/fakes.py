"""Test doubles for the Vultr/Cloudflare HTTP layer and SSH."""

import json
import subprocess


class FakeHttp:
    """Stands in for next_common.JsonHttp; ``routes`` maps (METHOD, path
    prefix) to a response dict or a callable(body) -> dict."""

    def __init__(self, routes=None):
        self.routes = dict(routes or {})
        self.calls = []

    def request(self, method, path, body=None, expect=None):
        self.calls.append((method, path, body))
        best = None
        for (m, prefix), value in self.routes.items():
            if m == method and path.startswith(prefix):
                if best is None or len(prefix) > len(best[0]):
                    best = (prefix, value)
        if best is None:
            raise AssertionError("unexpected %s %s" % (method, path))
        value = best[1]
        return value(body) if callable(value) else json.loads(json.dumps(value))

    def mutations(self):
        return [c for c in self.calls if c[0] != "GET"]


class FakeSsh:
    """Records scripts; ``answers`` is a list of (predicate(host, script),
    stdout bytes | callable) consulted in order."""

    def __init__(self, answers=None, returncode=0, port=822):
        self.answers = list(answers or [])
        self.runs = []
        self.puts = []
        self.tunnels = []
        self.returncode = returncode
        self.ports = {}
        self.default_port = port
        self.strict = []
        self.strict_answer = subprocess.CompletedProcess(["ssh"], 0, b"", b"")

    def port(self, host):
        return self.ports.get(host, self.default_port)

    def use_port(self, host, port):
        self.ports[host] = port

    def run_strict(self, host, script, timeout=None):
        self.strict.append((host, script, self.port(host)))
        return self.strict_answer

    def run(self, host, script, input_bytes=None, check=True, timeout=None):
        self.runs.append((host, script, input_bytes))
        for predicate, answer in self.answers:
            if predicate(host, script):
                if callable(answer):
                    answer = answer(host, script, input_bytes)
                if isinstance(answer, subprocess.CompletedProcess):
                    return answer
                return subprocess.CompletedProcess(["ssh"], self.returncode, answer, b"")
        return subprocess.CompletedProcess(["ssh"], self.returncode, b"", b"")

    def put(self, host, local, remote):
        self.puts.append((host, str(local), remote))

    def tunnel(self, host, local_port, remote="127.0.0.1:4001", bind="127.0.0.1"):
        self.tunnels.append((host, local_port, remote, bind))

        class Tunnel:
            def terminate(self):
                pass

            def wait(self, timeout=None):
                return 0
        return Tunnel()


def instance(label, tags=("forkmesh-next",), ip="198.51.100.10",
             instance_id="11111111-2222-3333-4444-555555555555"):
    return {"id": instance_id, "label": label, "tags": list(tags), "main_ip": ip,
            "status": "active", "plan": "vc2-1c-1gb", "region": "ewr"}


def repo_fleet_instances():
    import tomllib
    from pathlib import Path
    fleet_toml = Path(__file__).resolve().parents[1] / "fleet.toml"
    nodes = tomllib.loads(fleet_toml.read_text()).get("nodes") or []
    out = {}
    for node in nodes:
        number = int(str(node["name"])[1:])
        out[node.get("label") or "fmnext-core-%d" % number] = {
            "ip": "203.0.113.%d" % (10 + number),
            "ipv6": "2001:19f0:ffff::%d" % (10 + number)}
    out["fmnext-run-1"] = {"ip": "203.0.113.21"}
    return out
