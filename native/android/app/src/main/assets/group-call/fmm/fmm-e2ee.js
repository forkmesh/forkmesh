import { createCallSession, errorCode } from "../chat-call-e2ee.js";

export const FMM_E2EE_TYPES = Object.freeze({
  presence: "group-call-presence",
  key: "group-call-key",
  verify: "group-call-verify",
});

const DEBOUNCE_MS = 250;
const INSTALL_DELAY_MS = 1000;
const RESEND_MS = 2000;
const ROTATE_EVERY_MS = 10 * 60 * 1000;
const ROTATE_LIMIT_MS = 10 * 1000;
const HOUSEKEEP_MS = 5000;

export function createFmmE2ee({
  call,
  peer,
  acct,
  signer,
  send,
  announce = () => {},
  onSendKey = () => {},
  onPeerKey = () => {},
  onForget = () => {},
  onChange = () => {},
  onError = () => {},
  now = () => Date.now(),
  timers = globalThis,
  createSession = createCallSession,
  requireSealed = true,
}) {
  let session = null;
  let closed = false;
  let rekeyTimer = null;
  let commitTimer = null;
  let resendTimer = null;
  let housekeepTimer = null;
  let lastRotate = 0;
  let status = null;
  let mismatch = false;
  let changedBy = "";
  let lastCommit = null;
  let lastReveal = null;
  const pendingInstall = new Set();
  const opened = new Map();

  function guard(task) {
    return task().catch((error) => {
      if (!closed) onError(errorCode(error) || String(error?.message || error));
      return null;
    });
  }

  async function rotate(force = false) {
    if (!force && now() - lastRotate < ROTATE_LIMIT_MS) return false;
    lastRotate = now();
    await session.rotate();
    announce();
    return true;
  }

  function scheduleRekey() {
    if (closed || rekeyTimer) return;
    rekeyTimer = timers.setTimeout(() => {
      rekeyTimer = null;
      void guard(rekey);
    }, DEBOUNCE_MS);
  }

  function scheduleCommit() {
    if (closed || commitTimer) return;
    commitTimer = timers.setTimeout(() => {
      commitTimer = null;
      void guard(commit);
    }, DEBOUNCE_MS);
  }

  async function rekey() {
    if (closed) return;
    const { message, key } = await session.rekey();
    if (message) await send(FMM_E2EE_TYPES.key, { peer, e2ee: message });
    if (!message) {
      onSendKey(key);
      return;
    }
    const handle = timers.setTimeout(() => {
      pendingInstall.delete(handle);
      if (!closed) onSendKey(key);
    }, INSTALL_DELAY_MS);
    pendingInstall.add(handle);
  }

  async function commit() {
    if (closed) return;
    lastCommit = await session.commit();
    await send(FMM_E2EE_TYPES.verify, { peer, e2ee: lastCommit });
    await tryReveal();
    await refresh();
  }

  async function tryReveal() {
    try {
      const reveal = await session.reveal();
      if (reveal !== lastReveal) {
        lastReveal = reveal;
        await send(FMM_E2EE_TYPES.verify, { peer, e2ee: reveal });
      }
    } catch (error) {
      if (errorCode(error) !== "waiting") throw error;
    }
  }

  async function refresh() {
    const next = await session.verification();
    const before = status?.code ? status.code.digits : "";
    status = next;
    if (next.code && next.code.digits !== before) mismatch = false;
    if (next.needCommit) scheduleCommit();
    onChange(view());
  }

  async function resend() {
    if (closed || !status || !status.waiting.length) return;
    if (lastCommit) await send(FMM_E2EE_TYPES.verify, { peer, e2ee: lastCommit });
    if (lastReveal) await send(FMM_E2EE_TYPES.verify, { peer, e2ee: lastReveal });
    await tryReveal();
    await refresh();
  }

  async function housekeep() {
    if (closed) return;
    for (const gone of session.expire()) dropPeer(gone, "expired");
    session.prune();
    if (now() - lastRotate >= ROTATE_EVERY_MS) await rotate(true);
    await refresh();
  }

  function dropPeer(who, reason) {
    opened.delete(who);
    onForget(who);
    changedBy = reason === "left" ? who : changedBy;
    void guard(async () => {
      await rotate(true);
      scheduleRekey();
      scheduleCommit();
    });
  }

  function view() {
    const st = status || { code: null, waiting: [], late: false, verified: false, newcomers: [], unfinished: 0 };
    return {
      code: st.code ? { emoji: st.code.emoji.slice(), emojiText: st.code.emojiText, emojiNames: st.code.emojiNames.slice(), digits: st.code.digits, qr: st.code.qr } : null,
      waiting: st.waiting.slice(),
      late: st.late,
      verified: st.verified,
      newcomers: st.newcomers.slice(),
      unfinished: st.unfinished,
      mismatch,
      changedBy,
      peers: session ? session.peers() : [],
    };
  }

  async function handlePresence(plain, sealed) {
    const p = plain.e2ee;
    if (!p || typeof p !== "object") return;
    if (sealed && (p.kid !== sealed.kid || (sealed.acct && p.acct !== sealed.acct))) return;
    const result = await session.acceptPresence(p);
    if (result.kind === "joined") {
      changedBy = result.peer;
      await rotate(true);
      scheduleRekey();
      scheduleCommit();
      if (lastCommit) await send(FMM_E2EE_TYPES.verify, { peer, e2ee: lastCommit });
      if (lastReveal) await send(FMM_E2EE_TYPES.verify, { peer, e2ee: lastReveal });
    } else if (result.kind === "updated") {
      scheduleRekey();
    }
  }

  async function handleKey(plain, sealed) {
    const m = plain.e2ee;
    if (!m || typeof m !== "object") return;
    if (sealed && m.kid !== sealed.kid) return;
    try {
      const key = await session.openKeys(m);
      opened.set(key.peer, key.epoch);
      onPeerKey(key);
    } catch (error) {
      const code = errorCode(error);
      if (code === "recipient-epoch") {
        if (await rotate(false)) scheduleRekey();
        return;
      }
      if (code === "duplicate" || code === "stale" || code === "not-for-me") return;
      throw error;
    }
  }

  async function handleVerify(plain, sealed) {
    const m = plain.e2ee;
    if (!m || typeof m !== "object") return;
    if (m.t === "commit") {
      if (sealed && m.kid !== sealed.kid) return;
      try {
        await session.acceptCommit(m);
      } catch (error) {
        const code = errorCode(error);
        if (code === "duplicate") return;
        if (code === "unknown-peer") return;
        throw error;
      }
      await tryReveal();
    } else if (m.t === "reveal") {
      if (sealed && session.member(m.peer)?.kid !== sealed.kid) return;
      try {
        await session.acceptReveal(m);
      } catch (error) {
        const code = errorCode(error);
        if (code !== "waiting" && code !== "duplicate") throw error;
      }
    } else {
      return;
    }
    await refresh();
  }

  return {
    async start() {
      session = await createSession({ call, peer, acct, signer, now });
      lastRotate = now();
      resendTimer = timers.setInterval(() => void guard(resend), RESEND_MS);
      housekeepTimer = timers.setInterval(() => void guard(housekeep), HOUSEKEEP_MS);
      await guard(rekey);
      scheduleCommit();
      return this;
    },
    presence() {
      if (!session || closed) return Promise.resolve(null);
      return session.presence().catch(() => null);
    },
    handle(type, plain, sealed = null) {
      if (!session || closed || !plain) return Promise.resolve();
      if (requireSealed && !sealed && !(type === FMM_E2EE_TYPES.presence && plain.call === "out")) return Promise.resolve();
      if (type === FMM_E2EE_TYPES.presence) {
        if (plain.call === "out") {
          if (session.leave(String(plain.peer || ""))) dropPeer(String(plain.peer), "left");
          return Promise.resolve();
        }
        return guard(() => handlePresence(plain, sealed));
      }
      if (type === FMM_E2EE_TYPES.key) return guard(() => handleKey(plain, sealed));
      if (type === FMM_E2EE_TYPES.verify) return guard(() => handleVerify(plain, sealed));
      return Promise.resolve();
    },
    leave(who) {
      if (!session || closed) return;
      if (session.leave(who)) dropPeer(who, "left");
    },
    async confirm() {
      if (!session || !status?.code) return false;
      try {
        await session.markVerified(status.code.round);
      } catch (_) {
        return false;
      }
      mismatch = false;
      await refresh();
      return true;
    },
    reject() {
      mismatch = true;
      if (session) session.unverify();
      onChange(view());
    },
    get view() { return view(); },
    get session() { return session; },
    close() {
      closed = true;
      for (const t of [rekeyTimer, commitTimer]) if (t) timers.clearTimeout(t);
      for (const t of pendingInstall) timers.clearTimeout(t);
      pendingInstall.clear();
      if (resendTimer) timers.clearInterval(resendTimer);
      if (housekeepTimer) timers.clearInterval(housekeepTimer);
      session?.close();
    },
  };
}
