import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import * as frag from "../public/fmm/frag.js";
import * as wire from "../public/fmm/wire.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/frag/testdata/js-parity.json", import.meta.url), "utf8"));
const hex = (b) => (b ? Buffer.from(b).toString("hex") : "");
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));

function fillData(n, seed) {
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = (i * 131 + seed * 977 + (i >> 3) * 29) & 255;
  return b;
}

const errNames = new Map([
  [frag.ErrTooLarge, "ErrTooLarge"], [frag.ErrRef, "ErrRef"], [frag.ErrLayer, "ErrLayer"],
  [wire.ErrShort, "ErrShort"], [wire.ErrLong, "ErrLong"], [wire.ErrVersion, "ErrVersion"], [wire.ErrKind, "ErrKind"],
  [wire.ErrFragment, "ErrFragment"], [wire.ErrFlags, "ErrFlags"],
]);

function errName(e) {
  if (errNames.has(e)) return errNames.get(e);
  throw e;
}

function summary(p) {
  return { header: hex(p.subarray(0, wire.MediaHeaderLen)), len: p.length, sum: createHash("sha256").update(p).digest("hex") };
}

test("packetizer matches Go byte for byte", () => {
  for (const c of fixture.packetize) {
    const p = frag.newPacketizer(c.stream, c.maxPayload);
    assert.equal(p.maxFrame(), c.maxFrame, c.name);
    c.steps.forEach((s, i) => {
      const data = fillData(s.size, s.seed);
      const o = s.opts;
      let pkts = [];
      let error = "";
      try {
        pkts = s.simple
          ? p.packetize(data, o.keyframe, o.layer, o.independent, s.ts)
          : p.packetizeFrame(data, new frag.Opts(o), s.ts);
      } catch (e) {
        error = errName(e);
      }
      assert.equal(error, s.error || "", `${c.name} step ${i}`);
      assert.deepEqual(pkts.map(summary), s.packets, `${c.name} step ${i}`);
      assert.equal(p.lastID(), s.lastId, `${c.name} step ${i} lastID`);
      assert.equal(p.nextID(), s.nextId, `${c.name} step ${i} nextID`);
    });
  }
});

test("long packetizer runs wrap sequence numbers and frame ids like Go", () => {
  for (const lr of fixture.longRuns) {
    const p = frag.newPacketizer(lr.stream, lr.maxPayload);
    const h = createHash("sha256");
    for (let i = 0; i < lr.frames; i++) {
      const layer = lr.pattern[i % lr.pattern.length];
      for (const b of p.packetize(fillData(1 + (i % 97), i), i % 5000 === 0, layer, false, (i * 33333) >>> 0)) h.update(b);
    }
    assert.equal(h.digest("hex"), lr.sum);
    assert.equal(p.lastID(), lr.lastId);
  }
});

test("reassembler traces match Go", () => {
  for (const c of fixture.reassembly) {
    const r = frag.newReassembler();
    r.timeout = c.timeout;
    c.steps.forEach((s, i) => {
      const where = `${c.name} step ${i}`;
      if (s.op === "expire") {
        const lost = r.expire(s.now).map((l) => ({ id: l.id, keyframe: l.keyframe, layer: l.layer }));
        assert.deepEqual(lost, s.lost || [], where);
      } else {
        let m = null;
        let parseError = "";
        try {
          m = wire.parseMedia(unhex(s.packet));
        } catch (e) {
          parseError = errName(e);
        }
        assert.equal(parseError, s.parseError || "", where);
        if (m) {
          const f = r.add(m, s.recovered || false, s.now);
          const got = f && {
            id: f.id, keyframe: f.keyframe, recovery: f.recovery, longTerm: f.longTerm, face: f.face, layer: f.layer,
            ref: f.ref, aux: f.aux, timestamp: f.timestamp, firstSeq: f.firstSeq, lastSeq: f.lastSeq, data: hex(f.data),
            firstArrival: f.firstArrival, complete: f.complete, recovered: f.recovered,
          };
          assert.deepEqual(got, s.frame || null, where);
        }
      }
      assert.deepEqual({
        completed: r.completed, duplicates: r.duplicates, corrupt: r.corrupt, expired: r.expired,
        bytes: r.bytes, partials: r.order.length,
      }, s.counters, where);
    });
  }
});
