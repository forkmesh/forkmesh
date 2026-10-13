#!/usr/bin/env python3
"""Contracts for the instant World push surge on mirror-node cabinets.

When a public catalog publication advances a node's served head (code was
pushed onto that mirror), the outer Worker rings the live World room. Clients
treat that frame as a doorbell only: they re-read the signed public mirror
payload, and the scene plays its world-visible surge from the verified commit
change — never from unauthenticated relay data.
"""

import ast
import asyncio
import hmac
from pathlib import Path
from types import SimpleNamespace
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
CATALOG_ROUTES = ROOT / "src" / "catalog_routes.py"
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
WORLD_DIR = ROOT.parent / "world" / "public" / "world"
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. This contract still covers the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[1] / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8") + WORLD_ROUTE_SOURCE
CATALOG_ROUTES_TEXT = CATALOG_ROUTES.read_text(encoding="utf-8")
WORLD_DURABLE_OBJECTS_TEXT = WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8")
APP = (WORLD_DIR / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()


def _load(*names, extra_globals=None):
    trees = (
        ast.parse(ENTRY_TEXT, filename=str(ENTRY)),
        ast.parse(
            WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8"),
            filename=str(WORLD_DURABLE_OBJECTS),
        ),
    )
    selected = [
        node for tree in trees for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra_globals or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def _run(value):
    return asyncio.run(value)


class _Headers:
    def __init__(self, values):
        self.values = {
            str(key).lower(): str(value) for key, value in values.items()
        }

    def get(self, name, default=None):
        return self.values.get(str(name).lower(), default)


class _Request:
    def __init__(self, data, headers=None, method="POST"):
        self._data = data
        self.headers = _Headers(headers or {})
        self.method = method
        self.url = "https://forkmesh.internal/api/world/mirror-push"

    async def json(self):
        return self._data


def _response(payload, status=200, **kwargs):
    return {"payload": payload, "status": status, **kwargs}


async def _bounded_json(request, max_bytes=None):
    return await request.json()


def _mirror_push_runtime():
    import re as re_module
    runtime = _load(
        "ForkMeshWorld", "_world_mirror_push_signature",
        "_world_mirror_push_changed_files",
        extra_globals={
            "DurableObject": type("DurableObject", (), {}),
            "_account_session_secret": lambda env: b"unit-secret",
            "method_name": lambda request: request.method,
            "json_response": _response,
            "bounded_json_request": _bounded_json,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "MAX_NODE_NAME": 64,
            "re": re_module,
            "hmac": hmac,
        })
    instance = runtime["ForkMeshWorld"]()
    instance.env = None
    frames = []
    instance._broadcast = lambda frame, **_kwargs: frames.append(frame)
    return instance, runtime["_world_mirror_push_signature"], frames


def test_signed_head_advance_relays_short_commit_and_bounded_changed_files():
    instance, sign, frames = _mirror_push_runtime()
    commit = "a1" * 20
    changed_files = ["src/world.js", "docs/guide.md"]
    response = _run(instance._mirror_push(_Request(
        {"node": "mirror2", "repo": "forkmesh", "commit": commit,
         "changedFiles": changed_files},
        headers={
            "x-forkmesh-world-control": sign(
                None, "mirror2", "forkmesh", commit, changed_files),
        })))
    assert response["status"] == 200
    assert frames == [{
        "type": "mirror-push",
        "node": "mirror2",
        "repo": "forkmesh",
        "commit": commit[:12],
        "changedFiles": changed_files,
    }]


def test_unsigned_or_malformed_notices_never_reach_the_room():
    instance, sign, frames = _mirror_push_runtime()
    commit = "b2" * 20
    good = sign(None, "mirror2", "forkmesh", commit)
    rejected = [
        # A forged or replayed-for-other-values control signature.
        _Request({"node": "mirror2", "repo": "forkmesh", "commit": commit},
                 headers={"x-forkmesh-world-control": "f" * 64}),
        _Request({"node": "mirror3", "repo": "forkmesh", "commit": commit},
                 headers={"x-forkmesh-world-control": good}),
        # A non-hash commit cannot ride the frame even if signed for it.
        _Request({"node": "mirror2", "repo": "forkmesh",
                  "commit": "not-a-commit"},
                 headers={"x-forkmesh-world-control": sign(
                     None, "mirror2", "forkmesh", "not-a-commit")}),
        _Request({"node": "", "repo": "forkmesh", "commit": commit},
                 headers={"x-forkmesh-world-control": sign(
                     None, "", "forkmesh", commit)}),
    ]
    for request in rejected:
        assert _run(instance._mirror_push(request))["status"] == 401
    assert _run(instance._mirror_push(_Request(
        {}, method="GET")))["status"] == 405
    assert frames == []


def test_signed_status_generation_reaches_status_and_world_sockets_only():
    now = 1_786_555_200_000
    runtime = _load(
        "ForkMeshWorld", "_status_update_signature",
        extra_globals={
            "DurableObject": type("DurableObject", (), {}),
            "Date": SimpleNamespace(now=lambda: now),
            "_account_session_secret": lambda _env: b"unit-secret",
            "method_name": lambda request: request.method,
            "json_response": _response,
            "bounded_json_request": _bounded_json,
            "hmac": hmac,
        },
    )
    listener = object()
    peer = object()

    class Ctx:
        @staticmethod
        def getWebSockets(tag=None):
            return [listener] if tag == "status" else []

    instance = runtime["ForkMeshWorld"]()
    instance.ctx = Ctx()
    instance.env = None
    instance._live_sockets = lambda cleanup=False: [peer]
    frames = []
    instance._safe_send = lambda socket, frame: frames.append((socket, frame))
    signature = runtime["_status_update_signature"](None, now)

    response = _run(instance._status_push(_Request(
        {"generatedAt": now},
        headers={"x-forkmesh-status-control": signature},
    )))
    assert response["status"] == 200
    expected = {"type": "status-update", "generatedAt": now}
    assert frames == [(listener, expected), (peer, expected)]

    frames.clear()
    rejected = _run(instance._status_push(_Request(
        {"generatedAt": now},
        headers={"x-forkmesh-status-control": "0" * 64},
    )))
    assert rejected["status"] == 401
    assert frames == []


def test_signature_binds_node_repo_and_commit():
    _instance, sign, _frames = _mirror_push_runtime()
    base = sign(None, "mirror2", "forkmesh", "c" * 40)
    assert base == sign(None, "mirror2", "forkmesh", "c" * 40)
    assert base != sign(None, "mirror3", "forkmesh", "c" * 40)
    assert base != sign(None, "mirror2", "other", "c" * 40)
    assert base != sign(None, "mirror2", "forkmesh", "d" * 40)
    assert base != sign(
        None, "mirror2", "forkmesh", "c" * 40, ["src/world.js"])


def test_catalog_publish_announces_only_a_changed_public_head_after_purge():
    start = CATALOG_ROUTES_TEXT.index("async def catalog_handler")
    block = CATALOG_ROUTES_TEXT[start:]
    call = block.index("await _world_broadcast_mirror_push(")
    # Caches are purged before the announcement, so a viewer's immediate
    # refresh reads the new head rather than the stale cached payload.
    assert block.index("await purge_catalog_related_caches()") < call
    guard = block[:call]
    assert 'record["visibility"] == "public"' in guard
    assert "new_commit != prior_commit" in guard
    assert 'record.get("changedFiles", [])' in block[call:call + 300]
    assert 're.fullmatch(r"[0-9a-f]{40,64}", new_commit)' in guard
    # Best-effort: a World relay failure never fails the publication.
    assert "except Exception:" in block[call:call + 400]
    # The room route exists on the Durable Object alongside manual-block.
    assert '"/api/world/mirror-push"' in WORLD_DURABLE_OBJECTS_TEXT
    assert "await self._mirror_push(request)" in WORLD_DURABLE_OBJECTS_TEXT


def test_client_treats_the_frame_as_a_doorbell_and_coalesces_one_refresh():
    assert 'message.type === "mirror-push"' in APP
    assert "handleMirrorPush(message)" in APP
    handler = APP[APP.index("handleMirrorPush(message) {"):]
    handler = handler[:handler.index("\n  }")]
    # The announced name is sanitized before any display and the frame never
    # drives scene state directly — only a refresh of the signed payload.
    assert "sanitizePresenceText(message?.node" in handler
    assert "refreshMirrorCatalogs({ force: true })" in handler
    assert "clearTimeout(this.mirrorPushRefreshTimer)" in handler
    assert "spawnPushSurge" not in APP
    assert "armMirrorPushEffect?.(" in handler
    assert "changedFiles" in handler
    assert "!/^[0-9a-f]{12}$/.test(commit)" in handler
    assert "window.clearTimeout(this.mirrorPushRefreshTimer);" in APP


def test_verified_landing_notice_includes_publisher_and_commit_context():
    # The WebSocket packet remains a refresh doorbell. The chat-feed line is
    # written only after the matching signed mirror entry is in hand; it is
    # not a floating card (no toasts).
    handler = APP[APP.index("handleMirrorPush(message) {"):]
    handler = handler[:handler.index("\n  }", handler.index("handleMirrorPush(message) {"))]
    assert "this.announceMirrorPushLanding(landing)" in handler
    announce = APP[APP.index("async announceMirrorPushLanding(pending) {"):]
    announce = announce[:announce.index("\n  }", announce.index("async announceMirrorPushLanding(pending) {"))]
    assert "this.verifiedMirrorPushLanding(pending)" in announce
    assert "repository.lastCommitMessage" in announce
    assert "repository.lastCommitAuthorName" in announce
    assert "repository.changedFiles" in announce
    assert "node?.ownerUser" in announce
    assert "this.mirrorPushPublisherProfile(publisher)" in announce
    assert 'this.notifyChatArea([title, ...details].join(" · "), "success");' in announce
    assert "Published by" in announce
    assert "Git author" in announce
    assert "Changed" in announce
    assert "world-activity-details" not in APP


def test_scene_plays_a_bounded_disposed_surge_from_verified_commit_changes():
    # The surge fires from updateNetworkNodes when a cabinet's commit changes
    # between two signed payload snapshots — the same single trigger for the
    # instant socket-driven refresh and the regular fallback poll.
    update = SCENE[SCENE.index("function updateNetworkNodes"):]
    update = update[:update.index("function focusNetworkNode")]
    assert "cabinet?.userData?.nodeRecord?.commit" in update
    assert "nextCommit !== priorCommit" in update
    assert "spawnPushSurge(cabinet.position)" in update
    assert "verifiedPendingPush" in update
    assert "nextCommit.toLowerCase().startsWith(pendingPush.commit)" in update
    assert "function armMirrorPushEffect(" in SCENE
    assert "expiresAt: Date.now() + 120_000" in SCENE
    surge = SCENE[SCENE.index("function spawnPushSurge"):]
    surge = surge[:surge.index("function playRewardEvent")]
    # A burst of simultaneous publishes stays within a fixed effect budget.
    assert "pushSurges.length >= 8" in surge
    # The finished surge frees its geometry and materials like reward flights.
    animate = SCENE[SCENE.index("pushSurges.length - 1"):]
    animate = animate[:animate.index("updateCamera(delta)")]
    assert "pushSurges.splice(index, 1)" in animate
    assert "child.geometry?.dispose?.()" in animate
    assert "child.material?.dispose?.()" in animate


def test_verified_changed_files_collide_with_the_repository_ring():
    assert "function spawnRepositoryCodeLanding(pending = {})" in SCENE
    landing = SCENE[SCENE.index("function spawnRepositoryCodeLanding"):]
    landing = landing[:landing.index("function spawnServeFlights")]
    assert "repositoryCodeLandings.length >= 4" in landing
    assert "repository-changed-file:" in landing
    assert "repository-ring-collision:" in landing
    assert "repository-file-sizzle:" in landing
    assert "repository-night-lightning:" in landing
    assert "repository-file-shadow:" in landing
    assert "localDaylightMinute < 6 * 60" in landing
    assert "sizzleDuration: 10_000" in landing
    assert "portal.group.getWorldPosition(target)" in landing
    assert "repositoryChangedFileSegment(sizeMap.segments, path)" in landing
    assert "repository-changed-area:" in landing
    assert "repository-changed-file-leader:" in landing
    assert "ringFrame.localToWorld(" in landing

    update = SCENE[SCENE.index("function updateNetworkNodes"):]
    update = update[:update.index("function focusNetworkNode")]
    assert "spawnRepositoryCodeLanding(pendingPush)" in update
    assert "verifiedPendingPush" in update

    animate = SCENE[SCENE.index("repositoryCodeLandings.length - 1"):]
    animate = animate[:animate.index("for (let index = pushSurges.length - 1")]
    assert "lerpVectors(" in animate
    assert "sparkleAttribute.needsUpdate = true" in animate
    assert "effect.highlights.forEach((highlight)" in animate
    assert "state.impact," in animate
    assert "state.leader.quaternion.setFromUnitVectors(" in animate
    assert "landedFor >= effect.sizzleDuration" in animate
    assert "repositoryCodeLandings.splice(index, 1)" in animate
