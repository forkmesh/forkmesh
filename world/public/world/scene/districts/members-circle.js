import { landmarkById } from "../../world-data.js";
import { sharedGeometry } from "../../world-resource-cache.js";
import { SEATED_SEAT_TO_SOLE } from "../avatars.js";
import { canvasTexture, repaintCanvasRegion } from "../canvas.js";
import { CAMPFIRE_GROUND_Y, WORLD_PATH_WIDTH } from "../layout.js";
import {
  cloneSharedMesh,
  cloneSharedPlane,
  makeMaterial,
  makeSharedMaterial,
} from "../materials.js";
import {
  MEMBER_CIRCLE_AVATAR_BAND,
  MEMBER_ENGAGEMENT_BY_ID,
  MEMBER_ENGAGEMENT_STATES,
  memberCircleBandSpec,
  memberHasCustomAvatar,
} from "../member-engagement.js";
import {
  campfireDirtTexture,
  createCampfireBurningLogs,
  createProceduralCampfireEffect,
  drawCampfireSeatPlate,
  memberBandLetteringFor,
  memberBandLetteringText,
  memberBandLetteringTexture,
  memberEngagementBandTexture,
  membersCircleInfoTexture,
} from "../members-circle.js";
import { clamp, removeGeneratedLayer, setShadows } from "../object-utils.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
}) {
  // A place in the circle is a bench with a name on it. Every member the
  // public directory lists owns one, and the open ones past the roster are
  // there for arriving guests. A full-size avatar measures 1.75 across the
  // arms and about 1.05 front to back, so neighbours get
  // MEMBER_CIRCLE_STANDING_ROOM between shoulders and physical rings sit
  // MEMBER_CIRCLE_RING_DEPTH apart. Lifecycle changes get a slightly wider
  // seam: the warm-to-cold bands remain readable even when one category needs
  // several rows of benches.
  const MEMBER_CIRCLE_STANDING_ROOM = 2.4;
  const MEMBER_CIRCLE_RING_DEPTH = 1.5;
  const MEMBER_CIRCLE_FIRST_RADIUS = 4.4;
  const MEMBER_CIRCLE_STATE_GAP = 0.35;
  // Give the closest World-step and power rows full visual moats before the
  // next lifecycle band; the other category seams remain compact as rings grow.
  const MEMBER_CIRCLE_INNER_STATE_GAP = 0.85;
  const CAMPFIRE_BENCH_WIDTH = 1.6;
  const CAMPFIRE_BENCH_DEPTH = 0.6;
  // The circle is never bare: even an empty directory shows a handful of
  // benches waiting round the fire. The ceiling keeps one runaway roster from
  // asking for an unbounded name atlas; a member past it simply stands, the
  // way the whole circle did before the benches came back.
  const CAMPFIRE_MIN_BENCHES = 6;
  const CAMPFIRE_MAX_BENCHES = 500;
  let campfireSeatLabelsDirty = false;
  let campfireSeatAtlasStale = true;
  const campfireSeatLabelsChanged = new Set();

  let memberCircleInfoShown = "";
  let memberCountShown = "";


  function memberCircleRingCapacity(radius) {
    return Math.max(
      1,
      Math.floor((Math.PI * 2 * radius) / MEMBER_CIRCLE_STANDING_ROOM),
    );
  }

  function memberCircleRingPosition(radius, index, count) {
    // Keep the Town-side approach open while spreading a category evenly
    // around the hearth. A one-person band lands on the far side of the fire,
    // not in the walkway.
    const entranceGap = Math.min(Math.PI / 2, 3.6 / radius);
    const usableArc = Math.PI * 2 - entranceGap;
    const angle =
      -Math.PI / 2 + entranceGap / 2 +
      ((index + 0.5) / Math.max(1, count)) * usableArc;
    return new THREE.Vector3(
      Math.cos(angle) * radius,
      0,
      Math.sin(angle) * radius,
    );
  }

  function buildMemberCircleLayout(rankedMembers, guestBenches) {
    const circleBands = [MEMBER_CIRCLE_AVATAR_BAND, ...MEMBER_ENGAGEMENT_STATES];
    const membersByState = new Map(
      circleBands.map((state) => [state.id, []]),
    );
    const counts = Object.fromEntries(
      MEMBER_ENGAGEMENT_STATES.map((state) => [state.id, 0]),
    );
    rankedMembers.forEach((member) => {
      if (member.engagement.id in counts) counts[member.engagement.id] += 1;
      membersByState
        .get(
          memberHasCustomAvatar(member)
            ? MEMBER_CIRCLE_AVATAR_BAND.id
            : member.engagement.id,
        )
        ?.push(member);
    });
    const rings = [];
    const bands = [];
    let nextRadius = MEMBER_CIRCLE_FIRST_RADIUS;

    circleBands.forEach((state) => {
      const categoryMembers = membersByState.get(state.id) || [];
      if (!categoryMembers.length) return;
      const categoryRings = [];
      let offset = 0;
      while (offset < categoryMembers.length) {
        const capacity = memberCircleRingCapacity(nextRadius);
        const occupants = categoryMembers.slice(offset, offset + capacity);
        rings.push({
          radius: nextRadius,
          capacity,
          engagementId: state.id,
          occupants,
          open: 0,
        });
        categoryRings.push(nextRadius);
        offset += occupants.length;
        nextRadius += MEMBER_CIRCLE_RING_DEPTH;
      }
      bands.push({
        id: state.id,
        label: state.label,
        count: categoryMembers.length,
        innerRadius: Math.max(2.25, categoryRings[0] - 0.68),
        outerRadius: categoryRings[categoryRings.length - 1] + 0.68,
      });
      nextRadius +=
        state === MEMBER_CIRCLE_AVATAR_BAND ||
        state.id === "world-step" || state.id === "power"
          ? MEMBER_CIRCLE_INNER_STATE_GAP
          : MEMBER_CIRCLE_STATE_GAP;
    });

    // Open seats stay on the roster's last physical ring whenever shoulder
    // room remains. That keeps a guest close to the gathering and prevents a
    // single spare plank from needlessly widening a large circle.
    let openRemaining = Math.max(
      Math.round(Number(guestBenches) || 0),
      6 - rankedMembers.length,
      1,
    );
    const lastRing = rings[rings.length - 1];
    if (lastRing && lastRing.occupants.length < lastRing.capacity) {
      const shared = Math.min(
        openRemaining,
        lastRing.capacity - lastRing.occupants.length,
      );
      lastRing.open += shared;
      openRemaining -= shared;
    }
    while (openRemaining > 0) {
      const radius = rings.length ? nextRadius : MEMBER_CIRCLE_FIRST_RADIUS;
      const capacity = memberCircleRingCapacity(radius);
      const open = Math.min(capacity, openRemaining);
      rings.push({
        radius,
        capacity,
        engagementId: "",
        occupants: [],
        open,
      });
      openRemaining -= open;
      nextRadius = radius + MEMBER_CIRCLE_RING_DEPTH;
    }

    const slots = [];
    const memberSlots = [];
    rings.forEach((ring) => {
      const ringCount = ring.occupants.length + ring.open;
      ring.occupants.forEach((member, index) => {
        const slot = {
          position: memberCircleRingPosition(ring.radius, index, ringCount),
          engagementId: member.engagement?.id || ring.engagementId,
          bandId: ring.engagementId,
          member,
        };
        slots.push(slot);
        memberSlots.push(slot);
      });
      for (let index = 0; index < ring.open; index += 1) {
        slots.push({
          position: memberCircleRingPosition(
            ring.radius,
            ring.occupants.length + index,
            ringCount,
          ),
          engagementId: "",
          member: null,
        });
      }
    });
    return {
      slots,
      memberSlots,
      bands,
      counts,
      key: rings
        .map(
          (ring) =>
            `${ring.engagementId}:${ring.occupants.length}:${ring.open}:${ring.radius.toFixed(2)}`,
        )
        .join("|"),
    };
  }
  // One bench per place in the rings above, so seats and figures are laid out
  // by the same positioner and can never drift apart. Hundreds of individual
  // six-material seat boxes and separate legs made the lifetime account count
  // scale into thousands of draw submissions, so the shared wood is two
  // instanced draws and every name plate is merged into one atlas-backed mesh.
  // Invisible meshes preserve precise seat raycasting without becoming
  // renderer submissions themselves.
  function rebuildMemberCircleBenches(layout) {
    // Benches are capped, but the layout they are cut from is not: the rings
    // are laid out for everyone who wants a place, and only the woodwork stops
    // at the ceiling. A member past it then stands on a real ring slot of their
    // own instead of on the index the positioner reserves for nobody — which is
    // the middle of the fire.
    const slots = Array.isArray(layout?.slots) ? layout.slots : [];
    const wanted = Math.max(CAMPFIRE_MIN_BENCHES, slots.length);
    const count = Math.min(CAMPFIRE_MAX_BENCHES, wanted);
    if (
      ctx.campfire.userData.seatCount === count &&
      ctx.campfire.userData.seatLayoutKey === layout?.key
    ) {
      return ctx.campfire.userData.seatOffsets;
    }
    // Hands the old ring's wood, name atlas and seat hit targets back before
    // cutting a new one.
    removeGeneratedLayer(ctx.campfire, ctx.campfire.userData.seatRing, ctx.interactive);
    const ring = new THREE.Group();
    ring.name = "campfire-member-circle";
    const seatGeometry = resources.primitive(
      THREE,
      "BoxGeometry",
      CAMPFIRE_BENCH_WIDTH,
      ctx.CAMPFIRE_SEAT_HALF_THICKNESS * 2,
      CAMPFIRE_BENCH_DEPTH,
    );
    const seatMaterial = makeSharedMaterial(THREE, "#ffffff", { roughness: 0.86 });
    const seatInstances = new THREE.InstancedMesh(
      seatGeometry,
      seatMaterial,
      count,
    );
    seatInstances.name = "campfire-member-bench-seats";
    const legGeometry = resources.primitive(
      THREE,
      "BoxGeometry",
      0.16,
      ctx.CAMPFIRE_BENCH_LEG_HEIGHT,
      0.5,
    );
    const legMaterial = makeSharedMaterial(THREE, "#ffffff", { roughness: 0.9 });
    const legInstances = new THREE.InstancedMesh(
      legGeometry,
      legMaterial,
      count * 2,
    );
    legInstances.name = "campfire-member-bench-legs";
    const labelGeometry = new THREE.BufferGeometry();
    const labelMaterial = resources.uniqueMaterial(THREE, {
      toneMapped: false,
      polygonOffset: true,
      polygonOffsetFactor: -1,
      polygonOffsetUnits: -1,
    }, "MeshBasicMaterial");
    const labelMesh = new THREE.Mesh(labelGeometry, labelMaterial);
    labelMesh.name = "campfire-member-bench-label-atlas";
    labelMesh.visible = false;
    labelMesh.castShadow = false;
    labelMesh.receiveShadow = false;
    const labelPositions = [];
    const labelUvs = [];
    const labelIndices = [];
    const labelMatrix = new THREE.Matrix4();
    const labelCorner = new THREE.Vector3();
    const instanceTransform = new THREE.Object3D();
    ring.add(seatInstances, legInstances, labelMesh);
    const seatOffsets = [];
    const benches = [];
    for (let index = 0; index < count; index += 1) {
      const layoutSlot = slots[index];
      const slot = layoutSlot?.position || new THREE.Vector3();
      const engagement = MEMBER_ENGAGEMENT_BY_ID.get(
        String(layoutSlot?.engagementId || ""),
      );
      const bench = new THREE.Group();
      bench.position.set(slot.x, 0, slot.z);
      bench.userData.engagement = engagement?.id || "open";
      // Long axis across the sitter's shoulders, which is the heading the
      // figure on it takes: tangent to the ring, fronting the flames.
      bench.rotation.y = Math.atan2(slot.x, slot.z);
      instanceTransform.position.set(
        bench.position.x,
        ctx.CAMPFIRE_SEAT_Y,
        bench.position.z,
      );
      instanceTransform.rotation.set(0, bench.rotation.y, 0);
      instanceTransform.updateMatrix();
      seatInstances.setMatrixAt(index, instanceTransform.matrix);
      seatInstances.setColorAt(
        index,
        new THREE.Color(engagement?.bench || "#8a5a33"),
      );

      const seat = new THREE.Mesh(seatGeometry, seatMaterial);
      seat.position.y = ctx.CAMPFIRE_SEAT_Y;
      seat.visible = false;
      // Seat coordinates are read from the live world matrix at click time, so
      // a relocated campfire needs no bookkeeping and the seats can never
      // drift.
      seat.userData.campfireBench = true;
      seat.userData.raycastProxy = true;
      ctx.interactive.push(seat);
      bench.add(seat);
      [-0.62, 0.62].forEach((end, legIndex) => {
        instanceTransform.position.set(
          bench.position.x + Math.cos(bench.rotation.y) * end,
          ctx.CAMPFIRE_BENCH_LEG_HEIGHT / 2,
          bench.position.z - Math.sin(bench.rotation.y) * end,
        );
        instanceTransform.rotation.set(0, bench.rotation.y, 0);
        instanceTransform.updateMatrix();
        legInstances.setMatrixAt(index * 2 + legIndex, instanceTransform.matrix);
        legInstances.setColorAt(
          index * 2 + legIndex,
          new THREE.Color(engagement?.legs || "#4f3018"),
        );
      });
      // A resource-free child remains as the per-member chat-bubble anchor.
      // The visible quad is written into the single merged atlas mesh below.
      const label = new THREE.Object3D();
      label.name = `campfire-member-bench-label-${index + 1}`;
      label.position.y = ctx.CAMPFIRE_SEAT_TOP_Y + 0.002;
      label.rotation.x = -Math.PI / 2;
      bench.add(label);
      bench.updateMatrix();
      label.updateMatrix();
      labelMatrix.multiplyMatrices(bench.matrix, label.matrix);
      const vertex = index * 4;
      for (const [x, y] of [
        [-0.8, 0.3],
        [0.8, 0.3],
        [-0.8, -0.3],
        [0.8, -0.3],
      ]) {
        labelCorner.set(x, y, 0).applyMatrix4(labelMatrix);
        labelPositions.push(labelCorner.x, labelCorner.y, labelCorner.z);
      }
      labelUvs.push(0, 1, 1, 1, 0, 0, 1, 0);
      labelIndices.push(
        vertex,
        vertex + 2,
        vertex + 1,
        vertex + 2,
        vertex + 3,
        vertex + 1,
      );
      benches.push({
        bench,
        seat,
        label,
        labelKey: null,
        engagementId: engagement?.id || "",
      });
      ring.add(bench);
      // Offsets carry the top of the plank; sitters are placed by their hips
      // off it (seatedAvatarY), the same way the local player is.
      seatOffsets.push(
        new THREE.Vector3(
          bench.position.x,
          ctx.CAMPFIRE_SEAT_TOP_Y,
          bench.position.z,
        ),
      );
    }
    seatInstances.instanceMatrix.needsUpdate = true;
    seatInstances.instanceColor.needsUpdate = true;
    legInstances.instanceMatrix.needsUpdate = true;
    legInstances.instanceColor.needsUpdate = true;
    labelGeometry.setAttribute(
      "position",
      new THREE.Float32BufferAttribute(labelPositions, 3),
    );
    labelGeometry.setAttribute(
      "uv",
      new THREE.Float32BufferAttribute(labelUvs, 2),
    );
    labelGeometry.setIndex(labelIndices);
    labelGeometry.computeBoundingSphere();
    seatInstances.castShadow = true;
    seatInstances.receiveShadow = true;
    legInstances.castShadow = true;
    legInstances.receiveShadow = true;
    ctx.campfire.add(ring);
    ctx.campfire.userData.seatRing = ring;
    ctx.campfire.userData.seatCount = count;
    ctx.campfire.userData.seatLayoutTotal = wanted;
    ctx.campfire.userData.seatLayoutKey = layout?.key || "";
    ctx.campfire.userData.seatRadius = Math.max(
      0,
      ...seatOffsets.map((offset) => Math.hypot(offset.x, offset.z)),
    );
    ctx.campfire.userData.seatOffsets = seatOffsets;
    ctx.campfire.userData.seatBenches = benches;
    ctx.campfire.userData.seatLabelMesh = labelMesh;
    campfireSeatLabelsDirty = true;
    campfireSeatAtlasStale = true;
    return seatOffsets;
  }

  // Rebrands one bench's plank top, skipping the canvas work when the seat
  // already shows this name and away state. The ring is not built until the
  // first updateMemberLounge, so an earlier call simply finds no bench.
  function setCampfireSeatLabel(
    index,
    name,
    state = "seated",
    engagementId = "",
  ) {
    const bench = (ctx.campfire.userData.seatBenches || [])[index];
    if (!bench) return;
    const category =
      String(engagementId || "") || String(bench.engagementId || "");
    const key = `${name} ${state} ${category}`;
    if (bench.labelKey === key) return;
    bench.labelKey = key;
    bench.label.userData.name = String(name || "Campfire").slice(0, 32);
    bench.labelName = String(name || "");
    bench.labelState = state;
    bench.engagementId = category;
    bench.bench.userData.engagement = category || "open";
    campfireSeatLabelsDirty = true;
    campfireSeatLabelsChanged.add(index);
  }

  function repaintCampfireSeatLabels() {
    if (!campfireSeatLabelsDirty) return;
    campfireSeatLabelsDirty = false;
    const benches = ctx.campfire.userData.seatBenches || [];
    const labelMesh = ctx.campfire.userData.seatLabelMesh;
    const uvAttribute = labelMesh?.geometry?.attributes?.uv;
    if (!labelMesh?.material || !uvAttribute || !benches.length) return;
    const cellWidth = ctx.compactRenderer ? 128 : 160;
    const cellHeight = ctx.compactRenderer ? 48 : 60;
    const columns = Math.min(
      20,
      Math.max(1, Math.ceil(Math.sqrt(benches.length * 0.375))),
    );
    const rows = Math.ceil(benches.length / columns);
    const atlasWidth = columns * cellWidth;
    const atlasHeight = rows * cellHeight;
    const changed = [...campfireSeatLabelsChanged];
    campfireSeatLabelsChanged.clear();
    const atlasKey = `${benches.length}:${columns}:${cellWidth}:${cellHeight}`;
    if (
      !campfireSeatAtlasStale &&
      labelMesh.material.map?.userData?.seatAtlasKey === atlasKey &&
      changed.length < benches.length / 2
    ) {
      changed.forEach((index) => {
        const bench = benches[index];
        if (!bench) return;
        const x = (index % columns) * cellWidth;
        const y = Math.floor(index / columns) * cellHeight;
        repaintCanvasRegion(labelMesh.material, x, y, cellWidth, cellHeight, (context) => {
          drawCampfireSeatPlate(
            context,
            bench.labelName,
            bench.labelState,
            x,
            y,
            cellWidth,
            cellHeight,
            bench.engagementId,
          );
        });
      });
      return;
    }
    campfireSeatAtlasStale = false;
    const texture = canvasTexture(
      THREE,
      atlasWidth,
      atlasHeight,
      (context) => {
        benches.forEach((bench, index) => {
          drawCampfireSeatPlate(
            context,
            bench.labelName,
            bench.labelState,
            (index % columns) * cellWidth,
            Math.floor(index / columns) * cellHeight,
            cellWidth,
            cellHeight,
            bench.engagementId,
          );
        });
      },
    );
    const uvs = uvAttribute.array;
    benches.forEach((_bench, index) => {
      const column = index % columns;
      const row = Math.floor(index / columns);
      const u0 = (column * cellWidth) / atlasWidth;
      const u1 = ((column + 1) * cellWidth) / atlasWidth;
      const v1 = 1 - (row * cellHeight) / atlasHeight;
      const v0 = 1 - ((row + 1) * cellHeight) / atlasHeight;
      uvs.set([u0, v1, u1, v1, u0, v0, u1, v0], index * 8);
    });
    uvAttribute.needsUpdate = true;
    labelMesh.material.map?.dispose?.();
    texture.userData.seatAtlasKey = atlasKey;
    labelMesh.material.map = texture;
    labelMesh.material.needsUpdate = true;
    labelMesh.visible = true;
    labelMesh.userData.atlasWidth = atlasWidth;
    labelMesh.userData.atlasHeight = atlasHeight;
  }

  function rebuildMemberEngagementBands(layout) {
    const bands = Array.isArray(layout?.bands) ? layout.bands : [];
    const key = bands
      .map(
        (band) =>
          `${band.id}:${band.count}:${band.innerRadius.toFixed(2)}:${band.outerRadius.toFixed(2)}`,
      )
      .join("|");
    ctx.campfire.userData.engagementCounts = { ...(layout?.counts || {}) };
    ctx.campfire.userData.engagementBands = bands.map((band) => ({ ...band }));
    ctx.campfireGround.userData.engagementCounts = {
      ...(layout?.counts || {}),
    };
    if (ctx.campfire.userData.engagementBandKey === key) return;
    removeGeneratedLayer(
      ctx.campfire,
      ctx.campfire.userData.engagementBandLayer,
      ctx.interactive,
    );
    const layer = new THREE.Group();
    layer.name = "campfire-member-engagement-bands";
    const lettering = bands.length
      ? memberBandLetteringTexture(
          THREE,
          bands
            .map((band) => ({ band, spec: memberCircleBandSpec(band.id) }))
            .filter((entry) => entry.spec)
            .map((entry) => ({
              band: entry.band,
              text: memberBandLetteringText(entry.spec, entry.band.count),
            })),
        )
      : null;
    bands.forEach((band) => {
      const engagement = memberCircleBandSpec(band.id);
      if (!engagement) return;
      const letters = lettering
        ? memberBandLetteringFor(lettering, band.outerRadius)
        : null;
      const map = memberEngagementBandTexture(THREE, engagement);
      const repeat = Math.max(3, band.outerRadius / 2.4);
      map.repeat.set(repeat, repeat);
      const mesh = new THREE.Mesh(
        new THREE.RingGeometry(
          band.innerRadius,
          band.outerRadius,
          Math.max(48, Math.min(96, Math.round(band.outerRadius * 8))),
          1,
        ),
        resources.uniqueMaterial(THREE, {
          color: "#ffffff",
          map,
          emissive: engagement.accent,
          emissiveMap: letters,
          emissiveIntensity: letters ? 0.24 : 0,
          bumpMap: letters,
          bumpScale: 3,
          roughness: 0.98,
          metalness: 0,
          transparent: true,
          opacity: 0.82,
          depthWrite: false,
          polygonOffset: true,
          polygonOffsetFactor: -1,
          polygonOffsetUnits: -1,
        }, "MeshStandardMaterial"),
      );
      mesh.name = `campfire-member-engagement-band-${engagement.id}`;
      mesh.rotation.x = -Math.PI / 2;
      mesh.position.y = CAMPFIRE_GROUND_Y + 0.006;
      mesh.renderOrder = 1;
      mesh.userData.engagement = engagement.id;
      mesh.userData.label = engagement.label;
      mesh.userData.memberCount = band.count;
      mesh.userData.lettering = letters
        ? memberBandLetteringText(engagement, band.count)
        : "";
      mesh.userData.innerRadius = band.innerRadius;
      mesh.userData.outerRadius = band.outerRadius;
      layer.add(mesh);

      const rim = new THREE.Mesh(
        new THREE.TorusGeometry(
          band.outerRadius,
          0.025,
          4,
          Math.max(48, Math.min(96, Math.round(band.outerRadius * 8))),
        ),
        resources.uniqueMaterial(THREE, {
          color: engagement.accent,
          transparent: true,
          opacity: 0.7,
          toneMapped: false,
          depthWrite: false,
        }, "MeshBasicMaterial"),
      );
      rim.name = `campfire-member-engagement-rim-${engagement.id}`;
      rim.rotation.x = Math.PI / 2;
      rim.position.y = CAMPFIRE_GROUND_Y + 0.013;
      rim.renderOrder = 2;
      layer.add(rim);

    });
    ctx.campfire.add(layer);
    ctx.campfire.userData.engagementBandLayer = layer;
    ctx.campfire.userData.engagementBandKey = key;
  }
  function setMemberCircleInfo(total, newest = "", counts = {}) {
    const count = Math.max(0, Math.min(999999, Math.round(Number(total) || 0)));
    const latest = String(newest || "").trim().slice(0, 18);
    const boundedCounts = Object.fromEntries(
      MEMBER_ENGAGEMENT_STATES.map((state) => [
        state.id,
        Math.max(0, Math.round(Number(counts?.[state.id]) || 0)),
      ]),
    );
    const key = `${count}|${latest}|${JSON.stringify(boundedCounts)}`;
    if (memberCircleInfoShown === key) return;
    memberCircleInfoShown = key;
    ctx.memberCircleInfoFace.material.map?.dispose?.();
    ctx.memberCircleInfoFace.material.map = membersCircleInfoTexture(
      THREE,
      count,
      latest,
      boundedCounts,
    );
    ctx.memberCircleInfoFace.material.needsUpdate = true;
  }
  // Headline member data belongs at the approach sign. Nothing over the fire
  // changes with the roster, and the directory population remains bounded.
  function setCampfireMemberCount(total, newest = "", counts = {}) {
    const count = Math.max(0, Math.min(999999, Math.round(Number(total) || 0)));
    const latest = String(newest || "").trim().slice(0, 18);
    const key = `${count}|${latest}|${JSON.stringify(counts)}`;
    if (memberCountShown === key) return;
    memberCountShown = key;
    setMemberCircleInfo(count, latest, counts);
  }

  return {
    buildMemberCircleLayout,
    rebuildMemberCircleBenches,
    setCampfireSeatLabel,
    repaintCampfireSeatLabels,
    rebuildMemberEngagementBands,
    setMemberCircleInfo,
    setCampfireMemberCount,
  };
}

