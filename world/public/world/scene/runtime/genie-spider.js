import { reactionTexture } from "../avatar-rig.js";
import { canvasTexture } from "../canvas.js";
import * as resources from "../resources.js";
import {
  GENIE_EYES,
  GENIE_POOF_ACT,
  GENIE_REACTION_ACTS,
  GENIE_SPIDER_ACTS,
  GENIE_SPIDER_BODY_Y,
  GENIE_SPIDER_KEY,
  GENIE_SPIDER_LEGS,
  GENIE_SPIDER_OFF_KEY,
  GENIE_SPIDER_SIZE,
  GENIE_TARGET_SIZES,
  createGenieSpiderModel,
  drawGenieAct,
  genieActById,
  genieActTargetKind,
  genieKnee,
  genieLegHips,
  genieLegTargets,
  genieMotionPose,
  genieParticle,
  genieSpiderAllowed,
  paintGenieSpeech,
  setGenieSpiderEyes,
} from "../genie-spider.js";

const START_DELAY_MS = 4000;
const SCAN_MS = 1500;
const SCAN_RADIUS = 34;
const FOLLOW_DISTANCE = 8;
const TELEPORT_DISTANCE = 70;
const WALK_SPEED = 6.5;
const APPROACH_TIMEOUT_MS = 9000;
const IDLE_MIN_MS = 4500;
const IDLE_MAX_MS = 11000;
const PARTICLES = 18;
const THING_SAMPLE = 30;

