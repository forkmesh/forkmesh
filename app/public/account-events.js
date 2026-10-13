// One authenticated, payload-free event channel per signed-in account.
//
// This is the browser half of the same push channel the desktop node holds:
// the per-owner ForkMeshNodes Durable Object behind /api/nodes/events. The
// relay pushes {"type":"event","topic"} frames, with only a routing key such
// as `repo` when one surface can be targeted. Account-scoped topics include
// "pings" for the alert inbox, "issues" for issue-related notification
// refreshes, and "direct-messages" for a conversation's unread count. The page
// answers with the one authenticated read it would otherwise have made on a
// timer.
//
// That is the whole point: pages read their unread counts ONCE on open and
// then sit here (docs/operations/polling-elimination.md). There is no fallback
// poll behind this socket, so liveness is the socket's own job — reconnect is
// unconditional with bounded backoff, and every successful (re)connect fires
// one catch-up so anything raised while the channel was down is not lost.
//
// A WebSocket upgrade cannot carry an Authorization header, so each attempt
// trades the session for a fresh 60s ticket from /api/accounts/event-ticket.

const TICKET_ENDPOINT = "/api/accounts/event-ticket";
const INITIAL_RECONNECT_MS = 2000;
const MAX_RECONNECT_MS = 30000;
// Matches the desktop node's cadence and stays well inside the Durable
// Object's 15-minute staleness reaper.
const KEEPALIVE_MS = 4 * 60 * 1000;

