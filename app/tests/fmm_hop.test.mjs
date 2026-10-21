import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import * as wire from "../public/fmm/wire.js";
import * as frag from "../public/fmm/frag.js";
import * as fec from "../public/fmm/fec.js";
import * as cc from "../public/fmm/cc.js";
import * as hop from "../public/fmm/hop.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/hop/testdata/js-parity.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));

function digest(list) {
  const h = createHash("sha256");
  for (const b of list) {
    h.update(Uint8Array.of(b.length >> 8, b.length & 0xff));
    h.update(b);
  }
  return h.digest("hex").slice(0, 16);
}

function state(sc, snd) {
  const st = [snd.target(), snd.overheadBps(), snd.retransmits, snd.rtxSkipped, snd.padding, snd.sent,
    snd.pacer().queuedBytes(), snd.controller().rtt(), snd.controller().loss()];
  for (const s of sc.streams) {
    const [m, r] = snd.streamRate(s.id);
    st.push(snd.fecRatio(s.id), m, r, snd.streamOverheadBps(s.id));
  }
  return st;
}

function replay(sc) {
  const sent = [];
  let pending = [];
  const transport = {
    sendDatagram(p) {
      sent.push(p.slice());
      pending.push(p.slice());
    },
    maxDatagramSize: () => sc.maxDatagram,
  };
  const cfg = new cc.Config({ startBps: sc.config.StartBps, minBps: sc.config.MinBps, maxBps: sc.config.MaxBps });
  const snd = hop.newSender(transport, cfg);
  if (sc.fixed) snd.fixedFEC = new fec.Params({ k: sc.fixed.K, m: sc.fixed.M });
  const rcv = hop.newReceiver();
  rcv.setRepeat(sc.repeat);
  const pks = new Map();
  for (const s of sc.streams) {
    snd.addStream(s.id, s.audio);
    pks.set(s.id, frag.newPacketizer(s.id, snd.maxPayload()));
    if (s.nack) rcv.enableNack(s.id);
  }
  const fb = [];
  sc.ops.forEach((op, index) => {
    const where = `${sc.name} op ${index} (${op.o} at ${op.t})`;
    const k = op.k ?? false;
    const l = op.l ?? 0;
    switch (op.o) {
      case "f": {
        const pkts = pks.get(op.s).packetizeFrame(new Uint8Array(op.n), new frag.Opts({ independent: true, ref: -1, aux: op.a ?? 0 }), op.t >>> 0);
        assert.equal(pkts.length, op.c, where);
        for (const p of pkts) snd.send(p, op.t);
        break;
      }
      case "v": {
        const pkts = pks.get(op.s).packetize(new Uint8Array(op.n), k, l, false, op.t >>> 0);
        assert.equal(pkts.length, op.c, where);
        for (const p of pkts) snd.send(p, op.t);
        break;
      }
      case "t": {
        pending = [];
        snd.tick(op.t);
        assert.equal(pending.length, op.c, where + " datagrams sent");
        assert.equal(digest(pending), op.h, where + " datagram bytes");
        if (op.st) assert.deepEqual(state(sc, snd), op.st, where + " sender state");
        break;
      }
      case "d": {
        const p = sent[op.i ?? 0].slice();
        const out = rcv.handle(p, op.t);
        assert.equal(out.length, op.c, where + " receiver output count");
        if (op.h) assert.equal(digest(out), op.h, where + " receiver output");
        else assert.equal(hex(out[0]), hex(sent[op.i ?? 0]), where);
        break;
      }
      case "r": {
        assert.equal(snd.controller().rtt(), op.r ?? 0, where + " rtt");
        rcv.setRTT(op.r ?? 0);
        const msgs = [];
        const rep = rcv.report(op.t);
        if (rep) msgs.push(rep);
        msgs.push(...rcv.nacks(op.t));
        assert.equal(msgs.length, op.c, where + " feedback count");
        if (msgs.length > 0) assert.equal(digest(msgs), op.h, where + " feedback bytes");
        fb.push(...msgs);
        break;
      }
      case "b": {
        const [m] = wire.decode(fb[op.i ?? 0]);
        if (m instanceof wire.Report) snd.onReport(m, op.t);
        else if (m instanceof wire.Nack) snd.onNack(m, op.t);
        break;
      }
      default:
        assert.fail("unknown op " + op.o);
    }
  });
  assert.deepEqual(
    [snd.sent, snd.retransmits, snd.rtxSkipped, snd.padding, rcv.media, rcv.recovered, rcv.repairs, rcv.padding, rcv.invalid, rcv.nacked],
    sc.totals,
    sc.name + " totals",
  );
}

for (const sc of fixture.scenarios) {
  test(`hop scenario ${sc.name} replays byte for byte`, () => replay(sc));
}

test("packFeedback matches Go", () => {
  for (const c of fixture.pack) {
    const [d, ctl] = hop.packFeedback(c.msgs.map(unhex), c.max);
    assert.deepEqual(d.map(hex), c.datagrams ?? [], `max ${c.max}`);
    assert.deepEqual(ctl.map(hex), c.control ?? [], `max ${c.max}`);
  }
});