export function functions({ THREE, reducedMotion, ctx, fns }) {
  const s = {
    model: null,
    enabled: readEnabled(),
    startAt: 0,
    position: new THREE.Vector3(),
    yaw: 0,
    gait: 0,
    mode: "idle",
    act: null,
    target: null,
    anchor: new THREE.Vector3(),
    anchorYaw: 0,
    actStartedAt: 0,
    approachDeadline: 0,
    nextActAt: 0,
    scannedAt: -Infinity,
    candidates: new Map(),
    bag: [],
    blinkAt: 0,
    effect: null,
    speech: null,
    web: null,
    played: 0,
    history: [],
    lastPose: null,
  };
  const hips = genieLegHips();
  const v1 = new THREE.Vector3();
  const v2 = new THREE.Vector3();
  const v3 = new THREE.Vector3();
  const up = new THREE.Vector3(0, 1, 0);
  const matrix = new THREE.Matrix4();
  const quaternion = new THREE.Quaternion();
  const scale = new THREE.Vector3();
  const sizes = new WeakMap();

  function readEnabled() {
    try {
      return genieSpiderAllowed({
        cookie: globalThis.document?.cookie || "",
        offFlag: globalThis.localStorage?.getItem(GENIE_SPIDER_OFF_KEY) ?? null,
        worldFlag: globalThis.localStorage?.getItem(GENIE_SPIDER_KEY) ?? null,
      });
    } catch (_) {
      return true;
    }
  }

  function localPosition(object, target) {
    object.getWorldPosition(target);
    return ctx.world ? ctx.world.worldToLocal(target) : target;
  }

  function shown(object) {
    let node = object;
    for (let depth = 0; node && depth < 10; depth += 1) {
      if (node.visible === false) return false;
      if (node === ctx.world || node === ctx.scene) return true;
      node = node.parent;
    }
    return Boolean(object?.parent);
  }

  const floorCache = { x: NaN, z: NaN, y: 0 };

  function floorAt(x, z, fallback) {
    if (Math.hypot(x - floorCache.x, z - floorCache.z) < 0.35) return floorCache.y;
    const y = fns.outdoorFloorYAt?.(x, z);
    floorCache.x = x;
    floorCache.z = z;
    floorCache.y = Number.isFinite(y) ? y : fallback;
    return floorCache.y;
  }

  function outdoors() {
    if (ctx.officeSceneMode && ctx.officeSceneMode !== "town") return false;
    if (ctx.beachSceneActive) return false;
    return Boolean(ctx.player?.parent);
  }

  function build() {
    if (s.model) return;
    s.model = createGenieSpiderModel(THREE);
    s.model.visible = false;
    ctx.world.add(s.model);
    const player = localPosition(ctx.player, v1);
    s.position.set(player.x + 2.5, player.y, player.z + 1.5);
    s.nextActAt = performance.now() + 3000;
  }

  function teardownEffects() {
    if (s.effect) {
      s.effect.points.removeFromParent();
      s.effect.points.geometry.dispose();
      s.effect.points.material.map?.dispose?.();
      s.effect.points.material.dispose();
      s.effect = null;
    }
    if (s.speech) {
      s.speech.removeFromParent();
      s.speech.material.map?.dispose?.();
      s.speech.material.dispose();
      s.speech = null;
    }
    if (s.web) {
      s.web.removeFromParent();
      s.web.geometry.dispose();
      s.web.material.dispose();
      s.web = null;
    }
  }

  function disposeSpider() {
    teardownEffects();
    if (!s.model) return;
    s.model.removeFromParent();
    const data = s.model.userData.genieSpider;
    for (const mesh of [data.lit, data.glow, data.legs]) {
      mesh.material.dispose();
      mesh.dispose?.();
    }
    s.model = null;
  }

  function addCandidate(kind, object) {
    if (!object || !shown(object)) return;
    const list = s.candidates.get(kind) || [];
    list.push(object);
    s.candidates.set(kind, list);
  }

  function scan(now, player) {
    s.scannedAt = now;
    s.candidates = new Map();
    const near = (object) => {
      if (!object?.parent) return false;
      localPosition(object, v2);
      return v2.distanceTo(player) <= SCAN_RADIUS;
    };
    if (ctx.player) addCandidate("player", ctx.player);
    for (const avatar of ctx.remotePlayers?.values?.() || []) if (near(avatar)) addCandidate("avatar", avatar);
    if (near(ctx.forkbot)) addCandidate("bot", ctx.forkbot);
    for (const [id, object] of ctx.landmarkObjects || []) {
      if (!near(object)) continue;
      addCandidate(id === "fountain" ? "fountain" : id === "campfire" ? "campfire" : "landmark", object);
    }
    for (const state of ctx.bikeStates || []) if (near(state?.bike)) addCandidate("bike", state.bike);
    for (const state of ctx.carStates || []) if (near(state?.car)) addCandidate("car", state.car);
    for (const state of ctx.quadcopterStates || []) if (near(state?.quadcopter)) addCandidate("drone", state.quadcopter);
    if (near(ctx.swingSet)) addCandidate("swing", ctx.swingSet);
    for (const portal of ctx.repositoryPortals?.values?.() || []) if (near(portal?.group)) addCandidate("portal", portal.group);
    for (const board of [ctx.startHereBoard, ctx.worldBulletin, ctx.worldQaBoard, ctx.mastodonKiosk, ctx.officeAttendanceBoard]) {
      if (board?.isObject3D && near(board)) addCandidate("board", board);
    }
    const interactive = Array.isArray(ctx.interactive) ? ctx.interactive : [];
    for (let i = 0; i < THING_SAMPLE && interactive.length; i += 1) {
      const object = interactive[Math.floor(Math.random() * interactive.length)];
      if (object?.isObject3D && object !== ctx.player && near(object)) addCandidate("thing", object);
    }
  }

  function measure(kind, object) {
    const preset = GENIE_TARGET_SIZES[kind] || GENIE_TARGET_SIZES.thing;
    if (!object || kind === "ground") return { ...preset };
    if (kind === "player" || kind === "avatar") {
      const k = object.scale?.y || 1;
      return { radius: preset.radius * k, height: preset.height * k };
    }
    if (kind === "landmark" || kind === "fountain" || kind === "campfire") return { ...preset };
    const cached = sizes.get(object);
    if (cached) return cached;
    let size = { ...preset };
    try {
      const box = new THREE.Box3().setFromObject(object);
      if (!box.isEmpty()) {
        box.getSize(v3);
        size = {
          radius: Math.max(0.6, Math.min(8, Math.max(v3.x, v3.z) / 2)),
          height: Math.max(0.3, Math.min(14, v3.y)),
        };
      }
    } catch (_) {}
    sizes.set(object, size);
    return size;
  }

  function targetCenter(target, out) {
    if (!target?.object) return out.copy(s.anchor);
    return localPosition(target.object, out);
  }

  function say(text, emoji) {
    if (!text) return;
    const texture = canvasTexture(THREE, 512, 128, (context) => paintGenieSpeech(context, `${emoji ? `${emoji} ` : ""}${text}`));
    const sprite = new THREE.Sprite(resources.uniqueMaterial(THREE, { map: texture, transparent: true, depthTest: false, depthWrite: false }, "SpriteMaterial"));
    sprite.name = "forkmesh-genie-spider-speech";
    sprite.renderOrder = 12;
    sprite.scale.set(5.2, 1.3, 1);
    ctx.world.add(sprite);
    s.speech = sprite;
  }

  function startEffect(entry) {
    if (!entry.fx || entry.fx === "none" || reducedMotion) return;
    const material = resources.uniqueMaterial(THREE, {
      map: reactionTexture(THREE, entry.emoji),
      size: 0.85,
      sizeAttenuation: true,
      transparent: true,
      depthWrite: false,
      alphaTest: 0.05,
    }, "PointsMaterial");
    const points = new THREE.Points(undefined, material);
    points.geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(PARTICLES * 3), 3));
    points.name = "forkmesh-genie-spider-effect";
    points.frustumCulled = false;
    points.renderOrder = 11;
    ctx.world.add(points);
    s.effect = { points, mode: entry.fx, seed: Math.random() * 100 };
  }

  function startWeb() {
    const web = new THREE.Line(undefined, resources.uniqueMaterial(THREE, { color: "#e6ecf8", transparent: true, opacity: 0.85 }, "LineBasicMaterial"));
    web.geometry.setAttribute("position", new THREE.BufferAttribute(new Float32Array(6), 3));
    web.name = "forkmesh-genie-spider-web";
    web.frustumCulled = false;
    ctx.world.add(web);
    s.web = web;
  }

  function begin(entry, now, kind, object) {
    teardownEffects();
    s.act = entry;
    s.played += 1;
    const size = measure(kind, object);
    s.target = { kind, object: kind === "ground" ? null : object, ...size };
    s.history = [];
    if (kind === "ground" || entry.motion === "conga") {
      s.mode = "act";
      startAct(now);
    } else {
      s.mode = "approach";
      s.approachDeadline = now + APPROACH_TIMEOUT_MS;
    }
    return true;
  }

  function startAct(now) {
    s.mode = "act";
    s.actStartedAt = now;
    s.anchor.copy(s.position);
    const center = targetCenter(s.target, v2);
    s.anchorYaw = s.target?.object ? Math.atan2(center.x - s.position.x, center.z - s.position.z) : s.yaw;
    s.target.reach = s.target.object ? Math.max(0.5, Math.hypot(center.x - s.position.x, center.z - s.position.z)) : 0;
    if (s.target.object) s.yaw = s.anchorYaw;
    say(s.act.say, s.act.emoji);
    startEffect(s.act);
    if (["dangle", "orbit"].includes(s.act.motion) && (s.act.motion === "dangle" || s.act.p.web)) startWeb();
  }

  function finish(now) {
    teardownEffects();
    s.act = null;
    s.target = null;
    s.mode = "idle";
    s.nextActAt = now + IDLE_MIN_MS + Math.random() * (IDLE_MAX_MS - IDLE_MIN_MS);
  }

  function chooseAct(now) {
    const kinds = [...s.candidates.keys()];
    const entry = drawGenieAct(s.bag, kinds);
    if (!entry) return false;
    const kind = genieActTargetKind(entry, kinds);
    const pool = s.candidates.get(kind) || [];
    const object = pool[Math.floor(Math.random() * pool.length)] || null;
    return begin(entry, now, kind, object);
  }

  function walkToward(goal, delta, speed = WALK_SPEED) {
    const dx = goal.x - s.position.x;
    const dz = goal.z - s.position.z;
    const distance = Math.hypot(dx, dz);
    if (distance < 0.05) return distance;
    const step = Math.min(distance, speed * delta);
    s.position.x += (dx / distance) * step;
    s.position.z += (dz / distance) * step;
    const heading = Math.atan2(dx, dz);
    let turn = heading - s.yaw;
    turn = Math.atan2(Math.sin(turn), Math.cos(turn));
    s.yaw += turn * Math.min(1, delta * 10);
    s.gait += step * 4.5;
    return distance - step;
  }

  function placeLegs(model, mode, time, stride) {
    const data = model.userData.genieSpider;
    const feet = genieLegTargets(mode, { phase: s.gait, time, stride: 0.34 * Math.sign(stride || 1) });
    data.body.updateMatrix();
    for (let leg = 0; leg < GENIE_SPIDER_LEGS; leg += 1) {
      const side = leg < 4 ? -1 : 1;
      const hip = v1.set(...hips[leg]).applyMatrix4(data.body.matrix).toArray();
      const foot = feet[leg];
      const knee = genieKnee(hip, foot, side);
      segment(data.legs, leg * 2, hip, knee);
      segment(data.legs, leg * 2 + 1, knee, foot);
    }
    data.legs.instanceMatrix.needsUpdate = true;
  }

  function segment(mesh, index, from, to) {
    v2.set(to[0] - from[0], to[1] - from[1], to[2] - from[2]);
    const length = v2.length() || 0.001;
    quaternion.setFromUnitVectors(up, v2.divideScalar(length));
    v3.set((from[0] + to[0]) / 2, (from[1] + to[1]) / 2, (from[2] + to[2]) / 2);
    scale.set(1, length, 1);
    mesh.setMatrixAt(index, matrix.compose(v3, quaternion, scale));
  }

  function updateEffect(t, elapsed, origin) {
    const effect = s.effect;
    if (!effect) return;
    const attribute = effect.points.geometry.getAttribute("position");
    for (let i = 0; i < PARTICLES; i += 1) {
      const offset = genieParticle(effect.mode, i, PARTICLES, t, elapsed, effect.seed);
      if (!offset) attribute.setXYZ(i, origin.x, -10000, origin.z);
      else attribute.setXYZ(i, origin.x + offset[0], origin.y + offset[1], origin.z + offset[2]);
    }
    attribute.needsUpdate = true;
  }

  function applyPose(model, poseState, base, time) {
    const data = model.userData.genieSpider;
    const fx = Math.sin(s.anchorYaw);
    const fz = Math.cos(s.anchorYaw);
    const x = base.x + poseState.x * fz + poseState.z * fx;
    const z = base.z - poseState.x * fx + poseState.z * fz;
    const ground = floorAt(x, z, base.y);
    model.position.set(x, ground + poseState.y, z);
    model.rotation.set(0, (poseState.base === "anchor" || poseState.base === "follow" ? s.yaw : s.anchorYaw) + poseState.yaw, 0);
    model.scale.setScalar(poseState.scale * GENIE_SPIDER_SIZE);
    data.body.position.y = GENIE_SPIDER_BODY_Y * (1 - poseState.crouch * 0.6) + Math.sin(time * 0.002) * 0.03;
    data.body.rotation.set(poseState.pitch, 0, poseState.roll);
    data.body.scale.set(1 - poseState.stretch * 0.3, 1 + poseState.stretch, 1 - poseState.stretch * 0.3);
  }

  function updateEyes(now, mode) {
    if (!s.model) return;
    let openness = GENIE_EYES[mode] ?? 1;
    if (mode === "open" || mode === "wide") {
      if (now >= s.blinkAt + 140) s.blinkAt = now + 2500 + Math.random() * 3500;
      if (now >= s.blinkAt) openness = 0.1;
    }
    setGenieSpiderEyes(THREE, s.model, openness);
  }

  function updateGenieSpider(delta, time) {
    if (!s.enabled || !ctx.world || !ctx.player) {
      if (s.model) s.model.visible = false;
      return;
    }
    const now = performance.now();
    if (!s.startAt) s.startAt = now + START_DELAY_MS;
    if (now < s.startAt) return;
    build();
    const model = s.model;
    const visible = outdoors();
    model.visible = visible;
    if (!visible) {
      if (s.act) finish(now);
      return;
    }
    const player = localPosition(ctx.player, new THREE.Vector3());
    s.position.y = player.y;
    if (s.position.distanceTo(player) > TELEPORT_DISTANCE) {
      finish(now);
      s.position.set(player.x + 2.5, player.y, player.z + 1.5);
      begin(GENIE_POOF_ACT, now, "ground", null);
    }
    if (now - s.scannedAt > SCAN_MS) scan(now, player);
    let poseState = null;
    let base = s.position;
    if (s.mode === "approach") {
      const center = targetCenter(s.target, v2);
      const dx = s.position.x - center.x;
      const dz = s.position.z - center.z;
      const away = Math.hypot(dx, dz) || 1;
      const reach = s.target.radius + 2.2;
      const goal = v3.set(center.x + (dx / away) * reach, s.position.y, center.z + (dz / away) * reach);
      const left = reducedMotion ? 0 : walkToward(goal, delta, WALK_SPEED * 1.2);
      if (reducedMotion) s.position.copy(goal);
      if (left < 0.4) startAct(now);
      else if (now > s.approachDeadline || !s.target.object?.parent) finish(now);
      else poseState = genieMotionPose({ motion: "sit" }, 0, 0, s.target);
      if (poseState) poseState = { ...poseState, legs: "walk", crouch: 0, eyes: "open" };
    }
    if (s.mode === "act" && s.act) {
      const elapsed = (now - s.actStartedAt) / 1000;
      const t = elapsed / (s.act.ms / 1000);
      if (t >= 1) {
        finish(now);
      } else {
        poseState = genieMotionPose(s.act, t, elapsed, s.target);
        if (poseState.base === "center" && s.target.object) {
          base = targetCenter(s.target, v2).clone();
          base.y = s.anchor.y;
        } else if (poseState.base === "follow") {
          ctx.player.getWorldDirection(v3);
          const heading = Math.atan2(v3.x, v3.z);
          base = v2.set(player.x - Math.sin(heading) * 2.6, player.y, player.z - Math.cos(heading) * 2.6).clone();
          s.anchorYaw = heading;
          s.yaw += (Math.atan2(player.x - base.x, player.z - base.z) - s.yaw) * Math.min(1, delta * 6);
          s.gait += delta * 7;
        } else {
          base = s.anchor;
          if (poseState.legs === "walk") s.gait += delta * 6 * Math.abs(poseState.stride || 1);
        }
        if (poseState.base !== "anchor") s.position.set(base.x, s.position.y, base.z);
        const origin = v1.set(base.x, base.y, base.z);
        updateEffect(t, elapsed, model.position);
        if (s.speech) {
          s.speech.visible = t < 0.75;
          s.speech.position.set(model.position.x, model.position.y + 2.6 * poseState.scale * GENIE_SPIDER_SIZE, model.position.z);
        }
        if (s.web) {
          const array = s.web.geometry.getAttribute("position");
          const top = poseState.web === "target" ? targetCenter(s.target, v3) : origin;
          array.setXYZ(0, model.position.x, model.position.y + 0.8, model.position.z);
          array.setXYZ(1, top.x, poseState.web === "target" ? top.y + 0.8 : s.anchor.y + 9, top.z);
          array.needsUpdate = true;
        }
      }
    }
    if (!poseState) {
      const gap = s.position.distanceTo(player);
      if (gap > FOLLOW_DISTANCE) {
        const side = v3.set(player.x + 3.2, player.y, player.z + 1.8);
        if (reducedMotion) s.position.copy(side);
        else walkToward(side, delta, gap > 20 ? WALK_SPEED * 2 : WALK_SPEED);
        poseState = genieMotionPose({ motion: "sit" }, 0, 0, {});
        poseState = { ...poseState, legs: reducedMotion ? "stand" : "walk", crouch: 0, eyes: "open" };
      } else {
        poseState = { ...genieMotionPose({ motion: "sway" }, 0, time / 1000, {}), yaw: 0, roll: 0, eyes: "open" };
        const look = Math.atan2(player.x - s.position.x, player.z - s.position.z);
        let turn = look - s.yaw;
        turn = Math.atan2(Math.sin(turn), Math.cos(turn));
        s.yaw += turn * Math.min(1, delta * 2);
        if (!reducedMotion && s.mode === "idle" && now >= s.nextActAt) chooseAct(now);
      }
      s.anchorYaw = s.yaw;
      base = s.position;
    }
    s.lastPose = poseState;
    applyPose(model, poseState, base, time);
    placeLegs(model, poseState.legs, time / 1000, poseState.stride);
    updateEyes(now, poseState.eyes);
  }

  function playGenieSpiderAct(id) {
    const entry = id ? genieActById(String(id)) : null;
    if (id && !entry) return false;
    if (!s.enabled || !ctx.player) return false;
    const now = performance.now();
    s.startAt = Math.min(s.startAt || now, now);
    build();
    if (s.act) finish(now);
    if (!entry) return chooseAct(now);
    const kind = genieActTargetKind(entry, [...s.candidates.keys()]);
    const pool = s.candidates.get(kind) || [];
    const fallbackKind = kind === "ground" || pool.length ? kind : entry.on.includes("player") ? "player" : "ground";
    const object = fallbackKind === "player" ? ctx.player : pool[0] || null;
    return begin(entry, now, fallbackKind, object);
  }

  function reactGenieSpider(emoji) {
    const id = GENIE_REACTION_ACTS[String(emoji || "")];
    if (!id || !s.model || !s.enabled || reducedMotion) return false;
    if (s.mode === "act" && s.act?.motion !== "nap") return false;
    return playGenieSpiderAct(id);
  }

  function setGenieSpiderEnabled(on) {
    s.enabled = Boolean(on);
    if (!s.enabled) {
      finish(performance.now());
      disposeSpider();
    }
    return s.enabled;
  }

  function genieSpiderState() {
    return {
      enabled: s.enabled,
      built: Boolean(s.model),
      visible: Boolean(s.model?.visible),
      mode: s.mode,
      act: s.act?.id || "",
      progress: s.mode === "act" && s.act ? Number(Math.min(1, (performance.now() - s.actStartedAt) / s.act.ms).toFixed(2)) : 0,
      target: s.target?.kind || "",
      played: s.played,
      nearby: Object.fromEntries([...s.candidates].map(([kind, list]) => [kind, list.length])),
      position: s.model ? s.model.position.toArray().map((value) => Number(value.toFixed(2))) : null,
    };
  }

  function genieSpiderActs() {
    return GENIE_SPIDER_ACTS.map((entry) => ({ id: entry.id, title: entry.title, on: [...entry.on], motion: entry.motion, emoji: entry.emoji }));
  }

  return {
    updateGenieSpider,
    playGenieSpiderAct,
    reactGenieSpider,
    setGenieSpiderEnabled,
    genieSpiderState,
    genieSpiderActs,
    disposeGenieSpider: disposeSpider,
  };
}
