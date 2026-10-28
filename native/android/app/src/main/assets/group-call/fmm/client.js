import * as wire from "./wire.js";
import * as cc from "./cc.js";
import * as frag from "./frag.js";
import * as hop from "./hop.js";
import * as jitter from "./jitter.js";
import { near16 } from "./seqnum.js";

export const KindAudio = wire.TrackAudio;
export const KindVideo = wire.TrackVideo;
export const KindAuth = wire.TrackAuth;

export const EventJoined = 1;
export const EventTrackAdded = 2;
export const EventTrackRemoved = 3;
export const EventKeyframeRequest = 4;
export const EventBitrateHint = 5;
export const EventError = 6;
export const EventRecover = 7;
export const EventSendRepresentation = 8;
export const EventReferencePicture = 9;
export const EventRepresentation = 10;
export const EventFaceReference = 11;

export const RepOff = 0;
export const RepVideo = 1;
export const RepFace = 2;

export const AudioWaiting = jitter.StatusWaiting;
export const AudioPlay = jitter.StatusPlay;
export const AudioConceal = jitter.StatusConceal;
export const AudioInsert = jitter.StatusInsert;
export const AudioDRED = jitter.StatusDRED;

export const TickIntervalUs = 5_000;
const maxPublished = 8;
const maxEvents = 256;
const minVideoBps = 30_000;
const headerFactor = 0.97;
const nominalAudio = 40_000;
const nominalAuth = 8_000;
const maxAuthQueue = 64;
const maxLongTerm = 32;
const ackRepeat = 2;
const trackEveryUs = 100_000;
const faceDownBps = 150_000;
const faceUpBps = 250_000;
const faceHoldUs = 1_000_000;
const faceBps = 32_000;
const refEveryUs = 2_000_000;
const refShare = 0.5;
const refPartBytes = 500;
const refPrefix = 4;
const minRefBytes = 600;
const maxRefBytes = 16_000;
const minAudioBps = 16_000;
const maxAudioBps = 32_000;
const audioShare = 0.2;
const recoverRepeatUs = 200_000;

const minRedundancyMs = 200;
const maxRedundancyMs = 1000;
const redundancyLoss = 0.01;
const redundancyShare = 0.03;
const dredFixedBps = 2_400;
const dredBpsPerMs = 10;

export const ErrNotJoined = new wire.FmmError("fmm: not joined");
export const ErrUnknownTrack = new wire.FmmError("fmm: unknown track");
export const ErrTooMany = new wire.FmmError("fmm: too many tracks");
export const ErrClosed = new wire.FmmError("fmm: client closed");
export const ErrProtocol = new wire.FmmError("fmm: control stream broken");
export const ErrBadFrame = new wire.FmmError("fmm: bad frame options");

const simulcastWeights = [[1], [0.25, 0.75], [0.12, 0.28, 0.6]];

export class Config {
  constructor(init = {}) {
    this.startBitrate = init.startBitrate ?? 0;
    this.minBitrate = init.minBitrate ?? 0;
    this.maxBitrate = init.maxBitrate ?? 0;
    this.feedbackOnControl = init.feedbackOnControl ?? false;
    this.noRecovery = init.noRecovery ?? false;
    this.noLookAhead = init.noLookAhead ?? false;
    this.faceDownBitrate = init.faceDownBitrate ?? 0;
    this.faceUpBitrate = init.faceUpBitrate ?? 0;
  }
}

export class Event {
  constructor(init = {}) {
    this.kind = init.kind ?? 0;
    this.stream = init.stream ?? 0;
    this.track = init.track ?? 0;
    this.layers = init.layers ?? 0;
    this.peer = init.peer ?? "";
    this.name = init.name ?? "";
    this.bitrate = init.bitrate ?? 0;
    this.code = init.code ?? 0;
    this.text = init.text ?? "";
    this.frameID = init.frameID ?? 0;
    this.rep = init.rep ?? 0;
    this.maxBytes = init.maxBytes ?? 0;
  }
}

export class AudioFrame {
  constructor(init = {}) {
    this.status = init.status ?? 0;
    this.data = init.data ?? null;
    this.offset = init.offset ?? 0;
  }
}

export class VideoFrame {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.frameID = init.frameID ?? 0;
    this.keyframe = init.keyframe ?? false;
    this.layer = init.layer ?? 0;
    this.timestamp = init.timestamp ?? 0;
    this.face = init.face ?? false;
    this.recovery = init.recovery ?? false;
    this.longTerm = init.longTerm ?? false;
    this.data = init.data ?? null;
  }
}

