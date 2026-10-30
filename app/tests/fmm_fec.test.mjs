import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import * as fec from "../public/fmm/fec.js";
import * as wire from "../public/fmm/wire.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/fec/testdata/js-parity.json", import.meta.url), "utf8"));
const hex = (b) => Buffer.from(b).toString("hex");
const unhex = (h) => new Uint8Array(Buffer.from(h, "hex"));

const errNames = new Map([
  [fec.ErrTooFew, "ErrTooFew"], [fec.ErrGeometry, "ErrGeometry"], [wire.ErrShort, "ErrShort"], [wire.ErrLong, "ErrLong"],
  [wire.ErrRepair, "ErrRepair"], [wire.ErrKind, "ErrKind"], [wire.ErrVersion, "ErrVersion"],
]);

function errName(e) {
  if (errNames.has(e)) return errNames.get(e);
  throw e;
}

function makeBlocks(k, size, seed) {
  const out = [];
  for (let i = 0; i < k; i++) {
    const b = new Uint8Array(size);
    for (let x = 0; x < size; x++) b[x] = (i * 131 + x * 17 + seed * 977 + ((i * x) >> 3)) & 255;
    out.push(b);
  }
  return out;
}

test("Reed-Solomon encoding matches Go", () => {
  for (const c of fixture.encode) {
    assert.deepEqual(fec.encode(makeBlocks(c.k, c.size, c.seed), c.m).map(hex), c.repairs, `k ${c.k} m ${c.m}`);
  }
});

test("reconstruction matches Go", () => {
  fixture.reconstruct.forEach((c, i) => {
    const data = c.data.map((h) => (h == null ? null : unhex(h)));
    const repair = c.repair.map((h) => (h == null ? null : unhex(h)));
    let error = "";
    try {
      fec.reconstruct(data, repair);
    } catch (e) {
      error = errName(e);
    }
    assert.equal(error, c.error || "", `${c.name} ${i}`);
    if (!error) assert.deepEqual(data.map(hex), c.result, `${c.name} ${i}`);
  });
});

test("policy matches Go", () => {
  for (const r of fixture.residual) {
    const got = fec.residual(r.k, r.m, r.p, r.burst);
    const tolerance = 1e-12 * Math.max(Math.abs(r.value), Number.MIN_VALUE);
    assert.ok(Math.abs(got - r.value) <= tolerance, `residual ${JSON.stringify(r)} got ${got}`);
  }
  for (const c of fixture.choose) {
    const p = fec.choose(c.audio, c.loss, c.burst, c.maxK);
    assert.deepEqual([p.k, p.m], [c.k, c.m], `choose ${JSON.stringify(c)}`);
  }
  for (const c of fixture.chooseVideo) {
    const [i, o] = fec.chooseVideo(c.loss, c.burst, c.maxKI, c.maxKO);
    assert.deepEqual([i.k, i.m, o.k, o.m], [c.ik, c.im, c.ok, c.om], `chooseVideo ${JSON.stringify(c)}`);
  }
});

test("stream encoder traces match Go", () => {
  for (const t of fixture.encoders) {
    const e = fec.newEncoder(t.stream, t.maxDatagram, t.maxDelay);
    t.steps.forEach((s, i) => {
      const where = `${t.name} step ${i} ${s.op}`;
      let out = [];
      if (s.op === "setParams") e.setParams(new fec.Params({ k: s.k || 0, m: s.m || 0 }));
      else if (s.op === "setClass") e.setClassParams(new fec.Params({ k: s.k || 0, m: s.m || 0 }), new fec.Params({ k: s.ok || 0, m: s.om || 0 }));
      else if (s.op === "unequal") e.setUnequal(s.on || false);
      else if (s.op === "scale") e.scale(s.f || 0);
      else if (s.op === "add") {
        const pkt = unhex(s.pkt);
        out = e.add(pkt, s.now);
        assert.equal(pkt.length > 10 && (wire.flagsOf(pkt) & wire.FlagFEC) !== 0, s.fecOn || false, where + " fec flag");
      } else if (s.op === "flush") out = e.flush(s.now);
      assert.deepEqual(out.map(hex), s.out, where);
      const p = e.params();
      assert.deepEqual({ k: p.k, m: p.m, ratio: e.ratio(0.6), groups: e.groups, repairs: e.repairs }, s.state, where);
    });
  }
});

test("stream decoder traces match Go", () => {
  for (const t of fixture.decoders) {
    const d = fec.newDecoder(t.stream);
    t.steps.forEach((s, i) => {
      const where = `${t.name} step ${i} ${s.op}`;
      let out = [];
      if (s.op === "repair") {
        let r = null;
        let parseError = "";
        try {
          r = wire.parseRepair(unhex(s.pkt));
        } catch (e) {
          parseError = errName(e);
        }
        assert.equal(parseError, s.parseError || "", where);
        if (r) out = d.addRepair(r, s.now);
      } else {
        out = d.addMedia(unhex(s.pkt), s.protected || false, s.now);
      }
      assert.deepEqual(out.map(hex), s.out, where);
      assert.deepEqual([d.recovered, d.failed, d.groups.length], [s.recovered, s.failed, s.groups], where);
    });
  }
});
