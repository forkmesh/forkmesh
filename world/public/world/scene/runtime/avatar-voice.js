import {
  animateAvatarRig,
  animateWordBadge,
  createPrivacyBubble,
  placePrivacyBubble,
  reactionTexture,
  setAvatarFaceSource,
  setAvatarRigState,
  syncAvatarWordBadge,
} from "../avatar-rig.js";
import * as resources from "../resources.js";

const RIG_FORGET_MS = 6000;
const REACTION_FLIGHT_MS = 1300;
const REACTION_POP_MS = 900;

export function functions({ THREE, identity, ctx }) {
  const rigged = new Map();
  const speech = new Map();
  const bubbles = new Map();
  const flights = [];
  const scratch = new THREE.Vector3();

  function avatarFor(peerId) {
    const id = String(peerId || "");
    if (!id) return null;
    if (id === identity.id || id === "self") return ctx.player || null;
    return ctx.remotePlayers.get(id) || null;
  }

  function setAvatarRig(peerId, rig) {
    const avatar = avatarFor(peerId);
    if (!avatar) return false;
    const now = performance.now();
    setAvatarRigState(avatar, rig, now);
    if (rig) rigged.set(String(peerId), { avatar, at: now });
    else rigged.delete(String(peerId));
    return true;
  }

  function setAvatarFaceCanvas(peerId, canvas) {
    const avatar = avatarFor(peerId);
    if (!avatar) return false;
    setAvatarFaceSource(THREE, avatar, canvas || null);
    return true;
  }

  function setAvatarSpeech(peerId, state) {
    const avatar = avatarFor(peerId);
    const id = String(peerId || "");
    if (!state) speech.delete(id);
    else speech.set(id, state);
    if (!avatar) return false;
    syncAvatarWordBadge(THREE, avatar, state || null);
    return true;
  }

  function setPrivacyBubbles(groups) {
    const wanted = new Map();
    for (const group of Array.isArray(groups) ? groups : []) {
      const tag = String(group?.tag || "");
      if (!tag) continue;
      wanted.set(tag, (group.members || []).map(String));
    }
    for (const [tag, bubble] of bubbles) {
      if (wanted.has(tag)) continue;
      ctx.world.remove(bubble.mesh);
      bubble.mesh.material.dispose();
      bubbles.delete(tag);
    }
    for (const [tag, members] of wanted) {
      let bubble = bubbles.get(tag);
      if (!bubble) {
        bubble = { mesh: createPrivacyBubble(THREE, tag), members };
        ctx.world.add(bubble.mesh);
        bubbles.set(tag, bubble);
      }
      bubble.members = members;
    }
  }

  function headPosition(avatar, target) {
    avatar.getWorldPosition(target);
    target.y += 4.4 * (avatar.scale?.y || 1);
    return target;
  }

  function playAvatarReaction(fromId, toId, emoji) {
    const from = avatarFor(fromId);
    const to = avatarFor(toId);
    if (!from && !to) return false;
    const sprite = new THREE.Sprite(resources.uniqueMaterial(THREE, {
      map: reactionTexture(THREE, String(emoji || "👍")),
      transparent: true,
      depthTest: false,
      depthWrite: false,
    }, "SpriteMaterial"));
    sprite.renderOrder = 11;
    sprite.scale.set(0.9, 0.9, 1);
    ctx.world.add(sprite);
    flights.push({ sprite, from, to, startedAt: performance.now() });
    return true;
  }

  function updateFlights(time) {
    for (let index = flights.length - 1; index >= 0; index -= 1) {
      const flight = flights[index];
      const elapsed = time - flight.startedAt;
      const travel = flight.from && flight.to ? REACTION_FLIGHT_MS : 0;
      const start = flight.from ? headPosition(flight.from, scratch).clone() : null;
      const end = flight.to ? headPosition(flight.to, new THREE.Vector3()) : start;
      if (elapsed < travel && start && end) {
        const t = elapsed / travel;
        flight.sprite.position.lerpVectors(start, end, t);
        flight.sprite.position.y += Math.sin(t * Math.PI) * 2.2;
        flight.sprite.scale.setScalar(0.7 + t * 0.4);
        flight.sprite.material.opacity = 1;
      } else {
        const pop = (elapsed - travel) / REACTION_POP_MS;
        flight.sprite.position.copy(end);
        flight.sprite.position.y += 0.6 + pop * 1.4;
        flight.sprite.scale.setScalar(1.1 + Math.sin(Math.min(1, pop) * Math.PI) * 0.6);
        flight.sprite.material.opacity = Math.max(0, 1 - Math.max(0, pop - 0.4) / 0.6);
        if (pop >= 1) {
          ctx.world.remove(flight.sprite);
          flight.sprite.material.map?.dispose?.();
          flight.sprite.material.dispose();
          flights.splice(index, 1);
        }
      }
    }
  }

  function updateBubbles(time) {
    for (const bubble of bubbles.values()) {
      const positions = [];
      for (const id of bubble.members) {
        const avatar = avatarFor(id);
        if (avatar?.visible !== false && avatar?.parent) positions.push(avatar.getWorldPosition(new THREE.Vector3()));
      }
      bubble.mesh.visible = placePrivacyBubble(bubble.mesh, positions, time);
    }
  }

  function updateAvatarVoice(delta, time) {
    const now = performance.now();
    for (const [id, entry] of rigged) {
      const avatar = avatarFor(id) || entry.avatar;
      if (!avatar?.userData?.voiceRig) {
        rigged.delete(id);
        continue;
      }
      if (now - (avatar.userData.voiceRig.receivedAt || 0) > RIG_FORGET_MS) {
        setAvatarRigState(avatar, null, now);
        rigged.delete(id);
        continue;
      }
      animateAvatarRig(THREE, avatar, now, delta * 1000);
    }
    for (const [id, state] of speech) {
      const avatar = avatarFor(id);
      if (avatar && !avatar.userData.wordBadge) syncAvatarWordBadge(THREE, avatar, state);
      animateWordBadge(avatar?.userData?.wordBadge, now);
    }
    updateBubbles(time);
    updateFlights(time);
  }

  function avatarFacePhoto(peerId) {
    const data = avatarFor(peerId)?.userData;
    if (!data?.faceImageUrl) return null;
    const map = data.rigFaceTexture ? data.rigSavedFaceMap : data.faceMesh?.material?.map;
    const image = map?.image || null;
    return image && (image.naturalWidth || image.width) ? image : null;
  }

  function avatarFaceKey(peerId) {
    const avatar = avatarFor(peerId);
    const data = avatar?.userData;
    return String(data?.faceIdentityKey || data?.id || data?.name || peerId || "forkmesh-visitor");
  }

  function voiceAvatarIds() {
    return [...rigged.keys()];
  }

  function avatarNear(peerId) {
    const avatar = avatarFor(peerId);
    if (!avatar || !ctx.player) return Infinity;
    return avatar.position.distanceTo(ctx.player.position);
  }

  return {
    setAvatarRig,
    setAvatarFaceCanvas,
    setAvatarSpeech,
    setPrivacyBubbles,
    playAvatarReaction,
    updateAvatarVoice,
    voiceAvatarIds,
    avatarNear,
    avatarFacePhoto,
    avatarFaceKey,
  };
}
