import { normalizeWorldStatus } from "../world-data.js";
import { hashNumber, paintProceduralAvatarFace } from "../world-avatar-face.js";
import { badgeTexture } from "./avatar-badges.js";
import { syncAvatarAchievementBadges } from "./avatar-controls.js";
import {
  avatarDetailParent,
  avatarInstancedPartGeometry,
} from "./avatar-instancing.js";
import { countryShirtTexture } from "./avatar-outfits.js";
import {
  ACTIVITY_LIGHT_COLORS,
  ANTENNA_STALK_COLOR,
  AVATAR_EMOJI_SKIN_COLOR,
  AVATAR_LEG_SEGMENT,
  AVATAR_SHOE_HEIGHT,
  emojiPixels,
} from "./avatar-shared.js";
import { canvasArtTexture, canvasTexture, softwareCanvas } from "./canvas.js";
import { makeLabelSprite } from "./labels.js";
import {
  avatarHitProxyMaterial,
  cloneSharedMesh,
  cloneSharedPlane,
  makeMaterial,
  makeSharedMaterial,
} from "./materials.js";
import {
  clamp,
  disposeOwnedGeometry,
  disposeOwnedMaterial,
  setShadows,
} from "./object-utils.js";
import { syncAvatarWallet } from "./treasury.js";
import * as resources from "./resources.js";

// Legs hinge at the hip and again at the knee. Avatar fronts face local -Z, so
// the positive pitch about X is the one that swings the knee over the front
// edge of the bench instead of out behind the sitter; the knee then folds back
// by the same amount, which drops the shin straight down and leaves the shoe
// flat on the floor.
const SEATED_LEG_PITCH = 1.45;
const SEATED_KNEE_PITCH = -SEATED_LEG_PITCH;
// Walking swings the whole leg from the hip, so the stride pitch is about half
// what the old mid-leg pivot needed for the same amount of foot travel.
export const GAIT_LEG_SWING = 0.38;
// Avatar leg metrics, in local (unscaled) avatar units: the hip joint height,
// the length of one leg segment (thigh and shin are the same), and how far the
// sole of the shoe sits below the knee — which, with the sole on the avatar's
// origin, is simply the height of the knee joint.
const AVATAR_HIP_Y = 1.405;
export const AVATAR_ARM_X = 0.73;
export const AVATAR_ARM_Y = 2.08;
export const AVATAR_ARM_LENGTH = 1.25;
export const AVATAR_SHOULDER_Y = AVATAR_ARM_Y + AVATAR_ARM_LENGTH / 2;
// The shoe is centred exactly half its own height up, which puts the sole on
// the avatar's origin rather than a fraction of a unit above it. A floor
// height is therefore the surface the avatar is standing on, with no residual
// lift left to hover on once that floor is correct.
const AVATAR_SHOE_Y = AVATAR_SHOE_HEIGHT / 2;
const AVATAR_KNEE_TO_SOLE = AVATAR_HIP_Y - AVATAR_LEG_SEGMENT;
// A seated avatar is placed by its hips, not by its feet: the folded thigh
// rests on top of the plank (its half-depth once pitched over, so it lies on
// the seat instead of sinking through it) and the shin carries the shoe down
// to the floor, which lands the sole SEATED_SEAT_TO_SOLE below the plank.
const SEATED_HIP_ABOVE_SEAT = 0.3;
// How far below the plank a sitter's soles end up, which is what a bench has
// to be built up to for the feet to reach the floor.
export const SEATED_SEAT_TO_SOLE =
  AVATAR_LEG_SEGMENT * Math.cos(SEATED_LEG_PITCH) +
  AVATAR_KNEE_TO_SOLE -
  SEATED_HIP_ABOVE_SEAT;
export function seatedAvatarY(seatTopY, scale = 1) {
  return seatTopY - (AVATAR_HIP_Y - SEATED_HIP_ABOVE_SEAT) * scale;
}
export const REGISTERED_LOUNGE_STATUSES = new Set([
  "Registered",
  "Supporting member",
  "Mirror operator",
  "Organization admin",
]);

// Straight-legged pitches (standing, walking) leave the knees locked, so the
// leg reads as the one block it draws as.
export function applyLegPitch(avatar, leftPitch, rightPitch) {
  const legs = avatar?.userData;
  if (!legs?.leftLeg || !legs?.rightLeg) return;
  legs.leftLeg.rotation.x = leftPitch;
  legs.rightLeg.rotation.x = rightPitch;
  if (legs.leftKnee) legs.leftKnee.rotation.x = 0;
  if (legs.rightKnee) legs.rightKnee.rotation.x = 0;
}

export function applySeatedLegPose(avatar) {
  const legs = avatar?.userData;
  if (!legs?.leftLeg || !legs?.rightLeg) return;
  legs.leftLeg.rotation.x = SEATED_LEG_PITCH;
  legs.rightLeg.rotation.x = SEATED_LEG_PITCH;
  if (legs.leftKnee) legs.leftKnee.rotation.x = SEATED_KNEE_PITCH;
  if (legs.rightKnee) legs.rightKnee.rotation.x = SEATED_KNEE_PITCH;
}

// Worn on the head when a visitor has never stored a world-status emoji.
const AVATAR_DEFAULT_FACE_EMOJI = "🙂";
// The front of the head is cut off flat at this depth in front of the head's
// centre and the face is a plain disc lying on that cut, so an uploaded avatar
// photo is shown undistorted instead of being wrapped around a curved shell.
const AVATAR_HEAD_RADIUS = 0.45;
const AVATAR_FACE_DEPTH = 0.08;
// Where the cut plane meets the sphere: the flat circle's exact radius, so the
// disc covers the whole cut and no bare head shows around the face.
const AVATAR_FACE_RADIUS = Math.sqrt(
  AVATAR_HEAD_RADIUS * AVATAR_HEAD_RADIUS - AVATAR_FACE_DEPTH * AVATAR_FACE_DEPTH,
);
// Held just off the cut so the disc and the coplanar cap never z-fight.
const AVATAR_FACE_LIFT = 0.002;

// Slices the front off a head sphere: every vertex further forward than the
// cut plane is pulled straight back onto it, which leaves the front as one
// flat circle (avatar fronts face -Z) and the rest of the sphere untouched.
export function avatarHeadGeometry(THREE, widthSegments = 32, heightSegments = 24) {
  return flattenSphereFront(
    new THREE.SphereGeometry(AVATAR_HEAD_RADIUS, widthSegments, heightSegments),
    AVATAR_FACE_DEPTH,
  );
}

function flattenSphereFront(geometry, depth) {
  const position = geometry.attributes?.position;
  if (!position) return geometry;
  for (let i = 0; i < position.count; i += 1) {
    if (position.getZ(i) < -depth) position.setZ(i, -depth);
  }
  position.needsUpdate = true;
  geometry.computeVertexNormals();
  return geometry;
}

// Dominant colour of a flat list of sampled [r, g, b] triples, bucketed at
// 4 bits per channel. Returns "" unless one bucket owns at least `share` of
// the samples: a glyph that barely rendered (or is mostly outline) would
// otherwise hand back a near-black head.
function dominantSampleColor(samples, share) {
  const buckets = new Map();
  const total = samples.length / 3;
  for (let i = 0; i < samples.length; i += 3) {
    const key =
      ((samples[i] >> 4) << 8) |
      ((samples[i + 1] >> 4) << 4) |
      (samples[i + 2] >> 4);
    const bucket = buckets.get(key);
    if (bucket) {
      bucket.r += samples[i];
      bucket.g += samples[i + 1];
      bucket.b += samples[i + 2];
      bucket.count += 1;
    } else {
      buckets.set(key, {
        r: samples[i],
        g: samples[i + 1],
        b: samples[i + 2],
        count: 1,
      });
    }
  }
  let best = null;
  buckets.forEach((bucket) => {
    if (!best || bucket.count > best.count) best = bucket;
  });
  if (!best || !total || best.count / total < share) return "";
  const channel = (sum) =>
    Math.max(0, Math.min(255, Math.round(sum / best.count)))
      .toString(16)
      .padStart(2, "0");
  return `#${channel(best.r)}${channel(best.g)}${channel(best.b)}`;
}

// Most-common opaque colour anywhere in the drawn emoji, used as the second
// choice when the rim itself is too varied to read.
function dominantEmojiColor(context, canvas) {
  const pixels = emojiPixels(context, canvas);
  if (!pixels) return "";
  const samples = [];
  for (let i = 0; i < pixels.length; i += 4) {
    if (pixels[i + 3] < 224) continue;
    samples.push(pixels[i], pixels[i + 1], pixels[i + 2]);
  }
  return dominantSampleColor(samples, 0.25);
}

