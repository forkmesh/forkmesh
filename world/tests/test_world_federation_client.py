import json
import shutil
import subprocess
from pathlib import Path

import pytest

import world_mesh

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
NODE = shutil.which("node")

SCRIPT = r"""
import { WorldFederation, FEDERATED_PATHS, federatedPath, federationOrigin } from "./world-federation.js";
import { worldMeshSocketURL } from "./world-mesh.js";

const token = "fmwv1.eyJ2IjoxfQ." + "c".repeat(64);
const hub = { id: "forkmesh", api: "https://app.example.com", world: "https://world.example.com" };
const calls = [];
let clock = 1000;
let session = { status: 200, body: { token, expiresAt: 1000 + 15 * 60 * 1000, name: "alice", home: "code.example.org" } };
let visas = 0;
const fetchImpl = async (url, init) => {
  calls.push({ url, method: init.method, auth: init.headers.authorization || "", credentials: init.credentials, mode: init.mode, body: init.body || "" });
  if (url.endsWith("/api/world/visa/session")) {
    return { ok: session.status === 200, status: session.status, text: async () => JSON.stringify(session.body) };
  }
  return { ok: true, status: 200, text: async () => JSON.stringify({ path: url }) };
};
const fed = new WorldFederation({ requestVisa: async () => { visas += 1; return "visa-" + visas; }, fetchImpl, now: () => clock });
const out = {};
out.lazy = calls.length;
out.first = await fed.fetchJSON(hub, "/api/world/events", { maxAge: 5000 });
out.cached = await fed.fetchJSON(hub, "/api/world/events", { maxAge: 5000 });
out.afterFirst = calls.map((call) => [call.method, call.url, call.auth ? "bearer" : "", call.credentials, call.mode]);
out.exchangeBody = JSON.parse(calls[0].body);
out.session = fed.visitorSession(hub);
const [a, b] = await Promise.all([fed.token(hub), fed.token(hub)]);
out.sameToken = a === token && b === token && visas === 1;
clock += 15 * 60 * 1000 - 30 * 1000;
await fed.token(hub);
out.renewed = visas === 2;
out.refused = [];
for (const path of ["/api/accounts/me", "/api/world/events#x", "/api/world/visa", "https://evil.example.net/api/world/events"]) {
  try { await fed.fetchJSON(hub, path); out.refused.push("sent"); } catch (error) { out.refused.push(error.message); }
}
out.badOrigins = [federationOrigin("http://app.example.com"), federationOrigin("https://u:p@app.example.com"), federationOrigin("https://app.example.com/x")];
const old = { id: "b".repeat(32), api: "https://old.example.org", world: "https://old.example.org" };
session = { status: 404, body: { error: "not_found" } };
out.fallback = await fed.credential(old);
out.unsupportedOnce = (await fed.token(old)) === "" && calls.filter((call) => call.url.startsWith("https://old.example.org")).length === 1;
const strict = { id: "c".repeat(32), api: "https://strict.example.org", world: "https://strict.example.org" };
const blocked = new WorldFederation({ requestVisa: async () => { visas += 1; return "visa-" + visas; },
  fetchImpl: async (url) => { calls.push({ url }); throw new TypeError("Failed to fetch"); }, now: () => clock });
out.corsFallback = await blocked.credential(strict);
const strictCalls = calls.filter((call) => call.url.startsWith("https://strict.example.org")).length;
out.corsBackoff = (await blocked.credential(strict)).visa !== undefined
  && calls.filter((call) => call.url.startsWith("https://strict.example.org")).length === strictCalls;
const guest = new WorldFederation({ requestVisa: async () => "", fetchImpl, now: () => clock });
out.guest = await guest.credential(hub);
out.sockets = [
  worldMeshSocketURL("https://app.example.com", { visitor: token }),
  worldMeshSocketURL("https://app.example.com", { visa: "v" }),
  worldMeshSocketURL("https://app.example.com", { watch: true, visitor: token }),
];
out.paths = FEDERATED_PATHS;
out.federated = [federatedPath("/api/world/office/floors/"), federatedPath("/api/world/office/floors?x=1"), federatedPath("/api/tasks")];
console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    run = subprocess.run([NODE, "--input-type=module", "-e", SCRIPT],
                         cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    return json.loads(run.stdout)


def test_nothing_is_fetched_until_an_owners_ground_needs_it(result):
    assert result["lazy"] == 0


def test_calls_go_to_the_owner_with_a_bearer_token_and_no_cookies(result):
    assert result["afterFirst"] == [
        ["POST", "https://app.example.com/api/world/visa/session", "",
         "omit", "cors"],
        ["GET", "https://app.example.com/api/world/events", "bearer",
         "omit", "cors"],
    ]
    assert result["exchangeBody"] == {"visa": "visa-1"}
    assert result["first"] == result["cached"]
    assert result["session"] == {"nodeName": "alice@code.example.org",
                                 "visitor": True, "owner": "forkmesh"}


def test_tokens_are_shared_renewed_and_fall_back_to_the_visa(result):
    assert result["sameToken"] is True
    assert result["renewed"] is True
    assert result["fallback"] == {"visa": "visa-3"}
    assert result["unsupportedOnce"] is True
    assert result["guest"] == {}
    assert result["corsFallback"] == {"visa": "visa-4"}
    assert result["corsBackoff"] is True
    token = "fmwv1.eyJ2IjoxfQ." + "c" * 64
    assert result["sockets"] == [
        "wss://app.example.com/api/world/ws?visitor=" + token,
        "wss://app.example.com/api/world/ws?visa=v",
        "wss://app.example.com/api/world/ws?watch=1",
    ]


def test_only_federated_world_paths_cross_and_only_to_https_owners(result):
    assert result["refused"] == ["not_federated"] * 4
    assert result["badOrigins"] == ["", "", "https://app.example.com"]
    assert result["federated"] == ["/api/world/office/floors/",
                                   "/api/world/office/floors?x=1", ""]
    assert set(result["paths"]) == set(world_mesh.FEDERATED_PATHS)


def test_the_world_uses_the_federation_for_visit_sockets():
    assert "requestVisa: (world) => this.worldFederation().credential(world)," \
        in APP
    assert ('import { WorldFederation, meshRootRoute } from '
            '"./world-federation.js";') in APP
    assert "? this.worldFederation().fetchJSON(world, path, options)" in APP