export function createAccountEventChannel({
  sessionToken,
  // The machine whose object this channel wants to be accepted onto, for the
  // owner terminal relay (agent-terminal.js). A desktop node's socket lives on
  // the account's object and a managed mirror's on its own, so a stream that
  // only ever joined the account's object could never reach a mirror. The
  // relay decides: it proves the account owns this machine and then joins the
  // socket to whichever object the machine is actually on. Empty — every other
  // caller — keeps the account's own object.
  nodeName = "",
  onTopic = () => {},
  // Every frame the relay sends, topic events included. The account channel
  // itself only cares about "event", but the same socket carries the owner
  // terminal relay (agent-terminal.js), whose frames are addressed to one
  // session rather than broadcast to the page.
  onFrame = () => {},
  onConnected = () => {},
  onSignedOut = () => {},
  fetchImpl = globalThis.fetch.bind(globalThis),
  WebSocketImpl = globalThis.WebSocket,
  locationLike = globalThis.location,
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
} = {}) {
  if (typeof sessionToken !== "function") {
    throw new TypeError("createAccountEventChannel requires sessionToken()");
  }

  let socket = null;
  let reconnectTimer = null;
  let keepaliveTimer = null;
  let reconnectDelayMs = INITIAL_RECONNECT_MS;
  let generation = 0;
  let started = false;
  let disposed = false;

  function clearTimers() {
    if (reconnectTimer !== null) {
      clearTimeoutImpl(reconnectTimer);
      reconnectTimer = null;
    }
    if (keepaliveTimer !== null) {
      clearIntervalImpl(keepaliveTimer);
      keepaliveTimer = null;
    }
  }

  function scheduleReconnect() {
    if (disposed || !started || reconnectTimer !== null) return;
    const delay = reconnectDelayMs;
    reconnectDelayMs = Math.min(MAX_RECONNECT_MS, reconnectDelayMs * 2);
    reconnectTimer = setTimeoutImpl(() => {
      reconnectTimer = null;
      void connect();
    }, delay);
  }

  function closeSocket() {
    const current = socket;
    socket = null;
    if (!current) return;
    try {
      current.close(1000, "channel closed");
    } catch (_) {}
  }

  // Resolves to {ticket} on success, {signedOut: true} when the relay says
  // this session proves nobody, and {} for a transient failure. The three have
  // to stay distinguishable: retrying a stale token left in localStorage would
  // be a poll of the ticket endpoint, and going dark on a relay hiccup would
  // silently turn the page back into a reload-only one.
  async function requestTicket() {
    let response;
    try {
      response = await fetchImpl(TICKET_ENDPOINT, {
        headers: {
          accept: "application/json",
          Authorization: `Bearer ${sessionToken()}`,
        },
        cache: "no-store",
      });
    } catch (_) {
      return {};
    }
    if (response.status === 401 || response.status === 403) {
      return { signedOut: true };
    }
    if (!response.ok) return {};
    const data = await response.json().catch(() => null);
    if (!data) return {};
    if (data.authenticated === false) return { signedOut: true };
    const ticket = String(data.ticket || "");
    return ticket ? { ticket } : { signedOut: true };
  }

  async function connect() {
    if (disposed || !started) return;
    if (socket) return;
    // Signed out: stay dark instead of retrying forever, which is just a poll
    // of the ticket endpoint. restart() brings the channel up after a sign-in.
    if (!sessionToken()) {
      started = false;
      return;
    }
    const attempt = ++generation;
    const { ticket, signedOut } = await requestTicket();
    if (attempt !== generation || disposed || !started) return;
    if (signedOut) {
      // The stored session proves nobody. Retrying it would just be a poll;
      // restart() brings the channel up after a real sign-in.
      //
      // This is also the authoritative revocation signal the dashboard used to
      // buy with a 20s `GET /api/accounts/sessions` poll: every (re)connect
      // trades the stored session for a fresh ticket, so a session revoked
      // anywhere shows up here on the next reconnect for no extra request.
      started = false;
      try {
        onSignedOut();
      } catch (_) {}
      return;
    }
    if (!ticket) {
      scheduleReconnect();
      return;
    }
    const scheme = locationLike.protocol === "https:" ? "wss:" : "ws:";
    const url = new URL(
      "/api/nodes/events",
      `${scheme}//${locationLike.host}`,
    );
    url.searchParams.set("ticket", ticket);
    if (nodeName) url.searchParams.set("node", nodeName);
    let opened;
    try {
      opened = new WebSocketImpl(url.href);
    } catch (_) {
      scheduleReconnect();
      return;
    }
    socket = opened;
    opened.addEventListener("open", () => {
      if (socket !== opened) return;
      reconnectDelayMs = INITIAL_RECONNECT_MS;
      if (keepaliveTimer !== null) clearIntervalImpl(keepaliveTimer);
      keepaliveTimer = setIntervalImpl(() => {
        if (socket !== opened || opened.readyState !== 1) return;
        try {
          opened.send(JSON.stringify({ type: "ping" }));
        } catch (_) {}
      }, KEEPALIVE_MS);
      try {
        onConnected();
      } catch (_) {}
    });
    opened.addEventListener("message", (event) => {
      if (socket !== opened) return;
      let frame = null;
      try {
        frame = JSON.parse(event.data);
      } catch (_) {
        return;
      }
      try {
        onFrame(frame);
      } catch (_) {}
      if (!frame || frame.type !== "event") return;
      const topic = String(frame.topic || "");
      if (!topic) return;
      try {
        // Keep the first argument stable for existing consumers; the complete
        // payload-free envelope lets repository surfaces target the one row
        // named by a catalog/commit invalidation instead of polling them all.
        onTopic(topic, frame);
      } catch (_) {}
    });
    opened.addEventListener("close", () => {
      if (socket !== opened) return;
      socket = null;
      if (keepaliveTimer !== null) {
        clearIntervalImpl(keepaliveTimer);
        keepaliveTimer = null;
      }
      scheduleReconnect();
    });
    opened.addEventListener("error", () => {
      if (socket === opened) {
        try {
          opened.close();
        } catch (_) {}
      }
    });
  }

  return {
    // Send one frame to the relay. False means the socket is not up, which is
    // a real answer for a caller streaming a terminal: it can re-announce its
    // session on the next onConnected instead of queueing bytes nobody asked
    // for. Nothing here is retried for you.
    send(frame) {
      if (!socket || socket.readyState !== 1) return false;
      try {
        socket.send(JSON.stringify(frame));
        return true;
      } catch (_) {
        return false;
      }
    },
    start() {
      if (disposed || started) return;
      started = true;
      reconnectDelayMs = INITIAL_RECONNECT_MS;
      void connect();
    },
    stop() {
      started = false;
      generation += 1;
      clearTimers();
      closeSocket();
    },
    // A sign-in or account switch invalidates the ticket this socket was
    // opened with; drop it and come back under the new identity.
    restart() {
      if (disposed) return;
      const wasStarted = started;
      this.stop();
      if (wasStarted || sessionToken()) this.start();
    },
    dispose() {
      disposed = true;
      this.stop();
    },
    get connected() {
      return Boolean(socket) && socket.readyState === 1;
    },
  };
}