// Colour of the emoji's own outer rim. The head sphere is painted with it and
// the decal's padding is flooded with it, so the wrapped glyph and the sphere
// behind it meet without a seam or a gap of some other yellow.
function edgeEmojiColor(context, canvas) {
  const pixels = emojiPixels(context, canvas);
  if (!pixels) return "";
  const width = canvas.width;
  const height = canvas.height;
  const centerX = (width - 1) / 2;
  const centerY = (height - 1) / 2;
  const reach = Math.round(Math.max(width, height) / 2);
  // Step back in from the antialiased rim (and any dark outline drawn on it)
  // before sampling, so the head takes the glyph's body colour.
  const inset = Math.max(2, Math.round(Math.min(width, height) * 0.04));
  const rays = 72;
  const samples = [];
  const opaqueAt = (x, y) =>
    x >= 0 &&
    y >= 0 &&
    x < width &&
    y < height &&
    pixels[(y * width + x) * 4 + 3] >= 224;
  for (let ray = 0; ray < rays; ray += 1) {
    const angle = (ray / rays) * Math.PI * 2;
    const stepX = Math.cos(angle);
    const stepY = Math.sin(angle);
    let rim = -1;
    for (let d = reach; d >= 0; d -= 1) {
      const x = Math.round(centerX + stepX * d);
      const y = Math.round(centerY + stepY * d);
      if (opaqueAt(x, y)) {
        rim = d;
        break;
      }
    }
    if (rim < 0) continue;
    const d = Math.max(0, rim - inset);
    const x = Math.round(centerX + stepX * d);
    const y = Math.round(centerY + stepY * d);
    if (!opaqueAt(x, y)) continue;
    const i = (y * width + x) * 4;
    samples.push(pixels[i], pixels[i + 1], pixels[i + 2]);
  }
  // A rim that is half one colour and half another (flags, split glyphs) is
  // not a skin tone; let the whole-glyph sampler answer instead.
  if (samples.length < rays * 3 * 0.5) return "";
  return dominantSampleColor(samples, 0.45);
}

// Mouse-activity antenna: one solid green, blinked hard on and hard off.
const ANTENNA_LIT_COLOR = "#22e06a";
const ANTENNA_DARK_COLOR = "#0d3b22";
const ANTENNA_BLINK_MIN_HZ = 0.9;
const ANTENNA_BLINK_MAX_HZ = 5.4;

// A wave is a short right-arm pose rather than a stored animation: the arm
// lifts out to roughly shoulder-over-head, shakes a couple of times, and
// drops back. It rides on top of whatever the avatar is otherwise doing, so
// visitors can wave while walking, sitting, or riding a swing.
const AVATAR_WAVE_DURATION_MS = 2000;
const AVATAR_WAVE_LIFT = 2.35;
const AVATAR_WAVE_SWEEP = 0.34;
const AVATAR_WAVE_SHAKE_RATE = 0.014;

// A handshake is the two-person greeting. Both avatars reach the same right
// arm out in front of them — forward rather than up, which is what makes the
// pair read as one shared gesture instead of two simultaneous waves — hold
// while the pump rides on top, and lower again.
export const AVATAR_HANDSHAKE_DURATION_MS = 2200;
const AVATAR_HANDSHAKE_REACH = 1.5;
const AVATAR_HANDSHAKE_PUMP = 0.26;
const AVATAR_HANDSHAKE_PUMP_RATE = 0.012;

// The flag-coloured shirt cloth is mapped onto every face of the torso and
// arm boxes. The upward-facing tops are the ones read from across the square,
// so those four UVs are turned a half turn to keep the sash running the same
// way round.
function rotateBoxTopUVs(geometry) {
  const uv = geometry.attributes?.uv;
  if (!uv) return geometry;
  // BoxGeometry lays its faces out +X, -X, +Y, -Y, +Z, -Z with four vertices
  // each, so the top face owns vertices 8 through 11.
  for (let i = 8; i < 12; i += 1) {
    uv.setXY(i, 1 - uv.getX(i), 1 - uv.getY(i));
  }
  uv.needsUpdate = true;
  return geometry;
}

const memberCanvasTexture = (...args) =>
  softwareCanvas(() => canvasArtTexture(...args));

function avatarFaceTexture(THREE, emoji) {
  let color = "";
  // The glyph is drawn smaller than the canvas because the disc inscribes the
  // square: the padding is what fills the corners the circle cuts away, so the
  // face reaches the rim without the glyph itself being clipped.
  const texture = memberCanvasTexture(THREE, 128, 128, (context, canvas) => {
    context.clearRect(0, 0, 128, 128);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '104px system-ui, "Apple Color Emoji", "Segoe UI Emoji"';
    context.fillStyle = "#1d130c";
    context.fillText(emoji, 64, 67);
    color =
      edgeEmojiColor(context, canvas) || dominantEmojiColor(context, canvas);
    // Flood everything the glyph did not cover — the padding and the corners
    // outside a round emoji — with its own rim colour, painted underneath so
    // the face itself is untouched. Nothing of the head shows through, so
    // there is no gap and no seam where the decal ends.
    context.globalCompositeOperation = "destination-over";
    context.fillStyle = color || AVATAR_EMOJI_SKIN_COLOR;
    context.fillRect(0, 0, 128, 128);
    context.globalCompositeOperation = "source-over";
  });
  return { texture, color: color || AVATAR_EMOJI_SKIN_COLOR };
}

function proceduralAvatarFaceTexture(THREE, identityKey) {
  let skin = AVATAR_EMOJI_SKIN_COLOR;
  const texture = memberCanvasTexture(THREE, 128, 128, (context) => {
    skin = paintProceduralAvatarFace(context, identityKey);
  });
  return { texture, color: skin };
}

function syncAvatarFace(THREE, avatar) {
  const face = avatar.userData.faceMesh;
  if (!face?.material) return;
  // An account avatar photo keeps its place through status changes; the
  // deterministic portrait returns when the photo is taken off.
  if (avatar.userData.faceImageUrl) return;
  const statusEmoji = String(avatar.userData.statusEmoji || "");
  const identityKey = String(
    avatar.userData.faceIdentityKey ||
      avatar.userData.id ||
      avatar.userData.name ||
      "forkmesh-visitor",
  );
  const worn = statusEmoji ? `emoji:${statusEmoji}` : `person:${identityKey}`;
  if (avatar.userData.faceEmojiShown === worn) return;
  const drawn = statusEmoji
    ? avatarFaceTexture(THREE, statusEmoji)
    : proceduralAvatarFaceTexture(THREE, identityKey);
  face.material.map?.dispose?.();
  face.material.map = drawn.texture;
  face.material.needsUpdate = true;
  // Same unlit material family as the decal, so the sphere and the emoji
  // print the identical colour under every light in the world.
  const skin = avatar.userData.skin;
  if (skin) {
    skin.color.set(drawn.color);
    skin.needsUpdate = true;
  }
  avatar.userData.faceEmojiShown = worn;
}

// Wearing (or taking off) a consented account avatar photo as the 3D face —
// only an already-public /api/accounts image ever reaches here; presence
// frames carry nothing but the opt-in boolean.
const AVATAR_FACE_IMAGE_RETRIES = 3;
const AVATAR_FACE_IMAGE_RETRY_MS = 12_000;

export function applyAvatarFaceImage(THREE, avatar, url) {
  const face = avatar?.userData?.faceMesh;
  if (!face?.material) return;
  const worn = String(url || "");
  if (avatar.userData.faceImageUrl === worn) return;
  avatar.userData.faceImageUrl = worn;
  if (!worn) {
    // Redraw the emoji face on the next sync.
    avatar.userData.faceEmojiShown = "";
    syncAvatarFace(THREE, avatar);
    return;
  }
  new THREE.TextureLoader().load(
    worn,
    (texture) => {
      if (avatar.userData.faceImageUrl !== worn) {
        texture.dispose();
        return;
      }
      texture.colorSpace = THREE.SRGBColorSpace;
      face.material.map?.dispose?.();
      face.material.map = texture;
      face.material.needsUpdate = true;
      avatar.userData.faceEmojiShown = "";
      avatar.userData.faceImageRetry = null;
    },
    undefined,
    () => {
      if (avatar.userData.faceImageUrl !== worn) return;
      // A corrupt or unavailable public image must never leave a blank disc
      // stuck in the photo state. Restore the generated portrait and allow a
      // later directory refresh to retry a changed URL.
      avatar.userData.faceImageUrl = "";
      avatar.userData.faceEmojiShown = "";
      syncAvatarFace(THREE, avatar);
      const retry =
        avatar.userData.faceImageRetry?.url === worn
          ? avatar.userData.faceImageRetry
          : { url: worn, attempts: 0 };
      if (retry.attempts >= AVATAR_FACE_IMAGE_RETRIES) return;
      retry.attempts += 1;
      avatar.userData.faceImageRetry = retry;
      window.setTimeout(() => {
        if (
          avatar.parent &&
          !avatar.userData.faceImageUrl &&
          avatar.userData.faceImageRetry === retry
        ) {
          applyAvatarFaceImage(THREE, avatar, worn);
        }
      }, AVATAR_FACE_IMAGE_RETRY_MS * retry.attempts);
    },
  );
}

