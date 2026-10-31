import { voiceRoomsFromPlayers, worldVoiceRoomLabel } from "./world-voice-rooms.js";

export const WORLD_CHANNELS_SELECTED_KEY = "forkmesh.world.channels.selected";
const REFRESH_MS = 2000;
const ID_RE = /^[0-9a-f]{32}$/;

export function unifiedChannelList({ spaces = [], channels = [], directs = [], rooms = [], selfRoom = "" } = {}) {
  const byRoom = new Map(rooms.map((room) => [room.id, room]));
  const sections = [];
  sections.push({
    id: "world",
    title: "World",
    rows: spaces.map((space) => {
      const voice = space.space ? `space:${space.space}` : "world";
      return {
        id: space.id,
        kind: "space",
        space: space.space,
        label: space.label,
        voice,
        members: byRoom.get(voice)?.members || [],
        joined: selfRoom === voice,
      };
    }),
  });
  const adhoc = rooms.filter((room) => room.id.startsWith("adhoc:") || room.id.startsWith("bubble:"));
  sections.push({
    id: "voice",
    title: "Voice rooms",
    rows: adhoc.map((room) => ({
      id: `voice:${room.id}`,
      kind: room.id.startsWith("bubble:") ? "bubble" : "voice",
      label: room.id.startsWith("bubble:") ? "Privacy bubble" : room.name || "Voice room",
      voice: room.id,
      members: room.members,
      joined: selfRoom === room.id,
      locked: room.id.startsWith("bubble:") && selfRoom !== room.id,
    })),
  });
  sections.push({
    id: "office",
    title: "Office",
    rows: [{ id: "office:general", kind: "office", label: "#general meeting room", room: "general" }],
  });
  sections.push({
    id: "channels",
    title: "Channels",
    rows: channels.filter((channel) => ID_RE.test(channel.roomId)).map((channel) => ({
      id: `channel:${channel.roomId}`,
      kind: "channel",
      roomId: channel.roomId,
      label: channel.label,
      privateRoom: channel.visibility === "private",
      office: channel.roomId,
    })),
  });
  sections.push({
    id: "directs",
    title: "Direct messages",
    rows: directs.filter((direct) => ID_RE.test(direct.roomId)).map((direct) => ({
      id: `dm:${direct.roomId}`,
      kind: "dm",
      roomId: direct.roomId,
      label: direct.label,
      unread: direct.unread || 0,
    })),
  });
  return sections;
}

export function conversationUrl(row) {
  if (row?.kind === "channel" && ID_RE.test(row.roomId)) return `/chat?embed=world#room=${row.roomId}`;
  if (row?.kind === "dm" && ID_RE.test(row.roomId)) return `/chat?embed=world#direct=${row.roomId}`;
  return "";
}

function el(documentRef, tag, className = "", text = "") {
  const node = documentRef.createElement(tag);
  if (className) node.className = className;
  if (text) node.textContent = text;
  return node;
}

