import { canvasTexture } from "./canvas.js";
import { clamp } from "./object-utils.js";
import * as resources from "./resources.js";

export const AVATAR_RIG_STALE_MS = 1500;
export const AVATAR_RIG_SPAN_UNITS = 0.3;
export const AVATAR_RIG_EYE_Y = 3.4;
export const AVATAR_RIG_HAND_Z = -0.48;
export const AVATAR_RIG_DEPTH_UNITS = 0.5;
export const WORD_BADGE_COLORS = Object.freeze({
  talking: { fill: "#1f7a4d", ring: "#7cf2b0", text: "#f2fff8" },
  recent: { fill: "#7a5512", ring: "#ffcf6e", text: "#fff8e8" },
  quiet: { fill: "#2c3a44", ring: "#8fa3b0", text: "#e8f0f4" },
});

const ARM_X = 0.73;
const ARM_Y = 2.08;
const ARM_LENGTH = 1.25;
const SHOULDER_Y = ARM_Y + ARM_LENGTH / 2;
const DOWN = Object.freeze([0, -1, 0]);
const FINGER_X = [-0.085, -0.045, -0.005, 0.035, 0.075];
const FINGER_LENGTH = [0.1, 0.14, 0.15, 0.14, 0.11];

export function compactWordCount(words) {
  const value = Math.max(0, Math.floor(Number(words) || 0));
  if (value < 1000) return String(value);
  if (value < 10000) return `${(value / 1000).toFixed(1).replace(/\.0$/, "")}k`;
  if (value < 1000000) return `${Math.floor(value / 1000)}k`;
  return `${(value / 1000000).toFixed(1).replace(/\.0$/, "")}M`;
}

export function rigHandTarget(hand) {
  const k = AVATAR_RIG_SPAN_UNITS;
  return {
    x: clamp(-(Number(hand?.x) || 0) * k, -1.6, 1.6),
    y: clamp(AVATAR_RIG_EYE_Y - (Number(hand?.y) || 0) * k, 1.2, 4.4),
    z: clamp(AVATAR_RIG_HAND_Z - (Number(hand?.z) || 0) * AVATAR_RIG_DEPTH_UNITS, -1.3, -0.2),
  };
}

export function rigArmDirection(side, target) {
  const shoulder = [side * ARM_X, SHOULDER_Y, 0];
  let dx = target.x - shoulder[0];
  let dy = target.y - shoulder[1];
  let dz = target.z - shoulder[2];
  const length = Math.hypot(dx, dy, dz) || 1;
  dx /= length;
  dy /= length;
  dz /= length;
  return { shoulder, direction: [dx, dy, dz], reach: Math.min(1, length / ARM_LENGTH) };
}

function makeHand(THREE, arm) {
  const hand = new THREE.Group();
  hand.name = "avatar-rig-hand";
  const material = arm.userData.rigHandMaterial || resources.uniqueMaterial(THREE, { color: "#f0c8a0", roughness: 0.75 }, "MeshStandardMaterial");
  arm.userData.rigHandMaterial = material;
  const palm = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.22, 0.2, 0.08), material);
  palm.position.y = -0.1;
  hand.add(palm);
  hand.userData.fingers = FINGER_X.map((x, index) => {
    const joint = new THREE.Group();
    const thumb = index === 0;
    joint.position.set(thumb ? -0.12 : x, thumb ? -0.08 : -0.2, 0);
    if (thumb) joint.rotation.z = 0.7;
    const finger = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.04, FINGER_LENGTH[index], 0.05), material);
    finger.position.y = -FINGER_LENGTH[index] / 2;
    joint.add(finger);
    hand.add(joint);
    return joint;
  });
  hand.position.y = -ARM_LENGTH / 2;
  hand.visible = false;
  arm.add(hand);
  return hand;
}

function restArm(arm, side) {
  arm.position.set(side * ARM_X, ARM_Y, 0);
  arm.quaternion.identity();
  if (arm.userData.rigHand) arm.userData.rigHand.visible = false;
}

