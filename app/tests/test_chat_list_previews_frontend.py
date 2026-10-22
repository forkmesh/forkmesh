#!/usr/bin/env python3

import json
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT_TEXT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
CHAT_CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
DIRECT = "direct:" + "a" * 32
OTHER_DIRECT = "direct:" + "b" * 32
PRIVATE = "private:" + "c" * 32


def _run_module(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        cwd=PUBLIC,
    )
    return json.loads(result.stdout)


def _function_source(name):
    match = re.search(
        r"^(?:async )?function " + re.escape(name) + r"\(.*?^}\n", CHAT_TEXT,
        re.S | re.M)
    assert match, name
    return match.group(0)


def _helpers(*names):
    return "\n".join(_function_source(name) for name in names)


def _preview_run(steps):
    helpers = _helpers(
        "isDirectMessageKey",
        "directMessageKey",
        "isOwnChatMessage",
        "fmtTime",
        "directRecency",
        "sortedDirectMessages",
        "conversationPreviewRecord",
        "conversationPreviewText",
        "roomPreviewTime",
    )
    return _run_module(f"""
      import {{ plainTextFromRichSource }}
        from {json.dumps((PUBLIC / "chat-rich-text.js").as_uri())};
      import {{ sameLocalDate }}
        from {json.dumps((PUBLIC / "chat-date-groups.js").as_uri())};
      const selfId = "me";
      const userSession = () => ({{ nodeName: "jett" }});
      const channelMessages = new Map();
      const directMessages = new Map();
      {helpers}
      const add = (channel, fields) => {{
        const list = channelMessages.get(channel) || [];
        list.push({{ channel, ts: list.length + 1, senderId: "peer",
          sender: "ana", text: "", ...fields }});
        channelMessages.set(channel, list);
        return list[list.length - 1];
      }};
      const line = (channel) =>
        conversationPreviewText(conversationPreviewRecord(channel));
      {steps}
    """)


def test_direct_rows_show_the_newest_line_with_you_for_own_messages():
    result = _preview_run(f"""
      const empty = line({json.dumps(DIRECT)});
      add({json.dumps(DIRECT)}, {{ id: "m1", text: "first" }});
      const peer = line({json.dumps(DIRECT)});
      add({json.dumps(DIRECT)}, {{ id: "m2", senderId: "me", self: true,
        sender: "jett", text: "on my\\n  way" }});
      const own = line({json.dumps(DIRECT)});
      add({json.dumps(DIRECT)}, {{ id: "m3", senderId: "phone",
        sender: "Jett", text: "from my phone" }});
      const otherDevice = line({json.dumps(DIRECT)});
      process.stdout.write(JSON.stringify({{ empty, peer, own, otherDevice }}));
    """)
    assert result == {
        "empty": "",
        "peer": "first",
        "own": "You: on my way",
        "otherDevice": "You: from my phone",
    }


def test_channel_rows_name_the_sender_and_reduce_formatting():
    result = _preview_run(f"""
      add({json.dumps(PRIVATE)}, {{ id: "c1", sender: "ana",
        text: "**bold** and _it_",
        richText: {{ v: 1, source: "**bold** and _it_" }} }});
      const formatted = line({json.dumps(PRIVATE)});
      add({json.dumps(PRIVATE)}, {{ id: "c2", sender: "bo",
        attachment: {{ fileName: "plan.pdf" }} }});
      const file = line({json.dumps(PRIVATE)});
      add({json.dumps(PRIVATE)}, {{ id: "c3", senderId: "me", self: true,
        sender: "jett", attachment: {{ fileName: "shot.png" }} }});
      const ownFile = line({json.dumps(PRIVATE)});
      add("#general", {{ id: "g1", sender: "cy", text: "hi all" }});
      const general = line("#general");
      process.stdout.write(JSON.stringify({{
        formatted, file, ownFile, general }}));
    """)
    assert result == {
        "formatted": "ana: bold and it",
        "file": "bo: 📎 plan.pdf",
        "ownFile": "You: 📎 shot.png",
        "general": "cy: hi all",
    }