export function createWorldChannels(host) {
  const documentRef = host.documentRef || globalThis.document;
  const scope = host.scope || globalThis;
  const panel = host.panel;
  let selected = "";
  let privateChoices = { channels: [], directs: [] };
  let open = false;
  let refreshTimer = 0;
  let frame = null;
  let frameRow = "";
  try {
    selected = scope.localStorage?.getItem(WORLD_CHANNELS_SELECTED_KEY) || "";
  } catch (_) {}

  function remember(id) {
    selected = id;
    try {
      scope.localStorage?.setItem(WORLD_CHANNELS_SELECTED_KEY, id);
    } catch (_) {}
  }

  function sections() {
    const voice = host.voice();
    const rooms = voice ? voice.rooms() : voiceRoomsFromPlayers(host.players());
    return unifiedChannelList({
      spaces: host.spaces(),
      channels: privateChoices.channels,
      directs: privateChoices.directs,
      rooms,
      selfRoom: voice?.room?.id || "",
    });
  }

  function findRow(id) {
    for (const section of sections()) {
      const row = section.rows.find((entry) => entry.id === id);
      if (row) return row;
    }
    return null;
  }

  function iconButton(label, title, onClick, extra = "") {
    const node = el(documentRef, "button", `world-channels-action ${extra}`.trim(), label);
    node.type = "button";
    node.title = title;
    node.setAttribute("aria-label", title);
    node.addEventListener("click", (event) => {
      event.stopPropagation();
      onClick();
    });
    return node;
  }

  function memberList(members) {
    const list = el(documentRef, "ul", "world-channels-voice-members");
    for (const member of members.slice(0, 12)) {
      const item = el(documentRef, "li", "world-channels-voice-member");
      item.dataset.mic = member.mic ? "on" : "off";
      item.append(el(documentRef, "span", "world-channels-voice-dot"));
      item.append(el(documentRef, "span", "", member.self ? `${member.name} (you)` : member.name));
      if (member.cam) item.append(el(documentRef, "span", "world-channels-voice-flag", "camera"));
      list.append(item);
    }
    if (members.length > 12) list.append(el(documentRef, "li", "world-channels-voice-more", `+${members.length - 12} more`));
    return list;
  }

  function joinVoice(row) {
    void host.ensureVoice().then((voice) => {
      if (!voice) return;
      if (row.joined) voice.openPanel(true);
      else void voice.join({ id: row.voice, name: row.kind === "voice" ? row.label : "" }).then(() => render());
    });
  }

  function rowButton(row) {
    const item = el(documentRef, "li", "world-channels-row");
    item.dataset.kind = row.kind;
    item.dataset.selected = row.id === selected ? "true" : "false";
    const main = el(documentRef, "button", "world-channels-row-main");
    main.type = "button";
    main.setAttribute("aria-current", row.id === selected ? "true" : "false");
    const glyph = { space: "#", voice: "🔊", bubble: "🫧", office: "🏢", channel: row.privateRoom ? "🔒" : "#", dm: "@" }[row.kind] || "#";
    main.append(el(documentRef, "span", "world-channels-glyph", glyph));
    main.append(el(documentRef, "span", "world-channels-label", String(row.label || "").replace(/^[#@]/, "")));
    if (row.unread) main.append(el(documentRef, "span", "world-channels-unread", String(Math.min(99, row.unread))));
    if (row.members?.length) main.append(el(documentRef, "span", "world-channels-count", `🔊 ${row.members.length}`));
    main.addEventListener("click", () => select(row.id));
    item.append(main);
    if (row.voice && !row.locked) {
      item.append(iconButton(row.joined ? "●" : "🎙", row.joined ? "You are in this voice room" : `Join voice in ${row.label}`, () => joinVoice(row), row.joined ? "is-joined" : ""));
    }
    if (row.kind === "channel" || row.kind === "office") {
      item.append(iconButton("🏢", `Meet in the Office room for ${row.label}`, () => host.openOffice(row.office || row.room)));
    }
    if (row.members?.length) item.append(memberList(row.members));
    return item;
  }

  function renderList(nav) {
    nav.replaceChildren();
    const head = el(documentRef, "header", "world-channels-head");
    head.append(el(documentRef, "strong", "", "Channels"));
    const close = iconButton("×", "Close channels", () => setOpen(false), "world-channels-close");
    head.append(close);
    nav.append(head);
    for (const section of sections()) {
      const block = el(documentRef, "section", "world-channels-section");
      block.dataset.section = section.id;
      const title = el(documentRef, "h3", "world-channels-section-title", section.title);
      if (section.id === "voice") {
        title.append(iconButton("+", "Start a voice room only the people who join can hear", () => {
          const name = scope.prompt?.("Name the voice room", "Voice room");
          if (name === null || name === undefined) return;
          void host.ensureVoice().then((voice) => voice?.createRoom(name)).then(() => render());
        }, "world-channels-add"));
      }
      block.append(title);
      const list = el(documentRef, "ul", "world-channels-rows");
      if (!section.rows.length) {
        const empty = section.id === "voice"
          ? "No voice rooms yet. Start one, or select someone to begin a privacy bubble."
          : section.id === "channels" || section.id === "directs"
            ? (host.signedIn() ? "Nothing here yet." : "Sign in to see your channels and messages.")
            : "";
        if (empty) list.append(el(documentRef, "li", "world-channels-empty", empty));
      }
      for (const row of section.rows) list.append(rowButton(row));
      block.append(list);
      nav.append(block);
    }
    const foot = el(documentRef, "footer", "world-channels-foot");
    const link = el(documentRef, "a", "world-channels-external", "Open Chat in a new tab");
    link.href = "/chat";
    link.target = "_blank";
    link.rel = "noopener noreferrer";
    foot.append(link);
    nav.append(foot);
  }

  function renderMain(main) {
    const row = findRow(selected) || sections()[0]?.rows[0] || null;
    const url = conversationUrl(row);
    if (url) {
      if (!frame || frameRow !== row.id) {
        main.replaceChildren();
        const head = el(documentRef, "header", "world-channels-main-head");
        head.append(el(documentRef, "strong", "", row.label));
        main.append(head);
        frame = el(documentRef, "iframe", "world-channels-frame");
        frame.title = `Conversation ${row.label}`;
        frame.setAttribute("allow", "microphone; camera; autoplay; display-capture; clipboard-write");
        frame.src = url;
        frameRow = row.id;
        main.append(frame);
      }
      return;
    }
    frame = null;
    frameRow = "";
    main.replaceChildren();
    if (!row) return;
    const head = el(documentRef, "header", "world-channels-main-head");
    head.append(el(documentRef, "strong", "", row.kind === "space" ? row.label : row.label || worldVoiceRoomLabel(row.voice)));
    main.append(head);
    const copy = {
      space: row.voice === "world"
        ? "The whole World's channel. Messages appear in the chat box at the bottom; join voice to talk with everyone."
        : "A World channel. Messages appear in the chat box at the bottom; its voice room is open to everyone in this channel.",
      voice: "A voice room. Only the people who join hear what is said; nothing is broadcast to the whole World.",
      bubble: "A privacy bubble: only the people inside hear each other. Others see the bubble, never the conversation.",
      office: "The Office's general meeting room, with seats around the table.",
    }[row.kind] || "";
    if (copy) main.append(el(documentRef, "p", "world-channels-copy", copy));
    const actions = el(documentRef, "div", "world-channels-main-actions");
    if (row.kind === "space") {
      const chat = el(documentRef, "button", "world-voice-button", "Chat here");
      chat.type = "button";
      chat.addEventListener("click", () => host.selectSpace(row.space));
      actions.append(chat);
    }
    if (row.voice && !row.locked) {
      const voice = el(documentRef, "button", "world-voice-button world-voice-primary", row.joined ? "Voice controls" : "Join voice");
      voice.type = "button";
      voice.addEventListener("click", () => joinVoice(row));
      actions.append(voice);
    }
    if (row.locked) actions.append(el(documentRef, "p", "world-channels-copy", "Ask someone inside to invite you, or select them and knock."));
    if (row.kind === "office") {
      const office = el(documentRef, "button", "world-voice-button", "Go to the meeting room");
      office.type = "button";
      office.addEventListener("click", () => host.openOffice(row.room));
      actions.append(office);
    }
    main.append(actions);
    if (row.members?.length) main.append(memberList(row.members));
  }

  function render() {
    if (!panel) return;
    panel.hidden = !open;
    if (!open) return;
    let nav = panel.querySelector("[data-world-channels-list]");
    let main = panel.querySelector("[data-world-channels-main]");
    if (!nav || !main) {
      panel.replaceChildren();
      nav = el(documentRef, "nav", "world-channels-list");
      nav.dataset.worldChannelsList = "";
      nav.setAttribute("aria-label", "All channels");
      main = el(documentRef, "div", "world-channels-main");
      main.dataset.worldChannelsMain = "";
      panel.append(nav, main);
    }
    renderList(nav);
    renderMain(main);
  }

  function select(id) {
    remember(id);
    const row = findRow(id);
    if (row?.kind === "space") host.selectSpace(row.space);
    render();
  }

  async function refreshPrivate() {
    privateChoices = (await host.loadPrivate().catch(() => null)) || privateChoices;
    render();
  }

  function setOpen(next, { conversation = "" } = {}) {
    open = Boolean(next);
    if (conversation) remember(conversation);
    if (open) {
      void refreshPrivate();
      scope.clearInterval(refreshTimer);
      refreshTimer = scope.setInterval(() => {
        if (open && !frame) render();
        else if (open && panel.querySelector("[data-world-channels-list]")) renderList(panel.querySelector("[data-world-channels-list]"));
      }, REFRESH_MS);
    } else {
      scope.clearInterval(refreshTimer);
      frame = null;
      frameRow = "";
      panel?.replaceChildren();
    }
    render();
    return open;
  }

  return {
    setOpen,
    toggle: () => setOpen(!open),
    render,
    get open() {
      return open;
    },
    sections,
    destroy() {
      scope.clearInterval(refreshTimer);
      panel?.replaceChildren();
    },
  };
}