function poseArm(THREE, arm, side, hand, state) {
  const target = rigHandTarget(hand);
  const smooth = state.smooth;
  const current = state.current || (state.current = { ...target });
  current.x += (target.x - current.x) * smooth;
  current.y += (target.y - current.y) * smooth;
  current.z += (target.z - current.z) * smooth;
  const { shoulder, direction } = rigArmDirection(side, current);
  const from = state.from || (state.from = new THREE.Vector3(...DOWN));
  const to = state.to || (state.to = new THREE.Vector3());
  to.set(direction[0], direction[1], direction[2]);
  arm.quaternion.setFromUnitVectors(from, to);
  const half = ARM_LENGTH / 2;
  arm.position.set(shoulder[0] + direction[0] * half, shoulder[1] + direction[1] * half, shoulder[2] + direction[2] * half);
  const handGroup = arm.userData.rigHand || (arm.userData.rigHand = makeHand(THREE, arm));
  handGroup.visible = true;
  handGroup.rotation.y = clamp(Number(hand.roll) || 0, -1.4, 1.4) * 0.6;
  const curls = Array.isArray(hand.curl) ? hand.curl : [];
  handGroup.userData.fingers.forEach((joint, index) => {
    const goal = clamp(Number(curls[index]) || 0, 0, 1) * (index === 0 ? 1.1 : 1.6);
    joint.rotation.x += (goal - joint.rotation.x) * smooth;
  });
}

export function animateAvatarRig(THREE, avatar, time, delta) {
  const data = avatar?.userData;
  const rig = data?.voiceRig;
  if (!rig) return;
  const fresh = time - (rig.receivedAt || 0) < AVATAR_RIG_STALE_MS;
  const smooth = 1 - Math.pow(0.0005, Math.min(0.1, Math.max(0.001, delta / 1000 || 0.016)));
  const motion = rig.motion || (rig.motion = { yaw: 0, pitch: 0, roll: 0, arms: { left: {}, right: {} } });
  const goalYaw = fresh && rig.tracked ? clamp(rig.yaw, -1.1, 1.1) : 0;
  const goalPitch = fresh && rig.tracked ? clamp(rig.pitch, -0.6, 0.6) : 0;
  const goalRoll = fresh && rig.tracked ? clamp(rig.roll, -0.6, 0.6) : 0;
  motion.yaw += (goalYaw - motion.yaw) * smooth;
  motion.pitch += (goalPitch - motion.pitch) * smooth;
  motion.roll += (goalRoll - motion.roll) * smooth;
  if (data.headRig) {
    const rotation = data.headRig.rotation;
    const applied = motion.applied || (motion.applied = { yaw: 0, pitch: 0, y: NaN, x: NaN });
    const baseYaw = rotation.y === applied.y ? rotation.y - applied.yaw : rotation.y;
    const basePitch = rotation.x === applied.x ? rotation.x - applied.pitch : rotation.x;
    rotation.order = "YXZ";
    rotation.y = baseYaw + motion.yaw;
    rotation.x = clamp(basePitch + motion.pitch, -0.8, 0.8);
    rotation.z = motion.roll;
    applied.yaw = motion.yaw;
    applied.pitch = rotation.x - basePitch;
    applied.y = rotation.y;
    applied.x = rotation.x;
  }
  const busy = Boolean(data.waveStartedAt || data.handshakeStartedAt || data.hudAction);
  for (const [side, armKey, sign] of [["left", "leftArm", -1], ["right", "rightArm", 1]]) {
    const arm = data[armKey];
    if (!arm) continue;
    const hand = fresh && !busy ? (rig.hands || []).find((entry) => entry.side === side) : null;
    const state = motion.arms[side];
    if (hand) {
      state.smooth = smooth;
      poseArm(THREE, arm, sign, hand, state);
      state.posed = true;
    } else if (state.posed) {
      state.posed = false;
      state.current = null;
      restArm(arm, sign);
    }
  }
}

export function animateWordBadge(badge, time) {
  if (!badge?.material) return;
  const talking = badge.userData.state === "talking";
  badge.material.opacity = talking ? (Math.sin(time * 0.0125) > -0.2 ? 1 : 0.3) : 1;
}

export function setAvatarRigState(avatar, rig, now) {
  if (!avatar?.userData) return;
  if (!rig) {
    const data = avatar.userData;
    if (data.voiceRig?.motion?.arms) {
      if (data.voiceRig.motion.arms.left.posed && data.leftArm) restArm(data.leftArm, -1);
      if (data.voiceRig.motion.arms.right.posed && data.rightArm) restArm(data.rightArm, 1);
    }
    if (data.headRig) data.headRig.rotation.z = 0;
    data.voiceRig = null;
    return;
  }
  const previous = avatar.userData.voiceRig;
  avatar.userData.voiceRig = { ...rig, motion: previous?.motion || null, receivedAt: now };
}