export class OutFrame {
  constructor(init = {}) {
    this.data = init.data ?? null;
    this.keyframe = init.keyframe ?? false;
    this.layer = init.layer ?? 0;
    this.longTerm = init.longTerm ?? false;
    this.face = init.face ?? false;
    this.recover = init.recover ?? false;
    this.refFrame = init.refFrame ?? 0;
  }
}

export class Stats {
  constructor(init = {}) {
    this.targetBitrate = init.targetBitrate ?? 0;
    this.videoBitrate = init.videoBitrate ?? 0;
    this.rttUs = init.rttUs ?? 0;
    this.loss = init.loss ?? 0;
    this.sentPackets = init.sentPackets ?? 0;
    this.receivedPackets = init.receivedPackets ?? 0;
    this.recoveredPackets = init.recoveredPackets ?? 0;
    this.audioPlayed = init.audioPlayed ?? 0;
    this.audioConcealed = init.audioConcealed ?? 0;
    this.audioFEC = init.audioFEC ?? 0;
    this.audioDRED = init.audioDRED ?? 0;
    this.audioInserted = init.audioInserted ?? 0;
    this.videoReleased = init.videoReleased ?? 0;
    this.videoUndecodable = init.videoUndecodable ?? 0;
    this.keyframeRequests = init.keyframeRequests ?? 0;
    this.recoveryRequests = init.recoveryRequests ?? 0;
    this.recoveryFrames = init.recoveryFrames ?? 0;
    this.escalations = init.escalations ?? 0;
    this.packetsQueued = init.packetsQueued ?? 0;
    this.recoveries = init.recoveries ?? 0;
  }
}

class published {
  constructor(kind, layers, pk) {
    this.kind = kind;
    this.layers = layers;
    this.pk = pk;
    this.hint = 0;
    this.hinted = false;
    this.track = null;
    this.depth = 0;
    this.longTerm = [];
    this.acked = 0;
    this.hasAck = false;
    this.recRef = -1;
    this.recAt = 0;
    this.sending = RepVideo;
    this.refParts = [];
    this.refID = 0;
    this.refBytes = 0;
  }
}

class vtrack {
  constructor(id) {
    this.id = id;
    this.encs = [];
    this.face = 0;
    this.rep = RepVideo;
    this.lowSince = 0;
    this.highSince = 0;
    this.refAt = 0;
    this.lastEval = 0;
  }
}

class refPicture {
  constructor(id, n) {
    this.id = id;
    this.parts = new Array(n).fill(null);
    this.have = 0;
  }
}

class remote {
  constructor(kind, layers, peer, name) {
    this.kind = kind;
    this.layers = layers;
    this.peer = peer;
    this.name = name;
    this.subscribed = false;
    this.ra = null;
    this.audio = null;
    this.video = null;
    this.rep = RepOff;
    this.faceRef = null;
    this.building = null;
    this.acks = [];
    this.ackSent = [];
    this.authOn = false;
    this.auth = [];
  }

  addPart(b) {
    if (!b || b.length < refPrefix || b[3] === 0 || b[2] >= b[3]) return null;
    const id = (b[0] << 8) | b[1];
    if (!this.building || this.building.id !== id || this.building.parts.length !== b[3]) {
      if (this.building && ((id - this.building.id) & 0xffff) >= 0x8000) return null;
      this.building = new refPicture(id, b[3]);
    }
    const pic = this.building;
    if (pic.parts[b[2]] !== null) return null;
    pic.parts[b[2]] = b.subarray(refPrefix);
    pic.have++;
    if (pic.have < pic.parts.length) return null;
    let n = 0;
    for (const p of pic.parts) n += p.length;
    const out = new Uint8Array(n);
    let at = 0;
    for (const p of pic.parts) {
      out.set(p, at);
      at += p.length;
    }
    this.building = null;
    return out;
  }
}

const sorted = (m) => [...m.keys()].sort((a, b) => a - b);

function callErr(f) {
  try {
    const e = f();
    if (e) throw e;
  } catch (e) {
    return e;
  }
  return null;
}

