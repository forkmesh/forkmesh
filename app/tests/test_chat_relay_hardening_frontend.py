from pathlib import Path

APP = Path(__file__).resolve().parents[1]
ROOT = APP.parent
PUBLIC = APP / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
TRANSPORT = (PUBLIC / "chat-room-transport.js").read_text(encoding="utf-8")
CALLS = (PUBLIC / "chat-calls.js").read_text(encoding="utf-8")
WORLD = (ROOT / "world/public/world/world.js").read_text(encoding="utf-8")
OFFICE = (ROOT / "office/public/features/chat.js").read_text(encoding="utf-8")


def _body(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


def test_each_web_client_uses_a_node_key_per_browser_and_account():
    for source in (CHAT, DASHBOARD, OFFICE):
        assert "forkmesh.chat.selfId" not in source
        assert "chatProfileFor(" in source
        assert "bindNodeKey(" in source
        assert "store.ready()" in source
    assert "let selfId = \"\";" in CHAT and "let selfId = \"\";" in DASHBOARD
    assert "selfId: () => selfId," in CHAT
    assert 'typeof selfId === "function" ? selfId() : selfId' in CALLS


def test_the_transport_seals_every_person_frame_and_opens_only_server_marked_plaintext():
    send = _body(TRANSPORT, "async send(plain, { persist = false } = {}) {", "refsFor(ids) {")
    assert "currentControllerReadable\n        ? null" not in send
    assert "if (!seal || (!isDirectRoom(room) && isPrivateRelayFrame(plain))) return false;" in send
    assert "!direct && envelope?.srv === true" in TRANSPORT
    assert "} else if (controllerReadable) {\n        return { plain: framed };" not in TRANSPORT
    encrypt = _body(DASHBOARD, "async function encryptObject(obj) {", "let keyReplyTimer")
    assert "PUBLIC_WORLD_GENERAL" not in encrypt
    assert "if (PUBLIC_WORLD_GENERAL) return framed;" not in DASHBOARD


def test_only_the_sealed_author_edits_or_deletes_and_only_sealed_frames_react():
    chat = _body(CHAT, "function handlePlain(plain, scope = roomScopeForChannel()) {", "async function onFrame(")
    assert "if (!isSealedPlain(plain) && !isServerPlain(plain)) return;" in chat
    assert "target.senderId === plain.senderId" not in chat
    assert "rec.senderId === plain.senderId" not in chat
    assert chat.count("mayChangeMessage(plain,") == 2
    assert "isSealedPlain(plain) && targetMatchesScope(plain.target, scope)" in chat
    assert "historyEntries(plain)" in chat
    dash = _body(DASHBOARD, "function handlePlain(plain, historyReplay = false) {", "async function flushHistoryReplay(")
    assert "if (!isAuthenticatedPlain(plain)) return;" in dash
    assert "rec.senderId === plain.senderId" not in dash
    assert dash.count("frameAuth.mayChangeMessage(plain, rec)") == 2
    assert "frameAuth.isSealedPlain(plain) && once(plain.id)" in dash
    assert "frameAuth.historyEntries(plain)" in dash


def test_app_blocks_and_system_notices_render_only_from_server_frames():
    body = _body(CHAT, "function appMessageFromEntry(entry, channelKey) {", "function renderThreadEntry(")
    assert body.lstrip().startswith("if (!isServerPlain(entry)) return null;")


def test_names_come_from_the_server_checked_owner_before_a_frame_is_shown():
    assert "await nodeOwners.prefetch(plains, { selfNode: selfId });" in CHAT
    assert "onPlain: (plain, room) => receivePlains([plain], room, false)," in CHAT
    assert "onHistory: (plains, room) => receivePlains(plains, room, true)," in CHAT
    assert "await nodeOwners.prefetch(list, { selfNode: selfId });" in DASHBOARD
    assert "const decoded = await attributedPlains(frames);" in DASHBOARD
    assert "await owners.prefetch(plains, { selfNode: me });" in OFFICE
    for source in (CHAT, DASHBOARD):
        assert 'String(plain.reactorName || plain.sender || "peer")' not in source


def test_world_bubbles_need_a_confirmed_account_or_a_bot_line():
    handler = _body(WORLD, "handleWorldChatMessage = (event) => {", "handleNativeWorldChatMessage")
    assert "if (data.account === true || data.self === true) {" in handler
    assert "if (data.bot !== true && data.integration !== true) return;" in handler
    assert handler.index("data.bot !== true") < handler.index('senderName === "forkbot"')
    assert 'account: entry.accountKind === "user" && Boolean(entry.verifiedAccount) && !entry.postedBy,' in DASHBOARD
    assert "{ ...entry, account: false, bot: false, integration: false }" in DASHBOARD


def test_the_world_origin_serves_the_frame_auth_module():
    for path in ("world/worker.js", "world/wrangler.toml", "deploy/core/caddy_routes.py", "tools/cloudflare_bootstrap.py"):
        assert "/chat-frame-auth.js" in (ROOT / path).read_text(encoding="utf-8"), path


def test_bot_lines_show_who_posted_them_and_float_no_bot_bubble():
    assert "return botViaLine(plain, [\"forkbot\", \"claude\", \"codex\"]);" in CHAT
    assert "frameAuth.botLine(entry, [FORKBOT_SENDER_ID, CLAUDE_SENDER_ID, CODEX_SENDER_ID, ORG_BOT_SENDER_ID]);" in DASHBOARD
    for source in (CHAT, DASHBOARD):
        assert "sender: bot, senderId: bot" not in source
        assert "botVia ? " in source
    assert "sender: botViaName(plain) || String(plain.sender || 'someone').slice(0, 63)," in OFFICE
    assert "via: Boolean(entry.botVia)," in DASHBOARD
    assert "bot: meta?.bot === true && meta?.via !== true," in DASHBOARD
    handler = _body(WORLD, "handleWorldChatMessage = (event) => {", "handleNativeWorldChatMessage")
    guard = "if (data.via === true || /\\s·\\svia\\s/.test(sender)) return;"
    assert guard in handler
    assert handler.index(guard) < handler.index('senderName === "forkbot"')


def test_channel_invite_tokens_travel_in_the_body_not_the_path():
    import json
    import subprocess

    assert 'CHAT_INVITES_ENDPOINT + "/" + token' not in CHAT
    opener = "async function openChatLinkFromHash" + _body(CHAT, "async function openChatLinkFromHash", "\nfunction channelInviteeNames")
    script = """
      const calls = [];
      const token = "a".repeat(64);
      const CHAT_INVITES_ENDPOINT = "/api/chat/invites";
      const CHAT_ROOM_HASH_RE = /^#room=([0-9a-f]{32})$/;
      const CHAT_INVITE_HASH_RE = /^#invite=([0-9a-f]{64})$/;
      const isOfficeEmbed = false;
      const privateChannels = new Map();
      let pendingInviteToken = "";
      const location = { hash: "#invite=" + token, assign() {} };
      const userSession = () => ({ nodeName: "alice", sessionToken: "t" });
      const clearChatLinkHash = () => {};
      const showJoinDialog = () => {};
      const openJoinedChannel = async () => {};
      const chatInviteErrorMessage = () => "";
      const privateChannelRequest = async (path, options = {}) => {
        calls.push({ path, method: options.method || "GET", body: options.body || "" });
        return { channel: { id: "c".repeat(32), name: "room" }, joined: false };
      };
    """ + opener + """
      await openChatLinkFromHash();
      await acceptPendingInvite();
      process.stdout.write(JSON.stringify(calls));
    """
    done = subprocess.run(["node", "--input-type=module", "-e", script], capture_output=True, text=True, check=True)
    calls = json.loads(done.stdout)
    assert calls == [
        {"path": "/api/chat/invites/preview", "method": "POST", "body": json.dumps({"token": "a" * 64}, separators=(",", ":"))},
        {"path": "/api/chat/invites/accept", "method": "POST", "body": json.dumps({"token": "a" * 64}, separators=(",", ":"))},
    ]


def test_webhook_lines_carry_a_server_visible_marker_and_private_channels_take_webhooks():
    assert "} else if (!direct && envelope?.srv === true) {" in TRANSPORT
    assert "isServerPlain(entry) && entry.serverVisible === true," in CHAT
    assert 'marker.textContent = "server-visible";' in CHAT
    assert 'channelWebhooksSection.hidden = channel.visibility !== "public";' not in CHAT
    assert "channelWebhooksSection.hidden = !channelAcceptsWebhooks(channel);" in CHAT
    accepts = _body(CHAT, "function channelAcceptsWebhooks(channel) {", "\n}")
    assert "!channel.serverVisible" in accepts and "!channel.office" in accepts
    assert "visibility" not in accepts
    html = (PUBLIC / "chat.html").read_text(encoding="utf-8")
    assert "Public channels can accept controller integrations." not in html
    assert "the server can read them, in private channels too" in html