def test_deleted_roots_and_empty_records_fall_back_to_the_previous_line():
    result = _preview_run(f"""
      add({json.dumps(DIRECT)}, {{ id: "m1", text: "kept" }});
      const removed = add({json.dumps(DIRECT)}, {{ id: "m2", text: "gone" }});
      removed.deleted = true;
      removed.text = "";
      const afterDelete = line({json.dumps(DIRECT)});
      const edited = channelMessages.get({json.dumps(DIRECT)})[0];
      edited.text = "kept, edited";
      const afterEdit = line({json.dumps(DIRECT)});
      process.stdout.write(JSON.stringify({{ afterDelete, afterEdit }}));
    """)
    assert result == {"afterDelete": "kept", "afterEdit": "kept, edited"}


def test_direct_rows_sort_by_recency_then_name():
    result = _preview_run(f"""
      directMessages.set("a".repeat(32), {{ id: "a".repeat(32),
        otherUser: "zed", updatedAt: 10 }});
      directMessages.set("b".repeat(32), {{ id: "b".repeat(32),
        otherUser: "amy", updatedAt: 0 }});
      directMessages.set("d".repeat(32), {{ id: "d".repeat(32),
        otherUser: "bob", updatedAt: 0 }});
      const byServer = sortedDirectMessages().map((d) => d.otherUser);
      channelMessages.set({json.dumps(OTHER_DIRECT)}, [{{
        id: "x", channel: {json.dumps(OTHER_DIRECT)}, ts: 99999999999999,
        senderId: "peer", sender: "amy", text: "new" }}]);
      const afterMessage = sortedDirectMessages().map((d) => d.otherUser);
      process.stdout.write(JSON.stringify({{ byServer, afterMessage }}));
    """)
    assert result == {
        "byServer": ["zed", "amy", "bob"],
        "afterMessage": ["amy", "zed", "bob"],
    }


def test_row_time_is_short_and_local():
    result = _preview_run("""
      const now = new Date(2026, 9, 13, 18, 0).getTime();
      process.stdout.write(JSON.stringify({
        today: roomPreviewTime(new Date(2026, 9, 13, 14, 2).getTime(), now),
        week: roomPreviewTime(new Date(2026, 9, 10, 9, 0).getTime(), now),
        older: roomPreviewTime(new Date(2026, 9, 3, 9, 0).getTime(), now),
        lastYear: roomPreviewTime(new Date(2025, 9, 3, 9, 0).getTime(), now),
        none: roomPreviewTime(0, now),
      }));
    """)
    assert result == {
        "today": "2:02pm",
        "week": "Sat",
        "older": "3 Oct",
        "lastYear": "3 Oct 2025",
        "none": "",
    }


def test_rows_carry_the_preview_and_refresh_it_without_rebuilding():
    rooms = _function_source("renderRooms")
    direct = _function_source("renderDirectMessages")
    assert "btn.append(roomTextColumn(label, name));" in rooms
    assert "btn.dataset.channel = name;" in rooms
    assert "const column = roomTextColumn(label, key);" in direct
    assert "for (const direct of sortedDirectMessages()) {" in direct
    assert 'face.append(makeAvatar(direct.otherUser, "user"), presence);' in direct
    assert "queueRoomPreview(channel);" in _function_source("insertMessage")
    assert "queueRoomPreview(rec.channel);" in _function_source("removeMessage")
    assert "queueRoomPreview(record.channel);" in _function_source(
        "saveMessageEdit")
    assert "queueRoomPreview(channel);" in _function_source(
        "pruneExpiredMessages")
    flush = _function_source("flushRoomPreviews")
    assert "paintRoomPreview(column, row.dataset.channel);" in flush
    assert "directOrderChanged()" in flush
    assert "setTimeout(flushRoomPreviews, 50)" in _function_source(
        "queueRoomPreview")
    for selector in (
        ".chat-room-text {",
        ".chat-room-line {",
        ".chat-room-time {",
        ".chat-room-preview {",
    ):
        assert selector in CHAT_CSS
    preview = CHAT_CSS[CHAT_CSS.index(".chat-room-preview {"):]
    preview = preview[:preview.index("}")]
    assert "text-overflow: ellipsis;" in preview
    assert "white-space: nowrap;" in preview
    avatar = CHAT_CSS[CHAT_CSS.index(".chat-direct-row .chat-avatar {"):]
    assert "width: 30px;" in avatar[:avatar.index("}")]
    dot = CHAT_CSS[CHAT_CSS.index(
        ".chat-direct-row .chat-direct-face .chat-presence-dot {"):]
    assert "position: absolute;" in dot[:dot.index("}")]