export class Client {
  constructor(t, cfg) {
    let c = new cc.Config();
    let conf = new Config();
    if (cfg) {
      conf = new Config(cfg);
      c = new cc.Config({ startBps: conf.startBitrate, minBps: conf.minBitrate, maxBps: conf.maxBitrate });
    }
    if (conf.faceDownBitrate <= 0) conf.faceDownBitrate = faceDownBps;
    if (conf.faceUpBitrate <= 0) conf.faceUpBitrate = faceUpBps;
    this.cfg = conf;
    this.t = t;
    this.ctl = new wire.Reader();
    this.snd = hop.newSender(t, c);
    this.rcv = hop.newReceiver();
    this.pubs = new Map();
    this.tracks = new Map();
    this.next = 1;
    this.remotes = new Map();
    this.events = [];
    this.joined = false;
    this.peer = "";
    this.closed = false;
    this.err = null;
    this.last = 0;
    this.recs = 0;
    if (!conf.feedbackOnControl) this.rcv.setRepeat(1);
  }

  send(m) {
    const b = wire.append(null, m);
    const e = callErr(() => this.t.sendControl(b));
    if (e) throw e;
  }

  join(ticket, now) {
    if (this.closed) throw ErrClosed;
    this.send(new wire.Join({ ticket }));
  }

  leave(reason, now) {
    if (this.closed) throw ErrClosed;
    this.closed = true;
    this.send(new wire.Leave({ reason }));
  }

  peerID() {
    return this.peer;
  }

  publish(kind, layers, name, track, rank) {
    if (this.closed) throw ErrClosed;
    if (this.pubs.size >= maxPublished) throw ErrTooMany;
    const id = this.next;
    if (track === 0xff) track = id;
    this.send(new wire.Publish({ stream: id, kind: kind & 0xff, layers: layers & 0xff, name, track, rank: rank & 0xff }));
    this.next = (this.next + 1) & 0xff;
    this.pubs.set(id, new published(kind, layers, frag.newPacketizer(id, this.snd.maxPayload())));
    this.snd.addStream(id, kind !== KindVideo);
    return id;
  }

  publishAudio(name, now) {
    return this.publish(KindAudio, 1, name, 0, 0);
  }

  publishAuth(name, now) {
    return this.publish(KindAuth, 1, name, 0, 0);
  }

  sendAuth(stream, data, now) {
    const p = this.pubs.get(stream & 0xff);
    if (!p || p.kind !== KindAuth) throw ErrUnknownTrack;
    const pkts = p.pk.packetizeFrame(data, new frag.Opts({ independent: true, ref: -1 }), now >>> 0);
    for (const b of pkts) this.snd.send(b, now);
  }

  pollAuth(stream) {
    const r = this.remotes.get(stream & 0xff);
    if (!r || r.auth.length === 0) return null;
    return r.auth.shift();
  }

  publishVideo(name, layers, now) {
    return this.publishVideoTrack(name, layers, 1, false, now);
  }

  publishVideoTrack(name, layers, encodings, face, now) {
    layers = Math.min(Math.max(layers, 1), wire.MaxLayer + 1);
    encodings = Math.min(Math.max(encodings, 1), simulcastWeights.length);
    let extra = encodings - 1;
    if (face) extra++;
    if (this.pubs.size + 1 + extra > maxPublished) throw ErrTooMany;
    const base = extra > 0 ? 1 : 0;
    const marker = extra > 0 ? 0xff : 0;
    const id = this.publish(KindVideo, layers, name, marker, base + encodings - 1);
    const vt = new vtrack(id);
    this.tracks.set(id, vt);
    this.pubs.get(id).track = vt;
    for (let i = 0; i < encodings - 1; i++) {
      const e = this.publish(KindVideo, layers, name, id, base + i);
      this.pubs.get(e).track = vt;
      vt.encs.push(e);
    }
    vt.encs.push(id);
    if (face) {
      const f = this.publish(KindVideo, 1, name, id, 0);
      this.pubs.get(f).track = vt;
      this.pubs.get(f).sending = RepOff;
      vt.face = f;
    }
    return id;
  }

  encodingStream(track, index) {
    const vt = this.tracks.get(track & 0xff);
    if (!vt || index < 0 || index >= vt.encs.length) return 0;
    return vt.encs[index];
  }

  faceStream(track) {
    const vt = this.tracks.get(track & 0xff);
    if (vt) return vt.face;
    return 0;
  }

