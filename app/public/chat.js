import {
  b64ToBytes,
  b64UrlToBytes as b64urlToBytes,
  bytesToB64,
  decryptObject,
  derivePassphraseKey,
} from "./chat-crypto.js";
import {
  MAX_ATTACHMENT_BYTES,
  attachmentBlobType,
  attachmentFromEntry,
  attachmentFiles,
  formatAttachmentSize,
  safeAttachmentMime,
  safeAttachmentName,
} from "./chat-attachments.js";
import {
  linkSelection,
  plainTextFromRichSource,
  renderRichText,
  toggleLinePrefix,
  wrapSelection,
} from "./chat-rich-text.js";
import {
  fileWebIssue,
  issueFiledSummary,
  issueBodyFromMessage,
  issueTitleFromMessage,
  loadIssueRepositories,
} from "./chat-issue-filing.js";
import { createAccountEventChannel } from "./account-events.js";
import { createChatRoomTransport } from "./chat-room-transport.js";
import { createRoomSealStore } from "./chat-seal.js";
import { moderateChatPlain, moderateChatText } from "./chat-moderation.js";
import { celebrateNewMember, NEW_MEMBER_GREETING } from "./member-welcome.js";
import { createThreadStore } from "./chat-thread-model.js";
import {
  CALL_FRAME_TYPES,
  callsSupported,
  createChatCalls,
} from "./chat-calls.js";
import {
  CHAT_RETENTION_ENDPOINT,
  CHAT_RETENTION_PRESETS,
  chatRetentionLabel,
  createChatRetention,
} from "./chat-retention.js";
import {
  dateDividerLabel,
  localDateKey,
  sameLocalDate,
} from "./chat-date-groups.js";
import {
  VIEW_ONCE_KINDS,
  createViewOnceClient,
  forgetViewOnceKey,
  isFinalViewOnceState,
  openViewOnceViewer,
  paintViewOnceBubble,
  renderViewOnceBubble,
  viewOnceFromEntry,
  viewOnceStateAfterStatus,
} from "./chat-view-once.js";
import {
  openViewOnceRecorder,
  viewOnceRecordingSupported,
} from "./chat-view-once-recorder.js";
import { installViewOnceFilters, viewOnceFilterChoices } from "./chat-view-once-filters.js";

// Browser-side ForkMesh room chat. Reimplements the desktop client's room
// crypto (PBKDF2 + AES-256-GCM) and message envelope so the website can join
// the public encrypted rooms and talk to connected clients. Frames are
// ciphertext on the wire, but the relay derives the default shared passphrase.
// Public channels intentionally stop at that controller-readable layer.
// chat-seal.js automatically adds a second layer with signed, post-quantum device keys
// for private channels and DMs; participants never configure a phrase.
//
// Layout: rooms on the left, conversation in the middle (ts-ordered, newest at
// the bottom, avatars + timestamps + reactions), and people on the right.
// Public #general uses its own "world-general" room. Each authorized private
// channel uses a ticketed room and key version returned by the relay.

const ROOM_NAME = "general";
const PUBLIC_WORLD_GENERAL_ROOM = "world-general";
// The room key is no longer a public constant. Every client fetches a shared
// passphrase (derived server-side from the relay's DATA_KEY) from
// /api/chat/room-key and feeds it into the same PBKDF2 room-key derivation, so
// clients in a given scope converge on the same AES key. The public World
// #general endpoint is intentionally guest-readable; every other room-key
// request still requires repository-authorized authentication. The relay
// controls DATA_KEY and can derive either key too. Keys are cached by scope.
const PUBLIC_WORLD_ROOM_KEY_ENDPOINT =
  "/api/chat/room-key?owner=mainnode&repo=forkmesh&room=world-general";
const PUBLIC_WORLD_CHAT_WS_PATH =
  "/api/repo/mainnode/forkmesh/rooms/world-general/ws";
const PRIVATE_CHANNELS_ENDPOINT = "/api/chat/channels";
const EVERYONE_CHANNEL_ORDER = Object.freeze(["general", "random", "welcome"]);
const EVERYONE_MEMBERS_REFRESH_MS = 5 * 60 * 1000;
const DIRECT_MESSAGES_ENDPOINT = "/api/chat/direct-messages";
const CHAT_READS_ENDPOINT = "/api/chat/reads";
const READ_MARK_KEY_RE =
  /^(#[a-z0-9][a-z0-9_-]{0,63}|private:[0-9a-f]{32}|direct:[0-9a-f]{32})$/;
const READ_MARK_POST_DELAY_MS = 1000;
const READ_MARK_POST_MAX = 50;
const READ_MARK_FUTURE_SLACK_MS = 24 * 60 * 60 * 1000;
const CHAT_INVITES_ENDPOINT = "/api/chat/invites";
const CHAT_INVITE_HASH_RE = /^#invite=([0-9a-f]{64})$/;
const CHAT_ROOM_HASH_RE = /^#room=([0-9a-f]{32})$/;
const CALL_ICE_SERVERS = [{ urls: "stun:stun.cloudflare.com:3478" }];
const FORKBOT_ENDPOINT = "/api/forkbot/chat";
// Cloudflare Workers AI models ForkBot can be pointed at, plus the picked one.
// The pick is per-browser (not per-room): it only decides which model this
// client's own mentions are sent to, and the relay re-validates it.
const FORKBOT_MODELS_ENDPOINT = "/api/forkbot/models";
const FORKBOT_MODEL_KEY = "forkmesh.forkbot.model";
const FORKBOT_SENDER_ID = "forkbot";
const FORKBOT_MENTION_RE = /(?:^|[^A-Za-z0-9_-])@?forkbot\b/i;
// Mainnode base host for the room WebSocket. Defaults to the origin that served
// this page, so a self-hosted mainnode's own site talks to itself with zero
// config. A separately-hosted static site can point at a different relay by
// setting window.FORKMESH_RELAY_HOST (e.g. "relay.example.com") before this
// script loads. See public docs: /docs#self-hosting.
const RELAY_HOST = window.FORKMESH_RELAY_HOST || location.host;
const MAX_TEXT = 16000;
const MAX_NAME = 32;
const MAX_ACCOUNT_NAME = 63;
const CHAT_MENTION_RE = /(^|[^A-Za-z0-9_-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)\b/gi;
// Presence cadence + staleness mirror the desktop node (ServerNode.cpp:
// kPresenceIntervalMs / kPeerStaleMs). The beat doubles as the keep-alive the
// relay's room DO needs — it closes sockets with no frames for 3 minutes, which
// is why the old web client (which sent nothing while idle) kept disconnecting.
const PRESENCE_INTERVAL_MS = 60000;
// Registered-user directory (public, no secrets — see _account_users_directory
// in the Worker). Seeds the people pane so every account shows even before it
// has ever spoken in the room, and a light poll (the endpoint is edge-cached,
// and a signup invalidates that cache) surfaces brand-new signups right away
// with a welcome line in the log.
const USERS_DIRECTORY_ENDPOINT = "/api/accounts/users";
const USERS_DIRECTORY_REFRESH_MS = 5 * 60 * 1000;
// Counters behind the site-header chat badge; the chat page re-baselines them
// while open so time spent reading here counts as "seen".
const CHAT_ACTIVITY_ENDPOINT = "/api/chat/activity";
const CHAT_ACTIVITY_SEEN_KEY = "forkmesh.chat.activitySeen";
// Only a directory entry created this recently gets the "just joined" welcome
// line — an account merely missing from the previous (top-1000) page isn't news.
const NEW_USER_ANNOUNCE_WINDOW_MS = 10 * 60 * 1000;
const PEER_STALE_MS = 180000;
// Guests and World visitors are throwaway browser sessions: their id dies with
// the tab, so an offline row for one can never come back and the pane filled up
// with dozens of dead "Guest ####" people. Forget them after this much silence
// (registered accounts and nodes still keep their offline row) — adhoc #404.
const VISITOR_IDLE_FORGET_MS = 10 * 60 * 1000;
const GROUP_WINDOW_MS = 5 * 60 * 1000; // same-sender messages collapse under one header
const REACTION_EMOJI = ["👍", "❤️", "😂", "🎉", "👀", "🚀"];
const ACTIVE_CHANNEL_KEY = "forkmesh.chat.channel";
const ACTIVE_DIRECT_KEY = "forkmesh.chat.direct";
const ISSUE_REPOSITORY_KEY = "forkmesh.chat.issueRepository";
const SLASH_COMMANDS = Object.freeze([
  {
    command: "/issue",
    usage: "/issue [owner/repo] <title>",
    description: "Create a repository issue",
  },
  {
    command: "/topic",
    usage: "/topic <text>",
    description: "Change a managed channel topic",
  },
]);
const CHAT_SIGN_IN_REQUIRED = "Sign in again to join chat";
const OFFICE_SESSION_EXPIRED =
  "Your session expired. Log in again to use authorized channels.";
const OFFICE_RELAY_UNAVAILABLE = "Chat relay unavailable. Try again.";
const chatQuery = new URLSearchParams(window.location.search);
const isOfficeEmbed = chatQuery.get("embed") === "office";
// The Office embed is the organization's own room list, so channel creation
// there is a membership capability. The role is read from the Office's own
// access endpoint — never from the embedding page — and re-checked by the
// channels API on every write.
const OFFICE_FLOOR_ACCESS_ENDPOINT = "/api/world/office/floors";
// organization.chat.channels in the Worker's organization_rbac matrix:
// auditors are read-only and guests are not insiders, so neither creates.
const OFFICE_ORGANIZATION_ROLES = new Set(["owner", "admin", "maintainer", "member"]);
let officeOrganizationMember = false;

const logEl = document.querySelector("#chat-log");
const mainPane = document.querySelector(".chat-main-pane");
const nameInput = document.querySelector("#chat-name");
const input = document.querySelector("#chat-input");
const peopleCountEl = document.querySelector("#chat-people-count");
const peopleSearchInput = document.querySelector("#chat-people-search");
const slashSuggestEl = document.querySelector("#chat-slash-suggest");
const sendBtn = document.querySelector("#chat-send");
const clearBtn = document.querySelector("#chat-clear");
const formatButtons = [...document.querySelectorAll("[data-format]")];
const attachmentInput = document.querySelector("#chat-attachment-input");
const attachmentBtn = document.querySelector("#chat-attachment-button");
const viewOnceVideoBtn = document.querySelector("#chat-view-once-video");
const viewOnceVoiceBtn = document.querySelector("#chat-view-once-voice");
const attachmentFeedback = document.querySelector("#chat-attachment-feedback");
const attachmentDraftEl = document.querySelector("#chat-composer-attachments");
const composerEmojiBtn = document.querySelector("#chat-emoji-button");
const composerEmojiMenu = document.querySelector("#chat-composer-emoji-menu");
const composerEmojiButtons = [...document.querySelectorAll("[data-composer-emoji]")];
const composerMentionBtn = document.querySelector("#chat-mention-button");
const forkbotModelSelect = document.querySelector("#chat-forkbot-model");
const dropOverlay = document.querySelector("#chat-drop-overlay");
const statusEl = document.querySelector("#chat-status");
const roomsEl = document.querySelector("#chat-rooms");
const peopleEl = document.querySelector("#chat-people");
const peopleTitleEl = document.querySelector("#chat-people-title");
const roomFilterInput = document.querySelector("#chat-room-filter");
const roomFilterEmpty = document.querySelector("#chat-room-filter-empty");
const panesEl = document.querySelector(".chat-panes");
const peopleView = document.querySelector(".chat-people-view");
const threadView = document.querySelector(".chat-thread-view");
const threadTitleEl = document.querySelector("#chat-thread-title");
const threadCountEl = document.querySelector("#chat-thread-count");
const threadRootEl = document.querySelector("#chat-thread-root");
const threadRepliesEl = document.querySelector("#chat-thread-replies");
const threadInput = document.querySelector("#chat-thread-input");
const threadSendBtn = document.querySelector("#chat-thread-send");
const threadAttachmentInput = document.querySelector("#chat-thread-attachment-input");
const threadAttachmentBtn = document.querySelector("#chat-thread-attachment-button");
const threadAttachmentDraftEl = document.querySelector("#chat-thread-attachments");
const threadCloseBtn = document.querySelector("#chat-thread-close");
const channelTitleEl = document.querySelector("#chat-channel-title");
const channelTopicEl = document.querySelector("#chat-channel-topic");
const channelVisibilityBadge = document.querySelector("#chat-channel-visibility-badge");
const directServerCopiesEl = document.querySelector("#chat-direct-server-copies");
const channelCreateBtn = document.querySelector("#chat-channel-create");
const channelManageBtn = document.querySelector("#chat-channel-manage");
const chatRetentionSelect = document.querySelector("#chat-retention-select");
const chatRetention = createChatRetention({
  request: (body) => privateChannelRequest(CHAT_RETENTION_ENDPOINT, {
    method: "POST",
    body: JSON.stringify(body),
  }),
  storage: (() => {
    try {
      return window.localStorage;
    } catch (_) {
      return null;
    }
  })(),
});
const channelDialog = document.querySelector("#chat-channel-dialog");
const channelDialogClose = document.querySelector("#chat-channel-dialog-close");
const channelError = document.querySelector("#chat-channel-error");
const channelCreateSection = document.querySelector("#chat-channel-create-section");
const channelCreateForm = document.querySelector("#chat-channel-create-form");
const channelNameInput = document.querySelector("#chat-channel-name");
const channelVisibilityInput = document.querySelector("#chat-channel-visibility");
const channelInitialMembersInput = document.querySelector("#chat-channel-initial-members");
const channelMembersSection = document.querySelector("#chat-channel-members-section");
const channelMembersTitle = document.querySelector("#chat-channel-members-title");
const channelInviteForm = document.querySelector("#chat-channel-invite-form");
const channelUsernameInput = document.querySelector("#chat-channel-username");
const channelMembersEl = document.querySelector("#chat-channel-members");
const channelSettingsPanel = document.querySelector("#chat-channel-settings");
const channelSettingsClose = document.querySelector("#chat-channel-settings-close");
const channelSettingsError = document.querySelector("#chat-channel-settings-error");
const channelSettingsForm = document.querySelector("#chat-channel-settings-form");
const channelSettingsName = document.querySelector("#chat-channel-settings-name");
const channelSettingsTopic = document.querySelector("#chat-channel-settings-topic");
const channelSettingsVisibility = document.querySelector("#chat-channel-settings-visibility");
const channelSettingsSaved = document.querySelector("#chat-channel-settings-saved");
const channelDeleteName = document.querySelector("#chat-channel-delete-name");
const channelDeleteBtn = document.querySelector("#chat-channel-delete");
const channelDeleteConfirm = document.querySelector("#chat-channel-delete-confirm");
const channelDeleteCancel = document.querySelector("#chat-channel-delete-cancel");
const channelDeleteConfirmed = document.querySelector("#chat-channel-delete-confirmed");
const channelWebhooksSection = document.querySelector("#chat-channel-webhooks-section");
const channelWebhookForm = document.querySelector("#chat-channel-webhook-form");
const channelWebhookNameInput = document.querySelector("#chat-channel-webhook-name");
const channelWebhookCreated = document.querySelector("#chat-channel-webhook-created");
const channelWebhooksEl = document.querySelector("#chat-channel-webhooks");
const directSection = document.querySelector("#chat-direct-section");
const directCreateBtn = document.querySelector("#chat-direct-create");
const directListEl = document.querySelector("#chat-direct-list");
const directMoreBtn = document.querySelector("#chat-direct-more");
const directDialog = document.querySelector("#chat-direct-dialog");
const directDialogTitle = document.querySelector("#chat-direct-dialog-title");
const directDialogHint = document.querySelector("#chat-direct-dialog-hint");
const directDialogClose = document.querySelector("#chat-direct-dialog-close");
const directAddBtn = document.querySelector("#chat-direct-add");
const callAudioBtn = document.querySelector("#chat-call-audio");
const callVideoBtn = document.querySelector("#chat-call-video");
const directSearch = document.querySelector("#chat-direct-search");
const directOptions = document.querySelector("#chat-direct-options");
const directEmpty = document.querySelector("#chat-direct-empty");
const directError = document.querySelector("#chat-direct-error");
const sealBtn = document.querySelector("#chat-seal");
const sealIcon = document.querySelector("#chat-seal-icon");
const sealLabel = document.querySelector("#chat-seal-label");
const sealPanel = document.querySelector("#chat-seal-panel");
const sealPanelClose = document.querySelector("#chat-seal-close");
const sealStateEl = document.querySelector("#chat-seal-state");
const safetySection = document.querySelector("#chat-safety");
const safetyLead = document.querySelector("#chat-safety-lead");
const safetyDigits = document.querySelector("#chat-safety-digits");
const safetyQr = document.querySelector("#chat-safety-qr");
const safetyState = document.querySelector("#chat-safety-state");
const safetyVerifyBtn = document.querySelector("#chat-safety-verify");
const trustBanner = document.querySelector("#chat-trust-banner");
const trustBannerCopy = document.querySelector("#chat-trust-banner-copy");
const trustSafetyBtn = document.querySelector("#chat-trust-safety");
const trustApproveBtn = document.querySelector("#chat-trust-approve");
const trustBlockBtn = document.querySelector("#chat-trust-block");
const officeManageLink = document.querySelector("#chat-office-manage");
const officeAlert = document.querySelector("#chat-office-alert");
const officeAlertCopy = document.querySelector("#chat-office-alert-copy");
const officeLoginLink = document.querySelector("#chat-office-login");
const officeRetryBtn = document.querySelector("#chat-office-retry");
const deleteDialog = document.querySelector("#chat-delete-dialog");
const deleteCancelBtn = document.querySelector("#chat-delete-cancel");
const deleteConfirmBtn = document.querySelector("#chat-delete-confirm");
const joinDialog = document.querySelector("#chat-join-dialog");
const joinTitle = document.querySelector("#chat-join-title");
const joinCopy = document.querySelector("#chat-join-copy");
const joinConfirmBtn = document.querySelector("#chat-join-confirm");
const joinCancelBtn = document.querySelector("#chat-join-cancel");
const inviteLinkForm = document.querySelector("#chat-channel-invite-link-form");
const inviteLinkInput = document.querySelector("#chat-channel-invite-link");
const inviteLinkCopyBtn = document.querySelector("#chat-channel-invite-link-copy");
const inviteLinkResetBtn = document.querySelector("#chat-channel-invite-link-reset");
const inviteLinkOffBtn = document.querySelector("#chat-channel-invite-link-off");
const inviteLinkStatus = document.querySelector("#chat-channel-invite-link-status");
const composerControls = [...new Set([
  input,
  sendBtn,
  attachmentBtn,
  attachmentInput,
  composerEmojiBtn,
  composerMentionBtn,
  ...formatButtons,
  ...composerEmojiButtons,
].filter(Boolean))];

const enc = new TextEncoder();
// A stable per-browser chat id. The relay holds no roster — every participant
// is reconstructed client-side from the senderId on decrypted frames — so a
// fresh random id per page load made each reload/tab of the same person show up
// as a brand-new "ghost" participant (the "lots of jett users" symptom).
// Persisting it collapses reloads/tabs of the same browser into one entry;
// signed-in users still display under their account name via displayName().
const selfId = (() => {
  const STORAGE_KEY = "forkmesh.chat.selfId";
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) return saved;
  } catch (_) {}
  const fresh =
    (crypto.randomUUID && crypto.randomUUID()) ||
    String(Math.random()).slice(2) + Date.now();
  try { localStorage.setItem(STORAGE_KEY, fresh); } catch (_) {}
  return fresh;
})();

const roomPassphrases = new Map();
const roomKeys = new Map();
// One automatically generated device key plus public keys learned per room.
// Private key material stays in this browser; only signed public bundles travel.
const roomSeals = createRoomSealStore({
  acct: () => userSession()?.nodeName || "",
  node: selfId,
});
const trustNotices = new Map();
const trustNoticeKeys = new Set();
roomSeals.subscribe((event) => handleSealEvent(event));
// Per scope, why frames were dropped: sender ids whose content key was not
// wrapped for this browser, and whether an unsealed frame was refused. Both
// leave a reader staring at what looks like a broken relay.
const sealMismatches = new Map();
const privateChannels = new Map();
// webhookId -> the URL this tab minted for it. Tokens are stored server-side
// only as a blind index, so a URL exists here just until the page reloads --
// after that a manager rotates the webhook to get a working one back.
const revealedWebhookUrls = new Map();
const directMessages = new Map();
const directSearchResults = new Map();
const directReadPending = new Set();
const directReadDeferred = new Set();
const directServerCopies = new Map();
let directNextCursor = "";
let directSearchTimer = null;
let directSearchRequest = 0;
let directDialogMode = "new";
let directMessagesRevision = 0;
let roomTransport = null;
let openCallbacks = [];
let cachedUserSession = null;
let chatBadgeActivityAccount = "";
let chatSuspended = false;
let officeAuthorizationExpired = false;
let memberPlanTier = "";
let memberPlanLoading = null;
const seen = new Set();
// messageId -> message record { id, channel, ts, senderId, sender, text, self,
// el, body, reactionsEl }. `el`/`body` reference the on-screen row while the
// record's channel is the active one, so an edit or a (regular / admin) delete
// can find and update or remove the right row.
const rows = new Map();
const threads = createThreadStore();
const threadRenderedRows = new Map();
const threadDrafts = new Map();
const attachmentDrafts = new Map();
let attachmentDraftSequence = 0;
const threadAttachmentRecords = new Set();
let activeThreadRootId = "";
let threadReturnFocus = null;
// channel -> ts-ordered array of message records (newest last).
const channelMessages = new Map();
// channel -> idle while unseen, loading while its socket is opening, ready once
// the room has connected or supplied a frame. Cached messages remain usable
// across room switches even while the next room is synchronizing.
const channelSyncState = new Map();
// channel -> { unread } for the rooms list badges.
const channelMeta = new Map();
const readMarks = new Map();
const pendingReadMarks = new Map();
let readMarkTimer = 0;
const roomPreviewChannels = new Set();
let roomPreviewTimer = 0;
// messageId -> Map(emoji -> Map(reactorId -> reactorName)); same shape as the
// desktop's m_reactions so toggles converge across clients.
const reactions = new Map();
// senderId -> { id, name, kind: "user"|"guest"|"bot"|"node", lastSeenMs } built from
// every decrypted frame; drives the right-hand people pane.
const roster = new Map();
let activeChannel = "#general";
let defaultChannelChosen = false;
const everyoneChannelMembers = new Map();
let savedPrivateChannelId = "";
let savedDirectMessageId = "";
try {
  const saved = localStorage.getItem(ACTIVE_CHANNEL_KEY);
  if (/^[0-9a-f]{32}$/.test(saved || "")) savedPrivateChannelId = saved;
  const savedDirect = localStorage.getItem(ACTIVE_DIRECT_KEY);
  if (/^[0-9a-f]{32}$/.test(savedDirect || "")) {
    savedDirectMessageId = savedDirect;
  }
} catch (_) {}
// Rolling per-channel buffer of recent messages. Public channels can forward
// it to ForkBot so it can resolve references like "that bug"; protected
// conversations never cross that controller integration boundary.
const recentContext = new Map(); // channel -> [{sender, text}]
const RECENT_CONTEXT_MAX = 20;
function rememberContext(channel, sender, text) {
  const clean = String(text || "").trim();
  if (!clean) return;
  const list = recentContext.get(channel) || [];
  recentContext.set(channel, list);
  list.push({ sender: String(sender || "").slice(0, MAX_NAME), text: clean });
  if (list.length > RECENT_CONTEXT_MAX) list.shift();
}
const mentionProfileCache = new Map();
let mentionCardEl = null;
let activeMentionAnchor = null;
let mentionHideTimer = null;
let emojiPickerEl = null;
let emojiPickerTarget = null;
let pendingDeleteRecord = null;
let pendingInviteToken = "";
let pendingDeleteTrigger = null;
let dragDepth = 0;
let dateRefreshTimer = null;
let activeComposerPopoverTrigger = null;

function setAttachmentFeedback(message) {
  if (!attachmentFeedback) return;
  attachmentFeedback.textContent = String(message || "");
  if (message) {
    setTimeout(() => {
      if (attachmentFeedback.textContent === message) {
        attachmentFeedback.textContent = "";
      }
    }, 5000);
  }
}

// Verify an Ed25519 signature (raw 32-byte key, 64-byte sig) over a UTF-8
// string. Returns false on any unsupported-browser / malformed-input error, so
// a delete we can't authenticate is simply ignored rather than applied.
async function ed25519Verify(pubB64url, sigB64url, dataStr) {
  try {
    const key = await crypto.subtle.importKey(
      "raw",
      b64urlToBytes(pubB64url),
      { name: "Ed25519" },
      false,
      ["verify"]
    );
    return await crypto.subtle.verify(
      { name: "Ed25519" },
      key,
      b64urlToBytes(sigB64url),
      enc.encode(dataStr)
    );
  } catch (error) {
    return false;
  }
}

// ---- room crypto (matches RoomCrypto.cpp) -----------------------------------

function roomScopeForChannel(channel = activeChannel) {
  return channel === "#general" ? "public-world-general" : channel;
}

// Public is an explicit channel policy, not something a relay frame can opt
// into. Those channels use the controller-readable room layer so integrations
// can participate; private channels and DMs require the automatic device seal.
function controllerReadableScope(scope = roomScopeForChannel()) {
  if (scope === "public-world-general") return true;
  const record = isPrivateChannelKey(scope) ? privateChannelForKey(scope) : null;
  return Boolean(record) &&
    (record.visibility === "public" || record.serverVisible === true);
}

function isPrivateChannelKey(channel) {
  return /^private:[0-9a-f]{32}$/.test(String(channel || ""));
}

function privateChannelKey(channelId) {
  return "private:" + String(channelId || "");
}

function privateChannelForKey(channel) {
  if (!isPrivateChannelKey(channel)) return null;
  return privateChannels.get(String(channel).slice("private:".length)) || null;
}

function isDirectMessageKey(channel) {
  return /^direct:[0-9a-f]{32}$/.test(String(channel || ""));
}

function directMessageKey(conversationId) {
  return "direct:" + String(conversationId || "");
}

function directMessageForKey(channel) {
  if (!isDirectMessageKey(channel)) return null;
  return directMessages.get(String(channel).slice("direct:".length)) || null;
}

function channelDisplayLabel(channel = activeChannel) {
  if (channel === "#general") return "World";
  const direct = directMessageForKey(channel);
  if (direct) {
    const record = direct;
    return "@" + record.otherUser;
  }
  const record = privateChannelForKey(channel);
  return record ? "#" + record.name : "Channel";
}

function channelWireLabel(channel = activeChannel) {
  if (channel === "#general") return channel;
  const direct = directMessageForKey(channel);
  return direct ? "direct:" + direct.id : channelDisplayLabel(channel);
}

function isEveryoneChannel(record) {
  return record?.kind === "everyone";
}

function defaultChannelKey() {
  if (!userSession()) return "#general";
  for (const record of privateChannels.values()) {
    if (isEveryoneChannel(record) && record.name === EVERYONE_CHANNEL_ORDER[0]) {
      return privateChannelKey(record.id);
    }
  }
  return "#general";
}

function canJoinChannel(channel = activeChannel) {
  return roomScopeForChannel(channel) === "public-world-general" ||
    Boolean(userSession() && (
      privateChannelForKey(channel) || directMessageForKey(channel)
    ));
}

// The public World #general passphrase is intentionally available to guests
// and is isolated in its own DO room. Private channel keys come only from the
// authenticated room-access endpoint below.
async function fetchRoomPassphrase(scope = roomScopeForChannel()) {
  if (scope !== "public-world-general") {
    throw new Error("Private channel room access required.");
  }
  if (roomPassphrases.has(scope)) return roomPassphrases.get(scope);
  const headers = { accept: "application/json" };
  const res = await fetch(PUBLIC_WORLD_ROOM_KEY_ENDPOINT, {
    headers,
    cache: "no-store",
  });
  if (!res.ok) {
    const err = new Error("World chat key unavailable.");
    err.code = res.status === 401 || res.status === 403 ? "auth" : "server";
    throw err;
  }
  const data = await res.json().catch(() => ({}));
  if (!data || !data.passphrase) throw new Error("Room key unavailable.");
  const passphrase = String(data.passphrase);
  roomPassphrases.set(scope, passphrase);
  return passphrase;
}

async function deriveRoomKey(scope = roomScopeForChannel()) {
  if (scope !== "public-world-general") {
    throw new Error("Private channel room access required.");
  }
  if (roomKeys.has(scope)) return roomKeys.get(scope);
  const passphrase = await fetchRoomPassphrase(scope);
  const key = await derivePassphraseKey(passphrase, PUBLIC_WORLD_GENERAL_ROOM);
  roomKeys.set(scope, key);
  return key;
}

async function privateChannelRequest(path, options = {}) {
  const session = userSession();
  if (!session?.sessionToken) {
    const error = new Error("Sign in required");
    error.code = "auth";
    throw error;
  }
  const headers = new Headers(options.headers || {});
  headers.set("accept", "application/json");
  headers.set("Authorization", `Bearer ${session.sessionToken}`);
  if (options.body) headers.set("content-type", "application/json");
  let response;
  try {
    response = await fetch(path, {
      ...options,
      headers,
      cache: "no-store",
    });
  } catch (_) {
    const error = new Error("unavailable");
    error.code = "unavailable";
    throw error;
  }
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const error = new Error(data.error || "unavailable");
    error.code = response.status === 401 ? "auth" :
      data.error || (response.status >= 500 ? "unavailable" : "request_failed");
    error.status = response.status;
    throw error;
  }
  return data;
}

async function fetchRoomAccess(channelKey = activeChannel) {
  const direct = directMessageForKey(channelKey);
  if (direct) {
    const access = await privateChannelRequest(
      DIRECT_MESSAGES_ENDPOINT + "/" + direct.id + "/room-access"
    );
    if (!access.passphrase || !access.room || !access.webSocketUrl) {
      throw new Error("Direct message room access unavailable.");
    }
    return access;
  }
  const channel = privateChannelForKey(channelKey);
  if (!channel) throw new Error("Channel unavailable.");
  const access = await privateChannelRequest(
    PRIVATE_CHANNELS_ENDPOINT + "/" + channel.id + "/room-access"
  );
  if (!access.passphrase || !access.room || !access.webSocketUrl) {
    throw new Error("Channel room access unavailable.");
  }
  return access;
}

// ---- session ----------------------------------------------------------------

function readSession() {
  try {
    return JSON.parse(localStorage.getItem("forkmesh.session") || "null");
  } catch (error) {
    return null;
  }
}

function writeSession(session) {
  try {
    const stored = session && typeof session === "object"
      ? {
          ...session,
          sessionToken: (
            location.protocol === "https:" && session.sessionToken
              ? "cookie"
              : session.sessionToken || ""
          ),
        }
      : session;
    localStorage.setItem("forkmesh.session", JSON.stringify(stored));
  } catch (error) {
    /* storage disabled */
  }
}

function isUserLikeSession(session) {
  if (!session || !session.nodeName) return false;
  if (session.kind === "user") return true;
  if (session.kind === "node") return false;
  // Legacy browser sessions created before account kind was persisted still
  // carry email + nodeName for user accounts. Accept them, then hydrate below.
  return Boolean(session.email);
}

async function hydrateUserSession() {
  const session = readSession();
  if (isUserLikeSession(session)) {
    cachedUserSession = session;
    if (session.kind === "user") return session;
  } else if (!session || !session.nodeName || session.kind === "node") {
    cachedUserSession = null;
    return null;
  }
  try {
    const res = await fetch("/api/accounts/" + encodeURIComponent(session.nodeName), {
      headers: { accept: "application/json" },
    });
    if (!res.ok) return cachedUserSession;
    const body = await res.json();
    if (!body || body.kind !== "user") {
      cachedUserSession = null;
      return null;
    }
    cachedUserSession = {
      ...session,
      nodeName: body.nodeName || session.nodeName,
      email: body.email || session.email || "",
      status: body.status || session.status || "active",
      pubkey: body.pubkey || session.pubkey || "",
      emailVerified: Boolean(body.emailVerified),
      isAdmin: Boolean(body.isAdmin),
      adminUrl: body.adminUrl || session.adminUrl || "",
      solana: body.solana || session.solana || "",
      hasPayoutAddress: Boolean(body.hasPayoutAddress),
      avatarPng: body.avatarPng || session.avatarPng || "",
      avatarUpdatedAt: Number(body.avatarUpdatedAt) || 0,
      kind: "user",
      owner: body.owner || "",
      nodes: Array.isArray(body.nodes) ? body.nodes : [],
      accountSnapshotVersion: 1,
      at: Date.now(),
    };
    writeSession(cachedUserSession);
    return cachedUserSession;
  } catch (error) {
    return cachedUserSession;
  }
}

function userSession() {
  if (isUserLikeSession(cachedUserSession)) return cachedUserSession;
  const session = readSession();
  return isUserLikeSession(session) ? session : null;
}

async function loadMemberPlanTier() {
  const session = userSession();
  const token = String(session?.sessionToken || "");
  if (!token) {
    memberPlanTier = "";
    return "";
  }
  if (memberPlanLoading) return memberPlanLoading;
  memberPlanLoading = (async () => {
    try {
      // The plan surface answers from the role grant a membership leaves
      // behind, so the chip does not depend on any one payment provider.
      const response = await fetch("/api/plans/usage", {
        method: "GET",
        headers: {
          accept: "application/json",
          ...(token !== "cookie"
            ? { authorization: "Bearer " + token }
            : {}),
        },
        credentials: "same-origin",
        cache: "no-store",
      });
      const data = await response.json().catch(() => ({}));
      const held = data?.membership || data?.promotion || null;
      if (response.ok && (held?.tier === "pro" || held?.tier === "supporter")) {
        memberPlanTier = held.tier;
        document.documentElement.dataset.fmPlan = held.tier;
      } else {
        memberPlanTier = "";
        delete document.documentElement.dataset.fmPlan;
      }
    } catch (_) {
      memberPlanTier = "";
    } finally {
      memberPlanLoading = null;
      renderPeople();
    }
    return memberPlanTier;
  })();
  return memberPlanLoading;
}

