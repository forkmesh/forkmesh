from pathlib import Path
from types import SimpleNamespace

import world as world_protocol
import world_mesh


ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world"
APP = (WORLD / "public" / "world" / "world.js").read_text(encoding="utf-8")
MESH = (WORLD / "public" / "world" / "world-mesh.js").read_text(encoding="utf-8")
HEXES = (WORLD / "public" / "world" / "scene" / "instance-hexes.js").read_text(encoding="utf-8")
MOVEMENT = (WORLD / "public" / "world" / "scene" / "movement.js").read_text(encoding="utf-8")
RELAY = (WORLD / "src" / "world_durable_objects.py").read_text(encoding="utf-8")
ROUTES = (WORLD / "src" / "world_routes.py").read_text(encoding="utf-8")
RUNTIME = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")

SECRET = b"s" * 32
INSTANCE_ID = "a" * 32
HUB_ENV = SimpleNamespace(
    WORLD_ORIGIN="https://world.forkmesh.com",
    APP_ORIGIN="https://app.forkmesh.com",
)
INSTANCE_ENV = SimpleNamespace(
    INSTANCE_NAME="Example Code",
    WORLD_ORIGIN="https://code.example.org",
    APP_ORIGIN="https://code.example.org",
)


def instance_row(**overrides):
    row = {
        "id": INSTANCE_ID,
        "name": "Example Code",
        "url": "https://code.example.org/",
        "has_logo": 1,
        "slot": 3,
        "updated_at": 7,
    }
    row.update(overrides)
    return row


def hub_directory():
    return world_mesh.build_directory(
        world_mesh.hub_entry(HUB_ENV), [instance_row()],
        world_mesh.own_api_origin(HUB_ENV))


def test_hub_directory_lists_the_hub_and_instance_worlds_with_slots():
    worlds = hub_directory()
    assert [entry["id"] for entry in worlds] == ["forkmesh", INSTANCE_ID]
    hub, instance = worlds
    assert hub["world"] == "https://world.forkmesh.com"
    assert hub["api"] == "https://app.forkmesh.com"
    assert hub["slot"] == -1
    assert instance["world"] == instance["api"] == "https://code.example.org"
    assert instance["slot"] == 3
    assert instance["logo"] == (
        "https://app.forkmesh.com/api/start/instances/%s/logo?v=7"
        % INSTANCE_ID)
    assert world_mesh.self_id(HUB_ENV, worlds) == "forkmesh"


def test_directory_drops_duplicate_slots_bad_urls_and_out_of_range_slots():
    rows = [
        instance_row(),
        instance_row(id="b" * 32, url="https://other.example.org/", slot=3),
        instance_row(id="c" * 32, url="http://plain.example.org/", slot=4),
        instance_row(id="d" * 32, url="https://far.example.org/", slot=12),
    ]
    worlds = world_mesh.build_directory(
        world_mesh.hub_entry(HUB_ENV), rows, "https://app.forkmesh.com")
    assert [entry["id"] for entry in worlds] == ["forkmesh", INSTANCE_ID]


def test_an_instance_reads_the_hub_directory_and_finds_itself():
    payload = world_mesh.directory_payload(HUB_ENV, hub_directory())
    worlds = world_mesh.normalize_directory(payload)
    assert len(worlds) == 2
    assert world_mesh.self_id(INSTANCE_ENV, worlds) == INSTANCE_ID
    assert world_mesh.normalize_directory({"worlds": worlds[1:]}) == []


def test_only_neighbouring_world_origins_may_open_cross_world_sockets():
    worlds = hub_directory()
    assert world_mesh.neighbor_for_origin(
        HUB_ENV, worlds, "https://code.example.org")["id"] == INSTANCE_ID
    assert world_mesh.neighbor_for_origin(
        HUB_ENV, worlds, "https://world.forkmesh.com") is None
    assert world_mesh.neighbor_for_origin(
        HUB_ENV, worlds, "https://evil.example.net") is None
    assert world_mesh.neighbor_for_origin(
        INSTANCE_ENV, worlds, "https://world.forkmesh.com")["id"] == "forkmesh"


