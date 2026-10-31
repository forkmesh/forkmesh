"""Desktop chat executes the same /issue and /topic commands as web chat."""

import ast
import asyncio
import hashlib
from pathlib import Path
import re
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[2]
ENTRY = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
CHANNELS = (
    ROOT / "app" / "src" / "chat_channels_api.py"
).read_text(encoding="utf-8")
MIRROR_H = (
    ROOT / "desktop" / "src" / "OfficeChannelMirror.h"
).read_text(encoding="utf-8")
MIRROR_CPP = (
    ROOT / "desktop" / "src" / "OfficeChannelMirror.cpp"
).read_text(encoding="utf-8")
MESSAGES = (
    ROOT / "desktop" / "src" / "MainWindowMessages.cpp"
).read_text(encoding="utf-8")
ISSUES = (
    ROOT / "desktop" / "src" / "MainWindowIssues.cpp"
).read_text(encoding="utf-8")
SETUP = (
    ROOT / "desktop" / "src" / "MainWindowSetup.cpp"
).read_text(encoding="utf-8")


class _Request:
    def __init__(self, method, url):
        self.method = method
        self.url = url


def _signed_session_harness():
    tree = ast.parse(ENTRY, filename=str(ROOT / "app" / "src" / "entry.py"))
    wanted = {
        "_chat_channel_settings_fields",
        "_chat_channel_create_fields",
        "_chat_channel_signed_session",
    }
    functions = [
        node
        for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in wanted
    ]
    assert {node.name for node in functions} == wanted
    record = {
        "name": "admin",
        "status": "active",
        "kind": "user",
        "pubkey": "PK-admin",
    }

    async def owner_pubkey(_env, name):
        return record["pubkey"] if name == "admin" else ""

    async def account_row(_env, name):
        return ("bi:admin", record) if name == "admin" else ("", None)

    async def sha256_hex(value):
        return hashlib.sha256(value.encode()).hexdigest()

    async def verify(pubkey, signature, canonical):
        expected = hashlib.sha256(pubkey.encode() + canonical).hexdigest()
        return signature == expected

    namespace = {
        "parse_qs": parse_qs,
        "urlparse": urlparse,
        "method_name": lambda request: request.method,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 63,
        "_ts_ok": lambda _ts: True,
        "_owner_pubkey": owner_pubkey,
        "_account_row": account_row,
        "_account_kind": lambda value: value.get("kind", "user"),
        "ed25519_verify": verify,
        "sha256_hex": sha256_hex,
        "CHAT_CHANNELS_RE": re.compile(r"^/api/chat/channels/?$"),
        "CHAT_CHANNEL_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/?$"
        ),
        "CHAT_CHANNEL_ROOM_ACCESS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/access/?$"
        ),
        "CHAT_CHANNEL_HISTORY_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/history/?$"
        ),
        "CHAT_CHANNEL_WEBHOOKS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/webhooks/?$"
        ),
        "CHAT_CHANNEL_WEBHOOK_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/webhooks/([0-9a-f]{32})/?$"
        ),
        "CHAT_CHANNEL_MEMBERS_RE": re.compile(
            r"^/api/chat/channels/([0-9a-f]{32})/members/?$"
        ),
        "CHAT_CHANNEL_CREATE_PROOF": "forkmesh-chat-channel-create-v1",
        "CHAT_CHANNEL_MEMBERS_PROOF": "forkmesh-chat-channel-members-v1",
        "CHAT_CHANNEL_CREATE_MAX_MEMBERS": 500,
        "CHAT_CHANNEL_LIST_PROOF": "forkmesh-chat-channels-v1",
        "CHAT_CHANNEL_ACCESS_PROOF": "forkmesh-chat-channel-access-v1",
        "CHAT_CHANNEL_HISTORY_PROOF": "forkmesh-chat-channel-history-v1",
        "CHAT_CHANNEL_TOPIC_PROOF": "forkmesh-chat-channel-topic-v1",
        "CHAT_CHANNEL_SETTINGS_PROOF": "forkmesh-chat-channel-settings-v1",
        "CHAT_CHANNEL_DELETE_PROOF": "forkmesh-chat-channel-delete-v1",
        "CHAT_CHANNEL_WEBHOOKS_PROOF": "forkmesh-chat-channel-webhooks-v1",
        "CHAT_CHANNEL_WEBHOOK_PROOF": "forkmesh-chat-channel-webhook-v1",
        "CHAT_CHANNEL_TOPIC_MAX_LENGTH": 200,
        "CHAT_CHANNEL_NAME_MAX_LENGTH": 40,
        "CHAT_CHANNEL_WEBHOOK_NAME_MAX_LENGTH": 80,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=functions, type_ignores=[]))
    exec(compile(module, str(ROOT / "app" / "src" / "entry.py"), "exec"), namespace)
    return namespace, record