function paintWordBadge(context, words, state) {
  const colors = WORD_BADGE_COLORS[state] || WORD_BADGE_COLORS.quiet;
  context.clearRect(0, 0, 128, 64);
  context.fillStyle = colors.fill;
  context.strokeStyle = colors.ring;
  context.lineWidth = 6;
  context.beginPath();
  context.roundRect(4, 4, 120, 56, 28);
  context.fill();
  context.stroke();
  context.fillStyle = colors.text;
  context.font = "700 30px system-ui, sans-serif";
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(compactWordCount(words), 64, 34);
}

export function syncAvatarWordBadge(THREE, avatar, speech) {
  const data = avatar?.userData;
  const parent = data?.highDetail;
  if (!parent) return;
  if (!speech) {
    if (data.wordBadge) data.wordBadge.visible = false;
    return;
  }
  const words = Math.max(0, Math.floor(Number(speech.words) || 0));
  const state = WORD_BADGE_COLORS[speech.state] ? speech.state : "quiet";
  let badge = data.wordBadge;
  if (!badge) {
    const canvas = globalThis.document?.createElement?.("canvas");
    if (!canvas) return;
    canvas.width = 128;
    canvas.height = 64;
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    badge = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.34, 0.17),
      resources.uniqueMaterial(THREE, { map: texture, transparent: true, depthWrite: false, toneMapped: false }, "MeshBasicMaterial"),
    );
    badge.name = "forkmesh-word-count-badge";
    badge.position.set(0.7, 2.92, -0.24);
    badge.rotation.y = Math.PI;
    badge.renderOrder = 6;
    badge.userData.canvas = canvas;
    parent.add(badge);
    data.wordBadge = badge;
  }
  badge.visible = true;
  const key = `${words}:${state}`;
  if (badge.userData.key === key) return;
  badge.userData.key = key;
  badge.userData.state = state;
  paintWordBadge(badge.userData.canvas.getContext("2d"), words, state);
  badge.material.map.needsUpdate = true;
}

function paintOnAirSign(context) {
  context.clearRect(0, 0, 160, 48);
  context.fillStyle = "#b91c1c";
  context.strokeStyle = "#fecaca";
  context.lineWidth = 4;
  context.beginPath();
  context.roundRect(3, 3, 154, 42, 10);
  context.fill();
  context.stroke();
  context.fillStyle = "#ffffff";
  context.beginPath();
  context.arc(26, 24, 7, 0, Math.PI * 2);
  context.fill();
  context.font = "800 24px system-ui, sans-serif";
  context.textAlign = "left";
  context.textBaseline = "middle";
  context.fillText("ON AIR", 42, 25);
}

export function syncAvatarOnAir(THREE, avatar, on) {
  const data = avatar?.userData;
  const parent = data?.highDetail;
  if (!parent) return false;
  if (!on) {
    if (data.onAirSign) data.onAirSign.visible = false;
    return true;
  }
  let sign = data.onAirSign;
  if (!sign) {
    const canvas = globalThis.document?.createElement?.("canvas");
    if (!canvas) return false;
    canvas.width = 160;
    canvas.height = 48;
    paintOnAirSign(canvas.getContext("2d"));
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    sign = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.6, 0.18),
      resources.uniqueMaterial(THREE, { map: texture, transparent: true, depthWrite: false, toneMapped: false }, "MeshBasicMaterial"),
    );
    sign.name = "forkmesh-on-air-sign";
    sign.position.set(0, 1.62, -0.5);
    sign.rotation.y = Math.PI;
    sign.renderOrder = 6;
    parent.add(sign);
    data.onAirSign = sign;
  }
  sign.visible = true;
  return true;
}

export function animateOnAirSign(sign, time) {
  if (!sign?.visible || !sign.material) return;
  sign.material.opacity = Math.sin(time * 0.006) > -0.35 ? 1 : 0.25;
}

