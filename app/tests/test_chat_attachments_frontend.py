"""Browser attachment protocol and UI contracts for both chat surfaces."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
ATTACHMENTS = (PUBLIC / "chat-attachments.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
HTML = (PUBLIC / "chat.html").read_text(encoding="utf-8")
CSS = (PUBLIC / "chat.css").read_text(encoding="utf-8")
DESKTOP = ROOT.parent / "desktop"
QT_ROW = (DESKTOP / "src" / "MessageRow.cpp").read_text(encoding="utf-8")
QT_MESSAGES = (DESKTOP / "src" / "MainWindowMessages.cpp").read_text(
    encoding="utf-8"
)
QT_CMAKE = (DESKTOP / "CMakeLists.txt").read_text(encoding="utf-8")


def test_full_chat_has_accessible_image_and_document_controls():
    assert 'id="chat-attachment-input"' in HTML
    assert 'id="chat-attachment-button"' in HTML
    assert 'aria-label="Attach image or document"' in HTML
    assert 'type="file"' in HTML


def test_full_chat_uses_desktop_compatible_encrypted_attachment_fields():
    assert "const MAX_ATTACHMENT_BYTES = 1024 * 1024" in ATTACHMENTS
    assert 'from "./chat-attachments.js"' in CHAT
    assert "async function sendAttachment(" in CHAT
    assert "function renderAttachment(" in CHAT
    for field in ("fileName", "fileMime", "file"):
        assert field in CHAT
    assert 'input.addEventListener("paste"' in CHAT
    assert "clipboardData" in CHAT
    assert "event.preventDefault()" in CHAT


def test_full_chat_sends_picker_and_pasted_attachment_drafts_immediately():
    attachment_controls = CHAT[
        CHAT.index("if (attachmentBtn && attachmentInput) {"):
        CHAT.index("if (threadAttachmentBtn && threadAttachmentInput) {")
    ]
    paste = CHAT[
        CHAT.index('input.addEventListener("paste"'):
        CHAT.index('threadInput?.addEventListener("paste"')
    ]
    assert "stageAttachments(files);" in attachment_controls
    assert "void sendAttachmentDraft();" in attachment_controls
    assert "stageAttachments([file]);\n    void sendAttachmentDraft();" in paste


def test_full_chat_renders_safe_images_and_downloadable_documents():
    assert 'className = "chat-attachment-image"' in CHAT
    assert 'className = "chat-attachment-card"' in CHAT
    assert 'link.download = attachment.fileName' in CHAT
    assert "URL.createObjectURL" in CHAT
    assert "safeAttachmentName" in CHAT
    assert "export function safeAttachmentName" in ATTACHMENTS


def test_dashboard_chat_has_equivalent_dynamic_attachment_controls():
    assert "const MAX_ATTACHMENT_BYTES = 1024 * 1024" in DASHBOARD_CHAT
    assert "function mountAttachmentControl(" in DASHBOARD_CHAT
    assert "async function sendAttachment(" in DASHBOARD_CHAT
    assert "function renderAttachment(" in DASHBOARD_CHAT
    assert 'inputEl.addEventListener("paste"' in DASHBOARD_CHAT
    assert "clipboardData" in DASHBOARD_CHAT
    assert "fileName" in DASHBOARD_CHAT
    assert "fileMime" in DASHBOARD_CHAT
    assert "file:" in DASHBOARD_CHAT


def test_oversized_attachments_fail_before_encryption_or_socket_send():
    for source in (CHAT, DASHBOARD_CHAT):
        size_check = source.index("file.size > MAX_ATTACHMENT_BYTES")
        encode = source.index("file.arrayBuffer()", size_check)
        assert size_check < encode
        assert "1 MiB" in source[size_check:encode]


def test_full_chat_plays_video_attachments_inline():
    render = CHAT[
        CHAT.index("function renderAttachment("):
        CHAT.index("function renderRecordBody(")
    ]
    assert 'blobType.startsWith("video/")' in render
    assert 'document.createElement("video")' in render
    assert "video.controls = true;" in render
    assert 'video.preload = "metadata";' in render
    # A codec the browser cannot decode falls back to the downloadable card
    # rather than leaving a dead black box in the transcript.
    assert 'video.addEventListener("error"' in render
    assert "attachmentCard(attachment, objectUrl)" in render
    assert ".chat-attachment-video {" in CSS


def test_dashboard_chat_plays_video_attachments_at_both_densities():
    render = DASHBOARD_CHAT[
        DASHBOARD_CHAT.index("function renderAttachment("):
        DASHBOARD_CHAT.index("function ensureHistoryIndicator(")
    ]
    assert 'attachmentBlobType(attachment.fileMime).startsWith("video/")' in render
    assert 'document.createElement("video")' in render
    assert "video.controls = true;" in render
    # The World embed renders the same player, only shorter.
    assert "max-h-28" in render and "max-h-80" in render
    assert "attachmentFileCard(attachment, objectUrl, compact)" in render


def test_desktop_chat_videos_play_instead_of_only_offering_save():
    assert 'mime.startsWith("video/")' in QT_ROW
    assert 'card->setObjectName("videoCard")' in QT_ROW
    assert "emit videoActivated(" in QT_ROW
    assert "void MainWindow::showChatVideoDetail(" in QT_MESSAGES
    # Qt Multimedia is an optional module: a build without it still plays the
    # video by handing it to the desktop's own player.
    assert "#ifdef FORKMESH_HAVE_MULTIMEDIA" in QT_MESSAGES
    assert "openChatMediaExternally(fileName, fileMime, data)" in QT_MESSAGES
    assert (
        "find_package(Qt6 6.4 QUIET COMPONENTS Multimedia MultimediaWidgets)"
        in QT_CMAKE
    )
    assert "function(forkmesh_link_multimedia target)" in QT_CMAKE
    # Every target that compiles the MainWindow sources, or the player is
    # compiled out of the tests that cover it.
    for target in ("forkmesh", "forkmesh-window-tests", "forkmesh-perf-tests"):
        assert f"forkmesh_link_multimedia({target})" in QT_CMAKE


def test_desktop_external_playback_names_the_file_from_the_declared_type():
    handoff = QT_MESSAGES[
        QT_MESSAGES.index("bool MainWindow::openChatMediaExternally("):
        QT_MESSAGES.index("void MainWindow::showChatVideoDetail(")
    ]
    # The extension decides which program the desktop launches, so a sender's
    # file name never gets to pick it.
    assert (
        "QMimeDatabase().mimeTypeForName(fileMime).preferredSuffix()" in handoff
    )
    assert "[^A-Za-z0-9 ._-]" in handoff
    assert "QDesktopServices::openUrl(QUrl::fromLocalFile(path))" in handoff

