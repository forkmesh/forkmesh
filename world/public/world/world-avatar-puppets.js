import { RIG_REACTIONS, RIG_TEMPLATE, decodeRig, rigTemplatePoints } from "/call-avatar-rig-wire.js";
import { createFacePuppet } from "/call-avatar-puppet.js";
import { speakerState } from "/call-speech-meter.js";
import { paintProceduralAvatarFace } from "./world-avatar-face.js";

const FACE_SIZE = 128;
const FACE_PAINT_MS = 66;
const FACE_LOST_MS = 1200;
const SPEECH_TICK_MS = 500;

const PORTRAIT_EYE_Y = 0.42;
const PORTRAIT_FACE_WIDTH = 0.62;

export const WORLD_REACTIONS = RIG_REACTIONS;

export function portraitFaceAnchors() {
  const xs = RIG_TEMPLATE.map(([x]) => x);
  const scale = PORTRAIT_FACE_WIDTH / (Math.max(...xs) - Math.min(...xs));
  return RIG_TEMPLATE.map(([x, y]) => [
    Math.max(0, Math.min(1, 0.5 + x * scale)),
    Math.max(0, Math.min(1, PORTRAIT_EYE_Y + y * scale)),
  ]);
}

export function reactionEmoji(kind) {
  return RIG_REACTIONS[kind]?.emoji || "";
}

function makeCanvas(documentRef) {
  const canvas = documentRef.createElement("canvas");
  canvas.width = FACE_SIZE;
  canvas.height = FACE_SIZE;
  return canvas;
}

