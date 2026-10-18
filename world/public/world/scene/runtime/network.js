import { createInstancedMirrorSet } from "../../world-resource-cache.js";
import { hashNumber } from "../../world-avatar-face.js";
import { landmarkById } from "../../world-data.js";
import { createAgentRobot } from "../agent-bots.js";
import { createInstanceHex } from "../instance-hexes.js";
import {
  makeConsentedProfileFace,
  syncAvatarActivity,
  syncOperatorBelt,
  updateAvatarBadge,
} from "../avatars.js";
import { canvasTexture } from "../canvas.js";
import { makeLabelSprite, updatePlayerLabel } from "../labels.js";
import {
  CONTINUOUS_CITY_CENTER_Z,
  NODE_PLAZA_MAX_RADIUS,
  WORLD_BIKE_LANE_CENTER_X,
  worldMeshSlotOffset,
} from "../layout.js";
import {
  avatarHitProxyMaterial,
  makeMaterial,
  makeSharedMaterial,
} from "../materials.js";
import {
  createMirrorNodeMachines,
  createMirrorServerCabinet,
  mirrorNodeIsRecentlyActive,
  mirrorServedTotal,
  nodeDataKey,
  normalizeMirrorAgentTask,
} from "../mirror-nodes.js";
import {
  clamp,
  disposeObject3D,
  removeInteractiveObject,
  setShadows,
} from "../object-utils.js";
import {
  formatCapacityBytes,
  formatPlatformLimitBytes,
  systemCapacityMetricPairs,
} from "../office-infrastructure.js";
import { setWorldMeshFlowers, worldMeshOwnerAt } from "../movement.js";
import { safeRepositoryPath } from "../repositories.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  identity,
  onFediverseProfile,
  ctx,
  fns,
}) {
  // A mirror-push socket frame is only a doorbell. It may arm a short-lived
  // expectation, but the visual effect is released only after the next signed
  // catalog record confirms that exact node and commit prefix.
  const pendingMirrorPushEffects = new Map();
  let networkNodeSnapshot = [];
  const mirrorAgentTasksByNode = new Map();
  let mirrorAgentTasksKey = "";
  const MIRROR_TRAFFIC_WINDOW_MS = 10_000;
  const MIRROR_POLL_SPARK_WINDOW_MS = 60_000;
  const MIRROR_MACHINE_NEAR_DISTANCE = 190;
  const mirrorLiveTrafficAt = new Map();
  let nodeMachines = null;
  let nodeYardX = 0;
  let nodeYardZ = 0;

  function ensureNodeMachines() {
    if (nodeMachines || !ctx.nodeInterior) return nodeMachines;
    nodeMachines = createMirrorNodeMachines(THREE, ctx.nodeInterior);
    fns.registerWorldElement(
      "node-cabinets",
      "Mirror node cabinets",
      "Infrastructure",
      [nodeMachines.gears, nodeMachines.sparks],
    );
    return nodeMachines;
  }

  function mirrorCabinetNames(cabinet) {
    const record = cabinet?.userData?.nodeRecord || {};
    return [record.name, record.machineName, record.nodeId]
      .map((value) => String(value || "").trim().toLowerCase())
      .filter(Boolean);
  }

  function noteMirrorTraffic(entries) {
    if (!fns.worldElementEnabled("effects")) return 0;
    const counts = new Map();
    (Array.isArray(entries) ? entries : []).slice(0, 64).forEach((entry) => {
      const name = String(entry?.[0] || "").trim().toLowerCase();
      const count = Math.floor(Number(entry?.[1]) || 0);
      if (!/^[a-z0-9][a-z0-9.-]{0,79}$/.test(name) || count <= 0) return;
      counts.set(name, Math.min(10_000, (counts.get(name) || 0) + count));
    });
    if (!counts.size) return 0;
    const machines = ensureNodeMachines();
    if (!machines) return 0;
    let matched = 0;
    const now = Date.now();
    ctx.nodeInfrastructure.forEach((cabinet, id) => {
      const names = mirrorCabinetNames(cabinet);
      const count = names.reduce(
        (total, name) => total + (counts.get(name) || 0),
        0,
      );
      if (!count) return;
      matched += 1;
      mirrorLiveTrafficAt.set(id, now);
      machines.queueSparks(cabinet, count, MIRROR_TRAFFIC_WINDOW_MS);
    });
    return matched;
  }

  function updateNodeMachines(time, animate = true) {
    if (!nodeMachines) return;
    const near =
      Math.hypot(
        ctx.player.position.x - nodeYardX,
        ctx.player.position.z - nodeYardZ,
      ) <= MIRROR_MACHINE_NEAR_DISTANCE;
    nodeMachines.update(time, {
      animate: animate && near && fns.worldElementEnabled("node-cabinets"),
      sparksEnabled: fns.worldElementEnabled("effects"),
    });
  }


  function updateNetworkNodes(nodes = networkNodeSnapshot) {
    const activeSince = Date.now();
    networkNodeSnapshot = (Array.isArray(nodes) ? nodes : [])
      // The yard stands for the mesh as it is now. A node that has not been
      // seen, checked, synced, or served from within the last day is not part
      // of that picture, so its cabinet is dropped rather than left standing
      // as an offline monument crowding out the live ring.
      .filter((node) => mirrorNodeIsRecentlyActive(node, activeSince))
      .slice(0, 64)
      .map((node) => {
        const nodeName = String(node?.name || node?.label || "")
          .trim()
          .toLowerCase();
        return {
          ...node,
          agentTasks: (
            mirrorAgentTasksByNode.get(nodeName) || []
          ).map((task) => ({ ...task })),
        };
      });
    const fountain = ctx.landmarkObjects.get("fountain");
    const fountainPosition = landmarkById("fountain").position;
    const routingX = fountain?.position.x ?? fountainPosition[0];
    const routingZ = fountain?.position.z ?? fountainPosition[2];
    nodeYardX = routingX;
    nodeYardZ = routingZ;
    const seen = new Set();
    if (!ctx.nodeCabinetMirror) {
      ctx.nodeCabinetMirror = createInstancedMirrorSet(THREE, ctx.nodeInterior, {
        name: "mirror-server-cabinet-instances",
      });
      ctx.instancedMirrorSets.add(ctx.nodeCabinetMirror);
    }
    const cabinetMirror = ctx.nodeCabinetMirror;
    const removeCabinet = (cabinet) => {
      cabinet?.userData?.repeatedParts?.forEach((part) => cabinetMirror.remove(part));
      cabinet?.traverse?.((child) => {
        if (!child.userData?.nodeCabinet) return;
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
      });
      ctx.nodeInterior.remove(cabinet);
      disposeObject3D(cabinet);
    };
    // Mirror health and response times can reorder the live payload without a
    // membership change. A normalized name order keeps every surviving
    // cabinet in the same slot, while a changed member list is laid out again
    // immediately by this existing data-update path.
    const usableNodes = networkNodeSnapshot
      .map((node) => ({
        node,
        name: String(node?.name || node?.label || "").trim().slice(0, 80),
      }))
      .filter((entry) => entry.name)
      .filter(({ node, name }) =>
        [
          name,
          node?.machineName,
          node?.nodeId,
        ].every(
          (value) =>
            !ctx.deletedNodeIdentifiers.has(
              String(value || "").trim().toLowerCase(),
            ),
        ),
      )
      .sort((left, right) => {
        const leftKey = left.name.toLowerCase();
        const rightKey = right.name.toLowerCase();
        if (leftKey !== rightKey) return leftKey < rightKey ? -1 : 1;
        if (left.name === right.name) return 0;
        return left.name < right.name ? -1 : 1;
      })
      .filter(
        (entry, index, entries) =>
          index === 0 ||
          entry.name.toLowerCase() !== entries[index - 1].name.toLowerCase(),
      )
      .slice(0, 64);
    const circleSlots = fns.rewardCircleSlots(
      routingX,
      routingZ,
      usableNodes.length,
    );
    const nodeYardConcrete = fountain?.userData?.nodeYardConcrete;
    if (nodeYardConcrete) {
      const farthestRadius = circleSlots.reduce(
        (largest, slot) =>
          Math.max(largest, Math.hypot(slot.x - routingX, slot.z - routingZ)),
        0,
      );
      const plazaRadius = Math.min(
        NODE_PLAZA_MAX_RADIUS,
        Math.max(14, farthestRadius + 3.2),
      );
      nodeYardConcrete.scale.set(plazaRadius, 1, plazaRadius);
    }
    usableNodes.forEach(({ node, name: nodeName }, nodeIndex) => {
      const id = `node:${nodeName.toLowerCase()}`;
      const dataKey = nodeDataKey({ ...node, name: nodeName });
      seen.add(id);
      let cabinet = ctx.nodeInfrastructure.get(id);
      const followedPosition = cabinet?.position?.clone?.() || null;
      const focusFollowsCabinet = Boolean(
        ctx.cameraFocus &&
        followedPosition &&
        Math.abs(ctx.cameraFocus.x - followedPosition.x) < 0.01 &&
        Math.abs(ctx.cameraFocus.y - (followedPosition.y + 1.65)) < 0.01 &&
        Math.abs(ctx.cameraFocus.z - followedPosition.z) < 0.01
      );
      // The commit shown before this update, captured ahead of any rebuild.
      // Comparing it against the fresh signed record is what detects "code
      // was just pushed onto this node" — for both the instant socket-driven
      // refresh and the regular poll — without trusting any relay frame.
      const priorCommit = String(
        cabinet?.userData?.nodeRecord?.commit || "",
      );
      // Same idea for "this node just answered somebody": the served counters
      // shown before this update, captured ahead of any rebuild.
      const priorServed = mirrorServedTotal(cabinet?.userData?.nodeRecord);
      if (
        cabinet &&
        cabinet.userData.dataKey !== dataKey
      ) {
        removeCabinet(cabinet);
        ctx.nodeInfrastructure.delete(id);
        cabinet = null;
      }
      if (!cabinet) {
        cabinet = createMirrorServerCabinet(
          THREE,
          { ...node, name: nodeName },
          id,
        );
        ctx.nodeInterior.add(cabinet);
        cabinet.userData.repeatedParts?.forEach((part) => cabinetMirror.add(part));
        for (const panelName of [
          "mirror-server-front-panel",
          "mirror-server-rear-panel",
          "mirror-server-agent-panel-left",
          "mirror-server-agent-panel-right",
        ]) {
          const panel = cabinet.getObjectByName(panelName);
          if (panel) ctx.interactive.push(panel);
        }
        ctx.nodeInfrastructure.set(id, cabinet);
        const registeredCabinet = cabinet;
        fns.registerWorldElement(
          "node-cabinets",
          "Mirror node cabinets",
          "Infrastructure",
          cabinet,
          () => ctx.nodeInfrastructure.get(id) === registeredCabinet,
        );
      }
      const slot = circleSlots[nodeIndex];
      if (!slot) return;
      cabinet.position.set(slot.x, 0.38, slot.z);
      // The front display faces inward so each cabinet remains individually
      // readable from the surrounding walkway.
      cabinet.rotation.y = Math.atan2(
        routingX - slot.x,
        routingZ - slot.z,
      );
      if (focusFollowsCabinet) {
        ctx.cameraFocus.copy(cabinet.position);
        ctx.cameraFocus.y += 1.65;
      }
      const nextCommit = String(node?.commit || "");
      const pendingPush = pendingMirrorPushEffects.get(
        nodeName.toLowerCase(),
      );
      const verifiedPendingPush = Boolean(
        pendingPush &&
        pendingPush.expiresAt > Date.now() &&
        nextCommit.toLowerCase().startsWith(pendingPush.commit),
      );
      if (pendingPush && pendingPush.expiresAt <= Date.now()) {
        pendingMirrorPushEffects.delete(nodeName.toLowerCase());
      }
      if (
        verifiedPendingPush ||
        (priorCommit && nextCommit && nextCommit !== priorCommit)
      ) {
        if (verifiedPendingPush) {
          pendingMirrorPushEffects.delete(nodeName.toLowerCase());
          fns.spawnRepositoryCodeLanding(pendingPush);
        }
        fns.spawnPushSurge(cabinet.position);
      }
      const nextServed = mirrorServedTotal(node);
      if (
        priorServed !== null &&
        nextServed !== null &&
        nextServed > priorServed
      ) {
        fns.spawnServeFlights(cabinet.position, nextServed - priorServed);
        if (
          fns.worldElementEnabled("effects") &&
          Date.now() - (mirrorLiveTrafficAt.get(id) || 0) >
            MIRROR_POLL_SPARK_WINDOW_MS
        ) {
          ensureNodeMachines()?.queueSparks(
            cabinet,
            nextServed - priorServed,
            MIRROR_POLL_SPARK_WINDOW_MS,
          );
        }
      }
    });
    ctx.nodeInfrastructure.forEach((cabinet, id) => {
      if (seen.has(id)) return;
      removeCabinet(cabinet);
      ctx.nodeInfrastructure.delete(id);
      mirrorLiveTrafficAt.delete(id);
    });
    cabinetMirror.invalidate();
    ensureNodeMachines()?.layout(ctx.nodeInfrastructure);
    // The cabinets standing in the ring are exactly what the tally counts.
    fns.setRewardPoolMirrorCount(
      usableNodes.length,
      usableNodes.filter(
        ({ node }) =>
          node?.online === true ||
          String(node?.status || "").toLowerCase() === "online",
      ).length,
    );
  }

  function armMirrorPushEffect(
    nodeName,
    commitPrefix,
    repositoryOwner = "",
    repositoryName = "",
    changedFiles = [],
  ) {
    const node = String(nodeName || "").trim().toLowerCase();
    const commit = String(commitPrefix || "").trim().toLowerCase();
    if (
      !/^[a-z0-9][a-z0-9.-]{0,79}$/.test(node) ||
      !/^[0-9a-f]{12}$/.test(commit)
    ) {
      return false;
    }
    pendingMirrorPushEffects.set(node, {
      commit,
      owner: String(repositoryOwner || "").trim().toLowerCase().slice(0, 80),
      repo: String(repositoryName || "").trim().toLowerCase().slice(0, 120),
      changedFiles: (Array.isArray(changedFiles) ? changedFiles : [])
        .map((path) => safeRepositoryPath(path).slice(0, 160))
        .filter(
          (path, index, paths) =>
            path && paths.indexOf(path) === index,
        )
        .slice(0, 8),
      // The push frame arrives immediately after the relay accepts the signed
      // publication, but edge caches and a busy mirror can take longer than
      // the old 15-second window to expose the matching catalog snapshot.
      // Keep the doorbell armed through two normal catalog polls; the effect
      // still cannot fire unless that signed record confirms this exact node
      // and commit prefix.
      expiresAt: Date.now() + 120_000,
    });
    return true;
  }

  function relayoutNetworkNodes() {
    if (!networkNodeSnapshot.length && !ctx.nodeInfrastructure.size) return;
    updateNetworkNodes(networkNodeSnapshot);
  }

  function updateMirrorAgentTasks(sessions = []) {
    // The decrypted, engineering-authorized session projection is also the
    // roster for agent avatars: exactly one lightweight Claude/Codex robot per
    // session, reconciled away immediately when access or the session vanishes.
    fns.reconcileSessionAgentBots(sessions);
    const next = new Map();
    const nextByRepository = new Map();
    for (const session of Array.isArray(sessions) ? sessions.slice(0, 80) : []) {
      const node = String(session?.targetNode || "").trim().toLowerCase();
      const task = normalizeMirrorAgentTask(session);
      if (!node || !task) continue;
      const tasks = next.get(node) || [];
      if (tasks.length < 8) tasks.push({ ...session, ...task });
      next.set(node, tasks);
      if (
        task.status === "running" &&
        /^[a-z0-9._-]{1,64}\/[a-z0-9._-]{1,100}$/.test(task.repositoryKey)
      ) {
        const repositoryTasks =
          nextByRepository.get(task.repositoryKey) || [];
        if (repositoryTasks.length < 6) {
          repositoryTasks.push({ ...session, ...task });
        }
        nextByRepository.set(task.repositoryKey, repositoryTasks);
      }
    }
    const key = JSON.stringify(
      [...next.entries()]
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([node, tasks]) => [
          node,
          tasks.map((task) => [
            task.id,
            task.title,
            task.status,
            task.repositoryKey,
            task.updatedAt,
          ]),
        ]),
    );
    if (key === mirrorAgentTasksKey) return false;
    mirrorAgentTasksKey = key;
    mirrorAgentTasksByNode.clear();
    next.forEach((tasks, node) => mirrorAgentTasksByNode.set(node, tasks));
    ctx.repositoryAgentTasksByRepository.clear();
    nextByRepository.forEach((tasks, repositoryKey) =>
      ctx.repositoryAgentTasksByRepository.set(repositoryKey, tasks),
    );
    updateNetworkNodes(networkNodeSnapshot);
    const deskData = ctx.world.userData.repositoryRecordDeskData;
    if (deskData) {
      fns.updateRepositoryRecordDesk(deskData.selection, deskData.records);
    }
    const activityData = ctx.world.userData.repositoryActivityData;
    if (activityData) {
      fns.updateRepositoryActivity(activityData.activity, activityData.selection);
    }
    return true;
  }

  function focusNetworkNode(name) {
    const targetName = String(name || "").trim().toLowerCase();
    let target = null;
    ctx.nodeInfrastructure.forEach((cabinet) => {
      if (
        !target &&
        String(cabinet.userData?.nodeRecord?.name || "")
          .trim()
          .toLowerCase() === targetName
      ) {
        target = cabinet;
      }
    });
    if (!target) return false;
    ctx.cameraFocus = target.position.clone();
    ctx.cameraFocus.y += 1.65;
    return true;
  }

  function updateFederatedInstances(instances = []) {
    const district = ctx.landmarkObjects.get("fediverse");
    if (!district) return;
    const existing = district.userData.federatedInstanceLayer;
    if (existing) {
      district.remove(existing);
      existing.traverse((child) => {
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
    }
    const layer = new THREE.Group();
    layer.name = "approved-federated-instances";
    (Array.isArray(instances) ? instances : [])
      .filter((instance) => instance?.approved === true)
      .slice(0, 18)
      .forEach((instance, index, approved) => {
        const angle = (index / Math.max(1, approved.length)) * Math.PI * 2;
        const online = instance?.online === true;
        const tower = new THREE.Mesh(
          resources.primitive(THREE, "CylinderGeometry", 0.22, 0.34, online ? 1.2 : 0.72, 8),
          makeMaterial(THREE, online ? "#80e8ff" : "#718087", {
            emissive: online ? "#267e9a" : "#313a3d",
            emissiveIntensity: online ? 0.72 : 0.22,
          }),
        );
        tower.position.set(
          Math.cos(angle) * 4.3,
          online ? 0.75 : 0.5,
          Math.sin(angle) * 4.3,
        );
        tower.userData.interactive = "federated-world-portal";
        tower.userData.origin = String(instance?.origin || "");
        tower.userData.label = String(
          instance?.label || "ForkMesh instance",
        ).slice(0, 80);
        layer.add(tower);
        ctx.interactive.push(tower);
        const label = makeLabelSprite(
          THREE,
          String(instance?.label || "ForkMesh instance").slice(0, 22),
          online ? "verified online" : "approved · health unavailable",
          online ? "#80e8ff" : "#9aa6aa",
        );
        label.scale.set(1.6, 0.54, 1);
        label.position.copy(tower.position).add(
          new THREE.Vector3(0, online ? 1.05 : 0.78, 0),
        );
        label.userData.interactive = "federated-world-portal";
        label.userData.origin = String(instance?.origin || "");
        label.userData.label = String(
          instance?.label || "ForkMesh instance",
        ).slice(0, 80);
        layer.add(label);
        ctx.interactive.push(label);
      });
    district.add(layer);
    district.userData.federatedInstanceLayer = layer;
  }

  function updateInstanceHexes(worlds = [], selfSlot = -1) {
    const self = worldMeshSlotOffset(selfSlot);
    const placed = [];
    (self && Array.isArray(worlds) ? worlds : []).forEach((world) => {
      const offset = worldMeshSlotOffset(world?.slot);
      if (!offset || !world?.id) return;
      const dx = offset.x - self.x;
      const dz = offset.z - self.z;
      if (Math.hypot(dx, dz) < 1) return;
      placed.push({
        ...world,
        dx,
        dz,
        x: WORLD_BIKE_LANE_CENTER_X + dx,
        z: CONTINUOUS_CITY_CENTER_Z + dz,
      });
    });
    const key = JSON.stringify(placed);
    const existing = ctx.world.userData.instanceHexLayer;
    if (existing && ctx.world.userData.instanceHexKey === key) return placed;
    if (existing) {
      ctx.world.remove(existing);
      removeInteractiveObject(ctx.interactive, existing);
      disposeObject3D(existing);
    }
    const layer = new THREE.Group();
    layer.name = "forkmesh-instance-hexes";
    const home = { x: WORLD_BIKE_LANE_CENTER_X, z: CONTINUOUS_CITY_CENTER_Z };
    placed.forEach((world) => {
      const hex = createInstanceHex(THREE, world, ctx.interactive, home);
      if (hex) layer.add(hex);
    });
    setWorldMeshFlowers(placed);
    ctx.world.add(layer);
    ctx.world.userData.instanceHexLayer = layer;
    ctx.world.userData.instanceHexKey = key;
    ctx.groundSurfaces = null;
    return placed;
  }

  function worldMeshOwner(x, z) {
    const flower = worldMeshOwnerAt(x, z);
    return flower ? { id: flower.id, dx: flower.dx, dz: flower.dz } : null;
  }

  function updateBots(bots = []) {
    const seen = new Set();
    const namedBots = (Array.isArray(bots) ? bots : [])
      .filter(
        (bot) =>
          String(bot?.id || "").trim() &&
          String(bot?.name || "").trim() &&
          String(bot?.type || "").trim(),
      )
      .slice(0, 12);
    namedBots.forEach((bot, index) => {
      const id = `bot:${String(bot.id).trim().slice(0, 48)}`;
      seen.add(id);
      let robot = ctx.botAgents.get(id);
      if (!robot) {
        robot = createAgentRobot(
          THREE,
          {
            ...bot,
            name: String(bot.name).trim().slice(0, 32),
            type: String(bot.type).trim().slice(0, 20),
          },
          id,
        );
        robot.userData.hoverPhase = hashNumber(id) * 0.0001;
        ctx.world.add(robot);
        ctx.botAgents.set(id, robot);
        const registeredRobot = robot;
        fns.registerWorldElement(
          "directory-bots",
          "Verified bot avatars",
          "Avatars & bots",
          robot,
          () => ctx.botAgents.get(id) === registeredRobot,
        );
      }
      const center = landmarkById(
        index % 2 ? "fediverse" : "security",
      ).position;
      const angle = (index / Math.max(1, namedBots.length)) * Math.PI * 2;
      robot.position.set(
          center[0] + Math.cos(angle) * 5.2,
          0.38,
          center[2] + Math.sin(angle) * 5.2,
      );
      robot.rotation.y = -angle;
    });
    ctx.botAgents.forEach((robot, id) => {
      if (seen.has(id)) return;
      ctx.world.remove(robot);
      disposeObject3D(robot);
      ctx.botAgents.delete(id);
    });
  }

  function updateSystemCapacity(metrics = {}) {
    const records = Array.isArray(metrics)
      ? metrics
      : Array.isArray(metrics?.objects)
        ? metrics.objects
        : [];
    const safeRecords = records
      .map((record) => {
        const name = String(record?.name || record?.id || "").trim();
        const pairs = systemCapacityMetricPairs(record);
        // A discovered Durable Object with no client-observable limit still
        // earns a plinth: its relayed byte total is the whole point, and a
        // binding that has relayed nothing yet is itself worth showing.
        const bytesTotal = Number(record?.bytesTotal);
        const bytes =
          Number.isSafeInteger(bytesTotal) && bytesTotal > 0 ? bytesTotal : 0;
        return name
          ? {
              name: name.slice(0, 36),
              id: String(record?.id || name).slice(0, 72),
              pairs,
              bytes,
            }
          : null;
      })
      .filter(Boolean)
      .slice(0, 32);
    const tables = Array.isArray(metrics?.tables) ? metrics.tables : [];
    const database =
      metrics?.database && typeof metrics.database === "object"
        ? metrics.database
        : {};
    const databaseBytes = Math.max(0, Number(database.bytes) || 0);
    const freeDatabaseLimitBytes = Math.max(
      0,
      Number(database.freeDatabaseLimitBytes) || 0,
    );
    const paidDatabaseLimitBytes = Math.max(
      0,
      Number(database.paidDatabaseLimitBytes) || 0,
    );
    const safeTables = [
      ...new Map(
        tables
          .map((table) => {
            const name = String(table?.name || "").trim();
            const rowCount = Number(table?.rowCount);
            // A table that holds nothing yet still gets a bar; it is part of
            // the platform's shape and hiding it shrank the inventory.
            return /^[A-Za-z_][A-Za-z0-9_]{0,62}$/.test(name) &&
              Number.isSafeInteger(rowCount) &&
              rowCount >= 0
              ? [name, { name, rowCount }]
              : null;
          })
          .filter(Boolean),
      ).values(),
    ]
      .sort(
        (left, right) =>
          right.rowCount - left.rowCount ||
          left.name.localeCompare(right.name),
      )
      .slice(0, 256);
    const signature = JSON.stringify({
      objects: safeRecords,
      tables: safeTables,
      database: {
        bytes: databaseBytes,
        freeDatabaseLimitBytes,
        paidDatabaseLimitBytes,
      },
    });
    if (
      ctx.systemCapacityPlatform.userData.metricsSignature === signature
    ) {
      return safeRecords.length + safeTables.length;
    }
    ctx.systemCapacityPlatform.userData.metricsSignature = signature;

    const existing = ctx.systemCapacityPlatform.userData.metricsLayer;
    if (existing) {
      // Table bars are pickable, so the old ones have to leave the interactive
      // list before they are disposed or clicks would raycast dead meshes.
      removeInteractiveObject(ctx.interactive, existing);
      ctx.systemCapacityPlatform.remove(existing);
      existing.traverse((child) => {
        if (child.isInstancedMesh) child.dispose();
      });
      disposeObject3D(existing);
      ctx.systemCapacityPlatform.userData.metricsLayer = null;
    }

    const emptyMarker = ctx.systemCapacityPlatform.getObjectByName(
      "system-capacity-metrics-unavailable",
    );
    const hasMetrics = safeRecords.length > 0 || safeTables.length > 0;
    if (emptyMarker) emptyMarker.visible = !hasMetrics;
    ctx.systemCapacityPlatform.userData.metricsAvailable =
      hasMetrics;
    ctx.systemCapacityPlatform.userData.visibleObjectCount = safeRecords.length;
    ctx.systemCapacityPlatform.userData.visibleTableCount = safeTables.length;
    if (!hasMetrics) return 0;

    const layer = new THREE.Group();
    layer.name = "system-capacity-current-usage";
    if (safeTables.length) {
      const tableLayer = new THREE.Group();
      tableLayer.name = "system-capacity-database-tables";
      tableLayer.userData.tableCount = safeTables.length;
      const maxRows = Math.max(
        ...safeTables.map((table) => table.rowCount),
      );
      const usableWidth = 11.5;
      const usableDepth = 6.8;
      const columns = Math.max(
        1,
        Math.ceil(Math.sqrt(safeTables.length * (usableWidth / usableDepth))),
      );
      const rows = Math.ceil(safeTables.length / columns);
      const cellWidth = usableWidth / columns;
      const cellDepth = usableDepth / rows;
      const barWidth = clamp(
        Math.min(cellWidth, cellDepth) * 0.54,
        0.075,
        0.52,
      );
      const barTiers = [
        ["#587d75", "#203f38"],
        ["#80e8ff", "#267e9a"],
        ["#9ef7c6", "#2f8c5f"],
      ];
      const barTierOf = (normalized) =>
        normalized > 0.72 ? 2 : normalized > 0.38 ? 1 : 0;
      const barGeometry = resources.primitive(THREE, "BoxGeometry", 1, 1, 1);
      const bars = safeTables.map((table, index) => {
        const column = index % columns;
        const row = Math.floor(index / columns);
        // An all-empty inventory has no scale to draw against; every bar then
        // sits at its minimum height instead of collapsing to NaN geometry.
        const normalized = maxRows
          ? Math.log1p(table.rowCount) / Math.log1p(maxRows)
          : 0;
        const height = 0.18 + normalized * 3.15;
        const bar = new THREE.Mesh(barGeometry, avatarHitProxyMaterial(THREE));
        bar.name = `system-capacity-table:${table.name}`;
        bar.position.set(
          -usableWidth / 2 + cellWidth * (column + 0.5),
          0.38 + height / 2,
          -3.45 + cellDepth * (row + 0.5),
        );
        bar.scale.set(barWidth, height, barWidth);
        bar.updateMatrix();
        bar.visible = false;
        bar.userData.raycastProxy = true;
        bar.userData.tableName = table.name;
        bar.userData.rowCount = table.rowCount;
        bar.userData.barTier = barTierOf(normalized);
        // Clicking a bar opens the floating table browser for that table.
        bar.userData.interactive = "system-capacity-table";
        ctx.interactive.push(bar);
        tableLayer.add(bar);
        return bar;
      });
      barTiers.forEach(([color, emissive], tier) => {
        const tierBars = bars.filter((bar) => bar.userData.barTier === tier);
        if (!tierBars.length) return;
        const batch = new THREE.InstancedMesh(
          barGeometry,
          makeMaterial(THREE, color, {
            emissive,
            emissiveIntensity: 0.72,
            metalness: 0.24,
            roughness: 0.42,
          }),
          tierBars.length,
        );
        batch.name = `system-capacity-table-bars-${tier}`;
        batch.raycast = () => {};
        tierBars.forEach((bar, index) => batch.setMatrixAt(index, bar.matrix));
        batch.instanceMatrix.needsUpdate = true;
        batch.computeBoundingSphere();
        tableLayer.add(batch);
      });
      // The complete table identity is printed on its top face: a large row
      // count fills the width, with the table name immediately beneath it.
      const labelGrid = Math.ceil(Math.sqrt(bars.length));
      const labelCell = Math.min(512, Math.floor(2048 / labelGrid));
      const labelAtlasSize = labelGrid * labelCell;
      const labelScale = labelCell / 768;
      const labelAtlas = canvasTexture(
        THREE,
        labelAtlasSize,
        labelAtlasSize,
        (context) => {
          context.clearRect(0, 0, labelAtlasSize, labelAtlasSize);
          context.fillStyle = "#071c16";
          context.textAlign = "center";
          context.textBaseline = "middle";
          bars.forEach((bar, index) => {
            const left = (index % labelGrid) * labelCell;
            const top = Math.floor(index / labelGrid) * labelCell;
            const rowCount = bar.userData.rowCount.toLocaleString("en-US");
            context.save();
            context.beginPath();
            context.rect(left, top, labelCell, labelCell);
            context.clip();
            context.font = `800 ${(rowCount.length > 5 ? 146 : 184) * labelScale}px "ForkMesh Mono", ui-monospace, monospace`;
            context.fillText(
              rowCount,
              left + 384 * labelScale,
              top + 278 * labelScale,
              710 * labelScale,
            );
            context.font = `700 ${72 * labelScale}px "ForkMesh Mono", ui-monospace, monospace`;
            context.fillText(
              bar.userData.tableName,
              left + 384 * labelScale,
              top + 508 * labelScale,
              710 * labelScale,
            );
            context.restore();
          });
        },
        1,
        1,
      );
      const labelHalf = (barWidth * 0.96) / 2;
      const labelInset = 1 / labelAtlasSize;
      const labelPositions = new Float32Array(bars.length * 12);
      const labelNormals = new Float32Array(bars.length * 12);
      const labelUvs = new Float32Array(bars.length * 8);
      const labelIndices = new Uint32Array(bars.length * 6);
      bars.forEach((bar, index) => {
        const x = bar.position.x;
        const y = 0.38 + bar.scale.y + 0.014;
        const z = bar.position.z;
        labelPositions.set(
          [
            x - labelHalf, y, z - labelHalf,
            x + labelHalf, y, z - labelHalf,
            x - labelHalf, y, z + labelHalf,
            x + labelHalf, y, z + labelHalf,
          ],
          index * 12,
        );
        labelNormals.set([0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0], index * 12);
        const u0 = ((index % labelGrid) * labelCell) / labelAtlasSize + labelInset;
        const u1 = ((index % labelGrid) + 1) * labelCell / labelAtlasSize - labelInset;
        const v1 = 1 - (Math.floor(index / labelGrid) * labelCell) / labelAtlasSize - labelInset;
        const v0 = 1 - ((Math.floor(index / labelGrid) + 1) * labelCell) / labelAtlasSize + labelInset;
        labelUvs.set([u0, v1, u1, v1, u0, v0, u1, v0], index * 8);
        const base = index * 4;
        labelIndices.set(
          [base, base + 2, base + 1, base + 2, base + 3, base + 1],
          index * 6,
        );
      });
      const labelGeometry = new THREE.BufferGeometry();
      labelGeometry.setAttribute(
        "position",
        new THREE.BufferAttribute(labelPositions, 3),
      );
      labelGeometry.setAttribute(
        "normal",
        new THREE.BufferAttribute(labelNormals, 3),
      );
      labelGeometry.setAttribute("uv", new THREE.BufferAttribute(labelUvs, 2));
      labelGeometry.setIndex(new THREE.BufferAttribute(labelIndices, 1));
      labelGeometry.computeBoundingSphere();
      const topLabels = new THREE.Mesh(
        labelGeometry,
        resources.uniqueMaterial(THREE, {
          map: labelAtlas,
          transparent: true,
          toneMapped: false,
          side: THREE.DoubleSide,
          forceSinglePass: true,
          depthWrite: false,
        }, "MeshBasicMaterial"),
      );
      topLabels.name = "system-capacity-row-counts";
      tableLayer.add(topLabels);
      if (databaseBytes || freeDatabaseLimitBytes || paidDatabaseLimitBytes) {
        const freeRatio = freeDatabaseLimitBytes
          ? (databaseBytes / freeDatabaseLimitBytes) * 100
          : 0;
        const paidRatio = paidDatabaseLimitBytes
          ? (databaseBytes / paidDatabaseLimitBytes) * 100
          : 0;
        const storageLabel = makeLabelSprite(
          THREE,
          `D1 STORAGE ${formatCapacityBytes(databaseBytes)}`,
          `${freeRatio.toFixed(2)}% OF ${formatPlatformLimitBytes(
            freeDatabaseLimitBytes,
          )} FREE · ${paidRatio.toFixed(3)}% OF ${formatPlatformLimitBytes(
            paidDatabaseLimitBytes,
          )} PAID`,
          freeRatio >= 85 ? "#ff9ca4" : "#9ef7c6",
        );
        storageLabel.name = "system-capacity-d1-storage";
        storageLabel.scale.set(6.9, 1.15, 1);
        storageLabel.position.set(0, 4.65, -3.15);
        tableLayer.add(storageLabel);
      }
      layer.add(tableLayer);
    }

    // Relayed bytes have no configured ceiling, so the busiest Durable Object
    // sets the scale the others are drawn against.
    const peakBytes = safeRecords.reduce(
      (peak, record) => Math.max(peak, record.bytes),
      0,
    );
    safeRecords.forEach((record, index) => {
      const object = new THREE.Group();
      object.name = `system-capacity-service:${record.id}`;
      object.userData.metrics = record.pairs.map((metric) => ({
        key: metric.key,
        currentUsage: metric.usage,
        configuredLimit: metric.limit,
      }));
      object.userData.bytesRelayed = record.bytes;

      // Plinths share the platform with the table bars, so a large binding
      // list tightens its spacing rather than marching off the edge.
      const spacing = Math.min(
        safeRecords.length > 5 ? 1.65 : 2.5,
        11.5 / Math.max(1, safeRecords.length - 1),
      );
      object.position.set(
        (index - (safeRecords.length - 1) / 2) * spacing,
        0.38,
        safeTables.length ? 3.75 : 0,
      );
      const plinth = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.52, 0.64, 0.2, 8),
        makeSharedMaterial(THREE, "#173840", {
          emissive: "#1d5665",
          emissiveIntensity: 0.38,
          metalness: 0.28,
        }),
      );
      plinth.position.y = 0.13;
      object.add(plinth);

      const firstMetric = record.pairs[0];
      // Without a limit to fill against, the column shows this object's share
      // of the busiest object's traffic (log-scaled, like the table bars).
      const rawRatio = firstMetric
        ? firstMetric.usage / firstMetric.limit
        : peakBytes
          ? Math.log1p(record.bytes) / Math.log1p(peakBytes)
          : 0;
      const ratio = clamp(rawRatio, 0, 1);
      const overLimit = rawRatio > 1;
      const housing = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.62, 1.35, 0.62),
        makeSharedMaterial(THREE, "#1d343b", {
          transparent: true,
          opacity: 0.62,
          roughness: 0.46,
        }),
      );
      housing.position.y = 0.9;
      object.add(housing);
      const fillHeight = Math.max(0.08, ratio * 1.28);
      const fill = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.44, 1, 0.44),
        makeSharedMaterial(THREE, overLimit ? "#ff6d78" : "#80e8ff", {
          emissive: overLimit ? "#9f2632" : "#267e9a",
          emissiveIntensity: 0.92,
          metalness: 0.22,
        }),
      );
      fill.scale.y = fillHeight;
      fill.position.y = 0.25 + fillHeight / 2;
      fill.userData.currentUsage = firstMetric
        ? firstMetric.usage
        : record.bytes;
      fill.userData.configuredLimit = firstMetric ? firstMetric.limit : 0;
      object.add(fill);

      const exactValues = [
        ...record.pairs
          .slice(0, 2)
          .map((metric) => `${metric.key} ${metric.usage}/${metric.limit}`),
        ...(record.bytes || !record.pairs.length
          ? [`${formatCapacityBytes(record.bytes)} relayed`]
          : []),
      ].join(" · ");
      const label = makeLabelSprite(
        THREE,
        record.name,
        exactValues,
        overLimit ? "#ff9ca4" : "#80e8ff",
      );
      label.scale.set(2.15, 0.72, 1);
      label.position.y = 2.05;
      object.add(label);
      setShadows(object);
      layer.add(object);
    });
    ctx.systemCapacityPlatform.add(layer);
    ctx.systemCapacityPlatform.userData.metricsLayer = layer;
    return safeRecords.length + safeTables.length;
  }

  function updateOrganizations(organizations = []) {
    const district = ctx.landmarkObjects.get("organizations");
    if (!district) return;
    const existing = district.userData.organizationProfiles;
    if (existing) {
      district.remove(existing);
      existing.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
    }
    const profiles = new THREE.Group();
    profiles.name = "organization-profiles";
    organizations.slice(0, 8).forEach((organization, index) => {
      const name = String(
        organization.displayName || organization.name || organization.org || "Organization",
      ).slice(0, 28);
      const repoCount = Array.isArray(organization.repos)
        ? organization.repos.length
        : Number(organization.repos || 0);
      const column = index % 3;
      const row = Math.floor(index / 3);
      const sign = makeLabelSprite(
        THREE,
        name.toUpperCase(),
        `${repoCount} repository ${repoCount === 1 ? "floor" : "floors"}`,
        "#d5b6ff",
      );
      sign.scale.set(2.8, 0.92, 1);
      sign.position.set((column - 1) * 2.75, 4.6 + row * 1.0, -1.62);
      profiles.add(sign);
      const repos = Array.isArray(organization.repos)
        ? organization.repos.slice(0, 8)
        : [];
      repos.forEach((repo, repoIndex) => {
        const flower = new THREE.Mesh(
          resources.primitive(THREE, "IcosahedronGeometry", 0.17 + (repoIndex % 3) * 0.035, 1),
          makeMaterial(THREE, ["#d5b6ff", "#9ef7c6", "#77d9ff"][repoIndex % 3], {
            emissive: "#60458b",
            emissiveIntensity: 0.4,
          }),
        );
        flower.position.set(
          (column - 1) * 2.75 - 0.7 + (repoIndex % 4) * 0.45,
          0.55,
          2.1 + row * 0.7,
        );
        profiles.add(flower);
      });
      const offices = Array.isArray(organization.memberList)
        ? organization.memberList.slice(0, 6)
        : [];
      offices.forEach((member, officeIndex) => {
        const office = new THREE.Mesh(
          resources.primitive(THREE, "BoxGeometry", 0.38, 0.42, 0.32),
          makeSharedMaterial(THREE, "#f2d4ff", {
            emissive: "#60458b",
            emissiveIntensity: 0.25,
          }),
        );
        office.position.set(
          (column - 1) * 2.75 - 0.75 + (officeIndex % 3) * 0.75,
          1.1 + Math.floor(officeIndex / 3) * 0.58,
          -1.35,
        );
        office.userData.officeMember = String(member?.name || "").slice(0, 50);
        profiles.add(office);
      });
    });
    district.add(profiles);
    district.userData.organizationProfiles = profiles;
  }

  function updateFediverseDirectory(directory = {}) {
    const center = ctx.landmarkObjects.get("fediverse");
    if (!center) return;
    const existing = center.userData.instanceProfiles;
    if (existing) {
      center.remove(existing);
      existing.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
    }
    const layer = new THREE.Group();
    layer.name = "consented-fediverse-profiles";
    const instances = [
      ...(Array.isArray(directory.mastodon)
        ? directory.mastodon.map((instance) => ({
            ...instance,
            network: "Mastodon",
          }))
        : []),
      ...(Array.isArray(directory.lemmy)
        ? directory.lemmy.map((instance) => ({
            ...instance,
            network: "Lemmy",
          }))
        : []),
      ...(Array.isArray(directory.x)
        ? directory.x.map((instance) => ({
            ...instance,
            network: "X",
          }))
        : []),
      ...(Array.isArray(directory.reddit)
        ? directory.reddit.map((instance) => ({
            ...instance,
            network: "Reddit",
          }))
        : []),
    ].slice(0, 16);
    instances.forEach((instance, instanceIndex) => {
      const angle = (instanceIndex / Math.max(1, instances.length)) * Math.PI * 2;
      const instanceNode = new THREE.Mesh(
        resources.primitive(THREE, "IcosahedronGeometry", 0.3, 1),
        makeMaterial(THREE, {
          Mastodon: "#8c8dff",
          Lemmy: "#8fcf85",
          X: "#d8e3ea",
          Reddit: "#ff8a69",
        }[instance.network] || "#b6d8ff", {
          emissive: {
            Mastodon: "#5355bd",
            Lemmy: "#467f42",
            X: "#53626a",
            Reddit: "#a43c25",
          }[instance.network] || "#425b73",
          emissiveIntensity: 0.6,
        }),
      );
      instanceNode.position.set(
        Math.cos(angle) * 4.15,
        1.15 + (instanceIndex % 3) * 0.32,
        Math.sin(angle) * 4.15,
      );
      layer.add(instanceNode);
      const approvedSource =
        instance.network === "Mastodon"
          ? instance.consentedFollowers
          : instance.consentedProfiles;
      const approved = Array.isArray(approvedSource)
        ? approvedSource
            .filter(
              (follower) =>
                follower?.consent === true &&
                follower?.public === true &&
                follower?.consentEvidence?.oauthVerifiedByForkMesh === false,
            )
            .slice(0, 8)
        : [];
      approved.forEach((follower, followerIndex) => {
        const followerAngle =
          angle - 0.55 + (followerIndex / Math.max(1, approved.length - 1)) * 1.1;
        const face = makeConsentedProfileFace(THREE, follower);
        face.position.set(
          Math.cos(followerAngle) * 5.25,
          1.8 + (followerIndex % 2) * 0.48,
          Math.sin(followerAngle) * 5.25,
        );
        layer.add(face);
        const profile = makeLabelSprite(
          THREE,
          String(follower.handle || "public").slice(0, 18),
          "user-approved",
          "#ff9eb7",
        );
        profile.scale.set(1.2, 0.4, 1);
        profile.position.set(
          Math.cos(followerAngle) * 5.25,
          2.45 + (followerIndex % 2) * 0.48,
          Math.sin(followerAngle) * 5.25,
        );
        layer.add(profile);
      });
    });
    center.add(layer);
    center.userData.instanceProfiles = layer;
  }

  function updateMediaSpaces(spaces = [], activeSpace = {}) {
    const garden = ctx.landmarkObjects.get("broadcast");
    if (!garden) return;
    const existing = garden.userData.sharedMediaSpaces;
    if (existing) {
      garden.remove(existing);
      existing.traverse((child) => {
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
    }
    const layer = new THREE.Group();
    layer.name = "shared-media-spaces";
    const rooms = (Array.isArray(spaces) ? spaces : []).slice(0, 8);
    rooms.forEach((room, index) => {
      const angle = (index / Math.max(1, rooms.length)) * Math.PI * 2;
      const isActive = String(room?.id || "") === String(activeSpace?.id || "");
      const roomNode = new THREE.Mesh(
        resources.primitive(
          THREE,
          "CylinderGeometry",
          isActive ? 0.6 : 0.42,
          isActive ? 0.72 : 0.52,
          isActive ? 1.25 : 0.88,
          10,
        ),
        makeMaterial(THREE, isActive ? "#9ef7c6" : "#8fcfff", {
          emissive: isActive ? "#2f9f69" : "#2b6e9b",
          emissiveIntensity: isActive ? 0.85 : 0.45,
          metalness: 0.22,
        }),
      );
      roomNode.position.set(
        Math.cos(angle) * 3.65,
        0.68,
        Math.sin(angle) * 3.65,
      );
      roomNode.userData.mediaSpaceId = String(room?.id || "").slice(0, 40);
      roomNode.userData.landmark = "broadcast";
      ctx.interactive.push(roomNode);
      layer.add(roomNode);
      const itemCount = Math.max(0, Number(room?.itemCount) || 0);
      const scheduleCount = Math.max(0, Number(room?.scheduleCount) || 0);
      const label = makeLabelSprite(
        THREE,
        String(room?.name || "Shared room").slice(0, 24),
        `${itemCount} links · ${scheduleCount} scheduled`,
        isActive ? "#9ef7c6" : "#8fcfff",
      );
      label.scale.set(1.75, 0.58, 1);
      label.position.copy(roomNode.position).add(new THREE.Vector3(0, 1.1, 0));
      layer.add(label);
    });

    // The selected server-authoritative playlist and schedule become physical
    // records in the Broadcast Garden. These are coordination markers only:
    // no media URL, stream, or playback data is rendered or relayed here.
    const items = (Array.isArray(activeSpace?.items)
      ? activeSpace.items
      : []).slice(0, 16);
    items.forEach((item, index) => {
      const angle = (index / Math.max(1, items.length)) * Math.PI * 2;
      const disc = new THREE.Mesh(
        resources.primitive(THREE, "TorusGeometry", 0.18, 0.055, 8, 24),
        makeMaterial(THREE, item?.status === "stopped" ? "#9a9a9a" : "#ffd37a", {
          emissive: item?.status === "stopped" ? "#333333" : "#a96722",
          emissiveIntensity: 0.58,
        }),
      );
      disc.rotation.x = Math.PI / 2;
      disc.position.set(
        Math.cos(angle) * 2.0,
        1.1 + (index % 3) * 0.24,
        Math.sin(angle) * 2.0,
      );
      layer.add(disc);
    });
    const schedules = (Array.isArray(activeSpace?.schedules)
      ? activeSpace.schedules
      : []).filter((schedule) => schedule?.status === "scheduled").slice(0, 6);
    schedules.forEach((schedule, index) => {
      const marker = new THREE.Mesh(
        resources.primitive(THREE, "OctahedronGeometry", 0.2, 0),
        makeSharedMaterial(THREE, "#d5b6ff", {
          emissive: "#7650a7",
          emissiveIntensity: 0.72,
        }),
      );
      marker.position.set(-1.25 + index * 0.5, 3.75, 1.15);
      marker.userData.startsAt = Number(schedule?.startsAt) || 0;
      layer.add(marker);
    });
    garden.add(layer);
    garden.userData.sharedMediaSpaces = layer;
  }

  function updateIdentity(nextIdentity) {
    const previousName = String(identity.name || "").trim().toLowerCase();
    const previousStatus = String(identity.accountStatus || "Guest");
    Object.assign(identity, nextIdentity);
    // The visitor's own chest reads exactly like everyone else's: their live
    // activity ticket wins, and the directory record fills the rest in.
    const badgeIdentity = fns.withMemberFacts(identity);
    const nextName = String(identity.name || "").trim().toLowerCase();
    const nextStatus = String(identity.accountStatus || "Guest");
    const accountChanged =
      nextName && (nextName !== previousName || nextStatus !== previousStatus);
    if (accountChanged) {
      const loadingProfile = {
        state: "loading",
        account: nextName,
        handle: `@${nextName}`,
      };
      ctx.player.userData.fediverseProfile = loadingProfile;
      ctx.officeLobbyPlayer.userData.fediverseProfile = loadingProfile;
    }
    updateAvatarBadge(THREE, ctx.player, badgeIdentity, false);
    syncOperatorBelt(THREE, ctx.player, identity.nodes || []);
    updateAvatarBadge(THREE, ctx.officeLobbyPlayer, badgeIdentity, false);
    syncOperatorBelt(
      THREE,
      ctx.officeLobbyPlayer,
      identity.nodes || [],
    );
    updatePlayerLabel(ctx.playerLabel, identity);
    if (accountChanged) {
      queueMicrotask(() =>
        onFediverseProfile({
          peerId: identity.id,
          name: nextName,
          accountStatus: nextStatus,
          self: true,
        }),
      );
    }
    if (identity.isAdmin !== true) {
      ctx.remotePlayers.forEach((avatar, peerId) => {
        fns.removeRemoteModerationControls(avatar, peerId);
      });
    }
  }

  function setInputActive(active) {
    identity.inputActive = active === true;
    // Input activity only changes the shoulder antenna and idle fade. Calling
    // updateIdentity for this bit used to regenerate two canvas badge textures
    // synchronously in the first keydown task, directly delaying movement.
    syncAvatarActivity(THREE, ctx.player, identity);
    syncAvatarActivity(THREE, ctx.officeLobbyPlayer, identity);
    return identity.inputActive;
  }

  return {
    updateNetworkNodes,
    noteMirrorTraffic,
    updateNodeMachines,
    nodeMachineStats: () => nodeMachines?.stats() || null,
    armMirrorPushEffect,
    relayoutNetworkNodes,
    updateMirrorAgentTasks,
    focusNetworkNode,
    updateFederatedInstances,
    updateInstanceHexes,
    worldMeshOwner,
    updateBots,
    updateSystemCapacity,
    updateOrganizations,
    updateFediverseDirectory,
    updateMediaSpaces,
    updateIdentity,
    setInputActive,
  };
}