// Everyone in the public World room shows under the plain name they assert.
// The old "World visitor · jett" prefix read as a second, different person
// sitting next to the signed-in "jett", so it is stripped from anything that
// still carries it (session names, replayed history frames).
function publicWorldName(value) {
  return String(value || "")
    .replace(/^World visitor\s*·\s*/i, "")
    .trim()
    .slice(0, MAX_NAME) || "guest";
}

function displayName() {
  const session = userSession();
  let value;
  if (session) {
    value = String(session.nodeName).trim().slice(0, MAX_NAME);
  } else {
    value = (nameInput.value || "").trim();
    value = (value || "web-guest").slice(0, MAX_NAME);
  }
  return roomScopeForChannel() === "public-world-general"
    ? publicWorldName(value)
    : value;
}

function setStatus(text, state = "idle") {
  if (!statusEl) return;
  const compactConnectedStatus = state === "connected" && !isOfficeEmbed;
  statusEl.textContent = compactConnectedStatus ? "Connected" : text;
  statusEl.title = compactConnectedStatus ? text : "";
  statusEl.dataset.state = state;
}

function showOfficeFailure(kind = "") {
  if (!isOfficeEmbed || !officeAlert) return;
  const expired = kind === "auth";
  const relay = kind === "relay";
  officeAlert.hidden = !expired && !relay;
  if (officeAlertCopy) {
    officeAlertCopy.textContent = expired
      ? OFFICE_SESSION_EXPIRED
      : relay
        ? OFFICE_RELAY_UNAVAILABLE
        : "";
  }
  if (officeLoginLink) officeLoginLink.hidden = !expired;
  if (officeRetryBtn) officeRetryBtn.hidden = !relay;
}

function lockChatForNonUser() {
  setStatus("User login required for this channel", "authorization");
  [...composerControls, nameInput].forEach((el) => {
    if (el) el.disabled = true;
  });
  closeComposerPopovers();
  if (logEl) {
    const empty = logEl.querySelector(".chat-empty");
    if (empty) {
      empty.textContent = (
        "Log in as a user to join this channel. " +
        "Guests can chat only in World."
      );
    }
  }
}

function unlockChatForUser() {
  composerControls.forEach((el) => {
    if (el) el.disabled = false;
  });
  const session = userSession();
  if (nameInput) {
    if (session) {
      nameInput.value = session.nodeName || "";
      nameInput.disabled = true;
    } else {
      nameInput.value =
        `World Guest ${String(selfId).replace(/[^A-Za-z0-9]/g, "").slice(0, 6)}`;
      nameInput.disabled = true;
    }
  }
}

function clearEmpty() {
  const empty = logEl.querySelector(".chat-empty");
  if (empty) empty.remove();
}

// ---- avatars ------------------------------------------------------------------

const AVATAR_COLORS = [
  "#4f7ddb", "#8250df", "#bf3989", "#cf222e",
  "#bc4c00", "#4d7c0f", "#1f883d", "#0f766e",
];

function avatarColor(name) {
  const key = String(name || "").toLowerCase();
  let hash = 0;
  for (let i = 0; i < key.length; i += 1) hash = (hash * 31 + key.charCodeAt(i)) >>> 0;
  return AVATAR_COLORS[hash % AVATAR_COLORS.length];
}

// Portraits already resolved this page, by lowercase account name: "" for an
// account with no uploaded picture. fetchMentionProfile caches its promise, but
// a promise still resolves a tick late, so a re-rendered log would flash back
// to letters before settling. This lets a repeat render paint the portrait in
// the same tick it builds the row.
const avatarPortraits = new Map();

function avatarImageSource(png) {
  const value = String(png || "");
  if (!value) return "";
  return value.startsWith("data:") ? value : "data:image/png;base64," + value;
}

function paintAvatarPortrait(el, src) {
  if (!src || el.firstElementChild?.tagName === "IMG") return;
  const img = document.createElement("img");
  img.alt = "";
  img.src = src;
  el.textContent = "";
  el.style.background = "transparent";
  el.append(img);
}

// Stroke icons (24px grid, Lucide geometry) for the controls chat.js builds
// itself. Built node by node so no markup string is ever parsed.
const CHAT_ICON_PATHS = {
  hash: ["M4 9h16", "M4 15h16", "M10 3 8 21", "M16 3l-2 18"],
  lock: ["M5 11h14v10H5z", "M8 11V7a4 4 0 0 1 8 0v4"],
  thread: ["M21 15a2 2 0 0 1-2 2H7l-4 4V5a2 2 0 0 1 2-2h14a2 2 0 0 1 2 2z"],
  react: [
    "M22 11v1a10 10 0 1 1-9-10", "M8 14s1.5 2 4 2 4-2 4-2",
    "M9 9h.01", "M15 9h.01", "M16 5h6", "M19 2v6",
  ],
  issue: [
    "M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20z",
    "M12 13a1 1 0 1 0 0-2 1 1 0 0 0 0 2z",
  ],
  edit: [
    "M21.17 6.81a1 1 0 0 0-3.99-3.99L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z",
    "m15 5 4 4",
  ],
  trash: [
    "M3 6h18", "M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6",
    "M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2",
  ],
  copy: [
    "M10 8h9a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2h-9a2 2 0 0 1-2-2v-9a2 2 0 0 1 2-2z",
    "M4 16a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2",
  ],
  check: ["M20 6 9 17l-5-5"],
  globe: [
    "M12 22a10 10 0 1 0 0-20 10 10 0 0 0 0 20z",
    "M2 12h20",
    "M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20",
  ],
  gear: [
    "M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z",
    "M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z",
  ],
  mail: [
    "M4 4h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z",
    "m22 6-10 7L2 6",
  ],
};

function chatIcon(name, size = 16) {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("width", String(size));
  svg.setAttribute("height", String(size));
  svg.setAttribute("viewBox", "0 0 24 24");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "2");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  for (const d of CHAT_ICON_PATHS[name] || []) {
    const path = document.createElementNS(ns, "path");
    path.setAttribute("d", d);
    svg.append(path);
  }
  return svg;
}

// Letter avatar immediately; asynchronously swapped for the account's uploaded
// avatarPng when the profile has one (user accounts only — fetchMentionProfile
// resolves null for nodes, which keep the letter).
function makeAvatar(name, kind) {
  const el = document.createElement("div");
  el.className = "chat-avatar";
  const display = String(name || "?").trim() || "?";
  el.style.background = avatarColor(display);
  el.textContent = kind === "bot" ? "🤖" : display.charAt(0).toUpperCase();
  if (kind === "bot") return el;
  const known = avatarPortraits.get(mentionName(display));
  if (known !== undefined) {
    paintAvatarPortrait(el, known);
    return el;
  }
  fetchMentionProfile(display).then((profile) => {
    const src = avatarImageSource(profile && profile.avatarPng);
    avatarPortraits.set(mentionName(display), src);
    paintAvatarPortrait(el, src);
  });
  return el;
}

function fmtTime(ts) {
  const date = new Date(Number(ts) || Date.now());
  return date.toLocaleTimeString("en-US", {
    hour: "numeric", minute: "2-digit", hour12: true,
  }).toLowerCase().replace(/\s+/g, "");
}

// ---- mentions -----------------------------------------------------------------

function mentionName(value) {
  return String(value || "").trim().toLowerCase();
}

function mentionProfilePath(name) {
  const key = mentionName(name);
  if (!key) return "#";
  const official = new Set([
    "app.forkmesh.com", "forkmesh.com", "world.forkmesh.com",
  ]).has(location.hostname.toLowerCase());
  const origin = official ? "https://forkmesh.com" : location.origin;
  return origin + "/@" + encodeURIComponent(key);
}

async function fetchMentionProfile(name) {
  const key = mentionName(name);
  if (!key) return null;
  if (mentionProfileCache.has(key)) return mentionProfileCache.get(key);
  const session = userSession();
  if (
    session?.nodeName?.toLowerCase() === key &&
    Number(session.accountSnapshotVersion) >= 1
  ) {
    const ownProfile = Promise.resolve(session);
    mentionProfileCache.set(key, ownProfile);
    return ownProfile;
  }
  const pending = fetch("/api/accounts/" + encodeURIComponent(key), {
    headers: { accept: "application/json" },
  }).then(async (response) => {
    if (!response.ok) return null;
    const profile = await response.json().catch(() => null);
    if (!profile || profile.exists === false || profile.kind !== "user") return null;
    return profile;
  }).catch(() => null);
  mentionProfileCache.set(key, pending);
  return pending;
}

function mentionSummary(profile) {
  if (!profile) return "User profile";
  const bio = String(profile.profileBio || "").trim();
  if (bio) return bio.length > 120 ? bio.slice(0, 117) + "..." : bio;
  const nodes = Array.isArray(profile.nodes)
    ? profile.nodes.map((node) => String(node || "").trim()).filter(Boolean)
    : [];
  if (nodes.length) return "Nodes: " + nodes.slice(0, 3).join(", ");
  return profile.status ? String(profile.status) : "User profile";
}

function canMessagePerson(name) {
  const session = userSession();
  if (!session || isOfficeEmbed) return false;
  const key = mentionName(name);
  const self = String(session.nodeName || "").trim().toLowerCase();
  return Boolean(key) && key !== self && key !== FORKBOT_SENDER_ID;
}

function ensureMentionCard() {
  if (mentionCardEl) return mentionCardEl;
  mentionCardEl = document.createElement("div");
  mentionCardEl.className = "chat-mention-card";
  mentionCardEl.hidden = true;
  mentionCardEl.addEventListener("mouseenter", () => {
    if (mentionHideTimer) clearTimeout(mentionHideTimer);
  });
  mentionCardEl.addEventListener("mouseleave", hideMentionCardSoon);
  document.body.append(mentionCardEl);
  return mentionCardEl;
}

function renderMentionCard(card, name, profile) {
  const display = String(profile?.name || profile?.nodeName || name || "").trim() || name;
  card.textContent = "";
  const title = document.createElement("strong");
  title.textContent = "@" + display;
  const summary = document.createElement("span");
  summary.textContent = mentionSummary(profile);
  const action = document.createElement("div");
  action.className = "chat-mention-card-actions";
  const profileLink = document.createElement("a");
  profileLink.href = mentionProfilePath(name);
  profileLink.textContent = "Open profile";
  action.append(profileLink);
  if (canMessagePerson(name)) {
    const message = document.createElement("button");
    message.type = "button";
    message.className = "chat-mention-message";
    message.textContent = "Message";
    message.setAttribute("aria-label", `Message ${display}`);
    message.addEventListener("click", (event) => {
      event.preventDefault();
      card.hidden = true;
      startDirectMessage(name);
    });
    action.append(message);
  }
  card.append(title);
  // The profile lookup is fresher than the directory for this one member.
  if (profile?.userStatus) {
    window.ForkMeshUserStatus?.remember(name, profile.userStatus);
  }
  const statusLabel = window.ForkMeshUserStatus?.describe(
    window.ForkMeshUserStatus.forName(name));
  if (statusLabel) {
    const status = document.createElement("span");
    status.className = "chat-mention-status";
    status.textContent = statusLabel;
    card.append(status);
  }
  card.append(summary, action);
}

function positionMentionCard(anchor, card) {
  const rect = anchor.getBoundingClientRect();
  const margin = 12;
  const width = card.offsetWidth || 240;
  const x = Math.max(margin, Math.min(rect.left, window.innerWidth - width - margin));
  const y = Math.min(rect.bottom + 8, window.innerHeight - card.offsetHeight - margin);
  card.style.left = `${x + window.scrollX}px`;
  card.style.top = `${Math.max(margin, y) + window.scrollY}px`;
}

function hideMentionCardSoon() {
  if (mentionHideTimer) clearTimeout(mentionHideTimer);
  mentionHideTimer = setTimeout(() => {
    if (mentionCardEl) mentionCardEl.hidden = true;
    activeMentionAnchor = null;
  }, 140);
}

async function showMentionCard(anchor, name) {
  const key = mentionName(name);
  if (!key) return;
  if (mentionHideTimer) clearTimeout(mentionHideTimer);
  activeMentionAnchor = anchor;
  const card = ensureMentionCard();
  renderMentionCard(card, key, null);
  card.hidden = false;
  positionMentionCard(anchor, card);
  const profile = await fetchMentionProfile(key);
  if (activeMentionAnchor !== anchor) return;
  renderMentionCard(card, key, profile);
  positionMentionCard(anchor, card);
}

function makeMentionAnchor(text, value = text) {
  const name = mentionName(value);
  const anchor = document.createElement("a");
  anchor.className = "chat-mention";
  anchor.href = mentionProfilePath(name);
  anchor.dataset.chatMention = name;
  anchor.textContent = text;
  anchor.addEventListener("mouseenter", () => showMentionCard(anchor, name));
  anchor.addEventListener("focus", () => showMentionCard(anchor, name));
  anchor.addEventListener("mouseleave", hideMentionCardSoon);
  anchor.addEventListener("blur", hideMentionCardSoon);
  return anchor;
}

function renderMessageText(container, message) {
  const record = typeof message === "string" ? { text: message } : message;
  const blocksOnly = Boolean(record?.appMessage?.notice && window.ForkMeshChatApps);
  if (!blocksOnly) {
    renderRichText(container, record, {
      renderMention: (_parent, text, name) => makeMentionAnchor(text, name),
    });
  }
  if (record?.appMessage && window.ForkMeshChatApps) {
    window.ForkMeshChatApps.renderBlocks(container, record.appMessage, appsChannelContext());
  }
}

// ---- @mention autocomplete -----------------------------------------------------
// Typing "@" (plus an optional partial name) in the composer pops a suggestion
// list built from the roster; Tab (or Enter / click) accepts the highlighted
// name, arrows move, Escape dismisses.

let mentionSuggestEl = null;
// null when closed, else { items, index, start, end } where start/end bound the
// "@partial" token in the input's value.
let mentionSuggest = null;
let slashSuggest = null;

// Suggestion rows are picked by pointer, not by mouse alone: "pointerdown"
// covers mouse, pen, and touch, "mousedown" covers engines without pointer
// events, and "click" covers the ones that swallow the synthetic mousedown a
// tap produces. preventDefault keeps focus - and the caret - in the composer,
// so no blur fires to close the popup underneath the press. Accepting is a
// no-op once the popup has closed, so the events that follow the first one
// cannot insert a second name.
function wireSuggestPick(item, accept) {
  const pick = (event) => {
    if (event.cancelable) event.preventDefault();
    accept();
  };
  item.addEventListener("pointerdown", pick);
  item.addEventListener("mousedown", pick);
  item.addEventListener("click", pick);
}

// Move the highlight in place instead of re-rendering. Rebuilding the list from
// a hover handler detaches the row under the cursor; the browser re-fires
// mouseenter on its replacement on the next frame, which rebuilds again, so the
// row churns every frame and a press lands on a node that is already gone.
function highlightSuggestItems(box, selector, index, scroll) {
  if (!box) return;
  box.querySelectorAll(selector).forEach((el, i) => {
    const active = i === index;
    el.classList.toggle("is-active", active);
    if (el.hasAttribute("aria-selected")) {
      el.setAttribute("aria-selected", active ? "true" : "false");
    }
    if (active && scroll) el.scrollIntoView({ block: "nearest" });
  });
}

function closeSlashSuggest() {
  slashSuggest = null;
  if (slashSuggestEl) slashSuggestEl.hidden = true;
  input?.setAttribute("aria-expanded", "false");
}

function slashTokenAtCaret() {
  const caret = input?.selectionStart;
  if (
    caret == null ||
    input.selectionEnd !== caret ||
    caret !== input.value.length
  ) return null;
  const match = /^\/([A-Za-z]*)$/.exec(input.value);
  return match ? match[1].toLowerCase() : null;
}

function renderSlashSuggest() {
  if (!slashSuggest || !slashSuggestEl) return;
  slashSuggestEl.textContent = "";
  const hint = document.createElement("div");
  hint.className = "chat-slash-suggest-hint";
  hint.textContent = "Available commands · Tab to insert · Esc to dismiss";
  slashSuggestEl.append(hint);
  slashSuggest.items.forEach((entry, index) => {
    const active = index === slashSuggest.index;
    const item = document.createElement("button");
    item.type = "button";
    item.className =
      "chat-slash-suggest-item" + (active ? " is-active" : "");
    item.setAttribute("role", "option");
    item.setAttribute("aria-selected", active ? "true" : "false");
    const command = document.createElement("span");
    command.className = "chat-slash-suggest-command";
    command.textContent = entry.usage;
    const description = document.createElement("span");
    description.className = "chat-slash-suggest-description";
    description.textContent = entry.description;
    item.append(command, description);
    wireSuggestPick(item, () => acceptSlashSuggest(index));
    item.addEventListener("mouseenter", () => highlightSlashSuggest(index, false));
    slashSuggestEl.append(item);
  });
  slashSuggestEl.hidden = false;
  input.setAttribute("aria-expanded", "true");
}

function updateSlashSuggest() {
  const partial = slashTokenAtCaret();
  if (partial == null) {
    closeSlashSuggest();
    return;
  }
  const items = SLASH_COMMANDS.filter((entry) =>
    entry.command.slice(1).startsWith(partial));
  if (!items.length) {
    closeSlashSuggest();
    return;
  }
  const previous = slashSuggest?.items[slashSuggest.index]?.command;
  let index = previous
    ? items.findIndex((entry) => entry.command === previous)
    : 0;
  if (index < 0) index = 0;
  slashSuggest = { items, index };
  closeMentionSuggest();
  renderSlashSuggest();
}

function acceptSlashSuggest(index) {
  if (!slashSuggest || !input) return;
  const entry = slashSuggest.items[
    typeof index === "number" ? index : slashSuggest.index];
  if (!entry) return;
  input.value = entry.command + " ";
  closeSlashSuggest();
  resizeComposer();
  input.focus();
  input.setSelectionRange(input.value.length, input.value.length);
}

function highlightSlashSuggest(index, scroll) {
  if (!slashSuggest) return;
  const count = slashSuggest.items.length;
  if (!count) return;
  slashSuggest.index = ((index % count) + count) % count;
  highlightSuggestItems(
    slashSuggestEl, ".chat-slash-suggest-item", slashSuggest.index, scroll);
}

function moveSlashSuggest(step) {
  if (!slashSuggest) return;
  highlightSlashSuggest(slashSuggest.index + step, true);
}

function ensureMentionSuggest() {
  if (mentionSuggestEl) return mentionSuggestEl;
  mentionSuggestEl = document.createElement("div");
  mentionSuggestEl.className = "chat-mention-suggest";
  mentionSuggestEl.hidden = true;
  const bar = input.closest(".chat-input-bar") || document.body;
  bar.append(mentionSuggestEl);
  return mentionSuggestEl;
}

function closeMentionSuggest() {
  mentionSuggest = null;
  if (mentionSuggestEl) mentionSuggestEl.hidden = true;
}

// The "@partial" token the caret is currently inside, or null. A mention starts
// at "@" preceded by whitespace/start and uses the same charset the renderer
// links: letters, digits, and hyphens.
function mentionTokenAtCaret() {
  const caret = input.selectionStart;
  if (caret == null || input.selectionEnd !== caret) return null;
  const before = input.value.slice(0, caret);
  const match = /(^|\s)@([A-Za-z0-9-]{0,32})$/.exec(before);
  if (!match) return null;
  const start = caret - match[2].length - 1; // include the "@"
  return { start, end: caret, partial: match[2].toLowerCase() };
}

function mentionCandidates(partial) {
  const self = mentionName(displayName());
  const byName = new Map();
  for (const person of roster.values()) {
    const key = mentionName(person.name);
    if (!key || key === self) continue;
    const prev = byName.get(key);
    if (!prev || (personIsOnline(person) && !personIsOnline(prev))) {
      byName.set(key, person);
    }
  }
  for (const person of registeredUsers.values()) {
    const key = mentionName(person.name);
    if (!key || key === self || byName.has(key)) continue;
    byName.set(key, {
      id: "account:" + key,
      name: person.name,
      kind: "user",
      lastSeenMs: 0,
    });
  }
  if (!byName.has(FORKBOT_SENDER_ID)) {
    byName.set(FORKBOT_SENDER_ID, {
      id: FORKBOT_SENDER_ID, name: "forkbot", kind: "bot", lastSeenMs: Date.now(),
    });
  }
  const matches = [...byName.values()].filter((person) => {
    const key = mentionName(person.name);
    return !partial || key.startsWith(partial) || key.includes(partial);
  });
  matches.sort((a, b) => {
    const ak = mentionName(a.name);
    const bk = mentionName(b.name);
    const aPrefix = partial && ak.startsWith(partial) ? 0 : 1;
    const bPrefix = partial && bk.startsWith(partial) ? 0 : 1;
    return (aPrefix - bPrefix) ||
      (personIsOnline(b) - personIsOnline(a)) ||
      ak.localeCompare(bk);
  });
  return matches.slice(0, 8);
}

function renderMentionSuggest() {
  const box = ensureMentionSuggest();
  box.textContent = "";
  const hint = document.createElement("div");
  hint.className = "chat-mention-suggest-hint";
  hint.textContent = "Tab to mention · Esc to dismiss";
  box.append(hint);
  mentionSuggest.items.forEach((person, index) => {
    const item = document.createElement("button");
    item.type = "button";
    item.className = "chat-mention-suggest-item" +
      (index === mentionSuggest.index ? " is-active" : "");
    item.append(makeAvatar(person.name, person.kind));
    const label = document.createElement("span");
    label.className = "chat-mention-suggest-name";
    label.textContent = person.name;
    const dot = document.createElement("span");
    dot.className = "chat-presence-dot" + (personIsOnline(person) ? " is-online" : "");
    item.append(label, dot);
    wireSuggestPick(item, () => acceptMentionSuggest(index));
    item.addEventListener("mouseenter", () => highlightMentionSuggest(index, false));
    box.append(item);
  });
  box.hidden = false;
  const active = box.querySelector(".is-active");
  if (active) active.scrollIntoView({ block: "nearest" });
}

function updateMentionSuggest() {
  if (slashTokenAtCaret() != null) {
    closeMentionSuggest();
    return;
  }
  const token = mentionTokenAtCaret();
  if (!token) {
    closeMentionSuggest();
    return;
  }
  const items = mentionCandidates(token.partial);
  if (!items.length) {
    closeMentionSuggest();
    return;
  }
  const prevName = mentionSuggest &&
    mentionSuggest.items[mentionSuggest.index] &&
    mentionName(mentionSuggest.items[mentionSuggest.index].name);
  let index = prevName
    ? items.findIndex((person) => mentionName(person.name) === prevName)
    : 0;
  if (index < 0) index = 0;
  mentionSuggest = { items, index, start: token.start, end: token.end };
  renderMentionSuggest();
}

function acceptMentionSuggest(index) {
  if (!mentionSuggest) return;
  const person = mentionSuggest.items[
    typeof index === "number" ? index : mentionSuggest.index];
  if (!person) return;
  const name = mentionName(person.name);
  const value = input.value;
  const inserted = "@" + name + " ";
  input.value = value.slice(0, mentionSuggest.start) + inserted +
    value.slice(mentionSuggest.end);
  const caret = mentionSuggest.start + inserted.length;
  input.setSelectionRange(caret, caret);
  closeMentionSuggest();
  resizeComposer();
  input.focus();
}

function highlightMentionSuggest(index, scroll) {
  if (!mentionSuggest) return;
  const count = mentionSuggest.items.length;
  if (!count) return;
  mentionSuggest.index = ((index % count) + count) % count;
  highlightSuggestItems(
    mentionSuggestEl, ".chat-mention-suggest-item", mentionSuggest.index, scroll);
}

function moveMentionSuggest(step) {
  if (!mentionSuggest) return;
  highlightMentionSuggest(mentionSuggest.index + step, true);
}

// ---- rooms (left pane) --------------------------------------------------------

function normalizeChannelKey(channel) {
  const value = String(channel || "").trim();
  if (value === "#general") return value;
  if (isPrivateChannelKey(value) && privateChannelForKey(value)) return value;
  if (isDirectMessageKey(value) && directMessageForKey(value)) return value;
  return "";
}

function ensureChannel(channelKey) {
  const channel = normalizeChannelKey(channelKey);
  if (!channel) return "";
  if (!channelMeta.has(channel)) {
    channelMeta.set(channel, { unread: 0 });
    if (!channelMessages.has(channel)) channelMessages.set(channel, []);
    if (!channelSyncState.has(channel)) channelSyncState.set(channel, "idle");
    renderRooms();
  }
  return channel;
}

function markChannelLoading(channel = activeChannel) {
  if ((channelMessages.get(channel) || []).length) return;
  if (channelSyncState.get(channel) !== "ready") {
    channelSyncState.set(channel, "loading");
  }
}

// A line is "mine" when this browser sent it (senderId) OR when the signed-in
// account authored it from somewhere else — another tab, a phone, the desktop
// client. selfId is per-browser, so without the account check your own message
// echoed back from a second client lit an unread badge for something you just
// typed. Used for unread bookkeeping only: rendering, edit and delete still
// require the stronger senderId match, since chat frames carry no signature
// binding a display name to an account.
function isOwnChatMessage(record) {
  if (!record) return false;
  if (record.self || (record.senderId && record.senderId === selfId)) {
    return true;
  }
  const account = String(userSession()?.nodeName || "").trim().toLowerCase();
  if (!account) return false;
  return String(record.sender || "").trim().toLowerCase() === account;
}

function isReadMarked(channel, ts) {
  const value = Number(ts) || 0;
  return value > 0 && value <= (readMarks.get(channel) || 0);
}

function bumpUnread(channel, ts) {
  const meta = channelMeta.get(channel);
  if (!meta || isReadMarked(channel, ts)) return;
  meta.unread += 1;
  meta.unreadTs = Math.max(meta.unreadTs || 0, Number(ts) || 0);
  renderRooms();
}

function channelListRank(name) {
  const record = privateChannelForKey(name);
  if (isEveryoneChannel(record)) {
    const index = EVERYONE_CHANNEL_ORDER.indexOf(record.name);
    return index < 0 ? EVERYONE_CHANNEL_ORDER.length : index;
  }
  return name === "#general"
    ? EVERYONE_CHANNEL_ORDER.length + 1
    : EVERYONE_CHANNEL_ORDER.length + 2;
}

function renderRooms() {
  if (!roomsEl) return;
  roomsEl.textContent = "";
  const names = [...channelMeta.keys()]
    .filter((name) => !isDirectMessageKey(name))
    .sort((a, b) => channelListRank(a) - channelListRank(b) ||
      channelDisplayLabel(a).localeCompare(channelDisplayLabel(b)));
  for (const name of names) {
    const meta = channelMeta.get(name);
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "chat-room" + (name === activeChannel ? " is-active" : "");
    btn.dataset.channel = name;
    const channel = privateChannelForKey(name);
    const isPrivate = Boolean(channel) && channel.visibility !== "public";
    const isWorld = name === "#general";
    const icon = document.createElement("span");
    icon.className = "chat-room-icon";
    icon.append(chatIcon(isPrivate ? "lock" : isWorld ? "globe" : "hash", 15));
    btn.append(icon);
    const label = document.createElement("span");
    label.className = "chat-room-name";
    // The icon already says "channel", so the list drops the leading #.
    label.textContent = channelDisplayLabel(name).replace(/^#/, "");
    btn.dataset.filter = channelDisplayLabel(name).toLowerCase();
    if (isWorld) {
      btn.setAttribute("aria-label", "World chat, open to visitors");
      btn.title = "World chat, open to visitors";
    }
    if (isPrivateChannelKey(name)) {
      const visibilityLabel = isEveryoneChannel(channel)
        ? "Every ForkMesh member is in this channel"
        : channel?.serverVisible
        ? "Server notices, visible to the server, only you can see it"
        : channel?.visibility === "public"
          ? "Public channel"
          : "Private channel";
      btn.setAttribute(
        "aria-label",
        `${visibilityLabel} ${channelDisplayLabel(name)}`,
      );
      btn.title = visibilityLabel;
    }
    btn.append(roomTextColumn(label, name));
    if (meta.unread > 0 && name !== activeChannel) {
      const badge = document.createElement("span");
      badge.className = "chat-room-unread";
      badge.textContent = meta.unread > 999 ? "999+" : String(meta.unread);
      btn.append(badge);
    }
    if (!isOfficeEmbed && userSession() &&
        (channel?.canManage || channel?.serverVisible)) {
      const gear = document.createElement("span");
      gear.className = "chat-room-settings";
      gear.title = `Channel settings for ${channelDisplayLabel(name)}`;
      gear.setAttribute("aria-hidden", "true");
      gear.append(chatIcon("gear", 14));
      gear.addEventListener("click", (event) => {
        event.stopPropagation();
        setActiveChannel(name);
        if (channel.serverVisible) openSealPanel();
        else openChannelSettings();
      });
      btn.append(gear);
    }
    btn.addEventListener("click", () => setActiveChannel(name));
    roomsEl.append(btn);
  }
  renderDirectMessages();
}

function renderDirectMessages() {
  if (!directListEl) return;
  directListEl.textContent = "";
  for (const direct of sortedDirectMessages()) {
    const key = directMessageKey(direct.id);
    const meta = channelMeta.get(key) || { unread: 0 };
    const button = document.createElement("button");
    button.type = "button";
    button.className =
      "chat-room chat-direct-row" + (key === activeChannel ? " is-active" : "");
    button.dataset.channel = key;
    button.setAttribute("aria-label", `Direct message with ${direct.otherUser}`);
    button.title = `Direct message with ${direct.otherUser}`;
    button.dataset.filter = "@" + direct.otherUser.toLowerCase();
    const face = document.createElement("span");
    face.className = "chat-direct-face";
    const presence = document.createElement("span");
    presence.className = "chat-presence-dot" +
      (directPartnerOnline(direct.otherUser) ? " is-online" : "");
    face.append(makeAvatar(direct.otherUser, "user"), presence);
    button.append(face);
    const label = document.createElement("span");
    label.className = "chat-room-name";
    label.textContent = direct.otherUser;
    const column = roomTextColumn(label, key);
    const directBadge = userStatusBadge(direct.otherUser);
    if (directBadge) label.after(directBadge);
    button.append(column);
    if (meta.unread > 0 && key !== activeChannel) {
      const badge = document.createElement("span");
      badge.className = "chat-room-unread";
      badge.textContent = meta.unread > 999 ? "999+" : String(meta.unread);
      button.append(badge);
    }
    button.addEventListener("click", () => setActiveChannel(key));
    directListEl.append(button);
  }
  applyRoomFilter();
}

function directRecency(direct) {
  const updated = Number(direct.updatedAt) || 0;
  const updatedMs = updated && updated < 1e12 ? updated * 1000 : updated;
  return Math.max(updatedMs, Number(conversationPreviewRecord(
    directMessageKey(direct.id))?.ts) || 0);
}

function sortedDirectMessages() {
  return [...directMessages.values()]
    .map((direct) => ({ direct, recency: directRecency(direct) }))
    .sort((a, b) => b.recency - a.recency ||
      a.direct.otherUser.localeCompare(b.direct.otherUser))
    .map((entry) => entry.direct);
}

function conversationPreviewRecord(channel) {
  const list = channelMessages.get(channel) || [];
  for (let index = list.length - 1; index >= 0; index -= 1) {
    const record = list[index];
    if (!record.deleted && !record.rootId &&
        (record.text || record.attachment)) return record;
  }
  return null;
}

function conversationPreviewText(record) {
  if (!record) return "";
  const source = record.richText?.source
    ? plainTextFromRichSource(record.richText.source)
    : record.text;
  const body = String(source || "").replace(/\s+/g, " ").trim().slice(0, 200) ||
    (record.attachment?.fileName ? `📎 ${record.attachment.fileName}` : "");
  if (!body) return "";
  if (isOwnChatMessage(record)) return `You: ${body}`;
  if (isDirectMessageKey(record.channel)) return body;
  return `${record.sender || "peer"}: ${body}`;
}

function roomPreviewTime(ts, nowValue = Date.now()) {
  const value = Number(ts) || 0;
  if (!value) return "";
  if (sameLocalDate(value, nowValue)) return fmtTime(value);
  const date = new Date(value);
  const age = nowValue - value;
  if (age > 0 && age < 6 * 86400000) {
    return date.toLocaleDateString("en-US", { weekday: "short" });
  }
  const options = { day: "numeric", month: "short" };
  if (date.getFullYear() !== new Date(nowValue).getFullYear()) {
    options.year = "numeric";
  }
  return date.toLocaleDateString("en-GB", options);
}

function roomTextColumn(label, channel) {
  const column = document.createElement("span");
  column.className = "chat-room-text";
  const line = document.createElement("span");
  line.className = "chat-room-line";
  const time = document.createElement("span");
  time.className = "chat-room-time";
  line.append(label, time);
  const preview = document.createElement("span");
  preview.className = "chat-room-preview";
  column.append(line, preview);
  paintRoomPreview(column, channel);
  return column;
}

function paintRoomPreview(column, channel) {
  const record = conversationPreviewRecord(channel);
  const text = conversationPreviewText(record);
  const preview = column.querySelector(".chat-room-preview");
  const time = column.querySelector(".chat-room-time");
  if (preview) preview.textContent = text;
  if (time) time.textContent = text ? roomPreviewTime(record.ts) : "";
}

function queueRoomPreview(channel) {
  if (!channel) return;
  roomPreviewChannels.add(channel);
  if (!roomPreviewTimer) roomPreviewTimer = setTimeout(flushRoomPreviews, 50);
}

function directOrderChanged() {
  const shown = [...(directListEl?.querySelectorAll(".chat-direct-row") || [])]
    .map((row) => row.dataset.channel);
  const wanted = sortedDirectMessages()
    .map((direct) => directMessageKey(direct.id));
  return shown.join() !== wanted.join();
}

function flushRoomPreviews() {
  roomPreviewTimer = 0;
  const channels = new Set(roomPreviewChannels);
  roomPreviewChannels.clear();
  if (directListEl && [...channels].some(isDirectMessageKey) &&
      directOrderChanged()) {
    renderDirectMessages();
  }
  for (const row of [
    ...(roomsEl?.querySelectorAll(".chat-room") || []),
    ...(directListEl?.querySelectorAll(".chat-room") || []),
  ]) {
    if (!channels.has(row.dataset.channel)) continue;
    const column = row.querySelector(".chat-room-text");
    if (column) paintRoomPreview(column, row.dataset.channel);
  }
}

function directPartnerOnline(name) {
  const wanted = String(name || "").toLowerCase();
  for (const person of roster.values()) {
    if (person.kind === "user" && person.name.toLowerCase() === wanted &&
        personIsOnline(person)) {
      return true;
    }
  }
  return false;
}

// Repaint the DM rows' presence dots in place as the roster changes, and on
// the same 30s beat the people list uses to age peers out.
function refreshDirectPresence() {
  for (const row of directListEl?.querySelectorAll(".chat-direct-row") || []) {
    const name = String(row.dataset.filter || "").replace(/^@/, "");
    const dot = row.querySelector(".chat-presence-dot");
    const online = directPartnerOnline(name);
    dot?.classList.toggle("is-online", online);
    dot?.classList.toggle("is-away", online && userIsAway(name));
  }
}
setInterval(refreshDirectPresence, 30000);

// ---- sidebar filter ---------------------------------------------------------


function applyRoomFilter() {
  const query = String(roomFilterInput?.value || "").trim().toLowerCase()
    .replace(/^[#@]/, "");
  const rows = [
    ...(roomsEl?.querySelectorAll(".chat-room") || []),
    ...(directListEl?.querySelectorAll(".chat-room") || []),
  ];
  let visible = 0;
  for (const row of rows) {
    const match = !query || String(row.dataset.filter || "").includes(query);
    row.hidden = !match;
    if (match) visible += 1;
  }
  panesEl?.classList.toggle("is-room-filtering", Boolean(query));
  if (roomFilterEmpty) roomFilterEmpty.hidden = !query || visible > 0;
}

roomFilterInput?.addEventListener("input", applyRoomFilter);
roomFilterInput?.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    roomFilterInput.value = "";
    applyRoomFilter();
    roomFilterInput.blur();
    input?.focus();
  } else if (event.key === "Enter") {
    // Enter opens the first match, like a quick switcher.
    const first = [
      ...(roomsEl?.querySelectorAll(".chat-room:not([hidden])") || []),
      ...(directListEl?.querySelectorAll(".chat-room:not([hidden])") || []),
    ][0];
    if (!first) return;
    event.preventDefault();
    first.click();
    roomFilterInput.value = "";
    applyRoomFilter();
    input?.focus();
  }
});