function emailVerificationPinTexture(THREE, verified) {
  return canvasTexture(THREE, 96, 96, (context) => {
    context.clearRect(0, 0, 96, 96);
    context.fillStyle = verified ? "#174b3d" : "#541d25";
    context.strokeStyle = verified ? "#9ef7c6" : "#ff6b72";
    context.lineWidth = 7;
    context.beginPath();
    context.arc(48, 48, 39, 0, Math.PI * 2);
    context.fill();
    context.stroke();
    context.strokeStyle = verified ? "#eafff5" : "#fff0f1";
    context.lineWidth = 10;
    context.lineCap = "round";
    context.beginPath();
    if (verified) {
      context.moveTo(28, 49);
      context.lineTo(42, 63);
      context.lineTo(69, 33);
    } else {
      context.moveTo(31, 31);
      context.lineTo(65, 65);
      context.moveTo(65, 31);
      context.lineTo(31, 65);
    }
    context.stroke();
  });
}

export function syncAvatarVerifiedPin(THREE, avatar, identity) {
  const pin = avatar?.userData?.verifiedPin;
  if (!pin) return;
  const registered =
    String(identity?.accountStatus || "Guest") !== "Guest";
  const verified = identity?.emailVerified === true;
  pin.visible = registered;
  if (!registered || pin.userData.verified === verified) return;
  pin.userData.verified = verified;
  pin.material = avatarPinMaterial(THREE, verified);
}

function syncAvatarStatus(THREE, avatar, identity) {
  if (!avatar?.userData) return;
  const status = normalizeWorldStatus(
    identity?.statusEmoji,
    identity?.statusNote,
  );
  const key = `${status.emoji}\u0000${status.note}`;
  if (avatar.userData.emojiStatusKey === key) return;
  const previous = avatar.userData.emojiStatusSprite;
  if (previous) {
    avatar.remove(previous);
    previous.material?.map?.dispose?.();
    previous.material?.dispose?.();
  }
  avatar.userData.emojiStatusKey = key;
  avatar.userData.statusEmoji = status.emoji;
  avatar.userData.statusNote = status.note;
  // Mood is worn on the face and written on the front card. Nothing floats
  // over the head, so a row of people keeps a clean silhouette.
  avatar.userData.emojiStatusSprite = null;
  syncAvatarFace(THREE, avatar);
}

function avatarBackNameTexture(
  THREE,
  name,
  activity = {},
  work = {},
  plateScale = 1,
) {
  const label = String(name || "visitor").replace(/\s+/g, " ").trim().slice(0, 24);
  const pulls = Math.max(0, Number(activity.pulls) || 0);
  const issues = Math.max(0, Number(activity.issues) || 0);
  const discussions = Math.max(0, Number(activity.discussions) || 0);
  const taskTotal = Math.max(0, Number(work.total) || 0);
  const taskActive = Math.max(0, Number(work.active) || 0);
  return memberCanvasTexture(THREE, 640, 300, (context) => {
    context.clearRect(0, 0, 640, 300);
    context.textAlign = "center";
    context.textBaseline = "middle";
    const size = label.length > 16 ? 40 : label.length > 10 ? 50 : 62;
    context.font = `950 ${size}px "ForkMesh Mono", ui-monospace, monospace`;
    context.lineWidth = 12;
    context.strokeStyle = "rgba(4,16,12,0.96)";
    context.strokeText(label.toUpperCase(), 320, 43);
    context.fillStyle = "#ffffff";
    context.fillText(label.toUpperCase(), 320, 43);
    context.font = '850 25px "ForkMesh Mono", ui-monospace, monospace';
    context.lineWidth = 8;
    const firstCounts = `PRS ${pulls} · ISSUES ${issues}`;
    const discussionCounts = `DISCUSSIONS ${discussions}`;
    context.strokeText(firstCounts, 320, 96);
    context.fillStyle = "#9ef7c6";
    context.fillText(firstCounts, 320, 96);
    context.strokeText(discussionCounts, 320, 136);
    context.fillText(discussionCounts, 320, 136);
    context.font = '850 22px "ForkMesh Mono", ui-monospace, monospace';
    context.strokeText("OPEN FILTERED ACTIVITY ↗", 320, 181);
    context.fillStyle = "#f7c96b";
    context.fillText("OPEN FILTERED ACTIVITY ↗", 320, 181);
    const taskLine = work.state === "ready"
      ? `TASKS ${taskTotal} · RUNNING ${taskActive}`
      : "TASKS · OPEN WORK PANEL";
    context.strokeText(taskLine, 320, 239);
    context.fillStyle = "#77d9ff";
    context.fillText(taskLine, 320, 239);
    context.font = '750 18px "ForkMesh Mono", ui-monospace, monospace';
    context.strokeText("SELECT USER FOR FULL PROFILE", 320, 278);
    context.fillStyle = "#d9f7ff";
    context.fillText("SELECT USER FOR FULL PROFILE", 320, 278);
  }, plateScale);
}

export function syncAvatarBackName(THREE, avatar, identity) {
  const backName = avatar?.userData?.backName;
  if (!backName?.material) return;
  const activity =
    avatar.userData.fediverseProfile?.typeTotals &&
    typeof avatar.userData.fediverseProfile.typeTotals === "object"
      ? avatar.userData.fediverseProfile.typeTotals
      : identity?.contributionTotals || {};
  const work = avatar.userData.selfWorkBoard || {};
  const key = JSON.stringify({
    name: String(identity?.name || "visitor"),
    pulls: Math.max(0, Number(activity.pulls) || 0),
    issues: Math.max(0, Number(activity.issues) || 0),
    discussions: Math.max(0, Number(activity.discussions) || 0),
    taskState: String(work.state || ""),
    taskTotal: Math.max(0, Number(work.total) || 0),
    taskActive: Math.max(0, Number(work.active) || 0),
  });
  if (backName.userData.nameKey === key) return;
  backName.userData.nameKey = key;
  const previous = backName.material.map;
  backName.material.map = avatarBackNameTexture(
    THREE,
    identity?.name,
    activity,
    work,
    avatar.userData.plateScale,
  );
  backName.material.needsUpdate = true;
  previous?.dispose?.();
}

