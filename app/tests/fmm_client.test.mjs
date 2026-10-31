import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import * as fmm from "../public/fmm/client.js";

const fixture = JSON.parse(readFileSync(new URL("../../server/rtc/fmm/testdata/js-parity.json", import.meta.url), "utf8"));

function unrle(s) {
  const out = [];
  for (let i = 0; i < s.length;) {
    if (s[i] === "z") {
      const end = s.indexOf(",", i);
      const n = Number(s.slice(i + 1, end));
      for (let k = 0; k < n; k++) out.push(0);
      i = end + 1;
    } else {
      out.push(parseInt(s.slice(i, i + 2), 16));
      i += 2;
    }
  }
  return Uint8Array.from(out);
}

function digest(...parts) {
  const h = createHash("sha256");
  for (const b of parts) {
    const n = Buffer.alloc(4);
    n.writeUInt32BE(b ? b.length : 0);
    h.update(n);
    if (b) h.update(b);
  }
  return h.digest("hex").slice(0, 16);
}

const flag = (b) => (b ? 1 : 0);

function stats(s) {
  return [s.targetBitrate, s.videoBitrate, s.rttUs, s.loss, s.sentPackets, s.receivedPackets, s.recoveredPackets,
    s.audioPlayed, s.audioConcealed, s.audioFEC, s.audioDRED, s.audioInserted, s.videoReleased, s.videoUndecodable,
    s.keyframeRequests, s.recoveryRequests, s.recoveryFrames, s.escalations, s.packetsQueued, s.recoveries];
}

function attempt(f) {
  try {
    const v = f();
    return v === undefined ? "" : v;
  } catch (e) {
    if (!(e instanceof Error)) throw e;
    if (e.name !== "FmmError") throw e;
    return "!" + e.message;
  }
}

function replay(sc) {
  let out = [];
  const transport = {
    sendDatagram(p) {
      out.push(Uint8Array.of(0x64), p.slice());
    },
    sendControl(p) {
      out.push(Uint8Array.of(0x63), p.slice());
    },
    maxDatagramSize: () => sc.maxDatagram,
  };
  const cfg = {};
  for (const [k, v] of Object.entries(sc.config)) cfg[k[0].toLowerCase() + k.slice(1)] = v;
  const c = fmm.newClient(transport, cfg);
  sc.ops.forEach((op, index) => {
    const [code, t] = op;
    const tx = op[op.length - 1];
    const args = op.slice(2, -1);
    const where = `${sc.name} op ${index} ${code} at ${t}`;
    let got;
    let want = args[args.length - 1];
    switch (code) {
      case "J": got = attempt(() => c.join(args[0], t)); break;
      case "PA": got = attempt(() => c.publishAudio(args[0], t)); break;
      case "PH": got = attempt(() => c.publishAuth(args[0], t)); break;
      case "PV": got = attempt(() => c.publishVideoTrack(args[0], args[1], args[2], args[3] === 1, t)); break;
      case "ES": got = c.encodingStream(args[0], args[1]); break;
      case "FS": got = c.faceStream(args[0]); break;
      case "RD": got = attempt(() => c.setAudioRedundancy(args[0], args[1])); break;
      case "AB": got = c.audioBitrate(args[0]); break;
      case "AM": got = c.audioRedundancyMs(args[0]); break;
      case "SA": got = attempt(() => c.sendAudio(args[0], unrle(args[1]), t)); break;
      case "SH": got = attempt(() => c.sendAuth(args[0], unrle(args[1]), t)); break;
      case "VB": got = c.videoBitrate(args[0]); break;
      case "SV": {
        const fl = args[2];
        const f = new fmm.OutFrame({ data: unrle(args[1]), keyframe: !!(fl & 1), longTerm: !!(fl & 2), face: !!(fl & 4),
          recover: !!(fl & 8), layer: args[3], refFrame: args[4] });
        got = attempt(() => c.sendVideoFrame(args[0], f, t));
        break;
      }
      case "SB": got = attempt(() => c.subscribe(args[0], args[1], t)); break;
      case "US": got = attempt(() => c.unsubscribe(args[0], t)); break;
      case "UP": got = attempt(() => c.unpublish(args[0], t)); break;
      case "RL": got = attempt(() => c.setReceiveLimit(args[0], t)); break;
      case "LV": got = attempt(() => c.leave(args[0], t)); break;
      case "D": c.handleDatagram(unrle(args[0]), t); got = want = null; break;
      case "C": c.handleControl(unrle(args[0]), t); got = want = null; break;
      case "T": got = c.tick(t); break;
      case "EV": {
        const e = c.pollEvent();
        got = e && [e.kind, e.stream, e.track, e.layers, e.peer, e.name, e.bitrate, e.code, e.text, e.frameID, e.rep, e.maxBytes];
        break;
      }
      case "PO": {
        const f = c.pollVideo(args[0], t);
        got = f && [f.stream, f.frameID, flag(f.keyframe) | (flag(f.face) << 1) | (flag(f.recovery) << 2) | (flag(f.longTerm) << 3),
          f.layer, f.timestamp, digest(f.data)];
        break;
      }
      case "PL": {
        const b = c.pollAuth(args[0]);
        got = b && digest(b);
        break;
      }
      case "PU": {
        const f = c.pullAudio(args[0], t);
        got = [f.status, f.data ? digest(f.data) : "", f.offset];
        break;
      }
      case "ST": got = stats(c.stats()); want = args[0]; break;
      case "TB": got = c.targetBitrate(); break;
      case "PI": got = c.peerID(); break;
      case "AK": got = c.ackedFrame(args[0]); break;
      case "SR": got = c.sendingRepresentation(args[0]); break;
      case "RP": got = c.representation(args[0]); break;
      case "FR": {
        const r = c.faceReference(args[0]);
        got = r ? digest(r) : "";
        break;
      }
      default:
        assert.fail("unknown op " + code);
    }
    assert.deepEqual(got, want, where);
    let sent = "";
    if (out.length > 0) sent = `${out.length / 2}:${digest(...out)}`;
    out = [];
    assert.equal(sent, tx, where + " transport output");
  });
}

for (const sc of fixture.scenarios) {
  test(`client scenario ${sc.name} replays byte for byte`, () => replay(sc));
}
