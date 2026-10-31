import { createChatRoomTransport } from "../chat-room-transport.js";
import {
  createMeetingBinding,
  signMeetingProof,
  verifyMeetingProof,
} from "../chat-crypto.js";
import {
  attachmentBlobType,
  attachmentBytes,
  attachmentFromEntry,
  fileToAttachment,
  formatAttachmentSize,
} from "../chat-attachments.js";
import {
  createWorldBackoff,
  markWorldHTTPFailure,
  withWorldBackoff,
} from "./world-backoff.js";
import { officeMeetingStandingPoint } from "./world-office-tower.js";

// Room discovery and access run again on every reconnect attempt, so a Worker
// that is refusing meeting grants must not be asked once per retry.
const backoff = createWorldBackoff();

const GENERAL_ROOM = Object.freeze({
  id: "general",
  name: "general",
  visibility: "public",
  kind: "general",
});
// The room expires a participant after 90 seconds without a frame. A
// 40-second keepalive leaves ten seconds beyond one missed tick while cutting
// idle Durable Object wakeups and their authorization reads in half.
const OFFICE_PING_MS = 40000;
// The room retired this socket because the same account joined the meeting
// from another device. One account keeps one seat, so this is a handover to
// report rather than a meeting that ended.
const OFFICE_ACCOUNT_TAKEOVER_CODE = 4009;
const OFFICE_MOVEMENT_SEND_INTERVAL_MS = 1000;
const OFFICE_MOVEMENT_RETRY_MS = 250;
const OFFICE_SOCKET_BUFFER_HIGH_WATER_BYTES = 64 * 1024;
const MEETING_PROOF_MAX_AGE_MS = 2 * 60 * 1000;
const MAX_MEETING_TEXT = 16000;
const PUBLIC_ROOM_KEY_ENDPOINT =
  "/api/chat/room-key?owner=mainnode&repo=forkmesh&room=world-general";
const PUBLIC_CHAT_WEBSOCKET =
  "/api/repo/mainnode/forkmesh/rooms/world-general/ws";
const OFFICE_ENTRY_HEADER = "X-ForkMesh-Office-Entry";

function sessionHeaders(getSession) {
  const headers = new Headers({ accept: "application/json" });
  const token = String(getSession()?.sessionToken || "");
  if (token) headers.set("Authorization", `Bearer ${token}`);
  return headers;
}

function socketURL(path) {
  const url = new URL(String(path || ""), window.location.origin);
  const expectedProtocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  if (url.origin !== window.location.origin) {
    throw new Error("Office meeting socket must be same-origin");
  }
  url.protocol = expectedProtocol;
  return url.href;
}

function boundedParticipant(participant) {
  if (!participant || typeof participant !== "object") return null;
  const id = String(participant.id || "").slice(0, 64);
  if (!id) return null;
  return {
    id,
    name: String(participant.name || "Office visitor").slice(0, 32),
    accountStatus: String(participant.accountStatus || "Guest").slice(0, 32),
    x: Number(participant.x) || 0,
    y: Number(participant.y) || 0.38,
    z: Number(participant.z) || 0,
    yaw: Number(participant.yaw) || 0,
    moving: Boolean(participant.moving),
    pose: participant.pose === "seated" ? "seated" : "standing",
    chairId: /^chair-[1-8]$/.test(String(participant.chairId || ""))
      ? String(participant.chairId)
      : "",
    bindingKey: participant.bindingKey || null,
    updatedAt: Number(participant.updatedAt) || Date.now(),
  };
}

