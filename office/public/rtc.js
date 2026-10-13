
const RETRY_COOLDOWN = 2000;
const GRACE_MS = 1500;
export const SLOTS = ['audio', 'video', 'screen'];
const SLOT_KIND = { audio: 'audio', video: 'video', screen: 'video' };
const SCREEN_MAX_BITRATE = 1500000;
export const FALLBACK_ICE = [{ urls: 'stun:stun.cloudflare.com:3478' }];

export function iceConfig(list) {
  const servers = Array.isArray(list) ? list.filter((s) => s && (typeof s.urls === 'string' || Array.isArray(s.urls))) : [];
  return servers.length ? servers : FALLBACK_ICE;
}

export class Mesh {
  constructor(selfId, signal, { onStream, onClose, onScreen, iceServers, refreshIce } = {}) {
    this.iceServers = Array.isArray(iceServers) ? iceServers : [];
    this.refreshIce = refreshIce || null;
    this.selfId = selfId;
    this.signal = signal;
    this.onStream = onStream;
    this.onClose = onClose;
    this.onScreen = onScreen || null;
    this.local = { audio: null, video: null, screen: null };
    this.peers = new Map();
    this.cooldown = new Map();
  }

  has(id) {
    return this.peers.has(id);
  }

  screenOf(id) {
    return this.peers.get(id)?.screen || null;
  }

  want(ids) {
    const now = Date.now();
    if (ids.size) this.refreshIce?.();
    for (const id of ids) {
      if (this.peers.has(id) || this.selfId > id) continue;
      if ((this.cooldown.get(id) || 0) > now) continue;
      this.offer(id);
    }
    for (const [id, p] of this.peers) {
      if (!ids.has(id) && now - p.born > GRACE_MS) this.close(id, true);
    }
  }

  slotOf(pc, transceiver) {
    return SLOTS[pc.getTransceivers().indexOf(transceiver)] || null;
  }

  create(id) {
    const pc = new RTCPeerConnection({ iceServers: iceConfig(this.iceServers) });
    const p = { pc, stream: new MediaStream(), screen: new MediaStream(), ice: [], born: Date.now() };
    this.peers.set(id, p);
    pc.onicecandidate = (e) => e.candidate && this.signal(id, { ice: e.candidate });
    pc.ontrack = (e) => {
      if (this.slotOf(pc, e.transceiver) === 'screen') {
        if (!p.screen.getTracks().includes(e.track)) p.screen.addTrack(e.track);
        this.onScreen?.(id, p.screen, e.track);
        return;
      }
      p.stream.addTrack(e.track);
      this.onStream(id, p.stream);
    };
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === 'failed') this.close(id, true);
    };
    return p;
  }

  async tune(slot, sender) {
    if (slot !== 'screen' || !sender.track) return;
    try {
      const prm = sender.getParameters();
      prm.degradationPreference = 'maintain-resolution';
      if (prm.encodings?.[0]) prm.encodings[0].maxBitrate = SCREEN_MAX_BITRATE;
      await sender.setParameters(prm);
    } catch {}
  }

  async offer(id) {
    const p = this.create(id);
    for (const slot of SLOTS) p.pc.addTransceiver(this.local[slot] || SLOT_KIND[slot], { direction: 'sendrecv' });
    try {
      await p.pc.setLocalDescription();
      this.signal(id, { sdp: p.pc.localDescription });
    } catch (err) {
      console.warn('offer failed', err);
      this.close(id, true);
    }
  }

  async handle(from, d) {
    if (d.bye) return this.close(from, false);
    let p = this.peers.get(from);
    try {
      if (d.sdp) {
        if (d.sdp.type === 'offer') {
          if (p) this.close(from, false);
          p = this.create(from);
          await p.pc.setRemoteDescription(d.sdp);
          const list = p.pc.getTransceivers();
          for (let i = 0; i < list.length; i++) {
            const t = list[i];
            t.direction = 'sendrecv';
            const track = SLOTS[i] ? this.local[SLOTS[i]] : null;
            if (track && track.kind === t.receiver.track.kind) await t.sender.replaceTrack(track);
          }
          await p.pc.setLocalDescription();
          this.signal(from, { sdp: p.pc.localDescription });
          const st = list[SLOTS.indexOf('screen')];
          if (st) this.tune('screen', st.sender);
        } else if (p) {
          await p.pc.setRemoteDescription(d.sdp);
          const st = p.pc.getTransceivers()[SLOTS.indexOf('screen')];
          if (st) this.tune('screen', st.sender);
        }
        if (p) for (const c of p.ice.splice(0)) await p.pc.addIceCandidate(c);
      } else if (d.ice && p) {
        if (p.pc.remoteDescription) await p.pc.addIceCandidate(d.ice);
        else p.ice.push(d.ice);
      }
    } catch (err) {
      console.warn('signal error', err);
    }
  }

  async setTrack(kind, track) {
    this.local[kind] = track;
    const i = SLOTS.indexOf(kind);
    if (i < 0) return;
    for (const { pc } of this.peers.values()) {
      const t = pc.getTransceivers()[i];
      if (!t) continue;
      await t.sender.replaceTrack(track).catch(() => {});
      this.tune(kind, t.sender);
    }
  }

  close(id, notify) {
    const p = this.peers.get(id);
    if (!p) return;
    this.peers.delete(id);
    this.cooldown.set(id, Date.now() + RETRY_COOLDOWN);
    p.pc.close();
    if (notify) this.signal(id, { bye: 1 });
    this.onClose(id);
  }

  closeAll() {
    for (const id of [...this.peers.keys()]) this.close(id, true);
  }
}
