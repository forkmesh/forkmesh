import {
  canAccessOfficeFloor,
  normalizeOfficeFloorAccess,
  officeFloorById,
} from "./world-office-tower.js";

export {
  OFFICE_ENTER_DISTANCE,
  OFFICE_EXIT_DISTANCE,
  nextOfficeZoneState,
} from "./world-office-tower.js";

const OFFICE_CHAT_PATH = "/chat?embed=office";
const OFFICE_UNLOAD_DELAY_MS = 2000;
const OFFICE_ENTRY_PATH = "/api/world/office/general/entry";
const OFFICE_FLOORS_PATH = "/api/world/office/floors";
const OFFICE_ATTENDANCE_PATH = "/api/world/office/attendance";
const OFFICE_ATTENDANCE_HEARTBEAT_MS = 30_000;
// Mirrors the Worker's OFFICE_ATTENDANCE_LIVE_TTL_MS: once our writes have been
// quiet for this long the server has closed the open visit, and that visit id
// can no longer be extended.
const OFFICE_ATTENDANCE_LIVE_TTL_MS = 75_000;

function newOfficeAttendanceVisitId() {
  const randomUUID = globalThis.crypto?.randomUUID;
  if (typeof randomUUID === "function") {
    return randomUUID.call(globalThis.crypto).replaceAll("-", "");
  }
  const bytes = new Uint8Array(16);
  if (typeof globalThis.crypto?.getRandomValues === "function") {
    globalThis.crypto.getRandomValues(bytes);
  } else {
    for (let index = 0; index < bytes.length; index += 1) {
      bytes[index] = Math.floor(Math.random() * 256);
    }
  }
  return [...bytes]
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");
}

// True once the server can no longer be holding this visit open. Exported so
// the resume rule is testable without a live Office session.
export function officeAttendanceVisitExpired(seenAt, now) {
  const acknowledged = Number(seenAt) || 0;
  const observed = Number(now) || 0;
  if (acknowledged <= 0 || observed <= 0) return false;
  return observed - acknowledged >= OFFICE_ATTENDANCE_LIVE_TTL_MS;
}

