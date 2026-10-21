import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as wire from "../public/fmm/wire.js";
import * as seqnum from "../public/fmm/seqnum.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/wire/testdata/js-parity.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));
const text = (h) => new TextDecoder().decode(unhex(h));
const utf8 = (s) => hex(new TextEncoder().encode(s));

function errorOf(f) {
  try {
    f();
  } catch (err) {
    assert.ok(err instanceof wire.FmmError, `not an FmmError: ${err}`);
    return err.message;
  }
  return "";
}

function msgFrom(o) {
  const s = (k) => (o[k] === undefined ? undefined : text(o[k]));
  switch (o.type) {
    case wire.MsgJoin: return new wire.Join({ ticket: s("ticket") });
    case wire.MsgJoined: return new wire.Joined({ peer: s("peer") });
    case wire.MsgPublish: return new wire.Publish({ stream: o.stream, kind: o.kind, layers: o.layers, name: s("name"), track: o.track, rank: o.rank });
    case wire.MsgUnpublish: return new wire.Unpublish({ stream: o.stream });
    case wire.MsgTrackAdded: return new wire.TrackAdded({ stream: o.stream, kind: o.kind, layers: o.layers, peer: s("peer"), name: s("name") });
    case wire.MsgTrackRemoved: return new wire.TrackRemoved({ stream: o.stream });
    case wire.MsgSubscribe: return new wire.Subscribe({ stream: o.stream, maxLayer: o.maxLayer });
    case wire.MsgUnsubscribe: return new wire.Unsubscribe({ stream: o.stream });
    case wire.MsgKeyframeRequest: return new wire.KeyframeRequest({ stream: o.stream });
    case wire.MsgReport: return new wire.Report({ base: o.base, refTime: o.refTime, delay: o.delay, received: o.received, arrival: o.arrival });
    case wire.MsgBitrateHint: return new wire.BitrateHint({ stream: o.stream, bitrate: o.bitrate });
    case wire.MsgLeave: return new wire.Leave({ reason: s("reason") });
    case wire.MsgError: return new wire.Error({ code: o.code, text: s("text") });
    case wire.MsgNack: return new wire.Nack({ stream: o.stream, seqs: o.seqs });
    case wire.MsgAck: return new wire.Ack({ stream: o.stream, frames: o.frames });
    case wire.MsgRecovery: return new wire.Recovery({ stream: o.stream, frame: o.frame });
  }
  throw new Error("unknown type " + o.type);
}

function msgJSON(m) {
  const o = { type: m.type() };
  if (m instanceof wire.Join) o.ticket = utf8(m.ticket);
  else if (m instanceof wire.Joined) o.peer = utf8(m.peer);
  else if (m instanceof wire.Publish) Object.assign(o, { stream: m.stream, kind: m.kind, layers: m.layers, track: m.track, rank: m.rank, name: utf8(m.name) });
  else if (m instanceof wire.TrackAdded) Object.assign(o, { stream: m.stream, kind: m.kind, layers: m.layers, peer: utf8(m.peer), name: utf8(m.name) });
  else if (m instanceof wire.Subscribe) Object.assign(o, { stream: m.stream, maxLayer: m.maxLayer });
  else if (m instanceof wire.Report) Object.assign(o, { base: m.base, refTime: m.refTime, delay: m.delay, received: m.received, arrival: m.arrival });
  else if (m instanceof wire.BitrateHint) Object.assign(o, { stream: m.stream, bitrate: m.bitrate });
  else if (m instanceof wire.Leave) o.reason = utf8(m.reason);
  else if (m instanceof wire.ErrorMessage) Object.assign(o, { code: m.code, text: utf8(m.text) });
  else if (m instanceof wire.Nack) Object.assign(o, { stream: m.stream, seqs: m.seqs });
  else if (m instanceof wire.Ack) Object.assign(o, { stream: m.stream, frames: m.frames });
  else if (m instanceof wire.Recovery) Object.assign(o, { stream: m.stream, frame: m.frame });
  else o.stream = m.stream;
  return o;
}

function sameMsg(got, want, label) {
  if (want.lossy) {
    const w = { ...want };
    const g = { ...got };
    delete w.lossy;
    for (const k of ["ticket", "peer", "name", "reason", "text"]) {
      delete w[k];
      delete g[k];
    }
    assert.deepEqual(g, w, label);
    return;
  }
  assert.deepEqual(got, want, label);
}

