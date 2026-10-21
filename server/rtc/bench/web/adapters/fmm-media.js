import { createGroupCalls } from "/public/chat-group-calls.js";
import { signerFromSeed, keyId } from "/public/chat-call-e2ee.js";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function normalize(sample) {
  const out = { inbound: [], outbound: [], transport: {} };
  if (!sample) return out;
  for (const entry of sample.inbound || []) {
    const item = { ...entry };
    delete item.statuses;
    delete item.buffer;
    out.inbound.push(item);
  }
  for (const entry of sample.outbound || []) out.outbound.push({ ...entry });
  out.transport = { ...(sample.transport || {}) };
  if (out.transport.currentRoundTripTime !== undefined) out.transport.rtt = out.transport.currentRoundTripTime;
  out.engine = sample.engine || null;
  out.counters = sample.counters || null;
  out.codec = sample.codec || "";
  return out;
}

export async function join({ name, log, identify }) {
  let stopped = false;
  const signer = await signerFromSeed(crypto.getRandomValues(new Uint8Array(32)));
  const kid = await keyId(signer.idk);
  const account = "bench-" + name;
  const calls = createGroupCalls({
    sendFrame: async (channelKey, type, extra) => {
      await fetch("/engine/relay", {
        method: "POST",
        body: JSON.stringify({ type, sender: name, sealKid: kid, sealAcct: account, ...extra }),
      });
      return true;
    },
    fetchTicket: async (channelId, body) => {
      await identify(name);
      await fetch("/engine/expect?name=" + encodeURIComponent(name), { method: "POST" });
      return (await fetch("/engine/ticket", { method: "POST", body: JSON.stringify(body || {}) })).json();
    },
    accountName: () => name,
    workerUrl: "/public/chat-group-call-crypto.js",
    callSigner: async () => signer,
    callAccount: () => account,
    callId: () => "bench-call",
    fmm: {
      workerUrl: "/public/fmm/fmm-worker.js",
      workletUrl: "/public/fmm/fmm-playout-worklet.js",
      faceStreamUrl: "/public/chat-call-facestream.js",
      onEvent: (event) => {
        if (event.type === "state" || event.type === "error" || event.type === "log") log(JSON.stringify(event));
      },
    },
  });
  (async () => {
    let since = 0;
    while (!stopped) {
      try {
        const batch = await (await fetch("/engine/relay?since=" + since)).json();
        for (const frame of batch.frames || []) {
          if (frame.sender !== name) calls.handleFrame(frame, "#bench");
        }
        since = batch.next;
      } catch (_) {}
      await sleep(100);
    }
  })();
  const joined = await calls.join("#bench", "b".repeat(32), "video");
  if (joined.error) throw new Error(joined.error);
  return {
    self: () => calls.active?.peer || "",
    state: () => calls.active?.state || "left",
    remote: () => [...(calls.active?.tiles.values() || [])].map((tile) => ({ peer: tile.peer, stream: tile.stream })),
    stats: async () => normalize(await calls.active?.fmm?.stats()),
    leave: () => {
      stopped = true;
      calls.leave();
    },
  };
}