def test_direct_message_thread_replies_reach_the_thread_store():
    helpers = _helpers(
        "isPrivateChannelKey",
        "isDirectMessageKey",
        "directMessageForKey",
        "channelDisplayLabel",
        "channelWireLabel",
        "frameMatchesScope",
        "once",
        "portableMessageRecord",
        "renderThreadEntry",
    )
    result = _run_module(f"""
      import {{ createThreadStore }}
        from {json.dumps((PUBLIC / "chat-thread-model.js").as_uri())};
      import {{ isSealedPlain }}
        from {json.dumps((PUBLIC / "chat-frame-auth.js").as_uri())};
      const selfId = "me";
      const MAX_NAME = 40;
      const activeChannel = {json.dumps(DIRECT)};
      const seen = new Set();
      const threads = createThreadStore();
      const directMessages = new Map([["a".repeat(32),
        {{ id: "a".repeat(32), otherUser: "ana" }}]]);
      const privateChannels = new Map();
      const allowedChatAccountKind = () => true;
      const normalizedPublicWorldFrame = (plain) => plain;
      const noteRoster = () => {{}};
      const attachmentFromEntry = () => null;
      const knownRecipientCount = (value) => value ?? null;
      const knownRecipientNames = (value) => value || [];
      let activeThreadRootId = "r1";
      let summaries = 0;
      let threadRenders = 0;
      const renderThreadSummary = () => {{ summaries += 1; }};
      const renderThread = () => {{ threadRenders += 1; }};
      {helpers}
      threads.registerRoot({{ id: "r1", channel: activeChannel,
        senderId: "me", sender: "jett", text: "root", ts: 1 }});
      const reply = {{ type: "thread-reply", id: "t1", rootId: "r1",
        senderId: "peer", sender: "ana", ts: 2, text: "answer",
        channel: channelWireLabel(activeChannel),
        frameAuth: {{ sealed: true, node: "peer", acct: "ana", kid: "k".repeat(16), self: false }} }};
      const stray = {{ ...reply, id: "t2", channel: "direct:" + "b".repeat(32) }};
      const unsealed = {{ ...reply, id: "t3", frameAuth: undefined }};
      const matches = [frameMatchesScope(reply, activeChannel),
        frameMatchesScope(stray, activeChannel)];
      renderThreadEntry(reply, activeChannel);
      renderThreadEntry(reply, activeChannel);
      renderThreadEntry(unsealed, activeChannel);
      const replies = threads.replies("r1").map((r) => ({{
        id: r.id, self: r.self, relayed: r.relayed, channel: r.channel }}));
      process.stdout.write(JSON.stringify({{
        matches, replies, count: threads.summary("r1").count,
        summaries, threadRenders,
        wire: channelWireLabel(activeChannel) }}));
    """)
    assert result["wire"] == DIRECT
    assert result["matches"] == [True, False]
    assert result["replies"] == [
        {"id": "t1", "self": False, "relayed": True, "channel": DIRECT}]
    assert result["count"] == 1
    assert result["summaries"] == 1
    assert result["threadRenders"] == 1
    send = _function_source("sendThreadReply")
    assert "channel: channelWireLabel(activeChannel)," in send