def _topic_url(namespace, channel, topic, proof="forkmesh-chat-channel-topic-v1"):
    node = "admin"
    ts = "1700000000000"
    digest = hashlib.sha256(" ".join(topic.split()).encode()).hexdigest()
    canonical = f"{proof}\n{node}\n{channel}\n{ts}\n{digest}".encode()
    signature = hashlib.sha256(b"PK-admin" + canonical).hexdigest()
    return (
        f"https://forkmesh.test/api/chat/channels/{channel}"
        f"?node={node}&ts={ts}&sig={signature}"
    )


def _region(source, start, end):
    offset = source.index(start)
    return source[offset:source.index(end, offset)]


def test_desktop_composer_intercepts_commands_before_chat_send():
    send = _region(
        MESSAGES,
        "void MainWindow::sendCurrentMessage()",
        "void MainWindow::insertEmojiIntoComposer",
    )
    assert '/(issue|topic)' in send
    assert "createIssueFromChatCommand(argument)" in send
    assert "canManageConversation(" in send
    assert "setTopic(m_currentConversation, argument)" in send
    assert send.index("slashCommand.match(text)") < send.index("sendMessage(")


def test_desktop_composer_offers_contextual_slash_command_completions():
    helper = _region(
        MESSAGES,
        "void MainWindow::updateMentionPopup()",
        "void MainWindow::insertMention(",
    )
    assert 'QStringLiteral("\\\\A/[A-Za-z]*\\\\z")' in helper
    assert "/issue [owner/repo] <title>" in helper
    assert "/topic <text>" in helper
    assert "canManageConversation(" in helper
    assert "m_mentionCompleter->complete(rect)" in helper
    assert "void MainWindow::insertComposerCompletion(" in helper
    assert "completion.section(QLatin1Char(' '), 0, 0)" in helper
    assert "insertComposerCompletion(name)" in ISSUES


def test_desktop_issue_command_uses_existing_signed_and_local_paths():
    command = _region(
        ISSUES,
        "bool MainWindow::createIssueFromChatCommand(",
        "void MainWindow::composeNewIssue(",
    )
    assert "[owner/repo] <title>" in command
    assert "issueStoreForCurrentRepo()" in command
    assert "submitNewIssueToInbox(" in command
    assert "store.createIssue(" in command
    assert "appendCreatedIssue(store, created)" in command
    assert "propagateRepoUpdate(repoIndex)" in command


def test_desktop_topic_command_is_signed_and_updates_the_heading():
    assert "QByteArray channelTopicProof" in MIRROR_H
    assert 'forkmesh-chat-channel-topic-v1\\n%1\\n%2\\n%3\\n%4' in MIRROR_CPP
    setter = _region(
        MIRROR_CPP,
        "bool OfficeChannelMirror::setTopic(",
        "QUrl OfficeChannelMirror::signedUrl(",
    )
    assert 'QByteArrayLiteral("PATCH")' in setter
    assert "channelTopicProof(" in setter
    assert "emit topicChanged(conversation, saved)" in setter
    assert "&OfficeChannelMirror::topicChanged" in SETUP
    assert "updateChatConversationHeading()" in SETUP