export function setAvatarFaceSource(THREE, avatar, canvas) {
  const face = avatar?.userData?.faceMesh;
  if (!face?.material) return;
  const data = avatar.userData;
  if (!canvas) {
    if (data.rigFaceTexture) {
      if (face.material.map === data.rigFaceTexture) face.material.map = data.rigSavedFaceMap || null;
      data.rigFaceTexture.dispose();
      data.rigFaceTexture = null;
      data.rigSavedFaceMap = null;
      face.material.needsUpdate = true;
    }
    return;
  }
  if (!data.rigFaceTexture || data.rigFaceTexture.image !== canvas) {
    if (data.rigFaceTexture) data.rigFaceTexture.dispose();
    else data.rigSavedFaceMap = face.material.map || null;
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    data.rigFaceTexture = texture;
    face.material.map = texture;
    face.material.needsUpdate = true;
    return;
  }
  if (face.material.map !== data.rigFaceTexture) {
    data.rigSavedFaceMap = face.material.map || null;
    face.material.map = data.rigFaceTexture;
    face.material.needsUpdate = true;
  }
  data.rigFaceTexture.needsUpdate = true;
}

const BUBBLE_VERTEX = `
varying vec3 vNormal;
varying vec3 vView;
varying vec3 vLocal;
void main() {
  vLocal = position;
  vec4 world = modelViewMatrix * vec4(position, 1.0);
  vNormal = normalize(normalMatrix * normal);
  vView = normalize(-world.xyz);
  gl_Position = projectionMatrix * world;
}`;

const BUBBLE_FRAGMENT = `
uniform float uTime;
uniform vec3 uTint;
varying vec3 vNormal;
varying vec3 vView;
varying vec3 vLocal;
vec3 hue(float h) {
  return clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
}
void main() {
  float rim = 1.0 - abs(dot(normalize(vNormal), normalize(vView)));
  float swirl = sin(vLocal.y * 1.7 + uTime * 0.9) * 0.5 + sin(vLocal.x * 2.3 - uTime * 0.6) * 0.5;
  vec3 film = hue(fract(rim * 1.4 + swirl * 0.18 + uTime * 0.05));
  vec3 color = mix(uTint, film, 0.7);
  float alpha = 0.06 + pow(rim, 2.4) * 0.62;
  gl_FragColor = vec4(color, alpha);
}`;

export function createPrivacyBubble(THREE, tag) {
  const material = resources.uniqueMaterial(THREE, {
    vertexShader: BUBBLE_VERTEX,
    fragmentShader: BUBBLE_FRAGMENT,
    uniforms: { uTime: { value: 0 }, uTint: { value: new THREE.Color("#9fd8ff") } },
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
  }, "ShaderMaterial");
  const sphere = new THREE.Mesh(resources.primitive(THREE, "SphereGeometry", 1, 48, 32), material);
  sphere.name = `forkmesh-privacy-bubble:${tag}`;
  sphere.renderOrder = 4;
  sphere.userData.privacyBubble = tag;
  sphere.userData.radius = 3;
  return sphere;
}

export function placePrivacyBubble(bubble, members, time) {
  if (!members.length) return false;
  let x = 0;
  let y = 0;
  let z = 0;
  for (const position of members) {
    x += position.x;
    y += position.y;
    z += position.z;
  }
  x /= members.length;
  y /= members.length;
  z /= members.length;
  let reach = 0;
  for (const position of members) reach = Math.max(reach, Math.hypot(position.x - x, position.z - z));
  const goal = clamp(reach + 2.4, 2.8, 14);
  bubble.userData.radius += (goal - bubble.userData.radius) * 0.08;
  const wobble = 1 + Math.sin(time * 0.0021) * 0.025;
  bubble.position.set(x, y + 2.2, z);
  bubble.scale.set(bubble.userData.radius * wobble, bubble.userData.radius * 0.82 / wobble, bubble.userData.radius * wobble);
  bubble.material.uniforms.uTime.value = time / 1000;
  return true;
}

export function reactionTexture(THREE, emoji) {
  return canvasTexture(THREE, 128, 128, (context) => {
    context.clearRect(0, 0, 128, 128);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '96px system-ui, "Apple Color Emoji", "Segoe UI Emoji", "Noto Color Emoji"';
    context.fillText(emoji, 64, 70);
  });
}