function mediaJSON(m) {
  return {
    stream: m.stream, transportSeq: m.transportSeq, seq: m.seq, frameID: m.frameID, fragIndex: m.fragIndex,
    fragCount: m.fragCount, keyframe: m.keyframe, fec: m.fec, recovery: m.recovery, longTerm: m.longTerm,
    face: m.face, layer: m.layer, refDelta: m.refDelta, timestamp: m.timestamp, payload: hex(m.payload || []),
  };
}

function mediaFrom(o) {
  return new wire.Media({ ...o, payload: unhex(o.payload) });
}

function repairJSON(r) {
  return {
    stream: r.stream, transportSeq: r.transportSeq, baseSeq: r.baseSeq, scheme: r.scheme, k: r.k, m: r.m,
    index: r.index, offsets: hex(r.offsets || []), symbol: hex(r.symbol || []),
  };
}

function repairFrom(o) {
  return new wire.Repair({ ...o, offsets: unhex(o.offsets), symbol: unhex(o.symbol) });
}

function decodeJSON(b) {
  const o = { bytes: hex(b) };
  try {
    const [m, n] = wire.decode(b);
    o.msg = msgJSON(m);
    o.n = n;
  } catch (err) {
    assert.ok(err instanceof wire.FmmError);
    o.error = err.message;
  }
  return o;
}

function sameDecode(got, want, label) {
  assert.equal(got.bytes, want.bytes, label);
  assert.equal(got.error, want.error, `${label} error`);
  assert.equal(got.n, want.n, `${label} n`);
  if (want.msg) sameMsg(got.msg, want.msg, label);
}

test("seqnum unwrapping matches Go", () => {
  for (const c of fixture.seqnum.unwrap16) {
    const u = new seqnum.Unwrapper16();
    assert.equal(u.last(), null);
    assert.deepEqual(c.inputs.map((v) => u.unwrap(v)), c.outputs);
    assert.equal(u.last(), c.last);
  }
  for (const c of fixture.seqnum.unwrap32) {
    const u = new seqnum.Unwrapper32();
    assert.deepEqual(c.inputs.map((v) => u.unwrap(v)), c.outputs);
  }
  for (const c of fixture.seqnum.near) {
    assert.equal(seqnum.near16(c.v16, c.ref), c.near16);
    assert.equal(seqnum.near32(c.v32, c.ref), c.near32);
  }
});

test("media datagrams encode, validate and parse like Go", () => {
  for (const [i, c] of fixture.media.entries()) {
    const m = mediaFrom(c.in);
    assert.equal(hex(m.append(null)), c.bytes, `media ${i}`);
    assert.equal(errorOf(() => m.validate()), c.validate, `media ${i} validate`);
    if (!c.validate) assert.deepEqual(mediaJSON(wire.parseMedia(unhex(c.bytes))), c.in, `media ${i} parse`);
  }
  for (const [i, c] of fixture.mediaInvalid.entries()) {
    assert.equal(errorOf(() => mediaFrom(c.in).validate()), c.validate, `invalid media ${i}`);
  }
  assert.equal(hex(new wire.Media({ fragCount: 1 }).append(unhex("abcd"))).slice(0, 6), "abcd10");
});

test("repair and padding datagrams match Go", () => {
  for (const [i, c] of fixture.repair.entries()) {
    const r = repairFrom(c.in);
    assert.equal(hex(r.append(null)), c.bytes, `repair ${i}`);
    assert.equal(errorOf(() => r.validate()), c.validate, `repair ${i} validate`);
    assert.deepEqual(r.members(null), c.members, `repair ${i} members`);
    if (!c.validate) assert.deepEqual(repairJSON(wire.parseRepair(unhex(c.bytes))), c.in, `repair ${i} parse`);
  }
  for (const [i, c] of fixture.repairInvalid.entries()) {
    const r = repairFrom(c.in);
    assert.equal(errorOf(() => r.validate()), c.validate, `invalid repair ${i}`);
    assert.equal(hex(r.append(null)), c.bytes, `invalid repair ${i} bytes`);
  }
  for (const c of fixture.padding) {
    assert.equal(hex(wire.appendPadding(unhex(c.prefix), c.seq, c.size)), c.bytes);
  }
  for (const c of fixture.feedback) {
    assert.equal(hex(wire.appendFeedback(null, unhex(c.body))), c.bytes);
  }
});

