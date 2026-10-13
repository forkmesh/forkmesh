from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
CHAT = (ROOT / "app" / "public" / "chat.js").read_text(encoding="utf-8")
DESKTOP = "".join(
    (ROOT / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in (
        "MainWindowMessages.cpp",
        "MainWindowNavigation.cpp",
        "MainWindowRepoSync.cpp",
        "MainWindowSetup.cpp",
        "OfficeChannelMirror.cpp",
    )
)


def _function(source, name):
    start = source.index("function " + name + "(")
    end = source.index("\n}\n", start)
    return source[start:end]


def test_web_key_pattern_matches_the_server():
    import sys
    sys.path.insert(0, str(ROOT / "app" / "src"))
    import chat_reads_api
    pattern = re.search(r"READ_MARK_KEY_RE =\s*/(.+)/;", CHAT).group(1)
    assert pattern == chat_reads_api.CONVERSATION_RE.pattern
    assert 'CHAT_READS_ENDPOINT = "/api/chat/reads"' in CHAT


def test_web_posts_a_mark_only_for_the_conversation_on_screen():
    queue = _function(CHAT, "queueReadMark")
    assert "document.hidden" in queue
    assert "channel !== activeChannel" in queue
    assert "READ_MARK_POST_DELAY_MS" in queue
    assert "queueReadMark(channel);" in _function(CHAT, "insertMessage")
    flush = _function(CHAT, "flushReadMarks")
    assert 'method: "POST"' in flush and "READ_MARK_POST_MAX" in flush


def test_web_applies_marks_on_the_chat_read_event_and_on_reconnect():
    assert 'topic === "chat-read"' in CHAT
    on_connected = CHAT[CHAT.index("onConnected: () => {"):]
    assert on_connected.index("refreshReadMarks();") < on_connected.index(
        "accountEventsConnectedOnce")
    refresh = _function(CHAT, "refreshReadMarks")
    assert "meta.unreadTs <= readMarks.get(channel)" in refresh
    bump = _function(CHAT, "bumpUnread")
    assert "isReadMarked(channel, ts)" in bump


def test_desktop_fetches_marks_on_the_event_and_skips_read_arrivals():
    assert 'topic == QLatin1String("chat-read")' in DESKTOP
    assert "message.timestampMs > m_readMarks.value(conversation)" in DESKTOP
    assert "QStringLiteral(\"/api/chat/reads\")" in DESKTOP
    assert "readMarksPostProof(m_account, ts, keyed)" in DESKTOP