  unpublish(stream, now) {
    const id = stream & 0xff;
    const p = this.pubs.get(id);
    if (!p) throw ErrUnknownTrack;
    let ids = [id];
    const vt = this.tracks.get(id);
    if (vt) {
      ids = [...vt.encs.filter((x) => x !== id), id];
      if (vt.face !== 0) ids.push(vt.face);
      this.tracks.delete(id);
    }
    for (const x of ids) {
      this.pubs.delete(x);
      this.snd.removeStream(x);
    }
    this.send(new wire.Unpublish({ stream: id }));
  }

  setAudioRedundancy(stream, ms) {
    const p = this.pubs.get(stream & 0xff);
    if (!p || p.kind !== KindAudio) throw ErrUnknownTrack;
    p.depth = Math.min(Math.max(Math.trunc((ms * 1000) / wire.DepthUnitUs), 0), 255);
  }

  sendAudio(stream, data, now) {
    const p = this.pubs.get(stream & 0xff);
    if (!p || p.kind !== KindAudio) throw ErrUnknownTrack;
    const pkts = p.pk.packetizeFrame(data, new frag.Opts({ independent: true, ref: -1, aux: p.depth & 0xff }), now >>> 0);
    for (const b of pkts) this.snd.send(b, now);
  }

  audioBitrate(stream) {
    return Math.min(Math.max(Math.trunc(audioShare * this.snd.target()), minAudioBps), maxAudioBps);
  }

  audioRedundancyMs(stream) {
    const ctl = this.snd.controller();
    if (ctl.loss() < redundancyLoss) return minRedundancyMs;
    const ms = (redundancyShare * ctl.target() - dredFixedBps) / dredBpsPerMs;
    return Math.min(Math.max(Math.trunc(ms), minRedundancyMs), maxRedundancyMs);
  }

  sendVideo(stream, data, keyframe, layer, now) {
    this.sendVideoFrame(stream, new OutFrame({ data, keyframe, layer }), now);
  }

  sendVideoFrame(stream, f, now) {
    const p = this.pubs.get(stream & 0xff);
    if (!p || p.kind !== KindVideo) throw ErrUnknownTrack;
    if (!f || f.layer < 0 || f.layer >= p.layers) throw frag.ErrLayer;
    if (f.face && f.keyframe) {
      this.queueReference(p, f.data);
      return -1;
    }
    const o = new frag.Opts({ keyframe: f.keyframe, layer: f.layer, longTerm: f.longTerm, face: f.face, ref: -1, independent: f.face });
    if (f.recover && !f.keyframe && !f.face) {
      let ref = p.recRef;
      if (f.refFrame > 0) ref = f.refFrame;
      if (ref < 0 || !p.longTerm.includes(ref)) throw ErrBadFrame;
      o.ref = ref;
      o.layer = 0;
    }
    if (f.keyframe) p.longTerm = [];
    const pkts = p.pk.packetizeFrame(f.data, o, now >>> 0);
    const id = p.pk.lastID();
    if (f.longTerm) {
      p.longTerm.push(id);
      if (p.longTerm.length > maxLongTerm) p.longTerm.shift();
    }
    if (o.ref >= 0) p.recRef = -1;
    for (const b of pkts) this.snd.send(b, now);
    return id;
  }

  queueReference(p, data) {
    if (!data || data.length === 0 || data.length > maxRefBytes) throw ErrBadFrame;
    p.refID = (p.refID + 1) & 0xffff;
    const n = Math.trunc((data.length + refPartBytes - 1) / refPartBytes);
    p.refParts = [];
    for (let i = 0; i < n; i++) {
      const chunk = data.subarray(i * refPartBytes, Math.min((i + 1) * refPartBytes, data.length));
      const part = new Uint8Array(refPrefix + chunk.length);
      part[0] = p.refID >> 8;
      part[1] = p.refID & 0xff;
      part[2] = i & 0xff;
      part[3] = n & 0xff;
      part.set(chunk, refPrefix);
      p.refParts.push(part);
    }
  }

  sendReferenceParts(id, p, dt, now) {
    if (p.refParts.length === 0) {
      p.refBytes = 0;
      return;
    }
    const spare = Math.max(this.faceBudget(p) - faceBps, faceBps / 4);
    p.refBytes = Math.min(p.refBytes + (refShare * spare * dt) / 8e6, refPartBytes + refPrefix);
    while (p.refParts.length > 0 && p.refBytes >= p.refParts[0].length) {
      const part = p.refParts.shift();
      p.refBytes -= part.length;
      let pkts;
      try {
        pkts = p.pk.packetizeFrame(part, new frag.Opts({ keyframe: true, face: true, ref: -1 }), now >>> 0);
      } catch (_) {
        continue;
      }
      for (const b of pkts) this.snd.send(b, now);
    }
  }

