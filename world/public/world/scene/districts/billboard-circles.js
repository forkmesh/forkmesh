import { sharedGeometry, sharedMaterial } from "../../world-resource-cache.js";
import {
  BILLBOARD_CIRCLE_GAP,
  BILLBOARD_CIRCLE_GROUND_Y,
  BILLBOARD_CIRCLE_RIM_WIDTH,
  BILLBOARD_CIRCLE_SPECS,
  BILLBOARD_CIRCLE_SURFACE_Y,
} from "../layout.js";
import { districtGroundMaterial } from "../town.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
}) {
  const billboardCircles = new Map();
  // A board only fetches once somebody walks up to its circle, so each one
  // carries the ring that says a refresh is in flight. They are keyed by feed
  // because several boards (the three social banners) share one request.
  const billboardFeedRecords = new Map();
  let billboardPlacementSequence = 0;

  function billboardCircle(id) {
    const spec =
      BILLBOARD_CIRCLE_SPECS[id] || BILLBOARD_CIRCLE_SPECS.hub;
    let circle = billboardCircles.get(spec.id);
    if (circle) return circle;
    const group = new THREE.Group();
    group.name = `forkmesh-${spec.id}-billboard-circle`;
    group.position.set(spec.x, 0, spec.z);
    const groundMaterial = districtGroundMaterial(THREE, "leaderboards");
    groundMaterial.color = new THREE.Color(spec.color);
    const ground = new THREE.Mesh(
      resources.primitive(THREE, "CircleGeometry", spec.minRing + spec.groundMargin, 96),
      groundMaterial,
    );
    ground.name = `forkmesh-${spec.id}-circle-ground`;
    ground.rotation.x = -Math.PI / 2;
    ground.position.y = BILLBOARD_CIRCLE_GROUND_Y;
    ground.receiveShadow = true;
    ground.userData.ground = true;
    group.add(ground);
    const rim = new THREE.Mesh(
      resources.primitive(
        THREE,
        "RingGeometry",
        spec.minRing + spec.groundMargin - BILLBOARD_CIRCLE_RIM_WIDTH,
        spec.minRing + spec.groundMargin,
        96,
      ),
      resources.uniqueMaterial(THREE, { color: spec.accent, toneMapped: false }, "MeshBasicMaterial"),
    );
    rim.name = `forkmesh-${spec.id}-circle-rim`;
    rim.rotation.x = -Math.PI / 2;
    rim.position.y = BILLBOARD_CIRCLE_GROUND_Y + 0.01;
    group.add(rim);
    ctx.leaderboardInterior.add(group);
    circle = { spec, group, ground, rim, objects: [], radius: spec.minRing };
    billboardCircles.set(spec.id, circle);
    return circle;
  }
  function placeBillboardOnIsland(object, options = {}) {
    if (!object) return null;
    const circle = billboardCircle(String(options.circle || "hub"));
    object.parent?.remove(object);
    circle.group.add(object);
    // Measure the board square-on and unplaced: the circle owns its position
    // and heading from here, and the local extents drive both the arc it is
    // given and the height its spinner floats at.
    object.position.set(0, 0, 0);
    object.rotation.y = 0;
    object.updateMatrixWorld(true);
    ctx.billboardIslandBounds.setFromObject(object);
    circle.group.getWorldPosition(ctx.billboardCircleOrigin);
    const record = {
      object,
      circle: circle.spec.id,
      feed: String(options.feed || ""),
      order: Number.isFinite(Number(options.order))
        ? Number(options.order)
        : billboardPlacementSequence++,
      width: Math.max(
        1,
        ctx.billboardIslandBounds.max.x - ctx.billboardIslandBounds.min.x,
      ),
      baseY: ctx.billboardIslandBounds.min.y - ctx.billboardCircleOrigin.y,
      topY: ctx.billboardIslandBounds.max.y - ctx.billboardCircleOrigin.y,
    };
    circle.objects.push(record);
    if (record.feed) {
      // Only a board that has a request of its own gets a ring. It rides just
      // above the board's own top edge, in the board's local frame, so it
      // stays put when the circle re-levels the board onto the apron.
      const spinner = new THREE.Mesh(
        sharedGeometry(THREE, "TorusGeometry", 0.42, 0.1, 10, 30, Math.PI * 1.55),
        sharedMaterial(THREE, "MeshBasicMaterial", {
          color: "#8ff1c3",
          toneMapped: false,
        }),
      );
      spinner.name = `forkmesh-billboard-refresh-spinner-${record.feed}-${circle.objects.length}`;
      spinner.position.set(0, record.topY + 1.1, 0.4);
      spinner.visible = false;
      spinner.renderOrder = 46;
      object.add(spinner);
      record.spinner = spinner;
      ctx.billboardSpinners.push(spinner);
      const shared = billboardFeedRecords.get(record.feed) || [];
      shared.push(record);
      billboardFeedRecords.set(record.feed, shared);
    }
    ctx.relayoutBillboardCircle(circle);
    return record;
  }
  function removeBillboardFromIsland(record) {
    if (!record) return false;
    const circle = billboardCircles.get(record.circle);
    if (!circle) return false;
    const circleIndex = circle.objects.indexOf(record);
    if (circleIndex < 0) return false;
    circle.objects.splice(circleIndex, 1);
    if (record.feed) {
      const feedRecords = billboardFeedRecords.get(record.feed) || [];
      const feedIndex = feedRecords.indexOf(record);
      if (feedIndex >= 0) feedRecords.splice(feedIndex, 1);
      if (feedRecords.length) {
        billboardFeedRecords.set(record.feed, feedRecords);
      } else {
        billboardFeedRecords.delete(record.feed);
      }
    }
    if (record.spinner) {
      record.spinner.visible = false;
      const spinnerIndex = ctx.billboardSpinners.indexOf(record.spinner);
      if (spinnerIndex >= 0) ctx.billboardSpinners.splice(spinnerIndex, 1);
    }
    record.object?.parent?.remove?.(record.object);
    ctx.relayoutBillboardCircle(circle);
    return true;
  }
  // Called by the shell whenever a board's request starts or settles. Nothing
  // else makes a spinner visible, so a spinning ring always means a live fetch
  // that the visitor's own approach started.
  function setBillboardRefreshing(feed, loading) {
    for (const record of billboardFeedRecords.get(String(feed || "")) || []) {
      record.spinner.visible = loading === true;
    }
  }

  return {
    billboardCircle,
    placeBillboardOnIsland,
    removeBillboardFromIsland,
    setBillboardRefreshing,
  };
}