export function createOfficeMovementQueue({
  send = () => false,
  isReady = () => false,
  bufferedAmount = () => 0,
  now = () => performance.now(),
  setTimer = (callback, delay) => window.setTimeout(callback, delay),
  clearTimer = (timer) => window.clearTimeout(timer),
  intervalMs = OFFICE_MOVEMENT_SEND_INTERVAL_MS,
  retryMs = OFFICE_MOVEMENT_RETRY_MS,
  highWaterBytes = OFFICE_SOCKET_BUFFER_HIGH_WATER_BYTES,
} = {}) {
  const sendInterval = Math.max(250, Number(intervalMs) || 1000);
  const retryInterval = Math.max(50, Number(retryMs) || 250);
  const bufferLimit = Math.max(1024, Number(highWaterBytes) || 64 * 1024);
  let pending = null;
  let timer = null;
  let lastSentAt = Number.NEGATIVE_INFINITY;
  let disposed = false;

  function cancelTimer() {
    if (timer === null) return;
    clearTimer(timer);
    timer = null;
  }

  function schedule(delay) {
    if (disposed || timer !== null || !pending) return;
    timer = setTimer(() => {
      timer = null;
      flush();
    }, Math.max(0, Math.ceil(Number(delay) || 0)));
  }

  function flush() {
    cancelTimer();
    if (disposed || !pending || !isReady()) return false;
    if (Math.max(0, Number(bufferedAmount()) || 0) > bufferLimit) {
      schedule(retryInterval);
      return false;
    }
    const frame = pending;
    let sent = false;
    try {
      sent = send(frame) === true;
    } catch (_) {
      sent = false;
    }
    if (!sent) {
      if (isReady()) schedule(retryInterval);
      return false;
    }
    if (pending === frame) pending = null;
    lastSentAt = now();
    return true;
  }

  function queue(frame = {}) {
    if (disposed) return false;
    pending = {
      type: "move",
      x: frame.x,
      y: frame.y,
      z: frame.z,
      yaw: frame.yaw,
      moving: frame.moving === true,
    };
    // The stopped frame closes the interpolation window and must not wait for
    // the ordinary movement cadence. If the socket is backed up, flush() keeps
    // this final (latest) frame queued and retries it once the buffer drains.
    if (!pending.moving) {
      cancelTimer();
      return flush();
    }
    if (!isReady()) return false;
    // A cadence or backpressure retry is already scheduled. Replacing
    // `pending` is sufficient; restarting that timer for every scene frame
    // would let continuous movement postpone the flush forever.
    if (timer !== null) return true;
    const elapsed = Math.max(0, now() - lastSentAt);
    if (elapsed >= sendInterval) return flush();
    schedule(sendInterval - elapsed);
    return true;
  }

  function reset() {
    cancelTimer();
    pending = null;
    lastSentAt = Number.NEGATIVE_INFINITY;
  }

  function destroy() {
    reset();
    disposed = true;
  }

  return { queue, flush, reset, destroy };
}

