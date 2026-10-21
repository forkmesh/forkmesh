import { createGroupCalls } from "/public/chat-group-calls.js";
import { normalize } from "/web/adapters/webrtc-stats.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function announceUfrag(identify) {
  const original = RTCPeerConnection.prototype.setLocalDescription;
  const told = new WeakSet();
  RTCPeerConnection.prototype.setLocalDescription = async function (...args) {
    const result = await original.apply(this, args);
    if (!told.has(this)) {
      const ufrag = /a=ice-ufrag:(\S+)/.exec(this.localDescription?.sdp || "")?.[1];
      if (ufrag) {
        told.add(this);
        await identify(ufrag);
      }
    }
    return result;
  };
}

export async function join({ name, identify }) {
  announceUfrag(identify);
  let stopped = false;
  const calls = createGroupCalls({
    sendFrame: async (channelKey, type, extra) => {
      await fetch("/engine/relay", { method: "POST", body: JSON.stringify({ type, sender: name, ...extra }) });
      return true;
    },
    fetchTicket: async () => (await fetch("/engine/ticket")).json(),
    accountName: () => name,
    workerUrl: "/public/chat-group-call-crypto.js",
  });
  (async () => {
    let since = 0;
    while (!stopped) {
      try {
        const batch = await (await fetch("/engine/relay?since=" + since)).json();
        for (const frame of batch.frames || []) calls.handleFrame(frame, "#bench");
        since = batch.next;
      } catch (_) {}
      await sleep(150);
    }
  })();
  const joined = await calls.join("#bench", "b".repeat(32), "video");
  if (joined.error) throw new Error(joined.error);
  return {
    self: () => calls.active?.peer || "",
    state: () => calls.active?.state || "left",
    remote: () => [...(calls.active?.tiles.values() || [])].map((tile) => ({ peer: tile.peer, stream: tile.stream })),
    stats: async () => {
      const active = calls.active;
      if (!active?.pc) return normalize(null, new Map());
      const owners = new Map();
      for (const tile of active.tiles.values()) {
        for (const track of tile.stream.getTracks()) owners.set(track.id, tile.peer);
      }
      return normalize(await active.pc.getStats(), owners);
    },
    leave: () => {
      stopped = true;
      calls.leave();
    },
  };
}
