#!/usr/bin/env python3
"""Truthful live-data and scene contracts for World mirror cabinets."""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
MODULE = WORLD / "world-mirror-nodes.js"


def test_live_node_builder_preserves_signed_repo_facts_and_unknown_telemetry():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const commit = "a".repeat(40);
      const network = {{
        stats: {{ onlineNodes: ["mirror2", "mirror3"] }},
        leaderboards: {{ nodes: [
          {{ name: "mirror2", repositoryCount: 4,
             sizeBytes: 12, pullCount: 9, cpuPercent: null }},
          {{ name: "mirror3", sizeBytes: 18, pullCount: 4 }}
        ] }}
      }};
      const mirrors = {{
        owner: "mirror2",
        repo: "forkmesh",
        requestedOwner: "forkmesh",
        requestedRepo: "forkmesh",
        mirrors: [
          {{
            node: "mirror2", machineName: "rack-a-berlin",
            status: "online", integrity: "ok",
            cloneAvailable: true, commit, branch: "main", sizeBytes: 44,
            issueCount: 7, commitCount: 80, branchCount: 3, pullCount: 5,
            discussionCount: 2, artifactCount: 1, platform: "linux",
            lastCommitMessage: "Show the last commit on every node",
            lastCommitAuthorName: "Ada Lovelace",
            lastCommitAt: 1750000000000,
            version: "0.7.0", cpuPercent: 32,
            ownerUser: "jett", endpoint: "https://mirror2.example",
            checkedAt: 1750000001000, latencyMs: 42, region: "ewr",
            endpointHealthy: true, endpointFresh: true,
            endpointIntegrity: "ok", operations: ["clone", "browse"],
            memUsedBytes: 25, memTotalBytes: 100,
            diskUsedBytes: -1, diskTotalBytes: -1
          }},
          {{
            node: "mirror3", status: "online", integrity: "ok",
            cloneAvailable: true, commit, branch: "main",
            issueCount: -1, pullCount: -1
          }}
        ]
      }};
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes(network, mirrors)));
    """
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            script,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    nodes = json.loads(result.stdout)
    assert [node["name"] for node in nodes] == ["mirror2", "mirror3"]
    mirror2 = nodes[0]
    # The machine's advertised node name rides along for display; the account
    # name stays the identity key. A node that never advertised one omits it.
    assert mirror2["machineName"] == "rack-a-berlin"
    assert "machineName" not in nodes[1]
    assert mirror2["commit"] == "a" * 40
    assert mirror2["branch"] == "main"
    # What the cabinet's LAST COMMIT panel names: the subject, the author and
    # when it landed, straight from the signed mirror record (adhoc #337). A
    # node that never published them keeps the honest "not reported" state.
    assert mirror2["lastCommitMessage"] == "Show the last commit on every node"
    assert mirror2["lastCommitAuthorName"] == "Ada Lovelace"
    assert mirror2["lastCommitAt"] == 1_750_000_000_000
    assert mirror2["pullCount"] == 5
    assert mirror2["sizeBytes"] == 44
    assert mirror2["repositoryCount"] == 4
    assert mirror2["version"] == "0.7.0"
    assert mirror2["cpuPercent"] == 32
    assert mirror2["ownerUser"] == "jett"
    assert mirror2["endpoint"] == "https://mirror2.example"
    assert mirror2["latencyMs"] == 42
    assert mirror2["region"] == "ewr"
    assert mirror2["endpointFresh"] is True
    assert mirror2["operations"] == ["clone", "browse"]
    assert mirror2["memoryUsedBytes"] == 25
    assert mirror2["memoryTotalBytes"] == 100
    assert "diskUsedBytes" not in mirror2
    assert "diskTotalBytes" not in mirror2
    assert mirror2["repositories"][0]["owner"] == "forkmesh"
    assert mirror2["repositories"][0]["name"] == "forkmesh"
    mirror3 = nodes[1]
    # A signed -1 means "not advertised"; a cached aggregate zero/positive must
    # not replace that explicit unknown state for this exact mirror record.
    assert "pullCount" not in mirror3
    assert "issueCount" not in mirror3
    assert "lastCommitMessage" not in mirror3
    assert "lastCommitAuthorName" not in mirror3
    assert "cpuPercent" not in mirror3
    assert "latencyMs" not in mirror3
    assert "memoryUsedBytes" not in mirror3
    assert "diskUsedBytes" not in mirror3


def test_live_node_builder_keeps_long_names_distinct_and_rejected_routes_blocked():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const prefix = "mirror-" + "x".repeat(50);
      const names = [prefix + "-one", prefix + "-two"];
      const payload = {{
        requestedOwner: "forkmesh", requestedRepo: "forkmesh",
        mirrors: names.map((node) => ({{
          node, status: "online", integrity: "rejected",
          cloneAvailable: true, commit: "b".repeat(40), branch: "main"
        }}))
      }};
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes({{
        stats: {{ onlineNodes: names }}
      }}, payload)));
    """
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            script,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    nodes = json.loads(result.stdout)
    assert len(nodes) == 2
    assert nodes[0]["name"] != nodes[1]["name"]
    assert all(node["healthy"] is False for node in nodes)
    assert all(node["cloneAvailable"] is False for node in nodes)