export function createWorldOfficeController({
  root,
  world,
  meeting,
  tasks = null,
  chatPath = OFFICE_CHAT_PATH,
  getSession = () => null,
}) {
  const fallbackButton = root.querySelector("[data-world-office-fallback]");
  const panel = root.querySelector("[data-world-office-chat]");
  const heading = root.querySelector("#world-office-chat-title");
  const loading = root.querySelector("[data-world-office-loading]");
  const frame = root.querySelector("[data-world-office-frame]");

  let proximity = "distant";
  let active = false;
  let fallbackActive = false;
  let returnFocus = null;
  let unloadTimer = null;
  let frameSuspended = false;
  let entryPending = false;
  let meetingAuthorizationPromise = null;
  let exitPending = false;
  let authorizationGeneration = 0;
  let floorAuthorizationPromise = null;
  let officeEntryTicket = "";
  let officeEntryExpiresAt = 0;
  let officeAccess = normalizeOfficeFloorAccess({});
  let attendanceAccount = "";
  let attendanceWrite = Promise.resolve();
  let attendanceFloorId = "lobby";
  let attendanceVisitId = "";
  let attendanceHeartbeat = null;
  let attendanceSeenAt = 0;

  const resolvedChatURL = new URL(chatPath, window.location.origin);
  if (
    resolvedChatURL.origin !== window.location.origin ||
    resolvedChatURL.pathname !== "/chat" ||
    resolvedChatURL.searchParams.get("embed") !== "office"
  ) {
    throw new Error("ForkMesh Office chat must use the same-origin /chat embed.");
  }
  const safeChatPath = `${resolvedChatURL.pathname}${resolvedChatURL.search}`;

  function session() {
    try {
      const value = getSession();
      return value && typeof value === "object" ? value : null;
    } catch (_) {
      return null;
    }
  }

  function authenticatedSession() {
    const value = session();
    return String(value?.sessionToken || "") ? value : null;
  }

  function clearUnloadTimer() {
    if (!unloadTimer) return;
    window.clearTimeout(unloadTimer);
    unloadTimer = null;
  }

  function setEntryPending(pending) {
    entryPending = pending === true;
  }

  function closeFallback({ restoreFocus = true } = {}) {
    if (!fallbackActive && !frame?.hasAttribute("src")) return false;
    fallbackActive = false;
    if (panel) {
      panel.dataset.open = "false";
      panel.setAttribute("aria-hidden", "true");
    }
    if (frame?.contentWindow && frame.hasAttribute("src")) {
      frameSuspended = true;
      frame.contentWindow.postMessage(
        { type: "office-chat-suspend" },
        window.location.origin,
      );
    }
    clearUnloadTimer();
    unloadTimer = window.setTimeout(() => {
      frame?.removeAttribute("src");
      frameSuspended = false;
      unloadTimer = null;
    }, OFFICE_UNLOAD_DELAY_MS);
    if (restoreFocus) {
      window.requestAnimationFrame(() => fallbackButton?.focus());
    }
    return true;
  }

  function openFallback(trigger = null) {
    if (!active || meeting.inRoom || fallbackActive) return false;
    if (trigger instanceof HTMLElement) returnFocus = trigger;
    clearUnloadTimer();
    fallbackActive = true;
    if (panel) {
      panel.dataset.open = "true";
      panel.setAttribute("aria-hidden", "false");
    }
    if (loading) loading.hidden = false;
    if (frameSuspended) {
      frame?.removeAttribute("src");
      frameSuspended = false;
    }
    if (frame && frame.getAttribute("src") !== safeChatPath) {
      frame.setAttribute("src", safeChatPath);
    }
    window.requestAnimationFrame(() => heading?.focus());
    return true;
  }

  function setProximity(nextState) {
    proximity = nextState === "nearby" ? "nearby" : "distant";
  }

  function focusOffice(trigger = null) {
    if (trigger instanceof HTMLElement) returnFocus = trigger;
    world.focusLandmark("office");
  }

  async function loadFloorAccess(activeSession) {
    const payload = await root.fetchJSON(OFFICE_FLOORS_PATH, {
      timeout: 8000,
      cache: "no-store",
    });
    const normalized = normalizeOfficeFloorAccess({
      ...(payload && typeof payload === "object" ? payload : {}),
      account:
        String(payload?.account || "") ||
        String(activeSession?.nodeName || ""),
    });
    if (!normalized.authenticated) {
      const error = new Error("login_required");
      error.status = 401;
      throw error;
    }
    return {
      ...(payload && typeof payload === "object" ? payload : {}),
      ...normalized,
    };
  }

  // District packages (the aquarium Sky Office package, for one) share the
  // same server-authoritative organization membership without opening the
  // original Office, recording attendance, or starting its meeting runtime.
  async function hydrateFloorAccess() {
    const activeSession = authenticatedSession();
    if (!activeSession || typeof root.fetchJSON !== "function") {
      officeAccess = initialOfficeAccess();
      world.setOfficeAccess?.(officeAccess);
      return false;
    }
    try {
      officeAccess = await loadFloorAccess(activeSession);
      world.setOfficeAccess?.(officeAccess);
      return true;
    } catch (_) {
      officeAccess = initialOfficeAccess();
      world.setOfficeAccess?.(officeAccess);
      return false;
    }
  }

  function requestedFloorId(value) {
    const requested =
      value && typeof value === "object"
        ? value.floorId || value.id
        : value;
    return officeFloorById(requested)?.id || "";
  }

  function travelToOfficeFloor(value) {
    const floorId = requestedFloorId(value);
    const floor = officeFloorById(floorId);
    if (!active || !floor || !canAccessOfficeFloor(officeAccess, floorId)) {
      root.notice?.(
        floor
          ? `${floor.label} is available only to authorized members of that team.`
          : "That Office floor is unavailable.",
      );
      return false;
    }
    const travelled = world.travelToOfficeFloor?.(floorId);
    if (travelled === false) {
      root.notice?.(`${floor.label} is temporarily unavailable.`);
      return false;
    }
    tasks?.setActive?.(floor.id === "marketing");
    attendanceFloorId = floor.id;
    void recordAttendance("heartbeat");
    return travelled !== undefined ? travelled : true;
  }

  function applyAttendance(payload = {}) {
    const visits = Array.isArray(payload?.visits)
      ? payload.visits.slice(0, 20)
      : [];
    const leaderboard = Array.isArray(payload?.leaderboard)
      ? payload.leaderboard.slice(0, 20)
      : [];
    const asOfAt = Number(payload?.asOfAt);
    world.setOfficeAttendance?.({
      visits,
      leaderboard,
      ...(Number.isFinite(asOfAt) && asOfAt > 0 ? { asOfAt } : {}),
    });
    return visits;
  }

  async function loadAttendance() {
    if (typeof root.fetchJSON !== "function") return [];
    try {
      return applyAttendance(
        await root.fetchJSON(OFFICE_ATTENDANCE_PATH, {
          timeout: 6000,
          cache: "no-store",
        }),
      );
    } catch (_) {
      return [];
    }
  }

  function recordAttendance(
    direction,
    {
      loadPublicFallback = false,
      generation = authorizationGeneration,
    } = {},
  ) {
    const account = String(attendanceAccount || "");
    const visitId = String(attendanceVisitId || "");
    if (!account || typeof root.postJSON !== "function") {
      return attendanceWrite;
    }
    const action =
      direction === "out"
        ? "out"
        : direction === "heartbeat"
          ? "heartbeat"
          : "in";
    // Keep IN and OUT ordered if someone walks straight through the lobby.
    // One compact POST per transition replaces browser-local clock state.
    attendanceWrite = attendanceWrite
      .catch(() => null)
      .then(async () => {
        const payload = await root.postJSON(
          OFFICE_ATTENDANCE_PATH,
          { action, floor: attendanceFloorId, visitId },
          { timeout: 6000 },
        );
        applyAttendance(payload);
        // Server-acknowledged liveness for this visit id. A gap wider than the
        // Worker's TTL means the visit was closed behind our back.
        if (action !== "out") attendanceSeenAt = Date.now();
        return payload;
      })
      .catch(async () => {
        // A failed signed IN should still leave the public lobby's last-20
        // ledger useful. This fallback is sequential, never concurrent with
        // the authoritative POST response.
        if (
          loadPublicFallback &&
          action === "in" &&
          active &&
          generation === authorizationGeneration
        ) {
          await loadAttendance();
        }
        return null;
      });
    return attendanceWrite;
  }

  function stopAttendanceHeartbeat() {
    if (!attendanceHeartbeat) return;
    window.clearInterval(attendanceHeartbeat);
    attendanceHeartbeat = null;
  }

  function punchAttendanceTick() {
    if (!active || document.hidden || !attendanceAccount) return;
    // The scene owns the physical building boundary. A delayed doorway
    // callback must never let general World time leak into Office Hours.
    if (world.isOfficeInterior?.() === false) {
      stopAttendanceHeartbeat();
      void recordAttendance("out");
      return;
    }
    // Heartbeats pause while the tab is hidden (and stop outright during a
    // network outage), so the Worker's live-presence TTL closes the visit. A
    // heartbeat can no longer extend that closed id: resume as a new visit
    // instead, otherwise the member is stuck AWAY on the Office leaderboard and
    // the rest of the stay they are physically still in never counts.
    if (officeAttendanceVisitExpired(attendanceSeenAt, Date.now())) {
      attendanceVisitId = newOfficeAttendanceVisitId();
      void recordAttendance("in");
      return;
    }
    void recordAttendance("heartbeat");
  }

  function startAttendanceHeartbeat() {
    stopAttendanceHeartbeat();
    attendanceHeartbeat = window.setInterval(
      punchAttendanceTick,
      OFFICE_ATTENDANCE_HEARTBEAT_MS,
    );
  }

  // Resume the moment the tab is looked at again rather than up to one heartbeat
  // later, so a member who tabbed away is counted as present right away. Only
  // the resume case is urgent: a visit the server still holds open is refreshed
  // by the regular heartbeat, and punching on every tab switch would add
  // avoidable Worker traffic.
  function onVisibilityChange() {
    if (document.hidden || !attendanceHeartbeat) return;
    if (!officeAttendanceVisitExpired(attendanceSeenAt, Date.now())) return;
    punchAttendanceTick();
  }

  function initialOfficeAccess() {
    // The lobby is physically public, but elevator grants remain fail-closed
    // until the server validates a signed-in account in the background.
    return normalizeOfficeFloorAccess({});
  }

  function completeOfficeEntry({
    loadPublicAttendance = true,
    source = "",
  } = {}) {
    if (!world.enterOffice({ source })) return false;
    authorizationGeneration += 1;
    setEntryPending(false);
    officeAccess = initialOfficeAccess();
    attendanceAccount = "";
    attendanceFloorId = "lobby";
    attendanceVisitId = newOfficeAttendanceVisitId();
    attendanceSeenAt = 0;
    stopAttendanceHeartbeat();
    world.setOfficeAccess?.(officeAccess);
    officeEntryTicket = "";
    officeEntryExpiresAt = 0;
    meeting.setEntryTicket?.("", 0);
    active = true;
    exitPending = false;
    meeting.openLobby();
    tasks?.setActive?.(false);
    if (loadPublicAttendance) void loadAttendance();
    return true;
  }

  async function refreshOfficeAuthorization(
    activeSession,
    generation = authorizationGeneration,
    {
      recordEntry = true,
      loadPublicFallback = true,
    } = {},
  ) {
    if (
      !activeSession ||
      entryPending ||
      !active ||
      generation !== authorizationGeneration
    ) {
      return false;
    }
    if (typeof root.fetchJSON !== "function") {
      return false;
    }
    setEntryPending(true);
    world.setOfficeDoorStatus?.("syncing");
    const refresh = (async () => {
      let authorized = false;
      try {
        const floorAccess = await loadFloorAccess(activeSession);
        if (!active || generation !== authorizationGeneration) return false;
        officeAccess = floorAccess;
        attendanceAccount = officeAccess.account;
        attendanceFloorId = "lobby";
        world.setOfficeAccess?.(officeAccess);
        if (recordEntry) {
          void recordAttendance("in", {
            loadPublicFallback,
            generation,
          });
          startAttendanceHeartbeat();
        }
        authorized = true;
        return true;
      } catch (_) {
        // A stale/invalid signed session remains a public-lobby visit. Load
        // its read-only ledger only for the initial entry path; an explicit
        // post-membership refresh must not add unrelated network traffic.
        if (
          loadPublicFallback &&
          active &&
          generation === authorizationGeneration
        ) {
          void loadAttendance();
        }
        return false;
      } finally {
        if (generation === authorizationGeneration) {
          setEntryPending(false);
          world.setOfficeDoorStatus?.(authorized ? "ready" : "open");
        }
      }
    })();
    floorAuthorizationPromise = refresh;
    try {
      return await refresh;
    } finally {
      if (floorAuthorizationPromise === refresh) {
        floorAuthorizationPromise = null;
      }
    }
  }

  // One explicit, server-authoritative refresh after the signed-in viewer's
  // own organization-team membership changes. It waits for entry hydration
  // rather than racing it, never records another attendance punch, and owns no
  // interval/polling loop.
  async function refreshAuthorization() {
    const pending = floorAuthorizationPromise;
    if (pending) {
      try {
        await pending;
      } catch (_) {}
    }
    const activeSession = authenticatedSession();
    const generation = authorizationGeneration;
    if (!active || !activeSession || generation !== authorizationGeneration) {
      return false;
    }
    return refreshOfficeAuthorization(activeSession, generation, {
      recordEntry: false,
      loadPublicFallback: false,
    });
  }

  async function authorizeMeeting() {
    if (!active) return false;
    if (
      officeEntryTicket &&
      officeEntryExpiresAt > Date.now() + 5000
    ) {
      return true;
    }
    if (meetingAuthorizationPromise) return meetingAuthorizationPromise;
    const activeSession = authenticatedSession();
    if (activeSession?.visitor) {
      // Meetings belong to this Office's own World; visitors look around.
      root.notice?.(
        "Office meetings are for members of this World. As a visitor you can look around the lobby and the rooftop.",
      );
      return false;
    }
    if (!activeSession) {
      root.toggleWorldAccount?.(true, "login");
      root.setWorldAccountStatus?.("Sign in to join an Office meeting.");
      return false;
    }
    if (typeof root.postJSON !== "function") return false;
    const generation = authorizationGeneration;
    meetingAuthorizationPromise = (async () => {
      try {
        const payload = await root.postJSON(
          OFFICE_ENTRY_PATH,
          {},
          { timeout: 8000 },
        );
        if (
          !active ||
          generation !== authorizationGeneration ||
          payload?.ok !== true ||
          !String(payload?.entryTicket || "") ||
          !Number.isFinite(Number(payload?.expiresAt))
        ) {
          return false;
        }
        officeEntryTicket = String(payload.entryTicket).slice(0, 2048);
        officeEntryExpiresAt = Number(payload.expiresAt) || 0;
        meeting.setEntryTicket?.(officeEntryTicket, officeEntryExpiresAt);
        return officeEntryExpiresAt > Date.now() + 5000;
      } catch (_) {
        root.notice?.("Sign in again to join this Office meeting.");
        return false;
      } finally {
        if (generation === authorizationGeneration) {
          meetingAuthorizationPromise = null;
        }
      }
    })();
    return meetingAuthorizationPromise;
  }

  async function enterOffice(entry = {}) {
    const doorwayEntry = entry?.source === "doorway";
    try {
      // The scene emits "doorway" only after the avatar fully clears the inner
      // jamb. Do not reject a fast crossing merely because the later proximity
      // tick still contains the previous frame.
      if (active || (!doorwayEntry && proximity !== "nearby")) return false;
      const activeSession = authenticatedSession();
      const entered = completeOfficeEntry({
        loadPublicAttendance: !activeSession,
        source: doorwayEntry ? "doorway" : "",
      });
      if (entered && !activeSession) {
        world.setOfficeDoorStatus?.("open");
      }
      if (entered && activeSession) {
        // Physical admission never waits on the network. Signed-in visitors
        // receive team-floor and meeting permissions in the background.
        void refreshOfficeAuthorization(
          activeSession,
          authorizationGeneration,
        );
      }
      return entered;
    } finally {
      if (doorwayEntry) world.setOfficeDoorwayEntryPending?.(false);
    }
  }

  async function restoreSavedView(view = {}) {
    const wantsOffice = view?.office === true;
    if (!wantsOffice) {
      if (active) {
        world.leaveOfficeInterior?.();
        completeOfficeExit();
      }
      return world.restoreSavedViewState?.(view) === true;
    }

    const wasActive = active;
    if (!active) {
      const activeSession = authenticatedSession();
      const entered = completeOfficeEntry({
        loadPublicAttendance: !activeSession,
        source: "saved-view",
      });
      if (!entered) return false;
      if (activeSession) {
        await refreshOfficeAuthorization(
          activeSession,
          authorizationGeneration,
          { recordEntry: true },
        );
      } else {
        world.setOfficeDoorStatus?.("open");
      }
    } else if (
      String(view.floorId || "lobby") !== "lobby" &&
      authenticatedSession()
    ) {
      await refreshOfficeAuthorization(
        authenticatedSession(),
        authorizationGeneration,
        { recordEntry: false, loadPublicFallback: false },
      );
    }

    const restored = world.restoreSavedViewState?.(view) === true;
    if (!restored && !wasActive) {
      // Admission happened through the same attendance path as the door. If
      // the target floor is unauthorized, return to town and close that visit
      // instead of leaving a hidden lobby check-in behind.
      world.leaveOfficeInterior?.();
      completeOfficeExit();
    }
    return restored;
  }

  function completeOfficeExit() {
    if (!active) return false;
    closeFallback({ restoreFocus: false });
    tasks?.setActive?.(false);
    void recordAttendance("out");
    stopAttendanceHeartbeat();
    meeting.leaveOffice();
    meeting.setEntryTicket?.("", 0);
    officeEntryTicket = "";
    officeEntryExpiresAt = 0;
    meetingAuthorizationPromise = null;
    floorAuthorizationPromise = null;
    officeAccess = normalizeOfficeFloorAccess({});
    attendanceAccount = "";
    attendanceFloorId = "lobby";
    attendanceVisitId = "";
    attendanceSeenAt = 0;
    world.setOfficeAccess?.(officeAccess);
    world.setOfficeDoorStatus?.("open");
    authorizationGeneration += 1;
    setEntryPending(false);
    active = false;
    exitPending = false;
    const focusTarget = returnFocus?.isConnected ? returnFocus : null;
    window.requestAnimationFrame(() => focusTarget?.focus());
    return true;
  }

  function collapse() {
    if (!active || exitPending) return false;
    closeFallback({ restoreFocus: false });
    if (meeting.inRoom) meeting.leaveRoom();
    exitPending = true;
    if (!world.beginOfficeExit?.()) {
      exitPending = false;
      return false;
    }
    root.hudNotice?.("Walk through the Office entrance to return outside.");
    return true;
  }

  function onClick(event) {
    const focusControl = event.target.closest("[data-world-office-focus]");
    if (focusControl) {
      event.preventDefault();
      focusOffice(focusControl);
      return;
    }
    const fallbackControl = event.target.closest("[data-world-office-fallback]");
    if (fallbackControl) {
      event.preventDefault();
      openFallback(fallbackControl);
      return;
    }
    if (event.target.closest("[data-world-office-close]")) {
      event.preventDefault();
      closeFallback();
      return;
    }
    if (event.target.closest("[data-world-office-exit]")) {
      event.preventDefault();
      collapse();
      return;
    }
    if (event.target.closest("[data-world-office-lobby-exit]")) {
      event.preventDefault();
      completeOfficeExit();
    }
  }

  function onKeyDown(event) {
    if (event.key === "Escape" && fallbackActive) {
      event.preventDefault();
      closeFallback();
      return;
    }
    if (event.key === "Escape" && active) {
      event.preventDefault();
      if (meeting.inRoom) meeting.leaveRoom();
      else collapse();
      return;
    }
  }

  function onMessage(event) {
    if (event.origin !== window.location.origin) return;
    if (!frame || event.source !== frame.contentWindow) return;
    const type = String(event.data?.type || "");
    if (type === "office-chat-ready" && loading) loading.hidden = true;
    if (type !== "office-chat-ready" && type !== "office-chat-room-changed") {
      return;
    }
  }

  function destroy() {
    clearUnloadTimer();
    stopAttendanceHeartbeat();
    tasks?.setActive?.(false);
    world.setOfficeExitHandler?.(null);
    world.setOfficeDoorwayEntryPending?.(false);
    world.setOfficeFloorHandler?.(null);
    root.removeEventListener("click", onClick);
    window.removeEventListener("keydown", onKeyDown);
    window.removeEventListener("message", onMessage);
    document.removeEventListener("visibilitychange", onVisibilityChange);
    meeting.setEntryTicket?.("", 0);
    officeEntryTicket = "";
    officeEntryExpiresAt = 0;
    meetingAuthorizationPromise = null;
    floorAuthorizationPromise = null;
    officeAccess = normalizeOfficeFloorAccess({});
    authorizationGeneration += 1;
    setEntryPending(false);
    world.setOfficeAccess?.(officeAccess);
    world.setOfficeDoorStatus?.("open");
    closeFallback({ restoreFocus: false });
    frame?.removeAttribute("src");
  }

  root.addEventListener("click", onClick);
  world.setOfficeExitHandler?.(completeOfficeExit);
  world.setOfficeFloorHandler?.(travelToOfficeFloor);
  window.addEventListener("keydown", onKeyDown);
  window.addEventListener("message", onMessage);
  document.addEventListener("visibilitychange", onVisibilityChange);

  return {
    setProximity,
    focusOffice,
    hydrateFloorAccess,
    enterOffice,
    restoreSavedView,
    refreshAuthorization,
    authorizeMeeting,
    openFallback,
    collapse,
    destroy,
    get active() {
      return active;
    },
    get proximity() {
      return proximity;
    },
  };
}