export function makeConsentedProfileFace(THREE, follower) {
  const handle = String(follower?.handle || "public").slice(0, 80);
  const initials = handle
    .replace(/^@/, "")
    .split(/[^a-z0-9]+/i)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0]?.toUpperCase() || "")
    .join("")
    .slice(0, 2) || "•";
  const face = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.24, 18, 14),
    resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      map: canvasArtTexture(THREE, 128, 128, (context) => {
        const hue = hashNumber(handle) % 360;
        context.fillStyle = `hsl(${hue} 48% 34%)`;
        context.fillRect(0, 0, 128, 128);
        context.fillStyle = "#f4fff9";
        context.textAlign = "center";
        context.textBaseline = "middle";
        context.font = '800 52px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(initials, 64, 67);
      }),
      emissive: "#183a32",
      emissiveIntensity: 0.2,
      roughness: 0.7,
    }, "MeshStandardMaterial"),
  );
  const rawAvatar = String(follower?.avatar || "").trim();
  try {
    const avatar = new URL(rawAvatar, window.location.href);
    if (
      (avatar.origin === window.location.origin ||
        avatar.protocol === "data:") &&
      !avatar.username &&
      !avatar.password &&
      rawAvatar.length <= 120_000 &&
      !/\/missing(?:\.[a-z0-9]+)?(?:$|[?#])/i.test(avatar.pathname)
    ) {
      const loader = new THREE.TextureLoader();
      loader.load(
        avatar.href,
        (texture) => {
          texture.colorSpace = THREE.SRGBColorSpace;
          face.material.map = texture;
          face.material.color.set("#ffffff");
          face.material.needsUpdate = true;
        },
        undefined,
        () => {},
      );
    }
  } catch (_) {}
  face.userData.publicProfileHandle = handle;
  return face;
}

function mirrorNodeVisualState(node) {
  const status = String(
    typeof node === "object" ? node?.health || node?.status || "" : "",
  ).toLowerCase();
  if (["online", "healthy", "available"].includes(status)) {
    return { color: "#22e06a", blink: false };
  }
  if (["warning", "degraded", "stale"].includes(status)) {
    return { color: "#ffd23f", blink: true };
  }
  if (["offline", "failed", "error"].includes(status)) {
    return { color: "#ff4d57", blink: true };
  }
  return { color: "#a9b8b2", blink: false };
}

const operatorBlinkMaterials = new Map();

function operatorBlinkMaterial(THREE, color, index) {
  const key = `${color}:${index}`;
  let material = operatorBlinkMaterials.get(key);
  if (!material) {
    material = makeMaterial(THREE, color, {
      emissive: color,
      emissiveIntensity: 1.2,
    });
    material.userData.forkmeshSharedResource = true;
    operatorBlinkMaterials.set(key, material);
  }
  return material;
}

export function syncOperatorBelt(THREE, avatar, nodesOrCount) {
  const previous =
    avatar.userData?.operatorBeltLights?.[0]?.parent ||
    avatar.getObjectByName("forkmesh-operator-belt");
  if (previous) {
    previous.parent?.remove(previous);
    previous.traverse((child) => {
      disposeOwnedGeometry(child.geometry);
      disposeOwnedMaterial(child.material);
    });
  }
  if (avatar.userData) avatar.userData.operatorBeltLights = null;
  const nodes = Array.isArray(nodesOrCount)
    ? nodesOrCount.slice(0, 6)
    : Array.from({
        length: Math.max(0, Math.min(6, Number(nodesOrCount) || 0)),
      }, () => "node");
  const count = nodes.length;
  if (!count) {
    if (avatar.userData) avatar.userData.nodeCount = 0;
    return;
  }
  const beltGroup = new THREE.Group();
  beltGroup.name = "forkmesh-operator-belt";
  const belt = cloneSharedMesh(
    THREE,
    "avatar-operator-belt",
    () => new THREE.BoxGeometry(1.12, 0.18, 0.66),
    makeSharedMaterial(THREE, "#d6a44d", {
      metalness: 0.42,
      roughness: 0.4,
    }),
  );
  belt.position.y = 1.43;
  beltGroup.add(belt);
  for (let index = 0; index < count; index += 1) {
    const visual = mirrorNodeVisualState(nodes[index]);
    const light = cloneSharedMesh(
      THREE,
      "avatar-operator-light",
      () => new THREE.SphereGeometry(0.055, 10, 8),
      visual.blink
        ? operatorBlinkMaterial(THREE, visual.color, index)
        : makeSharedMaterial(THREE, visual.color, {
          emissive: visual.color,
          emissiveIntensity: 1.2,
        }),
    );
    light.name = `forkmesh-mirror-node-${index}`;
    light.userData.mirrorStatusLight = true;
    light.userData.blink = visual.blink;
    light.userData.baseColor = visual.color;
    light.userData.phase = index * 0.91;
    light.position.set(-0.4 + index * 0.16, 1.43, -0.35);
    beltGroup.add(light);
  }
  avatarDetailParent(avatar).add(beltGroup);
  if (avatar.userData) {
    avatar.userData.nodeCount = count;
    // Cached for the per-frame blink pass — a recursive getObjectByName over
    // every avatar subtree each frame was measurably expensive.
    avatar.userData.operatorBeltLights = beltGroup.children.filter(
      (child) => child.userData?.mirrorStatusLight,
    );
  }
}

function createAvatarJetpack(THREE) {
  const group = new THREE.Group();
  group.name = "forkmesh-avatar-jetpack";
  // Avatar fronts face -Z, so positive Z is the centre of the back.
  group.position.set(0, 2.18, 0.43);
  const shell = makeSharedMaterial(THREE, "#293748", {
    metalness: 0.78,
    roughness: 0.24,
  });
  const trim = makeSharedMaterial(THREE, "#79c0ff", {
    metalness: 0.5,
    roughness: 0.2,
    emissive: "#1f6feb",
    emissiveIntensity: 0.45,
  });
  const tanks = [];
  const flames = [];
  [-0.34, 0.34].forEach((x) => {
    const tank = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.2, 0.24, 1.24, 18),
      shell,
    );
    tank.position.set(x, 0, 0);
    group.add(tank);
    tanks.push(tank);
    const cap = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.2, 16, 10),
      trim,
    );
    cap.scale.y = 0.55;
    cap.position.set(x, 0.61, 0);
    group.add(cap);
    const nozzle = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.11, 0.16, 0.24, 14),
      shell,
    );
    nozzle.position.set(x, -0.73, 0);
    group.add(nozzle);
    const flame = new THREE.Mesh(
      resources.primitive(THREE, "ConeGeometry", 0.13, 0.9, 14),
      makeSharedMaterial(THREE, "#ffcf70", {
        emissive: "#ff6a2a",
        emissiveIntensity: 2.2,
        transparent: true,
        opacity: 0.9,
      }),
    );
    flame.rotation.z = Math.PI;
    flame.position.set(x, -1.27, 0);
    group.add(flame);
    flames.push(flame);
  });
  const spine = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.34, 1.02, 0.2),
    trim,
  );
  group.add(spine);
  const hint = makeLabelSprite(
    THREE,
    "JETPACK FLIGHT",
    "WASD MOVE · SPACE UP · C DOWN · SHIFT BOOST",
    "#ffcf70",
  );
  hint.name = "forkmesh-jetpack-control-hint";
  hint.position.set(0, 3.2, 0);
  hint.scale.set(6.4, 2.15, 1);
  group.add(hint);
  group.visible = false;
  group.userData.tanks = tanks;
  group.userData.flames = flames;
  return group;
}