document.addEventListener("keydown", (event) => {
  if ((event.metaKey || event.ctrlKey) && !event.altKey &&
      String(event.key).toLowerCase() === "k" && roomFilterInput &&
      roomFilterInput.offsetParent !== null) {
    event.preventDefault();
    roomFilterInput.focus();
    roomFilterInput.select();
  }
});

// Collapsible Channels / Direct messages sections, remembered per browser.
const SECTION_COLLAPSE_KEY = "forkmesh.chat.collapsedSections";
function readCollapsedSections() {
  try {
    const value = JSON.parse(localStorage.getItem(SECTION_COLLAPSE_KEY) || "[]");
    return new Set(Array.isArray(value) ? value : []);
  } catch (_) {
    return new Set();
  }
}
const collapsedSections = readCollapsedSections();
for (const toggle of document.querySelectorAll(".chat-section-toggle")) {
  const section = toggle.dataset.chatSection;
  const list = document.getElementById(toggle.getAttribute("aria-controls"));
  const paint = () => {
    const collapsed = collapsedSections.has(section);
    toggle.setAttribute("aria-expanded", String(!collapsed));
    list?.classList.toggle("is-collapsed", collapsed);
  };
  paint();
  toggle.addEventListener("click", () => {
    if (collapsedSections.has(section)) collapsedSections.delete(section);
    else collapsedSections.add(section);
    try {
      localStorage.setItem(SECTION_COLLAPSE_KEY, JSON.stringify([...collapsedSections]));
    } catch (_) {}
    paint();
  });
}

function updateChannelHeading() {
  const channel = privateChannelForKey(activeChannel);
  const direct = directMessageForKey(activeChannel);
  const label = channelDisplayLabel(activeChannel);
  if (channelTitleEl) channelTitleEl.textContent = label;
  if (channelTopicEl) {
    channelTopicEl.textContent = String(channel?.topic || "");
    channelTopicEl.hidden = !channel?.topic;
  }
  if (channelVisibilityBadge) {
    channelVisibilityBadge.hidden = !channel && !direct;
    channelVisibilityBadge.textContent = direct ? "direct" : (
      channel ? (channel.serverVisible ? "server-visible" : channel.visibility) : ""
    );
    channelVisibilityBadge.title = channel?.serverVisible
      ? "ForkMesh posts these notes itself, so they are not end-to-end encrypted. Only you can see this channel."
      : "";
  }
  renderDirectServerCopies();
}

function directServerCopiesText(stored, otherUser) {
  if (stored > 0) {
    return {
      text: stored === 1 ? "1 message on the server" : `${stored} messages on the server`,
      title: `Stored encrypted on the ForkMesh server until @${otherUser} receives ${
        stored === 1 ? "it" : "them"
      }, then deleted.`,
    };
  }
  return {
    text: "Not stored on the server",
    title: `Every message @${otherUser} received has been deleted from the ForkMesh server.`,
  };
}

function renderDirectServerCopies() {
  if (!directServerCopiesEl) return;
  const direct = directMessageForKey(activeChannel);
  const stored = direct ? directServerCopies.get(activeChannel) : undefined;
  if (stored === undefined) {
    directServerCopiesEl.hidden = true;
    directServerCopiesEl.textContent = "";
    directServerCopiesEl.classList.remove("is-stored");
    return;
  }
  const { text, title } = directServerCopiesText(stored, direct.otherUser);
  directServerCopiesEl.textContent = text;
  directServerCopiesEl.title = title;
  directServerCopiesEl.classList.toggle("is-stored", stored > 0);
  directServerCopiesEl.hidden = false;
}

function applyDirectServerCopies(stored, scope) {
  if (!isDirectMessageKey(scope)) return;
  directServerCopies.set(scope, stored);
  if (scope === activeChannel) renderDirectServerCopies();
}

// Who may open the channel manager on this page. Full chat: any signed-in
// account, exactly as the channels API allows. Office embed: members of the
// organization whose office this is (platform admins manage everywhere).
function canAuthorChannelsHere() {
  const session = userSession();
  if (!session) return false;
  return !isOfficeEmbed || officeOrganizationMember || Boolean(session.isAdmin);
}

// Ask the Office for this account's server-derived organization role. Called
// once the session is hydrated; a failure leaves the embed as it was, with
// creation closed.
async function loadOfficeOrganizationMembership() {
  if (!isOfficeEmbed || !userSession()) return false;
  try {
    const data = await privateChannelRequest(OFFICE_FLOOR_ACCESS_ENDPOINT);
    officeOrganizationMember = OFFICE_ORGANIZATION_ROLES.has(
      String(data?.organizationRole || "").trim().toLowerCase(),
    );
  } catch (_) {
    officeOrganizationMember = false;
  }
  updateAdminChannelControls();
  return officeOrganizationMember;
}

function updateAdminChannelControls() {
  const session = userSession();
  const channel = privateChannelForKey(activeChannel);
  const authorsChannels = canAuthorChannelsHere();
  if (officeManageLink) {
    officeManageLink.hidden = !(isOfficeEmbed && authorsChannels);
  }
  if (directSection) directSection.hidden = !session || isOfficeEmbed;
  if (directCreateBtn) directCreateBtn.hidden = !session || isOfficeEmbed;
  const direct = directMessageForKey(activeChannel);
  const directActions = Boolean(session && direct && !isOfficeEmbed);
  if (directAddBtn) directAddBtn.hidden = !directActions;
  const callable = directActions && callsSupported();
  if (callAudioBtn) callAudioBtn.hidden = !callable;
  if (callVideoBtn) callVideoBtn.hidden = !callable;
  if (viewOnceVideoBtn) {
    viewOnceVideoBtn.hidden = !(directActions && viewOnceRecordingSupported("video"));
  }
  if (viewOnceVoiceBtn) {
    viewOnceVoiceBtn.hidden = !(directActions && viewOnceRecordingSupported("voice"));
  }
  if (channelCreateBtn) channelCreateBtn.hidden = !authorsChannels;
  if (channelManageBtn) {
    channelManageBtn.hidden = !(session && channel?.canManage && !isOfficeEmbed);
    if (channelManageBtn.hidden) closeChannelSettings();
  }
  const readOnly = Boolean(channel?.readOnly);
  if (input) {
    input.disabled = readOnly;
    if (readOnly) input.placeholder = "Server notices are read-only";
  }
  if (sendBtn) sendBtn.disabled = readOnly;
  renderSealControl();
  renderRetentionControl();
}

function renderRetentionControl() {
  if (!chatRetentionSelect) return;
  const show = Boolean(userSession()?.sessionToken) && canJoinChannel();
  chatRetentionSelect.hidden = !show;
  if (!show) return;
  const label = channelDisplayLabel(activeChannel);
  const override = chatRetention.overrideMs(activeChannel);
  const chatGroup = document.createElement("optgroup");
  chatGroup.label = `${label}: messages disappear after`;
  chatGroup.append(new Option(
    `Default (${chatRetentionLabel(chatRetention.defaultMs())})`, ""));
  for (const preset of CHAT_RETENTION_PRESETS) {
    chatGroup.append(new Option(preset.label, String(preset.ms)));
  }
  if (override && !CHAT_RETENTION_PRESETS.some((preset) => preset.ms === override)) {
    chatGroup.append(new Option(chatRetentionLabel(override), String(override)));
  }
  const defaultGroup = document.createElement("optgroup");
  defaultGroup.label = "Default for all chats";
  for (const preset of CHAT_RETENTION_PRESETS) {
    defaultGroup.append(new Option(`All chats: ${preset.label}`, `default:${preset.ms}`));
  }
  chatRetentionSelect.replaceChildren(chatGroup, defaultGroup);
  chatRetentionSelect.value = override ? String(override) : "";
  chatRetentionSelect.title =
    `Messages in ${label} disappear after ` +
    chatRetentionLabel(chatRetention.effectiveMs(activeChannel));
}

async function changeChatRetention() {
  if (!chatRetentionSelect) return;
  const value = chatRetentionSelect.value;
  chatRetentionSelect.disabled = true;
  try {
    if (value.startsWith("default:")) {
      await chatRetention.setDefault(Number(value.slice("default:".length)));
    } else {
      await chatRetention.setChat(activeChannel, value ? Number(value) : null);
    }
  } catch (_) {
    setStatus("Could not save when messages disappear", "error");
  } finally {
    chatRetentionSelect.disabled = false;
    renderRetentionControl();
  }
}

function pruneExpiredMessages() {
  const nowMs = Date.now();
  let activeChanged = false;
  for (const [channel, list] of channelMessages) {
    const cutoff = chatRetention.cutoff(channel, nowMs);
    let count = 0;
    while (count < list.length && Number(list[count].ts) <= cutoff) count += 1;
    if (!count) continue;
    for (const record of list.splice(0, count)) {
      revokeAttachmentUrl(record);
      rows.delete(record.id);
      reactions.delete(record.id);
      threads.deleteTarget(record.id);
      if (activeThreadRootId === record.id) closeThread({ restoreFocus: false });
    }
    queueRoomPreview(channel);
    if (channel === activeChannel) activeChanged = true;
  }
  if (activeChanged) repaintChannel(activeChannel);
}

async function loadChatRetention() {
  chatRetention.apply(userSession()?.chatRetention);
  if (!userSession()?.sessionToken) return;
  try {
    await chatRetention.refresh();
  } catch (_) {}
  renderRetentionControl();
}

function setActiveChannel(name, options = {}) {
  const normalized = normalizeChannelKey(name);
  if (!normalized) return;
  if (!userSession() && normalized !== "#general") {
    setStatus("Guests can chat only in World", "authorization");
    return;
  }
  const channel = ensureChannel(normalized);
  defaultChannelChosen = true;
  if (channel === activeChannel) return;
  clearDropState();
  closeThread({ restoreFocus: false });
  // The settings panel belongs to the channel it was opened from.
  closeChannelSettings();
  const previousScope = roomScopeForChannel(activeChannel);
  activeChannel = channel;
  if (canJoinChannel(channel)) markChannelLoading(channel);
  renderAttachmentDraft();
  try {
    const record = privateChannelForKey(channel);
    const direct = directMessageForKey(channel);
    if (direct) {
      savedDirectMessageId = direct.id;
      savedPrivateChannelId = "";
      localStorage.setItem(ACTIVE_DIRECT_KEY, direct.id);
      localStorage.removeItem(ACTIVE_CHANNEL_KEY);
    } else if (record) {
      savedPrivateChannelId = record.id;
      savedDirectMessageId = "";
      localStorage.setItem(ACTIVE_CHANNEL_KEY, record.id);
      localStorage.removeItem(ACTIVE_DIRECT_KEY);
    } else {
      savedPrivateChannelId = "";
      savedDirectMessageId = "";
      localStorage.removeItem(ACTIVE_CHANNEL_KEY);
      localStorage.removeItem(ACTIVE_DIRECT_KEY);
    }
  } catch (_) {}
  const meta = channelMeta.get(channel);
  if (meta) {
    meta.unread = 0;
    meta.unreadTs = 0;
  }
  queueReadMark(channel);
  refreshEveryoneChannelMembers(privateChannelForKey(channel));
  const label = channelDisplayLabel(channel);
  updateChannelHeading();
  if (input) input.placeholder = `Message ${label}…`;
  updateAdminChannelControls();
  renderRooms();
  schedulePeopleRender();
  renderActiveChannel();
  if (isOfficeEmbed && window.parent !== window) {
    window.parent.postMessage(
      { type: "office-chat-room-changed" },
      window.location.origin,
    );
  }
  if (options.connect !== false && previousScope !== roomScopeForChannel(activeChannel)) {
    switchChatRoom();
  }
}

function releaseChannelMessages(channelKey) {
  for (const record of channelMessages.get(channelKey) || []) {
    revokeAttachmentUrl(record);
    rows.delete(record.id);
    reactions.delete(record.id);
  }
  channelMessages.delete(channelKey);
  channelSyncState.delete(channelKey);
  channelMeta.delete(channelKey);
}

function reconcilePrivateChannels(records, options = {}) {
  const previous = new Map(privateChannels);
  privateChannels.clear();
  for (const value of Array.isArray(records) ? records : []) {
    const id = String(value?.id || "");
    const name = String(value?.name || "").trim().toLowerCase();
    if (!/^[0-9a-f]{32}$/.test(id) || !name) continue;
    privateChannels.set(id, {
      id,
      name,
      updatedAt: Number(value.updatedAt) || 0,
      keyVersion: Number(value.keyVersion) || 1,
      kind: value.kind === "everyone" ? "everyone" : "channel",
      memberCount: Math.max(0, Number(value.memberCount) || 0),
      canManage: Boolean(value.canManage) && value.kind !== "everyone",
      visibility: value.visibility === "public" ? "public" : "private",
      topic: String(value.topic || "").trim().slice(0, 200),
      serverVisible: value.serverVisible === true,
      readOnly: value.readOnly === true,
      members: Array.isArray(value.members)
        ? [...new Set(value.members
          .map((member) => String(member || "").trim().toLowerCase())
          .filter(Boolean))].slice(0, 500)
        : [],
    });
    ensureChannel(privateChannelKey(id));
  }
  for (const id of previous.keys()) {
    if (!privateChannels.has(id)) releaseChannelMessages(privateChannelKey(id));
  }

  if (savedPrivateChannelId && !privateChannels.has(savedPrivateChannelId)) {
    savedPrivateChannelId = "";
    try { localStorage.removeItem(ACTIVE_CHANNEL_KEY); } catch (_) {}
  }

  const activeRecord = privateChannelForKey(activeChannel);
  if (isPrivateChannelKey(activeChannel) && !activeRecord) {
    setActiveChannel(defaultChannelKey());
    return;
  }
  if (activeRecord) {
    const old = previous.get(activeRecord.id);
    if (old && (
      old.keyVersion !== activeRecord.keyVersion ||
      old.visibility !== activeRecord.visibility
    )) switchChatRoom();
  } else if (
    options.selectSaved !== false &&
    savedPrivateChannelId &&
    privateChannels.has(savedPrivateChannelId)
  ) {
    setActiveChannel(privateChannelKey(savedPrivateChannelId), {
      connect: options.connect,
    });
  } else if (
    options.selectSaved !== false &&
    !defaultChannelChosen &&
    activeChannel === "#general" &&
    defaultChannelKey() !== "#general"
  ) {
    setActiveChannel(defaultChannelKey(), { connect: options.connect });
  }
  if (activeRecord) refreshEveryoneChannelMembers(activeRecord);
  updateAdminChannelControls();
  renderRooms();
  schedulePeopleRender();
}

async function refreshEveryoneChannelMembers(record) {
  if (!isEveryoneChannel(record) || !userSession()) return;
  const cached = everyoneChannelMembers.get(record.id);
  if (cached?.loading ||
      (cached?.names && Date.now() - cached.at < EVERYONE_MEMBERS_REFRESH_MS)) {
    return;
  }
  everyoneChannelMembers.set(record.id, { ...cached, loading: true });
  let names = cached?.names || null;
  let at = cached?.at || 0;
  try {
    const data = await privateChannelRequest(
      `${PRIVATE_CHANNELS_ENDPOINT}/${record.id}/members`);
    names = [...new Set((Array.isArray(data?.members) ? data.members : [])
      .map((member) => String(member?.username || "").trim().toLowerCase())
      .filter(Boolean))];
    at = Date.now();
  } catch (_) {}
  everyoneChannelMembers.set(record.id, { names, at, loading: false });
  if (privateChannelForKey(activeChannel)?.id === record.id) {
    schedulePeopleRender();
  }
}

function reconcileDirectMessages(records, options = {}) {
  const previous = new Map(directMessages);
  const replace = options.append !== true;
  if (replace) directMessages.clear();
  for (const value of Array.isArray(records) ? records : []) {
    const id = String(value?.id || "");
    const otherUser = String(value?.otherUser || "").trim().toLowerCase();
    if (!/^[0-9a-f]{32}$/.test(id) || !otherUser) continue;
    const unreadCount = Math.max(0, Number(value.unreadCount) || 0);
    directMessages.set(id, {
      id,
      otherUser,
      updatedAt: Number(value.updatedAt) || 0,
      keyVersion: Number(value.keyVersion) || 1,
      unreadCount,
    });
    const key = directMessageKey(id);
    ensureChannel(key);
    const meta = channelMeta.get(key);
    if (meta) {
      meta.unread = key === activeChannel
        ? 0
        : unreadCount;
      if (!meta.unread) meta.unreadTs = 0;
    }
  }
  if (replace) {
    for (const id of previous.keys()) {
      if (!directMessages.has(id)) {
        releaseChannelMessages(directMessageKey(id));
      }
    }
  }

  if (Object.prototype.hasOwnProperty.call(options, "nextCursor")) {
    directNextCursor = String(options.nextCursor || "");
  }
  if (directMoreBtn) directMoreBtn.hidden = !directNextCursor;

  if (savedDirectMessageId && !directMessages.has(savedDirectMessageId)) {
    savedDirectMessageId = "";
    try { localStorage.removeItem(ACTIVE_DIRECT_KEY); } catch (_) {}
  }

  const activeDirect = directMessageForKey(activeChannel);
  if (isDirectMessageKey(activeChannel) && !activeDirect) {
    setActiveChannel(defaultChannelKey());
    return;
  }
  if (activeDirect) {
    const old = previous.get(activeDirect.id);
    if (old && old.keyVersion !== activeDirect.keyVersion) switchChatRoom();
  } else if (
    options.selectSaved !== false &&
    savedDirectMessageId &&
    directMessages.has(savedDirectMessageId)
  ) {
    setActiveChannel(directMessageKey(savedDirectMessageId), {
      connect: options.connect,
    });
  }
  updateAdminChannelControls();
  renderDirectMessages();
}

async function refreshDirectMessages(options = {}) {
  if (!userSession()) {
    directNextCursor = "";
    reconcileDirectMessages([], { ...options, nextCursor: "" });
    return;
  }
  try {
    const requestRevision = directMessagesRevision;
    const cursor = String(options.cursor || "");
    const endpoint = cursor
      ? DIRECT_MESSAGES_ENDPOINT + "?cursor=" + encodeURIComponent(cursor)
      : DIRECT_MESSAGES_ENDPOINT;
    const data = await privateChannelRequest(endpoint);
    if (requestRevision !== directMessagesRevision) return;
    reconcileDirectMessages(data.conversations || [], {
      ...options,
      append: Boolean(cursor) || options.preserve === true,
      nextCursor: data.nextCursor || "",
    });
  } catch (error) {
    if (error?.code === "auth") {
      directNextCursor = "";
      reconcileDirectMessages([], { ...options, nextCursor: "" });
      if (isDirectMessageKey(activeChannel)) setActiveChannel(defaultChannelKey());
    }
  }
}

function loadMoreDirectMessages() {
  if (!directNextCursor) return;
  refreshDirectMessages({
    cursor: directNextCursor,
    selectSaved: false,
    connect: false,
  });
}

// The page's one authenticated push channel. It replaces the 30s re-read of
// the conversation list: an unread count only ever changes because somebody
// wrote to this account, and the relay says so the moment it happens.
let accountEvents = null;
let accountEventsConnectedOnce = false;

function startAccountEventChannel() {
  // Guests have no conversations to be unread, and no session to trade for a
  // ticket. Signing in reloads the page, which is where this runs.
  if (!userSession()?.sessionToken) return;
  if (accountEvents) {
    accountEvents.restart();
    return;
  }
  accountEvents = createAccountEventChannel({
    sessionToken: () => userSession()?.sessionToken || "",
    onTopic: (topic) => {
      // A new message in a conversation this browser is not sitting in, or a
      // conversation/channel that appeared because somebody else invited or
      // messaged us. Both lists used to be re-read every 30s to find these.
      if (topic === "direct-messages") {
        refreshDirectMessages({ preserve: true, selectSaved: false });
      } else if (topic === "private-channels") {
        refreshPrivateChannels();
      } else if (topic === "chat-read") {
        refreshReadMarks();
        refreshDirectMessages({ preserve: true, selectSaved: false });
      }
    },
    // One catch-up per RE-connect: a DM that arrived while the channel was
    // down pushed its frame into the void, and there is no fallback poll
    // behind this socket (docs/operations/polling-elimination.md). The first
    // connect needs no catch-up — the caller has just read both lists.
    onConnected: () => {
      refreshReadMarks();
      if (!accountEventsConnectedOnce) {
        accountEventsConnectedOnce = true;
        return;
      }
      refreshPrivateChannels();
      refreshDirectMessages({ preserve: true, selectSaved: false });
    },
  });
  accountEvents.start();
}

function newestMessageTs(channel) {
  const list = channelMessages.get(channel) || [];
  return list.length ? Math.floor(Number(list[list.length - 1].ts) || 0) : 0;
}

function queueReadMark(channel = activeChannel) {
  if (!userSession()?.sessionToken || document.hidden) return;
  if (channel !== activeChannel || !READ_MARK_KEY_RE.test(channel)) return;
  const ts = newestMessageTs(channel);
  if (ts <= 0 || ts > Date.now() + READ_MARK_FUTURE_SLACK_MS) return;
  if (ts <= (readMarks.get(channel) || 0)) return;
  pendingReadMarks.set(channel, ts);
  if (!readMarkTimer) {
    readMarkTimer = setTimeout(flushReadMarks, READ_MARK_POST_DELAY_MS);
  }
}

async function flushReadMarks() {
  readMarkTimer = 0;
  const marks = Object.fromEntries(
    [...pendingReadMarks].slice(0, READ_MARK_POST_MAX));
  pendingReadMarks.clear();
  if (!Object.keys(marks).length) return;
  for (const [channel, ts] of Object.entries(marks)) {
    readMarks.set(channel, Math.max(readMarks.get(channel) || 0, ts));
  }
  try {
    await privateChannelRequest(CHAT_READS_ENDPOINT, {
      method: "POST",
      body: JSON.stringify({ marks }),
    });
  } catch (_) {}
}

async function refreshReadMarks() {
  if (!userSession()?.sessionToken) return;
  let data;
  try {
    data = await privateChannelRequest(CHAT_READS_ENDPOINT);
  } catch (_) {
    return;
  }
  let changed = false;
  for (const [channel, value] of Object.entries(data?.marks || {})) {
    const ts = Number(value) || 0;
    if (ts > (readMarks.get(channel) || 0)) readMarks.set(channel, ts);
    const meta = channelMeta.get(channel);
    if (meta?.unread > 0 && meta.unreadTs > 0 &&
        meta.unreadTs <= readMarks.get(channel)) {
      meta.unread = 0;
      meta.unreadTs = 0;
      changed = true;
    }
  }
  if (changed) {
    renderRooms();
    renderDirectMessages();
  }
}

function markActiveDirectMessageReadIfDeferred() {
  if (document.hidden || !directReadDeferred.delete(activeChannel)) return;
  markDirectMessageRead(activeChannel);
}

async function markDirectMessageRead(channelKey) {
  const direct = directMessageForKey(channelKey);
  if (!direct) return;
  if (document.hidden || channelKey !== activeChannel) {
    directReadDeferred.add(channelKey);
    return;
  }
  if (directReadPending.has(direct.id)) return;
  directReadPending.add(direct.id);
  const meta = channelMeta.get(channelKey);
  if (meta) meta.unread = 0;
  renderDirectMessages();
  try {
    await privateChannelRequest(
      DIRECT_MESSAGES_ENDPOINT + "/" + direct.id + "/read",
      { method: "POST" },
    );
  } catch (_) {
  } finally {
    directReadPending.delete(direct.id);
  }
}

async function refreshPrivateChannels(options = {}) {
  if (!userSession()) {
    reconcilePrivateChannels([], options);
    return;
  }
  try {
    const data = await privateChannelRequest(PRIVATE_CHANNELS_ENDPOINT);
    officeAuthorizationExpired = false;
    showOfficeFailure("");
    reconcilePrivateChannels(data.channels || [], options);
  } catch (error) {
    if (error?.code === "auth") {
      officeAuthorizationExpired = true;
      reconcilePrivateChannels([], options);
      showOfficeFailure("auth");
    } else {
      showOfficeFailure("relay");
    }
  }
}

function privateChannelErrorMessage(error) {
  const messages = {
    invalid_channel_name: "Use lowercase letters, numbers, and hyphens.",
    channel_name_taken: "That channel name is already in use.",
    user_not_found: "That active registered user was not found.",
    too_many_channels: "This relay has reached its channel limit.",
    too_many_members: "This channel has reached its member limit.",
    invalid_visibility: "Choose Public or Private visibility.",
    invalid_members: "Choose registered users from the list.",
    members_not_allowed: "Public channels do not use member invitations.",
    invalid_topic: "Channel topics must be 200 characters or fewer.",
    invalid_webhook_name: "Webhook names must be 80 characters or fewer.",
    too_many_webhooks: "This channel has reached its webhook limit.",
    public_channel_required: "Controller integrations require a Public channel.",
    manager_required: "Only the channel creator or an administrator can manage it.",
    not_found: "That channel no longer exists.",
    admin_required: "Administrator access is required.",
    auth: OFFICE_SESSION_EXPIRED,
    invalid_session: OFFICE_SESSION_EXPIRED,
    unavailable: OFFICE_RELAY_UNAVAILABLE,
    request_failed: OFFICE_RELAY_UNAVAILABLE,
  };
  return messages[error?.code] || "The channel request could not be completed.";
}

function setChannelError(message) {
  // The create dialog and the settings panel each carry their own alert line;
  // only one of them is ever on screen.
  const text = String(message || "");
  if (channelError) channelError.textContent = text;
  if (channelSettingsError) channelSettingsError.textContent = text;
}

function activeManageableChannel() {
  const channel = privateChannelForKey(activeChannel);
  return channel?.canManage ? channel : null;
}

function activeTopicChannel() {
  const channel = privateChannelForKey(activeChannel);
  return channel?.canManage ? channel : null;
}

function renderChannelMembers(members) {
  if (!channelMembersEl) return;
  channelMembersEl.textContent = "";
  const list = Array.isArray(members) ? members : [];
  if (!list.length) {
    const empty = document.createElement("p");
    empty.textContent = "No invited members yet.";
    empty.className = "chat-attachment-meta";
    channelMembersEl.append(empty);
    return;
  }
  for (const member of list) {
    const username = String(member?.username || "").trim();
    if (!username) continue;
    const row = document.createElement("div");
    row.className = "chat-channel-member";
    const label = document.createElement("span");
    label.textContent = "@" + username;
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "chat-member-remove";
    remove.textContent = "Remove";
    remove.setAttribute("aria-label", `Remove ${username} from channel`);
    remove.addEventListener("click", () => removeChannelMember(username));
    row.append(label, remove);
    channelMembersEl.append(row);
  }
}

