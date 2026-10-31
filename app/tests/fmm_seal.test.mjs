import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createFrameSealer, createFrameOpener, CLEAR_AUDIO, CLEAR_VIDEO } from "../public/fmm/fmm-seal.js";
import { b64uDecode, parseKeyMessage } from "../public/chat-call-e2ee.js";

const vectors = JSON.parse(readFileSync(new URL("../../server/rtc/e2ee/testdata/go-vectors.json", import.meta.url), "utf8"));
const hex = (text) => new Uint8Array(Buffer.from(text, "hex"));
const toHex = (bytes) => Buffer.from(bytes).toString("hex");

async function senderKey() {
  const vk = vectors.keys[vectors.media.key];
  const m = await parseKeyMessage(vk.message);
  return {
    call: vectors.call,
    peer: m.peer,
    epoch: m.epoch,
    keyId: Number(vk.keyId),
    raw: b64uDecode(vk.frameKey),
    auth: m.auth,
    authSeed: b64uDecode(vk.authSeed),
  };
}

test("the sealer produces Go's FrameSender bytes and auth message", async () => {
  const key = await senderKey();
  const sealer = createFrameSealer();
  await sealer.setKey(key);
  const frames = vectors.frames.filter((f) => f.key === vectors.keys[vectors.media.key].frameKey);
  assert.ok(frames.length >= 3);
  for (const f of frames) {
    const sealed = await sealer.seal(hex(f.plain), f.clear);
    assert.equal(toHex(sealed), f.sealed);
  }
  const auth = await sealer.flush();
  assert.equal(auth.length, 1);
  assert.equal(toHex(auth[0]), vectors.media.auth[0]);
});

test("the opener decrypts Go frames, verifies them and refuses replays and forgeries", async () => {
  const key = await senderKey();
  const t = vectors.now;
  const opener = createFrameOpener({ now: () => t });
  await opener.setKey({ ...key, authSeed: undefined });
  const frames = vectors.frames.filter((f) => f.key === vectors.keys[vectors.media.key].frameKey);
  for (const f of frames) {
    const { plain, drop } = await opener.open(key.peer, hex(f.sealed), f.clear);
    assert.equal(drop, "");
    assert.equal(toHex(plain), f.plain);
  }
  const again = await opener.open(key.peer, hex(frames[0].sealed), frames[0].clear);
  assert.equal(again.drop, "replay");
  assert.equal(await opener.accept(key.peer, hex(vectors.media.auth[0])), "");
  assert.equal(opener.status(key.peer).state, "verified");
  const forged = await opener.open(key.peer, hex(vectors.media.forged), 1);
  assert.notEqual(forged.drop, "");
  const unknown = await opener.open("f".repeat(32), hex(frames[0].sealed), frames[0].clear);
  assert.equal(unknown.drop, "no-key");
});

test("a new key restarts the counter, the same key keeps it, and both ends agree on the clear bytes", async () => {
  const key = await senderKey();
  const sealer = createFrameSealer();
  await sealer.setKey(key);
  await sealer.seal(Uint8Array.of(1, 2, 3), CLEAR_AUDIO);
  await sealer.setKey(key);
  assert.equal(sealer.counter, 1n);
  const opener = createFrameOpener();
  await opener.setKey(key);
  const video = await sealer.seal(new Uint8Array(500).fill(7), CLEAR_VIDEO);
  assert.equal(video.length, 500 + 16 + 9);
  const opened = await opener.open(key.peer, video, CLEAR_VIDEO);
  assert.equal(opened.drop, "");
  assert.equal(opened.plain.length, 500);
  const wrongClear = await opener.open(key.peer, await sealer.seal(new Uint8Array(40).fill(9), CLEAR_VIDEO), CLEAR_AUDIO);
  assert.equal(wrongClear.drop, "tag");
});
