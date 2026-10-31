import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { Recorder, Pacer, newController, Config, ClassVideo } from "../public/fmm/cc.js";
import { Report, append } from "../public/fmm/wire.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/cc/testdata/js-parity.json", import.meta.url), "utf8"));

const hex = (b) => Buffer.from(b).toString("hex");

function toReport(r) {
  return new Report({
    base: r.base,
    refTime: r.refTime,
    delay: r.delay,
    received: Array.from(r.received, (c) => c === "1"),
    arrival: r.arrival.slice(),
  });
}

function fromReport(r) {
  return {
    base: r.base,
    refTime: r.refTime,
    delay: r.delay,
    received: r.received.map((x) => (x ? "1" : "0")).join(""),
    arrival: r.arrival.slice(),
  };
}

function snapshot(c, now) {
  return [c.target(), c.rtt(), c.loss(), c.burst(), c.signal(), c.ackedRate(), c.threshold(), c.queueDelay(),
    c.congested(now), c.probeEstimate(now), c.reports, c.overuses, c.lostTotal, c.recvTotal, c.probesSent, c.probesWon,
    c.fastCuts, c.silenceCuts, c.silenceUndone, c.standCuts];
}

for (const trace of fixture.recorder) {
  test(`recorder ${trace.name}`, () => {
    const rec = new Recorder({ repeat: trace.repeat });
    let reports = 0;
    for (const [i, e] of trace.events.entries()) {
      if (e[0] === 0) {
        rec.onPacket(e[1], e[2]);
        continue;
      }
      const [, now, due, want] = e;
      assert.equal(rec.due(now), due, `event ${i} due`);
      const got = rec.report(now);
      if (want === null) {
        assert.equal(got, null, `event ${i} report`);
        continue;
      }
      reports++;
      const { hex: wantHex, ...fields } = want;
      assert.deepEqual(fromReport(got), fields, `event ${i} report fields`);
      assert.equal(hex(append(null, got)), wantHex, `event ${i} report bytes`);
    }
    assert.equal(rec.packets, trace.final[0]);
    assert.ok(reports > 3);
  });
}

for (const trace of fixture.controller) {
  test(`controller ${trace.name}`, () => {
    const ctrl = newController(new Config({ startBps: trace.config.StartBps, minBps: trace.config.MinBps, maxBps: trace.config.MaxBps }));
    let seq = 0;
    let checked = 0;
    for (const [i, e] of trace.events.entries()) {
      if (typeof e === "number") {
        ctrl.checkFeedback(e);
        assert.equal(ctrl.startProbe(e), null, `event ${i} probe`);
        continue;
      }
      switch (e[0]) {
        case 0:
          seq = (seq + 1) & 0xffff;
          ctrl.onSent(seq, e[1], e[2], e[3] ?? 0);
          break;
        case 1:
          ctrl.onReport(toReport(e[2]), e[1]);
          assert.deepEqual(snapshot(ctrl, e[1]), e[3], `event ${i} after report at ${e[1]}`);
          checked++;
          break;
        case 2: {
          const now = e[1];
          const cuts = ctrl.silenceCuts;
          ctrl.checkFeedback(now);
          assert.equal(ctrl.silenceCuts !== cuts ? 1 : 0, e[2], `event ${i} silence cut`);
          if (e[3] > 0) ctrl.endProbe(e[3], now);
          let k = 4;
          const started = ctrl.startProbe(now);
          if (e.length > k && typeof e[k] === "number") {
            assert.deepEqual(started, [e[k], e[k + 1], e[k + 2]], `event ${i} start probe`);
            k += 3;
          } else {
            assert.equal(started, null, `event ${i} start probe`);
          }
          if (e[2] === 1) {
            assert.deepEqual(snapshot(ctrl, now), e[k], `event ${i} after silence`);
            checked++;
          }
          break;
        }
        case 3:
          ctrl.requestProbe(e[1]);
          break;
        default:
          throw new Error("unknown event " + e[0]);
      }
    }
    assert.ok(checked > 50);
  });
}

for (const trace of fixture.pacer) {
  test(`pacer ${trace.name}`, () => {
    const p = new Pacer();
    let id = 0;
    const mk = (size) => {
      id++;
      const b = new Uint8Array(Math.max(size, 4));
      new DataView(b.buffer).setUint32(0, id >>> 0);
      return b;
    };
    for (const [i, e] of trace.events.entries()) {
      switch (e[0]) {
        case 0:
          p.push(e[1], mk(e[2]), e[3]);
          break;
        case 1:
          p.setRate(e[1]);
          break;
        case 2:
          p.setProbe(e[1], e[2]);
          break;
        case 3:
          p.dropOlder(e[1], e[2]);
          break;
        case 4:
          p.setDrainCap(e[1]);
          break;
        case 5: {
          const [, now, padFlag, sent, queued, dropped, oldest, probing] = e;
          const got = [];
          p.tick(now, (b, probe) => {
            got.push(new DataView(b.buffer, b.byteOffset).getUint32(0), b.length, probe ? 1 : 0);
          }, padFlag ? (size) => mk(size) : null);
          assert.deepEqual(got, sent ?? [], `tick ${i} sends`);
          assert.deepEqual([p.queuedBytes(), p.dropped, p.oldestUs(now), p.probing()], [queued, dropped, oldest, probing], `tick ${i} state`);
          break;
        }
        case 6:
          for (let k = 0; k < e[4]; k++) p.push(e[1], mk(e[2]), e[3]);
          assert.equal(e[1], ClassVideo);
          break;
        default:
          throw new Error("unknown event " + e[0]);
      }
    }
  });
}