async function refreshChannelMembers() {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  if (channelMembersTitle) {
    channelMembersTitle.textContent = `Members of #${channel.name}`;
  }
  try {
    const data = await privateChannelRequest(
      `/api/chat/channels/${channel.id}/members`
    );
    renderChannelMembers(data.members || []);
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}


async function inviteChannelMember(username) {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  setChannelError("");
  try {
    await privateChannelRequest(`/api/chat/channels/${channel.id}/members`, {
      method: "POST",
      body: JSON.stringify({ username: String(username || "").trim().toLowerCase() }),
    });
    if (channelUsernameInput) channelUsernameInput.value = "";
    await refreshChannelMembers();
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function removeChannelMember(username) {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  setChannelError("");
  try {
    const removed = await privateChannelRequest(`/api/chat/channels/${channel.id}/members`, {
      method: "DELETE",
      body: JSON.stringify({ username }),
    });
    await refreshChannelMembers();
    await refreshPrivateChannels({ selectSaved: false });
    if (removed?.inviteReset) {
      renderChannelInviteLink(null);
      if (inviteLinkStatus) {
        inviteLinkStatus.textContent =
          "The invite link was turned off. Create a new one to keep inviting people.";
      }
    }
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

function channelInviteEndpoint(channel) {
  return `${PRIVATE_CHANNELS_ENDPOINT}/${channel.id}/invite`;
}

function renderChannelInviteLink(invite) {
  const url = String(invite?.url || "");
  if (inviteLinkInput) inviteLinkInput.value = url;
  if (inviteLinkCopyBtn) inviteLinkCopyBtn.textContent = url ? "Copy link" : "Create link";
  if (inviteLinkResetBtn) inviteLinkResetBtn.hidden = !url;
  if (inviteLinkOffBtn) inviteLinkOffBtn.hidden = !url;
}

async function refreshChannelInviteLink() {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  if (inviteLinkStatus) inviteLinkStatus.textContent = "";
  try {
    const data = await privateChannelRequest(channelInviteEndpoint(channel));
    renderChannelInviteLink(data.invite);
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function channelInviteUrl(channel, options = {}) {
  if (!options.fresh) {
    const data = await privateChannelRequest(channelInviteEndpoint(channel));
    if (data.invite?.url) return String(data.invite.url);
  }
  const made = await privateChannelRequest(channelInviteEndpoint(channel), {
    method: "POST",
    body: "{}",
  });
  return String(made.invite?.url || "");
}

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch (_) {
    return false;
  }
}

async function copyChannelInviteLink(options = {}) {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  setChannelError("");
  try {
    const url = await channelInviteUrl(channel, options);
    renderChannelInviteLink({ url });
    const copied = await copyText(url);
    if (!copied) inviteLinkInput?.select();
    if (inviteLinkStatus) {
      inviteLinkStatus.textContent = copied ? "Invite link copied" : "Copy the link above";
    }
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function turnOffChannelInviteLink() {
  const channel = activeManageableChannel();
  if (!channel || channel.visibility !== "private") return;
  setChannelError("");
  try {
    await privateChannelRequest(channelInviteEndpoint(channel), {
      method: "DELETE",
      body: "{}",
    });
    renderChannelInviteLink(null);
    if (inviteLinkStatus) inviteLinkStatus.textContent = "Invite link turned off";
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

function chatInviteErrorMessage(error) {
  if (error?.code === "invite_not_found" || error?.status === 404) {
    return "This invite link is no longer valid. Ask the room creator for a new one.";
  }
  if (error?.code === "too_many_members") {
    return "This room has reached its member limit.";
  }
  if (error?.code === "auth" || error?.code === "invalid_session") {
    return OFFICE_SESSION_EXPIRED;
  }
  return OFFICE_RELAY_UNAVAILABLE;
}

function clearChatLinkHash() {
  try {
    history.replaceState(history.state, "", location.pathname + location.search);
  } catch (_) {}
}

function showJoinDialog(title, copy, canJoin) {
  if (!joinDialog) return;
  if (joinTitle) joinTitle.textContent = title;
  if (joinCopy) joinCopy.textContent = copy;
  if (joinConfirmBtn) joinConfirmBtn.hidden = !canJoin;
  if (joinCancelBtn) joinCancelBtn.textContent = canJoin ? "Not now" : "Close";
  if (typeof joinDialog.showModal === "function") joinDialog.showModal();
  else joinDialog.setAttribute("open", "");
}

async function openJoinedChannel(channelId) {
  await refreshPrivateChannels({ selectSaved: false });
  if (privateChannels.has(channelId)) {
    setActiveChannel(privateChannelKey(channelId));
  }
}

async function openChatLinkFromHash() {
  const hash = String(location.hash || "");
  const room = CHAT_ROOM_HASH_RE.exec(hash);
  if (room) {
    clearChatLinkHash();
    if (privateChannels.has(room[1])) setActiveChannel(privateChannelKey(room[1]));
    return;
  }
  const invite = CHAT_INVITE_HASH_RE.exec(hash);
  if (!invite || isOfficeEmbed) return;
  const token = invite[1];
  if (!userSession()) {
    location.assign("/login?next=" + encodeURIComponent("/chat#invite=" + token));
    return;
  }
  clearChatLinkHash();
  let preview;
  try {
    preview = await privateChannelRequest(CHAT_INVITES_ENDPOINT + "/" + token);
  } catch (error) {
    showJoinDialog("Invite link not valid", chatInviteErrorMessage(error), false);
    return;
  }
  const channelId = String(preview.channel?.id || "");
  if (preview.joined) {
    await openJoinedChannel(channelId);
    return;
  }
  pendingInviteToken = token;
  const name = String(preview.channel?.name || "");
  const inviter = String(preview.invitedBy || "");
  showJoinDialog(
    `Join #${name}?`,
    (inviter ? `@${inviter} invited you to this private room. ` : "") +
      "Once you join, its members see you and can read what you post.",
    true,
  );
}

async function acceptPendingInvite() {
  const token = pendingInviteToken;
  pendingInviteToken = "";
  if (!token) return;
  try {
    const data = await privateChannelRequest(CHAT_INVITES_ENDPOINT + "/" + token, {
      method: "POST",
      body: "{}",
    });
    await openJoinedChannel(String(data.channel?.id || ""));
  } catch (error) {
    showJoinDialog("Could not join the room", chatInviteErrorMessage(error), false);
  }
}

function channelInviteeNames() {
  return [...new Set(String(channelInitialMembersInput?.value || "")
    .split(",")
    .map((value) => value.trim().replace(/^@/, "").toLowerCase())
    .filter(Boolean))];
}

function describeChannelAudience() {
  const audience = document.querySelector("#chat-channel-audience");
  const isPublic = channelVisibilityInput?.value === "public";
  if (channelInitialMembersInput) {
    channelInitialMembersInput.hidden = isPublic;
    channelInitialMembersInput.disabled = isPublic;
  }
  if (!audience) return;
  if (isPublic) {
    audience.textContent =
      "Every signed-in ForkMesh member can find, read and join this channel.";
    return;
  }
  const invitees = channelInviteeNames();
  audience.textContent = invitees.length
    ? `Only you and ${invitees.map((name) => "@" + name).join(", ")} can see or join this room. Nobody else, including administrators, is added.`
    : "Only you can see this room until you invite someone. Nobody else, including administrators, is added.";
}

async function createChannel() {
  if (!userSession()) return;
  setChannelError("");
  const name = String(channelNameInput?.value || "").trim().toLowerCase();
  const visibility = channelVisibilityInput?.value === "public"
    ? "public"
    : "private";
  const members = visibility === "private" ? channelInviteeNames() : [];
  try {
    const data = await privateChannelRequest(PRIVATE_CHANNELS_ENDPOINT, {
      method: "POST",
      body: JSON.stringify({ name, visibility, members }),
    });
    channelCreateForm?.reset();
    if (channelInitialMembersInput) channelInitialMembersInput.value = "";
    describeChannelAudience();
    channelDialog?.close();
    await refreshPrivateChannels({ selectSaved: false });
    const id = String(data?.channel?.id || "");
    if (id) setActiveChannel(privateChannelKey(id));
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

function renderChannelWebhooks(webhooks) {
  if (!channelWebhooksEl) return;
  channelWebhooksEl.textContent = "";
  const list = Array.isArray(webhooks) ? webhooks : [];
  if (!list.length) {
    const empty = document.createElement("p");
    empty.className = "chat-attachment-meta";
    empty.textContent = "No incoming webhooks yet.";
    channelWebhooksEl.append(empty);
    return;
  }
  for (const webhook of list) {
    const id = String(webhook?.id || "");
    const row = document.createElement("div");
    row.className = "chat-webhook-row";
    const head = document.createElement("div");
    head.className = "chat-webhook-row-head";
    const name = document.createElement("strong");
    name.textContent = String(webhook?.name || "Webhook");
    const actions = document.createElement("div");
    actions.className = "chat-webhook-row-actions";
    const rotate = document.createElement("button");
    rotate.type = "button";
    rotate.className = "chat-webhook-rotate";
    rotate.textContent = "New URL";
    rotate.title = "Replace this webhook's URL and show the new one";
    rotate.setAttribute("aria-label", `New URL for ${name.textContent} webhook`);
    rotate.addEventListener("click", () => rotateChannelWebhook(id));
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "chat-member-remove";
    remove.textContent = "Remove";
    remove.setAttribute("aria-label", `Remove ${name.textContent} webhook`);
    remove.addEventListener("click", () => removeChannelWebhook(id));
    actions.append(rotate, remove);
    head.append(name, actions);
    const usage = document.createElement("span");
    usage.className = "chat-attachment-meta";
    usage.textContent = Number(webhook?.lastUsedAt) > 0
      ? `Last used ${new Date(Number(webhook.lastUsedAt)).toLocaleString()}`
      : "Not used yet";
    row.append(head, usage);
    const revealed = revealedWebhookUrls.get(id);
    if (revealed) row.append(webhookUrlReveal(revealed));
    else {
      const hidden = document.createElement("span");
      hidden.className = "chat-attachment-meta";
      hidden.textContent =
        "URL hidden — the relay keeps no copy. Use New URL to get a working one.";
      row.append(hidden);
    }
    channelWebhooksEl.append(row);
  }
}

// The token behind a webhook URL is retained only as a blind index, so this
// block is the one place it can be read, for as long as the tab stays open.
function webhookUrlReveal(webhookUrl) {
  const wrap = document.createElement("div");
  wrap.className = "chat-webhook-reveal";
  const url = document.createElement("code");
  url.className = "chat-webhook-url";
  url.textContent = String(webhookUrl || "");
  const copy = document.createElement("button");
  copy.type = "button";
  copy.textContent = "Copy URL";
  copy.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(url.textContent);
      copy.textContent = "Copied";
    } catch (_) {
      copy.textContent = "Copy failed";
    }
  });
  wrap.append(url, copy);
  return wrap;
}

async function refreshChannelWebhooks() {
  const channel = activeManageableChannel();
  if (!channel) return;
  try {
    const data = await privateChannelRequest(
      `/api/chat/channels/${channel.id}/webhooks`,
    );
    renderChannelWebhooks(data.webhooks || []);
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

function noteWebhookUrl(webhookId, webhookUrl, message) {
  const id = String(webhookId || "");
  const url = String(webhookUrl || "");
  if (id && url) revealedWebhookUrls.set(id, url);
  if (!channelWebhookCreated) return;
  channelWebhookCreated.hidden = false;
  channelWebhookCreated.textContent = "";
  const label = document.createElement("strong");
  label.textContent = message;
  channelWebhookCreated.append(label);
}

async function createChannelWebhook() {
  const channel = activeManageableChannel();
  if (!channel) return;
  setChannelError("");
  try {
    const data = await privateChannelRequest(
      `/api/chat/channels/${channel.id}/webhooks`,
      {
        method: "POST",
        body: JSON.stringify({ name: channelWebhookNameInput?.value || "Webhook" }),
      },
    );
    if (channelWebhookNameInput) channelWebhookNameInput.value = "";
    noteWebhookUrl(
      data?.webhook?.id,
      data?.url,
      "Webhook added. Copy its URL below — it is shown only once.",
    );
    await refreshChannelWebhooks();
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function rotateChannelWebhook(webhookId) {
  const channel = activeManageableChannel();
  const id = String(webhookId || "");
  if (!channel || !/^[0-9a-f]{32}$/.test(id)) return;
  setChannelError("");
  try {
    const data = await privateChannelRequest(
      `/api/chat/channels/${channel.id}/webhooks/${id}`,
      { method: "POST", body: "{}" },
    );
    noteWebhookUrl(
      id,
      data?.url,
      "New URL issued. The previous one stopped working — copy this one now.",
    );
    await refreshChannelWebhooks();
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function removeChannelWebhook(webhookId) {
  const channel = activeManageableChannel();
  const id = String(webhookId || "");
  if (!channel || !/^[0-9a-f]{32}$/.test(id)) return;
  setChannelError("");
  try {
    await privateChannelRequest(
      `/api/chat/channels/${channel.id}/webhooks/${id}`,
      { method: "DELETE", body: "{}" },
    );
    await refreshChannelWebhooks();
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

// The dialog now only creates channels; everything about an existing one is
// in the settings panel next to its name.
function openChannelDialog(mode = "create") {
  const session = userSession();
  const creating = mode !== "manage";
  if (!creating) {
    openChannelSettings();
    return;
  }
  if (!session || !channelDialog) return;
  if (creating && !canAuthorChannelsHere()) return;
  setChannelError("");
  if (channelCreateSection) channelCreateSection.hidden = false;
  if (typeof channelDialog.showModal === "function") channelDialog.showModal();
  else channelDialog.setAttribute("open", "");
  channelNameInput?.focus();
}

function resetChannelDeleteConfirm() {
  if (channelDeleteConfirm) channelDeleteConfirm.hidden = true;
  if (channelDeleteBtn) channelDeleteBtn.hidden = false;
}

function populateChannelSettings() {
  const channel = activeManageableChannel();
  if (!channel) return;
  if (channelSettingsName) channelSettingsName.value = channel.name;
  if (channelSettingsTopic) channelSettingsTopic.value = channel.topic || "";
  if (channelSettingsVisibility) {
    channelSettingsVisibility.value = channel.visibility;
  }
  if (channelDeleteName) channelDeleteName.textContent = `#${channel.name}`;
  if (channelMembersTitle) {
    channelMembersTitle.textContent = `Members of #${channel.name}`;
  }
  // Public channels are open to every signed-in account, so they carry no
  // invitation list at all.
  if (channelMembersSection) {
    channelMembersSection.hidden = channel.visibility !== "private";
  }
  // A webhook originates at the controller and therefore cannot author a
  // recipient-sealed frame. Expose integrations only where the channel owner
  // has deliberately chosen controller-readable Public visibility.
  if (channelWebhooksSection) {
    channelWebhooksSection.hidden = channel.visibility !== "public";
  }
  resetChannelDeleteConfirm();
}

function openChannelSettings() {
  const channel = activeManageableChannel();
  if (!channel || !channelSettingsPanel || isOfficeEmbed) return;
  setChannelError("");
  if (channelSettingsSaved) channelSettingsSaved.textContent = "";
  if (channelWebhookCreated) channelWebhookCreated.hidden = true;
  populateChannelSettings();
  channelSettingsPanel.hidden = false;
  channelManageBtn?.setAttribute("aria-expanded", "true");
  if (channel.visibility === "private") {
    refreshChannelMembers();
    refreshChannelInviteLink();
  }
  if (channel.visibility === "public") refreshChannelWebhooks();
  channelSettingsName?.focus();
}

function closeChannelSettings(options = {}) {
  if (!channelSettingsPanel || channelSettingsPanel.hidden) return false;
  channelSettingsPanel.hidden = true;
  channelManageBtn?.setAttribute("aria-expanded", "false");
  resetChannelDeleteConfirm();
  if (options.restoreFocus && channelManageBtn && !channelManageBtn.hidden) {
    channelManageBtn.focus();
  }
  return true;
}

function toggleChannelSettings() {
  if (closeChannelSettings({ restoreFocus: true })) return;
  openChannelSettings();
}

async function saveChannelSettings() {
  const channel = activeManageableChannel();
  if (!channel) return;
  setChannelError("");
  if (channelSettingsSaved) channelSettingsSaved.textContent = "";
  const name = String(channelSettingsName?.value || "").trim().toLowerCase();
  const topic = String(channelSettingsTopic?.value || "")
    .trim()
    .replace(/\s+/g, " ");
  const visibility = channelSettingsVisibility?.value === "public"
    ? "public"
    : "private";
  try {
    const data = await privateChannelRequest(
      `${PRIVATE_CHANNELS_ENDPOINT}/${channel.id}`,
      {
        method: "PATCH",
        body: JSON.stringify({ name, topic, visibility }),
      },
    );
    const record = data?.channel || {};
    channel.name = String(record.name || channel.name);
    channel.topic = String(record.topic || "");
    channel.updatedAt = Number(record.updatedAt) || channel.updatedAt;
    // Leave visibility and keyVersion untouched until reconciliation compares
    // the old policy with the authoritative response. A policy change must
    // reconnect even when opening a private channel did not rotate its room.
    await refreshPrivateChannels({ selectSaved: false });
    updateChannelHeading();
    renderRooms();
    populateChannelSettings();
    if (channelSettingsSaved) channelSettingsSaved.textContent = "Saved";
    // Peers re-read the authoritative record instead of trusting the frame,
    // exactly as the /topic command does.
    runWhenConnected(() => send(makePlain("channel-topic", {
      channel: channelWireLabel(activeChannel),
    })));
  } catch (error) {
    setChannelError(privateChannelErrorMessage(error));
  }
}

async function deleteActiveChannel() {
  const channel = activeManageableChannel();
  if (!channel) return;
  setChannelError("");
  const label = `#${channel.name}`;
  try {
    await privateChannelRequest(
      `${PRIVATE_CHANNELS_ENDPOINT}/${channel.id}`,
      { method: "DELETE", body: "{}" },
    );
    closeChannelSettings();
    // Reconciling drops the channel and falls back to #general. That room
    // re-renders its log when it connects, so the notice has to wait for the
    // connection rather than land in a view that is about to be cleared.
    await refreshPrivateChannels({ selectSaved: false });
    runWhenConnected(() => appendSystem(`Channel ${label} was deleted.`));
  } catch (error) {
    resetChannelDeleteConfirm();
    setChannelError(privateChannelErrorMessage(error));
  }
}

function directMessageErrorMessage(error) {
  const messages = {
    user_not_found: "That active registered user was not found.",
    cannot_message_self: "Choose another person to message.",
    rate_limited: "Too many new conversations. Try again later.",
    auth: OFFICE_SESSION_EXPIRED,
    invalid_session: OFFICE_SESSION_EXPIRED,
    unavailable: OFFICE_RELAY_UNAVAILABLE,
    request_failed: OFFICE_RELAY_UNAVAILABLE,
  };
  return messages[error?.code] || "The direct message could not be opened.";
}

function setDirectMessageError(message) {
  if (directError) directError.textContent = String(message || "");
}

function renderDirectMessagePicker() {
  if (!directOptions) return;
  directOptions.textContent = "";
  const query = String(directSearch?.value || "").trim().toLowerCase();
  const currentName = String(userSession()?.nodeName || "").trim().toLowerCase();
  const adding = directDialogMode === "add";
  const partner = adding
    ? String(directMessageForKey(activeChannel)?.otherUser || "")
    : "";
  const candidates = new Map(registeredUsers);
  if (query) {
    for (const [name, user] of directSearchResults) {
      candidates.set(name, user);
    }
  }
  const users = [...candidates.values()]
    .filter((user) => user.name !== currentName && user.name !== partner)
    .filter((user) => !query || user.name.includes(query))
    .sort((a, b) => a.name.localeCompare(b.name));
  for (const user of users) {
    const option = document.createElement("button");
    option.type = "button";
    option.className = "chat-direct-option";
    option.setAttribute(
      "aria-label",
      adding ? `Add ${user.name}` : `Message ${user.name}`,
    );
    option.append(makeAvatar(user.name, "user"));
    const label = document.createElement("span");
    label.textContent = "@" + user.name;
    option.append(label);
    option.addEventListener("click", () => {
      if (directDialogMode === "add") void addPersonToDirectMessage(user.name);
      else void startDirectMessage(user.name);
    });
    directOptions.append(option);
  }
  if (directEmpty) {
    directEmpty.hidden = users.length > 0;
    directEmpty.textContent = candidates.size
      ? "No registered users match your search."
      : "No registered users are available yet.";
  }
}

async function refreshDirectMessageSearch() {
  const query = String(directSearch?.value || "").trim().toLowerCase();
  const requestId = ++directSearchRequest;
  directSearchResults.clear();
  if (!query) {
    renderDirectMessagePicker();
    return;
  }
  try {
    const data = await privateChannelRequest(
      DIRECT_MESSAGES_ENDPOINT + "/users?query=" + encodeURIComponent(query)
    );
    if (requestId !== directSearchRequest) return;
    for (const value of Array.isArray(data.users) ? data.users : []) {
      const name = String(value?.name || "").trim().toLowerCase();
      if (name) directSearchResults.set(name, { name, createdAt: 0 });
    }
    setDirectMessageError("");
  } catch (error) {
    if (requestId !== directSearchRequest) return;
    setDirectMessageError(directMessageErrorMessage(error));
  }
  renderDirectMessagePicker();
}

function scheduleDirectMessageSearch() {
  renderDirectMessagePicker();
  if (directSearchTimer) clearTimeout(directSearchTimer);
  directSearchTimer = setTimeout(() => {
    directSearchTimer = null;
    refreshDirectMessageSearch();
  }, 180);
}

function openDirectMessageDialog(mode = "new") {
  if (!userSession() || !directDialog || isOfficeEmbed) return;
  const direct = directMessageForKey(activeChannel);
  directDialogMode = mode === "add" && direct ? "add" : "new";
  if (directDialogTitle) {
    directDialogTitle.textContent = directDialogMode === "add"
      ? "Add someone to this conversation"
      : "New direct message";
  }
  if (directDialogHint) {
    directDialogHint.hidden = directDialogMode !== "add";
    directDialogHint.textContent = directDialogMode === "add" && direct
      ? "Direct messages stay one-to-one. Picking a person starts a new " +
        `private group channel with you, @${direct.otherUser}, and them.`
      : "";
  }
  setDirectMessageError("");
  if (directSearch) directSearch.value = "";
  directSearchResults.clear();
  directSearchRequest += 1;
  renderDirectMessagePicker();
  refreshUsersDirectory();
  if (typeof directDialog.showModal === "function") directDialog.showModal();
  else directDialog.setAttribute("open", "");
  directSearch?.focus();
}

function directGroupChannelName(names) {
  const parts = [];
  for (const name of names) {
    const slug = String(name || "").toLowerCase()
      .replace(/[^a-z0-9]+/g, "-")
      .replace(/^-+|-+$/g, "")
      .slice(0, 10)
      .replace(/-+$/g, "");
    if (slug) parts.push(slug);
  }
  const base = (parts.join("-").slice(0, 34).replace(/-+$/g, "") || "group");
  const suffix = Math.floor(Math.random() * 0x10000)
    .toString(16).padStart(4, "0");
  return `${base}-${suffix}`;
}

async function addPersonToDirectMessage(username) {
  const direct = directMessageForKey(activeChannel);
  if (!direct || !userSession()) return;
  const normalized = String(username || "").trim().toLowerCase();
  if (!normalized || normalized === direct.otherUser) return;
  setDirectMessageError("");
  const me = String(userSession()?.nodeName || "").trim().toLowerCase();
  const name = directGroupChannelName([me, direct.otherUser, normalized]);
  try {
    const data = await privateChannelRequest(PRIVATE_CHANNELS_ENDPOINT, {
      method: "POST",
      body: JSON.stringify({
        name,
        visibility: "private",
        members: [direct.otherUser, normalized],
      }),
    });
    directDialog?.close();
    await refreshPrivateChannels({ selectSaved: false });
    const id = String(data?.channel?.id || "");
    if (id) setActiveChannel(privateChannelKey(id));
    setStatus(
      `Private group #${data?.channel?.name || name} started with ` +
      `@${direct.otherUser} and @${normalized}`,
      "connected",
    );
  } catch (error) {
    const message = error?.code === "user_not_found"
      ? directMessageErrorMessage(error)
      : privateChannelErrorMessage(error);
    setDirectMessageError(message);
    if (!directDialog?.open) setStatus(message);
  }
}

async function startDirectMessage(username) {
  if (!userSession()) return;
  const normalized = String(username || "").trim().toLowerCase();
  if (!normalized) return;
  setDirectMessageError("");
  try {
    const data = await privateChannelRequest(DIRECT_MESSAGES_ENDPOINT, {
      method: "POST",
      body: JSON.stringify({ username: normalized }),
    });
    const conversation = data.conversation;
    if (!conversation?.id) throw new Error("Direct message unavailable.");
    directMessagesRevision += 1;
    reconcileDirectMessages(
      [...directMessages.values(), conversation],
      { selectSaved: false, connect: false },
    );
    if (directMessages.has(conversation.id)) {
      setActiveChannel(directMessageKey(conversation.id));
    }
    directDialog?.close();
  } catch (error) {
    const message = directMessageErrorMessage(error);
    setDirectMessageError(message);
    if (!directDialog?.open) setStatus(message);
  }
}

// ---- people (right pane) -------------------------------------------------------

// Coalesce roster paints: a history replay on join delivers hundreds of frames
// in a burst, and each one touches the roster.
let peopleRenderTimer = null;
function schedulePeopleRender() {
  if (peopleRenderTimer) return;
  peopleRenderTimer = setTimeout(() => {
    peopleRenderTimer = null;
    renderPeople();
    refreshDirectPresence();
  }, 100);
}

function noteRoster(plain) {
  const id = plain && plain.senderId;
  if (!id || String(id).startsWith("system:")) return;
  const name = String(plain.sender || "").trim().slice(0, MAX_NAME) || "peer";
  const kind = plain.accountKind === "user"
    ? (id === FORKBOT_SENDER_ID ? "bot" : "user")
    : plain.accountKind === "guest"
      ? "guest"
      : "node";
  // Use the frame's own timestamp (bounded by now): the relay replays retained
  // frames to a joining client, and a days-old replayed message must not paint
  // its author online. Never move lastSeen backwards either — a replayed old
  // frame (or old bye) can't demote a peer we've heard from more recently.
  const frameTs = Math.min(Number(plain.ts) || Date.now(), Date.now());
  const prev = roster.get(id);
  const prevSeen = prev ? prev.lastSeenMs : 0;
  let lastSeenMs;
  if (plain.type === "bye") {
    lastSeenMs = prevSeen > frameTs ? prevSeen : 0;
  } else {
    lastSeenMs = Math.max(prevSeen, frameTs);
  }
  roster.set(id, { id, name, kind, lastSeenMs });
  // Only a live account's beat speaks for its status: a replayed old frame
  // must not resurrect a status the member has since cleared.
  if (
    kind === "user" &&
    (plain.type === "hello" || plain.type === "presence") &&
    plain.userStatus &&
    Date.now() - frameTs <= PEER_STALE_MS
  ) {
    window.ForkMeshUserStatus?.remember(name, plain.userStatus, "frame");
  }
  schedulePeopleRender();
}

function noteSelfRoster() {
  roster.set(selfId, {
    id: selfId,
    name: displayName(),
    // Only an anonymous browser sits in the guest section: a signed-in person
    // in the public room stays a user, so their live entry dedupes against the
    // account-directory row instead of showing up as a second person.
    kind:
      roomScopeForChannel() === "public-world-general" && !userSession()
        ? "guest"
        : "user",
    lastSeenMs: Date.now(),
  });
  schedulePeopleRender();
}

function personIsOnline(person) {
  return person.lastSeenMs > 0 && Date.now() - person.lastSeenMs <= PEER_STALE_MS;
}

// An anonymous guest, or a World visitor from an older client that still
// prefixes its asserted name. Never a registered account or a node.
function personIsTransientVisitor(person) {
  if (!person || person.id === selfId) return false;
  if (person.kind === "guest") return true;
  return /^\s*(guest\b|world\s+guest\b|world\s+visitor\b)/i.test(
    String(person.name || ""),
  );
}

// Drop visitors that have gone quiet (or said "bye", which zeroes lastSeenMs),
// so the people pane only keeps rows that can still come back.
function forgetIdleVisitors() {
  const cutoff = Date.now() - VISITOR_IDLE_FORGET_MS;
  for (const [id, person] of [...roster]) {
    if (!personIsTransientVisitor(person)) continue;
    if (person.lastSeenMs > cutoff) continue;
    roster.delete(id);
  }
}

// ---- registered-user directory ------------------------------------------------
// The room roster only knows senders it has decrypted frames from, so a user
// who signed up on the website but never opened chat was invisible here. Merge
// in the public account directory: every registered user gets a (offline)
// roster entry, and a signup that appears between polls is announced in the
// log so the room can welcome them right away.

const directoryKnown = new Set(); // lowercased account names already merged
const registeredUsers = new Map();
let directorySeeded = false;

async function refreshUsersDirectory() {
  let users;
  try {
    const res = await fetch(USERS_DIRECTORY_ENDPOINT, {
      headers: { accept: "application/json" },
    });
    if (!res.ok) return;
    const data = await res.json().catch(() => null);
    users = data && Array.isArray(data.users) ? data.users : [];
  } catch (_) {
    return;
  }
  for (const user of users) {
    const name = String(user.name || "")
      .trim()
      .toLowerCase()
      .slice(0, MAX_ACCOUNT_NAME);
    if (!name) continue;
    registeredUsers.set(name, {
      name,
      createdAt: Number(user.createdAt) || 0,
    });
    window.ForkMeshUserStatus?.remember(name, user.userStatus);
    const fresh = !directoryKnown.has(name);
    directoryKnown.add(name);
    // Namespaced id so this offline placeholder never collides with a live
    // senderId entry for the same account; renderPeople dedupes the pair by
    // name, keeping the freshest sighting (i.e. the live one).
    const id = "account:" + name;
    const prev = roster.get(id);
    roster.set(id, {
      id,
      name,
      kind: "user",
      lastSeenMs: prev ? prev.lastSeenMs : 0,
    });
    if (fresh && directorySeeded &&
        Date.now() - Number(user.createdAt || 0) < NEW_USER_ANNOUNCE_WINDOW_MS) {
      appendSystem("🎉 " + name + " just joined ForkMesh - say hi!");
    }
  }
  directorySeeded = true;
  renderDirectMessagePicker();
  schedulePeopleRender();
  if (input === document.activeElement && mentionTokenAtCaret()) {
    updateMentionSuggest();
  }
}

// Record the current activity counters as "seen" so the chat icon in the site
// header (site-header.js reads the same key) shows no badge for what's on
// screen right now.
//
// This read is ABSOLUTE — it takes the server's counters rather than nudging
// the stored ones — which is what makes it the correction for any drift the
// incremental bumps below accumulate (two chat tabs each advance the one shared
// localStorage baseline for the same message). It used to run every 60s for
// that reason; it now runs when the page opens and when the reader leaves it,
// which is exactly when the baseline has to be right.
let chatActivitySeenAtMs = 0;

async function markChatActivitySeen() {
  chatActivitySeenAtMs = Date.now();
  try {
    const res = await fetch(CHAT_ACTIVITY_ENDPOINT, {
      headers: { accept: "application/json" },
      // The read on the way out races the page unload; keepalive is what lets
      // it (and the localStorage write below) still land.
      keepalive: true,
    });
    if (!res.ok) return;
    const data = await res.json().catch(() => null);
    if (!data || !data.ok) return;
    localStorage.setItem(CHAT_ACTIVITY_SEEN_KEY, JSON.stringify({
      messageCount: Number(data.messageCount) || 0,
      userCount: Number(data.userCount) || 0,
      at: Date.now(),
    }));
  } catch (_) {}
}

// You have obviously already read your own message, and — with the chat page
// open in front of you — everything that arrives in it too. So neither must
// light the header's chat badge. That badge is a delta between the retained
// #general count and this browser's stored baseline, so every retained line
// that crosses this page advances the baseline by one. Doing it here, off the
// socket, is what lets the page stop re-reading the counters on a timer:
// re-baselining used to mean a GET /api/chat/activity every minute.
function noteSeenChatActivity() {
  try {
    const raw = localStorage.getItem(CHAT_ACTIVITY_SEEN_KEY);
    if (!raw) return; // no baseline yet: the header seeds one silently
    const seenActivity = JSON.parse(raw);
    if (!seenActivity || typeof seenActivity !== "object") return;
    localStorage.setItem(CHAT_ACTIVITY_SEEN_KEY, JSON.stringify({
      ...seenActivity,
      messageCount: (Number(seenActivity.messageCount) || 0) + 1,
      at: Date.now(),
    }));
  } catch (_) {}
}

// Leaving the chat page is the moment the header badge starts mattering again,
// so re-baseline absolutely on the way out. Not a timer: it fires on the
// reader's own navigation or tab switch, with a floor so flicking between tabs
// cannot turn it into one.
const CHAT_ACTIVITY_REBASELINE_FLOOR_MS = 5000;

function rebaselineChatActivityOnLeave(event) {
  // visibilitychange fires on the way in as well; pagehide only ever fires on
  // the way out, and does so while the document is still visible.
  if (event?.type === "visibilitychange" && !document.hidden) return;
  if (Date.now() - chatActivitySeenAtMs < CHAT_ACTIVITY_REBASELINE_FLOOR_MS) {
    return;
  }
  markChatActivitySeen();
}

function activeRoomMembers(channel = activeChannel) {
  const direct = directMessageForKey(channel);
  if (direct) {
    const self = String(userSession()?.nodeName || "").trim().toLowerCase();
    return [self, String(direct.otherUser || "").trim().toLowerCase()]
      .filter(Boolean);
  }
  const record = privateChannelForKey(channel);
  if (isEveryoneChannel(record)) {
    return everyoneChannelMembers.get(record.id)?.names || null;
  }
  if (record && record.visibility !== "public") {
    return Array.isArray(record.members) ? record.members : [];
  }
  return null;
}

function renderPeople() {
  if (!peopleEl) return;
  // Sweep expired visitors first (this also runs on a 30s timer, so they clear
  // even in a silent room).
  forgetIdleVisitors();
  peopleEl.textContent = "";
  const currentName = String(userSession()?.nodeName || "").trim().toLowerCase();
  // Collapse multiple entries for the same person into one row: a person can
  // surface under several ids (history-replayed old senderIds, or the same
  // account from other tabs/devices) that all carry the same display name.
  // Dedupe WITHIN a section (users, nodes) by lowercased name — never across
  // kinds — keeping the freshest sighting so online state wins; fall back to
  // the id when a name is somehow missing.
  const dedupePeople = (list) => {
    const byIdentity = new Map();
    for (const person of list) {
      const key = (String(person.name || "").trim().toLowerCase() || person.id);
      const prev = byIdentity.get(key);
      if (!prev || person.lastSeenMs > prev.lastSeenMs) byIdentity.set(key, person);
    }
    return [...byIdentity.values()];
  };
  const bySection = { user: [], guest: [], node: [] };
  for (const person of roster.values()) {
    const section = person.kind === "node"
      ? "node"
      : person.kind === "guest"
        ? "guest"
        : "user";
    bySection[section].push(person);
  }
  bySection.user = dedupePeople(bySection.user);
  bySection.guest = dedupePeople(bySection.guest);
  bySection.node = dedupePeople(bySection.node);
  const roomMembers = activeRoomMembers();
  if (roomMembers) {
    const allowed = new Set(roomMembers);
    const seenMembers = new Set();
    bySection.user = bySection.user.filter((person) => {
      const key = String(person.name || "").trim().toLowerCase();
      if (!allowed.has(key)) return false;
      seenMembers.add(key);
      return true;
    });
    for (const name of roomMembers) {
      if (!seenMembers.has(name)) {
        bySection.user.push({ id: "member:" + name, name, kind: "user", lastSeenMs: 0 });
      }
    }
    bySection.guest = [];
    bySection.node = [];
  }
  const sortPeople = (list) =>
    list.sort((a, b) =>
      (personIsOnline(b) - personIsOnline(a)) ||
      a.name.localeCompare(b.name));
  const onlineCount =
    [...bySection.user, ...bySection.guest, ...bySection.node]
      .filter(personIsOnline).length;
  const everyone = privateChannelForKey(activeChannel);
  if (peopleTitleEl && isEveryoneChannel(everyone)) {
    peopleTitleEl.textContent =
      `Members · ${everyone.memberCount || bySection.user.length}`;
    peopleTitleEl.title = "Every ForkMesh member is in this channel.";
  } else if (peopleTitleEl) {
    peopleTitleEl.textContent = roomMembers
      ? `Members · ${bySection.user.length}`
      : `People · ${onlineCount} online`;
    peopleTitleEl.title = roomMembers
      ? "Only these people can see or join this conversation."
      : "";
  }
  if (peopleCountEl) peopleCountEl.textContent = String(onlineCount);
  const query = String(peopleSearchInput?.value || "").trim().toLowerCase()
    .replace(/^@/, "");
  const matchesQuery = (person) =>
    !query || String(person.name || "").toLowerCase().includes(query);
  let shown = 0;
  const renderSection = (title, allOfSection) => {
    const list = allOfSection.filter(matchesQuery);
    if (!list.length) return;
    shown += list.length;
    const head = document.createElement("div");
    head.className = "chat-people-section";
    const headLabel = document.createElement("span");
    headLabel.textContent = title;
    const headCount = document.createElement("span");
    headCount.className = "chat-people-section-count";
    headCount.textContent = String(list.length);
    head.append(headLabel, headCount);
    peopleEl.append(head);
    for (const person of sortPeople(list)) {
      // Each row links to the person's profile at /@username on this relay
      // (relative URL — a self-hosted relay links to its own pages). ForkBot
      // isn't an account, so its row stays a plain div.
      const hasProfile = person.kind === "user";
      const row = document.createElement(hasProfile ? "a" : "div");
      const online = personIsOnline(person);
      const away = hasProfile && online && userIsAway(person.name);
      row.className = "chat-person" + (online ? "" : " is-offline") +
        (away ? " is-away" : "");
      const statusLabel = hasProfile
        ? window.ForkMeshUserStatus?.describe(
          window.ForkMeshUserStatus.forName(person.name)) || ""
        : "";
      row.title = person.name + (online ? " · online" : " · offline") +
        (statusLabel ? " · " + statusLabel : "");
      if (hasProfile) row.href = mentionProfilePath(person.name);
      const face = document.createElement("span");
      face.className = "chat-person-face";
      face.append(makeAvatar(person.name, person.kind));
      row.append(face);
      const label = document.createElement("span");
      label.className = "chat-person-name";
      label.textContent = person.name;
      row.append(label);
      const statusSlot = document.createElement("span");
      statusSlot.className = "chat-person-status";
      const personBadge = hasProfile ? userStatusBadge(person.name) : null;
      if (personBadge) statusSlot.append(personBadge);
      row.append(statusSlot);
      if (hasProfile && currentName && person.name.toLowerCase() === currentName) {
        const you = document.createElement("span");
        you.className = "chat-you-chip";
        you.textContent = "you";
        row.append(you);
      }
      if (
        hasProfile
        && currentName
        && person.name.toLowerCase() === currentName
        && memberPlanTier
      ) {
        const chip = document.createElement("span");
        chip.className = "chat-plan-chip";
        chip.textContent = memberPlanTier === "pro" ? "PRO" : "SUPPORTER";
        chip.title = memberPlanTier === "pro"
          ? "ForkMesh Pro member" : "ForkMesh Supporter";
        row.append(chip);
      }
      const dot = document.createElement("span");
      dot.className = "chat-presence-dot" + (online ? " is-online" : "") +
        (away ? " is-away" : "");
      face.append(dot);
      if (hasProfile && currentName && person.name.toLowerCase() !== currentName) {
        const wrapper = document.createElement("div");
        wrapper.className = "chat-person-row";
        const message = document.createElement("button");
        message.type = "button";
        message.className = "chat-person-message";
        message.append(chatIcon("mail", 15));
        message.setAttribute("aria-label", `Message ${person.name}`);
        message.title = `Message ${person.name}`;
        message.addEventListener("click", () => startDirectMessage(person.name));
        wrapper.append(row, message);
        peopleEl.append(wrapper);
      } else {
        peopleEl.append(row);
      }
    }
  };
  renderSection("Online", bySection.user.filter(personIsOnline));
  renderSection("Offline", bySection.user.filter((person) => !personIsOnline(person)));
  renderSection("World guests", bySection.guest);
  renderSection("Nodes", bySection.node);
  if (query && !shown) {
    const empty = document.createElement("p");
    empty.className = "chat-people-empty";
    empty.textContent = `No one matches “${query}”.`;
    peopleEl.append(empty);
  }
}

// ---- people pane controls -----------------------------------------------------

const peopleToggleBtn = document.querySelector("#chat-people-toggle");
const peopleCloseBtn = document.querySelector("#chat-people-close");
const inviteCopyBtn = document.querySelector("#chat-invite-copy");
const inviteLabelEl = document.querySelector("#chat-invite-label");
const PEOPLE_HIDDEN_KEY = "forkmesh.chat.peopleHidden";
// Narrow layouts have no room for a third column, so the list opens as a
// drawer there instead of toggling the column.
const drawerPeopleQuery = window.matchMedia("(max-width: 900px)");

peopleSearchInput?.addEventListener("input", () => renderPeople());
peopleSearchInput?.addEventListener("keydown", (event) => {
  if (event.key !== "Escape") return;
  peopleSearchInput.value = "";
  renderPeople();
});

function paintPeopleToggle() {
  if (!panesEl || !peopleToggleBtn) return;
  const open = drawerPeopleQuery.matches
    ? panesEl.classList.contains("is-people-drawer-open")
    : !panesEl.classList.contains("is-people-hidden");
  peopleToggleBtn.setAttribute("aria-expanded", String(open));
  peopleToggleBtn.classList.toggle("is-active", open);
}

function setPeopleOpen(open) {
  if (!panesEl) return;
  if (drawerPeopleQuery.matches) {
    panesEl.classList.toggle("is-people-drawer-open", open);
  } else {
    panesEl.classList.toggle("is-people-hidden", !open);
    try { localStorage.setItem(PEOPLE_HIDDEN_KEY, open ? "0" : "1"); } catch (_) {}
  }
  paintPeopleToggle();
}

try {
  if (localStorage.getItem(PEOPLE_HIDDEN_KEY) === "1") {
    panesEl?.classList.add("is-people-hidden");
  }
} catch (_) {}
paintPeopleToggle();
drawerPeopleQuery.addEventListener?.("change", () => {
  panesEl?.classList.remove("is-people-drawer-open");
  paintPeopleToggle();
});
peopleToggleBtn?.addEventListener("click", () => {
  const open = peopleToggleBtn.getAttribute("aria-expanded") === "true";
  setPeopleOpen(!open);
});
peopleCloseBtn?.addEventListener("click", () => setPeopleOpen(false));
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && panesEl?.classList.contains("is-people-drawer-open")) {
    setPeopleOpen(false);
  }
});

function flashInviteLabel(text) {
  if (!inviteLabelEl) return;
  inviteLabelEl.textContent = text;
  setTimeout(() => { inviteLabelEl.textContent = "Copy invite link"; }, 2500);
}

inviteCopyBtn?.addEventListener("click", async () => {
  const channel = privateChannelForKey(activeChannel);
  let link = new URL("/chat", location.origin).href;
  if (channel?.visibility === "private") {
    if (!channel.canManage) {
      flashInviteLabel("Only the room creator can share an invite link");
      return;
    }
    try {
      link = await channelInviteUrl(channel);
    } catch (error) {
      flashInviteLabel(privateChannelErrorMessage(error));
      return;
    }
  } else if (channel) {
    link = new URL("/chat#room=" + channel.id, location.origin).href;
  }
  flashInviteLabel(await copyText(link) ? "Invite link copied" : link);
});

// ---- reactions ------------------------------------------------------------------

function reactionKey(plain) {
  return String(plain.reactorId || plain.senderId || "");
}

function applyReaction(plain) {
  const target = String(plain.target || "");
  const emoji = String(plain.emoji || "").slice(0, 8);
  const reactor = reactionKey(plain);
  if (!target || !emoji || !reactor) return;
  const perMessage = reactions.get(target) || new Map();
  reactions.set(target, perMessage);
  const perEmoji = perMessage.get(emoji) || new Map();
  perMessage.set(emoji, perEmoji);
  if (plain.added) {
    perEmoji.set(reactor, String(plain.reactorName || plain.sender || "peer"));
  } else {
    perEmoji.delete(reactor);
  }
  if (!perEmoji.size) perMessage.delete(emoji);
  renderReactions(target);
}

function renderReactions(messageId) {
  const rec = rows.get(messageId) || threadRenderedRows.get(messageId);
  if (!rec || !rec.reactionsEl) return;
  rec.reactionsEl.textContent = "";
  const perMessage = reactions.get(messageId);
  if (!perMessage) return;
  for (const [emoji, reactors] of perMessage) {
    if (!reactors.size) continue;
    const chip = document.createElement("button");
    chip.type = "button";
    chip.className = "chat-reaction-chip" + (reactors.has(selfId) ? " is-mine" : "");
    chip.title = [...reactors.values()].join(", ");
    const face = document.createElement("span");
    face.textContent = emoji;
    const count = document.createElement("span");
    count.className = "chat-reaction-count";
    count.textContent = String(reactors.size);
    chip.append(face, count);
    chip.addEventListener("click", () => toggleReaction(messageId, emoji));
    rec.reactionsEl.append(chip);
  }
}

function toggleReaction(messageId, emoji) {
  if (!canJoinChannel()) {
    lockChatForNonUser();
    return;
  }
  const rec = rows.get(messageId) || threadRenderedRows.get(messageId) ||
    threads.target(messageId);
  const mine = Boolean(reactions.get(messageId)?.get(emoji)?.has(selfId));
  const plain = makePlain("reaction", {
    conversation: channelWireLabel((rec && rec.channel) || activeChannel),
    target: messageId,
    emoji,
    reactorId: selfId,
    reactorName: displayName(),
    added: !mine,
  });
  seen.add(plain.id);
  applyReaction(plain);
  runWhenConnected(() => send(plain));
}

function ensureEmojiPicker() {
  if (emojiPickerEl) return emojiPickerEl;
  emojiPickerEl = document.createElement("div");
  emojiPickerEl.className = "chat-emoji-picker";
  emojiPickerEl.hidden = true;
  for (const emoji of REACTION_EMOJI) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.textContent = emoji;
    btn.addEventListener("click", () => {
      if (emojiPickerTarget) toggleReaction(emojiPickerTarget, emoji);
      hideEmojiPicker();
    });
    emojiPickerEl.append(btn);
  }
  document.body.append(emojiPickerEl);
  document.addEventListener("click", (event) => {
    if (!emojiPickerEl.hidden && !emojiPickerEl.contains(event.target)) hideEmojiPicker();
  }, true);
  return emojiPickerEl;
}

function hideEmojiPicker() {
  if (emojiPickerEl) emojiPickerEl.hidden = true;
  emojiPickerTarget = null;
}

function showEmojiPicker(anchor, messageId) {
  const picker = ensureEmojiPicker();
  emojiPickerTarget = messageId;
  picker.hidden = false;
  const rect = anchor.getBoundingClientRect();
  const margin = 8;
  const width = picker.offsetWidth || 220;
  const x = Math.max(margin, Math.min(rect.right - width, window.innerWidth - width - margin));
  const y = Math.max(margin, rect.top - picker.offsetHeight - 6);
  picker.style.left = `${x + window.scrollX}px`;
  picker.style.top = `${y + window.scrollY}px`;
}

// ---- message log (middle pane) --------------------------------------------------

function logNearBottom() {
  return logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 60;
}

function scrollLogToBottom() {
  logEl.scrollTop = logEl.scrollHeight;
}

// "Jump to latest": shown once the reader scrolls well above the newest
// message, counting messages that arrive while they are up there.
const jumpLatestBtn = document.querySelector("#chat-jump-latest");
const jumpLatestLabel = document.querySelector("#chat-jump-latest-label");
let missedWhileScrolledUp = 0;

function paintJumpLatest() {
  if (!jumpLatestBtn || !logEl) return;
  const away = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight > 240;
  if (!away) missedWhileScrolledUp = 0;
  jumpLatestBtn.hidden = !away;
  if (jumpLatestLabel) {
    jumpLatestLabel.textContent = missedWhileScrolledUp > 0
      ? `${missedWhileScrolledUp} new ${missedWhileScrolledUp === 1 ? "message" : "messages"}`
      : "Jump to latest";
  }
  jumpLatestBtn.classList.toggle("has-new", missedWhileScrolledUp > 0);
}

logEl?.addEventListener("scroll", paintJumpLatest, { passive: true });
jumpLatestBtn?.addEventListener("click", () => {
  missedWhileScrolledUp = 0;
  logEl.scrollTo({ top: logEl.scrollHeight, behavior: "smooth" });
  input?.focus();
});
if (logEl && typeof MutationObserver === "function") {
  new MutationObserver((mutations) => {
    let added = 0;
    for (const mutation of mutations) {
      // A cleared log is a channel re-render, not newly arrived messages.
      if (mutation.removedNodes.length) {
        added = 0;
        missedWhileScrolledUp = 0;
        break;
      }
      for (const node of mutation.addedNodes) {
        if (node.classList?.contains("chat-msg")) added += 1;
      }
    }
    if (added && !logNearBottom()) missedWhileScrolledUp += added;
    paintJumpLatest();
  }).observe(logEl, { childList: true });
}

function revokeAttachmentUrl(record) {
  if (!record || !record.attachmentUrl) return;
  URL.revokeObjectURL(record.attachmentUrl);
  record.attachmentUrl = "";
}

let imagePreviewDialog = null;

function openImagePreview(src, fileName) {
  if (!imagePreviewDialog) {
    imagePreviewDialog = document.createElement("dialog");
    imagePreviewDialog.id = "chat-image-preview-dialog";
    imagePreviewDialog.className = "chat-image-preview-dialog";
    imagePreviewDialog.innerHTML = '<button type="button" class="chat-image-preview-close" aria-label="Close image preview">×</button><img />';
    imagePreviewDialog.querySelector("button").addEventListener("click", () => imagePreviewDialog.close());
    imagePreviewDialog.addEventListener("click", (event) => {
      if (event.target === imagePreviewDialog) imagePreviewDialog.close();
    });
    document.body.append(imagePreviewDialog);
  }
  const image = imagePreviewDialog.querySelector("img");
  image.src = src;
  image.alt = fileName;
  imagePreviewDialog.showModal();
}

function portableMessageRecord(record) {
  return {
    id: record.id,
    rootId: record.rootId || "",
    channel: record.channel,
    ts: Number(record.ts) || Date.now(),
    senderId: record.senderId || "",
    sender: record.sender || "peer",
    text: record.text || "",
    richText: record.richText || null,
    attachment: record.attachment || null,
    editedAt: Number(record.editedAt) || 0,
    self: Boolean(record.self),
    deleted: Boolean(record.deleted),
    recipients: knownRecipientCount(record.recipients),
    recipientNames: knownRecipientNames(record.recipientNames),
    receipt: typeof record.receipt === "string" ? record.receipt : "",
    relayed: Boolean(record.relayed),
  };
}

function knownRecipientCount(value) {
  return Number.isSafeInteger(value) && value >= 0 ? value : null;
}

function knownRecipientNames(value) {
  if (!Array.isArray(value)) return [];
  return value
    .filter((name) => typeof name === "string" &&
      /^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$/.test(name))
    .slice(0, 20);
}

function recipientCountTitle(count, names = []) {
  const title = count === 1
    ? "Delivered to 1 person"
    : `Delivered to ${count} people`;
  const shown = knownRecipientNames(names).slice(0, count);
  if (!shown.length) return title;
  const more = count - shown.length;
  return `${title}: ${shown.map((name) => `@${name}`).join(", ")}` +
    (more > 0 ? ` and ${more} more` : "");
}

function paintRecipientLabel(label, value, names = []) {
  if (!label) return;
  const count = knownRecipientCount(value);
  label.hidden = count === null;
  label.textContent = count === null ? "" : `to ${count}`;
  if (count === null) {
    label.removeAttribute("title");
    label.removeAttribute("aria-label");
  } else {
    label.title = recipientCountTitle(count, names);
    label.setAttribute("aria-label", recipientCountTitle(count, names));
  }
}

function makeRecipientLabel(value, className = "chat-recipients", names = []) {
  const label = document.createElement("span");
  label.className = className;
  paintRecipientLabel(label, value, names);
  return label;
}

function applyRecipientCount(id, value, names = []) {
  const count = knownRecipientCount(value);
  if (count === null) return;
  const recipientNames = knownRecipientNames(names);
  const rec = rows.get(id);
  if (rec) {
    rec.recipients = count;
    rec.recipientNames = recipientNames;
    paintRecipientLabel(rec.recipientsEl, count, recipientNames);
    paintReceipt(rec);
    threads.updateTarget(id, { recipients: count, recipientNames });
    if (activeThreadRootId === id) renderThread();
    return;
  }
  const reply = threads.target(id);
  if (!reply?.rootId) return;
  threads.updateTarget(id, { recipients: count, recipientNames });
  if (activeThreadRootId === reply.rootId) renderThread();
}

const RECEIPT_LABELS = {
  pending: "Sending",
  sent: "Sent",
  delivered: "Delivered",
  read: "Read",
};
const RECEIPT_FRONT_TICK = "M11.7 7.2L13.4 8.9L16.4 5.6";
const RECEIPT_BACK_TICK = "M6.2 7.2L7.9 8.9L8.87 7.86";
const RECEIPT_BACK_TICK_READ = "M6.2 7.2L7.33 8.33";
const RECEIPT_BACK_RING = "M11.25 2.59A5.2 5.2 0 1 0 11.25 11.41";
const RECEIPT_BACK_DISC =
  "M10.21 1.35A5.9 5.9 0 1 0 10.21 12.65A6.8 6.8 0 0 1 10.21 1.35Z";

function receiptState(record) {
  if (!record?.self || record.deleted) return "";
  if (isDirectMessageKey(record.channel) &&
      (record.receipt === "read" || record.receipt === "delivered")) {
    return record.receipt;
  }
  if (record.receipt === "sent" || record.relayed ||
      knownRecipientCount(record.recipients) !== null) {
    return "sent";
  }
  return "pending";
}

function receiptIcon(state) {
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("viewBox", "0 0 22 14");
  svg.setAttribute("fill", "none");
  svg.setAttribute("stroke", "currentColor");
  svg.setAttribute("stroke-width", "1.4");
  svg.setAttribute("stroke-linecap", "round");
  svg.setAttribute("stroke-linejoin", "round");
  svg.setAttribute("aria-hidden", "true");
  const add = (tag, attributes) => {
    const shape = document.createElementNS(ns, tag);
    for (const [name, value] of Object.entries(attributes)) {
      shape.setAttribute(name, value);
    }
    svg.append(shape);
  };
  const front = { cx: "14", cy: "7", r: "5.2" };
  if (state === "read") {
    add("path", { d: RECEIPT_BACK_DISC, fill: "currentColor", stroke: "none" });
    add("circle", { ...front, fill: "currentColor" });
    add("path", { d: RECEIPT_BACK_TICK_READ, class: "chat-receipt-check" });
    add("path", { d: RECEIPT_FRONT_TICK, class: "chat-receipt-check" });
    return svg;
  }
  if (state === "delivered") {
    add("path", { d: RECEIPT_BACK_RING });
    add("path", { d: RECEIPT_BACK_TICK });
  }
  if (state === "pending") front["stroke-dasharray"] = "1.6 1.7";
  add("circle", front);
  if (state !== "pending") add("path", { d: RECEIPT_FRONT_TICK });
  return svg;
}

function paintReceipt(record) {
  const mark = record?.receiptEl;
  if (!mark) return;
  const state = receiptState(record);
  if (!state || mark.dataset.state === state) return;
  mark.dataset.state = state;
  mark.className = "chat-receipt is-" + state;
  mark.title = RECEIPT_LABELS[state];
  mark.setAttribute("aria-label", RECEIPT_LABELS[state]);
  mark.replaceChildren(receiptIcon(state));
}

function makeReceipt(record) {
  const mark = document.createElement("span");
  mark.setAttribute("role", "img");
  record.receiptEl = mark;
  paintReceipt(record);
  return mark;
}

function updateReceipt(id, receipt, allow) {
  const rec = rows.get(id);
  if (rec) {
    if (!allow(receiptState(rec))) return false;
    rec.receipt = receipt;
    paintReceipt(rec);
    threads.updateTarget(id, { receipt });
    if (activeThreadRootId === id) renderThread();
    return true;
  }
  const reply = threads.target(id);
  if (!reply?.rootId || !allow(receiptState(reply))) return false;
  threads.updateTarget(id, { receipt });
  if (activeThreadRootId === reply.rootId) renderThread();
  return true;
}

function applyReceiptSent(id) {
  updateReceipt(String(id || ""), "sent", (state) => state === "pending");
}

function applyDirectDelivered(ids) {
  for (const id of ids) {
    const rec = rows.get(id) || threads.target(id);
    if (!isDirectMessageKey(rec?.channel)) continue;
    updateReceipt(id, "delivered",
      (state) => state === "pending" || state === "sent");
  }
}

function applyDirectRead(scope) {
  if (!isDirectMessageKey(scope)) return;
  const readable = (state) => state === "sent" || state === "delivered";
  for (const rec of channelMessages.get(scope) || []) {
    if (readable(receiptState(rec))) {
      rec.receipt = "read";
      paintReceipt(rec);
      threads.updateTarget(rec.id, { receipt: "read" });
    }
    for (const reply of threads.replies(rec.id)) {
      if (readable(receiptState(reply))) {
        threads.updateTarget(reply.id, { receipt: "read" });
      }
    }
  }
  if (activeThreadRootId &&
      (rows.get(activeThreadRootId) || threads.root(activeThreadRootId))
        ?.channel === scope) {
    renderThread();
  }
}

function attachmentCard(attachment, objectUrl) {
  const card = document.createElement("div");
  card.className = "chat-attachment-card";
  const icon = document.createElement("span");
  icon.textContent = attachmentKind(attachment.fileMime);
  icon.setAttribute("aria-hidden", "true");
  const info = document.createElement("div");
  info.className = "chat-attachment-info";
  const name = document.createElement("div");
  name.className = "chat-attachment-name";
  name.textContent = attachment.fileName;
  name.title = attachment.fileName;
  const meta = document.createElement("div");
  meta.className = "chat-attachment-meta";
  meta.textContent = `${attachment.fileMime} - ${formatAttachmentSize(attachment.size)}`;
  info.append(name, meta);
  const link = document.createElement("a");
  link.className = "chat-attachment-download";
  link.href = objectUrl;
  link.download = attachment.fileName;
  link.textContent = "Download";
  link.setAttribute("aria-label", `Download ${attachment.fileName}`);
  card.append(icon, info, link);
  return card;
}

function attachmentKind(fileMime) {
  const mime = String(fileMime || "");
  if (mime.startsWith("image/")) return "Image";
  if (mime.startsWith("video/")) return "Video";
  return "File";
}

function renderAttachment(record) {
  const attachment = record && record.attachment;
  if (!attachment) return null;
  let bytes;
  try {
    bytes = b64ToBytes(attachment.file);
  } catch (_) {
    return null;
  }
  revokeAttachmentUrl(record);
  const blobType = attachmentBlobType(attachment.fileMime);
  const blob = new Blob([bytes], { type: blobType });
  const objectUrl = URL.createObjectURL(blob);
  record.attachmentUrl = objectUrl;

  const wrapper = document.createElement("div");
  wrapper.className = "chat-attachment";
  if (blobType.startsWith("image/")) {
    const preview = document.createElement("div");
    preview.className = "chat-attachment-image-preview";
    const image = document.createElement("img");
    image.className = "chat-attachment-image";
    image.src = objectUrl;
    image.alt = attachment.fileName;
    image.loading = "lazy";
    image.tabIndex = 0;
    image.setAttribute("role", "button");
    image.setAttribute("aria-label", `Open ${attachment.fileName} full size`);
    image.addEventListener("click", () => openImagePreview(objectUrl, attachment.fileName));
    image.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        openImagePreview(objectUrl, attachment.fileName);
      }
    });
    const download = document.createElement("a");
    download.className = "chat-attachment-image-download";
    download.href = objectUrl;
    download.download = attachment.fileName;
    download.setAttribute("aria-label", `Download ${attachment.fileName}`);
    download.title = `Download ${attachment.fileName}`;
    download.innerHTML = '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3v12m0 0 4-4m-4 4-4-4M5 21h14"></path></svg>';
    preview.append(image, download);
    wrapper.append(preview);
    return wrapper;
  }

  if (blobType.startsWith("video/")) {
    const preview = document.createElement("div");
    preview.className = "chat-attachment-video-preview";
    const video = document.createElement("video");
    video.className = "chat-attachment-video";
    video.src = objectUrl;
    video.controls = true;
    video.preload = "metadata";
    video.playsInline = true;
    video.setAttribute("playsinline", "");
    video.setAttribute("aria-label", `Play ${attachment.fileName}`);
    // A codec this browser cannot decode would otherwise leave a dead black
    // box with no way to get at the file, so the player steps aside for the
    // ordinary downloadable card.
    video.addEventListener("error", () => {
      if (!preview.isConnected) return;
      preview.replaceWith(attachmentCard(attachment, objectUrl));
    });
    const caption = document.createElement("div");
    caption.className = "chat-attachment-video-meta";
    const label = document.createElement("span");
    label.className = "chat-attachment-name";
    label.textContent = `${attachment.fileName} - ${formatAttachmentSize(attachment.size)}`;
    label.title = attachment.fileName;
    const download = document.createElement("a");
    download.className = "chat-attachment-download";
    download.href = objectUrl;
    download.download = attachment.fileName;
    download.textContent = "Download";
    download.setAttribute("aria-label", `Download ${attachment.fileName}`);
    caption.append(label, download);
    preview.append(video, caption);
    wrapper.append(preview);
    return wrapper;
  }

  wrapper.append(attachmentCard(attachment, objectUrl));
  return wrapper;
}

function renderRecordBody(record) {
  if (!record?.body) return;
  if (record.deleted) {
    record.body.classList.add("chat-deleted-root");
    record.body.textContent = "Message deleted";
    return;
  }
  record.body.classList.remove("chat-deleted-root");
  renderMessageText(record.body, record);
  if (!record.editedAt) return;
  record.body.append(document.createTextNode(" "));
  const edited = document.createElement("span");
  edited.className = "chat-edited";
  edited.textContent = "(edited)";
  edited.title = `Edited ${new Date(record.editedAt).toLocaleString()}`;
  record.body.append(edited);
}

const MESSAGE_ACTION_ICONS = {
  Reply: "thread",
  React: "react",
  Issue: "issue",
  Copy: "copy",
  Edit: "edit",
  Delete: "trash",
};

async function copyMessageText(record, button) {
  const text = String(record.text || "");
  let copied = false;
  try {
    await navigator.clipboard.writeText(text);
    copied = true;
  } catch (_) {}
  button.textContent = "";
  button.append(chatIcon(copied ? "check" : "copy", 16));
  button.title = copied ? "Copied" : "Copy failed";
  setTimeout(() => {
    button.textContent = "";
    button.append(chatIcon("copy", 16));
    button.title = "Copy text";
  }, 1400);
}

function messageAction(label, text, handler, tone = "") {
  const button = document.createElement("button");
  button.type = "button";
  button.className = `chat-message-action${tone ? ` is-${tone}` : ""}`;
  button.setAttribute("aria-label", label);
  button.title = label;
  const icon = MESSAGE_ACTION_ICONS[text];
  if (icon) {
    button.classList.add("is-icon");
    button.append(chatIcon(icon, 16));
  } else {
    button.textContent = text;
  }
  button.addEventListener("click", (event) => {
    event.stopPropagation();
    handler(button);
  });
  return button;
}

function dismissTouchMessageActions() {
  document.querySelectorAll(".chat-msg.show-actions, .chat-thread-message.show-actions")
    .forEach((row) => {
      row.classList.remove("show-actions");
    });
}

function bindTouchMessageActions(row) {
  row.addEventListener("pointerup", (event) => {
    if (event.pointerType !== "touch") return;
    if (event.target.closest("a, button, input, textarea, select, label")) return;
    if (!window.getSelection()?.isCollapsed) return;
    const shouldOpen = !row.classList.contains("show-actions");
    dismissTouchMessageActions();
    row.classList.toggle("show-actions", shouldOpen);
  });
}

document.addEventListener("pointerdown", (event) => {
  if (event.pointerType !== "touch") return;
  if (event.target.closest(".chat-message-actions")) return;
  if (!event.target.closest(".chat-msg, .chat-thread-message")) {
    dismissTouchMessageActions();
  }
});

function threadCountLabel(count) {
  return `${count} ${count === 1 ? "reply" : "replies"}`;
}

// Faces on a summary row before it starts to crowd the message it hangs off.
const THREAD_SUMMARY_FACES = 3;

// The row under a message that opens its thread: the faces of everyone who
// replied (most recent first), the name of whoever replied last, and the count.
function renderThreadSummary(rootId) {
  const record = rows.get(rootId);
  const summary = threads.summary(rootId);
  const summaryEl = record?.threadSummaryEl;
  if (summaryEl) {
    summaryEl.hidden = summary.count === 0;
    summaryEl.textContent = "";
    if (summary.count) {
      const faces = document.createElement("span");
      faces.className = "chat-thread-summary-faces";
      for (const replier of summary.repliers.slice(0, THREAD_SUMMARY_FACES)) {
        faces.append(makeAvatar(
          replier.sender,
          replier.senderId === FORKBOT_SENDER_ID ? "bot" : "user",
        ));
      }
      const count = document.createElement("span");
      count.className = "chat-thread-summary-count";
      count.textContent = threadCountLabel(summary.count);
      summaryEl.append(faces);
      if (summary.latestSender) {
        const name = document.createElement("span");
        name.className = "chat-thread-summary-name";
        name.textContent = summary.latestSender;
        summaryEl.append(name);
      }
      summaryEl.append(count);
      const label = summary.latestSender
        ? `Open thread with ${threadCountLabel(summary.count)}, ` +
          `last reply from ${summary.latestSender}`
        : `Open thread with ${threadCountLabel(summary.count)}`;
      summaryEl.setAttribute("aria-label", label);
      summaryEl.title = label;
    }
  }
  if (activeThreadRootId === rootId && threadCountEl) {
    threadCountEl.textContent = threadCountLabel(summary.count);
  }
}

function threadMessageHeader(record) {
  const header = document.createElement("div");
  header.className = "chat-thread-message-head";
  const avatar = makeAvatar(record.sender, record.senderId === FORKBOT_SENDER_ID ? "bot" : "user");
  const meta = document.createElement("div");
  meta.className = "chat-thread-message-meta";
  const author = document.createElement("strong");
  author.textContent = record.sender;
  const time = document.createElement("span");
  time.textContent = fmtTime(record.ts);
  time.title = new Date(Number(record.ts) || Date.now()).toLocaleString();
  meta.append(author, time);
  const receipt = receiptState(record);
  if (knownRecipientCount(record.recipients) !== null &&
      !(receipt && isDirectMessageKey(record.channel))) {
    meta.append(makeRecipientLabel(
      record.recipients, "chat-thread-recipients", record.recipientNames));
  }
  if (receipt) meta.append(makeReceipt(record));
  header.append(avatar, meta);
  return header;
}

function renderThreadRoot(record) {
  const card = document.createElement("article");
  card.className = "chat-thread-root-message";
  card.append(threadMessageHeader(record));
  if (record.deleted) {
    const deleted = document.createElement("p");
    deleted.className = "chat-thread-deleted";
    deleted.textContent = "Message deleted";
    card.append(deleted);
    return card;
  }
  if (record.text) {
    const body = document.createElement("div");
    body.className = "chat-text";
    renderMessageText(body, record);
    if (record.editedAt) {
      body.append(document.createTextNode(" "));
      const edited = document.createElement("span");
      edited.className = "chat-edited";
      edited.textContent = "(edited)";
      body.append(edited);
    }
    card.append(body);
  }
  const attachment = renderAttachment(record);
  if (attachment) {
    threadAttachmentRecords.add(record);
    card.append(attachment);
  }
  return card;
}

function buildThreadReply(sourceRecord) {
  const record = {
    ...sourceRecord,
    self: sourceRecord.senderId === selfId,
  };
  const row = document.createElement("article");
  row.className = `chat-thread-message${record.self ? " is-self" : ""}`;
  row.dataset.id = record.id;
  row.append(threadMessageHeader(record));
  const main = document.createElement("div");
  main.className = "chat-thread-message-main";
  let body = null;
  if (record.text || record.deleted) {
    body = document.createElement("div");
    body.className = "chat-text";
    main.append(body);
  }
  const attachment = renderAttachment(record);
  if (attachment) {
    threadAttachmentRecords.add(record);
    main.append(attachment);
  }
  const reactionsEl = document.createElement("div");
  reactionsEl.className = "chat-reactions";
  main.append(reactionsEl);
  row.append(main);
  record.el = row;
  record.body = body;
  record.reactionsEl = reactionsEl;
  renderRecordBody(record);

  const actions = document.createElement("div");
  actions.className = "chat-message-actions";
  actions.append(messageAction(
    "Add reaction",
    "React",
    (button) => showEmojiPicker(button, record.id),
  ));
  if (record.self) {
    if (record.text) {
      actions.append(messageAction(
        "Edit message",
        "Edit",
        (button) => beginMessageEdit(record, button),
      ));
    }
    actions.append(messageAction(
      "Delete message",
      "Delete",
      (button) => requestMessageDelete(record, button),
      "danger",
    ));
  }
  row.append(actions);
  bindTouchMessageActions(row);
  threadRenderedRows.set(record.id, record);
  renderReactions(record.id);
  return row;
}

function resizeThreadComposer() {
  if (!threadInput) return;
  threadInput.style.height = "auto";
  threadInput.style.height = `${Math.min(threadInput.scrollHeight, 96)}px`;
}

function renderThread() {
  if (!activeThreadRootId || !threadRootEl || !threadRepliesEl) return;
  for (const record of threadAttachmentRecords) revokeAttachmentUrl(record);
  threadAttachmentRecords.clear();
  threadRenderedRows.clear();
  threadRootEl.textContent = "";
  threadRepliesEl.textContent = "";
  const root = rows.get(activeThreadRootId) || threads.root(activeThreadRootId);
  if (!root) {
    const unavailable = document.createElement("p");
    unavailable.className = "chat-thread-deleted";
    unavailable.textContent = "This message is no longer available.";
    threadRootEl.append(unavailable);
  } else {
    const rootView = portableMessageRecord(root);
    threadRootEl.append(renderThreadRoot(rootView));
  }
  for (const reply of threads.replies(activeThreadRootId)) {
    threadRepliesEl.append(buildThreadReply(reply));
  }
  renderThreadSummary(activeThreadRootId);
}

function closeThread({ restoreFocus = true } = {}) {
  if (activeThreadRootId && threadInput) {
    threadDrafts.set(activeThreadRootId, threadInput.value);
  }
  activeThreadRootId = "";
  if (peopleView) peopleView.hidden = false;
  if (threadView) threadView.hidden = true;
  panesEl?.classList.remove("is-thread-open");
  for (const record of threadAttachmentRecords) revokeAttachmentUrl(record);
  threadAttachmentRecords.clear();
  threadRenderedRows.clear();
  if (threadRootEl) threadRootEl.textContent = "";
  if (threadRepliesEl) threadRepliesEl.textContent = "";
  if (restoreFocus) threadReturnFocus?.focus();
  threadReturnFocus = null;
}

function openThread(record, trigger = null) {
  const rootId = String(record?.rootId || record?.id || "");
  if (!rootId || !threadView || !peopleView) return;
  if (activeThreadRootId && threadInput) {
    threadDrafts.set(activeThreadRootId, threadInput.value);
  }
  activeThreadRootId = rootId;
  threadReturnFocus = trigger || record.threadSummaryEl || record.el;
  peopleView.hidden = false;
  threadView.hidden = false;
  panesEl?.classList.add("is-thread-open");
  if (threadTitleEl) threadTitleEl.textContent = "Thread";
  if (threadInput) threadInput.value = threadDrafts.get(rootId) || "";
  renderAttachmentDraft(rootId);
  resizeThreadComposer();
  renderThread();
  threadInput?.focus();
}

async function sendThreadReply() {
  if (!activeThreadRootId || !threadInput || !canJoinChannel()) return;
  const rootId = activeThreadRootId;
  const originalSource = threadInput.value.trim().slice(0, MAX_TEXT);
  const moderation = await moderateChatText(originalSource);
  const source = moderation.text;
  const text = plainTextFromRichSource(source).slice(0, MAX_TEXT);
  if (!text && !attachmentDraft(rootId).length) return;
  if (text) {
    const plain = makePlain("thread-reply", {
      rootId,
      channel: channelWireLabel(activeChannel),
      text,
      richText: { v: 1, source },
    });
    const record = portableMessageRecord({
      ...plain,
      channel: activeChannel,
      self: true,
    });
    threads.addReply(record);
    seen.add(plain.id);
    threadInput.value = "";
    threadDrafts.set(rootId, "");
    resizeThreadComposer();
    renderThread();
    threadRepliesEl?.lastElementChild?.scrollIntoView({ block: "nearest" });
    renderThreadSummary(rootId);
    runWhenConnected(() => send(plain));
    if (moderation.changed) appendSystem("Some words were filtered.");
  }
  void sendAttachmentDraft({ rootId });
}

function cancelMessageEdit(record, { restoreFocus = true } = {}) {
  const editor = record?.el?.querySelector(".chat-inline-editor");
  if (!editor) return;
  editor.remove();
  record.el.classList.remove("is-editing");
  if (record.body) record.body.hidden = false;
  if (restoreFocus) record.editTrigger?.focus();
}

async function saveMessageEdit(record, source) {
  if (!record?.self || record.senderId !== selfId) return;
  const originalSource = String(source || "").trim().slice(0, MAX_TEXT);
  const moderation = await moderateChatText(originalSource);
  const richSource = moderation.text;
  const text = plainTextFromRichSource(richSource).slice(0, MAX_TEXT);
  if (!text) return;
  const editedAt = Date.now();
  const richText = { v: 1, source: richSource };
  const plain = makePlain("edit", {
    conversation: channelWireLabel(record.channel),
    target: record.id,
    text,
    richText,
    editedAt,
  });
  record.text = text;
  record.richText = richText;
  record.editedAt = editedAt;
  threads.updateTarget(record.id, { text, richText, editedAt });
  cancelMessageEdit(record, { restoreFocus: false });
  if (record.rootId) {
    renderThread();
    renderThreadSummary(record.rootId);
  } else {
    renderRecordBody(record);
    queueRoomPreview(record.channel);
    if (activeThreadRootId === record.id) renderThread();
  }
  seen.add(plain.id);
  runWhenConnected(() => send(plain));
  if (moderation.changed) appendSystem("Some words were filtered.");
}

function beginMessageEdit(record, trigger) {
  if (!record?.self || record.senderId !== selfId || !record.body) return;
  cancelMessageEdit(record, { restoreFocus: false });
  record.editTrigger = trigger;
  record.el.classList.add("is-editing");
  record.body.hidden = true;

  const editor = document.createElement("div");
  editor.className = "chat-inline-editor";
  const textarea = document.createElement("textarea");
  textarea.className = "chat-edit-input";
  textarea.rows = 3;
  textarea.maxLength = MAX_TEXT;
  textarea.setAttribute("aria-label", "Edit message");
  textarea.value = record.richText?.source || record.text || "";
  const controls = document.createElement("div");
  controls.className = "chat-edit-actions";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.textContent = "Cancel";
  cancel.setAttribute("aria-label", "Cancel edit");
  const save = document.createElement("button");
  save.type = "button";
  save.className = "is-primary";
  save.textContent = "Save";
  save.setAttribute("aria-label", "Save edit");
  cancel.addEventListener("click", () => cancelMessageEdit(record));
  save.addEventListener("click", () => saveMessageEdit(record, textarea.value));
  textarea.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      cancelMessageEdit(record);
    } else if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      saveMessageEdit(record, textarea.value);
    }
  });
  controls.append(cancel, save);
  editor.append(textarea, controls);
  record.reactionsEl.before(editor);
  textarea.focus();
  textarea.setSelectionRange(textarea.value.length, textarea.value.length);
}

function requestMessageDelete(record, trigger) {
  if (!record?.self || record.senderId !== selfId || !deleteDialog) return;
  pendingDeleteRecord = record;
  pendingDeleteTrigger = trigger;
  if (typeof deleteDialog.showModal === "function") deleteDialog.showModal();
  else deleteDialog.setAttribute("open", "");
  deleteCancelBtn?.focus();
}

function confirmMessageDelete() {
  const record = pendingDeleteRecord;
  if (!record?.self || (record.senderId !== selfId && !record.viewOnce)) return;
  withdrawViewOnce(record);
  const plain = makePlain("delete", {
    conversation: channelWireLabel(record.channel),
    target: record.id,
  });
  pendingDeleteRecord = null;
  pendingDeleteTrigger = null;
  seen.add(plain.id);
  removeMessage(record.id);
  runWhenConnected(() => send(plain).then((sent) => {
    if (sent && isDirectMessageKey(record.channel)) {
      roomTransport?.supersede?.([record.id]);
    }
  }));
}

// ---- convert a message into a repository issue ------------------------------
// A bug report typed into chat should not have to be retyped on the issues
// page. Every message carries an "Issue" control (the dashboard and in-world
// chats carry the same one) that opens an inline form: an editable title seeded
// from the first line, plus a repository picker. Filing signs an "open" event
// into the maintainer's inbox — see chat-issue-filing.js — so it works from any
// account, not just the repository's owner.

function closeIssueForm(record, { restoreFocus = true } = {}) {
  record.issueFormEl?.remove();
  record.issueFormEl = null;
  if (restoreFocus) record.issueTrigger?.focus();
}

function beginIssueFromMessage(record, trigger) {
  if (!record?.text || record.deleted || !record.reactionsEl) return;
  if (record.issueFormEl) {
    record.issueFormEl.querySelector("input")?.focus();
    return;
  }
  record.issueTrigger = trigger;

  const form = document.createElement("div");
  form.className = "chat-inline-editor chat-issue-form";
  const title = document.createElement("input");
  title.type = "text";
  title.className = "chat-edit-input chat-issue-title";
  title.maxLength = 200;
  title.value = issueTitleFromMessage(record.text);
  title.setAttribute("aria-label", "Issue title");
  const repository = document.createElement("select");
  repository.className = "chat-issue-repo";
  repository.setAttribute("aria-label", "Issue repository");
  const status = document.createElement("span");
  status.className = "chat-issue-status";
  status.setAttribute("role", "status");
  const controls = document.createElement("div");
  controls.className = "chat-edit-actions";
  const cancel = document.createElement("button");
  cancel.type = "button";
  cancel.textContent = "Cancel";
  cancel.setAttribute("aria-label", "Cancel issue");
  const create = document.createElement("button");
  create.type = "button";
  create.className = "is-primary";
  create.textContent = "Create issue";
  create.setAttribute("aria-label", "File this issue");
  create.disabled = true;

  let entries = [];
  const setStatus = (message, bad = false) => {
    status.textContent = message;
    status.classList.toggle("is-bad", Boolean(bad));
  };
  const submit = async () => {
    const target = entries.find((entry) => entry.value === repository.value);
    if (!target) {
      setStatus("Choose a repository for this issue.", true);
      repository.focus();
      return;
    }
    const issueTitle = String(title.value || "").trim().slice(0, 200);
    if (!issueTitle) {
      setStatus("An issue needs a title.", true);
      title.focus();
      return;
    }
    create.disabled = true;
    setStatus("Signing issue…");
    try {
      const filed = await fileWebIssue(
        target,
        issueTitle,
        issueBodyFromMessage({
          text: record.text,
          who: record.sender,
          tsMs: record.ts,
          channelLabel: channelDisplayLabel(record.channel),
        }),
        String(readSession()?.nodeName || ""),
      );
      closeIssueForm(record);
      appendSystem(
        issueFiledSummary(`${target.owner}/${target.name}`, issueTitle, filed),
      );
    } catch (error) {
      create.disabled = false;
      setStatus(String(error?.message || "The issue could not be filed."), true);
    }
  };

  cancel.addEventListener("click", () => closeIssueForm(record));
  create.addEventListener("click", () => void submit());
  title.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      event.preventDefault();
      closeIssueForm(record);
    } else if (event.key === "Enter") {
      event.preventDefault();
      if (!create.disabled) void submit();
    }
  });
  controls.append(cancel, create);
  form.append(title, repository, controls, status);
  record.issueFormEl = form;
  record.reactionsEl.before(form);

  setStatus("Loading repositories…");
  loadIssueRepositories()
    .then((loaded) => {
      if (record.issueFormEl !== form) return;
      entries = loaded;
      for (const entry of entries) {
        const option = document.createElement("option");
        option.value = entry.value;
        option.textContent = entry.label;
        repository.append(option);
      }
      let preferred = "";
      try { preferred = localStorage.getItem(ISSUE_REPOSITORY_KEY) || ""; } catch (_) {}
      const defaultEntry = entries.find((entry) => entry.value === preferred) ||
        entries.find((entry) => entry.value === "forkmesh/forkmesh") || entries[0];
      if (defaultEntry) repository.value = defaultEntry.value;
      repository.addEventListener("change", () => {
        try { localStorage.setItem(ISSUE_REPOSITORY_KEY, repository.value); } catch (_) {}
      });
      create.disabled = !entries.length;
      setStatus(
        entries.length ? "" : "No repositories are available to file into.",
        !entries.length,
      );
    })
    .catch(() => {
      if (record.issueFormEl !== form) return;
      setStatus("The repository list is unavailable right now.", true);
    });
  title.focus();
  title.select();
}

function buildDateDivider(ts) {
  const divider = document.createElement("div");
  const timestamp = Number(ts);
  divider.className = "chat-date-divider";
  divider.dataset.timestamp = String(
    Number.isFinite(timestamp) ? timestamp : Date.now()
  );
  divider.setAttribute("role", "separator");
  const label = document.createElement("span");
  label.textContent = dateDividerLabel(divider.dataset.timestamp);
  divider.setAttribute("aria-label", label.textContent);
  divider.append(label);
  return divider;
}

function refreshDateDividerLabels() {
  for (const divider of logEl.querySelectorAll(".chat-date-divider")) {
    const label = divider.querySelector("span");
    if (!label) continue;
    label.textContent = dateDividerLabel(divider.dataset.timestamp);
    divider.setAttribute("aria-label", label.textContent);
  }
}

// Render one message record into the log. `prev` is the record already above
// it; consecutive same-sender messages within GROUP_WINDOW_MS collapse under a
// single avatar + name/time header, Discord-style.
function buildRow(record, prev) {
  const grouped = Boolean(
    prev && !prev.system && prev.senderId === record.senderId &&
    sameLocalDate(prev.ts, record.ts) &&
    Number(record.ts) - Number(prev.ts) < GROUP_WINDOW_MS);
  const row = document.createElement("div");
  const isBot = record.senderId === FORKBOT_SENDER_ID;
  row.className = "chat-msg" +
    (grouped ? " is-continuation" : " is-group-start") +
    (record.self ? " is-self" : "") +
    (isBot ? " is-bot" : "");
  row.dataset.id = record.id;
  // Clicking the avatar or the author name opens the sender's profile at
  // /@username on THIS relay (relative URL, so a self-hosted relay links to
  // its own profile pages). ForkBot isn't an account, so it stays plain.
  const avatar = makeAvatar(record.sender, isBot ? "bot" : "user");
  if (isBot) {
    row.append(avatar);
  } else {
    const avatarLink = document.createElement("a");
    avatarLink.className = "chat-avatar-link";
    avatarLink.href = mentionProfilePath(record.sender);
    avatarLink.append(avatar);
    row.append(avatarLink);
  }
  const main = document.createElement("div");
  main.className = "chat-msg-main";
  if (!grouped) {
    const head = document.createElement("div");
    head.className = "chat-msg-head";
    const author = document.createElement(isBot ? "span" : "a");
    author.className = "chat-author";
    author.textContent = record.sender;
    if (!isBot) {
      author.dataset.userStatusName = record.sender;
      author.href = mentionProfilePath(record.sender);
      author.addEventListener("mouseenter", () => showMentionCard(author, record.sender));
      author.addEventListener("focus", () => showMentionCard(author, record.sender));
      author.addEventListener("mouseleave", hideMentionCardSoon);
      author.addEventListener("blur", hideMentionCardSoon);
    }
    const time = document.createElement("span");
    time.className = "chat-time";
    time.textContent = fmtTime(record.ts);
    time.title = new Date(Number(record.ts) || Date.now()).toLocaleString();
    head.append(author);
    const authorBadge = isBot ? null : userStatusBadge(record.sender);
    if (authorBadge) head.append(authorBadge);
    head.append(time);
    main.append(head);
  }
  let body = null;
  if (record.text || record.deleted) {
    body = document.createElement("div");
    body.className = "chat-text";
    main.append(body);
  }
  const attachment = renderAttachment(record);
  if (attachment) main.append(attachment);
  const viewOnceBubble = renderViewOnceBubble(record, { onOpen: openViewOnceRecord });
  if (viewOnceBubble) main.append(viewOnceBubble);
  const threadSummary = document.createElement("button");
  threadSummary.type = "button";
  threadSummary.className = "chat-thread-summary";
  threadSummary.hidden = true;
  threadSummary.addEventListener("click", () => openThread(record, threadSummary));
  main.append(threadSummary);
  const reactionsEl = document.createElement("div");
  reactionsEl.className = "chat-reactions";
  main.append(reactionsEl);
  row.append(main);
  const receipt = receiptState(record);
  if (receipt && isDirectMessageKey(record.channel)) {
    record.recipientsEl = null;
  } else {
    record.recipientsEl = makeRecipientLabel(
      record.recipients, "chat-recipients", record.recipientNames);
    row.append(record.recipientsEl);
  }
  if (receipt) row.append(makeReceipt(record));
  record.el = row;
  record.body = body;
  record.reactionsEl = reactionsEl;
  record.threadSummaryEl = threadSummary;
  renderRecordBody(record);
  renderThreadSummary(record.id);
  const actions = document.createElement("div");
  actions.className = "chat-message-actions";
  actions.append(messageAction(
    "Reply in thread",
    "Reply",
    (button) => openThread(record, button),
  ));
  if (!record.deleted) {
    actions.append(messageAction(
      "Add reaction",
      "React",
      (button) => showEmojiPicker(button, record.id),
    ));
  }
  // Filing someone else's report is the common case, so this is not
  // author-only; the issue is signed by, and attributed to, whoever files it.
  if (!record.deleted && record.text) {
    actions.append(messageAction(
      "Copy text",
      "Copy",
      (button) => void copyMessageText(record, button),
    ));
    actions.append(messageAction(
      "Create an issue from this message",
      "Issue",
      (button) => beginIssueFromMessage(record, button),
    ));
  }
  if (!record.deleted && record.self &&
      (record.senderId === selfId || record.viewOnce)) {
    if (record.text) {
      actions.append(messageAction(
        "Edit message",
        "Edit",
        (button) => beginMessageEdit(record, button),
      ));
    }
    actions.append(messageAction(
      "Delete message",
      "Delete",
      (button) => requestMessageDelete(record, button),
      "danger",
    ));
  }
  row.append(actions);
  bindTouchMessageActions(row);
  return row;
}

function renderActiveChannel() {
  for (const record of rows.values()) revokeAttachmentUrl(record);
  logEl.textContent = "";
  const list = channelMessages.get(activeChannel) || [];
  if (!list.length) {
    const empty = document.createElement("div");
    empty.className = "chat-empty";
    const syncState = channelSyncState.get(activeChannel);
    if (canJoinChannel() && syncState === "loading") {
      empty.classList.add("chat-loading");
      empty.setAttribute("role", "status");
      empty.setAttribute("aria-live", "polite");
      empty.textContent = "Loading messages…";
      logEl.append(empty);
      return;
    }
    empty.textContent = canJoinChannel()
      ? `No messages in ${channelDisplayLabel(activeChannel)} yet. Say hi!`
      : (
          "Log in as a user to join this channel. " +
          "Guests can chat only in World."
        );
    logEl.append(empty);
    for (const notice of trustNotices.get(activeChannel) || []) {
      logEl.append(buildTrustNotice(notice));
    }
    return;
  }
  // Build the whole timeline detached and attach it once. A full channel is
  // hundreds of rows, and appending them one at a time makes the browser lay
  // the log out (and the user watch it grow) once per message.
  const fragment = document.createDocumentFragment();
  let prev = null;
  let previousDateKey = "";
  const notices = [...(trustNotices.get(activeChannel) || [])];
  for (const record of list) {
    while (notices.length && notices[0].ts <= Number(record.ts)) {
      fragment.append(buildTrustNotice(notices.shift()));
      prev = null;
    }
    const dateKey = localDateKey(record.ts);
    if (dateKey !== previousDateKey) {
      fragment.append(buildDateDivider(record.ts));
      prev = null;
      previousDateKey = dateKey;
    }
    fragment.append(buildRow(record, prev));
    renderReactions(record.id);
    prev = record;
  }
  for (const notice of notices) fragment.append(buildTrustNotice(notice));
  logEl.append(fragment);
  scrollLogToBottom();
}

// Non-null only while a replayed backlog is being taken in (handleHistory).
// Every record in that batch belongs to the same paint, so insertMessage keeps
// the model up to date and leaves the DOM alone; handleHistory renders the
// channels collected here once, at the end.
let historyBatchChannels = null;

// Insert a message record in ts order (newest at the bottom). Appends in the
// common case; an out-of-order arrival (a live message racing another) rebuilds
// the visible channel so ordering and grouping stay correct.
function insertMessage(record) {
  const channel = record.channel;
  if (!historyBatchChannels) channelSyncState.set(channel, "ready");
  const list = channelMessages.get(channel) || [];
  channelMessages.set(channel, list);
  let index = list.length;
  while (index > 0 && Number(list[index - 1].ts) > Number(record.ts)) index -= 1;
  list.splice(index, 0, record);
  rows.set(record.id, record);
  threads.registerRoot(portableMessageRecord(record));
  queueRoomPreview(channel);
  if (historyBatchChannels) {
    historyBatchChannels.add(channel);
    if (channel !== activeChannel && !isOwnChatMessage(record)) {
      bumpUnread(channel, record.ts);
    } else if (channel === activeChannel) {
      queueReadMark(channel);
    }
    return;
  }
  if (channel === activeChannel) {
    queueReadMark(channel);
    const pinned = logNearBottom();
    if (index === list.length - 1) {
      clearEmpty();
      const previous = list.length > 1 ? list[index - 1] : null;
      const groupedPrevious = previous && sameLocalDate(previous.ts, record.ts)
        ? previous
        : null;
      if (!groupedPrevious) logEl.append(buildDateDivider(record.ts));
      logEl.append(buildRow(record, groupedPrevious));
      renderReactions(record.id);
      if (pinned || record.self) scrollLogToBottom();
    } else {
      renderActiveChannel();
    }
  } else if (!isOwnChatMessage(record)) {
    bumpUnread(channel, record.ts);
  }
}

function appendMessage(
  kind,
  who,
  text,
  id,
  senderId,
  ts,
  channel,
  attachment = null,
  richText = null,
  appMessage = null,
  recipients = null,
  recipientNames = [],
  viewOnce = null,
  relayed = false,
) {
  const channelKey = normalizeChannelKey(channel) || activeChannel;
  const record = {
    id: id || String(Math.random()).slice(2) + Date.now(),
    channel: ensureChannel(channelKey) || activeChannel,
    ts: Number(ts) || Date.now(),
    senderId: senderId || "",
    sender: who,
    text,
    richText,
    attachment,
    self: kind === "self",
    appMessage,
    recipients: knownRecipientCount(recipients),
    recipientNames: knownRecipientNames(recipientNames),
    relayed: Boolean(relayed),
  };
  if (viewOnce) {
    record.viewOnce = viewOnce;
    record.viewOnceState = kind === "self" ? "sent" : "ready";
    if (kind === "self") forgetViewOnceKey(record);
    queueViewOnceStatus(record);
  }
  insertMessage(record);
  rememberContext(record.channel, who, text);
}

// Drop a message row from the screen (a deletion leaves no tombstone, matching
// the desktop client). Keep its id in `seen` so a late duplicate can't reappear.
function removeMessage(id) {
  const rec = rows.get(id);
  if (!rec) {
    const reply = threads.target(id);
    if (!reply?.rootId) return;
    threads.deleteTarget(id);
    reactions.delete(id);
    if (activeThreadRootId === reply.rootId) renderThread();
    renderThreadSummary(reply.rootId);
    return;
  }
  revokeAttachmentUrl(rec);
  if (threads.summary(id).count > 0) {
    rec.text = "";
    rec.richText = null;
    rec.attachment = null;
    rec.deleted = true;
    reactions.delete(id);
    threads.deleteTarget(id);
    repaintChannel(rec.channel);
    queueRoomPreview(rec.channel);
    if (activeThreadRootId === id) renderThread();
    return;
  }
  rows.delete(id);
  reactions.delete(id);
  threads.deleteTarget(id);
  const list = channelMessages.get(rec.channel) || [];
  const index = list.indexOf(rec);
  if (index >= 0) list.splice(index, 1);
  if (activeThreadRootId === id) closeThread({ restoreFocus: false });
  // Re-render so grouping headers stay correct around the gap.
  repaintChannel(rec.channel);
  queueRoomPreview(rec.channel);
}

// Re-render a channel, or — inside a replayed backlog — leave it to the single
// paint that closes the batch. A delete replayed alongside the message it
// removes should not cost the log an extra rebuild.
function repaintChannel(channel) {
  if (channel !== activeChannel) return;
  if (historyBatchChannels) historyBatchChannels.add(channel);
  else renderActiveChannel();
}

function appendSystem(text) {
  clearEmpty();
  const row = document.createElement("div");
  row.className = "chat-system";
  row.textContent = text;
  logEl.append(row);
  if (logNearBottom()) scrollLogToBottom();
}

function scheduleDateDividerRefresh() {
  if (dateRefreshTimer) clearTimeout(dateRefreshTimer);
  const now = new Date();
  const next = new Date(now.getFullYear(), now.getMonth(), now.getDate() + 1);
  dateRefreshTimer = setTimeout(() => {
    refreshDateDividerLabels();
    scheduleDateDividerRefresh();
  }, Math.max(1, next.getTime() - now.getTime() + 50));
}

// ---- protocol ---------------------------------------------------------------

function makePlain(type, extra) {
  return Object.assign(
    {
      type,
      id:
        (crypto.randomUUID && crypto.randomUUID()) ||
        String(Math.random()).slice(2) + Date.now(),
      senderId: selfId,
      sender: displayName(),
      // Only people without a session speak as guests. A signed-in browser
      // keeps its account kind in the public room too, so logging in no
      // longer spawns a second "guest" person beside the account.
      accountKind:
        roomScopeForChannel() === "public-world-general" && !userSession()
          ? "guest"
          : "user",
      ts: Date.now(),
    },
    ownUserStatusFrameField(type),
    extra || {}
  );
}

// Away + custom status (user-status.js) ride every hello/presence beat, so
// peers repaint within a beat instead of waiting on the account directory.
function ownUserStatusFrameField(type) {
  if ((type !== "hello" && type !== "presence") || !userSession()) return {};
  const own = window.ForkMeshUserStatus?.ownFrameStatus();
  return own ? { userStatus: own } : {};
}

function userStatusBadge(name, className = "") {
  return window.ForkMeshUserStatus?.badge(
    window.ForkMeshUserStatus.forName(name), { className }) || null;
}

function userIsAway(name) {
  return Boolean(window.ForkMeshUserStatus?.forName(name).away);
}

// Author lines are built once, so a status change repaints their badges here.
function repaintAuthorStatuses() {
  for (const author of document.querySelectorAll("[data-user-status-name]")) {
    if (author.nextElementSibling?.classList.contains("fm-user-status-badge")) {
      author.nextElementSibling.remove();
    }
    const badge = userStatusBadge(author.dataset.userStatusName);
    if (badge) author.after(badge);
  }
}

function watchUserStatuses() {
  const userStatus = window.ForkMeshUserStatus;
  if (!userStatus) return;
  userStatus.subscribe(() => {
    schedulePeopleRender();
    repaintAuthorStatuses();
    renderDirectMessages();
  });
  userStatus.loadMine();
  window.addEventListener("forkmesh:user-status-self", () => {
    if (roomTransport?.connected && canJoinChannel()) {
      send(makePlain("presence"));
      noteSelfRoster();
    }
  });
}

function allowedChatAccountKind(value, scope = roomScopeForChannel()) {
  return value === "user" ||
    (scope === "public-world-general" && value === "guest");
}

function normalizedPublicWorldFrame(
  plain,
  scope = roomScopeForChannel(),
) {
  if (scope !== "public-world-general" ||
      !plain || typeof plain !== "object") {
    return plain;
  }
  return {
    ...plain,
    accountKind: plain.accountKind === "user" ? "user" : "guest",
    sender: publicWorldName(plain.sender),
  };
}

function frameMatchesScope(plain, scope) {
  if (scope !== "public-world-general") {
    if (!isPrivateChannelKey(scope) && !isDirectMessageKey(scope)) return false;
    if (plain.type === "history" || plain.type === "hello" ||
        plain.type === "presence" || plain.type === "bye") return true;
    return plain.channel === channelWireLabel(scope) ||
      plain.conversation === channelWireLabel(scope);
  }
  if (plain.type === "history") return true;
  if (plain.type === "hello" || plain.type === "presence" ||
      plain.type === "bye") {
    return allowedChatAccountKind(plain.accountKind, scope);
  }
  return plain.channel === "#general" ||
    plain.conversation === "#general";
}

function once(id) {
  if (!id || seen.has(id)) return false;
  seen.add(id);
  return true;
}

function renderChatEntry(
  entry,
  kind,
  scope = roomScopeForChannel(),
  recipients = null,
  recipientNames = [],
) {
  if (!entry || !allowedChatAccountKind(entry.accountKind, scope)) return;
  entry = normalizedPublicWorldFrame(entry, scope);
  const channelKey = scope === "public-world-general" ? "#general" : scope;
  if (scope === "public-world-general" && entry.channel !== "#general") return;
  if ((isPrivateChannelKey(scope) || isDirectMessageKey(scope)) &&
      entry.channel !== channelWireLabel(scope)) return;
  if (entry.private && scope === "public-world-general") return;
  // Historical authors still belong in the people pane (as offline entries —
  // noteRoster derives presence from the entry's own timestamp).
  noteRoster(entry);
  if (!once(entry.id)) return;
  if (chatRetention.isExpired(entry.ts, channelKey)) return;
  const who = (entry.sender || "peer").slice(0, MAX_NAME);
  const viewOnce = isDirectMessageKey(scope) ? viewOnceFromEntry(entry) : null;
  const text = viewOnce ? "" : entry.text || "";
  const attachment = viewOnce ? null : attachmentFromEntry(entry);
  if (text || attachment || viewOnce) {
    const renderedKind = entry.senderId === selfId || isOwnViewOnceEntry(entry)
      ? "self"
      : kind;
    appendMessage(
      renderedKind,
      who,
      text,
      entry.id,
      entry.senderId,
      entry.ts,
      channelKey,
      attachment,
      entry.richText || null,
      appMessageFromEntry(entry, channelKey),
      recipients,
      recipientNames,
      viewOnce,
      true,
    );
  }
}

function appMessageFromEntry(entry, channelKey) {
  const senderId = String(entry?.senderId || "");
  const notice = senderId.startsWith("system:");
  if (!senderId.startsWith("app:") && !notice) return null;
  if (!Array.isArray(entry.blocks) || !entry.blocks.length) return null;
  const record = privateChannelForKey(channelKey);
  return {
    blocks: entry.blocks,
    notice,
    appMessageId: notice ? "" : String(entry.appMessageId || ""),
    channelId: record ? record.id : "",
  };
}

function renderThreadEntry(
  entry,
  scope = roomScopeForChannel(),
  recipients = null,
  recipientNames = [],
) {
  if (!entry || !allowedChatAccountKind(entry.accountKind, scope)) return;
  entry = normalizedPublicWorldFrame(entry, scope);
  const channelKey = scope === "public-world-general" ? "#general" : scope;
  if (scope === "public-world-general" && entry.channel !== "#general") return;
  if (isPrivateChannelKey(scope) && entry.channel !== channelWireLabel(scope)) return;
  if ((entry.private && scope === "public-world-general") ||
      !entry.rootId || !once(entry.id)) return;
  noteRoster(entry);
  const attachment = attachmentFromEntry(entry);
  const text = entry.text || "";
  if (!text && !attachment) return;
  const record = portableMessageRecord({
    ...entry,
    channel: channelKey,
    sender: (entry.sender || "peer").slice(0, MAX_NAME),
    text,
    attachment,
    self: entry.senderId === selfId,
    recipients,
    recipientNames,
    relayed: true,
  });
  threads.addReply(record);
  renderThreadSummary(record.rootId);
  if (activeThreadRootId === record.rootId) renderThread();
}

// An admin-delete frame is signed by the admin's identity key over a canonical
// string (must match the desktop's adminDeleteCanonical byte-for-byte). We only
// remove the message after verifying that signature AND confirming the signer's
// account is flagged admin on the relay — otherwise any room member could forge
// one (chat frames are otherwise unsigned).
async function verifyAdminDelete(plain) {
  const target = plain.target;
  const adminId = plain.senderId; // desktop node id == identity pubkey (b64url)
  const sig = plain.sig;
  if (!target || !sig || !adminId) return false;
  const canonical =
    "forkmesh-admin-delete-v1\n" +
    (plain.conversation || "") +
    "\n" +
    target +
    "\n" +
    adminId +
    "\n" +
    String(plain.ts);
  if (!(await ed25519Verify(adminId, sig, canonical))) return false;
  try {
    const name = (plain.sender || "").trim().toLowerCase();
    if (!name) return false;
    const res = await fetch("/api/accounts/" + encodeURIComponent(name));
    const rec = await res.json();
    return rec && rec.isAdmin === true && rec.pubkey === adminId;
  } catch (error) {
    return false;
  }
}

// One paint for a whole replayed backlog. The relay hands a joining socket the
// room's retained history as individual frames; taking them one at a time
// grew the log a row at a time and re-rendered the entire channel on every
// frame that arrived out of sequence, which is what made an opening chat
// flicker. chat-room-transport buffers the replay and delivers it here intact.
function handleHistory(plains, room) {
  const scope = room?.scope || roomScopeForChannel();
  const previousBatch = historyBatchChannels;
  const batch = previousBatch || new Set();
  historyBatchChannels = batch;
  try {
    // D1 ties frames stored in the same millisecond by opaque cipher hash, so
    // an edit, delete or reaction can replay ahead of the message it targets.
    // Register every chat record first, then apply the mutations in order.
    for (const plain of plains) {
      if (plain?.type === "chat" || plain?.type === "thread-reply") {
        handlePlain(plain, scope);
      }
    }
    for (const plain of plains) {
      if (plain && plain.type !== "chat" && plain.type !== "thread-reply") {
        handlePlain(plain, scope);
      }
    }
  } finally {
    historyBatchChannels = previousBatch;
  }
  if (previousBatch) return;
  // Ready even when the backlog was empty: the room has told us all it has, so
  // the channel stops loading and says its piece ("No messages yet. Say hi!").
  const connectedChannel = scope === "public-world-general" ? "#general" : scope;
  const wasLoading = channelSyncState.get(connectedChannel) !== "ready";
  for (const channel of batch) channelSyncState.set(channel, "ready");
  channelSyncState.set(connectedChannel, "ready");
  // A reconnect replays a backlog this page already has. Rendering only when
  // the batch actually brought something — or when the channel is still
  // showing its loading state — keeps that replay invisible.
  if (batch.has(activeChannel) ||
      (wasLoading && connectedChannel === activeChannel)) {
    renderActiveChannel();
  }
}

function targetMatchesScope(targetId, scope) {
  const target = rows.get(String(targetId || "")) ||
    threads.target(String(targetId || ""));
  if (!target) return false;
  const channel = scope === "public-world-general" ? "#general" : scope;
  return target.channel === channel;
}

function handlePlain(plain, scope = roomScopeForChannel()) {
  plain = botLine(plain);
  const type = plain.type;
  if (!frameMatchesScope(plain, scope)) return;
  if (type !== "history" && !allowedChatAccountKind(plain.accountKind, scope)) {
    return;
  }
  plain = normalizedPublicWorldFrame(plain, scope);
  // Roster + channel discovery run for EVERY decrypted frame — desktop nodes
  // announce themselves (hello/presence) without accountKind, and the people
  // pane should still show them with their online status.
  noteRoster(plain);
  // A live retained #general line is one the reader is looking at right now, so
  // it advances the header badge's baseline exactly like a line they sent. This
  // is the socket half of what used to be a GET /api/chat/activity every
  // minute. Replayed "history" is deliberately excluded: the baseline read when
  // the page opened already counted it.
  if (
    scope === "public-world-general" &&
    type !== "history" &&
    DURABLE_TYPES.has(type) &&
    !plain.file
  ) {
    noteSeenChatActivity();
  }
  if (scope === "public-world-general" &&
      (type === "hello" || type === "channel")) ensureChannel("#general");
  const sender = (plain.sender || "peer").slice(0, MAX_NAME);
  if (type === "channel-topic") {
    // The frame is only a prompt to re-read server-authoritative metadata.
    // Room messages are not signed, so never trust the topic text itself.
    refreshPrivateChannels({ selectSaved: false, connect: false });
  } else if (type === "chat") {
    renderChatEntry(
      plain, "peer", scope, plain.relayRecipients ?? null,
      plain.relayRecipientNames ?? []);
  } else if (type === "thread-reply") {
    renderThreadEntry(
      plain, scope, plain.relayRecipients ?? null,
      plain.relayRecipientNames ?? []);
  } else if (type === "history") {
    const entries = Array.isArray(plain.entries) ? plain.entries : [];
    for (const entry of entries) {
      if (entry?.type === "thread-reply") {
        renderThreadEntry(entry, scope);
      } else if (entry && (!entry.type || entry.type === "chat")) {
        renderChatEntry(entry, "peer", scope);
      }
    }
    for (const entry of entries) {
      if (entry?.type && entry.type !== "chat" &&
          entry.type !== "thread-reply" && entry.type !== "history") {
        handlePlain(entry, scope);
      }
    }
  } else if (type === "reaction") {
    if (targetMatchesScope(plain.target, scope) && once(plain.id)) {
      applyReaction(plain);
    }
  } else if (type === "edit") {
    // The author edited their own message; only honour it from that author.
    if (!targetMatchesScope(plain.target, scope)) return;
    const rec = rows.get(plain.target);
    const reply = rec ? null : threads.target(plain.target);
    const target = rec || reply;
    if (target && target.senderId === plain.senderId) {
      const patch = {
        text: plain.text || "",
        richText: plain.richText || null,
        editedAt: Number(plain.editedAt) || Number(plain.ts) || Date.now(),
      };
      if (reply?.rootId) {
        threads.updateTarget(plain.target, patch);
        if (activeThreadRootId === reply.rootId) renderThread();
        renderThreadSummary(reply.rootId);
        return;
      }
      rec.text = plain.text || "";
      rec.richText = plain.richText || null;
      rec.editedAt = Number(plain.editedAt) || Number(plain.ts) || Date.now();
      threads.updateTarget(rec.id, patch);
      renderRecordBody(rec);
      queueRoomPreview(rec.channel);
      if (activeThreadRootId === rec.id) renderThread();
    }
  } else if (type === "delete") {
    // A plain delete is only valid from the message's own author.
    if (!targetMatchesScope(plain.target, scope)) return;
    const rec = rows.get(plain.target) || threads.target(plain.target);
    if (rec && rec.senderId === plain.senderId) removeMessage(plain.target);
  } else if (type === "admin-delete") {
    // Moderation: remove any message once the admin signature checks out.
    if (!targetMatchesScope(plain.target, scope)) return;
    verifyAdminDelete(plain).then((ok) => {
      if (!ok) return;
      seen.add(plain.target); // also suppress a copy that arrives after the delete
      removeMessage(plain.target);
    });
  } else if (type === "hello" && !plain.to) {
    // Presence is shown in the World itself; do not add join noise to the
    // message timeline or push the composer upward.
  } else if (type === "bye") {
    appendSystem(sender + " left");
  } else if (CALL_FRAME_TYPES.has(type)) {
    if (isDirectMessageKey(scope)) void chatCalls.handleFrame(plain, scope);
  }
  if (
    isDirectMessageKey(scope)
    && (type === "chat" || type === "history")
  ) {
    markDirectMessageRead(scope);
  }
}

async function onFrame(event, key = null, scope = roomScopeForChannel()) {
  if (typeof event.data !== "string") return;
  let envelope;
  try {
    envelope = JSON.parse(event.data);
  } catch (error) {
    return;
  }
  // The relay's own signed announcement that an account was just created.
  // Peers can only send cipher envelopes, so this cannot be forged from a
  // browser: fireworks, confetti, and a nudge to greet them in #general.
  if (envelope?.kind === "forkmesh-new-member") {
    if (celebrateNewMember(envelope.name)) {
      appendSystem(
        NEW_MEMBER_GREETING(String(envelope.name).trim().toLowerCase()),
      );
    }
    return;
  }
  const plain = await decryptObject(envelope, key);
  if (!plain) return;
  handlePlain(await moderateChatPlain(plain), scope);
}

const DURABLE_TYPES = new Set([
  "chat",
  "thread-reply",
  "edit",
  "delete",
  "reaction",
  "admin-delete",
]);

function recordOwnChatBadgeActivity(plain) {
  // The room frames are encrypted, so the Worker never receives a sender name
  // it can trust from their contents. This authenticated, no-content request
  // follows the successful socket handoff for a real human-authored message.
  // Bot relay lines, reactions, edits, and guest traffic intentionally do not
  // earn the personal Chat sleeve mark.
  if (
    !["chat", "thread-reply"].includes(plain?.type) ||
    plain?.senderId !== selfId
  ) {
    return;
  }
  const session = userSession();
  if (!session) return;
  const account = String(session.nodeName || "").trim().toLowerCase();
  if (!account || chatBadgeActivityAccount === account) return;
  chatBadgeActivityAccount = account;
  const token = String(session.sessionToken || "").trim();
  const headers = { "content-type": "application/json" };
  if (token && token !== "cookie") {
    headers.authorization = `Bearer ${token}`;
  }
  void fetch(CHAT_ACTIVITY_ENDPOINT, {
    method: "POST",
    cache: "no-store",
    credentials: "same-origin",
    headers,
    body: JSON.stringify(
      token && token !== "cookie" ? { sessionToken: token } : {},
    ),
  }).then((response) => {
    if (!response.ok && chatBadgeActivityAccount === account) {
      chatBadgeActivityAccount = "";
    }
  }).catch(() => {
    if (chatBadgeActivityAccount === account) chatBadgeActivityAccount = "";
  });
}

function send(plain) {
  if (!canJoinChannel()) {
    lockChatForNonUser();
    return Promise.resolve();
  }
  const scope = roomTransport?.room?.scope || roomScopeForChannel();
  if (
    scope === "public-world-general" &&
    plain?.channel &&
    plain.channel !== "#general"
  ) {
    return Promise.resolve();
  }
  const envelope = { persist: false };
  if (DURABLE_TYPES.has(plain && plain.type)) envelope.persist = true;
  if (!roomTransport) return Promise.resolve(false);
  // Only retained #general frames reach the counter behind the header badge,
  // and oversized file frames are dropped before retention.
  if (envelope.persist && scope === "public-world-general" && !plain.file) {
    noteSeenChatActivity();
  }
  return roomTransport.send(plain, { persist: envelope.persist }).then((sent) => {
    if (sent) recordOwnChatBadgeActivity(plain);
    if (sent && envelope.persist && scope === "public-world-general") {
      applyReceiptSent(plain.id);
    }
    return sent;
  });
}

function sendCallSignal(channelKey, type, extra) {
  if (
    !isDirectMessageKey(channelKey) ||
    roomScopeForChannel() !== channelKey ||
    !roomTransport?.connected
  ) {
    return Promise.resolve(false);
  }
  return send(makePlain(type, {
    channel: channelWireLabel(channelKey),
    ...extra,
  }));
}

function sendMissedCallLine(channelKey, media) {
  if (roomScopeForChannel() !== channelKey || !roomTransport?.connected) return;
  const plain = makePlain("chat", {
    channel: channelWireLabel(channelKey),
    text: `📞 Missed ${media} call`,
  });
  void send(plain);
  seen.add(plain.id);
  appendMessage(
    "self", plain.sender, plain.text, plain.id, plain.senderId, plain.ts,
    channelKey,
  );
}

const chatCalls = createChatCalls({
  iceServers: CALL_ICE_SERVERS,
  selfId,
  accountName: () => String(userSession()?.nodeName || "").trim().toLowerCase(),
  sendSignal: sendCallSignal,
  partnerFor: (channelKey) => directMessageForKey(channelKey)?.otherUser || "",
  onMissed: sendMissedCallLine,
  requestStart: (media) => startDirectCall(media),
});

function offerRequestedCall(partner) {
  const requested = String(chatQuery.get("call") || "").trim().toLowerCase();
  if (requested !== "audio" && requested !== "video") return;
  if (!callsSupported() || !directMessageForKey(activeChannel)) return;
  previewDirectCall(requested);
}

function previewDirectCall(media) {
  const direct = directMessageForKey(activeChannel);
  if (!direct || !userSession() || chatCalls.active()) return;
  chatCalls.preview(activeChannel, direct.otherUser, media).catch(() => {
    setStatus("The camera or microphone is unavailable", "unavailable");
  });
}

function startDirectCall(media) {
  const direct = directMessageForKey(activeChannel);
  if (!direct || !userSession() || (chatCalls.active() && !chatCalls.previewing())) return;
  const channelKey = activeChannel;
  runWhenConnected(() => {
    chatCalls.start(channelKey, direct.otherUser, media).catch(() => {
      setStatus(
        media === "video"
          ? "The camera or microphone is unavailable"
          : "The microphone is unavailable",
        "unavailable",
      );
    });
  });
}

const viewOnceClient = createViewOnceClient({ request: privateChannelRequest });
const viewOnceFilterSupport = installViewOnceFilters();
const viewOnceStatusQueue = new Map();
let viewOnceStatusTimer = null;
let activeViewOnceViewer = null;

function ownAccountName() {
  return String(userSession()?.nodeName || "").trim().toLowerCase();
}

function isOwnViewOnceEntry(entry) {
  const own = ownAccountName();
  return Boolean(
    entry?.viewOnce && own &&
    String(entry.sender || "").trim().toLowerCase() === own,
  );
}

function holdsViewOnceReceipt(plain) {
  return Boolean(
    plain?.type === "chat" &&
    viewOnceFromEntry(plain) &&
    !isOwnViewOnceEntry(plain) &&
    plain.senderId !== selfId,
  );
}

function viewOnceRecords(mediaId) {
  return [...rows.values()].filter((record) =>
    record.viewOnce?.media === mediaId);
}

function releaseViewOnceReceipt(record) {
  if (record && !record.self) roomTransport?.releaseReceipt?.(record.id);
}

function setViewOnceState(record, state) {
  if (!record?.viewOnce || !state) return;
  if (isFinalViewOnceState(record.viewOnceState) && !isFinalViewOnceState(state)) {
    return;
  }
  record.viewOnceState = state;
  if (isFinalViewOnceState(state)) {
    forgetViewOnceKey(record);
    releaseViewOnceReceipt(record);
  }
  paintViewOnceBubble(record);
}

function applyViewOnceState(mediaId, state) {
  for (const record of viewOnceRecords(mediaId)) {
    if (state === "opened" && activeViewOnceViewer?.media === mediaId) continue;
    setViewOnceState(record, state);
  }
}

function queueViewOnceStatus(record) {
  const direct = directMessageForKey(record.channel);
  if (!direct || !record.viewOnce) return;
  const ids = viewOnceStatusQueue.get(direct.id) || new Set();
  ids.add(record.viewOnce.media);
  viewOnceStatusQueue.set(direct.id, ids);
  if (viewOnceStatusTimer !== null) return;
  viewOnceStatusTimer = setTimeout(flushViewOnceStatus, 250);
}

async function flushViewOnceStatus() {
  viewOnceStatusTimer = null;
  const pending = [...viewOnceStatusQueue.entries()];
  viewOnceStatusQueue.clear();
  for (const [conversationId, ids] of pending) {
    const list = [...ids];
    for (let start = 0; start < list.length; start += 50) {
      let states;
      try {
        states = await viewOnceClient.status(conversationId, list.slice(start, start + 50));
      } catch (_) {
        continue;
      }
      for (const [mediaId, item] of states) {
        for (const record of viewOnceRecords(mediaId)) {
          setViewOnceState(record, viewOnceStateAfterStatus(record, item));
        }
      }
    }
  }
}

function startViewOnceRecording(kind) {
  const direct = directMessageForKey(activeChannel);
  if (!direct || !userSession() || !VIEW_ONCE_KINDS[kind]) return;
  const channelKey = activeChannel;
  openViewOnceRecorder({
    kind,
    recipient: direct.otherUser,
    ...viewOnceFilterChoices(viewOnceFilterSupport),
    onSend: (recording, onProgress) =>
      sendViewOnce(channelKey, direct, recording, onProgress),
    onClose: () => (kind === "video" ? viewOnceVideoBtn : viewOnceVoiceBtn)?.focus(),
  });
}

async function sendViewOnce(channelKey, direct, recording, onProgress) {
  const viewOnce = await viewOnceClient.upload(
    direct.id, recording.kind, recording.blob, recording.durationMs,
    { mime: recording.mime, onProgress },
  );
  const sent = await new Promise((resolve) => {
    runWhenConnected(() => {
      if (roomScopeForChannel() !== channelKey) {
        resolve(false);
        return;
      }
      const plain = makePlain("chat", {
        channel: channelWireLabel(channelKey),
        text: VIEW_ONCE_KINDS[recording.kind].text,
        viewOnce,
      });
      Promise.resolve(send(plain)).then((ok) => {
        if (ok === false) {
          resolve(false);
          return;
        }
        seen.add(plain.id);
        appendMessage(
          "self", plain.sender, "", plain.id, plain.senderId, plain.ts,
          channelKey, null, null, null, null, [], viewOnce,
        );
        const record = rows.get(plain.id);
        if (record) forgetViewOnceKey(record);
        resolve(true);
      }, () => resolve(false));
    });
  });
  if (!sent) {
    viewOnceClient.withdraw(direct.id, viewOnce.media).catch(() => {});
    throw new Error("view_once_not_sent");
  }
}

function withdrawViewOnce(record) {
  if (!record?.viewOnce || !record.self) return;
  const direct = directMessageForKey(record.channel);
  if (!direct || isFinalViewOnceState(record.viewOnceState)) return;
  viewOnceClient.withdraw(direct.id, record.viewOnce.media).catch(() => {});
}

async function openViewOnceRecord(record) {
  const direct = directMessageForKey(record.channel);
  const viewOnce = record.viewOnce;
  if (!direct || !viewOnce?.key || record.self || activeViewOnceViewer) return;
  setViewOnceState(record, "opening");
  activeViewOnceViewer = { media: viewOnce.media };
  let bytes;
  try {
    bytes = await viewOnceClient.download(direct.id, viewOnce);
  } catch (error) {
    activeViewOnceViewer = null;
    if (error?.status === 410 || error?.status === 404) {
      setViewOnceState(record, error?.status === 404 ? "expired" : "opened");
      flushViewOnceStatusFor(record);
    } else {
      setViewOnceState(record, "failed");
    }
    return;
  }
  let consumed = null;
  openViewOnceViewer({
    kind: viewOnce.kind,
    bytes,
    mime: viewOnce.mime,
    onStart: () => {
      setViewOnceState(record, "playing");
      consumed = viewOnceClient.consume(direct.id, viewOnce.media).catch(() => null);
    },
    onClose: async ({ started }) => {
      activeViewOnceViewer = null;
      if (!started) {
        setViewOnceState(record, "ready");
        return;
      }
      if (consumed) await consumed;
      setViewOnceState(record, "opened");
    },
  });
}

function flushViewOnceStatusFor(record) {
  queueViewOnceStatus(record);
}

function makeForkbotPlain(text) {
  return makePlain("chat", {
    channel: channelWireLabel(activeChannel),
    text: String(text || "").slice(0, MAX_TEXT),
    sender: "forkbot",
    senderId: selfId,
    bot: FORKBOT_SENDER_ID,
    accountKind: "user",
  });
}

function botLine(plain) {
  const bot = String(plain?.bot || "").trim().toLowerCase();
  if (!["forkbot", "claude", "codex"].includes(bot)) return plain;
  return { ...plain, sender: bot, senderId: bot, postedBy: plain.senderId };
}

async function broadcastForkbotMessage(text) {
  const moderated = await moderateChatText(text);
  const plain = makeForkbotPlain(moderated.text);
  send(plain);
  seen.add(plain.id);
  appendMessage("peer", plain.sender, plain.text, plain.id, plain.bot, plain.ts, activeChannel);
}

function forkbotModel() {
  // The open composer's value is authoritative. Persistence is best-effort:
  // browsers can deny localStorage while still allowing a person to select and
  // send a model for this prompt.
  const selected = String(forkbotModelSelect?.value || "").trim();
  if (selected) return selected;
  try {
    return localStorage.getItem(FORKBOT_MODEL_KEY) || "";
  } catch (error) {
    return "";
  }
}

function rememberForkbotModel(value) {
  try {
    if (value) localStorage.setItem(FORKBOT_MODEL_KEY, value);
    else localStorage.removeItem(FORKBOT_MODEL_KEY);
  } catch (error) {
    /* private-mode storage refusal only costs the pick its persistence */
  }
}

// Populate the composer's ForkBot model picker from the relay so a new Workers
// AI model becomes selectable by deploying the Worker, with no site rebuild.
// The picker stays hidden when the relay does not offer the endpoint (an older
// or self-hosted relay), in which case its own default model is used.
async function loadForkbotModels() {
  if (!forkbotModelSelect) return;
  let models = [];
  try {
    const response = await fetch(FORKBOT_MODELS_ENDPOINT, {
      headers: { accept: "application/json" },
    });
    if (!response.ok) return;
    const data = await response.json().catch(() => ({}));
    models = Array.isArray(data?.models) ? data.models : [];
  } catch (error) {
    return;
  }
  if (!models.length) return;
  const saved = forkbotModel();
  forkbotModelSelect.textContent = "";
  for (const model of models) {
    const id = String(model?.id || "");
    if (!id) continue;
    const option = document.createElement("option");
    option.value = id;
    option.textContent = model.default
      ? `${model.label || id} (default)`
      : model.label || id;
    if (model.description) option.title = model.description;
    if (id === saved || (!saved && model.default)) option.selected = true;
    forkbotModelSelect.append(option);
  }
  if (!forkbotModelSelect.options.length) return;
  // A saved pick the relay no longer offers falls through to the first option;
  // clear it so the stored value cannot outlive the model.
  if (saved && forkbotModelSelect.value !== saved) rememberForkbotModel("");
  forkbotModelSelect.hidden = false;
  forkbotModelSelect.addEventListener("change", () => {
    rememberForkbotModel(forkbotModelSelect.value);
  });
}

async function maybeAskForkbot(text) {
  if (!userSession()) return;
  if (!FORKBOT_MENTION_RE.test(text || "")) return;
  if (!controllerReadableScope()) {
    appendSystem(
      "forkbot is available only in public, controller-readable channels",
    );
    return;
  }
  // The triggering line is the last buffer entry (appendMessage ran just
  // before this) and is sent separately as `message`; drop it, drop ForkBot's
  // own replies, and cap the rest so ForkBot sees the lead-up conversation of
  // the room the mention happened in.
  const context = (recentContext.get(activeChannel) || [])
    .slice(0, -1)
    .filter((m) => m.sender.toLowerCase() !== "forkbot")
    .slice(-12);
  try {
    const response = await fetch(FORKBOT_ENDPOINT, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        message: text,
        sender: displayName(),
        room: ROOM_NAME,
        context,
        model: forkbotModel(),
      }),
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || !data || !data.botMessage) return;
    runWhenConnected(() => broadcastForkbotMessage(data.botMessage));
  } catch (error) {
    appendSystem("forkbot is unavailable");
  }
}

function switchChatRoom() {
  roomTransport?.dispose();
  roomTransport = null;
  openCallbacks = [];
  if (canJoinChannel()) {
    unlockChatForUser();
    connect();
  } else {
    lockChatForNonUser();
  }
}

function scheduleReconnect() {
  if (chatSuspended) return;
  if (!canJoinChannel()) return;
  setStatus("Disconnected · reconnecting…", "reconnecting");
  if (!roomTransport || roomTransport.state === "unauthorized") connect();
}

async function authorizeChatRoom(room) {
  if (room.scope === "public-world-general") {
    return {
      scope: room.scope,
      key: await deriveRoomKey(room.scope),
      roomName: PUBLIC_WORLD_GENERAL_ROOM,
      webSocketUrl: PUBLIC_WORLD_CHAT_WS_PATH,
    };
  }
  const access = await fetchRoomAccess(room.scope);
  return {
    scope: room.scope,
    passphrase: access.passphrase,
    roomName: access.room,
    webSocketUrl: access.webSocketUrl,
  };
}

function handleTransportState(state, detail) {
  const scope = detail.room?.scope || roomScopeForChannel();
  if (state === "connected") {
    if (scope !== roomScopeForChannel()) {
      switchChatRoom();
      return;
    }
    if (!officeAuthorizationExpired) showOfficeFailure("");
    const connectedChannel = scope === "public-world-general" ? "#general" : scope;
    // An open socket is not a loaded channel: the relay's retained backlog is
    // still on the wire. Marking the channel ready here painted the empty
    // "Say hi!" placeholder for the width of one replay, and the backlog then
    // shoved it aside. handleHistory below is what flips it to ready.
    const wasReady = channelSyncState.get(connectedChannel) === "ready";
    markChannelLoading(connectedChannel);
    // Only the first connect changes what the log says. A reconnect re-opens a
    // channel already on screen, and rebuilding it there threw the reader back
    // to the bottom of a conversation they had scrolled up through.
    if (!wasReady && connectedChannel === activeChannel) renderActiveChannel();
    const direct = directMessageForKey(scope);
    setStatus(scope === "public-world-general"
      ? "Connected · World"
      : direct
        ? `Connected · direct ${channelDisplayLabel(scope)}`
        : `Connected · ${
          privateChannelForKey(scope)?.visibility || "private"
        } ${channelDisplayLabel(scope)}`,
      "connected",
    );
    send(makePlain("hello", {
      channels: [channelWireLabel(activeChannel)],
    }));
    noteSelfRoster();
    renderTrustBanner();
    if (sealPanel && !sealPanel.hidden) renderSafetyNumber();
    const callbacks = openCallbacks;
    openCallbacks = [];
    callbacks.forEach((callback) => callback());
    return;
  }
  if (state === "authorizing" || state === "connecting") {
    setStatus("Connecting...", "connecting");
    return;
  }
  if (state === "reconnecting") {
    setStatus("Disconnected · reconnecting…", "reconnecting");
    return;
  }
  if (state === "unauthorized") {
    setStatus(CHAT_SIGN_IN_REQUIRED, "authorization");
    showOfficeFailure("auth");
    return;
  }
  if (state === "unavailable") {
    setStatus(OFFICE_RELAY_UNAVAILABLE, "unavailable");
    showOfficeFailure("relay");
  }
}

// ── Channel encryption policy ────────────────────────────────────────────
// The relay derives every default room key from its own DATA_KEY, so the room
// layer stops eavesdroppers on the wire but not the person running the relay.
// Private channels and DMs add the automatic device seal. Public
// channels deliberately stop at the room layer so controller integrations can
// read and post there.

function sealSupportedHere() {
  return Boolean(sealBtn) && !isOfficeEmbed;
}

function renderSealControl() {
  if (!sealBtn) return;
  if (!sealSupportedHere()) {
    sealBtn.hidden = true;
    closeSealPanel();
    return;
  }
  const scope = roomScopeForChannel();
  const controllerReadable = controllerReadableScope(scope);
  sealBtn.hidden = false;
  sealBtn.dataset.sealed = String(!controllerReadable);
  if (sealIcon) sealIcon.textContent = controllerReadable ? "🌐" : "🔒";
  const serverNotices = Boolean(privateChannelForKey(scope)?.serverVisible);
  if (sealLabel) {
    sealLabel.textContent = serverNotices
      ? "Server-visible"
      : controllerReadable ? "Public" : "End-to-end";
  }
  renderTrustBanner();
  sealBtn.title = serverNotices
    ? "Server notices written by ForkMesh itself; not end-to-end encrypted"
    : controllerReadable
    ? "Controller-readable public channel — ForkBot, MCP, and webhooks can participate"
    : "Automatic device-to-device encryption — the controller key alone cannot read messages";
  renderSealState();
}

function renderSealState() {
  if (!sealStateEl) return;
  const scope = roomScopeForChannel();
  const dropped = sealMismatches.get(scope);
  const unavailable = [...(dropped || [])].filter(Boolean).length;
  const refusedUnsealed = Boolean(dropped?.has(""));
  if (privateChannelForKey(scope)?.serverVisible) {
    sealStateEl.textContent =
      "Server notices: ForkMesh writes these notes itself, so the server can " +
      "read them. They are not end-to-end encrypted. Only you can see this channel.";
    return;
  }
  if (controllerReadableScope(scope)) {
    sealStateEl.textContent = unavailable
      ? "This channel is public and controller-readable, so ForkBot, MCP, and " +
        "webhooks can participate. Some older end-to-end encrypted history " +
        "was not addressed to this device."
      : "This channel is public and controller-readable, so ForkBot, MCP, and " +
        "webhooks can participate.";
    return;
  }
  sealStateEl.textContent = refusedUnsealed
    ? "Automatic encryption is on. An unencrypted or controller-authored frame " +
      "was refused."
    : unavailable
      ? "Automatic encryption is on. Some retained messages were not addressed " +
        "to this device, usually because they predate its first visit."
      : "Automatic encryption is on. Message keys are wrapped only for known " +
        "room devices; the relay receives ciphertext and public keys.";
}

// keyId "" means an unsealed frame was refused; anything else is a sender
// whose content key was not wrapped for this browser.
function noteSealMismatch(keyId, scope) {
  if (!scope) return;
  if (!sealMismatches.has(scope)) sealMismatches.set(scope, new Set());
  const seen = sealMismatches.get(scope);
  if (seen.has(keyId)) return;
  seen.add(keyId);
  if (scope === roomScopeForChannel()) renderSealControl();
}

function buildTrustNotice(notice) {
  const row = document.createElement("div");
  row.className = "chat-system chat-trust-notice";
  row.textContent = notice.text;
  return row;
}

function sealOptionsFor(scope) {
  const direct = directMessageForKey(scope);
  const own = String(userSession()?.nodeName || "");
  return direct && own ? { participants: [own, direct.otherUser] } : {};
}

function trustDeviceOwner(acct, channel) {
  const own = String(userSession()?.nodeName || "").toLowerCase();
  if (acct && acct === own) return "you";
  if (acct) return acct;
  return directPeerName(channel);
}

function directPeerName(channel) {
  return directMessageForKey(channel)?.otherUser || "this person";
}

function addTrustNotice(channel, key, text) {
  if (!channel || trustNoticeKeys.has(key)) return;
  trustNoticeKeys.add(key);
  const notice = { ts: Date.now(), text };
  const list = trustNotices.get(channel) || [];
  list.push(notice);
  trustNotices.set(channel, list.slice(-20));
  if (channel !== activeChannel) return;
  if (historyBatchChannels) {
    historyBatchChannels.add(channel);
    return;
  }
  clearEmpty();
  logEl.append(buildTrustNotice(notice));
  if (logNearBottom()) scrollLogToBottom();
}

function notePendingDevice(channel, kid, acct) {
  addTrustNotice(
    channel,
    `pending\n${channel}\n${kid}`,
    `A new device for ${trustDeviceOwner(acct, channel)} wants to join this chat. ` +
      "Check the safety number, then approve or block.",
  );
}

function handleSealEvent(event) {
  const channel = String(event?.channelScope || "");
  if (!isDirectMessageKey(channel) || event.controllerScope !== RELAY_HOST) return;
  if (event.type === "pending") notePendingDevice(channel, event.kid, event.acct);
  if (event.type === "safety-changed") {
    addTrustNotice(
      channel,
      `safety\n${channel}\n${Date.now()}`,
      `Your safety number with ${directPeerName(channel)} changed.`,
    );
  }
  if (channel === activeChannel) {
    renderTrustBanner();
    if (sealPanel && !sealPanel.hidden) renderSafetyNumber();
  }
}

function activeTrustTransport() {
  const scope = roomScopeForChannel();
  if (!isDirectMessageKey(scope) || roomTransport?.room?.scope !== scope) return null;
  return roomTransport.trustPolicy === "strict" ? roomTransport : null;
}

function renderTrustBanner() {
  if (!trustBanner) return;
  const transport = activeTrustTransport();
  const pending = transport ? transport.pendingDevices() : [];
  if (!pending.length) {
    trustBanner.hidden = true;
    delete trustBanner.dataset.kid;
    return;
  }
  const device = pending[0];
  for (const record of pending) notePendingDevice(activeChannel, record.kid, record.acct);
  trustBanner.dataset.kid = device.kid;
  if (trustBannerCopy) {
    const more = pending.length > 1 ? ` (${pending.length} devices are waiting)` : "";
    trustBannerCopy.textContent =
      `A new device for ${trustDeviceOwner(device.acct, activeChannel)} wants to join this chat${more}. ` +
      "Check the safety number, then approve or block.";
  }
  trustBanner.hidden = false;
}

async function decideTrustBanner(approve) {
  const kid = trustBanner?.dataset.kid;
  const transport = activeTrustTransport();
  if (!kid || !transport) return;
  if (trustApproveBtn) trustApproveBtn.disabled = true;
  if (trustBlockBtn) trustBlockBtn.disabled = true;
  try {
    if (approve) await transport.approveDevice(kid);
    else await transport.blockDevice(kid);
  } finally {
    if (trustApproveBtn) trustApproveBtn.disabled = false;
    if (trustBlockBtn) trustBlockBtn.disabled = false;
    renderTrustBanner();
    if (sealPanel && !sealPanel.hidden) renderSafetyNumber();
  }
}

function loadSafetyQr() {
  if (window.ForkMeshQR) return Promise.resolve(true);
  return new Promise((resolve) => {
    const script = document.createElement("script");
    script.src = "/qr.js";
    script.onload = () => resolve(Boolean(window.ForkMeshQR));
    script.onerror = () => resolve(false);
    document.head.append(script);
  });
}

let safetyRender = 0;

async function renderSafetyNumber() {
  if (!safetySection) return;
  const transport = activeTrustTransport();
  if (!transport) {
    safetySection.hidden = true;
    return;
  }
  const generation = ++safetyRender;
  const number = await transport.safetyNumber().catch(() => null);
  if (generation !== safetyRender) return;
  if (!number) {
    safetySection.hidden = true;
    return;
  }
  safetySection.hidden = false;
  const peer = directPeerName(activeChannel);
  if (safetyLead) {
    safetyLead.textContent =
      `Compare these numbers with ${peer}, or scan the code on their device. ` +
      "They cover every approved device in this conversation.";
  }
  if (safetyDigits) {
    safetyDigits.replaceChildren(...number.groups.map((group) => {
      const cell = document.createElement("span");
      cell.textContent = group;
      return cell;
    }));
  }
  if (safetyQr) {
    safetyQr.replaceChildren();
    safetyQr.dataset.value = number.qr;
    loadSafetyQr().then((ok) => {
      if (generation !== safetyRender || safetyQr.dataset.value !== number.qr) return;
      if (ok) {
        try {
          safetyQr.replaceChildren(window.ForkMeshQR.toCanvas(number.qr, 168));
          return;
        } catch (_) {}
      }
      const text = document.createElement("code");
      text.textContent = number.qr;
      safetyQr.replaceChildren(text);
    });
  }
  if (safetyState) {
    safetyState.textContent = number.verified
      ? `Verified. You checked this safety number with ${peer}.`
      : "Not verified yet.";
  }
  if (safetyVerifyBtn) {
    safetyVerifyBtn.disabled = number.verified;
    safetyVerifyBtn.textContent = number.verified ? "Verified" : "Mark as verified";
  }
}

async function markSafetyVerified() {
  const transport = activeTrustTransport();
  if (!transport) return;
  await transport.markVerified();
  renderSafetyNumber();
}

function openSealPanel() {
  if (!sealPanel || !sealSupportedHere()) return;
  closeChannelSettings();
  sealPanel.hidden = false;
  sealBtn?.setAttribute("aria-expanded", "true");
  renderSealState();
  renderSafetyNumber();
}

function closeSealPanel(options = {}) {
  if (!sealPanel || sealPanel.hidden) return false;
  sealPanel.hidden = true;
  sealBtn?.setAttribute("aria-expanded", "false");
  if (options.restoreFocus && sealBtn && !sealBtn.hidden) sealBtn.focus();
  return true;
}

function toggleSealPanel() {
  if (closeSealPanel({ restoreFocus: true })) return;
  openSealPanel();
}

function newRoomTransport() {
  return createChatRoomTransport({
    authorize: authorizeChatRoom,
    allowsControllerAccess: (room) => controllerReadableScope(room.scope),
    isDirectRoom: (room) => isDirectMessageKey(room.scope),
    // Separate from authorize() on purpose: private E2EE identity material may
    // never come from the relay, so it is resolved from this browser only.
    resolveSeal: (room, roomName) =>
      roomSeals.resolve(room.scope, roomName, RELAY_HOST, sealOptionsFor(room.scope)),
    onSealed: (keyId, room) => noteSealMismatch(keyId, room.scope),
    onTrust: (event, room) => {
      if (room.scope === roomScopeForChannel()) renderTrustBanner();
    },
    onPlain: (plain, room) => handlePlain(plain, room.scope),
    onHistory: (plains, room) => handleHistory(plains, room),
    onRecipients: (id, count, _room, names) =>
      applyRecipientCount(id, count, names),
    onServerCopies: (stored, room) => applyDirectServerCopies(stored, room.scope),
    onDelivered: (ids) => applyDirectDelivered(ids),
    onRead: (room) => applyDirectRead(room.scope),
    onMedia: (id, state) => applyViewOnceState(id, state),
    holdReceipt: (plain) => holdsViewOnceReceipt(plain),
    onState: handleTransportState,
    locationLike: { protocol: location.protocol, host: RELAY_HOST },
  });
}

async function connect() {
  if (chatSuspended) return;
  const scope = roomScopeForChannel();
  if (!canJoinChannel()) {
    lockChatForNonUser();
    return;
  }
  // WebCrypto (crypto.subtle) only exists in a secure context. Served over
  // plain HTTP - a self-hosted node or LAN IP opened on mobile - it is
  // undefined, so the room key can never derive. Say so plainly.
  if (!window.isSecureContext || !(window.crypto && window.crypto.subtle)) {
    setStatus("Chat needs a secure (HTTPS) connection", "insecure");
    return;
  }
  if (roomTransport?.room?.scope === scope) {
    if (roomTransport.state === "suspended") await roomTransport.resume();
    return;
  }
  roomTransport?.dispose();
  roomTransport = newRoomTransport();
  await roomTransport.connect({ scope });
}

function runWhenConnected(callback) {
  const scope = roomScopeForChannel();
  if (
    roomTransport?.connected &&
    roomTransport.room?.scope === scope
  ) {
    callback();
    return;
  }
  openCallbacks.push(callback);
  if (roomTransport && roomTransport.room?.scope !== scope) {
    switchChatRoom();
    return;
  }
  connect();
}

function attachmentDraftKey(rootId = "") {
  return rootId ? `thread:${rootId}` : `channel:${activeChannel}`;
}

function attachmentDraft(rootId = "") {
  const key = attachmentDraftKey(rootId);
  if (!attachmentDrafts.has(key)) attachmentDrafts.set(key, []);
  return attachmentDrafts.get(key);
}

function attachmentDraftContainer(rootId = "") {
  return rootId ? threadAttachmentDraftEl : attachmentDraftEl;
}

function renderAttachmentDraft(rootId = "") {
  const container = attachmentDraftContainer(rootId);
  if (!container) return;
  container.textContent = "";
  for (const record of attachmentDraft(rootId)) {
    const fileName = safeAttachmentName(record.file.name);
    const fileMime = safeAttachmentMime(record.file.type);
    const item = document.createElement("div");
    item.className = "chat-composer-attachment";
    if (record.previewUrl) {
      const preview = document.createElement("img");
      preview.className = "chat-composer-attachment-preview";
      preview.src = record.previewUrl;
      preview.alt = fileName;
      item.append(preview);
    }
    const copy = document.createElement("div");
    copy.className = "chat-composer-attachment-copy";
    const name = document.createElement("div");
    name.className = "chat-composer-attachment-name";
    name.textContent = fileName;
    name.title = fileName;
    const meta = document.createElement("div");
    meta.className = "chat-composer-attachment-meta";
    meta.textContent = `${fileMime} · ${formatAttachmentSize(record.file.size)}`;
    copy.append(name, meta);
    const remove = document.createElement("button");
    remove.className = "chat-composer-attachment-remove";
    remove.type = "button";
    remove.textContent = "×";
    remove.setAttribute("aria-label", `Remove ${fileName}`);
    remove.title = `Remove ${fileName}`;
    remove.addEventListener("click", () => removeDraftAttachment(record.id, { rootId }));
    item.append(copy, remove);
    container.append(item);
  }
}

function removeDraftAttachment(id, { rootId = "" } = {}) {
  const key = attachmentDraftKey(rootId);
  const records = attachmentDrafts.get(key) || [];
  const index = records.findIndex((record) => record.id === id);
  if (index < 0) return;
  const [record] = records.splice(index, 1);
  if (record.previewUrl) URL.revokeObjectURL(record.previewUrl);
  if (!records.length) attachmentDrafts.delete(key);
  renderAttachmentDraft(rootId);
}

function clearAttachmentDraft(rootId = "") {
  const key = attachmentDraftKey(rootId);
  for (const record of attachmentDrafts.get(key) || []) {
    if (record.previewUrl) URL.revokeObjectURL(record.previewUrl);
  }
  attachmentDrafts.delete(key);
  renderAttachmentDraft(rootId);
}

function stageAttachments(files, { rootId = "" } = {}) {
  const records = attachmentDraft(rootId);
  const candidates = Array.from(files || []);
  const remaining = Math.max(0, 4 - records.length);
  const accepted = attachmentFiles(candidates, remaining);
  if (candidates.length > accepted.length) {
    setAttachmentFeedback("Share up to 4 files in a message.");
  }
  for (const file of accepted) {
    if (file.size > MAX_ATTACHMENT_BYTES) {
      setAttachmentFeedback("Attachments must be 1 MiB or smaller.");
      continue;
    }
    if (!file.size) {
      setAttachmentFeedback("That file is empty.");
      continue;
    }
    records.push({
      id: `draft-${++attachmentDraftSequence}`,
      file,
      previewUrl: String(file.type || "").startsWith("image/")
        ? URL.createObjectURL(file)
        : "",
    });
  }
  if (!records.length) attachmentDrafts.delete(attachmentDraftKey(rootId));
  renderAttachmentDraft(rootId);
}

async function sendAttachment(file, { rootId = "" } = {}) {
  if (!file || !canJoinChannel()) {
    if (!canJoinChannel()) lockChatForNonUser();
    return false;
  }
  if (file.size > MAX_ATTACHMENT_BYTES) {
    setAttachmentFeedback("Attachments must be 1 MiB or smaller.");
    return false;
  }
  if (!file.size) {
    setAttachmentFeedback("That file is empty.");
    return false;
  }
  let buffer;
  try {
    buffer = await file.arrayBuffer();
  } catch (_) {
    setAttachmentFeedback("Could not read that attachment.");
    return false;
  }
  const channelAtSend = activeChannel;
  if (rootId && rootId !== activeThreadRootId) return false;
  const fileName = safeAttachmentName(file.name);
  const fileMime = safeAttachmentMime(file.type);
  const encodedFile = bytesToB64(buffer);
  runWhenConnected(() => {
    if (channelAtSend !== activeChannel) return;
    const plain = makePlain(rootId ? "thread-reply" : "chat", {
      ...(rootId ? { rootId } : {}),
      channel: channelWireLabel(channelAtSend),
      fileName,
      fileMime,
      file: encodedFile,
    });
    const attachment = attachmentFromEntry(plain);
    if (!attachment) {
      setAttachmentFeedback("Could not prepare that attachment.");
      return;
    }
    send(plain);
    seen.add(plain.id);
    if (rootId) {
      threads.addReply(portableMessageRecord({
        ...plain,
        channel: channelAtSend,
        attachment,
        self: true,
      }));
      if (activeThreadRootId === rootId) renderThread();
      renderThreadSummary(rootId);
    } else {
      appendMessage(
        "self",
        plain.sender,
        "",
        plain.id,
        plain.senderId,
        plain.ts,
        channelAtSend,
        attachment,
      );
    }
    setAttachmentFeedback(`Shared ${fileName}`);
  });
  return true;
}

async function sendAttachments(files, { rootId = "" } = {}) {
  const candidates = Array.from(files || []);
  const accepted = attachmentFiles(candidates, 4);
  if (candidates.length > accepted.length) {
    setAttachmentFeedback("Share up to 4 files at a time.");
  }
  for (const file of accepted) {
    await sendAttachment(file, { rootId });
  }
}

async function sendAttachmentDraft({ rootId = "" } = {}) {
  const records = [...attachmentDraft(rootId)];
  if (!records.length) return false;
  let sent = false;
  for (const record of records) {
    sent = (await sendAttachment(record.file, { rootId })) || sent;
  }
  if (sent) clearAttachmentDraft(rootId);
  return sent;
}

function dataTransferHasFiles(dataTransfer) {
  return Array.from(dataTransfer?.types || []).includes("Files");
}

function showDropOverlay(rootId = "") {
  if (!dropOverlay) return;
  const title = dropOverlay.querySelector("strong");
  if (title) title.textContent = rootId ? "Drop files into this thread" : "Drop files to share";
  dropOverlay.hidden = false;
  dropOverlay.setAttribute("aria-hidden", "false");
}

function clearDropState() {
  dragDepth = 0;
  if (!dropOverlay) return;
  dropOverlay.hidden = true;
  dropOverlay.setAttribute("aria-hidden", "true");
}

function installDropTarget(element, rootIdForDrop = () => "") {
  if (!element) return;
  element.addEventListener("dragenter", (event) => {
    if (!dataTransferHasFiles(event.dataTransfer)) return;
    event.preventDefault();
    dragDepth += 1;
    showDropOverlay(rootIdForDrop());
  });
  element.addEventListener("dragover", (event) => {
    if (!dataTransferHasFiles(event.dataTransfer)) return;
    event.preventDefault();
    if (event.dataTransfer) event.dataTransfer.dropEffect = "copy";
    showDropOverlay(rootIdForDrop());
  });
  element.addEventListener("dragleave", (event) => {
    if (!dataTransferHasFiles(event.dataTransfer)) return;
    dragDepth = Math.max(0, dragDepth - 1);
    if (!dragDepth) clearDropState();
  });
  element.addEventListener("drop", (event) => {
    if (!dataTransferHasFiles(event.dataTransfer)) return;
    event.preventDefault();
    const rootId = rootIdForDrop();
    const files = event.dataTransfer?.files || [];
    clearDropState();
    stageAttachments(files, { rootId });
  });
}

function clipboardImage(event) {
  const items = Array.from(event.clipboardData?.items || []);
  const item = items.find((candidate) =>
    candidate.kind === "file" && String(candidate.type || "").startsWith("image/"));
  return item ? item.getAsFile() : null;
}

function resizeComposer() {
  if (!input) return;
  input.style.height = "auto";
  input.style.height = `${Math.min(input.scrollHeight, 140)}px`;
}

function setComposerPopover(button, menu, open) {
  if (!button || !menu) return;
  menu.hidden = !open;
  button.setAttribute("aria-expanded", String(open));
  if (open) activeComposerPopoverTrigger = button;
}

function closeComposerPopovers({ restoreFocus = false } = {}) {
  const trigger = activeComposerPopoverTrigger;
  const wasOpen = Boolean(composerEmojiMenu && !composerEmojiMenu.hidden);
  setComposerPopover(composerEmojiBtn, composerEmojiMenu, false);
  activeComposerPopoverTrigger = null;
  if (restoreFocus && wasOpen) trigger?.focus();
  return wasOpen;
}

function composerMenuItems(menu) {
  return menu
    ? [...menu.querySelectorAll('[role="menuitem"]:not(:disabled)')]
    : [];
}

function handleComposerMenuKeydown(event) {
  const items = composerMenuItems(event.currentTarget);
  if (!items.length) return;
  const currentIndex = Math.max(0, items.indexOf(document.activeElement));
  let nextIndex = null;
  if (event.key === "ArrowDown" || event.key === "ArrowRight") {
    nextIndex = (currentIndex + 1) % items.length;
  } else if (event.key === "ArrowUp" || event.key === "ArrowLeft") {
    nextIndex = (currentIndex - 1 + items.length) % items.length;
  } else if (event.key === "Home") {
    nextIndex = 0;
  } else if (event.key === "End") {
    nextIndex = items.length - 1;
  }
  if (nextIndex === null) return;
  event.preventDefault();
  items[nextIndex].focus();
}

function toggleComposerPopover(button, menu) {
  if (!button || !menu || button.disabled) return;
  const shouldOpen = menu.hidden;
  closeComposerPopovers();
  if (shouldOpen) {
    setComposerPopover(button, menu, true);
    composerMenuItems(menu)[0]?.focus();
  }
}

function insertComposerText(value) {
  if (!input || input.disabled) return;
  const start = input.selectionStart ?? input.value.length;
  const end = input.selectionEnd ?? start;
  input.focus();
  const capacity = MAX_TEXT - (input.value.length - (end - start));
  if (capacity <= 0) return;
  const text = String(value || "");
  if (!text || text.length > capacity) return;
  input.setRangeText(text, start, end, "end");
  resizeComposer();
  updateMentionSuggest();
}

function insertComposerMention() {
  if (!input || input.disabled) return;
  const start = input.selectionStart ?? input.value.length;
  const before = input.value.slice(0, start);
  insertComposerText(before && !/\s$/.test(before) ? " @" : "@");
}

function composerFormatShortcut(event) {
  if (!(event.ctrlKey || event.metaKey) || event.isComposing) return "";
  const key = String(event.key || "").toLowerCase();
  if (event.altKey && event.shiftKey) return key === "c" ? "codeblock" : "";
  if (event.altKey) return "";
  if (event.shiftKey) {
    return {
      x: "strike",
      c: "code",
      u: "link",
      "7": "number",
      "&": "number",
      "8": "bullet",
      "*": "bullet",
      "9": "quote",
      "(": "quote",
    }[key] || "";
  }
  return { b: "bold", i: "italic" }[key] || "";
}

function applyComposerFormat(type) {
  if (!input) return;
  const start = input.selectionStart ?? input.value.length;
  const end = input.selectionEnd ?? start;
  let result;
  if (type === "link") {
    result = linkSelection(input.value, start, end);
  } else if (type === "bullet") {
    result = toggleLinePrefix(input.value, start, end, "- ");
  } else if (type === "number") {
    result = toggleLinePrefix(input.value, start, end, "1. ");
  } else if (type === "quote") {
    result = toggleLinePrefix(input.value, start, end, "> ");
  } else {
    const markers = {
      bold: ["*", "*"],
      italic: ["_", "_"],
      underline: ["++", "++"],
      strike: ["~", "~"],
      code: ["`", "`"],
      codeblock: ["```\n", "\n```"],
    };
    const pair = markers[type];
    if (!pair) return;
    result = wrapSelection(input.value, start, end, pair[0], pair[1]);
  }
  input.value = result.value.slice(0, MAX_TEXT);
  input.focus();
  input.setSelectionRange(
    Math.min(result.selectionStart, input.value.length),
    Math.min(result.selectionEnd, input.value.length),
  );
  resizeComposer();
  updateMentionSuggest();
}

async function runIssueSlashCommand(argument) {
  const text = String(argument || "").trim();
  if (!text) {
    appendSystem("Usage: /issue [owner/repo] <title>");
    return;
  }
  if (attachmentDraft().length) {
    appendSystem("Staged attachments are not included in /issue. Remove them or use the issue form.");
    return;
  }
  try {
    const entries = await loadIssueRepositories();
    if (!entries.length) throw new Error("No repositories are available to file into.");
    const firstSpace = text.search(/\s/);
    const firstToken = firstSpace < 0 ? text : text.slice(0, firstSpace);
    const explicitTarget = /^[a-z][a-z0-9-]{0,62}\/[A-Za-z0-9._-]{1,100}$/.test(
      firstToken,
    );
    let target = explicitTarget
      ? entries.find(
          (entry) => entry.value.toLowerCase() === firstToken.toLowerCase(),
        )
      : null;
    if (explicitTarget && !target) {
      throw new Error(`Repository ${firstToken} is not available.`);
    }
    if (!target) {
      let preferred = "";
      try { preferred = localStorage.getItem(ISSUE_REPOSITORY_KEY) || ""; } catch (_) {}
      target = entries.find((entry) => entry.value === preferred) ||
        entries.find((entry) => entry.value === "forkmesh/forkmesh") || entries[0];
    }
    const issueText = explicitTarget ? text.slice(firstToken.length).trim() : text;
    const title = issueTitleFromMessage(issueText);
    if (!title) throw new Error("Usage: /issue [owner/repo] <title>");
    const filed = await fileWebIssue(
      target,
      title,
      issueBodyFromMessage({
        text: issueText,
        who: displayName(),
        tsMs: Date.now(),
        channelLabel: channelDisplayLabel(activeChannel),
      }),
      String(readSession()?.nodeName || ""),
    );
    try { localStorage.setItem(ISSUE_REPOSITORY_KEY, target.value); } catch (_) {}
    appendSystem(issueFiledSummary(target.value, title, filed));
  } catch (error) {
    appendSystem(String(error?.message || "The issue could not be filed."));
  }
}

async function runTopicSlashCommand(argument) {
  const channel = activeTopicChannel();
  if (!channel) {
    appendSystem("Only the channel creator or an administrator can change its topic.");
    return;
  }
  const topic = String(argument || "").trim().replace(/\s+/g, " ");
  if (topic.length > 200) {
    appendSystem("Channel topics must be 200 characters or fewer.");
    return;
  }
  try {
    const data = await privateChannelRequest(
      `${PRIVATE_CHANNELS_ENDPOINT}/${channel.id}`,
      { method: "PATCH", body: JSON.stringify({ topic }) },
    );
    channel.topic = String(data?.channel?.topic || "");
    channel.updatedAt = Number(data?.channel?.updatedAt) || channel.updatedAt;
    updateChannelHeading();
    runWhenConnected(() => send(makePlain("channel-topic", {
      channel: channelWireLabel(activeChannel),
    })));
    appendSystem(topic ? `Channel topic changed to “${topic}”.` : "Channel topic cleared.");
  } catch (error) {
    appendSystem(privateChannelErrorMessage(error));
  }
}

function appsChannelContext(channel = activeChannel) {
  const record = privateChannelForKey(channel);
  return {
    channelId: record && record.visibility === "public" ? record.id : "",
    showSystem: appendSystem,
    showEphemeral: appendAppEphemeral,
  };
}

function appendAppEphemeral(payload) {
  const apps = window.ForkMeshChatApps;
  if (!apps) return;
  clearEmpty();
  logEl.append(apps.ephemeralNode(payload, appsChannelContext()));
  scrollLogToBottom();
}

async function handleSlashCommand(source) {
  const match = String(source || "").match(/^\/(issue|topic)(?:\s+([\s\S]*))?$/i);
  if (!match) {
    if (!/^\/[a-z]/i.test(String(source || "")) || !window.ForkMeshChatApps) return false;
    const handled = await window.ForkMeshChatApps.run(source, appsChannelContext());
    if (handled) {
      input.value = "";
      resizeComposer();
      closeSlashSuggest();
      closeMentionSuggest();
    }
    return handled;
  }
  input.value = "";
  resizeComposer();
  closeSlashSuggest();
  closeMentionSuggest();
  if (match[1].toLowerCase() === "issue") {
    await runIssueSlashCommand(match[2] || "");
  } else {
    await runTopicSlashCommand(match[2] || "");
  }
  return true;
}

async function sendCurrentMessage() {
  if (!canJoinChannel()) {
    lockChatForNonUser();
    return;
  }
  const originalSource = input.value.trim().slice(0, MAX_TEXT);
  if (await handleSlashCommand(originalSource)) return;
  const moderation = await moderateChatText(originalSource);
  const source = moderation.text;
  const text = plainTextFromRichSource(source).slice(0, MAX_TEXT);
  if (!text && !attachmentDraft().length) return;
  if (text) {
    input.value = "";
    resizeComposer();
    closeMentionSuggest();
    runWhenConnected(() => {
      const plain = makePlain("chat", {
        channel: channelWireLabel(activeChannel),
        text,
        richText: { v: 1, source },
      });
      send(plain);
      seen.add(plain.id); // we render it here; ignore the echo if one comes back
      const appsContext = appsChannelContext();
      if (appsContext.channelId) {
        void window.ForkMeshChatApps?.forwardMentions(text, appsContext.channelId, plain.id);
      }
      appendMessage(
        "self",
        plain.sender,
        text,
        plain.id,
        plain.senderId,
        plain.ts,
        activeChannel,
        null,
        plain.richText,
      );
      maybeAskForkbot(text);
      if (moderation.changed) appendSystem("Some words were filtered.");
    });
  }
  void sendAttachmentDraft();
}

function suspendOfficeChat() {
  if (!isOfficeEmbed || chatSuspended) return;
  chatSuspended = true;
  roomTransport?.suspend();
  openCallbacks = [];
  clearDropState();
  setStatus("Chat paused outside ForkMesh Office", "paused");
}

function receiveOfficeMessage(event) {
  if (!isOfficeEmbed) return;
  if (event.origin !== window.location.origin) return;
  if (event.source !== window.parent) return;
  const message = event.data;
  if (!message || message.type !== "office-chat-suspend") return;
  suspendOfficeChat();
}

if (isOfficeEmbed) {
  window.addEventListener("message", receiveOfficeMessage);
}

let chatControlsWired = false;
function wireChatControls() {
  if (chatControlsWired) return;
  chatControlsWired = true;
  sendBtn.addEventListener("click", sendCurrentMessage);
  directCreateBtn?.addEventListener("click", () => openDirectMessageDialog("new"));
  directAddBtn?.addEventListener("click", () => openDirectMessageDialog("add"));
  callAudioBtn?.addEventListener("click", () => previewDirectCall("audio"));
  callVideoBtn?.addEventListener("click", () => previewDirectCall("video"));
  viewOnceVideoBtn?.addEventListener("click", () => startViewOnceRecording("video"));
  viewOnceVoiceBtn?.addEventListener("click", () => startViewOnceRecording("voice"));
  directSearch?.addEventListener("input", scheduleDirectMessageSearch);
  if (attachmentBtn && attachmentInput) {
    attachmentBtn.addEventListener("click", () => attachmentInput.click());
    attachmentInput.addEventListener("change", () => {
      const files = Array.from(attachmentInput.files || []);
      attachmentInput.value = "";
      if (files.length) {
        stageAttachments(files);
        void sendAttachmentDraft();
      }
    });
  }
}

let chatComposerWired = false;
function wireChatComposer() {
  if (chatComposerWired) return;
  chatComposerWired = true;
  input.addEventListener("paste", (event) => {
    const file = clipboardImage(event);
    if (!file) return;
    event.preventDefault();
    stageAttachments([file]);
    void sendAttachmentDraft();
  });
  input.addEventListener("keydown", (event) => {
    if (slashSuggest) {
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        moveSlashSuggest(event.key === "ArrowDown" ? 1 : -1);
        return;
      }
      if ((event.key === "Tab" || event.key === "Enter") && !event.shiftKey) {
        event.preventDefault();
        acceptSlashSuggest();
        return;
      }
      if (event.key === "Escape") {
        event.preventDefault();
        closeSlashSuggest();
        return;
      }
    }
    // While the @mention popup is open it owns the keyboard: Tab (or Enter)
    // accepts the highlighted name, arrows move, Escape dismisses - only then
    // does Enter fall through to send.
    if (mentionSuggest) {
      const acceptsMention = event.key === "Tab" || event.key === "Enter";
      if (acceptsMention && !event.shiftKey) {
        event.preventDefault();
        acceptMentionSuggest();
        return;
      }
      if (event.key === "ArrowDown" || event.key === "ArrowUp") {
        event.preventDefault();
        moveMentionSuggest(event.key === "ArrowDown" ? 1 : -1);
        return;
      }
      if (event.key === "Escape") {
        event.preventDefault();
        closeMentionSuggest();
        return;
      }
    }
    const formatShortcut = composerFormatShortcut(event);
    if (formatShortcut) {
      event.preventDefault();
      applyComposerFormat(formatShortcut);
      return;
    }
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      sendCurrentMessage();
    }
  });
  // Track the "@partial" token under the caret as it changes - typing, caret
  // moves (arrows/click), and focus loss (delayed so a suggestion mousedown
  // still lands).
  input.addEventListener("input", updateSlashSuggest);
  input.addEventListener("input", () => {
    resizeComposer();
    updateMentionSuggest();
  });
  input.addEventListener("click", () => {
    updateSlashSuggest();
    updateMentionSuggest();
  });
  input.addEventListener("keyup", (event) => {
    if (["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
      updateSlashSuggest();
      updateMentionSuggest();
    }
  });
  input.addEventListener("blur", () => {
    setTimeout(closeSlashSuggest, 120);
    setTimeout(closeMentionSuggest, 120);
  });
}

