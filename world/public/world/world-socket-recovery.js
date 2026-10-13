// Small, dependency-free timer gate for the World presence socket.
//
// Keeping these three timers behind one owner makes the recovery rules
// executable in isolation: one active handshake, one reconnect, and one
// sustained-backpressure check. Callers still own protocol messages and socket
// event listeners; this module only prevents stale callbacks from reviving a
// replaced connection.
export function createWorldSocketRecoveryTimers({
  setTimeoutImpl = globalThis.setTimeout.bind(globalThis),
  clearTimeoutImpl = globalThis.clearTimeout.bind(globalThis),
} = {}) {
  let activeSocket = null;
  let connectDeadlineTimer = 0;
  let reconnectTimer = 0;
  let backpressureTimer = 0;

  const cancelConnectDeadline = () => {
    if (connectDeadlineTimer) clearTimeoutImpl(connectDeadlineTimer);
    connectDeadlineTimer = 0;
  };

  const cancelReconnect = () => {
    if (reconnectTimer) clearTimeoutImpl(reconnectTimer);
    reconnectTimer = 0;
  };

  const cancelBackpressure = () => {
    if (backpressureTimer) clearTimeoutImpl(backpressureTimer);
    backpressureTimer = 0;
  };

  const adopt = (socket, deadlineMs, onDeadline) => {
    if (!socket) throw new TypeError("A socket is required");
    const replaced = activeSocket === socket ? null : activeSocket;
    activeSocket = socket;
    cancelConnectDeadline();
    cancelBackpressure();
    const delay = Math.max(1, Number(deadlineMs) || 1);
    connectDeadlineTimer = setTimeoutImpl(() => {
      connectDeadlineTimer = 0;
      if (activeSocket !== socket) return;
      onDeadline?.(socket);
    }, delay);
    return replaced;
  };

  const markOpen = (socket) => {
    if (!socket || activeSocket !== socket) return false;
    cancelConnectDeadline();
    return true;
  };

  const retire = (socket) => {
    if (!socket || activeSocket !== socket) return false;
    activeSocket = null;
    cancelConnectDeadline();
    cancelBackpressure();
    return true;
  };

  const scheduleReconnect = (delayMs, reconnect) => {
    if (reconnectTimer) return false;
    const delay = Math.max(1, Number(delayMs) || 1);
    reconnectTimer = setTimeoutImpl(() => {
      reconnectTimer = 0;
      reconnect?.();
    }, delay);
    return true;
  };

  const armBackpressure = (
    socket,
    delayMs,
    shouldRecover,
    recover,
  ) => {
    if (!socket || activeSocket !== socket || backpressureTimer) return false;
    const delay = Math.max(1, Number(delayMs) || 1);
    backpressureTimer = setTimeoutImpl(() => {
      backpressureTimer = 0;
      if (activeSocket !== socket || !shouldRecover?.(socket)) return;
      recover?.(socket);
    }, delay);
    return true;
  };

  const clearAll = () => {
    activeSocket = null;
    cancelConnectDeadline();
    cancelReconnect();
    cancelBackpressure();
  };

  return Object.freeze({
    adopt,
    armBackpressure,
    cancelBackpressure,
    cancelReconnect,
    clearAll,
    markOpen,
    retire,
    scheduleReconnect,
    get activeSocket() {
      return activeSocket;
    },
    get hasReconnect() {
      return Boolean(reconnectTimer);
    },
  });
}