  subscribe(stream, maxLayer, now) {
    const r = this.remotes.get(stream & 0xff);
    if (!r) throw ErrUnknownTrack;
    if (!r.subscribed) {
      r.subscribed = true;
      r.ra = frag.newReassembler();
      if (r.kind === KindAudio) {
        r.audio = jitter.newAudio(new jitter.AudioConfig({ noLookAhead: this.cfg.noLookAhead }));
      } else if (r.kind === KindAuth) {
        r.authOn = true;
      } else {
        r.video = jitter.newVideo(new jitter.VideoConfig({ noRecovery: this.cfg.noRecovery }));
        this.rcv.enableNack(stream & 0xff);
      }
    }
    this.send(new wire.Subscribe({ stream: stream & 0xff, maxLayer: Math.min(Math.max(maxLayer, 0), wire.MaxLayer) }));
  }

  unsubscribe(stream, now) {
    const r = this.remotes.get(stream & 0xff);
    if (!r) throw ErrUnknownTrack;
    r.subscribed = false;
    r.ra = null;
    r.audio = null;
    r.video = null;
    r.authOn = false;
    r.auth = [];
    r.acks = [];
    r.ackSent = [];
    r.rep = RepOff;
    r.building = null;
    this.rcv.removeStream(stream & 0xff);
    this.send(new wire.Unsubscribe({ stream: stream & 0xff }));
  }

  setReceiveLimit(bitrate, now) {
    this.send(new wire.BitrateHint({ stream: wire.AllStreams, bitrate: Math.max(bitrate, 0) >>> 0 }));
  }

  handleDatagram(p, now) {
    let k = -1;
    try {
      k = wire.kind(p);
    } catch (_) {}
    if (k === wire.KindFeedback) {
      let msgs;
      try {
        msgs = wire.parseFeedback(p);
      } catch (_) {
        return;
      }
      for (const m of msgs) this.dispatch(m, now);
      return;
    }
    for (const b of this.rcv.handle(p, now)) {
      let m;
      try {
        m = wire.parseMedia(b);
      } catch (_) {
        continue;
      }
      const r = this.remotes.get(m.stream);
      if (!r || !r.subscribed) continue;
      const f = r.ra.add(m, b !== p, now);
      if (!f) continue;
      if (r.audio) r.audio.pushDepth(f.id, f.timestamp, f.data, f.aux * wire.DepthUnitUs, now);
      else if (r.video) r.video.push(f, now);
      else if (r.authOn && r.auth.length < maxAuthQueue) r.auth.push(f.data);
    }
  }

  handleControl(p, now) {
    try {
      this.ctl.write(p);
    } catch (_) {
      this.fail(ErrProtocol);
      return;
    }
    for (;;) {
      let m;
      try {
        m = this.ctl.next();
      } catch (_) {
        this.fail(ErrProtocol);
        return;
      }
      if (!m) return;
      this.dispatch(m, now);
    }
  }

  fail(err) {
    if (!this.err) {
      this.err = err;
      this.event(new Event({ kind: EventError, text: err.message }));
    }
  }

  event(e) {
    if (this.events.length < maxEvents) this.events.push(e);
  }

  unwrapSent(p, id) {
    return near16(id, Math.max(p.pk.lastID(), 0));
  }