let threadControlsWired = false;
function wireThreadControls() {
  if (threadControlsWired) return;
  threadControlsWired = true;
  threadSendBtn?.addEventListener("click", sendThreadReply);
  threadCloseBtn?.addEventListener("click", () => closeThread());
  threadInput?.addEventListener("input", resizeThreadComposer);
  threadInput?.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !threadInput.value) {
      event.preventDefault();
      closeThread();
    } else if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      sendThreadReply();
    }
  });
  if (threadAttachmentBtn && threadAttachmentInput) {
    threadAttachmentBtn.addEventListener("click", () => threadAttachmentInput.click());
    threadAttachmentInput.addEventListener("change", () => {
      const files = Array.from(threadAttachmentInput.files || []);
      threadAttachmentInput.value = "";
      if (files.length && activeThreadRootId) {
        stageAttachments(files, { rootId: activeThreadRootId });
      }
    });
  }
  threadInput?.addEventListener("paste", (event) => {
    const file = clipboardImage(event);
    if (!file || !activeThreadRootId) return;
    event.preventDefault();
    stageAttachments([file], { rootId: activeThreadRootId });
  });
}

let messageActionControlsWired = false;
function wireMessageActionControls() {
  if (messageActionControlsWired) return;
  messageActionControlsWired = true;
  deleteConfirmBtn?.addEventListener("click", confirmMessageDelete);
  deleteDialog?.addEventListener("close", () => {
    const trigger = pendingDeleteTrigger;
    pendingDeleteRecord = null;
    pendingDeleteTrigger = null;
    trigger?.focus();
  });
}