def test_live_node_builder_uses_the_freshest_repository_state_per_node():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const payloads = [
        {{
          requestedOwner: "forkmesh", requestedRepo: "alpha",
          mirrors: [{{
            node: "mirror2", status: "online", integrity: "ok",
            cloneAvailable: true, commit: "a".repeat(40), branch: "main",
            lastSync: 1000
          }}]
        }},
        {{
          requestedOwner: "forkmesh", requestedRepo: "zeta",
          mirrors: [{{
            node: "mirror2", status: "online", integrity: "ok",
            cloneAvailable: true, commit: "b".repeat(40), branch: "main",
            lastSync: 2000
          }}]
        }},
        {{
          requestedOwner: "forkmesh", requestedRepo: "fresh",
          mirrors: [{{
            node: "mirror3", status: "online", integrity: "ok",
            cloneAvailable: true, commit: "c".repeat(40), branch: "main",
            lastSync: 3000
          }}]
        }}
      ];
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes({{
        stats: {{ onlineNodes: ["mirror2", "mirror3"] }}
      }}, payloads)));
    """
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            script,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    nodes = json.loads(result.stdout)
    assert [node["name"] for node in nodes] == ["mirror3", "mirror2"]
    mirror2 = nodes[1]
    assert mirror2["commit"] == "b" * 40
    assert [repo["name"] for repo in mirror2["repositories"]] == ["zeta", "alpha"]


def test_live_node_builder_retains_offline_payload_rows_but_not_unrelated_names():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const network = {{
        stats: {{ onlineNodes: ["jett", "mirror2", "accidental-name"] }},
        leaderboards: {{ nodes: [
          {{ name: "jett", sizeBytes: 999 }},
          {{ name: "mirror2", sizeBytes: 22 }},
          {{ name: "accidental-name", sizeBytes: 11 }}
        ] }}
      }};
      const mirrors = {{
        requestedOwner: "forkmesh", requestedRepo: "forkmesh",
        mirrors: [
          {{
            node: "forkmesh", owner: "jett", machineName: "forkmesh",
            status: "online", integrity: "ok", cloneAvailable: true,
            commit: "a".repeat(40), branch: "main"
          }},
          {{
            node: "mirror2", ownerUser: "jett",
            status: "online", integrity: "ok", cloneAvailable: true,
            commit: "a".repeat(40), branch: "main"
          }},
          {{
            node: "retired-node", status: "offline", integrity: "unknown",
            cloneAvailable: false
          }}
        ]
      }};
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes(network, mirrors)));
    """
    result = subprocess.run(
        [
            "node",
            "--input-type=module",
            "-e",
            script,
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    nodes = json.loads(result.stdout)
    assert {node["name"] for node in nodes} == {
        "forkmesh",
        "mirror2",
        "retired-node",
    }
    assert all(node["name"] not in {"jett", "accidental-name"} for node in nodes)
    online = [node for node in nodes if node["online"]]
    assert {node["name"] for node in online} == {"forkmesh", "mirror2"}
    assert all(
        repo["status"] == "online"
        for node in online
        for repo in node["repositories"]
    )
    retired = next(node for node in nodes if node["name"] == "retired-node")
    assert retired["online"] is False
    assert retired["healthy"] is False
    assert retired["status"] == "offline"
    assert retired["integrity"] == "unknown"
    assert retired["cloneAvailable"] is False
    assert retired["repositories"][0]["status"] == "offline"


def test_recent_signed_endpoint_health_keeps_a_physical_cabinet_live_during_pin_convergence():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const checkedAt = Date.now() - 30_000;
      const payload = {{
        requestedOwner: "forkmesh", requestedRepo: "forkmesh",
        mirrors: [{{
          node: "mirror2", status: "offline", integrity: "rejected",
          cloneAvailable: false, endpointHealthy: true, endpointFresh: false,
          checkedAt, commit: "a".repeat(40), branch: "main"
        }}]
      }};
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes({{}}, payload)));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    node = json.loads(result.stdout)[0]
    # The machine is alive, so its cabinet remains present and green. The
    # repository route remains blocked and its integrity evidence is retained.
    assert node["name"] == "mirror2"
    assert node["online"] is True
    assert node["status"] == "online"
    assert node["cloneAvailable"] is False
    assert node["integrity"] == "rejected"
    assert node["repositories"][0]["status"] == "offline"
    assert node["repositories"][0]["endpointHealthy"] is True