export function createAvatarPuppets({
  world,
  documentRef = globalThis.document,
  now = () => performance.now(),
  ownPhoto = () => null,
  onSelfFace = () => {},
} = {}) {
  const peers = new Map();
  const fallbackAnchors = portraitFaceAnchors();
  let lastSpeechTick = 0;

  function entry(id) {
    let peer = peers.get(id);
    if (!peer) {
      peer = {
        id,
        rig: null,
        rigAt: 0,
        anchors: null,
        canvas: null,
        puppet: null,
        paintedAt: 0,
        dirty: false,
        faceShown: false,
        speech: { words: 0, talking: false, lastSpokeAt: 0, inVoice: false },
        speechKey: "",
      };
      peers.set(id, peer);
    }
    return peer;
  }

  function scene() {
    return typeof world === "function" ? world() : world;
  }

  function apply(id, rig) {
    const peer = entry(id);
    const at = now();
    if (rig.anchors) peer.anchors = rig.anchors;
    peer.rig = rig;
    peer.rigAt = at;
    peer.dirty = true;
    const tracked = Boolean(rig.deltas);
    scene()?.setAvatarRig?.(id, tracked || rig.hands?.length
      ? { yaw: rig.yaw, pitch: rig.pitch, roll: rig.roll, hands: rig.hands || [], tracked }
      : null);
    noteSpeech(id, { words: rig.words, talking: rig.talking });
  }

  function noteSpeech(id, { words = null, talking = null, inVoice = null } = {}) {
    const peer = entry(id);
    if (Number.isFinite(Number(words)) && words !== null) peer.speech.words = Math.max(peer.speech.words, Math.floor(Number(words)));
    if (talking !== null) {
      peer.speech.talking = Boolean(talking);
      if (talking) peer.speech.lastSpokeAt = Date.now();
    }
    if (inVoice !== null) peer.speech.inVoice = Boolean(inVoice);
    syncSpeech(peer);
  }

  function syncSpeech(peer) {
    const visible = peer.speech.inVoice || peer.speech.words > 0 || peer.speech.lastSpokeAt > 0;
    const state = speakerState(peer.speech);
    const key = visible ? `${peer.speech.words}:${state}` : "";
    if (key === peer.speechKey) return;
    peer.speechKey = key;
    scene()?.setAvatarSpeech?.(peer.id, visible ? { words: peer.speech.words, state } : null);
  }

  function paint(peer) {
    const rig = peer.rig;
    const lost = !rig?.deltas || now() - peer.rigAt > FACE_LOST_MS;
    const self = peer.id === "self";
    if (lost) {
      if (peer.faceShown) {
        scene()?.setAvatarFaceCanvas?.(peer.id, null);
        peer.faceShown = false;
        if (self) onSelfFace(null);
      }
      return;
    }
    peer.canvas ||= makeCanvas(documentRef);
    const context = peer.canvas.getContext("2d");
    const photo = scene()?.avatarFacePhoto?.(peer.id) || (self ? ownPhoto() : null) || null;
    let drawn = false;
    if (photo) {
      peer.puppet ||= createFacePuppet();
      context.clearRect(0, 0, FACE_SIZE, FACE_SIZE);
      try {
        drawn = peer.puppet.render(context, { image: photo, anchors: peer.anchors || fallbackAnchors, deltas: rig.deltas, width: FACE_SIZE, height: FACE_SIZE });
      } catch (_) {
        drawn = false;
      }
    }
    if (!drawn) {
      const points = rigTemplatePoints(rig);
      context.setTransform(1, 0, 0, 1, 0, 0);
      paintProceduralAvatarFace(context, scene()?.avatarFaceKey?.(peer.id) || peer.id, {
        lidLeft: rig.lidLeft,
        lidRight: rig.lidRight,
        gazeX: rig.gazeX,
        gazeY: rig.gazeY,
        browLeft: rig.browLeft,
        browRight: rig.browRight,
        mouth: rig.mouth,
        smile: rig.smile,
        round: rig.round,
        tongue: rig.tongue,
        lips: points.slice(48, 68),
      });
    }
    scene()?.setAvatarFaceCanvas?.(peer.id, peer.canvas);
    peer.faceShown = true;
    if (self) onSelfFace(peer.canvas);
  }

  return {
    emojiFor: reactionEmoji,
    receive(message) {
      const rig = decodeRig(message);
      if (!rig || !message?.id) return false;
      apply(String(message.id), rig);
      return true;
    },
    local(id, rig) {
      if (!rig) return;
      apply(String(id || "self"), rig);
    },
    speech: noteSpeech,
    presence(id, player) {
      if (!player) return;
      noteSpeech(String(id), { words: Number(player.words) || 0, inVoice: Boolean(player.voice) });
    },
    tick() {
      const at = now();
      for (const peer of peers.values()) {
        if (peer.dirty && at - peer.paintedAt >= FACE_PAINT_MS) {
          peer.dirty = false;
          peer.paintedAt = at;
          paint(peer);
        } else if (peer.faceShown && at - peer.rigAt > FACE_LOST_MS) {
          paint(peer);
        }
      }
      if (at - lastSpeechTick >= SPEECH_TICK_MS) {
        lastSpeechTick = at;
        for (const peer of peers.values()) {
          if (peer.speech.talking && at - peer.rigAt > 1500) peer.speech.talking = false;
          syncSpeech(peer);
        }
      }
    },
    currentSpeaker(exclude = "") {
      let best = null;
      for (const peer of peers.values()) {
        if (peer.id === exclude || peer.id === "self" || !peer.speech.lastSpokeAt) continue;
        if (Date.now() - peer.speech.lastSpokeAt > 15000) continue;
        if (!best || peer.speech.lastSpokeAt > best.speech.lastSpokeAt) best = peer;
      }
      return best?.id || "";
    },
    speakers() {
      return [...peers.values()].map((peer) => ({ id: peer.id, ...peer.speech, state: speakerState(peer.speech) }));
    },
    forget(id) {
      const peer = peers.get(String(id));
      if (!peer) return;
      peers.delete(String(id));
      scene()?.setAvatarRig?.(peer.id, null);
      scene()?.setAvatarFaceCanvas?.(peer.id, null);
      scene()?.setAvatarSpeech?.(peer.id, null);
    },
  };
}