async function initChat() {
  wireChatControls();
  wireChatComposer();
  wireThreadControls();
  wireMessageActionControls();
  await hydrateUserSession();
  await loadOfficeOrganizationMembership();
  ensureChannel("#general");
  await refreshPrivateChannels({ connect: false });
  await refreshDirectMessages({ connect: false });
  const requestedDirect = String(chatQuery.get("dm") || "").trim().toLowerCase()
    .replace(/^@/, "");
  if (requestedDirect && canMessagePerson(requestedDirect)) {
    await startDirectMessage(requestedDirect);
    offerRequestedCall(requestedDirect);
  } else if (chatQuery.has("dm") && !requestedDirect && userSession() &&
      !isOfficeEmbed) {
    openDirectMessageDialog("new");
  }
  const label = channelDisplayLabel(activeChannel);
  updateChannelHeading();
  if (input) input.placeholder = `Message ${label}…`;
  updateAdminChannelControls();
  renderRooms();
  renderPeople();
  // Fill the people pane with every registered user (and thereafter pick up
  // brand-new signups — accounts that have never spoken have no socket frame
  // to announce them, so this one stays on its edge-cached tick).
  refreshUsersDirectory();
  watchUserStatuses();
  loadForkbotModels();
  // Once on open, then again when the reader leaves. In between, every
  // retained line that crosses this page advances the baseline locally off the
  // socket (noteSeenChatActivity), which keeps the header badge at zero while
  // chat is the page in front of you; the read on the way out is what corrects
  // the drift two open tabs would otherwise accumulate.
  markChatActivitySeen();
  document.addEventListener("visibilitychange", rebaselineChatActivityOnLeave);
  document.addEventListener("visibilitychange", markActiveDirectMessageReadIfDeferred);
  document.addEventListener("visibilitychange", () => queueReadMark());
  window.addEventListener("pagehide", rebaselineChatActivityOnLeave);
  setInterval(() => {
    if (!document.hidden) refreshUsersDirectory();
  }, USERS_DIRECTORY_REFRESH_MS);
  // Conversations and their unread counts were read once above; from here they
  // are push-driven. A DM that lands in a conversation this browser is not
  // sitting in is the one case the room sockets cannot report, and the relay
  // pushes a payload-free "direct-messages" frame for exactly that
  // (notify_account_event).
  startAccountEventChannel();
  // Public World #general connects for everyone. Private channels remain
  // session-gated and each uses its own ticketed room and current key version.
  if (canJoinChannel()) {
    markChannelLoading();
    unlockChatForUser();
    connect();
    renderActiveChannel();
  } else {
    lockChatForNonUser();
  }
  openChatLinkFromHash();
  if (clearBtn) clearBtn.addEventListener("click", clearChat);
  directMoreBtn?.addEventListener("click", loadMoreDirectMessages);
  directDialogClose?.addEventListener("click", () => directDialog?.close());
  for (const button of formatButtons) {
    button.addEventListener("mousedown", (event) => event.preventDefault());
    button.addEventListener("click", () => {
      applyComposerFormat(button.dataset.format);
    });
  }
  composerEmojiBtn?.addEventListener("click", () => {
    toggleComposerPopover(composerEmojiBtn, composerEmojiMenu);
  });
  for (const menu of [composerEmojiMenu]) {
    menu?.addEventListener("keydown", handleComposerMenuKeydown);
  }
  composerMentionBtn?.addEventListener("click", () => {
    closeComposerPopovers();
    insertComposerMention();
  });
  for (const button of composerEmojiButtons) {
    button.addEventListener("click", () => {
      closeComposerPopovers();
      insertComposerText(button.dataset.composerEmoji || button.textContent);
    });
  }
  document.addEventListener("click", (event) => {
    const target = event.target;
    const insideEmoji = composerEmojiBtn?.contains(target) || composerEmojiMenu?.contains(target);
    if (!insideEmoji) closeComposerPopovers();
  });
  document.addEventListener("focusin", (event) => {
    const trigger = activeComposerPopoverTrigger;
    if (!trigger) return;
    const menu = composerEmojiMenu;
    if (trigger.contains(event.target) || menu?.contains(event.target)) return;
    closeComposerPopovers();
  });
  channelCreateBtn?.addEventListener("click", () => openChannelDialog("create"));
  channelVisibilityInput?.addEventListener("change", describeChannelAudience);
  channelInitialMembersInput?.addEventListener("input", describeChannelAudience);
  channelManageBtn?.addEventListener("click", () => toggleChannelSettings());
  sealBtn?.addEventListener("click", () => toggleSealPanel());
  sealPanelClose?.addEventListener("click", () => closeSealPanel({ restoreFocus: true }));
  safetyVerifyBtn?.addEventListener("click", () => { void markSafetyVerified(); });
  trustApproveBtn?.addEventListener("click", () => { void decideTrustBanner(true); });
  trustBlockBtn?.addEventListener("click", () => { void decideTrustBanner(false); });
  trustSafetyBtn?.addEventListener("click", () => openSealPanel());
  channelSettingsClose?.addEventListener("click", () => {
    closeChannelSettings({ restoreFocus: true });
  });
  channelSettingsForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    saveChannelSettings();
  });
  channelDeleteBtn?.addEventListener("click", () => {
    if (channelDeleteConfirm) channelDeleteConfirm.hidden = false;
    if (channelDeleteBtn) channelDeleteBtn.hidden = true;
    channelDeleteCancel?.focus();
  });
  channelDeleteCancel?.addEventListener("click", () => {
    resetChannelDeleteConfirm();
    channelDeleteBtn?.focus();
  });
  channelDeleteConfirmed?.addEventListener("click", () => {
    deleteActiveChannel();
  });
  // A click anywhere outside the anchored panel dismisses it, like a menu.
  document.addEventListener("click", (event) => {
    if (channelSettingsPanel?.hidden !== false) return;
    if (channelSettingsPanel.contains(event.target)) return;
    if (channelManageBtn?.contains(event.target)) return;
    closeChannelSettings();
  });
  document.addEventListener("click", (event) => {
    if (sealPanel?.hidden !== false) return;
    if (sealPanel.contains(event.target)) return;
    if (sealBtn?.contains(event.target)) return;
    if (trustSafetyBtn?.contains(event.target)) return;
    closeSealPanel();
  });
  channelDialogClose?.addEventListener("click", () => channelDialog?.close());
  channelCreateForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    createChannel();
  });
  channelInviteForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    inviteChannelMember(channelUsernameInput?.value || "");
  });
  channelWebhookForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    createChannelWebhook();
  });
  inviteLinkForm?.addEventListener("submit", (event) => {
    event.preventDefault();
    copyChannelInviteLink();
  });
  inviteLinkResetBtn?.addEventListener("click", () => copyChannelInviteLink({ fresh: true }));
  inviteLinkOffBtn?.addEventListener("click", turnOffChannelInviteLink);
  joinConfirmBtn?.addEventListener("click", acceptPendingInvite);
  joinDialog?.addEventListener("close", () => {
    if (joinDialog.returnValue !== "confirm") pendingInviteToken = "";
  });
  window.addEventListener("hashchange", openChatLinkFromHash);
  officeRetryBtn?.addEventListener("click", () => {
    showOfficeFailure("");
    connect();
  });
  installDropTarget(mainPane);
  installDropTarget(threadView, () => activeThreadRootId);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && closeComposerPopovers({ restoreFocus: true })) {
      event.preventDefault();
      return;
    }
    if (event.key === "Escape" && closeSealPanel({ restoreFocus: true })) {
      event.preventDefault();
      return;
    }
    if (event.key === "Escape" && closeChannelSettings({ restoreFocus: true })) {
      event.preventDefault();
      return;
    }
    if (event.key === "Escape" && dragDepth) clearDropState();
  });
  resizeComposer();
  scheduleDateDividerRefresh();
  // Presence beat, matching the desktop's cadence: keeps our roster entry
  // fresh for peers AND keeps the room DO from closing the socket as stale
  // (it reaps sockets that send nothing for 3 minutes — the old web client's
  // idle disconnects).
  setInterval(() => {
    if (roomTransport?.connected && canJoinChannel()) {
      send(makePlain("presence"));
      noteSelfRoster();
    }
  }, PRESENCE_INTERVAL_MS);
  // Re-evaluate online dots as peers go stale even with no traffic.
  setInterval(renderPeople, 30000);
  void loadMemberPlanTier();
  chatRetention.onChange(() => {
    pruneExpiredMessages();
    renderRetentionControl();
  });
  chatRetentionSelect?.addEventListener("change", () => void changeChatRetention());
  setInterval(pruneExpiredMessages, 60000);
  pruneExpiredMessages();
  void loadChatRetention();
  if (isOfficeEmbed && window.parent !== window) {
    window.parent.postMessage(
      { type: "office-chat-ready" },
      window.location.origin,
    );
  }
}

if (logEl && input && sendBtn) {
  initChat();
}