def test_worker_accepts_only_the_exact_manager_topic_proof():
    signed = _region(
        ENTRY,
        "async def _chat_channel_signed_session",
        "# Canonical prefixes a desktop signs",
    )
    assert 'method not in {"GET", "POST", "PATCH", "DELETE"}' in signed
    assert "CHAT_CHANNEL_TOPIC_PROOF" in signed
    assert "await sha256_hex(topic)" in signed
    assert "channel.group(1)" in signed
    assert "CHAT_CHANNEL_TOPIC_MAX_LENGTH = 200" in ENTRY
    assert "MAX_CHANNEL_TOPIC_LENGTH = 200" in CHANNELS
    # The signed proof identifies the account; normal API authorization still
    # requires either the channel creator/manager or a platform administrator.
    assert '"chat.channel.topic.update"' in CHANNELS
    assert "_require_manager(" in CHANNELS
    assert "created_by_bi" in CHANNELS
    assert "is_admin" in CHANNELS


def test_worker_verifies_desktop_topic_proof_against_the_exact_body():
    namespace, record = _signed_session_harness()
    resolve = namespace["_chat_channel_signed_session"]
    channel = "a" * 32
    topic = "  Release\n coordination  "
    request = _Request("PATCH", _topic_url(namespace, channel, topic))

    assert asyncio.run(resolve(object(), request, {"topic": topic})) == (
        "bi:admin",
        record,
    )
    assert asyncio.run(
        resolve(object(), request, {"topic": "Different topic"})
    ) == ("", None)
    read_proof = _topic_url(
        namespace, channel, topic, proof="forkmesh-chat-channel-access-v1"
    )
    assert asyncio.run(
        resolve(object(), _Request("PATCH", read_proof), {"topic": topic})
    ) == ("", None)


def _signed_url(path, canonical, node="admin", ts="1700000000000"):
    signature = hashlib.sha256(b"PK-admin" + canonical.encode()).hexdigest()
    return f"https://forkmesh.test{path}?node={node}&ts={ts}&sig={signature}"


def test_worker_verifies_the_desktop_channel_settings_proof():
    namespace, record = _signed_session_harness()
    resolve = namespace["_chat_channel_signed_session"]
    channel = "a" * 32
    ts = "1700000000000"
    fields = "name=shipping\nvisibility=private\ntopic=release planning"
    digest = hashlib.sha256(fields.encode()).hexdigest()
    # The desktop pins this exact digest in test_chat_transport.cpp.
    assert digest == (
        "0b62f098453457605adbb8b28b1f666bcb1adee749ef31dd9d9fb3f961ff8745"
    )
    body = {
        "name": "  Shipping ",
        "visibility": "private",
        "topic": "  release   planning ",
    }
    url = _signed_url(
        f"/api/chat/channels/{channel}",
        f"forkmesh-chat-channel-settings-v1\nadmin\n{channel}\n{ts}\n{digest}",
    )
    request = _Request("PATCH", url)
    assert asyncio.run(resolve(object(), request, body)) == (
        "bi:admin",
        record,
    )

    # One signature authorizes one body: no field can be swapped afterwards.
    for swapped in (
        {**body, "name": "security"},
        {**body, "visibility": "public"},
        {**body, "topic": "something else"},
        {"name": body["name"], "visibility": body["visibility"]},
    ):
        assert asyncio.run(resolve(object(), request, swapped)) == ("", None)

    # The read proofs cannot stand in for it.
    for proof in (
        "forkmesh-chat-channel-topic-v1",
        "forkmesh-chat-channel-access-v1",
        "forkmesh-chat-channels-v1",
    ):
        wrong = _signed_url(
            f"/api/chat/channels/{channel}",
            f"{proof}\nadmin\n{channel}\n{ts}\n{digest}",
        )
        assert asyncio.run(
            resolve(object(), _Request("PATCH", wrong), body)
        ) == ("", None)

    # A body naming nothing settable is refused before any signature check.
    empty = _signed_url(
        f"/api/chat/channels/{channel}",
        f"forkmesh-chat-channel-settings-v1\nadmin\n{channel}\n{ts}\n{digest}",
    )
    assert asyncio.run(resolve(object(), _Request("PATCH", empty), {})) == (
        "",
        None,
    )


