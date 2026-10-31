export const WORLD_VOICE_ROOM_RE = /^(?:world|channel|space:[a-z0-9]+(?:-[a-z0-9]+){0,9}|adhoc:[0-9a-f]{16}|bubble:[0-9a-f]{16})$/;
export const WORLD_VOICE_NAME_MAX = 24;

export function worldVoiceRoomLabel(room, players = null) {
  const id = String(room?.id || room || "");
  if (id === "world") return "#general · whole World";
  if (id === "channel") return "Private channel call";
  if (id.startsWith("space:")) return `#world-${id.slice(6)}`;
  if (id.startsWith("bubble:")) return "Privacy bubble";
  if (id.startsWith("adhoc:")) {
    const name = String(room?.name || "").trim();
    if (name) return name;
    for (const player of players?.values?.() || []) {
      if (player.voice === id && player.voiceName) return player.voiceName;
    }
    return "Voice room";
  }
  return "Voice";
}

export function cleanVoiceRoomName(value) {
  const name = String(value || "").normalize("NFKC").replace(/\s+/g, " ").trim().slice(0, WORLD_VOICE_NAME_MAX).trim();
  return /^[\p{L}\p{N}][\p{L}\p{M}\p{N} '’-]*$/u.test(name) ? name : "";
}

export function randomHex(bytes, scope = globalThis) {
  const raw = new Uint8Array(bytes);
  scope.crypto.getRandomValues(raw);
  return [...raw].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export async function bubbleTag(secret, scope = globalThis) {
  const digest = await scope.crypto.subtle.digest("SHA-256", new TextEncoder().encode(`forkmesh-world-bubble-v1\n${secret}`));
  return [...new Uint8Array(digest)].slice(0, 8).map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export function voiceRoomsFromPlayers(players, self = null) {
  const rooms = new Map();
  const add = (id, name, member) => {
    if (!WORLD_VOICE_ROOM_RE.test(id) || id === "channel") return;
    let room = rooms.get(id);
    if (!room) {
      room = { id, name: "", members: [] };
      rooms.set(id, room);
    }
    if (!room.name && name) room.name = name;
    room.members.push(member);
  };
  for (const [id, player] of players || []) {
    if (!player?.voice) continue;
    add(String(player.voice), String(player.voiceName || ""), { id, name: player.name || "Someone", mic: Boolean(player.mic), cam: Boolean(player.cam), words: Number(player.words) || 0 });
  }
  if (self?.room) add(self.room.id, self.room.name || "", { id: "self", name: self.name || "You", mic: Boolean(self.mic), cam: Boolean(self.cam), words: Number(self.words) || 0, self: true });
  return [...rooms.values()];
}

export function bubbleGroups(players, self = null) {
  const groups = new Map();
  for (const [id, player] of players || []) {
    const voice = String(player?.voice || "");
    if (!voice.startsWith("bubble:")) continue;
    if (!groups.has(voice)) groups.set(voice, []);
    groups.get(voice).push(String(id));
  }
  if (self?.room?.id?.startsWith("bubble:")) {
    if (!groups.has(self.room.id)) groups.set(self.room.id, []);
    groups.get(self.room.id).push("self");
  }
  return [...groups.entries()].map(([tag, members]) => ({ tag, members }));
}