export function createWorldOfficeMeeting({
  root,
  scene,
  getSession = () => null,
  onActivity = () => {},
  onStepTotal = () => {},
  onLeaveOffice = () => {},
}) {
  const lobby = root.querySelector("[data-world-office-lobby]");
  const roomPanel = root.querySelector("[data-world-office-room]");
  const roomBoard = root.querySelector("[data-world-office-room-board]");
  const lobbyStatus = root.querySelector("[data-world-office-lobby-status]");
  const roomTitle = root.querySelector("[data-world-office-room-title]");
  const roomStatus = root.querySelector("[data-world-office-room-status]");
  const participantList = root.querySelector("[data-world-office-participants]");
  const seatList = root.querySelector("[data-world-office-seats]");
  const standButton = root.querySelector("[data-world-office-stand]");
  const transcript = root.querySelector("[data-world-office-transcript]");
  const input = root.querySelector("[data-world-office-input]");
  const sendButton = root.querySelector("[data-world-office-send]");
  const attachmentInput = root.querySelector("[data-world-office-attachment-input]");
  const attachmentFeedback = root.querySelector("[data-world-office-attachment]");
  const liveRegion = root.querySelector("[data-world-office-live]");

  const participants = new Map();
  const seenMessages = new Set();
  const transcriptURLs = new Set();
  let rooms = [GENERAL_ROOM];
  let activeRoom = null;
  let participantId = "";
  let socket = null;
  let chatTransport = null;
  let meetingBinding = null;
  let chatReady = false;
  let pingTimer = null;
  let leaving = false;
  let entryTicket = "";
  let entryTicketExpiresAt = 0;
  let entryTicketProvider = null;
  const movementQueue = createOfficeMovementQueue({
    send: (frame) => sendMeeting(frame),
    isReady: () => Boolean(socket && socket.readyState === WebSocket.OPEN),
    bufferedAmount: () => Number(socket?.bufferedAmount || 0),
  });

  function setOpen(element, open) {
    if (!element) return;
    element.dataset.open = String(Boolean(open));
    element.setAttribute("aria-hidden", String(!open));
  }

  function setLobbyStatus(message) {
    if (lobbyStatus) lobbyStatus.textContent = String(message || "");
  }

  function setRoomStatus(message) {
    if (roomStatus) roomStatus.textContent = String(message || "");
  }

  function setAttachmentFeedback(message) {
    if (attachmentFeedback) attachmentFeedback.textContent = String(message || "");
  }

  function syncComposer() {
    const enabled = Boolean(chatReady && participantId && meetingBinding && activeRoom);
    if (input) {
      input.disabled = !enabled;
      input.placeholder = activeRoom
        ? `Message #${activeRoom.name}`
        : "Join a meeting to chat";
    }
    if (sendButton) sendButton.disabled = !enabled;
    if (attachmentInput) attachmentInput.disabled = !enabled;
  }

  function clearTranscript() {
    transcript?.replaceChildren();
    seenMessages.clear();
    transcriptURLs.forEach((url) => URL.revokeObjectURL(url));
    transcriptURLs.clear();
    if (liveRegion) liveRegion.textContent = "";
  }

  function renderRoomBoard() {
    if (!roomBoard) return;
    roomBoard.replaceChildren();
    rooms.forEach((room) => {
      const button = document.createElement("button");
      button.type = "button";
      button.dataset.worldOfficeJoin = room.id;
      button.className = "world-office-room-card";
      button.setAttribute("aria-label", `Join #${room.name}`);
      const copy = document.createElement("span");
      copy.innerHTML = `<strong>#${room.name}</strong><small>${
        room.kind === "general"
          ? "Public World room"
          : `${room.visibility === "private" ? "Private" : "Public"} channel`
      }</small>`;
      const action = document.createElement("span");
      action.textContent = "Join";
      action.setAttribute("aria-hidden", "true");
      button.append(copy, action);
      roomBoard.appendChild(button);
    });
  }

  async function fetchRooms() {
    const session = getSession();
    if (!session?.sessionToken) {
      rooms = [GENERAL_ROOM];
      renderRoomBoard();
      return;
    }
    try {
      const payload = await withWorldBackoff(
        backoff,
        "GET:/api/chat/channels",
        async () => {
          const response = await fetch("/api/chat/channels", {
            headers: sessionHeaders(getSession),
            credentials: "same-origin",
            cache: "no-store",
          });
          const body = await response.json().catch(() => ({}));
          if (!response.ok) {
            throw markWorldHTTPFailure(
              new Error(body.error || "unavailable"),
              response,
            );
          }
          return body;
        },
      );
      rooms = [
        GENERAL_ROOM,
        ...(Array.isArray(payload.channels) ? payload.channels : [])
          .filter((channel) => /^[0-9a-f]{32}$/.test(String(channel.id || "")))
          .map((channel) => ({
            id: String(channel.id),
            name: String(channel.name || "channel").slice(0, 32),
            visibility: channel.visibility === "private" ? "private" : "public",
            kind: "channel",
          })),
      ];
    } catch (_) {
      rooms = [GENERAL_ROOM];
      setLobbyStatus("Channels are temporarily unavailable. #general is still open.");
    }
    renderRoomBoard();
  }

  function openLobby() {
    leaving = false;
    root.classList.add("world-office-active");
    scene.enterOfficeLobby();
    setOpen(roomPanel, false);
    // Entry is spatial now: visitors arrive in the physical lobby and use
    // the glass elevator plus the meeting board on Marketing. Keeping the old
    // centered room chooser closed preserves the uninterrupted World view.
    setOpen(lobby, false);
    setLobbyStatus(
      "Ride the glass elevator to a floor your team is on. " +
        "The meeting board stands on Marketing.",
    );
    onActivity("visiting-office");
    renderRoomBoard();
    void fetchRooms();
    return true;
  }

  async function roomAccess(room) {
    const path = room.kind === "general"
      ? "/api/world/office/general/access"
      : `/api/chat/channels/${encodeURIComponent(room.id)}/room-access`;
    const headers = sessionHeaders(getSession);
    if (
      room.kind === "general" &&
      entryTicket &&
      entryTicketExpiresAt > Date.now() &&
      entryTicket.length <= 2048
    ) {
      headers.set(OFFICE_ENTRY_HEADER, entryTicket);
    }
    return withWorldBackoff(backoff, `GET:${path}`, async () => {
      const response = await fetch(path, {
        headers,
        credentials: "same-origin",
        cache: "no-store",
      });
      const payload = await response.json().catch(() => ({}));
      if (!response.ok || !payload.meetingWebSocketUrl) {
        const error = new Error(payload.error || "meeting_unavailable");
        error.code = response.status === 401 ? "auth" : payload.error || "unavailable";
        throw markWorldHTTPFailure(error, response);
      }
      return payload;
    });
  }

  async function chatRoomAccess(room) {
    if (room.kind === "general") {
      const payload = await withWorldBackoff(
        backoff,
        `GET:${PUBLIC_ROOM_KEY_ENDPOINT}`,
        async () => {
          const response = await fetch(PUBLIC_ROOM_KEY_ENDPOINT, {
            headers: sessionHeaders(getSession),
            credentials: "same-origin",
            cache: "no-store",
          });
          const body = await response.json().catch(() => ({}));
          if (!response.ok || !body.passphrase) {
            const error = new Error(body.error || "chat_unavailable");
            error.code = response.status === 401 ? "auth" : "unavailable";
            throw markWorldHTTPFailure(error, response);
          }
          return body;
        },
      );
      return {
        scope: "public-world-general",
        passphrase: String(payload.passphrase),
        roomName: "world-general",
        webSocketUrl: PUBLIC_CHAT_WEBSOCKET,
      };
    }
    const access = await roomAccess(room);
    if (!access.passphrase || !access.room || !access.webSocketUrl) {
      throw new Error("Channel chat access is unavailable");
    }
    return {
      scope: room.id,
      passphrase: String(access.passphrase),
      roomName: String(access.room),
      webSocketUrl: String(access.webSocketUrl),
    };
  }

  function chatProofInput(plain, participantIdForProof) {
    const attachment = attachmentFromEntry(plain);
    return {
      participantId: participantIdForProof,
      messageId: String(plain.id || ""),
      senderId: String(plain.senderId || ""),
      ts: Number(plain.ts),
      text: String(plain.text || ""),
      attachment,
    };
  }

  async function verifiedBubbleParticipant(plain) {
    const proof = plain?.meetingProof;
    const proofParticipantId = String(proof?.participantId || "");
    const participant = participants.get(proofParticipantId);
    if (!participant?.bindingKey) return null;
    const timestamp = Number(plain.ts);
    if (
      !Number.isSafeInteger(timestamp) ||
      Math.abs(Date.now() - timestamp) > MEETING_PROOF_MAX_AGE_MS
    ) {
      return null;
    }
    const valid = await verifyMeetingProof(
      participant.bindingKey,
      proof,
      chatProofInput(plain, proofParticipantId),
    );
    return valid ? participant : null;
  }

  function attachmentElement(attachment) {
    const bytes = attachmentBytes(attachment);
    if (!bytes) return null;
    const blobType = attachmentBlobType(attachment.fileMime);
    const blob = new Blob([bytes], { type: blobType });
    const url = URL.createObjectURL(blob);
    transcriptURLs.add(url);
    if (blobType.startsWith("image/")) {
      const image = document.createElement("img");
      image.className = "world-office-transcript-image";
      image.src = url;
      image.alt = attachment.fileName;
      image.loading = "eager";
      return image;
    }
    const link = document.createElement("a");
    link.className = "world-office-transcript-file";
    link.href = url;
    link.download = attachment.fileName;
    const icon = document.createElement("b");
    icon.textContent = "DOC";
    const name = document.createElement("span");
    name.textContent = attachment.fileName;
    const size = document.createElement("small");
    size.textContent = formatAttachmentSize(attachment.size);
    link.append(icon, name, size);
    return link;
  }

  async function renderChatPlain(plain) {
    if (!plain || plain.type !== "chat") return;
    const messageId = String(plain.id || "");
    if (!/^[A-Za-z0-9_-]{1,64}$/.test(messageId) || seenMessages.has(messageId)) {
      return;
    }
    const participant = await verifiedBubbleParticipant(plain);
    // Only the ephemeral key published through the authoritative meeting
    // socket may bind chat to an avatar. An arbitrary encrypted-room member
    // cannot claim a live participant's senderId/name, reserve a message id,
    // or place a bubble over somebody else's head.
    if (!participant) return;
    seenMessages.add(messageId);
    const sender = participant.name;
    const text = String(plain.text || "").slice(0, MAX_MEETING_TEXT);
    const attachment = attachmentFromEntry(plain);
    const item = document.createElement("li");
    item.dataset.worldOfficeMessage = messageId;
    const meta = document.createElement("span");
    meta.className = "world-office-transcript-meta";
    const author = document.createElement("strong");
    author.textContent = sender;
    const time = document.createElement("time");
    time.dateTime = new Date(Number(plain.ts) || Date.now()).toISOString();
    time.textContent = new Intl.DateTimeFormat(undefined, {
      hour: "numeric",
      minute: "2-digit",
    }).format(new Date(Number(plain.ts) || Date.now()));
    meta.append(author, time);
    const body = document.createElement("span");
    body.className = "world-office-transcript-body";
    if (text) {
      const copy = document.createElement("span");
      copy.textContent = text;
      body.appendChild(copy);
    }
    if (attachment) {
      const attachmentNode = attachmentElement(attachment);
      if (attachmentNode) body.appendChild(attachmentNode);
    }
    item.append(meta, body);
    transcript?.appendChild(item);
    item.scrollIntoView({ block: "nearest" });
    const summary = text || (attachment ? `shared ${attachment.fileName}` : "sent a message");
    if (liveRegion) liveRegion.textContent = `${sender}: ${summary}`;
    if (participant) {
      scene.showOfficeBubble(participant.id, {
        text: text || "",
        fileName: attachment?.fileName || "",
        fileMime: attachment?.fileMime || "",
      });
    }
  }

  function newMessageId() {
    return (crypto.randomUUID?.() || `${Date.now()}-${Math.random()}`)
      .replace(/[^A-Za-z0-9_-]/g, "")
      .slice(0, 64);
  }

  async function sendChat({ text = "", attachment = null } = {}) {
    if (!chatTransport?.connected || !participantId || !meetingBinding) return false;
    const session = getSession();
    const plain = {
      type: "chat",
      id: newMessageId(),
      senderId: participantId,
      sender: String(session?.nodeName || "World visitor").slice(0, 32),
      accountKind: session?.sessionToken ? "user" : "guest",
      channel: `#${activeRoom?.name || "general"}`,
      text: String(text || "").slice(0, MAX_MEETING_TEXT),
      ts: Date.now(),
    };
    if (attachment) {
      plain.fileName = attachment.fileName;
      plain.fileMime = attachment.fileMime;
      plain.file = attachment.file;
    }
    plain.meetingProof = await signMeetingProof(
      meetingBinding.privateKey,
      chatProofInput(plain, participantId),
    );
    const sent = await chatTransport.send(plain, { persist: true });
    if (!sent) return false;
    await renderChatPlain(plain);
    return true;
  }

  async function sendCurrentMessage() {
    const text = String(input?.value || "").trim();
    if (!text) return;
    if (await sendChat({ text })) {
      input.value = "";
      setAttachmentFeedback("");
    }
  }

  async function sendAttachment(file) {
    if (!file) return;
    try {
      const attachment = await fileToAttachment(file);
      if (await sendChat({ attachment })) {
        setAttachmentFeedback(`Shared ${attachment.fileName}`);
      } else {
        setAttachmentFeedback("The attachment could not be sent.");
      }
    } catch (error) {
      setAttachmentFeedback(error?.message || "The attachment could not be read.");
    }
  }

  function createRoomChatTransport(room) {
    chatTransport?.dispose();
    chatReady = false;
    syncComposer();
    chatTransport = createChatRoomTransport({
      authorize: () => chatRoomAccess(room),
      onPlain: (plain) => renderChatPlain(plain),
      onState: (state) => {
        chatReady = state === "connected";
        syncComposer();
        if (state === "connected") {
          chatTransport.send({
            type: "hello",
            id: newMessageId(),
            senderId: participantId || "office-joining",
            sender: String(getSession()?.nodeName || "World visitor").slice(0, 32),
            channel: `#${room.name}`,
            ts: Date.now(),
          });
        } else if (state === "unauthorized") {
          setRoomStatus("Chat authorization expired. Return to the lobby to rejoin.");
        } else if (state === "reconnecting") {
          setRoomStatus("Chat reconnecting...");
        }
      },
    });
    chatTransport.connect({ scope: room.id, kind: room.kind });
  }

  function localParticipant() {
    const session = getSession();
    // (0, 4.1) is the middle of the Marketing room, which is the middle of the
    // round meeting table. Join on the open floor in front of it instead.
    const standing = officeMeetingStandingPoint(0, 4.1);
    return boundedParticipant({
      id: participantId,
      name: session?.nodeName || "World visitor",
      accountStatus: session?.sessionToken ? "Registered" : "Guest",
      x: standing.x,
      y: 0.38,
      z: standing.z,
      yaw: Math.PI,
      pose: "standing",
      chairId: "",
      bindingKey: meetingBinding?.publicJwk || null,
    });
  }

  function renderParticipants() {
    const entries = Array.from(participants.values());
    scene.setOfficeParticipants(entries);
    if (participantList) {
      participantList.replaceChildren();
      entries.forEach((participant) => {
        const item = document.createElement("li");
        item.dataset.worldOfficeParticipant = participant.id;
        const state = participant.pose === "seated"
          ? `Seated at ${participant.chairId.replace("chair-", "chair ")}`
          : "Standing";
        item.innerHTML = `<span aria-hidden="true">${
          participant.pose === "seated" ? "●" : "○"
        }</span><span><strong></strong><small></small></span>`;
        item.querySelector("strong").textContent = participant.name;
        item.querySelector("small").textContent = state;
        participantList.appendChild(item);
      });
    }
    const self = participants.get(participantId);
    const occupied = new Map(
      entries
        .filter((participant) => participant.chairId)
        .map((participant) => [participant.chairId, participant.id]),
    );
    seatList?.querySelectorAll("[data-world-office-seat]").forEach((button) => {
      const chairId = button.dataset.worldOfficeSeat;
      const owner = occupied.get(chairId) || "";
      const mine = owner === participantId;
      button.disabled = Boolean(owner && !mine);
      button.setAttribute("aria-pressed", String(mine));
      button.dataset.occupied = String(Boolean(owner));
    });
    if (standButton) standButton.hidden = self?.pose !== "seated";
  }

  function receiveMeetingFrame(frame) {
    if (!frame || typeof frame !== "object") return;
    if (frame.type === "step-total") {
      onStepTotal(frame);
      return;
    }
    if (frame.type === "welcome") {
      participantId = String(frame.id || "").slice(0, 64);
      participants.clear();
      (Array.isArray(frame.participants) ? frame.participants : []).forEach((entry) => {
        const participant = boundedParticipant(entry);
        if (participant) participants.set(participant.id, participant);
      });
      const self = localParticipant();
      if (self) participants.set(self.id, self);
      scene.enterOfficeMeeting({
        roomName: activeRoom?.name || "general",
        participants: Array.from(participants.values()),
        participantId,
      });
      setRoomStatus("Meeting connected");
      renderParticipants();
      sendMeeting({
        type: "presence",
        bindingKey: meetingBinding?.publicJwk || null,
      });
      syncComposer();
      return;
    }
    if (frame.type === "join" || frame.type === "presence" || frame.type === "seat") {
      const participant = boundedParticipant(frame.participant);
      if (!participant) return;
      participants.set(participant.id, participant);
      if (frame.type === "seat") {
        scene.setOfficeSeatState({
          participantId: participant.id,
          chairId: participant.chairId,
          pose: participant.pose,
        });
        if (participant.id === participantId) {
          setRoomStatus(
            participant.pose === "seated"
              ? `Seated at ${participant.chairId.replace("-", " ")}`
              : "Standing",
          );
        }
      }
      renderParticipants();
      return;
    }
    if (frame.type === "move") {
      const current = participants.get(String(frame.id || ""));
      if (!current || current.pose === "seated") return;
      const participant = boundedParticipant({ ...current, ...frame });
      if (participant) participants.set(participant.id, participant);
      renderParticipants();
      return;
    }
    if (frame.type === "leave") {
      participants.delete(String(frame.id || ""));
      renderParticipants();
      return;
    }
    if (frame.type === "seat-denied") {
      setRoomStatus(frame.message || "That seat was just taken.");
    }
  }

  function sendMeeting(frame) {
    if (!socket || socket.readyState !== WebSocket.OPEN) return false;
    socket.send(JSON.stringify(frame));
    return true;
  }

  async function joinRoom(roomId) {
    if (!rooms.some((candidate) => candidate.id === String(roomId || ""))) await fetchRooms();
    const room = rooms.find((candidate) => candidate.id === String(roomId || ""));
    if (!room || socket) return false;
    activeRoom = room;
    clearTranscript();
    chatReady = false;
    syncComposer();
    setLobbyStatus(`Opening #${room.name}...`);
    if (
      room.kind === "general" &&
      (
        !entryTicket ||
        entryTicketExpiresAt <= Date.now() + 5000
      )
    ) {
      const authorized =
        typeof entryTicketProvider === "function"
          ? await entryTicketProvider()
          : false;
      if (
        !authorized ||
        !entryTicket ||
        entryTicketExpiresAt <= Date.now() + 5000
      ) {
        setLobbyStatus("Sign in to join this meeting.");
        activeRoom = null;
        return false;
      }
    }
    try {
      meetingBinding = await createMeetingBinding();
    } catch (_) {
      setLobbyStatus("This browser could not create a private meeting identity.");
      activeRoom = null;
      return false;
    }
    let access;
    try {
      access = await roomAccess(room);
    } catch (error) {
      setLobbyStatus(
        error?.code === "auth"
          ? "Sign in again to enter this meeting."
          : "This meeting could not be opened.",
      );
      activeRoom = null;
      meetingBinding = null;
      return false;
    }
    let meetingSocket;
    try {
      meetingSocket = new WebSocket(socketURL(access.meetingWebSocketUrl));
    } catch (_) {
      setLobbyStatus("This meeting could not be opened.");
      activeRoom = null;
      meetingBinding = null;
      return false;
    }
    socket = meetingSocket;
    createRoomChatTransport(room);
    setOpen(lobby, false);
    setOpen(roomPanel, true);
    if (roomTitle) roomTitle.textContent = `#${room.name}`;
    setRoomStatus("Joining meeting...");
    scene.enterOfficeMeeting({ roomName: room.name, participants: [] });
    meetingSocket.addEventListener("message", (event) => {
      if (socket !== meetingSocket) return;
      let frame;
      try {
        frame = JSON.parse(event.data);
      } catch (_) {
        return;
      }
      receiveMeetingFrame(frame);
    });
    meetingSocket.addEventListener("open", () => {
      if (socket !== meetingSocket) return;
      window.clearInterval(pingTimer);
      pingTimer = window.setInterval(() => sendMeeting({ type: "ping" }), OFFICE_PING_MS);
    });
    meetingSocket.addEventListener("close", (event) => {
      if (socket !== meetingSocket) return;
      movementQueue.reset();
      socket = null;
      chatTransport?.dispose();
      chatTransport = null;
      chatReady = false;
      meetingBinding = null;
      syncComposer();
      window.clearInterval(pingTimer);
      pingTimer = null;
      participants.clear();
      participantId = "";
      activeRoom = null;
      scene.setOfficeParticipants([]);
      renderParticipants();
      clearTranscript();
      setAttachmentFeedback("");
      if (!leaving) {
        setLobbyStatus(
          event?.code === OFFICE_ACCOUNT_TAKEOVER_CODE
            ? "You joined this meeting on another device. Select the " +
              "Marketing meeting board to bring it back here."
            : "The meeting ended. Select the Marketing meeting board to rejoin.",
        );
        setOpen(roomPanel, false);
        setOpen(lobby, false);
        scene.enterOfficeLobby({ floorId: "marketing" });
      }
    });
    meetingSocket.addEventListener("error", () => {
      setRoomStatus("Meeting connection interrupted");
      try {
        meetingSocket.close();
      } catch (_) {}
    });
    return true;
  }

  function requestSeat(chairId) {
    const normalized = String(chairId || "");
    if (!/^chair-[1-8]$/.test(normalized)) return false;
    setRoomStatus(`Requesting ${normalized.replace("-", " ")}...`);
    return sendMeeting({ type: "seat-request", chairId: normalized });
  }

  function stand() {
    setRoomStatus("Standing up...");
    return sendMeeting({ type: "seat-request", chairId: "" });
  }

  function move(movement = {}) {
    const self = participants.get(participantId);
    if (!self || self.pose === "seated") return false;
    const next = boundedParticipant({
      ...self,
      x: movement.x,
      y: movement.y,
      z: movement.z,
      yaw: movement.yaw,
      moving: movement.moving,
      updatedAt: Date.now(),
    });
    if (!next) return false;
    participants.set(participantId, next);
    return movementQueue.queue({
      type: "move",
      x: next.x,
      y: next.y,
      z: next.z,
      yaw: next.yaw,
      moving: next.moving,
    });
  }

  function leaveRoom() {
    leaving = true;
    window.clearInterval(pingTimer);
    pingTimer = null;
    movementQueue.reset();
    const previous = socket;
    socket = null;
    chatTransport?.dispose();
    chatTransport = null;
    chatReady = false;
    meetingBinding = null;
    try {
      previous?.close(1000, "left meeting");
    } catch (_) {}
    participants.clear();
    participantId = "";
    activeRoom = null;
    clearTranscript();
    setAttachmentFeedback("");
    syncComposer();
    scene.enterOfficeLobby({ floorId: "marketing" });
    setOpen(roomPanel, false);
    setOpen(lobby, false);
    leaving = false;
    return true;
  }

  function leaveOffice() {
    if (activeRoom || socket) leaveRoom();
    setOpen(roomPanel, false);
    setOpen(lobby, false);
    scene.leaveOfficeInterior();
    entryTicket = "";
    entryTicketExpiresAt = 0;
    root.classList.remove("world-office-active");
    onActivity("exploring-town-square");
    onLeaveOffice();
    return true;
  }

  function onClick(event) {
    const join = event.target.closest("[data-world-office-join]");
    if (join) {
      joinRoom(join.dataset.worldOfficeJoin);
      return;
    }
    const seat = event.target.closest("[data-world-office-seat]");
    if (seat) {
      requestSeat(seat.dataset.worldOfficeSeat);
      return;
    }
    if (event.target.closest("[data-world-office-stand]")) {
      stand();
      return;
    }
    if (event.target.closest("[data-world-office-leave-room]")) {
      leaveRoom();
    }
  }

  function onInputKeyDown(event) {
    if (event.key !== "Enter" || event.shiftKey || event.isComposing) return;
    event.preventDefault();
    sendCurrentMessage();
  }

  function onPaste(event) {
    const item = Array.from(event.clipboardData?.items || []).find((candidate) =>
      candidate.kind === "file" && String(candidate.type || "").startsWith("image/"));
    const file = item?.getAsFile();
    if (!file) return;
    event.preventDefault();
    sendAttachment(file);
  }

  function onAttachmentChange() {
    const file = attachmentInput?.files?.[0] || null;
    if (file) sendAttachment(file);
    if (attachmentInput) attachmentInput.value = "";
  }

  function onSendClick() {
    sendCurrentMessage();
  }

  function destroy() {
    leaving = true;
    window.clearInterval(pingTimer);
    movementQueue.destroy();
    root.removeEventListener("click", onClick);
    input?.removeEventListener("keydown", onInputKeyDown);
    input?.removeEventListener("paste", onPaste);
    attachmentInput?.removeEventListener("change", onAttachmentChange);
    sendButton?.removeEventListener("click", onSendClick);
    try {
      socket?.close(1000, "Office destroyed");
    } catch (_) {}
    chatTransport?.dispose();
    chatTransport = null;
    socket = null;
    entryTicket = "";
    entryTicketExpiresAt = 0;
    entryTicketProvider = null;
    clearTranscript();
    participants.clear();
    scene.leaveOfficeInterior();
    root.classList.remove("world-office-active");
  }

  root.addEventListener("click", onClick);
  input?.addEventListener("keydown", onInputKeyDown);
  input.addEventListener("paste", onPaste);
  attachmentInput.addEventListener("change", onAttachmentChange);
  sendButton?.addEventListener("click", onSendClick);
  renderRoomBoard();
  syncComposer();

  return {
    setEntryTicket(ticket, expiresAt) {
      const normalized = String(ticket || "").trim();
      const expiry = Number(expiresAt);
      entryTicket =
        normalized && normalized.length <= 2048 && Number.isFinite(expiry)
          ? normalized
          : "";
      entryTicketExpiresAt = entryTicket ? expiry : 0;
      return Boolean(entryTicket);
    },
    setEntryTicketProvider(provider) {
      entryTicketProvider =
        typeof provider === "function" ? provider : null;
      return Boolean(entryTicketProvider);
    },
    openLobby,
    joinRoom,
    requestSeat,
    stand,
    move,
    leaveRoom,
    leaveOffice,
    destroy,
    get active() {
      return lobby?.dataset.open === "true" || roomPanel?.dataset.open === "true";
    },
    get inRoom() {
      return Boolean(activeRoom && socket);
    },
  };
}