def test_worker_verifies_the_desktop_delete_and_webhook_proofs():
    namespace, record = _signed_session_harness()
    resolve = namespace["_chat_channel_signed_session"]
    channel = "a" * 32
    webhook = "c" * 32
    ts = "1700000000000"
    empty_digest = hashlib.sha256(b"").hexdigest()
    name_digest = hashlib.sha256(b"Deploy bot").hexdigest()

    delete_url = _signed_url(
        f"/api/chat/channels/{channel}",
        f"forkmesh-chat-channel-delete-v1\nadmin\n{channel}\n{ts}",
    )
    assert asyncio.run(
        resolve(object(), _Request("DELETE", delete_url), {})
    ) == ("bi:admin", record)
    # A delete proof is not a settings proof.
    assert asyncio.run(
        resolve(object(), _Request("PATCH", delete_url), {"topic": ""})
    ) == ("", None)

    list_url = _signed_url(
        f"/api/chat/channels/{channel}/webhooks",
        "forkmesh-chat-channel-webhooks-v1\nadmin\n"
        f"{channel}\n{ts}\nGET\n{empty_digest}",
    )
    assert asyncio.run(resolve(object(), _Request("GET", list_url), None)) == (
        "bi:admin",
        record,
    )
    # Listing them does not authorize creating one.
    assert asyncio.run(
        resolve(object(), _Request("POST", list_url), {"name": "Deploy bot"})
    ) == ("", None)

    create_url = _signed_url(
        f"/api/chat/channels/{channel}/webhooks",
        "forkmesh-chat-channel-webhooks-v1\nadmin\n"
        f"{channel}\n{ts}\nPOST\n{name_digest}",
    )
    assert asyncio.run(
        resolve(
            object(), _Request("POST", create_url),
            {"name": "  Deploy  bot "},
        )
    ) == ("bi:admin", record)
    assert asyncio.run(
        resolve(object(), _Request("POST", create_url), {"name": "Other bot"})
    ) == ("", None)

    rotate_url = _signed_url(
        f"/api/chat/channels/{channel}/webhooks/{webhook}",
        "forkmesh-chat-channel-webhook-v1\nadmin\n"
        f"{channel}\n{webhook}\n{ts}\nPOST",
    )
    assert asyncio.run(
        resolve(object(), _Request("POST", rotate_url), {})
    ) == ("bi:admin", record)
    # Rotating is not removing, and neither reaches another webhook.
    assert asyncio.run(
        resolve(object(), _Request("DELETE", rotate_url), {})
    ) == ("", None)
    other = _signed_url(
        f"/api/chat/channels/{channel}/webhooks/{'d' * 32}",
        "forkmesh-chat-channel-webhook-v1\nadmin\n"
        f"{channel}\n{webhook}\n{ts}\nPOST",
    )
    assert asyncio.run(resolve(object(), _Request("POST", other), {})) == (
        "",
        None,
    )


def test_desktop_chat_header_opens_channel_settings_from_the_info_button():
    header = _region(
        ISSUES,
        "m_channelSettingsButton = new QPushButton",
        "m_firewallBanner = new QWidget;",
    )
    assert "&MainWindow::openChannelSettings" in header
    assert 'setOcticon(m_channelSettingsButton, "info", 18)' in header
    settings_row = (
        "headerLayout->addWidget(m_channelSettingsButton, 0, Qt::AlignVCenter);")
    assert header.index(
        "headerLayout->addWidget(m_channelTopicLabel, 1, Qt::AlignVCenter);"
    ) < header.index(settings_row)
    assert header.index(
        "headerLayout->addWidget(m_encryptionLabel, 0, Qt::AlignVCenter);"
    ) < header.index(settings_row)
    gate = _region(
        MESSAGES,
        "void MainWindow::refreshChannelSettingsButton()",
        "void MainWindow::renameChatConversation(",
    )
    assert "canManageConversation(m_currentConversation)" in gate
    assert "isOfficeConversation(m_currentConversation)" in gate
    assert "m_channelSettingsButton->setVisible(\n        !m_currentConversation.isEmpty() &&\n        !isDirectConversation(m_currentConversation));" in gate
    dialog = _region(
        MESSAGES,
        "void MainWindow::openChannelSettings()",
        "void MainWindow::promptDeleteRoom(",
    )
    assert "const bool manage = m_officeChannelMirror->canManageConversation(target);" in dialog
    assert "if (!manage) {" in dialog
    assert "refreshChannelSettingsButton();" in MESSAGES