  dispatch(v, now) {
    if (v instanceof wire.Report) {
      this.snd.onReport(v, now);
    } else if (v instanceof wire.Nack) {
      this.snd.onNack(v, now);
    } else if (v instanceof wire.Joined) {
      this.joined = true;
      this.peer = v.peer;
      this.event(new Event({ kind: EventJoined, peer: v.peer }));
    } else if (v instanceof wire.TrackAdded) {
      if (!this.remotes.has(v.stream)) this.remotes.set(v.stream, new remote(v.kind, v.layers, v.peer, v.name));
      this.event(new Event({ kind: EventTrackAdded, stream: v.stream, track: v.kind, layers: v.layers, peer: v.peer, name: v.name }));
    } else if (v instanceof wire.TrackRemoved) {
      const r = this.remotes.get(v.stream);
      if (r) {
        this.remotes.delete(v.stream);
        this.rcv.removeStream(v.stream);
        this.event(new Event({ kind: EventTrackRemoved, stream: v.stream, peer: r.peer, name: r.name }));
      }
    } else if (v instanceof wire.KeyframeRequest) {
      const p = this.pubs.get(v.stream);
      if (p && p.kind === KindVideo) {
        if (p.sending === RepFace) p.track.refAt = 0;
        else this.event(new Event({ kind: EventKeyframeRequest, stream: v.stream }));
      }
    } else if (v instanceof wire.Ack) {
      const p = this.pubs.get(v.stream);
      if (p && p.kind === KindVideo) {
        for (const x of v.frames) {
          const id = this.unwrapSent(p, x);
          if (!p.hasAck || id > p.acked) {
            p.acked = id;
            p.hasAck = true;
          }
        }
      }
    } else if (v instanceof wire.Recovery) {
      const p = this.pubs.get(v.stream);
      if (p && p.kind === KindVideo && p.sending === RepVideo) this.recover(v.stream, p, this.unwrapSent(p, v.frame), now);
    } else if (v instanceof wire.BitrateHint) {
      const p = this.pubs.get(v.stream);
      if (p) {
        p.hint = v.bitrate;
        p.hinted = true;
        this.event(new Event({ kind: EventBitrateHint, stream: v.stream, bitrate: v.bitrate }));
      }
    } else if (v instanceof wire.Error) {
      this.event(new Event({ kind: EventError, code: v.code, text: v.text }));
    }
  }

  recover(id, p, ref, now) {
    if (p.recRef === ref && now - p.recAt < recoverRepeatUs) return;
    if (!p.longTerm.includes(ref) || p.pk.nextID() - ref > 255) {
      this.event(new Event({ kind: EventKeyframeRequest, stream: id }));
      return;
    }
    p.recRef = ref;
    p.recAt = now;
    this.recs++;
    this.event(new Event({ kind: EventRecover, stream: id, frameID: ref }));
  }

  tick(now) {
    let dt = 0;
    if (this.last > 0) dt = now - this.last;
    this.last = now;
    for (const id of sorted(this.tracks)) this.evaluate(this.tracks.get(id), now);
    for (const id of sorted(this.pubs)) {
      const p = this.pubs.get(id);
      if (p.kind === KindVideo) this.sendReferenceParts(id, p, dt, now);
    }
    this.snd.tick(now);
    const rtt = this.snd.controller().rtt();
    this.rcv.setRTT(rtt);
    const fb = [];
    const rep = this.rcv.report(now);
    if (rep) fb.push(rep);
    fb.push(...this.rcv.nacks(now));
    for (const id of sorted(this.remotes)) {
      const r = this.remotes.get(id);
      if (!r.video) continue;
      r.video.setTiming(2 * rtt);
      for (const l of r.ra.expire(now)) r.video.lost(l, now);
      const b = this.ackBytes(id, r);
      if (b) fb.push(b);
      const [ok, stuck] = r.video.recoveryNeeded(now);
      if (ok) {
        try {
          fb.push(wire.append(null, new wire.Recovery({ stream: id, frame: stuck & 0xffff })));
        } catch (_) {}
      }
      if (r.video.keyframeNeeded(now)) {
        try {
          this.send(new wire.KeyframeRequest({ stream: id }));
        } catch (_) {}
      }
    }
    this.sendFeedback(fb);
    return now + TickIntervalUs;
  }

  ackBytes(id, r) {
    for (const a of r.video.takeAcks()) {
      r.acks.push(a);
      r.ackSent.push(0);
    }
    const repeat = this.cfg.feedbackOnControl ? 1 : ackRepeat;
    const frames = [];
    let keep = 0;
    for (let i = 0; i < r.acks.length; i++) {
      const a = r.acks[i];
      if (r.ackSent[i] >= repeat) continue;
      r.ackSent[i]++;
      if (frames.length < wire.MaxAck) frames.push(a & 0xffff);
      r.acks[keep] = a;
      r.ackSent[keep] = r.ackSent[i];
      keep++;
    }
    r.acks.length = keep;
    r.ackSent.length = keep;
    if (frames.length === 0) return null;
    try {
      return wire.append(null, new wire.Ack({ stream: id, frames }));
    } catch (_) {
      return null;
    }
  }