def test_a_visa_is_bound_to_its_home_destination_and_lifetime():
    now = 1_800_000_000_000
    claim = world_mesh.visa_for(
        "alice", "https://world.forkmesh.com", "https://code.example.org",
        "nonce1234", now)
    visa = world_mesh.visa_encode(SECRET, claim)
    assert world_mesh.visa_unverified_home(visa) == "https://world.forkmesh.com"
    verified = world_mesh.visa_verify(
        SECRET, visa, now + 1000, "https://world.forkmesh.com",
        "https://code.example.org")
    assert verified == {"name": "alice", "nonce": "nonce1234"}
    assert world_mesh.visa_verify(
        b"x" * 32, visa, now + 1000, "https://world.forkmesh.com",
        "https://code.example.org") is None
    assert world_mesh.visa_verify(
        SECRET, visa, now + 1000, "https://world.forkmesh.com",
        "https://other.example.org") is None
    assert world_mesh.visa_verify(
        SECRET, visa, now + world_mesh.VISA_TTL_MS + 1,
        "https://world.forkmesh.com", "https://code.example.org") is None
    payload, signature = visa.rsplit(".", 1)
    assert world_mesh.visa_verify(
        SECRET, payload + "." + "0" * 64, now,
        "https://world.forkmesh.com", "https://code.example.org") is None


def test_visitors_carry_a_home_and_never_fold_into_a_local_account():
    assert "home" in world_protocol.WORLD_PUBLIC_FIELDS
    assert "visiting" in world_protocol.WORLD_SPACE_VALUES
    assert world_protocol.default_presence("abcd", 1)["home"] == ""
    local = world_protocol.account_presence_key("alice")
    visitor = world_protocol.account_presence_key("alice", "world.forkmesh.com")
    assert local == "alice"
    assert visitor == "alice@world.forkmesh.com"
    state = world_protocol.default_presence("abcd", 1)
    state.update({"accountStatus": "Registered", "home": "code.example.org"})
    _kind, updated = world_protocol.sanitize_message(
        {"type": "presence", "faceImage": True,
         "solana": "So11111111111111111111111111111111111111112"},
        state, 2)
    assert updated["faceImage"] is False
    assert updated["solana"] == ""


def test_relay_keeps_receive_only_watch_sockets_out_of_presence():
    assert 'self.ctx.acceptWebSocket(server, to_js(["watch"]))' in RELAY
    assert '"x-forkmesh-world-watch"' in RELAY
    assert 'state["home"] = mesh_home' in RELAY
    assert "WORLD_WATCH_FRAME_TYPES" in RELAY
    assert world_protocol.WORLD_WATCH_FRAME_TYPES == {
        "join", "presence", "move", "leave"}


def test_cross_world_sockets_and_visas_are_routed():
    assert '"/api/world/mesh"' in RUNTIME
    assert '"/api/world/visa"' in RUNTIME
    assert '"/api/world/visa/verify"' in RUNTIME
    assert "world_mesh_visitor(" in RUNTIME
    assert '"/api/world/visa/verify"' in ROUTES
    assert '"redirect": "error"' in ROUTES


def test_walking_into_a_neighbour_switches_worlds_without_a_page_change():
    assert "/api/world/mesh?refresh=" in APP
    assert "this.meshLinks.queueMove(this.pendingMovement);" in APP
    assert "safe.space = WORLD_MESH_VISITING_SPACE;" in APP
    assert "this.world?.worldMeshOwner?.(x, z)" in APP
    assert "Entering ${world.name}" in APP
    assert "this.worldMeshLinks()?.setNeighbors(" in APP
    portal_handler = APP.split("onInstanceHexagonSelect")[1].split(
        "const destination = safeHTTPURL(url);")[0]
    assert "window.open(" not in portal_handler
    assert 'url.searchParams.set("watch", "1")' in MESH
    assert 'url.searchParams.set("visa", visa)' in MESH
    assert "/api/world/ws" in MESH


def test_neighbour_worlds_are_drawn_and_walkable_as_full_flowers():
    assert "WORLD_MESH_FLOWER_HEXES" in HEXES
    assert "forkmesh-instance-hex-sand" in HEXES
    assert 'image.crossOrigin = "anonymous"' in HEXES
    assert "export function setWorldMeshFlowers" in MOVEMENT
    assert "export function worldMeshOwnerAt" in MOVEMENT
    assert "value === WORLD_MESH_SPACE" in MOVEMENT