def test_desktop_channel_settings_dialog_covers_every_managed_setting():
    dialog = _region(
        MESSAGES,
        "void MainWindow::openChannelSettings()",
        "void MainWindow::promptDeleteRoom(",
    )
    for control in (
        'QStringLiteral("Channel name")',
        'QStringLiteral("Topic")',
        'QStringLiteral("Visibility")',
        'QStringLiteral("Add webhook")',
        'QStringLiteral("New URL")',
        'QStringLiteral("Copy URL")',
        'QStringLiteral("Delete channel")',
    ):
        assert control in dialog
    assert "updateChannel(" in dialog
    assert "createWebhook(" in dialog
    assert "rotateWebhook(" in dialog
    assert "deleteWebhook(" in dialog
    assert "deleteChannel(" in dialog
    # Destructive actions confirm first.
    assert dialog.count("QMessageBox::question(") == 3
    # A URL exists only for a webhook this process issued.
    assert "URL hidden — the relay keeps no copy" in dialog
    assert "refreshWebhooks(*conversation)" in dialog


def test_desktop_rename_moves_the_conversation_it_already_holds():
    rename = _region(
        MESSAGES,
        "void MainWindow::renameChatConversation(",
        "void MainWindow::openChannelSettings()",
    )
    assert "m_history.take(oldConversation)" in rename
    assert "message.conversation = newConversation" in rename
    assert "m_unread.remove(oldConversation)" in rename
    assert "m_unreadCounts.take(oldConversation)" in rename
    assert "m_currentConversation = newConversation" in rename
    assert "rebuildConversationView()" in rename
    assert "scheduleChatSave()" in rename
    assert "&OfficeChannelMirror::conversationRenamed" in SETUP
    assert "&MainWindow::renameChatConversation" in SETUP
    assert "&OfficeChannelMirror::channelDeleted" in SETUP
    # A rename another client made arrives through the periodic list read.
    assert "emit conversationRenamed(previousConversation, conversation)" in (
        MIRROR_CPP
    )


def test_desktop_channel_settings_are_signed_per_action():
    for proof in (
        "QByteArray channelSettingsProof",
        "QByteArray channelDeleteProof",
        "QByteArray channelWebhooksProof",
        "QByteArray channelWebhookProof",
    ):
        assert proof in MIRROR_H
    assert 'forkmesh-chat-channel-settings-v1\\n%1\\n%2\\n%3\\n%4' in (
        MIRROR_CPP
    )
    assert 'forkmesh-chat-channel-delete-v1\\n%1\\n%2\\n%3' in MIRROR_CPP
    assert 'forkmesh-chat-channel-webhooks-v1\\n%1\\n%2\\n%3\\n%4\\n%5' in (
        MIRROR_CPP
    )
    assert 'forkmesh-chat-channel-webhook-v1\\n%1\\n%2\\n%3\\n%4\\n%5' in (
        MIRROR_CPP
    )
    # Every write is manager-gated on the desktop and again on the relay.
    for setter in (
        "bool OfficeChannelMirror::updateChannel(",
        "bool OfficeChannelMirror::deleteChannel(",
        "bool OfficeChannelMirror::refreshWebhooks(",
        "bool OfficeChannelMirror::createWebhook(",
        "bool OfficeChannelMirror::rotateWebhook(",
        "bool OfficeChannelMirror::deleteWebhook(",
    ):
        body = MIRROR_CPP[MIRROR_CPP.index(setter):]
        assert "channelIdForConversation(conversation, true)" in body[:1200]
    assert '"chat.channel.delete"' in CHANNELS
    assert '"chat.channel.webhook.rotate"' in CHANNELS