def test_cabinet_carries_last_served_clone_and_web_stamps_with_client_class():
    script = f"""
      import {{ buildLiveMirrorNodes }} from {json.dumps(MODULE.as_uri())};
      const payload = {{
        requestedOwner: "forkmesh", requestedRepo: "forkmesh",
        mirrors: [
          {{
            node: "mirror2", status: "online", integrity: "ok",
            cloneAvailable: true, commit: "a".repeat(40), branch: "main",
            clonesServed: 12, websiteServed: 40,
            cloneServedAt: 1750000000000, cloneServedAgent: "git-client",
            websiteServedAt: 1750000900000, websiteServedAgent: "browser"
          }},
          {{
            node: "mirror3", status: "online", integrity: "ok",
            cloneAvailable: true, commit: "a".repeat(40), branch: "main",
            clonesServed: 3,
            cloneServedAt: 0,
            websiteServedAt: 1750000900000,
            websiteServedAgent: "git/2.43.0 (10.0.0.4)"
          }}
        ]
      }};
      process.stdout.write(JSON.stringify(buildLiveMirrorNodes({{}}, payload)));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    nodes = {node["name"]: node for node in json.loads(result.stdout)}
    served = nodes["mirror2"]
    assert served["cloneServedAt"] == 1_750_000_000_000
    assert served["cloneServedAgent"] == "git-client"
    assert served["websiteServedAt"] == 1_750_000_900_000
    assert served["websiteServedAgent"] == "browser"
    # A node that has served no clone yet keeps the honest unknown, and a raw
    # User-Agent smuggled into the record is never carried into the scene.
    partial = nodes["mirror3"]
    assert "cloneServedAt" not in partial
    assert "cloneServedAgent" not in partial
    assert partial["websiteServedAt"] == 1_750_000_900_000
    assert "websiteServedAgent" not in partial


def test_cabinet_serve_counters_are_stamped_with_when_and_who():
    # The two serve rows on the cabinet card carry a second line: how long ago
    # that kind of request was last answered, and the class of client answered.
    assert "function mirrorServeStamp" in SCENE
    assert "MIRROR_SERVE_AGENT_LABELS" in SCENE
    assert (
        "mirrorServeStamp(node?.cloneServedAt, node?.cloneServedAgent)" in SCENE
    )
    assert (
        "mirrorServeStamp(node?.websiteServedAt, node?.websiteServedAgent)"
        in SCENE
    )
    # Texture caching has to notice a new serve, or the card would freeze at
    # whatever stamp it was first drawn with.
    assert "cloneServedAt: node?.cloneServedAt" in SCENE
    assert "websiteServedAgent: node?.websiteServedAgent" in SCENE
    assert "<dt>Last clone served</dt>" in APP
    assert "<dt>Last web request served</dt>" in APP


def test_world_fetches_the_flagship_mirror_snapshot_once_and_uses_cabinets():
    # Bootstrap and the bounded visible-page fallback share one cached,
    # single-flight/backoff helper. Push signals remain the instant path.
    assert APP.count('this.fetchJSON("/api/repo/forkmesh/forkmesh/mirrors"') == 1
    assert "this.fetchMirrorCatalog({ force: forceMirrors })" in APP
    assert "this.fetchMirrorCatalog({ force })" in APP
    assert "const MIRROR_STATUS_POLL_MS = 5 * 60 * 1000" in APP
    assert "if (this.destroyed || document.hidden) return;" in APP
    assert "startMirrorPolling()" in APP
    assert "this.inflightRequests = new Map()" in APP
    assert "this.requestFailures = new Map()" in APP
    assert "staleIfError: true" in APP
    assert "buildLiveMirrorNodes" in APP
    assert "this.mirrorCatalogs" in APP
    assert "function createMirrorServerCabinet" in SCENE
    assert "serverPanelTexture" in SCENE
    assert "`mirror-server-cabinet:${id}`" in SCENE
    assert '"CPU"' in SCENE
    assert '"RAM"' in SCENE
    assert '"DISK"' in SCENE
    assert '"NOT SHARED"' in SCENE
    assert '"MESSAGE"' in SCENE
    assert '"AUTHOR"' in SCENE
    assert "mirrorCommitSnapshot" in SCENE
    assert "COMMIT SUBJECT NOT REPORTED" in SCENE
    assert "AUTHOR NOT REPORTED" in SCENE
    assert "mirrorCommitAgeLabel" in SCENE
    assert "`PING ${Math.round(pingLatency)} MS" in SCENE
    assert "node?.ownerUser" in SCENE
    assert "node?.nodeId" in SCENE
    # The cabinet title is the machine's advertised node name, falling back to
    # the account for publishers that predate the machineName catalog field.
    assert 'String(node?.machineName || node?.name || "MIRROR")' in SCENE
    assert "machineName: node?.machineName" in SCENE
    assert "nodeDataKey" in SCENE
    assert ".slice(0, 64)" in SCENE
    assert "mirror-server-front-panel" in SCENE
    assert "nodeCabinet" in SCENE
    assert "data-world-mirror-node" in APP
    assert "data-world-mirror-node-detail" in APP
    assert "operator-reported" in APP
    assert "createMirrorNodePylon" not in SCENE
    assert "<dt>Relay HTTPS ping</dt>" in APP
    assert "<dt>Supported operations</dt>" in APP
    assert "<dt>Latest commit message</dt>" in APP


def test_cabinet_face_matches_every_qt_column_and_adds_live_endpoint_context():
    qt_columns = (
        "NODE", "OWNER", "REPOS", "COMMIT", "MESSAGE", "AUTHOR",
        "SYNCED", "SIZE", "ISSUES", "COMMITS", "BRANCHES", "PULLS",
        "DISCUSSIONS", "HEALTH", "CPU", "RAM", "DISK", "PLATFORM",
        "VERSION", "NODE ID", "TUNNEL", "CLONES", "WEBSITE",
        "ARTIFACTS", "REACHABILITY",
    )
    for label in qt_columns:
        assert f'"{label}"' in SCENE, f"cabinet is missing Qt field: {label}"
    for extra in (
        "PRIMARY REPO", "WORKTREES", "ACTIVITY", "LAST SEEN",
        "HOSTED SINCE", "REGION", "HTTPS ENDPOINT", "OPERATIONS",
    ):
        assert f'"{extra}"' in SCENE, f"cabinet is missing extra field: {extra}"
    assert "repositoryCount: node?.repositoryCount" in SCENE
    assert "repositoryCount: aggregate.repositoryCount ?? repositories.length" in (
        MODULE.read_text(encoding="utf-8")
    )
    assert "<dt>Repositories</dt>" in APP
    assert "<dt>Health</dt>" in APP
    assert "<dt>Tunnel</dt>" in APP


def test_every_cabinet_screen_uses_crisp_text_sampling():
    # The technical face now matches the cabinet plane's portrait aspect and
    # starts with substantially more source pixels than the old 512-square
    # texture. Every text-bearing face also uses mipmaps and anisotropy.
    technical = SCENE.split("function serverPanelTexture", 1)[1].split(
        "const MIRROR_AGENT_TASK_STATUS", 1
    )[0]
    actions = SCENE.split("function mirrorActionsPanelTexture", 1)[1].split(
        "function normalizeMirrorAgentTask", 1
    )[0]
    agents = SCENE.split("function mirrorAgentTaskPanelTexture", 1)[1].split(
        "function createServedVisitorFigure", 1
    )[0]
    assert "canvasTexture(THREE, 768, 1024" in technical
    for painter in (technical, actions, agents):
        assert "sharpenCanvasTextTexture(THREE, texture)" in painter
    assert "texture.generateMipmaps = true" in SCENE
    assert "texture.anisotropy = 8" in SCENE


def test_only_nodes_active_within_a_day_get_a_cabinet():
    active = SCENE.split("function mirrorNodeActiveAt(node) {", 1)[1].split(
        "function mirrorMetric(", 1
    )[0]
    update = SCENE.split("function updateNetworkNodes(", 1)[1].split(
        "const fountain = landmarkObjects.get", 1
    )[0]
    assert "const NODE_ACTIVE_WINDOW_MS = 24 * 60 * 60 * 1000;" in SCENE
    # Liveness is read from observations of the machine itself…
    for stamp in (
        "node?.lastSeen",
        "node?.lastSync",
        "node?.updatedAt",
        "node?.activityUpdatedAt",
        "node?.cloneServedAt",
        "node?.websiteServedAt",
    ):
        assert stamp in active, f"activity stamp not considered: {stamp}"
    # …never from the mirrored repository's commit dates, and never from the
    # prober's checkedAt, which is refreshed even when the probe fails and
    # would therefore keep a dead mirror standing forever.
    assert "lastCommitAt" not in active
    assert "checkedAt" not in active
    # An online node always stands; an unstamped offline one never does.
    assert "if (mirrorNodeIsOnline(node)) return true;" in SCENE
    assert "return activeAt > 0 && now - activeAt <= NODE_ACTIVE_WINDOW_MS;" in SCENE
    # The yard payload is filtered before the 64-cabinet cut, so stale nodes
    # cannot crowd live ones out of the ring.
    assert ".filter((node) => mirrorNodeIsRecentlyActive(node, activeSince))" in update
    assert update.index(".filter((node) => mirrorNodeIsRecentlyActive") < update.index(
        ".slice(0, 64)"
    )


def test_status_beacons_are_open_topped_and_alert_colours_sweep():
    # The world camera looks down on the yard, so the lens top has to stay
    # uncapped for the status colour to be visible at all.
    assert "beaconCap" not in SCENE
    assert "beaconCollar" in SCENE
    # The physical liveness beacon is green whenever the node is reachable;
    # offline integrity/healing states keep their red/yellow attention sweep.
    assert 'online\n      ? "#00cc44"' in SCENE
    assert 'statusColor === "#ff0000" || statusColor === "#ffcc00"' in SCENE
    assert "group.userData.beaconSweep = beaconSweep" in SCENE
    assert "sweep.rotation.y = time * 0.0038" in SCENE


def test_cabinet_back_shows_authorized_actions_runs_and_bounded_log_tails():
    assert (
        '"/api/repo/forkmesh/forkmesh/actions/runs"' in APP
    )
    assert "const MIRROR_ACTIONS_POLL_MS = 2 * 60 * 1000" in APP
    assert "normalizeMirrorActionRuns" in APP
    assert "this.sessionAuthenticated" in APP
    assert "actionRunsAvailable: actionRunsByNode.has(name)" in APP
    assert "mirrorNodeActionsHTML(node)" in APP
    assert "No authorized, fresh Actions summary" in APP
    assert "bounded, already-redacted log tail" in APP
    assert "function mirrorActionsPanelTexture" in SCENE
    assert '"ACTIONS RUNS"' in SCENE
    assert '"SIGNED · REDACTED LOG TAILS · CLICK FOR ALL"' in SCENE
    assert "mirror-server-rear-panel" in SCENE
    assert "actionPulseAttention" in SCENE
    assert "Math.sin(time * 0.0065)" in SCENE