  sendFeedback(msgs) {
    if (msgs.length === 0) return;
    if (this.cfg.feedbackOnControl) {
      for (const m of msgs) callErr(() => this.t.sendControl(m));
      return;
    }
    const [dgs, ctl] = hop.packFeedback(msgs, Math.min(this.t.maxDatagramSize(), hop.MaxPacketSize));
    for (const d of dgs) callErr(() => this.t.sendDatagram(d));
    for (const m of ctl) callErr(() => this.t.sendControl(m));
  }

  evaluate(vt, now) {
    if (now - vt.lastEval < trackEveryUs) return;
    vt.lastEval = now;
    if (vt.face !== 0) {
      const budget = this.trackBudget(vt);
      if (vt.rep === RepVideo && budget < this.cfg.faceDownBitrate) {
        vt.highSince = 0;
        if (vt.lowSince === 0) vt.lowSince = now;
        else if (now - vt.lowSince >= faceHoldUs) {
          vt.rep = RepFace;
          vt.lowSince = 0;
        }
      } else if (vt.rep === RepFace && budget > this.cfg.faceUpBitrate) {
        this.snd.controller().requestProbe(now);
        vt.lowSince = 0;
        if (vt.highSince === 0) vt.highSince = now;
        else if (now - vt.highSince >= faceHoldUs) {
          vt.rep = RepVideo;
          vt.highSince = 0;
        }
      } else {
        vt.lowSince = 0;
        vt.highSince = 0;
        if (vt.rep === RepFace) this.snd.controller().requestProbe(now);
      }
    }
    for (const id of vt.encs) {
      const p = this.pubs.get(id);
      let want = RepVideo;
      if (vt.rep === RepFace && id === vt.id) want = RepFace;
      else if (vt.rep === RepFace) want = RepOff;
      else if (vt.encs.length > 1 && p.hinted && p.hint === 0) want = RepOff;
      this.setSending(id, p, want, now);
    }
    if (vt.face !== 0) {
      const p = this.pubs.get(vt.face);
      let want = RepOff;
      if (vt.rep === RepVideo && p.hinted && p.hint > 0) want = RepFace;
      this.setSending(vt.face, p, want, now);
    }
    for (const id of [...vt.encs, vt.face]) {
      const p = this.pubs.get(id);
      if (!p || p.sending !== RepFace) continue;
      if (vt.refAt === 0 || now - vt.refAt >= refEveryUs) {
        const spare = this.faceBudget(p) - faceBps;
        if (vt.refAt !== 0 && spare < faceBps / 4) continue;
        vt.refAt = now;
        const maxBytes = Math.min(Math.max(Math.trunc(((spare * refEveryUs) / 8e6) * refShare), minRefBytes), maxRefBytes);
        this.event(new Event({ kind: EventReferencePicture, stream: id, maxBytes }));
      }
    }
  }

  setSending(id, p, rep, now) {
    if (!p || p.sending === rep) return;
    p.sending = rep;
    if (rep === RepFace) p.track.refAt = 0;
    else p.refParts = [];
    this.event(new Event({ kind: EventSendRepresentation, stream: id, rep }));
  }

  pullAudio(stream, now) {
    const r = this.remotes.get(stream & 0xff);
    if (!r || !r.audio) return new AudioFrame({ status: AudioWaiting });
    const o = r.audio.pullOut(now);
    return new AudioFrame({ status: o.status, data: o.data, offset: o.offset });
  }

  pollVideo(stream, now) {
    const r = this.remotes.get(stream & 0xff);
    if (!r || !r.video) return null;
    for (;;) {
      const f = r.video.pop(now);
      if (!f) return null;
      const rep = f.face ? RepFace : RepVideo;
      if (rep !== r.rep) {
        r.rep = rep;
        this.event(new Event({ kind: EventRepresentation, stream, rep }));
      }
      if (f.face && f.keyframe) {
        const pic = r.addPart(f.data);
        if (pic) {
          r.faceRef = pic;
          this.event(new Event({ kind: EventFaceReference, stream, maxBytes: pic.length }));
          return new VideoFrame({ stream, frameID: f.id, keyframe: true, face: true, timestamp: f.timestamp, data: pic });
        }
        continue;
      }
      return new VideoFrame({
        stream, frameID: f.id, keyframe: f.keyframe, layer: f.layer, timestamp: f.timestamp,
        face: f.face, recovery: f.recovery, longTerm: f.longTerm, data: f.data,
      });
    }
  }