export function createAvatar(THREE, identity, options = {}) {
  const seed = hashNumber(identity.id || identity.name);
  const group = new THREE.Group();
  group.name = `avatar:${identity.id || identity.name}`;
  const scale = options.scale || 1;
  const remote = Boolean(options.remote);
  const highDetail = new THREE.Group();
  highDetail.name = "avatar-full-detail";
  // Avatars represent people, so they keep their complete form and scale at
  // every camera distance. Scene LOD may simplify world props, never users.
  group.add(highDetail);

  // Unlit so the head sphere renders the sampled emoji colour exactly, with
  // no lighting term to pull it off the flat decal wrapped over it.
  const skin = resources.uniqueMaterial(THREE, {
    color: AVATAR_EMOJI_SKIN_COLOR,
  }, "MeshBasicMaterial");
  const shirt = makeMaterial(THREE, "#ffffff", {
    roughness: 0.8,
  });
  applyOutfit(THREE, shirt, identity);
  shirt.needsUpdate = true;
  // A crowd of directory figures is read from across a clearing, not from a
  // profile page, so their chest card and name board can be painted at a
  // fraction of the bytes a live peer's plates get. See canvasTextureScaleFor.
  const plateScale = Number(options.plateScale) > 0
    ? Math.min(1, Number(options.plateScale))
    : 1;
  // Given a crowd batch, the legs, shoes and antenna become transform-only
  // nodes whose world matrices are copied into shared instanced draws. Without
  // one — a bare createAvatar in a test, or a scene built before the batch —
  // they fall back to their own meshes, so an avatar is never legless.
  const instances = options.instances || null;
  const instancedParts = instances
    ? { leg: [], shoe: [], antennaStalk: [], antennaBulb: [] }
    : null;
  const instancedPart = (kind) => {
    const node = new THREE.Object3D();
    node.name = `avatar-instanced-${kind}`;
    instancedParts[kind].push(node);
    return node;
  };
  const dark = instancedParts
    ? null
    : makeMaterial(THREE, "#101d19", { roughness: 0.85 });
  const shoe = instancedParts
    ? null
    : makeMaterial(THREE, "#07100e", { roughness: 0.82 });

  const torso = cloneSharedMesh(
    THREE,
    "avatar-torso",
    () => rotateBoxTopUVs(new THREE.BoxGeometry(1.1, 1.5, 0.62)),
    shirt,
  );
  torso.position.y = 2.15;
  highDetail.add(torso);

  // Rounded everywhere except the front, which is cut off flat to carry the
  // face disc: the two share AVATAR_FACE_DEPTH so the cut and the disc are the
  // same circle.
  const headRig = new THREE.Group();
  headRig.name = "avatar-head-look-rig";
  headRig.position.y = 3.36;
  highDetail.add(headRig);
  const head = cloneSharedMesh(
    THREE,
    "avatar-head",
    () => avatarHeadGeometry(THREE),
    shirt,
  );
  // Left unscaled: an egg-shaped head would stretch the flat cut into an
  // ellipse and put the avatar photo back out of proportion.
  headRig.add(head);

  // The face wears the last world-status emoji the visitor set (default
  // smile), or the visitor's avatar photo. It is a flat disc filling the cut
  // front of the head, so the picture is shown square-on and unwarped rather
  // than wrapped around a curve; syncAvatarFace keeps its texture current.
  const faceMesh = cloneSharedMesh(
    THREE,
    "avatar-face",
    () => new THREE.CircleGeometry(AVATAR_FACE_RADIUS, 48),
    // The canvas is flooded opaque, so the decal renders in the solid pass and
    // never sorts against the head it is lying on.
    resources.uniqueMaterial(THREE, {}, "MeshBasicMaterial"),
  );
  // Avatar fronts face -Z, so the disc is turned to look out of the cut.
  faceMesh.position.z = -(AVATAR_FACE_DEPTH + AVATAR_FACE_LIFT);
  faceMesh.rotation.y = Math.PI;
  headRig.add(faceMesh);

  const activityRing = cloneSharedMesh(
    THREE,
    "avatar-activity-ring",
    () => new THREE.RingGeometry(
      AVATAR_FACE_RADIUS - 0.05,
      AVATAR_FACE_RADIUS - 0.012,
      40,
    ),
    avatarRingMaterial(THREE, "#8da09a", -1),
  );
  activityRing.name = "avatar-activity-ring";
  activityRing.position.z = -(AVATAR_FACE_DEPTH + AVATAR_FACE_LIFT * 2);
  activityRing.rotation.y = Math.PI;
  activityRing.renderOrder = 6;
  headRig.add(activityRing);

  const leftArm = cloneSharedMesh(
    THREE,
    "avatar-limb",
    () => rotateBoxTopUVs(new THREE.BoxGeometry(0.29, AVATAR_ARM_LENGTH, 0.32)),
    shirt,
  );
  leftArm.position.set(-AVATAR_ARM_X, AVATAR_ARM_Y, 0);
  highDetail.add(leftArm);
  const rightArm = leftArm.clone();
  rightArm.position.x = AVATAR_ARM_X;
  highDetail.add(rightArm);

  const jetpack = remote ? null : createAvatarJetpack(THREE);
  if (jetpack) highDetail.add(jetpack);

  // Mouse activity reads as a small antenna riding on the visitor's shoulder
  // that blinks solid green, faster the more their mouse is moving.
  const antenna = new THREE.Group();
  antenna.name = "mouse-activity-antenna";
  const antennaStalk = instancedParts
    ? instancedPart("antennaStalk")
    : new THREE.Mesh(
      avatarInstancedPartGeometry.antennaStalk(THREE),
      resources.uniqueMaterial(THREE, { color: ANTENNA_STALK_COLOR }, "MeshBasicMaterial"),
    );
  antennaStalk.position.y = 0.2;
  antenna.add(antennaStalk);
  const antennaBulb = instancedParts
    ? instancedPart("antennaBulb")
    : new THREE.Mesh(
      avatarInstancedPartGeometry.antennaBulb(THREE),
      resources.uniqueMaterial(THREE, { color: ANTENNA_LIT_COLOR }, "MeshBasicMaterial"),
    );
  antennaBulb.userData.partColor = ANTENNA_LIT_COLOR;
  antennaBulb.position.y = 0.42;
  antenna.add(antennaBulb);
  // Avatar fronts face -Z; perched on the right shoulder (top corner of the
  // torso, just inboard of the arm) and tipped outward away from the head.
  antenna.position.set(0.52, 2.86, 0.06);
  antenna.rotation.x = 0.1;
  antenna.rotation.z = -0.35;
  antenna.visible = identity.inputActive === true;
  highDetail.add(antenna);

  // Each leg is a hip pivot carrying a thigh, and a knee pivot carrying the
  // shin plus that leg's shoe. Standing (every pitch at zero) the two segments
  // stack into the same block the single-box leg used to be, but the joints let
  // a sitter fold the thigh forward and keep the shin and foot under the knee
  // instead of swinging one rigid block — and shoes now travel with the leg
  // they belong to rather than staying planted on the plank.
  const legSegment = () =>
    (instancedParts
      ? instancedPart("leg")
      : cloneSharedMesh(
        THREE,
        "avatar-leg-segment",
        () => avatarInstancedPartGeometry.leg(THREE),
        dark,
      ));
  const buildLeg = (side) => {
    const hip = new THREE.Group();
    hip.position.set(side * 0.3, AVATAR_HIP_Y, 0);
    const thigh = legSegment();
    thigh.position.y = -AVATAR_LEG_SEGMENT / 2;
    hip.add(thigh);
    const knee = new THREE.Group();
    knee.position.y = -AVATAR_LEG_SEGMENT;
    const shin = legSegment();
    shin.position.y = -AVATAR_LEG_SEGMENT / 2;
    knee.add(shin);
    const foot = instancedParts
      ? instancedPart("shoe")
      : cloneSharedMesh(
        THREE,
        "avatar-shoe",
        () => avatarInstancedPartGeometry.shoe(THREE),
        shoe,
      );
    foot.position.set(0, AVATAR_SHOE_Y - AVATAR_HIP_Y + AVATAR_LEG_SEGMENT, -0.09);
    knee.add(foot);
    hip.add(knee);
    highDetail.add(hip);
    return { hip, knee };
  };
  const leftLegRig = buildLeg(-1);
  const rightLegRig = buildLeg(1);
  const leftLeg = leftLegRig.hip;
  const rightLeg = rightLegRig.hip;
  // Instanced legs live in a batch nobody owns, so the lower body would stop
  // being clickable and the selection box would shrink to the torso. One
  // invisible proxy over the leg region keeps both — it is never drawn, and the
  // raycastProxy flag is what tells the hit tests and the Elements panel that
  // an object hidden on purpose is still a legitimate target.
  if (instancedParts) {
    const legProxy = cloneSharedMesh(
      THREE,
      "avatar-leg-hit-proxy",
      () => new THREE.BoxGeometry(1.02, AVATAR_HIP_Y, 0.56),
      avatarHitProxyMaterial(THREE),
    );
    legProxy.name = "avatar-leg-hit-proxy";
    legProxy.position.y = AVATAR_HIP_Y / 2;
    legProxy.visible = false;
    legProxy.userData.raycastProxy = true;
    highDetail.add(legProxy);
  }

  const badge = cloneSharedPlane(
    THREE,
    0.88,
    0.88,
    resources.uniqueMaterial(THREE, {
      map: badgeTexture(
        THREE,
        { ...identity, walletEditable: remote !== true },
        remote ? "#77d9ff" : "#9ef7c6",
        { state: "loading" },
        plateScale,
      ),
      transparent: false,
      // The card is a document, not a lit surface. Tone mapping ran its
      // painted contrast through ACES and the live daylight exposure, which
      // pulled white type off white and dimmed every row after dusk; the name
      // board on the same avatar's back has always opted out for this reason.
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  badge.scale.set(1, 1.12, 1);
  // The card is worn on the shirt, so it hangs off the torso rather than off
  // the avatar root: the look-direction lean turns the torso about its own
  // centre, and a card pinned to the root had the swinging front corner of
  // the outfit cut across it. Local to the 1.1 x 1.5 x 0.62 torso box at
  // y=2.15, which puts the card at the same chest height as before and
  // clears its -0.31 front face by a hair.
  badge.position.set(0, 0.15, -0.33);
  badge.rotation.y = Math.PI;
  badge.userData.chestBadge = true;
  torso.add(badge);

  const backName = cloneSharedPlane(
    THREE,
    1.72,
    0.806,
    resources.uniqueMaterial(THREE, {
      transparent: true,
      depthWrite: false,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  backName.name = "avatar-back-username";
  backName.scale.setScalar(0.628);
  backName.position.set(0, 0.47, 0.33);
  backName.renderOrder = 5;
  torso.add(backName);

  const verifiedPin = cloneSharedMesh(
    THREE,
    "avatar-verified-pin",
    () => new THREE.CircleGeometry(0.105, 24),
    avatarPinMaterial(THREE, identity.emailVerified === true),
  );
  verifiedPin.name = "forkmesh-verified-email-pin";
  verifiedPin.position.set(-0.66, 2.92, -0.22);
  verifiedPin.rotation.y = Math.PI;
  verifiedPin.renderOrder = 5;
  verifiedPin.userData.verified = identity.emailVerified === true;
  highDetail.add(verifiedPin);

  group.scale.setScalar(scale);
  group.userData = {
    id: identity.id,
    name: identity.name,
    leftArm,
    rightArm,
    leftLeg,
    rightLeg,
    leftKnee: leftLegRig.knee,
    rightKnee: rightLegRig.knee,
    badge,
    badgeIdentity: identity,
    badgeRemote: remote,
    // "far" until this card is drawn large enough to be read. See
    // updateAvatarBadgeDetail.
    badgeDetail: "far",
    badgeFarPixels: badge.material.map.image.height,
    chestTabs: null,
    fediverseProfile: null,
    shirt,
    shirtMeshes: [torso, leftArm, rightArm, head],
    head,
    skin,
    antenna,
    antennaBulb,
    activityLight: null,
    activityRing,
    activityBucket: "",
    walletChip: null,
    walletKey: "",
    inputEnergy: 0,
    phase: (seed % 100) / 10,
    targetPosition: new THREE.Vector3(),
    targetHeading: 0,
    status: identity.status || "exploring",
    nodeCount: 0,
    accountStatus: identity.accountStatus || "Guest",
    inputActive: identity.inputActive === true,
    statusEmoji: "",
    statusNote: "",
    emojiStatusKey: "",
    emojiStatusSprite: null,
    faceMesh,
    headRig,
    torso,
    faceEmojiShown: "",
    faceIdentityKey: identity.id || identity.name || "",
    backName,
    verifiedPin,
    jetpack,
    highDetail,
    instancedParts,
    plateScale,
  };
  syncAvatarAchievementBadges(THREE, group, identity);
  syncOperatorBelt(THREE, group, identity.nodes || []);
  syncAvatarBackName(THREE, group, identity);
  syncAvatarStatus(THREE, group, identity);
  syncAvatarActivity(THREE, group, identity);
  syncAvatarWallet(THREE, group, identity);
  syncAvatarVerifiedPin(THREE, group, identity);
  setShadows(group, true, true);
  // Shadows on the instanced parts are a property of their batch, not of these
  // transform nodes, so the crowd batch is joined after setShadows has run.
  instances?.register(group);
  return group;
}

export function applyAvatarLookDirection(avatar, yaw, pitch, turnBody = true) {
  if (!avatar?.userData) return;
  if (turnBody && Number.isFinite(yaw)) avatar.rotation.y = yaw;
  // Keep the body driven by movement. Camera orbit is instead expressed by
  // the head rig, which lets a visitor look back at their own avatar while
  // peers still see where they are looking.
  const headYaw = Number.isFinite(yaw)
    ? clamp(
        Math.atan2(
          Math.sin(yaw - avatar.rotation.y),
          Math.cos(yaw - avatar.rotation.y),
        ),
        -1.25,
        1.25,
      )
    : 0;
  // Three.js camera pitch and the avatar's front-facing rig use opposite X
  // rotation signs. Invert at this boundary so looking up raises the avatar's
  // face and looking down lowers it.
  const lookPitch = clamp(-(Number(pitch) || 0), -0.62, 0.62);
  if (avatar.userData.headRig) {
    avatar.userData.headRig.rotation.order = "YXZ";
    avatar.userData.headRig.rotation.y = headYaw;
    avatar.userData.headRig.rotation.x = lookPitch;
  }
  if (avatar.userData.torso) {
    avatar.userData.torso.rotation.y = headYaw * 0.08;
    avatar.userData.torso.rotation.x = lookPitch * 0.12;
  }
}

/**
 * Squares one avatar's head and torso back onto its body. A visitor's own
 * avatar is held here (see rotateCamera) so the portrait, chest card and name
 * board they are reading stay straight on; peers render the same visitor
 * through applyAvatarLookDirection and still see the angles.
 */
export function resetAvatarLookDirection(avatar) {
  if (!avatar?.userData) return;
  avatar.userData.headRig?.rotation.set(0, 0, 0);
  avatar.userData.torso?.rotation.set(0, 0, 0);
}

const WORK_BADGE_ROWS = 6;

export function normalizeSelfWorkBoard(board = {}) {
  const state = ["locked", "loading", "ready"].includes(board?.state)
    ? board.state
    : "locked";
  const items = Array.isArray(board?.items) ? board.items : [];
  return {
    state,
    message: String(board?.message || "").replace(/\s+/g, " ").trim().slice(0, 64),
    tracked: String(board?.tracked || "").trim().slice(0, 16),
    items: items.slice(0, WORK_BADGE_ROWS).map((item) => ({
      kind: item?.kind === "issue" ? "issue" : "task",
      title: String(item?.title || "").replace(/\s+/g, " ").trim().slice(0, 64),
      status: ["active", "done"].includes(item?.status)
        ? item.status
        : "idle",
      elapsed: String(item?.elapsed || "").trim().slice(0, 12),
      checkin: String(item?.checkin || "").trim().slice(0, 24),
    })).filter((item) => item.title),
    total: Math.max(0, Math.min(999, Number(board?.total) || 0)),
    active: Math.max(0, Math.min(999, Number(board?.active) || 0)),
  };
}

function avatarWorkBadgeTexture(THREE, board = {}) {
  const snapshot = normalizeSelfWorkBoard(board);
  const clip = (context, text, width) => {
    let value = String(text || "");
    while (value.length > 1 && context.measureText(value).width > width) {
      value = value.slice(0, -1);
    }
    return value === String(text || "") ? value : value.slice(0, -1) + "\u2026";
  };
  return canvasTexture(THREE, 768, 960, (context) => {
    context.fillStyle = "#071714";
    context.fillRect(0, 0, 768, 960);
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 16;
    context.strokeRect(10, 10, 748, 940);
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#9ef7c6";
    context.font = '900 56px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("MY WORK", 44, 78);
    context.fillStyle = "#7fb9a5";
    context.font = '800 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("PRIVATE \u00b7 SELF ONLY", 44, 132);

    if (snapshot.state !== "ready") {
      context.fillStyle = snapshot.state === "loading" ? "#9ef7c6" : "#f7c96b";
      context.font = '900 42px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        snapshot.state === "loading" ? "SYNCING\u2026" : "SIGN IN",
        44,
        260,
      );
      context.fillStyle = "#e9fff6";
      context.font = '650 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        clip(context, snapshot.message || "No assigned work loaded.", 660),
        44,
        320,
      );
      context.fillStyle = "#7fb9a5";
      context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("HIDDEN FROM PEERS + SCREENSHOTS", 44, 912);
      return;
    }

    // Stats first: the plate is read at a glance while walking past a mirror,
    // so the counters stay above the per-task rows.
    context.fillStyle = "#f7c96b";
    context.font = '900 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("ASSIGNED", 44, 214);
    context.fillText("RUNNING", 300, 214);
    context.fillText("TRACKED", 540, 214);
    context.fillStyle = "#ffffff";
    context.font = '900 52px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(snapshot.total), 44, 274);
    context.fillText(String(snapshot.active), 300, 274);
    context.font = '900 40px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(snapshot.tracked || "0:00", 540, 274);

    context.strokeStyle = "rgba(158,247,198,0.3)";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(44, 322);
    context.lineTo(724, 322);
    context.stroke();

    if (!snapshot.items.length) {
      context.fillStyle = "#e9fff6";
      context.font = '650 31px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NOTHING ASSIGNED TO YOU YET", 44, 392);
    }
    snapshot.items.forEach((item, index) => {
      const top = 386 + index * 92;
      context.fillStyle =
        item.kind === "issue"
          ? "#77d9ff"
          : item.status === "active"
            ? "#1f9c6a"
            : item.status === "done"
              ? "#9ef7c6"
            : "#3a4b45";
      context.beginPath();
      context.arc(60, top, 13, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = "#ffffff";
      context.font = '700 32px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(clip(context, item.title, 500), 92, top - 12);
      context.fillStyle =
        item.kind === "issue"
          ? "#77d9ff"
          : item.status === "active"
            ? "#9ef7c6"
            : item.status === "done"
              ? "#9ef7c6"
            : "#7fb9a5";
      context.font = '650 26px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        clip(
          context,
          item.kind === "issue"
            ? `ISSUE${item.checkin ? ` \u00b7 ${item.checkin}` : ""}`
            : `${
                item.status === "active"
                  ? "RUNNING"
                  : item.status === "done"
                    ? "DONE"
                    : "TO DO"
              } \u00b7 ${
                item.elapsed || "0:00"
              }${item.checkin ? ` \u00b7 ${item.checkin}` : ""}`,
          580,
        ),
        92,
        top + 24,
      );
    });

    context.fillStyle = "#7fb9a5";
    context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("HIDDEN FROM PEERS + SCREENSHOTS", 44, 912);
  });
}

export function setAvatarWorkBadge(THREE, avatar, board, visible = true) {
  if (!avatar?.userData) return null;
  const snapshot = normalizeSelfWorkBoard(board);
  avatar.userData.selfWorkBoard = snapshot;
  syncAvatarBackName(
    THREE,
    avatar,
    avatar.userData.badgeIdentity || {},
  );
  let badge = avatar.userData.selfWorkBadge;
  if (snapshot.state === "locked" && !snapshot.items.length) {
    if (badge) badge.visible = false;
    return badge || null;
  }
  if (!badge) {
    badge = cloneSharedPlane(
      THREE,
      1.06,
      1.32,
      resources.uniqueMaterial(THREE, { toneMapped: false }, "MeshBasicMaterial"),
    );
    badge.name = "forkmesh-self-work-back-badge";
    badge.scale.setScalar(0.63);
    badge.position.set(0, -0.2, 0.33);
    badge.renderOrder = 4;
    (avatar.userData.torso || avatarDetailParent(avatar)).add(badge);
    avatar.userData.selfWorkBadge = badge;
  }
  const previous = badge.material.map;
  badge.material.map = avatarWorkBadgeTexture(THREE, snapshot);
  badge.material.needsUpdate = true;
  badge.visible = visible === true;
  previous?.dispose?.();
  return badge;
}

function applyOutfit(THREE, shirt, identity) {
  // Every public field the tailor reads is folded into one key so a presence
  // frame that changes none of them never redraws the cloth.
  const key = [
    identity.name,
    identity.countryCode,
    identity.flag,
    identity.outfitColor,
    identity.outfitStyle,
  ]
    .map((value) => String(value || ""))
    .join("\u0000");
  if (shirt.userData.outfitKey === key && shirt.map) return;
  shirt.userData.outfitKey = key;
  const previous = shirt.map;
  shirt.map = countryShirtTexture(THREE, identity);
  shirt.color.set("#ffffff");
  shirt.needsUpdate = true;
  if (previous !== shirt.map) previous?.dispose?.();
}

export function syncCountryShirt(THREE, avatar, identity) {
  const shirt = avatar?.userData?.shirt;
  if (!shirt) return;
  applyOutfit(THREE, shirt, identity);
}

const AVATAR_RING_PHASE_SLOTS = 4;
const avatarRingMaterialCaches = new WeakMap();
function avatarRingMaterial(THREE, color, phaseSlot) {
  let cache = avatarRingMaterialCaches.get(THREE);
  if (!cache) {
    cache = new Map();
    avatarRingMaterialCaches.set(THREE, cache);
  }
  const key = `${color}:${phaseSlot}`;
  let material = cache.get(key);
  if (!material) {
    material = resources.uniqueMaterial(THREE, {
      color,
      transparent: true,
      opacity: 0.95,
      depthWrite: false,
      toneMapped: false,
      side: THREE.DoubleSide,
      forceSinglePass: true,
    }, "MeshBasicMaterial");
    material.userData.forkmeshSharedResource = true;
    material.userData.ringPhase =
      phaseSlot >= 0
        ? (phaseSlot / AVATAR_RING_PHASE_SLOTS) * Math.PI * 2
        : null;
    cache.set(key, material);
  }
  return material;
}

const avatarPinMaterialCaches = new WeakMap();
function avatarPinMaterial(THREE, verified) {
  let cache = avatarPinMaterialCaches.get(THREE);
  if (!cache) {
    cache = new Map();
    avatarPinMaterialCaches.set(THREE, cache);
  }
  let material = cache.get(verified);
  if (!material) {
    material = resources.uniqueMaterial(THREE, {
      map: emailVerificationPinTexture(THREE, verified),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial");
    material.userData.forkmeshSharedResource = true;
    cache.set(verified, material);
  }
  return material;
}

export function syncAvatarActivity(THREE, avatar, identity) {
  if (!avatar?.userData) return;
  const active = identity.inputActive === true;
  avatar.userData.inputActive = active;
  avatar.userData.accountStatus = identity.accountStatus || "Guest";
  if (avatar.userData.antenna) avatar.userData.antenna.visible = active;
  avatar.userData.activityBucket = ACTIVITY_LIGHT_COLORS[
    identity.activityBucket
  ]
    ? String(identity.activityBucket)
    : "";
  const ring = avatar.userData.activityRing;
  if (ring) {
    const color =
      String(identity.accountStatus || "Guest") !== "Guest" &&
      ACTIVITY_LIGHT_COLORS[identity.activityBucket]
        ? ACTIVITY_LIGHT_COLORS[identity.activityBucket]
        : "#8da09a";
    const phaseSlot = avatar.userData.activityBucket
      ? Math.floor(
        ((Number(avatar.userData.phase) || 0) / 10) * AVATAR_RING_PHASE_SLOTS,
      ) % AVATAR_RING_PHASE_SLOTS
      : -1;
    ring.material = avatarRingMaterial(THREE, color, phaseSlot);
  }
}

export function renderAvatarBadge(THREE, avatar, remote = false) {
  const badge = avatar?.userData?.badge;
  if (!badge?.material) return;
  const identity = avatar.userData.badgeIdentity || {};
  const accent = remote ? "#77d9ff" : "#9ef7c6";
  const old = badge.material.map;
  badge.material.map = badgeTexture(
    THREE,
    {
      ...identity,
      walletEditable: remote !== true,
    },
    accent,
    avatar.userData.fediverseProfile || { state: "loading" },
    avatar.userData.plateScale,
    avatar.userData.badgeDetail,
  );
  badge.material.needsUpdate = true;
  // The height the memory budget gives this card when it is not being read,
  // which is the size its text starts being magnified past. Read off the
  // canvas rather than recomputed, so the two can never disagree.
  if (avatar.userData.badgeDetail !== "reading") {
    avatar.userData.badgeFarPixels = badge.material.map.image.height;
  }
  old?.dispose?.();
}

/**
 * Move one card between the reading and distant resolutions. The paint path
 * is the ordinary badge repaint, so the card keeps whatever identity, profile
 * and wallet state it was already showing; only its backing store changes.
 */
export function setAvatarBadgeDetail(THREE, avatar, detail) {
  const next = detail === "reading" ? "reading" : "far";
  if (!avatar?.userData?.badge?.material) return false;
  if (avatar.userData.badgeDetail === next) return false;
  avatar.userData.badgeDetail = next;
  renderAvatarBadge(THREE, avatar, avatar.userData.badgeRemote === true);
  return true;
}

export function updateAvatarBadge(THREE, avatar, identity, remote = false) {
  if (!avatar?.userData?.badge?.material) return;
  avatar.userData.badgeIdentity = identity;
  avatar.userData.badgeRemote = remote === true;
  syncAvatarAchievementBadges(THREE, avatar, identity);
  renderAvatarBadge(THREE, avatar, remote);
  avatar.userData.name = identity.name;
  syncAvatarBackName(THREE, avatar, identity);
  const faceIdentityKey = identity.id || identity.name || "";
  if (avatar.userData.faceIdentityKey !== faceIdentityKey) {
    avatar.userData.faceIdentityKey = faceIdentityKey;
    avatar.userData.faceEmojiShown = "";
    syncAvatarFace(THREE, avatar);
  }
  syncCountryShirt(THREE, avatar, identity);
  // Taking the perk off reverts to the emoji face immediately. An identity
  // that simply never carried the flag — a persisted contributor row, an
  // office attendee — keeps whatever public photo it was dressed with.
  if (identity.faceImage === false && avatar.userData.faceImageUrl) {
    applyAvatarFaceImage(THREE, avatar, "");
  }
  syncAvatarActivity(THREE, avatar, identity);
  syncAvatarWallet(THREE, avatar, identity);
  syncAvatarStatus(THREE, avatar, identity);
  syncAvatarVerifiedPin(THREE, avatar, identity);
  syncOperatorBelt(THREE, avatar, identity.nodes || []);
}

// Starts (or restarts) the wave on one avatar. The pose itself is played by
// animateAvatarActivity, which every walk/sit/ride path already calls after it
// has written the arm rotations for the frame.
export function startAvatarWave(avatar, startedAt = performance.now()) {
  if (!avatar?.userData?.rightArm) return false;
  // The wave and the handshake pose the same arm, so the newer gesture takes
  // it over cleanly instead of the two fighting over the shoulder each frame.
  if (avatar.userData.handshakeStartedAt) {
    avatar.userData.handshakeStartedAt = 0;
    avatar.userData.rightArm.rotation.x = 0;
  }
  avatar.userData.waveStartedAt = startedAt;
  return true;
}

// Starts (or restarts) the handshake pose on one avatar. Like the wave it
// rides on top of whatever the avatar is otherwise doing, so two visitors can
// shake hands while walking, sitting, or riding a swing.
export function startAvatarHandshake(avatar, startedAt = performance.now()) {
  if (!avatar?.userData?.rightArm) return false;
  if (avatar.userData.waveStartedAt) {
    avatar.userData.waveStartedAt = 0;
    avatar.userData.rightArm.rotation.z = 0;
  }
  avatar.userData.handshakeStartedAt = startedAt;
  return true;
}

export function startAvatarHudAction(avatar, action, startedAt = performance.now()) {
  if (!avatar || !action) return false;
  const previous = avatar.userData.hudAction;
  if (previous) {
    avatar.position.y = previous.baseY;
    avatar.rotation.set(
      previous.baseRotation.x,
      previous.baseRotation.y,
      previous.baseRotation.z,
    );
    avatar.scale.copy(previous.baseScale);
  }
  avatar.userData.hudAction = {
    action,
    startedAt,
    baseY: avatar.position.y,
    baseRotation: avatar.rotation.clone(),
    baseScale: avatar.scale.clone(),
  };
  return true;
}

// Rotating the arm box around its own centre would pull the shoulder end out
// of the torso, so the mesh is nudged along the arc that keeps the shoulder
// pinned where it hangs at rest.
function poseWavingArm(arm, rest, angle) {
  const halfLength = (arm.geometry?.parameters?.height || 1.25) / 2;
  arm.rotation.x = 0;
  arm.rotation.z = angle;
  arm.position.set(
    rest.x + halfLength * Math.sin(angle),
    rest.y + halfLength * (1 - Math.cos(angle)),
    rest.z,
  );
}

// The same shoulder-pinning arc as the wave, swung forward instead of out.
// Avatar fronts face -Z, so a positive angle reaches the hand out ahead of
// the body, where the other half of the handshake is standing.
function poseHandshakingArm(arm, rest, angle) {
  const halfLength = (arm.geometry?.parameters?.height || 1.25) / 2;
  arm.rotation.z = 0;
  arm.rotation.x = angle;
  arm.position.set(
    rest.x,
    rest.y + halfLength * (1 - Math.cos(angle)),
    rest.z - halfLength * Math.sin(angle),
  );
}

function poseReachingArm(arm, side, pitch, sink) {
  if (!arm) return;
  const halfLength = AVATAR_ARM_LENGTH / 2;
  arm.rotation.x = pitch;
  arm.rotation.z = 0;
  arm.position.set(
    side * AVATAR_ARM_X,
    AVATAR_ARM_Y + halfLength * (1 - Math.cos(pitch)),
    sink - halfLength * Math.sin(pitch),
  );
}

export function poseAvatarArms(avatar, leftPitch, rightPitch, sink = 0) {
  const data = avatar?.userData;
  if (!data) return;
  poseReachingArm(data.leftArm, -1, leftPitch, sink);
  poseReachingArm(data.rightArm, 1, rightPitch, sink);
}

export function animateAvatarActivity(avatar, time, delta, reducedMotion) {
  if (!avatar?.userData) return;
  const activityRing = avatar.userData.activityRing;
  if (activityRing?.material) {
    const ringPhase = activityRing.material.userData.ringPhase;
    activityRing.material.opacity = typeof ringPhase === "number"
      ? reducedMotion
        ? 0.95
        : 0.72 + Math.sin(time * 0.004 + ringPhase) * 0.23
      : 0.58;
  }
  const beltLights = avatar.userData.operatorBeltLights;
  if (beltLights) {
    for (const child of beltLights) {
      if (!child.userData.blink) continue;
      const lit =
        reducedMotion ||
        Math.sin(time * 0.008 + child.userData.phase) >= 0;
      child.material.opacity = lit ? 1 : 0.18;
      child.material.transparent = true;
      child.material.emissiveIntensity = lit ? 1.8 : 0.12;
    }
  }
  const hudAction = avatar.userData.hudAction;
  if (hudAction) {
    const duration = reducedMotion ? 360 : 1800;
    const progress = (time - hudAction.startedAt) / duration;
    avatar.position.y = hudAction.baseY;
    avatar.rotation.copy(hudAction.baseRotation);
    avatar.scale.copy(hudAction.baseScale);
    if (!(progress >= 0) || progress >= 1) {
      delete avatar.userData.hudAction;
    } else if (!reducedMotion) {
      const cycle = Math.sin(progress * Math.PI);
      if (hudAction.action === "jump") {
        avatar.position.y += Math.abs(Math.sin(progress * Math.PI * 4)) * 0.9;
      } else if (hudAction.action === "superjump") {
        avatar.position.y += Math.sin(progress * Math.PI) * 8;
      } else if (hudAction.action === "spin") {
        avatar.rotation.y += progress * Math.PI * 4;
      } else if (hudAction.action === "backflip") {
        avatar.position.y += cycle * 0.8;
        avatar.rotation.x -= progress * Math.PI * 2;
      } else if (hudAction.action === "dance") {
        avatar.rotation.z = Math.sin(progress * Math.PI * 8) * 0.22 * cycle;
        avatar.rotation.y += Math.sin(progress * Math.PI * 4) * 0.45;
      } else if (hudAction.action === "float") {
        avatar.position.y += cycle * 1.8;
        avatar.rotation.y += progress * Math.PI;
      } else if (hudAction.action === "wobble") {
        avatar.rotation.z = Math.sin(progress * Math.PI * 12) * 0.32 * cycle;
        avatar.scale.copy(hudAction.baseScale).multiplyScalar(
          1 + Math.sin(progress * Math.PI * 8) * 0.14 * cycle,
        );
      } else if (hudAction.action === "sparkle") {
        avatar.position.y += Math.abs(Math.sin(progress * Math.PI * 6)) * 0.35;
        avatar.rotation.y += progress * Math.PI * 6;
        avatar.scale.copy(hudAction.baseScale).multiplyScalar(1 + cycle * 0.22);
      }
    }
  }
  const waveStartedAt = avatar.userData.waveStartedAt || 0;
  const waveArm = waveStartedAt ? avatar.userData.rightArm : null;
  if (waveArm) {
    // The rest pose is captured on the first wave: nothing else ever moves an
    // arm's position, only its rotation.
    const rest =
      avatar.userData.rightArmRest ||
      (avatar.userData.rightArmRest = waveArm.position.clone());
    const elapsed = time - waveStartedAt;
    const progress = elapsed / AVATAR_WAVE_DURATION_MS;
    if (!(progress >= 0) || progress >= 1) {
      avatar.userData.waveStartedAt = 0;
      waveArm.rotation.z = 0;
      waveArm.position.copy(rest);
    } else {
      // One half sine lifts the arm and lowers it again; the shake rides on
      // top of the lift so it fades in and out with the raise.
      const lift = Math.sin(Math.PI * progress);
      const shake = reducedMotion
        ? 0
        : Math.sin(elapsed * AVATAR_WAVE_SHAKE_RATE) * AVATAR_WAVE_SWEEP;
      poseWavingArm(waveArm, rest, lift * (AVATAR_WAVE_LIFT + shake));
    }
  }
  const handshakeStartedAt = avatar.userData.handshakeStartedAt || 0;
  const handshakeArm = handshakeStartedAt ? avatar.userData.rightArm : null;
  if (handshakeArm) {
    const rest =
      avatar.userData.rightArmRest ||
      (avatar.userData.rightArmRest = handshakeArm.position.clone());
    // A gesture that started between two frames is younger than the frame
    // timestamp animating it, so the age is clamped rather than treated as a
    // finished pose: a throttled tab must not swallow the handshake outright.
    const elapsed = Math.max(0, time - handshakeStartedAt);
    const progress = elapsed / AVATAR_HANDSHAKE_DURATION_MS;
    if (!(progress >= 0) || progress >= 1) {
      avatar.userData.handshakeStartedAt = 0;
      handshakeArm.rotation.x = 0;
      handshakeArm.position.copy(rest);
    } else {
      // The reach saturates early and stays out for most of the gesture, so
      // the two hands meet and hold rather than passing each other mid-swing.
      const reach = Math.min(1, Math.sin(Math.PI * progress) * 1.8);
      const pump = reducedMotion
        ? 0
        : Math.sin(elapsed * AVATAR_HANDSHAKE_PUMP_RATE) *
          AVATAR_HANDSHAKE_PUMP;
      poseHandshakingArm(
        handshakeArm,
        rest,
        reach * (AVATAR_HANDSHAKE_REACH + pump),
      );
    }
  }
  const antenna = avatar.userData.antenna;
  const bulb = avatar.userData.antennaBulb;
  if (antenna?.visible && bulb) {
    // A square blink rather than a pulse, so it reads as an unambiguous
    // on/off beacon. Idle mouse activity ticks slowly; a mouse that is
    // really moving drives it up to a few blinks a second.
    const energy = Math.max(0, Math.min(1, avatar.userData.inputEnergy || 0));
    const hz = ANTENNA_BLINK_MIN_HZ +
      energy * (ANTENNA_BLINK_MAX_HZ - ANTENNA_BLINK_MIN_HZ);
    const cycle = (time * 0.001 * hz + avatar.userData.phase) % 1;
    const lit = reducedMotion || cycle < 0.5;
    const glow = lit ? ANTENNA_LIT_COLOR : ANTENNA_DARK_COLOR;
    // The bulb is normally one instance of a crowd batch, so its blink rides in
    // the per-instance colour; a fallback bulb still paints its own material.
    bulb.userData.partColor = glow;
    bulb.material?.color?.set(glow);
  }
  // Presence is presence: inactivity changes the antenna signal and activity
  // label, never the visibility of the person. Guests and members remain fully
  // opaque until they actually leave the World.
}