test("every parser gives Go's result or Go's error", () => {
  for (const [i, c] of fixture.parse.entries()) {
    const b = unhex(c.bytes);
    const label = `parse ${i} ${c.bytes.slice(0, 40)}`;
    let k = 0;
    const kindErr = errorOf(() => { k = wire.kind(b); });
    assert.equal(kindErr, c.kindErr, `${label} kind error`);
    assert.equal(k, c.kind, `${label} kind`);
    let media = null;
    assert.equal(errorOf(() => { media = wire.parseMedia(b); }), c.mediaErr ?? "", `${label} media error`);
    if (c.media) assert.deepEqual(mediaJSON(media), c.media, `${label} media`);
    let repair = null;
    assert.equal(errorOf(() => { repair = wire.parseRepair(b); }), c.repairErr ?? "", `${label} repair error`);
    if (c.repair) assert.deepEqual(repairJSON(repair), c.repair, `${label} repair`);
    if (b.length >= 4) {
      let padding = null;
      assert.equal(errorOf(() => { padding = wire.parsePadding(b); }), c.paddingErr ?? "", `${label} padding error`);
      if (c.padding !== undefined) assert.equal(padding, c.padding, `${label} padding`);
    }
    let feedback = null;
    assert.equal(errorOf(() => { feedback = wire.parseFeedback(b); }), c.feedbackErr ?? "", `${label} feedback error`);
    if (c.feedback) {
      assert.equal(feedback.length, c.feedback.length, `${label} feedback count`);
      feedback.forEach((m, j) => sameMsg(msgJSON(m), c.feedback[j], `${label} feedback ${j}`));
    }
    assert.equal(wire.dedupKey(b), c.dedup, `${label} dedup`);
  }
});

test("control messages encode and decode byte for byte", () => {
  for (const [i, c] of fixture.control.entries()) {
    const m = msgFrom(c.msg);
    const b = wire.append(unhex(c.prefix), m);
    assert.equal(hex(b), c.bytes, `control ${i}`);
    sameDecode(decodeJSON(b.subarray(1)), c.decoded, `control ${i} decode`);
  }
  for (const [i, c] of fixture.appendErrors.entries()) {
    if (c.msg.lossy) continue;
    assert.equal(errorOf(() => wire.append(null, msgFrom(c.msg))), c.error, `append error ${i}`);
  }
  for (const [i, c] of fixture.decode.entries()) {
    sameDecode(decodeJSON(unhex(c.bytes)), c, `decode ${i}`);
  }
});

test("byte accessors read and write in place", () => {
  for (const c of fixture.access) {
    const b = unhex(c.bytes);
    assert.equal(wire.transportSeqOf(b), c.transportSeq);
    assert.equal(wire.streamOf(b), c.stream);
    assert.equal(wire.seqOf(b), c.seq);
    assert.equal(wire.flagsOf(b), c.flags);
    assert.equal(wire.frameIDOf(b), c.frameID);
    assert.equal(wire.auxOf(b), c.aux);
    wire.setTransportSeq(b, c.set.transportSeq);
    wire.setStream(b, c.set.stream);
    wire.setSeq(b, c.set.seq);
    wire.setFrameID(b, c.set.frameID);
    wire.setFECFlag(b, c.set.fec);
    assert.equal(hex(b), c.after);
  }
});

test("the control stream reader reassembles any chunking", () => {
  for (const [i, c] of fixture.reader.entries()) {
    const r = new wire.Reader();
    const out = [];
    let error = "";
    for (const chunk of c.chunks) {
      const bytes = typeof chunk === "string" ? unhex(chunk) : new Uint8Array(chunk.zeros);
      error = errorOf(() => r.write(bytes));
      if (error) break;
      for (;;) {
        let m = null;
        error = errorOf(() => { m = r.next(); });
        if (error || !m) break;
        out.push(msgJSON(m));
      }
      if (error) break;
    }
    assert.equal(error, c.error, `reader ${i} error`);
    assert.equal(out.length, c.messages.length, `reader ${i} count`);
    out.forEach((m, j) => sameMsg(m, c.messages[j], `reader ${i} message ${j}`));
    if (error) assert.equal(errorOf(() => r.next()), error, `reader ${i} stays broken`);
  }
});

test("track kinds", () => {
  assert.equal(wire.audioLike(wire.TrackAudio), true);
  assert.equal(wire.audioLike(wire.TrackAuth), true);
  assert.equal(wire.audioLike(wire.TrackVideo), false);
  assert.equal(wire.FECReserve, 22);
});