export async function build({
  THREE,
  animated,
  ctx,
  interactive,
  landmarkObjects,
  registerWorldElement,
  sceneSlice,
  setMemberCircleInfo,
  world,
}) {
  const campfire = new THREE.Group();
  campfire.userData.engagementLegend = MEMBER_ENGAGEMENT_STATES.map(
    (state, order) => ({
      id: state.id,
      label: state.label,
      order,
      bench: state.bench,
      ground: state.ground,
      accent: state.accent,
      texture: state.texture,
    }),
  );
  // The fire stands on its own map landmark, so the world-map spot and the
  // benches can never drift apart.
  campfire.position.set(...landmarkById("campfire").position);
  const campfireGround = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", 1, 96),
    resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      map: campfireDirtTexture(THREE),
      roughness: 1,
      metalness: 0,
    }, "MeshStandardMaterial"),
  );
  campfireGround.name = "campfire-member-circle-dirt";
  campfireGround.rotation.x = -Math.PI / 2;
  // The dirt is laid just above the promenade it extends, so the pavement is
  // physically present but disappears beneath the circle.
  campfireGround.position.y = CAMPFIRE_GROUND_Y;
  campfireGround.receiveShadow = true;
  campfire.add(campfireGround);

  const MEMBER_CLEARING_RADIUS = 12;
  campfireGround.scale.setScalar(MEMBER_CLEARING_RADIUS + 0.8);

  // Bench height is set by the sitters, not the other way round: the plank top
  // lands SEATED_SEAT_TO_SOLE above the clearing's own dirt so a seated
  // avatar's shins reach the ground instead of dangling (or folding through
  // it), and the legs below reach that same dirt.
  const CAMPFIRE_SEAT_HALF_THICKNESS = 0.07;
  const CAMPFIRE_SEAT_TOP_Y = CAMPFIRE_GROUND_Y + SEATED_SEAT_TO_SOLE;
  const CAMPFIRE_SEAT_Y = CAMPFIRE_SEAT_TOP_Y - CAMPFIRE_SEAT_HALF_THICKNESS;
  const CAMPFIRE_BENCH_LEG_HEIGHT =
    CAMPFIRE_SEAT_Y - CAMPFIRE_SEAT_HALF_THICKNESS;

  // The roster total remains useful without an enclosure. Mount it on a
  // freestanding, low-triangle sign beside the open circle's Town-side
  // approach. From the approaching visitor's view +X is left; keeping the
  // complete board beyond the path edge leaves the route into the circle
  // unobstructed.
  const MEMBER_INFO_SIGN_WIDTH = 6.2;
  const MEMBER_INFO_SIGN_X =
    (WORLD_PATH_WIDTH + MEMBER_INFO_SIGN_WIDTH) / 2 + 1.1;
  const memberCircleInfo = new THREE.Group();
  memberCircleInfo.name = "members-circle-info-sign";
  memberCircleInfo.position.set(
    MEMBER_INFO_SIGN_X,
    0,
    -MEMBER_CLEARING_RADIUS - 1.1,
  );
  const memberCircleSignMaterial = resources.uniqueMaterial(THREE, {
    transparent: true,
    toneMapped: false,
  }, "MeshBasicMaterial");
  const memberCircleInfoFace = cloneSharedPlane(
    THREE,
    MEMBER_INFO_SIGN_WIDTH,
    3.1,
    memberCircleSignMaterial,
  );
  ctx.memberCircleInfoFace = memberCircleInfoFace;
  memberCircleInfoFace.name = "members-circle-member-info";
  memberCircleInfoFace.position.y = 3.25;
  memberCircleInfoFace.rotation.y = Math.PI;
  memberCircleInfoFace.renderOrder = 12;
  memberCircleInfoFace.userData.interactive = "member-circle-info";
  interactive.push(memberCircleInfoFace);
  memberCircleInfo.add(memberCircleInfoFace);
  const memberCircleInfoBack = cloneSharedPlane(
    THREE,
    MEMBER_INFO_SIGN_WIDTH,
    3.1,
    memberCircleSignMaterial,
  );
  memberCircleInfoBack.name = "members-circle-member-info-back";
  memberCircleInfoBack.position.set(0, 3.25, 0.02);
  memberCircleInfoBack.renderOrder = 12;
  memberCircleInfoBack.userData.interactive = "member-circle-info";
  interactive.push(memberCircleInfoBack);
  memberCircleInfo.add(memberCircleInfoBack);
  const memberCirclePostMaterial = makeMaterial(THREE, "#6e421f", {
    roughness: 0.9,
  });
  [-2.65, 2.65].forEach((x) => {
    const post = cloneSharedMesh(
      THREE,
      "member-circle-sign-post",
      () => new THREE.BoxGeometry(0.24, 3.5, 0.3),
      memberCirclePostMaterial,
    );
    post.position.set(x, 1.75, 0.08);
    memberCircleInfo.add(post);
  });
  campfire.add(memberCircleInfo);
  setMemberCircleInfo(0, "");
  const firePit = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 1.45, 1.65, 0.22, 16),
    makeSharedMaterial(THREE, "#4a4038", { roughness: 0.9 }),
  );
  firePit.position.y = 0.11;
  firePit.userData.campfirePit = true;
  interactive.push(firePit);
  campfire.add(firePit);
  for (let index = 0; index < 12; index += 1) {
    const stoneAngle = (index / 12) * Math.PI * 2;
    const stone = new THREE.Mesh(
      sharedGeometry(THREE, "DodecahedronGeometry", 0.22, 0),
      makeSharedMaterial(THREE, "#7d766c", { roughness: 0.95 }),
    );
    stone.position.set(
      Math.cos(stoneAngle) * 1.85,
      0.16,
      Math.sin(stoneAngle) * 1.85,
    );
    campfire.add(stone);
  }
  const burningLogs = createCampfireBurningLogs(THREE);
  campfire.add(burningLogs);
  const proceduralFire = createProceduralCampfireEffect(THREE);
  const FLAME_HEIGHT = 1.4;
  const FLAME_BASE_Y = 0.28;
  proceduralFire.position.y = FLAME_BASE_Y;
  campfire.add(proceduralFire);
  // The blaze is a milestone marker rather than a per-member trickle: it holds
  // its size through a hundred accounts and steps up a notch when the next
  // century lands, bounded so a large community's fire stays welcoming.
  const CAMPFIRE_BASE_FIRE_LEVEL = 3;
  const CAMPFIRE_FIRE_LEVEL_PER_CENTURY = 0.35;
  const CAMPFIRE_MAX_FIRE_LEVEL = 5.4;
  let fireLevel = CAMPFIRE_BASE_FIRE_LEVEL;
  // Reduced-motion visitors skip animated callbacks, so establish a complete
  // static procedural blaze before the first roster update.
  proceduralFire.scale.set(
    CAMPFIRE_BASE_FIRE_LEVEL,
    CAMPFIRE_BASE_FIRE_LEVEL,
    CAMPFIRE_BASE_FIRE_LEVEL,
  );
  const fireLight = new THREE.PointLight("#ffa14d", 3.2, 14, 1.8);
  fireLight.position.y = FLAME_BASE_Y + FLAME_HEIGHT * CAMPFIRE_BASE_FIRE_LEVEL * 0.5;
  fireLight.castShadow = false;
  campfire.add(fireLight);
  animated.push((time) => {
    const seconds = time * 0.001;
    const slowFlicker = Math.sin(time * 0.0073 + Math.sin(time * 0.0011) * 1.7);
    const fastFlicker = Math.sin(time * 0.0197 + Math.sin(time * 0.0043) * 2.2);
    const sparkFlicker = Math.sin(time * 0.0431 + Math.sin(time * 0.0127));
    const flicker = 1 + slowFlicker * 0.075 + fastFlicker * 0.04;
    // A milestone fire grows mostly upward: widening it at the same rate would
    // push the flames out past their own stone ring.
    const fireGrowth = fireLevel / CAMPFIRE_BASE_FIRE_LEVEL;
    const fireWidthScale =
      CAMPFIRE_BASE_FIRE_LEVEL * (1 + (fireGrowth - 1) * 0.32);
    proceduralFire.scale.set(
      fireWidthScale * flicker,
      fireLevel * (1 + slowFlicker * 0.055 + fastFlicker * 0.025),
      fireWidthScale * flicker,
    );
    proceduralFire.userData.flameMaterials.forEach((material, index) => {
      material.uniforms.uTime.value = seconds + index * 0.17;
    });
    proceduralFire.userData.emberMaterial.uniforms.uTime.value = seconds;
    proceduralFire.userData.emberMaterial.uniforms.uHeight.value =
      Math.min(1.7, fireGrowth);
    proceduralFire.userData.smokeMaterial.uniforms.uTime.value = seconds;
    proceduralFire.userData.smokeMaterial.uniforms.uGrowth.value = fireGrowth;
    proceduralFire.userData.groundGlow.material.uniforms.uOpacity.value =
      0.28 + slowFlicker * 0.035 + fastFlicker * 0.018;
    burningLogs.userData.coalUniforms.uCoalClock.value =
      (time * 0.009) % (Math.PI * 2);
    burningLogs.userData.coalUniforms.uCoalSpark.value = sparkFlicker;
    fireLight.intensity =
      3.2 * fireLevel +
      slowFlicker * 0.65 +
      fastFlicker * 0.36 +
      sparkFlicker * 0.16;
    fireLight.position.set(
      Math.sin(time * 0.0041 + fastFlicker) * 0.13,
      FLAME_BASE_Y + FLAME_HEIGHT * fireLevel * 0.5 + slowFlicker * 0.09,
      Math.sin(time * 0.0033 + slowFlicker * 1.4) * 0.11,
    );
  });
  // The landmark is complete before directory data arrives; the first member
  // refresh only repaints its freestanding information sign.
  setShadows(campfire);
  world.add(campfire);
  registerWorldElement(
    "campfire", "Members Circle campfire", "Districts", campfire,
  );
  await sceneSlice();
  landmarkObjects.set("campfire", campfire);
  return {
    campfire,
    campfireGround,
    MEMBER_CLEARING_RADIUS,
    CAMPFIRE_SEAT_HALF_THICKNESS,
    CAMPFIRE_SEAT_TOP_Y,
    CAMPFIRE_SEAT_Y,
    CAMPFIRE_BENCH_LEG_HEIGHT,
    memberCircleInfo,
    get fireLevel() {
      return fireLevel;
    },
    set fireLevel(value) {
      fireLevel = value;
    },
  };
}