export function build({
  THREE,
  animated,
  ctx,
  placeBillboardOnIsland,
  worldBulletin,
  worldDiscordBoard,
  worldGeneralChatBoard,
}) {
  const billboardIslandBounds = new THREE.Box3();
  ctx.billboardIslandBounds = billboardIslandBounds;
  const billboardCircleOrigin = new THREE.Vector3();
  ctx.billboardCircleOrigin = billboardCircleOrigin;
  const billboardSpinners = [];
  ctx.billboardSpinners = billboardSpinners;
  // Boards are laid out by the arc each one actually needs rather than by an
  // even split, so a wide assembly grows its circle instead of overlapping the
  // board beside it, and a circle of narrow leaderboard cards stays compact.
  const relayoutBillboardCircle = (circle) => {
    const objects = circle.objects;
    objects.sort((left, right) => left.order - right.order);
    let widest = 0;
    let span = 0;
    for (const record of objects) {
      widest = Math.max(widest, record.width);
      span += record.width + BILLBOARD_CIRCLE_GAP;
    }
    // A single board also has to fit inside its own circle. The widest board
    // is flat, so its ends reach sqrt(r² + (w/2)²) from the centre; holding the
    // radius at 0.75w keeps those corners inside the apron's margin even when
    // one wide assembly is the only thing on the circle.
    const radius = Math.max(
      circle.spec.minRing,
      objects.length ? span / (Math.PI * 2) : 0,
      widest * 0.75,
    );
    circle.radius = radius;
    const groundRadius = radius + circle.spec.groundMargin;
    circle.ground.geometry.dispose();
    circle.ground.geometry = resources.primitive(THREE, "CircleGeometry", groundRadius, 96);
    // Scaling would widen the band as well as move it, so the rim follows the
    // apron by rebuilding — the same cheap 96-segment rebuild the apron does.
    circle.rim.geometry.dispose();
    circle.rim.geometry = resources.primitive(
      THREE,
      "RingGeometry",
      groundRadius - BILLBOARD_CIRCLE_RIM_WIDTH,
      groundRadius,
      96,
    );
    let cursor = 0;
    objects.forEach((record) => {
      const object = record.object;
      const slice = record.width + BILLBOARD_CIRCLE_GAP;
      const angle = -Math.PI / 2 + ((cursor + slice / 2) / span) * Math.PI * 2;
      cursor += slice;
      object.position.set(
        Math.cos(angle) * radius,
        0,
        Math.sin(angle) * radius,
      );
      object.rotation.y = Math.atan2(-object.position.x, -object.position.z);
      // Board authors use different local origins (some at their center,
      // others at their footings). Normalize from actual geometry so no
      // frame, post, label, or base can be buried by the shared terrain.
      object.position.y = BILLBOARD_CIRCLE_SURFACE_Y - record.baseY;
    });
  };
  ctx.relayoutBillboardCircle = relayoutBillboardCircle;
  animated.push((time) => {
    for (const spinner of billboardSpinners) {
      if (spinner.visible) spinner.rotation.z = -time * 0.006;
    }
  });
  // The bulletin, the general-chat board, and the Discord board carry no
  // request of their own — they are painted from data the page already holds —
  // so they take a circle but no feed, and never show a spinner.
  placeBillboardOnIsland(worldBulletin, { circle: "hub" });
  placeBillboardOnIsland(worldGeneralChatBoard, { circle: "hub" });
  placeBillboardOnIsland(worldDiscordBoard, { circle: "social" });
  return {
  };
}