  faceReference(stream) {
    const r = this.remotes.get(stream & 0xff);
    if (r) return r.faceRef;
    return null;
  }

  representation(stream) {
    const r = this.remotes.get(stream & 0xff);
    if (r) return r.rep;
    return RepOff;
  }

  sendingRepresentation(stream) {
    const p = this.pubs.get(stream & 0xff);
    if (p && p.kind === KindVideo) return p.sending;
    return RepOff;
  }

  ackedFrame(stream) {
    const p = this.pubs.get(stream & 0xff);
    if (p && p.hasAck) return p.acked;
    return -1;
  }

  pollEvent() {
    if (this.events.length === 0) return null;
    return this.events.shift();
  }

  targetBitrate() {
    return this.snd.target();
  }

  videoBitrate(stream) {
    return this._videoBitrate(stream & 0xff);
  }

  faceBudget(p) {
    let b = this.trackBudget(p.track);
    if (p.hinted && p.hint > 0) b = Math.min(b, p.hint);
    return b;
  }

  trackBudget(vt) {
    let avail = this.snd.target();
    let videos = 0;
    for (const sid of sorted(this.pubs)) {
      const q = this.pubs.get(sid);
      if (q.kind !== KindVideo) {
        const nominal = q.kind === KindAuth ? nominalAuth : nominalAudio;
        let [media, repair] = this.snd.streamRate(sid);
        if (media === 0) {
          media = nominal;
          repair = nominal * this.snd.fecRatio(sid);
        }
        avail -= media + repair + this.snd.streamOverheadBps(sid);
        continue;
      }
      avail -= this.snd.streamOverheadBps(sid);
      if (!q.track || q.track.id === sid) videos++;
    }
    let fec = 0;
    if (vt) fec = this.snd.fecRatio(vt.id);
    return (avail / Math.max(videos, 1) / (1 + fec)) * headerFactor;
  }

  _videoBitrate(id) {
    const p = this.pubs.get(id);
    if (!p || p.kind !== KindVideo) return 0;
    if (p.sending === RepOff) return 0;
    const vt = p.track;
    let share = this.trackBudget(vt);
    if (vt && p.sending === RepFace) return Math.max(Math.trunc(Math.min(share, faceBps)), minVideoBps);
    if (vt && vt.encs.length > 1) {
      const f = this.pubs.get(vt.face);
      if (f && f.sending === RepFace) share -= faceBps;
      const w = simulcastWeights[vt.encs.length - 1];
      let total = 0;
      let mine = 0;
      vt.encs.forEach((e, i) => {
        const q = this.pubs.get(e);
        if (q && q.sending === RepVideo) {
          total += w[i];
          if (e === id) mine = w[i];
        }
      });
      if (total > 0) share = (share * mine) / total;
    } else if (vt) {
      const f = this.pubs.get(vt.face);
      if (f && f.sending === RepFace) share -= faceBps;
    }
    if (p.hint > 0) share = Math.min(share, p.hint);
    if (share < minVideoBps / 2) return 0;
    return Math.max(Math.trunc(share), minVideoBps);
  }

  stats() {
    const ctl = this.snd.controller();
    const s = new Stats({
      targetBitrate: ctl.target(), rttUs: ctl.rtt(), loss: ctl.loss(),
      sentPackets: this.snd.sent, receivedPackets: this.rcv.media, recoveredPackets: this.rcv.recovered,
      packetsQueued: this.snd.pacer().queuedBytes(), recoveries: this.recs,
    });
    for (const [id, p] of this.pubs) if (p.kind === KindVideo) s.videoBitrate += this._videoBitrate(id);
    for (const r of this.remotes.values()) {
      if (r.audio) {
        s.audioPlayed += r.audio.stats.played;
        s.audioConcealed += r.audio.stats.concealed;
        s.audioFEC += r.audio.stats.fec;
        s.audioDRED += r.audio.stats.dred;
        s.audioInserted += r.audio.stats.inserted;
      }
      if (r.video) {
        s.videoReleased += r.video.stats.released;
        s.videoUndecodable += r.video.stats.undecodable;
        s.keyframeRequests += r.video.stats.keyframeRequests;
        s.recoveryRequests += r.video.stats.recoveryRequests;
        s.recoveryFrames += r.video.stats.recovered;
        s.escalations += r.video.stats.escalated;
      }
    }
    return s;
  }
}

export function newClient(t, cfg) {
  return new Client(t, cfg);
}
