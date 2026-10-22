import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { newAudio, newVideo, AudioConfig, VideoConfig } from "../public/fmm/jitter.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/jitter/testdata/js-parity.json", import.meta.url), "utf8"));

const lower = (key) => {
  const m = /^[A-Z]+(?=[A-Z][a-z]|$)|^[A-Z]/.exec(key);
  return m ? m[0].toLowerCase() + key.slice(m[0].length) : key;
};
const camel = (object) => Object.fromEntries(Object.entries(object).map(([k, v]) => [lower(k), v]));

const audioData = (id) => Uint8Array.of(id & 0xff, (id >> 8) & 0xff);
const tag = (d) => (d == null ? -1 : d[0] | (d[1] << 8));

for (const trace of fixture.audio) {
  test(`audio parity: ${trace.name}`, () => {
    const a = newAudio(new AudioConfig(camel(trace.config)));
    trace.ops.forEach((op, i) => {
      const where = `${trace.name} op ${i}`;
      if (op[0] === 0) {
        a.pushDepth(op[1], op[2], audioData(op[1]), op[3], op[4]);
      } else if (op[0] === 1) {
        const o = a.pullOut(op[1]);
        assert.deepEqual([o.status, tag(o.data), o.offset], [op[2], op[3], op[4]], where);
      } else {
        assert.deepEqual([a.targetUs(), a.extraUs()], [op[1], op[2]], where);
      }
    });
    assert.deepEqual({ ...a.stats }, camel(trace.stats), trace.name);
  });
}

function frame(op) {
  return {
    id: op[1], keyframe: op[2] === 1, recovery: op[3] === 1, longTerm: op[4] === 1, face: op[5] === 1,
    layer: op[6], ref: op[7], aux: 0, timestamp: op[8], firstSeq: op[9], lastSeq: op[10],
    data: null, firstArrival: 0, complete: op[11], recovered: 0,
  };
}

for (const trace of fixture.video) {
  test(`video parity: ${trace.name}`, () => {
    const v = newVideo(new VideoConfig(camel(trace.config)));
    trace.ops.forEach((op, i) => {
      const where = `${trace.name} op ${i}`;
      switch (op[0]) {
        case 0:
          v.push(frame(op), op[12]);
          break;
        case 1: {
          const f = v.pop(op[1]);
          assert.deepEqual([f ? f.id : -1, v.waitUs(), v.lastID()], [op[2], op[3], op[4]], where);
          break;
        }
        case 2:
          v.lost({ id: op[1], keyframe: op[2] === 1, layer: op[3] }, op[4]);
          break;
        case 3:
          v.setTiming(op[1]);
          break;
        case 4: {
          const [ok, stuck] = v.recoveryNeeded(op[1]);
          assert.deepEqual([ok ? 1 : 0, stuck], [op[2], op[3]], where);
          break;
        }
        case 5:
          assert.equal(v.keyframeNeeded(op[1]) ? 1 : 0, op[2], where);
          break;
        case 6:
          assert.deepEqual(v.takeAcks(), op.slice(1), where);
          break;
        case 7: {
          const ids = [];
          for (let f = v.pop(op[1]); f; f = v.pop(op[1])) ids.push(f.id);
          const [ok, stuck] = v.recoveryNeeded(op[1]);
          const key = v.keyframeNeeded(op[1]);
          assert.deepEqual([ok ? 1 : 0, stuck, key ? 1 : 0, v.waitUs(), v.lastID(), ...ids], op.slice(2), where);
          break;
        }
        default:
          assert.fail(`unknown op ${op[0]}`);
      }
    });
    assert.deepEqual({ ...v.stats }, camel(trace.stats), trace.name);
  });
}
